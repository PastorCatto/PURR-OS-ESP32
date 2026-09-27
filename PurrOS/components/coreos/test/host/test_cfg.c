#include <stdlib.h>
#include <string.h>

#include "purr_cfg.h"
#include "testkit.h"

/* A fake flash: two sectors that can be made to lose power mid-operation. */
typedef struct {
    uint8_t data[PURR_CFG_SECTORS * PURR_CFG_SECTOR_SIZE];
    int ops;                 /* operations done so far (erase or write) */
    int fail_at;             /* the operation that loses power, -1 = never */
    uint32_t partial;        /* bytes of a failing write that reach the flash */
    int dead;                /* power is off: every later call fails */
} fake_t;

static void fake_init(fake_t *f)
{
    memset(f->data, 0xFF, sizeof(f->data));
    f->ops = 0;
    f->fail_at = -1;
    f->partial = 0;
    f->dead = 0;
}

static int f_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
    fake_t *f = ctx;
    if (addr + len > sizeof(f->data)) {
        return -1;
    }
    memcpy(buf, f->data + addr, len);
    return 0;
}

static int f_erase(void *ctx, uint32_t addr)
{
    fake_t *f = ctx;
    if (f->dead) {
        return -1;
    }
    if (f->ops++ == f->fail_at) {
        /* Power lost during an erase: the sector is left in an undefined state. */
        for (uint32_t i = 0; i < PURR_CFG_SECTOR_SIZE; i += 3) {
            f->data[addr + i] = (uint8_t)(i * 7);
        }
        f->dead = 1;
        return -1;
    }
    memset(f->data + addr, 0xFF, PURR_CFG_SECTOR_SIZE);
    return 0;
}

static int f_write(void *ctx, uint32_t addr, const void *buf, uint32_t len)
{
    fake_t *f = ctx;
    if (f->dead) {
        return -1;
    }
    if (f->ops++ == f->fail_at) {
        uint32_t n = f->partial < len ? f->partial : len;
        memcpy(f->data + addr, buf, n);      /* a torn write */
        f->dead = 1;
        return -1;
    }
    memcpy(f->data + addr, buf, len);
    return 0;
}

static purr_flash_t flash_of(fake_t *f)
{
    purr_flash_t fl = {f_read, f_erase, f_write, f, 0};
    return fl;
}

static void test_defaults_and_valid(void)
{
    purr_cfg_t c;
    purr_cfg_defaults(&c);
    CHECK(purr_cfg_valid(&c));
    CHECK_EQ(c.secure_mode, PURR_SECURE_WARN);
    CHECK_EQ(c.boot_target, PURR_TARGET_NORMAL);

    c.flags = 1;                       /* any change breaks the CRC */
    CHECK(!purr_cfg_valid(&c));
    purr_cfg_seal(&c);
    CHECK(purr_cfg_valid(&c));

    c.magic ^= 1;
    purr_cfg_seal(&c);
    CHECK(!purr_cfg_valid(&c));        /* a good CRC does not fix a bad magic */
}

static void test_empty_flash_gives_defaults(void)
{
    fake_t f;
    purr_cfg_t c;
    int which;

    fake_init(&f);
    purr_flash_t fl = flash_of(&f);
    CHECK_EQ(purr_cfg_load(&fl, &c, &which), 1);
    CHECK_EQ(which, -1);
    CHECK_EQ(c.secure_mode, PURR_SECURE_WARN);
}

