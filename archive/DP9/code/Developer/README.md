# PURR OS — Developer Tools

Tools in this folder are for driver developers, kernel contributors, and community builders.
They are not part of the standard firmware build pipeline (purrstrap / modulestrap / catstrap).

---

## driverstrap — Driver Template Generator

Generates a complete, ready-to-modify PURR OS driver skeleton for any supported catcall type.

### Quick start

```bash
# Interactive wizard (recommended)
python3 Developer/driverstrap/driverstrap_ui.py

# Or CLI directly
python3 Developer/driverstrap/driverstrap.py new
python3 Developer/driverstrap/driverstrap.py new display my_panel --interface spi
python3 Developer/driverstrap/driverstrap.py new touch my_touch --interface i2c --chip esp32s3
```

### What it generates

For each driver, driverstrap creates a ready-to-modify directory:

```
user_drivers/<type>/<name>/
  driver.pcat        — modulestrap manifest (type, interface, chip, versions)
  CMakeLists.txt     — IDF component definition
  <name>.h           — public configuration struct + function declarations
  <name>.c           — full catcall implementation with TODO stubs
```

The `.c` file is not empty — every catcall function is stubbed with comments showing exactly what hardware operations belong there (register writes, SPI transactions, UART reads, etc.).

### Supported driver types

| Type | Catcall | Typical hardware |
|------|---------|-----------------|
| `display` | `catcall_display_t` | ST7789, ILI9341, SSD1306, AXS15231B |
| `touch` | `catcall_touch_t` | GT911, CST816S, XPT2046 |
| `input` | `catcall_input_t` | BBQ20 keyboard, trackball, button matrix |
| `radio` | `catcall_radio_t` | SX1276, SX1262, CC1101 |
| `gps` | `catcall_gps_t` | Any NMEA UART GPS module |
| `wifi` | *(system module, no catcall)* | ESP32 built-in WiFi stack |

### Interfaces

| Interface | Used for |
|-----------|---------|
| `spi` | SPI displays, SPI touch (XPT2046), SPI radio |
| `i2c` | I2C displays (OLED), I2C touch (GT911, CST816S), I2C keyboards |
| `qspi` | QSPI displays (AXS15231B-style, 4-bit parallel via esp_lcd) |
| `parallel` | Parallel 8/16-bit panels via esp_lcd panel IO |
| `gpio` | Trackball, button arrays, GPIO-driven input |
| `uart` | GPS modules, UART-attached radio modules |
| `builtin` | ESP32 built-in silicon (WiFi) |

### Workflow after generation

1. Open `<name>.c` and fill in the `// TODO` sections
2. Check `driver.pcat` — verify chip list, version string
3. Build with modulestrap:
   ```bash
   python3 modulestrap/modulestrap.py build <name>
   ```
4. Add to your device.pcat:
   ```toml
   [drivers]
   display = "<name>"
   ```
5. Build firmware:
   ```bash
   python3 purrstrap/purrstrap.py build <device>
   ```

### Output location

By default, drivers are generated into `user_drivers/<type>/<name>/` — this is the community drop zone that modulestrap auto-scans.

Use `--core` to write into `source/drivers/` instead (for PURR OS built-in driver contributions).

---

## Adding more developer tools

Place any new developer tool in its own subdirectory here:
```
Developer/
  driverstrap/     driver template generator
  <your-tool>/     your tool here
```
