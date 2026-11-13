# plan4: the VM flavor -- wired Ethernet (W5500) instead of Wi-Fi, and the emulated hardware

**Source of truth for this work.** Re-read after any context compaction. Update the Status table
and the Session log as work happens.

Started 2026-10-26. Two repositories are involved:

- `~/Projects/PURR-OS-ESP32` (this one): the PURR OS **VM flavor** of the T-Deck Plus build.
- `~/Projects/ESP32-Emulator`: QEMU (Espressif's fork, patched) plus a Rust shell. Its own Claude
  session (`esp32-emulator-9f`) built the display, keyboard and boot-menu support; its transcript is
  `~/.claude/projects/-Users-pastorcatto-Projects-ESP32-Emulator/4aca0d04-....jsonl`. Its branch is
  `vm/new-boot-chain`.

## Goal

An image of PURR OS (the full OS, KittenOS, and the **network recovery loader**) that runs in the
emulator with **no Wi-Fi at all** and a **W5500 SPI Ethernet controller** instead, so the whole
experience works there, including internet recovery. The same code is the wired backend a real
board with a W5500 module would use.

## Why

The ESP32-S3 emulation has no radio (Espressif's QEMU does not model it; the driver is a closed
blob), and the OS hangs bringing the radio up. Never starting the radio avoids that, and a wired
backend gives the network stack (lwIP, TLS, the recovery loader) a link it can actually use. The
owner wants to switch back to this session once the emulator is stable, so this session can run
and test PURR OS there itself and cross-check against real hardware.

## Decisions (from the owner)

- Stay on the **T-Deck Plus**; no separate hardware-profile layer; the NIC is added as a device.
- The Ethernet chip is the **W5500** (it needs no radio, and ESP-IDF has a driver for it).
- **Network recovery must work over the wire** ("so people can get the full experience").
- The VM flavor is signed with the published **developer keys**, not the production boot key
  (see `Keys/SPEC.md`); a way for the OS to know it is a VM is still to be designed.
- This session builds both halves: the OS side and the emulated hardware.

## Design: the OS side

- `purr_net.h` stays the one API. Two backends behind it, chosen by Kconfig (`PURR_NET_BACKEND`):
  `PURR_NET_WIFI` (default, today's `purr_net.c`, renamed `purr_net_wifi.c`) and
  `PURR_NET_ETH_W5500` (new `purr_net_eth.c`).
- New call `purr_net_wired()`: 1 when the backend is wired. Callers that ask the user for an SSID
  and password (the recovery loader, stage-2 install, the Wi-Fi shell) skip that when wired.
- Wired `purr_net_connect()` ignores the SSID and password and waits for link plus a DHCP address;
  `purr_net_scan()` finds nothing; `purr_net_status()` reports `ssid = "ethernet"`.
- The radio is never touched in the VM flavor: no `esp_wifi_init`, so no PHY bring-up, which is
  where the emulated boot stalls. (`CONFIG_ESP_WIFI_ENABLED` is a hidden IDF option and cannot be
  turned off; unreferenced Wi-Fi code is dropped by the linker instead.)
- Build: `sdkconfig.flavor.vm` overlays `sdkconfig.board.<board>` and `sdkconfig.profile.<profile>`;
  `purrstrap coreos build --flavor vm`; build directory `build/<board>-<profile>-vm`.

## The contract the emulated W5500 must meet

The OS uses IDF's own `esp_eth` W5500 driver (`esp_eth_mac_new_w5500`), so the emulated chip has
to behave like a real one at the SPI level. This is the whole contract:

| Item | Value |
|---|---|
| Bus | the T-Deck's shared SPI2 (SCK 40, MOSI 41, MISO 38), mode 0, up to 20 MHz (`PURR_ETH_SPI_MHZ`) |
| Chip select | **GPIO 9** (the slot the LoRa module uses on a real T-Deck Plus; the VM flavor has no LoRa) |
| Interrupt | **none**: `PURR_ETH_INT_GPIO = -1`, the driver polls RX every `PURR_ETH_POLL_MS` (10 ms) |
| Reset | none (`-1`); the driver resets the chip with the MR register's RST bit |
| SPI framing | W5500 variable-length mode: 16-bit address, 8-bit control (BSB[7:3], RWB[2], OM[1:0] = 00), then data until CS rises |
| Registers the IDF driver uses | VERSIONR (0x0039) reads 0x04; MR (0x0000) RST/PB; SHAR (0x0009) MAC; PHYCFGR (0x002E) link/speed/duplex bits; common IR/IMR; socket 0 registers in the socket-0 block (BSB 0x01) with TX buffer (BSB 0x02) and RX buffer (BSB 0x03) |
| Mode | socket 0 in **MACRAW**; RX/TX memory sizes set by the driver; RX frames are prefixed with a 2-byte length |
| Network side | frames go to and from QEMU's user-mode network (`-netdev user`: DHCP, DNS, NAT) |
| Link | PHYCFGR reports link up, 100 Mbit, full duplex |

## Status

| # | Item | State |
|---|---|---|
| 1 | Plan and contract | written |
| 2 | OS: Kconfig, `purr_net_eth.c`, `purr_net_wired()`, backend split | done, builds |
| 3 | OS: recovery loader, stage-2 install and shell skip Wi-Fi credentials when wired | **done and run in the VM (2026-10-27)**: loader installs KittenOS over the wire, KittenOS stage 2 installs kernel and modules, every slot verified in enforce mode |
| 4 | OS: `sdkconfig.flavor.vm`, `purrstrap coreos build --flavor vm` | done |
| 5 | OS: build the VM flavor (full, recovery, minimal); check the radio code is gone | done (full checked: no `esp_wifi_init`); standard Wi-Fi full build still builds |
| 6 | Emulator: W5500 model | done (`crates/devices/src/w5500.rs`, board `t-deck-plus-vm`), branch `vm/w5500`, uncommitted |
| 7 | Emulator: frames to and from QEMU's user-mode network | done: `dgram` netdev + two `hubport`s + `user`; QEMU patched (framed SPI phases, receive-only reads) and rebuilt |
| 8 | Run the VM image in the emulator: boot, DHCP, HTTPS, internet recovery | boots to the GUI with no hang; W5500 up through the real IDF driver, DHCP 10.0.2.15. **Since run (2026-10-27):** HTTPS download from the GitHub release, internet recovery end to end, signed slots in enforce mode |
| 10 | eFuse soft burn: the emulator backs the eFuses with a file (`ESP32_EFUSE=path`, 336 bytes, created blank), `espefuse.py --chip esp32s3 --virt --path-efuse-file` reads it; the OS logs what it reads at VM detection (secure boot, flash encryption, user data block) | wiring + OS read verified (blank file). **Burn test not run yet**: burn with espefuse while the emulator is stopped, reboot, check the log |
| 11 | VM-info device (I2C 0x6e) tells the OS it is a VM and that the Ethernet controller has its own SPI bus (SPI3, GPIO 47/48/21, CS 9); the OS opens that bus | done and verified in the window: link up, DHCP, owner reports the GUI is faster and more stable. Code: `purr_vm_info()` in `purr_kernel.c`, `purr_vm.h`, `purr_net_eth.c`; emulator `crates/devices/src/vminfo.rs`, board `t-deck-plus-vm` |
| 9 | Docs: `Network/SPEC.md` wired section (section 5b) | **done**. The cross-check against a real W5500 module is left out: no hardware here |

## The shared-bus stall (found 2026-10-26)

With the GUI drawing, the VM locks up: the W5500's PHY reads start failing ("update link duplex speed
failed") and then every task blocks. Both CPUs idle in `waiti`, the last SPI transfer (a W5500 read) had
raised its done interrupt and the guest never cleared it. The W5500 shares SPI2 with the display, as the
LoRa module does on a real T-Deck. Working theory: the emulated controller loses a completion interrupt when
a polling device and queued display transfers interleave. Mitigation: the VM board gives the W5500 its own
controller (SPI3) and the OS learns that from the VM-info device. The root cause in the controller model is
still open (real hardware shares the bus fine, so the model should too).

## Where things stand (end of 2026-10-26)

- Secure image works end to end in the emulator window: enforce-mode trust block, signed boot package, signed
  kernel/KittenOS/loader slots (boot key id 1), boot menu, kernel boot, GUI. Build it with
  `scratchpad/mkimage.sh` (needs `PURRSTRAP_KEY_PASSWORD`); the recipe is in this session's history: package-component
  kernel/loader, recovery wrap for KittenOS, `keys sign --key-id 1`, `coreos slot-image`, `keys trust-block --mode enforce`,
  then write bootloader@0, table@0xA000, purrcfg@0xF000, bootpkg@0x12000, kittenos@0x20000, kernel@0x1A0000, loader@0x380000.
- Not yet tested in the window: boot the loader slot and run network recovery over the wire; HTTPS `fetch`; the eFuse burn.
- Emulator work is on branch `vm/w5500` in `~/Projects/ESP32-Emulator`, uncommitted, including two shared-controller
  fixes (forward command/address phases; receive-only reads) and `gpspi3.vpb_controller = 3` in `qemu/apply.sh`.
  Run the window with `ESP32_EFUSE=<file>`, `ESP32_SERIAL_LOG=<file>`, `ESP32_QEMU_ARGS="-gdb tcp:127.0.0.1:3334"`.
- The root cause of the shared-bus stall in the controller model is still open (see above).

## Open questions

- How the OS learns it is a VM (the bootloader flag mentioned in the emulator chat), and the
  "verified (developer key)" display.
- Time: TLS certificate checks and the recovery loader's authenticated-time floor need a sane clock
  in the VM; check what the emulator provides.
- `wifi.cat` (a signed module) still says "wifi:" in its status line; left alone to avoid
  re-signing modules. Cosmetic.

## Session log

- 2026-10-26 (later): wired backend built; emulated W5500 + QEMU frame path working; VM image boots in
  the emulator and gets a DHCP lease. Owner asked to stop once the network works, so the HTTPS fetch and
  recovery tests are left for next. Test image: unsigned `purros.bin` at 0x1A0000, bootloader built from
  `bootloader/` (table at 0xA000). Headless runner takes `BOARD_TOML`, `HEADLESS_TYPE`, `HEADLESS_SERIAL`.
  Text shell: press space ~45 times before the login prompt, then log in (spaces must stop first).
- 2026-10-26: read the emulator chat; designed the wired backend and the W5500 contract; the owner
  asked this session to build the emulated hardware as well.

## Closing note (2026-10-27)
The VM flavor does what it was built for: wired boot, DHCP, HTTPS and the full internet recovery run in the emulator. Left open on
purpose, none of them blocking: the eFuse burn test (item 10), the root cause of the shared-bus stall in the emulated SPI
controller (the VM avoids it with its own bus), and a real W5500 module. The "(not yet run)" lines further up are history.

## Addendum (2026-11-11): a standard build also boots in the emulator
The owner wants any firmware to boot in the emulator, with the radio switched off by the emulator, not by the build. A standard (Wi-Fi) kernel booted in the emulator
stalled in `esp_wifi_init` ("phy: error: pll_cal exceeds 2ms"). Now `purr_net_init` in `purr_net_wifi.c` asks `purr_vm_info()` first: the VM-info device answering
means a VM, and unless its flags byte carries the new bit `PURR_VM_HAS_RADIO` (0x02, set by no emulator today) the Wi-Fi stack is never started; init returns
`ESP_ERR_NOT_SUPPORTED`, scan/connect say the same, status reads "not connected". The full OS goes on without a network (`purr_net_setup` only prints it); the
recovery loader stops with "this virtual device has no Wi-Fi radio: boot the VM flavor for a wired network". The VM flavor (wired W5500) is still the way to get a
network in the emulator. Applies to firmware built from this source on; a kernel built before this change (the released 1.1 included, byte for byte) still has the
stall. **Not built or run yet** (the firmware builds are the owner's to start).

## Addendum (2026-11-11, later): the W5500 as a loadable module
The owner wants the wired network to be a module the SDK can put into any image, so a standard build also has internet in the VM. Done in source, **not yet run on a
device or in the emulator**:
- **Bus ABI 8** (`spi_open_bus`) and the **ETHERNET driver class** (`purr_eth_ops_t`), see `Drivers/SPEC.md` 4.6.
- **`Modules/drivers/w5500_module.c`**: the chip's register protocol in MACRAW mode (reset, version check, buffers, MAC, OPEN, send, receive with the 2-byte length header, link
  from PHYCFGR). 4,085 bytes built for the T-Deck Plus, freestanding. Host test `test_drv_eth.c` (93 checks) against a model of the chip, including pointer wrap, bad headers and chip
  faults.
- **`purr_device_add()`** and the kernel's `eth0` declaration (`declare_vm_devices`, from the VM-info device: own bus or the shared one).
- **`purr_net_link.c`**: the kernel side, an esp_netif over the driver's frames with one task for receive and link. `purr_net_wifi.c` uses it when it runs in a VM with no radio (the
  guard added earlier today): the wire comes up the first time an Ethernet driver is bound, whether that is at start or later (`purr_net_connect`, `purr_net_status`, `purr_net_wired`
  ask again; a failed bring-up is not retried for 5 s).
- Syntax-checked with the real toolchain and flags (nothing built in `out/`); C host suite and SDK suite pass.

What stays in the kernel: the lwIP netif (a module cannot create one), declaring the device, the order drivers load in. What moved out: everything about the chip.
**Baked in (2026-11-11):** the module is signed with the developer key (id 4) and carried in `commands.c` as `s_w5500_driver_module`, in `s_temp_drivers` like the display driver, so the
recovery loader (no file system) loads it from memory and the full OS plants it into `/boot/drivers` at start. `temp_driver_wanted()` keeps it off real devices: only a VM
(`purr_vm_info()->present`) plants and loads it. It is in `BAKED` in `sdk/purrsdk/source.py`, so a build from source signs it again with the builder's own key (15 binaries now).
**Open before it can be called done:** (1) this changes the kernel, so it ships with the next kernel (1.1.x), not as 1.1; (2) the SDK must put `w5500.cat` into a VM image (and into `drivers.manifest` with abi_min 8);
(3) a run in the emulator; (4) the compile-time Ethernet backend (`purr_net_eth.c`, `PURR_NET_ETH_W5500`) stays until (1) to (3) are proven, then it can go.

## Addendum (2026-11-11, from the first Windows run of the module)
Run in the emulator on Windows with a compiled image: the wire comes up through the module (link, DHCP 10.0.2.15, NTP synced). Two bugs of mine found from the log, both fixed: (1) `link_task`
freed a received frame after handing it to `esp_netif_receive`, which frees it itself when it refuses a frame (double free, panic a second after the address arrived); (2) `purr_net_wired()`
answered "no" until the network was up, and the loader asks it first, so with no saved Wi-Fi network it went into Wi-Fi setup (the OOBE) instead of the automatic restore, even after many failed boots.
Now `purr_net_wired()` answers yes at once in a VM without a radio that has an Ethernet driver bound. Studio's system build also gained a restore address (see `sdk/NOTES-compile-from-source.md`).


## Playtime board and its gamepad driver (2026-11-12, branch `playtime`; syntax-checked, host-tested, not built into an image, not run on a unit or in an emulator)
- **Why:** the owner wants a hardware partner to try the SDK on Playtime (ESP32-S3-WROOM-1-N16R8, ST7789V 320x240, no touch, a gamepad module in an input slot). Specs and sources: the local-only notes `local/playtime-specs.md` (never committed; the board is called Playtime everywhere that ships).
- **Board:** `PurrOS/components/kernel/boards/playtime.c`, `PURR_BOARD_PLAYTIME`, `sdkconfig.board.playtime`, `partitions/playtime.csv` (the T-Deck Plus 16 MB layout), `bootpkg/boards/playtime.h`, `sdk/boards/playtime.toml`, and the board name added to purrstrap's and the SDK's board tables and to Studio's lists. A hidden `PURR_BOARD_COLOR_LCD` option replaces the 17 "T-Deck or T-Deck Plus" tests in `main/` (the UI, wizard and recovery app are built for every colour-LCD board). The boot package's power, idle-pin and backlight pins are optional now (`#ifdef`).
- **Display:** the existing ST7789 driver, with Playtime's pins (SPI3: sclk 10, mosi 11, cs 14, dc 13, rst 12; no MISO). **Unchecked on a unit:** `madctl 0xA0` (swap X/Y + mirror Y, from the reference firmware's LVGL rotation 90), RGB order with inversion, 40 MHz. No backlight pin is known, so none is driven (if the panel is dark on a real unit, that is the first suspect).
- **Gamepad:** `Modules/drivers/gamepad_mcp23017_module.c`, compatible `gamepad-mcp23017`, the board's "keyboard" device. Address 0x20, matrix, scan and active-low LEDs from the maker's library (see the spec notes). It hands out UP/DOWN/RIGHT/LEFT as LVGL's arrow keys, A as Enter, B as Esc (Back), X as `PURR_KEY_MENU` (1), Y as `LV_KEY_HOME`, FUNC as Tab; a direction repeats while held; the LED of a button lights while it is down. A module that is absent at start-up is not an error; it is looked for every half second. `purr_android.c`'s key hook handles Home and Menu (the key strip on screen needs touch).
- **Tests:** `test_drv_gamepad.c` (152 checks against a model of the MCP23017 and the diode matrix; `fakebus.h` gained an I2C fake), `PlaytimeBoard` in `sdk/tests/test_boards_templates.py`. The module builds for the device (1,893 bytes) and is baked into `commands.c` (`s_gamepad_driver_module`, planted on Playtime only) and into the SDK's re-sign list (`BAKED`). **The baked copy is unsigned** until PURR's developer key signs it; a source build re-signs it with the builder's key.
- **Not done:** the emulator's hardware definition (the owner, QEMU side); a custom UI for the board; text entry with no keyboard and no touch (the setup wizard asks for an account name and password, which needs a way to type, e.g. an on-screen keyboard driven by the d-pad); SD card on its own bus (the ABI 8 `spi_open_bus` fits), I2S audio, the IMU, battery sense (its pin is a guess in the old notes, so it is left out), the module port, the boot menu on the gamepad (the boot package never waits for a key on this board), the keyboard module (the maker will change it).
- **Addendum (2026-11-12, later):** the gamepad driver is also a gamepad-class driver (`PURR_DRV_CLASS_GAMEPAD`, `purr_gamepad_ops_t` in `purr_driver_class.h`: the raw button state and the lights; 194 host checks) for the Type shell (plan7.md), and was rebuilt (2,069 bytes) and re-baked, still unsigned. Froyo is not to be changed any more (owner's rule); its Home and Menu keys from the gamepad stay as committed in d1af6f0f.
- **Addendum (2026-11-12, the emulator and the wire):** in an emulator Playtime is told it is in a VM by the `vm-info` device at I2C 0x6e on its main bus, as every VM is (`purr_vm.h`); with the "has radio" bit unset the OS keeps Wi-Fi off. For a wired network the emulated board carries a W5500 on SPI2 with the pins and chip select 9 the SD slot has (the display has SPI3, the OS does not drive the SD slot yet, and there are only two controllers), and the `vm-info` entry names that bus (`nic_host = 2`, sclk 7, mosi 8, miso 6, cs 9). The OS then opens that bus itself and loads the baked W5500 driver (`purr_vm.h`, `declare_vm_devices`). The board file is the emulator project's (kept local there), not this repository's. **Bug found and fixed (5738a0ef):** `purr_vm_info()` skipped its probe when the board's keyboard address was 0, which Playtime sets, so a Playtime build never detected a VM and stalled starting Wi-Fi; images built before that commit (the first two scratch images) do not detect one. The W5500 on SPI2 and the whole chain are unverified until a rebuilt image runs in the emulator.
