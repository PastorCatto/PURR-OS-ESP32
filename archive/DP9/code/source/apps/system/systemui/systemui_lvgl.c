// systemui_lvgl.c — chrome for the graphical session: one combined
// status/nav bar along the BOTTOM of the screen (clock bottom-left,
// battery bottom-right, a tap-to-lock control in the middle), drawn once
// on lv_layer_top() (NOT the default screen — see below for why that
// split matters) and refreshed periodically via the optional claw_
// personal_tick() entry point (claw_loader.h's own comment on why that
// exists).
//
// Status bar and nav bar were originally going to be two separate
// concepts (a top status strip, a bottom nav strip) — collapsed into one
// bottom bar per direction after the first hardware round: T-Deck Plus's
// screen is small enough that two strips ate real content space for no
// real benefit yet, and every affordance this pass actually has (clock,
// battery, lock) fits one bar.
//
// ── No wrapping "bar" container — cross-checked against archive/ ────────
// ui_backends_v1/modules/systemui/systemui_ios.c and .../systemui_
// android.c after the SECOND hardware round still looked wrong (this had
// wrapped everything in one lv_obj_create() panel with a "Lock" chip
// nested inside it — two DEFAULT-themed containers, each with their own
// unstyled border/background, nested inside each other, on top of the
// scrollbar bug the first round already had). Both archived status bars
// do the opposite: s_status_clock/s_status_batt (systemui_ios.c:675-701)
// are BARE lv_label_create(lv_layer_top()) calls with no wrapping
// container at all — a label has no background/border of its own to look
// wrong. The only thing in either archived file that DOES wrap itself in
// a real lv_obj_create() is something genuinely interactive with its own
// visible affordance (mochi_springboard.c's "dock"/"home button"), and
// even the home button (mochi_springboard.c:582-600) sets its own
// background fully TRANSPARENT (lv_obj_set_style_bg_opa(..., LV_OPA_
// TRANSP, 0)) rather than accepting the default theme's box look — the
// tap target is invisible, only a child "pill" or label is ever actually
// drawn. This file now follows both lessons: clock/battery are bare
// labels, and the lock control is a fully transparent tap target whose
// only visible content is its own "Lock" label — no boxes, nested or
// otherwise, anywhere in this file any more.
//
// ── Real bug this pass also fixes: bleeding into speed-demon mode ───────
// The FIRST hardware round showed the status bar rendering on top of a
// speed-demon game's own full-screen framebuffer pushes — confirmed live,
// not a design guess. Root cause: speed_demon.c's teardown (source/
// modules/speed_demon/) was written against the OLD static-module UI
// backend — it walks and unloads the P0-P3 registry by name, so it has
// no idea a claw-LOADED systemui package even exists, and kernel_tdp_
// boot.c's session loop kept calling lv_tick_inc()/lv_timer_handler()
// (which re-flushes any invalidated area — this package's own clock/
// battery tick invalidates a small region once a second) straight through
// the game's own drawing. Fixed at the host, not here: run_graphical_
// session() now polls purr_speed_demon_active() and stops touching the
// display at all while a game holds it — see kernel_tdp_boot.c's own
// comment on that loop. This file needed NO changes for that part; it's
// listed here so the connection between the two files' comments is
// findable from either side.
//
// ── Ownership split ────────────────────────────────────────────────────
// This package does NOT own a forever-loop, unlike loginUI (which blocks
// inside its own init() until a real login succeeds) and unlike the
// launcher used to before this package existed. kernel_tdp_boot.c's own
// session loop is the one place that now calls lv_tick_inc()/
// lv_timer_handler() each frame, and calls THIS package's tick() (if
// non-NULL) and the launcher's own tick() (currently NULL — it has
// nothing per-frame to do) right alongside it. Two loaded modules each
// trying to own the same LVGL forever-loop was never going to work — only
// one task can safely call lv_timer_handler() — so the host is the one
// true owner, and every loaded UI package becomes a thinner "create
// widgets, then get ticked" participant instead. See kernel_tdp_boot.c's
// own comment on the session loop for the concrete call sequence.
//
// ── Why lv_layer_top(), not the default screen ────────────────────────
// The launcher's own claw_personal_init() calls lv_obj_clean() on the
// default screen every time it (re)builds the tile grid — including
// after a relock cycle, when loginUI runs again and leaves its own
// widgets on the default screen for claw_personal_init() to clear. A
// status bar drawn on the DEFAULT screen would get wiped by that same
// lv_obj_clean() call the moment the launcher rebuilds — LVGL's layer_top
// is a separate object tree that always composites OVER the active
// screen and is never touched by lv_scr_load()/an lv_obj_clean() on
// scr_act, so this survives every screen swap for free, with no
// coordination between the two packages needed at all.
//
// ── Lock control ────────────────────────────────────────────────────────
// Tapping the "Lock" chip calls purr_kernel_request_lock() — see
// purr_kernel.h's own comment on why that's a plain flag rather than a
// callback: a loaded module has no way to call back into host code except
// by name through the import table, and the host's session loop already
// polls this exact flag once per frame (purr_kernel_consume_lock_request())
// to decide when to tear down the launcher and run loginUI again. This
// file never calls claw_loader_system_load() itself, and never touches
// claw_loaded_module_t at all — that whole relock cycle is the host's
// job, not this package's; see kernel_tdp_boot.c for why (mirroring that
// struct here would mean hand-declaring void*/uint32_t fields whose real
// layout only claw_loader.h actually owns — the exact struct-mirroring
// anti-pattern this session's own purr_kernel_poll_key() was written to
// get away from for input, kept away from here too).
#if defined(SYSCLAW_BACKEND_LVGL)

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct lv_obj_t   lv_obj_t;
typedef struct lv_event_t lv_event_t;
typedef int16_t lv_coord_t;   // confirmed against lv_conf.h, same as launcher_lvgl.c's own comment
typedef int     lv_event_code_t;

