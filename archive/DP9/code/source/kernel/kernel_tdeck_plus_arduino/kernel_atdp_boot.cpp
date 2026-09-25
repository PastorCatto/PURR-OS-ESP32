// kernel_atdp_boot.cpp — Arduino-backed boot for T-Deck Plus
//
// Uses Arduino Wire for I2C instead of IDF 5.3 i2c_master (which has a
// known NACK/ESP_ERR_INVALID_STATE regression on this hardware).
//
// Hardware-only kernel: brings up every baked-in peripheral and registers
// catcalls, then hands off to the static module loader. No hardcoded UI or
// apps live here — this is the foundation a UI module (e.g. MiniWin) bolts
// onto via device.pcat's [modules] section.
//
// Baked-in (Layer 0):
//   ST7789  — SPI display  (CS=12 DC=11 MOSI=41 SCLK=40 RST=-1 BL=42)
//   GT911   — I2C touch    (SDA=18 SCL=8 INT=16 RST=NC) via Wire
//   Trackball — GPIO       (UP=3 DN=15 LT=1 RT=2 CLK=0)
//   BBQ20   — I2C keyboard (SDA=18 SCL=8 addr=0x55) via Wire
//   SD card — shares display SPI bus (SPI2_HOST), CS=39
//   LoRa SX1262/SX1276 — shares display SPI bus (SPI2_HOST), CS=9
//   PMIC — I2C scan: IP5306 (0x75), AXP2101 (0x34), MAX17048 (0x36)
//   GPS L76K — UART2 (RX=44 TX=43, 9600 baud)
//   Bluetooth — BLE controller
//   WiFi — STA mode

#include "Arduino.h"
#include "Wire.h"
#include "HardwareSerial.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/semphr.h"
#include <sys/stat.h>
#include <string.h>

extern "C" {
#include "../kernel_arduino/kernel_arduino.h"
#include "../../drivers/display/st7789/st7789.h"
#include "../../drivers/input/trackball/trackball.h"
}

#include "wince_shell.h"

// ── Pin assignments ───────────────────────────────────────────────────────────

#define TDP_DISPLAY_CS    12
#define TDP_DISPLAY_DC    11
#define TDP_DISPLAY_MOSI  41
#define TDP_DISPLAY_MISO  38
#define TDP_DISPLAY_SCLK  40
#define TDP_DISPLAY_RST   (-1)
#define TDP_DISPLAY_BL    42

#define TDP_I2C_SDA       18
#define TDP_I2C_SCL        8
#define TDP_TOUCH_INT     16
#define TDP_BOARD_POWERON 10

#define GT911_ADDR        0x5D
#define GT911_ADDR_ALT    0x14
#define GT911_REG_STATUS  0x814E
#define GT911_REG_POINT1  0x8150
#define GT911_REG_PRODUCT 0x8140

#define BBQ20_ADDR        0x55

// SD card — shares display SPI bus (MOSI=41, SCLK=40), separate CS
#define SD_CS    39
#define SD_MISO  38

// LoRa SX1262/SX1276 — shares display SPI bus (SPI2_HOST)
#define LORA_MOSI 41
#define LORA_MISO 38
#define LORA_SCLK 40
#define LORA_CS   9
#define LORA_RST  17
#define LORA_IRQ  45
#define LORA_BUSY 46

// GPS L76K — UART2
#define GPS_RX_PIN 44   // data from GPS module
#define GPS_TX_PIN 43   // data to GPS module

// PMIC — scan multiple known battery gauge addresses
// Note: 0x55 is deliberately excluded — it's the BBQ20 keyboard's address,
// not a PMIC. A prior version misidentified BBQ20 as a "BQ27220" PMIC.
#define IP5306_ADDR   0x75
#define AXP2101_ADDR  0x34
#define MAX17048_ADDR 0x36

static const char *TAG = "atdp_boot";

// ── Wire mutex — protects all Wire I2C calls across tasks ─────────────────────

static SemaphoreHandle_t s_wire_mutex = NULL;

