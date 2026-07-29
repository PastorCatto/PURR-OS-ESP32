# PURR OS — Devices

## device.pcat Format

Each supported device has a `device.pcat` at `source/devices/<slug>/device.pcat`. This is a TOML-like manifest describing everything `purrstrap` needs to build firmware for that device.

```ini
[device]
name        = "human readable name"
chip        = "esp32" | "esp32s3"
kernel_type = "native" | "arduino"    # drives CMake's Wire-vs-IDF-driver REQUIRES
flash_mb    = 4 | 8 | 16
psram       = true | false
psram_mb    = 8
port        = "/dev/ttyACM0"          # default flash port
spiffs_offset = "0xD90000"            # SPIFFS partition start (hex)
boot_settle_ms = 50                   # optional; kernel default 500 if omitted

[drivers]
display = "ili9341" | "st7789" | "axs15231b" | "ssd1306" | ""
touch   = "xpt2046" | "cst816s" | "gt911" | ""
input   = "trackball" | "bbq20" | ""
radio   = "sx1262" | "sx1262_rl" | "sx1276" | ""
gps     = "generic_nmea" | ""

[radio]
wifi = true | false
bt   = true | false
lora = "sx1262" | "sx1262_rl" | "sx1276" | ""

[modules]
ui          = "kittenui" | "miniwin" | "cupcake" | "cardstack" | "mochi"
              | "tabby" | "nougat" | "pounce" | "blackpurr" | "oled_ui"
              | "lvgldebug"        # see 03_Modules.md for the tier of each
app_manager = "app_manager"
systemui    = "systemui"           # optional: status bar / panels / lock screen

[ui]
theme = "wce" | "dark"             # LVGL backends + MiniWin
style = "full" | "compact"         # oled_ui only
systemui_style = "android" | "ios" # which systemui implementation compiles in

[apps]
# true = bake this app blob into the SPIFFS image at build time
settings   = true | false
about      = true | false
terminal   = true | false
fileman    = true | false
calculator = true | false

[flash]
# Maps module/driver slug to load priority (1=required, 2=important, 3=optional)
display/st7789  = 1
touch/gt911     = 1
mochi           = 2
app_manager     = 2

[pins]
# GPIO assignments. Use -1 for not connected / not applicable.
display_cs    = 12
display_dc    = 11
...
```

purrstrap reads this file to:
1. Determine which driver blobs to include in the SPIFFS image
2. Generate `purr_device_glue.c` with pin `#defines` and radio capability flags
3. Generate `CoreOS/sdkconfig_<slug>` (chip, flash size, PSRAM, UI backend, Arduino kernel config) via `purrstrap generate` — see [07_Build_Tools.md](07_Build_Tools.md)
4. Decide which system apps to bake in
5. Decide **which components compile at all** — modulestrap takes the transitive
   closure of what this file references and emits only that into
   `components_manifest.cmake`

Point 5 is newer than the rest and changes how the file should be read. A
`device.pcat` is no longer a description of a build that happens anyway; it *is*
the build. A name removed from `[modules]` is not merely unregistered — its
component is never added, so a stale reference to it in `CoreOS/main/CMakeLists.txt`
becomes a configure-time failure rather than silently linking dead code. This is
why the hardcoded `TAB5_REQUIRES` and driver lists there had to become
`PURR_DRIVER_REQUIRES`, fed from this same `[drivers]` section.