extern lv_obj_t  *lv_layer_top(void);
extern lv_obj_t  *lv_obj_create(lv_obj_t *parent);
extern void       lv_obj_set_pos(lv_obj_t *obj, lv_coord_t x, lv_coord_t y);
extern void       lv_obj_set_size(lv_obj_t *obj, lv_coord_t w, lv_coord_t h);
extern lv_obj_t  *lv_label_create(lv_obj_t *parent);
extern void       lv_label_set_text(lv_obj_t *obj, const char *text);
extern void      *lv_obj_add_event_cb(lv_obj_t *obj, void (*event_cb)(lv_event_t *e),
                                       lv_event_code_t filter, void *user_data);
extern void       lv_obj_clear_flag(lv_obj_t *obj, uint32_t f);
extern void       lv_obj_set_style_bg_opa(lv_obj_t *obj, uint8_t value, uint32_t selector);

// Same confirmed-against-lv_event.h value launcher_lvgl.c's own comment
// documents (LV_EVENT_CLICKED == 7) — see that file for the full
// enum-ordering note; not re-derived here.
#define LV_EVENT_CLICKED_VALUE 7

// Confirmed against CoreOS/managed_components/lvgl__lvgl/src/core/
// lv_obj.h's own anonymous flag enum (lv_obj_flag_t is a plain uint32_t
// bitmask there, LV_OBJ_FLAG_SCROLLABLE = (1L << 4)) — not assumed, same
// "confirmed, not assumed" discipline as LV_EVENT_CLICKED_VALUE above.
// Found live: a plain lv_obj_create() is scrollable BY DEFAULT in this
// LVGL build, which is harmless for the launcher's tiles (nothing sizes a
// tile's content bigger than the tile itself) but very visible here — the
// bottom bar and lock chip both showed real scrollbars and could actually
// be dragged in every direction, because nothing had told either
// container it wasn't meant to scroll.
#define LV_OBJ_FLAG_SCROLLABLE_VALUE (1UL << 4)

// Confirmed against lv_color.h's own lv_opa_t (plain uint8_t) and its
// LV_OPA_TRANSP/LV_OPA_COVER enum values (0 and 255) — plain scalars, no
// lv_color_t struct-by-value risk anywhere in this file (deliberately:
// lv_color_t's real layout depends on CONFIG_LV_COLOR_DEPTH, and this
// package never needed to touch color at all once the lock control's
// background is just made transparent instead of colored — see this
// file's own top comment on why).
#define LV_OPA_TRANSP_VALUE 0

extern uint64_t purr_kernel_uptime_ms(void);
extern int      purr_kernel_battery_percent(void);   // -1 = unknown
extern void     purr_kernel_time_hhmm(char *out, size_t out_sz);
extern void     purr_kernel_request_lock(void);

// Fixed T-Deck Plus resolution, same "no lv_disp_get_hor_res()/_ver_res()
// in the import list yet" tradeoff launcher_lvgl.c's own grid geometry
// comment already makes.
#define SCREEN_W 320
#define SCREEN_H 240
#define BAR_H    20   // reserved bottom-row height clock/lock/battery all sit within

#define CLOCK_LABEL_MAX  6    // "HH:MM\0"
#define BATT_LABEL_MAX   8    // "100%\0" plus slack

static lv_obj_t *s_clock_label = NULL;
static lv_obj_t *s_batt_label  = NULL;

static void lock_tap_cb(lv_event_t *e)
{
    (void)e;
    purr_kernel_request_lock();
}

