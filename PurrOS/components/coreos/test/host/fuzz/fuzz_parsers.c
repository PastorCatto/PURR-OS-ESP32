/*
 * fuzz_parsers.c - a deterministic mutation fuzzer for the parsers of untrusted input
 * (plan3.md item 1, SECURITY.md section 11). Built and run by fuzz.py under AddressSanitizer and
 * UndefinedBehaviorSanitizer.
 *
 * Every input is a fresh malloc() of exactly its length, so reading even one byte past the end is
 * an AddressSanitizer error, and nothing is NUL-terminated for the parsers. The generator is a
 * fixed-seed xorshift, so a run is exactly reproducible: the same seed and iteration count hit
 * the same inputs. The crashing input is also written to crash-<target>.bin.
 *
 *   fuzz_parsers <target|all> [iterations] [seed]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "purr_cfg.h"
#include "purr_manifest.h"
#include "purr_trust.h"
#include "purr_module.h"
#include "purr_relocate.h"
#include "purr_trailer.h"
#include "purr_util.h"
#include "purr_verify.h"
#include "purr_wifi.h"

#if defined(__has_include)
#if __has_include(<sanitizer/common_interface_defs.h>)
#include <sanitizer/common_interface_defs.h>
#define HAVE_DEATH_CB 1
#endif
#endif

/* ------------------------------------------------------------------ random + mutation */

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;

static uint64_t rnd(void)
{
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}

static uint32_t rnd_to(uint32_t n)       /* 0 .. n-1 */
{
    return n ? (uint32_t)(rnd() % n) : 0;
}

static const uint32_t k_interesting[] = {
    0u, 1u, 2u, 3u, 4u, 7u, 8u, 15u, 16u, 31u, 32u, 63u, 64u, 127u, 128u, 172u, 173u, 174u,
    255u, 256u, 511u, 512u, 513u, 1023u, 1024u, 4095u, 4096u, 4097u, 8191u, 8192u, 65535u,
    65536u, 0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFFFF0u, 0xFFFFFFFCu, 0xFFFFFFFEu,
    0xFFFFFFFFu,
};

/* The current input, kept for the death callback. */
static const uint8_t *g_cur;
static size_t g_cur_len;
static const char *g_target = "?";

#ifdef HAVE_DEATH_CB
static void on_death(void)
{
    char name[64];
    snprintf(name, sizeof(name), "crash-%s.bin", g_target);
    FILE *f = fopen(name, "wb");
    if (f) {
        fwrite(g_cur, 1, g_cur_len, f);
        fclose(f);
        fprintf(stderr, "fuzz: crashing input (%zu bytes) written to %s\n", g_cur_len, name);
    }
}
#endif

/* Returns a new malloc()ed exact-size buffer; *len updated. The seed is untouched. */
static uint8_t *mutate(const uint8_t *seed, size_t seed_len, size_t *out_len)
{
    size_t cap = seed_len + 64;
    uint8_t *b = malloc(cap ? cap : 1);
    memcpy(b, seed, seed_len);
    size_t len = seed_len;

    int rounds = 1 + (int)rnd_to(6);
    for (int r = 0; r < rounds; r++) {
        switch (rnd_to(8)) {
        case 0:                                   /* flip a bit */
            if (len) b[rnd_to((uint32_t)len)] ^= (uint8_t)(1u << rnd_to(8));
            break;
        case 1:                                   /* random byte */
            if (len) b[rnd_to((uint32_t)len)] = (uint8_t)rnd();
            break;
        case 2:                                   /* an interesting 32-bit value, anywhere */
            if (len >= 4) {
                uint32_t v = k_interesting[rnd_to(sizeof(k_interesting) / sizeof(k_interesting[0]))];
                memcpy(b + rnd_to((uint32_t)(len - 3)), &v, 4);
            }
            break;
        case 3:                                   /* an interesting value in the header area */
            if (len >= 4) {
                uint32_t lim = len < 200 ? (uint32_t)len : 200u;
                uint32_t v = k_interesting[rnd_to(sizeof(k_interesting) / sizeof(k_interesting[0]))];
                memcpy(b + rnd_to(lim - 3), &v, 4);
            }
            break;
        case 4:                                   /* truncate */
            if (len > 1) len = rnd_to((uint32_t)len);
            break;
        case 5:                                   /* extend with random bytes */
            for (uint32_t i = 0, n = 1 + rnd_to(32); i < n && len < cap; i++) b[len++] = (uint8_t)rnd();
            break;
        case 6:                                   /* copy a chunk over another place */
            if (len > 8) {
                uint32_t n = 1 + rnd_to(16), s = rnd_to((uint32_t)len), d = rnd_to((uint32_t)len);
                if (s + n > len) n = (uint32_t)len - s;
                if (d + n > len) n = (uint32_t)len - d;
                memmove(b + d, b + s, n);
            }
            break;
        default:                                  /* zero or 0xFF a run */
            if (len) {
                uint32_t n = 1 + rnd_to(24), s = rnd_to((uint32_t)len);
                if (s + n > len) n = (uint32_t)len - s;
                memset(b + s, rnd() & 1 ? 0xFF : 0x00, n);
            }
            break;
        }
    }
    /* exact-size copy so the allocation ends where the input ends */
    uint8_t *exact = malloc(len ? len : 1);
    memcpy(exact, b, len);
    free(b);
    *out_len = len;
    return exact;
}

