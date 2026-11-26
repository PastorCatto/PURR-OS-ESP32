/* purr_link.c - see purr_link.h. */
#include "purr_link.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "purr_driver.h"
#include "purr_shell.h"
#include "purr_ui_service.h"

static const char *TAG = "link";

static SemaphoreHandle_t s_lock;
static const purr_link_ops_t *s_ops;
static void *s_h;

/* The driver, found by class each time it might have changed (it can be loaded after boot, or replaced by an update). */
static int find_driver(void)
{
    void *h = NULL;
    const purr_link_ops_t *ops = purr_class_ops(PURR_DRV_CLASS_LINK, &h);
    if (ops != s_ops || h != s_h) {
        s_ops = ops;
        s_h = h;
        if (ops != NULL) {
            ESP_LOGI(TAG, "proximity driver found");
        }
    }
    return s_ops != NULL;
}

static int take(void)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(300)) != pdTRUE) {
        return 0;
    }
    if (!find_driver()) {
        xSemaphoreGive(s_lock);
        return 0;
    }
    return 1;
}

static void give(void)
{
    xSemaphoreGive(s_lock);
}

int purr_link_present(void)
{
    if (!take()) {
        return 0;
    }
    give();
    return 1;
}

#define CALL(expr)                \
    do {                          \
        if (!take()) {            \
            return -1;            \
        }                         \
        int r_ = (expr);          \
        give();                   \
        return r_;                \
    } while (0)

int purr_link_self(uint8_t id[8], char name[17]) { CALL(s_ops->self(s_h, id, name)); }
int purr_link_nearby(purr_link_peer_t *out, int max) { CALL(s_ops->nearby(s_h, out, max)); }
int purr_link_paired(purr_link_peer_t *out, int max) { CALL(s_ops->paired(s_h, out, max)); }
int purr_link_pair(const uint8_t id[8]) { CALL(s_ops->pair(s_h, id)); }
int purr_link_forget(const uint8_t id[8]) { CALL(s_ops->forget(s_h, id)); }
int purr_link_nets(char ssid[][33], int max) { CALL(s_ops->nets(s_h, ssid, max)); }
int purr_link_forget_net(const char *ssid) { CALL(s_ops->forget_net(s_h, ssid)); }
int purr_link_option(int opt, int value) { CALL(s_ops->option(s_h, opt, value)); }
int purr_link_connect(const uint8_t id[8]) { CALL(s_ops->connect(s_h, id)); }
int purr_link_send(const uint8_t id[8], uint8_t channel, const uint8_t *data, size_t len) { CALL(s_ops->send(s_h, id, channel, data, len)); }
int purr_link_recv(const uint8_t id[8], uint8_t *channel, uint8_t *buf, size_t cap) { CALL(s_ops->recv(s_h, id, channel, buf, cap)); }
int purr_link_share_wifi(const uint8_t id[8], const char *ssid) { CALL(s_ops->share_wifi(s_h, id, ssid)); }

int purr_link_has_peers(void) { CALL(s_ops->version >= 2 ? s_ops->has_peers(s_h) : 0); }
int purr_link_stream_accept(const uint8_t *hello1, size_t n, uint8_t *hello2, size_t cap, uint8_t peer_id[8]) { CALL(s_ops->version >= 2 ? s_ops->stream_accept(s_h, hello1, n, hello2, cap, peer_id) : -1); }
int purr_link_stream_seal(int stream, uint8_t channel, const uint8_t *pt, size_t n, uint8_t *out, size_t cap) { CALL(s_ops->version >= 2 ? s_ops->stream_seal(s_h, stream, channel, pt, n, out, cap) : -1); }
int purr_link_stream_open(int stream, const uint8_t *rec, size_t n, uint8_t *channel, uint8_t *pt, size_t cap) { CALL(s_ops->version >= 2 ? s_ops->stream_open(s_h, stream, rec, n, channel, pt, cap) : -1); }

void purr_link_stream_close(int stream)
{
    if (take()) {
        if (s_ops->version >= 2) {
            s_ops->stream_close(s_h, stream);
        }
        give();
    }
}

/* ---------------------------------------------------------------- the pairing question */

/* A modal on the top layer, in every shell the same: it says who, shows the six digits, and asks. Both devices show the same digits; a person checks they match. */
static lv_obj_t *s_dlg;
static volatile int s_dlg_shown;
static uint8_t s_dlg_id[8];

