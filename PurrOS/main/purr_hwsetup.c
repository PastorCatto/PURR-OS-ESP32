/* purr_hwsetup.c - see purr_hwsetup.h. */
#include "purr_hwsetup.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include "commands.h"
#include "purr_abi.h"
#include "purr_board.h"
#include "purr_driver.h"
#include "purr_driver_bus.h"
#include "purr_fetch.h"
#include "purr_fs.h"
#include "purr_hwscan.h"
#include "purr_manifest.h"
#include "purr_repo_dev.h"
#include "purr_util.h"

static const char *TAG = "hwsetup";

#define PROFILE_PATH   "/etc/board.conf"
#define DRIVERS_DIR    "/boot/drivers"
#define INDEX_MAX      (16 * 1024)
#define DRIVER_MAX     (96 * 1024)

/* ---------------------------------------------------------------- the profile and its board */

static purr_hwp_t s_prof;
static purr_board_t s_board;                         /* a copy of the real board with the discovered devices as its device list */
static purr_device_t s_devs[PURR_HWP_MAX];
static purr_i2c_dev_cfg_t s_cfg[PURR_HWP_MAX];
static purr_kv_cfg_t s_kv[PURR_HWP_MAX];
static char s_dev_name[PURR_HWP_MAX][PURR_HWP_NAME], s_dev_compat[PURR_HWP_MAX][PURR_HWP_COMPAT];

static void rebuild_board(void)
{
    s_board = *purr_board();
    for (int i = 0; i < s_prof.count; i++) {
        const purr_hwp_dev_t *d = &s_prof.dev[i];
        snprintf(s_dev_name[i], sizeof(s_dev_name[i]), "%s", d->name);
        snprintf(s_dev_compat[i], sizeof(s_dev_compat[i]), "%s", d->compatible);
        s_devs[i] = (purr_device_t){.name = s_dev_name[i], .compatible = s_dev_compat[i], .priority = PURR_PRI_OPTIONAL};
        for (int k = 0; k < PURR_MAX_EXTRA_PINS; k++) {
            s_devs[i].extra_pins[k] = PURR_PIN_NONE;
        }
        if (d->bus[0] != '\0' && strcmp(d->bus, "i2c") != 0) {
            /* A device configured by named values (a GPS on a UART, a LoRa radio on SPI): its driver reads them with purr_kv(); every pin it names is
             * claimed in the pin registry, so a second device cannot take it. */
            _Static_assert(PURR_HWP_KV == PURR_KV_MAX, "the profile's value list and the kernel's must be the same size");
            memset(&s_kv[i], 0, sizeof(s_kv[i]));
            int slot = 0;
            for (int k = 0; k < d->nkv && k < PURR_KV_MAX; k++) {
                memcpy(s_kv[i].kv[k].key, d->kv[k].key, sizeof(s_kv[i].kv[k].key));
                s_kv[i].kv[k].val = d->kv[k].val;
                s_kv[i].count++;
                if (purr_hwp_key_is_pin(d->kv[k].key) && d->kv[k].val >= 0 && slot < PURR_MAX_EXTRA_PINS) {
                    s_devs[i].extra_pins[slot++] = d->kv[k].val;
                }
            }
            s_devs[i].cfg = &s_kv[i];
        } else {
            s_cfg[i] = (purr_i2c_dev_cfg_t){.port = 1, .sda = d->sda, .scl = d->scl, .addr = d->addr, .hz = 100000};
            s_devs[i].cfg = &s_cfg[i];
            s_devs[i].extra_pins[0] = d->sda;        /* claimed in the pin registry: a second device cannot take them */
            s_devs[i].extra_pins[1] = d->scl;
        }
    }
    s_board.devices = s_devs;
    s_board.device_count = s_prof.count;
}

typedef struct { char *buf; size_t len, cap; } rd_t;

static int rd_cb(void *ctx, const void *data, uint32_t n)
{
    rd_t *r = ctx;
    if (r->len + n > r->cap) {
        return -1;
    }
    memcpy(r->buf + r->len, data, n);
    r->len += n;
    return 0;
}

