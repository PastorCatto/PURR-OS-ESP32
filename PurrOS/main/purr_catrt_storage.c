/*
 * purr_catrt_storage.c - the `storage` catcall (purr_catcalls.h, plan9.md): an app's own data folder.
 *
 * An app reads and writes files under /home/<signed-in user>/appdata/<app name>/ and nowhere else: a path is
 * relative, may not contain ".." or start with "/", and is joined onto that folder here. The filesystem
 * stores whole files, so a handle holds the file in memory (at most STORE_FILE_MAX bytes) and a written one
 * goes back to flash when it is closed (or when the app stops). Four handles per app.
 */
#include "purr_catrt_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "commands.h"
#include "login.h"
#include "purr_catcalls.h"
#include "purr_catrt.h"
#include "purr_fs.h"
#include "purr_runtime.h"

#define TAG "storage-cc"
#define STORE_HANDLES 4
#define STORE_FILE_MAX 16384
#define STORE_PATH 112

typedef struct {
    int used;
    int flags;
    char path[STORE_PATH];
    uint8_t *buf;
    size_t len, cap, pos;
    int dirty;
} fh_t;

static fh_t s_fh[PURR_RT_MAX_APPS][STORE_HANDLES];

/* ---------------------------------------------------------------- paths */

static int rel_ok(const char *rel)
{
    if (rel == NULL || rel[0] == '/' || rel[0] == '\0' || strlen(rel) > 48 || strchr(rel, '\\') != NULL) {
        return 0;
    }
    const char *p = rel;
    while (*p != '\0') {
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == 0 || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) {
            return 0;
        }
        p += n + (e ? 1 : 0);
    }
    return 1;
}

/* The app's folder, made if it is not there yet. */
static int app_root(const char *app, char *out, size_t cap)
{
    const char *user = purr_login_current()->name;
    if (user[0] == '\0') {
        return -1;
    }
    char p[STORE_PATH];
    purr_fs_t *fs = purr_login_fs();
    snprintf(p, sizeof(p), "/home/%.24s", user);
    purr_fs_mkdir(fs, "/home");
    purr_fs_mkdir(fs, p);
    snprintf(p, sizeof(p), "/home/%.24s/appdata", user);
    purr_fs_mkdir(fs, p);
    snprintf(out, cap, "/home/%.24s/appdata/%.31s", user, app);
    purr_fs_mkdir(fs, out);
    return 0;
}

static int full_path(const char *rel, char *out, size_t cap, int make_dirs)
{
    char app[PURR_RT_NAME_LEN];
    int slot = purr_catrt_caller(app, sizeof(app));
    if (slot < 0 || !rel_ok(rel)) {
        return -1;
    }
    char root[STORE_PATH];
    if (app_root(app, root, sizeof(root)) != 0) {
        return -1;
    }
    if (snprintf(out, cap, "%s/%s", root, rel) >= (int)cap) {
        return -1;
    }
    if (make_dirs) {                                    /* a/b/c.txt: make a and a/b */
        for (size_t i = strlen(root) + 1; out[i] != '\0'; i++) {
            if (out[i] == '/') {
                out[i] = '\0';
                purr_fs_mkdir(purr_login_fs(), out);
                out[i] = '/';
            }
        }
    }
    return slot;
}

/* ---------------------------------------------------------------- whole-file reads */

typedef struct {
    uint8_t *buf;
    size_t cap, len;
} acc_t;

static int accumulate(void *vctx, const void *data, uint32_t n)
{
    acc_t *a = vctx;
    if (a->len + n > a->cap) {
        return -1;
    }
    memcpy(a->buf + a->len, data, n);
    a->len += n;
    return 0;
}

static uint8_t *alloc(size_t n)
{
    uint8_t *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p != NULL ? p : malloc(n);
}

static void fh_free(fh_t *f)
{
    free(f->buf);
    memset(f, 0, sizeof(*f));
}

static int fh_flush(fh_t *f)
{
    if (!f->dirty) {
        return 0;
    }
    int rc = purr_fs_write(purr_login_fs(), f->path, f->buf, (uint32_t)f->len);
    f->dirty = rc < 0;
    return rc < 0 ? -1 : 0;
}

/* ---------------------------------------------------------------- the catcall */

static int cc_open(const char *rel, int flags)
{
    char path[STORE_PATH];
    int slot = full_path(rel, path, sizeof(path), (flags & PURR_CC_O_WRITE) != 0);
    if (slot < 0 || (flags & (PURR_CC_O_READ | PURR_CC_O_WRITE)) == 0) {
        return -1;
    }
    int idx = -1;
    for (int i = 0; i < STORE_HANDLES; i++) {
        if (!s_fh[slot][i].used) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return -1;
    }
    fh_t *f = &s_fh[slot][idx];
    memset(f, 0, sizeof(*f));
    int is_dir = 0;
    uint32_t size = 0;
    int exists = purr_fs_stat(purr_login_fs(), path, &is_dir, &size) == 0 && !is_dir;
    if (!exists && !(flags & PURR_CC_O_CREATE)) {
        return -1;
    }
    if (exists && size > STORE_FILE_MAX) {
        return -1;
    }
    f->cap = (size_t)(exists ? size : 0) + 256;
    f->buf = alloc(f->cap);
    if (f->buf == NULL) {
        return -1;
    }
    if (exists && !(flags & PURR_CC_O_TRUNC)) {
        acc_t a = {f->buf, f->cap, 0};
        if (purr_fs_read(purr_login_fs(), path, accumulate, &a) < 0) {
            free(f->buf);
            memset(f, 0, sizeof(*f));
            return -1;
        }
        f->len = a.len;
    }
    f->used = 1;
    f->flags = flags;
    snprintf(f->path, sizeof(f->path), "%s", path);
    f->pos = (flags & PURR_CC_O_APPEND) ? f->len : 0;
    f->dirty = !exists || (flags & PURR_CC_O_TRUNC) != 0;      /* creating or truncating is itself a write */
    return idx;
}

