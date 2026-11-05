/*
 * purr_store.c - see purr_store.h and Repositories/SPEC.md.
 *
 * Layout on the root file system:
 *   /etc/repos.conf                       the repositories an administrator trusted (purr_repo_trust.h)
 *   /var/store/<id>/index.json (+ .sig)   the last signed index fetched from that repository, checked against its key
 *   /var/store/<id>/idx-<h8>.json (+ .sig) a copy of the index an install came from: the receipt of an app points at it
 *   /home/<user>/apps/<name>/receipt      repo=, idx=, sha= (the SHA-256 of the package)
 *   /home/<user>/assets/<item>/...        flash assets and extras, with a .version file
 *   /sd/purr/<item>/...                   SD card assets (when an SD card is mounted)
 */
#include "purr_store.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "commands.h"
#include "login.h"
#include "purr_brand.h"
#include "purr_sd.h"
#include "purr_catcalls.h"
#include "purr_catrt.h"
#include "purr_catrt_power.h"
#include "purr_crypto_mbedtls.h"
#include "purr_fetch.h"
#include "purr_fs.h"
#include "purr_net.h"
#include "purr_perm.h"
#include "purr_repo.h"
#include "purr_repo_trust.h"
#include "purr_settings.h"
#include "purr_users.h"
#include "purr_util.h"
#include "purr_w10m_priv.h"

#define TAG "store"
#define REPOS_PATH "/etc/repos.conf"
#define STORE_DIR "/var/store"
#define MAX_ITEMS 48
#define IDX8 8

typedef struct {
    int busy;
    int slot;
    char what[16], a[104], b[24];
    char msg[200];
    int rc;
} job_t;

static job_t s_job;
static char s_last[200];
static SemaphoreHandle_t s_lock;
static purr_repos_t s_repos;
static int s_repos_loaded;

/* a repository fetched and checked, waiting for the person to read its fingerprint and say yes */
static struct {
    int valid;
    purr_repo_info_t info;
    char url[96];
    purr_repo_tier_t tier;
    purr_repo_add_class_t cls;
} s_pending;

static TaskHandle_t s_job_task;                     /* the task running the current job (it may rebuild the item list while the job is busy) */
static purr_store_item_t *s_items;
static int s_nitems, s_nupdates, s_items_dirty = 1;

/* ------------------------------------------------------------------ small helpers */

static purr_fs_t *fs(void)
{
    return purr_login_fs();
}

/* The files the Store touches live on the flash file system, or under /sd on the card; these pick by path. */
static int x_read(const char *path, purr_fs_read_fn cb, void *ctx)
{
    return purr_sd_is_path(path) ? purr_sd_read(path, cb, ctx) : purr_fs_read(fs(), path, cb, ctx);
}

static int x_write(const char *path, const void *data, uint32_t len)
{
    return purr_sd_is_path(path) ? purr_sd_write(path, data, len) : purr_fs_write(fs(), path, data, len);
}

static int x_mkdir(const char *path)
{
    return purr_sd_is_path(path) ? purr_sd_mkdir(path) : purr_fs_mkdir(fs(), path);
}

static int x_remove(const char *path)
{
    return purr_sd_is_path(path) ? purr_sd_remove(path) : purr_fs_remove(fs(), path);
}

static int x_list(const char *path, purr_fs_list_fn cb, void *ctx)
{
    return purr_sd_is_path(path) ? purr_sd_list(path, cb, ctx) : purr_fs_list(fs(), path, cb, ctx);
}

static void lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p != NULL ? p : malloc(n);
}

typedef struct {
    uint8_t *buf;
    uint32_t cap, len;
    int over;
} rb_t;

static int rb_cb(void *ctx, const void *data, uint32_t len)
{
    rb_t *r = ctx;
    if (r->len + len > r->cap) {
        r->over = 1;
        return 1;
    }
    memcpy(r->buf + r->len, data, len);
    r->len += len;
    return 0;
}

/* Reads a whole file into buf (cap bytes, NUL-terminated if there is room). Returns the length, or -1. */
static int read_file(const char *path, uint8_t *buf, uint32_t cap, uint32_t *len_out)
{
    rb_t r = {buf, cap > 0 ? cap - 1 : 0, 0, 0};
    if (x_read(path, rb_cb, &r) < 0 || r.over) {
        return -1;
    }
    buf[r.len] = '\0';
    if (len_out != NULL) {
        *len_out = r.len;
    }
    return (int)r.len;
}

static void mkdirs(const char *path)
{
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            x_mkdir(tmp);
            *p = '/';
        }
    }
    x_mkdir(tmp);
}

static void hex_of(const uint8_t *d, size_t n, char *out)
{
    static const char x[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = x[d[i] >> 4];
        out[i * 2 + 1] = x[d[i] & 15];
    }
    out[n * 2] = '\0';
}

static int is_admin_now(void)
{
    return purr_login_is_root() || purr_login_current()->role == PURR_ROLE_USER_ADMIN;
}

static const char *user_name(void)
{
    return purr_login_current()->name;
}

static void setmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void setmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_job.msg, sizeof(s_job.msg), fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ the trusted list */

static void repos_load(void)
{
    lock();
    if (!s_repos_loaded && purr_fs_mounted(fs())) {
        static uint8_t buf[PURR_REPOS_MAX * 480 + 128];
        uint32_t n = 0;
        memset(&s_repos, 0, sizeof(s_repos));
        if (read_file(REPOS_PATH, buf, sizeof(buf), &n) >= 0) {
            purr_repos_parse((const char *)buf, n, &s_repos);
        }
        s_repos_loaded = 1;
    }
    unlock();
}

static int repos_save(void)
{
    static char buf[PURR_REPOS_MAX * 480 + 128];
    int n = purr_repos_format(&s_repos, buf, sizeof(buf));
    if (n < 0) {
        return -1;
    }
    return purr_fs_write(fs(), REPOS_PATH, buf, (uint32_t)n) < 0 ? -1 : 0;
}

/* ------------------------------------------------------------------ fetching a repository's files */

/* <base>/<rel> for a flat repository, <base>/purr/v1/<rel> for a tree. */
static int file_url(const purr_repo_entry_t *e, const char *rel, char *out, size_t cap)
{
    if (e->flat) {
        return snprintf(out, cap, "%s/%s", e->url, rel) < (int)cap ? 0 : -1;
    }
    return purr_repo_url_join(e->url, rel, out, cap);
}

static int fetch_to(const char *url, uint8_t *buf, size_t cap, size_t *got)
{
    esp_err_t err = purr_fetch_into(url, buf, cap, got);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s", url, esp_err_to_name(err));
        return -1;
    }
    return 0;
}

static int net_up(void)
{
    purr_net_status_t st;
    purr_net_status(&st);
    return st.connected || purr_net_wired();
}

/* ------------------------------------------------------------------ add a repository */

static void tidy_base(const char *in, char *out, size_t cap)
{
    snprintf(out, cap, "%s", in);
    size_t n = strlen(out);
    while (n > 0 && out[n - 1] == '/') {
        out[--n] = '\0';
    }
    for (const char *suf = "/repo.json"; n > strlen(suf) && strcmp(out + n - strlen(suf), suf) == 0;) {   /* a pasted link to the file itself */
        out[n - strlen(suf)] = '\0';
        break;
    }
}

