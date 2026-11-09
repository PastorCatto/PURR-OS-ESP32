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
