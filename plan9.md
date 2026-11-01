# plan9 - the `ui` catcall, and Notes as the first sideloaded UI app

Status: phases 1-4 written and compile-checked 2026-10-27; not yet run in the VM (the VM was left alone on request).

## Why
Only one app ships pre-installed. Everything else is sideloaded (SDK build, then the Developer Portal). Notes was
first built into the shell by mistake and was taken back out. A sideloaded app cannot draw anything today: the
`ui` catcall does not exist (AppSDK/SPEC.md says so), and apps are freestanding callback modules with no libc.

## Done
- Built-in Notes removed from `purr_w10m.c`, `purr_w10m_priv.h` and `main/CMakeLists.txt`.
- The draft layout is kept as `Apps/notes-ui-draft/purr_w10m_notes.c.txt` (list of notes, editor, delete-twice,
  `/home/<user>/notes/NNNN.txt`). It is the visual reference for the app, not code to ship.
- Fixed this session: the text console no longer paints over the shell (`purr_console_set_muted`).

## Built (compile-checked, not yet run)
- `purr_catcalls.h`: `purr_cc_ui_t` (win_title, win_action, win_back, clear, label, button, row, textarea, spacer,
  set_text, get_text, focus, remove), roles, actions, limits (`PURR_CC_UI_MAX_WIDGETS` 48, `PURR_CC_UI_MAX_TEXT` 2048).
  `purr_app_abi.h`: `PURR_APPEV_UI`.
- `main/purr_catrt_ui.c`: window host and the table. One sheet per UI app, created on the first call, shown on resume,
  hidden on pause, deleted on unload (a hook in `teardown`). Placed beneath the lock screen and status strip by
  `w10m_place_sheet`. Re-laid out on rotation. Events go out through `purr_catrt_post_ui`.
- `main/purr_catrt.c`: `purr_catrt_set_ui`, `purr_catrt_set_storage`, `purr_catrt_caller`, `purr_catrt_post_ui`,
  `purr_catrt_close_slot`; `catcall_get("ui")` only for an app whose kind is ui.
- `main/purr_catrt_storage.c`: `storage` v1 in `/home/<user>/appdata/<app>/`; relative paths only, no `..`; whole-file
  handles (16 KB, four per app) flushed on close and on unload.
- `Apps/notes/` (notes.c, manifest.toml): the real Notes app, 2.7 KB, signed with developer key 2 and published to the
  dev repo as notes 1.1.0 (replaces the old demo of the same name).
- The shell no longer has a Notes of its own (it was a mistake: see memory apps-are-sideloaded).

## Still to do
- Run it in the VM: install Notes 1.1.0 from the portal, open it, add/edit/delete notes, check the theme follows, check
  back from the list closes it and back from the editor returns to the list. The image is ready in the scratchpad
  (`vm-debug-flash.bin`); the VM itself has not been restarted.
- Host test for the storage path rules (`rel_ok`) and for the widget handle limits.
- `purrstrap apps build` needs Python 3.11 (`tomllib`); today apps are built with `modules build --kind app`.
- The mini-library (phase 4), the on-screen keyboard question, and the pre-installed set (phase 5) are open.

## Design: `ui` catcall v1 (`purr_cc_ui_t`, in purr_catcalls.h)
- **Handles, not pointers.** The app holds small integer widget handles; the OS owns the LVGL objects. A bad handle
  is refused. A per-app cap on widgets (about 64) and on text length keeps one app from exhausting memory.
- **One window per UI app.** The shell gives the foreground UI app a themed sheet (the same one Settings and the
  portal use): header with back arrow, title and one optional action button. Background means hidden; stopping the
  app deletes everything it made (the runtime already releases what an app owns).
- **Widgets:** label (text, dim, title roles), button, text area (multi-line, max length), list row (title and
  sub-line), spacer, panel. Styling comes only from theme roles (`PURR_TH_*`), never raw colours, so accent, dark or
  light and transparency apply to apps for free.
- **Events go back to the app** as a new `PURR_APPEV_UI` (arg = widget handle, data = "click" | "changed" | "back" |
  "action"), queued to the app's own task like timers. No LVGL callback ever runs app code.
- **Threading:** every call hops to the UI service task with `purr_ui_service_run`; the UI task only posts events
  (non-blocking), so it never waits on an app.
- **Back:** the app sees "back" first; if it has nothing to go back to, it calls `app->close_self` or the shell
  sends it to the background.
- **Permissions:** `ui` is granted by manifest (`kind = "ui"`, `[catcalls] ui = "1.0"`); an app without it gets NULL.

