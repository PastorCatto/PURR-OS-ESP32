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

## 3. Themes

A theme is a single signed `.kit` package -- a PURR container holding colors, fonts, spacing,
corner radius, animations, an icon set and images -- not a text config file. **A theme is data,
not code**: the UI engine only ever reads a `.kit`, it never executes one.

**Where it lives:**

- The user-facing copy sits in LittleFS like any other file (fixed path TBD, e.g.
  `/etc/themes/active.kit`). This is what the user drops in or swaps out.
- The system also carves out a dedicated **`themes` partition** -- raw flash, not LittleFS --
  sized to the device's tier (500KB on smaller-PSRAM boards, 1MB on 8MB+ boards). That partition
  *is* the flash-storage budget for a theme; there's no separate number to track.
- At runtime the UI engine reads assets straight out of the `themes` partition (memory-mapped raw
  flash), not out of LittleFS and not into PSRAM. This is what makes a theme execute-in-place:
  LittleFS files aren't guaranteed to sit in contiguous flash blocks, so a dedicated partition is
  the only way to read a theme's assets directly without paying a PSRAM cost for it.

**Boot-time sync.** Once, at boot, the system compares the LittleFS `.kit`'s header -- which
carries a signature/hash, the same convention as modules, kernelmods and the bootpkg -- against
what's currently burned into the `themes` partition.

- Match: the partition is left alone; no flash write.
- Mismatch: the LittleFS file is verified (the same signature check everything else PURR signs
  goes through), then burned whole into the `themes` partition, replacing what was there.
- No `.kit` in LittleFS, or it fails verification: whatever's already burned in the partition
  stays active. A device with nothing burned yet falls back to a built-in default theme baked
  into the firmware image itself (not going through the partition at all), so there's always
  something to render.

**Applying a new theme takes a reboot,** the same shape as the kernel/kittenos fallback chain
(`../bootloader/SPEC.md`). Drop the new `.kit` into LittleFS and reboot; there's no apply-now path
that hot-swaps the UI service while it's running.

Rules:

- The theme is **system-wide.** Apps get the current one automatically for every widget.
- The user changes it in one place, with the `system-settings` permission (replace the file, then
  reboot).
- An app can draw its own colors on a canvas, but it **cannot restyle the standard widgets.**
- The old system's default look was a Windows CE Classic theme. Whether that stays the built-in
  default is open.

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