static int repo_add_fetch(const char *typed)
{
    char base[96], why[80];
    tidy_base(typed, base, sizeof(base));
    if (!purr_repo_url_allowed(PURR_REPO_STORE, base, why, sizeof(why))) {
        setmsg("That address is not allowed: %s.", why);
        return -1;
    }
    if (!net_up()) {
        setmsg("Connect to the network first.");
        return -1;
    }
    uint8_t *buf = big_alloc(8192);
    if (buf == NULL) {
        setmsg("Out of memory.");
        return -1;
    }
    char url[160];
    size_t got = 0, sgot = 0;
    uint8_t sigtxt[200];
    int flat = 1;
    purr_repo_entry_t probe = {.flat = 1};
    snprintf(probe.url, sizeof(probe.url), "%s", base);
    if (file_url(&probe, "repo.json", url, sizeof(url)) != 0 || fetch_to(url, buf, 4096, &got) != 0) {
        probe.flat = 0;                                         /* not a release folder: try a web host's purr/v1 tree */
        flat = 0;
        if (file_url(&probe, "repo.json", url, sizeof(url)) != 0 || fetch_to(url, buf, 4096, &got) != 0) {
            setmsg("I could not find a repository there. A repository has a repo.json.");
            free(buf);
            return -1;
        }
    }
    char sigurl[176];
    snprintf(sigurl, sizeof(sigurl), "%s.sig", url);
    if (fetch_to(sigurl, sigtxt, sizeof(sigtxt), &sgot) != 0) {
        setmsg("The repository has no repo.json.sig, so I cannot check who made it.");
        free(buf);
        return -1;
    }
    char err[96];
    memset(&s_pending, 0, sizeof(s_pending));
    uint8_t sig[PURR_SIG_LEN];
    if (purr_repo_info_parse((const char *)buf, got, &s_pending.info, err, sizeof(err)) != 0 || purr_repo_sig_decode((const char *)sigtxt, sgot, sig) != 0 ||
        purr_repo_info_check(&purr_crypto_mbedtls, &s_pending.info, buf, got, sig, err, sizeof(err)) != 0) {
        setmsg("That is not a repository I can trust: %s.", err[0] ? err : "its signature does not check out");
        memset(&s_pending, 0, sizeof(s_pending));
        free(buf);
        return -1;
    }
    free(buf);
    s_pending.info.flat = (uint8_t)flat;
    snprintf(s_pending.url, sizeof(s_pending.url), "%s", base);
    s_pending.tier = purr_repo_endorsement_tier(&purr_crypto_mbedtls, purr_os_keybag(), &s_pending.info);
    repos_load();
    s_pending.cls = purr_repos_classify(&s_repos, &s_pending.info, base);
    if (s_pending.tier == PURR_TIER_UNTRUSTED) {
        if (!purr_security_dev_active()) {
            setmsg("This repository says it is endorsed, but the endorsement does not check out. Developer mode has to be on to use it.");
            memset(&s_pending, 0, sizeof(s_pending));
            return -1;
        }
    }
    s_pending.valid = 1;
    setmsg("Check the fingerprint, then confirm.");
    return 0;
}

static int repo_confirm(void)
{
    if (!s_pending.valid) {
        setmsg("There is nothing to confirm.");
        return -1;
    }
    repos_load();
    lock();
    int rc = purr_repos_add(&s_repos, &s_pending.info, s_pending.url, s_pending.tier, (uint32_t)time(NULL) > 1000000000u ? (uint32_t)time(NULL) : 0);
    if (rc == 0) {
        purr_fs_mkdir(fs(), "/var");
        purr_fs_mkdir(fs(), STORE_DIR);
        rc = repos_save();
    }
    unlock();
    if (rc != 0) {
        setmsg("I could not save the list of repositories (it may be full).");
        return -1;
    }
    setmsg("Added %s (%s).", s_pending.info.name, purr_repo_tier_name(s_pending.tier));
    s_pending.valid = 0;
    s_items_dirty = 1;
    return 0;
}

static int repo_remove(const char *id)
{
    repos_load();
    lock();
    int rc = purr_repos_remove(&s_repos, id);
    if (rc == 0) {
        repos_save();
    }
    unlock();
    s_items_dirty = 1;
    if (rc != 0) {
        setmsg("There is no such repository.");
        return -1;
    }
    setmsg("Removed.");
    return 0;
}

/* ------------------------------------------------------------------ refresh: the signed index */

static int refresh_one(purr_repo_entry_t *e, char *note, size_t notecap)
{
    uint8_t *buf = big_alloc(PURR_REPO_MAX_INDEX + 256);
    if (buf == NULL) {
        snprintf(note, notecap, "out of memory");
        return -1;
    }
    char url[176];
    uint8_t sigtxt[200], sig[PURR_SIG_LEN];
    size_t got = 0, sgot = 0;
    int rc = -1;
    /* the release file first: if the repository has changed its key, it is not the repository that was trusted */
    {
        uint8_t rj[4200];
        size_t rgot = 0, rsgot = 0;
        char u2[176];
        if (file_url(e, "repo.json", u2, sizeof(u2)) == 0 && fetch_to(u2, rj, sizeof(rj) - 1, &rgot) == 0) {
            snprintf(u2 + strlen(u2), sizeof(u2) - strlen(u2), ".sig");
            purr_repo_info_t info;
            char err[96];
            if (fetch_to(u2, sigtxt, sizeof(sigtxt), &rsgot) == 0 && purr_repo_sig_decode((const char *)sigtxt, rsgot, sig) == 0 &&
                purr_repo_info_parse((const char *)rj, rgot, &info, err, sizeof(err)) == 0 &&
                purr_repo_info_check(&purr_crypto_mbedtls, &info, rj, rgot, sig, err, sizeof(err)) == 0 && strcmp(info.id, e->id) != 0) {
                e->tier = PURR_TIER_UNTRUSTED;
                lock();
                repos_save();
                unlock();
                snprintf(note, notecap, "%s changed its key. It is untrusted until an administrator adds it again.", e->name);
                free(buf);
                return -1;
            }
        }
    }
    if (file_url(e, "index.json", url, sizeof(url)) != 0 || fetch_to(url, buf, PURR_REPO_MAX_INDEX + 64, &got) != 0) {
        snprintf(note, notecap, "could not fetch the index of %s", e->name);
        goto out;
    }
    snprintf(url + strlen(url), sizeof(url) - strlen(url), ".sig");
    if (fetch_to(url, sigtxt, sizeof(sigtxt), &sgot) != 0 || purr_repo_sig_decode((const char *)sigtxt, sgot, sig) != 0) {
        snprintf(note, notecap, "%s has no signature on its index", e->name);
        goto out;
    }
    if (purr_repo_check_signed(&purr_crypto_mbedtls, e->pub, buf, got, sig) != 0) {
        snprintf(note, notecap, "the index of %s is not signed by its key: I ignored it", e->name);
        goto out;
    }
    purr_repo_index_t *idx = big_alloc(sizeof(*idx));
    if (idx == NULL) {
        snprintf(note, notecap, "out of memory");
        goto out;
    }
    purr_repo_kind_t kind = PURR_REPO_STORE;
    char err[96];
    if (purr_repo_index_parse((const char *)buf, got, idx, &kind, err, sizeof(err)) != 0 || strcmp(idx->id, e->id) != 0) {
        snprintf(note, notecap, "the index of %s is not usable (%s)", e->name, err[0] ? err : "it names another repository");
        free(idx);
        goto out;
    }
    int n = idx->count;
    free(idx);
    char dir[48], path[80];
    snprintf(dir, sizeof(dir), STORE_DIR "/%s", e->id);
    purr_fs_mkdir(fs(), "/var");
    purr_fs_mkdir(fs(), STORE_DIR);
    purr_fs_mkdir(fs(), dir);
    snprintf(path, sizeof(path), "%s/index.json", dir);
    purr_fs_write(fs(), path, buf, (uint32_t)got);
    snprintf(path, sizeof(path), "%s/index.json.sig", dir);
    purr_fs_write(fs(), path, sigtxt, (uint32_t)sgot);
    snprintf(note, notecap, "%s: %d item(s)", e->name, n);
    rc = 0;
out:
    free(buf);
    return rc;
}

