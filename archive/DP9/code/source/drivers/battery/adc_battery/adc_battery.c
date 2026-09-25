// adc_battery.c — single-pin ADC battery voltage/percent driver.
//
// No fuel-gauge IC on this board — LilyGo's T-Deck/T-Deck Plus measure
// battery voltage the same simple way Meshtastic's own firmware does
// (confirmed against their variant.h): a resistive divider on GPIO4 into
// ADC1, ~2:1 ratio. Pushes into the same plain kernel-global pattern
// wifi_mgr/bt_mgr/mesh already use for sd_available/wifi_connected/
// lora_available — no catcall_battery_t contract exists (or is needed);
// nothing else in the kernel dispatches through a battery driver, it's
// purely push-based status.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

#include "../../../kernel/core/purr_module.h"
#include "../../../kernel/core/purr_kernel.h"
#include "purr_quirk.h"

static const char *TAG = "adc_battery";

// Layout for a "adc_battery.config" .purr v2 quirk block — see
// purr_quirk_pkg.h. Deliberately plain int/float fields, matching
// adc_battery_configure()'s own parameter list exactly (channel/ctrl_pin
// as int rather than the enum types the driver uses internally, same
// reasoning that function's own signature already follows: a stable,
// simple wire shape rather than exposing enum layout details a future
// ESP-IDF version could change). A generator producing this block must
// match this exact layout (struct packing/alignment on this toolchain);
// see purrstrap.py's own quirk-package generator for the Python-side
// struct.pack() that has to stay byte-for-byte in sync with it.
typedef struct {
    int32_t channel;
    int32_t ctrl_pin;
    float   multiplier;
    int32_t atten;
} adc_battery_quirk_t;

// GPIO4 / ADC1_CHANNEL_3, x2.11 — confirmed against Meshtastic's own
// T-Deck variant.h (`#define BATTERY_PIN 4`, `ADC1_GPIO4_CHANNEL`). This
// remains the default/fallback since it's this driver's original board
// (T-Deck/T-Deck Plus) and most devices never call adc_battery_configure()
// at all. See that function's comment for how a second board (confirmed:
// Heltec V3, different pin/multiplier/an enable pin this board never
// needed) overrides these at boot instead of adding more #ifndef configs
// here — device.pcat's CONFIG_DRV_*_PIN glue macros are scoped to the
// generated translation unit and never reach a driver .c file directly
// (see bbq20.c's KB_BL_PIN comment for the same limitation), so a runtime
// configure() call is this project's established pattern for this exact
// problem (see sx1262_rl_configure()/ssd1306_configure()).
#ifndef BATTERY_ADC_CHANNEL
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_3
#endif

// Voltage divider ratio (RD2=100k, RD3=100k => 2.0), plus Meshtastic's own
// +10% correction factor for display-load undervoltage — same source.
#define BATTERY_ADC_MULTIPLIER 2.11f

#define BATTERY_POLL_MS (10 * 1000)

static adc_channel_t             s_channel    = BATTERY_ADC_CHANNEL;
static float                     s_multiplier = BATTERY_ADC_MULTIPLIER;
static int                       s_ctrl_pin   = -1;   // -1 == no enable pin needed
static adc_atten_t               s_atten      = ADC_ATTEN_DB_12;
static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t         s_cali = NULL;
static bool                      s_cali_ok = false;
static TaskHandle_t              s_task = NULL;

