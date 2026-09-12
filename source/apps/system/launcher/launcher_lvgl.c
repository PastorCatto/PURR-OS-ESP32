// launcher_lvgl.c — the home screen: a basic LVGL tile grid talking to
// app_manager (local and remote — see below), loaded once loginUI's
// claw_personal_init() has already succeeded (app_manager_notify_
// unlocked() already fired inside login_core.c). LVGL-only (app.pcat's
// `variants = "lvgl"`) — this is a tap-driven grid, not something a plain
// framebuffer/keyboard UI could meaningfully be.
//
// Deliberately basic — "a bunch of windows phone squares", nothing more
// yet: no nav bar. No scrolling (fits exactly as many tiles as one screen
// holds; a device with more apps than that just doesn't show the rest
// yet). No window management for a launched app to actually draw into —
// app_manager_launch_idx() below DOES really launch the app (spawns its
// task, app_entry_t.state goes RUNNING, all real, not a stub), but
// nothing here registers a catcall_ui_t for it to create a window against
// yet. That's real, honest scope — a window manager is systemUI's job,
// not this pass's.
//
// Status bar + lock screen now live in the separate systemui package
// (source/apps/system/systemui/) — genuinely separate, not just "not
// bundled in here": this file never calls into it, never links against
// it, and has no idea it exists. The two packages only ever meet inside
// kernel_tdp_boot.c's own session loop, which loads both and ticks
// whichever ones export claw_personal_tick(). This file does NOT export
// one — see claw_personal_init()'s own comment on why it returns instead
// of looping forever now.
//
// Local vs remote is NOT this file's concern at all: app_manager_count()/
// _entry_name()/_launch_idx() already dispatch on app_manager's own
// s_remote_mode internally (see app_manager.c's own app_manager_count()),
// so this grid renders and launches identically either way — "talks to
// app manager (both remote and local)" falls out of using the ordinary,
// already-dual-mode API rather than needing two code paths here.
//
// Display/touch-indev setup is NOT here — kernel_tdp_boot.c's shared
// lvgl_hw_init() (gated on CONFIG_PURR_LOGIN_UI_LVGL) already did that
// before this package was ever loaded, same split login_render_lvgl.c's
// own top comment explains for loginUI. This file only ever creates
// WIDGETS and attaches click callbacks on the already-set-up default
// screen — every declaration below is an OPAQUE pointer type, no LVGL
// struct mirroring at all (see purrstrap.py's _CLAW_IMPORT_LVGL_
// ESSENTIALS for the exact, deliberately short import list this uses).
#if defined(SYSCLAW_BACKEND_LVGL)

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct lv_obj_t   lv_obj_t;
typedef struct lv_disp_t  lv_disp_t;
typedef struct lv_event_t lv_event_t;
typedef int16_t lv_coord_t;   // LV_USE_LARGE_COORD is off in this project's lv_conf.h — confirmed, not assumed
typedef int     lv_event_code_t;

extern lv_disp_t *lv_disp_get_default(void);
extern lv_obj_t  *lv_disp_get_scr_act(lv_disp_t *disp);
extern lv_obj_t  *lv_obj_create(lv_obj_t *parent);
extern void       lv_obj_set_pos(lv_obj_t *obj, lv_coord_t x, lv_coord_t y);
extern void       lv_obj_set_size(lv_obj_t *obj, lv_coord_t w, lv_coord_t h);
extern lv_obj_t  *lv_label_create(lv_obj_t *parent);
extern void       lv_label_set_text(lv_obj_t *obj, const char *text);
extern void      *lv_obj_add_event_cb(lv_obj_t *obj, void (*event_cb)(lv_event_t *e),
                                       lv_event_code_t filter, void *user_data);
extern void      *lv_event_get_user_data(lv_event_t *e);
extern void       lv_obj_clean(lv_obj_t *obj);

// LV_EVENT_CLICKED's real numeric value — confirmed against CoreOS/
// managed_components/lvgl__lvgl/src/core/lv_event.h's own lv_event_code_t
// enum (LV_EVENT_ALL=0, PRESSED, PRESSING, PRESS_LOST, SHORT_CLICKED,
// LONG_PRESSED, LONG_PRESSED_REPEAT, then CLICKED=7) before writing this,
// not assumed — same "confirmed, not assumed" discipline login_render_
// lvgl.c's own lv_coord_t comment already uses. A future LVGL point
// release that reorders this enum would silently break tap detection
// here; re-verify against the real header if tiles ever stop responding
// to taps after an LVGL version bump.
#define LV_EVENT_CLICKED_VALUE 7