static int refresh(const char *only_id)
{
    repos_load();
    if (s_repos.count == 0) {
        setmsg("No repository is trusted yet. Add one first.");
        return -1;
    }
    if (!net_up()) {
        setmsg("Connect to the network first.");
        return -1;
    }
    int ok = 0, bad = 0;
    char note[120] = "", all[200] = "";
    for (int i = 0; i < s_repos.count; i++) {
        purr_repo_entry_t *e = &s_repos.e[i];
        if (only_id != NULL && only_id[0] != '\0' && strcmp(only_id, e->id) != 0) {
            continue;
        }
        if (e->tier == PURR_TIER_UNTRUSTED && !purr_security_dev_active()) {
            continue;
        }
        if (refresh_one(e, note, sizeof(note)) == 0) {
            ok++;
        } else {
            bad++;
        }
        snprintf(all + strlen(all), sizeof(all) - strlen(all), "%s%s", all[0] ? "; " : "", note);
    }
    s_items_dirty = 1;
    snprintf(s_job.msg, sizeof(s_job.msg), "%s", all[0] ? all : "Nothing to refresh.");
    return bad && !ok ? -1 : 0;
}

/* ------------------------------------------------------------------ the item list */

static void assets_root(char *out, size_t cap)
{
    snprintf(out, cap, "/home/%s/assets", user_name());
}

static int sd_present_now(void)
{
    return purr_sd_present();
}

static void asset_version(const purr_repo_app_t *a, char *out, size_t cap)
{
    char path[160];
    uint8_t v[24];
    out[0] = '\0';
    const char *roots[2] = {NULL, "/sd/purr"};
    char ar[64];
    assets_root(ar, sizeof(ar));
    roots[0] = ar;
    for (int r = 0; r < 2; r++) {
        snprintf(path, sizeof(path), "%s/%s/.version", roots[r], a->name);
        if (read_file(path, v, sizeof(v), NULL) > 0) {
            snprintf(out, cap, "%s", (const char *)v);
            return;
        }
    }
}

static void rebuild_items(void)
{
    lock();
    if (s_items != NULL && (!s_items_dirty || (s_job.busy && xTaskGetCurrentTaskHandle() != s_job_task))) {   /* a job is using the shared scratch buffer: show what we had */
        unlock();
        return;
    }
    if (s_items == NULL) {
        s_items = big_alloc(sizeof(purr_store_item_t) * MAX_ITEMS);
    }
    s_nitems = s_nupdates = 0;
    if (s_items == NULL) {
        unlock();
        return;
    }
    repos_load();
    static purr_app_registry_t reg;
    memset(&reg, 0, sizeof(reg));
    if (user_name()[0] != '\0') {
        purr_apps_scan_current_user(&reg);
    }
    purr_repo_index_t *idx = big_alloc(sizeof(*idx));
    uint8_t *buf = big_alloc(PURR_REPO_MAX_INDEX + 8);
    for (int r = 0; idx != NULL && buf != NULL && r < s_repos.count; r++) {
        const purr_repo_entry_t *e = &s_repos.e[r];
        if (e->tier == PURR_TIER_UNTRUSTED && !purr_security_dev_active()) {
            continue;
        }
        char path[80];
        uint32_t n = 0;
        snprintf(path, sizeof(path), STORE_DIR "/%s/index.json", e->id);
        purr_repo_kind_t kind = PURR_REPO_STORE;
        char err[64];
        if (read_file(path, buf, PURR_REPO_MAX_INDEX + 8, &n) < 0 || purr_repo_index_parse((const char *)buf, n, idx, &kind, err, sizeof(err)) != 0) {
            continue;
        }
        for (int i = 0; i < idx->count && s_nitems < MAX_ITEMS; i++) {
            const purr_repo_app_t *a = &idx->apps[i];
            purr_store_item_t *it = &s_items[s_nitems++];
            memset(it, 0, sizeof(*it));
            snprintf(it->name, sizeof(it->name), "%s", a->name);
            snprintf(it->title, sizeof(it->title), "%s", a->title);
            snprintf(it->version, sizeof(it->version), "%s", a->version);
            snprintf(it->desc, sizeof(it->desc), "%s", a->desc);
            snprintf(it->repo, sizeof(it->repo), "%s", e->name);
            snprintf(it->repo_id, sizeof(it->repo_id), "%s", e->id);
            snprintf(it->tier, sizeof(it->tier), "%s", purr_repo_tier_name((purr_repo_tier_t)e->tier));
            snprintf(it->perms, sizeof(it->perms), "%s", a->perms);
            snprintf(it->category, sizeof(it->category), "%s", a->category);
            it->size = a->size;
            it->type = a->type;
            it->target = a->target;
            it->needs_sd = a->needs_sd;
            if (a->type == PURR_REPO_ITEM_APP) {
                const purr_app_entry_t *have = purr_appmgr_find(&reg, a->name);
                if (have != NULL) {
                    snprintf(it->installed, sizeof(it->installed), "%s", have->version);
                }
            } else {
                asset_version(a, it->installed, sizeof(it->installed));
            }
            if (it->installed[0]) {
                it->state = purr_repo_version_cmp(a->version, it->installed) > 0 ? PURR_STORE_UPDATE : PURR_STORE_INSTALLED;
                s_nupdates += it->state == PURR_STORE_UPDATE;
            }
        }
    }
    free(idx);
    free(buf);
    s_items_dirty = 0;
    unlock();
}

/* ------------------------------------------------------------------ install and remove */

typedef struct {
    char name[24][40];
    uint8_t dir[24];
    int n;
} dirlist_t;

static void dl_cb(void *ctx, const char *name, int is_dir, uint32_t size)
{
    dirlist_t *d = ctx;
    (void)size;
    if (d->n < 24) {
        snprintf(d->name[d->n], sizeof(d->name[0]), "%s", name);
        d->dir[d->n] = (uint8_t)(is_dir != 0);
        d->n++;
    }
}

