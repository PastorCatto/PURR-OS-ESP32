# 11. The UI stack and the three shells

## Short version

- The screen is drawn by **LVGL 9.2.2** inside one task, the **UI service**. Apps never touch LVGL; they call the **`ui` catcall** and the OS draws.
- A **shell** is the home screen, lock screen, sign-in, notification area and app list. **One official shell is compiled into each image**: Windows 10 Mobile style (`w10m`, the default) or Android 2.2 style (`android`). The iOS-style shell is planned and **not written**.
- The `ui` catcall table is at **version 5** in the header (`PurrOS/components/coreos/include/purr_catcalls.h`). Ask for the version you need with `catcall_get("ui", n)`.
- Read first: [plan7.md](../plan7.md) (shells), [plan9.md](../plan9.md) (the `ui` and `files` catcalls), and [UI/SHELL-OPTIMIZATION.md](../UI/SHELL-OPTIMIZATION.md) (how to write a fast shell).
- Most of the Android shell is compiled and was only checked headless in the VM. Little of it is verified on a device.

## What it is for

The device has a 320x240 touch screen, a keyboard and a trackball. The UI stack turns that into something people can use, and it does one more job: it lets an app be written once and look right under any shell.

The way that works: an app asks for a widget by **role** ("a button", "a list row", "a title"). The OS makes the LVGL object. The shell's **theme** dresses it. Swap the shell and every app changes look with no change to the app. This was decided in [plan7.md](../plan7.md) ("Skin layer", owner question of 2026-11-04).

## The files

| Area | File | What it is |
|---|---|---|
| Design | [UI/SPEC.md](../UI/SPEC.md) | The `ui` catcall and system UI design (draft 0.1; section 3 is marked superseded) |
| Design | [UI/SHELL-OPTIMIZATION.md](../UI/SHELL-OPTIMIZATION.md) | The guide for writing a fast shell: 13 rules, memory budget, scenarios |
| Design | [UI/DISPLAY_NOTES.md](../UI/DISPLAY_NOTES.md) | Hand-off notes of 2026-10-25 about replacing LVGL (nothing decided) |
| LVGL glue | [purr_lvgl.c](../PurrOS/main/purr_lvgl.c) | Display flush, touch and keyboard readers, tick, trackball and key hooks |
| UI task | [purr_ui_service.c](../PurrOS/main/purr_ui_service.c) | The service task that owns LVGL; `purr_ui_service_run` hops work onto it |
| Seam | [purr_shell.h](../PurrOS/main/purr_shell.h) | Neutral entry points: start, orientation, lock, notify, refresh apps |
| `ui` catcall | [purr_catrt_ui.c](../PurrOS/main/purr_catrt_ui.c) | Window host and the widget table apps call |
| Theme | [purr_w10m_theme.c](../PurrOS/main/purr_w10m_theme.c) | One theme, two skins (Windows, Froyo), roles, accent, base |
| W10M shell | [purr_w10m.c](../PurrOS/main/purr_w10m.c) | Start screen, control center, lock, sign-in, orientation |
| W10M shell | [purr_w10m_settings.c](../PurrOS/main/purr_w10m_settings.c) | Settings pages (shared by both shells, laid out per skin) |
| W10M shell | [purr_w10m_tasks.c](../PurrOS/main/purr_w10m_tasks.c) | Task manager |
| W10M shell | [purr_w10m_devportal.c](../PurrOS/main/purr_w10m_devportal.c) | Developer Portal |
| W10M shell | [purr_w10m_perm.c](../PurrOS/main/purr_w10m_perm.c) | Permission prompt |
| Android shell | [purr_android.c](../PurrOS/main/purr_android.c) | Status strip, key strip, banner, orientation, brightness |
| Android shell | [purr_android_home.c](../PurrOS/main/purr_android_home.c) | Home pages, dock, app drawer, options menu |
| Android shell | [purr_android_shade.c](../PurrOS/main/purr_android_shade.c) | Notification shade |
| Android shell | [purr_android_lock.c](../PurrOS/main/purr_android_lock.c) | Lock screen and sign-in |
| Old shells | [purr_card_shell.c](../PurrOS/main/purr_card_shell.c), [purr_ui_config.c](../PurrOS/main/purr_ui_config.c) | The card stack and the text-config "nav" screen; the fallbacks |
| Art | [purr_ui_assets.c](../PurrOS/main/purr_ui_assets.c) | Finds a picture in a pack (a raw flash partition up to 1.0.28; files in LittleFS from 1.0.29, see [chapter 12](12-picture-pack-and-fonts.md)) |
| Wallpaper | [purr_wallpaper.c](../PurrOS/main/purr_wallpaper.c) | The device-wide `display.wallpaper` choice |
| Shell registry | [purr_shell.c](../PurrOS/main/purr_shell.c) | The built-in shell, the installed shells, the choice and the way out |
| Build fragment | [sdkconfig.shell.android](../PurrOS/sdkconfig.shell.android) | Switches the Android shell on |
| Host render | [tools/lvgl_host/render.py](../tools/lvgl_host/render.py) | Draws the real pages to PNG on the host |

