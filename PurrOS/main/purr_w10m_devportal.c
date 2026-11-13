/*
 * purr_w10m_devportal.c - the Developer Portal (plan8.md): a separate app on the Home screen, there only
 * while developer mode is active, that works like a small app store for testing. It finds an SDK server
 * on the local network (`purrstrap dev serve` on the developer's computer), lists the apps that server
 * offers, and installs them into the user's devapps folder. It never leaves the local network.
 *
 * It is its own app with its own window, its own install path and its own connection; the Settings app is
 * not involved. What it installs may be signed with the developer key or, with "Allow unsigned apps" on in
 * Settings, unsigned; the rules are the app manager's and the loader's, not this file's.
 *
 * Everything that waits on the network runs on a short worker task, one at a time, and reports back to
 * this window if it is still the page on screen.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "commands.h"
#include "login.h"
#include "purr_appmgr.h"
#include "purr_catrt.h"
#include "purr_lvgl.h"
#include "purr_repo_dev.h"
#include "purr_ui_service.h"
#include "purr_w10m_priv.h"
#include "purr_w10m_theme.h"

#define HEAD_H 30
#define MAX_FOUND 4

typedef enum { PORTAL_MAIN, PORTAL_SERVERS } portal_page_t;

static lv_obj_t *s_sheet, *s_head, *s_head_title, *s_body;
static lv_group_t *s_group;
static portal_page_t s_page = PORTAL_MAIN;
static int s_open;
static int s_gen;
static lv_obj_t *s_status;
static lv_obj_t *s_armed;

static purr_repo_server_t s_server;              /* the one in use */
static int s_have_server;
static EXT_RAM_BSS_ATTR purr_repo_index_t s_idx;                  /* its apps */
static int s_idx_ok;
static purr_app_registry_t s_reg;                /* what is in devapps */
static char s_runq[1600];
static purr_repo_server_t s_found[MAX_FOUND];
static int s_nfound;
static purr_repo_server_t s_pending;             /* a server waiting for the owner to trust it */
static lv_obj_t *s_avail, *s_have, *s_server_row_title, *s_list_servers, *s_addr, *s_trust_panel, *s_trust_text;

static void show_page(portal_page_t p);

/* ---------------------------------------------------------------- small widget helpers */

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, purr_th_style_t style)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_font(l, font, 0);
    purr_theme_apply(l, style);
    return l;
}

static lv_obj_t *text(lv_obj_t *p, const char *t) { return label(p, t, &lv_font_montserrat_14, PURR_TH_TEXT); }
static lv_obj_t *dim(lv_obj_t *p, const char *t) { return label(p, t, &lv_font_montserrat_12, PURR_TH_DIM); }
static lv_obj_t *title(lv_obj_t *p, const char *t) { return label(p, t, &lv_font_montserrat_16, PURR_TH_ACCENT_TEXT); }

static void space(lv_obj_t *p, int h)
{
    lv_obj_t *s = lv_obj_create(p);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, 1, h);
}

static lv_obj_t *button(lv_obj_t *parent, const char *t, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static void button_text(lv_obj_t *b, const char *t) { lv_label_set_text(lv_obj_get_child(b, 0), t); }

static lv_obj_t *input(lv_obj_t *parent, const char *placeholder)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    lv_group_add_obj(s_group, ta);
    return ta;
}

static lv_obj_t *row(lv_obj_t *parent, const char *t, const char *sub, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), 44);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (cb != NULL) {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        purr_theme_apply(r, PURR_TH_ROW_PRESSED);
        lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
    }
    lv_obj_t *a = lv_label_create(r);
    lv_label_set_text(a, t);
    lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
    lv_obj_set_width(a, LV_PCT(55));
    lv_obj_set_style_text_font(a, &lv_font_montserrat_14, 0);
    purr_theme_apply(a, PURR_TH_TEXT);
    lv_obj_align(a, LV_ALIGN_LEFT_MID, 2, sub ? -9 : 0);
    if (sub != NULL) {
        lv_obj_t *d = lv_label_create(r);
        lv_label_set_text(d, sub);
        lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);
        lv_obj_set_width(d, LV_PCT(58));
        lv_obj_set_style_text_font(d, &lv_font_montserrat_12, 0);
        purr_theme_apply(d, PURR_TH_DIM);
        lv_obj_align(d, LV_ALIGN_LEFT_MID, 2, 10);
    }
    return r;
}