/* Removes everything under path (not path itself). */
static void rm_tree(const char *path, int depth)
{
    if (depth > 6) {
        return;
    }
    dirlist_t *d = malloc(sizeof(*d));
    if (d == NULL) {
        return;
    }
    for (int pass = 0; pass < 8; pass++) {                 /* a folder of more than 24 entries is emptied in several passes */
        d->n = 0;
        x_list(path, dl_cb, d);
        if (d->n == 0) {
            break;
        }
        for (int i = 0; i < d->n; i++) {
            char sub[200];
            snprintf(sub, sizeof(sub), "%s/%s", path, d->name[i]);
            if (d->dir[i]) {
                rm_tree(sub, depth + 1);
            }
            x_remove(sub);
        }
    }
    free(d);
}

/* The destination folder of an asset or extra for this device, or "" with the reason in why. */
static int asset_dest(const purr_repo_app_t *a, char *out, size_t cap, char *why, size_t whycap)
{
    int sd = sd_present_now();
    int use_sd = a->target == PURR_REPO_TARGET_SD || (a->target == PURR_REPO_TARGET_ANY && sd && a->needs_sd);
    if (a->needs_sd && !sd) {
        snprintf(why, whycap, "%s needs an SD card, and there is none.", a->title);
        return -1;
    }
    if (a->target == PURR_REPO_TARGET_SD && !sd) {
        snprintf(why, whycap, "%s is for an SD card, and there is none.", a->title);
        return -1;
    }
    if (use_sd) {
        snprintf(out, cap, "/sd/purr/%s", a->name);
    } else {
        char ar[64];
        assets_root(ar, sizeof(ar));
        snprintf(out, cap, "%s/%s", ar, a->name);
    }
    return 0;
}

static int install_item(const char *name, const char *repo_id)
{
    repos_load();
    const purr_repo_entry_t *e = purr_repos_find(&s_repos, repo_id);
    if (e == NULL) {
        setmsg("That repository is not trusted.");
        return -1;
    }
    if (e->tier == PURR_TIER_UNTRUSTED && !purr_security_dev_active()) {
        setmsg("%s is untrusted. Developer mode has to be on to use it.", e->name);
        return -1;
    }
    if (!net_up()) {
        setmsg("Connect to the network first.");
        return -1;
    }
    if (user_name()[0] == '\0') {
        setmsg("Sign in first.");
        return -1;
    }
    uint8_t *ibuf = big_alloc(PURR_REPO_MAX_INDEX + 8);
    purr_repo_index_t *idx = big_alloc(sizeof(*idx));
    uint8_t *pkg = NULL;
    int rc = -1;
    if (ibuf == NULL || idx == NULL) {
        setmsg("Out of memory.");
        goto done;
    }
    char path[96];
    uint8_t sigtxt[200], sig[PURR_SIG_LEN];
    uint32_t n = 0, sn = 0;
    snprintf(path, sizeof(path), STORE_DIR "/%s/index.json", e->id);
    if (read_file(path, ibuf, PURR_REPO_MAX_INDEX + 8, &n) < 0) {
        setmsg("Refresh %s first.", e->name);
        goto done;
    }
    snprintf(path, sizeof(path), STORE_DIR "/%s/index.json.sig", e->id);
    if (read_file(path, sigtxt, sizeof(sigtxt), &sn) < 0 || purr_repo_sig_decode((const char *)sigtxt, sn, sig) != 0 ||
        purr_repo_check_signed(&purr_crypto_mbedtls, e->pub, ibuf, n, sig) != 0) {
        setmsg("The saved index of %s does not check out. Refresh it.", e->name);
        goto done;
    }
    purr_repo_kind_t kind = PURR_REPO_STORE;
    char err[64];
    if (purr_repo_index_parse((const char *)ibuf, n, idx, &kind, err, sizeof(err)) != 0) {
        setmsg("The index of %s is not usable.", e->name);
        goto done;
    }
    const purr_repo_app_t *a = purr_repo_index_find(idx, name);
    if (a == NULL) {
        setmsg("%s does not offer %s.", e->name, name);
        goto done;
    }
    char url[200];
    if (file_url(e, a->url, url, sizeof(url)) != 0) {
        setmsg("The address of %s is not valid.", name);
        goto done;
    }
    char dest[120] = "";
    if (a->type != PURR_REPO_ITEM_APP) {
        char why[100];
        if (asset_dest(a, dest, sizeof(dest), why, sizeof(why)) != 0) {
            setmsg("%s", why);
            goto done;
        }
        if (strncmp(dest, "/sd/", 4) != 0) {                   /* flash: leave room (never past 90 % full) */
            uint32_t used = 0, total = 0;
            if (purr_fs_usage(fs(), &used, &total) == 0 && total > 0 && ((uint64_t)used * 4096 + a->size) * 100 > (uint64_t)total * 4096 * 90) {
                setmsg("There is not enough room for %s (it needs %u KB).", a->title, (unsigned)((a->size + 1023) / 1024));
                goto done;
            }
        }
    }
    pkg = big_alloc((size_t)a->size + 64);
    if (pkg == NULL) {
        setmsg("Out of memory for the download.");
        goto done;
    }
    size_t got = 0;
    if (fetch_to(url, pkg, (size_t)a->size + 64, &got) != 0) {
        setmsg("Could not download %s.", name);
        goto done;
    }
    purr_repo_pkg_result_t chk = purr_repo_check_package(a, pkg, got);
    if (chk != PURR_REPO_PKG_OK) {
        setmsg("The download of %s does not match the signed index (%s). Nothing was installed.", name, purr_repo_pkg_result_name(chk));
        goto done;
    }
    /* the receipt's copy of the index the package came from */
    uint8_t ih[32];
    char i8[IDX8 + 1], full[65];
    purr_sha256(ibuf, n, ih);
    hex_of(ih, IDX8 / 2, i8);
    char snap[96];
    snprintf(snap, sizeof(snap), STORE_DIR "/%s/idx-%s.json", e->id, i8);
    purr_fs_write(fs(), snap, ibuf, n);
    snprintf(snap, sizeof(snap), STORE_DIR "/%s/idx-%s.json.sig", e->id, i8);
    purr_fs_write(fs(), snap, sigtxt, sn);
    hex_of(a->sha256, 32, full);
    if (a->type == PURR_REPO_ITEM_APP) {
        if (purr_apps_install_vouched(pkg, got, name, e->id, i8, full, s_job.msg, sizeof(s_job.msg)) != 0) {
            goto done;
        }
        setmsg("Installed %s %s.", a->title, a->version);
    } else {
        char file[200];
        snprintf(file, sizeof(file), "%s/%s", dest, a->dest);
        mkdirs(dest);
        char folder[200];
        snprintf(folder, sizeof(folder), "%s", file);
        char *sl = strrchr(folder, '/');
        if (sl != NULL) {
            *sl = '\0';
            mkdirs(folder);
        }
        if (x_write(file, pkg, (uint32_t)got) < 0) {
            setmsg("Could not write %s.", a->dest);
            goto done;
        }
        char vfile[200];
        snprintf(vfile, sizeof(vfile), "%s/.version", dest);
        x_write(vfile, a->version, (uint32_t)strlen(a->version));
        setmsg("Saved %s %s.", a->title, a->version);
    }
    rc = 0;
done:
    free(pkg);
    free(ibuf);
    free(idx);
    s_items_dirty = 1;
    return rc;
}