static fh_t *get(int h)
{
    int slot = purr_catrt_caller(NULL, 0);
    return slot >= 0 && h >= 0 && h < STORE_HANDLES && s_fh[slot][h].used ? &s_fh[slot][h] : NULL;
}

static int cc_read(int h, void *buf, size_t len)
{
    fh_t *f = get(h);
    if (f == NULL || !(f->flags & PURR_CC_O_READ) || buf == NULL) {
        return -1;
    }
    size_t n = f->pos < f->len ? f->len - f->pos : 0;
    if (n > len) {
        n = len;
    }
    memcpy(buf, f->buf + f->pos, n);
    f->pos += n;
    return (int)n;
}

static int cc_write(int h, const void *buf, size_t len)
{
    fh_t *f = get(h);
    if (f == NULL || !(f->flags & PURR_CC_O_WRITE) || buf == NULL) {
        return -1;
    }
    if (f->flags & PURR_CC_O_APPEND) {
        f->pos = f->len;
    }
    if (f->pos + len > STORE_FILE_MAX) {
        return -1;
    }
    if (f->pos + len > f->cap) {
        size_t ncap = f->pos + len + 256;
        uint8_t *nb = alloc(ncap);
        if (nb == NULL) {
            return -1;
        }
        memcpy(nb, f->buf, f->len);
        free(f->buf);
        f->buf = nb;
        f->cap = ncap;
    }
    if (f->pos > f->len) {
        memset(f->buf + f->len, 0, f->pos - f->len);
    }
    memcpy(f->buf + f->pos, buf, len);
    f->pos += len;
    if (f->pos > f->len) {
        f->len = f->pos;
    }
    f->dirty = 1;
    return (int)len;
}

static int cc_seek(int h, long offset, int whence)
{
    fh_t *f = get(h);
    if (f == NULL) {
        return -1;
    }
    long base = whence == 1 ? (long)f->pos : whence == 2 ? (long)f->len : 0;
    long np = base + offset;
    if (np < 0 || np > STORE_FILE_MAX) {
        return -1;
    }
    f->pos = (size_t)np;
    return 0;
}

static int cc_close(int h)
{
    fh_t *f = get(h);
    if (f == NULL) {
        return -1;
    }
    int rc = fh_flush(f);
    fh_free(f);
    return rc;
}

static int cc_mkdir(const char *rel)
{
    char path[STORE_PATH];
    return full_path(rel, path, sizeof(path), 1) < 0 ? -1 : purr_fs_mkdir(purr_login_fs(), path) < 0 ? -1 : 0;
}

static int cc_remove(const char *rel)
{
    char path[STORE_PATH];
    return full_path(rel, path, sizeof(path), 0) < 0 ? -1 : purr_fs_remove(purr_login_fs(), path) < 0 ? -1 : 0;
}

static int cc_stat(const char *rel, uint32_t *size, int *is_dir)
{
    char path[STORE_PATH];
    int d = 0;
    uint32_t sz = 0;
    if (full_path(rel, path, sizeof(path), 0) < 0 || purr_fs_stat(purr_login_fs(), path, &d, &sz) < 0) {
        return -1;
    }
    if (size) {
        *size = sz;
    }
    if (is_dir) {
        *is_dir = d;
    }
    return 0;
}

typedef struct {
    void (*cb)(const char *name, int is_dir, uint32_t size, void *ctx);
    void *ctx;
} list_ctx_t;

static void list_adapter(void *vctx, const char *name, int is_dir, uint32_t size)
{
    list_ctx_t *l = vctx;
    l->cb(name, is_dir, size, l->ctx);
}

static int cc_list(const char *rel, void (*cb)(const char *, int, uint32_t, void *), void *ctx)
{
    char path[STORE_PATH];
    if (cb == NULL) {
        return -1;
    }
    if (rel == NULL || rel[0] == '\0' || strcmp(rel, ".") == 0) {       /* the app's own folder itself */
        char app[PURR_RT_NAME_LEN];
        if (purr_catrt_caller(app, sizeof(app)) < 0 || app_root(app, path, sizeof(path)) != 0) {
            return -1;
        }
    } else if (full_path(rel, path, sizeof(path), 0) < 0) {
        return -1;
    }
    list_ctx_t l = {cb, ctx};
    return purr_fs_list(purr_login_fs(), path, list_adapter, &l) < 0 ? -1 : 0;
}

static const purr_cc_storage_t s_cc_storage = {
    .version = 1, .open = cc_open, .read = cc_read, .write = cc_write, .seek = cc_seek, .close = cc_close,
    .mkdir = cc_mkdir, .remove = cc_remove, .stat = cc_stat, .list = cc_list,
};

static void release(int slot)
{
    for (int i = 0; i < STORE_HANDLES; i++) {
        fh_t *f = &s_fh[slot][i];
        if (f->used) {
            if (fh_flush(f) != 0) {
                ESP_LOGW(TAG, "could not save %s", f->path);
            }
            fh_free(f);
        }
    }
}

void purr_catrt_storage_init(void)
{
    purr_catrt_set_storage(&s_cc_storage, release);
}
