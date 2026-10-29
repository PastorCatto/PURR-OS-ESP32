/* purr_oobe.c - see purr_oobe.h. */
#include "purr_oobe.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#include "purr_reason.h"
#include "purr_settings.h"
#include "purr_w10m_theme.h"
#include "purr_w10m_widgets.h"

#define MAX_AP 8
#define HEAD_H 24          /* part 2 only: a line for the install that runs in the background */
#define BAR_H  40

typedef enum { PG_WRONG, PG_DEVSERVER, PG_LEGAL, PG_WIFI, PG_RESTORING, PG_COLOUR, PG_ACCOUNT, PG_TIME, PG_DONE, PG_FINISHING } page_t;

/* The last page is only reached from a failed restore ("Use a recovery computer"); the normal flow ends on the restore. */
static const page_t k_loader[] = {PG_WRONG, PG_WIFI, PG_RESTORING, PG_DEVSERVER};
/* When the bootloader allowed a dev server (its DEV_RECOVERY flag), one extra page comes after Wi-Fi and before the install. */
static const page_t k_loader_dev[] = {PG_WRONG, PG_WIFI, PG_DEVSERVER, PG_RESTORING};
static const page_t k_loader_setup[] = {PG_WRONG, PG_WIFI};
static const page_t k_loader_setup_dev[] = {PG_WRONG, PG_WIFI, PG_DEVSERVER};
static const page_t k_finishing[] = {PG_FINISHING};
static const page_t k_first[] = {PG_COLOUR, PG_LEGAL, PG_ACCOUNT, PG_TIME, PG_DONE};

static const purr_oobe_ops_t *s_ops;
static const page_t *s_pages;
static int s_npages, s_idx, s_part;

static lv_obj_t *s_sheet, *s_head, *s_body, *s_bar, *s_back, *s_next, *s_next_label;
static lv_obj_t *s_bg_label, *s_bg_bar;                  /* the background install, above the bottom bar */
static lv_obj_t *s_strip_lbl, *s_strip_bar;              /* the loader's and the finishing page's strip: a status line and a thin bar up top */
static lv_obj_t *s_status;                               /* the page's one-line result */
static lv_group_t *s_group;
static lv_timer_t *s_timer;
static int s_job_active = -1;                            /* purr_oobe_job_t being polled, or -1 */

/* What the user chose, for the summary and for the next steps. */
static char s_ssid[33], s_account[32];
static int s_connected, s_account_ok, s_restoring_done;
static purr_oobe_ap_t s_ap[MAX_AP];
static int s_nap;
static lv_obj_t *s_ta_name, *s_ta_pass, *s_ta_confirm, *s_ta_wifi_pass;
static lv_obj_t *s_relay_btn;
static lv_obj_t *s_progress, *s_stage, *s_wifi_connect, *s_legal_label, *s_srv_list;
#define MAX_SRV 4
static purr_oobe_server_t s_srv[MAX_SRV];
static int s_nsrv, s_srv_sel = -1;

static void show_page(int idx);

static void set_status(const char *text, int good)
{
    if (s_status == NULL) {
        return;
    }
    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status, good ? purr_theme_accent_color() : lv_color_hex(0xFFB900), 0);
}

static void make_status(void)
{
    s_status = lv_label_create(s_body);
    lv_label_set_text(s_status, "");
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, LV_PCT(100));
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_12, 0);
}

static void set_next(const char *text, int enabled)
{
    lv_label_set_text(s_next_label, text);
    if (enabled) {
        lv_obj_remove_state(s_next, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_next, LV_STATE_DISABLED);
    }
}

static void next_clicked(lv_event_t *e);
static void back_clicked(lv_event_t *e);

/* ---------------------------------------------------------------- jobs */

static void job_start(purr_oobe_job_t kind, const char *a, const char *b)
{
    if (s_ops->job_start(kind, a, b) == 0) {
        s_job_active = (int)kind;
    } else {
        set_status("Could not start that.", 0);
    }
}

static void wifi_list_build(void);
static void srv_list_build(void);

/* The strip (the same look as part 2's background-install line): 24 px with the status on the right and a 3 px bar on its lower edge.
 * On while the loader restores or the system finishes; off on every other page of those parts, which get the room back. */