static void load_profile(void)
{
    memset(&s_prof, 0, sizeof(s_prof));
    char *text = malloc(2048);
    purr_fs_t *fs = purr_login_fs();
    if (text == NULL || fs == NULL) {
        free(text);
        return;
    }
    rd_t r = {text, 0, 2047};
    if (purr_fs_read(fs, PROFILE_PATH, rd_cb, &r) < 0) {
        free(text);
        return;                                      /* no profile yet: nothing was ever set up */
    }
    purr_hwp_parse(&s_prof, text, r.len, purr_hw_reserved_s3());
    free(text);
    if (s_prof.dropped > 0) {
        ESP_LOGW(TAG, "%d entr%s in %s ignored (a bad pin, address or missing field)", s_prof.dropped, s_prof.dropped == 1 ? "y" : "ies", PROFILE_PATH);
    }
}

static int save_profile(void)
{
    char *text = malloc(2048);
    purr_fs_t *fs = purr_login_fs();
    int n = text != NULL ? purr_hwp_format(&s_prof, text, 2048) : -1;
    int rc = -1;
    if (n >= 0 && fs != NULL) {
        purr_fs_mkdir(fs, "/etc");
        rc = purr_fs_write(fs, PROFILE_PATH, text, (uint32_t)n) < 0 ? -1 : 0;
    }
    free(text);
    return rc;
}

void purr_hwsetup_boot(purr_cli_t *cli)
{
    load_profile();
    rebuild_board();
    purr_drivers_bind_extra(&s_board);
    int bound = 0;
    for (int i = 0; i < s_prof.count; i++) {
        bound += purr_device_state(s_prof.dev[i].name) == PURR_DEV_BOUND;
    }
    if (s_prof.count > 0) {
        purr_cli_printf(cli, "hardware: %d discovered device(s), %d bound\n", s_prof.count, bound);
    }
}

int purr_hwsetup_profile(purr_hwp_t *out)
{
    *out = s_prof;
    return s_prof.count;
}

