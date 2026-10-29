/* recovery_app.c - see recovery_app.h. */
#include "recovery_app.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "purr_abi.h"

int recovery_gate_open_for(uint16_t reasons)
{
    return (reasons & (PURR_RR_DAMAGED | PURR_RR_NO_SYSTEM | PURR_RR_CONFIG | PURR_RR_REPEATED)) != 0;
}

#if CONFIG_PURR_PROFILE_RECOVERY && (CONFIG_PURR_BOARD_TDECK || CONFIG_PURR_BOARD_TDECK_PLUS)

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "login.h"
#include "oobe_job.h"
#include "purr_cfg.h"
#include "purr_cfgstore.h"
#include "purr_kernel.h"
#include "purr_lvgl.h"
#include "purr_reason.h"
#include "purr_ui_service.h"
#include "purr_trust_os.h"
#include "purr_users.h"
#include "purr_w10m_theme.h"
#include "purr_w10m_widgets.h"

static const char *TAG = "recovery";

typedef enum {
    SC_HOME, SC_KEEP, SC_ERASE_INFO, SC_GATE, SC_ERASE_SURE, SC_WORKING,
    SC_SECURITY, SC_WEAKEN, SC_WEAKEN_KEEP
} screen_t;

static lv_obj_t *s_sheet, *s_body;
static lv_group_t *s_group;
static lv_timer_t *s_timer;
static lv_obj_t *s_ta_user, *s_ta_pass, *s_status, *s_bar, *s_stage;
static lv_obj_t *s_strip_lbl, *s_strip_bar;            /* the working screen's strip: a status line and a thin bar up top (the setup wizard's look) */
#define STRIP_H 24
static oobe_job_t *s_job;
static uint16_t s_reasons;
static int s_open;                         /* damaged system, or no accounts: reinstalling and erasing need no password */
static char s_user[PURR_USER_NAME_LEN], s_pass[72];
static int s_pending;                      /* a job was started and its result not handled yet */
static screen_t s_after;                   /* where the administrator check leads once it passes */
static int s_weaken_ok;                    /* the 'I understand' switch on the keep-my-data warning */
static int s_erase;                        /* what the working screen is doing: 1 erase + reinstall, 0 reinstall */

static void show(screen_t sc);

/* ---------------------------------------------------------------- the actions (worker task) */

static int set_flags(uint32_t set)
{
    purr_flash_t fl;
    purr_cfg_t cfg;
    if (purr_cfgstore_open(&fl) != 0 || purr_cfg_load(&fl, &cfg, NULL) < 0) {
        return -1;
    }
    cfg.flags |= set;
    return purr_cfg_store(&fl, &cfg);
}

static int wipe_partition(oobe_job_t *job, const char *label, esp_partition_subtype_t sub, int from, int to)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, sub, label);
    if (p == NULL) {
        return -1;
    }
    const uint32_t step = 0x10000;
    for (uint32_t off = 0; off < p->size; off += step) {
        uint32_t n = p->size - off < step ? p->size - off : step;
        if (esp_partition_erase_range(p, off, (n + 0xFFF) & ~0xFFFu) != ESP_OK) {
            return -1;
        }
        oobe_job_report(job, NULL, from + (int)((uint64_t)(to - from) * (off + n) / p->size));
    }
    return 0;
}

static int job_reinstall(oobe_job_t *job, void *arg)
{
    int erase = (int)(intptr_t)arg;
    if (erase) {
        oobe_job_report(job, "Erasing everything...", 0);
        if (wipe_partition(job, "root", (esp_partition_subtype_t)0x83, 0, 900) != 0) {
            oobe_job_abort(job, "Couldn't erase the storage. Nothing was reinstalled.");
        }
    }
    oobe_job_report(job, "Getting ready to reinstall...", 950);
    /* A reset: the kernel and KittenOS slots are marked BAD (the bootloader will not launch them, even if this is interrupted), the
     * loader is forced, and it is asked to reinstall on its own with the saved network, which is kept for that (the unattended
     * path is the one that restores devices reliably). FRESH again after an erase: the next system starts as a first run. */
    if (set_flags(PURR_CFGF_BAD_KERNEL | PURR_CFGF_BAD_KITTENOS | PURR_CFGF_FORCE_LOADER | PURR_CFGF_AUTO_REINSTALL |
                  (erase ? PURR_CFGF_FRESH : 0)) != 0) {
        oobe_job_abort(job, "Couldn't ask for the reinstall.");
    }
    oobe_job_report(job, "Restarting...", 1000);
    return 0;
}