static int remove_item(const char *name, const char *repo_id)
{
    rebuild_items();
    for (int i = 0; i < s_nitems; i++) {
        if (strcmp(s_items[i].name, name) == 0 && strcmp(s_items[i].repo_id, repo_id) == 0) {
            if (s_items[i].type == PURR_REPO_ITEM_APP) {
                if (purr_apps_remove(name) != 0) {
                    setmsg("%s is not installed.", name);
                    return -1;
                }
            } else {
                char ar[64], dir[120];
                assets_root(ar, sizeof(ar));
                snprintf(dir, sizeof(dir), "%s/%s", ar, name);
                rm_tree(dir, 0);
                purr_fs_remove(fs(), dir);
                snprintf(dir, sizeof(dir), "/sd/purr/%s", name);
                if (sd_present_now()) {
                    rm_tree(dir, 0);
                    x_remove(dir);
                }
            }
            s_items_dirty = 1;
            setmsg("Removed %s.", name);
            return 0;
        }
    }
    setmsg("I do not know %s.", name);
    return -1;
}

static int update_all(void)
{
    rebuild_items();
    int done = 0, failed = 0;
    char last[200] = "";
    purr_store_item_t *snapshot = big_alloc(sizeof(purr_store_item_t) * MAX_ITEMS);
    if (snapshot == NULL) {
        setmsg("Out of memory.");
        return -1;
    }
    int n = s_nitems;
    memcpy(snapshot, s_items, sizeof(purr_store_item_t) * (size_t)n);
    for (int i = 0; i < n; i++) {
        if (snapshot[i].state != PURR_STORE_UPDATE) {
            continue;
        }
        if (install_item(snapshot[i].name, snapshot[i].repo_id) == 0) {
            done++;
        } else {
            failed++;
            snprintf(last, sizeof(last), "%s", s_job.msg);
        }
    }
    free(snapshot);
    if (done + failed == 0) {
        setmsg("Everything is up to date.");
        return 0;
    }
    setmsg("Updated %d item(s)%s%.120s", done, failed ? ". Not updated: " : ".", failed ? last : "");
    return failed && !done ? -1 : 0;
}

static int check_updates(void)
{
    if (refresh("") != 0 && s_repos.count == 0) {
        return -1;
    }
    rebuild_items();
    if (s_nupdates > 0) {
        char line[96];
        snprintf(line, sizeof(line), "%d update%s ready in the Store", s_nupdates, s_nupdates == 1 ? "" : "s");
        w10m_note_post("Store", line);
        setmsg("%s", line);
    } else {
        setmsg("Everything is up to date.");
    }
    return 0;
}

int purr_store_bootstrap(char *msg, size_t cap)
{
    repos_load();
    if (purr_repos_find(&s_repos, "") == NULL && s_repos.count == 0) {
        if (repo_add_fetch(PURR_REPO_OFFICIAL_URL) != 0) {
            snprintf(msg, cap, "%s", s_job.msg);
            return -1;
        }
        if (repo_confirm() != 0) {
            snprintf(msg, cap, "%s", s_job.msg);
            return -1;
        }
    }
    if (refresh("") != 0) {
        snprintf(msg, cap, "%s", s_job.msg);
        return -1;
    }
    rebuild_items();
    int ok = 0;
    for (int i = 0; i < s_nitems; i++) {
        if ((strcmp(s_items[i].name, "store") == 0 || strcmp(s_items[i].name, "storesvc") == 0) && s_items[i].type == PURR_REPO_ITEM_APP &&
            s_items[i].state == PURR_STORE_NOT_INSTALLED) {
            char nm[32], rid[20];
            snprintf(nm, sizeof(nm), "%s", s_items[i].name);
            snprintf(rid, sizeof(rid), "%s", s_items[i].repo_id);
            if (install_item(nm, rid) == 0) {
                ok++;
            }
        }
    }
    snprintf(msg, cap, ok ? "The Store is installed." : "The Store is already installed, or the repository does not have it yet.");
    return 0;
}

/* ------------------------------------------------------------------ jobs */

static int companion_listed(void);
static int companion_set(int on);

static int run_job(void)
{
    const char *w = s_job.what;
    if (strcmp(w, "repo-add") == 0) {
        return repo_add_fetch(s_job.a) == 0 ? 2 : -1;
    }
    if (strcmp(w, "repo-confirm") == 0) {
        return repo_confirm();
    }
    if (strcmp(w, "companion") == 0) {                      /* the Store's background companion, on or off (a Store setting) */
        if (!is_admin_now()) {
            setmsg("Only an administrator can change this.");
            return -1;
        }
        int on = strcmp(s_job.a, "on") == 0;
        if (companion_set(on) != 0) {
            setmsg("Could not change it.");
            return -1;
        }
        setmsg(on ? "Background updates are on." : "Background updates are off.");
        return 0;
    }
    if (strcmp(w, "repo-remove") == 0) {
        return repo_remove(s_job.a);
    }
    if (strcmp(w, "refresh") == 0) {
        return refresh(s_job.a);
    }
    if (strcmp(w, "install") == 0) {
        return install_item(s_job.a, s_job.b);
    }
    if (strcmp(w, "update-all") == 0) {
        return update_all();
    }
    if (strcmp(w, "remove") == 0) {
        return remove_item(s_job.a, s_job.b);
    }
    if (strcmp(w, "check") == 0) {
        return check_updates();
    }
    if (strcmp(w, "bootstrap") == 0) {
        return purr_store_bootstrap(s_job.msg, sizeof(s_job.msg));
    }
    setmsg("I do not know that job.");
    return -1;
}

static void job_task(void *arg)
{
    (void)arg;
    s_job_task = xTaskGetCurrentTaskHandle();
    s_job.msg[0] = '\0';
    uint32_t fast0 = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_job.rc = run_job();
    uint32_t fast1 = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "job \"%s\": fast RAM %u -> %u bytes (biggest free block %u, lowest ever %u)", s_job.what, (unsigned)fast0, (unsigned)fast1,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));   /* what a job leaves behind: this task's own stack is still counted as used */
    snprintf(s_last, sizeof(s_last), "%s", s_job.msg);
    int slot = s_job.slot;
    s_job.busy = 0;
    purr_catrt_post_ui(slot, s_job.rc < 0 ? 1 : s_job.rc == 2 ? 2 : 0, "job");
    vTaskDelete(NULL);
}

static int gate(int admin_only)
{
    if (admin_only && !is_admin_now()) {
        return PURR_STORE_DENIED;
    }
    return purr_catrt_perm_gate(PURR_PERM_INSTALL_APPS);
}

