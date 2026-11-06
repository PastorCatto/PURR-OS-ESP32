/*
 * purr_catrt_files.c - the `files` catcall (purr_catcalls.h): what a file manager may do, and nothing more.
 *
 * Every call goes through the signed-in user's file system view (purr_login_fs), so the file system's own owner checks still apply on top of
 * these. On top of those: a path under /home/<user> needs "shared-storage" and may be changed; any other path needs "system-files", which only an
 * administrator can be granted, and is read-only. The one file an app must not rewrite is its own permission list.
 */
#include "purr_catrt_files.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "commands.h"
#include "login.h"
#include "purr_catcalls.h"
#include "purr_catrt.h"
#include "purr_catrt_power.h"
#include "purr_fs.h"
#include "purr_perm.h"
#include "purr_users.h"

#define PATH_MAX_LEN 120
#define COPY_MAX (64 * 1024)

static int is_admin_now(void)
{
    return purr_login_is_root() || purr_login_current()->role == PURR_ROLE_USER_ADMIN;
}

/* A clean absolute path: starts with "/", no empty, "." or ".." parts, no backslash. */
static int path_ok(const char *p)
{
    if (p == NULL || p[0] != '/' || strlen(p) > PATH_MAX_LEN || strchr(p, '\\') != NULL) {
        return 0;
    }
    if (p[1] == '\0') {
        return 1;                                       /* the root */
    }
    const char *q = p + 1;
    while (*q != '\0') {
        const char *e = strchr(q, '/');
        size_t n = e ? (size_t)(e - q) : strlen(q);
        if (n == 0 || (n == 1 && q[0] == '.') || (n == 2 && q[0] == '.' && q[1] == '.')) {
            return 0;                                   /* empty part (a double or trailing slash), "." or ".." */
        }
        q += n + (e ? 1 : 0);
    }
    return 1;
}

static int under_home(const char *p)
{
    const char *user = purr_login_current()->name;
    char home[48];
    if (user[0] == '\0') {
        return 0;
    }
    int hn = snprintf(home, sizeof(home), "/home/%.24s", user);
    return strncmp(p, home, (size_t)hn) == 0 && (p[hn] == '\0' || p[hn] == '/');
}

static int is_perm_file(const char *p)
{
    const char *slash = strrchr(p, '/');
    return slash != NULL && strcmp(slash + 1, "permissions.conf") == 0;
}

/* 0 if the calling app may do this to this path now, else a PURR_FILES_* code (ASK means a prompt is up). */
static int allow(const char *path, int write)
{
    if (!path_ok(path)) {
        return PURR_FILES_BAD;
    }
    if (under_home(path)) {
        if (write && is_perm_file(path)) {
            return PURR_FILES_DENIED;
        }
        return purr_catrt_perm_gate(PURR_PERM_SHARED_STORAGE);
    }
    if (write || !is_admin_now()) {
        return PURR_FILES_DENIED;
    }
    return purr_catrt_perm_gate(PURR_PERM_SYSTEM_FILES);
}

static int map(int rc)
{
    return rc == 0 ? PURR_FILES_OK : PURR_FILES_BAD;
}

static int cc_home(char *buf, size_t cap)
{
    const char *user = purr_login_current()->name;
    if (user[0] == '\0' || buf == NULL || cap < 8) {
        return PURR_FILES_BAD;
    }
    return snprintf(buf, cap, "/home/%.24s", user);
}

static int cc_is_admin(void) { return is_admin_now(); }

typedef struct {
    void (*cb)(const char *name, int is_dir, uint32_t size, void *ctx);
    void *ctx;
} list_ctx_t;

static void list_thunk(void *vctx, const char *name, int is_dir, uint32_t size)
{
    list_ctx_t *l = vctx;
    l->cb(name, is_dir, size, l->ctx);
}

static int cc_list(const char *path, void (*cb)(const char *name, int is_dir, uint32_t size, void *ctx), void *ctx)
{
    int a = allow(path, 0);
    if (a != 0 || cb == NULL) {
        return a != 0 ? a : PURR_FILES_BAD;
    }
    list_ctx_t l = {cb, ctx};
    return map(purr_fs_list(purr_login_fs(), path, list_thunk, &l));
}

static int cc_stat(const char *path, uint32_t *size, int *is_dir)
{
    int a = allow(path, 0);
    if (a != 0) {
        return a;
    }
    uint32_t sz = 0;
    int dir = 0;
    if (purr_fs_stat(purr_login_fs(), path, &dir, &sz) != 0) {
        return PURR_FILES_BAD;
    }
    if (size != NULL) {
        *size = sz;
    }
    if (is_dir != NULL) {
        *is_dir = dir;
    }
    return PURR_FILES_OK;
}

