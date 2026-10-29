/*
 * PURR OS boot package: the boot menu (bootloader/SPEC.md section 9). The look is pkg_ui.c: one frame (mark,
 * wordmark, status chips) and a bottom band that changes; here the band is the entry list and a draining bar.
 *
 * The menu rules are PurrOS/components/coreos/src/purr_menu.c, shared with the host
 * tests. This file draws it and feeds it keys and time.
 *
 * Version 0.3: on the keyboard boards the menu is only up when it is needed (the bootloader decides, and says why in the services: held Space, a start
 * that failed, a system that did not verify, or Settings asking). A normal start is the splash (SPLASH, then KEYCHECK for the menu key) and no menu at all.
 */
#include <stdint.h>

#include "board.h"
#include "pkg_hw.h"
#include "pkg_ui.h"
#include "purr_abi.h"
#include "purr_menu.h"

#define TICK_MS 50
#define Y_ABOVE_TEXT 188

/* No snprintf here: these build the few strings the menu shows. */
static void put_str(char *dst, int *n, const char *s)
{
    while (*s && *n < 47) dst[(*n)++] = *s++;
    dst[*n] = '\0';
}

/* The display is brought up by whichever call comes first (a progress call from the bootloader while it verifies,
 * or the menu), and the frame is drawn once: the menu then continues on the same screen without clearing it. */
static int s_up, s_frame;

static int bring_up(const purr_boot_services_t *svc)
{
    if (!s_up) {
        hw_init(svc);
        if (hw_display_init() != 0) {
            return 0;
        }
        s_up = 1;
#ifdef BOARD_CODENAME
        svc->log("bootpkg: " BOARD_CODENAME " (" BOARD_NAME ")");
#endif
        svc->log("bootpkg: display up");
        svc->log(hw_secure_boot_enabled() ? "bootpkg: secure boot HW = ON" : "bootpkg: secure boot HW = off");
    }
    return 1;
}

#if BOARD_HAS_KEYBOARD
static void put_num(char *dst, int *n, int v)
{
    char t[6];
    int k = 0;
    if (v <= 0) t[k++] = '0';
    while (v > 0 && k < 5) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k > 0 && *n < 47) dst[(*n)++] = t[--k];
    dst[*n] = '\0';
}
#endif

/* The status chips under the wordmark, in priority order (pkg_ui_chips drops the last ones that do not fit):
 * the development-mode mark if it applies, the policy the bootloader is enforcing and whether its trust block is
 * signed, the chip's hardware Secure Boot (only when it is on), and the input. */
static int build_chips(const purr_boot_services_t *svc, ui_chip_t *c)
{
    static const char *const modes[] = {"OFF", "WARN", "ENFORCE", "WARN"};
    int n = 0;
    if (svc->version >= 2) {
        uint32_t s = svc->secure_state;
        if (s & PURR_SECST_DEV) {
            c[n].label = "DEV MODE"; c[n++].kind = UI_CHIP_HOT;
        }
        c[n].label = modes[PURR_SECST_MODE(s)];
        c[n++].kind = PURR_SECST_MODE(s) == 2 ? UI_CHIP_OK : UI_CHIP_WARN;
        if (!(s & PURR_SECST_DEV)) {
            c[n].label = (s & PURR_SECST_SIGNED) ? "SIGNED" : "UNSIGNED";
            c[n++].kind = (s & PURR_SECST_SIGNED) ? UI_CHIP_OK : UI_CHIP_WARN;
        }
    }
    if (hw_secure_boot_enabled()) {
        c[n].label = "HW SB on"; c[n++].kind = UI_CHIP_OK;
    }
#if BOARD_HAS_KEYBOARD
    c[n].label = hw_kbd_ok() ? "kbd ok" : "NO KBD";
    c[n++].kind = hw_kbd_ok() ? UI_CHIP_OK : UI_CHIP_WARN;
#else
    c[n].label = "BOOT / PWR"; c[n++].kind = UI_CHIP_PLAIN;
#endif
    return n;
}

#if !BOARD_HAS_KEYBOARD          /* the e-ink board keeps the original menu; the keyboard boards use kbd_menu() below */
typedef struct {
    const purr_menu_part_t *mp;
    int preferred;
} menu_view_t;

/* The entry rows (or the message when there is nothing to show). */
static void draw_band(const purr_menu_t *m, const menu_view_t *v)
{
    ui_clear_band();
    if (!purr_menu_visible(m)) {
        ui_row(0, "No system found...", "", 0, 0);
        return;
    }
    for (int i = 0; i < m->count; i++) {
        int part = m->entries[i].part;
        int usable = part < 0 || v->mp[part].bootable;
        const char *tag = part == v->preferred && part >= 0 ? "default" : (!usable ? "unusable" : "");
        ui_row(i, m->entries[i].label, tag, i == m->selected, usable);
    }
    if (m->no_boot_options) {
        ui_row(m->count, "Nothing to boot.", "", 0, 0);
    }
}

