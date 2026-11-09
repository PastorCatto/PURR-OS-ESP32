/* test_safeload.c - a file that exists but cannot be read is never treated as empty (the account list, the saved Wi-Fi and the settings were once replaced by
 * defaults that way: plan7.md). purr_fs_slurp, purr_fs_write_kept and the account store, against a RAM disk whose reads can be made to fail. */
#include <stdlib.h>
#include <string.h>

#include "purr_fs.h"
#include "purr_userstore.h"
#include "testkit.h"

#define BLOCKS 64
#define BSIZE  4096

static uint8_t s_ram[BLOCKS * BSIZE];
static int s_fail_reads;                       /* the next N reads fail */
static int s_fail_all;                         /* every read fails */

static int ram_read(void *ctx, uint32_t block, uint32_t off, void *buf, uint32_t size)
{
    (void)ctx;
    if (s_fail_all) return -5;
    if (s_fail_reads > 0) { s_fail_reads--; return -5; }
    if (block >= BLOCKS || off + size > BSIZE) return -1;
    memcpy(buf, s_ram + block * BSIZE + off, size);
    return 0;
}

static int ram_prog(void *ctx, uint32_t block, uint32_t off, const void *buf, uint32_t size)
{
    (void)ctx;
    if (block >= BLOCKS || off + size > BSIZE) return -1;
    uint8_t *dst = s_ram + block * BSIZE + off;
    const uint8_t *src = buf;
    for (uint32_t i = 0; i < size; i++) dst[i] &= src[i];
    return 0;
}

static int ram_erase(void *ctx, uint32_t block)
{
    (void)ctx;
    if (block >= BLOCKS) return -1;
    memset(s_ram + block * BSIZE, 0xFF, BSIZE);
    return 0;
}

static purr_fs_t fs;

static void fresh(void)
{
    memset(s_ram, 0xFF, sizeof(s_ram));
    s_fail_reads = s_fail_all = 0;
    purr_bd_t b = {ram_read, ram_prog, ram_erase, NULL, NULL, BSIZE, BLOCKS};
    CHECK_EQ(purr_fs_format(&fs, &b), 0);
}

static void make_accounts(purr_user_list_t *u, purr_shadow_list_t *sh)
{
    uint8_t salt[PURR_SALT_LEN];
    memset(salt, 7, sizeof(salt));
    purr_user_list_init(u);
    purr_shadow_list_init(sh);
    CHECK(purr_user_list_add(u, "Catto", 1, PURR_ROLE_USER_ADMIN) >= 0);
    CHECK(purr_user_list_add(u, "guest", 2, PURR_ROLE_USER_STANDARD) >= 0);
    purr_shadow_set(sh, "Catto", "secret-one", salt, 1000);
    purr_shadow_set(sh, "guest", "secret-two", salt, 1000);
}

static void test_slurp_tells_absent_from_unreadable(void)
{
    char buf[64];
    uint32_t n = 99;
    fresh();
    CHECK_EQ(purr_fs_slurp(&fs, "/etc/none", buf, sizeof(buf), &n), 1);               /* the only case that means "nothing saved yet" */
    CHECK_EQ(n, 0);
    purr_fs_mkdir(&fs, "/etc");
    CHECK_EQ(purr_fs_write(&fs, "/etc/f", "hello", 5), 0);
    CHECK_EQ(purr_fs_slurp(&fs, "/etc/f", buf, sizeof(buf), &n), 0);
    CHECK_EQ(n, 5);
    CHECK(strcmp(buf, "hello") == 0);
    CHECK_EQ(purr_fs_slurp(&fs, "/etc/f", buf, 4, &n), -1);                            /* does not fit: an error, not a short file */
    s_fail_reads = 1;                                                                  /* one transient failure is absorbed by the retry */
    CHECK_EQ(purr_fs_slurp(&fs, "/etc/f", buf, sizeof(buf), &n), 0);
    CHECK(strcmp(buf, "hello") == 0);
    s_fail_all = 1;                                                                    /* a failure that stays is -1, never "absent" */
    CHECK_EQ(purr_fs_slurp(&fs, "/etc/f", buf, sizeof(buf), &n), -1);
    CHECK_EQ(purr_fs_slurp(&fs, "/etc/none", buf, sizeof(buf), &n), -1);
    s_fail_all = 0;
}

