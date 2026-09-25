// bbq20.c — LilyGO T-Deck / T-Deck Plus keyboard driver (RP2040 I2C bridge)
//
// The T-Deck keyboard is a full QWERTY matrix scanned by an embedded RP2040
// and exposed over I2C at address 0x55. Reading one byte from the device
// returns the ASCII character of the most recently pressed key, or 0x00 when
// no key is pending.
//
// Default pins (shared I2C bus with GT911 touch):
//   SDA = 18   SCL = 8
//
// The RP2040 uses a FIFO and holds each key until the host reads it, so
// polling at ~20 ms intervals is sufficient for human typing speeds.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"

#include "../../../kernel/core/purr_module.h"
#include "../../../kernel/core/purr_kernel.h"
#include "../../../kernel/catcalls/catcall_input.h"

static const char *TAG = "bbq20";

// ── Pin defaults ──────────────────────────────────────────────────────────────

#ifndef BBQ20_SDA_PIN
#define BBQ20_SDA_PIN  18
#endif
#ifndef BBQ20_SCL_PIN
#define BBQ20_SCL_PIN   8
#endif

#define BBQ20_I2C_ADDR    0x55
#define BBQ20_I2C_FREQ_HZ 400000
#define BBQ20_I2C_PORT    0       // shared with GT911

// Keyboard backlight is NOT software-controllable, at all, from the main
// ESP32-S3 — confirmed two ways: (1) Meshtastic's own T-Deck driver
// (kbI2cBase.cpp) hardcodes Q10keyboard.setBacklight(0) at init and never
// calls it again for this keyboard class, and its variant.h's `KB_BL_PIN
// 46` is commented "not used for now"; (2) LilyGo's own engineer, on
// github.com/Xinyuan-LilyGO/T-Deck issue #49, confirmed the boot-time
// backlight flash is caused by IO strapping selection and is unavoidable,
// and that there is currently no runtime control exposed — it lives
// entirely in the keyboard's own secondary MCU firmware. Both the I2C
// REG_BKL register (ACKs, does nothing) and driving GPIO46 via PWM from
// this side (no visible effect either) were tried and ruled out live.
#define BBQ20_REG_BKL     0x05

#define BBQ20_POLL_MS  20
#define EVENT_QUEUE_DEPTH 32

// ── State ─────────────────────────────────────────────────────────────────────

static i2c_master_bus_handle_t s_bus  = NULL;
static i2c_master_dev_handle_t s_dev  = NULL;
static QueueHandle_t           s_queue = NULL;
static TaskHandle_t            s_task  = NULL;
static bool                    s_owns_bus = false;  // true if we created the bus
static bool                    s_initialized = false;

// ── Poll task ─────────────────────────────────────────────────────────────────

static void bbq20_poll_task(void *arg)
{
    uint8_t key = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BBQ20_POLL_MS));

        esp_err_t ret = i2c_master_receive(s_dev, &key, 1, pdMS_TO_TICKS(10));
        if (ret != ESP_OK || key == 0x00) continue;

        input_event_t ev = {
            .type      = INPUT_EVENT_KEY_DOWN,
            .keycode   = (uint16_t)key,
            .delta_x   = 0,
            .delta_y   = 0,
            .modifiers = 0,
        };
        xQueueSend(s_queue, &ev, 0);
    }
}

// ── Catcall: init ─────────────────────────────────────────────────────────────

static esp_err_t bbq20_init(void)
{
    if (s_initialized) return ESP_OK;

    s_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "queue alloc failed");
        return ESP_ERR_NO_MEM;
    }

    // Attempt to add to existing I2C bus (GT911 may have already created it)
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = BBQ20_I2C_PORT,
        .sda_io_num        = BBQ20_SDA_PIN,
        .scl_io_num        = BBQ20_SCL_PIN,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    // Try to create a new bus — if port already exists, use the handle returned
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (ret == ESP_OK) {
        s_owns_bus = true;
    } else {
        // Port in use (GT911 already owns it). Acquire via port handle.
        ret = i2c_master_get_bus_handle(BBQ20_I2C_PORT, &s_bus);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "can't get I2C bus handle: %s", esp_err_to_name(ret));
            return ret;
        }
        s_owns_bus = false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BBQ20_I2C_ADDR,
        .scl_speed_hz    = BBQ20_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev),
                        TAG, "add keyboard device failed");

    xTaskCreate(bbq20_poll_task, "bbq20", 2048, NULL, 3, &s_task);
    s_initialized = true;
    ESP_LOGI(TAG, "keyboard ready (SDA=%d, SCL=%d, addr=0x%02X)",
             BBQ20_SDA_PIN, BBQ20_SCL_PIN, BBQ20_I2C_ADDR);
    return ESP_OK;
}

// ── Catcall: poll_event ───────────────────────────────────────────────────────

static bool bbq20_poll_event(input_event_t *out)
{
    if (!s_queue || !out) return false;
    return xQueueReceive(s_queue, out, 0) == pdTRUE;
}

// ── Backlight ─────────────────────────────────────────────────────────────────
// Deliberately a no-op — see the REG_BKL comment up top for why this can't
// actually be implemented. Kept as a real (non-NULL) catcall entry only
// because cupcake_win.c's ck_has_physical_keyboard() uses "does this input
// driver implement set_backlight" as its signal to gate the on-screen
// keyboard off — removing the field entirely would silently break that.

esp_err_t bbq20_set_backlight(uint8_t brightness)
{
    (void)brightness;
    return ESP_ERR_NOT_SUPPORTED;
}

// ── Catcall: deinit ───────────────────────────────────────────────────────────

static esp_err_t bbq20_deinit(void)
{
    if (!s_initialized) return ESP_OK;
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
    if (s_queue) { vQueueDelete(s_queue); s_queue = NULL; }
    if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
    if (s_bus && s_owns_bus) { i2c_del_master_bus(s_bus); s_bus = NULL; }
    s_initialized = false;
    return ESP_OK;
}

// ── Catcall descriptor ────────────────────────────────────────────────────────

static const catcall_input_t s_catcall = {
    .name            = "bbq20",
    .catcall_version = CATCALL_INPUT_VERSION,
    .init            = bbq20_init,
    .poll_event      = bbq20_poll_event,
    .deinit          = bbq20_deinit,
    .set_backlight   = bbq20_set_backlight,
};

// ── Module lifecycle ──────────────────────────────────────────────────────────

int bbq20_drv_init(void)
{
    esp_err_t ret = bbq20_init();
    if (ret != ESP_OK) return -1;
    purr_kernel_register_input(&s_catcall);
    return 0;
}

static int module_init(void) { return bbq20_drv_init(); }

static void module_deinit(void) { bbq20_deinit(); }

// ── Module header ─────────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(bbq20) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "bbq20",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_INPUT,
    .required_catcalls = 0,
    .init              = module_init,
    .deinit            = module_deinit,
};
