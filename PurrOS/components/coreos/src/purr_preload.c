#include "purr_preload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char file[PURR_PRELOAD_MAX * 2][PURR_APP_NAME_LEN + 8];
    int n;
} files_t;

static void collect(void *ctx, const char *name, int is_dir, uint32_t size)
{
    (void)size;
    files_t *f = ctx;
    size_t n = strlen(name);
    if (!is_dir && n > 4 && n < sizeof(f->file[0]) && strcmp(name + n - 4, ".cat") == 0 && f->n < PURR_PRELOAD_MAX * 2) {
        snprintf(f->file[f->n++], sizeof(f->file[0]), "%s", name);
    }
}

typedef struct {
    uint8_t *buf;
    uint32_t cap, len;
} acc_t;

static int accumulate(void *ctx, const void *data, uint32_t len)
{
    acc_t *a = ctx;
    if (a->len + len > a->cap) {
        return -1;                                 /* bigger than the buffer: the file is skipped */
    }
    memcpy(a->buf + a->len, data, len);
    a->len += len;
    return 0;
}

static int file_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static int read_package(const purr_appmgr_fs_t *fs, const char *dir, const char *file, uint8_t *scratch, uint32_t cap, uint32_t *len)
{
    char path[PURR_APPMGR_ROOT_MAX + PURR_APP_NAME_LEN + 16];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    acc_t a = {scratch, cap, 0};
    if (fs->read(path, accumulate, &a) != 0 || a.len < sizeof(purr_image_header_t)) {
        return -1;
    }
    *len = a.len;
    return 0;
}

int purr_preload_list(const purr_appmgr_fs_t *fs, const char *dir, uint16_t chip_id, uint8_t *scratch, uint32_t cap, purr_preload_app_t *out, int max)
{
    files_t *f = &(files_t){.n = 0};
    if (fs->list(dir, collect, f) != 0) {
        return 0;
    }
    qsort(f->file, (size_t)f->n, sizeof(f->file[0]), file_cmp);
    int count = 0;
    for (int i = 0; i < f->n && count < max; i++) {
        uint32_t len = 0;
        if (read_package(fs, dir, f->file[i], scratch, cap, &len) != 0) {
            continue;
        }
        purr_image_header_t h;
        memcpy(&h, scratch, sizeof(h));
        if (h.magic != PURR_IMAGE_MAGIC || h.image_type != PURR_IMG_APP || (h.chip_id != chip_id && h.chip_id != PURR_CHIP_ANY)) {
            continue;
        }
        size_t nl = strnlen(h.name, sizeof(h.name));
        if (nl == 0 || nl >= sizeof(h.name)) {
            continue;
        }
        purr_preload_app_t *o = &out[count++];
        memset(o, 0, sizeof(*o));
        snprintf(o->file, sizeof(o->file), "%s", f->file[i]);
        snprintf(o->name, sizeof(o->name), "%s", h.name);
        snprintf(o->version, sizeof(o->version), "%.*s", (int)sizeof(o->version) - 1, h.version);
        o->size = len;
    }
    return count;
}

purr_app_result_t purr_preload_plant(const purr_appmgr_fs_t *fs, const char *dir, const char *file, const char *root, const purr_appmgr_env_t *env,
                                     const purr_cfg_t *cfg, const purr_app_registry_t *reg, uint8_t *scratch, uint32_t cap)
{
    if (file == NULL || strchr(file, '/') != NULL || strstr(file, "..") != NULL) {
        return PURR_APP_BAD_NAME;
    }
    uint32_t len = 0;
    if (read_package(fs, dir, file, scratch, cap, &len) != 0) {
        return PURR_APP_IO_ERROR;
    }
    return purr_appmgr_add(fs, root, env, cfg, reg, scratch, len);
}