static void say(const char *t, int good)
{
    if (s_status == NULL) {
        return;
    }
    lv_label_set_text(s_status, t);
    lv_obj_set_style_text_color(s_status, good ? purr_theme_accent_color() : purr_theme_warn_color(), 0);
}

static void make_status(void)
{
    s_status = lv_label_create(s_body);
    lv_label_set_text(s_status, "");
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, LV_PCT(100));
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_12, 0);
}

static int confirm_tap(lv_obj_t *btn, const char *again)
{
    if (s_armed == btn) {
        s_armed = NULL;
        return 1;
    }
    s_armed = btn;
    button_text(btn, again);
    return 0;
}

/* ---------------------------------------------------------------- jobs (one at a time) */

typedef struct job {
    int gen;
    int rc;
    char msg[112];
    char a[96];
    int flag;
    int (*run)(struct job *);
    void (*done)(struct job *);
} job_t;

static int s_busy;

static void *job_ui_cb(void *arg)
{
    job_t *j = arg;
    s_busy = 0;
    if (j->gen == s_gen && j->done != NULL) {
        j->done(j);
    }
    free(j);
    return NULL;
}

static void job_task(void *arg)
{
    job_t *j = arg;
    j->rc = j->run(j);
    purr_ui_service_run(job_ui_cb, j, NULL);
    vTaskDelete(NULL);
}

static job_t *job_new(int (*run)(job_t *), void (*done)(job_t *))
{
    job_t *j = calloc(1, sizeof(*j));
    if (j != NULL) {
        j->gen = s_gen;
        j->run = run;
        j->done = done;
    }
    return j;
}