// app_manager's own dual-mode registry — app_manager_count()/_entry_name()/
// _launch_idx() already dispatch on s_remote_mode internally (see this
// file's own top comment). app_manager_entry_name() is a plain accessor,
// never exposing app_entry_t itself — see its own definition comment in
// app_manager.c for why a loaded module uses this instead of hand-
// mirroring that real, actively-evolving struct.
extern int  app_manager_count(void);
extern bool app_manager_entry_name(int idx, char *out, size_t out_sz);
extern int  app_manager_launch_idx(int idx);

// Grid geometry — fixed, not computed from the real screen size (no
// lv_disp_get_hor_res()/_ver_res() in the import list yet — nothing here
// needed it badly enough to add another entry for it this pass). Sized
// for T-Deck Plus's known 320x240: 3 columns x 2 rows = 6 tiles, which
// happens to be exactly this device's current real app count (see
// app_mgr's own boot-log "scan complete: 6 apps found") — a device with
// more apps than TILE_COUNT_MAX just doesn't show the rest yet; real
// scrolling is a later, concrete need, not guessed at here.
#define TILE_SIZE      90
#define TILE_MARGIN    10
#define GRID_COLS      3
#define TILE_COUNT_MAX 6
#define TILE_NAME_MAX  32

static void tile_click_cb(lv_event_t *e)
{
    void *user_data = lv_event_get_user_data(e);
    int idx = (int)(intptr_t)user_data;
    app_manager_launch_idx(idx);
}

int claw_personal_init(void)
{
    lv_disp_t *disp = lv_disp_get_default();
    if (!disp) return -1;   // display driver not registered — see kernel_tdp_boot.c's lvgl_hw_init()
    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    if (!scr) return -1;

    // Wipe whatever the previous screen (loginUI) left on the default
    // screen — see purrstrap.py's own comment on lv_obj_clean's addition
    // for why this is done defensively here rather than relying on
    // loginUI to have cleaned up after itself.
    lv_obj_clean(scr);

    int count = app_manager_count();
    int shown = count < TILE_COUNT_MAX ? count : TILE_COUNT_MAX;

    for (int i = 0; i < shown; i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;

        lv_obj_t *tile = lv_obj_create(scr);
        lv_obj_set_pos(tile, TILE_MARGIN + col * (TILE_SIZE + TILE_MARGIN),
                              TILE_MARGIN + row * (TILE_SIZE + TILE_MARGIN));
        lv_obj_set_size(tile, TILE_SIZE, TILE_SIZE);
        lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED_VALUE, (void *)(intptr_t)i);

        char name[TILE_NAME_MAX];
        app_manager_entry_name(i, name, sizeof(name));
        lv_obj_t *label = lv_label_create(tile);
        lv_label_set_text(label, name);
        lv_obj_set_pos(label, 4, 4);
    }

    // Tiles built — return instead of looping forever. kernel_tdp_boot.c's
    // own session loop is now the one place driving lv_tick_inc()/
    // lv_timer_handler() (and ticking systemui's status bar alongside
    // this package), so a tap on a tile still gets detected and still
    // calls app_manager_launch_idx() above exactly as before — this file
    // just no longer needs to pump that loop itself. See this file's own
    // top comment for why that ownership moved to the host.
    return 0;
}

void claw_personal_deinit(void)
{
    // Called for real now (unlike before this package stopped owning its
    // own forever-loop): kernel_tdp_boot.c's session loop unloads the
    // launcher on every relock, before reloading loginUI. No heap
    // allocation of our own to free, no registered catcall_ui_t (this
    // package draws straight through LVGL's own already-set-up default
    // screen, same as login_render_lvgl.c) — nothing to actually tear
    // down, but the tiles themselves are left in place deliberately: the
    // NEXT thing to touch the default screen (loginUI, on relock) already
    // clears it via login_render_lvgl.c's own lv_obj_clean() at init,
    // same as this package clears loginUI's leftovers on ITS OWN init —
    // see that comment just above for the established convention this
    // follows.
}

#endif // SYSCLAW_BACKEND_LVGL
