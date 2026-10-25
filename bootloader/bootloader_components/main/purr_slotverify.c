// See purr_slotverify.h. The verification itself is PurrOS/components/coreos/src/purr_trailer.c,
// shared with CoreOS and tested on the PC; this file only supplies flash, keys and the hash.
#include <stdint.h>
#include <string.h>

#include "bootloader_flash_priv.h"
/* Our own lines stay at INFO even though the rest of the bootloader logs at WARN. */
#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "esp_log.h"
#include "sdkconfig.h"

#include "purr_abi.h"
#include "purr_crypto_uecc.h"
#include "purr_keybag.h"
#include "purr_slotverify.h"
#include "purr_trailer.h"

extern const purr_key_t purr_default_keys[];
extern const size_t purr_default_keys_count;

static const char *TAG = "purr_boot";

#if CONFIG_IDF_TARGET_ESP32S3
#define THIS_CHIP PURR_CHIP_ESP32S3
#else
#define THIS_CHIP PURR_CHIP_ESP32
#endif

/* ------------------------------------------------------------ hardware SHA-256 */

/* IDF's bootloader SHA (bootloader_support, private_include/bootloader_sha.h): the chip's SHA
 * block driven through the ROM. The header is private to that component, so the three functions
 * are declared here; they are ordinary global symbols. The block takes whole words only and
 * there is one shared context, so only one hash can be in flight -- fine, the verifier hashes
 * the payload of one image at a time. Measured on a T-Deck Plus (plan3.md item 2): software
 * SHA-256 over a 1.38 MB kernel slot took about 1.3 s at the bootloader's 80 MHz. */
extern void *bootloader_sha256_start(void);
extern void bootloader_sha256_data(void *handle, const void *data, size_t data_len);
extern void bootloader_sha256_finish(void *handle, uint8_t *digest);

static const purr_hash_t s_hw_hash = {
    .start = bootloader_sha256_start,
    .update = bootloader_sha256_data,
    .finish = bootloader_sha256_finish,
};

/* ------------------------------------------------------------ reading a slot */

// The flash reader wants whole words at aligned addresses, and the verifier asks for odd
// offsets and lengths (the payload starts 173 bytes into the virtual image) in 512-byte pieces,
// so reads go through an aligned read-ahead window: one flash read per 4 KB instead of one per
// 512 bytes (2,694 reads of ~0.12 ms each were 328 ms of a 1954 ms verify).
#define WINDOW_BYTES 4096
static uint32_t s_window[WINDOW_BYTES / sizeof(uint32_t)];

typedef struct {
    uint32_t base;        // partition offset in flash
    uint32_t size;        // partition size
    uint32_t win_start;   // partition offset of s_window[0]; win_len == 0 means empty
    uint32_t win_len;
} slot_ctx_t;

static int slot_read(void *vctx, uint32_t off, void *buf, uint32_t len)
{
    slot_ctx_t *c = vctx;
    uint8_t *dst = buf;

    if ((uint64_t)off + len > c->size) {
        return -1;
    }
    while (len > 0) {
        if (off < c->win_start || off >= c->win_start + c->win_len) {
            // refill: an aligned window starting at the word containing `off`
            uint32_t addr = c->base + off;
            uint32_t start = addr & ~3u;
            uint32_t avail = c->base + c->size - start;
            uint32_t n = avail < WINDOW_BYTES ? avail : WINDOW_BYTES;
            n = (n + 3u) & ~3u;
            if (bootloader_flash_read(start, s_window, n, false) != ESP_OK) {
                c->win_len = 0;
                return -1;
            }
            c->win_start = start - c->base;
            c->win_len = n;
        }
        uint32_t in_win = off - c->win_start;
        uint32_t take = c->win_len - in_win;
        if (take > len) {
            take = len;
        }
        // partition offsets are sector aligned, so window offsets and flash offsets agree mod 4
        memcpy(dst, (const uint8_t *)s_window + in_win, take);
        dst += take;
        off += take;
        len -= take;
    }
    return 0;
}

void purr_slot_verify(const esp_partition_pos_t *pos, purr_slot_kind_t kind,
                      const purr_cfg_t *cfg, purr_slot_verdict_t *out)
{
    uint8_t type = PURR_IMG_MODULE, subtype = PURR_MOD_KERNEL, comp = PURR_COMP_KERNEL;
    switch (kind) {
    case PURR_SLOT_KERNEL:   break;
    case PURR_SLOT_KITTENOS: type = PURR_IMG_RECOVERY; subtype = 0; comp = PURR_COMP_KITTENOS; break;
    case PURR_SLOT_LOADER:   subtype = PURR_MOD_LOADER; comp = PURR_COMP_LOADER; break;
    }

    // The newest version confirmed on this device for this slot (purrcfg floors; the OS raises
    // it when a boot of that slot reaches healthy). Always applied as "enforce" to the verifier:
    // an older, validly signed image is a failed verification, which warn mode boots with a
    // warning and enforce mode refuses, and off mode never reaches here. 0 = no floor yet.
    uint32_t floor = 0;
    for (int i = 0; i < PURR_FLOOR_SLOTS; i++) {
        if (cfg->floors[i].component == comp) {
            floor = cfg->floors[i].version;
        }
    }

    purr_keybag_t bag;
    purr_keybag_build(&bag, purr_default_keys, purr_default_keys_count, cfg);

    purr_verify_env_t env = {
        .chip_id = THIS_CHIP,
        .bag = &bag,
        .crypto = &purr_crypto_uecc,
        .bootloader_version = 0,   // the bootloader has no version scheme yet (as for bootpkg)
        .version_floor = floor,
        .enforce_floor = 1,
        .payload_hash = &s_hw_hash,
    };
    slot_ctx_t ctx = {.base = pos->offset, .size = pos->size, .win_start = 0, .win_len = 0};
    purr_image_header_t h;

    purr_verify_result_t r = purr_trailer_verify(&env, slot_read, &ctx, pos->size, type, subtype, &h);

    out->result = r;
    out->verified = (r == PURR_V_OK);
    out->key_id = out->verified ? h.key_id : 0xFF;
    ESP_LOGI(TAG, "slot @0x%x: %s", (unsigned)pos->offset, purr_verify_name(r));
}