static void test_write_kept_keeps_the_previous_file(void)
{
    char buf[64];
    uint32_t n;
    fresh();
    purr_fs_mkdir(&fs, "/etc");
    CHECK_EQ(purr_fs_write_kept(&fs, "/etc/c", "one", 3), 0);
    CHECK_EQ(purr_fs_slurp(&fs, "/etc/c.bak", buf, sizeof(buf), &n), 1);               /* nothing to keep the first time */
    CHECK_EQ(purr_fs_write_kept(&fs, "/etc/c", "two", 3), 0);
    CHECK_EQ(purr_fs_slurp(&fs, "/etc/c", buf, sizeof(buf), &n), 0);
    CHECK(strcmp(buf, "two") == 0);
    CHECK_EQ(purr_fs_slurp(&fs, "/etc/c.bak", buf, sizeof(buf), &n), 0);
    CHECK(strcmp(buf, "one") == 0);
}

static void test_the_account_store(void)
{
    purr_user_list_t u, lu;
    purr_shadow_list_t sh, lsh;
    fresh();
    purr_fs_mkdir(&fs, "/etc");
    CHECK_EQ(purr_userstore_load(&fs, &lu, &lsh), 0);                                   /* nothing saved: "no accounts yet" is right */
    CHECK_EQ(lu.count, 0);
    make_accounts(&u, &sh);
    CHECK_EQ(purr_userstore_save(&fs, &u, &sh), 0);
    CHECK_EQ(purr_userstore_load(&fs, &lu, &lsh), 0);
    CHECK_EQ(lu.count, 2);
    CHECK(purr_shadow_check(&lsh, "Catto", "secret-one"));

    s_fail_all = 1;                                                                    /* the files are there but cannot be read: NOT "no accounts yet" */
    CHECK_EQ(purr_userstore_load(&fs, &lu, &lsh), PURR_USERSTORE_READ_ERROR);
    CHECK_EQ(lu.count, 0);
    s_fail_all = 0;
    CHECK_EQ(purr_userstore_load(&fs, &lu, &lsh), 0);                                   /* and when they can be again, the accounts are all there */
    CHECK_EQ(lu.count, 2);
}

static void test_a_damaged_list_falls_back_to_the_backup(void)
{
    purr_user_list_t u, lu;
    purr_shadow_list_t sh, lsh;
    fresh();
    purr_fs_mkdir(&fs, "/etc");
    make_accounts(&u, &sh);
    CHECK_EQ(purr_userstore_save(&fs, &u, &sh), 0);
    CHECK_EQ(purr_userstore_save(&fs, &u, &sh), 0);                                     /* the second save keeps the first as .bak */
    CHECK_EQ(purr_fs_write(&fs, "/etc/passwd", "garbage without any account\n", 28), 0); /* text, but not an account in it */
    CHECK_EQ(purr_userstore_load(&fs, &lu, &lsh), PURR_USERSTORE_FROM_BACKUP);
    CHECK_EQ(lu.count, 2);
    CHECK(purr_shadow_check(&lsh, "guest", "secret-two"));
    CHECK_EQ(purr_fs_write(&fs, "/etc/passwd.bak", "also garbage\n", 13), 0);           /* both damaged: an error, not an empty list */
    CHECK_EQ(purr_userstore_load(&fs, &lu, &lsh), PURR_USERSTORE_READ_ERROR);
    CHECK_EQ(lu.count, 0);
}

int main(void)
{
    test_slurp_tells_absent_from_unreadable();
    test_write_kept_keeps_the_previous_file();
    test_the_account_store();
    test_a_damaged_list_falls_back_to_the_backup();
    TK_DONE("test_safeload");
}
