#include "purr_card_shell.h"

#include "esp_log.h"

#include "purr_ui_config.h"
#include "purr_ui_service.h"

static const char *TAG = "card_shell";

esp_err_t purr_card_shell_start(void)
{
    /* Loaded here, not in purr_ui_service.c: the service task's own rule is "don't touch
     * anything but LVGL state and the request queue" (purr_ui_service.h's own invariant),
     * and this is a filesystem read. purr_ui_service_request() blocks until the request is
     * handled, so this stack-local screen stays valid for the whole call -- build_card_shell()
     * never needs to keep the pointer around past it. A missing/corrupt config isn't fatal:
     * build_card_shell() falls back to its own built-in defaults, one panel/title at a time,
     * if `cfg` is NULL here. */
    purr_ui_screen_t screen;
    const purr_ui_screen_t *cfg = NULL;
    if (purr_ui_config_load("/ui/cardshell.conf", &screen) == ESP_OK) {
        cfg = &screen;
    } else {
        ESP_LOGW(TAG, "/ui/cardshell.conf missing or invalid; using built-in defaults");
    }

    purr_ui_request_t req = {.op = PURR_UI_OP_BUILD_CARD_SHELL, .card_shell_screen = cfg};
    return purr_ui_service_request(&req, NULL);
}