static inline void wire_lock(void)   { if (s_wire_mutex) xSemaphoreTake(s_wire_mutex, portMAX_DELAY); }
static inline void wire_unlock(void) { if (s_wire_mutex) xSemaphoreGive(s_wire_mutex); }

// ── GT911 Wire helpers ────────────────────────────────────────────────────────

static uint8_t s_gt911_addr = GT911_ADDR;
static bool    s_gt911_ok   = false;

static bool gt911_write_reg(uint16_t reg, uint8_t val)
{
    wire_lock();
    Wire.beginTransmission(s_gt911_addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    Wire.write(val);
    bool ok = Wire.endTransmission() == 0;
    wire_unlock();
    return ok;
}

static bool gt911_read_reg(uint16_t reg, uint8_t *buf, size_t len)
{
    wire_lock();
    Wire.beginTransmission(s_gt911_addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    bool ok = Wire.endTransmission(false) == 0;
    if (ok) {
        Wire.requestFrom(s_gt911_addr, (uint8_t)len);
        for (size_t i = 0; i < len; i++) {
            if (!Wire.available()) { ok = false; break; }
            buf[i] = Wire.read();
        }
    }
    wire_unlock();
    return ok;
}

static bool gt911_probe(uint8_t addr)
{
    wire_lock();
    Wire.beginTransmission(addr);
    Wire.write((uint8_t)(GT911_REG_PRODUCT >> 8));
    Wire.write((uint8_t)(GT911_REG_PRODUCT & 0xFF));
    bool ok = Wire.endTransmission(false) == 0;
    char pid[5] = {0};
    if (ok) {
        Wire.requestFrom(addr, (uint8_t)4);
        for (int i = 0; i < 4 && Wire.available(); i++) pid[i] = (char)Wire.read();
    }
    wire_unlock();
    ESP_LOGI(TAG, "GT911 at 0x%02X — ID: %s", addr, ok ? pid : "N/A");
    return ok;
}

// ── catcall_touch_t implementation ────────────────────────────────────────────

// GT911 command register — periodically re-writing 0x00 (normal scan mode)
// keeps the chip reporting touches. Without this it stops updating its
// status/point registers after the first touch (observed: touch works once,
// then goes dead). Same fix already proven in kernel_tdp_mobile_ui.cpp.
#define GT911_REG_COMMAND 0x8040
static uint32_t s_gt911_keepalive_at = 0;

static void gt911_keepalive(void)
{
    uint32_t now = (uint32_t)millis();
    if (s_gt911_keepalive_at != 0 && now < s_gt911_keepalive_at) return;
    s_gt911_keepalive_at = now + 2000;
    gt911_write_reg(GT911_REG_COMMAND, 0x00);
}

static bool gt911_catcall_is_pressed(void)
{
    if (!s_gt911_ok) return false;
    gt911_keepalive();
    uint8_t status = 0;
    if (!gt911_read_reg(GT911_REG_STATUS, &status, 1)) return false;

    // Buffer-ready bit set with a 0 touch count is GT911's "release" event.
    // It must be cleared here or the buffer-ready flag stays latched and the
    // chip never reports the next touch (this was the "works once, then
    // dies" bug — the keepalive write alone doesn't clear this).
    if (status & 0x80) {
        bool down = (status & 0x0F) > 0;
        if (!down) gt911_write_reg(GT911_REG_STATUS, 0x00);
        return down;
    }
    return false;
}

static bool gt911_catcall_read_point(uint16_t *x, uint16_t *y)
{
    if (!s_gt911_ok || !x || !y) return false;
    gt911_keepalive();
    uint8_t status = 0;
    bool read_ok = gt911_read_reg(GT911_REG_STATUS, &status, 1);
    static uint8_t s_dbg_last_status = 0xFF;
    static bool    s_dbg_last_ok     = true;
    if (status != s_dbg_last_status || read_ok != s_dbg_last_ok) {
        s_dbg_last_status = status;
        s_dbg_last_ok     = read_ok;
        ESP_LOGI(TAG, "DBG status edge: read_ok=%d status=0x%02X", read_ok, status);
    }
    if (!read_ok) return false;

    if (!(status & 0x80)) return false;
    if ((status & 0x0F) == 0) {
        // Release event — clear the buffer-ready latch so the next touch
        // is reported (see gt911_catcall_is_pressed comment above).
        gt911_write_reg(GT911_REG_STATUS, 0x00);
        return false;
    }

    uint8_t pt[5] = {0};
    bool ok = gt911_read_reg(GT911_REG_POINT1, pt, sizeof(pt));
    gt911_write_reg(GT911_REG_STATUS, 0x00);
    if (!ok) return false;

    // pt: [track_id, x_low, x_high, y_low, y_high]
    // Return genuinely raw, unscaled digitiser units here — any
    // rescale-to-pixel-space work belongs to whichever UI backend's HAL
    // consumes this (each one needs a different target range), not baked
    // into the shared driver. See meow420_hal.c's touch_read_cb for the
    // current backend's swap+scale, derived from LilyGo's own reference
    // driver (TouchDrvGT911: setSwapXY(true), setMirrorXY(false,true)) plus
    // this board's live-captured raw bounds.
    *x = (uint16_t)pt[1] | ((uint16_t)pt[2] << 8);
    *y = (uint16_t)pt[3] | ((uint16_t)pt[4] << 8);
    return true;
}

static esp_err_t gt911_catcall_init(const touch_config_t *cfg)   { return ESP_OK; }
static esp_err_t gt911_catcall_deinit(void)                      { return ESP_OK; }

static const catcall_touch_t s_touch_catcall = {
    .name            = "gt911_wire",
    .catcall_version = CATCALL_TOUCH_VERSION,
    .init            = gt911_catcall_init,
    .read_point      = gt911_catcall_read_point,
    .is_pressed      = gt911_catcall_is_pressed,
    .deinit          = gt911_catcall_deinit,
};

// ── BBQ20 keyboard (Wire) ─────────────────────────────────────────────────────

static QueueHandle_t s_kb_queue = NULL;
static bool          s_kb_ok    = false;

static void bbq20_poll_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));
        wire_lock();
        Wire.requestFrom((uint8_t)BBQ20_ADDR, (uint8_t)1);
        bool avail = Wire.available();
        uint8_t key = avail ? Wire.read() : 0;
        wire_unlock();
        if (!avail || key == 0) continue;
        input_event_t ev = { .type = INPUT_EVENT_KEY_DOWN, .keycode = key };
        if (s_kb_queue) xQueueSend(s_kb_queue, &ev, 0);
    }
}