/* Weakening the bootloader (development mode relaxes enforce to warn). With the wipe the data is gone first, so nobody can
 * weaken a device and then read what was on it. */
static int job_weaken(oobe_job_t *job, void *arg)
{
    int wipe = (int)(intptr_t)arg;
    if (wipe) {
        oobe_job_report(job, "Erasing your data first...", 0);
        if (wipe_partition(job, "root", (esp_partition_subtype_t)0x83, 0, 900) != 0) {
            oobe_job_abort(job, "Couldn't erase the storage, so security was not changed.");
        }
        wipe_partition(job, "netrec", (esp_partition_subtype_t)0x41, 900, 950);
        set_flags(PURR_CFGF_FRESH);
    }
    oobe_job_report(job, "Changing security...", 950);
    int r = purr_secure_set_dev(1);
    if (r == -2) {
        oobe_job_abort(job, "This device's signed policy doesn't allow that.");
    }
    if (r != 0) {
        oobe_job_abort(job, "Couldn't change the security setting.");
    }
    oobe_job_report(job, "Restarting...", 1000);
    return 0;
}

static int job_restore_security(oobe_job_t *job, void *arg)
{
    (void)arg;
    if (purr_secure_set_dev(0) != 0) {
        oobe_job_abort(job, "Couldn't change the security setting.");
    }
    oobe_job_report(job, "Restarting...", 1000);
    return 0;
}

/* Checks an administrator's password (a PBKDF2 hash: takes a moment, so it is a job too). */
static int job_verify(oobe_job_t *job, void *arg)
{
    (void)arg;
    uint32_t wait = 0;
    purr_login_result_t r = purr_login_elevate(s_user, s_pass, &wait);   /* the session becomes theirs until restart */
    memset(s_pass, 0, sizeof(s_pass));
    if (r == PURR_LOGIN_WAIT) {
        char m[56];
        snprintf(m, sizeof(m), "Too many tries. Wait %u s.", (unsigned)wait);
        oobe_job_report(job, m, -1);
        return 1;
    }
    if (r != PURR_LOGIN_OK) {
        oobe_job_report(job, "That's not right, or that account isn't an administrator.", -1);
        return 1;
    }
    return 0;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

/* ---------------------------------------------------------------- screens */

static lv_obj_t *heading(const char *text)
{
    lv_obj_t *h = lv_label_create(s_body);
    lv_label_set_text(h, text);
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, LV_PCT(100));
    lv_obj_set_style_text_font(h, &lv_font_montserrat_20, 0);
    purr_theme_apply(h, PURR_TH_TEXT);
    return h;
}

static void status_line(const char *text, int good)
{
    if (s_status != NULL) {
        lv_label_set_text(s_status, text);
        lv_obj_set_style_text_color(s_status, good ? purr_theme_accent_color() : lv_color_hex(0xFFB900), 0);
    }
}

static void make_status(void)
{
    s_status = lv_label_create(s_body);
    lv_label_set_text(s_status, "");
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, LV_PCT(100));
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_12, 0);
}

static int is_admin_session(void)
{
    const purr_user_t *u = purr_login_current();
    return u != NULL && u->role == PURR_ROLE_USER_ADMIN;
}

/* User account control: screens that change the device. Open if this session is already an administrator's; otherwise the
 * administrator check comes first and, once it passes, the session stays theirs until the device restarts. The first two
 * also open when the system is damaged or has no accounts (s_open): a broken device must stay fixable. */
static void go_admin(screen_t next, int allow_open)
{
    if (is_admin_session() || (allow_open && s_open)) {
        show(next);
    } else {
        s_after = next;
        show(SC_GATE);
    }
}

static void go_cb(lv_event_t *e) { show((screen_t)(intptr_t)lv_event_get_user_data(e)); }
static void admin_cb(lv_event_t *e) { go_admin((screen_t)(intptr_t)lv_event_get_user_data(e), 1); }
static void admin_strict_cb(lv_event_t *e) { go_admin((screen_t)(intptr_t)lv_event_get_user_data(e), 0); }

static void restart_cb(lv_event_t *e)
{
    (void)e;
    xTaskCreatePinnedToCore(restart_task, "restart", 2048, NULL, 3, NULL, 0);   /* core 0: see park_app_cpu() in the bootloader */
    show(SC_WORKING);
    s_erase = -1;
    lv_label_set_text(s_stage, "Restarting...");
}

