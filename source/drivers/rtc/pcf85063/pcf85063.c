// pcf85063.c — NXP PCF85063 I2C RTC (Waveshare ESP32-S3-ePaper-1.54's
// onboard RTC). Register map (CTRL1=0x00, CTRL2=0x01, OFFSET=0x02,
// RAM=0x03, SEC=0x04, MIN=0x05, HR=0x06, DAY=0x07, WEEKDAY=0x08,
// MONTH=0x09, YEAR=0x0A) and I2C address (0x51) confirmed against the
// vendor's own SensorLib (waveshareteam/ESP32-S3-ePaper-1.54,
// 02_Example/ESP-IDF/V2/02_I2C_PCF85063/components/SensorLib/src/REG/
// PCF85063Constants.h and that example's own user_config.h) — not
// assumed from a generic datasheet reading.
//
// ── Read-only this pass ──────────────────────────────────────────────────
// purr_kernel.h's own time-authority model (purr_kernel_time_set()'s doc
// comment) is built for MULTIPLE sources pushing readings, ranked by
// authority — GPS/NTP already do this. A real RTC driver's other half is
// writing the chip's registers back whenever a higher-authority source
// syncs, so the RTC keeps drifting less than "nothing" between boots even
// after NTP/GPS go away again. That write-back needs some "a fresher
// reading just landed" notification purr_kernel.h doesn't currently
// expose (purr_kernel_time_set() has no subscriber/callback mechanism) —
// a real, separate addition worth doing generically (any future RTC
// driver would want it), not bolted onto this one board's first pass.
// This driver only ever READS the chip once at boot, seeding the coarse
// startup guess exactly the way purr_kernel.c's own NVS fallback already
// does — genuinely better than NVS here (a real battery/cap-backed clock
// vs. "epoch at last accepted sync, however long ago that was"), just not
// yet kept in sync going forward.
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../../../kernel/core/purr_module.h"
#include "../../../kernel/core/purr_kernel.h"

static const char *TAG = "drv:pcf85063";

#ifndef CONFIG_DRV_RTC_SDA_PIN
#  define CONFIG_DRV_RTC_SDA_PIN 47
#endif
#ifndef CONFIG_DRV_RTC_SCL_PIN
#  define CONFIG_DRV_RTC_SCL_PIN 48
#endif

#define PCF85063_I2C_ADDR   0x51
// Standard mode (100kHz), not 300kHz — found live: the schematic
// (files.waveshare.com/wiki/ESP32-S3-ePaper-1.54/ESP32-S3-Touch-ePaper-
// 1.54-Schematic.pdf) shows no external pull-up resistors on this bus,
// relying on the ESP32-S3's own internal ones — which i2c_master.h's own
// enable_internal_pullup doc comment calls out as "not strong enough...
// under high-speed frequency". This bus is also shared with the ES8311
// codec and SHTC3 sensor (same schematic sheet), adding trace/pin
// capacitance neither of those needs to fight at speed. 300kHz produced
// ESP_ERR_INVALID_STATE (a real transaction-timeout, not a config
// error) on real hardware; dropping to the same 100kHz every I2C-based
// RTC reference design defaults to is the concrete next thing to try.
#define PCF85063_I2C_HZ     100000

#define REG_SEC     0x04
#define REG_MIN     0x05
#define REG_HR      0x06
#define REG_DAY     0x07
#define REG_MONTH   0x09
#define REG_YEAR    0x0A

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

static inline uint8_t bcd_to_bin(uint8_t bcd) { return (uint8_t)((bcd >> 4) * 10 + (bcd & 0x0F)); }

static esp_err_t rtc_read_regs(uint8_t start_reg, uint8_t *out, size_t len)
{
    // i2c_master_bus_wait_all_done() first — found live, not theoretical:
    // without it this read failed every time with ESP_ERR_INVALID_STATE
    // on real hardware, right after i2c_master_bus_add_device() with
    // nothing else on the bus yet to actually be busy. Matches the
    // vendor's own working I2C helper (waveshareteam/ESP32-S3-ePaper-1.54,
    // components/i2c_bsp/i2c_bsp.c's i2c_read_buff()), which calls this
    // before every single transaction — copied here for the same reason,
    // not derived from the ESP-IDF driver docs alone.
    esp_err_t err = i2c_master_bus_wait_all_done(s_bus, 1000);
    if (err != ESP_OK) return err;
    return i2c_master_transmit_receive(s_dev, &start_reg, 1, out, len, pdMS_TO_TICKS(100));
}

// Reads SEC..YEAR (7 consecutive registers, REG_SEC..REG_YEAR — WEEKDAY
// at 0x08 falls in the middle and is read along with them but unused, no
// separate transaction needed) and seeds purr_kernel's wall clock.
// Skips entirely (no purr_kernel_time_set() call, just a warning) when
// the SEC register's own OS (oscillator stop) flag — bit 7 — is set: the
// chip itself is telling us its own time is unreliable (e.g. this is a
// brand-new, never-before-powered board), and pushing a reading anyway
// would look like a real synced clock instead of the NVS/none fallback
// this correctly should stay at.
static void rtc_seed_kernel_time(void)
{
    uint8_t regs[7] = {0};
    esp_err_t err = rtc_read_regs(REG_SEC, regs, sizeof(regs));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "register read failed: %s — not seeding wall clock", esp_err_to_name(err));
        return;
    }

    uint8_t sec_raw = regs[REG_SEC - REG_SEC];
    if (sec_raw & 0x80) {
        ESP_LOGW(TAG, "OS (oscillator stop) flag set — chip's own time is unreliable, not seeding");
        return;
    }

    uint8_t sec   = bcd_to_bin(sec_raw & 0x7F);
    uint8_t min   = bcd_to_bin(regs[REG_MIN   - REG_SEC] & 0x7F);
    uint8_t hour  = bcd_to_bin(regs[REG_HR    - REG_SEC] & 0x3F);   // PCF85063 is 24h-only, no AM/PM bit
    uint8_t day   = bcd_to_bin(regs[REG_DAY   - REG_SEC] & 0x3F);
    uint8_t month = bcd_to_bin(regs[REG_MONTH - REG_SEC] & 0x1F);
    uint16_t year = (uint16_t)(2000 + bcd_to_bin(regs[REG_YEAR - REG_SEC]));

    time_t epoch = purr_kernel_time_from_utc_calendar(year, month, day, hour, min, sec);
    purr_kernel_time_set(PURR_TIME_SOURCE_RTC_HW, epoch);
    ESP_LOGI(TAG, "seeded wall clock from RTC: %04u-%02u-%02u %02u:%02u:%02u UTC",
             year, month, day, hour, min, sec);
}

static int module_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = CONFIG_DRV_RTC_SDA_PIN,
        .scl_io_num        = CONFIG_DRV_RTC_SCL_PIN,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return -1;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PCF85063_I2C_ADDR,
        .scl_speed_hz    = PCF85063_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return -1;
    }

    rtc_seed_kernel_time();
    return 0;
}

static void module_deinit(void)
{
    if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
    if (s_bus) { i2c_del_master_bus(s_bus); s_bus = NULL; }
}

PURR_MODULE_REGISTER(pcf85063) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "pcf85063",
    .version           = "0.1.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = module_init,
    .deinit            = module_deinit,
};
