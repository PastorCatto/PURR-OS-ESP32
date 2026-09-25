#!/usr/bin/env python3
"""
driverstrap — PURR OS driver template generator

Generates a complete, ready-to-modify driver skeleton for any supported
catcall type. Output goes to user_drivers/<type>/<name>/ (or source/drivers/
if --core is passed).

Usage:
  driverstrap new                          interactive wizard
  driverstrap new <type> <name>            non-interactive
  driverstrap new <type> <name> [options]
  driverstrap list-types                   show all supported driver types
  driverstrap list-interfaces <type>       show interfaces for a type

Driver types:    display  touch  input  radio  gps  wifi
Interfaces:
  display:  spi  i2c  qspi  parallel
  touch:    i2c  spi
  input:    gpio  i2c  spi
  radio:    spi  uart
  gps:      uart  i2c
  wifi:     builtin

Options:
  --name NAME            driver name (slug, e.g. "my_oled")
  --type TYPE            driver type (display|touch|input|radio|gps|wifi)
  --interface IFACE      hardware interface
  --chip CHIP            esp32|esp32s3|both  (default: both)
  --output DIR           override output directory
  --core                 write to source/drivers/ instead of user_drivers/
  --no-header            skip generating the .h public header
"""

import argparse
import os
import re
import sys
import textwrap

os.system("")  # enable ANSI on Windows

C_RST  = "\033[0m"
C_BOLD = "\033[1m"
C_GRY  = "\033[90m"
C_RED  = "\033[91m"
C_GRN  = "\033[92m"
C_YLW  = "\033[93m"
C_CYN  = "\033[96m"
C_WHT  = "\033[97m"

PURROS_VERSION = "0.13.0"
KITT_VERSION   = "0.9.2"

REPO_DIR     = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
USER_DRV_DIR = os.path.join(REPO_DIR, "user_drivers")
SRC_DRV_DIR  = os.path.join(REPO_DIR, "source", "drivers")

def ok(msg):   print(f"{C_GRN}[driverstrap]{C_RST}   {C_GRN}OK{C_RST}  {msg}")
def info(msg): print(f"{C_GRN}[driverstrap]{C_RST} {msg}")
def warn(msg): print(f"{C_YLW}[warn]       {C_RST} {msg}")
def die(msg, code=1):
    print(f"{C_RED}[err]        {C_RST} {msg}", file=sys.stderr)
    sys.exit(code)
def div(label=""):
    line = f"─ {label} " + "─" * max(0, 50 - len(label) - 2) if label else "─" * 50
    print(f"{C_GRY}{line}{C_RST}")

# ── Type / interface metadata ─────────────────────────────────────────────────

DRIVER_TYPES = {
    "display": {
        "catcall":    "catcall_display_t",
        "flag":       "CATCALL_FLAG_DISPLAY",
        "version":    "CATCALL_DISPLAY_VERSION",
        "provides":   "catcall_display",
        "interfaces": ["spi", "i2c", "qspi", "parallel"],
        "default_if": "spi",
        "desc":       "Pixel display (push_pixels, fill_rect, brightness)",
    },
    "touch": {
        "catcall":    "catcall_touch_t",
        "flag":       "CATCALL_FLAG_TOUCH",
        "version":    "CATCALL_TOUCH_VERSION",
        "provides":   "catcall_touch",
        "interfaces": ["i2c", "spi"],
        "default_if": "i2c",
        "desc":       "Touch controller (read_point, is_pressed)",
    },
    "input": {
        "catcall":    "catcall_input_t",
        "flag":       "CATCALL_FLAG_INPUT",
        "version":    "CATCALL_INPUT_VERSION",
        "provides":   "catcall_input",
        "interfaces": ["gpio", "i2c", "spi"],
        "default_if": "gpio",
        "desc":       "HID input — keyboard, trackball, buttons (poll_event)",
    },
    "radio": {
        "catcall":    "catcall_radio_t",
        "flag":       "CATCALL_FLAG_RADIO",
        "version":    "CATCALL_RADIO_VERSION",
        "provides":   "catcall_radio",
        "interfaces": ["spi", "uart"],
        "default_if": "spi",
        "desc":       "LoRa / sub-GHz radio (send, receive, rssi, snr)",
    },
    "gps": {
        "catcall":    "catcall_gps_t",
        "flag":       "CATCALL_FLAG_GPS",
        "version":    "CATCALL_GPS_VERSION",
        "provides":   "catcall_gps",
        "interfaces": ["uart", "i2c"],
        "default_if": "uart",
        "desc":       "NMEA GPS (get_fix — lat, lon, speed, altitude)",
    },
    "wifi": {
        "catcall":    None,
        "flag":       None,
        "version":    None,
        "provides":   None,
        "interfaces": ["builtin"],
        "default_if": "builtin",
        "desc":       "WiFi system module (no catcall — registered as PURR_MOD_SYSTEM)",
    },
}

CHIP_CHOICES = {
    "both":    '["esp32", "esp32s3"]',
    "esp32":   '["esp32"]',
    "esp32s3": '["esp32s3"]',
}