/* How much of the countdown is left, 0..1000; 0 once a key has stopped it. */
static int countdown_per_mille(const purr_menu_t *m)
{
    if (!m->counting || m->elapsed_ms >= PURR_MENU_TIMEOUT_MS) {
        return 0;
    }
    return (int)((PURR_MENU_TIMEOUT_MS - m->elapsed_ms) * 1000u / PURR_MENU_TIMEOUT_MS);
}

static void draw_footer(const purr_menu_t *m)
{
    ui_bar(countdown_per_mille(m));
    char foot[48];
    int n = 0;
    foot[0] = '\0';
    int s = purr_menu_seconds_left(m);
    if (s >= 0) {
        char d[2] = {(char)('0' + (s % 10)), 0};
        put_str(foot, &n, "Booting in ");
        put_str(foot, &n, d);
        put_str(foot, &n, "...");
        ui_above(foot, "any key stops");
    } else {
#if BOARD_HAS_KEYBOARD
        ui_above("W up   S down   D select", 0);
#else
        ui_above("BOOT=down  PWR=up", 0);
#endif
    }
    hw_flush();
}

#endif

/* A progress call from the bootloader: "Verifying <slot>" and how far along. */
static void draw_progress(const purr_boot_services_t *svc, const char *label, int per_mille)
{
#if BOARD_HAS_KEYBOARD                      /* a slow e-ink panel would spend seconds on every refresh */
    if (!bring_up(svc)) {
        return;
    }
    ui_chip_t chips[5];
    if (!s_frame) {
        ui_frame();
        s_frame = 1;
    }
    char stage[48], pct[8];
    int n = 0, m = 0;
    if (per_mille >= 1000) {
        put_str(stage, &n, "Verified");
    } else {
        put_str(stage, &n, "Verifying ");
        put_str(stage, &n, label);
    }
    put_num(pct, &m, per_mille / 10);
    put_str(pct, &m, "%");
    ui_above(stage, pct);
    ui_bar(per_mille);
    ui_chips(chips, build_chips(svc, chips));
    hw_flush();
#else
    (void)svc; (void)label; (void)per_mille;
#endif
}

#if !BOARD_HAS_KEYBOARD
/* The last screen before the bootloader takes over: the same frame, the bar full. */
static void draw_handoff(const purr_boot_services_t *svc, const char *what)
{
    ui_chip_t chips[5];
    char stage[48];
    int n = 0;
    put_str(stage, &n, what);
    ui_splash(chips, build_chips(svc, chips), stage, "100%", 1000);
}

#endif

#if BOARD_HAS_KEYBOARD

/* The normal-start screen: logo, version, the stage line, the chips. No bar. */
static int stage_splash(const purr_boot_services_t *svc, const char *what)
{
    if (!bring_up(svc)) {
        return 0;
    }
    ui_chip_t chips[5];
    ui_frame();
    s_frame = 1;
    ui_centred(Y_ABOVE_TEXT, what, UI_WHITE);
    ui_chips(chips, build_chips(svc, chips));
    hw_flush();
    return 0;
}

/* Watches the keyboard for `ms`; 1 if Space was pressed. Anything already waiting in the controller counts (it was pressed while the system was
 * being checked). */
static int stage_keycheck(const purr_boot_services_t *svc, int ms)
{
    if (!bring_up(svc)) {
        return 0;
    }
    for (int t = 0; t <= ms; t += 10) {
        char c;
        while ((c = hw_key()) != 0) {
            if (c == ' ') {
                return 1;
            }
        }
        hw_delay_ms(10);
    }
    return 0;
}

static const char *display_name(const char *label, int problem)
{
    if (label[0] == 'K' && label[1] == 'e') {                   /* "Kernel" */
        return "PURR OS";
    }
    if (problem && label[0] == 'K' && label[1] == 'i') {        /* "KittenOS" */
        return "Start recovery";
    }
    return label;
}

static int is_kittenos(const char *label) { return label[0] == 'K' && label[1] == 'i'; }

typedef struct {
    int problem;                      /* the system did not verify: heading in orange, the reason in the description */
    int rows_y;
} kview_t;

static void put_secs(char *dst, int *n, int s)
{
    char t[4];
    int k = 0;
    if (s <= 0) t[k++] = '0';
    while (s > 0 && k < 3) { t[k++] = (char)('0' + s % 10); s /= 10; }
    while (k > 0 && *n < 47) dst[(*n)++] = t[--k];
    dst[*n] = '\0';
}

