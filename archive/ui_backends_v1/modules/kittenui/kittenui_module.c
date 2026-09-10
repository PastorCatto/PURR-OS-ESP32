// kittenui_module.c — KittenUI kernel module entry
//
// Reads theme from NVS (namespace "purr_settings", key "theme"), falls back to WCE Classic.
// Supported theme IDs: "wce", "dark" (built-in); any registered via
// kittenui_register_theme() also works if the module is loaded first.

#include "kittenui.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "kittenui";

extern void kittenui_win_register(void);

// ── Theme registry ────────────────────────────────────────────────────────────

#define MAX_THEMES 16

static const kittenui_theme_t *s_themes[MAX_THEMES];
static int                     s_theme_count = 0;
static const kittenui_theme_t *s_active      = NULL;

void kittenui_register_theme(const kittenui_theme_t *theme)
{
    if (!theme) return;
    // Replace if same id already registered
    for (int i = 0; i < s_theme_count; i++) {
        if (strcmp(s_themes[i]->id, theme->id) == 0) {
            s_themes[i] = theme;
            ESP_LOGI(TAG, "theme updated: %s", theme->name);
            return;
        }
    }
    if (s_theme_count < MAX_THEMES) {
        s_themes[s_theme_count++] = theme;
        ESP_LOGI(TAG, "theme registered: %s (%s)", theme->name, theme->id);
    }
}

void kittenui_apply_theme(void)
{
    if (!s_active) return;
    kittenui_apply_theme_styles();
    ESP_LOGI(TAG, "theme applied: %s", s_active->name);
}

const kittenui_theme_t *kittenui_active_theme(void) { return s_active; }

// ── Theme style application ───────────────────────────────────────────────────

// Global styles owned by KittenUI — applied to lv_obj_get_style_* defaults
static lv_style_t s_style_screen;
static lv_style_t s_style_btn;
static lv_style_t s_style_btn_pressed;
static lv_style_t s_style_label;
static lv_style_t s_style_list_item;
static lv_style_t s_style_list_item_selected;

void kittenui_apply_theme_styles(void)
{
    const kittenui_theme_t *t = s_active;
    if (!t) return;
    const kittenui_palette_t *p = &t->palette;
    const kittenui_style_flags_t *f = &t->flags;
    const lv_font_t *font_body = t->fonts.body ? t->fonts.body : LV_FONT_DEFAULT;

    // Screen / window background (applied to screen objects by kittenui_desktop_init)
    lv_style_reset(&s_style_screen);
    lv_style_set_bg_color(&s_style_screen, p->window_bg);
    lv_style_set_bg_opa(&s_style_screen, LV_OPA_COVER);
    lv_style_set_text_color(&s_style_screen, p->text);
    lv_style_set_text_font(&s_style_screen, font_body);

    // Button normal
    lv_style_reset(&s_style_btn);
    lv_style_set_bg_color(&s_style_btn, p->surface);
    lv_style_set_bg_opa(&s_style_btn, LV_OPA_COVER);
    lv_style_set_text_color(&s_style_btn, p->text);
    lv_style_set_text_font(&s_style_btn, font_body);
    lv_style_set_border_color(&s_style_btn, p->border);
    lv_style_set_border_width(&s_style_btn, 1);
    lv_style_set_radius(&s_style_btn, f->corner_radius);
    lv_style_set_pad_all(&s_style_btn, f->padding);

    if (f->raised_buttons) {
        // Simulate 3D raised: outline top/left lighter, bottom/right darker
        lv_style_set_outline_color(&s_style_btn, p->border_light);
        lv_style_set_outline_width(&s_style_btn, 1);
        lv_style_set_shadow_color(&s_style_btn, p->border_dark);
        lv_style_set_shadow_width(&s_style_btn, 2);
        lv_style_set_shadow_ofs_x(&s_style_btn, 1);
        lv_style_set_shadow_ofs_y(&s_style_btn, 1);
    }

    // Button pressed
    lv_style_reset(&s_style_btn_pressed);
    lv_style_set_bg_color(&s_style_btn_pressed, p->selected);
    lv_style_set_text_color(&s_style_btn_pressed, p->selected_text);
    lv_style_set_radius(&s_style_btn_pressed, f->corner_radius);
    lv_style_set_border_color(&s_style_btn_pressed, p->border_dark);
    lv_style_set_pad_all(&s_style_btn_pressed, f->padding);

    // List item normal
    lv_style_reset(&s_style_list_item);
    lv_style_set_bg_color(&s_style_list_item, p->surface);
    lv_style_set_text_color(&s_style_list_item, p->text);
    lv_style_set_text_font(&s_style_list_item, font_body);
    lv_style_set_min_height(&s_style_list_item, f->item_height);
    lv_style_set_pad_all(&s_style_list_item, f->padding);

    // List item selected
    lv_style_reset(&s_style_list_item_selected);
    lv_style_set_bg_color(&s_style_list_item_selected, p->selected);
    lv_style_set_text_color(&s_style_list_item_selected, p->selected_text);
    lv_style_set_min_height(&s_style_list_item_selected, f->item_height);
    lv_style_set_pad_all(&s_style_list_item_selected, f->padding);

    // Let the theme's apply_fn do anything else (gradients, custom widgets, etc.)
    if (t->apply_fn) t->apply_fn(t);
}

