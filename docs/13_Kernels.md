# PURR OS — Specialized Kernels

## What a Specialized Kernel Is

The generic PURR OS kernel (`source/kernel/core/`) is intentionally hardware-agnostic. It mounts SPIFFS, loads `.purr` module blobs, and idles. All hardware is reached through drivers loaded at runtime.

A **specialized kernel** replaces the generic boot entirely for one device. Instead of `core/boot.c`, a device-specific `kernel_<device>/boot.c` or `boot.cpp` is compiled as the IDF main component. The specialized kernel:

1. Performs all hardware initialization directly at boot (before any module loads)
2. Calls `purr_kernel_register_*()` to hand drivers into the catcall registry
3. Then calls `purr_kernel_scan_modules()` just like the generic core

From the perspective of everything above the kernel (UI frameworks, apps, modules), there is no difference. The catcall interface is identical.

---

## When to Use a Specialized Kernel

Use a specialized kernel when:

- **The IDF driver stack is broken for your hardware** (e.g., IDF 5.3 i2c_master regression on GT911)
- **Boot order matters** — the device has a power-gate GPIO that must be driven before any peripheral will respond
- **You need a non-IDF I2C/SPI path** — for example, Arduino Wire, bit-bang, or a vendor HAL
- **You need a diagnostic or test mode** — a kernel that boots to a hardware visualizer instead of the OS

If the standard IDF driver stack works for your device, use the generic core and a `.purr` driver blob instead.

---

## CMake Selection

`CoreOS/main/CMakeLists.txt` selects the kernel automatically:

```cmake
# Look for a specialized kernel directory
set(KERNEL_SPECIALIZED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../source/kernel/kernel_${PURR_DEVICE}")

if(EXISTS "${KERNEL_SPECIALIZED_DIR}")
    # Use specialized kernel — glob .c and .cpp
    file(GLOB KERNEL_SRCS "${KERNEL_SPECIALIZED_DIR}/*.c" "${KERNEL_SPECIALIZED_DIR}/*.cpp")
    # Also include kernel_arduino/ helpers for Arduino-backed kernels
    set(EXTRA_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/../../source/kernel/kernel_arduino")
else()
    # Fall back to generic core
    file(GLOB KERNEL_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/../../source/kernel/core/*.c")
endif()
```

The device name comes from the CMake variable `PURR_DEVICE`, set by purrstrap at build time:
```bash
cmake -DPURR_DEVICE=tdeck_plus_arduino ...
```

---

## Arduino-Backed Kernels

Some specialized kernels use the **arduino-esp32** managed component for I2C (Wire), UART (Serial), and other Arduino APIs. This completely bypasses the IDF 5.3 driver stack that has regressions on certain hardware.

### Requirements for Arduino kernels

**`CoreOS/main/idf_component.yml`** must include:
```yaml
dependencies:
  espressif/arduino-esp32:
    version: ">=3.0.0"
```

**`CoreOS/sdkconfig_<device>`** must include:
```
CONFIG_FREERTOS_HZ=1000
CONFIG_ARDUINO_RUNNING_CORE=1
CONFIG_ARDUINO_LOOP_STACK_SIZE=8192
CONFIG_ARDUINO_EVENT_RUNNING_CORE=1
CONFIG_ARDUINO_SERIAL_EVENT_TASK_RUNNING_CORE=1
```

`CONFIG_FREERTOS_HZ=1000` is mandatory — arduino-esp32 requires a 1 kHz FreeRTOS tick. The default ESP-IDF tick (100 Hz) will cause compile errors.

**`CoreOS/main/CMakeLists.txt`** must add `espressif__arduino-esp32` to REQUIRES:
```cmake
string(FIND "${PURR_DEVICE}" "arduino" _IS_ARDUINO)
if(_IS_ARDUINO GREATER -1)
    set(EXTRA_REQUIRES espressif__arduino-esp32)
endif()
```

### Shared Arduino kernel helpers

**`source/kernel/kernel_arduino/kernel_arduino.h`** provides two static inline helpers used by all Arduino-backed kernels:

```c
static inline void arduino_kernel_nvs_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

static inline void arduino_kernel_spiffs_init(const char *tag) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/flash",
        .partition_label = "spiffs",
        .max_files = 16,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK)
        ESP_LOGE(tag, "SPIFFS mount failed: %s", esp_err_to_name(err));
}
```

All Arduino kernels call these at the top of `app_main` before any hardware init.

### Serial console in Arduino kernels

Arduino kernels use `Serial.begin(115200)` and `Serial.available()` / `Serial.read()` instead of `uart_driver_install()`. This avoids a conflict where both Arduino and IDF try to own UART0. The pre-existing "Writing to serial is timing out" bug in the non-Arduino kernels is not present in Arduino kernels because they never call `uart_driver_install`.