IDF_REQUIRES = {
    "spi":     ["driver", "esp_driver_spi"],
    "i2c":     ["driver", "esp_driver_i2c"],
    "qspi":    ["driver", "esp_lcd"],
    "parallel":["driver", "esp_lcd"],
    "uart":    ["driver", "esp_driver_uart"],
    "gpio":    ["driver"],
    "builtin": ["esp_wifi", "esp_event", "nvs_flash"],
}

# ── Template generators ───────────────────────────────────────────────────────

def _slug(name):
    return re.sub(r"[^a-z0-9_]", "_", name.lower())

def _upper(name):
    return _slug(name).upper()


def gen_pcat(name, drv_type, interface, chip):
    meta = DRIVER_TYPES[drv_type]
    chip_str = CHIP_CHOICES.get(chip, CHIP_CHOICES["both"])
    provides = f'["{meta["provides"]}"]' if meta["provides"] else "[]"
    return f"""\
# driver.pcat — {name}
# Auto-generated by driverstrap v{PURROS_VERSION}
# Edit as needed before building with modulestrap.

name              = "{name}"
version           = "0.1.0"
type              = "{drv_type}"
interface         = "{interface}"
{"catcall_version  = 1" if meta["version"] else "# no catcall — system module"}

idf_min           = "5.3.0"
chip              = {chip_str}

kernel_min        = "0.9.0"
kernel_max        = ""

module_type       = "{"PURR_MOD_DRIVER" if meta["catcall"] else "PURR_MOD_SYSTEM"}"
provides          = {provides}
required_catcalls = []
"""


def gen_cmake(name, drv_type, interface):
    requires = " ".join(IDF_REQUIRES.get(interface, ["driver"]))
    meta = DRIVER_TYPES[drv_type]
    extra_req = ""
    if meta["catcall"]:
        extra_req = " purr_kernel"
    return f"""\
idf_component_register(
    SRCS        "{name}.c"
    INCLUDE_DIRS "."
                 "${{CMAKE_SOURCE_DIR}}/../source/kernel/catcalls"
                 "${{CMAKE_SOURCE_DIR}}/../source/kernel/core"
    REQUIRES    {requires}{extra_req}
)
"""


def gen_header(name, drv_type, interface):
    guard = f"{_upper(name)}_H"
    meta  = DRIVER_TYPES[drv_type]
    iface_note = f" ({interface.upper()})" if interface != "builtin" else ""
    catcall_include = f'#include "{meta["catcall"].replace("_t","")}.h"\n' if meta["catcall"] else ""
    return f"""\
#pragma once
// {name}.h — {meta["desc"]}{iface_note}
// Auto-generated by driverstrap. Modify freely.

#ifndef {guard}
#define {guard}

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
{catcall_include}
// ── Public configuration (set before calling {name}_drv_init) ─────────────────

typedef struct {{
{"    int     spi_host;" if interface == "spi" else ""}
{"    int     cs_pin;" if interface in ("spi",) else ""}
{"    int     dc_pin;" if drv_type == "display" and interface in ("spi", "qspi", "parallel") else ""}
{"    int     rst_pin;       // -1 = not wired" if drv_type in ("display", "touch") else ""}
{"    int     bl_pin;        // -1 = no backlight" if drv_type == "display" else ""}
{"    int     sda_pin;" if interface == "i2c" else ""}
{"    int     scl_pin;" if interface == "i2c" else ""}
{"    int     int_pin;       // -1 = not wired" if drv_type == "touch" else ""}
{"    int     tx_pin;" if interface == "uart" else ""}
{"    int     rx_pin;" if interface == "uart" else ""}
{"    int     gpio_pin;" if interface == "gpio" else ""}
    int     reserved;
}} {name}_config_t;

// Configure the driver. Call before drv_init().
void {name}_configure(const {name}_config_t *cfg);

// Initialize hardware and register the catcall with the kernel.
int  {name}_drv_init(void);

// Tear down and release hardware resources.
int  {name}_drv_deinit(void);

#endif // {guard}
"""


