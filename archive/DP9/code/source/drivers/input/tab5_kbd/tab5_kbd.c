// tab5_kbd.c — M5Stack Tab5 attachable keyboard driver for PURR OS
// Catcall-compatible, pure C on driver/i2c_master.h (bbq20 pattern).
//
// The Tab5 keyboard is an I2C peripheral (its own MCU) at 0x6D on a dedicated
// bus: SDA=GPIO0, SCL=GPIO1, INT=GPIO50. Protocol reimplemented in C from
// M5Stack's official m5_tab5_keyboard_component (MIT) + the Tab5 keyboard I2C
// protocol PDF — the C++ component and its i2c_bus dependency aren't worth
// dragging into a kernel driver for the handful of registers we use.
//
// POLLING ONLY — never wire the GPIO50 interrupt. The INT-pin ISR path stops
// delivering events once esp-hosted/WiFi is running (P4 GPIO-ISR coexistence
// issue, confirmed live in the donor Tab5 SSH-terminal project). The device
// latches INT_STA until it's cleared by writing 0, so polling loses nothing.
//
// String mode: the keyboard MCU decodes its own matrix and sends either a
// printable string or a special-key word ("enter", "backspace", "up", ...).
// PURR's input pipeline is plain ASCII key-down events masked to one byte
// (see miniwin_keyboard.c), so words map to control bytes; keys with no
// single-byte representation (arrows, F-keys, home/end) are dropped for now —
// nothing in MiniWin consumes them yet.

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

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

static const char *TAG = "drv:tab5_kbd";

// ── Fixed Tab5 wiring / protocol ──────────────────────────────────────────────
#define KB_I2C_PORT       I2C_NUM_1     // dedicated bus — GPIO0 is kbd SDA, so
#define KB_SDA_PIN        0             // it can never double as a boot button
#define KB_SCL_PIN        1
#define KB_I2C_ADDR       0x6D
#define KB_I2C_FREQ_HZ    400000        // official default is 100k; 400k proven

#define KB_REG_INT_CFG    0x00          // per-mode interrupt enable bits
#define KB_REG_INT_STA    0x01          // latched; WRITE 0 TO CLEAR
#define KB_REG_EVENT_NUM  0x02          // queue depth; write 0 clears queue
#define KB_REG_BRIGHTNESS 0x03          // keyboard RGB backlight (0–255)
#define KB_REG_MODE       0x10          // 0=Normal 1=HID 2=String 3=BLE
#define KB_REG_CHAR_LEN   0x40          // String mode: pending event length
#define KB_REG_CHAR_BASE  0x50          // String mode: modifier + chars
#define KB_REG_VERSION    0xFE

#define KB_MODE_STRING    2
#define KB_INT_STA_STRING 0x04          // String-mode event bit in INT_STA

// str_modifier bits (USB-HID-shaped, left|right per nibble)
#define KB_MOD_CTRL       (0x01 | 0x10)
#define KB_MOD_ALT        (0x04 | 0x40)

#define KB_POLL_MS        20
#define EVENT_QUEUE_DEPTH 32

// ── State ─────────────────────────────────────────────────────────────────────

static i2c_master_bus_handle_t s_bus   = NULL;
static i2c_master_dev_handle_t s_dev   = NULL;
static QueueHandle_t           s_queue = NULL;
static TaskHandle_t            s_task  = NULL;
static bool                    s_initialized = false;

// ── Register access ───────────────────────────────────────────────────────────

static esp_err_t kb_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, pdMS_TO_TICKS(50));
}

static esp_err_t kb_read_regs(uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, n, pdMS_TO_TICKS(50));
}

// ── Event mapping ─────────────────────────────────────────────────────────────

static void push_key(uint8_t byte, uint8_t modifiers)
{
    input_event_t ev = {
        .type      = INPUT_EVENT_KEY_DOWN,
        .keycode   = byte,
        .delta_x   = 0,
        .delta_y   = 0,
        .modifiers = modifiers,
    };
    xQueueSend(s_queue, &ev, 0);
}

// Special keys arrive as literal words (same list the donor SSH terminal
// handles). Map to the single control byte PURR's ASCII key pipeline can
// carry; return false for keys with no byte representation.
static bool push_special_word(const char *w, uint8_t mods)
{
    if (!strcmp(w, "enter"))     { push_key('\r',   mods); return true; }
    if (!strcmp(w, "backspace")) { push_key(0x08,   mods); return true; }
    if (!strcmp(w, "tab"))       { push_key('\t',   mods); return true; }
    if (!strcmp(w, "esc"))       { push_key(0x1B,   mods); return true; }
    if (!strcmp(w, "del"))       { push_key(0x7F,   mods); return true; }
    if (!strcmp(w, "space"))     { push_key(' ',    mods); return true; }
    return false;   // arrows / f-keys / home/end — no consumer yet
}

static void handle_string_event(uint8_t modifier, const char *str, uint8_t len)
{
    // The firmware's CHAR_EVENT_LEN counts the trailing NUL: a single "a" is
    // reported as len=2, "enter" as len=6 (confirmed live on fw v1) — so the
    // wire length is useless for the one-char-vs-word distinction; measure
    // the string itself.
    (void)len;
    size_t n = strlen(str);

    uint8_t mods = 0;
    if (modifier & KB_MOD_CTRL) mods |= 0x01;
    if (modifier & KB_MOD_ALT)  mods |= 0x02;

    if (n == 1) {
        uint8_t c = (uint8_t)str[0];
        if ((modifier & KB_MOD_CTRL) && c >= 0x40) {
            push_key(c & 0x1F, mods);   // Ctrl+A..Z → control byte
        } else {
            push_key(c, mods);
        }
        return;
    }
    if (!push_special_word(str, mods)) {
        ESP_LOGD(TAG, "unmapped key word \"%s\" (mod 0x%02X)", str, modifier);
    }
}