// ── NVS theme loader ──────────────────────────────────────────────────────────

static const kittenui_theme_t *load_theme_from_nvs(void)
{
    nvs_handle_t nvs;
    char id[32] = {0};
    if (nvs_open("purr_settings", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(id);
        nvs_get_str(nvs, "theme", id, &len);
        nvs_close(nvs);
    }
    if (!id[0]) return NULL;

    for (int i = 0; i < s_theme_count; i++) {
        if (strcmp(s_themes[i]->id, id) == 0)
            return s_themes[i];
    }
    ESP_LOGW(TAG, "theme '%s' from NVS not found — using default", id);
    return NULL;
}

// ── LVGL handler task ─────────────────────────────────────────────────────────

static TaskHandle_t s_task = NULL;

#include "kittenui_desktop.h"

static void kittenui_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "task started");
    kittenui_apply_theme_styles();
    ESP_LOGI(TAG, "theme applied, starting desktop");
    kittenui_desktop_init();
    ESP_LOGI(TAG, "desktop init done");
    uint32_t tick = 0;
    while (1) {
        // See purr_kernel_ui_lock()'s doc comment — this is what keeps an
        // app's background-task UI update (e.g. about.c's 5s refresh) from
        // racing this render loop. purr_win.h's dispatch macros take the
        // same lock from whatever task calls them.
        purr_kernel_ui_lock();
        lv_tick_inc(5);
        lv_timer_handler();
        if (++tick % 1000 == 0) kittenui_desktop_tick();
        purr_kernel_ui_unlock();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ── Module lifecycle ──────────────────────────────────────────────────────────

int kittenui_init(void)
{
#ifndef CONFIG_PURR_UI_BACKEND_KITTENUI
    ESP_LOGI(TAG, "KittenUI built-in but not selected for this device — skipping");
    return 0;
#endif

    if (purr_kernel_ui()) {
        ESP_LOGW(TAG, "UI catcall already registered — skipping KittenUI");
        return 0;
    }

    // Register built-in themes
    kittenui_register_theme(kittenui_theme_wce());
    kittenui_register_theme(kittenui_theme_dark());

    // Determine active theme: NVS → fallback to WCE
    s_active = load_theme_from_nvs();
    if (!s_active) s_active = kittenui_theme_wce();

    ESP_LOGI(TAG, "active theme: %s", s_active->name);

    if (kittenui_hal_init() != 0) return -1;
    ESP_LOGI(TAG, "HAL done, creating task");

    // LVGL handler task — separate from tick task created in HAL
    // Theme styles are applied inside the task (see kittenui_task above)
    BaseType_t ret = xTaskCreatePinnedToCore(kittenui_task, "kittenui", 32768, NULL, 4, &s_task, 1);
    ESP_LOGI(TAG, "xTaskCreate ret=%d", (int)ret);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create kittenui task");
        return -1;
    }

    kittenui_win_register();
    ESP_LOGI(TAG, "win registered");

    ESP_LOGI(TAG, "KittenUI ready (%ux%u, theme=%s)",
             kittenui_hal_width(), kittenui_hal_height(), s_active->name);
    return 0;
}

void kittenui_deinit(void)
{
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
    // lv_deinit() only exists when LV_MEM_CUSTOM is off (or GC is on) — see
    // lv_obj.c's matching guard.
#if LV_ENABLE_GC || !LV_MEM_CUSTOM
    lv_deinit();
#endif
}

// ── Module header ─────────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(kittenui) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_UI,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "kittenui",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = CATCALL_FLAG_DISPLAY,
    .init              = kittenui_init,
    .deinit            = kittenui_deinit,
};