static void start_reinstall(int erase)
{
    s_erase = erase;
    show(SC_WORKING);
    if (oobe_job_start_stack(s_job, job_reinstall, (void *)(intptr_t)erase, OOBE_STACK_SMALL) != 0) {
        lv_label_set_text(s_stage, "Couldn't start that.");
    } else {
        s_pending = 1;
    }
}

static void keep_go_cb(lv_event_t *e) { (void)e; start_reinstall(0); }
static void erase_go_cb(lv_event_t *e) { (void)e; start_reinstall(1); }

static void verify_cb(lv_event_t *e)
{
    (void)e;
    snprintf(s_user, sizeof(s_user), "%s", lv_textarea_get_text(s_ta_user));
    snprintf(s_pass, sizeof(s_pass), "%s", lv_textarea_get_text(s_ta_pass));
    if (s_user[0] == '\0' || s_pass[0] == '\0') {
        status_line("Type the administrator's name and password.", 0);
        return;
    }
    status_line("Checking...", 1);
    if (oobe_job_start_stack(s_job, job_verify, NULL, OOBE_STACK_SMALL) != 0) {
        status_line("Couldn't start that.", 0);
    } else {
        s_pending = 1;
    }
}

static void home(void)
{
    heading("Hi, welcome to recovery.");
    char who[96];
    snprintf(who, sizeof(who), "Recovery mode - signed in as %.32s%s", purr_login_current()->name,
             purr_login_elevated() ? " (administrator)" : "");
    w10m_dim(s_body, who);
    const char *lines[PURR_RR_COUNT];
    int n = purr_reason_lines(s_reasons, lines, PURR_RR_COUNT);
    if (n == 0) {
        w10m_dim(s_body, "Nothing looks wrong. Pick what you'd like to do.");
    } else {
        w10m_dim(s_body, "Here's why you're here:");
        for (int i = 0; i < n; i++) {
            char buf[96];
            snprintf(buf, sizeof(buf), "- %s", lines[i]);
            w10m_dim(s_body, buf);
        }
    }
    w10m_space(s_body, 6);
    w10m_row(s_body, "Restart", "Try starting normally again", 1, restart_cb, NULL);
    w10m_row(s_body, "Reinstall PURR OS", "Keeps your accounts and files", 1, admin_cb, (void *)(intptr_t)SC_KEEP);
    w10m_row(s_body, "Reinstall and erase everything", "Starts over like a new device", 1, admin_cb,
             (void *)(intptr_t)SC_ERASE_INFO);
    w10m_row(s_body, "Security", "The bootloader's protection", 1, admin_strict_cb, (void *)(intptr_t)SC_SECURITY);
}

static void keep(void)
{
    heading("Reinstall PURR OS");
    w10m_dim(s_body, "We'll download a fresh copy of the system. Your accounts, apps and files stay right where they are.");
    w10m_dim(s_body, "It needs Wi-Fi (or a cable) and takes a few minutes. Keep it charged.");
    w10m_space(s_body, 6);
    lv_obj_t *b = w10m_button(s_body, "Reinstall", keep_go_cb, NULL);
    purr_theme_apply(b, PURR_TH_ACCENT_BG);
    w10m_button(s_body, "Back", go_cb, (void *)(intptr_t)SC_HOME);
}

static void erase_info(void)
{
    heading("Erase everything");
    w10m_dim(s_body, "This deletes every account, app and file on this device, then installs PURR OS again as if it were new. The saved Wi-Fi network is kept so it can download PURR OS by itself.");
    w10m_dim(s_body, "It can't be undone.");
    w10m_space(s_body, 6);
    lv_obj_t *b = w10m_button(s_body, "Continue", go_cb, (void *)(intptr_t)SC_ERASE_SURE);
    purr_theme_apply(b, PURR_TH_ACCENT_BG);
    w10m_button(s_body, "Back", go_cb, (void *)(intptr_t)SC_HOME);
}

static void gate(void)
{
    heading("Administrator needed");
    w10m_dim(s_body, "This changes the device, so an administrator has to say it's OK. Type their name and password. They stay signed in until the device restarts.");
    s_ta_user = w10m_input(s_body, s_group, "Administrator name", 0);
    s_ta_pass = w10m_input(s_body, s_group, "Password", 1);
    make_status();
    lv_obj_t *b = w10m_button(s_body, "Check", verify_cb, NULL);
    purr_theme_apply(b, PURR_TH_ACCENT_BG);
    w10m_button(s_body, "Back", go_cb, (void *)(intptr_t)SC_HOME);
}

