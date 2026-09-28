#include "purr_appmgr.h"

#include <stdio.h>
#include <string.h>

#include "purr_keybag.h"
#include "purr_util.h"

#define PACKAGE_FILE "package.cat"
#define TMP_SUFFIX   ".tmp"

const char *purr_app_result_name(purr_app_result_t r)
{
    switch (r) {
    case PURR_APP_OK:              return "ok";
    case PURR_APP_BAD_MAGIC:       return "not a PURR image";
    case PURR_APP_BAD_TYPE:        return "not an app image";
    case PURR_APP_BAD_CHIP:        return "no payload for this chip";
    case PURR_APP_BAD_VERIFY:      return "failed verification";
    case PURR_APP_NAME_TOO_LONG:   return "name too long";
    case PURR_APP_EXISTS_NEWER:    return "an equal or newer version is already installed";
    case PURR_APP_REGISTRY_FULL:   return "too many apps installed";
    case PURR_APP_IO_ERROR:        return "filesystem error";
    }
    return "unknown";
}

const purr_app_entry_t *purr_appmgr_find(const purr_app_registry_t *reg, const char *name)
{
    for (int i = 0; i < reg->count; i++) {
        if (strcmp(reg->apps[i].name, name) == 0) {
            return &reg->apps[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- small helpers */

static purr_verify_result_t check_app_header(const purr_appmgr_env_t *env, purr_read_fn rd, void *ctx,
                                             uint32_t size, purr_image_header_t *hdr)
{
    purr_verify_env_t venv = {
        .chip_id = env->chip_id,
        .bag = env->bag,
        .crypto = env->crypto,
        .bootloader_version = 0,      /* apps do not gate on the bootloader's version */
        .version_floor = 0,
        .enforce_floor = 0,
    };
    return purr_image_verify(&venv, rd, ctx, size, hdr);
}

/* Accumulates a purr_fs_read stream into a fixed buffer; fails (nonzero) if it would
 * overflow, so a package bigger than the buffer is cleanly rejected rather than corrupting
 * memory. */
typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;
} accum_t;

static int accumulate(void *ctx, const void *data, uint32_t len)
{
    accum_t *a = ctx;
    if (a->len + len > a->cap) {
        return -1;
    }
    memcpy(a->buf + a->len, data, len);
    a->len += len;
    return 0;
}

static int hash_accumulate(void *ctx, const void *data, uint32_t len)
{
    purr_sha256_update((purr_sha256_t *)ctx, data, len);
    return 0;
}

/* ---------------------------------------------------------------- scanning */

typedef struct {
    char folder[PURR_APP_NAME_LEN];
    int is_dir;
} listing_row_t;

typedef struct {
    listing_row_t rows[PURR_APP_MAX];
    int count;
} listing_t;

static void collect_row(void *ctx, const char *name, int is_dir, uint32_t size)
{
    (void)size;
    listing_t *l = ctx;
    if (l->count < PURR_APP_MAX) {
        strncpy(l->rows[l->count].folder, name, sizeof(l->rows[l->count].folder) - 1);
        l->rows[l->count].folder[sizeof(l->rows[l->count].folder) - 1] = '\0';
        l->rows[l->count].is_dir = is_dir;
        l->count++;
    }
}

void purr_appmgr_recover(purr_fs_t *fs)
{
    listing_t l = {.count = 0};
    if (purr_fs_list(fs, "/", collect_row, &l) != 0) {
        return;
    }
    for (int i = 0; i < l.count; i++) {
        size_t n = strlen(l.rows[i].folder);
        if (l.rows[i].is_dir && n > 4 && strcmp(l.rows[i].folder + n - 4, TMP_SUFFIX) == 0) {
            char path[PURR_APP_NAME_LEN + 2];
            snprintf(path, sizeof(path), "/%s", l.rows[i].folder);
            /* Remove the package file first (purr_fs_remove needs an empty directory). */
            char inner[PURR_APP_NAME_LEN + 16];
            snprintf(inner, sizeof(inner), "%s/%s", path, PACKAGE_FILE);
            purr_fs_remove(fs, inner);
            purr_fs_remove(fs, path);
        }
    }
}

void purr_appmgr_scan(purr_fs_t *fs, const purr_appmgr_env_t *env, uint8_t *scratch,
                      uint32_t scratch_cap, purr_app_registry_t *reg)
{
    memset(reg, 0, sizeof(*reg));
    listing_t l = {.count = 0};
    if (purr_fs_list(fs, "/", collect_row, &l) != 0) {
        return;
    }
    for (int i = 0; i < l.count && reg->count < PURR_APP_MAX; i++) {
        if (!l.rows[i].is_dir) {
            continue;                          /* a stray file at the root: not an app folder */
        }
        size_t n = strlen(l.rows[i].folder);
        if (n > 4 && strcmp(l.rows[i].folder + n - 4, TMP_SUFFIX) == 0) {
            continue;                          /* a leftover staging folder: recover() handles it */
        }
        if (strcmp(l.rows[i].folder, "incoming") == 0) {
            continue;                          /* the transport drop folder, not an app */
        }

        char path[PURR_APP_NAME_LEN + 16];
        snprintf(path, sizeof(path), "%s/%s", l.rows[i].folder, PACKAGE_FILE);
        int is_dir = 0;
        uint32_t size = 0;
        if (purr_fs_stat(fs, path, &is_dir, &size) != 0 || is_dir || size < sizeof(purr_image_header_t)) {
            reg->dropped++;
            continue;
        }
        if (size > scratch_cap) {
            reg->dropped++;                    /* too big to verify with the memory given */
            continue;
        }

        accum_t a = {scratch, scratch_cap, 0};
        if (purr_fs_read(fs, path, accumulate, &a) != 0) {
            reg->dropped++;
            continue;
        }

        purr_mem_read_ctx_t mrc = {scratch, a.len};
        purr_image_header_t hdr;
        purr_verify_result_t vr = check_app_header(env, purr_mem_read, &mrc, a.len, &hdr);
        if (hdr.magic != PURR_IMAGE_MAGIC || hdr.image_type != PURR_IMG_APP) {
            reg->dropped++;
            continue;
        }

        purr_app_entry_t *e = &reg->apps[reg->count++];
        memset(e, 0, sizeof(*e));
        strncpy(e->name, hdr.name, sizeof(e->name) - 1);
        strncpy(e->version, hdr.version, sizeof(e->version) - 1);
        e->size = a.len;
        e->chip_ok = (hdr.chip_id == env->chip_id || hdr.chip_id == PURR_CHIP_ANY);
        e->signer_role = PURR_ROLE_NONE;
        const purr_key_t *key = purr_keybag_find(env->bag, hdr.key_id);
        if (key != NULL) {
            e->signer_role = key->role;
        }
        e->verified = (vr == PURR_V_OK);
    }
}

/* ---------------------------------------------------------------- add and remove */

purr_app_result_t purr_appmgr_add(purr_fs_t *fs, const purr_appmgr_env_t *env, const purr_cfg_t *cfg,
                                  const purr_app_registry_t *reg, const uint8_t *data, uint32_t len)
{
    if (len < sizeof(purr_image_header_t)) {
        return PURR_APP_BAD_MAGIC;
    }
    purr_mem_read_ctx_t mrc = {data, len};
    purr_image_header_t hdr;
    purr_verify_result_t vr = check_app_header(env, purr_mem_read, &mrc, len, &hdr);

    if (hdr.magic != PURR_IMAGE_MAGIC) {
        return PURR_APP_BAD_MAGIC;
    }
    if (hdr.image_type != PURR_IMG_APP) {
        return PURR_APP_BAD_TYPE;
    }
    if (hdr.chip_id != env->chip_id && hdr.chip_id != PURR_CHIP_ANY) {
        return PURR_APP_BAD_CHIP;
    }
    if (vr != PURR_V_OK && (cfg == NULL || cfg->secure_mode != PURR_SECURE_OFF)) {
        return PURR_APP_BAD_VERIFY;
    }
    /* hdr.name is a fixed char[32] with no guarantee of a null terminator (a hostile or
     * corrupt header could fill every byte); strnlen never reads past the field either way. */
    size_t name_len = strnlen(hdr.name, sizeof(hdr.name));
    if (name_len == 0 || name_len >= sizeof(hdr.name)) {
        return PURR_APP_NAME_TOO_LONG;
    }

    const purr_app_entry_t *existing = purr_appmgr_find(reg, hdr.name);
    if (existing != NULL) {
        uint32_t old_ver, new_ver;
        if (purr_version_parse(existing->version, strlen(existing->version), &old_ver) != 0) {
            old_ver = 0;
        }
        if (purr_version_parse(hdr.version, strlen(hdr.version), &new_ver) != 0) {
            new_ver = 0;
        }
        if (new_ver <= old_ver) {
            return PURR_APP_EXISTS_NEWER;
        }
    } else if (reg->count >= PURR_APP_MAX) {
        return PURR_APP_REGISTRY_FULL;
    }

    char tmp_dir[PURR_APP_NAME_LEN + 8], tmp_file[PURR_APP_NAME_LEN + 24], final_dir[PURR_APP_NAME_LEN + 2];
    snprintf(tmp_dir, sizeof(tmp_dir), "/%s%s", hdr.name, TMP_SUFFIX);
    snprintf(tmp_file, sizeof(tmp_file), "%s/%s", tmp_dir, PACKAGE_FILE);
    snprintf(final_dir, sizeof(final_dir), "/%s", hdr.name);

    purr_fs_remove(fs, tmp_file);              /* in case a previous attempt left one */
    purr_fs_remove(fs, tmp_dir);
    if (purr_fs_mkdir(fs, tmp_dir) != 0 || purr_fs_write(fs, tmp_file, data, len) != 0) {
        return PURR_APP_IO_ERROR;
    }

    /* Check the copy, not just trust the write. */
    purr_sha256_t sha;
    purr_sha256_init(&sha);
    if (purr_fs_read(fs, tmp_file, hash_accumulate, &sha) != 0) {
        purr_fs_remove(fs, tmp_file);
        purr_fs_remove(fs, tmp_dir);
        return PURR_APP_IO_ERROR;
    }
    uint8_t digest[PURR_SHA256_LEN], expected[PURR_SHA256_LEN];
    purr_sha256_final(&sha, digest);
    purr_sha256(data, len, expected);
    if (memcmp(digest, expected, sizeof(digest)) != 0) {
        purr_fs_remove(fs, tmp_file);
        purr_fs_remove(fs, tmp_dir);
        return PURR_APP_IO_ERROR;
    }

    if (existing != NULL) {
        char old_file[PURR_APP_NAME_LEN + 16];
        snprintf(old_file, sizeof(old_file), "%s/%s", final_dir, PACKAGE_FILE);
        purr_fs_remove(fs, old_file);
        purr_fs_remove(fs, final_dir);
    }
    if (purr_fs_rename(fs, tmp_dir, final_dir) != 0) {
        return PURR_APP_IO_ERROR;
    }
    return PURR_APP_OK;
}

int purr_appmgr_remove(purr_fs_t *fs, const char *name)
{
    char file[PURR_APP_NAME_LEN + 16], dir[PURR_APP_NAME_LEN + 2];
    snprintf(file, sizeof(file), "/%s/%s", name, PACKAGE_FILE);
    snprintf(dir, sizeof(dir), "/%s", name);
    int had_file = purr_fs_remove(fs, file) == 0;
    int had_dir = purr_fs_remove(fs, dir) == 0;
    return (had_file || had_dir) ? 0 : -1;
}
