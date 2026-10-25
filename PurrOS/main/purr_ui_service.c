#include "purr_ui_service.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "commands.h"
#include "purr_net.h"
#include "purr_ui_assets.h"

static const char *TAG = "ui_service";

#define LVGL_TICK_PERIOD_MS 5
#define SERVICE_QUEUE_LEN   8

typedef struct {
    purr_ui_request_t req;
    QueueHandle_t reply_q;     /* holds exactly one void* reply */
} internal_msg_t;

static QueueHandle_t s_cmd_q;
static int s_started;

/* ---------------------------------------------------------------- widget construction
 * (moved here from purr_ui_config.c: this file is now the only place that calls an lv_*
 * function directly -- UI/SPEC.md section 2/4's rule, "no app call reaches LVGL outside the
 * service", proven here against the one real caller that exists today before any app does). */

static lv_anim_path_cb_t easing_from_name(const char *name)
{
    if (strcmp(name, "ease_in") == 0) return lv_anim_path_ease_in;
    if (strcmp(name, "ease_out") == 0) return lv_anim_path_ease_out;
    if (strcmp(name, "ease_in_out") == 0) return lv_anim_path_ease_in_out;
    return lv_anim_path_linear;
}

static void anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

/* Only "fade_in" exists for now -- one real animation to prove the mechanism end to end,
 * same reasoning as shipping one widget pair (label/button) instead of a dozen. A config
 * naming anything else is logged and just doesn't animate (the widget still appears,
 * instantly), not a parse-time or build-time failure. */
static void apply_animation(lv_obj_t *obj, const purr_ui_widget_t *w)
{
    if (strcmp(w->anim_type, "fade_in") != 0) {
        ESP_LOGW(TAG, "%s: unknown animation '%s', skipping", w->name, w->anim_type);
        return;
    }
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, w->anim_duration_ms > 0 ? (uint32_t)w->anim_duration_ms : 300);
    lv_anim_set_path_cb(&a, easing_from_name(w->anim_easing));
    lv_anim_start(&a);
}

static const lv_font_t *font_for_scale(int scale)
{
#if LV_FONT_MONTSERRAT_28
    if (scale >= 3) return &lv_font_montserrat_28;
#endif
#if LV_FONT_MONTSERRAT_20
    if (scale == 2) return &lv_font_montserrat_20;
#endif
    return NULL;        /* NULL leaves LVGL's own default (14) in place */
}

/* lv_img_set_src() just stores the pointer it's given -- the lv_img_dsc_t it points to has
 * to outlive the lv_obj_t. Indexed by request sequence number modulo the pool, which is fine
 * for the config builder's one-shot "create everything once" use; an app that keeps creating
 * images after the pool wraps would start reusing slots still in use by an older image --
 * acceptable for now (no app does this yet), worth revisiting once one does. */
#define IMG_DESC_POOL 32
static lv_img_dsc_t s_img_descs[IMG_DESC_POOL];
static int s_img_desc_next;

static void button_clicked_cb(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "button '%s' tapped (no app owns this yet -- chunk C)", name ? name : "?");
}

static lv_obj_t *create_image(lv_obj_t *scr, const purr_ui_widget_t *w)
{
    const purr_asset_t *asset = purr_ui_assets_find(w->asset);
    if (asset == NULL) {
        ESP_LOGW(TAG, "%s: asset '%s' not found, skipping", w->name, w->asset);
        return NULL;
    }
    lv_img_dsc_t *d = &s_img_descs[s_img_desc_next];
    s_img_desc_next = (s_img_desc_next + 1) % IMG_DESC_POOL;
    d->header.cf = LV_IMG_CF_TRUE_COLOR;
    d->header.always_zero = 0;
    d->header.w = asset->width;
    d->header.h = asset->height;
    d->data_size = (uint32_t)asset->width * asset->height * 2;
    d->data = (const uint8_t *)asset->pixels;

    lv_obj_t *obj = lv_img_create(scr);
    lv_img_set_src(obj, d);
    return obj;
}

static void *do_add_widget(const purr_ui_widget_t *w)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *obj, *text_target = NULL;

    if (w->type == PURR_UI_WIDGET_IMAGE) {
        obj = create_image(scr, w);
        if (obj == NULL) {
            return NULL;            /* missing art: skip this widget, nothing else */
        }
    } else if (w->type == PURR_UI_WIDGET_BUTTON) {
        obj = lv_btn_create(scr);
        if (w->w > 0 && w->h > 0) {
            lv_obj_set_size(obj, w->w, w->h);
        }
        /* user_data outlives this call (static storage, not the stack) -- the widget's own
         * name, same string the config/caller already gave it, so the tap log can name it. */
        lv_obj_add_event_cb(obj, button_clicked_cb, LV_EVENT_CLICKED, (void *)w->name);
        text_target = lv_label_create(obj);
        lv_obj_center(text_target);
    } else {
        obj = lv_label_create(scr);
        text_target = obj;
    }
    lv_obj_set_pos(obj, w->x, w->y);

    if (text_target != NULL) {
        lv_label_set_text(text_target, w->text);
        if (w->color != PURR_UI_COLOR_UNSET) {
            lv_obj_set_style_text_color(text_target, lv_color_hex(w->color), 0);
        }
        const lv_font_t *font = font_for_scale(w->scale);
        if (font != NULL) {
            lv_obj_set_style_text_font(text_target, font, 0);
        }
    }
    if (w->anim_type[0] != '\0') {
        apply_animation(obj, w);
    }
    return obj;
}

/* ---------------------------------------------------------------- card shell (chunk I-1)
 *
 * Static layout only -- no drag gesture yet (chunk I-2's job). Home is a genuinely
 * full-screen card (320x240, flush to every edge) with nothing else visible at rest: the
 * first version of this got that wrong (inset the card to show a permanent peek), confirmed
 * wrong against real hardware and corrected -- a panel is only ever visible as the result of
 * an active drag (chunk I-2: peek at a small pull, a partial-open snap at more, full screen
 * at most), never a static affordance. For now the three panels just sit fully OFF-SCREEN at
 * their resting position (top panel above y=0, bottom panel below y=240, left panel left of
 * x=0) -- LVGL clips rendering to the screen bounds, so an off-screen object is simply
 * invisible, no separate "hidden" flag needed. Chunk I-2 is what ever moves them into view.
 *
 * Deliberately not configurable yet: sizes/positions are constants here, same as every
 * widget in this file started out (label/button/image all began this way too).
 */
#define SHELL_W            320
#define SHELL_H            240
#define SHELL_TOP_H        70     /* fallback only now -- see s_top_h below */
#define SHELL_BOTTOM_H     60     /* fallback only now -- see s_bottom_h below */
#define SHELL_LEFT_W       70     /* fallback only now -- see s_left_w below */
#define SHELL_CARD_RADIUS  16

static lv_obj_t *s_panel_left, *s_panel_top, *s_panel_bottom;
static lv_obj_t *s_card;

/* Each panel's actual thickness, set once in build_card_shell() from /ui/cardshell.conf (or
 * the SHELL_*_H/W fallbacks above if the config didn't describe that edge) -- the drag
 * engine's clamp/snap math reads these instead of the compile-time constants directly, so a
 * configured panel size and how far the card actually drags to reveal it always agree. */
static int s_top_h = SHELL_TOP_H;
static int s_bottom_h = SHELL_BOTTOM_H;
static int s_left_w = SHELL_LEFT_W;

/* Right side (chunk I-4): not a reveal panel like the other three edges -- a horizontal
 * carousel of placeholder "open app" cards living just off-screen to the right of Home, at
 * x = SHELL_W*(i+1). Home and every app card move together as one connected row during the
 * drag, which is why this needs its own page-tracking state instead of just uncovering a
 * fixed object the way the edge panels do. */
/* 12 is a hard system limit, not just a display convenience -- there's no dynamic allocation
 * here, s_app_cards[]/s_app_closed[] are sized exactly to it, so nothing can ever open past
 * it (there's no real app-launch path to enforce this against yet, but the array itself
 * already can't hold a 13th). */