static void strip_set(int on)
{
    if (s_strip_lbl == NULL) {
        return;
    }
    int h = lv_display_get_vertical_resolution(NULL);
    if (on) {
        lv_obj_remove_flag(s_strip_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_strip_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_local_style_prop(s_strip_lbl, LV_STYLE_TEXT_COLOR, 0);      /* a failure turned it amber */
        lv_label_set_text(s_strip_lbl, "Starting...");
        lv_bar_set_value(s_strip_bar, 0, LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(s_strip_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_strip_bar, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 0, on ? HEAD_H : 0);
    lv_obj_set_height(s_body, h - (on ? HEAD_H : 0) - BAR_H);
}

static void bg_update(void)
{
    char msg[48] = "";
    int p = s_ops->background != NULL ? s_ops->background(msg, sizeof(msg)) : -1;
    if (s_pages[s_idx] == PG_FINISHING && s_progress != NULL) {
        if (p == -2) {                                          /* it failed: say so and stop; power cycling tries again */
            lv_label_set_text(s_stage, msg[0] ? msg : "The install did not finish. Power it off and on to try again.");
            lv_obj_set_style_text_color(s_stage, lv_color_hex(0xFFB900), 0);
        } else if (p == -1 || p >= 1000) {
            if (!s_restoring_done) {
                s_restoring_done = 1;
                lv_bar_set_value(s_progress, 1000, LV_ANIM_OFF);
                lv_label_set_text(s_stage, "Restarting...");
                s_ops->finish();
            }
        } else {
            lv_bar_set_value(s_progress, p, LV_ANIM_OFF);
            lv_label_set_text(s_stage, msg[0] ? msg : "Installing...");
        }
        return;
    }
    if (s_bg_label == NULL) {
        return;
    }
    if (s_pages[s_idx] == PG_DONE) {                            /* Finish waits only for an install that is still running */
        set_next("Finish", p == -1 || p == -2 || p >= 1000);
    }
    if (p == -2) {                                              /* it failed: say so, in amber, and let them carry on */
        lv_obj_remove_flag(s_bg_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_bg_bar, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_bg_label, msg[0] ? msg : "The install did not finish");
        lv_obj_set_style_text_color(s_bg_label, lv_color_hex(0xFFB900), 0);
        lv_obj_align(s_bg_label, LV_ALIGN_RIGHT_MID, -10, 0);
        return;
    }
    if (p < 0) {
        lv_obj_add_flag(s_bg_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_bg_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_bg_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_bg_bar, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_bg_label, "%s", msg[0] ? msg : (p >= 1000 ? "System installed" : "Still installing the system..."));
    lv_obj_align(s_bg_label, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_bar_set_value(s_bg_bar, p, LV_ANIM_OFF);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    bg_update();
    if (s_job_active < 0) {
        return;
    }
    char msg[64] = "";
    int per = -1;
    int r = s_ops->job_poll((purr_oobe_job_t)s_job_active, msg, sizeof(msg), &per);
    if (r < 0) {
        if (s_job_active == PURR_OOBE_JOB_RESTORE && s_progress != NULL && per >= 0) {
            lv_bar_set_value(s_progress, per, LV_ANIM_OFF);
            if (msg[0]) {
                lv_label_set_text(s_stage, msg);
            }
        }
        return;
    }
    purr_oobe_job_t kind = (purr_oobe_job_t)s_job_active;
    s_job_active = -1;
    switch (kind) {
    case PURR_OOBE_JOB_DEVSCAN:
        s_nsrv = s_ops->server_results != NULL ? s_ops->server_results(s_srv, MAX_SRV) : 0;
        if (s_pages[s_idx] == PG_DEVSERVER) {
            srv_list_build();
        }
        break;
    case PURR_OOBE_JOB_DEVVERIFY:
        if (s_pages[s_idx] == PG_DEVSERVER) {
            set_status(msg[0] ? msg : (r == 0 ? "Verified." : "That didn't check out."), r == 0);
            set_next("Use it", r == 0);
        }
        break;
    case PURR_OOBE_JOB_SCAN:
        s_nap = s_ops->scan_results(s_ap, MAX_AP);
        if (s_pages[s_idx] == PG_WIFI) {
            wifi_list_build();
        }
        break;
    case PURR_OOBE_JOB_CONNECT:
        if (r == 0) {
            s_connected = 1;
            set_status("Connected!", 1);
            set_next("Next", 1);
        } else {
            set_status(msg[0] ? msg : "Couldn't connect. Check the password and try again.", 0);
        }
        break;
    case PURR_OOBE_JOB_ACCOUNT:
        if (r == 0) {
            s_account_ok = 1;
            show_page(s_idx + 1);
        } else {
            set_status(msg[0] ? msg : "We couldn't create the account.", 0);
            set_next("Next", 1);
        }
        break;
    case PURR_OOBE_JOB_LEGAL:
        if (s_pages[s_idx] == PG_LEGAL && s_legal_label != NULL) {
            const char *text = r == 0 && s_ops->legal != NULL ? s_ops->legal() : NULL;
            while (text != NULL && text[0] == '#') {          /* lines starting with # are notes for whoever edits the file */
                const char *nl = strchr(text, '\n');
                text = nl != NULL ? nl + 1 : text + strlen(text);
            }
            lv_label_set_text(s_legal_label, text != NULL && text[0]
                              ? text : "We couldn't load the legal text. You can read it in Settings once you are set up.");
        }
        break;
    case PURR_OOBE_JOB_RESTORE:
        if (r == 0) {
            s_restoring_done = 1;
            lv_bar_set_value(s_progress, 1000, LV_ANIM_OFF);
            lv_label_set_text(s_stage, "Restarting...");
            s_ops->finish();
        } else {
            lv_label_set_text(s_stage, msg[0] ? msg : "That didn't work. Power it off and on to try again.");
            lv_obj_set_style_text_color(s_stage, lv_color_hex(0xFFB900), 0);
            if (s_relay_btn != NULL) {
                lv_obj_remove_flag(s_relay_btn, LV_OBJ_FLAG_HIDDEN);       /* the server could not be reached: another route */
            }
        }
        break;
    }
}

/* ---------------------------------------------------------------- the pages */

/* One line up top, then why. The reasons come from the bootloader's hand-off (PURR_RR_*, purr_reason.h). */
static void page_wrong(void)
{
    uint16_t reasons = s_ops->reasons != NULL ? s_ops->reasons() : 0;
    if (purr_reason_is_first_run(reasons)) {                    /* nothing was ever installed: not a failure, a first run */
        lv_obj_t *g = lv_label_create(s_body);
        lv_label_set_text(g, "Hi there, first time?");
        lv_label_set_long_mode(g, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(g, LV_PCT(100));
        lv_obj_set_style_text_font(g, &lv_font_montserrat_20, 0);
        purr_theme_apply(g, PURR_TH_TEXT);
        w10m_text(s_body, "This is a network install.");
        w10m_space(s_body, 6);
        w10m_dim(s_body, "Looks like this is the first time it has been run. We'll hop on Wi-Fi and grab PURR OS for you.");
        set_next("OK", 1);
        return;
    }
    lv_obj_t *h = lv_label_create(s_body);
    lv_label_set_text(h, "Uh-oh, something went wrong.");
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, LV_PCT(100));
    lv_obj_set_style_text_font(h, &lv_font_montserrat_20, 0);
    purr_theme_apply(h, PURR_TH_TEXT);
    w10m_text(s_body, "We need to recover this device.");
    w10m_space(s_body, 6);
    const char *lines[PURR_RR_COUNT];
    int n = purr_reason_lines(reasons, lines, PURR_RR_COUNT);
    if (n == 0) {
        w10m_dim(s_body, "The device could not start the way it should.");
    }
    for (int i = 0; i < n; i++) {
        char buf[96];
        snprintf(buf, sizeof(buf), "- %s", lines[i]);
        w10m_dim(s_body, buf);
    }
    set_next("OK", 1);
}

static void page_legal(void)
{
    w10m_title(s_body, "The legal stuff & cool people");
    s_legal_label = lv_label_create(s_body);
    lv_label_set_text(s_legal_label, "Just a sec...");
    lv_label_set_long_mode(s_legal_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_legal_label, LV_PCT(100));
    lv_obj_set_style_text_font(s_legal_label, &lv_font_montserrat_12, 0);
    purr_theme_apply(s_legal_label, PURR_TH_DIM);
    set_next("Continue", 1);
    job_start(PURR_OOBE_JOB_LEGAL, NULL, NULL);
}

static void ap_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_nap) {
        return;
    }
    snprintf(s_ssid, sizeof(s_ssid), "%s", s_ap[i].ssid);
    const char *rem = s_ops->remembered_ssid ? s_ops->remembered_ssid() : NULL;
    if (rem != NULL && strcmp(rem, s_ssid) == 0) {              /* a remembered network needs no password */
        set_status("Connecting...", 1);
        job_start(PURR_OOBE_JOB_CONNECT, s_ssid, "");
        return;
    }
    lv_textarea_set_text(s_ta_wifi_pass, "");
    lv_obj_remove_flag(s_ta_wifi_pass, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_wifi_connect, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_status, "%s: type the password, then Connect.", s_ssid);
    lv_obj_set_style_text_color(s_status, purr_theme_accent_color(), 0);
}

static void wifi_connect_cb(lv_event_t *e)
{
    (void)e;
    if (s_ssid[0] == '\0') {
        set_status("Pick a network first.", 0);
        return;
    }
    set_status("Connecting...", 1);
    job_start(PURR_OOBE_JOB_CONNECT, s_ssid, lv_textarea_get_text(s_ta_wifi_pass));
}

static lv_obj_t *s_ap_list;

static void wifi_list_build(void)
{
    lv_obj_clean(s_ap_list);
    if (s_nap <= 0) {
        w10m_dim(s_ap_list, "No networks found. Move a little closer, then go Back and try again.");
        return;
    }
    const char *rem = s_ops->remembered_ssid ? s_ops->remembered_ssid() : NULL;
    for (int i = 0; i < s_nap; i++) {
        char sub[48];
        snprintf(sub, sizeof(sub), "%s%s%d dBm", (rem && strcmp(rem, s_ap[i].ssid) == 0) ? "remembered   " : "",
                 s_ap[i].secure ? "secured   " : "open   ", s_ap[i].rssi);
        w10m_row(s_ap_list, s_ap[i].ssid, sub, 1, ap_clicked, (void *)(intptr_t)i);
    }
}

static void page_wifi(void)
{
    w10m_title(s_body, "Let's get you connected");
    w10m_dim(s_body, "Pick your network and PURR OS will be ready for you.");
    s_connected = 0;
    set_next("Next", 0);
    if (s_ops->wired != NULL && s_ops->wired()) {
        w10m_dim(s_body, "You're on a wired network. Waiting for an address...");
        make_status();
        job_start(PURR_OOBE_JOB_CONNECT, "", "");
        return;
    }
    s_ap_list = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_ap_list);
    lv_obj_set_width(s_ap_list, LV_PCT(100));
    lv_obj_set_height(s_ap_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_ap_list, LV_FLEX_FLOW_COLUMN);
    w10m_dim(s_ap_list, "Looking for networks...");
    s_ta_wifi_pass = w10m_input(s_body, s_group, "Password", 1);
    lv_obj_add_flag(s_ta_wifi_pass, LV_OBJ_FLAG_HIDDEN);
    s_wifi_connect = w10m_button(s_body, "Connect", wifi_connect_cb, NULL);
    lv_obj_set_width(s_wifi_connect, LV_PCT(100));
    lv_obj_add_flag(s_wifi_connect, LV_OBJ_FLAG_HIDDEN);
    make_status();
    s_nap = 0;
    s_ssid[0] = '\0';
    job_start(PURR_OOBE_JOB_SCAN, NULL, NULL);
}

/* A bar in the theme's accent on a dim track. */
static void style_bar(lv_obj_t *bar)
{
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x3A3A3A), 0);
    lv_obj_set_style_bg_color(bar, purr_theme_accent_color(), LV_PART_INDICATOR);
}

