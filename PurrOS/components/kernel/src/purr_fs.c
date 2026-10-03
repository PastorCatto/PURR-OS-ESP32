#include "purr_fs.h"

#include <string.h>

/* ---------------------------------------------------------------- block device glue */

static int bd_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buf, lfs_size_t size)
{
    purr_fs_t *fs = c->context;
    return fs->bd.read(fs->bd.ctx, block, off, buf, size) == 0 ? LFS_ERR_OK : LFS_ERR_IO;
}

static int bd_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buf, lfs_size_t size)
{
    purr_fs_t *fs = c->context;
    return fs->bd.prog(fs->bd.ctx, block, off, buf, size) == 0 ? LFS_ERR_OK : LFS_ERR_IO;
}

static int bd_erase(const struct lfs_config *c, lfs_block_t block)
{
    purr_fs_t *fs = c->context;
    return fs->bd.erase(fs->bd.ctx, block) == 0 ? LFS_ERR_OK : LFS_ERR_IO;
}

static int bd_sync(const struct lfs_config *c)
{
    purr_fs_t *fs = c->context;
    return (fs->bd.sync == NULL || fs->bd.sync(fs->bd.ctx) == 0) ? LFS_ERR_OK : LFS_ERR_IO;
}

static int setup(purr_fs_t *fs, const purr_bd_t *bd)
{
    if (bd == NULL || bd->read == NULL || bd->prog == NULL || bd->erase == NULL ||
        bd->block_size < PURR_FS_CACHE || bd->block_size % PURR_FS_CACHE != 0 || bd->block_count < 2) {
        return LFS_ERR_INVAL;
    }
    memset(fs, 0, sizeof(*fs));
    fs->bd = *bd;
    fs->cfg = (struct lfs_config){
        .context = fs,
        .read = bd_read,
        .prog = bd_prog,
        .erase = bd_erase,
        .sync = bd_sync,
        .read_size = 16,
        .prog_size = 16,
        .block_size = bd->block_size,
        .block_count = bd->block_count,
        .block_cycles = 500,
        .cache_size = PURR_FS_CACHE,
        .lookahead_size = sizeof(fs->look_buf),
        .read_buffer = fs->read_buf,
        .prog_buffer = fs->prog_buf,
        .lookahead_buffer = fs->look_buf,
    };
    return 0;
}

int purr_fs_mount(purr_fs_t *fs, const purr_bd_t *bd)
{
    int e = setup(fs, bd);
    if (e < 0) {
        return e;
    }
    e = lfs_mount(&fs->lfs, &fs->cfg);
    fs->mounted = (e == 0);
    return e;
}

int purr_fs_format(purr_fs_t *fs, const purr_bd_t *bd)
{
    int e = setup(fs, bd);
    if (e < 0) {
        return e;
    }
    e = lfs_format(&fs->lfs, &fs->cfg);
    if (e < 0) {
        return e;
    }
    e = lfs_mount(&fs->lfs, &fs->cfg);
    fs->mounted = (e == 0);
    return e;
}

void purr_fs_unmount(purr_fs_t *fs)
{
    if (fs->mounted) {
        lfs_unmount(&fs->lfs);
        fs->mounted = 0;
    }
}

int purr_fs_mounted(const purr_fs_t *fs)
{
    return fs->mounted;
}

#define NEED_MOUNT(fs) do { if (!(fs)->mounted) return LFS_ERR_INVAL; } while (0)

/* ---------------------------------------------------------------- operations */

static int count_block(void *p, lfs_block_t block)
{
    (void)block;
    (*(uint32_t *)p)++;
    return 0;
}

int purr_fs_usage(purr_fs_t *fs, uint32_t *used_blocks, uint32_t *total_blocks)
{
    NEED_MOUNT(fs);
    uint32_t used = 0;
    int e = lfs_fs_traverse(&fs->lfs, count_block, &used);
    if (e < 0) {
        return e;
    }
    *used_blocks = used;
    *total_blocks = fs->bd.block_count;
    return 0;
}

int purr_fs_list(purr_fs_t *fs, const char *path, purr_fs_list_fn cb, void *ctx)
{
    NEED_MOUNT(fs);
    lfs_dir_t dir;
    int e = lfs_dir_open(&fs->lfs, &dir, path);
    if (e < 0) {
        return e;
    }
    struct lfs_info info;
    while ((e = lfs_dir_read(&fs->lfs, &dir, &info)) > 0) {
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }
        cb(ctx, info.name, info.type == LFS_TYPE_DIR, info.size);
    }
    lfs_dir_close(&fs->lfs, &dir);
    return e < 0 ? e : 0;
}

int purr_fs_read(purr_fs_t *fs, const char *path, purr_fs_read_fn cb, void *ctx)
{
    NEED_MOUNT(fs);
    lfs_file_t file;
    struct lfs_file_config fc = {.buffer = fs->file_buf};
    int e = lfs_file_opencfg(&fs->lfs, &file, path, LFS_O_RDONLY, &fc);
    if (e < 0) {
        return e;
    }
    uint8_t chunk[128];
    for (;;) {
        lfs_ssize_t n = lfs_file_read(&fs->lfs, &file, chunk, sizeof(chunk));
        if (n < 0) {
            e = (int)n;
            break;
        }
        if (n == 0) {
            e = 0;
            break;
        }
        e = cb(ctx, chunk, (uint32_t)n);
        if (e != 0) {
            break;
        }
    }
    lfs_file_close(&fs->lfs, &file);
    return e;
}

