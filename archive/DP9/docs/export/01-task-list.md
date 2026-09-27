# Task list at export time

1. **[in_progress]** Build `tdeck_plus` and `tdeck_plus_arduino` to verify
   compile clean after the taskbar/dtbtn background-fill fixes (the
   stale-pixel bug fix — see 04-technical-quirks.md for what this fix
   actually does).
2. **[pending]** Flash `tdeck_plus` and verify clean boot + no stale pixels
   on Start Menu close, app close, lock/unlock. This is the real
   verification step — the fix has been written and committed but never
   actually run on hardware.
3. **[pending]** Find the wallpaper `.webp` asset and convert it to a
   bitmap MiniWin can render, wire it in as the wallpaper. Source image
   already located: `~/Downloads/wallpaper.webp` (498×337, VP8). Still need
   to check what bitmap format/loading mechanism MiniWin's `mw_gl_*` API
   actually supports before writing a converter — MiniWin is NOT LVGL, so
   `source/assets/icons/convert_icons.py`'s LVGL `lv_img_dsc_t` output
   format does not directly apply; it's only useful as a reference for the
   general "SVG/image → C array" pattern.
4. **[pending]** Investigate how app windows are currently created
   full-screen (`purr_win_create()` / `source/modules/miniwin/miniwin_win.c`)
   to plan windowed/overlapping support. Not started — was about to read
   this when interrupted.
5. **[pending]** Design and implement movable, resizable, overlapping app
   windows (not full-screen) for the `tdeck_plus` MiniWin desktop. User's
   request verbatim: "Let's also add the feature where multiple windows can
   be opened at the same time and overlap each other, and let's stop
   opening everything in full screen. Let's open them in a window, not the
   full screen." Not designed yet.
6. **[pending]** Mirror windowed-app changes into `wince_shell.cpp` (per
   this session's established pattern of keeping the Arduino-kernel WinCE
   shell in sync with the MiniWin-module version, compile-only target,
   never flashed).
7. **[pending]** Build, flash, and verify overlapping windows + wallpaper
   behavior on device.

## Order these were requested in (most recent first is queued last)

The wallpaper and overlapping-windows requests both arrived back-to-back
while the stale-pixel fix (#1/#2) was mid-flight, so they're queued behind
it, not instead of it. Nothing about them changes priority — just be aware
the user hasn't forgotten they asked for them; picking up in list order is
correct.