static bool bbq20_poll_event(input_event_t *out)
{
    if (!s_kb_queue || !out) return false;
    return xQueueReceive(s_kb_queue, out, 0) == pdTRUE;
}

static esp_err_t bbq20_catcall_init(void)  { return ESP_OK; }
static esp_err_t bbq20_catcall_deinit(void){ return ESP_OK; }

static const catcall_input_t s_kb_catcall = {
    .name            = "bbq20_wire",
    .catcall_version = CATCALL_INPUT_VERSION,
    .init            = bbq20_catcall_init,
    .poll_event      = bbq20_poll_event,
    .deinit          = bbq20_catcall_deinit,
};

// ── PMIC — scan multiple known battery gauge addresses ────────────────────────

typedef enum { PMIC_NONE=0, PMIC_IP5306, PMIC_AXP2101, PMIC_MAX17048 } pmic_type_t;
static pmic_type_t s_pmic_type = PMIC_NONE;
static bool        s_pmic_ok   = false;

static uint8_t i2c_read_reg(uint8_t addr, uint8_t reg)
{
    wire_lock();
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom(addr, (uint8_t)1);
    uint8_t v = Wire.available() ? Wire.read() : 0;
    wire_unlock();
    return v;
}

// Returns 1-4 bars, 5=charging, -1=not found
static int pmic_battery_bars(void)
{
    if (!s_pmic_ok) {
        // ADC fallback: battery voltage divider. T-Deck Plus exposes VBAT/2 on GPIO4.
        int raw = analogRead(4);
        float vbat = raw * 3.3f / 4095.0f * 2.0f;
        if (vbat >= 4.0f) return 4;
        if (vbat >= 3.7f) return 3;
        if (vbat >= 3.5f) return 2;
        if (vbat >= 3.1f) return 1;
        return 1;
    }

    if (s_pmic_type == PMIC_AXP2101) {
        uint8_t stat = i2c_read_reg(AXP2101_ADDR, 0x01);
        uint8_t pct  = i2c_read_reg(AXP2101_ADDR, 0xA4);
        if (stat & 0x20) return 5;
        return (pct >= 75) ? 4 : (pct >= 50) ? 3 : (pct >= 25) ? 2 : 1;
    }
    if (s_pmic_type == PMIC_IP5306) {
        uint8_t sys = i2c_read_reg(IP5306_ADDR, 0x78);
        uint8_t lvl = i2c_read_reg(IP5306_ADDR, 0x7A);
        if (sys & 0x08) return 5;
        return (int)((lvl >> 4) & 0x03) + 1;
    }
    if (s_pmic_type == PMIC_MAX17048) {
        uint8_t pct = i2c_read_reg(MAX17048_ADDR, 0x04);
        return (pct >= 75) ? 4 : (pct >= 50) ? 3 : (pct >= 25) ? 2 : 1;
    }
    return -1;
}