// Overrides the T-Deck-shaped defaults above for a board whose battery
// divider sits on a different pin/ratio, and — new requirement Heltec V3
// introduced — behind a GPIO that must be driven to the correct level to
// actually connect the divider to the ADC pin at all (confirmed against
// Meshtastic's heltec_v3/variant.h: `ADC_CTRL` GPIO37 feeding `BATTERY_PIN`
// GPIO1 / `ADC_CHANNEL_0`, multiplier `4.9 * 1.045`).
//
// The generic `ADC_CTRL_ENABLED LOW` macro in that variant.h is NOT what
// Meshtastic's own Power.cpp actually uses for this specific board —
// battery_adcEnable() has a HELTEC_V3-only branch that skips the fixed
// level entirely: it reads ADC_CTRL as a floating input first (sensing
// whatever this board revision's own external pull resistor already
// holds it at), then drives the pin OUTPUT to the *inverse* of that
// reading. Different Heltec V3 board revisions apparently wire the
// enable transistor with opposite polarity, so a hardcoded LOW works on
// some boards and silently reads ~0V (divider never connected) on
// others — this auto-detect dance is how upstream copes with that.
// Applied unconditionally whenever a ctrl_pin is configured, since only
// Heltec V3 uses one today and this exactly mirrors upstream's handling
// of it.
//
// Must be called before module_init() runs — i.e. from a device's
// purr_device_init(), same timing purr_radio_init()'s Vext GPIO block
// already uses, and for the same reason (this needs to be live before
// module_init() ever touches the ADC channel). ctrl_pin is driven once
// here and left low permanently, same "set it and forget it" treatment
// Vext gets — this driver's 10s poll interval makes per-read toggling for
// power savings not worth the added complexity.
//
// atten: an adc_atten_t ordinal (0=DB_0, 1=DB_2_5, 2=DB_6, 3=DB_12), not
// a plain dB value — this board's default (DB_12) assumes the T-Deck's
// low-impedance divider; a high-impedance divider (confirmed: Heltec V3,
// per Meshtastic's own variant.h: "lower dB for high resistance voltage
// divider") reads near-zero raw counts at DB_12 and needs DB_2_5 instead.
void adc_battery_configure(int channel, int ctrl_pin, float multiplier, int atten)
{
    s_channel    = (adc_channel_t)channel;
    s_multiplier = multiplier;
    s_ctrl_pin   = ctrl_pin;
    s_atten      = (adc_atten_t)atten;

    if (s_ctrl_pin >= 0) {
        gpio_config_t in_cfg = {
            .pin_bit_mask = 1ULL << s_ctrl_pin,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&in_cfg);
        int floating_level = gpio_get_level(s_ctrl_pin);
        int enable_level = floating_level ? 0 : 1;

        gpio_config_t out_cfg = {
            .pin_bit_mask = 1ULL << s_ctrl_pin,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&out_cfg);
        gpio_set_level(s_ctrl_pin, enable_level);
    }
}

// Standard 1S LiPo open-circuit-voltage discharge curve — there's no fuel
// gauge IC here to do real coulomb counting, so this is an approximation
// (the same kind of piecewise table most no-fuel-gauge ESP32 battery
// projects use), not a precise reading. Good enough for a status-bar icon.
typedef struct { float mv; uint8_t pct; } curve_point_t;
static const curve_point_t s_curve[] = {
    { 4200, 100 }, { 4060, 90 }, { 3980, 80 }, { 3920, 70 },
    { 3870, 60 },  { 3820, 50 }, { 3790, 40 }, { 3770, 30 },
    { 3740, 20 },  { 3680, 10 }, { 3450, 0 },
};
#define CURVE_N (sizeof(s_curve) / sizeof(s_curve[0]))

static uint8_t voltage_to_percent(float mv)
{
    if (mv >= s_curve[0].mv) return 100;
    if (mv <= s_curve[CURVE_N - 1].mv) return 0;
    for (size_t i = 0; i < CURVE_N - 1; i++) {
        float hi = s_curve[i].mv, lo = s_curve[i + 1].mv;
        if (mv <= hi && mv >= lo) {
            float frac = (mv - lo) / (hi - lo);
            return (uint8_t)(s_curve[i + 1].pct + frac * (s_curve[i].pct - s_curve[i + 1].pct));
        }
    }
    return 0;
}

// A single adc_oneshot_read() is noisy enough on its own that, run through
// s_multiplier (5x+ on Heltec V3), it produces a real, measurable bias —
// confirmed against an identical board+battery reading ~0.10V lower.
// Meshtastic's own espAdcRead() (Power.cpp) never trusts one raw sample
// either: it averages BATTERY_SENSE_SAMPLES (15) raw reads before handing
// the result to calibration. Mirrored here for the same reason.
#define BATTERY_SENSE_SAMPLES 15