---

## Existing Specialized Kernels

### `kernel_tdeck` — T-Deck

**Source:** `source/kernel/kernel_tdeck/kernel_td_boot.c`
**Device:** `tdeck`

Minimal specialization. T-Deck has no touch and uses a trackball for navigation. The specialized kernel initializes the trackball GPIO ISR directly at boot and handles the SD card mount, then hands off to the module system for display and LoRa.

---

### `kernel_tdeck_plus` — T-Deck Plus (IDF path, canonical)

**Source:** `source/kernel/kernel_tdeck_plus/kernel_tdp_boot.c`
**Device:** `tdeck_plus`

IDF i2c_master path for T-Deck Plus. Handles BOARD_POWERON, ST7789 display, and GT911 touch init via `gt911_configure()`.

**✅ Fixed in `322159c6`:** the IDF 5.3 `i2c_master_probe()`/`i2c_master_transmit()` regression that previously broke GT911 discovery is worked around at the driver level — see `source/drivers/touch/gt911/gt911.c`. The earlier byte-offset bug in point parsing, the missing hardware RST pulse (now wired to GPIO17), and unsigned-vs-signed coordinate interpretation have all been corrected and confirmed against hardware. Display, touch, and trackball all work. This is now the canonical T-Deck Plus kernel — `kernel_tdeck_plus_arduino` is staged for deprecation (see its section below).

**Boot sequence:**
```c
// 1. Pull INT LOW before power-on (anchors GT911 at I2C 0x5D)
gpio_set_direction(GPIO_NUM_16, GPIO_MODE_OUTPUT);
gpio_set_level(GPIO_NUM_16, 0);

// 2. BOARD_POWERON HIGH — gates all peripherals on
gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT);
gpio_set_level(GPIO_NUM_10, 1);
vTaskDelay(pdMS_TO_TICKS(50));

// 3. Release INT as input
gpio_set_direction(GPIO_NUM_16, GPIO_MODE_INPUT);
vTaskDelay(pdMS_TO_TICKS(50));

// 4. ST7789 display init (SPI)
st7789_configure(CS=12, DC=11, MOSI=41, SCLK=40, RST=-1, BL=42);
st7789_drv_init();

// 5. GT911 touch init (RST=GPIO17, fixed in 322159c6)
gt911_configure(SDA=18, SCL=8, RST=17, INT=-1, poll_mode=0);
gt911_drv_init();

// 6. Trackball GPIO ISR
trackball_drv_init();

// 7. Module system
purr_kernel_scan_modules("/flash/modules");
```

---

### `kernel_tdeck_plus_arduino` — T-Deck Plus (Arduino kernel, **DEPRECATED**)

**Source:** `source/kernel/kernel_tdeck_plus_arduino/kernel_atdp_boot.cpp`
**Device:** `tdeck_plus_arduino`
**Language:** C++ (Arduino framework)

**⚠️ DEPRECATED — scheduled for removal.** This kernel existed solely to work around the IDF 5.3 `i2c_master` regression that broke GT911 touch on `kernel_tdeck_plus`. That regression is now fixed at the driver level (see `kernel_tdeck_plus` above, commit `322159c6`), so this kernel is no longer needed and has *not* received the same GT911 fixes (it still has the old byte-offset/unsigned-coordinate behavior). It remains buildable as a rollback target only until the on-hardware validation checklist in `PURR_OS_1.0_CHECKLIST.md` passes, after which it will be archived to `archive/`. Do not build new functionality on top of this kernel — target `kernel_tdeck_plus` instead.

Uses Arduino Wire for all I2C, bypassing the IDF 5.3 i2c_master regression. All hardware confirmed working: display, touch, trackball, keyboard, SD.

**What it initializes:**

| Peripheral | Method | Details |
|-----------|--------|---------|
| ST7789 display | Baked-in SPI driver | CS=12, DC=11, MOSI=41, SCLK=40, RST=-1, BL=42 |
| GT911 touch | Arduino Wire | SDA=18, SCL=8, INT=16, RST=NC, addr=0x5D |
| Trackball | GPIO ISR | UP=3, DN=15, LT=1, RT=2, CLK=0 |
| BBQ20 keyboard | Arduino Wire | I2C 0x55, plain `requestFrom` read |
| Serial console | `Serial.begin(115200)` | `scan` command via `Wire.beginTransmission` sweep |

**GT911 Wire helpers:**

