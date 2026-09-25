# PURR OS — driverstrap

driverstrap is the PURR OS driver template generator. It lives in `Developer/driverstrap/` and is aimed at driver developers and community contributors. Give it a type, a name, and an interface — it generates a complete, ready-to-modify driver skeleton in seconds.

---

## Quick Start

```bash
# Interactive wizard (recommended for first-timers)
python3 Developer/driverstrap/driverstrap_ui.py

# CLI — fully non-interactive
python3 Developer/driverstrap/driverstrap.py new display my_panel --interface spi
python3 Developer/driverstrap/driverstrap.py new touch my_touch --interface i2c --chip esp32s3
python3 Developer/driverstrap/driverstrap.py new radio my_lora --interface spi --chip esp32s3
python3 Developer/driverstrap/driverstrap.py new input my_kb --interface i2c
python3 Developer/driverstrap/driverstrap.py new gps my_gps --interface uart
python3 Developer/driverstrap/driverstrap.py new wifi my_wifi
```

---

## What Gets Generated

For every driver, driverstrap creates a directory under `user_drivers/<type>/<name>/` containing four files:

```
user_drivers/display/my_panel/
  driver.pcat        modulestrap manifest
  CMakeLists.txt     IDF component definition
  my_panel.h         public config struct + function declarations
  my_panel.c         full catcall implementation with TODO stubs
```

The `.c` file is not a skeleton with empty functions. Every catcall stub has comments showing the exact hardware operations that belong there — SPI bus init, register writes, I2C transactions, UART task setup, etc. — so you know immediately what to fill in.

---

## Supported Driver Types

| Type | Catcall registered | Description |
|------|--------------------|-------------|
| `display` | `catcall_display_t` | Pixel displays — push_pixels, fill_rect, brightness |
| `touch` | `catcall_touch_t` | Touch controllers — read_point, is_pressed |
| `input` | `catcall_input_t` | HID input — keyboard, trackball, buttons — poll_event queue |
| `radio` | `catcall_radio_t` | LoRa / sub-GHz radio — send, receive, rssi, snr |
| `gps` | `catcall_gps_t` | NMEA GPS — UART task, get_fix |
| `wifi` | *(none — system module)* | ESP32 WiFi stack — STA mode, event group, reconnect |

---

## Supported Interfaces

| Interface | What it generates |
|-----------|------------------|
| `spi` | SPI bus + device handle, CS/DC/RST/BL pin stubs, DMA push_pixels pattern |
| `i2c` | I2C master bus + device handle, register read/write helpers |
| `qspi` | 4-bit QSPI via `esp_lcd` panel IO (AXS15231B-style) |
| `parallel` | Parallel 8/16-bit via `esp_lcd` panel IO |
| `gpio` | GPIO ISR / polling task, FreeRTOS queue for events |
| `uart` | UART driver install, background parse task, line buffer |
| `builtin` | ESP32 WiFi init, event handler, `EventGroupHandle_t` connect/fail bits |

---

## CLI Reference

```
driverstrap new [type] [name] [options]
driverstrap list-types
driverstrap list-interfaces <type>
```

### `driverstrap new`

Generates a driver template. If `type` or `name` are omitted, the wizard prompts interactively.

| Option | Short | Default | Description |
|--------|-------|---------|-------------|
| `--interface` | `-i` | type default | Hardware interface (spi/i2c/qspi/parallel/gpio/uart/builtin) |
| `--chip` | `-c` | `both` | Target chip: `both` \| `esp32` \| `esp32s3` |
| `--output` | `-o` | `user_drivers/<type>/<name>/` | Override output directory |
| `--core` | | off | Write to `source/drivers/` instead of `user_drivers/` (core contribution) |
| `--no-header` | | off | Skip generating the `.h` public header |

### `driverstrap list-types`

Prints all supported types with their catcall and available interfaces.

### `driverstrap list-interfaces <type>`

Lists the valid `--interface` values for a given driver type.

---

## Interactive Wizard

Run `python3 Developer/driverstrap/driverstrap_ui.py` for a step-by-step guided session:

```
── driverstrap ────────────────────────────────────
  PURR OS driver template generator
  Generates a ready-to-modify driver skeleton in seconds.
──────────────────────────────────────────────────

What do you want to do?

  1. Generate a new driver template
  2. List all driver types
  3. List interfaces for a type
  q. Quit
```

Selecting **1** walks through six steps:

| Step | Prompt | Notes |
|------|--------|-------|
| 1 | Driver type | Numbered list with descriptions |
| 2 | Driver name | Validated: lowercase, digits, underscores |
| 3 | Hardware interface | Only shows valid options for the chosen type |
| 4 | Target chip | ESP32 + ESP32-S3 / ESP32-S3 only / ESP32 only |
| 5 | Output location | user_drivers / source/drivers / custom path |
| 6 | Options | Toggle .h header generation |

A summary is shown before any files are written; you can cancel at that point.

---

## Output Location

