/*
 * purr_shell.c - forwards the neutral shell entry points (purr_shell.h) to the shell that is running, and chooses it.
 *
 * The Android style shell is compiled into every image: it is the shell that always works (the fallback). Any other shell is a loadable module installed under
 * /ui/shells/<id>/ (plan7.md, "Shells as installable items"; the Windows 10 Mobile shell is the first). /ui/active-shell.conf names the one to use; purr_shell_select()
 * loads it at start-up, before the theme and the pictures are set up (the module says which skin it draws the shared screens in). If it cannot be loaded, or the last
 * starts did not get as far as showing it, the built-in shell runs and says so.
 *
 * The functions both kinds of shell implement for the shared code (Settings, the Developer Portal, the task manager, the permission prompt, the ui catcall windows,
 * the notify log) are defined here as wrappers; the built-in shell's own are renamed `<name>_droid` (purr_shell_names.h).
 */
#include "purr_shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "esp_log.h"
#include "lvgl.h"
#include "purr_android.h"
#include "purr_fs.h"
#include "purr_shell_abi.h"
#include "purr_w10m_theme.h"

#define TAG "shell"
#define ACTIVE_PATH "/ui/active-shell.conf"
#define MODE_PATH   "/ui/mode.conf"
#define TRY_PATH    "/ui/shell-try.conf"
#define FAILED_PATH "/ui/shell-failed.conf"
#define COMPAT_PATH "/ui/shell-compat.conf"             /* the shell that was given up this start because it no longer fits this system (so it is told once, not every start) */
#define MAX_TRIES   2                                   /* starts that may fail to show the shell before the built-in one takes over */

#include "sdkconfig.h"
#if CONFIG_PURR_PROFILE_FULL
const purr_shell_ops_t *purr_shell_module_load(const char *id);          /* commands.c: verify, load, relocate, patch, call the entry point */
/* purr_shell_module_check() is in purr_shell_abi.h */
#else
#define purr_shell_module_load(id) ((const purr_shell_ops_t *)NULL)       /* only the full profile has shells to load (the recovery loader has no screen shell at all) */
#define purr_shell_module_check(id, bf, cap) PURR_SHELL_CHECK_BAD
#endif

static const purr_shell_ops_t *s_mod;                   /* the loaded shell module, NULL while the built-in shell runs */
static int s_compat_notice;                             /* a chosen shell was given up this start because it does not fit: say so once, when the built-in shell is up */
static char s_mod_id[24];

static int id_ok(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n >= sizeof(s_mod_id)) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

/* The first word of a small text file, "" if it is missing or unreadable. */
static void word_of(const char *path, char *out, size_t cap)
{
    char buf[24];
    uint32_t n = 0;
    out[0] = '\0';
    if (purr_fs_slurp(purr_login_fs(), path, buf, sizeof(buf) - 1, &n) != 0) {
        return;
    }
    buf[n < sizeof(buf) - 1 ? n : sizeof(buf) - 1] = '\0';
    size_t len = strcspn(buf, " \t\r\n");
    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(out, buf, len);
    out[len] = '\0';
}

static int put_line(const char *path, const char *v)
{
    char line[40];
    snprintf(line, sizeof(line), "%s\n", v);
    purr_fs_mkdir(purr_login_fs(), "/ui");
    return purr_fs_write(purr_login_fs(), path, line, (uint32_t)strlen(line)) < 0 ? -1 : 0;
}