// ── LoRa SX1262/SX1276 — third device on shared SPI2_HOST ────────────────────

static bool                s_lora_ok      = false;
static spi_device_handle_t s_lora_spi_dev = NULL;

static uint8_t lora_reg_read(uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)(reg & 0x7F), 0x00 };
    uint8_t rx[2] = { 0, 0 };
    spi_transaction_t t = {};
    t.length    = 16;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    spi_device_transmit(s_lora_spi_dev, &t);
    return rx[1];
}

static bool lora_probe(void)
{
    // RST pulse — before SPI init so pin is just GPIO
    gpio_set_direction((gpio_num_t)LORA_RST, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)LORA_RST, 0);  delay(20);
    gpio_set_level((gpio_num_t)LORA_RST, 1);  delay(50);

    // LoRa shares SPI2_HOST with display and SD — bus already initialized by st7789
    spi_device_interface_config_t dev = {};
    dev.mode           = 0;
    dev.clock_speed_hz = 1000000;  // 1 MHz probe speed
    dev.spics_io_num   = LORA_CS;
    dev.queue_size     = 1;

    esp_err_t e = spi_bus_add_device(SPI2_HOST, &dev, &s_lora_spi_dev);
    if (e != ESP_OK) {
        printf("[LoRa] add device err=0x%x\r\n", e);
        return false;
    }

    // Try SX1276: version register 0x42 should read 0x12
    uint8_t ver = lora_reg_read(0x42);
    printf("[LoRa] reg42=0x%02X\r\n", ver);
    if (ver == 0x12) {
        puts("[LoRa] SX1276 detected");
        return true;
    }

    // Try SX1262: GetStatus command 0xC0 — returns non-0xFF chip mode byte
    {
        uint8_t tx[2] = { 0xC0, 0x00 };
        uint8_t rx[2] = { 0, 0 };
        spi_transaction_t t = {};
        t.length    = 16;
        t.tx_buffer = tx;
        t.rx_buffer = rx;
        spi_device_transmit(s_lora_spi_dev, &t);
        printf("[LoRa] SX1262 status=0x%02X 0x%02X\r\n", rx[0], rx[1]);
        if ((rx[0] != 0x00 && rx[0] != 0xFF) || (rx[1] != 0x00 && rx[1] != 0xFF)) {
            puts("[LoRa] SX1262 detected");
            return true;
        }
    }

    puts("[LoRa] no radio response");
    return false;
}

// ── GPS L76K via UART2 ────────────────────────────────────────────────────────

static HardwareSerial s_gps_serial(2);
static bool s_gps_started = false;

// ── Bluetooth BLE controller ──────────────────────────────────────────────────
// Deferred: Arduino's BLEDevice wrapper (esp-nimble-cpp) needs Kconfig symbols
// this build doesn't carry yet. BT/BLE is parked here until MiniWin boots and
// we wire it up directly against the IDF/NimBLE stack instead of the Arduino
// shim.

