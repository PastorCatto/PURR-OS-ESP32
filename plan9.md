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

### Apps to make, and the foundations they need (owner, 2026-11-05; OS work goes into 1.0.23)
Owner's list: Clock (clock, stopwatch, timer), Calculator, a tiny terminal, Music player and Image viewer (inspired by Froyo's Music and Gallery), possibly a Video player; Meshtastic is on hold until after the public release (10 Nov).
- **Today's `ui` catcall (v2) is a column of label, button, row, textarea, spacer** (48 widgets per app): no button grid, big text, image, slider or progress bar. **`ui` v3** (planned, 1.0.23): a big-text role, a button grid, a progress bar, a slider, an image widget (LVGL's JPEG/PNG decoders), a monospace text area. The Calculator and the Clock face wait for it; the image viewer needs the image widget.
- **`system` v2** (built in the tree, ships in 1.0.23): `local_time()` fills a `purr_cc_tm_t` in the device's zone; apps ask for it with `catcall_get("system", 2)` and fall back to UTC on an older OS (`purr_catrt.c`, `purr_catcalls.h`).
- **Clock 0.1.0** (`Apps/clock/`, on the dev server's app repository): the time, a stopwatch with laps, a timer (presets or typed minutes). Runs while open; an alarm in the background needs the `power` wake alarm (later). Built and signed with the developer key.
- **Terminal** (later): an administrator only. Launched by an administrator it opens straight to a shell with administrator rights; launched by anyone else it asks an administrator to sign in first. It needs a `shell` catcall with its own permission, and a hard refusal list: no flashing (install, reinstall, bootloader, loader, slots, recovery writes), no key or trust commands (keys, trust, secure mode, dev mode, revoke). The list is code, not a setting.
- **Audio and the Music player** (later): there is no audio in the firmware yet; the T-Deck Plus board file already describes the I2S speaker. An I2S driver module, a WAV player first and MP3 after, an `audio` catcall, then the app. **Video** needs audio and an MJPEG decoder; last.

### Picking drivers one by one, and the one that stops the device (owner, 2026-11-05; in 1.0.23)
Owner: "Get all missing drivers" was all or nothing, and one driver of the set crashed the whole device with nothing saying which.
- **One at a time.** Device Manager > Drivers: every built-in device with no driver is its own tappable row ("driver x: tap to get it"); "Get all N" stays. Job `get-driver` (`devices` catcall, `a` = the driver's `compatible` name from `list("missing")`) -> `purr_hwsetup_get_one()`: reads the index and runs the same `ensure_driver()` as the all-at-once path. Devices app 1.2.0.
- **The one that stops the device finds itself.** The kernel calls `purr_driver_guard()` (`purr_driver.c`, a weak hook) around every driver's probe; `commands.c` writes the driver's file to `/boot/drivers.trying` before and removes it after. If the device dies inside the probe the file is still there next start, and `load_drivers_from()` moves that file to `/boot/drivers.crashed` before loading anything: the device comes up without it, the boot log says which, and Device Manager lists it under "stopped the device while it was starting" (`list("crashed")`); tapping a row (`retry-driver`) puts it back for the next restart, and it is set aside again if it stops the device again. A crash after probe returns (a driver's own task) is not caught; built-in (non-module) drivers cannot be set aside.
- Next suspects, by what we know: the LoRa driver shares the display's SPI bus, the SD card driver too; the GPS driver uses a UART. Install them one at a time, LoRa and SD last.
- **Drivers must not be started on a PSRAM stack (2026-11-05).** The first 1.0.23 put the Devices job (`dev-job`) on a PSRAM stack; one driver installed and the rest crashed the device. A driver's start-up runs on the loading task's stack and may hand buffers from it to the hardware (SPI, I2C and UART use DMA, which cannot read PSRAM), so the job that loads drivers keeps a fast-RAM stack (`purr_catrt_devices.c`, with the reason in the code). The Store jobs and the fetch worker run no driver code and stay on PSRAM. `purr_driver_guard` also skips its marker write on a task with under 4 KB of stack to spare (the Wi-Fi settle task has 3 KB). Not confirmed on a device: a serial capture of a crash would settle it.

### Display link (mirror, remote UI, control): moved to plan11.md (owner, 2026-11-05)


### Drivers load after sign-in (owner, 2026-11-05; built, not yet run on a device)
Owner's idea, after the driver crashes: only the drivers sign-in needs load at start, the rest once somebody has signed in (Wi-Fi is not a driver file and is unchanged). On the full system `purr_modules_setup` loads `kbd-raw` and `trackball-gpio` (`s_core_drivers`, commands.c) and `purr_session_started` (main.c) calls `purr_drivers_load_deferred()` once per start: a `drv-late` task (12 KB internal stack, the size a driver load needs) writes `/boot/drivers.settling`, loads the other files, re-binds the profile (`purr_hwsetup_boot`), and a 4 KB `drv-settle` task removes the marker after 30 s. If the marker is still there at the next start, the device stopped within 30 s of the late load: every non-core driver is set aside in `/boot/drivers.crashed` (Device Manager > Drivers offers them back one at a time). The start-up marker (`drivers.trying`) and the unsettled-start rule (boot_fail_count of 2 or more sets aside all drivers, core included) stay. KittenOS and the loader load everything at start as before (mode 0). The text-shell escape (hold space) skips sign-in and so never loads the late drivers. Until sign-in finishes, Device Manager lists devices whose drivers are still waiting as missing; that is expected. "First unlock" is the sign-in: both shells' comments say the first unlock of a boot is the sign-in screen (the lock screen only comes from the second unlock on), so for an account with a password the late drivers wait for the password. With automatic sign-in there is no unlock step, the session starts at boot, and the late load starts 5 s after it (the home screen first). A driver installed later from Device Manager loads when it is tapped, as before, and from the next start on is one of the late drivers.

### Messages and Contacts apps (owner decisions, 2026-11-05; a draft only, nothing built)
A draft with renders of the Froyo-style Messages app was made and shown to the owner (an artifact, not in the repository). Decisions so far: the services are **Meshtastic, Reticulum and MeshCore**, each an app that **announces itself as a messaging service in its manifest**; PURR OS provides the front end, and if the user picks it the service's own app is hidden and its settings are folded into Messages, otherwise the service's own app is used. **SMS is not a fallback: there is no LTE hardware.** A separate **Contacts app** is wanted. A new **`messaging` permission** and a **manifest section** for service apps are wanted. **One thread per service** (not per person); Contacts joins a person's threads. **Images and emoji are in**, not text only. Emoji and other fonts load from TTF files (see `showtime1.md`, Part D). Owner's answer (later the same day): the mesh services **can run as apps if they respect the radio** (a radio lease: ask for it, yield when asked, release when stopped), and may ship a **companion driver or system service** where an app cannot do the work. Contacts follows the Froyo Contacts layouts (letter headers, rows with a quick-action icon, a card header over data rows, Done and Discard in the editor; read from the AOSP `froyo` branch). Open and to check before building: the size and memory of each stack inside an app, and the approval for a companion system service; all three share one LoRa radio with incompatible settings, so only one can own it at a time; none of the three stacks' licences, sizes or image limits have been checked yet. Meshtastic work stays on hold until after 10 November. The apps will live in a new apps repository (see `showtime1.md`).

**Owner answers, 2026-11-05 (later), written into `Messaging/SPEC.md`:** the permissions are `messaging`, `contacts` and `radio` (the last already existed); messages are stored in a folder on flash until they use 1 MB, then, if a micro SD card is inserted, everything stored so far moves to the card and fresh messages stay on flash; the Menu key shows Channels (LongFast, General, and others), New message, Contacts, Services and Settings, and channels are separate from private threads; emoji are monochrome; a companion driver or system service may be published by anyone with a signature for now, with a trusted-publisher list later (the owner expects the first Reticulum and Meshtastic services to come from himself). Open: what happens at 1 MB with no card.

**Owner answers, 2026-11-05 (still later):** at 1 MB with no card the app warns and offers "use more device space" or "export everything, then delete the oldest" (the delete only after the export worked); a contact card shows a short history and indicators of the groups and channels the person is in and where you have talked; the radio lease ends when the user chooses to switch, after an orderly shutdown of the current app, one app at a time, and the owner wants a way to keep the radio up across the switch (a radio keeper that holds it initialised) and Reticulum running on Wi-Fi independently so that it uses both links. A **message server node** (a big SD card) with a smaller companion device over Wi-Fi is a separate later plan, recorded in `Messaging/SPEC.md` section 12.