static void battery_poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t raw_sum = 0;
        int raw_count = 0;
        for (int i = 0; i < BATTERY_SENSE_SAMPLES; i++) {
            int val = 0;
            if (adc_oneshot_read(s_adc, s_channel, &val) == ESP_OK) {
                raw_sum += (uint32_t)val;
                raw_count++;
            }
        }
        if (raw_count > 0) {
            int raw = (int)(raw_sum / (uint32_t)raw_count);
            int pin_mv = raw;
            if (s_cali_ok) adc_cali_raw_to_voltage(s_cali, raw, &pin_mv);
            int batt_mv = (int)((float)pin_mv * s_multiplier);
            purr_kernel_set_battery_voltage_mv(batt_mv);
            purr_kernel_set_battery_percent(voltage_to_percent((float)batt_mv));
        } else {
            ESP_LOGW(TAG, "adc_oneshot_read failed (all samples)");
        }
        vTaskDelay(pdMS_TO_TICKS(BATTERY_POLL_MS));
    }
}

static int module_init(void)
{
    // Checked BEFORE anything below reads s_channel/s_atten (both set by
    // adc_battery_configure(), called here if a matching quirk block is
    // loaded) — same precedence purrstrap's own generated
    // adc_battery_configure() call already has today (also runs before
    // this init(), via purr_device_init()), so a loaded .purr quirk
    // package is simply the more specific, more current override: this
    // runs LAST, whichever mechanism configured the device before it.
    // Self-contained — no boot.c or generated-glue change needed for a
    // device to pick this up; every device with no quirk package for
    // this driver behaves exactly as before.
    size_t qsz = 0;
    const void *qblk = purr_quirk_get_block("adc_battery.config", &qsz);
    if (qblk && qsz == sizeof(adc_battery_quirk_t)) {
        const adc_battery_quirk_t *q = (const adc_battery_quirk_t *)qblk;
        ESP_LOGI(TAG, "applying loaded quirk package: channel=%d ctrl_pin=%d multiplier=%.3f atten=%d",
                 (int)q->channel, (int)q->ctrl_pin, q->multiplier, (int)q->atten);
        adc_battery_configure(q->channel, q->ctrl_pin, q->multiplier, q->atten);
    } else if (qblk) {
        ESP_LOGW(TAG, "quirk block 'adc_battery.config' has wrong size (%u, expected %u) — ignoring",
                 (unsigned)qsz, (unsigned)sizeof(adc_battery_quirk_t));
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed");
        return -1;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = s_atten,
    };
    if (adc_oneshot_config_channel(s_adc, s_channel, &chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed");
        return -1;
    }

    // Curve fitting (ESP32-S2/S3/C3) vs. line fitting (original ESP32) are
    // mutually exclusive per chip — confirmed live: building unconditionally
    // against curve fitting fails outright on plain-ESP32 boards (CYD/
    // CYD_s024c/CYD_s028r) with "unknown type name
    // 'adc_cali_curve_fitting_config_t'", since that scheme doesn't exist
    // there at all. ADC_CALI_SCHEME_*_SUPPORTED (adc_cali_scheme.h) is
    // ESP-IDF's own portable way to pick whichever this target actually has.
    // Calibration can fail on unfused/older-rev silicon either way — not
    // fatal, just falls back to raw-code voltage (less accurate, still
    // usable for a status icon rather than a precision measurement).
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .chan     = s_channel,
        .atten    = s_atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_ok = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .atten    = s_atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
#if CONFIG_IDF_TARGET_ESP32
        .default_vref = 1100,   // only used as a fallback if eFuse cal data is absent
#endif
    };
    s_cali_ok = adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali) == ESP_OK;
#else
    s_cali_ok = false;
#endif
    if (!s_cali_ok) ESP_LOGW(TAG, "ADC calibration unavailable — using raw counts");

    xTaskCreate(battery_poll_task, "adc_battery", 3072, NULL, 2, &s_task);
    ESP_LOGI(TAG, "battery ADC ready (ch=%d ctrl_pin=%d, x%.3f, cali=%d)",
             (int)s_channel, s_ctrl_pin, (double)s_multiplier, s_cali_ok);
    return 0;
}

static void module_deinit(void)
{
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
    if (s_cali_ok) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_delete_scheme_curve_fitting(s_cali);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        adc_cali_delete_scheme_line_fitting(s_cali);
#endif
        s_cali_ok = false;
    }
    if (s_adc) { adc_oneshot_del_unit(s_adc); s_adc = NULL; }
}

PURR_MODULE_REGISTER(adc_battery) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .load_priority     = PURR_PRIORITY_OPTIONAL,
    .name              = "adc_battery",
    .version           = "1.0.1",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = module_init,
    .deinit            = module_deinit,
};