static bool s_bt_ok = false;

static void bt_init_ble(void)
{
    printf("[--] BLE init skipped (deferred — Arduino BLE shim disabled)\r\n");
}

// ── WiFi ──────────────────────────────────────────────────────────────────────

static bool s_wifi_started = false;

static void wifi_init_sta(void)
{
    // Silences ESP-IDF's internal "Haven't to connect to a suitable AP now!"
    // spam — it logs at WARN every reconnect attempt while no AP is in range.
    esp_log_level_set("wifi", ESP_LOG_ERROR);

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    s_wifi_started = true;
}

// ── SD card ────────────────────────────────────────────────────────────────────

static bool          s_sd_ok   = false;
static sdmmc_card_t *s_sd_card = NULL;

static void ensure_sd_dirs(void)
{
    if (!purr_kernel_sd_available()) return;
    const char *dirs[] = {
        "/sdcard/apps", "/sdcard/modules", "/sdcard/drivers",
        "/sdcard/drivers/display", "/sdcard/drivers/touch",
        "/sdcard/drivers/input", "/sdcard/drivers/radio",
        "/sdcard/drivers/gps", "/sdcard/system", "/sdcard/system/logs", NULL
    };
    for (int i = 0; dirs[i]; i++) {
        struct stat st;
        if (stat(dirs[i], &st) != 0) mkdir(dirs[i], 0755);
    }
}

// ── Serial console ────────────────────────────────────────────────────────────

static void serial_console_task(void *arg)
{
    // Use Arduino Serial — avoids uart_driver_install conflict with Arduino framework
    Serial.begin(115200);
    while (!Serial) vTaskDelay(pdMS_TO_TICKS(10));

    char line[32]; int len = 0;
    Serial.print("\r\nPURR OS console (atdp) — commands: scan, status\r\n> ");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        while (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\r' || c == '\n') {
                Serial.print("\r\n");
                line[len] = '\0'; len = 0;
                if (strcmp(line, "scan") == 0) {
                    Serial.print("[SCAN] Wire I2C scan (SDA=18 SCL=8):\r\n");
                    int found = 0;
                    for (uint8_t a = 0x03; a <= 0x77; a++) {
                        wire_lock();
                        Wire.beginTransmission(a);
                        bool ack = Wire.endTransmission() == 0;
                        wire_unlock();
                        if (ack) {
                            Serial.printf("[SCAN]   0x%02X ACK\r\n", a);
                            found++;
                        }
                    }
                    if (!found) Serial.print("[SCAN]   no devices found\r\n");
                    Serial.printf("[SCAN] done (%d device(s))\r\n", found);
                } else if (strcmp(line, "status") == 0) {
                    Serial.printf("[STATUS] touch=%d kb=%d pmic=%d(bars=%d) lora=%d sd=%d gps=%d bt=%d wifi=%d\r\n",
                        s_gt911_ok, s_kb_ok, (int)s_pmic_type, pmic_battery_bars(),
                        s_lora_ok, s_sd_ok, s_gps_started, s_bt_ok, s_wifi_started);
                } else if (line[0]) {
                    Serial.printf("unknown: %s\r\n", line);
                }
                Serial.print("> ");
            } else if ((c == 0x7F || c == '\b') && len > 0) {
                len--; Serial.print("\b \b");
            } else if (len < (int)sizeof(line) - 1 && c >= 0x20) {
                line[len++] = c; Serial.print(c);
            }
        }
    }
}

