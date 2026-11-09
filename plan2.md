# plan2: display performance and display breadth

**Source of truth for this work.** Re-read after any context compaction. Update Status and
append to the Session log as work happens. Nothing here has been started in code.

Last updated: 2026-10-25. All numbers below are from reading the code and the specs, not from
measurements on hardware.

## Goals (from the owner)

- Replace or fix LVGL: it is laggy, and the T-Deck Plus has no TE (tearing-effect) pin.
- Get close to playing video on the 320x240 ST7789.
- Bring in broad display support, plug-and-play, in the project's driver model.

## What the code does today (T-Deck Plus, `tdeck_plus-full` build)

| Finding | Where |
|---|---|
| LVGL 8.3.11 | `PurrOS/dependencies.lock` |
| Build optimized for debug (`-Og`) | `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y` |
| CPU 160 MHz (S3 can run 240) | sdkconfig |
| One full-screen draw buffer in PSRAM, no second buffer | `PurrOS/main/purr_lvgl.c:78` |
| Per-pixel byte-swap loop in the flush path, CPU reading PSRAM | `Modules/drivers/st7789_module.c:189` |
| Blocking SPI send per 16-line chunk, no overlap | `send_raw` in the same loop |
| SPI at 40 MHz (a full frame is >= 31 ms on the wire) | `PurrOS/components/kernel/boards/tdeck_plus.c:34` |
| The ST7789 driver now runs as a PSRAM-loaded module, so its hot loop executes through the cache from external RAM | hypothesis, to measure |
| A known bug: LVGL's first full-screen draw renders a blank screen (a fix was tried and reverted after a backlight init crash) | `st7789_module.c` header |

## Plan A: make the current UI fast (before any library change)

1. **Measure.** Enable the LVGL perf monitor; log timestamps around render, convert and send.
2. **Cheap changes:** `-O2` (`COMPILER_OPTIMIZATION_PERF`), 240 MHz, `LV_COLOR_16_SWAP=y` and
   remove the swap loop from the LVGL path (keep a separate swapped blit, because the console
   uses the shared host-endian path), try 80 MHz SPI and watch for artifacts. Check the kernel
   image still fits its `0x1E0000` partition at `-O2`.
3. **Restructure the flush:** two partial draw buffers in internal DMA-capable RAM (for example
   320x40 pixels each), queued SPI transactions, `lv_disp_flush_ready` from the completion
   callback. Needs a driver bus ABI addition (v5): async blit with a completion callback.
4. **Card cache:** snapshot each card to an image once and animate only its position, instead of
   re-rendering widgets (and opacity/shadow) every frame.
5. **Video mode that bypasses LVGL:** pause the UI, decode MJPEG with the S3's accelerated
   decoder into two DMA buffers, push at 80 MHz. On the T-Deck the SD card shares the SPI bus
   with the display, so read ahead into PSRAM.
6. **Tearing (no TE pin):** reduce, do not expect to eliminate. Dirty rectangles; full-frame
   writes that finish within one panel refresh (about 15 ms at 80 MHz vs about 16.7 ms); the
   ST7789 frame-rate register to tune the panel's refresh; reading the scanline (command 0x45)
   if the panel's data-out is actually wired (unchecked on the T-Deck).

## LovyanGFX and MiniWin

- **Not adopted for now.** LovyanGFX wants to own the SPI bus, which conflicts with the driver
  registry and the shared display/SD/LoRa bus; it is C++, which does not fit the freestanding
  `-nostdlib` module model (it would live in the kernel image, which loses driver-as-a-file).
- **If pursued:** use LovyanGFX's panel classes with a custom bus class that forwards to the
  PURR driver-bus ABI and a light class that calls the backlight PWM.
- **Gate:** run a one-hour standalone benchmark on the T-Deck first. If LovyanGFX is not clearly
  faster than the tuned pipeline at equal settings, the speed came from the settings, not the
  library.
- MiniWin is a separate question (a window toolkit); it does not speed up the pixel pipe. It
  exists only in `PURR-OS-0.11/ui/lib_miniwin/` and `archive/DP9`'s `miniwin` module.

