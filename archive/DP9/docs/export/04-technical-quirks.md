# Technical quirks and gotchas — MiniWin internals

Everything below was learned this session by reading MiniWin's actual
source (`MiniWin/miniwin.c` etc.), not assumed. Re-deriving these cost real
time; worth reading before touching the WinCE desktop code again.

## Z-order / touch resolution

- MiniWin is **not** a true compositor — windows paint directly into one
  shared framebuffer. "Layering" is purely paint-dispatch-order, not
  persistent per-layer buffers.
- `find_window_point_is_in()` (`miniwin.c:1127`) resolves touch by picking
  the **highest z-order window whose `window_rect` contains the point** —
  rect + z-order, not "what's visually drawn there." A window's hit-test
  footprint is exactly its current `window_rect`, regardless of what's
  actually painted on screen at that moment.
- `MW_ROOT_Z_ORDER = 0` (`miniwin.h:65`) is permanently reserved for the
  root window — no real window can ever occupy z=0. `mw_add_window()`
  always assigns `max_existing_z_order + 1`, so windows created in sequence
  get contiguous z-orders automatically (root=0, first real window=1, etc.)
  as long as nothing reassigns z-order afterward.
- `MW_MAX_WINDOW_COUNT = 20` — confirmed in
  `source/modules/miniwin/miniwin_config.h:13` (this project's own override
  of the vendored template's default of 14). Root always takes 1 slot,
  leaving **19 usable**. Relevant for the overlapping-windows feature: no
  hard need to cap open-app-window count much below that, but worth
  keeping in mind.

## Coordinate translation

- MiniWin delivers touch coordinates **already translated to be relative to
  each window's own current origin**: `client_x = touch_x -
  window.client_rect.x` (`process_touch_message()`, `miniwin.c:3279-3280`).
- Paint draw_info is similarly window-local:
  `client_draw_info.origin_x/y = client_rect.x/y`
  (`do_paint_window_client2()`).
- This means a window whose `window_rect` moves (like the resizing
  taskbar+menu window) needs ALL of its internal drawing/touch math
  converted from screen-absolute to window-local coordinates — not just the
  touch handler, the paint function too. This session's fix used a single
  `current_menu_height()` helper as the one source-of-truth offset for this
  conversion.

## Resize/reposition don't auto-repaint

- `mw_resize_window(handle, w, h)` and `mw_reposition_window(handle, x, y)`
  (`miniwin.h:1082`/`1093`, impl `miniwin.c:4542-4649`) both directly
  overwrite `window_rect`/`client_rect` immediately — touch hit-testing is
  correct right away. But **neither triggers a repaint** — "it is up to the
  user to issue a paint all message" (from the code's own doc comments).
  Call order between the two doesn't matter (each only touches 2 of 4 rect
  fields).

## Paint does NOT clear the client area first — the actual root cause of the stale-pixel bug

- MiniWin does not clear a window's client area before invoking its paint
  callback (confirmed via an existing comment already in
  `miniwin_win.c`'s `win_paint_func()`, written for a different window,
  making exactly this point).
- Any window's paint function that doesn't explicitly fill its own
  background will show through whatever was on screen before, indefinitely,
  regardless of how many "please repaint" triggers get added elsewhere in
  the code. This is NOT the same bug as "nothing told this window to
  repaint" — it's "this window's paint function doesn't touch these
  pixels at all."
- Practical rule going forward: every window's paint function should be
  self-sufficient — fill its own full current rect first, then draw
  content on top — exactly like a monolithic single-window design would
  naturally do. Don't rely on a separate, lower z-order window having
  already cleared a region.

## Visibility / z-order manipulation

- `mw_set_window_visible(handle, bool)` (`miniwin.h:1062`) — invisible
  windows are excluded from `find_window_point_is_in()`'s candidate list
  entirely (checked via the `MW_WINDOW_FLAG_IS_VISIBLE` flag).
- `mw_bring_window_to_front(handle)` / `mw_send_window_to_back(handle)`
  (`miniwin.h:1041`/`1050`) — confirmed `mw_bring_window_to_front()` raises
  a window's z-order above *any* currently open window, not just the
  system windows. Used for the lock overlay.
- `mw_paint_all()` → `do_paint_all()` (`miniwin.c:3965`) walks z-order
  **sequentially from 0 to visible_windows-1** — this assumes contiguous
  z-orders with no gaps. (Turned out not to be the actual cause of the
  stale-pixel bug, but worth knowing this assumption exists if z-order
  churn ever becomes suspect for a different bug.)

## Chrome-less full-screen windows

- No dedicated "no border" flag exists — window chrome (border/title
  bar/menu bar/scroll bars) is purely additive via
  `MW_WINDOW_FLAG_HAS_BORDER` / `_HAS_TITLE_BAR` / etc. Omitting all of them
  (passing `""` for title) makes `do_paint_window_frame2()` draw nothing,
  giving a pure client-area-only window. This is how all four WinCE system
  windows (wallpaper/icons/taskbar/lock) are built.

## Two parallel WinCE desktop implementations — keep them in sync

- `source/modules/miniwin/miniwin_wince_desktop.c` — the generic MiniWin
  `.purr` module, used by `tdeck_plus`/`tdeck`/`cyd*`/etc via
  `CONFIG_PURR_MINIWIN_DESKTOP_WINCE`. **This is what actually runs on real
  hardware.**
- `source/kernel/kernel_tdeck_plus_arduino/wince_shell.cpp` — baked into
  the Arduino kernel, used only by `tdeck_plus_arduino` via
  `CONFIG_PURR_UI_WINCE_SHELL`. **Compile-only verification target, never
  flashed** (per standing instruction to avoid the Arduino kernel).
- Critical divergence: `wince_shell.cpp`'s apps (`wince_apps.cpp`:
  About/WiFi/LoRa/Files/Restart) are hand-rolled MiniWin windows that call
  `taskbar_register()`/`taskbar_unregister()` directly (from
  `wince_taskbar.h`/`.cpp`) — they do **NOT** go through
  `purr_win_create()`/`miniwin_win.c`'s `mw_win_create()`/`mw_win_destroy()`
  at all. Any fix added to `miniwin_win.c` (like the app-close redraw hook)
  structurally cannot reach `wince_shell.cpp`'s apps. Known, accepted gap —
  not worth fixing given the Arduino kernel is deprioritized.

## `purrstrap.py` / sdkconfig caching gotcha

- A device's active UI backend is controlled purely by the one-line
  `[modules] ui = "..."` field in `device.pcat`, translated via
  `_sdkconfig_lines()`'s `UI_BACKEND_MAP` dict into
  `CONFIG_PURR_UI_BACKEND_<NAME>=y`.
- ESP-IDF only **merges new keys** into an existing cached sdkconfig — it
  never overwrites already-present values. So switching a device's UI (or
  any other `device.pcat`-driven config) requires deleting the stale
  `CoreOS/build_<device>/sdkconfig` file to force full regeneration,
  otherwise the old value silently sticks around. Bit twice this session
  (once for a Heltec Meshtastic flag, once for `tdeck_plus`'s UI switch).
- `[flash]` section entries in `device.pcat` are unrelated to UI backend
  selection — they only control optional SPIFFS-staged dynamic-load blobs
  via `cattobaked/components_manifest.cmake`. Every module/driver/app is
  always an available IDF component regardless of what's active; `[flash]`
  just controls what gets staged for runtime hot-load.