static void answer_cb(lv_event_t *e)
{
    int yes = (int)(intptr_t)lv_event_get_user_data(e);
    if (take()) {
        s_ops->answer(s_h, yes);
        give();
    }
    if (s_dlg != NULL) {
        lv_obj_del(s_dlg);
        s_dlg = NULL;
    }
    s_dlg_shown = 0;
}

static void *show_cb(void *arg)
{
    const purr_link_prompt_t *p = arg;
    if (s_dlg != NULL) {
        lv_obj_del(s_dlg);
    }
    s_dlg = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_dlg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_dlg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_dlg, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_dlg, 0, 0);
    lv_obj_set_style_radius(s_dlg, 0, 0);
    lv_obj_set_flex_flow(s_dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_dlg, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_dlg, 8, 0);
    lv_obj_clear_flag(s_dlg, LV_OBJ_FLAG_SCROLLABLE);

    char line[64];
    snprintf(line, sizeof(line), "Pair with %s?", p->name);
    lv_obj_t *t = lv_label_create(s_dlg);
    lv_label_set_text(t, line);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);

    char code[16];
    if (p->code >= 0) {
        snprintf(code, sizeof(code), "%03d %03d", p->code / 1000, p->code % 1000);
    } else {
        snprintf(code, sizeof(code), "...");
    }
    lv_obj_t *c = lv_label_create(s_dlg);
    lv_label_set_text(c, code);
    lv_obj_set_style_text_color(c, lv_color_white(), 0);
    lv_obj_set_style_text_letter_space(c, 4, 0);
    lv_obj_set_style_transform_zoom(c, 384, 0);                      /* a larger rendering of the same font: the digits are what the person compares */
    lv_obj_set_style_pad_all(c, 10, 0);

    lv_obj_t *n = lv_label_create(s_dlg);
    lv_label_set_text(n, "The same six digits must show on the other device.");
    lv_obj_set_style_text_color(n, lv_color_make(0xB0, 0xB0, 0xB0), 0);

    lv_obj_t *row = lv_obj_create(s_dlg);
    lv_obj_set_size(row, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_btn_create(row);
        lv_obj_set_size(b, 110, 44);
        lv_obj_add_event_cb(b, answer_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i == 0));
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, i == 0 ? "Pair" : "Cancel");
        lv_obj_center(l);
    }
    memcpy(s_dlg_id, p->id, 8);
    return NULL;
}

static void *hide_cb(void *arg)
{
    (void)arg;
    if (s_dlg != NULL) {
        lv_obj_del(s_dlg);
        s_dlg = NULL;
    }
    return NULL;
}

/* ---------------------------------------------------------------- the pump */

static void link_task(void *arg)
{
    (void)arg;
    uint32_t last_notice = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30));
        if (!take()) {
            vTaskDelay(pdMS_TO_TICKS(500));                 /* no driver (yet): look again in a moment */
            continue;
        }
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        s_ops->tick(s_h, now);
        purr_link_prompt_t p;
        memset(&p, 0, sizeof(p));
        int has = s_ops->prompt(s_h, &p);
        char note[64] = "";
        int got_note = 0;
        if (now - last_notice > 2000) {
            last_notice = now;
            got_note = s_ops->notice(s_h, note);
        }
        give();

        if (has && p.kind == PURR_LINK_PROMPT_PAIR) {
            if (!s_dlg_shown || memcmp(s_dlg_id, p.id, 8) != 0) {
                s_dlg_shown = 1;
                purr_ui_service_run(show_cb, &p, NULL);
            }
        } else if (s_dlg_shown && s_dlg != NULL) {
            /* the question is gone (the other person said no, or it timed out) */
            purr_ui_service_run(hide_cb, NULL, NULL);
            s_dlg_shown = 0;
        }
        if (got_note > 0) {
            purr_shell_notify("Wi-Fi added", note);
        }
    }
}

void purr_link_start(void)
{
    if (s_lock != NULL) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    /* An internal stack, not PSRAM: the driver saves its pairings to NVS from this task, and a flash write turns the cache off, which a PSRAM stack cannot survive (found on the device, 2026-11-25). */
    xTaskCreate(link_task, "purr_link", 6144, NULL, 3, NULL);
}