static int job_start(job_t *j)
{
    if (j == NULL) {
        return -1;
    }
    if (s_busy) {
        say("Still working on the last request.", 0);
        free(j);
        return -1;
    }
    s_busy = 1;
    if (xTaskCreate(job_task, "dev-job", 7168, j, 3, NULL) != pdPASS) {
        s_busy = 0;
        free(j);
        say("Out of memory.", 0);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- the main page */

static int is_running(const char *name)
{
    size_t n = strlen(name);
    for (const char *p = s_runq; (p = strstr(p, "app ")) != NULL; p += 4) {
        const char *colon = strchr(p, ':');
        if (colon == NULL) {
            break;
        }
        colon++;
        while (*colon == ' ') colon++;
        if (strncmp(colon, name, n) == 0 && (colon[n] == ' ' || colon[n] == '\n' || colon[n] == '\0' || colon[n] == ',')) {
            return 1;
        }
    }
    return 0;
}

static const purr_app_entry_t *installed(const char *name)
{
    for (int i = 0; i < s_reg.count; i++) {
        if (strcmp(s_reg.apps[i].name, name) == 0) {
            return &s_reg.apps[i];
        }
    }
    return NULL;
}

static void refresh(void);
static void install_cb(lv_event_t *e);
static void act_cb(lv_event_t *e);

static void fill_lists(void)
{
    if (s_avail == NULL || s_have == NULL) {
        return;
    }
    lv_obj_clean(s_avail);
    lv_obj_clean(s_have);

    if (!s_have_server) {
        dim(s_avail, "Choose a server first.");
    } else if (!s_idx_ok) {
        dim(s_avail, "Could not read this server. Tap Refresh.");
    } else if (s_idx.count == 0) {
        dim(s_avail, "The server has no apps yet. On your computer: purrstrap dev publish --file app.cat");
    }
    for (int i = 0; s_idx_ok && i < s_idx.count; i++) {
        const purr_repo_app_t *a = &s_idx.apps[i];
        const purr_app_entry_t *have = installed(a->name);
        int cmp = have ? purr_repo_version_cmp(a->version, have->version) : 1;
        char sub[80];
        snprintf(sub, sizeof(sub), "v%s  %u KB  %s", a->version, (unsigned)((a->size + 1023) / 1024),
                 a->signed_claim ? "signed" : "UNSIGNED");
        lv_obj_t *r = row(s_avail, a->title, sub, NULL, NULL);
        if (cmp > 0) {
            lv_obj_t *b = button(r, have ? "Update" : "Install", install_cb, (void *)(intptr_t)i);
            lv_obj_set_height(b, 28);
            lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
        } else {
            lv_obj_t *t = lv_label_create(r);
            lv_label_set_text(t, LV_SYMBOL_OK " installed");
            lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
            purr_theme_apply(t, PURR_TH_DIM);
            lv_obj_align(t, LV_ALIGN_RIGHT_MID, -2, 0);
        }
    }

    if (s_reg.count == 0) {
        dim(s_have, "Nothing installed from the portal yet.");
    }
    for (int i = 0; i < s_reg.count; i++) {
        const purr_app_entry_t *a = &s_reg.apps[i];
        char sub[80];
        snprintf(sub, sizeof(sub), "v%s  %s%s", a->version, a->verified ? "signed" : "UNSIGNED",
                 is_running(a->name) ? "  running" : "");
        lv_obj_t *r = row(s_have, a->name, sub, NULL, NULL);
        lv_obj_t *rm = button(r, "Remove", act_cb, (void *)(intptr_t)(i * 4 + 2));
        lv_obj_set_height(rm, 28);
        lv_obj_align(rm, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_t *go = button(r, is_running(a->name) ? "Stop" : "Open", act_cb,
                              (void *)(intptr_t)(i * 4 + (is_running(a->name) ? 1 : 0)));
        lv_obj_set_height(go, 28);
        lv_obj_align_to(go, rm, LV_ALIGN_OUT_LEFT_MID, -4, 0);
    }
}

static int refresh_run(job_t *j)
{
    char err[96] = "";
    s_idx_ok = 0;
    if (s_have_server) {
        s_idx_ok = purr_repo_dev_index(s_server.base, &s_idx, err, sizeof(err)) == 0;
    }
    memset(&s_reg, 0, sizeof(s_reg));
    purr_devapps_scan(&s_reg);
    s_runq[0] = '\0';
    purr_catrt_query("apps", s_runq, sizeof(s_runq));
    snprintf(j->msg, sizeof(j->msg), "%s", s_idx_ok ? "" : err);
    return s_idx_ok ? 0 : -1;
}

static void refresh_done(job_t *j)
{
    fill_lists();
    if (s_have_server && !s_idx_ok) {
        say(j->msg[0] ? j->msg : "Could not read the server.", 0);
    } else if (s_have_server) {
        char buf[80];
        snprintf(buf, sizeof(buf), "%s: %d app(s)", s_server.name[0] ? s_server.name : "server", s_idx.count);
        say(buf, 1);
    }
}

static void refresh(void)
{
    job_start(job_new(refresh_run, refresh_done));
}

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    say("Reading the server...", 1);
    refresh();
}

static int install_run(job_t *j)
{
    int idx = j->flag;
    if (idx < 0 || idx >= s_idx.count) {
        snprintf(j->msg, sizeof(j->msg), "that app is no longer listed");
        return -1;
    }
    purr_repo_app_t app = s_idx.apps[idx];
    int rc = purr_repo_dev_install(s_server.base, &app, j->msg, sizeof(j->msg));
    if (rc != 0) {
        char t[112];
        snprintf(t, sizeof(t), "%s", j->msg);
        snprintf(j->msg, sizeof(j->msg), "Could not install: %.90s", t);
    } else {
        snprintf(j->msg, sizeof(j->msg), "Installed %s %s.", app.name, app.version);
    }
    return rc;
}

static void install_done(job_t *j)
{
    say(j->msg, j->rc == 0);
    if (j->rc == 0) {
        w10m_apps_changed();                              /* a tile for it, with the DEV mark */
    }
    refresh();
}

static void install_cb(lv_event_t *e)
{
    job_t *j = job_new(install_run, install_done);
    if (j == NULL) {
        return;
    }
    j->flag = (int)(intptr_t)lv_event_get_user_data(e);
    say("Downloading and installing...", 1);
    job_start(j);
}

static int act_run(job_t *j)
{
    char out[96] = "", line[128];
    if (j->flag == 2) {
        snprintf(line, sizeof(line), "close %s", j->a);
        purr_catrt_dispatch(line, out, sizeof(out));
        if (purr_devapps_remove(j->a) == 0) {
            snprintf(j->msg, sizeof(j->msg), "%.60s removed.", j->a);
            return 0;
        }
        snprintf(j->msg, sizeof(j->msg), "Could not remove %.60s.", j->a);
        return -1;
    }
    snprintf(line, sizeof(line), "%s %s", j->flag == 1 ? "close" : "open", j->a);
    int rc = purr_catrt_dispatch(line, out, sizeof(out));
    char *nl = strchr(out, '\n');
    if (nl) *nl = '\0';
    snprintf(j->msg, sizeof(j->msg), "%s", out[0] ? out : (rc == 0 ? "done" : "failed"));
    return rc;
}

static void act_done(job_t *j)
{
    say(j->msg, j->rc == 0);
    if (j->flag == 2 && j->rc == 0) {
        w10m_apps_changed();
    }
    refresh();
}

static void act_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    int idx = code / 4, what = code % 4;
    if (idx < 0 || idx >= s_reg.count) {
        return;
    }
    if (what == 2 && !confirm_tap(lv_event_get_target(e), "Sure?")) {
        return;
    }
    job_t *j = job_new(act_run, act_done);
    if (j == NULL) {
        return;
    }
    snprintf(j->a, sizeof(j->a), "%s", s_reg.apps[idx].name);
    j->flag = what;
    say(what == 2 ? "Removing..." : what == 1 ? "Stopping..." : "Opening...", 1);
    job_start(j);
}

static void servers_cb(lv_event_t *e)
{
    (void)e;
    show_page(PORTAL_SERVERS);
}

static void page_main(void)
{
    title(s_body, "Developer Portal");
    dim(s_body, "Installs apps from an SDK server on your local network into your own developer folder. "
                "Nothing here reaches the internet.");
    char buf[96];
    snprintf(buf, sizeof(buf), "Unsigned apps: %s. Change this in Settings > Privacy & security > Developer options.",
             purr_security_unsigned_apps_allowed() ? "allowed" : "not allowed");
    dim(s_body, buf);

    space(s_body, 2);
    lv_obj_t *srv = row(s_body, s_have_server ? (s_server.name[0] ? s_server.name : "SDK server") : "No server chosen",
                        s_have_server ? s_server.base : "Tap to find or enter one", servers_cb, NULL);
    s_server_row_title = srv;
    button(s_body, "Refresh", refresh_cb, NULL);

    space(s_body, 2);
    title(s_body, "Available");
    s_avail = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_avail);
    lv_obj_set_width(s_avail, LV_PCT(100));
    lv_obj_set_height(s_avail, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_avail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_avail, 2, 0);

    space(s_body, 2);
    title(s_body, "On this device");
    s_have = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_have);
    lv_obj_set_width(s_have, LV_PCT(100));
    lv_obj_set_height(s_have, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_have, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_have, 2, 0);

    make_status();
    fill_lists();
    refresh();
}

