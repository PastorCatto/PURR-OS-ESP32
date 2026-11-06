#include "esp_attr.h"
#include "purr_ui_assets.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "commands.h"
#include "purr_fs.h"
#include "purr_sd.h"
#include "purr_util.h"

static const char *TAG = "ui_assets";

#define MAGIC 0x54534150u   /* 'PAST', little-endian: "Purr ASset Table" */
#define MAX_ASSETS 128
#define MAX_PACK_BYTES (5u * 1024u * 1024u)

/* The pack file (plan1.md "Flash map v2", plan7.md "UI packages"): the same bytes `purrstrap uiassets` has always written,
 *   header_t, dir_entry_t[header.count], then the pixel data, each entry's offset/length pointing into that region;
 * crc32 covers everything from the end of the header to the end of the pixel data. It is a FILE in LittleFS now, not a partition:
 *   /ui/<shell>/pack.bin           the shell's own default pack (shell = w10m or android)
 *   /ui/themes/<id>/pack.bin       an installed theme (assets only; /ui/themes/<id>/theme.conf says which shell it is for)
 *   /ui/active.conf                "theme=<id>": the theme chosen in Settings, applied at start (empty or absent: none)
 * Both packs are read into PSRAM once at start. A lookup asks the active theme first and the shell's default pack second. */
typedef struct {
    uint32_t magic;
    uint32_t count;
    uint32_t crc32;
} header_t;

typedef struct {
    char name[24];
    uint32_t offset;       /* from the start of the pixel-data region */
    uint32_t length;
    uint16_t width;
    uint16_t height;
    uint8_t color_format;  /* PURR_ASSET_RGB565 or PURR_ASSET_RGB565A8 */
    uint8_t reserved[3];
} dir_entry_t;

typedef struct {
    uint8_t *data;                          /* the whole file, in PSRAM */
    purr_asset_t assets[MAX_ASSETS];
    int count;
    volatile int bad;                       /* the background check found the image data damaged: hand out nothing more from this pack */
} pack_t;

static EXT_RAM_BSS_ATTR pack_t s_default, s_theme;
static char s_theme_id[40];                 /* the theme in use, "" if none */
static char s_theme_failed[40];             /* a theme that was chosen but could not be used, "" if none */

typedef struct {
    const void *dir;
    size_t dir_bytes;
    const void *pixels;
    size_t used;
    uint32_t want;
    pack_t *pack;
} crc_job_t;

/* The CRC over several MB of flash takes seconds. Done before the shell was drawn it left a white screen for all that time, so it runs
 * after, in small steps. The directory's bounds are checked up front (nothing can read outside the partition); the CRC only guards the
 * pixels themselves, so a bad one just means the pictures stop being offered. */
/* 4 KB, was 3 KB (1.0.28, plan7.md): measured in the emulator, the task used about 2 KB at its deepest, so 3 KB was enough there; the extra KB is margin, freed again when the task ends.
 * The line it logs at the end says how much of the stack was never used. */
#define CRC_TASK_STACK 4096
static void crc_task(void *arg)
{
    crc_job_t j = *(crc_job_t *)arg;
    free(arg);
    uint32_t crc = purr_crc32(0, j.dir, j.dir_bytes);
    const uint8_t *p = j.pixels;
    for (size_t off = 0; off < j.used; off += 16384) {
        size_t n = j.used - off < 16384 ? j.used - off : 16384;
        crc = purr_crc32(crc, p + off, n);
        vTaskDelay(1);
    }
    if (crc != j.want) {
        j.pack->bad = 1;
        ESP_LOGW(TAG, "CRC mismatch (corrupt or partial write): the pictures are switched off");
    } else {
        ESP_LOGI(TAG, "image data checked");
    }
    ESP_LOGI(TAG, "assets-crc: %u of %u stack bytes were never used", (unsigned)uxTaskGetStackHighWaterMark(NULL), CRC_TASK_STACK);
    vTaskDelete(NULL);
}


static int read_cb(void *ctx, const void *data, uint32_t len)
{
    struct { uint8_t *buf; uint32_t cap, len; } *r = ctx;
    if (r->len + len > r->cap) {
        return -1;
    }
    memcpy(r->buf + r->len, data, len);
    r->len += len;
    return 0;
}

