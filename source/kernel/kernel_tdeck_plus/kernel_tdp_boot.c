// kernel_tdp_boot.c — specialized boot for T-Deck Plus
//
// Inits display, touch, trackball, and keyboard directly before the module
// loader runs. This avoids module priority races and ensures all Layer 0
// catcalls are registered before KittenUI tries to use them.
//
// Baked-in (Layer 0, initialized here, NOT via module loader):
//   ST7789  — SPI display  (CS=12 DC=11 MOSI=41 SCLK=40 RST=-1 BL=42)
//   GT911   — I2C touch    (SDA=18 SCL=8 INT=16 RST=NC)
//   Trackball — GPIO       (UP=3 DN=15 LT=1 RT=2 CLK=0)
//   BB Q20  — I2C keyboard (SDA=18 SCL=8 addr=0x55)
//
// Plug-and-play (Layer 4, loaded by module loader):
//   MiniWin, app_manager, SX1276 LoRa, generic_nmea GPS

#include "purr_kernel.h"
#include "purr_crash_guard.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>

// Baked-in driver headers
#include "../../drivers/display/st7789/st7789.h"
#include "../../drivers/touch/gt911/gt911.h"
#include "../../drivers/input/trackball/trackball.h"
#include "../../drivers/input/bbq20/bbq20.h"
#include "../../drivers/radio/sx1262_rl/sx1262_rl.h"
#include "../../modules/boot_splash/boot_splash.h"
#include "driver/i2c_master.h"
#include "esp_rom_sys.h"
// usb_msc.h — pinned/dormant (source/modules/usb_msc/DISABLED.md), its own
// module.pcat renamed so modulestrap no longer discovers it. Everything
// that touches it (this include, tdp_usb_share_sd() below, and the actual
// usb_msc_init() call further down) is #if 0'd together — the component
// isn't part of the build at all right now, so a bare #include would fail
// to resolve the header just as the init call would fail to link.
#if 0
#include "../../modules/usb_msc/usb_msc.h"
#endif

// SD card (shares the display's SPI bus — see mount_sd_vfs()'s comment)
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

// WiFi station-mode bring-up (esp_wifi_init() itself, not connect/scan —
// that's wifi_mgr.c, a plug-and-play module driven by Settings)
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"

// AES hardware-lock prewarm (see app_main()'s call site comment)
#include "aes/esp_aes.h"

static const char *TAG = "tdp_boot";

// ── T-Deck Plus pin assignments ───────────────────────────────────────────────

#define TDP_DISPLAY_CS    12
#define TDP_DISPLAY_DC    11
#define TDP_DISPLAY_MOSI  41
#define TDP_DISPLAY_SCLK  40
#define TDP_DISPLAY_RST   (-1)
#define TDP_DISPLAY_BL    42

// SD shares the display's SPI2_HOST bus (same MOSI=41/SCLK=40 on the PCB,
// separate CS) — see mount_sd_vfs()'s comment for why it must be mounted
// before st7789_drv_init() runs.
// CS confirmed against LilyGo's own utilities.h/TDECK_PINS.h (TDECK_SDCARD_CS)
// — the previous value of 13 here was wrong and collided with the real LoRa
// BUSY pin (see TDP_LORA_BUSY below).
#define TDP_SD_CS    39
#define TDP_SD_MOSI  41
#define TDP_SD_MISO  38
#define TDP_SD_SCLK  40

// LoRa (SX1262) shares the same SPI2_HOST bus as display/SD above — only CS
// differs. Confirmed against LilyGo's utilities.h/TDECK_PINS.h; this device
// previously ran the wrong radio driver entirely (sx1276, with pins that
// happened to describe a separate, nonexistent bus) — see device.pcat.
#define TDP_LORA_MOSI  41
#define TDP_LORA_MISO  38
#define TDP_LORA_SCLK  40
#define TDP_LORA_CS    9
#define TDP_LORA_RST   17
#define TDP_LORA_BUSY  13
#define TDP_LORA_IRQ   45

// GT911 and BB Q20 share I2C bus on port 0
// Trackball uses GPIO only

// ── Flash VFS (SPIFFS) ────────────────────────────────────────────────────────

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