## How it works

### One task owns LVGL

LVGL is not thread-safe. The UI service task runs `lv_timer_handler()` and sleeps on a queue for at most 5 ms between passes. Any other task that wants to change the screen calls `purr_ui_service_run(...)`, which runs the function on the UI task. An app's `ui` calls do the same. Handlers on the UI task must be short; slow work goes to a job task and reports back.

### From a tap to the panel

1. LVGL polls the touch and keyboard readers (`touch_read_cb`, `kbd_read_cb` in `purr_lvgl.c`).
2. LVGL finds the object and runs its handler on the UI task.
3. Changes mark areas dirty; LVGL merges them.
4. LVGL redraws only the dirty areas into one 153,600-byte PSRAM buffer.
5. The flush callback sends each rectangle to the panel driver (rotating it first if the screen is rotated).

The full walk-through is section 3 of [SHELL-OPTIMIZATION.md](../UI/SHELL-OPTIMIZATION.md). The cost of an interaction is (dirty area) x (layers under it) plus the work in handlers.

### Which shell starts

- The **Android style shell is built into every image** (the one that always works). Any other shell, the Windows 10 Mobile style one first, is a **loadable module** (subtype `PURR_MOD_SHELL`) installed under `/ui/shells/<id>/` and chosen in Settings, Interface; the choice (`/ui/active-shell.conf`) takes effect at the next restart, and a shell that does not come up (two starts that never reached the screen, or a bad signature) is given up for the built-in one. Built with `purrstrap shells build`; the interface it links against is `PurrOS/main/purr_shell_exports.def`. Owner decisions, 2026-11-04 and 2026-11-06 ([plan7.md](../plan7.md), "Shells as installable items").
- `/ui/mode.conf` still exists. In `main.c` the value `nav` runs the text-config screen, the official shell runs `purr_shell_start()`, and anything else runs the card shell. The names `w10m`, `android` and `shell` all mean "the official shell".
- A shell that fails to start falls back to the text shell. The recovery profile (KittenOS) shows the recovery app instead of a shell.
- `ui.shell` is a settings key that writes `/ui/mode.conf` and takes effect on the next boot.

### Apps never see the shell

Apps call these catcalls: `ui`, `system`, `settings`, the permission prompt. `purr_w10m_priv.h` is the real contract a shell must provide (banner, busy, notifications, sheets, brightness, orientation, apps changed, and entry points for Settings, Portal and the task manager). The Android shell implements the same functions in its own files. The shared pages (Settings, Portal, task manager, permission prompt, `ui` windows, Store) are not copied.

### The `ui` catcall versions

| Version | Added | What it adds |
|---|---|---|
| 1 | OS 1.0.x (plan9) | Window title, header action, back handling, label, button, row, text area, spacer, set/get text, focus, remove |
| 2 | | `status` message in the status strip |
| 3 | OS 1.0.23 | Roles BIG / MEDIUM / MONO, button grid, one-line input, progress, slider, picture frame with `set_image` |
| 4 | OS 1.0.27 | `bubble` (a chat message) |
| 5 | OS 1.0.27 | `win_style` (light list) and `conv_row` (conversation or person row) |

Limits: 48 widgets per app, 2048 bytes in one text area. Widgets are **handles**, never pointers; a bad handle is refused. Events come back as `PURR_APPEV_UI` in the app's own queue, so no LVGL callback ever runs app code. `ui` is given only to an app whose manifest kind is `ui`.

### Two skins, one theme file

`purr_w10m_theme.c` holds both skins and the roles (`PURR_TH_BUTTON`, `PURR_TH_LIST_ROW`, `PURR_TH_TITLE_BAR` and the older ones). The accent defaults to Amber under Android (Froyo orange). `settings set theme.accent` works in both. Apps can draw on their own canvas but cannot restyle standard widgets (owner design, 2026-10-26).

### The Windows 10 Mobile shell

Ported from an MIT-licensed reference project, laid out for 320x240 landscape and also portrait. It has a start screen of tiles from the installed apps, an app list, a pull-down control center, a lock screen, a sign-in screen and a task manager (a list with an End button, built in 1.0.16 and later). Owner decisions of 2026-10-26: the navigation bar is hidden and shows only inside apps; art stays as is until the owner swaps it. The design is original apart from the reference's layout: no Microsoft imagery is added (owner, 2026-11-01).

