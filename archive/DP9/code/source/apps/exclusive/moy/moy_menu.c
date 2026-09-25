// moy_menu.c — pick a cart when more than one is installed.
//
// Drawn with the console's own primitives into g_port.fb8 and presented through
// purr_port, exactly as a cart is. That is deliberate: the picker cannot use
// LVGL because speed demon has already unloaded the UI backend by the time this
// runs, and building a second text renderer for it would be silly when
// moy_print() is right there.
//
// Skipped entirely when there is exactly one cart — a menu you cannot make a
// choice in is just an extra button press.

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "moy.h"

static const char *TAG = "moy_menu";

#define ROW_H       12
#define TOP_Y       40
#define VISIBLE     14          // (240 - TOP_Y - 16) / ROW_H

// Strip the ".moy" suffix for display — the folder name is the cart's identity
// on disk, but the extension is noise in a menu.
static void pretty(const char *folder, char *out, size_t n)
{
    snprintf(out, n, "%s", folder);
    size_t l = strlen(out);
    if (l > 4 && strcasecmp(out + l - 4, ".moy") == 0) out[l - 4] = '\0';
}

static void draw(const moy_cart_list_t *carts, int sel, int top)
{
    moy_cls(1);                                     // dark blue, palette index 1

    moy_print("MOY", 8, 8, 7);
    moy_rectb(0, 0, MOY_SCREEN_W, MOY_SCREEN_H, 12);
    moy_line(0, 26, MOY_SCREEN_W - 1, 26, 12);

    char buf[72];
    snprintf(buf, sizeof(buf), "%d carts", carts->n);
    moy_print(buf, MOY_SCREEN_W - 8 - (int)strlen(buf) * 8, 8, 6);

    for (int i = 0; i < VISIBLE && top + i < carts->n; i++) {
        int idx = top + i;
        int y   = TOP_Y + i * ROW_H;
        char name[64];
        pretty(carts->name[idx], name, sizeof(name));

        if (idx == sel) {
            moy_rect(4, y - 2, MOY_SCREEN_W - 8, ROW_H, 12);
            moy_print(name, 12, y, 7);
        } else {
            moy_print(name, 12, y, 6);
        }
    }

    moy_print("W/S select   SPACE start", 8, MOY_SCREEN_H - 14, 5);
    purr_port_present(&g_port);
}

// Returns the chosen folder name, or NULL if the player backed out.
const char *moy_menu_pick(const moy_cart_list_t *carts)
{
    if (carts->n <= 0) return NULL;
    if (carts->n == 1) {
        ESP_LOGI(TAG, "one cart, skipping the picker");
        return carts->name[0];
    }

    int sel = 0, top = 0;
    draw(carts, sel, top);

    for (;;) {
        purr_port_heartbeat();
        moy_input_poll();

        // The same gesture that leaves a running cart also leaves the picker,
        // so there is one way out of moy rather than two to remember.
        const char *how = NULL;
        if (moy_exit_requested(&how)) {
            ESP_LOGI(TAG, "exit gesture (%s) from the picker", how);
            return NULL;
        }

        bool dirty = false;
        if (moy_btnp(MOY_BTN_DOWN, 0) && sel < carts->n - 1) { sel++; dirty = true; }
        if (moy_btnp(MOY_BTN_UP,   0) && sel > 0)            { sel--; dirty = true; }

        if (sel < top)               { top = sel;               dirty = true; }
        if (sel >= top + VISIBLE)    { top = sel - VISIBLE + 1; dirty = true; }

        if (moy_btnp(MOY_BTN_A, 0)) {
            ESP_LOGI(TAG, "picked %s", carts->name[sel]);
            return carts->name[sel];
        }

        if (dirty) draw(carts, sel, top);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