/* ---------------------------------------------------------------- choosing a server */

static void use_server(const purr_repo_server_t *sv)
{
    s_server = *sv;
    s_have_server = 1;
    purr_repo_dev_remember(sv);
    s_idx_ok = 0;
    show_page(PORTAL_MAIN);
}

static int connect_run(job_t *j)
{
    char base[80], err[96] = "";
    if (purr_repo_dev_resolve(j->a, base, sizeof(base), err, sizeof(err)) != 0) {
        snprintf(j->msg, sizeof(j->msg), "%s", err);
        return -1;
    }
    static EXT_RAM_BSS_ATTR purr_repo_index_t probe;
    if (purr_repo_dev_index(base, &probe, err, sizeof(err)) != 0) {
        snprintf(j->msg, sizeof(j->msg), "%s", err);
        return -1;
    }
    memset(&s_pending, 0, sizeof(s_pending));
    snprintf(s_pending.base, sizeof(s_pending.base), "%s", base);
    snprintf(s_pending.name, sizeof(s_pending.name), "%s", probe.name[0] ? probe.name : "SDK server");
    snprintf(s_pending.id, sizeof(s_pending.id), "%s", probe.id);
    snprintf(j->msg, sizeof(j->msg), "%d app(s)", probe.count);
    return 0;
}

