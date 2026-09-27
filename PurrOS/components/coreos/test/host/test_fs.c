#include <stdlib.h>
#include <string.h>

#include "purr_fs.h"
#include "testkit.h"

/* A RAM disk that behaves like flash: erase sets bytes to 0xFF, and programming can only
 * clear bits. That catches code that forgets to erase. */
#define BLOCKS 64
#define BSIZE  4096

static uint8_t s_ram[BLOCKS * BSIZE];
static int s_erases;

static int ram_read(void *ctx, uint32_t block, uint32_t off, void *buf, uint32_t size)
{
    (void)ctx;
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
    for (uint32_t i = 0; i < size; i++) {
        dst[i] &= src[i];                         /* flash can only turn 1s into 0s */
    }
    return 0;
}

static int ram_erase(void *ctx, uint32_t block)
{
    (void)ctx;
    if (block >= BLOCKS) return -1;
    memset(s_ram + block * BSIZE, 0xFF, BSIZE);
    s_erases++;
    return 0;
}

static purr_bd_t bd(void)
{
    purr_bd_t b = {ram_read, ram_prog, ram_erase, NULL, NULL, BSIZE, BLOCKS};
    return b;
}

static void blank(void)
{
    memset(s_ram, 0xFF, sizeof(s_ram));
    s_erases = 0;
}

static purr_fs_t fs;

/* ---------------------------------------------------------------- listing helpers */

typedef struct {
    char names[64][40];
    int dirs[64];
    uint32_t sizes[64];
    int n;
} listing_t;

static void collect(void *ctx, const char *name, int is_dir, uint32_t size)
{
    listing_t *l = ctx;
    if (l->n < 64) {
        snprintf(l->names[l->n], sizeof(l->names[0]), "%s", name);
        l->dirs[l->n] = is_dir;
        l->sizes[l->n] = size;
        l->n++;
    }
}

static int find(const listing_t *l, const char *name)
{
    for (int i = 0; i < l->n; i++) {
        if (strcmp(l->names[i], name) == 0) return i;
    }
    return -1;
}

typedef struct {
    uint8_t data[40000];
    uint32_t len;
} sink_t;

static int append(void *ctx, const void *data, uint32_t len)
{
    sink_t *s = ctx;
    if (s->len + len > sizeof(s->data)) return -1;
    memcpy(s->data + s->len, data, len);
    s->len += len;
    return 0;
}

static int stop_after_first(void *ctx, const void *data, uint32_t len)
{
    (void)data;
    (*(uint32_t *)ctx) += len;
    return 7;
}

/* ---------------------------------------------------------------- tests */

static void test_mount_and_format(void)
{
    blank();
    purr_bd_t b = bd();
    CHECK(purr_fs_mount(&fs, &b) < 0);            /* a blank device is not a filesystem */
    CHECK(!purr_fs_mounted(&fs));
    CHECK(strstr(purr_fs_strerror(purr_fs_mount(&fs, &b)), "no valid") != NULL);

    CHECK_EQ(purr_fs_format(&fs, &b), 0);
    CHECK(purr_fs_mounted(&fs));
    purr_fs_unmount(&fs);
    CHECK(!purr_fs_mounted(&fs));
    CHECK_EQ(purr_fs_mount(&fs, &b), 0);          /* and it mounts afterwards */

    /* Bad block devices are refused, not crashed on. */
    purr_bd_t bad = b;
    bad.block_size = 100;
    CHECK(purr_fs_mount(&fs, &bad) < 0);
    bad = b;
    bad.read = NULL;
    CHECK(purr_fs_format(&fs, &bad) < 0);
    bad = b;
    bad.block_count = 1;
    CHECK(purr_fs_mount(&fs, &bad) < 0);
}

static void test_unmounted_is_refused(void)
{
    blank();
    purr_bd_t b = bd();
    purr_fs_mount(&fs, &b);                       /* fails, stays unmounted */
    listing_t l = {0};
    CHECK(purr_fs_mkdir(&fs, "/x") < 0);
    CHECK(purr_fs_write(&fs, "/x", "a", 1) < 0);
    CHECK(purr_fs_list(&fs, "/", collect, &l) < 0);
    CHECK(purr_fs_remove(&fs, "/x") < 0);
    CHECK(purr_fs_rename(&fs, "/x", "/y") < 0);
    CHECK(purr_fs_stat(&fs, "/x", NULL, NULL) < 0);
    uint32_t u, t;
    CHECK(purr_fs_usage(&fs, &u, &t) < 0);
}

