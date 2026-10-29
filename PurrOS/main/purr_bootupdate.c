/* purr_bootupdate.c - see purr_bootupdate.h. */
#include "purr_bootupdate.h"

#include <stdio.h>
#include <string.h>

#include "esp_flash.h"
#include "esp_log.h"

#include "purr_battery.h"

#define SECTOR 4096u
#define MIN_PERCENT 50

static const char *TAG = "bootupdate";

int purr_bootupdate_read(uint8_t *buf, size_t len)
{
    return esp_flash_read(NULL, buf, 0, (uint32_t)len) == ESP_OK ? 0 : -1;
}

int purr_bootupdate_write(const uint8_t *img, size_t len)
{
    uint32_t sectors = (uint32_t)((len + SECTOR - 1) / SECTOR);
    uint8_t want[SECTOR], have[SECTOR];
    for (int s = (int)sectors - 1; s >= 0; s--) {                         /* last first, so the header sector is the final one touched */
        uint32_t addr = (uint32_t)s * SECTOR;
        size_t n = len - addr < SECTOR ? len - addr : SECTOR;
        memset(want, 0xFF, sizeof(want));
        memcpy(want, img + addr, n);
        if (esp_flash_read(NULL, have, addr, SECTOR) == ESP_OK && memcmp(have, want, SECTOR) == 0) {
            continue;                                                     /* already right: not touched */
        }
        if (esp_flash_erase_region(NULL, addr, SECTOR) != ESP_OK || esp_flash_write(NULL, want, addr, SECTOR) != ESP_OK) {
            ESP_LOGE(TAG, "sector at 0x%x did not write", (unsigned)addr);
            return -1;
        }
    }
    for (uint32_t s = 0; s < sectors; s++) {                              /* read every sector back */
        uint32_t addr = s * SECTOR;
        size_t n = len - addr < SECTOR ? len - addr : SECTOR;
        if (esp_flash_read(NULL, have, addr, SECTOR) != ESP_OK || memcmp(have, img + addr, n) != 0) {
            ESP_LOGE(TAG, "sector at 0x%x does not read back as written", (unsigned)addr);
            return -1;
        }
    }
    return 0;
}

int purr_bootupdate_power_ok(char *why, size_t cap)
{
    int pct = 0, mv = 0;
    if (!purr_battery_get(&pct, &mv)) {
        return 1;                                                         /* no cell connected: the board is on USB power */
    }
    if (pct < MIN_PERCENT) {
        snprintf(why, cap, "Charge the battery to at least %d%% first (it is at %d%%).", MIN_PERCENT, pct);
        return 0;
    }
    return 1;
}