static void test_alternating_copies(void)
{
    fake_t f;
    purr_cfg_t c, r;
    int which;

    fake_init(&f);
    purr_flash_t fl = flash_of(&f);
    purr_cfg_defaults(&c);

    c.secure_mode = PURR_SECURE_OFF;
    CHECK_EQ(purr_cfg_store(&fl, &c), 0);
    CHECK_EQ(c.seq, 1);
    CHECK_EQ(purr_cfg_load(&fl, &r, &which), 0);
    CHECK_EQ(which, 0);                  /* the first store goes to A */
    CHECK_EQ(r.secure_mode, PURR_SECURE_OFF);

    c.secure_mode = PURR_SECURE_ENFORCE;
    CHECK_EQ(purr_cfg_store(&fl, &c), 0);
    CHECK_EQ(c.seq, 2);
    CHECK_EQ(purr_cfg_load(&fl, &r, &which), 0);
    CHECK_EQ(which, 1);                  /* then B */
    CHECK_EQ(r.secure_mode, PURR_SECURE_ENFORCE);

    c.boot_fail_count = 3;
    CHECK_EQ(purr_cfg_store(&fl, &c), 0);
    CHECK_EQ(purr_cfg_load(&fl, &r, &which), 0);
    CHECK_EQ(which, 0);                  /* and back to A */
    CHECK_EQ(r.seq, 3);
    CHECK_EQ(r.boot_fail_count, 3);
}

static void test_corrupt_newest_falls_back(void)
{
    fake_t f;
    purr_cfg_t c, r;
    int which;

    fake_init(&f);
    purr_flash_t fl = flash_of(&f);
    purr_cfg_defaults(&c);
    c.secure_mode = PURR_SECURE_OFF;
    purr_cfg_store(&fl, &c);              /* seq 1 in A */
    c.secure_mode = PURR_SECURE_ENFORCE;
    purr_cfg_store(&fl, &c);              /* seq 2 in B */

    f.data[PURR_CFG_SECTOR_SIZE + 20] ^= 0x55;   /* corrupt B */
    CHECK_EQ(purr_cfg_load(&fl, &r, &which), 0);
    CHECK_EQ(which, 0);
    CHECK_EQ(r.secure_mode, PURR_SECURE_OFF);    /* the older, intact copy */

    f.data[20] ^= 0x55;                            /* corrupt A too */
    CHECK_EQ(purr_cfg_load(&fl, &r, &which), 1);   /* defaults */
    CHECK_EQ(which, -1);
}

/*
 * Power loss. For every operation of a store, and for several sizes of torn
 * write, cut the power there, reboot, and load: the result must be the old
 * config or the new one, never anything else.
 */
static void test_power_loss(void)
{
    uint32_t partials[] = {0, 1, 100, 200, sizeof(purr_cfg_t) - 1, sizeof(purr_cfg_t)};
    int trials = 0;

    for (int history = 0; history < 4; history++) {         /* 0..3 stores before the cut */
        for (int op = 0; op < 2; op++) {                     /* the erase, then the write */
            for (unsigned p = 0; p < sizeof(partials) / sizeof(partials[0]); p++) {
                fake_t f;
                purr_cfg_t old_cfg, next, r;

                fake_init(&f);
                purr_flash_t fl = flash_of(&f);
                purr_cfg_defaults(&old_cfg);
                for (int i = 0; i < history; i++) {
                    old_cfg.boot_fail_count = (uint32_t)i + 10;
                    CHECK_EQ(purr_cfg_store(&fl, &old_cfg), 0);
                }

                next = old_cfg;
                next.boot_fail_count = 999;
                f.ops = 0;
                f.fail_at = op;
                f.partial = partials[p];
                int rc = purr_cfg_store(&fl, &next);
                CHECK_EQ(rc, -1);

                /* Power comes back. */
                f.dead = 0;
                f.fail_at = -1;
                int lrc = purr_cfg_load(&fl, &r, NULL);
                CHECK(lrc == 0 || lrc == 1);
                if (history == 0) {
                    /* Nothing was stored yet: defaults, or the new copy if it survived whole. */
                    CHECK(r.boot_fail_count == 0 || r.boot_fail_count == 999);
                } else {
                    CHECK(r.boot_fail_count == old_cfg.boot_fail_count || r.boot_fail_count == 999);
                }
                CHECK(purr_cfg_valid(&r));
                trials++;

                /* And the system can go on: a store after the cut works. */
                next.boot_fail_count = 1234;
                CHECK_EQ(purr_cfg_store(&fl, &next), 0);
                CHECK_EQ(purr_cfg_load(&fl, &r, NULL), 0);
                CHECK_EQ(r.boot_fail_count, 1234);
            }
        }
    }
    CHECK_EQ(trials, 4 * 2 * 6);
}