### The Android shell

Froyo-style, for QWERTY devices. Home pages of icons with a dock, a drawer, a notification shade, a key strip (Menu, Home, Back), a Froyo lock slider and sign-in card. Home is a long press on the trackball by default. Icons are from the Android Open Source Project (Apache-2.0; see [UI/android-assets/README.md](../UI/android-assets/README.md) and [THIRD_PARTY.md](../THIRD_PARTY.md)). Hold an icon to pin or unpin it; pins are kept per user in `/home/<user>/.config/home.pins`.

## How to build and test it

The VM and device builds are covered in [chapter 18](18-emulator-and-vm.md) and the runbook. Per CLAUDE.md, do not start firmware builds unless the owner asks. Fast checks that need no device:

1. Host render of the real pages: `python3 tools/lvgl_host/render.py` (needs gcc, writes PNGs to `out/lvgl_host/`). `HOST_DEFS=-DCONFIG_PURR_SHELL_ANDROID=1 HOST_OUT=out/lvgl_host_android python3 tools/lvgl_host/render.py` renders the Froyo skin.
2. Host tests: `cd PurrOS/components/coreos/test/host && IDF_PATH=$HOME/esp/esp-idf-v5.3.5 python3 run.py statusicons` (status icon rules, 56 checks per plan7).
3. An Android build (from `PurrOS/`, background only): `cmake -B <dir> -DIDF_TARGET=esp32s3 -DSDKCONFIG=<dir>.sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.board.tdeck_plus;sdkconfig.profile.full;sdkconfig.shell.android" -G Ninja .` as recorded in plan7. The loader and KittenOS must also be built with the Android fragment, or the setup wizard keeps the Windows look.

## What goes wrong

| Symptom | Cause | Fix |
|---|---|---|
| Words instead of icons, poor-looking home | The picture pack is not on the device | `netinstall uiassets` (1.0.28 and older; from 1.0.29 the pack is a file in `root`, installed by the setup wizard's second stage), or a full image; see [chapter 12](12-picture-pack-and-fonts.md) |
| Apps will not open after drivers load | Low internal RAM: an app needs a stack plus the reserve | Read the free-RAM line in the task manager; 1.0.22 moved app stacks to PSRAM ([plan7.md](../plan7.md)) |
| Text cut instead of ending in dots (lock-screen notification) | Labels sized for the full display | Fixed in `and_note_row`, built, not yet published |
| Emoji draw as empty boxes | No emoji font yet | Part D of [showtime1.md](../showtime1.md) |
| Panic in `lv_indev_read` after an early tap (VM) | Not found; a bad indev callback pointer | Open, see [plan7.md](../plan7.md) "Third occurrence" |
| Panic in the I2C interrupt after screen-off then tap (VM) | Likely a bus shut down across screen-off | Open; see [chapter 15](15-power.md) and plan7 |
| A locked-out first run | Restarting the wizard counted as failed starts | Fixed in 1.0.23 (`oobe_firstboot_run` marks start healthy) |

## What is not finished

- **iOS-style shell:** not written (plan7: "the iOS-style shell"). The guide has a scenario for it ([SHELL-OPTIMIZATION.md](../UI/SHELL-OPTIMIZATION.md), Scenario 2).
- **Live tiles:** plan7 designed a `tile` catcall; I found no tile table in `purr_catcalls.h`, and the Android shell ignores tile updates.
- **Android drawer:** text tiles with raw app ids and no icons (found 2026-11-06). Part E of [showtime1.md](../showtime1.md) closes it.
- **W10M parity** with the Android shell (lock-screen notifications, `bubble`, a side-list sign-in) is Part E, not started.
- **Shell choice in the SDK and `purrstrap`** (a UI-select button) is not built ([plan7.md](../plan7.md)).
- **Plan and doc disagree:** [UI/SPEC.md](../UI/SPEC.md) used to say the `ui_assets` partition was 512 KB (fixed on 2026-11-06 to describe flash map v2, where there is no such partition and the pack is a file in LittleFS) and that themes are signed `.kit` packages; the SPEC itself marks the `.kit` idea superseded.
- **Keyboard and trackball focus** on Android home icons: touch only so far.
- **Not verified on a device:** the Android lock screen, sign-in and wizard skin (built into 1.0.19), and the task manager's behaviour with ten apps (1.0.22).
- Where the older plan text and the code differ on LVGL: plan7's goal paragraph says the OS is on LVGL 8.3.11, but its Status table records the upgrade to 9.2.2 as done.

Next: [12 The picture pack and fonts](12-picture-pack-and-fonts.md). See also [plan9.md](../plan9.md) for how the first sideloaded UI app (Notes) was built on `ui` v1.