/* ---- the dev server page (only when the bootloader's DEV_RECOVERY flag allowed it) */

static void srv_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_nsrv) {
        return;
    }
    s_srv_sel = i;
    set_status("Checking it...", 1);
    set_next("Use it", 0);
    job_start(PURR_OOBE_JOB_DEVVERIFY, s_srv[i].base, NULL);
}

static void srv_list_build(void)
{
    lv_obj_clean(s_srv_list);
    if (s_nsrv <= 0) {
        w10m_dim(s_srv_list, "Nothing found. Is broken-kitten-recovery running on a computer on this network? You can try the internet instead.");
        return;
    }
    for (int i = 0; i < s_nsrv; i++) {
        char sub[120];
        snprintf(sub, sizeof(sub), "%s   %s", s_srv[i].base, strncmp(s_srv[i].id, "relay-", 6) == 0 ? "recovery computer" : "dev server");
        w10m_row(s_srv_list, s_srv[i].name, sub, 1, srv_clicked, (void *)(intptr_t)i);
    }
}

static void show_page(int idx);
static void goto_page(page_t pg)
{
    for (int i = 0; i < s_npages; i++) {
        if (s_pages[i] == pg) {
            show_page(i);
            return;
        }
    }
}

static void internet_cb(lv_event_t *e)
{
    (void)e;
    if (s_ops->use_server != NULL) {
        s_ops->use_server(NULL);
    }
    goto_page(PG_RESTORING);
}