static void kbd_draw_rows(const purr_menu_t *m, const kview_t *v, const purr_menu_part_t *mp, int nparts)
{
    ui_clear_rect(v->rows_y, 24 * 4);
    int row = 0;
    for (int i = 0; i < m->count; i++, row++) {
        const char *lab = m->entries[i].part >= 0 ? m->entries[i].label : (m->entries[i].part == -2 ? "Restart" : "Internet recovery");
        ui_menu_row(v->rows_y + row * 24, display_name(lab, v->problem), i == m->default_sel ? "default" : "", 1, i == m->selected, 1);
    }
    for (int p = 0; p < nparts && row < 4; p++) {                     /* what is not bootable, listed but never selectable */
        if (!mp[p].bootable) {
            ui_menu_row(v->rows_y + row * 24, display_name(mp[p].label, v->problem), "unusable", 0, 0, 0);
            row++;
        }
    }
}

static void kbd_draw_desc(const purr_boot_services_t *svc, const purr_menu_t *m, const kview_t *v)
{
    ui_clear_rect(155, 24);
    const char *l1 = 0, *l2 = 0;
    if (v->problem) {
        l1 = svc->detail1;
        l2 = svc->detail2;
    } else if (m->entries[m->selected].part == -2) {
        l1 = "Restarts the device.";
    } else if (m->entries[m->selected].part >= 0) {
        const char *lab = m->entries[m->selected].label;
        if (is_kittenos(lab)) {
            l1 = "Repairs or reinstalls PURR OS.";
            l2 = "Your files are kept.";
        } else if (lab[0] == 'K') {
            l1 = "Starts PURR OS normally.";
        }
    }
    if (l1) ui_left(155, l1, UI_GREY);
    if (l2) ui_left(166, l2, UI_GREY);
}

static void kbd_draw_foot(const purr_boot_services_t *svc, const purr_menu_t *m, const kview_t *v)
{
    ui_clear_rect(189, 24);
    ui_centred(189, "W up   S down   D select", UI_GREY);
    int s = purr_menu_seconds_left(m);
    if (s >= 0) {
        char line[48];
        int n = 0;
        put_str(line, &n, display_name(m->entries[m->default_sel].label, v->problem));
        put_str(line, &n, " starts in ");
        put_secs(line, &n, s);
        put_str(line, &n, " s");
        ui_centred(200, line, UI_GREY);
    }
    (void)svc;
}

static int kbd_menu(const purr_boot_services_t *svc, const purr_boot_part_t *parts, int n, int preferred)
{
    purr_menu_part_t mp[PURR_PKG_MAX_PARTS];
    for (int i = 0; i < n; i++) {
        int j = 0;
        for (; j < 15 && parts[i].name[j]; j++) mp[i].label[j] = parts[i].name[j];
        mp[i].label[j] = '\0';
        mp[i].bootable = parts[i].bootable;
    }
    int reason = svc->version >= 4 ? (int)svc->menu_reason : PURR_MENU_WHY_KEY;
    kview_t v = {reason == PURR_MENU_WHY_NO_VERIFY, 0};
    v.rows_y = v.problem ? 68 : 90;

    purr_menu_t m;
    purr_menu_init(&m, mp, n, preferred);
    if (v.problem) {
        purr_menu_add_restart(&m);
    }
    ui_chip_t chips[5];
    ui_frame_small();
    s_frame = 1;
    if (v.problem) {
        ui_left(46, "PURR OS did not pass its check.", UI_ORANGE);
    } else {
        ui_left(46, reason == PURR_MENU_WHY_LAST_FAILED ? "PURR OS did not start last time." : "Boot menu", UI_WHITE);
        ui_left(66, "Pick what to start.", UI_GREY);
    }
    ui_rule(147);
    ui_chips(chips, build_chips(svc, chips));
    hw_key();
    kbd_draw_rows(&m, &v, mp, n);
    kbd_draw_desc(svc, &m, &v);
    kbd_draw_foot(svc, &m, &v);
    hw_flush();

    int last_sel = m.selected, last_secs = purr_menu_seconds_left(&m), last_kbd = hw_kbd_ok();
    purr_menu_result_t r = {PURR_MENU_ACT_NONE, -1};
    while (r.action == PURR_MENU_ACT_NONE) {
        hw_delay_ms(TICK_MS);
        hw_tick(TICK_MS);
        r = purr_menu_step(&m, TICK_MS, purr_key_from_char(hw_key()));
        if (hw_kbd_ok() != last_kbd) {
            last_kbd = hw_kbd_ok();
            ui_chips(chips, build_chips(svc, chips));
        }
        if (m.selected != last_sel) {
            kbd_draw_rows(&m, &v, mp, n);
            kbd_draw_desc(svc, &m, &v);
        }
        int secs = purr_menu_seconds_left(&m);
        if (secs != last_secs || m.selected != last_sel) {
            kbd_draw_foot(svc, &m, &v);
        }
        last_sel = m.selected;
        last_secs = secs;
    }

    if (r.action == PURR_MENU_ACT_RESTART) {
        svc->log("bootpkg: restart chosen");
        return PURR_PKG_CHOICE_RESTART;
    }
    if (r.action == PURR_MENU_ACT_BOOT) {
        svc->log("bootpkg: booting the chosen slot");
        stage_splash(svc, is_kittenos(mp[r.index].label) ? "Starting KittenOS" : "Starting PURR OS");
        return r.index;
    }
    svc->log("bootpkg: starting internet recovery");
    stage_splash(svc, "Starting recovery loader");
    return PURR_PKG_CHOICE_INTERNET_RECOVERY;
}