def gen_source_display(name, interface):
    iface_up = interface.upper()
    config_fields = ""
    init_body = ""
    push_body = ""
    fill_body = ""

    if interface == "spi":
        config_fields = """\
static int s_spi_host = SPI2_HOST;
static int s_cs       = -1;
static int s_dc       = -1;
static int s_rst      = -1;
static int s_bl       = -1;"""
        init_body = f"""\
    // TODO: init SPI bus + device
    // spi_bus_config_t bus = {{ .mosi_io_num = MOSI, .sclk_io_num = SCLK, ... }};
    // spi_bus_initialize(s_spi_host, &bus, SPI_DMA_CH_AUTO);
    // spi_device_interface_config_t dev = {{ .clock_speed_hz = 40*1000*1000, .spics_io_num = s_cs, ... }};
    // spi_bus_add_device(s_spi_host, &dev, &s_spi_dev);

    // TODO: hardware reset
    // if (s_rst >= 0) {{ gpio_set_level(s_rst, 0); vTaskDelay(10); gpio_set_level(s_rst, 1); vTaskDelay(50); }}

    // TODO: send init sequence to panel
    // {name}_send_cmd(0x01);  // SWRESET
    // vTaskDelay(150);
    // {name}_send_cmd(0x11);  // SLPOUT
    // ...

    // TODO: enable backlight
    // if (s_bl >= 0) gpio_set_level(s_bl, 1);"""
        push_body = """\
    // TODO: set column/row address window, then send pixel data
    // {name}_set_window(x, y, x + w - 1, y + h - 1);
    // {name}_write_data((const uint8_t *)pixels, w * h * 2);"""
        fill_body = """\
    // TODO: fill region with a single color
    // {name}_set_window(x, y, x + w - 1, y + h - 1);
    // for (int i = 0; i < w * h; i++) {name}_write_data16(color);"""

    elif interface == "i2c":
        config_fields = """\
static int s_sda = -1;
static int s_scl = -1;
static int s_rst = -1;"""
        init_body = """\
    // TODO: init I2C master
    // i2c_master_bus_config_t bus_cfg = { .i2c_port = I2C_NUM_0, .sda_io_num = s_sda, .scl_io_num = s_scl, ... };
    // i2c_new_master_bus(&bus_cfg, &s_bus);
    // i2c_device_config_t dev_cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x3C, ... };
    // i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);

    // TODO: send init commands
    // {name}_cmd(0xAE);  // display off
    // ...
    // {name}_cmd(0xAF);  // display on"""
        push_body = """\
    // TODO: update display region
    // Typically for OLED: convert RGB565 → 1-bit, write to page buffer"""
        fill_body = """\
    // TODO: fill region
    // Set page buffer bytes for the affected rows"""

    elif interface == "qspi":
        config_fields = """\
static int s_cs   = -1;
static int s_pclk = -1;
static int s_d0   = -1, s_d1 = -1, s_d2 = -1, s_d3 = -1;
static int s_rst  = -1;
static int s_bl   = -1;"""
        init_body = """\
    // TODO: init QSPI panel IO via esp_lcd
    // esp_lcd_panel_io_spi_config_t io_cfg = { .cs_gpio_num = s_cs, .pclk_hz = 40*1000*1000, ... };
    // esp_lcd_new_panel_io_spi(s_spi_bus, &io_cfg, &s_io);
    // esp_lcd_panel_dev_config_t panel_cfg = { .reset_gpio_num = s_rst, ... };
    // esp_lcd_new_panel_{name}(s_io, &panel_cfg, &s_panel);
    // esp_lcd_panel_reset(s_panel);
    // esp_lcd_panel_init(s_panel);"""
        push_body = """\
    // TODO: push pixel data via esp_lcd
    // esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, pixels);"""
        fill_body = """\
    // TODO: fill region
    // Allocate a row buffer and call draw_bitmap in strips"""

    return f"""\
// {name}.c — display driver ({iface_up})
// Auto-generated by driverstrap. Fill in the TODO sections.

#include "{name}.h"
#include "catcall_display.h"
#include "purr_kernel.h"
#include "purr_module.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

static const char *TAG = "{name}";

// ── Pin / bus state ───────────────────────────────────────────────────────────
{config_fields}

static bool s_initialized = false;

// ── Config ────────────────────────────────────────────────────────────────────

void {name}_configure(const {name}_config_t *cfg) {{
    if (!cfg) return;
    // TODO: copy cfg fields into static state
    // Example: s_cs = cfg->cs_pin; s_dc = cfg->dc_pin; ...
    (void)cfg;
}}

// ── Hardware init / deinit ────────────────────────────────────────────────────

static esp_err_t hw_init(void) {{
{textwrap.indent(init_body, "    ")}

    s_initialized = true;
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}}

static esp_err_t hw_deinit(void) {{
    s_initialized = false;
    // TODO: release SPI/I2C device and bus handles
    return ESP_OK;
}}

// ── catcall_display_t implementation ──────────────────────────────────────────

static esp_err_t catcall_init(const display_config_t *cfg) {{
    (void)cfg;
    return hw_init();
}}

static esp_err_t catcall_push_pixels(int x, int y, int w, int h, const uint16_t *pixels) {{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
{textwrap.indent(push_body, "    ")}
    (void)x; (void)y; (void)w; (void)h; (void)pixels;
    return ESP_OK;
}}

static esp_err_t catcall_fill_rect(int x, int y, int w, int h, uint16_t color) {{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
{textwrap.indent(fill_body, "    ")}
    (void)x; (void)y; (void)w; (void)h; (void)color;
    return ESP_OK;
}}

static esp_err_t catcall_set_brightness(uint8_t level) {{
    // TODO: PWM or GPIO backlight control
    // ledcWrite(BL_CHANNEL, level); or gpio_set_level(s_bl, level > 0);
    (void)level;
    return ESP_OK;
}}

static void catcall_get_info(display_info_t *out) {{
    if (!out) return;
    // TODO: fill in your panel's real resolution
    out->width          = 320;
    out->height         = 240;
    out->bits_per_pixel = 16;
    out->name           = "{name}";
}}

static esp_err_t catcall_deinit(void) {{
    return hw_deinit();
}}

// ── Catcall struct (never on stack — must be static) ──────────────────────────

static const catcall_display_t s_catcall = {{
    .name            = "{name}",
    .catcall_version = CATCALL_DISPLAY_VERSION,
    .init            = catcall_init,
    .push_pixels     = catcall_push_pixels,
    .fill_rect       = catcall_fill_rect,
    .set_brightness  = catcall_set_brightness,
    .get_info        = catcall_get_info,
    .deinit          = catcall_deinit,
}};

// ── Module entry points ───────────────────────────────────────────────────────

int {name}_drv_init(void) {{
    display_config_t cfg = {{ .landscape = true, .rotation = 0 }};
    if (catcall_init(&cfg) != ESP_OK) {{
        ESP_LOGE(TAG, "init failed");
        return -1;
    }}
    purr_kernel_register_display(&s_catcall);
    ESP_LOGI(TAG, "display catcall registered");
    return 0;
}}

int {name}_drv_deinit(void) {{
    return catcall_deinit() == ESP_OK ? 0 : -1;
}}

// ── .purr module header ───────────────────────────────────────────────────────

purr_module_header_t purr_module = {{
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .name              = "{name}",
    .version           = "0.1.0",
    .kernel_min        = "0.9.0",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_DISPLAY,
    .required_catcalls = 0,
    .init              = {name}_drv_init,
    .deinit            = (void (*)(void)){name}_drv_deinit,
}};
"""


