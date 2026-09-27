/*
 * PURR OS - boot menu preview.
 *
 * Runs the boot menu's logic (coreos purr_menu.c) on the real screen and keyboard,
 * built from the real partition table. It runs as the app for now, so it does not
 * boot anything: a choice is shown on screen. Every other round it pretends there
 * is nothing to boot, to show the "Internet recovery" case. The bootloader-side
 * version comes after this is checked by eye.
 */
#include <stdio.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "purr_gfx.h"
#include "purr_kernel.h"
#include "purr_menu.h"

static const char *TAG = "purros";

#define BLACK   PURR_RGB565(0, 0, 0)
#define WHITE   PURR_RGB565(255, 255, 255)
#define GREY    PURR_RGB565(150, 150, 150)
#define ORANGE  PURR_RGB565(255, 180, 0)
#define TICK_MS 50

#define MAX_PARTS 8

/* The app partitions from the table. One is bootable if it starts with an ESP image. */
static int read_parts(purr_menu_part_t *parts, int max, int *preferred)
{
    int n = 0;
    *preferred = -1;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL && n < max; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        uint8_t first = 0;
        esp_partition_read(p, 0, &first, 1);
        snprintf(parts[n].label, sizeof(parts[n].label), "%s", p->label);
        parts[n].bootable = (first == 0xE9);     /* ESP image magic */
        if (p == running) {
            *preferred = n;
        }
        n++;
    }
    esp_partition_iterator_release(it);
    return n;
}

static void draw(const purr_display_v2_t *d, int w, int h, const purr_menu_t *m,
                 const char *message)
{
    d->fill(0, 0, w, h, BLACK);
    purr_gfx_text(d, 16, 16, "PURR OS", ORANGE, BLACK, 3);
    purr_gfx_text(d, 16, 48, "boot menu", GREY, BLACK, 1);

    if (message) {
        purr_gfx_text(d, 16, 100, message, WHITE, BLACK, 2);
        return;
    }
    if (!purr_menu_visible(m)) {
        purr_gfx_text(d, 16, 100, "No system found...", GREY, BLACK, 2);
        return;
    }
    if (m->no_boot_options) {
        purr_gfx_text(d, 16, 76, "Nothing to boot.", WHITE, BLACK, 1);
    }
    for (int i = 0; i < m->count; i++) {
        int y = 100 + i * 28;
        bool sel = (i == m->selected);
        d->fill(12, y - 4, w - 24, 24, sel ? ORANGE : BLACK);
        char line[40];
        snprintf(line, sizeof(line), "%s%s", m->no_boot_options ? "" : "Boot ", m->entries[i].label);
        purr_gfx_text(d, 20, y, line, sel ? BLACK : WHITE, sel ? ORANGE : BLACK, 2);
    }
    int s = purr_menu_seconds_left(m);
    char foot[48];
    if (s >= 0) {
        snprintf(foot, sizeof(foot), "Booting in %d...  any key stops", s);
    } else {
        snprintf(foot, sizeof(foot), "W up   S down   D or Enter select");
    }
    purr_gfx_text(d, 16, h - 24, foot, GREY, BLACK, 1);
}

void app_main(void)
{
    ESP_LOGI(TAG, "PURR OS boot menu preview");
    if (purr_kernel_init() != ESP_OK || purr_kernel_display() == NULL) {
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            ESP_LOGE(TAG, "display did not come up");
        }
    }
    const purr_display_v2_t *d = purr_kernel_display();
    purr_display_info_t info;
    d->get_info(&info);
    if (d->set_brightness) {
        d->set_brightness(255);
    }

    for (int round = 0;; round++) {
        purr_menu_part_t parts[MAX_PARTS];
        int preferred, n = read_parts(parts, MAX_PARTS, &preferred);
        if (round % 2 == 1) {
            n = 0;                       /* pretend nothing is there */
        }
        purr_menu_t m;
        purr_menu_init(&m, parts, n, preferred);
        ESP_LOGI(TAG, "round %d: %d partitions, %d entries", round, n, m.count);

        draw(d, info.width, info.height, &m, NULL);
        int last_sel = m.selected, last_secs = purr_menu_seconds_left(&m), last_vis = purr_menu_visible(&m);
        purr_menu_result_t r = {PURR_MENU_ACT_NONE, -1};
        while (r.action == PURR_MENU_ACT_NONE) {
            vTaskDelay(pdMS_TO_TICKS(TICK_MS));
            char c = purr_kernel_key();
            r = purr_menu_step(&m, TICK_MS, purr_key_from_char(c));
            if (m.selected != last_sel || purr_menu_seconds_left(&m) != last_secs ||
                purr_menu_visible(&m) != last_vis) {
                last_sel = m.selected;
                last_secs = purr_menu_seconds_left(&m);
                last_vis = purr_menu_visible(&m);
                draw(d, info.width, info.height, &m, NULL);
            }
        }

        char msg[48];
        if (r.action == PURR_MENU_ACT_BOOT) {
            snprintf(msg, sizeof(msg), "Would boot %s", parts[r.index].label);
        } else {
            snprintf(msg, sizeof(msg), "Recovery: not built yet");
        }
        ESP_LOGI(TAG, "%s", msg);
        draw(d, info.width, info.height, &m, msg);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
