# Network install and hardware discovery spec (draft 0.1)

How a board can build itself over Wi-Fi from a tiny first flash, and how it learns what hardware it
has. The recovery loader is in `../RecoveryLoader/SPEC.md`, KittenOS in `../KittenOS/SPEC.md`, boards
in `../Boards/SPEC.md`, and the drivers and the pin registry in
`../PurrOS/components/kernel/SPEC.md`.

## 1. The network install

**Modular boards only** (at least 4 MB flash, 2 MB PSRAM and MTP), because it needs KittenOS's Wi-Fi.

1. **Flash once by wire:** the bootloader, the partition table and the recovery loader. The ESP32's
   ROM cannot boot from a network, so this step always needs a cable. Every board of a chip family
   gets the same loader.
2. **The recovery loader** connects to Wi-Fi and downloads the newest signed KittenOS.
3. **KittenOS,** which has Wi-Fi on these boards, downloads the rest into the root filesystem: the
   kernel, CoreOS, AppManager, the runtimes, the boot package and the board's driver pack
   (`../Drivers/SPEC.md`). It formats
   the filesystem and boots normally.

Everything is verified by signature. The recovery manifest (`../OTA/SPEC.md` section 5) can list an **install profile** per board,
so one plan builds the whole system. The same path recovers a device that has been bricked.

**Catches**

- **Wi-Fi details on first boot,** either written when flashing or typed over the serial console.
- **The clock.** HTTPS checks certificate dates and a fresh device does not know the time. The loader
  uses its own build date as a floor for that first connection, and the signed images protect the
  content anyway (`../Keys/SPEC.md` section 5).
- **Loadable drivers.** A driver pack that loads on top of a generic kernel needs the module loading
  from the spike to work (`../ModuleSpike/SPEC.md`). Until then the fallback is a per-board kernel file
  with its drivers built in, downloaded the same way.

## 2. Knowing which board it is

- **A supported board has its codename built into the bootloader** (`../Boards/SPEC.md` section 3). The
  installer knows from the first boot which driver pack to fetch, so nothing is guessed.
- **A board with no driver pack** goes through discovery (section 3).

## 3. Discovering hardware on an unknown board

This is what makes an unrecognised board usable. It is a walk, not a scan of everything, because not
every kind of hardware can be found.

**I2C**

1. Try candidate pin pairs: the chip's usual defaults, plus the I2C pins of every known board in the
   repo's board database. A wrong guess is harmless, because it only listens and pulls the lines
   briefly.
2. Scan each candidate bus, and match every address (and ID register, where the chip has one) against
   a **device database in the repo.**
3. For each match, download the signed driver. Data-description drivers cover the common single-device
   cases, such as an OLED or a touch chip (`../PurrOS/components/kernel/SPEC.md` section 14).
4. Save the result as the board's **discovered profile** (for example `/etc/board.conf`), so the next boot
   does not scan again. The user confirms it once.

**SPI.** SPI cannot be scanned. A device only answers when its own chip-select pin is pulled low, and the
right ID command has to be sent. The big colour displays on the T-Deck Plus (ST7789) and the CYD
(ILI9341) are SPI. So the installer also **tries the SPI pin maps of known boards,** since many boards
share a wiring layout, and pulls the display driver the same way if one works.

**UART.** Known UART pins can be opened at common baud rates and listened to. A GPS announces itself
with NMEA text. *(Proposed.)*

**The safety rule.** Probes only use pins that appear in a known board's map, start them as inputs, and
**ask the user before driving any output** on wiring that has not been confirmed. Driving an unknown
pin can damage hardware.

**What discovery cannot find.** Parts wired straight to a pin with no bus, and SPI devices without a
matching known map. Some boards also **power their peripherals through an enable pin.** Until that pin is
set, the display and sensors are invisible, so a scan finds nothing. The database needs those enable pins
for known boards.

## 4. Shell tools

- `i2cscan`: lists every address that answers, with a best-guess chip name from the table. **Unknown
  devices are listed and marked unknown.** An optional verbose flag reads a few common ID registers
  from unknown devices and prints the raw bytes, read-only and never writing, so the chip can be
  identified and added to the table. Reads can rarely have side effects, so it is opt-in. *(Verbose
  mode proposed.)*
- `spiprobe`: probes a bus with the default candidate chip-select pins from known profiles. Root may
  **name their own pins,** with a warning that driving the wrong pin can damage hardware.
- Both scan only buses and pins that the board profile or the user names.

## 5. The fallback

If discovery finds nothing usable, the installer falls back to a **serial-only console install:** a
kernel with no display or keyboard drivers, so the board still boots to a shell over the USB serial
console. From there the tools above can teach it its hardware. *(Proposed.)*

## 6. Testing

- Fake I2C and SPI buses with fake devices: the scan, the database match, unknown devices, and the
  verbose reads that never write.
- The safety rule: a probe never drives a pin outside a known map without confirmation.
- The network install against a fake network: each step, a dropped connection and resume, a bad
  signature, and a board with no pack.
- The discovered profile is saved, and used on the next boot instead of scanning again.
- The install profile chooses the right components for each board.

## 7. Open questions

- **The device database:** its format, how it is hosted, versioned and signed, and how it is built up.
- **Power enable pins** for known boards, so discovery works on boards that gate their peripherals.
- **Sharing a discovered profile** back to the repo, so the next person with that board gets a driver pack.
- **The SPI probe's command set** and how it chooses candidate chip-select pins.
- **The install profile format** in the recovery manifest.
- **What a user sees during the install,** which is text on the serial console at first.
- **Whether the serial-only fallback is the right last step,** and how a board leaves it.
