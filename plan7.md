# plan7: a Windows 10 Mobile style shell for PURR OS (adapting fbiego/windows-10-mobile-lvgl)

**Source of truth for this work.** Re-read after any context compaction. Update the Status table and the
Session log as work happens. Started 2026-10-26. Depends on plan6 (the app runtime): a UI is a client of the
runtime's event stream and dispatchers, so swapping the UI does not touch apps.

## Goal

Make a second shell for PURR OS from https://github.com/fbiego/windows-10-mobile-lvgl (MIT licence): a start
screen of live tiles, an app list, a lock screen, a status bar, a navigation bar, settings and a notification
panel, running on the T-Deck Plus, driven by the PURR app runtime, and usable by apps written once for any UI.

## What the reference project is (read from a shallow clone, 2025-06-05 commit `c62690f`)

- **LVGL 9.2** (`lvgl/lvgl@^9.2`). PURR OS is on **LVGL 8.3.11** (`~8.3.0` in `PurrOS/main/idf_component.yml`).
- **Generated UI.** `src/ui/ui.c` (1659 lines) and `ui_common.c` (642 lines) are a SquareLine Studio export plus
  a small helper layer (`cm_*`: `cm_start_tile`, `cm_app_list`, `cm_settings_list`, `cm_create_switch`, ...).
  `squareline/W10M.spj` is the editable project.
- **Portrait 320x480.** The panel drivers are 320x480. The T-Deck Plus is **320x240 landscape**, so every
  screen needs a landscape layout, not a scale.
- **Apps are compiled in.** `hal/esp32/core/app_manager.cpp` registers apps by function pointer, and each app
  (`hal/esp32/apps/led`, `blog`, `fbiego`, `sample`) builds its own LVGL widgets directly. That is the opposite
  of PURR's signed, UI-agnostic `.cat` apps, so apps are **not** carried over; the shell is.
- **Arduino / PlatformIO.** The hardware layer (`hal/esp32`) uses Arduino, ESP32Time, LovyanGFX and its own Wi-Fi
  and HTTP code. None of that is reusable; PURR has its own.
- **Assets are 22 MB of C arrays** (`src/ui/images`, 58 files: wallpapers, tile art, icons). That cannot ship in a
  1.9 MB kernel slot. PURR keeps images on the `ui_assets` partition as raw RGB565 (`purrstrap uiassets build`).
- **Live tiles** are animated image panels (`liveTileVertical6_Animation`, `liveTileHorizontal5_Animation`)
  registered per tile, plus tile definitions in a static `start_tile_t tiles[]` table.

## The architecture (so this is a plug-in shell, not a fork of the OS)

```
 apps (.cat, UI-agnostic)        runtime (plan6)              shells (LVGL, in the UI service)
 console / ui / tile catcalls -> manager, events, dispatchers -> card stack  |  Windows 10 Mobile shell
```

- A **shell** is a client of the runtime: it subscribes to the event stream (`openapp`, `closeapp`, `focus`,
  `state`, `killed`) and calls dispatchers (`open`, `focus`, `close`, `background`). It never reaches into apps.
- **Tiles come from installed apps**, not a compiled table: name, accent colour and icon from the app (the
  manifest, once it carries them), tile size from `/etc/apps.conf` or the app's own request.
- **Live tiles are app-driven.** An app updates its tile through a `tile` catcall (title, text, image, "flip"
  animation), or an `event` the shell maps to its tile. The shell animates; the app only says what to show.
- **An app's window is a surface** the shell gives it (the `ui` catcall: widgets and a canvas, UI/SPEC.md). A
  console-only app gets a terminal surface. Status bar and nav bar belong to the shell and read the `system`
  catcall (clock, battery, network).
- Which shell is active is a setting (`/ui/mode.conf` already exists for text-versus-card); a shell failing to
  start falls back to the text shell, as today.

## Decisions needed

1. **LVGL 9.2 or back-port to 8.3.** The OS's own LVGL code is `purr_lvgl.c` (161 lines), `purr_ui_config.c`,
   `purr_ui_service.c` (1328 lines, mostly the card shell) and `purr_card_shell.c`. If the card stack is being
   retired, **upgrading the OS to LVGL 9.2** is the cleaner path: the reference code is native 9.x, 9.x has
   better draw performance, and only the small display and tick layer needs porting. Back-porting 2,300 lines of
   generated 9.x to 8.3 is possible but repeated for every upstream change. Recommendation: upgrade to 9.2, and
   keep the card shell building behind a flag until the new shell replaces it.
2. **Landscape layout.** Portrait Windows 10 Mobile on a 320x240 landscape screen needs a designed layout
   (start screen scrolling sideways in two rows of tiles, a slim status strip, the nav bar as three keys on the
   side). Does the owner want that, or a rotated portrait mode for boards that have a portrait panel?
