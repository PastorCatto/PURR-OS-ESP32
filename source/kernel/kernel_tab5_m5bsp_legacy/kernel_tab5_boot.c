// kernel_tab5_boot.c — specialized boot for M5Stack Tab5 (ESP32-P4)
//
// Much shorter than kernel_tdp_boot.c's own equivalent: display/touch/SD
// bring-up is delegated to m5tab5_bsp.c (native driver — MIPI-DSI/DPI panel
// bring-up, DCS init tables, and IO-expander sequencing all sourced from
// espp/m5stack-tab5's real hardware-tested code, cross-checked against
// M5Stack's own M5GFX — see that file's header comment), not hand-sequenced
// here.
//
// Phase 1 scope (this pass): display, touch, SD. No physical Tab5 in hand
// yet — build-verified only. The ESP32-C6 companion (SDIO, for WiFi/BT —
// see device.pcat's [pins] c6_* entries) is Phase 2, not brought up here.
//
// Baked-in (Layer 0, initialized here, NOT via module loader):
//   m5tab5_bsp — MIPI-DSI display + I2C touch (ILI9881C+GT911 or ST7123,
//                auto-detected), SDMMC SD card
//
// Plug-and-play (Layer 1, loaded by module loader):
//   MiniWin, app_manager

#include "purr_kernel.h"
#include "purr_crash_guard.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>

#include "../../drivers/display/m5tab5_bsp/m5tab5_bsp.h"
#include "../../modules/purr_console/purr_console.h"
#include "../../modules/purr_console_login/purr_console_login.h"

static const char *TAG = "tab5_boot";

// ── Flash VFS (SPIFFS) ────────────────────────────────────────────────────

static void mount_flash_vfs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/flash",
        .partition_label        = NULL,
        .max_files              = 12,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed (%s)", esp_err_to_name(ret));
    } else {
        size_t total = 0, used = 0;
        esp_spiffs_info(NULL, &total, &used);
        ESP_LOGI(TAG, "flash VFS: %u KB / %u KB",
                 (unsigned)(used / 1024), (unsigned)(total / 1024));
    }
}

static void ensure_sd_dirs(void)
{
    if (!purr_kernel_sd_available()) return;
    const char *dirs[] = {
        "/sdcard/apps", "/sdcard/modules", "/sdcard/drivers",
        "/sdcard/drivers/display", "/sdcard/drivers/touch",
        "/sdcard/drivers/input", "/sdcard/drivers/radio",
        "/sdcard/drivers/gps", "/sdcard/system", "/sdcard/system/logs",
        NULL
    };
    for (int i = 0; dirs[i]; i++) {
        struct stat st;
        if (stat(dirs[i], &st) != 0) mkdir(dirs[i], 0755);
    }
}

// ── UART0 binding for the shared console core ───────────────────────────────
//
// This device had NO console/login of any kind before this — a real gap
// against "shell reachable when things go wrong" (see purr_kernel_start_
// protected()'s own doc comment). UART0, not USB-Serial-JTAG: no hardware
// in hand to confirm which peripheral this board's own USB-C port actually
// reaches (the discovery that mattered for T-Deck Plus — see that kernel's
// own usbjtag_console_read_byte() comment), so this uses the same
// conservative, universally-reachable default every other migrated kernel
// besides T-Deck Plus uses.
static int uart0_console_read_byte(uint32_t timeout_ms)
{
    uint8_t c = 0;
    if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(timeout_ms)) > 0) return c;
    return -1;
}

static void uart0_console_write(const void *data, size_t len)
{
    uart_write_bytes(UART_NUM_0, (const char *)data, len);
}

static void uart0_console_flush(void)
{
    uart_wait_tx_done(UART_NUM_0, portMAX_DELAY);
}

static const purr_console_io_t s_console_io = {
    .read_byte = uart0_console_read_byte,
    .write     = uart0_console_write,
    .flush     = uart0_console_flush,
};

static void serial_console_task(void *arg)
{
    (void)arg;
    esp_err_t ret = uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s — console unavailable", esp_err_to_name(ret));
        return;
    }

    purr_console_set_login_fn(purr_console_login_default_login_fn);
    purr_console_set_exec_fn(purr_console_login_default_exec_fn);
    purr_console_run(&s_console_io, true);   // never returns
}

// ── app_main ──────────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "PURR OS %s / KITT %s  M5Stack Tab5 booting...",
             PURR_KERNEL_VERSION, KITT_VERSION);

    // NVS
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    mount_flash_vfs();

    // ── Phase 0: baked-in hardware init ──────────────────────────────────
    //
    // Display+touch (and SD) must be up before ANY module loads, so the UI
    // backend's own init() finds working catcalls — same guarantee
    // kernel_tdp_boot.c gives T-Deck Plus.

    ESP_LOGI(TAG, "=== phase 0: baked-in drivers ===");

    if (m5tab5_bsp_drv_init() != 0) {
        purr_kernel_panic("M5Stack Tab5 BSP (display/touch) init failed");
    }

    if (m5tab5_bsp_sdcard_init() == 0) {
        purr_kernel_set_sd_available(true);
        ensure_sd_dirs();
    }

    ESP_LOGI(TAG, "baked-in drivers ready");

    // Checked here — after display/touch (Layer 0) are up, so the blue
    // recoverable panic screen can actually render/accept touch if this
    // trips, but before any module (including the UI backend itself) gets
    // a chance to load. See purr_crash_guard.h for the full design.
    purr_crash_guard_check_reset_reason();

    // ── Phase 1: plug-and-play modules ───────────────────────────────────
    //
    // purr_register_static_modules() is generated by purrstrap from
    // device.pcat and omits display/touch for this device (baked in
    // above) — same skip purrstrap already does for T-Deck Plus.

    ESP_LOGI(TAG, "=== phase 1: static modules ===");
    extern void purr_register_static_modules(void);
    purr_register_static_modules();
    purr_kernel_load_static_modules();

    // app_manager's own init() scans for apps before P3 system apps have
    // registered, so its first scan always finds 0 — re-scan now that
    // every priority tier above has loaded.
    extern int app_manager_scan(void);
    app_manager_scan();
    purr_kernel_set_boot_ready(true);

    // ── Phase 2: SD extras ───────────────────────────────────────────────

    if (purr_kernel_sd_available()) {
        ESP_LOGI(TAG, "=== phase 2: SD extras ===");
        purr_kernel_scan_modules("/sdcard/modules", NULL);
        purr_kernel_scan_modules("/sdcard/drivers", NULL);
    }

    if (!purr_kernel_display()) {
        ESP_LOGW(TAG, "no display catcall — check m5tab5_bsp init");
    }
    if (!purr_kernel_get_module("app_manager")) {
        ESP_LOGW(TAG, "app_manager not loaded");
    }

    ESP_LOGI(TAG, "boot complete — %u bytes free", (unsigned)purr_kernel_free_ram());
    purr_kernel_notify("PURR OS ready", "M5Stack Tab5 booted", "kernel");

    // Protected process, not a raw xTaskCreate(): see
    // purr_kernel_start_protected()'s own doc comment on why the login/
    // recovery console must never be silently strike-disabled the way a
    // misbehaving P2/P3 module can be. This is also this device's FIRST
    // console/login of any kind — previously app_manager's local app
    // registry (s_local_unlocked, defaults closed) had no caller on this
    // device that ever opened it at all.
    static const purr_protected_process_t s_console_proc = {
        .name       = "console",
        .run        = serial_console_task,
        .arg        = NULL,
        .stack_size = 4096,
        .priority   = 1,
        .core_id    = -1,
    };
    purr_kernel_start_protected(&s_console_proc);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
