#include "purr_ui_service.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "commands.h"
#include "purr_lvgl.h"
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

/* lv_image_set_src() just stores the pointer it's given -- the lv_image_dsc_t it points to has
 * to outlive the lv_obj_t. Indexed by request sequence number modulo the pool, which is fine
 * for the config builder's one-shot "create everything once" use; an app that keeps creating
 * images after the pool wraps would start reusing slots still in use by an older image --
 * acceptable for now (no app does this yet), worth revisiting once one does. */
#define IMG_DESC_POOL 32
static lv_image_dsc_t s_img_descs[IMG_DESC_POOL];
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
    lv_image_dsc_t *d = &s_img_descs[s_img_desc_next];
    s_img_desc_next = (s_img_desc_next + 1) % IMG_DESC_POOL;
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    int alpha = asset->color_format == PURR_ASSET_RGB565A8;
    d->header.cf = alpha ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565;
    d->header.flags = 0;
    d->header.w = asset->width;
    d->header.h = asset->height;
    d->header.stride = (uint32_t)asset->width * 2;
    d->data_size = (uint32_t)asset->width * asset->height * (alpha ? 3 : 2);
    d->data = (const uint8_t *)asset->pixels;

    lv_obj_t *obj = lv_image_create(scr);
    lv_image_set_src(obj, d);
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
    case PURR_UI_OP_CALL:
        return req->call_fn ? req->call_fn(req->call_arg) : NULL;
    default:
        ESP_LOGW(TAG, "unknown op %d", (int)req->op);
        return NULL;
    }
}

/* ---------------------------------------------------------------- the task */

static volatile int s_paused;

static void *pause_cb(void *arg)
{
    int on = (int)(intptr_t)arg;
    if (!on && s_paused) {
        lv_obj_invalidate(lv_screen_active());          /* the panel was used by something else: repaint every layer */
        lv_obj_invalidate(lv_layer_top());
        lv_obj_invalidate(lv_layer_sys());
    }
    s_paused = on;
    return NULL;
}

static void *repaint_cb(void *arg)
{
    (void)arg;
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(lv_layer_sys());
    lv_refr_now(NULL);                                  /* draw it now, not on the next timer pass: the caller lights the backlight next */
    return NULL;
}

void purr_ui_service_repaint(void)
{
    if (strcmp(pcTaskGetName(NULL), "ui_service") == 0) {
        repaint_cb(NULL);                               /* already the UI task: running through its own queue would wait for itself */
    } else {
        purr_ui_service_run(repaint_cb, NULL, NULL);
    }
}

void purr_ui_service_pause(int on)
{
    purr_ui_service_run(pause_cb, (void *)(intptr_t)on, NULL);
}

static void service_task(void *arg)
{
    (void)arg;
    for (;;) {
        purr_lvgl_guard();                              /* an overwritten input callback is put back before LVGL calls it (plan7.md) */
        {
            static int64_t s_hwm_log_us;
            int64_t now_us = esp_timer_get_time();
            if (now_us - s_hwm_log_us > 30000000) {         /* the least stack this task has ever had left, every 30 seconds (1.0.28: about 5.6 KB of the 12 KB had been used at the deepest, so this is not what overwrote the input device, plan7.md) */
                s_hwm_log_us = now_us;
                ESP_LOGI(TAG, "stack: at least %u bytes have never been used", (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
        }
        uint32_t next_ms = s_paused ? LVGL_TICK_PERIOD_MS : lv_timer_handler();
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
         * permanently stuck boot screen: the first shell request sat in s_cmd_q,
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
    if (xTaskCreate(service_task, "ui_service", 12288, NULL, 5, NULL) != pdPASS) {
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

esp_err_t purr_ui_service_run(void *(*fn)(void *), void *arg, void **result)
{
    purr_ui_request_t req = {.op = PURR_UI_OP_CALL, .call_fn = fn, .call_arg = arg};
    return purr_ui_service_request(&req, result);
}