3. **Assets.** Re-export wallpapers and tile art at 320x240, RGB565, to the `ui_assets` partition (about 150 KB per
   full-screen image). Which wallpapers and icon set to keep, and the licence of the art (the repository is MIT
   but the Windows logos and Lumia imagery are Microsoft's, so a shipped OS should use its own art).
4. **Attribution.** MIT needs the copyright and licence text kept. A `THIRD_PARTY.md` entry, and the licence
   file alongside any copied code.

## Phases

| # | Phase | Result |
|---|---|---|
| A | Decide the LVGL version; if 9.2, port `purr_lvgl.c` and the service's display, tick and input layer; card shell behind a flag | the existing shell runs on 9.2 (screenshot in the emulator) |
| B | Shell interface: a clean seam in the UI service (create shell, surface for an app, status text), fed by the runtime's events | the card stack and a stub shell both run through it |
| C | Import and re-lay-out the start screen, app list, lock screen, status bar and nav bar for 320x240, with PURR assets and no Microsoft art | the start screen shows tiles for the installed apps, in the emulator |
| D | Live tiles: the `tile` catcall, the flip animations, an app that updates its tile | a demo app's tile changes on screen |
| E | App surfaces: the `ui` catcall (widgets, canvas, events in the app's queue), a console surface for text apps | a demo app opens in the shell and takes touch and keys |
| F | Settings, notifications, personalisation (accent colour, wallpaper) wired to real settings and the event stream | the shell is usable end to end |

## Status

| # | Item | State |
|---|---|---|
| 0 | Reference read, plan written | done |
| A | LVGL 9.2: `idf_component.yml` bumped to `~9.2.0`; `purr_lvgl.c` ported to the 9.x display, tick and indev API; image descriptor in `purr_ui_service.c` ported; the rest of the LVGL 8 code compiles through LVGL's v8 API map | **done**: the OS builds and the card shell renders on LVGL 9.2.2 in the emulator (screenshot checked) |
| B-C | Shell on the UI service (`purr_ui_service_run`), `w10m` mode in `/ui/mode.conf`, start screen with the original tiles and art laid out for landscape and portrait, status strip, orientation support | built and checked in the emulator (owner confirmed rotation and layout) |
| C | Control center: pull-down sheet, clock, quick actions (5 shown in landscape, 4 in portrait, expand button for the rest; brightness and rotate work, unsupported ones dimmed), notifications, Lock button | written; collapse/expand and Lock **not yet built** (owner asked to hold builds) |
| C | "All apps" round button at the end of the tiles, and the alphabetical app list | written, not yet built |
| C | **Sign-in screen before first unlock (BFU)**: the Windows 10 "Other user" screen (avatar, title, user name, password with arrow), hardware keyboard through an LVGL keypad, PBKDF2 check on a worker task | written, not yet built |
| C | **Lock screen after first unlock**: Windows 10 Mobile style (wallpaper, big time and date, swipe up) | written, not yet built |
| G | **Theme + Settings** (owner, 2026-10-26): one theme struct (base dark/light, accent, on-accent) the whole shell and every app widget styles from; the reference's `cm_*` helpers (switch, slider, dropdown, title, text, button, text area, accent swatch) ported onto it; Settings app ported from the reference (main list with search box, System, Personalization with accent grid, About, later Network); per-user settings file; transparency toggle (default off); tiles and app list from the installed apps (no placeholders), tap opens through the runtime | **built and bench-tested on the VM, owner: looks good** (2026-10-26); not yet flashed to the T-Deck |
| D-F | Live tile animations, tiles from the runtime's apps, app surfaces and the nav bar inside apps | todo |

Owner decisions (2026-10-26): upgrade to LVGL 9.2 (done); keep the full design but lay it out for **landscape**, with support for **multiple orientations**; the **navigation bar is hidden and only appears inside apps**; keep the original assets as they are for now (the owner will swap in replacements later).

## Sign-in and lock design (owner, 2026-10-26)

Two separate screens. **BFU (before first unlock)**: at boot the Windows 10 "Other user" sign-in screen is
the only thing reachable, and nothing is started for a user until it succeeds. **Lock screen (after first
unlock)**: the Windows 10 Mobile lock screen, reached by the Lock button in the control center (and later an
idle timeout); it is swipe-to-unlock and does not ask for the password again. That is what the owner asked for
and it is weaker than a locked phone: someone who picks up an unlocked-then-locked device only has to swipe.
A "require password to unlock" setting that reuses the sign-in screen would close that; not built.
First-time setup (no accounts yet) still uses the text prompt, then the shell starts already signed in.

`purr_login_try()` (login.c) is the non-blocking sign-in call with the text prompt's lockout rules (1, 2, 4 ...
60 s, kept in the account file). Timing note: an unknown user name returns faster than a wrong password for a
real one, as in the text prompt; worth hardening with a dummy hash.

## Theme and Settings design (owner, 2026-10-26)

The shell owns the look; apps get it. Theme struct (base, accent, on-accent by luminance) -> LVGL display
theme callback, so widgets an app creates through the `ui` catcall are W10M with no styling code. Apps may draw
on their own canvas but cannot restyle standard widgets. Settings is ported from the reference
(`cm_settings_list`, `cm_accent_color`, `cm_create_*`, `ui.c` settings pages) laid out for 320x240; pages are
wired to real things (backlight, idle timer, Wi-Fi, version/secure state, accounts); Devices and Privacy are
dimmed until the OS has them. Settings saved per user (text file, `accent=cobalt`, `base=dark`), sign-in uses
the system default. Order: struct -> helpers -> Settings main + Personalization -> System, About -> Network.

## Open issue: VM panic in the I2C interrupt (2026-10-26, found at the end of the bench test)

After ~23 min of use on the new build, tapping the Settings tile produced `LoadStorePIFAddrError` on core 0
(`PC 0x4037a183`, `A2 0x3c270004`) inside `i2c_ll_read_rxfifo` <- `i2c_isr_receive_handler` <-
`i2c_master_isr_handler_default`, interrupting the W5500 SPI task (`emac_w5500_task`). Nothing from the W10M,
theme, settings or account code is in the backtrace. Suspects: the I2C master receive buffer sits in PSRAM
(0x3c...) and an interrupt writes it; or the emulator's I2C model. Log: scratchpad `vm-serial-crash.log`.
To do: reproduce from a fresh boot with the same taps, find where the touch/keyboard driver's I2C receive
buffer is allocated, check `CONFIG_I2C_ISR_IRAM_SAFE` and any heap-in-PSRAM defaults.

## Session log

- 2026-10-26 (settings, nearly complete): self-test stripped from the code (it fought the user for the app
  manager). New `purr_settings.{h,c}`: a registry of dotted keys (`theme.*`, `display.*`, `ui.shell`, `time.*`)
  and the `settings [list] | get | set` shell command, with admin-only keys; device-wide values live in
  `/etc/system.conf`. Settings app pages now real: System (backlight, rotation, start screen, restart), Devices,
  Network (status, scan, connect with password, forget), Personalization, Apps (installed apps from the app
  manager: open, stop, remove, install from a path or http address), Accounts (add, remove, password, role;
  `login.c` enforces who may do what), Time & Language (zone, 12/24 h), About. Privacy stays dimmed (no
  permission model yet). Stalls seen in the VM coincided with UI work and sign-in; the manager now logs any
  request over 1 s and settings saves log their time. Run on the VM from a fresh flash: first-time setup into the shell, Settings pages opened, no faults or stalls in the log; owner approved. Next: flash the device; app windows and nav bar; live tiles; `storage` and `ctl` catcalls; Privacy needs a permission model.

- 2026-10-26 (theme built): `purr_w10m_theme.{h,c}` (styles, LVGL theme layer, `/etc/theme.conf` and
  `/home/<user>/.config/theme.conf`), `purr_w10m_settings.{c}` (Settings: System, Personalization, About;
  other sections dimmed), `purr_w10m_priv.h`. Shell moved onto the theme. Tiles and the app list now come
  from the signed-in user's verified apps plus the built-in Settings; refreshed at sign-in and by
  `purr_w10m_refresh_apps()`. Compile-checked only; nothing flashed, emulator untouched.

- 2026-10-26 (theme): owner asked for the UI to provide the app theme, an accent colour setting, and pointed at
  the reference's Settings app as the layout. Plan above. Starting with the theme struct.

- 2026-10-26 (even later): wrote the control center collapse/expand, Lock, the All apps button and list, the
  BFU sign-in screen and the lock screen; also `exec-boot` versus `exec-once` in the runtime (essential apps at
  boot, the rest after the first login, host-tested, 233 checks). Owner asked for no builds until told.

- 2026-10-26 (later): phase A done (see Status). Starting the shell import. Constraint found: the original assets are 22 MB of C arrays, so "keep them as is" means repackaging the same art (RGB565, unchanged look) into the assets partition or root filesystem, not compiling it into the kernel image.

- 2026-10-26: owner shared fbiego/windows-10-mobile-lvgl and plans to switch UIs. Cloned it shallowly into the
  session scratchpad (`scratchpad/ref/w10m`, not part of the repo) and read its structure. Found: LVGL 9.2 versus
  the OS's 8.3.11, a portrait 320x480 design, compiled-in apps, 22 MB of assets. Wrote this plan. Nothing built.

## Status check (2026-10-27)
Checked in the VM headless (fresh device, text account setup, then the shell): the W10M shell starts ("shell up: 320x240, 1 tiles,
accent Cobalt") and shows the status strip, the Settings tile and the "All apps" button, so those lines in the table above are no
longer "not yet built". Not seen yet because headless runs have no touch: the control center, the lock screen, the BFU sign-in
screen with an existing account, the app list. Still open in this plan: D (the `tile` catcall and live tile animations), F's
notifications and wallpaper, "require password to unlock", and the sign-in timing difference for unknown user names (dummy hash).
The `ui` catcall and window host from E exist (plan9). The I2C-interrupt panic above is probably a PSRAM buffer touched while the
flash cache was off: `bus_i2c_*` in `purr_kernel.c` now bounce any buffer outside internal RAM. Compiles; the panic took ~23 minutes
to show up, so it is not confirmed fixed.

### Pinned (owner, 2026-10-28)
Plan7 D-F stay pinned until the owner is ready; they have their own list of shell changes. Only known slow spot on device: the notification
shade swipe from the top; everything else is faster than earlier UIs. Do not start D-F unasked.