def gen_source_touch(name, interface):
    iface_up = interface.upper()
    if interface == "i2c":
        config_fields = """\
static int  s_sda  = -1;
static int  s_scl  = -1;
static int  s_int  = -1;
static int  s_rst  = -1;
static bool s_initialized = false;"""
        read_body = """\
    // TODO: read touch point register(s) over I2C
    // Example:
    // uint8_t buf[4] = {0};
    // i2c_master_receive(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(50));
    // *x = ((buf[0] & 0x0F) << 8) | buf[1];
    // *y = ((buf[2] & 0x0F) << 8) | buf[3];
    // return true;"""
        pressed_body = """\
    // TODO: check touch status register or INT pin
    // return gpio_get_level(s_int) == 0;  // INT active low"""
    else:  # spi
        config_fields = """\
static int  s_cs   = -1;
static int  s_irq  = -1;
static bool s_initialized = false;"""
        read_body = """\
    // TODO: read touch coordinates via SPI
    // Send X measure command (0xD0), read 2 bytes
    // Send Y measure command (0x90), read 2 bytes
    // Apply calibration offsets from NVS"""
        pressed_body = """\
    // TODO: return IRQ pin state
    // return gpio_get_level(s_irq) == 0;"""

    return f"""\
// {name}.c — touch driver ({iface_up})
// Auto-generated by driverstrap. Fill in the TODO sections.

#include "{name}.h"
#include "catcall_touch.h"
#include "purr_kernel.h"
#include "purr_module.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "{name}";

// ── Pin / bus state ───────────────────────────────────────────────────────────
{config_fields}

// ── Config ────────────────────────────────────────────────────────────────────

void {name}_configure(const {name}_config_t *cfg) {{
    if (!cfg) return;
    // TODO: copy cfg fields into static state
    (void)cfg;
}}

// ── catcall_touch_t implementation ────────────────────────────────────────────

static esp_err_t catcall_init(const touch_config_t *cfg) {{
    (void)cfg;
    // TODO: initialize {iface_up} bus, device handle, INT pin

    s_initialized = true;
    ESP_LOGI(TAG, "touch initialized");
    return ESP_OK;
}}

static bool catcall_is_pressed(void) {{
    if (!s_initialized) return false;
{textwrap.indent(pressed_body, "    ")}
    return false;
}}

static bool catcall_read_point(uint16_t *x, uint16_t *y) {{
    if (!s_initialized || !x || !y) return false;
{textwrap.indent(read_body, "    ")}
    return false;
}}

static esp_err_t catcall_deinit(void) {{
    s_initialized = false;
    // TODO: release bus handles
    return ESP_OK;
}}

static const catcall_touch_t s_catcall = {{
    .name            = "{name}",
    .catcall_version = CATCALL_TOUCH_VERSION,
    .init            = catcall_init,
    .is_pressed      = catcall_is_pressed,
    .read_point      = catcall_read_point,
    .deinit          = catcall_deinit,
}};

// ── Module entry points ───────────────────────────────────────────────────────

int {name}_drv_init(void) {{
    touch_config_t cfg = {{0}};
    if (catcall_init(&cfg) != ESP_OK) {{
        ESP_LOGE(TAG, "init failed");
        return -1;
    }}
    purr_kernel_register_touch(&s_catcall);
    return 0;
}}

int {name}_drv_deinit(void) {{
    return catcall_deinit() == ESP_OK ? 0 : -1;
}}

purr_module_header_t purr_module = {{
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .name              = "{name}",
    .version           = "0.1.0",
    .kernel_min        = "0.9.0",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_TOUCH,
    .required_catcalls = 0,
    .init              = {name}_drv_init,
    .deinit            = (void (*)(void)){name}_drv_deinit,
}};
"""


