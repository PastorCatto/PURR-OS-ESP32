/*
 * PURR OS Mini - the small kernel of a remote display client (RemoteDisplay/SPEC.md, plan1.md). It boots under the same PURR bootloader as every other board (so the image is checked the same way),
 * has no recovery loader and no file system, and boots into the iPod UI. At power-up it quietly reconnects to the host it knows; the iPod home appears only when none answers (or on a first start).
 * Virtual display looks for a host, Wi-Fi chooses a network, Touch calibration maps the panel; Mini PDA (a basic desktop for a board with no host in reach) is not built yet.
 */
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "mini_health.h"
#include "mini_lcd.h"
#include "mini_link.h"
#include "mini_px.h"
#include "mini_touch.h"
#include "mini_ui.h"
#include "mini_wifi.h"

static const char *TAG = "purr-mini";

/* The saved network, quietly. true once the Wi-Fi is up. */
static bool join_saved(void)
{
    char ssid[33], pass[65];
    if (!mini_wifi_saved(ssid, sizeof(ssid), pass, sizeof(pass))) {
        return false;
    }
    mini_ui_status("Wi-Fi", "Joining the network", ssid);
    return mini_wifi_connect(ssid, pass, 15000);
}

/* Asks for a network and joins it. false if the user backed out or it did not join. */
static bool choose_network(void)
{
    mini_ap_t ap;
    if (!mini_ui_pick_network(&ap)) {
        return false;
    }
    char typed[65] = "";
    if (ap.secure && !mini_ui_password(ap.ssid, typed, sizeof(typed))) {
        return false;
    }
    mini_ui_status("Wi-Fi", "Joining the network", ap.ssid);
    if (mini_wifi_connect(ap.ssid, typed, 20000)) {
        mini_wifi_save(ap.ssid, typed);
        return true;
    }
    mini_ui_status("Wi-Fi", "Could not join", ap.ssid);
    vTaskDelay(pdMS_TO_TICKS(1800));
    return false;
}

static void say_and_wait(const char *title, const char *l1, const char *l2)
{
    mini_ui_status(title, l1, l2);
    int x, y;
    mini_ui_tap(&x, &y, 4000);
}

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    if (mini_lcd_init() != ESP_OK) {
        ESP_LOGE(TAG, "the panel did not start");
        return;
    }
    mini_lcd_backlight(1);
    if (mini_touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "the touch panel did not start");
    }
    mini_ui_boot(0, "hold BOOT now to set up the touch panel again");
    gpio_config_t boot = {.pin_bit_mask = 1ULL << 0, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&boot);
    bool redo = false;
    for (int i = 0; i < 20 && !redo; i++) {                      /* two seconds to hold the BOOT button */
        redo = gpio_get_level(0) == 0;
        mini_ui_boot(5 + i * 2, NULL);                           /* the bar moves while the two seconds run */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (redo) {
        mini_touch_cal_clear();
    }
    if (!mini_touch_cal_load()) {
        mini_ui_calibrate();                                     /* the first start, or a request: the panel's grid is mapped by four taps */
    }
    mini_ui_boot(50, NULL);
    if (mini_wifi_init() != ESP_OK) {
        mini_ui_status("PURR OS Mini", "Wi-Fi did not start", "restart the board");
        return;
    }
    mini_px_start();
    mini_ui_set_interrupt(mini_link_pair_interrupt);
    if (!mini_px_have_paired()) {
        mini_px_open_window(300);                                    /* nothing paired yet: a device may ask to pair for the first five minutes */
    }                                             /* the link: hosts in range, pairing, the sealed display stream */
    mini_ui_boot(100, NULL);
    vTaskDelay(pdMS_TO_TICKS(500));
    mini_mark_healthy();                                         /* the panel and Wi-Fi are up: this boot is a good one */

    /* power-up: quietly back to the host it knows; the menu either way (with one quiet line when none was found) */
    const char *strip = NULL;
    if (mini_link_have_last() && join_saved()) {
        int r = mini_link_reconnect(10000);
        for (int again = 0; r == MINI_LINK_LOST && again < 3; again++) {      /* it was there and went away: a few quiet tries before giving up */
            r = mini_link_reconnect(8000);
        }
        if (r == MINI_LINK_NOHOST || r == MINI_LINK_LOST) {
            strip = "Couldn't find your host. Pick Virtual display to look again.";
        }
    }

    for (;;) {
        char host[24] = "", ssid[33] = "", pass[65];
        purr_link_peer_t online;
        mini_home_t info = {.strip = strip};
        if (mini_px_paired_name(host, sizeof(host))) {
            info.host = host;
            info.host_online = mini_px_paired_host(&online);
        }
        if (mini_wifi_up() && mini_wifi_saved(ssid, sizeof(ssid), pass, sizeof(pass))) {
            info.wifi = ssid;
        }
        strip = NULL;
        switch (mini_ui_home(&info)) {
        case MINI_HOME_DISPLAY: {
            if (!mini_wifi_up() && !join_saved() && !choose_network()) {
                strip = "Choose a Wi-Fi network first.";
                break;
            }
            int r = mini_link_search();
            if (r == MINI_LINK_LOST) {
                strip = "The host went away.";
            }
            break;
        }
        case MINI_HOME_APPS:
            say_and_wait("Apps", "Coming soon", "apps from your host, drawn on this screen");
            break;
        case MINI_HOME_PDA:
            say_and_wait("Mini PDA", "Not in this version yet", "a basic desktop for a board with no host in reach");
            break;
        case MINI_HOME_WIFI:
            mini_wifi_disconnect();
            choose_network();
            break;
        case MINI_HOME_SETTINGS:
            switch (mini_ui_settings(mini_px_have_paired())) {
            case MINI_SET_FORGET:
                mini_px_forget_all();
                strip = "Paired devices forgotten.";
                break;
            case MINI_SET_CAL:
                mini_ui_calibrate();
                break;
            case MINI_SET_ABOUT: {
                char mem[40];
                snprintf(mem, sizeof(mem), "%u KB of internal memory free", (unsigned)(esp_get_free_internal_heap_size() / 1024));
                say_and_wait("About", "PURR OS Mini 1.2.0", mem);
                break;
            }
            default:
                break;
            }
            break;
        default:
            break;
        }
    }
}