#endif /* BOARD_HAS_KEYBOARD */

int purr_pkg_entry(const purr_boot_services_t *svc, const purr_boot_part_t *parts,
                   int nparts, int preferred)
{
    if (nparts < 0) {                       /* a stage call (purr_abi.h); `parts` is the label or the stage text */
#if BOARD_HAS_KEYBOARD
        if (-nparts == PURR_PKG_STAGE_SPLASH) {
            return stage_splash(svc, (const char *)parts);
        }
        if (-nparts == PURR_PKG_STAGE_KEYCHECK) {
            return stage_keycheck(svc, preferred);
        }
#endif
        if (-nparts == PURR_PKG_STAGE_VERIFY) {
            draw_progress(svc, (const char *)parts, preferred);
        }
        return 0;
    }
    if (!bring_up(svc)) {
        return -1;
    }
#if BOARD_HAS_KEYBOARD
    return kbd_menu(svc, parts, nparts > PURR_PKG_MAX_PARTS ? PURR_PKG_MAX_PARTS : nparts, preferred);
#else

    for (;;) {
        purr_menu_part_t mp[PURR_PKG_MAX_PARTS];
        int n = nparts > PURR_PKG_MAX_PARTS ? PURR_PKG_MAX_PARTS : nparts;
        for (int i = 0; i < n; i++) {
            int j = 0;
            for (; j < 15 && parts[i].name[j]; j++) mp[i].label[j] = parts[i].name[j];
            mp[i].label[j] = '\0';
            mp[i].bootable = parts[i].bootable;
        }

        purr_menu_t m;
        purr_menu_init(&m, mp, n, preferred);
        menu_view_t view = {mp, preferred};
        ui_chip_t chips[5];
        hw_key();
        if (!s_frame) {
            ui_frame();
            s_frame = 1;
        }
        ui_chips(chips, build_chips(svc, chips));
        draw_band(&m, &view);
        draw_footer(&m);

        int last_sel = m.selected, last_secs = purr_menu_seconds_left(&m);
        int last_vis = purr_menu_visible(&m);
        int last_kbd = hw_kbd_ok();
        int last_step = countdown_per_mille(&m) * UI_BAR_STEPS / 1000;
        purr_menu_result_t r = {PURR_MENU_ACT_NONE, -1};
        while (r.action == PURR_MENU_ACT_NONE) {
            hw_delay_ms(TICK_MS);
            hw_tick(TICK_MS);
            r = purr_menu_step(&m, TICK_MS, purr_key_from_char(hw_key()));
            if (hw_kbd_ok() != last_kbd) {
                last_kbd = hw_kbd_ok();
                ui_chips(chips, build_chips(svc, chips));       /* the keyboard chip changed */
                hw_flush();
            }
            int secs = purr_menu_seconds_left(&m), vis = purr_menu_visible(&m);
            int step = countdown_per_mille(&m) * UI_BAR_STEPS / 1000;
            if (m.selected != last_sel || vis != last_vis) {
                draw_band(&m, &view);
            }
            if (secs != last_secs || vis != last_vis || m.selected != last_sel || step != last_step) {
                draw_footer(&m);
            }
            last_sel = m.selected;
            last_secs = secs;
            last_vis = vis;
            last_step = step;
        }

        if (r.action == PURR_MENU_ACT_BOOT) {
            svc->log("bootpkg: booting the chosen slot");
            char what[48];
            int w = 0;
            put_str(what, &w, "Starting ");
            put_str(what, &w, mp[r.index].label);
            draw_handoff(svc, what);
            return r.index;
        }
        svc->log("bootpkg: starting internet recovery");
        draw_handoff(svc, "Starting recovery loader");
        return PURR_PKG_CHOICE_INTERNET_RECOVERY;
    }
#endif /* BOARD_HAS_KEYBOARD */
}