static void erase_sure(void)
{
    heading("Last chance");
    w10m_dim(s_body, "Everything on this device will be erased and PURR OS reinstalled.");
    w10m_space(s_body, 6);
    lv_obj_t *b = w10m_button(s_body, "Erase everything", erase_go_cb, NULL);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xC42B1C), 0);
    w10m_button(s_body, "Back", go_cb, (void *)(intptr_t)SC_HOME);
}

static void restore_cb(lv_event_t *e)
{
    (void)e;
    s_erase = -1;
    show(SC_WORKING);
    lv_label_set_text(s_stage, "Restoring security...");
    s_pending = oobe_job_start_stack(s_job, job_restore_security, NULL, OOBE_STACK_SMALL) == 0;
}

static void weaken_start(int wipe)
{
    s_erase = wipe;
    show(SC_WORKING);
    s_pending = oobe_job_start_stack(s_job, job_weaken, (void *)(intptr_t)wipe, OOBE_STACK_SMALL) == 0;
}

static void weaken_wipe_cb(lv_event_t *e) { (void)e; weaken_start(1); }

static void weaken_keep_cb(lv_event_t *e)
{
    (void)e;
    if (!s_weaken_ok) {
        show(SC_WEAKEN_KEEP);                                  /* the switch is off: stay here */
        return;
    }
    weaken_start(0);
}

static void understand_cb(lv_event_t *e) { s_weaken_ok = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED); }

static void security(void)
{
    purr_secure_status_t st;
    purr_secure_status(&st);
    static const char *const modes[] = {"off", "warn", "enforce"};
    heading("Security");
    char b[96];
    snprintf(b, sizeof(b), "Bootloader now: %s%s", modes[st.mode_now < 3 ? st.mode_now : 1],
             st.dev_now ? " (developer mode)" : "");
    w10m_text(s_body, b);
    w10m_dim(s_body, st.signed_now ? "This device has a signed security policy." : "No signed policy: the default rules apply.");
    w10m_space(s_body, 6);
    if (st.dev_now) {
        w10m_dim(s_body, "Developer mode lets unsigned or failed images start. You can turn the full protection back on.");
        w10m_row(s_body, "Restore full security", "Takes effect after a restart", 1, restore_cb, NULL);
    } else if (purr_secure_dev_locked()) {
        w10m_dim(s_body, "This device's signed policy doesn't allow weakening the bootloader.");
    } else {
        w10m_row(s_body, "Weaken security...", "Lets unsigned system images start", 1, go_cb, (void *)(intptr_t)SC_WEAKEN);
    }
    w10m_button(s_body, "Back", go_cb, (void *)(intptr_t)SC_HOME);
}

static void weaken(void)
{
    s_weaken_ok = 0;
    heading("Weaken security?");
    w10m_dim(s_body, "The bootloader will start images that aren't signed, or that fail their check. Anyone who gets hold of the device could load their own code onto it.");
    w10m_dim(s_body, "So by default your data is erased first, and nobody can use this to get at it.");
    w10m_space(s_body, 6);
    lv_obj_t *b = w10m_button(s_body, "Erase my data and weaken", weaken_wipe_cb, NULL);
    purr_theme_apply(b, PURR_TH_ACCENT_BG);
    w10m_button(s_body, "Keep my data (risky)", go_cb, (void *)(intptr_t)SC_WEAKEN_KEEP);
    w10m_button(s_body, "Cancel", go_cb, (void *)(intptr_t)SC_SECURITY);
}

static void weaken_keep(void)
{
    heading("Keep your data?");
    w10m_dim(s_body, "It's your device and your call. But with security weakened and your data kept, someone with the device could load code that reads your accounts and files.");
    w10m_switch(s_body, "I understand the risk", s_weaken_ok, understand_cb);
    w10m_space(s_body, 6);
    lv_obj_t *b = w10m_button(s_body, "Weaken and keep my data", weaken_keep_cb, NULL);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xC42B1C), 0);
    w10m_button(s_body, "Back", go_cb, (void *)(intptr_t)SC_WEAKEN);
}

/* The strip on (the working screen) or off (every other screen). The body moves down to make room and gives it back. */
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
    lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 0, on ? STRIP_H : 0);
    lv_obj_set_height(s_body, h - (on ? STRIP_H : 0));
}

static void working(void)
{
    strip_set(1);
    s_stage = s_strip_lbl;
    s_bar = s_strip_bar;
    heading(s_erase == 1 ? "Erasing..." : "Hang on...");
    w10m_dim(s_body, "Don't switch it off.");
}