```cpp
static bool gt911_write_reg(uint16_t reg, uint8_t val) {
    Wire.beginTransmission(s_gt911_addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool gt911_read_reg(uint16_t reg, uint8_t *buf, size_t len) {
    Wire.beginTransmission(s_gt911_addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom(s_gt911_addr, (uint8_t)len);
    for (size_t i = 0; i < len; i++) {
        if (!Wire.available()) return false;
        buf[i] = Wire.read();
    }
    return true;
}
```

**Touch catcall (`catcall_touch_t`) implementation:**

```cpp
static bool gt911_catcall_is_pressed(void) {
    if (!s_gt911_ok) return false;
    uint8_t status = 0;
    if (!gt911_read_reg(0x814E, &status, 1)) return false;
    return (status & 0x80) && (status & 0x0F) > 0;
}

static bool gt911_catcall_read_point(uint16_t *x, uint16_t *y) {
    if (!s_gt911_ok || !x || !y) return false;
    uint8_t status = 0;
    if (!gt911_read_reg(0x814E, &status, 1)) return false;
    if (!(status & 0x80) || (status & 0x0F) == 0) return false;

    uint8_t pt[5] = {0};
    bool ok = gt911_read_reg(0x8150, pt, sizeof(pt));
    gt911_write_reg(0x814E, 0x00);   // MUST clear status on every read with buffer-ready set
    if (!ok) return false;

    *x = (uint16_t)pt[1] | ((uint16_t)pt[2] << 8);
    *y = (uint16_t)pt[3] | ((uint16_t)pt[4] << 8);
    return true;
}
```

**BBQ20 keyboard polling task:**

```cpp
static void bbq20_poll_task(void *arg) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));
        Wire.requestFrom((uint8_t)BBQ20_ADDR, (uint8_t)1);  // plain read, no write preamble
        if (!Wire.available()) continue;
        uint8_t key = Wire.read();
        if (key == 0) continue;
        input_event_t ev = { .type = INPUT_EVENT_KEY_DOWN, .keycode = key };
        if (s_kb_queue) xQueueSend(s_kb_queue, &ev, 0);
    }
}
```

**Boot sequence:**
```cpp
extern "C" void app_main(void) {
    arduino_kernel_nvs_init();
    arduino_kernel_spiffs_init(TAG);

    // BOARD_POWERON sequence (see 04_Devices.md)
    gpio_set_level(GPIO_NUM_16, 0);   // INT LOW — address anchor
    gpio_set_level(GPIO_NUM_10, 1);   // BOARD_POWERON HIGH
    vTaskDelay(50);
    gpio_set_direction(GPIO_NUM_16, GPIO_MODE_INPUT);
    vTaskDelay(50);

    // Display
    st7789_configure(12, 11, 41, 40, -1, 42);
    st7789_drv_init();

    // I2C via Wire — bypasses IDF 5.3 regression
    Wire.begin(18, 8);
    Wire.setClock(400000);
    Wire.setTimeOut(50);      // prevents BBQ20 NACK from hanging the bus
    vTaskDelay(300);          // let peripherals settle

    // GT911 touch probe + register
    if (gt911_probe(0x5D) || gt911_probe(0x14)) {
        gt911_write_reg(0x814E, 0x00);
        purr_kernel_register_touch(&s_touch_catcall);
    }

    // Trackball
    trackball_drv_init();

    // BBQ20 keyboard
    Wire.beginTransmission(0x55);
    if (Wire.endTransmission() == 0) {
        s_kb_queue = xQueueCreate(32, sizeof(input_event_t));
        xTaskCreate(bbq20_poll_task, "bbq20", 2048, NULL, 3, NULL);
        purr_kernel_register_input(&s_kb_catcall);
    }

    // Static modules + module scanner
    purr_register_static_modules();
    purr_kernel_load_static_modules();
    purr_kernel_scan_modules("/flash/modules");

    xTaskCreate(serial_console_task, "serial_con", 4096, NULL, 1, NULL);
    while (1) vTaskDelay(10000);
}
```

---

### `kernel_tdeck_plus_pounce` — T-Deck Plus (Pounce backend)

**Source:** `source/kernel/kernel_tdeck_plus_pounce/`
**Device:** `tdeck_plus_pounce`

Same hardware bring-up as `kernel_tdeck_plus`, paired with the `pounce`
framebuffer UI backend instead of an LVGL one. Exists as a parallel target so
that backend can be built and flashed without disturbing the production
`tdeck_plus` device. See `pounce_plan.md`.

### `kernel_tab5_m5bsp_legacy` — M5Stack Tab5 (BSP path)

**Source:** `source/kernel/kernel_tab5_m5bsp_legacy/kernel_tab5_boot.c`
**Device:** `tab5` (legacy path)

