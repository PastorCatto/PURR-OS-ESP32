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
- **Themes belong to the system** and are plain-text config files (section 3).
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

Themes are written as config files in the Hyprland style: simple `key = value` lines with
comments, in `/etc/themes`. The user can edit them, and they **reload live** when they change.

A theme controls:

- colors: background, text, accent, borders, and the states of widgets (normal, pressed, disabled)
- fonts and sizes
- spacing and padding
- corner radius and border width
- the icon set
- animations: on or off, and speed

Rules:

- The theme is **system-wide.** Apps get the current one automatically for every widget.
- The user changes it in one place, with the `system-settings` permission.
- An app can draw its own colors on a canvas, but it **cannot restyle the standard widgets.**
- The old system's default look was a Windows CE Classic theme. Whether that stays the default is
  open.

## 4. Testing

- The catcall against fake backends: the same app calls give the same widget tree on a fake LVGL
  backend and a fake MiniWin backend.
- Events: a `ui` event lands in the same queue in the right order relative to key and pointer
  events, and the queue drops the oldest when full.
- Theme files: parsing, a bad line, a missing key falling back to the default, and a live reload.
- Rotation and different screen sizes: the size event arrives, and layouts stay inside the screen.
- The threading rule: no app call reaches LVGL outside the service.

## 5. Open questions

- **Which widgets** come first, and how much of LVGL the catcall exposes: a subset with wrappers,
  or close to all of it.
- **The canvas drawing calls.**
- **Fonts and Unicode,** since each font costs flash and RAM.
- **The icon set format.**
- **The theme file syntax** in detail: sections, variables, and how one theme handles different
  screen sizes and densities.
- **An on-screen keyboard,** which the CYD needs for typing a Wi-Fi password, and a text-entry
  component in general.
- **Memory accounting.** The widgets an app creates live in the UI service's memory. Whether they
  count toward the app's block for the out-of-memory killer
  (`../AppRuntime/SPEC.md` section 7).
- **The e-paper backend,** which needs its own refresh rules on the display catcall.
- **Gestures, screen lock, notifications** and dark mode.
- **How much MiniWin and LVGL differ,** and whether the catcall can hide it.
- **Whether the Windows CE Classic look stays the default.**