## Phases
1. `ui` catcall in the runtime and a window host in the shell (reuse the portal's sheet code). Host test for the
   handle table and limits.
2. `storage` catcall (declared in the header, not implemented): per-app data folder, no `..`, no absolute paths.
   Notes needs it for its files.
3. Notes app in `Apps/notes/`, manifest `kind = "ui"`, built with `purrstrap apps build`, signed with developer key 2,
   published with `purrstrap dev publish`, installed from the portal in the VM.
4. A small mini-library for apps (string copy, number formatting) so the app is not hand-rolled byte loops.
5. Pre-installed set decided with the user (Settings only, plus the portal in developer mode).

## Open questions
- Which single app is pre-installed? Assumed Settings.
- Whether the on-screen keyboard belongs to the `ui` catcall (a text area asks the OS for it) or to the shell.

### Device findings (owner, 2026-10-28)
- Notes (ui catcall) works well on the device.
- BUG: installing an app while signed in as a second user shows "filesystem error" (PURR_APP_IO_ERROR from `purr_appmgr_install`).
  Installs go to `/home/<user>/apps`. Suspected cause: the second user's apps folder (or the dev-apps folder) is missing or not writable by
  that user. Not confirmed. Owner's wish: with developer mode on, sideloaded apps are present for all users.
  To decide: a shared apps root (`/apps`) for dev-mode installs, or copy-on-demand per user.

### Decided (owner, 2026-10-28)
Shared apps, separate data: sideloaded/dev-mode apps live in one shared `/apps` (visible to every user), each user's data stays under their own home. The owner will
reproduce the second-user "filesystem error" themselves; do not spend VM time on it.


## Files app and the `files` catcall (2026-10-28)
Scope chosen by the owner: A (home folder) and B (everything, read-only) in one app; C (everything, writable for admins) is a separate, later version.
- `files` catcall (`purr_catcalls.h`, `main/purr_catrt_files.c`): absolute paths in the signed-in user's view; list, stat, read (offset), mkdir, remove, rename, copy
  (whole files up to 64 KB). Two new dangerous permissions in `purr_perm`: `shared-storage` (the user's home, read and write) and `system-files` (anywhere
  else, read-only, administrators only). Asked at first use like `power` (`purr_catrt_perm_gate`, answer `PURR_APPEV_PERM`). `permissions.conf` can be read, never
  written through this. The file system's own owner checks still apply underneath.
- `Apps/files/` (kind ui, sideloaded, 4.8 KB): places (Home, System files for admins), folder list (folders first), item page (View/Open, Copy, Rename,
  Delete with a second tap), text viewer (first 2000 bytes), new folder, paste. Published to the dev server only; not built into the shell.
- No SD card: nothing mounts one yet (plan8/browser idea, shared SPI bus unverified).
- Not run on a device or in the VM yet.

### Files app: the "going back" hard lock, 2026-10-29 (owner report), and what I changed
Not reproduced (the VM was left alone): found by reading. Real defects fixed in the runtime, any of which can freeze the UI: (1) `teardown` destroyed the app's window **before** stopping the app, so a late event
(a second Back tap, a permission answer) made the dying app call `ui->...` and create a ghost window for a dead slot; apps now get no catcalls once they are closing (`purr_catrt_caller` refuses) and the window is
destroyed after the app task has stopped. (2) Settings > Apps > Permissions had labels for only 2 of 4 permissions (a NULL label for any app that had asked for files access). (3) App task stack 6144 -> 8192
(littlefs runs on the app's stack for a listing). Files 1.1.0: status messages and errors under the failing button. **Needs the owner to try it on the device**; if it still locks, `Back` at the top level (close) is the
first thing to look at.
- New catcall `devices` and the Device Manager app (`Apps/devices/`, 1.0.0), published to the dev server (unsigned).

### Files "going back" lock, round 2 (2026-10-31, owner: it still happens)
Still not reproduced (no device attached, VM left alone). Found by reading, fixed in `main/purr_catrt.c`: `purr_catrt_post_ui`, `purr_catrt_post_event` and the timer callback sent into an app's queue
without checking that the app was closing, while `teardown` deleted that queue. A second Back tap (or a late permission answer) during the close lands in that window: a send into a queue being freed
can wedge the UI task. Now every post goes through `q_post` (a mutex guards the queue's lifetime, refuses once `closing` is set), `teardown` clears `d->q` under it before deleting, and the wait for a
straggling app task before the forced delete went from 0.5 s to 2 s (a forced delete can leave a littlefs or UI lock held). Builds clean in stdmin, stdrec, stdfull. **Needs the owner to try it on the device**;
if it still locks, attach OpenOCD at the freeze (`thread apply all bt`) rather than reading further.

### The real cause of the Files and Device Manager crashes: jump tables (2026-11-01, found and fixed, verified in the VM)
Reproduced in the VM (tap through a headless run, serial log, `addr2line` on the panic). **Not** the queue race above (that was a real but separate defect). Cause: a `switch` compiles to a jump table whose
words hold addresses in the MIDDLE of a function. The module build (`purrstrap/scripts/modules.py`) relocated only words equal to a function's first byte against the executable alias; a jump-table word
went to the writable (data) alias, so the first `jx` jumped into data memory and the app died: Device Manager on its first tap (`on_click` switches on the page), Files on Back (`go_back` switches on the page),
the Store, and the `modem-at` driver. Fix: every address inside a FUNC symbol's range now counts as code (`_CodeAddresses`; test `purrstrap/tests/test_modules_relocs.py`). Rebuilt and republished: Device Manager,
Files (dev repo), the built-in Store (`purr_builtin_apps.c` regenerated, all kernels rebuilt). **Still to republish: the `modem-at` driver** (one jump table). VM result: all six Device Manager pages open, Back
closes it; Files goes folder -> item -> places -> closed; the built-in Store opens and asks for its permission. Any app or driver already signed and shipped with a `switch` of five or more dense cases needs a rebuild.
Also fixed: an app asking for a permission inside `on_create` was refused as "background" (no prompt ever appeared): `purr_catrt_slot_info` counts a starting UI app as foreground (Device Manager showed "for administrators").
Open, found in the VM: tapping a sleeping screen to wake it panicked once in the I2C interrupt handler (bus error reading the I2C FIFO register, `i2c_isr_receive_handler`); not seen on an awake screen. Probably the
emulated I2C, not the firmware: not investigated.