// ── SD card VFS (FAT over SPI) ───────────────────────────────────────────────
//
// T-Deck Plus wires the SD card and the ST7789 display to the same physical
// SPI bus (MOSI=41, SCLK=40 on both — only CS differs, display=12 vs sd=13).
// spi_bus_initialize() fixes a host's pin config on its FIRST successful call
// for the lifetime of the bus; every later call for that host just returns
// ESP_ERR_INVALID_STATE and is ignored (st7789.c already tolerates this, see
// its own spi_bus_initialize call). Display never needs to read, so
// st7789_configure() hardcodes MISO=-1 (not connected) — if display's own
// init ran first, the bus would come up with no working MISO line at all,
// and any later SD device on that same host could never read a byte back.
// So SD must call spi_bus_initialize() (with a real MISO pin) before
// st7789_drv_init() runs, and display's later call just no-ops into the
// bus SD already brought up.
//
// Bus host is SPI2_HOST — confirmed against LilyGo's own utilities.h/
// TDECK_PINS.h for this exact pin set (MOSI=41/MISO=38/SCLK=40/CS=39).
// (A detour through SPI3_HOST + a longer settle delay, matching an archived
// PURR OS 0.11 reference, reproduced the identical failure on real hardware
// — that reference's own tdeck_plus build was never actually compiled into
// a flashable image either, so it wasn't a verified baseline. Back to the
// pin-verified SPI2_HOST here; the delay/retry logic stays as reasonable
// defense-in-depth while this gets debugged further.)
static sdmmc_card_t *s_sd_card = NULL;

static void mount_sd_vfs(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = TDP_SD_MOSI,
        .miso_io_num     = TDP_SD_MISO,
        .sclk_io_num     = TDP_SD_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        // Must cover the display's own transfer size too, since this call
        // (not st7789's) is the one that actually configures the shared bus.
        .max_transfer_sz = 320 * 240 * 2 + 8,
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SD: spi_bus_initialize failed (%s) — continuing without SD",
                  esp_err_to_name(ret));
        return;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    // SDSPI_HOST_DEFAULT()'s 20MHz default probes the card (CMD0/CMD52/etc.)
    // at full speed from the very first transaction — conservative to keep
    // this slower given the shared/comparatively long bus.
    host.max_freq_khz = 4000;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = TDP_SD_CS;
    slot_cfg.host_id = SPI2_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    // Extra settle delay beyond the fixed 50ms upstream (tuned for GT911,
    // not the SD card), plus a retry loop — defensive measures against the
    // flaky/non-deterministic failure signature observed on this bus.
    vTaskDelay(pdMS_TO_TICKS(200));

    ret = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_cfg, &mount_cfg, &s_sd_card);
        if (ret == ESP_OK) break;
        ESP_LOGW(TAG, "SD mount attempt %d/3 failed (%s)", attempt, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (%s) — continuing without SD", esp_err_to_name(ret));
        return;
    }

    purr_kernel_set_sd_available(true);
    uint64_t mb = ((uint64_t)s_sd_card->csd.capacity) * s_sd_card->csd.sector_size / (1024 * 1024);
    ESP_LOGI(TAG, "SD VFS mounted: %s, %llu MB", s_sd_card->cid.name, mb);

    char body[PURR_NOTIFY_BODY_LEN];
    snprintf(body, sizeof(body), "%s, %llu MB", s_sd_card->cid.name, mb);
    purr_kernel_notify("SD card mounted", body, "sd");
}

// purr_kernel_set_panic_usb_share_cb() callbacks are void(void) — this just
// closes over s_sd_card (mount_sd_vfs()'s own handle, still valid at panic
// time regardless of which task/entity actually panicked).
// Dormant along with the #if 0'd usb_msc_init() call below — see the
// #include's matching comment above.
#if 0
static void tdp_usb_share_sd(void)
{
    usb_msc_share_sd("/sdcard", s_sd_card);
}
#endif

static void ensure_sd_dirs(void)
{
    if (!purr_kernel_sd_available()) return;
    const char *dirs[] = {
        "/sdcard/apps", "/sdcard/modules", "/sdcard/drivers",
        "/sdcard/drivers/display", "/sdcard/drivers/touch",
        "/sdcard/drivers/input", "/sdcard/drivers/radio",
        "/sdcard/drivers/gps", "/sdcard/system", "/sdcard/system/logs",
        "/sdcard/meshchat",       // MeshChat DM/room history text files
        NULL
    };
    for (int i = 0; dirs[i]; i++) {
        struct stat st;
        if (stat(dirs[i], &st) != 0) mkdir(dirs[i], 0755);
    }
}