// ── Poll task ─────────────────────────────────────────────────────────────────

static void kb_poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(KB_POLL_MS));

        uint8_t status = 0;
        if (kb_read_regs(KB_REG_INT_STA, &status, 1) != ESP_OK) continue;
        if (!(status & KB_INT_STA_STRING)) continue;

        uint8_t count = 0;
        if (kb_read_regs(KB_REG_EVENT_NUM, &count, 1) == ESP_OK) {
            if (count > 32) count = 32;   // safety cap, mirrors vendor driver
            while (count-- > 0) {
                uint8_t len = 0;
                if (kb_read_regs(KB_REG_CHAR_LEN, &len, 1) != ESP_OK) break;
                if (len == 0 || len > 15) break;

                uint8_t buf[17];
                if (kb_read_regs(KB_REG_CHAR_BASE, buf, (size_t)len + 1) != ESP_OK) break;
                char str[16];
                memcpy(str, &buf[1], len);
                str[len] = '\0';
                handle_string_event(buf[0], str, len);
            }
        }

        // INT_STA is latched — the device holds it (and the INT line) asserted
        // until a 0 write releases it.
        kb_write_reg(KB_REG_INT_STA, 0);
    }
}

// ── Catcall interface ─────────────────────────────────────────────────────────

static esp_err_t tab5_kbd_init(void)
{
    if (s_initialized) return ESP_OK;

    s_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = KB_I2C_PORT,
        .sda_io_num        = KB_SDA_PIN,
        .scl_io_num        = KB_SCL_PIN,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = KB_I2C_ADDR,
        .scl_speed_hz    = KB_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev),
                        TAG, "add device");

    // Probe — keyboard is detachable; absent keyboard = clean non-fatal fail
    // (priority IMPORTANT: kernel logs a warning and boots on with touch).
    uint8_t version = 0;
    esp_err_t err = kb_read_regs(KB_REG_VERSION, &version, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "keyboard not detected (0x%02X @ I2C%d): %s",
                 KB_I2C_ADDR, KB_I2C_PORT, esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);   s_dev = NULL;
        i2c_del_master_bus(s_bus);         s_bus = NULL;
        vQueueDelete(s_queue);             s_queue = NULL;
        return err;
    }

    // String mode, drained queue, released interrupt latch (vendor init order).
    kb_write_reg(KB_REG_MODE, KB_MODE_STRING);
    kb_write_reg(KB_REG_EVENT_NUM, 0);
    kb_write_reg(KB_REG_INT_STA, 0);

    if (xTaskCreate(kb_poll_task, "tab5_kbd", 3072, NULL, 3, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "keyboard ready (fw v%u, SDA=%d SCL=%d addr=0x%02X, polling)",
             version, KB_SDA_PIN, KB_SCL_PIN, KB_I2C_ADDR);
    return ESP_OK;
}

static bool tab5_kbd_poll_event(input_event_t *out)
{
    if (!s_queue || !out) return false;
    return xQueueReceive(s_queue, out, 0) == pdTRUE;
}

// Real, working backlight (unlike bbq20's): the keyboard MCU exposes RGB
// brightness at reg 0x03. Non-NULL also signals "physical keyboard present"
// to UI code that gates the on-screen keyboard off (see bbq20.c's comment).
static esp_err_t tab5_kbd_set_backlight(uint8_t brightness)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return kb_write_reg(KB_REG_BRIGHTNESS, brightness);
}

static esp_err_t tab5_kbd_deinit(void)
{
    if (!s_initialized) return ESP_OK;
    if (s_task)  { vTaskDelete(s_task);   s_task = NULL; }
    if (s_queue) { vQueueDelete(s_queue); s_queue = NULL; }
    if (s_dev)   { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
    if (s_bus)   { i2c_del_master_bus(s_bus); s_bus = NULL; }
    s_initialized = false;
    return ESP_OK;
}

// ── Catcall descriptor ────────────────────────────────────────────────────────

static const catcall_input_t s_catcall = {
    .name            = "tab5_kbd",
    .catcall_version = CATCALL_INPUT_VERSION,
    .init            = tab5_kbd_init,
    .poll_event      = tab5_kbd_poll_event,
    .deinit          = tab5_kbd_deinit,
    .set_backlight   = tab5_kbd_set_backlight,
};

// ── Module lifecycle ──────────────────────────────────────────────────────────

static int tab5_kbd_drv_init(void)
{
    if (tab5_kbd_init() != ESP_OK) return -1;   // IMPORTANT priority: warn + boot on
    purr_kernel_register_input(&s_catcall);
    return 0;
}

static void tab5_kbd_drv_deinit(void)
{
    tab5_kbd_deinit();
}

PURR_MODULE_REGISTER(tab5_kbd) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "tab5_kbd",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_INPUT,
    .required_catcalls = 0,
    .init              = tab5_kbd_drv_init,
    .deinit            = tab5_kbd_drv_deinit,
};