#define N_PLACEHOLDER_APPS 12
static lv_obj_t *s_app_cards[N_PLACEHOLDER_APPS];
static int s_current_page;    /* 0 = Home, 1..N_PLACEHOLDER_APPS = that app card */
static int s_app_closed[N_PLACEHOLDER_APPS];  /* set once closed from inside the overview */

/* Each edge panel is LV_OBJ_FLAG_HIDDEN at rest and only unhidden for the one actually being
 * pulled -- NOT a static affordance ("a panel is only ever visible as the result of an active
 * drag", chunk I-1's own rule above). This replaced an earlier z-order-only scheme (bring the
 * revealed panel + card to the front of the stack, every panel always present just offscreen-
 * covered) that broke twice: once from a wrong lv_obj_move_to_index() index computation, and
 * again for real once the app carousel (chunk I-4) added more objects above the card -- a
 * panel promoted to the front of the *whole* stack during a Home-page drag stayed in front of
 * the app cards forever after, so paging into the carousel later showed the panel bleeding
 * through around the current app card's edges no matter what order anything was created in.
 * An actually-hidden panel can't bleed through anything, regardless of stacking order, so this
 * removes the whole class of z-order bug instead of patching it again. Passing NULL hides all
 * three (closed). */
static void panel_reveal(lv_obj_t *only)
{
    lv_obj_t *panels[3] = { s_panel_left, s_panel_top, s_panel_bottom };
    for (int i = 0; i < 3; i++) {
        if (panels[i] == only) {
            lv_obj_clear_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static lv_obj_t *make_panel(lv_obj_t *scr, int x, int y, int w, int h, uint32_t color,
                            const char *label)
{
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_remove_style_all(p);        /* no default card border/padding/scrollbars */
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    /* Each panel IS a card too, same as Home -- the whole shell is built on one visual
     * primitive ("each app is also a card", memory/[[project_card_shell_ui_redesign]]'s own
     * note). A flat rectangular box with square corners read as a generic panel, not a card;
     * matching Home's own radius is what actually makes it look like a separate card sitting
     * behind/around it rather than background chrome. */
    lv_obj_set_style_radius(p, SHELL_CARD_RADIUS, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);

    /* NULL label: this panel is getting real populated content instead (populate_apps_
     * panel()/populate_status_panel() below) -- the generic centered placeholder label
     * would just sit underneath/behind it. */
    if (label != NULL) {
        lv_obj_t *l = lv_label_create(p);
        lv_label_set_text(l, label);
        lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(l);
    }
    return p;
}

/* Apps panel: real installed-apps data, same registry scan chunk F's Home-screen app_grid
 * already used (purr_apps_scan_current_user(), commands.h) -- not a second, different data
 * source. One row of small buttons; AppManager's own install/remove already keeps the
 * registry this reads current, so there's nothing to wire beyond calling it at build time.
 * Takes `panel_h` explicitly rather than reading it back via lv_obj_get_height(panel) --
 * confirmed by instrumenting with ESP_LOGI and reading a real serial capture back (same
 * method as the chunk I-2 drag bug): LVGL doesn't refresh an object's cached coordinates
 * synchronously within the same call stack as lv_obj_set_size(), only on its next layout
 * pass, so a get_height() called this soon after make_panel() reliably reads back 0 even
 * though the panel itself renders at the right size moments later. The caller already knows
 * the real height (it just built the panel at that size), so there's nothing to read back. */
static void populate_apps_panel(lv_obj_t *panel, int panel_h)
{
    /* static, not a stack local: purr_app_registry_t is ~1.6KB (PURR_APP_MAX=32 entries) --
     * real stack overflow found on hardware the first time this ran, on the ui_service
     * task's 4KB stack already carrying LVGL's own call overhead underneath it. Safe as
     * static: this only ever runs on the single-threaded ui_service task, never reentered. */
    static purr_app_registry_t reg;
    purr_apps_scan_current_user(&reg);

    int cell_w = 70, pad = 4;
    for (int i = 0; i < reg.count; i++) {
        lv_obj_t *btn = lv_btn_create(panel);
        lv_obj_set_size(btn, cell_w - pad, panel_h - 2 * pad);
        lv_obj_set_pos(btn, pad + i * cell_w, pad);
        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, reg.apps[i].name);
        lv_obj_center(l);
    }
    if (reg.count == 0) {
        lv_obj_t *l = lv_label_create(panel);
        lv_label_set_text(l, "No apps installed");
        lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(l);
    }
}

/* Status panel: battery/Wi-Fi/Bluetooth/LoRa, stacked (the panel is narrow and tall).
 * Wi-Fi is real (purr_net_status(), already-proven kernel API -- the same one wifi.cat's
 * shell commands use). The other three are honest placeholders, not faked data: there is no
 * battery driver, no Bluetooth stack, and no LoRa/companion-discovery protocol anywhere in
 * this rewrite yet (archive/DP9's adc_battery/NimBLE/sx1262_rl were never ported) -- each is
 * real, separate future work on the order of the GT911 touch port, not a quick addition, so
 * showing a plausible-looking fake number would be actively misleading. Plain text labels,
 * not LV_SYMBOL_* icon glyphs, until it's confirmed the built-in fonts here actually carry
 * the symbol range (not yet checked on this hardware). */
static void populate_status_panel(lv_obj_t *panel)
{
    char line[48];
    int y = 6;
    const int row_h = 24;

    lv_obj_t *battery = lv_label_create(panel);
    lv_label_set_text(battery, "Batt: --");
    lv_obj_set_style_text_color(battery, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(battery, 4, y);
    y += row_h;

    purr_net_status_t net;
    purr_net_status(&net);
    lv_obj_t *wifi = lv_label_create(panel);
    if (net.connected) {
        snprintf(line, sizeof(line), "WiFi: %d dBm", net.rssi);
    } else {
        snprintf(line, sizeof(line), "WiFi: off");
    }
    lv_label_set_text(wifi, line);
    lv_obj_set_style_text_color(wifi, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(wifi, 4, y);
    y += row_h;

    lv_obj_t *bt = lv_label_create(panel);
    lv_label_set_text(bt, "BT: --");
    lv_obj_set_style_text_color(bt, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(bt, 4, y);
    y += row_h;

    lv_obj_t *lora = lv_label_create(panel);
    lv_label_set_text(lora, "LoRa: --");
    lv_obj_set_style_text_color(lora, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(lora, 4, y);
}

/* ---------------------------------------------------------------- card shell drag (chunk
 * I-2, now covering all three edges with one generalized handler, proven first on the
 * bottom edge alone -- memory/[[project_card_shell_ui_redesign]]'s own writeup of the real
 * bug found and fixed there, and why it had to be fixed before extending to more edges.
 *
 * Finger-following drag with snap: the card moves live with the touch point while dragging
 * (LV_EVENT_PRESSING), then animates the rest of the way open or closed on release, based on
 * which side of the halfway point the drag ended on. Deliberately uniform across all three
 * edges for this pass -- top's richer "peek shows a count, pull further goes full screen"
 * behavior would need the top panel sized to the full screen, which overlaps the left
 * panel's own region behind the card; simplified to the same closed/one-open-stage shape
 * every edge already proved on bottom, until there's real notification content to justify
 * the bigger panel and the overlap handling it would need.
 *
 * One axis per gesture, decided from whichever way the touch moves first (a few pixels'
 * tolerance before committing) -- a single card object can't sensibly follow both axes from
 * one drag. Y spans both top and bottom (negative = bottom panel revealed, positive = top
 * panel revealed, one clamp range covers both directions from any starting position); X
 * spans only rightward (the left panel) -- leftward is the open-app carousel, separate
 * scope (chunk I-4), not handled by this edge-panel code at all. */
typedef enum {
    DRAG_AXIS_NONE, DRAG_AXIS_Y, DRAG_AXIS_X, DRAG_AXIS_CAROUSEL,
    /* A vertical swipe starting from the bottom-center zone while inside an app (not Home):
     * quick release minimizes back to Home (the whole row -- card + every app card -- lifts
     * together, same connected-row idea as the carousel, just on Y instead of X); holding it
     * instead opens the multitasking overview (see that section below), consistent with "apps
     * don't exit, they minimize" and "swipe up and hold shows the open cards". */
    DRAG_AXIS_HOME_SWIPE,
} drag_axis_t;

static int s_drag_start_touch_x, s_drag_start_touch_y;
static int s_drag_start_card_x, s_drag_start_card_y;
static int s_drag_start_app_x[N_PLACEHOLDER_APPS];
static int s_drag_start_app_y[N_PLACEHOLDER_APPS];
static int s_drag_active;
static int s_drag_moved;      /* did PRESSING ever actually move the card this press? */
static drag_axis_t s_drag_axis;

static void card_anim_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }
static void card_anim_x_cb(void *obj, int32_t v) { lv_obj_set_x((lv_obj_t *)obj, v); }
static void card_anim_w_cb(void *obj, int32_t v) { lv_obj_set_width((lv_obj_t *)obj, v); }
static void card_anim_h_cb(void *obj, int32_t v) { lv_obj_set_height((lv_obj_t *)obj, v); }

/* ready_cb fires only once the animation actually finishes (or is skipped entirely because
 * the object was already at its target) -- used to hide a panel only once its close animation
 * has really played out, instead of the panel popping away the instant the drag released
 * while the card is still visibly sliding shut over it. */
static void panel_hide_all_ready_cb(lv_anim_t *a) { (void)a; panel_reveal(NULL); }

static void card_snap_axis(lv_obj_t *card, drag_axis_t axis, int32_t target,
                           lv_anim_ready_cb_t ready_cb)
{
    int32_t start = axis == DRAG_AXIS_Y ? lv_obj_get_y(card) : lv_obj_get_x(card);
    if (start == target) {
        if (ready_cb != NULL) {
            ready_cb(NULL);     /* nothing to animate, but the caller still expects the result */
        }
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, card);
    lv_anim_set_exec_cb(&a, axis == DRAG_AXIS_Y ? card_anim_y_cb : card_anim_x_cb);
    lv_anim_set_values(&a, start, target);
    lv_anim_set_time(&a, 350);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (ready_cb != NULL) {
        lv_anim_set_ready_cb(&a, ready_cb);
    }
    lv_anim_start(&a);
}

/* Closes whichever axis is currently open, if any -- tap-to-dismiss (card_drag_cb's own
 * comment on why this is a deliberate separate interaction, not a drag-math side effect).
 * Checks both: only one is ever non-zero at rest in practice, but this stays correct even if
 * that ever changes. Each animated axis hides the panels itself once it actually finishes
 * (panel_hide_all_ready_cb); if neither axis was open there's nothing playing to wait for. */
static void card_snap_closed(lv_obj_t *card)
{
    if (lv_obj_get_y(card) != 0) card_snap_axis(card, DRAG_AXIS_Y, 0, panel_hide_all_ready_cb);
    if (lv_obj_get_x(card) != 0) card_snap_axis(card, DRAG_AXIS_X, 0, panel_hide_all_ready_cb);
}

/* Animates Home and every app card to their resting positions for the given page in one
 * move -- card_snap_axis is already generic over which object it animates (it just reads/
 * writes whichever one it's handed), so the whole row is just that helper called once per
 * object instead of new animation plumbing. Never touches the edge panels, so no ready_cb. */
static void carousel_snap_to_page(int target_page)
{
    if (target_page < 0) target_page = 0;
    if (target_page > N_PLACEHOLDER_APPS) target_page = N_PLACEHOLDER_APPS;
    s_current_page = target_page;

    card_snap_axis(s_card, DRAG_AXIS_X, -(int32_t)SHELL_W * target_page, NULL);
    for (int i = 0; i < N_PLACEHOLDER_APPS; i++) {
        if (s_app_closed[i]) {
            continue;
        }
        card_snap_axis(s_app_cards[i], DRAG_AXIS_X, (int32_t)SHELL_W * (i + 1 - target_page),
                       NULL);
    }
}

/* ---------------------------------------------------------------- multitasking overview
 *
 * Corrected design (the first pass had this wrong): a plain swipe up out of an app never
 * closes it -- it just minimizes back to Home, same as tapping Home would, and the app stays
 * exactly where it was in the carousel. HOLDING that swipe instead (not releasing) opens a
 * real overview of every still-open card -- Home plus every app that hasn't been explicitly
 * closed, in creation order -- where a tap picks a card and an upward swipe ON a card *inside
 * the overview* is the only thing that actually closes it.
 *
 * A snapping, depth-stacked carousel (the "iOS 8" switcher look, not the flat equal-spaced
 * "iOS 7" filmstrip this went through first): one card is focused at a time -- full size and
 * topmost -- with up to OVERVIEW_MAX_SIDE_STEPS neighbors on each side peeking out from
 * behind it, each step smaller and further back. Dragging moves the whole stack; releasing
 * snaps to whichever card ended up nearest the focus position, same "commit on release"
 * pattern as every other drag in this file. */
#define OVERVIEW_HOLD_MS        450   /* fallback only now -- see s_overview_hold_ms below */
#define OVERVIEW_FOCUS_W        200   /* fallback only now -- see s_overview_focus_w below */
#define OVERVIEW_FOCUS_H        160   /* fallback only now -- see s_overview_focus_h below */
#define OVERVIEW_STEP_X         60    /* fallback only now -- see s_overview_step_x below */
#define OVERVIEW_STEP_SHRINK_W  30    /* fallback only now -- see s_overview_step_shrink_w below */
#define OVERVIEW_STEP_SHRINK_H  24    /* fallback only now -- see s_overview_step_shrink_h below */
#define OVERVIEW_MAX_SIDE_STEPS 2     /* fallback only now -- see s_overview_max_side_steps below */
#define OVERVIEW_CLOSE_LIFT     20    /* fallback only now -- see s_overview_close_lift below */

/* Resolved from /ui/cardshell.conf's `overview` block in build_card_shell() (or the
 * OVERVIEW_* fallbacks above if that block's missing) -- every function below reads these,
 * never the #defines directly, so config and the live gesture math always agree. */
static int s_overview_hold_ms = OVERVIEW_HOLD_MS;
static int s_overview_focus_w = OVERVIEW_FOCUS_W;
static int s_overview_focus_h = OVERVIEW_FOCUS_H;
static int s_overview_step_x = OVERVIEW_STEP_X;
static int s_overview_step_shrink_w = OVERVIEW_STEP_SHRINK_W;
static int s_overview_step_shrink_h = OVERVIEW_STEP_SHRINK_H;
static int s_overview_max_side_steps = OVERVIEW_MAX_SIDE_STEPS;
static int s_overview_close_lift = OVERVIEW_CLOSE_LIFT;

/* Generic touch-recognition sensitivity, shared by every drag handler in the file (not owned
 * by any one screen element) -- resolved from /ui/cardshell.conf's `gesture` block. Stored as
 * a plain pixel distance; squared at each use site against the already-squared
 * dx*dx+dy*dy so nothing has to call a sqrt(). */
#define GESTURE_TOLERANCE_PX 3   /* fallback only now -- see s_gesture_tolerance_px below */
static int s_gesture_tolerance_px = GESTURE_TOLERANCE_PX;

static int s_in_overview;
static uint32_t s_press_start_tick;

typedef enum { OVERVIEW_AXIS_NONE, OVERVIEW_AXIS_SCROLL, OVERVIEW_AXIS_VERTICAL } overview_axis_t;

static lv_obj_t *s_overview_touch_target;
static int s_overview_touch_start_x, s_overview_touch_start_y;
static int s_overview_focus;           /* index into the current open-card list */
static overview_axis_t s_overview_axis;
static int s_overview_close_armed;     /* vertical drag on a card crossed the close threshold */
static int s_overview_suppress_tap_exit;   /* see the hold-handoff in card_drag_cb */

static void anim_prop(lv_obj_t *obj, lv_anim_exec_xcb_t exec_cb, int32_t start, int32_t end,
                      uint32_t duration_ms, uint32_t delay_ms)
{
    if (start == end && delay_ms == 0) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, exec_cb);
    lv_anim_set_values(&a, start, end);
    lv_anim_set_time(&a, duration_ms);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static int page_index_of(lv_obj_t *obj)
{
    if (obj == s_card) {
        return 0;
    }
    for (int i = 0; i < N_PLACEHOLDER_APPS; i++) {
        if (obj == s_app_cards[i]) {
            return i + 1;
        }
    }
    return 0;
}

/* Fills `objs` with every still-open card, Home first, in creation order -- the same list
 * both the layout code and the live-drag code need, so it's built in one place. */
static int overview_open_objs(lv_obj_t **objs)
{
    int n = 0;
    objs[n++] = s_card;
    for (int i = 0; i < N_PLACEHOLDER_APPS; i++) {
        if (!s_app_closed[i]) {
            objs[n++] = s_app_cards[i];
        }
    }
    return n;
}

/* Size and position for a card `d` steps away from focus (negative = to the left, positive =
 * to the right, 0 = the focused card itself; fractional during a live drag so size and
 * position both interpolate continuously with the finger instead of snapping only once the
 * nearest card is decided -- the earlier integer-only version looked fine at rest but was
 * why a live drag read as cards abruptly clipping over each other instead of growing/shrinking
 * as they approached/left focus). Still returns a (small, far-out) rect past
 * OVERVIEW_MAX_SIDE_STEPS -- callers decide whether that's shown or hidden -- so a card
 * crossing the cap mid-drag doesn't have to pop between two completely different formulas. */
static void overview_card_rect(float d, int *x, int *y, int *w, int *h)
{
    float ad = d < 0 ? -d : d;
    if (ad > s_overview_max_side_steps) {
        ad = (float)s_overview_max_side_steps;
    }
    *w = s_overview_focus_w - (int)(ad * s_overview_step_shrink_w);
    *h = s_overview_focus_h - (int)(ad * s_overview_step_shrink_h);
    *x = (SHELL_W - *w) / 2 + (int)(d * s_overview_step_x);
    *y = (SHELL_H - *h) / 2;
}

/* Farthest-from-focus first, so the focused card itself (distance 0) ends up topmost. */
static void overview_restack(lv_obj_t **objs, int n, int focus)
{
    for (int dist = s_overview_max_side_steps; dist >= 0; dist--) {
        for (int i = 0; i < n; i++) {
            int ad = i - focus;
            if (ad < 0) ad = -ad;
            if (ad == dist) {
                lv_obj_move_foreground(objs[i]);
            }
        }
    }
}

/* Animates every still-open card to its resting position for the current s_overview_focus --
 * used on entry, after closing a card, and to settle a drag once it's released. Anything more
 * than OVERVIEW_MAX_SIDE_STEPS away is hidden outright rather than left sitting tiny and
 * off to the side. */
static void overview_layout_animate(void)
{
    lv_obj_t *objs[N_PLACEHOLDER_APPS + 1];
    int n = overview_open_objs(objs);
    if (s_overview_focus >= n) s_overview_focus = n - 1;
    if (s_overview_focus < 0) s_overview_focus = 0;

    overview_restack(objs, n, s_overview_focus);

    for (int i = 0; i < n; i++) {
        int d = i - s_overview_focus;
        int ad = d < 0 ? -d : d;
        if (ad > s_overview_max_side_steps) {
            lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        int x, y, w, h;
        overview_card_rect((float)d, &x, &y, &w, &h);
        anim_prop(objs[i], card_anim_x_cb, lv_obj_get_x(objs[i]), x, 300, 0);
        anim_prop(objs[i], card_anim_y_cb, lv_obj_get_y(objs[i]), y, 300, 0);
        anim_prop(objs[i], card_anim_w_cb, lv_obj_get_width(objs[i]), w, 300, 0);
        anim_prop(objs[i], card_anim_h_cb, lv_obj_get_height(objs[i]), h, 300, 0);
    }
}

/* Nearest focus index for a given raw drag distance `dx` from `from_focus`, clamped to a
 * valid index -- shared by the live drag (which card is "about to win") and the release
 * commit (which card actually wins), so they always agree. */
static int overview_nearest_focus(int from_focus, int dx, int n)
{
    int steps = (dx + (dx >= 0 ? s_overview_step_x / 2 : -s_overview_step_x / 2))
              / s_overview_step_x;
    int focus = from_focus - steps;
    if (focus < 0) focus = 0;
    if (focus > n - 1) focus = n - 1;
    return focus;
}

static void overview_enter(void)
{
    s_in_overview = 1;
    s_overview_focus = s_current_page;   /* opens centered on whatever you swiped up out of */
    overview_layout_animate();
}

/* Restores every still-open card to full screen and pages to `target_page` -- reuses
 * carousel_snap_to_page() for the X move and page-tracking instead of duplicating it. */
static void overview_exit(int target_page)
{
    s_in_overview = 0;

    for (int i = 0; i < N_PLACEHOLDER_APPS; i++) {
        if (s_app_closed[i]) {
            continue;
        }
        lv_obj_t *obj = s_app_cards[i];
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        anim_prop(obj, card_anim_w_cb, lv_obj_get_width(obj), SHELL_W, 300, 0);
        anim_prop(obj, card_anim_h_cb, lv_obj_get_height(obj), SHELL_H, 300, 0);
        anim_prop(obj, card_anim_y_cb, lv_obj_get_y(obj), 0, 300, 0);
    }
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    anim_prop(s_card, card_anim_w_cb, lv_obj_get_width(s_card), SHELL_W, 300, 0);
    anim_prop(s_card, card_anim_h_cb, lv_obj_get_height(s_card), SHELL_H, 300, 0);
    anim_prop(s_card, card_anim_y_cb, lv_obj_get_y(s_card), 0, 300, 0);

    carousel_snap_to_page(target_page);
}

/* Touch handling while s_in_overview is set -- a tap picks that card, a horizontal drag moves
 * the stack (snapping to a new focus on release), an upward drag closes the card it started
 * on (Home can't be closed: page_index_of() returns 0 for it, and the close branch below is
 * gated on page != 0). Routed here from card_drag_cb() itself rather than a second set of
 * event callbacks, so there's exactly one place deciding which gesture mode a touch is in. */
static void overview_touch_cb(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == NULL) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        s_overview_touch_target = target;
        s_overview_touch_start_x = p.x;
        s_overview_touch_start_y = p.y;
        s_overview_axis = OVERVIEW_AXIS_NONE;
        s_overview_close_armed = 0;
        /* A genuine fresh press always behaves normally (no movement + release = pick this
         * card) -- only the hold-handoff touch that just opened the overview suppresses that,
         * and this never runs for that touch (its PRESSED already fired before the overview
         * existed), so this only ever clears a suppress flag left over from an earlier touch. */
        s_overview_suppress_tap_exit = 0;
    } else if (code == LV_EVENT_PRESSING && s_overview_touch_target == target) {
        int dx = p.x - s_overview_touch_start_x;
        int dy = p.y - s_overview_touch_start_y;
        if (s_overview_axis == OVERVIEW_AXIS_NONE) {
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            if (adx * adx + ady * ady < s_gesture_tolerance_px * s_gesture_tolerance_px) {
                return;         /* same tolerance as card_drag_cb's own commit */
            }
            s_overview_axis = (adx > ady) ? OVERVIEW_AXIS_SCROLL : OVERVIEW_AXIS_VERTICAL;
        }
        if (s_overview_axis == OVERVIEW_AXIS_SCROLL) {
            lv_obj_t *objs[N_PLACEHOLDER_APPS + 1];
            int n = overview_open_objs(objs);
            int nearest = overview_nearest_focus(s_overview_focus, dx, n);
            overview_restack(objs, n, nearest);

            /* Continuous, not integer: each card's distance from focus moves smoothly with
             * the finger, so size interpolates right along with position instead of staying
             * static until release -- "doesn't grow until I let go, just clips over the
             * other one" was this being driven by the old fixed per-card `d`. */
            float continuous_focus = (float)s_overview_focus - (float)dx / s_overview_step_x;
            for (int i = 0; i < n; i++) {
                float cd = (float)i - continuous_focus;
                lv_obj_clear_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
                int x, y, w, h;
                overview_card_rect(cd, &x, &y, &w, &h);
                lv_obj_set_pos(objs[i], x, y);
                lv_obj_set_size(objs[i], w, h);
            }
        } else if (dy < -s_overview_close_lift) {
            s_overview_close_armed = 1;
        }
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
               && s_overview_touch_target == target) {
        s_overview_touch_target = NULL;
        int page = page_index_of(target);
        if (s_overview_axis == OVERVIEW_AXIS_SCROLL) {
            lv_obj_t *objs[N_PLACEHOLDER_APPS + 1];
            int n = overview_open_objs(objs);
            int dx = p.x - s_overview_touch_start_x;
            s_overview_focus = overview_nearest_focus(s_overview_focus, dx, n);
            overview_layout_animate();
        } else if (s_overview_axis == OVERVIEW_AXIS_VERTICAL && s_overview_close_armed
                   && page != 0) {
            s_app_closed[page - 1] = 1;
            lv_obj_add_flag(target, LV_OBJ_FLAG_HIDDEN);
            overview_layout_animate();     /* reflow what's left at the current focus */
        } else if (s_overview_axis == OVERVIEW_AXIS_NONE && !s_overview_suppress_tap_exit) {
            overview_exit(page);           /* a real tap, no movement at all: pick this card */
        }
        /* OVERVIEW_AXIS_NONE with the suppress flag set: this is the same touch that just
         * hold-triggered entry into the overview, simply released without moving any further
         * -- stay in the overview (that's the whole point of "hold to see it, then you can
         * just let go"), don't treat "didn't scroll" as "tap to pick a card". */
    }
}

/* ---------------------------------------------------------------- minimize to home
 *
 * The visual for a committed quick-swipe-up-to-home: Home slides straight up from off-screen
 * below to fill the screen, while the departing app (and any other open app) just returns to
 * its normal carousel resting spot. Replaced an earlier 3-card stair-stack/cut effect (kept
 * from an accidental z-order bug that happened to look good) -- that stayed for a while, but
 * "slides up from the bottom" for this specific gesture is simpler and was asked for over it
 * directly, not as an addition to it. */
#define MINIMIZE_SLIDE_MS 300   /* fallback only now -- see s_minimize_slide_ms below */
/* Bottom-center touch zone (fallbacks only -- see s_minimize_zone_* below): a swipe up has
 * to start inside this box, this many px tall from the bottom edge, to be recognized as
 * "minimize to home" instead of an ordinary left/right carousel drag. */
#define MINIMIZE_ZONE_H     48
#define MINIMIZE_ZONE_X_MIN (SHELL_W / 4)
#define MINIMIZE_ZONE_X_MAX (SHELL_W * 3 / 4)

/* Resolved from /ui/cardshell.conf's `minimize` block in build_card_shell() (or the
 * MINIMIZE_* fallbacks above if that block's missing). */
static int s_minimize_slide_ms = MINIMIZE_SLIDE_MS;
static int s_minimize_zone_h = MINIMIZE_ZONE_H;
static int s_minimize_zone_x_min = MINIMIZE_ZONE_X_MIN;
static int s_minimize_zone_x_max = MINIMIZE_ZONE_X_MAX;

/* Once the departing card has actually slid off the top of the screen (and is therefore
 * invisible), its X/Y get corrected to their normal resting values for whatever page it is --
 * fixing it up only now, instead of up front, is what lets it visibly travel straight up from
 * wherever it was sitting instead of silently teleporting sideways first. */
static void minimize_departing_ready_cb(lv_anim_t *a)
{
    lv_obj_t *obj = (lv_obj_t *)a->var;
    int page = page_index_of(obj);
    lv_obj_set_x(obj, SHELL_W * page);
    lv_obj_set_y(obj, 0);
}

static void minimize_slide_home(void)
{
    int from_page = s_current_page;

    /* Teleporting to the off-screen starting rect happens synchronously, in the same event-
     * processing pass as every other instant lv_obj_set_* call in this file, so nothing is
     * ever actually drawn there -- only the animated slide up to y=0 is visible. Explicitly
     * promoted to the top of the z-order first: app cards are created after s_card, so
     * without this Home would slide up *behind* whatever app is currently showing and only
     * become visible once that app's own card moved out of the way -- reads as "sliding all
     * the way back through every card" instead of Home popping up and landing on top. */
    lv_obj_move_foreground(s_card);
    lv_obj_set_size(s_card, SHELL_W, SHELL_H);
    lv_obj_set_x(s_card, 0);
    lv_obj_set_y(s_card, SHELL_H);
    anim_prop(s_card, card_anim_y_cb, SHELL_H, 0, (uint32_t)s_minimize_slide_ms, 0);

    for (int i = 0; i < N_PLACEHOLDER_APPS; i++) {
        if (s_app_closed[i]) {
            continue;
        }
        if (i == from_page - 1) {
            /* The card actually being swiped away: it slides up and off the top of the
             * screen in sync with Home sliding up from the bottom -- a real push, not a
             * teleport-then-reveal. X deliberately isn't touched here (it's already 0, right
             * where it's visibly sitting); see minimize_departing_ready_cb(). */
            lv_obj_t *obj = s_app_cards[i];
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, obj);
            lv_anim_set_exec_cb(&a, card_anim_y_cb);
            lv_anim_set_values(&a, lv_obj_get_y(obj), -SHELL_H);
            lv_anim_set_time(&a, (uint32_t)s_minimize_slide_ms);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_set_ready_cb(&a, minimize_departing_ready_cb);
            lv_anim_start(&a);
            continue;
        }
        /* Every other open app isn't visible either way -- its X/page state just resets
         * silently (no animation) instead of visibly sliding back through every intervening
         * page the way carousel_snap_to_page() would. */
        lv_obj_set_x(s_app_cards[i], SHELL_W * (i + 1));
        lv_obj_set_y(s_app_cards[i], 0);
    }
    s_current_page = 0;
}

/* This callback is attached to Home AND to every placeholder app card (chunk I-4) -- whichever
 * one is actually visible is the one that receives the touch, since Home sits off-screen once
 * the carousel has paged away from it. It deliberately never acts on lv_event_get_target(e):
 * every branch below always drives s_card/s_app_cards[] directly, so the logic is identical no
 * matter which object physically caught the press. */
static void card_drag_cb(lv_event_t *e)
{
    if (s_in_overview) {
        overview_touch_cb(e);
        return;
    }

    lv_indev_t *indev = lv_indev_get_act();
    if (indev == NULL) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_drag_start_touch_x = p.x;
        s_drag_start_touch_y = p.y;
        s_drag_start_card_x = lv_obj_get_x(s_card);
        s_drag_start_card_y = lv_obj_get_y(s_card);
        for (int i = 0; i < N_PLACEHOLDER_APPS; i++) {
            s_drag_start_app_x[i] = lv_obj_get_x(s_app_cards[i]);
            s_drag_start_app_y[i] = lv_obj_get_y(s_app_cards[i]);
        }
        s_press_start_tick = lv_tick_get();
        s_drag_active = 1;
        s_drag_moved = 0;
        s_drag_axis = DRAG_AXIS_NONE;
    } else if (code == LV_EVENT_PRESSING && s_drag_active) {
        int dx = p.x - s_drag_start_touch_x;
        int dy = p.y - s_drag_start_touch_y;
        if (s_drag_axis == DRAG_AXIS_NONE) {
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            if (adx * adx + ady * ady < s_gesture_tolerance_px * s_gesture_tolerance_px) {
                return;         /* a few pixels' tolerance before committing to an axis */
            }
            if (adx > ady) {
                /* Rightward on Home uncovers the left status panel (pulling it out from the
                 * left edge); rightward anywhere else in the carousel means "head back toward
                 * Home". Leftward is always "go deeper into the carousel" (pulling the next
                 * app card in from the right edge), clamped to a no-op once already on the
                 * last page by carousel_snap_to_page()/the PRESSING clamp below. */
                if (dx > 0 && s_current_page == 0) {
                    s_drag_axis = DRAG_AXIS_X;
                    panel_reveal(s_panel_left);
                } else {
                    s_drag_axis = DRAG_AXIS_CAROUSEL;
                }
            } else if (s_current_page == 0) {
                s_drag_axis = DRAG_AXIS_Y;
                /* dy<0: dragging up, bottom panel revealed. dy>0: dragging down, top panel
                 * revealed. */
                panel_reveal(dy < 0 ? s_panel_bottom : s_panel_top);
            } else if (dy < 0 &&
                       s_drag_start_touch_y >= SHELL_H - s_minimize_zone_h &&
                       s_drag_start_touch_x >= s_minimize_zone_x_min &&
                       s_drag_start_touch_x <= s_minimize_zone_x_max) {
                /* Upward, starting from the bottom-center zone, while inside an app. */
                s_drag_axis = DRAG_AXIS_HOME_SWIPE;
            } else {
                return;         /* no gesture defined for vertical drags elsewhere in the carousel */
            }
        }
        if (s_drag_axis == DRAG_AXIS_HOME_SWIPE &&
            lv_tick_elaps(s_press_start_tick) > (uint32_t)s_overview_hold_ms) {
            /* Held past the threshold without releasing: this is the multitasking overview,
             * not a minimize. overview_enter() takes it from here -- but it's still the same
             * physical touch, which never ran through overview_touch_cb's own PRESSED branch,
             * so its tracking state would otherwise stay NULL/stale for the rest of this touch
             * and silently ignore every further drag update until the finger actually lifted
             * and pressed again (the bug: "it doesn't follow the scroll"). Seed that state
             * here instead, right as the handoff happens, so the same finger keeps scrolling
             * the overview without needing to be lifted first. Also arm the tap-exit
             * suppression: just releasing right here (no further movement) should leave you
             * sitting in the overview -- "swipe up and hold, then let go" -- not read as a
             * tap that immediately exits it again. */
            overview_enter();
            s_overview_touch_target = lv_event_get_target(e);
            s_overview_touch_start_x = p.x;
            s_overview_touch_start_y = p.y;
            s_overview_axis = OVERVIEW_AXIS_NONE;
            s_overview_close_armed = 0;
            s_overview_suppress_tap_exit = 1;
            s_drag_active = 0;
            return;
        }
        if (s_drag_axis == DRAG_AXIS_Y) {
            int new_y = s_drag_start_card_y + dy;
            if (new_y > s_top_h) new_y = s_top_h;
            if (new_y < -s_bottom_h) new_y = -s_bottom_h;
            lv_obj_set_y(s_card, new_y);
        } else if (s_drag_axis == DRAG_AXIS_X) {
            int new_x = s_drag_start_card_x + dx;
            if (new_x < 0) new_x = 0;
            if (new_x > s_left_w) new_x = s_left_w;
            lv_obj_set_x(s_card, new_x);
        } else if (s_drag_axis == DRAG_AXIS_HOME_SWIPE) {
            /* Quick-swipe path (still under the hold threshold): card + every app card lift
             * together as one row, same connected-group idea as the carousel branch below,
             * just on Y. Clamping Home's own new Y and re-deriving the actual delta keeps
             * every object in lockstep the same way the carousel does on X. */
            int32_t new_card_y = s_drag_start_card_y + dy;
            if (new_card_y > 0) new_card_y = 0;
            if (new_card_y < -SHELL_H) new_card_y = -SHELL_H;
            int32_t actual_dy = new_card_y - s_drag_start_card_y;
            lv_obj_set_y(s_card, new_card_y);
            for (int i = 0; i < N_PLACEHOLDER_APPS; i++) {
                if (!s_app_closed[i]) {
                    lv_obj_set_y(s_app_cards[i], s_drag_start_app_y[i] + actual_dy);
                }
            }
        } else {
            /* DRAG_AXIS_CAROUSEL: Home and every app card are one connected row, so the same
             * (clamped) delta applies to all of them -- clamping Home's own new position and
             * re-deriving the actual delta from that is simpler than clamping each object
             * separately and risking them drifting out of step with each other. */
            int32_t min_x = -(int32_t)SHELL_W * N_PLACEHOLDER_APPS;
            int32_t new_card_x = s_drag_start_card_x + dx;
            if (new_card_x > 0) new_card_x = 0;
            if (new_card_x < min_x) new_card_x = min_x;
            int32_t actual_dx = new_card_x - s_drag_start_card_x;
            lv_obj_set_x(s_card, new_card_x);
            for (int i = 0; i < N_PLACEHOLDER_APPS; i++) {
                lv_obj_set_x(s_app_cards[i], s_drag_start_app_x[i] + actual_dx);
            }
        }
        s_drag_moved = 1;
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && s_drag_active) {
        s_drag_active = 0;
        if (!s_drag_moved) {
            /* A real tap (no drag distance at all): dismiss, if open -- the explicit
             * interaction asked for ("tap the card to pull it back down to full screen"),
             * not a side effect of drag math. */
            card_snap_closed(s_card);
            return;
        }
        if (s_drag_axis == DRAG_AXIS_Y) {
            int32_t cur = lv_obj_get_y(s_card);
            int32_t target = cur <= -s_bottom_h / 2 ? -s_bottom_h
                            : cur >= s_top_h / 2 ? s_top_h : 0;
            card_snap_axis(s_card, DRAG_AXIS_Y, target,
                           target == 0 ? panel_hide_all_ready_cb : NULL);
        } else if (s_drag_axis == DRAG_AXIS_X) {
            int32_t cur = lv_obj_get_x(s_card);
            int32_t target = cur >= s_left_w / 2 ? s_left_w : 0;
            card_snap_axis(s_card, DRAG_AXIS_X, target,
                           target == 0 ? panel_hide_all_ready_cb : NULL);
        } else if (s_drag_axis == DRAG_AXIS_CAROUSEL) {
            int32_t cur = lv_obj_get_x(s_card);
            int target_page = (-cur + SHELL_W / 2) / SHELL_W;
            carousel_snap_to_page(target_page);
        } else if (s_drag_axis == DRAG_AXIS_HOME_SWIPE) {
            /* Lifted past a quarter of the screen height: minimize to Home, sliding up from
             * off-screen below (minimize_slide_home() handles every card's reset, including
             * Y). The app stays open (s_app_closed untouched) -- it's still wherever it was
             * in the carousel, only Home's X/page state changes. Not lifted far enough:
             * cancel, plain Y reset for everything, no slide. */
            int32_t cur = lv_obj_get_y(s_card);
            if (cur <= -SHELL_H / 4) {
                minimize_slide_home();
            } else {
                card_snap_axis(s_card, DRAG_AXIS_Y, 0, NULL);
                for (int i = 0; i < N_PLACEHOLDER_APPS; i++) {
                    if (!s_app_closed[i]) {
                        card_snap_axis(s_app_cards[i], DRAG_AXIS_Y, 0, NULL);
                    }
                }
            }
        }
    }
}

/* First panel widget in `cfg` for the given edge, or NULL if `cfg` is NULL (config failed to
 * load) or no widget describes that edge -- either way the caller falls back to the original
 * hardcoded default for just that one panel, not the whole shell. */
static const purr_ui_widget_t *find_panel(const purr_ui_screen_t *cfg, const char *edge)
{
    if (cfg == NULL) {
        return NULL;
    }
    for (int i = 0; i < cfg->widget_count; i++) {
        if (cfg->widgets[i].type == PURR_UI_WIDGET_PANEL &&
            strcmp(cfg->widgets[i].edge, edge) == 0) {
            return &cfg->widgets[i];
        }
    }
    return NULL;
}

/* First label widget in `cfg` with the given name, same NULL-means-fall-back contract as
 * find_panel() above. */
static const purr_ui_widget_t *find_label(const purr_ui_screen_t *cfg, const char *name)
{
    if (cfg == NULL) {
        return NULL;
    }
    for (int i = 0; i < cfg->widget_count; i++) {
        if (cfg->widgets[i].type == PURR_UI_WIDGET_LABEL &&
            strcmp(cfg->widgets[i].name, name) == 0) {
            return &cfg->widgets[i];
        }
    }
    return NULL;
}

/* First widget of the given type in `cfg` -- carousel/overview/minimize are each singleton
 * blocks (only one of each makes sense), so unlike panels/labels there's no name to match,
 * just the type. Same NULL-means-fall-back contract as find_panel()/find_label() above. */
static const purr_ui_widget_t *find_widget_type(const purr_ui_screen_t *cfg,
                                                 purr_ui_widget_type_t type)
{
    if (cfg == NULL) {
        return NULL;
    }
    for (int i = 0; i < cfg->widget_count; i++) {
        if (cfg->widgets[i].type == type) {
            return &cfg->widgets[i];
        }
    }
    return NULL;
}

static void build_card_shell(const purr_ui_screen_t *cfg)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    /* Found the hard way (chunk I-1): lv_scr_act()'s own object has LV_OBJ_FLAG_SCROLLABLE
     * set by default like any lv_obj_t, and LVGL computes a parent's scrollable content area
     * from every child's bounding box -- a real scrollbar showed up even with every child
     * positioned within the visible 320x240 area the first time this had a bug that put a
     * child off-screen. Kept cleared defensively; costs nothing. */
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Which edges have panels, their thickness/color/optional label text now come from
     * /ui/cardshell.conf (Hyprland-style: the engine below stays compiled C, only this
     * structure is data) -- each falls back independently to the original hardcoded default
     * if `cfg` is NULL or that specific edge isn't described, so a missing/corrupt config
     * can't leave the shell half-built. s_top_h/s_bottom_h/s_left_w are set from the SAME
     * resolved values the panels are actually built at, so the drag engine's clamp/snap math
     * always agrees with what's really on screen. */
    const purr_ui_widget_t *left_cfg = find_panel(cfg, "left");
    const purr_ui_widget_t *top_cfg = find_panel(cfg, "top");
    const purr_ui_widget_t *bottom_cfg = find_panel(cfg, "bottom");

    s_left_w = (left_cfg != NULL && left_cfg->h > 0) ? left_cfg->h : SHELL_LEFT_W;
    uint32_t left_color = (left_cfg != NULL && left_cfg->color != PURR_UI_COLOR_UNSET)
                         ? left_cfg->color : 0x1e5a4a;
    const char *left_label = (left_cfg != NULL && left_cfg->text[0] != '\0')
                            ? left_cfg->text : NULL;

    s_top_h = (top_cfg != NULL && top_cfg->h > 0) ? top_cfg->h : SHELL_TOP_H;
    uint32_t top_color = (top_cfg != NULL && top_cfg->color != PURR_UI_COLOR_UNSET)
                        ? top_cfg->color : 0x2a2a3a;
    const char *top_label = (top_cfg != NULL && top_cfg->text[0] != '\0')
                           ? top_cfg->text : "Notifications";

    s_bottom_h = (bottom_cfg != NULL && bottom_cfg->h > 0) ? bottom_cfg->h : SHELL_BOTTOM_H;
    uint32_t bottom_color = (bottom_cfg != NULL && bottom_cfg->color != PURR_UI_COLOR_UNSET)
                           ? bottom_cfg->color : 0x8a3fd1;
    const char *bottom_label = (bottom_cfg != NULL && bottom_cfg->text[0] != '\0')
                              ? bottom_cfg->text : NULL;

    /* Panels sit at their natural ON-SCREEN resting coordinate (not off-screen) -- hidden at
     * rest via LV_OBJ_FLAG_HIDDEN (panel_reveal(), above) rather than position or z-order, so
     * each panel's own position never changes and at most one is ever visible at a time: with
     * only one shown, the fact that they'd otherwise overlap each other in the two left-side
     * corners (each spans its own full natural edge -- top/bottom full width, left full
     * height) never actually matters. */
    s_panel_left = make_panel(scr, 0, 0, s_left_w, SHELL_H, left_color, left_label);
    populate_status_panel(s_panel_left);
    s_panel_top = make_panel(scr, 0, 0, SHELL_W, s_top_h, top_color, top_label);
    s_panel_bottom = make_panel(scr, 0, SHELL_H - s_bottom_h, SHELL_W, s_bottom_h,
                                bottom_color, bottom_label);
    populate_apps_panel(s_panel_bottom, s_bottom_h);
    panel_reveal(NULL);

    /* Home card: genuinely full screen, flush to all four edges. */
    lv_obj_t *card = lv_obj_create(scr);
    s_card = card;
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, 0, 0);
    lv_obj_set_size(card, SHELL_W, SHELL_H);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x141414), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, SHELL_CARD_RADIUS, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, card_drag_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(card, card_drag_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(card, card_drag_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(card, card_drag_cb, LV_EVENT_PRESS_LOST, NULL);

    const purr_ui_widget_t *title_cfg = find_label(cfg, "title");
    const char *title_text = (title_cfg != NULL && title_cfg->text[0] != '\0')
                            ? title_cfg->text : "[AdultSwim]";
    uint32_t title_color = (title_cfg != NULL && title_cfg->color != PURR_UI_COLOR_UNSET)
                          ? title_cfg->color : 0xFDA000;
    int title_scale = (title_cfg != NULL && title_cfg->scale > 0) ? title_cfg->scale : 2;
    int title_x = (title_cfg != NULL) ? title_cfg->x : 16;
    int title_y = (title_cfg != NULL) ? title_cfg->y : 12;

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, title_text);
    lv_obj_set_style_text_color(title, lv_color_hex(title_color), 0);
    const lv_font_t *f = font_for_scale(title_scale);
    if (f != NULL) {
        lv_obj_set_style_text_font(title, f, 0);
    }
    lv_obj_set_pos(title, title_x, title_y);

    /* Carousel/overview/minimize tuning, same independent-fallback contract as the panels
     * above -- each resolves from its own config block if present, else the compiled
     * default, so a config missing one block (or missing entirely) still gets a fully
     * working shell. */
    const purr_ui_widget_t *carousel_cfg = find_widget_type(cfg, PURR_UI_WIDGET_CAROUSEL);
    int carousel_capacity = (carousel_cfg != NULL && carousel_cfg->columns > 0)
                           ? carousel_cfg->columns : N_PLACEHOLDER_APPS;
    if (carousel_capacity > N_PLACEHOLDER_APPS) {
        /* N_PLACEHOLDER_APPS is the real hard ceiling (the arrays are sized to it) -- config
         * can only dial it down, never raise it. */
        carousel_capacity = N_PLACEHOLDER_APPS;
    }

    const purr_ui_widget_t *overview_cfg = find_widget_type(cfg, PURR_UI_WIDGET_OVERVIEW);
    s_overview_hold_ms = (overview_cfg != NULL && overview_cfg->anim_duration_ms > 0)
                        ? overview_cfg->anim_duration_ms : OVERVIEW_HOLD_MS;
    s_overview_focus_w = (overview_cfg != NULL && overview_cfg->w > 0)
                        ? overview_cfg->w : OVERVIEW_FOCUS_W;
    s_overview_focus_h = (overview_cfg != NULL && overview_cfg->h > 0)
                        ? overview_cfg->h : OVERVIEW_FOCUS_H;
    s_overview_step_x = (overview_cfg != NULL && overview_cfg->step_x > 0)
                       ? overview_cfg->step_x : OVERVIEW_STEP_X;
    s_overview_step_shrink_w = (overview_cfg != NULL && overview_cfg->step_shrink_w > 0)
                              ? overview_cfg->step_shrink_w : OVERVIEW_STEP_SHRINK_W;
    s_overview_step_shrink_h = (overview_cfg != NULL && overview_cfg->step_shrink_h > 0)
                              ? overview_cfg->step_shrink_h : OVERVIEW_STEP_SHRINK_H;
    s_overview_max_side_steps = (overview_cfg != NULL && overview_cfg->max_side_steps > 0)
                               ? overview_cfg->max_side_steps : OVERVIEW_MAX_SIDE_STEPS;
    s_overview_close_lift = (overview_cfg != NULL && overview_cfg->columns > 0)
                           ? overview_cfg->columns : OVERVIEW_CLOSE_LIFT;

    const purr_ui_widget_t *minimize_cfg = find_widget_type(cfg, PURR_UI_WIDGET_MINIMIZE);
    s_minimize_slide_ms = (minimize_cfg != NULL && minimize_cfg->anim_duration_ms > 0)
                         ? minimize_cfg->anim_duration_ms : MINIMIZE_SLIDE_MS;
    s_minimize_zone_h = (minimize_cfg != NULL && minimize_cfg->w > 0)
                       ? minimize_cfg->w : MINIMIZE_ZONE_H;
    s_minimize_zone_x_min = (minimize_cfg != NULL && minimize_cfg->x > 0)
                           ? minimize_cfg->x : MINIMIZE_ZONE_X_MIN;
    s_minimize_zone_x_max = (minimize_cfg != NULL && minimize_cfg->y > 0)
                           ? minimize_cfg->y : MINIMIZE_ZONE_X_MAX;

    const purr_ui_widget_t *gesture_cfg = find_widget_type(cfg, PURR_UI_WIDGET_GESTURE);
    s_gesture_tolerance_px = (gesture_cfg != NULL && gesture_cfg->columns > 0)
                            ? gesture_cfg->columns : GESTURE_TOLERANCE_PX;

    /* Right-side carousel (chunk I-4): placeholder "open app" cards, parked just off-screen
     * at x = SHELL_W*(i+1) until paged into view. Real content (actually-running AppRuntime
     * apps) is explicitly later work -- this gets the gesture and the full-row movement
     * right first, per "get the full UI look ready for building the backend". Reuses
     * make_panel() (same card styling/radius as every other card here) and attaches the
     * exact same card_drag_cb used by Home, which is why that callback never trusts
     * lv_event_get_target(e) -- whichever card is currently on screen has to drive the same
     * shared state. All N_PLACEHOLDER_APPS slots are always created (that's the compile-time
     * hard ceiling, never shrinks) -- anything past `carousel_capacity` just starts out
     * closed+hidden, reusing the exact same state a user closing an app from the
     * multitasking overview already leaves it in, instead of teaching every other loop in
     * this file a second "not really there" concept. */
    static const uint32_t app_colors[N_PLACEHOLDER_APPS] = {
        0x2d6a4f, 0x1d3f6e, 0x6e1d4f, 0x6e4f1d, 0x1d6e5f, 0x4f1d6e,
        0x6e2d2d, 0x2d4f6e, 0x5f6e1d, 0x6e1d1d, 0x3f6e1d, 0x1d2d6e,
    };
    static const char *app_labels[N_PLACEHOLDER_APPS] = {
        "App 1", "App 2", "App 3", "App 4", "App 5", "App 6",
        "App 7", "App 8", "App 9", "App 10", "App 11", "App 12",
    };
    for (int i = 0; i < N_PLACEHOLDER_APPS; i++) {
        lv_obj_t *app = make_panel(scr, SHELL_W * (i + 1), 0, SHELL_W, SHELL_H,
                                   app_colors[i], app_labels[i]);
        s_app_cards[i] = app;
        lv_obj_add_event_cb(app, card_drag_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(app, card_drag_cb, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(app, card_drag_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(app, card_drag_cb, LV_EVENT_PRESS_LOST, NULL);
        if (i >= carousel_capacity) {
            s_app_closed[i] = 1;
            lv_obj_add_flag(app, LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_current_page = 0;
}

static void *process_request(const purr_ui_request_t *req)
{
    switch (req->op) {
    case PURR_UI_OP_CLEAR_SCREEN:
        lv_obj_clean(lv_scr_act());
        return NULL;
    case PURR_UI_OP_SET_BACKGROUND:
        if (req->widget.color != PURR_UI_COLOR_UNSET) {
            lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(req->widget.color), 0);
            lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
        }
        return NULL;
    case PURR_UI_OP_ADD_WIDGET:
        return do_add_widget(&req->widget);
    case PURR_UI_OP_BUILD_CARD_SHELL:
        ESP_LOGI(TAG, "diag2: build_card_shell entering, cfg=%p", (void *)req->card_shell_screen);
        build_card_shell(req->card_shell_screen);
        ESP_LOGI(TAG, "diag2: build_card_shell returned");
        return NULL;
    default:
        ESP_LOGW(TAG, "unknown op %d", (int)req->op);
        return NULL;
    }
}

/* ---------------------------------------------------------------- the task */

static void service_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t next_ms = lv_timer_handler();
        if (next_ms == LV_NO_TIMER_READY || next_ms > LVGL_TICK_PERIOD_MS) {
            next_ms = LVGL_TICK_PERIOD_MS;
        }
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(next_ms);
        /* A do/while, not a for(;;) that checks the deadline BEFORE the first receive: when
         * LVGL reports next_ms == 0 (nothing scheduled "soon", which includes "idle, nothing
         * queued right now" -- not just "busy, call again instantly"), deadline == now
         * already, and checking the deadline first meant this loop broke without ever calling
         * xQueueReceive even once. That starved the queue forever the moment LVGL settled
         * into next_ms == 0 between frames -- found 2026-10-15 as the real cause of a
         * permanently stuck boot screen: purr_card_shell_start()'s request sat in s_cmd_q,
         * never picked up, while this task spun calling lv_timer_handler() with nothing left
         * to render (hence no further flush activity either -- it wasn't hung, it just never
         * looked at its own inbox again). xQueueReceive's own timeout of 0 when the deadline
         * has already passed is a legitimate non-blocking poll, not a bug -- the guarantee
         * this fixes is "check at least once," not "always wait the full next_ms." */
        do {
            TickType_t now = xTaskGetTickCount();
            TickType_t wait = (now >= deadline) ? 0 : (deadline - now);
            internal_msg_t msg;
            if (xQueueReceive(s_cmd_q, &msg, wait) != pdTRUE) {
                break;              /* nothing waiting -- time to tick LVGL again */
            }
            ESP_LOGI(TAG, "diag2: received op=%d", (int)msg.req.op);
            void *obj = process_request(&msg.req);
            ESP_LOGI(TAG, "diag2: replying op=%d", (int)msg.req.op);
            xQueueSend(msg.reply_q, &obj, portMAX_DELAY);
        } while (xTaskGetTickCount() < deadline);
    }
}

esp_err_t purr_ui_service_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    s_cmd_q = xQueueCreate(SERVICE_QUEUE_LEN, sizeof(internal_msg_t));
    if (s_cmd_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* Bumped from 4096: a real stack overflow on hardware (a ~1.6KB purr_app_registry_t
     * stack local, since fixed to be static -- but this task also carries LVGL's own call
     * overhead underneath every panel/widget-building function here, so a flat 4KB margin
     * was already thin before that bug and will keep being thin for any future addition of
     * similar size). A follow-up test with this reverted to 4096 looked like it hung with no
     * panic at all -- turned out to be a false signal: the device was sitting at a real
     * login prompt on its own screen (run_login() now always runs before LVGL), which only
     * a person at the physical keyboard can get past, not an automated serial reset. Not a
     * regression from this number; restored. */
    if (xTaskCreate(service_task, "ui_service", 8192, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = 1;
    ESP_LOGI(TAG, "up");
    return ESP_OK;
}

esp_err_t purr_ui_service_request(const purr_ui_request_t *req, void **out_obj)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    /* A fresh one-slot queue per call, not a shared static reply queue -- this function has
     * to be safe to call from several tasks at once once apps exist (chunk C), and a shared
     * reply queue would let one caller read another's answer. */
    QueueHandle_t reply_q = xQueueCreate(1, sizeof(void *));
    if (reply_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    internal_msg_t msg = {.req = *req, .reply_q = reply_q};
    if (xQueueSend(s_cmd_q, &msg, portMAX_DELAY) != pdTRUE) {
        vQueueDelete(reply_q);
        return ESP_FAIL;
    }
    void *obj = NULL;
    xQueueReceive(reply_q, &obj, portMAX_DELAY);
    vQueueDelete(reply_q);
    if (out_obj != NULL) {
        *out_obj = obj;
    }
    return ESP_OK;
}
