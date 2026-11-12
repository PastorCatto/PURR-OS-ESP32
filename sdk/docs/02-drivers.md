# Writing a driver

*Current as of PURR OS 1.1 (2026-11-08).*

A driver teaches PURR OS to talk to one chip. It is a small, **freestanding** program: no standard library, no operating-system calls. It reaches the hardware only through a table of functions the OS hands it, called the **bus table**. That is why a driver cannot take the system down by touching things it should not, and why it can be built, signed, and sent to a device on its own.

## The shape of a driver

```c
static const purr_driver_t s_driver = {
    .name = "gnss-mine", .compatible = "gnss-mine",
    .probe = d_probe, .remove = d_remove,
    .bus_abi = PURR_DRIVER_BUS_ABI_VERSION,
    .class_id = PURR_DRV_CLASS_GNSS, .ops = &s_ops,
};

const purr_driver_t *gnss_mine_entry(const purr_driver_bus_table_t *bus) { s_bus = bus; return &s_driver; }
```

- **compatible** is the name a board uses to ask for it. A device in a board file with `compatible = "gnss-mine"` gets this driver.
- **probe** is called when the device is found. Check that the chip really answers, keep what you need, and return a handle. Return -1 if it is not there.
- **remove** lets go of everything.
- **class_id** and **ops** say what kind of driver it is, so the rest of the system can use it without knowing which chip it is.

## The bus table (ABI 8)

| Group | Functions |
|---|---|
| SPI | `spi_add_device`, `spi_transfer` |
| I2C | `i2c_add_device`, `i2c_write`, `i2c_read`, `i2c_write_read`, `i2c_remove_device` |
| GPIO | `gpio_set_output`, `gpio_set_input_pullup`, `gpio_get`, `gpio_write` |
| Time | `delay_ms`, `now_us` |
| Memory | `dma_alloc` |
| Backlight | `backlight_init`, `backlight_set` |
| ADC | `adc_read_mv` |
| UART | `uart_open`, `uart_write`, `uart_read`, `uart_set_baud`, `uart_close` |
| Shared SPI bus | `spi_lock`, `spi_unlock` (hold the bus across several transfers; recursive) |
| A bus of the device's own | `spi_open_bus` (ABI 8: start an SPI bus for a chip that is not on the board's shared one, then `spi_add_device` on it) |

The full list, with what each argument means, is in `PurrOS/components/kernel/include/purr_driver_bus.h`.

## Settings come from the board

A device's pins and numbers travel as a small list of named values. In the board file:

```toml
[[device]]
name = "gps0"
compatible = "gnss-nmea"
bus = "uart"
port = 1
tx = 43
rx = 44
baud = 38400
```

In the driver, read them with a fallback:

```c
int baud = purr_kv(cfg, "baud", 38400);
```

Names can be up to 7 letters, and a device can carry up to 12 of them. Anything called `tx`, `rx`, `cs`, `rst`, `busy`, `dio1` and so on is a pin, and the OS checks it against the pins the chip keeps for itself.

## Driver classes

A class is a typed interface. Every driver of a class does the same job, so the OS (and your apps) can use "the GPS" without caring which chip it is.

| Class | What it does | Interface |
|---|---|---|
| `gnss` | GPS and other satellite receivers | `purr_gnss_ops_t`: power, poll, get_fix, set_rate_hz |
| `lora` | a LoRa packet radio | `purr_lora_ops_t`: configure, send, receive, sleep |
| `modem` | a cellular (LTE, LTE-M, NB-IoT) modem | `purr_modem_ops_t`: power, status, connect, disconnect, at |
| `wifi` | Wi-Fi that is not the chip's own | `purr_wifi_ops_t`: power, scan, connect, status, sockets |
| `ethernet` | a wired network chip that moves raw frames | `purr_eth_ops_t`: start, stop, link, send_frame, recv_frame |
| `bluetooth` | a Bluetooth controller, as an HCI transport | `purr_hci_ops_t`: reset, write, read |
| `storage` (`BLOCK`) | a card or disk, as 512-byte sectors | `purr_block_ops_t`: info, read, write, sync |

They are all in `PurrOS/components/kernel/include/purr_driver_class.h`. The first field of each is a version number, so an interface can grow without breaking old drivers.

## Learning from real ones

`Modules/drivers/` has the drivers the T-Deck Plus uses:

- `st7789_module.c`, `gt911_module.c`, `kbd_i2c_module.c`, `kbd_backlight_module.c`, `battery_module.c`, `bme280_module.c`
- `gnss_nmea_module.c`: a GPS that speaks NMEA over a UART. Parses GGA, RMC and GSV with no floating point and no libc.
- `lora_sx126x_module.c`: an SX1262 over SPI.
- `modem_at_module.c`: a cellular modem over AT commands (SIMCom, Quectel, u-blox): registration, signal, operator, IMEI, a data session.
- `wifi_esp_at_module.c`: a second ESP chip running ESP-AT: scan, join, TCP sockets.
- `hci_h4_module.c`: a Bluetooth controller on a UART: HCI framing and reset.
- `w5500_module.c`: a W5500 wired Ethernet chip used as a plain MAC (class `ethernet`): it moves raw frames and the kernel's TCP/IP stack does the rest. It can sit on the board's shared bus or, with `own_bus=1`, on a bus of its own. The tests play the part of the chip (`test_drv_eth.c`).
- `sd_spi_module.c`: an SD card in SPI mode. It drives its own chip select and holds the bus lock for a whole command, and the tests play the part of the card (`FakeSdCard` in `sdk/tests/test_drivers_host.py`).

## Build, sign, hand it over

In Studio, make a driver project and press **build**. From a terminal:

```
bin/purr new driver gnss-mine --class gnss
bin/purr build gnss-mine --key developer
bin/purr serve
```

Then add a device with `compatible = "gnss-mine"` to your board file. A device in developer mode finds the server by itself and offers the driver in **Settings, Updates, Find hardware**.

## Test it before it touches a board

Because a driver only uses the bus table, you can run the real driver against a pretend bus and play the part of the hardware. The kit's tests do exactly that: `sdk/tests/driverhost.py` compiles a driver as a shared library, loads it from Python, and hands it a bus table whose functions are Python. Then a test feeds a GPS its sentences, answers a radio over SPI, plays a modem or a Wi-Fi module that replies to AT commands, and checks what the driver did.

Look at `sdk/tests/test_drivers_host.py` for a worked example of each class. To test your own driver, copy the closest one: change the source file, the entry name and what the fake says. That is how the five reference drivers were checked before any board was involved (and it found real mistakes: a two-digit year read as 2094, a TCXO voltage code that was off by one).

```
python3 -m unittest discover -s sdk/tests -t sdk/tests -p "test_drivers_host.py"
```
