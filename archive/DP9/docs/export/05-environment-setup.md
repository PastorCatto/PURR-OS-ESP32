# Environment / toolchain setup

Captured from this machine at export time so a fresh install can be
reproduced without guesswork.

## ESP-IDF

- Version installed: **v5.3.5**
- Install location on this machine: `~/esp/esp-idf` (also mirrored at
  `~/.espressif/v5.3.5/esp-idf`)
- Standard ESP-IDF install process applies on a new machine:
  `git clone -b v5.3.5 --recursive https://github.com/espressif/esp-idf.git`
  then run `./install.sh esp32,esp32s3` from inside it, then `source
  export.sh` in every new shell before building (or add to shell profile).
- This project's build tooling (`purrstrap`) shells out to the standard IDF
  `idf.py` under the hood — confirm `idf.py --version` reports 5.3.5 after
  reinstalling before trusting a build.

## Python

- System Python: 3.12.13 (`/usr/bin/python3`)
- Packages needed by various scripts in this repo — **none were found
  installed globally on this machine** at export time (`Pillow`,
  `cairosvg`, `requests`, `esptool` all reported not-found via `pip3
  show`). They're likely provided by the ESP-IDF venv created by
  `install.sh`/`export.sh` rather than the system Python — after setting up
  IDF on the new machine and sourcing its `export.sh`, re-check with `pip
  list` inside that environment before assuming a fresh `pip install` is
  needed. Scripts that need these:
  - `source/assets/icons/convert_icons.py` — needs `requests`, `Pillow`,
    `cairosvg` (MDI SVG → LVGL C array icon pipeline).
  - `source/assets/wallpapers/convert_wallpaper.py` — needs `Pillow` only
    (Cupcake UI's raw RGB565 wallpaper converter — NOT applicable to the
    new MiniWin wallpaper-bitmap work, which uses a different UI backend
    entirely; see 03-session-summary.md's queued task #2).

## Device / hardware

- Target device for this session: **T-Deck Plus** (LilyGo), ESP32-S3,
  16MB flash, 8MB PSRAM, ST7789 display, GT911 touch, trackball,
  SX1262 LoRa radio via RadioLib, BBQ20 keyboard.
- Serial port on this machine: **`/dev/ttyACM0`** (owned by group
  `dialout` — on a fresh Linux install, the user account needs to be added
  to the `dialout` group, or `udev` rules adjusted, before flashing works
  without sudo).
- Device profile source of truth: `source/devices/tdeck_plus/device.pcat`
  — full pin mapping, driver selection, and module/app manifest lives
  there. Nothing hardware-specific needs to be re-derived; it's all in that
  file (already tracked in git, travels with the zip).

## purrstrap workflow (this project's build CLI)

```
python3 purrstrap/purrstrap.py build tdeck_plus      # build only
python3 purrstrap/purrstrap.py flash tdeck_plus      # build + flash
python3 purrstrap/purrstrap.py monitor tdeck_plus    # serial monitor
python3 purrstrap/purrstrap.py generate tdeck_plus   # regen sdkconfig from device.pcat
python3 purrstrap/purrstrap.py doctor                # environment health check
python3 purrstrap/purrstrap.py status                # show workspace config
```

`doctor` is the fastest way to confirm a fresh machine is set up correctly
before resuming any work — run it first after reinstalling.

## Local, gitignored config (NOT in the zip's git history, but IS on disk
## so it WILL travel with a full-folder zip)

- `.purrstrap` (repo root, JSON) — current workspace state:
  ```json
  {
    "device": "tdeck_plus",
    "chip": "esp32s3",
    "purros_version": "1.0.0-dp5",
    "kitt_version": "1.0.0",
    "last_build": "2026-07-11T13:26:21.493839"
  }
  ```
- `Builder/purr_build.cfg`, `Builder/purr_sdk.cfg`, `SDK/purr_sdk.cfg` —
  all empty/unused on this machine at export time, nothing to preserve.
- `CoreOS/build_<device>/sdkconfig` files (one per device, IDF-generated
  cache) — gitignored, regenerated automatically by `purrstrap generate` or
  the first build. If a UI backend or other `device.pcat`-driven setting
  seems "stuck" after editing `device.pcat` post-reinstall, delete the
  relevant `CoreOS/build_<device>/sdkconfig` file and rebuild — see
  04-technical-quirks.md's sdkconfig-caching note.

## Git remote

- `origin` → `https://github.com/PastorCatto/PURR-OS-ESP32.git`
- At export time, local `main` was **1 commit ahead of `origin/main`**
  (commit `9dd5db25`, not pushed). The zip captures this local commit
  regardless since it's full git history, not just working-tree files —
  but if this repo is ever cloned fresh from GitHub instead of unzipped
  from this machine, that commit won't be there until pushed.
