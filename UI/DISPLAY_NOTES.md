# Display / UI notes (handoff, 2026-10-25)

Written so the thread isn't lost if the session is. Facts here are either what the repo shows
or what the owner said; open questions are marked as such.

## What the owner wants

- **Replace LVGL** in the current UI. Reasons given: it is laggy, has too many problems, and
  the T-Deck Plus panel has **no TE (tearing-effect) pin**, so a smooth refresh rate is hard.
- Go **lighter and more optimized**, with the goal of getting **close enough to playing video**.
- The candidate idea is **LovyanGFX plus MiniWin (a window manager)**. It was "in talks" and the
  owner was toying with the idea; nothing is decided or built.

## What the repo shows

- **Current UI:** LVGL, with the card shell. Files in `PurrOS/main/`: `purr_lvgl.c`,
  `purr_card_shell.c`, `purr_ui_service.c`, `purr_ui_config.c`, `purr_ui_assets.c` (and their
  headers). Configured at runtime from `/ui/cardshell.conf` and `/ui/mode.conf`. Only built for
  the T-Deck and T-Deck Plus boards.
- **LovyanGFX:** not present anywhere in the repo or its history (checked with a file and
  commit search).
- **MiniWin:** present only in older trees. `PURR-OS-0.11/ui/lib_miniwin/` (the library plus
  per-device `miniwin_config*.h` and WCE, BlackBerry and Luna themes) and `archive/DP9`'s
  `miniwin` module (a PURR HAL: `hal_lcd.c`, `miniwin_win.c`, `miniwin_cursor.c`, a WinCE-style
  desktop). Git history has "MiniWin/WinCE rewrite" and "declarative UI-config format +
  MiniWin app platform" commits, and a commit that archived the old UI stack.
- **Display driver, in flux:** the ST7789 driver is being converted from a compiled-in kernel
  driver to a loadable module (`Modules/drivers/st7789_module.c`, uncommitted; the old
  `components/kernel/src/st7789.c` is deleted). The driver bus ABI went to v4, adding
  `gpio_write`, `dma_alloc` and a backlight PWM. **Known bug recorded in that module's header:**
  LVGL's first full-screen draw renders a stuck blank screen; a pre-callback fix was tried and
  reverted after a backlight init crash.
- **Panel and memory (T-Deck Plus):** 3.2 inch ST7789, 320x240, SPI. ESP32-S3, 16 MB flash,
  8 MB octal PSRAM at 80 MHz.
- **Other display on a supported board:** the Waveshare 1.54 inch e-paper (`epd1in54.c`) is
  still compiled into the kernel and needs a different refresh model from a color LCD.
- **Console/UI split:** `purr_console` (text terminal) is the fallback when LVGL is skipped or
  stops. Anything replacing LVGL has to keep that fallback path.

## Open questions (not answered yet)

1. Replace LVGL inside the existing card shell, or bring MiniWin back as the window manager
   on top of a new graphics layer? These are different jobs.
2. Where would the new graphics layer sit relative to the driver-bus ABI: a loadable module,
   or part of the kernel? The module system's rules (relocatable, `-nostdlib`) constrain a
   C++ library like LovyanGFX. Not checked.
3. What is the realistic path to smooth updates without a TE pin: partial-region updates,
   double buffering, DMA from PSRAM or internal RAM, SPI clock? Needs measuring on the board.
4. What does "close to video" mean in numbers: frame rate, resolution, codec or raw frames?
5. Which boards first: the T-Deck / T-Deck Plus color panel, or all of them? The e-paper board
   would keep its own path.
6. The pending blank-screen bug in the ST7789 module needs resolving or understanding before
   any graphics layer is swapped, so the two problems are not tangled.

## Session state, in case the conversation is lost

- **Nothing in this session is committed.** Work is in the working tree on branch `PURR2.0`.
- **Bootloader work done (unbuilt, untested on hardware):** `boot_fail_count` ladder spec fixed
  to match the code; handoff block written by the bootloader and read by the kernel; `bootpkg`
  partition grown to 56 KB (with `nvs` shrunk and `purrcfg` moved to `0xD000`); partition lookup
  no longer maps flash (fixes the "mmap twice" crash); `PurrOS/` and `Kernel/` now build the
  same bootloader as `bootloader/`. Full report: `BOOTLOADER_SESSION_NOTES.md` (repo root).
- **Docs written:** `SECURITY.md` (repo root).
- **Pending decision:** per-image signature checking for `kernel`, `kittenos`, `loader`. The
  proposal is a signature trailer in each partition (see the notes file above).
- **Toolchain:** ESP-IDF v5.3.5 is installed at `~/esp/esp-idf-v5.3.5`. Activate it with
  `. ~/esp/esp-idf-v5.3.5/export.sh` in each new terminal. ESP32-P4 support, if wanted, needs a
  newer IDF (5.5 or later) alongside it.
- **Hygiene:** about 90 files show line-ending churn (CRLF) from the Windows-to-Mac move;
  normalise or ignore before committing.
- **Idea, not decided:** renaming PURR's software verification "Secure Cat" to separate it from
  the hardware eFuse feature. Identifiers (`secure_mode` and its flags) would stay.