## Plan B: broad display support, plug-and-play

- **Data, not code, for the common case.** Most SPI TFTs (ST7789, ST7735, ILI9341/9342/9488,
  ST7796, GC9A01, HX8357) share the MIPI command set. One generic DCS module reads a **panel
  table** per panel:
  - transport and SPI mode, max clock
  - native size and per-rotation offsets and MADCTL values
  - color order, invert, pixel format
  - ID check (read-ID command 0x04, expected bytes)
  - init sequence as `(cmd, data bytes, delay)`
- New panel = new signed data file (device-config bundle). Init tables are treated as code for
  signing (bad power or gamma settings can damage a panel), so they use the existing roles.
- **Other transports need their own code:** quad-SPI panels, 8/16-bit parallel (i80), RGB
  parallel, MIPI-DSI (P4, needs IDF 5.5+), mono OLED, e-paper (the existing `epd1in54.c`).
- **Prefer ESP-IDF's `esp_lcd` for the peripheral-bound panels.** The component registry has
  panel drivers on `esp_lcd` for most of them (C, MIT/Apache, IDF's own SPI driver so they share
  the kernel's bus, async color transfer with a done callback). They need IDF, so they cannot
  be freestanding modules: a kernel-resident backend wraps them behind `purr_display_v2_t`,
  selected by the registry from the `compatible` string.
- **Auto-detect** by trying candidates and reading the ID, only where the panel's data-out is
  wired; otherwise the board profile names the panel.
- **Init data sources:** LovyanGFX, TFT_eSPI, Adafruit libraries (check each license before
  copying; not yet read).

## The white screen: root cause (found 2026-10-25 with serial diagnostics + OpenOCD/GDB)

**Symptom:** after login, LVGL starts and builds the card shell without error, then the screen
stays white (LVGL's default empty screen) and the log goes silent.

**Evidence chain:**
1. `diag3` logging in `flush_cb` (`purr_lvgl.c`): flush #1 (the white default screen, 320x240) is
   fine at 37.6 ms; there is never a flush #2.
2. A 2 s heartbeat from the esp_timer task showed `loops=2` frozen (the `ui_service` task's
   loop count), `lv_tick` advancing, `inv_p=1` (the card shell waiting to be drawn),
   `rendering=0`. So the UI service task hangs inside its 2nd `lv_timer_handler()`, before
   rendering starts.
3. OpenOCD + GDB on the live hung device (USB-JTAG, no reset needed; attach with
   `openocd -f board/esp32s3-builtin.cfg` then `xtensa-esp32s3-elf-gdb -batch purros.elf -ex
   "target extended-remote :3333" -ex "monitor halt" -ex "thread apply all bt"`) gave the stack:
   `lv_timer_handler -> lv_indev_read_timer_cb -> touch_read_cb -> purr_kernel_touch_read ->
   purr_device_read -> (GT911 module code in PSRAM) -> bus_delay_ms(ms=1107373540)`. `ms` is
   `0x42012DE4`, a code address: a 12.8-day sleep.
4. **Cause: a stale driver module.** Between bus ABI 3 and 4, `gpio_write` was inserted in the
   MIDDLE of `purr_driver_bus_table_t`, before `delay_ms`. The embedded `gt911` blob in
   `commands.c` was built against ABI 3: its `now_us()` call (no arguments) lands on the new
   table's `delay_ms()` slot with a leftover register as the argument. (Its `delay_ms(20)` lands
   on `gpio_write(20, junk)`.) LVGL's touch timer is the first thing to call it repeatedly.
   The same blobs were stale for kbd_i2c and st7789 too (payloads differ from a rebuild of the
   current sources).
5. Why it stayed hidden: the driver loader never checked a module's bus ABI
   (`purr_driver_t` had no field), and `/boot/drivers/*.cat` files were planted only if missing,
   so a fixed firmware would not even replace the stale file already on the filesystem.

**Fix (code done in the working tree, uncommitted):**
- `purr_driver_t` gets `uint32_t bus_abi`; every loadable driver sets
  `.bus_abi = PURR_DRIVER_BUS_ABI_VERSION` (gt911, kbd_i2c, st7789, hello_driver);
  `load_one_driver_file()` in `commands.c` rejects a mismatch with an error naming the module.
- Driver planting now replaces a file whose contents differ from the baked-in blob.
- Done: the four rebuilt driver modules were signed (developer key, key id 2, with the user's
  go-ahead) and re-embedded in `commands.c`; flashed to `0x20000`; card shell appears.
- The `diag3` logging is removed; `flush_cb` keeps a warning when `blit()` fails.
- A stale `/boot/drivers/hello_driver.cat` on the user's device is now rejected at every boot
  with `built for driver bus ABI 0` (the guard working). It is not in the baked-in set, so it is
  not replanted; running `testdriverload` rewrites it with the new blob, or delete the file.
- Documented as `Drivers/SPEC.md` section 4.1.
- **Measured on the working UI** (one full-screen flush): 37.4 ms at 40 MHz SPI; animation
  frames arrive about 57-59 ms apart (~17 fps). Plan A step 2 (80 MHz, no swap loop, `-O2`)
  is the next lever; the pipeline is now correct, so measuring is meaningful.

**Lesson:** any change to `purr_driver_bus_table_t` layout (or the ABI) requires rebuilding,
re-signing and re-embedding every driver blob in `commands.c`; the new `bus_abi` check makes a
missed one fail loudly at load instead of misbehaving.

## Status

| # | Item | State |
|---|---|---|
| A1 | Measure (perf monitor, timestamps) | todo |
| A2 | `-O2`, 240 MHz, color swap, 80 MHz SPI trial | todo |
| A3 | Async blit (driver ABI v5), partial internal draw buffers | todo |
| A4 | Card snapshot cache for animations | todo |
| A5 | Video mode (MJPEG, double DMA buffers) | todo |
| A6 | Tearing mitigation experiments | todo |
| A7 | The white screen on the T-Deck Plus | **FIXED and verified on the device 2026-10-25** (the user confirmed the screen works; the log shows hundreds of flushes and the service loop advancing). See "The white screen" below. It was never the display driver or LVGL's renderer. |
| B1 | Refactor `st7789_module.c` into a generic DCS module with ST7789 as the first table entry | todo |
| B2 | Panel table format; `purrstrap` bundle builder and signer; import script | todo |
| B3 | ID probing | todo |
| B4 | Transport modules / `esp_lcd` backend | todo |
| B5 | LovyanGFX benchmark (the gate above) | todo |

Order: A1, A2, A7, A3 (the ABI change helps every panel and is independent), then B1, A4, A5.
The boot chain (`plan1.md`) comes first.

## Open questions

1. Which animation or screen feels worst: card swipes, the overview, text scrolling?
2. What does "close to video" mean: frame rate, resolution, source (SD files or a Wi-Fi stream)?
3. Is the T-Deck display's data-out line connected?
4. Which boards and panels first? That decides which transports to build early (and whether
   the JC3248W535 quad-SPI panel is on the list).
5. Which PNP property matters most: swapping between known panels, loading new panel support
   from a file, or auto-detecting at boot?

## Session log

- 2026-10-25: analysis only, from the repo and specs. Plan written. `UI/DISPLAY_NOTES.md` holds
  the earlier handoff notes.
- 2026-10-25 (later): debugged the white screen live on the T-Deck Plus (`/dev/cu.usbmodem101`).
  Flashed the working-tree app to 0x20000 four times (stock bootloader and old partition table
  still on the device). Root cause: stale driver modules after a bus ABI change. Fix prepared.

### Owner verdict (2026-10-28)
Display performance is good enough on the device and passes; the A and B items stay parked. Only the notification-shade swipe is sluggish.
Real follow-up is app lifecycle (unload/freeze hidden apps, see plan5/plan6/plan10), which also frees RAM and helps that swipe.
