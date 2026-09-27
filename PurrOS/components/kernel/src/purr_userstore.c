#include "purr_userstore.h"

#include <string.h>

#define PASSWD_PATH "/etc/passwd"
#define SHADOW_PATH "/etc/shadow"

/* Files here are small (PURR_USER_MAX entries, well under a kilobyte each); one
 * fixed-size buffer read in whole is simpler than streaming. */
#define BUF_LEN 4096

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
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

int purr_userstore_load(purr_fs_t *fs, purr_user_list_t *users, purr_shadow_list_t *shadow)
{
    purr_user_list_init(users);
    purr_shadow_list_init(shadow);
    if (!purr_fs_mounted(fs)) {
        return -1;
    }

    static char buf[BUF_LEN];
    accum_t a = {buf, sizeof(buf), 0};
    if (purr_fs_read(fs, PASSWD_PATH, accumulate, &a) == 0) {
        purr_user_list_parse(users, buf, a.len);
    }

    a.len = 0;
    if (purr_fs_read(fs, SHADOW_PATH, accumulate, &a) == 0) {
        purr_shadow_list_parse(shadow, buf, a.len);
    }
    return 0;
}

int purr_userstore_save(purr_fs_t *fs, const purr_user_list_t *users, const purr_shadow_list_t *shadow)
{
    if (!purr_fs_mounted(fs)) {
        return -1;
    }
    purr_fs_mkdir(fs, "/etc");                    /* ignored if it already exists */

    static char buf[BUF_LEN];
    int n = purr_user_list_format(users, buf, sizeof(buf));
    if (n < 0 || purr_fs_write(fs, PASSWD_PATH, buf, (uint32_t)n) != 0) {
        return -1;
    }
    n = purr_shadow_list_format(shadow, buf, sizeof(buf));
    if (n < 0 || purr_fs_write(fs, SHADOW_PATH, buf, (uint32_t)n) != 0) {
        return -1;
    }
    return 0;
}