// ── Serial console ────────────────────────────────────────────────────────────
// Always-on task on UART0. Type 'kb' to echo BBQ20 keypresses to serial.
// Lets you confirm keyboard hardware works independent of the UI.

static void kb_test_loop(void)
{
    const catcall_input_t *kbd = purr_kernel_input();
    if (!kbd) {
        printf("[KB TEST] no keyboard catcall — bbq20 not ready\r\n");
        return;
    }
    printf("[KB TEST] press keys on the device keyboard. type 'q' here to exit.\r\n");
    fflush(stdout);
    for (;;) {
        uint8_t c = 0;
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(0)) > 0) {
            if (c == 'q' || c == 3) break;
        }
        input_event_t ev;
        while (kbd->poll_event(&ev)) {
            if (ev.type == INPUT_EVENT_KEY_DOWN && ev.keycode) {
                uint16_t k = ev.keycode;
                if (k >= 0x20 && k <= 0x7E)
                    printf("[KB] '%c' (0x%02X)\r\n", (char)k, k);
                else
                    printf("[KB] 0x%02X\r\n", k);
                fflush(stdout);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    printf("[KB TEST] done.\r\n");
}

static void i2c_scan_cmd(void)
{
    i2c_master_bus_handle_t bus = NULL;
    bool created = false;
    esp_err_t r = i2c_master_get_bus_handle(I2C_NUM_0, &bus);
    if (r != ESP_OK) {
        i2c_master_bus_config_t cfg = {
            .i2c_port          = I2C_NUM_0,
            .sda_io_num        = 18,
            .scl_io_num        = 8,
            .clk_source        = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
            printf("[SCAN] failed to acquire I2C bus\r\n");
            return;
        }
        created = true;
    }
    printf("[SCAN] full I2C sweep SDA=18 SCL=8:\r\n");
    int found = 0;
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        esp_err_t res = i2c_master_probe(bus, addr, pdMS_TO_TICKS(10));
        if (res == ESP_OK) {
            const char *name = "";
            if (addr == 0x5D) name = " (GT911 primary)";
            else if (addr == 0x14) name = " (GT911 alt)";
            else if (addr == 0x55) name = " (BBQ20 keyboard)";
            printf("[SCAN]   0x%02X ACK%s\r\n", addr, name);
            found++;
        }
    }
    if (found == 0) printf("[SCAN]   no devices found\r\n");
    printf("[SCAN] done (%d device(s))\r\n", found);
    if (created) i2c_del_master_bus(bus);
}

static void serial_console_task(void *arg)
{
    esp_err_t ret = uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        vTaskDelete(NULL);
        return;
    }

    char line[32];
    int  len = 0;
    printf("\r\nPURR OS console (tdp) — commands: kb, scan\r\n> ");
    fflush(stdout);

    for (;;) {
        uint8_t c = 0;
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(50)) <= 0) continue;

        if (c == '\r' || c == '\n') {
            printf("\r\n");
            line[len] = '\0';
            len = 0;
            if (strcmp(line, "kb") == 0)       kb_test_loop();
            else if (strcmp(line, "scan") == 0) i2c_scan_cmd();
            else if (line[0] != '\0')           printf("unknown: %s\r\n", line);
            printf("> ");
            fflush(stdout);
        } else if ((c == 0x7F || c == '\b') && len > 0) {
            len--;
            printf("\b \b");
            fflush(stdout);
        } else if (len < (int)sizeof(line) - 1 && c >= 0x20) {
            line[len++] = (char)c;
            putchar(c);
            fflush(stdout);
        }
    }
}

// ── Recovery-boot bounded bring-up ───────────────────────────────────────
//
// Only used when purr_crash_guard_pending_recovery() says this boot is
// recovering from a hang-triggered reboot (see purr_crash_guard.c) — a
// still-wedged SPI2 bus right after that reboot would otherwise just hang
// SD mount / display init the exact same way, defeating the whole point.
// On a normal cold boot these are never invoked; mount_sd_vfs()/
// st7789_drv_init() run exactly as they always have, unwrapped.
#define TDP_SD_MOUNT_TIMEOUT_MS      6000UL
#define TDP_DISPLAY_INIT_TIMEOUT_MS  3000UL