/* Reads the pack file at `path` into PSRAM and indexes it. 0, or an esp_err_t-style negative code; `p` is left empty on failure. */
static esp_err_t load_pack(pack_t *p, const char *path)
{
    memset(p, 0, sizeof(*p));
    purr_fs_t *fs = purr_login_fs();
    if (fs == NULL || !purr_fs_mounted(fs)) {
        return ESP_ERR_NOT_FOUND;                      /* no file system here (the recovery loader): no pictures, and nothing else is affected */
    }
    int is_dir = 0;
    uint32_t size = 0;
    if (purr_fs_stat(fs, path, &is_dir, &size) != 0 || is_dir) {
        return ESP_ERR_NOT_FOUND;
    }
    if (size < sizeof(header_t) || size > MAX_PACK_BYTES) {
        ESP_LOGW(TAG, "%s: %u bytes is not a pack", path, (unsigned)size);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGW(TAG, "%s: no memory for the %u byte pack", path, (unsigned)size);
        return ESP_ERR_NO_MEM;
    }
    struct { uint8_t *buf; uint32_t cap, len; } r = {buf, size, 0};
    if (purr_fs_read(fs, path, read_cb, &r) != 0 || r.len != size) {
        ESP_LOGW(TAG, "%s: could not be read", path);
        free(buf);
        return ESP_FAIL;
    }
    const header_t *hdr = (const header_t *)buf;
    if (hdr->magic != MAGIC) {
        ESP_LOGW(TAG, "%s: unrecognized (magic %08x)", path, (unsigned)hdr->magic);
        free(buf);
        return ESP_ERR_INVALID_CRC;
    }
    if (hdr->count > MAX_ASSETS) {
        ESP_LOGW(TAG, "%s: %u assets, more than the %d this build can index", path, (unsigned)hdr->count, MAX_ASSETS);
        free(buf);
        return ESP_ERR_INVALID_SIZE;
    }
    size_t dir_bytes = (size_t)hdr->count * sizeof(dir_entry_t);
    if (sizeof(header_t) + dir_bytes > size) {
        ESP_LOGW(TAG, "%s: the directory runs past the end of the file", path);
        free(buf);
        return ESP_ERR_INVALID_SIZE;
    }
    const dir_entry_t *dir = (const dir_entry_t *)(buf + sizeof(header_t));
    const uint8_t *pixel_region = (const uint8_t *)dir + dir_bytes;
    size_t region_bytes = size - sizeof(header_t) - dir_bytes;
    /* `used` is how far into the pixel region the last entry reaches; every entry has to lie inside the file. */
    size_t used = 0;
    for (uint32_t i = 0; i < hdr->count; i++) {
        uint64_t end = (uint64_t)dir[i].offset + dir[i].length;
        if (end > region_bytes || (dir[i].offset & 1u) != 0) {
            ESP_LOGW(TAG, "%s: entry %u's pixels run past the end of the file or are not aligned", path, (unsigned)i);
            free(buf);
            return ESP_ERR_INVALID_SIZE;
        }
        if (end > used) {
            used = (size_t)end;
        }
    }
    p->data = buf;
    crc_job_t *job = malloc(sizeof(*job));
    bool check = false;
    if (job != NULL) {
        job->dir = dir;
        job->dir_bytes = dir_bytes;
        job->pixels = pixel_region;
        job->used = used;
        job->want = hdr->crc32;
        job->pack = p;
        check = xTaskCreate(crc_task, "assets-crc", CRC_TASK_STACK, job, 1, NULL) == pdPASS;
        if (!check) {
            free(job);
        }
    }
    if (!check) {                                  /* no task: do it here */
        uint32_t crc = purr_crc32(0, dir, dir_bytes);
        crc = purr_crc32(crc, pixel_region, used);
        if (crc != hdr->crc32) {
            ESP_LOGW(TAG, "%s: CRC mismatch (corrupt or partial write)", path);
            p->data = NULL;
            free(buf);
            return ESP_ERR_INVALID_CRC;
        }
    }
    for (uint32_t i = 0; i < hdr->count; i++) {
        purr_asset_t *a = &p->assets[p->count];
        memcpy(a->name, dir[i].name, sizeof(a->name));
        a->name[sizeof(a->name) - 1] = '\0';
        a->pixels = (const uint16_t *)(pixel_region + dir[i].offset);
        a->width = dir[i].width;
        a->height = dir[i].height;
        a->color_format = dir[i].color_format;
        p->count++;
    }
    ESP_LOGI(TAG, "%s: %d asset(s)", path, p->count);
    return ESP_OK;
}

#if CONFIG_PURR_SHELL_ANDROID
#define SHELL_ID "android"
#else
#define SHELL_ID "w10m"
#endif

static int id_ok(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n >= sizeof(s_theme_id)) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

/* "key=value" from a small text file, into out; 1 if the key is there and not empty. */
static int conf_get(const char *path, const char *key, char *out, size_t cap)
{
    purr_fs_t *fs = purr_login_fs();
    char buf[512];
    uint32_t n = 0;
    out[0] = '\0';
    if (purr_fs_slurp(fs, path, buf, sizeof(buf), &n) != 0) {
        return 0;
    }
    size_t kl = strlen(key);
    for (char *p = buf; *p;) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            size_t len = strcspn(p + kl + 1, "\r\n");
            if (len >= cap) {
                len = cap - 1;
            }
            memcpy(out, p + kl + 1, len);
            out[len] = '\0';
            return len > 0;
        }
        char *nl = strchr(p, '\n');
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
    return 0;
}