static int silent_write(void *ctx, uint32_t a, const void *b, uint32_t l)
{
    (void)ctx; (void)a; (void)b; (void)l;
    return 0;
}

static void test_readback_failure(void)
{
    fake_t f;
    purr_cfg_t c;

    fake_init(&f);
    purr_flash_t fl = flash_of(&f);
    purr_cfg_defaults(&c);
    /* A flash that silently drops the write is caught by the read back. */
    fl.write = silent_write;
    CHECK_EQ(purr_cfg_store(&fl, &c), -1);
}

/* The one-shot flags, and the round trip `reboot recovery` and the bootloader make. */
static void test_flags(void)
{
    purr_cfg_t c;
    purr_cfg_defaults(&c);
    CHECK_EQ(purr_cfg_take_flag(&c, PURR_CFGF_FORCE_RECOVERY), 0);   /* not set: nothing to take */

    CHECK_EQ(purr_cfg_set_flag(&c, PURR_CFGF_FORCE_RECOVERY), 1);
    CHECK_EQ(purr_cfg_set_flag(&c, PURR_CFGF_FORCE_RECOVERY), 0);    /* already set */
    CHECK(c.flags & PURR_CFGF_FORCE_RECOVERY);

    /* Other flags are not disturbed. */
    purr_cfg_set_flag(&c, PURR_CFGF_IGNORE_ONCE);
    CHECK_EQ(purr_cfg_take_flag(&c, PURR_CFGF_FORCE_RECOVERY), 1);
    CHECK_EQ(purr_cfg_take_flag(&c, PURR_CFGF_FORCE_RECOVERY), 0);   /* only once */
    CHECK(!(c.flags & PURR_CFGF_FORCE_RECOVERY));
    CHECK(c.flags & PURR_CFGF_IGNORE_ONCE);
}

static void test_recovery_round_trip(void)
{
    fake_t f;
    fake_init(&f);
    purr_flash_t fl = flash_of(&f);

    /* The shell: load, set the flag, store. */
    purr_cfg_t c;
    CHECK_EQ(purr_cfg_load(&fl, &c, NULL), 1);        /* blank: defaults */
    CHECK_EQ(purr_cfg_set_flag(&c, PURR_CFGF_FORCE_RECOVERY), 1);
    CHECK_EQ(purr_cfg_store(&fl, &c), 0);

    /* The bootloader, after a restart: load, see the flag, clear it, store. */
    purr_cfg_t b;
    CHECK_EQ(purr_cfg_load(&fl, &b, NULL), 0);
    CHECK_EQ(purr_cfg_take_flag(&b, PURR_CFGF_FORCE_RECOVERY), 1);
    CHECK_EQ(purr_cfg_store(&fl, &b), 0);

    /* The next boot finds nothing to do: recovery was for one boot only. */
    purr_cfg_t n;
    CHECK_EQ(purr_cfg_load(&fl, &n, NULL), 0);
    CHECK_EQ(purr_cfg_take_flag(&n, PURR_CFGF_FORCE_RECOVERY), 0);
    CHECK(n.seq > b.seq - 1 && b.seq > c.seq - 1);    /* seq only goes up */
}

int main(void)
{
    test_defaults_and_valid();
    test_empty_flash_gives_defaults();
    test_alternating_copies();
    test_corrupt_newest_falls_back();
    test_power_loss();
    test_readback_failure();
    test_flags();
    test_recovery_round_trip();
    TK_DONE("test_cfg");
}
