/* oobe_ui.c - see oobe_ui.h. */
#include "oobe_ui.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_PURR_BOARD_COLOR_LCD

#include "lvgl.h"

#include "oobe_job.h"
#include "purr_lvgl.h"
#include "purr_ui_service.h"
#if CONFIG_PURR_BOARD_PLAYTIME
#include "purr_tp_priv.h"
#endif
#include "purr_w10m_theme.h"

static const char *TAG = "oobe";

int oobe_ui_available(void) { return 1; }

typedef struct {
    int part, start_page;
    const purr_oobe_ops_t *ops;
} start_t;

static void *build_cb(void *arg)
{
    start_t *s = arg;
    purr_theme_init();                              /* the same theme as the shell and Settings */
    purr_oobe_start(s->part, s->ops, s->start_page);
#if CONFIG_PURR_BOARD_PLAYTIME
    tp_nav_init();                                  /* the d-pad: purr_lvgl_init() installed the key hook; this starts the highlight */
#endif
    return NULL;
}

/* The wizard asks for its page's text boxes to receive the hardware keyboard. */
static void use_keypad(void *group)
{
    lv_indev_t *kb = (lv_indev_t *)purr_lvgl_keypad();
    if (kb != NULL) {
        lv_indev_set_group(kb, (lv_group_t *)group);
    }
}

int oobe_ui_run(int part, const purr_oobe_ops_t *ops, int start_page)
{
    if (purr_lvgl_init() != ESP_OK) {
        ESP_LOGW(TAG, "LVGL did not start; staying with the text screens");
        return -1;
    }
    static purr_oobe_ops_t s_ops;
    s_ops = *ops;
    s_ops.use_keypad = use_keypad;
    start_t s = {part, start_page, &s_ops};
    if (purr_ui_service_run(build_cb, &s, NULL) != ESP_OK) {
        return -1;
    }
    ESP_LOGI(TAG, "setup wizard part %d is up; internal RAM free %u bytes, largest block %u", part,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    oobe_job_host_run();                            /* the main task has nothing else to do: it runs the install jobs */
}

#else  /* not a colour T-Deck */

int oobe_ui_available(void) { return 0; }
int oobe_ui_run(int part, const purr_oobe_ops_t *ops, int start_page) { (void)part; (void)ops; (void)start_page; return -1; }

#endif