#define BEGIN_INPUT(buf, len) do { g_cur = (buf); g_cur_len = (len); } while (0)

/* ------------------------------------------------------------------ crypto stub */

/* The fuzzer is after parser bugs, not crypto: a stub that sometimes says yes drives execution
 * past the signature check as well as up to it. */
static int stub_verify(const uint8_t pub[PURR_PUBKEY_LEN], const uint8_t hash[PURR_SHA256_LEN],
                       const uint8_t sig[PURR_SIG_LEN])
{
    return (pub[0] ^ hash[0] ^ sig[0]) & 1;
}
static const purr_crypto_t g_stub = {stub_verify};

static purr_keybag_t g_bag;

static void setup_bag(void)
{
    purr_key_t d[3];
    memset(d, 0, sizeof(d));
    d[0].key_id = 0; d[0].role = PURR_ROLE_BOOT;
    d[1].key_id = 1; d[1].role = PURR_ROLE_SYSTEM;
    d[2].key_id = 2; d[2].role = PURR_ROLE_DEVELOPER;
    for (int i = 0; i < 3; i++) for (int k = 0; k < PURR_PUBKEY_LEN; k++) d[i].pub[k] = (uint8_t)(i * 31 + k);
    purr_keybag_build(&g_bag, d, 3, NULL);
}

/* ------------------------------------------------------------------ seeds */

static void make_header(purr_image_header_t *h, uint8_t type, uint8_t sub, uint8_t key,
                        uint32_t payload_size, const uint8_t *payload)
{
    memset(h, 0, sizeof(*h));
    h->magic = PURR_IMAGE_MAGIC;
    h->header_version = PURR_IMAGE_HEADER_VERSION;
    h->header_size = sizeof(*h);
    h->chip_id = PURR_CHIP_ESP32S3;
    h->image_type = type;
    h->key_id = key;
    h->flags = sub;
    memcpy(h->name, "fuzz", 5);
    memcpy(h->version, "1.2.3", 6);
    memcpy(h->min_boot_version, "1.0.0", 6);
    h->payload_offset = sizeof(*h);
    h->payload_size = payload_size;
    purr_sha256(payload, payload_size, h->payload_sha256);
}

/* ------------------------------------------------------------------ targets */

static void fuzz_verify(uint32_t iters)
{
    uint8_t payload[96];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i * 7 + 1);
    uint8_t seed[sizeof(purr_image_header_t) + sizeof(payload)];
    purr_image_header_t h;
    make_header(&h, PURR_IMG_OS, 0, 0, sizeof(payload), payload);
    memcpy(seed, &h, sizeof(h));
    memcpy(seed + sizeof(h), payload, sizeof(payload));

    for (uint32_t it = 0; it < iters; it++) {
        size_t len;
        uint8_t *in = mutate(seed, sizeof(seed), &len);
        BEGIN_INPUT(in, len);
        purr_verify_env_t env = {.chip_id = (uint16_t)(rnd() & 1 ? PURR_CHIP_ESP32S3 : PURR_CHIP_ESP32),
                                 .bag = &g_bag, .crypto = &g_stub,
                                 .bootloader_version = rnd_to(0x01000000), .version_floor = rnd_to(0x01000000),
                                 .enforce_floor = (int)(rnd() & 1)};
        purr_mem_read_ctx_t m = {in, (uint32_t)len};
        uint32_t claimed = rnd_to(4) == 0 ? (uint32_t)rnd() : (uint32_t)len;   /* sometimes lie about the size */
        purr_image_header_t out;
        (void)purr_image_verify(&env, purr_mem_read, &m, claimed, &out);
        free(in);
    }
}