static void page_devserver(void)
{
    if (s_ops->dev_recovery != NULL && s_ops->dev_recovery()) {
        w10m_title(s_body, "Use your dev server?");
        w10m_dim(s_body, "Developer mode is on, so we can grab PURR OS from a server on your network instead of the internet.");
    } else {
        w10m_title(s_body, "Use a recovery computer?");
        w10m_dim(s_body, "A computer on this network running broken-kitten-recovery can send PURR OS to this device. Pick it only if it is yours.");
    }
    s_srv_list = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_srv_list);
    lv_obj_set_width(s_srv_list, LV_PCT(100));
    lv_obj_set_height(s_srv_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_srv_list, LV_FLEX_FLOW_COLUMN);
    w10m_dim(s_srv_list, "Looking for servers...");
    make_status();
    lv_obj_t *b = w10m_button(s_body, "Use the internet instead", internet_cb, NULL);
    lv_obj_set_width(b, LV_PCT(100));
    w10m_dim(s_body, "It still has to be signed by the boot key before anything is installed.");
    s_nsrv = 0;
    s_srv_sel = -1;
    set_next("Use it", 0);
    job_start(PURR_OOBE_JOB_DEVSCAN, NULL, NULL);
}

static void relay_cb(lv_event_t *e)
{
    (void)e;
    goto_page(PG_DEVSERVER);
}