static void show(screen_t sc)
{
    s_status = s_stage = s_bar = NULL;
    strip_set(0);
    lv_obj_clean(s_body);
    lv_group_remove_all_objs(s_group);
    lv_obj_scroll_to_y(s_body, 0, LV_ANIM_OFF);
    switch (sc) {
    case SC_HOME:       home(); break;
    case SC_KEEP:       keep(); break;
    case SC_ERASE_INFO: erase_info(); break;
    case SC_GATE:       gate(); break;
    case SC_ERASE_SURE: erase_sure(); break;
    case SC_WORKING:    working(); break;
    case SC_SECURITY:   security(); break;
    case SC_WEAKEN:     weaken(); break;
    case SC_WEAKEN_KEEP: weaken_keep(); break;
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_pending) {
        return;
    }
    char msg[64] = "";
    int per = -1;
    int r = oobe_job_poll(s_job, msg, sizeof(msg), &per);
    if (r < 0) {
        if (s_bar != NULL && per >= 0) {
            lv_bar_set_value(s_bar, per, LV_ANIM_OFF);
        }
        if (s_stage != NULL && msg[0]) {
            lv_label_set_text(s_stage, msg);
        }
        return;
    }
    s_pending = 0;
    if (s_stage != NULL) {                                     /* the reinstall job */
        if (r == 0) {
            lv_bar_set_value(s_bar, 1000, LV_ANIM_OFF);
            lv_label_set_text(s_stage, "Restarting...");
            xTaskCreatePinnedToCore(restart_task, "restart", 2048, NULL, 3, NULL, 0);   /* core 0: see park_app_cpu() in the bootloader */
        } else {
            lv_label_set_text(s_stage, msg[0] ? msg : "That didn't work.");
            lv_obj_set_style_text_color(s_stage, lv_color_hex(0xFFB900), 0);
            w10m_button(s_body, "Back", go_cb, (void *)(intptr_t)SC_HOME);
        }
        return;
    }
    if (s_status != NULL) {                                    /* the password check */
        if (r == 0) {
            show(s_after);
        } else {
            status_line(msg[0] ? msg : "That's not right. Try again.", 0);
        }
    }
}

/* ---------------------------------------------------------------- bring-up */

static void use_keypad(lv_group_t *g)
{
    lv_indev_t *kb = (lv_indev_t *)purr_lvgl_keypad();
    if (kb != NULL) {
        lv_indev_set_group(kb, g);
    }
}

static void *build_cb(void *arg)
{
    (void)arg;
    purr_theme_init();
    int w = lv_display_get_horizontal_resolution(NULL), h = lv_display_get_vertical_resolution(NULL);
    s_group = lv_group_create();
    use_keypad(s_group);
    s_sheet = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_sheet);
    purr_theme_apply(s_sheet, PURR_TH_SHEET);
    lv_obj_set_size(s_sheet, w, h);
    lv_obj_clear_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);
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
    lv_obj_set_style_bg_color(s_strip_bar, lv_color_hex(0x3A3A3A), 0);
    lv_obj_set_style_bg_color(s_strip_bar, purr_theme_accent_color(), LV_PART_INDICATOR);
    lv_obj_align(s_strip_bar, LV_ALIGN_TOP_LEFT, 0, STRIP_H - 2);
    lv_obj_add_flag(s_strip_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_strip_bar, LV_OBJ_FLAG_HIDDEN);
    s_body = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, w, h);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_body, 4, 0);
    lv_obj_set_style_pad_hor(s_body, 12, 0);
    lv_obj_set_style_pad_top(s_body, 10, 0);
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
    s_timer = lv_timer_create(tick_cb, 100, NULL);
    show(SC_HOME);
    return NULL;
}

int recovery_app_start(void)
{
    purr_handoff_t hand;
    s_reasons = purr_kernel_handoff(&hand) == 0 ? hand.recovery_reasons : 0;
    s_job = oobe_job_new();
    /* Nobody left to ask (no accounts), or the system is too damaged to trust: erasing needs no password. */
    s_open = recovery_gate_open_for(s_reasons) || !purr_login_has_accounts();
    purr_login_kitten();                    /* recovery signs in as kitten: a user, never nobody */
    ESP_LOGI(TAG, "recovery app: reasons 0x%x, erase gate %s; internal RAM free %u, largest block %u", (unsigned)s_reasons,
             s_open ? "open" : "closed", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return purr_ui_service_run(build_cb, NULL, NULL) == ESP_OK ? 0 : -1;
}

#else

int recovery_app_start(void) { return -1; }

#endif