static void trust_yes_cb(lv_event_t *e)
{
    (void)e;
    use_server(&s_pending);
}

static void trust_no_cb(lv_event_t *e)
{
    (void)e;
    if (s_trust_panel) {
        lv_obj_add_flag(s_trust_panel, LV_OBJ_FLAG_HIDDEN);
    }
    say("Not trusted.", 0);
}

/* A server seen before (same id) is used at once; a new one has to be confirmed by the owner. */
static void ask_trust(const purr_repo_server_t *sv)
{
    s_pending = *sv;
    if (purr_repo_dev_trusted(sv->id)) {
        use_server(sv);
        return;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "Use \"%s\" at %s (id %s)? Apps from it can run on this device. Only trust your own computer.",
             sv->name, sv->base, sv->id[0] ? sv->id : "none");
    lv_label_set_text(s_trust_text, buf);
    lv_obj_remove_flag(s_trust_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_view(s_trust_panel, LV_ANIM_ON);
}

static void connect_done(job_t *j)
{
    if (j->rc != 0) {
        say(j->msg, 0);
        return;
    }
    ask_trust(&s_pending);
}

static void connect_cb(lv_event_t *e)
{
    (void)e;
    const char *typed = lv_textarea_get_text(s_addr);
    if (typed[0] == '\0') {
        say("Type an address first, such as 10.0.2.2:47801.", 0);
        return;
    }
    job_t *j = job_new(connect_run, connect_done);
    if (j == NULL) {
        return;
    }
    snprintf(j->a, sizeof(j->a), "%s", typed);
    say("Connecting...", 1);
    job_start(j);
}

static void found_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < s_nfound) {
        ask_trust(&s_found[i]);
    }
}

static int find_run(job_t *j)
{
    s_nfound = purr_repo_dev_discover(s_found, MAX_FOUND, 2500);
    snprintf(j->msg, sizeof(j->msg), s_nfound ? "Found %d server(s)." : "No server answered. Type its address below.",
             s_nfound);
    return 0;
}

static void find_done(job_t *j)
{
    say(j->msg, s_nfound > 0);
    if (s_list_servers == NULL) {
        return;
    }
    lv_obj_clean(s_list_servers);
    for (int i = 0; i < s_nfound; i++) {
        row(s_list_servers, s_found[i].name, s_found[i].base, found_cb, (void *)(intptr_t)i);
    }
}

static void find_cb(lv_event_t *e)
{
    (void)e;
    say("Looking on the local network...", 1);
    job_start(job_new(find_run, find_done));
}

static void page_servers(void)
{
    title(s_body, "SDK servers");
    dim(s_body, "Run `purrstrap dev serve` on your computer. Only addresses on your local network are accepted.");
    button(s_body, "Find servers", find_cb, NULL);
    s_list_servers = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_list_servers);
    lv_obj_set_width(s_list_servers, LV_PCT(100));
    lv_obj_set_height(s_list_servers, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_list_servers, LV_FLEX_FLOW_COLUMN);

    space(s_body, 2);
    text(s_body, "Or type an address");
    s_addr = input(s_body, "10.0.2.2:47801");
    button(s_body, "Connect", connect_cb, NULL);

    s_trust_panel = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_trust_panel);
    lv_obj_set_width(s_trust_panel, LV_PCT(100));
    lv_obj_set_height(s_trust_panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_trust_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_trust_panel, 6, 0);
    lv_obj_set_style_pad_row(s_trust_panel, 4, 0);
    purr_theme_apply(s_trust_panel, PURR_TH_SURFACE);
    purr_theme_apply(s_trust_panel, PURR_TH_ACCENT_BORDER);
    s_trust_text = text(s_trust_panel, "");
    lv_obj_t *btns = lv_obj_create(s_trust_panel);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 6, 0);
    button(btns, "Trust", trust_yes_cb, NULL);
    button(btns, "Cancel", trust_no_cb, NULL);
    lv_obj_add_flag(s_trust_panel, LV_OBJ_FLAG_HIDDEN);

    make_status();
    if (s_nfound > 0) {
        find_done(&(job_t){.rc = 0});
    }
}