static void fuzz_trailer(uint32_t iters)
{
    static const uint32_t sizes[] = {4096, 4097, 8192, 16384};
    for (uint32_t it = 0; it < iters; it++) {
        uint32_t psize = sizes[rnd_to(4)];
        uint8_t *part = malloc(psize);
        memset(part, 0xFF, psize);
        uint32_t plen = psize > 4096 ? rnd_to(psize - 4096 + 1) : 0;
        for (uint32_t i = 0; i < plen; i++) part[i] = (uint8_t)(i * 13 + 5);
        purr_image_header_t h;
        make_header(&h, PURR_IMG_MODULE, PURR_MOD_KERNEL, 0, plen, part);
        memcpy(part + (psize > 4096 ? psize - 4096 : 0), &h, sizeof(h) < psize ? sizeof(h) : psize);

        size_t len;
        uint8_t *in = mutate(part, psize, &len);
        free(part);
        BEGIN_INPUT(in, len);
        purr_verify_env_t env = {.chip_id = PURR_CHIP_ESP32S3, .bag = &g_bag, .crypto = &g_stub,
                                 .bootloader_version = 0, .version_floor = 0, .enforce_floor = (int)(rnd() & 1)};
        purr_mem_read_ctx_t m = {in, (uint32_t)len};
        uint32_t claimed = rnd_to(3) == 0 ? (uint32_t)rnd_to(1u << 20) : (uint32_t)len;
        purr_image_header_t out;
        (void)purr_trailer_verify(&env, purr_mem_read, &m, claimed, PURR_IMG_MODULE,
                                  rnd() & 1 ? PURR_MOD_KERNEL : PURR_MOD_LOADER, &out);
        free(in);
    }
}

static void fuzz_module(uint32_t iters)
{
    /* entry, data count, [data offsets], code count, [code offsets], code */
    uint32_t seedw[] = {4, 2, 0, 8, 1, 4, 0x11111111, 0x22222222, 0x33333333, 0x44444444};
    for (uint32_t it = 0; it < iters; it++) {
        size_t len;
        uint8_t *in = mutate((const uint8_t *)seedw, sizeof(seedw), &len);
        BEGIN_INPUT(in, len);
        purr_module_layout_t lay;
        if (purr_module_parse_layout(in, (uint32_t)len, &lay) == PURR_MOD_LAYOUT_OK) {
            uint32_t *d = malloc(lay.data_reloc_count * sizeof(uint32_t) + 1);
            uint32_t *c = malloc(lay.code_reloc_count * sizeof(uint32_t) + 1);
            purr_module_read_data_relocs(in, &lay, d);
            purr_module_read_code_relocs(in, &lay, c);
            uint8_t *code = malloc(lay.code_size + 1);
            memcpy(code, in + lay.code_offset, lay.code_size);
            (void)purr_relocate(code, lay.code_size, d, lay.data_reloc_count, 0x3C000000u);
            (void)purr_relocate(code, lay.code_size, c, lay.code_reloc_count, 0x42000000u);
            free(code); free(d); free(c);
        }
        free(in);
    }
}

static void fuzz_manifest(uint32_t iters)
{
    static const char seed[] =
        "release=1.2.0\nreleased=2026-09-27\n\ncomponent=kernel\nversion=1.2.0\nchip=esp32s3\n"
        "board=tdeck_plus\nfile=kernel-1.2.0.cat\nsize=245760\n"
        "sha256=abababababababababababababababababababababababababababababababab\nkey=boot\n"
        "payload_size=1000\n"
        "payload_sha256=cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd\n"
        "min_bootloader=1.0.0\n\ncomponent=kittenos\nversion=1.2.0\nfile=k.kitt\nsize=99\n";
    for (uint32_t it = 0; it < iters; it++) {
        size_t len;
        uint8_t *in = mutate((const uint8_t *)seed, sizeof(seed) - 1, &len);   /* no NUL terminator */
        BEGIN_INPUT(in, len);
        purr_manifest_t man;
        memset(&man, 0, sizeof(man));
        (void)purr_manifest_parse(&man, (const char *)in, len);
        (void)purr_manifest_find(&man, "kernel", "esp32s3", "tdeck_plus");
        free(in);
    }
}

typedef struct { uint8_t data[2 * PURR_CFG_SECTOR_SIZE]; } fakeflash_t;