static void test_files_and_dirs(void)
{
    blank();
    purr_bd_t b = bd();
    CHECK_EQ(purr_fs_format(&fs, &b), 0);

    CHECK_EQ(purr_fs_mkdir(&fs, "/boot"), 0);
    CHECK_EQ(purr_fs_mkdir(&fs, "/etc"), 0);
    CHECK_EQ(purr_fs_write(&fs, "/etc/name", "kitten", 6), 0);
    CHECK_EQ(purr_fs_write(&fs, "/boot/empty", "", 0), 0);

    listing_t l = {0};
    CHECK_EQ(purr_fs_list(&fs, "/", collect, &l), 0);
    CHECK_EQ(l.n, 2);                             /* "." and ".." are not shown */
    CHECK(find(&l, "boot") >= 0 && l.dirs[find(&l, "boot")]);
    CHECK(find(&l, "etc") >= 0 && l.dirs[find(&l, "etc")]);

    memset(&l, 0, sizeof(l));
    CHECK_EQ(purr_fs_list(&fs, "/etc", collect, &l), 0);
    CHECK_EQ(l.n, 1);
    CHECK(strcmp(l.names[0], "name") == 0);
    CHECK_EQ(l.dirs[0], 0);
    CHECK_EQ(l.sizes[0], 6);

    static sink_t s;
    s.len = 0;
    CHECK_EQ(purr_fs_read(&fs, "/etc/name", append, &s), 0);
    CHECK_EQ(s.len, 6);
    CHECK(memcmp(s.data, "kitten", 6) == 0);

    s.len = 0;
    CHECK_EQ(purr_fs_read(&fs, "/boot/empty", append, &s), 0);
    CHECK_EQ(s.len, 0);

    int is_dir = -1;
    uint32_t size = 99;
    CHECK_EQ(purr_fs_stat(&fs, "/etc/name", &is_dir, &size), 0);
    CHECK_EQ(is_dir, 0);
    CHECK_EQ(size, 6);
    CHECK_EQ(purr_fs_stat(&fs, "/etc", &is_dir, NULL), 0);
    CHECK_EQ(is_dir, 1);
}

static void test_errors(void)
{
    blank();
    purr_bd_t b = bd();
    purr_fs_format(&fs, &b);
    static sink_t s;
    s.len = 0;

    CHECK_EQ(purr_fs_read(&fs, "/nope", append, &s), LFS_ERR_NOENT);
    CHECK_EQ(purr_fs_stat(&fs, "/nope", NULL, NULL), LFS_ERR_NOENT);
    CHECK_EQ(purr_fs_remove(&fs, "/nope"), LFS_ERR_NOENT);
    CHECK_EQ(purr_fs_write(&fs, "/no/dir/file", "x", 1), LFS_ERR_NOENT);
    CHECK_EQ(purr_fs_list(&fs, "/nope", collect, &s), LFS_ERR_NOENT);

    purr_fs_mkdir(&fs, "/d");
    CHECK_EQ(purr_fs_mkdir(&fs, "/d"), LFS_ERR_EXIST);
    purr_fs_write(&fs, "/d/f", "x", 1);
    CHECK_EQ(purr_fs_remove(&fs, "/d"), LFS_ERR_NOTEMPTY);   /* not while it has files */
    CHECK_EQ(purr_fs_read(&fs, "/d", append, &s), LFS_ERR_ISDIR);
    CHECK_EQ(purr_fs_list(&fs, "/d/f", collect, &s), LFS_ERR_NOTDIR);
    CHECK_EQ(purr_fs_remove(&fs, "/d/f"), 0);
    CHECK_EQ(purr_fs_remove(&fs, "/d"), 0);                  /* fine once empty */

    char longname[400];
    memset(longname, 'a', sizeof(longname) - 1);
    longname[0] = '/';
    longname[sizeof(longname) - 1] = '\0';
    CHECK_EQ(purr_fs_write(&fs, longname, "x", 1), LFS_ERR_NAMETOOLONG);

    /* A reader that stops early gets its own value back, and the file is closed properly. */
    purr_fs_write(&fs, "/f", "abcdef", 6);
    uint32_t seen = 0;
    CHECK_EQ(purr_fs_read(&fs, "/f", stop_after_first, &seen), 7);
    CHECK_EQ(seen, 6);
    CHECK_EQ(purr_fs_write(&fs, "/f", "ok", 2), 0);          /* still usable afterwards */
}