// Forward-declared so claw_personal_init() below can call it directly for
// an immediate first draw — defined further down, next to the periodic
// call site's own doc comment.
void claw_personal_tick(void);

int claw_personal_init(void)
{
    lv_obj_t *top = lv_layer_top();
    if (!top) return -1;   // no display registered yet — same clean failure every backend uses

    // Clock — bottom-left. Bare label straight on lv_layer_top(), no
    // wrapping container — see this file's own top comment on why.
    s_clock_label = lv_label_create(top);
    lv_obj_set_pos(s_clock_label, 4, SCREEN_H - BAR_H + 2);

    // Lock control — centered, bottom row. A plain lv_obj_create() is
    // clickable by default in this LVGL build (confirmed live already by
    // launcher's own tiles), so this still needs to BE a real object to
    // have something to attach a click callback to — it just never gets
    // to look like a box: background fully transparent (mochi's own home-
    // button pattern), so the only thing actually visible is its "Lock"
    // child label.
    lv_obj_t *lock_hit = lv_obj_create(top);
    lv_obj_set_pos(lock_hit, (SCREEN_W - 44) / 2, SCREEN_H - BAR_H);
    lv_obj_set_size(lock_hit, 44, BAR_H);
    lv_obj_clear_flag(lock_hit, LV_OBJ_FLAG_SCROLLABLE_VALUE);
    lv_obj_set_style_bg_opa(lock_hit, LV_OPA_TRANSP_VALUE, 0);
    lv_obj_add_event_cb(lock_hit, lock_tap_cb, LV_EVENT_CLICKED_VALUE, NULL);
    lv_obj_t *lock_label = lv_label_create(lock_hit);
    lv_label_set_text(lock_label, "Lock");
    lv_obj_set_pos(lock_label, 2, 2);

    // Battery — bottom-right. Fixed x rather than a real right-align (no
    // lv_obj_align/text-width query in the import list — see this
    // package's own "deliberately basic" framing) — close enough for
    // "100%"/"?" both to land near the corner without the ideal amount
    // of right padding matching exactly.
    s_batt_label = lv_label_create(top);
    lv_obj_set_pos(s_batt_label, SCREEN_W - 36, SCREEN_H - BAR_H + 2);

    // Draw an initial reading immediately rather than waiting up to
    // CLOCK_REFRESH_MS for the first tick() call — a status bar that's
    // blank until the first refresh window elapses would look broken on
    // a fresh unlock even though it's actually fine.
    claw_personal_tick();

    return 0;
}

// Called once per host frame (see this file's own top comment on why the
// host, not this package, drives the loop) — throttles the actual label
// updates to roughly once a second internally rather than reformatting
// strings 30+ times a second for a value that only changes once a minute
// (clock) or occasionally (battery).
#define CLOCK_REFRESH_MS 1000

void claw_personal_tick(void)
{
    static uint64_t s_last_refresh_ms = 0;
    uint64_t now = purr_kernel_uptime_ms();
    if (s_last_refresh_ms != 0 && (now - s_last_refresh_ms) < CLOCK_REFRESH_MS) return;
    s_last_refresh_ms = now;

    if (s_clock_label) {
        char buf[CLOCK_LABEL_MAX];
        purr_kernel_time_hhmm(buf, sizeof(buf));
        lv_label_set_text(s_clock_label, buf);
    }
    if (s_batt_label) {
        int pct = purr_kernel_battery_percent();
        char buf[BATT_LABEL_MAX];
        if (pct < 0) {
            buf[0] = '?'; buf[1] = '\0';
        } else {
            // Plain itoa-by-hand — snprintf's %d is already proven to
            // resolve fine here (login_core.c uses it), but this stays
            // consistent with "as few imports as this file actually
            // needs" rather than pulling snprintf in for one integer.
            int n = pct;
            int i = 0;
            char tmp[4];
            if (n == 0) { tmp[i++] = '0'; }
            while (n > 0 && i < 3) { tmp[i++] = (char)('0' + (n % 10)); n /= 10; }
            int j = 0;
            while (i > 0) { buf[j++] = tmp[--i]; }
            buf[j++] = '%';
            buf[j] = '\0';
        }
        lv_label_set_text(s_batt_label, buf);
    }
}

void claw_personal_deinit(void)
{
    // Never actually called today — this package is loaded once for the
    // whole graphical session and outlives every launcher/loginUI relock
    // cycle (see kernel_tdp_boot.c's session loop). Kept for the same
    // "every loaded module has both entry points" contract claw_loader_
    // load() requires either way, same as launcher_lvgl.c's own no-op.
}

#endif // SYSCLAW_BACKEND_LVGL
