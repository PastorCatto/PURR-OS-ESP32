# UI spec (draft 0.1)

The `ui` catcall and the system UI behind it. The display underneath is the kernel's
(`../PurrOS/components/kernel/SPEC.md` section 8), the input events come from
`../Catcalls/SPEC.md` section 2, and the shell that runs first is in
`../AppManager/SPEC.md` section 8.1. The graphical launcher and task manager come after the
shell, built on the same services.

## 1. Decisions so far

- **Two UI libraries.** **LVGL** for PURR OS. **MiniWin** (the window manager from the old
  system) for KittenOS, for its own system screens such as the update UI and for its mini-apps.
- **The `ui` catcall hides the backend,** the way the old `purr_win.h` did. Apps call `ui`, and
  it maps to LVGL on PURR OS and to MiniWin in KittenOS. An app written once runs on either,
  and a new backend (for example for e-paper) needs no change to apps.
- **Widgets, plus a canvas.** The main interface is widgets: windows, buttons, labels, lists,
  text fields, and so on. The UI library does the drawing, layout and theming. A canvas widget
  covers custom drawing, such as games and charts.
- **UI events use the app's normal event queue.** A tap on a button arrives as an event of type
  `ui` (which widget, what happened) in the same queue as key and pointer events
  (`../Catcalls/SPEC.md` section 2). The app has one loop and one place to wait, and no callbacks
  run on the UI's own thread.
- **Themes are signed binary packages** (`.kit` files, section 3), not text config. They're read
  straight from a dedicated flash partition, never dumped into PSRAM.
- **Full-screen apps with a system status bar.** Only the foreground app is visible. Switching
  goes through the task manager.
- **Screen size and orientation are reported to the app,** with an event when they change, so it
  can re-lay-out on rotation. Widgets use flexible layouts, so most apps adapt by themselves.

## 2. How it is built

- **A UI service owns the library.** LVGL is not thread-safe, so the service runs the LVGL task
  and holds its lock. An app's `ui` calls go through the catcall into the service and are applied
  there, and never touch LVGL directly from the app's thread.
- **Drawing** goes out through the display catcall's `blit`, and pointer input comes in through the
  input catcall, as in the old KittenUI design.
- **No frame buffer** on boards without PSRAM. LVGL renders partial buffers and the display driver
  copies each one (`../PurrOS/SPEC.md` section 9).
- **The system status bar** (clock, battery, network) is drawn by the system, on top of the
  foreground app. It reads the same `system` values apps can.
- **KittenOS's own screens** use MiniWin directly, not through the catcall.

## 3. Themes and screen layout

**Superseded, 2026-10-10:** this section originally specced a theme as a single signed `.kit`
binary package (colors, fonts, spacing, icons and images all in one container, "not a text
config file"), read execute-in-place from a dedicated `themes` partition, boot-time-synced
against a LittleFS copy the same way modules/kernelmods/bootpkg are. That was designed but
never built. What got built instead, proven on real T-Deck Plus hardware the same day, is
below -- deliberately not signed, and deliberately a text format, both explicit choices once
it was clear what this actually needed to do.

**A screen is declared in a small, hand-rolled, Hyprland-flavored text config**
(`PurrOS/main/purr_ui_config.h`/`.c`), read from LittleFS (`/ui/shell.conf` is the first one,
planted with a sane default on first boot if nothing is there yet -- same missing-only
convention `purr_modules_setup()`'s auto-plant already uses). A screen is a block containing
widgets (label, button, image so far), each with position/size/color/scale and at most one
animation:

```
screen "shell" {
    background = #000000
    label "title" { text = "PURR OS"; position = 16, 16; color = #fda000; scale = 3;
                     animation = fade_in, 400, ease_out }
}
```

Unknown widget types, properties or animation names are logged and skipped, not fatal -- a
config written against a newer parser should still mostly render on an older build. A missing
or genuinely malformed config *is* treated as critical (`purr_lvgl_report_critical_error()`),
falling back to the text shell (section on fallback behavior, `PurrOS/main/main.c`).

**Images live on their own partition, separately from the config that references them** --
the one part of the original design that did carry over. `ui_assets` (512KB, carved out of
`root`; `PurrOS/partitions/tdeck.csv`/`tdeck_plus.csv`) holds raw RGB565 pixel data, built by
`purrstrap uiassets build` from a directory of PNGs (a hand-rolled decoder -- 8bpc RGB/RGBA,
non-interlaced only -- since this project has never taken a Pillow dependency), read
read-only/memory-mapped straight from flash at boot (`purr_ui_assets.c`, zero-copy, no PSRAM
cost), validated by a magic number and a CRC32 rather than a signature. A config's `image`
widget references one by name (`asset = "icon_name"`); a missing asset is logged and that one
widget is skipped, not a critical error -- missing art isn't treated the same as a missing
config.

**Why not signed, and why not binary:** nothing on this partition is executable code the way
a kernelmod, kittenos or bootpkg image is -- the existing signature/verification machinery
(`purr_verify`, the key roles) exists to stop a compromised or buggy system from smuggling
code into a boot-critical slot, a threat model that doesn't apply to "which color is the
title text." A plain text config is also directly human-editable and diffable, which a
binary `.kit` container deliberately was not.

**Still true from the original design, not yet revisited:**

- The theme/config is system-wide; apps get the current one for standard widgets (once apps
  exist at all to care -- `AppRuntime/SPEC.md`'s minimal-first scope, in progress).
- Applying a changed config happens on the next boot's `purr_ui_load_and_build()`, not a
  live hot-reload -- no apply-now path exists yet, though nothing architectural rules one out
  later (unlike the old design's "`.kit` + a flash burn," a text file is cheap to re-read on
  demand).
- Whether an app can restyle standard widgets, or only draw on its own canvas, is still open
  -- revisit once the `ui` catcall (section 2) actually exists.
- The old system's default look (Windows CE Classic) is not carried over; the current default
  (`/ui/shell.conf`'s planted content) is this project's own, not trying to match it.

## 4. Testing

- The catcall against fake backends: the same app calls give the same widget tree on a fake LVGL
  backend and a fake MiniWin backend.
- Events: a `ui` event lands in the same queue in the right order relative to key and pointer
  events, and the queue drops the oldest when full.
- Themes: `.kit` parsing, a corrupt or unsigned package being rejected, the boot-time sync
  (match leaves the partition alone, mismatch burns it, a missing/invalid LittleFS copy falls
  back to what's already burned or to the built-in default).
- Rotation and different screen sizes: the size event arrives, and layouts stay inside the screen.
- The threading rule: no app call reaches LVGL outside the service.

## 5. Open questions

- **Which widgets** come first, and how much of LVGL the catcall exposes: a subset with wrappers,
  or close to all of it.
- **The canvas drawing calls.**
- **Fonts and Unicode,** since each font costs flash and RAM.
- **The `.kit` container layout** in detail: binary format for colors, fonts, the icon set and
  images, versioning, and how one theme handles different screen sizes and densities.
- **Where the `themes` partition goes** in the partition table, and the exact LittleFS path for
  the active `.kit` file.
- **An on-screen keyboard,** which the CYD needs for typing a Wi-Fi password, and a text-entry
  component in general.
- **Memory accounting.** The widgets an app creates live in the UI service's memory. Whether they
  count toward the app's block for the out-of-memory killer
  (`../AppRuntime/SPEC.md` section 7).
- **The e-paper backend,** which needs its own refresh rules on the display catcall.
- **Gestures, screen lock, notifications** and dark mode.
- **How much MiniWin and LVGL differ,** and whether the catcall can hide it.
- **Whether the Windows CE Classic look stays the default.**