static void run_mount_sd_vfs(void *arg)
{
    (void)arg;
    mount_sd_vfs();
}

typedef struct { int rc; } display_init_ctx_t;

static void run_st7789_drv_init(void *arg)
{
    display_init_ctx_t *c = (display_init_ctx_t *)arg;
    c->rc = st7789_drv_init();
}

// ── app_main ──────────────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "PURR OS %s / KITT %s  T-Deck Plus booting...",
             PURR_KERNEL_VERSION, KITT_VERSION);

    // NVS
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Prewarm the hardware AES lock while internal DRAM is still abundant.
    // esp_aes_acquire_hardware() lazily creates its underlying mutex (via
    // newlib's lock_init_generic() -> xQueueCreateMutex()) the FIRST time
    // anything calls it — if that first call happens much later (e.g.
    // meshtastic's periodic self-NodeInfo broadcast, after WiFi/BT/LoRa/app
    // windows have already eaten most of internal DRAM), the allocation can
    // fail and lock_init_generic() hard-aborts the whole device. Confirmed
    // live via a real boot-time crash ("abort() was called ... /* No more
    // semaphores available or OOM */" inside lock_init_generic, reached via
    // esp_aes_acquire_hardware <- mesh_radio_aes_ctr). One throwaway
    // acquire/release here — before anything else has a chance to touch
    // internal DRAM — makes that lock permanently initialized so it can
    // never fail this way again for the rest of the session.
    esp_aes_acquire_hardware();
    esp_aes_release_hardware();

    mount_flash_vfs();

    // WiFi station mode: bring up esp_netif/esp_event/esp_wifi once here —
    // not started/connected yet, just initialized so wifi_mgr.c (a
    // plug-and-play module loaded below, driven by Settings) can scan/
    // connect on demand. CONFIG_ESP_WIFI_ENABLED was already set in
    // sdkconfig but nothing on this native boot path ever called into it.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_LOGI(TAG, "WiFi station mode initialized (not connected yet)");

    // ── Phase 0: baked-in hardware init ──────────────────────────────────────
    //
    // These run before ANY module is loaded, guaranteeing that display and
    // touch catcalls exist when KittenUI's init() checks for them.

    ESP_LOGI(TAG, "=== phase 0: baked-in drivers ===");

    // GT911 has no RST pin on T-Deck Plus (NC per LilyGO utilities.h).
    // INT=16, BOARD_POWERON=10.
    // Drive INT LOW before BOARD_POWERON so GT911 latches address 0x5D on power-up,
    // then release INT to input after 50ms startup margin.
    gpio_set_direction(GPIO_NUM_16, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_16, 0);

    gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_10, 1);
    ESP_LOGI(TAG, "BOARD_POWERON HIGH");
    vTaskDelay(pdMS_TO_TICKS(50));

    // Display and radio share this SPI bus with the SD card but haven't
    // called spi_bus_add_device() yet at this point in boot — their CS
    // pins (display=TDP_DISPLAY_CS, radio=TDP_LORA_CS) are still at
    // whatever state they reset to, effectively floating. If either floats
    // low while the SD card's own CMD52 probe clocks out, that chip can
    // wake up and drive garbage back onto the shared MISO line, corrupting
    // the very first transaction the SD card ever sees — independent of
    // SPI host/clock/timing, which matches every mount failure logged here
    // so far. Park both CS lines HIGH (deasserted) and pull up the shared
    // MISO line before SD ever touches the bus.
    gpio_set_direction((gpio_num_t)TDP_DISPLAY_CS, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)TDP_DISPLAY_CS, 1);
    gpio_set_direction((gpio_num_t)TDP_LORA_CS, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)TDP_LORA_CS, 1);
    gpio_set_pull_mode((gpio_num_t)TDP_SD_MISO, GPIO_PULLUP_ONLY);

    // NVS is already up (nvs_flash_init() above) — safe to peek here.
    // Computed once, reused for both SD and display below; Phase 1's own
    // module loading (purr_kernel_load_static_modules()) does its own,
    // separate peek for the same reason.
    // Name captured, not just the bool: a recovery from GAME MODE is worth
    // telling the user about specifically. That path leaves the device with no
    // UI backend loaded, so a game that faults during startup produces a black
    // screen with no indication anything is wrong — the boot splash saying so
    // is the only feedback available at that point.
    char recover_name[32] = {0};
    bool recovering = purr_crash_guard_pending_recovery(recover_name, sizeof(recover_name), NULL, 0);
    bool from_game  = recovering && strcmp(recover_name, "speed_demon") == 0;
    if (recovering) {
        ESP_LOGW(TAG, "recovering from a hang-triggered reboot (%s) — bounding SD/display bring-up this boot",
                 recover_name[0] ? recover_name : "unknown");
    }

    // SD card slot is on the same BOARD_POWERON peripheral rail as touch/etc.
    // — mounting it before this GPIO went high meant the slot had no power
    // yet, so the mount failed every time regardless of correct wiring.
    // Must still run before st7789_drv_init() below — see mount_sd_vfs()'s
    // comment (SD's own spi_bus_initialize() call must win the shared bus's
    // pin config, in particular a real MISO pin).
    if (recovering) {
        if (!purr_kernel_run_bounded("sd_mount", run_mount_sd_vfs, NULL, TDP_SD_MOUNT_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "SD mount did not respond within %lu ms post-recovery — continuing without SD",
                     (unsigned long)TDP_SD_MOUNT_TIMEOUT_MS);
        }
    } else {
        mount_sd_vfs();
    }
    ensure_sd_dirs();

    // Composite USB CDC+MSC — PAUSED. Confirmed live: with
    // CONFIG_TINYUSB_CDC_ENABLED + the console switched off USB-Serial-JTAG
    // (sdkconfig_tdeck_plus.overrides, see its own comment), the board
    // never re-enumerated on the USB bus at all after flashing — not even
    // as an unrecognized device — with no serial path left to diagnose why.
    // Reverted alongside those sdkconfig options so the board goes back to
    // its known-working USB-Serial-JTAG console. Re-enable only once the
    // enumeration failure is understood, ideally testing the MSC-only path
    // (no CDC, no console conflict) first since CDC's console takeover is
    // the more likely culprit.