static int cc_start(const char *what, const char *a, const char *b)
{
    if (what == NULL) {
        return PURR_STORE_BAD;
    }
    int admin = strncmp(what, "repo-", 5) == 0 || strcmp(what, "bootstrap") == 0;
    int g = gate(admin);
    if (g != 0) {
        return g;
    }
    if (s_job.busy) {
        return PURR_STORE_BUSY;
    }
    int slot = purr_catrt_caller(NULL, 0);
    if (slot < 0) {
        return PURR_STORE_BAD;
    }
    s_job.busy = 1;
    s_job.slot = slot;
    snprintf(s_job.what, sizeof(s_job.what), "%s", what);
    snprintf(s_job.a, sizeof(s_job.a), "%s", a != NULL ? a : "");
    snprintf(s_job.b, sizeof(s_job.b), "%s", b != NULL ? b : "");
    if (xTaskCreate(job_task, "store-job", 14336, NULL, 3, NULL) != pdPASS) {
        s_job.busy = 0;
        return PURR_STORE_BAD;
    }
    return PURR_STORE_OK;
}

static int cc_result(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return PURR_STORE_BAD;
    }
    snprintf(out, cap, "%s", s_last);
    return (int)strlen(out);
}

static int cc_pending(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return PURR_STORE_BAD;
    }
    if (!s_pending.valid) {
        out[0] = '\0';
        return 0;
    }
    char fp[24];
    snprintf(fp, sizeof(fp), "%.4s %.4s %.4s %.4s", s_pending.info.id, s_pending.info.id + 4, s_pending.info.id + 8, s_pending.info.id + 12);
    const char *how = s_pending.tier == PURR_TIER_OFFICIAL ? "Official: the project vouches for it." :
                      s_pending.tier == PURR_TIER_COMMUNITY ? "Community: made by someone else. Trust it only if you know who runs it." :
                      "Untrusted: its endorsement does not check out.";
    snprintf(out, cap, "%s\n%s\nBy %s\nFingerprint %s\n%s%s", s_pending.info.name, s_pending.url, s_pending.info.maintainer[0] ? s_pending.info.maintainer : "(not said)", fp, how,
             s_pending.cls == PURR_RA_REKEYED ? "\nIt has CHANGED ITS KEY since you added it." : "");
    return (int)strlen(out);
}

static int cc_repo(int i, char *name, size_t ncap, char *info, size_t icap, char *id, size_t idcap)
{
    int g = gate(0);
    if (g != 0) {
        return g;
    }
    repos_load();
    if (i < 0 || i >= s_repos.count) {
        return 0;
    }
    const purr_repo_entry_t *e = &s_repos.e[i];
    snprintf(name, ncap, "%s", e->name);
    snprintf(info, icap, "%s  %.4s %.4s %.4s %.4s", purr_repo_tier_name((purr_repo_tier_t)e->tier), e->id, e->id + 4, e->id + 8, e->id + 12);
    snprintf(id, idcap, "%s", e->id);
    return 1;
}

static int cc_item_count(void)
{
    int g = gate(0);
    if (g != 0) {
        return g;
    }
    rebuild_items();
    return s_nitems;
}

static int cc_item(int i, purr_store_item_t *out)
{
    int g = gate(0);
    if (g != 0) {
        return g;
    }
    rebuild_items();
    if (i < 0 || i >= s_nitems || out == NULL) {
        return PURR_STORE_BAD;
    }
    memcpy(out, &s_items[i], sizeof(*out));
    return 0;
}

static int cc_updates(void)
{
    int g = gate(0);
    if (g != 0) {
        return g;
    }
    rebuild_items();
    return s_nupdates;
}

static int cc_sd_present(void)
{
    return sd_present_now();
}

static int cc_is_admin(void)
{
    return is_admin_now();
}

static int cc_setting(const char *key, int fallback)
{
    if (key == NULL || strncmp(key, "store.", 6) != 0) {
        return fallback;
    }
    if (strcmp(key, "store.companion") == 0) {              /* the truth is whether it is listed to start (an exec-once line), not a second copy of it */
        return companion_listed();
    }
    return purr_settings_kv_int(key, fallback);
}

static const purr_cc_store_t s_cc_store = {
    .version = 1, .is_admin = cc_is_admin, .start = cc_start, .result = cc_result, .pending = cc_pending, .repo = cc_repo,
    .item_count = cc_item_count, .item = cc_item, .updates = cc_updates, .sd_present = cc_sd_present, .setting = cc_setting,
};

/* ---- settings: store.auto_update and store.check_hours (what the companion follows) */

static int auto_get(char *o, size_t c)
{
    snprintf(o, c, "%s", purr_settings_kv_int("store.auto_update", 0) ? "on" : "off");
    return 0;
}

static int auto_set(const char *v, char *e, size_t c)
{
    int on;
    if (purr_settings_parse_bool(v, &on) != 0) {
        snprintf(e, c, "expected on|off");
        return -1;
    }
    return purr_settings_kv_set("store.auto_update", on ? "1" : "0") == 0 ? 0 : (snprintf(e, c, "could not save"), -1);
}

static int hours_get(char *o, size_t c)
{
    snprintf(o, c, "%d", purr_settings_kv_int("store.check_hours", 12));
    return 0;
}

static int hours_set(const char *v, char *e, size_t c)
{
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < 1 || n > 168) {
        snprintf(e, c, "expected 1-168 hours");
        return -1;
    }
    return purr_settings_kv_set("store.check_hours", v) == 0 ? 0 : (snprintf(e, c, "could not save"), -1);
}

static int companion_get(char *o, size_t c)
{
    snprintf(o, c, "%s", companion_listed() ? "on" : "off");
    return 0;
}

static int companion_setting_set(const char *v, char *e, size_t c)
{
    int on;
    if (purr_settings_parse_bool(v, &on) != 0) {
        snprintf(e, c, "expected on|off");
        return -1;
    }
    return companion_set(on) == 0 ? 0 : (snprintf(e, c, "could not change it"), -1);
}

static const purr_setting_t s_store_keys[] = {
    {"store.companion", "run the Store's background companion (it looks for app updates); off by default, it holds about 10 KB of fast RAM", "on|off", 1, companion_get, companion_setting_set},
    {"store.auto_update", "install app updates by themselves when the Store's check finds them", "on|off", 1, auto_get, auto_set},
    {"store.check_hours", "how often the Store's companion looks for updates, in hours", "1-168", 1, hours_get, hours_set},
};

void purr_catrt_store_init(void)
{
    purr_catrt_set_store(&s_cc_store);
    for (size_t i = 0; i < sizeof(s_store_keys) / sizeof(s_store_keys[0]); i++) {
        purr_settings_register(&s_store_keys[i]);
    }
}

/* ------------------------------------------------------------------ the loader's questions */