static int ff_read(void *c, uint32_t a, void *b, uint32_t n)
{
    fakeflash_t *f = c;
    if ((uint64_t)a + n > sizeof(f->data)) return -1;
    memcpy(b, f->data + a, n);
    return 0;
}
static int ff_erase(void *c, uint32_t a)
{
    fakeflash_t *f = c;
    if (a % PURR_CFG_SECTOR_SIZE || a + PURR_CFG_SECTOR_SIZE > sizeof(f->data)) return -1;
    memset(f->data + a, 0xFF, PURR_CFG_SECTOR_SIZE);
    return 0;
}
static int ff_write(void *c, uint32_t a, const void *b, uint32_t n)
{
    fakeflash_t *f = c;
    if ((uint64_t)a + n > sizeof(f->data)) return -1;
    memcpy(f->data + a, b, n);
    return 0;
}

static void fuzz_cfg(uint32_t iters)
{
    fakeflash_t seed;
    memset(seed.data, 0xFF, sizeof(seed.data));
    purr_cfg_t c;
    purr_cfg_defaults(&c);
    c.seq = 3;
    purr_cfg_seal(&c);
    memcpy(seed.data, &c, sizeof(c));
    c.seq = 4;
    purr_cfg_seal(&c);
    memcpy(seed.data + PURR_CFG_SECTOR_SIZE, &c, sizeof(c));

    for (uint32_t it = 0; it < iters; it++) {
        size_t len;
        uint8_t *in = mutate(seed.data, sizeof(seed.data), &len);
        BEGIN_INPUT(in, len);
        fakeflash_t ff;
        memset(ff.data, 0xFF, sizeof(ff.data));
        memcpy(ff.data, in, len < sizeof(ff.data) ? len : sizeof(ff.data));
        free(in);
        purr_flash_t fl = {ff_read, ff_erase, ff_write, &ff, 0};
        purr_cfg_t out;
        int which = -1;
        if (purr_cfg_load(&fl, &out, &which) >= 0) {
            purr_keybag_t bag;
            purr_key_t d[2];
            memset(d, 0, sizeof(d));
            d[0].key_id = 1; d[0].role = PURR_ROLE_BOOT;
            for (int k = 0; k < PURR_PUBKEY_LEN; k++) d[0].pub[k] = (uint8_t)(k * 3 + 1);
            purr_keybag_build(&bag, d, 0, &out);          /* override slots from a hostile purrcfg */
            /* the trust evaluation reads the signed block and the flags of a hostile purrcfg */
            purr_trust_t tr;
            purr_trust_eval(&out, d, 1, &g_stub, &tr);
            purr_cfg_t view;
            purr_trust_view(&out, &tr, &view);
            purr_keybag_build(&bag, d, 1, &view);
            (void)purr_trust_state(&tr);
            (void)purr_cfg_store(&fl, &view);             /* must refuse: a view is never stored */
            (void)purr_cfg_store(&fl, &out);
        }
    }
}

static void fuzz_wifi(uint32_t iters)
{
    static const char seed[] = "home\thunter2\ncafe\t\noffice wifi\ts3cret!\nx\ty\n";
    for (uint32_t it = 0; it < iters; it++) {
        size_t len;
        uint8_t *in = mutate((const uint8_t *)seed, sizeof(seed) - 1, &len);
        BEGIN_INPUT(in, len);
        purr_wifi_list_t l;
        purr_wifi_list_init(&l);
        purr_wifi_list_parse(&l, (const char *)in, len);
        char out[512];
        (void)purr_wifi_list_format(&l, out, sizeof(out));
        (void)purr_wifi_list_find(&l, "home");
        free(in);
    }
}

typedef struct { const char *name; void (*fn)(uint32_t); } target_t;
static const target_t k_targets[] = {
    {"verify", fuzz_verify}, {"trailer", fuzz_trailer}, {"module", fuzz_module},
    {"manifest", fuzz_manifest}, {"cfg", fuzz_cfg}, {"wifi", fuzz_wifi},
};

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "all";
    uint32_t iters = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 100000u;
    uint64_t seed = argc > 3 ? strtoull(argv[3], NULL, 0) : 1;
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef HAVE_DEATH_CB
    __sanitizer_set_death_callback(on_death);
#endif
    setup_bag();
    int ran = 0;
    for (size_t i = 0; i < sizeof(k_targets) / sizeof(k_targets[0]); i++) {
        if (strcmp(which, "all") && strcmp(which, k_targets[i].name)) continue;
        g_target = k_targets[i].name;
        g_rng = 0x9E3779B97F4A7C15ull ^ (seed * 0xD6E8FEB86659FD93ull);
        printf("fuzz %-9s %u iterations, seed %llu ... ", g_target, (unsigned)iters, (unsigned long long)seed);
        k_targets[i].fn(iters);
        printf("ok\n");
        ran++;
    }
    if (!ran) { fprintf(stderr, "unknown target %s\n", which); return 2; }
    return 0;
}