static void page_restoring(void)
{
    strip_set(1);
    s_stage = s_strip_lbl;
    s_progress = s_strip_bar;
    w10m_title(s_body, "Okay, getting this sorted out.");
    w10m_dim(s_body, "This takes a few minutes. Keep it charged or plugged in.");
    s_relay_btn = w10m_button(s_body, "Use a recovery computer", relay_cb, NULL);
    lv_obj_set_width(s_relay_btn, LV_PCT(100));
    lv_obj_add_flag(s_relay_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_next, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    s_restoring_done = 0;
    job_start(PURR_OOBE_JOB_RESTORE, NULL, NULL);           /* KittenOS and what it needs to restart; it does the rest */
}

/* The reinstall carries on after the restore (an existing account, so no setup): the same look as the restore page, so it reads as a
 * system finishing its work and not as the recovery menu. The install itself is the background job; restarts into PURR OS when it ends. */
static void page_finishing(void)
{
    strip_set(1);
    s_stage = s_strip_lbl;
    s_progress = s_strip_bar;
    w10m_title(s_body, "Just a few more minutes");
    w10m_dim(s_body, "Your files are safe. PURR OS starts by itself when this is done. Keep it charged or plugged in.");
    lv_obj_add_flag(s_next, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    s_restoring_done = 0;
}

/* ---- part 2 */

static int name_ok(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len > 31 || strcmp(n, "root") == 0) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return 0;
        }
    }
    return 1;
}