static void test_overwrite_and_rename(void)
{
    blank();
    purr_bd_t b = bd();
    purr_fs_format(&fs, &b);
    static sink_t s;

    purr_fs_write(&fs, "/a", "first version", 13);
    purr_fs_write(&fs, "/a", "v2", 2);            /* replaces, does not append */
    s.len = 0;
    purr_fs_read(&fs, "/a", append, &s);
    CHECK_EQ(s.len, 2);
    CHECK(memcmp(s.data, "v2", 2) == 0);

    /* The update swap: current -> .bak, staged .new -> current. */
    purr_fs_write(&fs, "/kernel", "old", 3);
    purr_fs_write(&fs, "/kernel.new", "new", 3);
    CHECK_EQ(purr_fs_rename(&fs, "/kernel", "/kernel.bak"), 0);
    CHECK_EQ(purr_fs_rename(&fs, "/kernel.new", "/kernel"), 0);
    s.len = 0;
    purr_fs_read(&fs, "/kernel", append, &s);
    CHECK(s.len == 3 && memcmp(s.data, "new", 3) == 0);
    s.len = 0;
    purr_fs_read(&fs, "/kernel.bak", append, &s);
    CHECK(s.len == 3 && memcmp(s.data, "old", 3) == 0);
    CHECK_EQ(purr_fs_stat(&fs, "/kernel.new", NULL, NULL), LFS_ERR_NOENT);

    /* Renaming onto an existing file replaces it. */
    CHECK_EQ(purr_fs_rename(&fs, "/kernel.bak", "/kernel"), 0);
    s.len = 0;
    purr_fs_read(&fs, "/kernel", append, &s);
    CHECK(s.len == 3 && memcmp(s.data, "old", 3) == 0);
    CHECK_EQ(purr_fs_rename(&fs, "/missing", "/x"), LFS_ERR_NOENT);
}

static void test_big_file_and_persistence(void)
{
    blank();
    purr_bd_t b = bd();
    purr_fs_format(&fs, &b);

    static uint8_t big[30000];
    for (unsigned i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 7 + (i >> 8));
    CHECK_EQ(purr_fs_write(&fs, "/big.bin", big, sizeof(big)), 0);

    uint32_t used, total;
    CHECK_EQ(purr_fs_usage(&fs, &used, &total), 0);
    CHECK_EQ(total, BLOCKS);
    CHECK(used >= 8);                             /* 30000 bytes needs at least 8 blocks */
    CHECK(used < total);

    /* Unmount, mount again from the same bytes: everything is still there. */
    purr_fs_unmount(&fs);
    CHECK_EQ(purr_fs_mount(&fs, &b), 0);
    static sink_t s;
    s.len = 0;
    CHECK_EQ(purr_fs_read(&fs, "/big.bin", append, &s), 0);
    CHECK_EQ(s.len, sizeof(big));
    CHECK(memcmp(s.data, big, sizeof(big)) == 0);

    /* Deleting gives the space back. */
    uint32_t before = used;
    CHECK_EQ(purr_fs_remove(&fs, "/big.bin"), 0);
    CHECK_EQ(purr_fs_usage(&fs, &used, &total), 0);
    CHECK(used < before);
}

static void test_full_disk(void)
{
    blank();
    purr_bd_t b = bd();
    purr_fs_format(&fs, &b);
    static uint8_t chunk[8192];
    memset(chunk, 0x5A, sizeof(chunk));
    int wrote = 0, err = 0;
    for (int i = 0; i < 100 && err == 0; i++) {
        char name[16];
        snprintf(name, sizeof(name), "/f%d", i);
        err = purr_fs_write(&fs, name, chunk, sizeof(chunk));
        if (err == 0) wrote++;
    }
    CHECK(wrote > 5);
    CHECK_EQ(err, LFS_ERR_NOSPC);                 /* it stops cleanly, it does not corrupt */
    purr_fs_unmount(&fs);
    CHECK_EQ(purr_fs_mount(&fs, &b), 0);          /* and the filesystem is still sound */
    listing_t l = {0};
    CHECK_EQ(purr_fs_list(&fs, "/", collect, &l), 0);
    CHECK(l.n >= wrote);
}

static void test_format_erases(void)
{
    blank();
    purr_bd_t b = bd();
    purr_fs_format(&fs, &b);
    purr_fs_write(&fs, "/secret", "data", 4);
    purr_fs_format(&fs, &b);                      /* a format is a fresh start */
    CHECK_EQ(purr_fs_stat(&fs, "/secret", NULL, NULL), LFS_ERR_NOENT);
    listing_t l = {0};
    purr_fs_list(&fs, "/", collect, &l);
    CHECK_EQ(l.n, 0);
}

int main(void)
{
    test_mount_and_format();
    test_unmounted_is_refused();
    test_files_and_dirs();
    test_errors();
    test_overwrite_and_rename();
    test_big_file_and_persistence();
    test_full_disk();
    test_format_erases();
    TK_DONE("test_fs");
}