// ── app_main ──────────────────────────────────────────────────────────────────

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "PURR OS %s / KITT %s  T-Deck Plus (Arduino kernel) booting...",
             PURR_KERNEL_VERSION, KITT_VERSION);
    ESP_LOGW(TAG, "kernel_tdeck_plus_arduino is DEPRECATED — use kernel_tdeck_plus (device 'tdeck_plus') instead. "
                  "This kernel is scheduled for removal once on-hardware validation of the IDF kernel's GT911 fix passes.");

    arduino_kernel_nvs_init();
    arduino_kernel_spiffs_init(TAG);

    ESP_LOGI(TAG, "=== phase 0: baked-in drivers ===");

    // Deassert SD and LoRa CS BEFORE BOARD_POWERON so those chips power up
    // with CS already high — prevents SD entering an undefined SPI state.
    gpio_set_direction((gpio_num_t)SD_CS,   GPIO_MODE_OUTPUT); gpio_set_level((gpio_num_t)SD_CS,   1);
    gpio_set_direction((gpio_num_t)LORA_CS, GPIO_MODE_OUTPUT); gpio_set_level((gpio_num_t)LORA_CS, 1);
    // Explicit MISO pull-up before the bus is claimed by st7789
    gpio_set_direction((gpio_num_t)SD_MISO, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)SD_MISO, GPIO_PULLUP_ONLY);

    // BOARD_POWERON — gates all I2C/SPI peripherals
    // INT LOW before power-on anchors GT911 I2C address at 0x5D
    gpio_set_direction(GPIO_NUM_16, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_16, 0);
    gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_10, 1);
    ESP_LOGI(TAG, "BOARD_POWERON HIGH");
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_direction(GPIO_NUM_16, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Display — ST7789 SPI (hand-rolled driver). MISO shared with SD/LoRa.
    st7789_configure(TDP_DISPLAY_CS, TDP_DISPLAY_DC, TDP_DISPLAY_MOSI,
                     TDP_DISPLAY_MISO, TDP_DISPLAY_SCLK, TDP_DISPLAY_RST, TDP_DISPLAY_BL);
    if (st7789_drv_init() != 0) {
        purr_kernel_panic("ST7789 display init failed");
    }

    // Wire mutex must exist before any task that calls Wire
    s_wire_mutex = xSemaphoreCreateMutex();

    // I2C via Arduino Wire — bypasses IDF 5.3 i2c_master regression
    Wire.begin(TDP_I2C_SDA, TDP_I2C_SCL);
    Wire.setClock(400000);
    vTaskDelay(pdMS_TO_TICKS(300));

    // GT911 touch — probe both addresses
    if (gt911_probe(GT911_ADDR)) {
        s_gt911_addr = GT911_ADDR;
        s_gt911_ok   = true;
    } else if (gt911_probe(GT911_ADDR_ALT)) {
        s_gt911_addr = GT911_ADDR_ALT;
        s_gt911_ok   = true;
    } else {
        ESP_LOGW(TAG, "GT911 not found — touch disabled");
    }
    if (s_gt911_ok) {
        gt911_write_reg(GT911_REG_STATUS, 0x00);
        purr_kernel_register_touch(&s_touch_catcall);
        ESP_LOGI(TAG, "GT911 touch ready at 0x%02X (Wire)", s_gt911_addr);
    }

    // Trackball — GPIO (unchanged)
    if (trackball_drv_init() != 0)
        ESP_LOGW(TAG, "trackball init failed");

    // BBQ20 keyboard — polled via Wire
    wire_lock();
    Wire.beginTransmission(BBQ20_ADDR);
    s_kb_ok = (Wire.endTransmission() == 0);
    wire_unlock();
    if (s_kb_ok) {
        s_kb_queue = xQueueCreate(32, sizeof(input_event_t));
        xTaskCreate(bbq20_poll_task, "bbq20", 2048, NULL, 3, NULL);
        purr_kernel_register_input(&s_kb_catcall);
        ESP_LOGI(TAG, "BBQ20 keyboard ready (Wire)");
    } else {
        ESP_LOGW(TAG, "BBQ20 not found — keyboard disabled");
    }

    // PMIC — scan known battery gauge addresses. 0x55 (BBQ20) deliberately excluded.
    {
        static const struct { uint8_t addr; pmic_type_t type; const char *name; }
        pmic_addrs[] = {
            { IP5306_ADDR,   PMIC_IP5306,   "IP5306"   },
            { AXP2101_ADDR,  PMIC_AXP2101,  "AXP2101"  },
            { MAX17048_ADDR, PMIC_MAX17048, "MAX17048" },
        };
        for (auto &p : pmic_addrs) {
            wire_lock();
            Wire.beginTransmission(p.addr);
            bool found = (Wire.endTransmission() == 0);
            wire_unlock();
            if (found) {
                s_pmic_type = p.type;
                s_pmic_ok   = true;
                ESP_LOGI(TAG, "%s PMIC @ 0x%02X", p.name, p.addr);
                break;
            }
        }
        if (!s_pmic_ok) {
            ESP_LOGI(TAG, "No I2C PMIC found — using ADC battery estimate (GPIO4)");
        }
    }

    // SD card — third device on shared SPI2_HOST (bus already init by display)
    {
        sdmmc_host_t host          = SDSPI_HOST_DEFAULT();
        host.slot                  = SPI2_HOST;
        host.max_freq_khz          = SDMMC_FREQ_PROBING;  // 400 kHz probe — avoids CMD8 framing errors

        sdspi_device_config_t sd_slot = SDSPI_DEVICE_CONFIG_DEFAULT();
        sd_slot.host_id  = SPI2_HOST;
        sd_slot.gpio_cs  = (gpio_num_t)SD_CS;
        sd_slot.gpio_cd  = SDSPI_SLOT_NO_CD;
        sd_slot.gpio_wp  = SDSPI_SLOT_NO_WP;
        sd_slot.gpio_int = SDSPI_SLOT_NO_INT;

        esp_vfs_fat_sdmmc_mount_config_t mcfg = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 16 * 1024,
        };
        esp_err_t e = esp_vfs_fat_sdspi_mount("/sdcard", &host, &sd_slot, &mcfg, &s_sd_card);
        s_sd_ok = (e == ESP_OK);
        purr_kernel_set_sd_available(s_sd_ok);
        ESP_LOGI(TAG, "%s SD card err=0x%x", s_sd_ok ? "OK" : "WARN", e);
        ensure_sd_dirs();
    }

    // LoRa SX1262/SX1276
    s_lora_ok = lora_probe();
    purr_kernel_set_lora_available(s_lora_ok);
    ESP_LOGI(TAG, "%s LoRa radio", s_lora_ok ? "OK" : "WARN");

    // GPS UART2
    s_gps_serial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    s_gps_started = true;
    ESP_LOGI(TAG, "GPS UART started (waiting for NMEA)");

    // Bluetooth BLE
    bt_init_ble();

    // WiFi STA
    wifi_init_sta();
    ESP_LOGI(TAG, "WiFi started");

    ESP_LOGI(TAG, "baked-in drivers ready");

    ESP_LOGI(TAG, "=== phase 1: static modules ===");
    extern void purr_register_static_modules(void);
    purr_register_static_modules();
    purr_kernel_load_static_modules();

