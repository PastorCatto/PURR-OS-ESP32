#include "purr_appmgr.h"

#include <stdio.h>
#include <string.h>

#include "purr_keybag.h"
#include "purr_util.h"

#define PACKAGE_FILE "package.cat"
#define TMP_SUFFIX   ".tmp"
/* F-34: an update's old version parks here, one rename away from either side, for exactly
 * as long as it takes to rename the new one into place -- never deleted outright before the
 * replacement exists, so a power cut always leaves one complete version findable. */
#define OLD_SUFFIX   ".old"

const char *purr_app_result_name(purr_app_result_t r)
{
    switch (r) {
    case PURR_APP_OK:              return "ok";
    case PURR_APP_BAD_MAGIC:       return "not a PURR image";
    case PURR_APP_BAD_TYPE:        return "not an app image";
    case PURR_APP_BAD_CHIP:        return "no payload for this chip";
    case PURR_APP_BAD_VERIFY:      return "failed verification";
    case PURR_APP_NAME_TOO_LONG:   return "name too long";
    case PURR_APP_BAD_NAME:        return "invalid name";
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

/* Joins root + "/" + suffix without ever doubling the slash when root is the literal
 * filesystem root ("/"). Every path this file builds goes through this. */
static void join(char *out, size_t outcap, const char *root, const char *suffix)
{
    if (root[0] == '/' && root[1] == '\0') {
        snprintf(out, outcap, "/%s", suffix);
    } else {
        snprintf(out, outcap, "%s/%s", root, suffix);
    }
}

/* Removes a folder together with the files in it. A folder holds the package and, for a Store install, its receipt: removing only the package left the receipt behind, the folder
 * could not be removed, and every update or removal left a `<name>.old` or an empty-looking folder that nothing cleaned up and every scan counted as dropped. Files only (an app's
 * folder has no subfolders); a subfolder, or a file that will not go, leaves the folder for the next boot, as before. Defined below the listing it uses. */
static void remove_tree(const purr_appmgr_fs_t *fs, const char *dir);

/* ---------------------------------------------------------------- scanning */

typedef struct {
    char folder[PURR_APP_NAME_LEN];
    int is_dir;
} listing_row_t;

/* The folder's entries: the apps (at most PURR_APP_MAX), and next to them whatever a cut left (`<name>.tmp`, `<name>.old`) or a person dropped there. The listing used to hold only
 * PURR_APP_MAX rows, so a folder with 30 apps and a few leftovers lost the last ones to the scan and to recover() without a word. `overflow` counts what did not fit. */
#define LIST_MAX (PURR_APP_MAX + 16)

typedef struct {
    listing_row_t rows[LIST_MAX];
    int count;
    int overflow;
} listing_t;

static void collect_row(void *ctx, const char *name, int is_dir, uint32_t size)
{
    (void)size;
    listing_t *l = ctx;
    if (l->count >= LIST_MAX) {
        l->overflow++;
        return;
    }
    {
        strncpy(l->rows[l->count].folder, name, sizeof(l->rows[l->count].folder) - 1);
        l->rows[l->count].folder[sizeof(l->rows[l->count].folder) - 1] = '\0';
        l->rows[l->count].is_dir = is_dir;
        l->count++;
    }
}

typedef struct {
    char name[8][PURR_APP_NAME_LEN];
    int n;
} filelist_t;

static void collect_file(void *ctx, const char *name, int is_dir, uint32_t size)
{
    (void)size;
    filelist_t *f = ctx;
    if (!is_dir && f->n < 8) {
        snprintf(f->name[f->n++], sizeof(f->name[0]), "%s", name);
    }
}

static void remove_tree(const purr_appmgr_fs_t *fs, const char *dir)
{
    filelist_t f = {.n = 0};
    if (fs->list(dir, collect_file, &f) == 0) {
        for (int i = 0; i < f.n; i++) {
            char path[PURR_APPMGR_ROOT_MAX + 2 * PURR_APP_NAME_LEN + 8];
            snprintf(path, sizeof(path), "%s/%s", dir, f.name[i]);
            fs->remove(path);
        }
    }
    fs->remove(dir);
}

void purr_appmgr_recover(const purr_appmgr_fs_t *fs, const char *root)
{
    listing_t l = {.count = 0};
    if (fs->list(root, collect_row, &l) != 0) {
        return;
    }
    for (int i = 0; i < l.count; i++) {
        size_t n = strlen(l.rows[i].folder);
        if (l.rows[i].is_dir && n > 4 && strcmp(l.rows[i].folder + n - 4, TMP_SUFFIX) == 0) {
            char path[PURR_APPMGR_ROOT_MAX + PURR_APP_NAME_LEN + 2];
            join(path, sizeof(path), root, l.rows[i].folder);
            remove_tree(fs, path);                  /* the package, and the receipt a Store install stages beside it */
        }
        /* F-34: an update's parked old version (purr_appmgr_add(), above). If the real name
         * is missing, the cut happened before the new version's rename took -- the old one
         * is still the only complete copy, so it goes back, not into the trash. If the real
         * name is already there, the new version made it; this is just leftover cleanup. */
        if (l.rows[i].is_dir && n > 4 && strcmp(l.rows[i].folder + n - 4, OLD_SUFFIX) == 0) {
            char old_path[PURR_APPMGR_ROOT_MAX + PURR_APP_NAME_LEN + 2];
            join(old_path, sizeof(old_path), root, l.rows[i].folder);
            char real_name[PURR_APP_NAME_LEN];
            size_t real_len = n - 4 < sizeof(real_name) - 1 ? n - 4 : sizeof(real_name) - 1;
            memcpy(real_name, l.rows[i].folder, real_len);
            real_name[real_len] = '\0';
            char real_path[PURR_APPMGR_ROOT_MAX + PURR_APP_NAME_LEN + 2];
            join(real_path, sizeof(real_path), root, real_name);

            int is_dir = 0;
            uint32_t size = 0;
            if (fs->stat(real_path, &is_dir, &size) != 0) {
                fs->rename(old_path, real_path);
            } else {
                remove_tree(fs, old_path);
            }
        }
    }
}

void purr_appmgr_scan(const purr_appmgr_fs_t *fs, const char *root, const purr_appmgr_env_t *env,
                      uint8_t *scratch, uint32_t scratch_cap, purr_app_registry_t *reg)
{
    memset(reg, 0, sizeof(*reg));
    listing_t l = {.count = 0};
    if (fs->list(root, collect_row, &l) != 0) {
        return;
    }
    reg->dropped += l.overflow;                /* entries the listing had no room for: said, not lost without a word */
    for (int i = 0; i < l.count && reg->count < PURR_APP_MAX; i++) {
        if (!l.rows[i].is_dir) {
            continue;                          /* a stray file at the root: not an app folder */
        }
        size_t n = strlen(l.rows[i].folder);
        if (n > 4 && strcmp(l.rows[i].folder + n - 4, TMP_SUFFIX) == 0) {
            continue;                          /* a leftover staging folder: recover() handles it */
        }
        if (n > 4 && strcmp(l.rows[i].folder + n - 4, OLD_SUFFIX) == 0) {
            continue;                          /* F-34: an update's parked old version: recover() handles it */
        }
        if (strcmp(l.rows[i].folder, "incoming") == 0) {
            continue;                          /* the transport drop folder, not an app */
        }

        char entry[PURR_APPMGR_ROOT_MAX + PURR_APP_NAME_LEN + 2];
        join(entry, sizeof(entry), root, l.rows[i].folder);
        char path[sizeof(entry) + 16];
        snprintf(path, sizeof(path), "%s/%s", entry, PACKAGE_FILE);
        int is_dir = 0;
        uint32_t size = 0;
        if (fs->stat(path, &is_dir, &size) != 0 || is_dir || size < sizeof(purr_image_header_t)) {
            reg->dropped++;
            continue;
        }
        if (size > scratch_cap) {
            reg->dropped++;                    /* too big to verify with the memory given */
            continue;
        }

        accum_t a = {scratch, scratch_cap, 0};
        if (fs->read(path, accumulate, &a) != 0) {
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

purr_app_result_t purr_appmgr_add(const purr_appmgr_fs_t *fs, const char *root, const purr_appmgr_env_t *env,
                                  const purr_cfg_t *cfg, const purr_app_registry_t *reg,
                                  const uint8_t *data, uint32_t len)
{
    return purr_appmgr_add_ex(fs, root, env, cfg, reg, data, len, NULL, NULL, 0);
}

purr_app_result_t purr_appmgr_add_ex(const purr_appmgr_fs_t *fs, const char *root, const purr_appmgr_env_t *env,
                                     const purr_cfg_t *cfg, const purr_app_registry_t *reg,
                                     const uint8_t *data, uint32_t len,
                                     const char *extra_name, const uint8_t *extra, uint32_t extra_len)
{
    if (extra_name != NULL && (extra == NULL || extra_len == 0 || strlen(extra_name) >= 16 || strchr(extra_name, '/') != NULL || strcmp(extra_name, PACKAGE_FILE) == 0)) {
        return PURR_APP_IO_ERROR;
    }
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
    /* F-11: hdr.name comes from a signed header and goes straight into a path join below
     * (join(final_dir, ..., root, hdr.name)) -- a hostile or corrupt header with "/" or
     * ".." in its name would escape the apps directory entirely. Letters, digits, '-', '_'
     * only rules that out the same way it does for account names. */
    if (!purr_name_is_safe(hdr.name, sizeof(hdr.name))) {
        return PURR_APP_BAD_NAME;
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

    char tmp_name[PURR_APP_NAME_LEN + 8];
    snprintf(tmp_name, sizeof(tmp_name), "%s%s", hdr.name, TMP_SUFFIX);
    char tmp_dir[PURR_APPMGR_ROOT_MAX + sizeof(tmp_name) + 1];
    char tmp_file[sizeof(tmp_dir) + 16];
    char final_dir[PURR_APPMGR_ROOT_MAX + PURR_APP_NAME_LEN + 1];
    join(tmp_dir, sizeof(tmp_dir), root, tmp_name);
    snprintf(tmp_file, sizeof(tmp_file), "%s/%s", tmp_dir, PACKAGE_FILE);
    join(final_dir, sizeof(final_dir), root, hdr.name);

    remove_tree(fs, tmp_dir);                  /* in case a previous attempt left one (with whatever it had staged beside the package) */
    if (fs->mkdir(tmp_dir) != 0 || fs->write(tmp_file, data, len) != 0) {
        remove_tree(fs, tmp_dir);
        return PURR_APP_IO_ERROR;
    }
    if (extra_name != NULL) {
        /* A file that has to exist together with the package (a Store install's receipt) goes into the staging folder too, so the one rename below makes both appear. Written
         * afterwards into the live folder it left a window: a reset between the rename and the write left an app with no receipt, which is hidden from every list (its data
         * still there) until someone reinstalls it. */
        char extra_path[sizeof(tmp_dir) + 16];
        snprintf(extra_path, sizeof(extra_path), "%s/%s", tmp_dir, extra_name);
        if (fs->write(extra_path, extra, extra_len) != 0) {
            remove_tree(fs, tmp_dir);
            return PURR_APP_IO_ERROR;
        }
    }

    /* Check the copy, not just trust the write. */
    purr_sha256_t sha;
    purr_sha256_init(&sha);
    if (fs->read(tmp_file, hash_accumulate, &sha) != 0) {
        remove_tree(fs, tmp_dir);
        return PURR_APP_IO_ERROR;
    }
    uint8_t digest[PURR_SHA256_LEN], expected[PURR_SHA256_LEN];
    purr_sha256_final(&sha, digest);
    purr_sha256(data, len, expected);
    if (memcmp(digest, expected, sizeof(digest)) != 0) {
        remove_tree(fs, tmp_dir);
        return PURR_APP_IO_ERROR;
    }

    /* F-34: this used to delete the old version outright, then rename the new one in -- a
     * power cut between those two steps left only the (unverified-by-recover) tmp dir on
     * disk, which purr_appmgr_recover() then discarded as ordinary leftover staging,
     * destroying the one complete version the spec promises always survives. Parking the
     * old version at a reversible name until the new one is confirmed in place closes that
     * window: at every point up to the final cleanup, either the old or the new version is
     * findable at `final_dir` or `final_dir.old`, never neither (purr_appmgr_recover()
     * resolves whichever one a cut left behind). */
    if (existing != NULL) {
        char old_dir[sizeof(final_dir) + sizeof(OLD_SUFFIX)];
        snprintf(old_dir, sizeof(old_dir), "%s%s", final_dir, OLD_SUFFIX);
        remove_tree(fs, old_dir);       /* in case a previous attempt left one */

        if (fs->rename(final_dir, old_dir) != 0) {
            remove_tree(fs, tmp_dir);
            return PURR_APP_IO_ERROR;
        }
        if (fs->rename(tmp_dir, final_dir) != 0) {
            fs->rename(old_dir, final_dir);    /* put the old one back; best-effort */
            return PURR_APP_IO_ERROR;
        }
        remove_tree(fs, old_dir);       /* with its receipt: the folder used to stay for ever, one more entry in every listing */
        return PURR_APP_OK;
    }
    if (fs->rename(tmp_dir, final_dir) != 0) {
        return PURR_APP_IO_ERROR;
    }
    return PURR_APP_OK;
}

int purr_appmgr_remove(const purr_appmgr_fs_t *fs, const char *root, const char *name)
{
    char dir[PURR_APPMGR_ROOT_MAX + PURR_APP_NAME_LEN + 1];
    join(dir, sizeof(dir), root, name);
    char file[sizeof(dir) + 16];
    snprintf(file, sizeof(file), "%s/%s", dir, PACKAGE_FILE);
    int is_dir = 0;
    uint32_t size = 0;
    if (fs->stat(file, &is_dir, &size) != 0 && fs->stat(dir, &is_dir, &size) != 0) {
        return -1;                              /* there is no such app */
    }
    remove_tree(fs, dir);                       /* the package and its receipt, then the folder */
    return fs->stat(dir, &is_dir, &size) == 0 ? -1 : 0;
}