Earlier Tab5 bring-up routed through M5Stack's own BSP (`m5tab5_bsp` display
driver) rather than the direct `st7123` MIPI-DSI path the device uses now.
Retained as a fallback and reference — the name records that it is the legacy
route, not the current one.

Tab5 is the only **ESP32-P4** device in the tree, so this and the current Tab5
path are the only kernels not targeting ESP32/ESP32-S3.

### `kernel_tdeck_plus_test` — T-Deck Plus Input Test Mode

**Source:** `source/kernel/kernel_tdeck_plus_test/kernel_tdp_test.cpp`
**Device:** `tdeck_plus_test`
**Language:** C++ (Arduino framework)

Diagnostic kernel that boots directly to a full-screen input visualizer. No modules, no apps. Use this to confirm all input hardware is working before running the full OS.

**Screen layout (320×240):**

```
┌────────────────┬────────────────┬────────────────┐
│   TOUCH        │   TRACKBALL    │   KEYBOARD     │
│                │                │                │
│  X: 160        │  ← (LEFT)      │  'A' (0x41)    │
│  Y: 120        │                │                │
│  (tap here)    │  ↑ ↓ ← → CLK  │                │
└────────────────┴────────────────┴────────────────┘
  col 0–79         col 80–159       col 160–239
```

**Built-in font:** 6×8 pixel bitmap font embedded directly in the source (no external font dependency).

**GT911 keepalive:** `touch_write_reg(0x8040, 0x00)` called every 2000 ms to prevent GT911 sleep lockout.

**All events also print over serial** at 115200 baud:
```
[TOUCH] x=160 y=120
[TRACKBALL] LEFT
[TRACKBALL] CLK
[KEY] 'A' (0x41)
```

**Building and flashing:**
```bash
python3 purrstrap/purrstrap.py build tdeck_plus_test
python3 purrstrap/purrstrap.py flash tdeck_plus_test -p /dev/ttyACM0 --erase
python3 purrstrap/purrstrap.py monitor tdeck_plus_test -p /dev/ttyACM0
```

---

## Creating a New Specialized Kernel

1. **Create the directory:**
   ```
   source/kernel/kernel_<device>/
     boot.c   or   boot.cpp
   ```

2. **Include the right headers:**
   ```c
   // For a C kernel:
   #include "purr_kernel.h"
   #include "purr_module.h"

   // For an Arduino kernel (C++):
   #include "Arduino.h"
   #include "Wire.h"
   extern "C" {
   #include "../kernel_arduino/kernel_arduino.h"
   #include "purr_kernel.h"
   }
   ```

3. **Implement `app_main`:**
   - Initialize NVS and SPIFFS (use `arduino_kernel_nvs_init()` helpers for Arduino kernels)
   - Init hardware in boot order specific to your device
   - Call `purr_kernel_register_display()`, `purr_kernel_register_touch()`, etc.
   - Call `purr_kernel_scan_modules("/flash/modules")`
   - Idle loop at the end

4. **Add a device config:**
   ```
   source/devices/<device>/device.pcat
   CoreOS/sdkconfig_<device>
   ```

5. **If using Arduino Wire, add to sdkconfig:**
   ```
   CONFIG_FREERTOS_HZ=1000
   CONFIG_ARDUINO_RUNNING_CORE=1
   CONFIG_ARDUINO_LOOP_STACK_SIZE=8192
   ```

6. **Build:**
   ```bash
   python3 purrstrap/purrstrap.py build <device>
   ```

CMake will find `kernel_<device>/` automatically and use it instead of the generic core.

---

## Kernel Directory Map

```
source/kernel/
  core/                          Generic kernel — most devices use this
    boot.c
    purr_kernel.h / .c
    purr_module.h
  catcalls/                      Catcall headers — shared by all kernels
    catcall_display.h
    catcall_touch.h
    catcall_input.h
    catcall_radio.h
    catcall_gps.h
    catcall_ui.h
    catcalls.h
    purr_win.h
  kernel_arduino/                Shared helpers for Arduino-backed kernels
    kernel_arduino.h
  kernel_tdeck/                  T-Deck specialized kernel
    kernel_td_boot.c
  kernel_tdeck_plus/             T-Deck Plus IDF kernel (touch broken — see §Known Issues)
    kernel_tdp_boot.c
  kernel_tdeck_plus_arduino/     T-Deck Plus Arduino kernel (production)
    kernel_atdp_boot.cpp
  kernel_tdeck_plus_test/        Input test mode (dev/debug)
    kernel_tdp_test.cpp
```

---

*DP8 documentation pass performed by Claude Opus 5 in agentic/auto mode.*