def gen_source_input(name, interface):
    iface_up = interface.upper()
    queue_setup = """\
static QueueHandle_t s_queue = NULL;"""
    if interface == "gpio":
        poll_body = """\
    // TODO: read GPIO pins and enqueue INPUT_EVENT_KEY_DOWN / INPUT_EVENT_POINTER events
    // Example (button):
    // if (gpio_get_level(s_gpio_pin) == 0) {
    //     input_event_t ev = { .type = INPUT_EVENT_KEY_DOWN, .keycode = 'A' };
    //     xQueueSend(s_queue, &ev, 0);
    // }"""
    elif interface == "i2c":
        poll_body = """\
    // TODO: read key data from device over I2C
    // i2c_master_receive(s_dev, &key, 1, pdMS_TO_TICKS(50));
    // if (key) {
    //     input_event_t ev = { .type = INPUT_EVENT_KEY_DOWN, .keycode = key };
    //     xQueueSend(s_queue, &ev, 0);
    // }"""
    else:
        poll_body = """\
    // TODO: read key data from device over SPI"""

    return f"""\
// {name}.c — input driver ({iface_up})
// Auto-generated by driverstrap. Fill in the TODO sections.

#include "{name}.h"
#include "catcall_input.h"
#include "purr_kernel.h"
#include "purr_module.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "{name}";

{queue_setup}
static bool s_initialized = false;

void {name}_configure(const {name}_config_t *cfg) {{
    (void)cfg;
    // TODO: copy cfg fields
}}

static void poll_task(void *arg) {{
    (void)arg;
    while (1) {{
        vTaskDelay(pdMS_TO_TICKS(20));
{textwrap.indent(poll_body, "        ")}
    }}
}}

static esp_err_t catcall_init(void) {{
    s_queue = xQueueCreate(32, sizeof(input_event_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    // TODO: initialize {iface_up} hardware

    xTaskCreate(poll_task, "{name}_poll", 2048, NULL, 3, NULL);
    s_initialized = true;
    ESP_LOGI(TAG, "input driver initialized");
    return ESP_OK;
}}

static bool catcall_poll_event(input_event_t *out) {{
    if (!s_queue || !out) return false;
    return xQueueReceive(s_queue, out, 0) == pdTRUE;
}}

static esp_err_t catcall_deinit(void) {{
    s_initialized = false;
    if (s_queue) {{ vQueueDelete(s_queue); s_queue = NULL; }}
    return ESP_OK;
}}

static const catcall_input_t s_catcall = {{
    .name            = "{name}",
    .catcall_version = CATCALL_INPUT_VERSION,
    .init            = catcall_init,
    .poll_event      = catcall_poll_event,
    .deinit          = catcall_deinit,
}};

int {name}_drv_init(void) {{
    if (catcall_init() != ESP_OK) {{
        ESP_LOGE(TAG, "init failed");
        return -1;
    }}
    purr_kernel_register_input(&s_catcall);
    return 0;
}}

int {name}_drv_deinit(void) {{
    return catcall_deinit() == ESP_OK ? 0 : -1;
}}

purr_module_header_t purr_module = {{
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .name              = "{name}",
    .version           = "0.1.0",
    .kernel_min        = "0.9.0",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_INPUT,
    .required_catcalls = 0,
    .init              = {name}_drv_init,
    .deinit            = (void (*)(void)){name}_drv_deinit,
}};
"""