static int parse_receipt(const char *folder, char repo[20], char idx8[IDX8 + 1], uint8_t sha[32])
{
    char path[140];
    uint8_t buf[200];
    snprintf(path, sizeof(path), "%s/receipt", folder);
    if (read_file(path, buf, sizeof(buf), NULL) <= 0) {
        return -1;
    }
    char shahex[72] = "";
    repo[0] = idx8[0] = '\0';
    for (char *line = (char *)buf; line != NULL && *line;) {
        char *nl = strchr(line, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }
        if (strncmp(line, "repo=", 5) == 0) snprintf(repo, 20, "%s", line + 5);
        else if (strncmp(line, "idx=", 4) == 0) snprintf(idx8, IDX8 + 1, "%s", line + 4);
        else if (strncmp(line, "sha=", 4) == 0) snprintf(shahex, sizeof(shahex), "%s", line + 4);
        line = nl != NULL ? nl + 1 : NULL;
    }
    return repo[0] && idx8[0] && purr_repo_hex_to_sha256(shahex, sha) == 0 ? 0 : -1;
}

/* Does the saved, signed index a receipt points at list this app with this hash? */
static int receipt_vouches(const char *name, const char *repo, const char *idx8, const uint8_t sha[32])
{
    repos_load();
    const purr_repo_entry_t *e = purr_repos_find(&s_repos, repo);
    if (e == NULL || (e->tier == PURR_TIER_UNTRUSTED && !purr_security_dev_active())) {
        return 0;
    }
    char path[96];
    uint8_t sigtxt[200], sig[PURR_SIG_LEN];
    uint32_t n = 0, sn = 0;
    uint8_t *ibuf = big_alloc(PURR_REPO_MAX_INDEX + 8);
    purr_repo_index_t *idx = big_alloc(sizeof(*idx));
    int ok = 0;
    if (ibuf != NULL && idx != NULL) {
        snprintf(path, sizeof(path), STORE_DIR "/%s/idx-%s.json", repo, idx8);
        if (read_file(path, ibuf, PURR_REPO_MAX_INDEX + 8, &n) >= 0) {
            snprintf(path, sizeof(path), STORE_DIR "/%s/idx-%s.json.sig", repo, idx8);
            purr_repo_kind_t kind = PURR_REPO_STORE;
            char err[40];
            if (read_file(path, sigtxt, sizeof(sigtxt), &sn) >= 0 && purr_repo_sig_decode((const char *)sigtxt, sn, sig) == 0 &&
                purr_repo_check_signed(&purr_crypto_mbedtls, e->pub, ibuf, n, sig) == 0 &&
                purr_repo_index_parse((const char *)ibuf, n, idx, &kind, err, sizeof(err)) == 0) {
                ok = purr_repo_vouches(idx, name, sha);
            }
        }
    }
    free(ibuf);
    free(idx);
    return ok;
}

int purr_store_vouches(const char *path, const uint8_t *data, size_t len)
{
    const char *slash = strrchr(path, '/');
    if (slash == NULL || strstr(path, "/apps/") == NULL || strstr(path, "..") != NULL) {
        return 0;
    }
    char folder[128];
    snprintf(folder, sizeof(folder), "%.*s", (int)(slash - path), path);
    const char *name = strrchr(folder, '/');
    if (name == NULL) {
        return 0;
    }
    name++;
    char repo[20], idx8[IDX8 + 1];
    uint8_t sha[32], mine[32];
    if (parse_receipt(folder, repo, idx8, sha) != 0) {
        return 0;
    }
    purr_sha256(data, len, mine);
    if (memcmp(mine, sha, 32) != 0) {
        return 0;                                    /* not the file that was installed */
    }
    if (strcmp(repo, "builtin") == 0) {              /* it rides inside the signed firmware: it counts if it is still exactly what the firmware carries */
        for (int i = 0; i < purr_builtin_apps_count; i++) {
            if (strcmp(purr_builtin_apps[i].name, name) == 0 && memcmp(purr_builtin_apps[i].sha256, mine, 32) == 0) {
                return 1;
            }
        }
        return 0;
    }
    return receipt_vouches(name, repo, idx8, sha);
}

void purr_store_mark_vouched(purr_app_registry_t *reg, const char *apps_root)
{
    static struct {
        char name[32];
        uint8_t sha[8];
        uint8_t ok;
    } cache[8];
    static int ncache;
    for (int i = 0; i < reg->count; i++) {
        purr_app_entry_t *a = &reg->apps[i];
        if (a->verified) {
            continue;
        }
        char folder[128], repo[20], idx8[IDX8 + 1];
        uint8_t sha[32];
        snprintf(folder, sizeof(folder), "%s/%s", apps_root, a->name);
        if (parse_receipt(folder, repo, idx8, sha) != 0) {
            continue;
        }
        int ok = -1;
        for (int k = 0; k < ncache; k++) {
            if (strcmp(cache[k].name, a->name) == 0 && memcmp(cache[k].sha, sha, 8) == 0) {
                ok = cache[k].ok;
            }
        }
        if (ok < 0) {
            ok = 0;
            if (strcmp(repo, "builtin") == 0) {
                for (int b = 0; b < purr_builtin_apps_count; b++) {
                    if (strcmp(purr_builtin_apps[b].name, a->name) == 0 && memcmp(purr_builtin_apps[b].sha256, sha, 32) == 0) {
                        ok = 1;
                    }
                }
            } else {
                ok = receipt_vouches(a->name, repo, idx8, sha);
            }
            if (ncache < 8) {
                snprintf(cache[ncache].name, sizeof(cache[ncache].name), "%s", a->name);
                memcpy(cache[ncache].sha, sha, 8);
                cache[ncache].ok = (uint8_t)ok;
                ncache++;
            }
        }
        if (ok) {
            a->verified = 1;
            a->signer_role = PURR_ROLE_VENDOR;
        }
    }
}

/* ------------------------------------------------------------------ the built-in Store, seeded at first sign-in */

static void ensure_exec_once(const char *app)
{
    static uint8_t buf[4300];
    uint32_t n = 0;
    if (read_file("/etc/apps.conf", buf, sizeof(buf) - 64, &n) < 0) {
        n = 0;
        buf[0] = '\0';
    }
    if (strstr((const char *)buf, app) != NULL && strstr((const char *)buf, "exec-once") != NULL) {
        return;
    }
    n += (uint32_t)snprintf((char *)buf + n, sizeof(buf) - n, "%sexec-once = %s\n", n > 0 && buf[n - 1] != '\n' ? "\n" : "", app);
    purr_fs_write(fs(), "/etc/apps.conf", buf, n);
    char err[80];
    purr_catrt_reload(err, sizeof(err));
}

/* Whether the companion is listed to start after sign-in (the line ensure_exec_once adds). */
static int companion_listed(void)
{
    static uint8_t buf[4300];
    uint32_t n = 0;
    if (read_file("/etc/apps.conf", buf, sizeof(buf) - 1, &n) < 0) {
        return 0;
    }
    buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
    for (const char *p = (const char *)buf; (p = strstr(p, "exec-once")) != NULL; p++) {
        const char *eol = strchr(p, '\n');
        const char *at = strstr(p, "storesvc");
        if (at != NULL && (eol == NULL || at < eol)) {
            return 1;
        }
    }
    return 0;
}

