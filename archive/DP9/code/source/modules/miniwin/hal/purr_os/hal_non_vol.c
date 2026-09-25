// hal_non_vol.c — MiniWin HAL non-volatile storage backend for PURR OS
//
// Stores MiniWin calibration + settings as a blob in NVS.
// NVS is already initialised by the kernel before modules run.

#include "hal/hal_non_vol.h"
#include "nvs_flash.h"
#include "nvs.h"

#define NVS_NAMESPACE  "miniwin"
#define NVS_KEY        "settings_v2"

void mw_hal_non_vol_init(void)
{
    // NVS already initialised by kernel boot.c — nothing to do.
}

void mw_hal_non_vol_load(uint8_t *data, uint16_t length)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = (size_t)length;
    nvs_get_blob(h, NVS_KEY, data, &sz);
    nvs_close(h);
}

void mw_hal_non_vol_save(uint8_t *data, uint16_t length)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY, data, (size_t)length);
    nvs_commit(h);
    nvs_close(h);
}