def gen_source_radio(name, interface):
    iface_up = interface.upper()
    return f"""\
// {name}.c — radio driver ({iface_up})
// Auto-generated by driverstrap. Fill in the TODO sections.

#include "{name}.h"
#include "catcall_radio.h"
#include "purr_kernel.h"
#include "purr_module.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "{name}";

static bool    s_initialized = false;
static int32_t s_last_rssi   = 0;
static float   s_last_snr    = 0.0f;

void {name}_configure(const {name}_config_t *cfg) {{
    (void)cfg;
    // TODO: copy cfg fields (CS, RST, IRQ, BUSY pins etc.)
}}

static esp_err_t catcall_init(const radio_config_t *cfg) {{
    (void)cfg;
    // TODO: init {iface_up} bus, reset radio, configure modem
    // For LoRa:
    //   {name}_reset();
    //   {name}_set_mode(MODE_SLEEP);
    //   {name}_set_freq(cfg->freq_hz);
    //   {name}_set_bandwidth(cfg->bandwidth);
    //   {name}_set_sf(cfg->spreading_factor);
    //   {name}_set_mode(MODE_STDBY);
    s_initialized = true;
    ESP_LOGI(TAG, "radio initialized");
    return ESP_OK;
}}

static esp_err_t catcall_send(const uint8_t *data, size_t len) {{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    // TODO: transmit packet
    (void)data; (void)len;
    return ESP_OK;
}}

static esp_err_t catcall_receive(uint8_t *buf, size_t *len, uint32_t timeout_ms) {{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    // TODO: wait for packet (poll IRQ or use interrupt)
    // Set *len to actual received byte count
    (void)buf; (void)len; (void)timeout_ms;
    return ESP_ERR_TIMEOUT;
}}

static int32_t catcall_rssi(void) {{
    return s_last_rssi;
}}

static float catcall_snr(void) {{
    return s_last_snr;
}}

static esp_err_t catcall_set_freq(uint32_t freq_hz) {{
    // TODO: set carrier frequency
    (void)freq_hz;
    return ESP_OK;
}}

static esp_err_t catcall_set_power(int8_t dbm) {{
    // TODO: set TX power
    (void)dbm;
    return ESP_OK;
}}

static esp_err_t catcall_deinit(void) {{
    s_initialized = false;
    return ESP_OK;
}}

static const catcall_radio_t s_catcall = {{
    .name            = "{name}",
    .catcall_version = CATCALL_RADIO_VERSION,
    .init            = catcall_init,
    .send            = catcall_send,
    .receive         = catcall_receive,
    .rssi            = catcall_rssi,
    .snr             = catcall_snr,
    .set_freq        = catcall_set_freq,
    .set_power       = catcall_set_power,
    .deinit          = catcall_deinit,
}};

int {name}_drv_init(void) {{
    radio_config_t cfg = {{ .freq_hz = 915000000, .spreading_factor = 7, .bandwidth = 125000 }};
    if (catcall_init(&cfg) != ESP_OK) {{
        ESP_LOGE(TAG, "init failed");
        return -1;
    }}
    purr_kernel_register_radio(&s_catcall);
    return 0;
}}

int {name}_drv_deinit(void) {{
    return catcall_deinit() == ESP_OK ? 0 : -1;
}}

purr_module_header_t purr_module = {{
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .name              = "{name}",
    .version           = "0.1.0",
    .kernel_min        = "0.9.0",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_RADIO,
    .required_catcalls = 0,
    .init              = {name}_drv_init,
    .deinit            = (void (*)(void)){name}_drv_deinit,
}};
"""


def gen_source_gps(name, interface):
    iface_up = interface.upper()
    parse_body = """\
    // TODO: parse NMEA sentences
    // GGA gives lat, lon, fix quality, altitude
    // RMC gives lat, lon, speed, date
    // Minimally parse $GPGGA:
    // if (strncmp(line, "$GPGGA", 6) == 0) { ... }"""

    return f"""\
// {name}.c — GPS driver ({iface_up} NMEA)
// Auto-generated by driverstrap. Fill in the TODO sections.

#include "{name}.h"
#include "catcall_gps.h"
#include "purr_kernel.h"
#include "purr_module.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include <string.h>

static const char *TAG = "{name}";

static bool      s_initialized = false;
static gps_fix_t s_fix         = {{0}};

void {name}_configure(const {name}_config_t *cfg) {{
    (void)cfg;
    // TODO: copy tx_pin, rx_pin, baud rate
}}

static void parse_nmea(const char *line) {{
{textwrap.indent(parse_body, "    ")}
    (void)line;
}}

static void gps_task(void *arg) {{
    (void)arg;
    char  buf[256];
    int   pos = 0;
    uint8_t ch;

    while (1) {{
        // TODO: replace UART_NUM_1 with your configured UART port
        int len = uart_read_bytes(UART_NUM_1, &ch, 1, pdMS_TO_TICKS(100));
        if (len > 0) {{
            if (ch == '\\n') {{
                buf[pos] = '\\0';
                parse_nmea(buf);
                pos = 0;
            }} else if (pos < (int)sizeof(buf) - 1) {{
                buf[pos++] = (char)ch;
            }}
        }}
    }}
}}

static esp_err_t catcall_init(const gps_config_t *cfg) {{
    (void)cfg;
    // TODO: configure and install UART driver
    // uart_config_t uart_cfg = {{ .baud_rate = 9600, .data_bits = UART_DATA_8_BITS, ... }};
    // uart_param_config(UART_NUM_1, &uart_cfg);
    // uart_set_pin(UART_NUM_1, TX_PIN, RX_PIN, -1, -1);
    // uart_driver_install(UART_NUM_1, 256, 0, 0, NULL, 0);

    xTaskCreate(gps_task, "{name}_gps", 4096, NULL, 2, NULL);
    s_initialized = true;
    ESP_LOGI(TAG, "GPS driver initialized");
    return ESP_OK;
}}

static esp_err_t catcall_get_fix(gps_fix_t *out) {{
    if (!s_initialized || !out) return ESP_ERR_INVALID_STATE;
    *out = s_fix;
    return s_fix.valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}}

static esp_err_t catcall_deinit(void) {{
    s_initialized = false;
    uart_driver_delete(UART_NUM_1);
    return ESP_OK;
}}

static const catcall_gps_t s_catcall = {{
    .name            = "{name}",
    .catcall_version = CATCALL_GPS_VERSION,
    .init            = catcall_init,
    .get_fix         = catcall_get_fix,
    .deinit          = catcall_deinit,
}};

int {name}_drv_init(void) {{
    gps_config_t cfg = {{0}};
    if (catcall_init(&cfg) != ESP_OK) {{
        ESP_LOGE(TAG, "init failed");
        return -1;
    }}
    purr_kernel_register_gps(&s_catcall);
    return 0;
}}

int {name}_drv_deinit(void) {{
    return catcall_deinit() == ESP_OK ? 0 : -1;
}}

purr_module_header_t purr_module = {{
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .name              = "{name}",
    .version           = "0.1.0",
    .kernel_min        = "0.9.0",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_GPS,
    .required_catcalls = 0,
    .init              = {name}_drv_init,
    .deinit            = (void (*)(void)){name}_drv_deinit,
}};
"""