#if 0
    if (purr_kernel_sd_available()) {
        if (usb_msc_init() == ESP_OK) {
            purr_kernel_set_panic_usb_share_cb(tdp_usb_share_sd);
        }
    }
#endif

    gpio_set_direction(GPIO_NUM_16, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Display — ST7789 SPI
    // ST7789 init includes a ~120ms SLPOUT delay internally, which counts
    // toward the GT911's required 300ms startup time after BOARD_POWERON.
    st7789_configure(TDP_DISPLAY_CS, TDP_DISPLAY_DC, TDP_DISPLAY_MOSI,
                     -1, TDP_DISPLAY_SCLK, TDP_DISPLAY_RST, TDP_DISPLAY_BL);
    // Default SPI2_HOST (st7789_set_spi_host() available if a future fix
    // needs to move this — see mount_sd_vfs()'s comment).
    //
    // Clock-speed tuning (80/40/20/10MHz, all tried this session) only ever
    // changed the ODDS of the shared-bus hang, never eliminated it — root
    // cause is the SPI driver calls on both the display and radio side
    // having no software timeout (see purr_crash_guard.c's new hang->reboot
    // path). Deliberately back at the driver's 80MHz default here — this is
    // now the FASTEST repro condition on purpose, to exercise that recovery
    // path (hang -> reboot -> staged bring-up -> notify/dump) under test,
    // not an attempt to avoid the hang.
    st7789_set_spi_freq(80 * 1000 * 1000);
    if (recovering) {
        // On a normal cold boot, display-init failure is fatal (below) —
        // the rest of the OS assumes a working screen. But on a recovery
        // boot specifically, if the bus is STILL bad, purr_kernel_panic()
        // would just reboot again straight back into this same state
        // (pending-recovery isn't cleared until after Phase 1 — see
        // below), looping forever. Degrade instead: log and continue with
        // no display registered — purr_kernel_register_display() (called
        // from inside st7789_drv_init() only on success) never runs, so
        // s_display stays NULL, which the rest of the kernel already
        // tolerates via its existing `if (disp)` guards.
        // Heap-allocated, NOT a stack local — this is the actual bug that
        // caused a real, live infinite reboot loop tonight. If
        // st7789_drv_init() doesn't hang forever but just finishes some
        // time after the timeout window (plausible right after a bus
        // reset — slow, not permanently stuck), the zombie helper task
        // writes into `dctx->rc` AFTER purr_kernel_run_bounded() already
        // returned false and app_main() moved on. A stack local at that
        // point is stale memory now holding something else — a silent
        // stack corruption hitting at a near-identical point on every
        // boot, which is exactly what was observed. load_one_static()'s
        // own bounded call (purr_kernel.c) already did this correctly;
        // this call site didn't — fixed to match.
        display_init_ctx_t *dctx = (display_init_ctx_t *)calloc(1, sizeof(*dctx));
        if (!dctx) {
            ESP_LOGE(TAG, "display init: ctx alloc failed — falling back to unbounded call");
            if (st7789_drv_init() != 0) {
                ESP_LOGE(TAG, "display init failed post-recovery (unbounded fallback) — continuing without display");
            }
        } else {
            dctx->rc = -1;
            bool ok = purr_kernel_run_bounded("display_init", run_st7789_drv_init, dctx, TDP_DISPLAY_INIT_TIMEOUT_MS);
            if (!ok) {
                ESP_LOGE(TAG, "display init did not respond within %lu ms post-recovery — continuing without display",
                         (unsigned long)TDP_DISPLAY_INIT_TIMEOUT_MS);
                // Deliberately NOT freed — see purr_kernel_run_bounded()'s
                // doc comment. The zombie task may still write into this
                // later; freeing it here would just move the same bug
                // from "stack" to "heap" (a use-after-free instead of a
                // stack corruption) rather than fixing it.
            } else {
                if (dctx->rc != 0) {
                    ESP_LOGE(TAG, "display init failed post-recovery (rc=%d) — continuing without display", dctx->rc);
                }
                free(dctx);
            }
        }
    } else if (st7789_drv_init() != 0) {
        purr_kernel_panic("ST7789 display init failed");
    }

    // Raw-framebuffer splash — drawn directly via catcall_display_t, well
    // before any UI backend (Cupcake/MiniWin) has started (that happens in
    // Phase 1 below). A no-op if display init above degraded to "continuing
    // without display" on a recovery boot.
    //
    // A game-mode recovery says so explicitly. That boot is the aftermath of a
    // game faulting while it owned the whole device with no UI loaded, so the
    // user's last experience was a black screen with no explanation; the normal
    // "Starting PURR OS..." would give them no reason to think anything had
    // gone wrong, and no hint as to what.
    if (from_game) {
        purr_splash_show("Recovering from Game Mode", BOOT_SPLASH_STEPS);
        purr_splash_status("game exited unexpectedly - restoring");
    } else {
        boot_splash_show();
    }

    // Perf mode: bulk-transfer PSRAM buffer for push_pixels — collapses
    // per-row transfers into one per flush (see st7789.h's doc comment).
    //
    // RE-ENABLED. This was disabled after producing visible corruption
    // (black rectangular chunks) and the note here blamed the buffer's own
    // bookkeeping rather than cache coherency. Measurement says otherwise, and
    // st7789_set_perf_mode() now allocates the buffer cache-line aligned and
    // padded to whole lines — see its comment for why a partial line at either
    // end is exactly what renders as a rectangular block.
    //
    // Why this is worth the second attempt: Step 0 of DP8_CHECKLIST.md measured
    // the row-by-row path on real hardware at ~80-100us per SPI transaction
    // against only ~18us of actual wire time. Roughly 83% of display time is
    // per-transaction overhead, and this path is what removes it — 66
    // transactions per flush should collapse to about 4.
    //
    // ROOT CAUSE, finally: the SPI transfer-length register is 18 bits, so one
    // transaction can carry at most 2^18 bits = 32,768 bytes. A full 320x240
    // frame is 153,600 and a single 80-line LVGL flush buffer is 51,200 — both
    // were rejected outright with
    //
    //   E spi_master: check_trans_valid: txdata transfer > hardware max
    //                 supported len
    //
    // and a rejected transfer sends NOTHING, so that region of GRAM kept
    // whatever was in it. Hence black rectangular blocks. Small dirty rects
    // stayed under the limit, which is why the trackball cursor still worked
    // while opening an app did not.
    //
    // Neither hypothesis in DP8_CHECKLIST.md Step 1 was right — it was not
    // cache alignment and not buffer lifetime. push_pixels now splits the push
    // into chunks bounded by spi_bus_get_max_transaction_len(), which the panel
    // cannot see: after RAMWR the ST7789 consumes a continuous pixel stream.
    //
    // Measured before this fix, small flushes already showed the payoff:
    // 6 trans/flush at 1453us, down from 34 at 3251us.
    st7789_set_perf_mode(true);

    // Touch — GT911 I2C (creates I2C bus on port 0)
    // GT911 needs ~300ms from BOARD_POWERON before I2C responds.
    // Display init (SWRESET 150ms + ~30ms GRAM clear) counts toward that budget.
    // Wait the remainder — 400ms gives comfortable margin.
    // Use polling mode (int_pin=-1): avoids GPIO ISR and is simpler to diagnose.
    vTaskDelay(pdMS_TO_TICKS(400));

    // RST=17, matching PURR OS 0.11's devices/tdeck_plus/hal_touch.cpp
    // exactly — this "plug and play" driver previously configured RST as
    // not-connected (-1), skipping the hardware reset pulse 0.11 always
    // did before talking to the chip. Found by direct comparison against
    // 0.11's code once the touch instability traced back to "this only
    // started after the driver became plug-and-play."
    gt911_configure(18, 8, -1, 17, 0);   // SDA=18 SCL=8 poll-mode RST=17 port=0
    if (gt911_drv_init() != 0) {
        ESP_LOGW(TAG, "GT911 touch init failed — continuing without touch");
    }

    // Trackball — GPIO
    if (trackball_drv_init() != 0) {
        ESP_LOGW(TAG, "trackball init failed — continuing without trackball");
    }


    // Keyboard — BB Q20 (joins GT911's I2C bus on port 0)
#if CONFIG_PURR_TDECK_PLUS_PHYSICAL_KEYBOARD
    if (bbq20_drv_init() != 0) {
        ESP_LOGW(TAG, "BB Q20 keyboard init failed — continuing without keyboard");
    }
#else
    ESP_LOGI(TAG, "physical keyboard disabled via Kconfig — on-screen keyboard only");
#endif

    ESP_LOGI(TAG, "baked-in drivers ready");
    boot_splash_advance();

    // Checked here — after display/touch (Layer 0) are up, so the blue
    // recoverable panic screen can actually render/accept touch if this
    // trips, but before any P2/P3 module (including the UI backend
    // itself) gets a chance to load. See purr_crash_guard.h for the full
    // design: this correlates an unclean reset (a real hard crash) against
    // a breadcrumb left over from the previous boot.
    purr_crash_guard_check_reset_reason();

    // ── Phase 1: plug-and-play modules ───────────────────────────────────────
    //
    // Only modules NOT baked in above: UI backend, app_manager, radio (SX1262),
    // GPS, meshtastic. purr_register_static_modules() is generated by purrstrap
    // from device.pcat and omits display/touch/trackball/keyboard for this device.
    //
    // sx1262_rl_configure() must run before purr_kernel_load_static_modules()
    // below, since that's what calls the radio module's own module_init() ->
    // sx1262_rl_init(), which reads these pins at SPI-bus-setup time.
    //
    // Guarded: this call is unconditional in source, but the SYMBOL only
    // exists when source/drivers/radio/sx1262_rl/ is actually compiled,
    // which is device.pcat's [drivers].radio choice, not anything this
    // specialized kernel controls itself — a device with that cleared
    // (the "minimal" build profile; see purrstrap.py's apply_build_profile())
    // would otherwise fail at LINK time with "undefined reference to
    // sx1262_rl_configure", the exact gap PURR_OS_1.0_CHECKLIST.md's
    // "[drivers] are not freely toggleable on a device with a specialized
    // kernel" entry described. PURR_HAS_RADIO_DRIVER is purrstrap-generated
    // (see CoreOS/main/CMakeLists.txt); the #ifndef fallback below defaults
    // to 1 so a build that bypasses purrstrap keeps today's behaviour.
#ifndef PURR_HAS_RADIO_DRIVER
#define PURR_HAS_RADIO_DRIVER 1
#endif
#if PURR_HAS_RADIO_DRIVER
    sx1262_rl_configure(TDP_LORA_MOSI, TDP_LORA_MISO, TDP_LORA_SCLK,
                         TDP_LORA_CS, TDP_LORA_RST, TDP_LORA_BUSY, TDP_LORA_IRQ);
    // Default SPI2_HOST (sx1262_rl_set_spi_host() available if a future fix
    // needs to move this — see mount_sd_vfs()'s comment).
#else
    ESP_LOGI(TAG, "drivers.radio unset in device.pcat — skipping sx1262_rl_configure()");
#endif

    ESP_LOGI(TAG, "=== phase 1: static modules ===");
    extern void purr_register_static_modules(void);
    purr_register_static_modules();
    purr_kernel_load_static_modules();
    boot_splash_advance();

    // Ends the "recovering" window — every device (SD/display above,
    // radio/UI/everything else via load_one_static()'s own gating inside
    // purr_kernel_load_static_modules()) has now had its one bounded
    // bring-up attempt for this boot. Must run AFTER the call above, not
    // before — load_one_static() needs the flag still set while Phase 1
    // is running.
    if (recovering) {
        purr_crash_guard_clear_pending_recovery();
    }

    // bt_mgr no longer brings the NimBLE controller/host up here (or
    // anywhere in boot) — confirmed live that doing so unconditionally at
    // boot permanently starved this board's small internal DMA-capable
    // memory pool within seconds, breaking every SD card read (and
    // therefore .meow/.hiss script loading) for the rest of boot.
    // Bring-up is now lazy: bt_mgr_ensure_active() runs the first time a
    // user actually asks for Bluetooth (Settings' toggle, or Meshtastic's
    // BLE phone-companion toggle). See bt_mgr.h/mesh_ble.c for the full
    // story.

    // app_manager's own init() scans for apps before the P3 system apps
    // (settings/about/terminal/fileman/calculator) have registered, so its
    // first scan always finds 0. Re-scan now that every priority tier above
    // has loaded — by here the registry is complete.
    // Reuses `recovering`, captured earlier in this same function (Phase 0)
    // — by this point purr_crash_guard_clear_pending_recovery() has already
    // run, so re-reading the NVS flag here would incorrectly say "not
    // recovering" even on a boot that very much still is. See
    // app_manager_scan_ex()'s comment for why SD gets skipped specifically.
    extern int app_manager_scan_ex(bool include_sd);
    app_manager_scan_ex(!recovering);
    purr_kernel_set_boot_ready(true);

    // ── Phase 2: SD extras ───────────────────────────────────────────────────

    if (purr_kernel_sd_available()) {
        ESP_LOGI(TAG, "=== phase 2: SD extras ===");
        // One-time snapshot of the MALLOC_CAP_DMA pool right at the point
        // where live captures show "esp_dma_capable_malloc(172): Not
        // enough heap memory" -> "sdmmc_read_blocks failed" starting to
        // happen (this scan is the first thing to touch the SD card after
        // bt_mgr/wifi_mgr/meshtastic have all loaded). purr_kernel.c's
        // periodic heapwatch log only ticks every 2s and its first tick
        // lands ~800ms+ after this point — too late to see the pool state
        // at the actual moment of failure. dma_free is the reserved
        // CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL pool specifically (32KB by
        // default), distinct from and narrower than internal_free.
        ESP_LOGW(TAG, "phase 2 DMA pool: dma_free=%u largest_dma=%u internal_free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        purr_kernel_scan_modules("/sdcard/modules", NULL);
        purr_kernel_scan_modules("/sdcard/drivers", NULL);
    }

    boot_splash_advance();

    if (!purr_kernel_display()) {
        ESP_LOGW(TAG, "no display catcall — check ST7789 init");
    }
    if (!purr_kernel_get_module("app_manager")) {
        ESP_LOGW(TAG, "app_manager not loaded");
    }

    boot_splash_advance();   // final step — UI backend's own first paint supersedes this shortly after
    ESP_LOGI(TAG, "boot complete — %u bytes free", (unsigned)purr_kernel_free_ram());
    purr_kernel_notify("PURR OS ready", "T-Deck Plus booted", "kernel");

    xTaskCreate(serial_console_task, "serial_con", 4096, NULL, 1, NULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