/* "key=value" out of a small file (a shell's shell.conf). 1 if the key is there and not empty. */
static int conf_key(const char *path, const char *key, char *out, size_t cap)
{
    char buf[400];
    uint32_t n = 0;
    out[0] = '\0';
    if (purr_fs_slurp(purr_login_fs(), path, buf, sizeof(buf) - 1, &n) != 0) {
        return 0;
    }
    buf[n < sizeof(buf) - 1 ? n : sizeof(buf) - 1] = '\0';
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

/* ---------------------------------------------------------------- choosing the shell at start-up */

void purr_shell_select(void)
{
    s_mod = NULL;
    s_mod_id[0] = '\0';
    char id[24], tries_s[8];
    word_of(ACTIVE_PATH, id, sizeof(id));
    if (id[0] == '\0' || strcmp(id, "builtin") == 0 || !id_ok(id)) {
        return;
    }
    int chk = purr_shell_module_check(id, NULL, 0);
    if (chk == PURR_SHELL_CHECK_INCOMPATIBLE) {         /* not a failure: the shell (or the system, after an update) is out of step. The built-in shell runs, the choice stays, so an update of either brings it back */
        ESP_LOGW(TAG, "shell %s does not fit this system; using the built-in shell for now", id);
        char seen[24];
        word_of(COMPAT_PATH, seen, sizeof(seen));
        if (strcmp(seen, id) != 0) {
            put_line(COMPAT_PATH, id);
            s_compat_notice = 1;
        }
        char gp[64];
        snprintf(gp, sizeof(gp), "/ui/shells/%s/good.conf", id);
        purr_fs_remove(purr_login_fs(), gp);            /* no longer known good */
        return;
    }
    word_of(TRY_PATH, tries_s, sizeof(tries_s));
    int tries = atoi(tries_s);
    if (tries >= MAX_TRIES) {                           /* the last starts never got as far as showing the shell: the built-in one takes over, and the choice is undone */
        ESP_LOGW(TAG, "shell %s: %d starts did not get through; using the built-in shell", id, tries);
        put_line(FAILED_PATH, id);
        put_line(ACTIVE_PATH, "builtin");
        put_line(TRY_PATH, "0");
        return;
    }
    char n[16];
    snprintf(n, sizeof(n), "%d", tries + 1);
    put_line(TRY_PATH, n);                              /* cleared again by purr_shell_started() once the shell is on screen */
    const purr_shell_ops_t *ops = purr_shell_module_load(id);
    if (ops == NULL) {
        ESP_LOGW(TAG, "shell %s could not be loaded; using the built-in shell", id);
        put_line(FAILED_PATH, id);
        put_line(ACTIVE_PATH, "builtin");
        put_line(TRY_PATH, "0");
        return;
    }
    s_mod = ops;
    snprintf(s_mod_id, sizeof(s_mod_id), "%s", id);
    purr_theme_set_skin(ops->skin == PURR_SHELL_SKIN_ANDROID ? PURR_SKIN_ANDROID : PURR_SKIN_W10M);
    ESP_LOGI(TAG, "shell %s loaded (ABI %u)", id, (unsigned)ops->abi_version);
}

void purr_shell_started(void)
{
    if (s_mod != NULL) {
        put_line(TRY_PATH, "0");
        char gp[64], v[8] = "1";
        snprintf(gp, sizeof(gp), "/ui/shells/%s/good.conf", s_mod_id);
        put_line(gp, v);                                /* it fits and it came up: the system marks it good */
        purr_fs_remove(purr_login_fs(), COMPAT_PATH);   /* it fits again (after an update): a later mismatch is worth a new notice */
    }
    if (s_compat_notice) {
        char text[160], chosen[24];
        word_of(ACTIVE_PATH, chosen, sizeof(chosen));
        snprintf(text, sizeof(text), "The %s shell needs an update for this system, so the built-in shell is in use. It stays chosen.", chosen);
        purr_shell_notify("Shell", text);
        s_compat_notice = 0;
    }
    char failed[24];
    word_of(FAILED_PATH, failed, sizeof(failed));
    if (failed[0] != '\0') {                             /* tell the user once why the chosen shell is not the one they see */
        char text[96];
        snprintf(text, sizeof(text), "The %s shell did not start, so the built-in shell is in use.", failed);
        purr_shell_notify("Shell", text);
        purr_fs_remove(purr_login_fs(), FAILED_PATH);
    }
}

int purr_shell_is_android(void)
{
    return purr_theme_skin() == PURR_SKIN_ANDROID;
}

/* ---------------------------------------------------------------- the registry (what Settings lists)  */

typedef struct {
    purr_shell_info_t *out;
    int max, n;
} list_ctx_t;

static void list_cb(void *ctx, const char *name, int is_dir, uint32_t size)
{
    (void)size;
    list_ctx_t *l = ctx;
    if (!is_dir || !id_ok(name) || l->n >= l->max) {
        return;
    }
    char path[64], skin[16];
    snprintf(path, sizeof(path), "/ui/shells/%s/shell.cat", name);
    int is_d = 0;
    uint32_t sz = 0;
    if (purr_fs_stat(purr_login_fs(), path, &is_d, &sz) != 0 || is_d) {
        return;                                         /* a folder without a module in it is not an installed shell */
    }
    purr_shell_info_t *s = &l->out[l->n++];
    memset(s, 0, sizeof(*s));
    snprintf(s->id, sizeof(s->id), "%s", name);
    snprintf(path, sizeof(path), "/ui/shells/%s/shell.conf", name);
    if (!conf_key(path, "name", s->name, sizeof(s->name))) {
        snprintf(s->name, sizeof(s->name), "%s", name);
    }
    conf_key(path, "author", s->author, sizeof(s->author));
    conf_key(path, "version", s->version, sizeof(s->version));
    s->skin = conf_key(path, "skin", skin, sizeof(skin)) && strcmp(skin, "android") == 0 ? PURR_SHELL_SKIN_ANDROID : PURR_SHELL_SKIN_W10M;
    int chk = purr_shell_module_check(name, s->built_for, sizeof(s->built_for));
    snprintf(path, sizeof(path), "/ui/shells/%s/good.conf", name);
    s->state = chk == PURR_SHELL_CHECK_INCOMPATIBLE ? PURR_SHELL_STATE_NEEDS_UPDATE : chk == PURR_SHELL_CHECK_BAD ? PURR_SHELL_STATE_BAD :
               purr_fs_stat(purr_login_fs(), path, &is_d, &sz) == 0 ? PURR_SHELL_STATE_GOOD : PURR_SHELL_STATE_UNTESTED;
}

int purr_shell_list(purr_shell_info_t *out, int max)
{
    purr_fs_t *fs = purr_login_fs();
    if (fs == NULL || !purr_fs_mounted(fs) || out == NULL || max <= 0) {
        return 0;
    }
    list_ctx_t l = {out, max, 0};
    purr_fs_list(fs, "/ui/shells", list_cb, &l);
    return l.n;
}

void purr_shell_chosen(char *id, size_t cap)
{
    char v[24];
    word_of(ACTIVE_PATH, v, sizeof(v));
    snprintf(id, cap, "%s", v[0] != '\0' && id_ok(v) ? v : "builtin");
}

int purr_shell_choose(const char *id)
{
    if (strcmp(id, "builtin") != 0) {
        if (!id_ok(id)) {
            return -1;
        }
        purr_shell_info_t all[8];
        int n = purr_shell_list(all, 8), found = 0;
        for (int i = 0; i < n; i++) {
            found |= strcmp(all[i].id, id) == 0;
        }
        if (!found) {
            return -1;
        }
    }
    if (put_line(ACTIVE_PATH, id) != 0 || put_line(MODE_PATH, "w10m") != 0) {           /* "w10m" in mode.conf means: the official (graphical) shell starts */
        return -1;
    }
    put_line(TRY_PATH, "0");                            /* a fresh choice gets a fresh count */
    purr_fs_remove(purr_login_fs(), FAILED_PATH);
    return 0;
}

int purr_shell_mode_get(char *out, size_t cap)
{
    char mode[16];
    word_of(MODE_PATH, mode, sizeof(mode));
    if (strcmp(mode, "nav") == 0 || strcmp(mode, "card_shell") == 0) {
        snprintf(out, cap, "%s", mode);
    } else {
        snprintf(out, cap, "%s", s_mod != NULL ? s_mod_id : "builtin");
    }
    return 0;
}

int purr_shell_mode_set(const char *v, char *err, size_t cap)
{
    if (strcmp(v, "nav") == 0 || strcmp(v, "card_shell") == 0) {
        if (put_line(MODE_PATH, v) != 0) {
            snprintf(err, cap, "could not save");
            return -1;
        }
        return 0;
    }
    if (strcmp(v, "android") == 0) {
        v = "builtin";
    }
    if (purr_shell_choose(v) != 0) {
        snprintf(err, cap, "no such shell is installed (builtin, nav, card_shell or an installed id)");
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- the entry points purr_shell.h forwards */

esp_err_t purr_shell_start(void) { return s_mod != NULL ? s_mod->start() : purr_android_start(); }
esp_err_t purr_shell_set_orientation(int degrees) { return s_mod != NULL ? s_mod->set_orientation(degrees) : purr_android_set_orientation(degrees); }
esp_err_t purr_shell_lock(void) { return s_mod != NULL ? s_mod->lock() : purr_android_lock(); }
esp_err_t purr_shell_notify(const char *title, const char *text) { return s_mod != NULL ? s_mod->notify(title, text) : purr_android_notify(title, text); }
esp_err_t purr_shell_refresh_apps(void) { return s_mod != NULL ? s_mod->refresh_apps() : purr_android_refresh_apps(); }
const char *purr_shell_name(void) { return s_mod != NULL ? s_mod->id : "android"; }

/* ---------------------------------------------------------------- the shared interface (purr_w10m_priv.h, purr_notify.h) */

void w10m_apply_orientation_droid(int degrees);
void w10m_apps_changed_droid(void);
void w10m_apps_changed_post_droid(void);
void w10m_banner_droid(const char *msg, int bad);
void w10m_banner_post_droid(const char *msg, int bad);
void w10m_busy_begin_droid(const char *msg);
void w10m_busy_end_droid(void);
void w10m_dev_badge_update_droid(void);
int w10m_disp_h_droid(void);
int w10m_disp_w_droid(void);
int w10m_get_brightness_droid(void);
const lv_image_dsc_t *w10m_img_droid(const char *name);
int w10m_is_locked_droid(void);
void w10m_note_post_droid(const char *title, const char *text);
void w10m_place_sheet_droid(lv_obj_t *sheet);
void w10m_save_display_droid(void);
void w10m_set_brightness_droid(int level);
void purr_shell_notified_droid(const char *title, const char *text);

void w10m_apply_orientation(int degrees) { s_mod != NULL ? s_mod->apply_orientation(degrees) : w10m_apply_orientation_droid(degrees); }
void w10m_apps_changed(void) { s_mod != NULL ? s_mod->apps_changed() : w10m_apps_changed_droid(); }
void w10m_apps_changed_post(void) { s_mod != NULL ? s_mod->apps_changed_post() : w10m_apps_changed_post_droid(); }
void w10m_banner(const char *msg, int bad) { s_mod != NULL ? s_mod->banner(msg, bad) : w10m_banner_droid(msg, bad); }
void w10m_banner_post(const char *msg, int bad) { s_mod != NULL ? s_mod->banner_post(msg, bad) : w10m_banner_post_droid(msg, bad); }
void w10m_busy_begin(const char *msg) { s_mod != NULL ? s_mod->busy_begin(msg) : w10m_busy_begin_droid(msg); }
void w10m_busy_end(void) { s_mod != NULL ? s_mod->busy_end() : w10m_busy_end_droid(); }
void w10m_dev_badge_update(void) { s_mod != NULL ? s_mod->dev_badge_update() : w10m_dev_badge_update_droid(); }
int w10m_disp_h(void) { return s_mod != NULL ? s_mod->disp_h() : w10m_disp_h_droid(); }
int w10m_disp_w(void) { return s_mod != NULL ? s_mod->disp_w() : w10m_disp_w_droid(); }
int w10m_get_brightness(void) { return s_mod != NULL ? s_mod->get_brightness() : w10m_get_brightness_droid(); }
const lv_image_dsc_t *w10m_img(const char *name) { return s_mod != NULL ? s_mod->img(name) : w10m_img_droid(name); }
int w10m_is_locked(void) { return s_mod != NULL ? s_mod->is_locked() : w10m_is_locked_droid(); }
void w10m_note_post(const char *title, const char *text) { s_mod != NULL ? s_mod->note_post(title, text) : w10m_note_post_droid(title, text); }
void w10m_place_sheet(lv_obj_t *sheet) { s_mod != NULL ? s_mod->place_sheet(sheet) : w10m_place_sheet_droid(sheet); }
void w10m_save_display(void) { s_mod != NULL ? s_mod->save_display() : w10m_save_display_droid(); }
void w10m_set_brightness(int level) { s_mod != NULL ? s_mod->set_brightness(level) : w10m_set_brightness_droid(level); }
void purr_shell_notified(const char *title, const char *text) { s_mod != NULL ? s_mod->shell_notified(title, text) : purr_shell_notified_droid(title, text); }