def gen_source_wifi(name):
    return f"""\
// {name}.c — WiFi system module
// Auto-generated by driverstrap. Fill in the TODO sections.
//
// This is a PURR_MOD_SYSTEM module — it does not register a catcall.
// It initializes the ESP32 WiFi stack and handles connection events.

#include "{name}.h"
#include "purr_kernel.h"
#include "purr_module.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "{name}";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          5

static EventGroupHandle_t s_wifi_events = NULL;
static int                s_retry_count = 0;
static bool               s_initialized = false;

// TODO: set your SSID and password (or load from NVS)
#define WIFI_SSID     "your_ssid"
#define WIFI_PASSWORD "your_password"

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data) {{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {{
        esp_wifi_connect();
    }} else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {{
        if (s_retry_count < MAX_RETRY) {{
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "retry %d/%d", s_retry_count, MAX_RETRY);
        }} else {{
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
            ESP_LOGW(TAG, "connection failed");
        }}
    }} else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {{
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }}
}}

static int module_init(void) {{
    s_wifi_events = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_cfg = {{
        .sta = {{
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
        }},
    }};
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();

    // Wait for connection or failure
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(10000));

    if (bits & WIFI_CONNECTED_BIT) {{
        ESP_LOGI(TAG, "WiFi connected");
        s_initialized = true;
        return 0;
    }}

    ESP_LOGW(TAG, "WiFi not connected — continuing without network");
    return 0;  // non-fatal: OS boots even without WiFi
}}

static void module_deinit(void) {{
    esp_wifi_stop();
    esp_wifi_deinit();
    s_initialized = false;
}}

purr_module_header_t purr_module = {{
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_SYSTEM,
    .name              = "{name}",
    .version           = "0.1.0",
    .kernel_min        = "0.9.0",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = 0,
    .init              = module_init,
    .deinit            = module_deinit,
}};
"""


def gen_source(name, drv_type, interface):
    if drv_type == "display": return gen_source_display(name, interface)
    if drv_type == "touch":   return gen_source_touch(name, interface)
    if drv_type == "input":   return gen_source_input(name, interface)
    if drv_type == "radio":   return gen_source_radio(name, interface)
    if drv_type == "gps":     return gen_source_gps(name, interface)
    if drv_type == "wifi":    return gen_source_wifi(name)
    die(f"unknown type: {drv_type}")


# ── File writer ───────────────────────────────────────────────────────────────

def write_file(path, content):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(content)
    ok(f"{os.path.relpath(path, REPO_DIR)}")


def generate(name, drv_type, interface, chip, out_dir, gen_hdr=True):
    meta = DRIVER_TYPES[drv_type]
    slug = _slug(name)

    if out_dir is None:
        base = SRC_DRV_DIR if drv_type == "wifi" else USER_DRV_DIR
        subdir = drv_type if drv_type != "wifi" else ""
        out_dir = os.path.join(base, subdir, slug) if subdir else os.path.join(REPO_DIR, "source", "modules", slug)

    div(f"generating {drv_type} driver: {slug}")

    write_file(os.path.join(out_dir, "driver.pcat"),     gen_pcat(slug, drv_type, interface, chip))
    write_file(os.path.join(out_dir, "CMakeLists.txt"),  gen_cmake(slug, drv_type, interface))
    write_file(os.path.join(out_dir, f"{slug}.c"),       gen_source(slug, drv_type, interface))
    if gen_hdr and meta["catcall"]:
        write_file(os.path.join(out_dir, f"{slug}.h"),   gen_header(slug, drv_type, interface))

    div()
    info(f"Driver template ready at: {C_CYN}{os.path.relpath(out_dir, REPO_DIR)}{C_RST}")
    info(f"Next steps:")
    print(f"  1. Edit {C_WHT}{slug}.c{C_RST} — fill in the TODO sections")
    print(f"  2. Edit {C_WHT}driver.pcat{C_RST} — verify chip list and version")
    print(f"  3. {C_GRY}python3 modulestrap/modulestrap.py build {slug}{C_RST}")
    print(f"  4. Add to your device.pcat  [drivers] {drv_type} = \"{slug}\"")
    print(f"  5. {C_GRY}python3 purrstrap/purrstrap.py build <device>{C_RST}")
    print()