#if CONFIG_PURR_UI_WINCE_SHELL
    // WinCE shell — baked directly into this kernel, no .purr module wrapper.
    // miniwin_module.c skips its own desktop/task for this device (see
    // CONFIG_PURR_UI_WINCE_SHELL in sdkconfig_tdeck_plus_arduino). Only
    // relevant when MiniWin is the selected UI backend — other backends
    // (e.g. Meow420) start themselves as a normal static module above.
    wince_shell_start();
#endif

    if (purr_kernel_sd_available()) {
        ESP_LOGI(TAG, "=== phase 2: SD extras ===");
        purr_kernel_scan_modules("/sdcard/modules", NULL);
        purr_kernel_scan_modules("/sdcard/drivers", NULL);
    }

    ESP_LOGI(TAG, "boot complete — %u bytes free", (unsigned)purr_kernel_free_ram());
    xTaskCreate(serial_console_task, "serial_con", 4096, NULL, 1, NULL);

    // Periodic status loop — keeps kernel-level accessors (read by MiniWin's
    // status bar) up to date as WiFi association and battery level change.
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3000));

        wifi_ap_record_t ap_info;
        bool connected = s_wifi_started && (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
        purr_kernel_set_wifi_connected(connected);

        if (s_pmic_ok) {
            // pmic_battery_bars() returns 1-5 bars — scale to an approximate
            // 0-100 percent for display.
            int bars = pmic_battery_bars();
            purr_kernel_set_battery_percent(bars > 0 ? (bars * 100) / 5 : -1);
        }
    }
}