int purr_fs_write(purr_fs_t *fs, const char *path, const void *data, uint32_t len)
{
    NEED_MOUNT(fs);
    lfs_file_t file;
    struct lfs_file_config fc = {.buffer = fs->file_buf};
    int e = lfs_file_opencfg(&fs->lfs, &file, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &fc);
    if (e < 0) {
        return e;
    }
    if (len > 0) {
        lfs_ssize_t n = lfs_file_write(&fs->lfs, &file, data, len);
        if (n < 0) {
            lfs_file_close(&fs->lfs, &file);
            return (int)n;
        }
        if ((uint32_t)n != len) {
            lfs_file_close(&fs->lfs, &file);
            return LFS_ERR_NOSPC;
        }
    }
    return lfs_file_close(&fs->lfs, &file);
}

int purr_fs_mkdir(purr_fs_t *fs, const char *path)
{
    NEED_MOUNT(fs);
    return lfs_mkdir(&fs->lfs, path);
}

int purr_fs_remove(purr_fs_t *fs, const char *path)
{
    NEED_MOUNT(fs);
    return lfs_remove(&fs->lfs, path);
}

int purr_fs_rename(purr_fs_t *fs, const char *from, const char *to)
{
    NEED_MOUNT(fs);
    return lfs_rename(&fs->lfs, from, to);
}

int purr_fs_stat(purr_fs_t *fs, const char *path, int *is_dir, uint32_t *size)
{
    NEED_MOUNT(fs);
    struct lfs_info info;
    int e = lfs_stat(&fs->lfs, path, &info);
    if (e < 0) {
        return e;
    }
    if (is_dir) {
        *is_dir = (info.type == LFS_TYPE_DIR);
    }
    if (size) {
        *size = info.size;
    }
    return 0;
}

/* ---------------------------------------------------------------- ownership (F-10) */

#define OWNER_ATTR_TYPE 0x01

int purr_fs_get_owner(purr_fs_t *fs, const char *path, purr_fs_owner_t *out)
{
    NEED_MOUNT(fs);
    uint8_t buf[2];
    lfs_ssize_t n = lfs_getattr(&fs->lfs, path, OWNER_ATTR_TYPE, buf, sizeof(buf));
    if (n < 0) {
        return (int)n;
    }
    if (n != sizeof(buf)) {
        return LFS_ERR_NOATTR;       /* a stale/short attribute is as good as none */
    }
    out->owner_uid = buf[0];
    out->mode = buf[1];
    return 0;
}

int purr_fs_set_owner(purr_fs_t *fs, const char *path, const purr_fs_owner_t *owner)
{
    NEED_MOUNT(fs);
    uint8_t buf[2] = {owner->owner_uid, owner->mode};
    return lfs_setattr(&fs->lfs, path, OWNER_ATTR_TYPE, buf, sizeof(buf));
}

void purr_fs_effective_owner(purr_fs_t *fs, const char *path, purr_fs_owner_t *out)
{
    char buf[128];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    memcpy(buf, path, len);
    buf[len] = '\0';

    for (;;) {
        if (purr_fs_get_owner(fs, buf, out) == 0) {
            return;
        }
        char *slash = strrchr(buf, '/');
        if (slash == NULL) {
            break;
        }
        if (slash == buf) {
            /* buf is "/something" with no parent left except "/" itself. */
            if (purr_fs_get_owner(fs, "/", out) == 0) {
                return;
            }
            break;
        }
        *slash = '\0';
    }
    out->owner_uid = 0;
    out->mode = 0;
}

const char *purr_fs_strerror(int err)
{
    switch (err) {
    case LFS_ERR_OK:          return "ok";
    case LFS_ERR_IO:          return "I/O error";
    case LFS_ERR_CORRUPT:     return "no valid filesystem (blank or damaged)";
    case LFS_ERR_NOENT:       return "no such file or directory";
    case LFS_ERR_EXIST:       return "already exists";
    case LFS_ERR_NOTDIR:      return "not a directory";
    case LFS_ERR_ISDIR:       return "is a directory";
    case LFS_ERR_NOTEMPTY:    return "directory not empty";
    case LFS_ERR_BADF:        return "bad file";
    case LFS_ERR_FBIG:        return "file too large";
    case LFS_ERR_INVAL:       return "invalid, or the filesystem is not mounted";
    case LFS_ERR_NOSPC:       return "no space left";
    case LFS_ERR_NOMEM:       return "out of memory";
    case LFS_ERR_NOATTR:      return "no such attribute";
    case LFS_ERR_NAMETOOLONG: return "name too long";
    case PURR_FS_ERR_DENIED:  return "permission denied";
    default:                  return "filesystem error";
    }
}