# ── CLI ───────────────────────────────────────────────────────────────────────

def cmd_list_types(args):
    div("driver types")
    for t, meta in DRIVER_TYPES.items():
        ifaces = ", ".join(meta["interfaces"])
        print(f"  {C_CYN}{t:<10}{C_RST}  {meta['desc']}")
        print(f"  {' '*10}  interfaces: {ifaces}")
    div()

def cmd_list_interfaces(args):
    t = args.type
    if t not in DRIVER_TYPES:
        die(f"unknown type '{t}'. Run 'driverstrap list-types'.")
    meta = DRIVER_TYPES[t]
    div(f"interfaces for {t}")
    for iface in meta["interfaces"]:
        print(f"  {C_CYN}{iface}{C_RST}")
    div()

def cmd_new(args):
    drv_type  = getattr(args, "type",      None)
    name      = getattr(args, "name",      None)
    interface = getattr(args, "interface", None)
    chip      = getattr(args, "chip",      "both")
    out_dir   = getattr(args, "output",    None)
    gen_hdr   = not getattr(args, "no_header", False)
    core      = getattr(args, "core",      False)

    # Prompt for missing fields
    if not drv_type:
        print(f"\n{C_BOLD}Driver type:{C_RST}")
        types = list(DRIVER_TYPES.keys())
        for i, t in enumerate(types, 1):
            print(f"  {C_CYN}{i}.{C_RST} {t:<10} — {DRIVER_TYPES[t]['desc']}")
        try:
            idx = int(input(f"\n  Choice [1-{len(types)}]: ").strip()) - 1
            drv_type = types[idx]
        except (ValueError, IndexError):
            die("invalid choice")

    if drv_type not in DRIVER_TYPES:
        die(f"unknown type '{drv_type}'. Run 'driverstrap list-types'.")

    if not name:
        name = input(f"\n  Driver name (slug, e.g. 'my_oled'): ").strip()
    if not re.match(r"^[a-z][a-z0-9_]*$", _slug(name)):
        die("name must be lowercase letters, digits, underscores only")

    meta = DRIVER_TYPES[drv_type]
    if not interface:
        ifaces = meta["interfaces"]
        if len(ifaces) == 1:
            interface = ifaces[0]
        else:
            print(f"\n{C_BOLD}Interface:{C_RST}")
            for i, ifc in enumerate(ifaces, 1):
                print(f"  {C_CYN}{i}.{C_RST} {ifc}")
            try:
                idx = int(input(f"\n  Choice [1-{len(ifaces)}]: ").strip()) - 1
                interface = ifaces[idx]
            except (ValueError, IndexError):
                die("invalid choice")

    if interface not in meta["interfaces"]:
        die(f"'{interface}' is not valid for type '{drv_type}'. "
            f"Options: {', '.join(meta['interfaces'])}")

    if not chip:
        chip = "both"
    if chip not in CHIP_CHOICES:
        die(f"chip must be one of: {', '.join(CHIP_CHOICES)}")

    if core and out_dir is None:
        out_dir = os.path.join(SRC_DRV_DIR, drv_type, _slug(name))

    generate(_slug(name), drv_type, interface, chip, out_dir, gen_hdr)


def main():
    parser = argparse.ArgumentParser(
        prog="driverstrap",
        description="PURR OS driver template generator",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="command")

    # new
    p_new = sub.add_parser("new", help="generate a driver template")
    p_new.add_argument("type",      nargs="?", choices=list(DRIVER_TYPES), help="driver type")
    p_new.add_argument("name",      nargs="?", help="driver name (slug)")
    p_new.add_argument("--interface", "-i", help="hardware interface")
    p_new.add_argument("--chip",      "-c", choices=list(CHIP_CHOICES), default="both")
    p_new.add_argument("--output",    "-o", help="override output directory")
    p_new.add_argument("--core",      action="store_true", help="write to source/drivers/ instead of user_drivers/")
    p_new.add_argument("--no-header", action="store_true", help="skip .h file generation")

    # list-types
    sub.add_parser("list-types", help="show all supported driver types")

    # list-interfaces
    p_li = sub.add_parser("list-interfaces", help="show interfaces for a type")
    p_li.add_argument("type", choices=list(DRIVER_TYPES))

    args = parser.parse_args()

    print()
    div("driverstrap")
    print(f"  {C_BOLD}PURR OS driver template generator{C_RST}  v{PURROS_VERSION}")
    div()
    print()

    if args.command == "new":           cmd_new(args)
    elif args.command == "list-types":  cmd_list_types(args)
    elif args.command == "list-interfaces": cmd_list_interfaces(args)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