static void account_check_cb(lv_event_t *e)
{
    (void)e;
    const char *n = lv_textarea_get_text(s_ta_name), *p = lv_textarea_get_text(s_ta_pass),
               *c = lv_textarea_get_text(s_ta_confirm);
    if (n[0] && !name_ok(n)) {
        set_status("Names can use letters, digits, - and _.", 0);
    } else if (p[0] && c[0] && strcmp(p, c) != 0) {
        set_status("The passwords don't match yet.", 0);
    } else if (name_ok(n) && p[0] && strcmp(p, c) == 0) {
        set_status("Looks good.", 1);
    } else {
        set_status("", 1);
    }
    set_next("Next", name_ok(n) && p[0] && strcmp(p, c) == 0);
}

static void page_account(void)
{
    w10m_title(s_body, "Let's set up your account");
    s_ta_name = w10m_input(s_body, s_group, "User name", 0);
    s_ta_pass = w10m_input(s_body, s_group, "Password", 1);
    s_ta_confirm = w10m_input(s_body, s_group, "Confirm password", 1);
    lv_obj_add_event_cb(s_ta_name, account_check_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_ta_pass, account_check_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_ta_confirm, account_check_cb, LV_EVENT_VALUE_CHANGED, NULL);
    make_status();
    w10m_dim(s_body, "You'll be the administrator, so you can add people and change security settings.");
    set_next("Next", 0);
}

static void zone_cb(lv_event_t *e)
{
    char name[48];
    lv_dropdown_get_selected_str(lv_event_get_target(e), name, sizeof(name));
    const purr_setting_t *s = purr_settings_find("time.zone");
    char err[48];
    if (s != NULL) {
        s->set(name, err, sizeof(err));
    }
}

static void clock_cb(lv_event_t *e)
{
    purr_settings_kv_set("time.clock24", lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? "1" : "0");
}

static void swatch_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    purr_theme_set_accent(i);
}

static void mode_cb(lv_event_t *e)
{
    purr_theme_set_dark(lv_dropdown_get_selected(lv_event_get_target(e)) == 0);
}

static void page_time(void)
{
    w10m_title(s_body, "Date and time");
    w10m_text(s_body, "Time zone");
    char names[400] = "";
    for (int i = 0; i < purr_settings_zone_count(); i++) {
        size_t used = strlen(names);
        snprintf(names + used, sizeof(names) - used, "%s%s", i ? "\n" : "", purr_settings_zone_name(i));
    }
    char cur[48] = "UTC";
    purr_settings_kv_get("time.zone", cur, sizeof(cur));
    int sel = 0;
    for (int i = 0; i < purr_settings_zone_count(); i++) {
        if (strcasecmp(cur, purr_settings_zone_name(i)) == 0) {
            sel = i;
        }
    }
    w10m_dropdown(s_body, names, sel, 200, zone_cb);
    w10m_space(s_body, 4);
    w10m_switch(s_body, "24-hour clock", purr_settings_kv_int("time.clock24", 1), clock_cb);
    w10m_dim(s_body, "No stress, you can change these in Settings any time.");
}

