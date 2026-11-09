/*
 * purr_catrt_game.c - the `game` catcall: the screen and the buttons for an app of kind `game`.
 *
 * Ownership follows the app's place in the runtime, not a call the game makes: when the game is brought to the front the shell stops drawing (LVGL
 * is paused) and the keyboard controller goes into raw mode so the game can see which keys are held; when it leaves the front, or stops, both are
 * handed back and the shell repaints. A game that is frozen or in the background can therefore never draw over the shell. A watchdog independent of
 * the game watches the menu button: held for a second, the game is sent back to the start screen, so a game that hangs can always be left.
 */
#include "purr_catrt_game.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "purr_catcalls.h"
#include "purr_catrt.h"
#include "purr_display.h"
#include "purr_driver.h"
#include "purr_kernel.h"
#include "purr_power.h"
#include "purr_power_plat.h"
#include "purr_runtime.h"
#include "purr_ui_service.h"

static const char *TAG = "game";

#define RAW_DEV "kbdraw"
#define MENU_HOLD_MS 1000
#define WATCH_MS 50

static int s_owner = -1;                        /* the slot that owns the screen, or -1 */
static int s_raw;                               /* the keyboard is in raw mode */
static int s_menu_ms;
static esp_timer_handle_t s_watch;

/* The T-Deck and the T-Deck Plus: which bit of the keyboard controller's raw bitmap is which button (from sdk/boards/tdeck_plus.toml [game.keys]; Retro-Go's target
 * uses the same numbers). A board without a table has no buttons yet. TODO: generate this table from the board file with the rest of the board. */
#if CONFIG_PURR_BOARD_TDECK_PLUS || CONFIG_PURR_BOARD_TDECK      /* the original T-Deck has the same keyboard controller and trackball */
#define MENU_GPIO 0                             /* the BOOT button, which the trackball's click shares */
static const struct { uint8_t num; uint32_t btn; } k_keys[] = {
    {18, PURR_GBTN_UP}, {17, PURR_GBTN_RIGHT}, {20, PURR_GBTN_DOWN}, {19, PURR_GBTN_LEFT},
    {7, PURR_GBTN_SELECT}, {30, PURR_GBTN_START}, {31, PURR_GBTN_OPTION},
    {1, PURR_GBTN_A}, {3, PURR_GBTN_B}, {0, PURR_GBTN_X}, {21, PURR_GBTN_Y}, {24, PURR_GBTN_L}, {14, PURR_GBTN_R},
};
#else
#define MENU_GPIO (-1)
static const struct { uint8_t num; uint32_t btn; } k_keys[] = {{0, 0}};
#endif

static int menu_down(void)
{
    return MENU_GPIO >= 0 && gpio_get_level((gpio_num_t)MENU_GPIO) == 0;
}

static int raw_mode(int on)
{
    uint8_t b[2] = {0xB1, (uint8_t)on};
    if (purr_device_state(RAW_DEV) != PURR_DEV_BOUND || purr_device_read(RAW_DEV, b, sizeof(b)) != 1) {
        return -1;
    }
    return 0;
}

static void watch_cb(void *arg)
{
    (void)arg;
    if (s_owner < 0) {
        return;
    }
    s_menu_ms = menu_down() ? s_menu_ms + WATCH_MS : 0;
    if (s_menu_ms >= MENU_HOLD_MS) {
        s_menu_ms = 0;
        ESP_LOGI(TAG, "menu held: back to the start screen");
        purr_catrt_home();
    }
}

static void take(int slot)
{
    if (s_owner == slot) {
        return;
    }
    purr_ui_service_pause(1);
    s_owner = slot;
    /* A game's keys never pass through the shell's input path, so the idle timeout would turn the screen off under it (and with the shell paused nothing
     * could wake it: it looks like a hard lock). While a game has the screen, the screen stays on; the lock is dropped in release(). */
    purr_power_sys_hold(slot, PURR_WAKE_SCREEN, 0);
    s_raw = raw_mode(1) == 0;
    if (!s_raw) {
        ESP_LOGW(TAG, "no raw keyboard (the kbd-raw driver is not loaded): this game has no buttons");
    }
    s_menu_ms = 0;
    if (s_watch == NULL) {
        const esp_timer_create_args_t a = {.callback = watch_cb, .name = "game-menu"};
        if (esp_timer_create(&a, &s_watch) != ESP_OK) {
            s_watch = NULL;
        }
    }
    if (s_watch != NULL) {
        esp_timer_start_periodic(s_watch, WATCH_MS * 1000);
    }
    ESP_LOGI(TAG, "slot %d has the screen", slot);
}

static void release(void)
{
    if (s_owner < 0) {
        return;
    }
    if (s_watch != NULL) {
        esp_timer_stop(s_watch);
    }
    if (s_raw) {
        raw_mode(0);
        s_raw = 0;
    }
    int was = s_owner;
    s_owner = -1;
    purr_power_sys_release(was, PURR_WAKE_SCREEN);
    purr_ui_service_pause(0);
    ESP_LOGI(TAG, "slot %d gave the screen back", was);
}

/* Called by the runtime after a game's RESUME or PAUSE callback returned, and when it is torn down. */
static void lifecycle(int slot, int ev)
{
    if (ev == PURR_RT_EV_RESUME) {
        take(slot);
    } else if (s_owner == slot) {
        release();
    }
}

/* ---------------------------------------------------------------- the catcall */

static int cc_lcd_write(int x, int y, int w, int h, const uint16_t *pixels)
{
    if (pixels == NULL || purr_catrt_caller(NULL, 0) != s_owner || s_owner < 0) {
        return -1;
    }
    const purr_display_v2_t *d = purr_kernel_display();
    return d != NULL && d->blit(x, y, w, h, pixels) == ESP_OK ? 0 : -1;
}

static uint32_t cc_buttons(void)
{
    if (purr_catrt_caller(NULL, 0) != s_owner || s_owner < 0) {
        return 0;
    }
    uint32_t mask = menu_down() ? PURR_GBTN_MENU : 0;
    uint8_t b[6] = {0xB2};
    if (s_raw && purr_device_read(RAW_DEV, b, sizeof(b)) == 5) {
        /* Retro-Go's layout of the five bytes: 32 bits, a bit per key number. */
        uint32_t word = ((uint32_t)b[1] << 25) | ((uint32_t)b[2] << 18) | ((uint32_t)b[3] << 11) | ((uint32_t)(b[4] & 0xF8) << 4) | b[5];
        for (size_t i = 0; i < sizeof(k_keys) / sizeof(k_keys[0]); i++) {
            if (k_keys[i].btn != 0 && (word & (1u << k_keys[i].num)) != 0) {
                mask |= k_keys[i].btn;
            }
        }
    }
    return mask;
}

static uint64_t cc_ticks_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

static void cc_quit(void)
{
    int slot = purr_catrt_caller(NULL, 0);
    if (slot >= 0) {
        purr_catrt_close_slot(slot);
    }
}

static purr_cc_game_t s_cc_game = {
    .version = 1, .lcd_write = cc_lcd_write, .buttons = cc_buttons, .ticks_us = cc_ticks_us, .quit = cc_quit,
};

void purr_catrt_game_init(void)
{
    const purr_display_v2_t *d = purr_kernel_display();
    purr_display_info_t info;
    if (d != NULL && d->get_info(&info) == ESP_OK) {
        s_cc_game.width = info.width;
        s_cc_game.height = info.height;
    }
    purr_catrt_set_game(&s_cc_game, lifecycle);
}
