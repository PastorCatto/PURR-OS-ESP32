# The T-Deck Plus, part by part

The goal for this board is **100 % supported**: every part has a driver you can read and rebuild. Here is where each one stands tonight. Pins marked "LilyGO" come from LilyGO's published headers and have not been checked on a real unit yet. They live in the board file as data, so correcting one is a one-line change.

| Part | Chip and wiring | Driver | State |
|---|---|---|---|
| Display | ST7789, 320 x 240, SPI, CS 12, DC 11, backlight 42 | `st7789` | works on the device |
| Touch | GT911, I2C, reset on GPIO 17 (the board file has the measured range) | `gt911` | works on the device |
| Keyboard | its own controller, I2C 0x55 | `kbd-i2c` | works on the device |
| Keyboard backlight | same controller, command `[0x01, level]` | `kbd-backlight` | works on the device |
| Battery | ADC on GPIO 4 behind a 1:2 divider | `battery-adc` | built, not yet compared with a multimeter |
| GPS | u-blox MIA-M10Q, UART, TX 43 / RX 44, 38400 baud (LilyGO) | `gnss-nmea` | written, tested against a scripted receiver |
| LoRa | SX1262, SPI shared with the display, CS 9, BUSY 13, DIO1 45 (LilyGO) | `lora-sx126x` | written, tested against a scripted chip |
| Trackball | five GPIOs (LilyGO) | `trackball-gpio` | written, tested against scripted pins; which pin is which direction is to verify |
| SD card | SPI shared, CS 39 | `sd-spi` (host-tested; not yet run on a unit) | written |
| Speaker | I2S | none yet | template |

## The flash

16 MB. Flash map v2 (release 1.0.29; written into the board file and the tools, not yet run on a device):

| Partition | Offset | Size | What |
|---|---|---|---|
| `nvs`, `purrcfg`, `phy_init`, `bootpkg` | 0xB000 to 0x20000 | 16 KB, 8 KB, 4 KB, 56 KB | Wi-Fi data, the trust block, radio calibration, the boot package |
| `kittenos` | 0x20000 | 1.75 MB | the recovery system |
| `kernel` | 0x1E0000 | 2.125 MB | the full system |
| `loader` | 0x400000 | 1.75 MB | the recovery loader |
| `netrec` | 0x5C0000 | 4 KB | the saved network for recovery |
| `root` | 0x5C1000 | 0xA3F000 (about 10.2 MiB) | the file system: apps, drivers, settings, picture packs, themes |

Up to 1.0.28 there was also a 5 MB `ui_assets` partition and `root` was about 6 MB. A device with the old table must be flashed with a whole 16 MB image over USB (that rewrites `root`); an update over the air never changes the table.

## Things to check on the unit

- **LoRa reset.** LilyGO wires the radio's reset to GPIO 17, the same line as the touch controller's reset. The board file leaves it unset (-1) until someone confirms the wiring; sharing it means resetting the radio resets the touch controller too.
- **TCXO and RF switch.** The SX1262 on this board is believed to have a TCXO on DIO3 and an RF switch on DIO2. The board file sets `tcxo_mv = 1800` (1.8 V from DIO3) and `dio2sw = 1`; if the radio does not answer, try `tcxo_mv = 0`, or another voltage.
- **GPS baud rate.** The receiver ships at 38400. If you see garbage, it may have been reconfigured.
- **GPIO 45** (LoRa DIO1) is a strapping pin. It is fine as an input after start-up.

## Wi-Fi, Bluetooth, LTE

- **Wi-Fi** is the chip's own radio and is already in the kernel.
- **Bluetooth** is the chip's own too, but no stack is in the system yet. The `bluetooth` class is the interface for an external controller over HCI.
- **LTE** needs an external modem on a UART (the T-Deck Plus has none). The `modem` class and its template are ready for one.