Use `modulestrap modules <device>` to see the resulting component set, and
`modulestrap enable`/`disable` to change it — see
[07_Build_Tools.md](07_Build_Tools.md#toggling-modules-per-device). Editing by
hand is fine; the tool exists mainly so the consequences are visible before a
rebuild.

### `boot_settle_ms`

The pause the kernel takes between the P2 (UI) and P3 (radios, deferred apps)
module tiers — see `BOOT_SETTLE_MS` in `purr_kernel.c` for what it guards
against. Omit it and the kernel's own default of 500 ms applies; a hand-run
`idf.py` outside purrstrap gets the default too.

It is per-device because the right value depends on what the image contains. A
build with no mesh, BLE, proximity or Nearby stack has nothing to race the UI at
P3, and `tdeck_plus`'s minimal profile sets 50 on exactly that reasoning —
roughly halving time-to-launcher. A full image should leave it unset.

---

## Supported Devices

### jc3248w535

```
Chip:     ESP32-S3
Flash:    16 MB
PSRAM:    8 MB
Screen:   3.5" AXS15231B 480×320 QSPI
Touch:    AXS15231B integrated capacitive (I2C)
Input:    —
Radio:    WiFi + BT (built-in silicon)
SD:       no
UI:       MiniWin
Apps:     settings, about, terminal, fileman, calculator
Kernel:   generic core
```

Largest display in the lineup. 8 MB PSRAM makes it the best candidate for MagicMac and MagiDOS. Uses QSPI (4-bit parallel) rather than SPI — the AXS15231B driver uses ESP-IDF's panel IO QSPI interface. Touch is integrated into the same IC as the display controller.

---

### tdeck_plus

```
Chip:     ESP32-S3
Flash:    16 MB
PSRAM:    8 MB
Screen:   3.2" ST7789 320×240 SPI
Touch:    GT911 capacitive (I2C — SDA=18, SCL=8, INT=16, RST=NC)
Input:    Trackball (UP=3, DN=15, LT=1, RT=2, CLK=0) + BBQ20 keyboard (I2C 0x55)
Radio:    WiFi + BT (built-in) + SX1262 LoRa via sx1262_rl (SPI, shared bus)
GPS:      Generic NMEA UART (TX=43, RX=44)
SD:       yes (SPI)
UI:       Mochi (iOS-style springboard; hosts systemui, iOS style)
Apps:     settings, about, terminal, fileman, calculator
Kernel:   kernel_tdeck_plus_arduino (production), kernel_tdeck_plus (IDF path — touch broken on IDF 5.3)
```

Full-featured field device. LoRa, GPS, trackball, physical keyboard (BBQ20 RP2040 bridge at I2C 0x55), and SD. The **Arduino kernel** (`kernel_tdeck_plus_arduino`) is the production path — it uses Arduino Wire for I2C, bypassing an IDF 5.3 regression that breaks GT911 touch discovery.

#### BOARD_POWERON (GPIO 10)

T-Deck Plus has a power gate GPIO that must be driven HIGH before any peripheral (SPI display, I2C touch, I2C keyboard) will respond. This is handled in the specialized kernel boot sequence:

```c
// INT LOW first — anchors GT911 I2C address at 0x5D
gpio_set_direction(GPIO_NUM_16, GPIO_MODE_OUTPUT);
gpio_set_level(GPIO_NUM_16, 0);

// Gate all peripherals on
gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT);
gpio_set_level(GPIO_NUM_10, 1);
vTaskDelay(pdMS_TO_TICKS(50));

// Release INT as input so GT911 can drive it
gpio_set_direction(GPIO_NUM_16, GPIO_MODE_INPUT);
vTaskDelay(pdMS_TO_TICKS(50));
```

If BOARD_POWERON is not set HIGH, all I2C and SPI peripherals appear absent.

#### GT911 Touch Notes

- I2C address is 0x5D (INT pulled LOW during power-on) or 0x14 (INT pulled HIGH)
- Status register 0x814E must be cleared (write `0x00`) after every read with the buffer-ready bit (0x80) set, or the chip stops reporting touches
- GT911 enters power-save mode if not polled. Write `0x00` to command register 0x8040 periodically (every ~2 s) to prevent sleep lockout
- `Wire.setTimeOut(50)` is required on the shared I2C bus to prevent the BBQ20 from hanging the bus on NACK

#### BBQ20 Keyboard Notes

- RP2040-based keyboard bridge at I2C 0x55, shared bus with GT911
- Read protocol: plain `Wire.requestFrom(0x55, 1)` — no write preamble needed. Returns 0x00 when no key is pressed
- A `beginTransmission` / `endTransmission(false)` probe before `requestFrom` confuses the RP2040 bridge and returns no key data
- Has a controllable under-key LED backlight (register `0x05`, single byte 0-255) — driven via `bbq20_set_backlight()`, exposed generically through `catcall_input_t.set_backlight` + `purr_kernel_keyboard_set_backlight()`, controllable from Settings
- `CONFIG_PURR_TDECK_PLUS_PHYSICAL_KEYBOARD` (default `y`, `CoreOS/main/Kconfig.projbuild`) can disable BBQ20 init entirely for bring-up/testing without the physical keyboard attached — the on-screen keyboard (`purr_win_keyboard_show()`) still works either way. Previously there was no way to disable this at all; it was unconditionally hardcoded into `kernel_tdp_boot.c`.

---

### tdeck

```
Chip:     ESP32-S3
Flash:    16 MB
PSRAM:    none
Screen:   3.2" ST7789 320×240 SPI
Touch:    none
Input:    Trackball (4-dir GPIO + click)
Radio:    WiFi + BT (built-in) + SX1262 LoRa (SPI)
SD:       yes (SPI)
UI:       MiniWin
Apps:     settings, about, terminal, fileman, calculator
Kernel:   kernel_tdeck
```

Same form factor as T-Deck Plus but no touch. Both use an SX1262; `tdeck` selects the plain `sx1262` SPI driver while `tdeck_plus` uses the RadioLib-backed `sx1262_rl`. Navigation via trackball only — apps should handle trackball focus events through `catcall_input_t`.

---

### cyd

```
Chip:     ESP32
Flash:    4 MB
PSRAM:    none
Screen:   2.8" ILI9341 320×240 SPI
Touch:    XPT2046 resistive (SPI, shared bus with display)
Input:    —
Radio:    WiFi + BT (built-in)
SD:       yes (optional SPI)
UI:       MiniWin
Apps:     settings, about, terminal, fileman, calculator
Kernel:   generic core
```

The original "Cheap Yellow Display". XPT2046 resistive touch shares the SPI bus via a separate CS pin. Resistive touch requires slightly more pressure than capacitive — tap targets should be at least 24 px. 3-point calibration stored in NVS on first boot.

---

### cyd_s024c

```
Chip:     ESP32
Flash:    4 MB
PSRAM:    none
Screen:   2.4" ILI9341-compatible 240×320 SPI
Touch:    CST816S capacitive (I2C, address 0x15)
Input:    —
Radio:    WiFi + BT (built-in)
SD:       yes (optional SPI)
UI:       MiniWin
Apps:     settings, about, terminal, fileman, calculator
Kernel:   generic core
```

**Critical:** Backlight is GPIO **27**, not GPIO 21. Incorrect backlight pin was a common v0.11 issue. The device.pcat has the correct value.

---

### cyd_s028r

```
Chip:     ESP32
Flash:    4 MB
PSRAM:    none
Screen:   2.8" ILI9341 320×240 SPI
Touch:    XPT2046 resistive (SPI, shared bus)
Input:    —
Radio:    WiFi + BT (built-in)
SD:       yes (optional SPI)
UI:       MiniWin
Apps:     settings, about, terminal, fileman, calculator
Kernel:   generic core
```

Same panel size as `cyd` but a portrait-flip MADCTL issue requires `display_madctl = 0x40` in the device.pcat. The driver applies this automatically.

---

### heltec

```
Chip:     ESP32-S3
Flash:    8 MB
PSRAM:    none
Screen:   128×64 SSD1306 OLED (I2C)
Touch:    none
Input:    —
Radio:    WiFi + BT (built-in) + SX1262 LoRa (SPI)
SD:       no
UI:       oled_ui (text-mode only)
Apps:     none (screen too small)
Kernel:   generic core
```

LoRa-first node. No GUI apps — `oled_ui` provides a text-mode status display with a 6×8 bitmap font. The `oled_ui` module exposes `oled_ui_log(const char *line)` for other modules to print status lines. Radio and terminal tools work via SD card.

---

### waveshare169

```
Chip:     ESP32-S3
Flash:    4 MB
PSRAM:    none
Screen:   1.69" ST7789 240×280 SPI
Touch:    CST816S capacitive (I2C, address 0x15)
Input:    —
Radio:    WiFi + BT (built-in)
SD:       no
UI:       MiniWin
Apps:     none (screen below medium threshold)
Kernel:   generic core
```

Small wearable / badge device. KittenUI loads so custom `.meow`/`.paws` scripts on SD work, but built-in system apps are excluded because 240×280 is too small for their layouts. Use `.meow` Lua scripts for small-screen UIs.

---

### tab5

M5Stack Tab5 — the only **ESP32-P4** device in the tree, and the only one not on
an ESP32/ESP32-S3.

| | |
|---|---|
| Chip | esp32p4 |
| Flash / PSRAM | 16 MB / yes |
| Display | `st7123` |
| Touch | `st7123` (same controller) |
| Input | `tab5_kbd` |
| Radio | none |
| UI | `cupcake` |

Originally brought up on `nougat` (LVGL 9). It was moved back to `cupcake` once
LVGL 8 proved stable on this hardware, so Nougat is now parked — its manifest
and Kconfig help still describe Tab5 as its target, which is historical. See
`02_Catcalls.md`.

### tdeck_plus_pounce

Same hardware as `tdeck_plus`, but running the `pounce` framebuffer backend
instead of an LVGL one. A parallel target rather than a replacement, so the
experimental backend can be built and flashed without disturbing the production
`tdeck_plus` device.

| | |
|---|---|
| Chip | esp32s3 |
| Flash / PSRAM | 16 MB / yes |
| Display / Touch | `st7789` / `gt911` |
| Input | `trackball` (+ BBQ20 keyboard via the specialized kernel) |
| Radio | `sx1262` |
| UI | `pounce` |

Uses `kernel_tdeck_plus_pounce`. See `pounce_plan.md` for the backend's design
and current status.

---

## Development / Debug Targets

These targets are not shipped as production firmware. They exist for hardware bring-up and diagnostics.

### tdeck_plus_arduino

Same hardware as `tdeck_plus`. Uses `kernel_tdeck_plus_arduino` which drives all peripherals via Arduino Wire instead of IDF i2c_master. This is the current recommended production kernel for T-Deck Plus until the IDF 5.3 regression is resolved. Enabled by having `source/devices/tdeck_plus_arduino/device.pcat` and `source/kernel/kernel_tdeck_plus_arduino/`.

### tdeck_plus_test

Input test mode for T-Deck Plus. Uses `kernel_tdeck_plus_test` which boots to a 3-panel visualizer (no modules, no apps):
- Left panel: live touch coordinates
- Center panel: trackball direction + click events
- Right panel: keyboard keypresses (character + hex)

All events also print over serial at 115200 baud. Use this target to verify all input hardware is working before running the full OS stack.

---

## Screen Size Classification

purrstrap uses this to decide the default app bundle:

| Class | Resolution | Devices | Apps bundled |
|-------|-----------|---------|--------------|
| Small | < 240×240 | heltec (128×64) | none |
| Small-medium | 240×280 | waveshare169 | none |
| Medium | 240×320 – 320×240 | cyd, cyd_s024c, cyd_s028r | all 5 |
| Large | 320×240+ | tdeck, tdeck_plus, jc3248w535 | all 5 |

Override with explicit `apps.* = true | false` in device.pcat.

---

## Adding a New Device

1. Create `source/devices/<slug>/device.pcat` — fill in all sections, including `kernel_type`
2. If the device needs specialized hardware init, create `source/kernel/kernel_<slug>/` with a `boot.c` or `boot.cpp`
3. `purrstrap build <slug>` (or `purrstrap generate <slug>`) generates `CoreOS/sdkconfig_<slug>` automatically from device.pcat — only add a hand-maintained `CoreOS/sdkconfig_<slug>.overrides` if the device needs a hardware-specific quirk with no equivalent pcat field (panel mirroring, WinCE shell, etc. — see the existing `.overrides` files for examples)
4. Run `purrstrap list` — your device should appear
5. Run `purrstrap build <slug>` — purrstrap generates glue + sdkconfig and assembles the image

If the display or touch driver does not exist yet, see [05_Drivers.md](05_Drivers.md).

---

## Pin Reference

### Display — SPI

| Key | Description |
|-----|-------------|
| `display_cs` | Chip select |
| `display_dc` | Data/command |
| `display_mosi` | SPI MOSI |
| `display_sclk` | SPI clock |
| `display_rst` | Reset (-1 if not wired) |
| `display_bl` | Backlight PWM or GPIO |
| `display_madctl` | MADCTL override byte (orientation fix — optional) |

### Display — QSPI (AXS15231B)

| Key | Description |
|-----|-------------|
| `display_cs` | Chip select |
| `display_pclk` | Pixel clock |
| `display_d0` – `display_d3` | 4-bit data lines |
| `display_bl` | Backlight |
| `display_rst` | Reset (-1 if not wired) |

### Touch — I2C

| Key | Description |
|-----|-------------|
| `touch_sda` | I2C data |
| `touch_scl` | I2C clock |
| `touch_int` | Interrupt (-1 if unused) |
| `touch_rst` | Reset (-1 if not wired) |

### Touch — SPI (XPT2046)

| Key | Description |
|-----|-------------|
| `touch_cs` | Chip select (shares bus with display) |
| `touch_miso` | SPI MISO |
| `touch_mosi` | SPI MOSI (shared) |
| `touch_sclk` | SPI clock (shared) |
| `touch_irq` | Touch interrupt |

### Input — Trackball

| Key | Description |
|-----|-------------|
| `trackball_up` | Up GPIO |
| `trackball_down` | Down GPIO |
| `trackball_left` | Left GPIO |
| `trackball_right` | Right GPIO |
| `trackball_click` | Click GPIO |

### Input — BBQ20 Keyboard (T-Deck Plus)

BBQ20 is at I2C address 0x55, shared bus with GT911. No separate pin keys — uses `touch_sda` and `touch_scl`. Polled via `Wire.requestFrom(0x55, 1)` every ~20 ms.

### Radio — LoRa (SX1262 / SX1276)

| Key | Description |
|-----|-------------|
| `lora_mosi` | SPI MOSI |
| `lora_miso` | SPI MISO |
| `lora_sclk` | SPI clock |
| `lora_cs` | Chip select |
| `lora_rst` | Reset |
| `lora_irq` | DIO0/IRQ interrupt |
| `lora_busy` | BUSY pin (SX1262 only) |

### GPS — UART

| Key | Description |
|-----|-------------|
| `gps_tx` | GPS TX → ESP RX |
| `gps_rx` | ESP TX → GPS RX |

### SD Card — SPI

| Key | Description |
|-----|-------------|
| `sd_cs` | Chip select |
| `sd_mosi` | SPI MOSI |
| `sd_miso` | SPI MISO |
| `sd_sclk` | SPI clock |
| `sd_enabled` | `true` to mount SD at boot |

### T-Deck Plus Complete Pin Reference

| Signal | GPIO | Notes |
|--------|------|-------|
| Display CS | 12 | SPI |
| Display DC | 11 | SPI |
| Display MOSI | 41 | SPI |
| Display SCLK | 40 | SPI |
| Display RST | -1 | Not wired |
| Display BL | 42 | Backlight |
| I2C SDA | 18 | Shared: GT911 touch + BBQ20 keyboard |
| I2C SCL | 8 | Shared: GT911 touch + BBQ20 keyboard |
| Touch INT | 16 | GT911 interrupt + address-select pin |
| Touch RST | -1 | Not wired (GT911 RST=NC) |
| BOARD_POWERON | 10 | Must be HIGH before any peripheral works |
| Trackball UP | 3 | Active HIGH via ISR |
| Trackball DN | 15 | Active HIGH via ISR |
| Trackball LT | 1 | Active HIGH via ISR |
| Trackball RT | 2 | Active HIGH via ISR |
| Trackball CLK | 0 | Click button |
| GPS TX (→ ESP RX) | 43 | UART |
| GPS RX (← ESP TX) | 44 | UART |
| LoRa CS | 9 | SX1262 (`lora_cs`) |
| LoRa BUSY | 13 | SX1262 (`lora_busy`) |
| LoRa RST | 17 | `lora_rst` |
| LoRa IRQ/DIO1 | 45 | `lora_irq` |

---

*DP8 documentation pass performed by Claude Opus 5 in agentic/auto mode. Every
per-device UI backend above was verified against its `device.pcat`.*
