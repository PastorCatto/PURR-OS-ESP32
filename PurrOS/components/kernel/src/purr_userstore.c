#include "esp_attr.h"
#include "purr_userstore.h"

#include <stdio.h>
#include <string.h>

#define PASSWD_PATH "/etc/passwd"
#define SHADOW_PATH "/etc/shadow"

/* Files here are small (PURR_USER_MAX entries, well under a kilobyte each); one
 * fixed-size buffer read in whole is simpler than streaming. */
#define BUF_LEN 4096

/* One of the two files: 0 read, 1 absent, -1 unreadable. An unreadable (or unparseable) one is tried again as path.bak. *used_bak says that happened. */
static int load_file(purr_fs_t *fs, const char *path, char *buf, uint32_t cap, uint32_t *len, int *used_bak)
{
    int r = purr_fs_slurp(fs, path, buf, cap, len);
    if (r < 0) {
        char bak[24];
        snprintf(bak, sizeof(bak), "%s.bak", path);
        if (purr_fs_slurp(fs, bak, buf, cap, len) == 0 && *len > 0) {
            *used_bak = 1;
            return 0;
        }
    }
    return r;
}

int purr_userstore_load(purr_fs_t *fs, purr_user_list_t *users, purr_shadow_list_t *shadow)
{
    purr_user_list_init(users);
    purr_shadow_list_init(shadow);
    if (!purr_fs_mounted(fs)) {
        return -1;
    }

    static EXT_RAM_BSS_ATTR char buf[BUF_LEN];
    uint32_t n = 0;
    int used_bak = 0;
    int pr = load_file(fs, PASSWD_PATH, buf, sizeof(buf), &n, &used_bak);
    if (pr == 0) {
        purr_user_list_parse(users, buf, n);
        if (n > 0 && users->count == 0) {                      /* there is text but no account in it: damaged, try the backup */
            if (!used_bak) {
                char bak[24];
                uint32_t bn = 0;
                snprintf(bak, sizeof(bak), "%s.bak", PASSWD_PATH);
                if (purr_fs_slurp(fs, bak, buf, sizeof(buf), &bn) == 0 && bn > 0) {
                    purr_user_list_init(users);
                    purr_user_list_parse(users, buf, bn);
                    used_bak = users->count > 0;
                }
            }
            if (users->count == 0) {
                pr = -1;
            }
        }
    }
    int sr = -1;
    if (pr >= 0) {
        sr = load_file(fs, SHADOW_PATH, buf, sizeof(buf), &n, &used_bak);
        if (sr == 0) {
            purr_shadow_list_parse(shadow, buf, n);
        }
    }
    if (pr < 0 || sr < 0) {                                    /* a file is there and cannot be read: never report "no accounts yet" */
        purr_user_list_init(users);
        purr_shadow_list_init(shadow);
        return PURR_USERSTORE_READ_ERROR;
    }
    return used_bak ? PURR_USERSTORE_FROM_BACKUP : 0;
}

int purr_userstore_save(purr_fs_t *fs, const purr_user_list_t *users, const purr_shadow_list_t *shadow)
{
    if (!purr_fs_mounted(fs)) {
        return -1;
    }
    purr_fs_mkdir(fs, "/etc");                    /* ignored if it already exists */

    static EXT_RAM_BSS_ATTR char buf[BUF_LEN];
    int n = purr_user_list_format(users, buf, sizeof(buf));
    if (n < 0 || purr_fs_write_kept(fs, PASSWD_PATH, buf, (uint32_t)n) != 0) {
        return -1;
    }
    n = purr_shadow_list_format(shadow, buf, sizeof(buf));
    if (n < 0 || purr_fs_write_kept(fs, SHADOW_PATH, buf, (uint32_t)n) != 0) {
        return -1;
    }
    return 0;
}