typedef struct {
    uint8_t *buf;
    size_t cap, got;
    uint32_t skip;
    int stopped;
} rd_t;

static int rd_cb(void *vctx, const void *data, uint32_t n)
{
    rd_t *r = vctx;
    const uint8_t *d = data;
    if (r->skip >= n) {
        r->skip -= n;
        return 0;
    }
    d += r->skip;
    n -= r->skip;
    r->skip = 0;
    size_t take = r->cap - r->got < n ? r->cap - r->got : n;
    memcpy(r->buf + r->got, d, take);
    r->got += take;
    if (r->got >= r->cap) {
        r->stopped = 1;
        return -1;                                      /* enough: stop reading */
    }
    return 0;
}

static int cc_read(const char *path, uint32_t offset, void *buf, size_t len)
{
    int a = allow(path, 0);
    if (a != 0 || buf == NULL || len == 0) {
        return a != 0 ? a : PURR_FILES_BAD;
    }
    rd_t r = {buf, len, 0, offset, 0};
    int rc = purr_fs_read(purr_login_fs(), path, rd_cb, &r);
    if (rc != 0 && !r.stopped) {
        return PURR_FILES_BAD;
    }
    return (int)r.got;
}

/* The whole of a file into PSRAM, with the calling app's permission, for the ui catcall's pictures. 0 and *out (the caller frees it) / *len, else a PURR_FILES_* code. */
int purr_catrt_files_load(const char *path, uint8_t **out, size_t *len, size_t max)
{
    uint32_t size = 0;
    int is_dir = 0;
    int rc = cc_stat(path, &size, &is_dir);
    if (rc != PURR_FILES_OK) {
        return rc;
    }
    if (is_dir || size == 0 || size > max) {
        return PURR_FILES_BAD;
    }
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        return PURR_FILES_BAD;
    }
    int got = cc_read(path, 0, buf, size);               /* one read: cc_read starts from the top each time */
    if (got != (int)size) {
        free(buf);
        return got < 0 ? got : PURR_FILES_BAD;
    }
    *out = buf;
    *len = size;
    return PURR_FILES_OK;
}

static int cc_mkdir(const char *path)
{
    int a = allow(path, 1);
    return a != 0 ? a : map(purr_fs_mkdir(purr_login_fs(), path));
}

static int cc_remove(const char *path)
{
    int a = allow(path, 1);
    return a != 0 ? a : map(purr_fs_remove(purr_login_fs(), path));
}

static int cc_rename(const char *from, const char *to)
{
    int a = allow(from, 1);
    if (a == 0) {
        a = allow(to, 1);
    }
    return a != 0 ? a : map(purr_fs_rename(purr_login_fs(), from, to));
}

typedef struct {
    uint8_t *buf;
    size_t len;
} acc_t;

static int acc_cb(void *vctx, const void *data, uint32_t n)
{
    acc_t *a = vctx;
    if (a->len + n > COPY_MAX) {
        return -1;
    }
    memcpy(a->buf + a->len, data, n);
    a->len += n;
    return 0;
}

static int cc_copy(const char *from, const char *to)
{
    int a = allow(from, 0);
    if (a == 0) {
        a = allow(to, 1);
    }
    if (a != 0) {
        return a;
    }
    purr_fs_t *fs = purr_login_fs();
    int dir = 0;
    uint32_t sz = 0;
    if (purr_fs_stat(fs, from, &dir, &sz) != 0 || dir || sz > COPY_MAX) {
        return PURR_FILES_BAD;
    }
    uint8_t *buf = heap_caps_malloc(COPY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = malloc(COPY_MAX);
    }
    if (buf == NULL) {
        return PURR_FILES_BAD;
    }
    acc_t acc = {buf, 0};
    int rc = purr_fs_read(fs, from, acc_cb, &acc);
    if (rc == 0) {
        rc = purr_fs_write(fs, to, buf, (uint32_t)acc.len) < 0 ? -1 : 0;
    }
    free(buf);
    return map(rc);
}

static const purr_cc_files_t s_cc_files = {
    .version = 1,
    .home = cc_home,
    .is_admin = cc_is_admin,
    .list = cc_list,
    .stat = cc_stat,
    .read = cc_read,
    .mkdir = cc_mkdir,
    .remove = cc_remove,
    .rename = cc_rename,
    .copy = cc_copy,
};

void purr_catrt_files_init(void)
{
    purr_catrt_set_files(&s_cc_files);
}