/* ---------------------------------------------------------------- pages and the sheet */

static void show_page(portal_page_t p)
{
    s_page = p;
    s_gen++;
    s_armed = NULL;
    s_status = s_avail = s_have = s_server_row_title = s_list_servers = s_addr = s_trust_panel = s_trust_text = NULL;
    lv_group_remove_all_objs(s_group);
    lv_obj_clean(s_body);
    lv_obj_scroll_to(s_body, 0, 0, LV_ANIM_OFF);
    lv_label_set_text(s_head_title, p == PORTAL_MAIN ? "Developer Portal" : "SDK servers");
    if (p == PORTAL_MAIN) {
        page_main();
    } else {
        page_servers();
    }
}

static void x_cb(void *obj, int32_t v) { lv_obj_set_x((lv_obj_t *)obj, v); }

static void anim_done(lv_anim_t *a)
{
    (void)a;
    if (!s_open) {
        lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);
    }
}

static void move(int open)
{
    s_open = open;
    lv_indev_t *kb = purr_lvgl_keypad();
    if (open) {
        purr_repo_server_t last;
        s_have_server = purr_repo_dev_last(&last) == 0 && purr_repo_dev_trusted(last.id);
        if (s_have_server) {
            s_server = last;
        }
        lv_obj_remove_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);
        show_page(PORTAL_MAIN);
        if (kb != NULL) {
            lv_indev_set_group(kb, s_group);
        }
    } else {
        s_gen++;
        if (kb != NULL) {
            lv_indev_set_group(kb, NULL);
        }
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_sheet);
    lv_anim_set_exec_cb(&a, x_cb);
    lv_anim_set_values(&a, lv_obj_get_x(s_sheet), open ? 0 : w10m_disp_w());
    lv_anim_set_duration(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, anim_done);
    lv_anim_start(&a);
}

static void back_clicked(lv_event_t *e)
{
    (void)e;
    if (s_page == PORTAL_SERVERS) {
        show_page(PORTAL_MAIN);
    } else {
        move(0);
    }
}

void w10m_devportal_open(void)
{
    if (s_sheet != NULL && !s_open && purr_security_dev_active()) {
        move(1);
    }
}

int w10m_devportal_is_open(void) { return s_open; }
lv_obj_t *w10m_devportal_root(void) { return s_open ? s_sheet : NULL; }

void w10m_devportal_back(void)
{
    if (s_sheet != NULL && s_open) {
        back_clicked(NULL);
    }
}

void w10m_devportal_layout(void)
{
    if (s_sheet == NULL) {
        return;
    }
    int w = w10m_disp_w(), h = w10m_disp_h();
    lv_obj_set_size(s_sheet, w, h - W10M_STATUS_H - W10M_NAV_H);
    lv_obj_set_pos(s_sheet, s_open ? 0 : w, W10M_STATUS_H);
    lv_obj_set_size(s_head, w, HEAD_H);
    lv_obj_set_size(s_body, w, h - W10M_STATUS_H - W10M_NAV_H - HEAD_H);
    lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 0, HEAD_H);
    if (!s_open) {
        lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);
    }
}

void w10m_devportal_build(void)
{
    s_group = lv_group_create();
    s_sheet = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_sheet);
    purr_theme_apply(s_sheet, PURR_TH_SHEET);
    lv_obj_clear_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);

    s_head = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(s_head);
    lv_obj_add_flag(s_head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_head, back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow = lv_label_create(s_head);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    purr_theme_apply(arrow, PURR_TH_TEXT);
    lv_obj_align(arrow, LV_ALIGN_LEFT_MID, 8, 0);
    s_head_title = lv_label_create(s_head);
    lv_label_set_text(s_head_title, "Developer Portal");
    lv_obj_set_style_text_font(s_head_title, &lv_font_montserrat_16, 0);
    purr_theme_apply(s_head_title, PURR_TH_TEXT);
    lv_obj_align(s_head_title, LV_ALIGN_LEFT_MID, 34, 0);
    purr_theme_title_bar(s_head, arrow, s_head_title);

    s_body = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_body, 4, 0);
    lv_obj_set_style_pad_hor(s_body, 10, 0);
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
    w10m_devportal_layout();
}
