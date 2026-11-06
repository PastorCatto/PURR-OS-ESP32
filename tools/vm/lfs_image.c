/* lfs_image.c - builds a LittleFS root image on the host, with files and accounts in it, from the OS's own file system and account code (plan1.md, flash map v2).
 *
 *   lfs_image --blocks 2623 --out root.bin [--user name:password:admin|standard]... [--file /device/path=host/file]... [--mkdir /path]...
 *
 * The image is what the `root` partition holds: splice it into a flash image at the partition's offset (tools/vm/make_root.py does that). Meant for test images
 * (a user to sign in as, a picture pack already in place): the accounts use a fixed salt, so never use it for a device that matters. 4 KB blocks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "purr_fs.h"
#include "purr_userstore.h"
#include "purr_users.h"

#define BSIZE 4096

static uint8_t *s_ram;
static uint32_t s_blocks;

static int ram_read(void *ctx, uint32_t block, uint32_t off, void *buf, uint32_t size)
{
    (void)ctx;
    if (block >= s_blocks || off + size > BSIZE) return -1;
    memcpy(buf, s_ram + (size_t)block * BSIZE + off, size);
    return 0;
}

static int ram_prog(void *ctx, uint32_t block, uint32_t off, const void *buf, uint32_t size)
{
    (void)ctx;
    if (block >= s_blocks || off + size > BSIZE) return -1;
    uint8_t *d = s_ram + (size_t)block * BSIZE + off;
    const uint8_t *s = buf;
    for (uint32_t i = 0; i < size; i++) d[i] &= s[i];          /* flash semantics: programming only clears bits */
    return 0;
}

static int ram_erase(void *ctx, uint32_t block)
{
    (void)ctx;
    if (block >= s_blocks) return -1;
    memset(s_ram + (size_t)block * BSIZE, 0xFF, BSIZE);
    return 0;
}

static purr_fs_t fs;

static void mkdirs(const char *path)
{
    char p[256];
    snprintf(p, sizeof(p), "%s", path);
    for (char *c = p + 1; *c; c++) {
        if (*c == '/') {
            *c = '\0';
            purr_fs_mkdir(&fs, p);
            *c = '/';
        }
    }
    purr_fs_mkdir(&fs, p);
}

int main(int argc, char **argv)
{
    const char *out = NULL;
    purr_user_list_t ul;
    purr_shadow_list_t sh;
    purr_user_list_init(&ul);
    purr_shadow_list_init(&sh);
    int have_users = 0;
    for (int i = 1; i < argc; i++) {                            /* first pass: the size and the output */
        if (strcmp(argv[i], "--blocks") == 0 && i + 1 < argc) s_blocks = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out = argv[++i];
    }
    if (s_blocks < 16 || out == NULL) {
        fprintf(stderr, "usage: lfs_image --blocks N --out FILE [--user name:password:admin|standard] [--file /dest=hostfile] [--mkdir /path]\n");
        return 2;
    }
    s_ram = malloc((size_t)s_blocks * BSIZE);
    if (s_ram == NULL) return 1;
    memset(s_ram, 0xFF, (size_t)s_blocks * BSIZE);
    purr_bd_t bd = {ram_read, ram_prog, ram_erase, NULL, NULL, BSIZE, s_blocks};
    if (purr_fs_format(&fs, &bd) != 0) {
        fprintf(stderr, "could not format\n");
        return 1;
    }
    mkdirs("/etc");
    mkdirs("/home");
    uint8_t salt[PURR_SALT_LEN];
    memset(salt, 0x5A, sizeof(salt));
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
            char spec[160];
            snprintf(spec, sizeof(spec), "%s", argv[++i]);
            char *pw = strchr(spec, ':');
            if (pw == NULL) { fprintf(stderr, "--user needs name:password:role\n"); return 2; }
            *pw++ = '\0';
            char *role = strchr(pw, ':');
            if (role != NULL) *role++ = '\0';
            int admin = role == NULL || strcmp(role, "admin") == 0;
            uint8_t uid = (uint8_t)(ul.count + 1);
            if (purr_user_list_add(&ul, spec, uid, admin ? PURR_ROLE_USER_ADMIN : PURR_ROLE_USER_STANDARD) < 0) {
                fprintf(stderr, "could not add the user %s\n", spec);
                return 1;
            }
            purr_shadow_set(&sh, spec, pw, salt, PURR_PBKDF2_ITERATIONS);
            char home[96];
            snprintf(home, sizeof(home), "/home/%s", spec);
            mkdirs(home);
            have_users = 1;
        } else if (strcmp(argv[i], "--mkdir") == 0 && i + 1 < argc) {
            mkdirs(argv[++i]);
        } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            char spec[512];
            snprintf(spec, sizeof(spec), "%s", argv[++i]);
            char *eq = strchr(spec, '=');
            if (eq == NULL) { fprintf(stderr, "--file needs /dest=hostfile\n"); return 2; }
            *eq++ = '\0';
            FILE *f = fopen(eq, "rb");
            if (f == NULL) { fprintf(stderr, "cannot read %s\n", eq); return 1; }
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *data = malloc((size_t)n + 1);
            if (data == NULL || fread(data, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read %s\n", eq); return 1; }
            fclose(f);
            char dir[256];
            snprintf(dir, sizeof(dir), "%s", spec);
            char *sl = strrchr(dir, '/');
            if (sl != NULL && sl != dir) { *sl = '\0'; mkdirs(dir); }
            if (purr_fs_write(&fs, spec, data, (uint32_t)n) != 0) { fprintf(stderr, "could not write %s (%ld bytes)\n", spec, n); return 1; }
            free(data);
            printf("%s: %ld bytes\n", spec, n);
        }
    }
    if (have_users && purr_userstore_save(&fs, &ul, &sh) != 0) {
        fprintf(stderr, "could not save the accounts\n");
        return 1;
    }
    uint32_t used = 0, total = 0;
    purr_fs_usage(&fs, &used, &total);
    printf("root image: %u blocks, %u in use\n", (unsigned)s_blocks, (unsigned)used);
    purr_fs_unmount(&fs);
    FILE *o = fopen(out, "wb");
    if (o == NULL || fwrite(s_ram, 1, (size_t)s_blocks * BSIZE, o) != (size_t)s_blocks * BSIZE) { fprintf(stderr, "cannot write %s\n", out); return 1; }
    fclose(o);
    return 0;
}