static void remove_exec_once(const char *app)
{
    static uint8_t in[4300], out[4300];
    uint32_t n = 0;
    if (read_file("/etc/apps.conf", in, sizeof(in) - 1, &n) < 0) {
        return;
    }
    in[n < sizeof(in) ? n : sizeof(in) - 1] = '\0';
    uint32_t o = 0;
    for (const char *p = (const char *)in; *p != '\0';) {
        const char *eol = strchr(p, '\n');
        size_t len = eol != NULL ? (size_t)(eol - p) + 1 : strlen(p);
        int drop = strncmp(p, "exec-once", 9) == 0 && memmem(p, len, app, strlen(app)) != NULL;
        if (!drop && o + len < sizeof(out)) {
            memcpy(out + o, p, len);
            o += (uint32_t)len;
        }
        p += len;
    }
    purr_fs_write(fs(), "/etc/apps.conf", out, o);
    char err[80];
    purr_catrt_reload(err, sizeof(err));
}

/* The companion is an app task of its own (8 KB of fast RAM and its bookkeeping), so it is not started for everyone: the Store switches it on
 * (store.companion). On: it may install apps, it is listed to start after sign-in, and it starts now. Off: it is stopped and unlisted. */
static int companion_set(int on)
{
    char out[120];
    if (on) {
        purr_catrt_perm_put("storesvc", PURR_PERM_INSTALL_APPS, PURR_PERM_ALWAYS);          /* a background app cannot be asked: the Store grants its own companion */
        ensure_exec_once("storesvc");
        purr_catrt_dispatch("exec storesvc", out, sizeof(out));
    } else {
        purr_catrt_dispatch("close storesvc", out, sizeof(out));
        remove_exec_once("storesvc");
    }
    return 0;
}

static void seed_task(void *arg)
{
    (void)arg;
    const char *u = user_name();
    char marker[80], root[80];
    snprintf(marker, sizeof(marker), "/home/%s/.store-seeded", u);
    snprintf(root, sizeof(root), "/home/%s/apps", u);
    int is_dir = 0;
    uint32_t sz = 0;
    if (u[0] != '\0' && purr_fs_stat(fs(), marker, &is_dir, &sz) != 0) {
        char msg[80];
        int ok = 0;
        for (int i = 0; i < purr_builtin_apps_count; i++) {
            const purr_builtin_app_t *b = &purr_builtin_apps[i];
            char folder[120], pkg[140];
            snprintf(folder, sizeof(folder), "%s/%s", root, b->name);
            snprintf(pkg, sizeof(pkg), "%s/package.cat", folder);
            if (purr_fs_stat(fs(), pkg, &is_dir, &sz) == 0) {
                ok++;                                    /* already there (an update from a repository, or put there by hand) */
                continue;
            }
            char full[65];
            hex_of(b->sha256, 32, full);
            if (purr_apps_install_vouched(b->data, b->len, b->name, "builtin", "00000000", full, msg, sizeof(msg)) == 0) {
                ok++;
            } else {
                ESP_LOGW(TAG, "built-in %s: %s", b->name, msg);
            }
        }
        if (ok == purr_builtin_apps_count) {
            /* The companion is installed but NOT started: it is an app task that holds fast RAM, and the owner turns it on in the Store (store.companion). */
            purr_fs_write(fs(), marker, "1\n", 2);
            w10m_apps_changed_post();
            ESP_LOGI(TAG, "the Store and its companion are installed for %s (the companion is off until the Store turns it on)", u);
        }
    }
    vTaskDelete(NULL);
}

void purr_store_session_started(void)
{
    if (purr_builtin_apps_count > 0 && xTaskCreate(seed_task, "store-seed", 12288, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "could not start the Store seeding task");
    }
}

/* ------------------------------------------------------------------ the shell command */

static int cmd_wait_job(purr_cli_t *cli, int *rc)
{
    *rc = run_job();
    purr_cli_printf(cli, "%s\n", s_job.msg);
    return *rc < 0;
}

int purr_store_cmd(purr_cli_t *cli, int argc, char **argv)
{
    const char *sub = argc >= 2 ? argv[1] : "list";
    if (strcmp(sub, "repos") == 0) {
        repos_load();
        for (int i = 0; i < s_repos.count; i++) {
            const purr_repo_entry_t *e = &s_repos.e[i];
            purr_cli_printf(cli, "%s  %s  %s  %s  (%s)\n", e->id, purr_repo_tier_name((purr_repo_tier_t)e->tier), e->name, e->url, e->flat ? "flat" : "tree");
        }
        if (s_repos.count == 0) {
            purr_cli_printf(cli, "no repository is trusted yet: store add <https address>\n");
        }
        return 0;
    }
    if (strcmp(sub, "list") == 0) {
        rebuild_items();
        for (int i = 0; i < s_nitems; i++) {
            const purr_store_item_t *it = &s_items[i];
            purr_cli_printf(cli, "%-16s %-8s %-10s %s%s%s  [%s %s]\n", it->name, it->version, it->tier, it->installed[0] ? "installed " : "", it->installed[0] ? it->installed : "",
                            it->state == PURR_STORE_UPDATE ? "  UPDATE" : "", it->type == 0 ? "app" : it->type == 1 ? "asset" : "extra", it->repo);
        }
        if (s_nitems == 0) {
            purr_cli_printf(cli, "nothing offered yet: store refresh\n");
        }
        return 0;
    }
    int rc;
    memset(&s_job, 0, sizeof(s_job));
    if (strcmp(sub, "add") == 0 && argc >= 3) {
        if (!is_admin_now()) {
            purr_cli_printf(cli, "only an administrator decides which repositories are trusted\n");
            return 1;
        }
        snprintf(s_job.a, sizeof(s_job.a), "%s", argv[2]);
        snprintf(s_job.what, sizeof(s_job.what), "repo-add");
        if (cmd_wait_job(cli, &rc)) {
            return 1;
        }
        char text[300];
        cc_pending(text, sizeof(text));
        purr_cli_printf(cli, "%s\nIf that is right: store confirm\n", text);
        return 0;
    }
    if (strcmp(sub, "confirm") == 0) {
        snprintf(s_job.what, sizeof(s_job.what), "repo-confirm");
        return cmd_wait_job(cli, &rc);
    }
    if (strcmp(sub, "refresh") == 0 || strcmp(sub, "check") == 0 || strcmp(sub, "bootstrap") == 0) {
        snprintf(s_job.what, sizeof(s_job.what), "%s", sub);
        if (argc >= 3) {
            snprintf(s_job.a, sizeof(s_job.a), "%s", argv[2]);
        }
        return cmd_wait_job(cli, &rc);
    }
    if ((strcmp(sub, "install") == 0 || strcmp(sub, "remove") == 0) && argc >= 4) {
        snprintf(s_job.what, sizeof(s_job.what), "%s", sub);
        snprintf(s_job.a, sizeof(s_job.a), "%s", argv[2]);
        snprintf(s_job.b, sizeof(s_job.b), "%s", argv[3]);
        return cmd_wait_job(cli, &rc);
    }
    purr_cli_printf(cli, "usage: store [repos | list | add <https address> | confirm | refresh [id] | check | install <name> <repo-id> | remove <name> <repo-id> | bootstrap]\n");
    return 1;
}