esp_err_t purr_ui_assets_init(void)
{
    s_theme_id[0] = s_theme_failed[0] = '\0';
    esp_err_t e = load_pack(&s_default, "/ui/" SHELL_ID "/pack.bin");
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "no default pack for this shell (%s): pictures are missing until one is installed", esp_err_to_name(e));
    }
    char id[40];
    if (conf_get("/ui/active.conf", "theme", id, sizeof(id)) && id_ok(id)) {
        char path[96], shell[16];
        snprintf(path, sizeof(path), "/ui/themes/%s/theme.conf", id);
        int fits = 1;
        if (conf_get(path, "shell", shell, sizeof(shell)) && strcmp(shell, "any") != 0 && strcmp(shell, SHELL_ID) != 0) {
            ESP_LOGW(TAG, "theme %s is for the %s shell, not this one", id, shell);
            fits = 0;
        }
        snprintf(path, sizeof(path), "/ui/themes/%s/pack.bin", id);
        if (fits && load_pack(&s_theme, path) == ESP_OK) {
            snprintf(s_theme_id, sizeof(s_theme_id), "%s", id);
            ESP_LOGI(TAG, "theme: %s", id);
        } else {
            snprintf(s_theme_failed, sizeof(s_theme_failed), "%s", id);       /* the shell starts with its own pack and says so */
        }
    }
    return s_default.data != NULL ? ESP_OK : (e != ESP_OK ? e : ESP_FAIL);
}

static const purr_asset_t *find_in(pack_t *p, const char *name)
{
    if (p->data == NULL || p->bad) {
        return NULL;
    }
    for (int i = 0; i < p->count; i++) {
        if (strcmp(p->assets[i].name, name) == 0) {
            return &p->assets[i];
        }
    }
    return NULL;
}

const purr_asset_t *purr_ui_assets_find(const char *name)
{
    const purr_asset_t *a = find_in(&s_theme, name);                          /* the theme first */
    return a != NULL ? a : find_in(&s_default, name);                         /* then the shell's own pack */
}

const char *purr_ui_assets_theme(void)
{
    return s_theme_id;
}

const char *purr_ui_assets_theme_failed(void)
{
    return s_theme_failed;
}

const char *purr_ui_assets_shell(void)
{
    return SHELL_ID;
}


/* ---------------------------------------------------------------- the installed themes */

typedef struct {
    purr_theme_info_t *out;
    int max, n;
} list_ctx_t;

static void list_cb(void *ctx, const char *name, int is_dir, uint32_t size)
{
    (void)size;
    list_ctx_t *l = ctx;
    if (!is_dir || l->n >= l->max || !id_ok(name)) {
        return;
    }
    purr_theme_info_t *t = &l->out[l->n];
    memset(t, 0, sizeof(*t));
    snprintf(t->id, sizeof(t->id), "%s", name);
    char path[96];
    snprintf(path, sizeof(path), "/ui/themes/%s/theme.conf", name);
    if (!conf_get(path, "name", t->name, sizeof(t->name))) {
        snprintf(t->name, sizeof(t->name), "%s", name);
    }
    conf_get(path, "author", t->author, sizeof(t->author));
    conf_get(path, "version", t->version, sizeof(t->version));
    if (!conf_get(path, "shell", t->shell, sizeof(t->shell))) {
        snprintf(t->shell, sizeof(t->shell), "any");
    }
    l->n++;
}

/* The same listing for the SD card: themes there are only added when they are not already in a slot. */
static void list_sd_cb(void *ctx, const char *name, int is_dir, uint32_t size)
{
    (void)size;
    list_ctx_t *l = ctx;
    if (!is_dir || l->n >= l->max || !id_ok(name)) {
        return;
    }
    for (int i = 0; i < l->n; i++) {
        if (strcmp(l->out[i].id, name) == 0) {
            return;
        }
    }
    purr_theme_info_t *t = &l->out[l->n];
    memset(t, 0, sizeof(*t));
    snprintf(t->id, sizeof(t->id), "%s", name);
    t->on_sd = 1;
    snprintf(t->name, sizeof(t->name), "%s", name);                    /* the SD card's theme.conf is not read here: the name is the folder's */
    snprintf(t->shell, sizeof(t->shell), "any");
    l->n++;
}