| Flag / selection | Where files go |
|-----------------|----------------|
| Default | `user_drivers/<type>/<name>/` |
| `--core` | `source/drivers/<type>/<name>/` |
| `--output <path>` | Any directory you specify |
| Wizard → custom | Any directory you type |

`user_drivers/` is the community drop zone — modulestrap auto-scans it, so no extra configuration is needed.

`source/drivers/` is for drivers being contributed back to the PURR OS built-in driver set.

---

## After Generation — Workflow

```bash
# 1. Fill in the TODO sections
$EDITOR user_drivers/display/my_panel/my_panel.c

# 2. Build the .purr blob
python3 modulestrap/modulestrap.py build my_panel

# 3. Reference it in your device.pcat
#    [drivers]
#    display = "my_panel"

# 4. Build and flash
python3 purrstrap/purrstrap.py build <device>
python3 purrstrap/purrstrap.py flash <device> -p /dev/ttyACM0 --erase
```

---

## Generated File Reference

### `driver.pcat`

```toml
name              = "my_panel"
version           = "0.1.0"
type              = "display"
interface         = "spi"
catcall_version   = 1

idf_min           = "5.3.0"
chip              = ["esp32", "esp32s3"]

kernel_min        = "0.9.0"
kernel_max        = ""

module_type       = "PURR_MOD_DRIVER"
provides          = ["catcall_display"]
required_catcalls = []
```

Edit `version` and `chip` as needed. `kernel_max` is empty by default — the module will load on any future KITT version; narrow it if your driver depends on a specific catcall struct layout.

### `CMakeLists.txt`

Auto-selects the right IDF REQUIRES for the chosen interface (`esp_driver_spi`, `esp_driver_i2c`, `esp_lcd`, `esp_driver_uart`, etc.) and adds `purr_kernel` for all types that register a catcall. Edit if you need additional IDF components.

### `<name>.h`

Declares a `<name>_config_t` struct with pins relevant to your interface, plus `<name>_configure()`, `<name>_drv_init()`, and `<name>_drv_deinit()`. The config struct fields are pre-populated for the interface type (e.g., `cs_pin`, `dc_pin`, `bl_pin` for SPI display).

### `<name>.c`

Contains:

1. **Static pin state** — pre-declared for the interface (e.g., `s_cs`, `s_dc`, `s_rst`, `s_bl` for SPI display)
2. **`<name>_configure()`** — copies a `<name>_config_t` into the static state; fill in the assignments
3. **`hw_init()` / `hw_deinit()`** — hardware bring-up and teardown with interface-specific TODO comments
4. **catcall function stubs** — one for every function in the catcall struct, with comments showing the expected implementation
5. **`static const <catcall_type_t> s_catcall`** — the catcall struct, declared `static const` at file scope (never on the stack — a compound literal passed directly to `purr_kernel_register_*` would be a dangling pointer)
6. **`<name>_drv_init()` / `<name>_drv_deinit()`** — public entry points called by modulestrap
7. **`purr_module_header_t purr_module`** — the `.purr` ABI export with all fields filled

---

## Example: I2C Touch Driver

```bash
python3 Developer/driverstrap/driverstrap.py new touch ft6336 --interface i2c --chip esp32s3
```

Generated `ft6336.c` (key sections):

```c
static esp_err_t catcall_init(const touch_config_t *cfg) {
    (void)cfg;
    // TODO: initialize I2C bus, device handle, INT pin
    // i2c_master_bus_config_t bus_cfg = { .i2c_port = I2C_NUM_0, .sda_io_num = s_sda, ... };
    // i2c_new_master_bus(&bus_cfg, &s_bus);
    // i2c_device_config_t dev_cfg = { .device_address = 0x38, ... };
    // i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    s_initialized = true;
    return ESP_OK;
}

static bool catcall_read_point(uint16_t *x, uint16_t *y) {
    if (!s_initialized || !x || !y) return false;
    // TODO: read touch point register(s) over I2C
    // uint8_t buf[4] = {0};
    // i2c_master_receive(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(50));
    // *x = ((buf[0] & 0x0F) << 8) | buf[1];
    // *y = ((buf[2] & 0x0F) << 8) | buf[3];
    // return true;
    return false;
}
```

Fill in the I2C address and register layout for your specific chip, and the driver is ready to build.

---

## Tips

- **Name collisions:** If a driver with the same name already exists in `user_drivers/`, driverstrap will overwrite it. Rename before generating if you want to keep the old version.
- **WiFi module:** The wifi type generates a `PURR_MOD_SYSTEM` module, not a driver. It does not register any catcall. Place it in `source/modules/` or a custom path — `user_drivers/` is for hardware drivers only.
- **Multiple drivers from one chip:** Some ICs provide both display and touch (e.g., AXS15231B). Generate two separate drivers — one `display`, one `touch` — and cross-reference them in your device.pcat.
- **Baked-in vs. .purr:** If your device needs a specialized kernel (e.g., requires Arduino Wire for I2C), the driver source goes in `source/kernel/kernel_<device>/` instead of a `.purr` blob. See [13_Kernels.md](13_Kernels.md) for that path.