int purr_hwsetup_is_set_up(const purr_hwscan_found_t *f)
{
    for (int i = 0; i < s_prof.count; i++) {
        const purr_hwp_dev_t *d = &s_prof.dev[i];
        if (d->addr == f->addr && d->sda == f->sda && d->scl == f->scl && f->known && strcmp(d->compatible, f->match.compatible) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- the driver index */

static purr_manifest_t *s_idx;                      /* allocated for one install or update, freed after (about 4 KB) */

static void say(void (*progress)(const char *), const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(void (*progress)(const char *), const char *fmt, ...)
{
    if (progress == NULL) {
        return;
    }
    char line[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    progress(line);
}

/* The directory part of a URL, with the trailing slash. */
static void url_dir(const char *url, char *out, size_t cap)
{
    const char *slash = strrchr(url, '/');
    size_t n = slash ? (size_t)(slash - url) + 1 : 0;
    if (n >= cap) {
        n = 0;
    }
    memcpy(out, url, n);
    out[n] = '\0';
}

typedef struct {
    char index_url[232];                 /* where the index came from */
    char dir[200];                       /* the files are next to it */
    int local;                           /* a dev server on the local network: plain HTTP, local addresses only */
} source_t;

/* Official first, then the dev server the owner chose (developer mode only). */
static int sources(source_t *out, int max)
{
    int n = 0;
    if (n < max) {
        char dir[200];
        url_dir(CONFIG_PURR_MODULE_INDEX_URL, dir, sizeof(dir));
        snprintf(out[n].index_url, sizeof(out[n].index_url), "%sdrivers.manifest", dir);
        snprintf(out[n].dir, sizeof(out[n].dir), "%s", dir);
        out[n].local = 0;
        n++;
    }
    purr_repo_server_t sv;
    if (n < max && purr_security_dev_active() && purr_repo_dev_last(&sv) == 0) {
        snprintf(out[n].index_url, sizeof(out[n].index_url), "%s/recovery/drivers.manifest", sv.base);
        snprintf(out[n].dir, sizeof(out[n].dir), "%s/recovery/", sv.base);
        out[n].local = 1;
        n++;
    }
    return n;
}

/* The index and the parsed manifest are about 4 KB each: take them from PSRAM when there is some, internal RAM is the scarce kind. */
static void *big_calloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
    return p != NULL ? p : calloc(1, n);
}

static int fetch(const source_t *s, const char *url, uint8_t **out, size_t *len, size_t max)
{
    if (!s->local) {
        return purr_fetch_alloc(url, out, len, max) == 0 ? 0 : -1;
    }
    uint8_t *b = malloc(max);
    if (b == NULL) {
        return -1;
    }
    if (purr_fetch_local_into(url, b, max, len) != 0 || *len == 0) {
        free(b);
        return -1;
    }
    *out = b;
    return 0;
}

static int abi_ok(const purr_manifest_entry_t *e)
{
    unsigned cur = PURR_DRIVER_BUS_ABI_VERSION;
    return (e->abi_min == 0 || e->abi_min <= cur) && (e->abi_max == 0 || e->abi_max >= cur);
}

/* Merges one source's driver entries into s_idx.entries starting at `have` (a later source never replaces an earlier one's entry of the same
 * driver and version). Returns the new count. */
static int load_index(const source_t *s, int have)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    if (fetch(s, s->index_url, &buf, &len, INDEX_MAX) != 0) {
        return have;
    }
    purr_manifest_t *mp = big_calloc(sizeof(*mp));
    if (mp == NULL) {
        free(buf);
        return have;
    }
    purr_manifest_t *m = mp;
    purr_manifest_parse(m, (const char *)buf, len);
    free(buf);
    const char *board = purr_board()->name;
    for (int i = 0; i < m->count && have < PURR_MANIFEST_MAX_ENTRIES; i++) {
        const purr_manifest_entry_t *e = &m->entries[i];
        if (strcmp(e->type, "driver") != 0 || e->blocked || !abi_ok(e) || e->compatible[0] == '\0') {
            continue;
        }
        if (strcmp(e->board, "any") != 0 && strcmp(e->board, board) != 0) {
            continue;
        }
        s_idx->entries[have] = *e;
        have++;
    }
    free(mp);
    return have;
}

/* ---------------------------------------------------------------- downloading and loading a driver */

static int name_clean(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (; *in && n + 1 < cap; in++) {
        char c = *in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            out[n++] = c;
        }
    }
    out[n] = '\0';
    return n > 0;
}

/* Fetches the entry's file, checks it against the index (size and SHA-256), writes /boot/drivers/<name>.cat. The loader checks the signature
 * when it loads. keep_old: park the file being replaced as .bak. Returns 0 or -1 with why. */
static int download_driver(const source_t *s, const purr_manifest_entry_t *e, int keep_old, char *path, size_t pathcap, char *why, size_t whycap)
{
    char name[32];
    if (!name_clean(e->component, name, sizeof(name))) {
        snprintf(why, whycap, "the index names it badly");
        return -1;
    }
    char url[300];
    snprintf(url, sizeof(url), "%s%s", s->dir, e->file);
    uint8_t *buf = NULL;
    size_t len = 0;
    if (fetch(s, url, &buf, &len, DRIVER_MAX) != 0) {
        snprintf(why, whycap, "could not download %s", e->file);
        return -1;
    }
    uint8_t digest[PURR_SHA256_LEN];
    purr_sha256(buf, len, digest);
    if (len != e->size || memcmp(digest, e->sha256, PURR_SHA256_LEN) != 0) {
        free(buf);
        snprintf(why, whycap, "%s does not match the index (size or hash)", e->file);
        return -1;
    }
    purr_fs_t *fs = purr_login_fs();
    purr_fs_mkdir(fs, "/boot");
    purr_fs_mkdir(fs, DRIVERS_DIR);
    snprintf(path, pathcap, DRIVERS_DIR "/%s.cat", name);
    if (keep_old) {
        char bak[64];
        snprintf(bak, sizeof(bak), "%s.bak", path);
        purr_fs_remove(fs, bak);
        purr_fs_rename(fs, path, bak);
    }
    int rc = purr_fs_write(fs, path, buf, (uint32_t)len) < 0 ? -1 : 0;
    free(buf);
    if (rc != 0) {
        snprintf(why, whycap, "could not save it (the file system is full?)");
    }
    return rc;
}

/* Makes sure a driver for `compatible` is loaded: already registered this boot, or fetched from the index (s_idx, cnt entries) and loaded through the
 * normal loader. 0 on success, else why. */
static int ensure_driver(const char *compatible, const source_t *src, int nsrc, int cnt, char *why, size_t whycap)
{
    if (purr_driver_registered(compatible)) {
        return 0;
    }
    const purr_manifest_entry_t *e = NULL;
    for (int k = 0; k < cnt; k++) {
        if (strcmp(s_idx->entries[k].compatible, compatible) == 0) {
            e = &s_idx->entries[k];
            break;
        }
    }
    if (e == NULL) {
        snprintf(why, whycap, "no driver for \"%s\" is listed yet", compatible);
        return -1;
    }
    char path[64];
    int ok = -1;
    for (int si = 0; si < nsrc && ok != 0; si++) {
        ok = download_driver(&src[si], e, 0, path, sizeof(path), why, whycap);
    }
    if (ok != 0) {
        return -1;
    }
    if (purr_driver_load_file(path) != 0) {
        purr_fs_remove(purr_login_fs(), path);
        snprintf(why, whycap, "the %s driver was refused (it must be signed unless developer mode allows it)", compatible);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- set up */

int purr_hwsetup_install(const purr_hwscan_report_t *rep, void (*progress)(const char *), char *summary, size_t cap)
{
    summary[0] = '\0';
    if (!purr_fs_mounted(purr_login_fs())) {
        snprintf(summary, cap, "The file system is not ready.");
        return -1;
    }
    int todo = 0;
    for (int i = 0; i < rep->nfound; i++) {
        todo += rep->found[i].known && !purr_hwsetup_is_set_up(&rep->found[i]);
    }
    if (todo == 0) {
        snprintf(summary, cap, "Nothing new to set up.");
        return 0;
    }
    source_t src[2];
    int nsrc = sources(src, 2);
    s_idx = big_calloc(sizeof(*s_idx));
    if (s_idx == NULL) {
        snprintf(summary, cap, "Out of memory.");
        return -1;
    }
    say(progress, "Reading the driver list...");
    int cnt = 0;
    for (int i = 0; i < nsrc; i++) {
        cnt = load_index(&src[i], cnt);
    }
    int done = 0, failed = 0;
    char first_fail[96] = "";
    for (int i = 0; i < rep->nfound; i++) {
        const purr_hwscan_found_t *f = &rep->found[i];
        if (!f->known || purr_hwsetup_is_set_up(f)) {
            continue;
        }
        char why[96] = "";
        say(progress, "Getting the %s driver...", f->match.compatible);
        if (ensure_driver(f->match.compatible, src, nsrc, cnt, why, sizeof(why)) != 0) {
            failed++;
            snprintf(first_fail, sizeof(first_fail), "%s", why);
            continue;
        }
        purr_hwp_dev_t d = {0};
        snprintf(d.compatible, sizeof(d.compatible), "%s", f->match.compatible);
        snprintf(d.label, sizeof(d.label), "%s", f->match.name);
        d.sda = f->sda;
        d.scl = f->scl;
        d.addr = f->addr;
        if (purr_hwp_add(&s_prof, &d, purr_hw_reserved_s3()) < 0) {
            failed++;
            snprintf(first_fail, sizeof(first_fail), "cannot add it to the profile (full, or an unusable pin)");
            continue;
        }
        done++;
    }
    free(s_idx);
    s_idx = NULL;
    if (done > 0) {
        if (save_profile() != 0) {
            snprintf(summary, cap, "Set up, but the profile could not be saved: it will be forgotten at restart.");
        }
        rebuild_board();
        purr_drivers_bind_extra(&s_board);
        int bound = 0;
        for (int i = 0; i < s_prof.count; i++) {
            bound += purr_device_state(s_prof.dev[i].name) == PURR_DEV_BOUND;
        }
        ESP_LOGI(TAG, "profile now has %d device(s), %d bound", s_prof.count, bound);
        snprintf(summary, cap, "Set up %d device(s)%s%s.", done, failed ? "; " : "", failed ? first_fail : "");
        return done;
    }
    snprintf(summary, cap, "Could not set up %d device(s): %s.", failed, first_fail);
    return 0;
}

/* ---------------------------------------------------------------- the installed drivers and updates */

typedef struct { purr_hwsetup_driver_t *out; int max, n; } ls_t;

static void ls_cb(void *ctx, const char *name, int is_dir, uint32_t size)
{
    (void)size;
    ls_t *l = ctx;
    size_t len = strlen(name);
    if (is_dir || len < 5 || strcmp(name + len - 4, ".cat") != 0 || l->n >= l->max) {
        return;
    }
    snprintf(l->out[l->n].file, sizeof(l->out[l->n].file), "%s", name);
    l->out[l->n].name[0] = l->out[l->n].version[0] = '\0';
    l->n++;
}

static int header_cb(void *ctx, const void *data, uint32_t n)
{
    rd_t *r = ctx;
    if (r->len >= r->cap) {
        return 1;                                    /* enough: stop reading */
    }
    uint32_t take = n < r->cap - r->len ? n : (uint32_t)(r->cap - r->len);
    memcpy(r->buf + r->len, data, take);
    r->len += take;
    return r->len >= r->cap ? 1 : 0;
}

int purr_hwsetup_list_drivers(purr_hwsetup_driver_t *out, int max)
{
    ls_t l = {out, max, 0};
    purr_fs_t *fs = purr_login_fs();
    if (fs == NULL || purr_fs_list(fs, DRIVERS_DIR, ls_cb, &l) != 0) {
        return 0;
    }
    for (int i = 0; i < l.n; i++) {
        char path[96];
        uint8_t hdr[sizeof(purr_image_header_t)];
        rd_t r = {(char *)hdr, 0, sizeof(hdr)};
        snprintf(path, sizeof(path), DRIVERS_DIR "/%s", out[i].file);
        purr_fs_read(fs, path, header_cb, &r);
        if (r.len >= sizeof(hdr)) {
            const purr_image_header_t *h = (const purr_image_header_t *)hdr;
            if (h->magic == PURR_IMAGE_MAGIC) {
                snprintf(out[i].name, sizeof(out[i].name), "%.31s", h->name);
                snprintf(out[i].version, sizeof(out[i].version), "%.11s", h->version);
            }
        }
        if (out[i].name[0] == '\0') {
            snprintf(out[i].name, sizeof(out[i].name), "%.31s", out[i].file);
        }
    }
    return l.n;
}

int purr_hwsetup_update_drivers(void (*progress)(const char *), char *summary, size_t cap)
{
    summary[0] = '\0';
    static purr_hwsetup_driver_t inst[PURR_HWSETUP_MAX_DRIVERS];
    int ninst = purr_hwsetup_list_drivers(inst, PURR_HWSETUP_MAX_DRIVERS);
    source_t src[2];
    int nsrc = sources(src, 2);
    s_idx = big_calloc(sizeof(*s_idx));
    if (s_idx == NULL) {
        snprintf(summary, cap, "Out of memory.");
        return -1;
    }
    say(progress, "Reading the driver list...");
    int cnt = 0;
    for (int i = 0; i < nsrc; i++) {
        cnt = load_index(&src[i], cnt);
    }
    if (cnt == 0) {
        free(s_idx);
        s_idx = NULL;
        snprintf(summary, cap, "Could not read a driver list (no network, or none published yet).");
        return -1;
    }
    int updated = 0;
    for (int i = 0; i < ninst; i++) {
        uint32_t have_v = 0;
        if (purr_version_parse(inst[i].version, strlen(inst[i].version), &have_v) != 0) {
            have_v = 0;
        }
        const purr_manifest_entry_t *best = NULL;
        uint32_t best_v = have_v;
        for (int k = 0; k < cnt; k++) {
            const purr_manifest_entry_t *e = &s_idx->entries[k];
            uint32_t v;
            if (strcmp(e->component, inst[i].name) != 0 || purr_version_parse(e->version, strlen(e->version), &v) != 0) {
                continue;
            }
            if (v > best_v) {
                best = e;
                best_v = v;
            }
        }
        if (best == NULL) {
            continue;
        }
        say(progress, "Updating %s to %s...", inst[i].name, best->version);
        char path[64], why[96];
        int ok = -1;
        for (int si = 0; si < nsrc && ok != 0; si++) {
            ok = download_driver(&src[si], best, 1, path, sizeof(path), why, sizeof(why));
        }
        if (ok == 0) {
            updated++;
        }
    }
    free(s_idx);
    s_idx = NULL;
    snprintf(summary, cap, updated ? "Updated %d driver(s). Restart to use them." : "All drivers are up to date.", updated);
    return updated;
}

/* ---------------------------------------------------------------- by hand */

int purr_hwsetup_add_manual(const char *compatible, int sda, int scl, int addr, char *summary, size_t cap)
{
    summary[0] = '\0';
    if (!purr_fs_mounted(purr_login_fs())) {
        snprintf(summary, cap, "The file system is not ready.");
        return -1;
    }
    purr_hwp_dev_t d = {0};
    snprintf(d.compatible, sizeof(d.compatible), "%s", compatible);
    snprintf(d.label, sizeof(d.label), "%s (added by hand)", compatible);
    d.sda = sda;
    d.scl = scl;
    d.addr = (uint8_t)addr;
    purr_hwp_t trial = s_prof;
    int r = purr_hwp_add(&trial, &d, purr_hw_reserved_s3());
    if (r < 0 || addr < 0 || addr > 255) {
        snprintf(summary, cap, "Not accepted: the pins must be free GPIOs (not flash, USB or boot), different, and the address 0x08-0x77.");
        return -1;
    }
    if (r == 1) {
        snprintf(summary, cap, "That device is already in the profile.");
        return 0;
    }
    source_t src[2];
    int nsrc = sources(src, 2);
    s_idx = big_calloc(sizeof(*s_idx));
    if (s_idx == NULL) {
        snprintf(summary, cap, "Out of memory.");
        return -1;
    }
    int cnt = 0;
    for (int i = 0; i < nsrc; i++) {
        cnt = load_index(&src[i], cnt);
    }
    char why[96] = "";
    int rc = ensure_driver(compatible, src, nsrc, cnt, why, sizeof(why));
    free(s_idx);
    s_idx = NULL;
    if (rc != 0) {
        snprintf(summary, cap, "%s.", why);
        return -1;
    }
    s_prof = trial;
    if (save_profile() != 0) {
        snprintf(summary, cap, "Added, but the profile could not be saved: it will be forgotten at restart.");
    }
    rebuild_board();
    purr_drivers_bind_extra(&s_board);
    const char *name = s_prof.dev[s_prof.count - 1].name;
    snprintf(summary, cap, "Added %s: %s.", name, purr_device_state(name) == PURR_DEV_BOUND ? "running" : "saved, but the driver did not bind (see the log)");
    return 0;
}

int purr_hwsetup_remove(const char *name, char *summary, size_t cap)
{
    for (int i = 0; i < s_prof.count; i++) {
        if (strcmp(s_prof.dev[i].name, name) == 0) {
            for (int j = i; j < s_prof.count - 1; j++) {
                s_prof.dev[j] = s_prof.dev[j + 1];
            }
            s_prof.count--;
            save_profile();
            snprintf(summary, cap, "Removed %s. It stops at the next restart.", name);
            return 0;
        }
    }
    snprintf(summary, cap, "No device called %s in the profile.", name);
    return -1;
}

int purr_hwsetup_read_text(const char *name, char *out, size_t cap)
{
    uint8_t buf[48] = {0};
    int n = purr_device_read(name, buf, sizeof(buf));
    if (n < 0) {
        snprintf(out, cap, "%s: no reading (not set up, not running, or the driver has nothing to read)", name);
        return -1;
    }
    int text = n > 1;
    for (int i = 0; i < n && text; i++) {
        text = buf[i] >= 32 && buf[i] < 127;
    }
    if (text) {
        snprintf(out, cap, "%s: %s", name, (const char *)buf);
        return 0;
    }
    size_t k = (size_t)snprintf(out, cap, "%s:", name);
    for (int i = 0; i < n && k + 5 < cap; i++) {
        k += (size_t)snprintf(out + k, cap - k, " %d", buf[i]);
    }
    return 0;
}

int purr_hwsetup_cmd(purr_cli_t *cli, int argc, char **argv)
{
    char msg[160];
    if (argc >= 6 && strcmp(argv[1], "add") == 0) {
        int r = purr_hwsetup_add_manual(argv[2], atoi(argv[3]), atoi(argv[4]), (int)strtol(argv[5], NULL, 0), msg, sizeof(msg));
        purr_cli_printf(cli, "%s\n", msg);
        return r == 0 ? 0 : 1;
    }
    if (argc >= 3 && strcmp(argv[1], "read") == 0) {
        int r = purr_hwsetup_read_text(argv[2], msg, sizeof(msg));
        purr_cli_printf(cli, "%s\n", msg);
        return r == 0 ? 0 : 1;
    }
    if (argc >= 3 && strcmp(argv[1], "remove") == 0) {
        int r = purr_hwsetup_remove(argv[2], msg, sizeof(msg));
        purr_cli_printf(cli, "%s\n", msg);
        return r == 0 ? 0 : 1;
    }
    if (argc == 1) {
        purr_hwp_t p;
        int n = purr_hwsetup_profile(&p);
        purr_cli_printf(cli, "%d device(s) in /etc/board.conf\n", n);
        for (int i = 0; i < n; i++) {
            purr_cli_printf(cli, "  %-18s driver \"%s\"  pins %d/%d  addr 0x%02X  %s\n", p.dev[i].name, p.dev[i].compatible, p.dev[i].sda, p.dev[i].scl,
                            p.dev[i].addr, purr_device_state(p.dev[i].name) == PURR_DEV_BOUND ? "running" : "not running");
        }
        return 0;
    }
    purr_cli_printf(cli, "usage: hwdev | hwdev add <driver> <sda> <scl> <addr> | hwdev read <name> | hwdev remove <name>\n");
    return 1;
}