int purr_ui_theme_list(purr_theme_info_t *out, int max)
{
    purr_fs_t *fs = purr_login_fs();
    if (fs == NULL || !purr_fs_mounted(fs) || out == NULL || max <= 0) {
        return 0;
    }
    list_ctx_t l = {out, max, 0};
    purr_fs_list(fs, "/ui/themes", list_cb, &l);
    if (purr_sd_present()) {
        purr_sd_list("/sd/ui/themes", list_sd_cb, &l);
    }
    return l.n;
}

static void count_cb(void *ctx, const char *name, int is_dir, uint32_t size)
{
    (void)size;
    struct { const char *except; int n; } *c = ctx;
    if (is_dir && id_ok(name) && (c->except == NULL || strcmp(c->except, name) != 0)) {
        c->n++;
    }
}

int purr_ui_theme_slots_used(const char *except)
{
    purr_fs_t *fs = purr_login_fs();
    struct { const char *except; int n; } c = {except, 0};
    if (fs != NULL && purr_fs_mounted(fs)) {
        purr_fs_list(fs, "/ui/themes", count_cb, &c);
    }
    return c.n;
}

/* Copies one file from the SD card into LittleFS (a theme's pack or its theme.conf). */
static int copy_from_sd(const char *from, const char *to, uint32_t cap, int required)
{
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        return -1;
    }
    struct { uint8_t *buf; uint32_t cap, len; } r = {buf, cap, 0};
    int rc = -1;
    if (purr_sd_read(from, read_cb, &r) == 0 && r.len > 0) {
        rc = purr_fs_write(purr_login_fs(), to, buf, r.len) == 0 ? 0 : -1;
    } else if (!required) {
        rc = 0;
    }
    free(buf);
    return rc;
}

int purr_ui_theme_chosen(char *id, size_t cap)
{
    char v[40];
    if (cap > 0) {
        id[0] = '\0';
    }
    if (!conf_get("/ui/active.conf", "theme", v, sizeof(v)) || !id_ok(v)) {
        return 0;
    }
    snprintf(id, cap, "%s", v);
    return 1;
}

int purr_ui_theme_select(const char *id)
{
    purr_fs_t *fs = purr_login_fs();
    if (fs == NULL || !purr_fs_mounted(fs)) {
        return -1;
    }
    char text[64];
    if (id == NULL || id[0] == '\0') {
        snprintf(text, sizeof(text), "theme=\n");
    } else {
        char path[96];
        int is_dir = 0;
        uint32_t size = 0;
        snprintf(path, sizeof(path), "/ui/themes/%s", id);
        if (!id_ok(id)) {
            return -1;
        }
        if (purr_fs_stat(fs, path, &is_dir, &size) != 0 || !is_dir) {
            /* Not in a slot: if it is on the SD card, copy it in (the card can then be removed). All slots taken: say so, nothing is changed. */
            char from[112], to[112];
            snprintf(from, sizeof(from), "/sd/ui/themes/%s/pack.bin", id);
            if (!purr_sd_present()) {
                return -1;
            }
            if (purr_ui_theme_slots_used(NULL) >= PURR_UI_THEME_SLOTS) {
                return -2;
            }
            purr_fs_mkdir(fs, "/ui");
            purr_fs_mkdir(fs, "/ui/themes");
            purr_fs_mkdir(fs, path);
            snprintf(to, sizeof(to), "%s/pack.bin", path);
            if (copy_from_sd(from, to, MAX_PACK_BYTES, 1) != 0) {
                purr_fs_remove(fs, path);
                return -1;
            }
            snprintf(from, sizeof(from), "/sd/ui/themes/%s/theme.conf", id);
            snprintf(to, sizeof(to), "%s/theme.conf", path);
            copy_from_sd(from, to, 1024, 0);
        }
        snprintf(text, sizeof(text), "theme=%s\n", id);
    }
    purr_fs_mkdir(fs, "/ui");
    return purr_fs_write_kept(fs, "/ui/active.conf", text, (uint32_t)strlen(text)) == 0 ? 0 : -1;
}

int purr_ui_theme_remove(const char *id)
{
    purr_fs_t *fs = purr_login_fs();
    if (fs == NULL || !purr_fs_mounted(fs) || id == NULL || !id_ok(id)) {
        return -1;
    }
    char chosen[40];
    if (purr_ui_theme_chosen(chosen, sizeof(chosen)) && strcmp(chosen, id) == 0) {
        purr_ui_theme_select("");
    }
    char path[112];
    static const char *files[] = {"pack.bin", "pack.bin.bak", "theme.conf", "theme.conf.bak", ".version"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "/ui/themes/%s/%s", id, files[i]);
        purr_fs_remove(fs, path);                          /* a file that is not there is fine */
    }
    snprintf(path, sizeof(path), "/ui/themes/%s", id);
    return purr_fs_remove(fs, path) == 0 ? 0 : -1;
}