static void page_colour(void)
{
    w10m_title(s_body, "Pick a colour you like");
    lv_obj_t *grid = lv_obj_create(s_body);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, 82);                      /* three rows of swatches */
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, 4, 0);
    for (int i = 0; i < purr_theme_accent_count(); i++) {
        lv_obj_t *sw = lv_obj_create(grid);
        lv_obj_remove_style_all(sw);
        lv_obj_set_size(sw, 28, 22);
        lv_obj_set_style_bg_color(sw, lv_color_hex(purr_theme_accent_at(i)->color), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(sw, swatch_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    w10m_space(s_body, 2);
    w10m_text(s_body, "Mode");
    w10m_dropdown(s_body, "Dark\nLight", purr_theme_dark() ? 0 : 1, 120, mode_cb);
}

static void page_done(void)
{
    w10m_title(s_body, "All set!");
    char line[64];
    snprintf(line, sizeof(line), "Account:  %s", s_account[0] ? s_account : "-");
    w10m_text(s_body, line);
    char zone[48] = "";
    purr_settings_kv_get("time.zone", zone, sizeof(zone));
    snprintf(line, sizeof(line), "Time zone:  %s", zone[0] ? zone : "UTC");
    w10m_text(s_body, line);
    snprintf(line, sizeof(line), "Look:  %s, %s", purr_theme_accent_at(purr_theme_accent())->name,
             purr_theme_dark() ? "dark" : "light");
    w10m_text(s_body, line);
    w10m_space(s_body, 4);
    w10m_dim(s_body, "Finish restarts into PURR OS, and then you can sign in.");
    set_next("Finish", 0);
    bg_update();
}

/* ---------------------------------------------------------------- the frame */

static void show_page(int idx)
{
    if (idx < 0 || idx >= s_npages) {
        if (idx >= s_npages) {
            s_ops->finish();
        }
        return;
    }
    s_idx = idx;
    s_job_active = -1;
    s_status = NULL;
    s_progress = s_stage = s_legal_label = s_srv_list = s_relay_btn = NULL;
    strip_set(0);                                               /* a page that wants the strip turns it back on */
    lv_obj_clean(s_body);
    lv_group_remove_all_objs(s_group);
    lv_obj_remove_flag(s_next, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    if (idx == 0) {
        lv_obj_add_flag(s_back, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s_next, 200);                          /* the only button on the first page: centred */
        lv_obj_align(s_next, LV_ALIGN_CENTER, 0, 0);
    } else {
        lv_obj_set_width(s_next, 110);
        lv_obj_align(s_next, LV_ALIGN_RIGHT_MID, -10, 0);
    }
    set_next("Next", 1);
    lv_obj_scroll_to_y(s_body, 0, LV_ANIM_OFF);
    switch (s_pages[idx]) {
    case PG_WRONG:     page_wrong(); break;
    case PG_DEVSERVER: page_devserver(); break;
    case PG_LEGAL:     page_legal(); break;
    case PG_WIFI:      page_wifi(); break;
    case PG_RESTORING: page_restoring(); break;
    case PG_ACCOUNT:   page_account(); break;
    case PG_TIME:      page_time(); break;
    case PG_COLOUR:    page_colour(); break;
    case PG_DONE:      page_done(); break;
    case PG_FINISHING: page_finishing(); break;
    }
}

static void next_clicked(lv_event_t *e)
{
    (void)e;
    if (s_pages[s_idx] == PG_ACCOUNT) {                          /* creating the account takes a moment (hashing) */
        snprintf(s_account, sizeof(s_account), "%s", lv_textarea_get_text(s_ta_name));
        set_status("Creating your account...", 1);
        set_next("Next", 0);
        job_start(PURR_OOBE_JOB_ACCOUNT, s_account, lv_textarea_get_text(s_ta_pass));
        return;
    }
    if (s_pages[s_idx] == PG_DONE) {
        s_ops->finish();
        return;
    }
    if (s_pages[s_idx] == PG_DEVSERVER) {                        /* the owner confirmed this server: from here on, pull from it */
        if (s_srv_sel >= 0 && s_srv_sel < s_nsrv && s_ops->use_server != NULL) {
            s_ops->use_server(&s_srv[s_srv_sel]);
        }
        goto_page(PG_RESTORING);
        return;
    }
    show_page(s_idx + 1);
}

static void back_clicked(lv_event_t *e)
{
    (void)e;
    show_page(s_idx - 1);
}

void purr_oobe_start(int part, const purr_oobe_ops_t *ops, int start_page)
{
    s_ops = ops;
    s_part = part;
    int dev = (part == PURR_OOBE_PART_LOADER || part == PURR_OOBE_PART_LOADER_SETUP) && ops->dev_recovery != NULL && ops->dev_recovery();
    if (part == PURR_OOBE_PART_FINISHING) {
        s_pages = k_finishing;
        s_npages = 1;
    } else if (part == PURR_OOBE_PART_LOADER_SETUP) {
        s_pages = dev ? k_loader_setup_dev : k_loader_setup;
        s_npages = dev ? 3 : 2;
    } else {
    s_pages = part == PURR_OOBE_PART_LOADER ? (dev ? k_loader_dev : k_loader) : k_first;
    s_npages = part == PURR_OOBE_PART_LOADER ? (dev ? (int)(sizeof(k_loader_dev) / sizeof(k_loader_dev[0]))
                                                    : (int)(sizeof(k_loader) / sizeof(k_loader[0])))
                                             : (int)(sizeof(k_first) / sizeof(k_first[0]));
    }
    int w = lv_display_get_horizontal_resolution(NULL), h = lv_display_get_vertical_resolution(NULL);

    s_group = lv_group_create();
    if (s_ops->use_keypad != NULL) {
        s_ops->use_keypad(s_group);
    }
    s_sheet = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_sheet);
    purr_theme_apply(s_sheet, PURR_TH_SHEET);
    lv_obj_set_size(s_sheet, w, h);
    lv_obj_clear_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);

    int head_h = part == PURR_OOBE_PART_FIRSTBOOT ? HEAD_H : 0;       /* only part 2 has a header: the background install */
    s_head = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(s_head);
    lv_obj_set_size(s_head, w, head_h > 0 ? head_h : 1);
    lv_obj_clear_flag(s_head, LV_OBJ_FLAG_SCROLLABLE);

    s_bar = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_size(s_bar, w, BAR_H);
    lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);
    s_back = w10m_button(s_bar, "Back", back_clicked, NULL);
    lv_obj_set_size(s_back, 78, 28);
    lv_obj_align(s_back, LV_ALIGN_LEFT_MID, 10, 0);
    s_next = w10m_button(s_bar, "Next", next_clicked, NULL);
    lv_obj_set_size(s_next, 110, 28);
    lv_obj_align(s_next, LV_ALIGN_RIGHT_MID, -10, 0);
    purr_theme_apply(s_next, PURR_TH_ACCENT_BG);
    s_next_label = lv_obj_get_child(s_next, 0);

    /* The install that carries on in the background: a line of text in the header and a thin bar under it. */
    s_bg_label = lv_label_create(s_head);
    lv_obj_set_style_text_font(s_bg_label, &lv_font_montserrat_12, 0);
    purr_theme_apply(s_bg_label, PURR_TH_DIM);
    s_bg_bar = lv_bar_create(s_sheet);
    lv_obj_set_size(s_bg_bar, w, 3);
    lv_bar_set_range(s_bg_bar, 0, 1000);
    style_bar(s_bg_bar);
    lv_obj_align(s_bg_bar, LV_ALIGN_TOP_LEFT, 0, HEAD_H - 2);
    lv_obj_add_flag(s_bg_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_bg_bar, LV_OBJ_FLAG_HIDDEN);

    s_strip_lbl = s_strip_bar = NULL;
    if (part != PURR_OOBE_PART_FIRSTBOOT) {                     /* part 2 has its own header line above; the others borrow the same look */
        s_strip_lbl = lv_label_create(s_sheet);
        lv_obj_set_style_text_font(s_strip_lbl, &lv_font_montserrat_12, 0);
        purr_theme_apply(s_strip_lbl, PURR_TH_DIM);
        lv_label_set_long_mode(s_strip_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(s_strip_lbl, w - 20);
        lv_obj_set_style_text_align(s_strip_lbl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(s_strip_lbl, LV_ALIGN_TOP_LEFT, 10, 5);
        s_strip_bar = lv_bar_create(s_sheet);
        lv_obj_set_size(s_strip_bar, w, 3);
        lv_bar_set_range(s_strip_bar, 0, 1000);
        style_bar(s_strip_bar);
        lv_obj_align(s_strip_bar, LV_ALIGN_TOP_LEFT, 0, HEAD_H - 2);
        lv_obj_add_flag(s_strip_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_strip_bar, LV_OBJ_FLAG_HIDDEN);
    }

    s_body = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, w, h - head_h - BAR_H);
    lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 0, head_h);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_body, 4, 0);
    lv_obj_set_style_pad_hor(s_body, 12, 0);
    lv_obj_set_style_pad_top(s_body, 10, 0);
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);

    if (s_timer == NULL) {
        s_timer = lv_timer_create(tick_cb, 100, NULL);
    }
    if (start_page < 0) {                                       /* -1: straight to the install (an unattended recovery) */
        goto_page(PG_RESTORING);
        return;
    }
    show_page(start_page < s_npages ? start_page : 0);
}

int purr_oobe_page_count(int part)
{
    return part == PURR_OOBE_PART_LOADER ? (int)(sizeof(k_loader) / sizeof(k_loader[0]))
                                         : (int)(sizeof(k_first) / sizeof(k_first[0]));
}
