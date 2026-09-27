// kernel_td_boot.c — specialized boot for T-Deck
//
// Identical to kernel_tdp_boot except no GPS module in phase 1.
//
// Baked-in (Layer 0):
//   ST7789  — SPI display  (CS=12 DC=11 MOSI=41 SCLK=40 RST=-1 BL=42)
//   Trackball — GPIO       (UP=3 DN=15 LT=1 RT=2 CLK=0)
//   BB Q20  — I2C keyboard (SDA=18 SCL=8 addr=0x55)
//   Note: T-Deck has no touch panel.
//
// Plug-and-play (Layer 4):
//   KittenUI, app_manager, SX1262 LoRa

#include "purr_kernel.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/stat.h>
#include <string.h>

#include "../../drivers/display/st7789/st7789.h"
#include "../../drivers/input/trackball/trackball.h"
#include "../../drivers/input/bbq20/bbq20.h"
#include "../../modules/purr_console/purr_console.h"
#include "../../modules/purr_console_login/purr_console_login.h"
#include "../../modules/purr_fbtty/purr_fbtty.h"

static const char *TAG = "td_boot";

#define TD_DISPLAY_CS    12
#define TD_DISPLAY_DC    11
#define TD_DISPLAY_MOSI  41
#define TD_DISPLAY_SCLK  40
#define TD_DISPLAY_RST   (-1)
#define TD_DISPLAY_BL    42

static void mount_flash_vfs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/flash",
        .partition_label        = NULL,
        .max_files              = 12,
        .format_if_mount_failed = false,
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
        "/sdcard/system", "/sdcard/system/logs", NULL
    };
    for (int i = 0; dirs[i]; i++) {
        struct stat st;
        if (stat(dirs[i], &st) != 0) mkdir(dirs[i], 0755);
    }
}

// ── UART0 binding for the shared console core ───────────────────────────────
//
// T-Deck (unlike its T-Deck Plus sibling) has no native USB-Serial-JTAG
// peripheral broken out to its USB-C port — UART0 is the real, reachable
// console transport here, matching this board's own original ad hoc
// serial_console_task() this replaces.
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

    // Real "console mode" — the same bbq20 keyboard + ST7789 screen T-Deck
    // Plus proved this on: type on the device's own keyboard, read its
    // own screen, no cable required. Falls back to UART0 only if no
    // display ever registered (shouldn't happen — display is baked in at
    // Phase 0, before this task starts).
    if (purr_fbtty_init()) {
        purr_console_run(&purr_fbtty_io, true);   // never returns
    } else {
        ESP_LOGW(TAG, "purr_fbtty_init failed (no display?) — falling back to UART0");
        purr_console_run(&s_console_io, true);    // never returns
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "PURR OS %s / KITT %s  T-Deck booting...",
             PURR_KERNEL_VERSION, KITT_VERSION);

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    mount_flash_vfs();
    ensure_sd_dirs();

    ESP_LOGI(TAG, "=== phase 0: baked-in drivers ===");

    st7789_configure(TD_DISPLAY_CS, TD_DISPLAY_DC, TD_DISPLAY_MOSI,
                     -1, TD_DISPLAY_SCLK, TD_DISPLAY_RST, TD_DISPLAY_BL);
    if (st7789_drv_init() != 0) {
        purr_kernel_panic("ST7789 display init failed");
    }

    if (trackball_drv_init() != 0) {
        ESP_LOGW(TAG, "trackball init failed — continuing");
    }

    if (bbq20_drv_init() != 0) {
        ESP_LOGW(TAG, "keyboard init failed — continuing");
    }

    ESP_LOGI(TAG, "baked-in drivers ready");

    ESP_LOGI(TAG, "=== phase 1: static modules ===");
    extern void purr_register_static_modules(void);
    purr_register_static_modules();
    purr_kernel_load_static_modules();

    if (purr_kernel_sd_available()) {
        ESP_LOGI(TAG, "=== phase 2: SD extras ===");
        purr_kernel_scan_modules("/sdcard/modules", NULL);
        purr_kernel_scan_modules("/sdcard/drivers", NULL);
    }

    ESP_LOGI(TAG, "boot complete — %u bytes free", (unsigned)purr_kernel_free_ram());

    // Protected process, not a raw xTaskCreate(): see
    // purr_kernel_start_protected()'s own doc comment on why the login/
    // recovery console must never be silently strike-disabled the way a
    // misbehaving P2/P3 module can be.
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
