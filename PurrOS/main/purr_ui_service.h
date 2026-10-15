/*
 * purr_ui_service.h - the `ui` catcall's backing service (chunk B of the full-stack plan:
 * Catcalls/SPEC.md's `ui` row, UI/SPEC.md section 2's "a UI service owns the library").
 *
 * LVGL is not thread-safe. One dedicated task owns every ongoing, cross-task-reachable
 * widget/drawing call -- runs lv_timer_handler(), and is where purr_ui_config.c's widget
 * construction now lives (moved here from that file). purr_lvgl.c still calls a few lv_*
 * functions of its own (lv_init(), the display driver registration, the flush callback) --
 * that's fine, not a violation of the same rule: those either run once at startup before
 * this task (or anything else) exists, or are invoked synchronously BY LVGL itself from
 * inside this task's own lv_timer_handler() call, never from a second thread. What this file
 * actually guards against is a THIRD caller -- the config builder today, an app's own task
 * later through the real `ui` catcall -- reaching into live widget state from somewhere
 * else. Those go through purr_ui_service_request() instead: post a request, block on a
 * reply queue, get back the resulting lv_obj_t* (opaque to the caller as void*, so this
 * header never has to include lvgl.h). This is UI/SPEC.md section 4's own testing rule ("no
 * app call reaches LVGL outside the service"), proven here against the one real caller that
 * exists today (the config builder), before any app exists to be a second one.
 *
 * Deliberately small: exactly the operations purr_ui_config.c's builder already needed
 * before this existed (clear the screen, set its background, add one widget). Button taps
 * are logged for now, not delivered anywhere -- real per-app event queues are chunk C's job,
 * once there is an app's own task to deliver them to.
 */
#ifndef PURR_UI_SERVICE_H
#define PURR_UI_SERVICE_H

#include "esp_err.h"
#include "purr_ui_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PURR_UI_OP_CLEAR_SCREEN,     /* wipes the active screen and resets its background */
    PURR_UI_OP_SET_BACKGROUND,   /* widget.color is the new background */
    PURR_UI_OP_ADD_WIDGET,       /* widget describes what to create, same shape purr_ui_
                                    config.h's parser already fills in */
    /* Builds the card-based Home shell (purr_card_shell.c, memory/
     * [[project_card_shell_ui_redesign]]'s gesture-UI redesign) -- gesture state, rounded
     * card containers and coordinated multi-object positioning don't fit the declarative
     * label/button/image/app_grid/navbar widget model above, so this is real construction
     * code living here (same invariant as everything else in this file: only the service
     * task calls lv_*), not a config block on its own. The *structure* (which edges have
     * panels, their size/color, the title) now comes from `card_shell_screen` below, parsed
     * from /ui/cardshell.conf by purr_card_shell_start() -- the engine driving it (drag/
     * reveal/carousel/overview/minimize) is still compiled C. */
    PURR_UI_OP_BUILD_CARD_SHELL,
} purr_ui_op_t;

typedef struct {
    purr_ui_op_t op;
    purr_ui_widget_t widget;     /* PURR_UI_OP_ADD_WIDGET / SET_BACKGROUND (widget.color) */
    /* PURR_UI_OP_BUILD_CARD_SHELL only. May be NULL (build_card_shell() falls back to its own
     * built-in defaults if so, e.g. the config failed to load) -- the caller owns this and
     * purr_ui_service_request() blocks until the request is handled, so a stack-local pointer
     * is safe to pass here. */
    const purr_ui_screen_t *card_shell_screen;
} purr_ui_request_t;

/* Starts the service task. Call once, after purr_lvgl_init() succeeds and before any call
 * to purr_ui_service_request() -- main.c does both right next to each other. */
esp_err_t purr_ui_service_start(void);

/* Posts `req` to the service task and blocks for the result. `*out_obj` (may be NULL if the
 * caller doesn't need it) receives the created widget as an opaque handle -- NULL on
 * CLEAR_SCREEN/SET_BACKGROUND (nothing to hand back), or if PURR_UI_OP_ADD_WIDGET's widget
 * type isn't recognized (same "skip it, don't fail the screen" rule purr_ui_config.c's
 * parser already applies to unknown widget types). Safe to call from any task once the
 * service is running -- the whole point of routing through a queue instead of calling lv_*
 * directly. */
esp_err_t purr_ui_service_request(const purr_ui_request_t *req, void **out_obj);

#ifdef __cplusplus
}
#endif

#endif /* PURR_UI_SERVICE_H */
