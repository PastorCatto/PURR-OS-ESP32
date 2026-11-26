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

**Owner answers, 2026-11-05 (last):** a message export goes to the SD card; MTP support comes later (copy to a computer, and share to another device); automatic fallback stays off unless the user changes it. What is left open is only the radio keeper and Reticulum over Wi-Fi, both to be measured.

**Built 2026-11-05 (night), compiled and host-tested, not run on a device:** the message store and address book (host tests), the `messaging` and `contacts` catcalls, `ui` version 4 (`bubble`), Messages 0.1.0, Contacts 0.1.0, the Echo test service and notifications on the lock screen; all in the 1.0.27 set and on the dev server. First run in the VM headless (`ESP32-Emulator` `headless_tap` example with the VM board) is recorded below once done. Known gaps: no emoji font yet (the picker inserts the characters, which draw once a font exists), no SD export target (no SD card is mounted by the OS yet), services register at run time, not from the manifest.


## Meshtastic service (started 2026-11-25 night) — RESUME HERE

**Owner's mission (2026-11-25):** Meshtastic that is device agnostic (whatever LoRa radio the driver layer offers, same language on air), US 915 LongFast to start, near-full parity with the official client minus the map: a Meshtastic **service** plus a Meshtastic **app** with its own settings; messages forwarded into Messages; contacts saved from Messages into Contacts; private chats, private groups, several channels; a map later (needs a Maps app, parked).

**Architecture (follows `Messaging/SPEC.md`, nothing new invented):**
1. `Modules/meshtastic/purr_mt.{h,c}` — plain C protocol core, no libc, no radio: header, AES-CTR, channel hash and keys, the protobuf messages (Data, User, Position, Routing, Telemetry), node database, flood routing and acks, presets and region to `purr_lora_cfg_t`. Host-tested (`PurrOS/components/coreos/test/host/test_mt*.c`).
2. The radio is `PURR_DRV_CLASS_LORA` (`purr_lora_ops_t`): the core only asks for a modem configuration and moves frames, so any LoRa driver works. Other radio kinds join by a new class with the same two calls (configure, send/receive).
3. A `radio` catcall (app-facing, with the lease of `Messaging/SPEC.md` section 4) — TO BUILD.
4. `Apps/meshtastic` — the service app (declares `[provides.messaging]`, registers with the `messaging` catcall, settings in the manifest) — TO BUILD.
5. Messages and Contacts already exist; Meshtastic channels appear under Channels, direct messages as threads.

**Parameters worth remembering (from the firmware, verify with the host tests):** LongFast = SF11, BW 250 kHz, CR 4/5, preamble 16, sync word 0x2B (register 0x24B4), US region 902 to 928 MHz, 104 channels of 250 kHz, default slot = hash("LongFast") mod 104 (expect 19, 906.875 MHz), default PSK index 1 = the 16 byte default key. Over the air: 16 byte header (dest, sender, id, flags with hop limit and want-ack and hop start, channel hash, next hop, relay), then AES-128-CTR of a `Data` protobuf; nonce = packet id (8 bytes LE), sender (4 bytes LE), 4 zero bytes. Channel hash = xor of the name bytes xor the key bytes.

**Order and status (update as it goes):**
- [x] 1 core: header, crypto (AES-128/256 CTR), channel hash, protobuf codec, Data/User/Position/Routing — DONE 2026-11-25, `test_mt.c` 62 checks; default channel hash 8 and slot 19 = 906.875 MHz confirmed
- [x] 2 core: node database, dedupe, flood rebroadcast with SNR delay, acks and retries, presets — DONE, `purr_mt_node.c`, `test_mt_node.c` 40 checks on a simulated mesh (broadcast, direct + ack across two hops, retry, failure, private channel relay, hop limit)
- [ ] 3 core: direct messages with the public key scheme (X25519 + AES-CCM) — second tier
- [x] 4 `radio` catcall and lease — DONE (compiles, not run): `purr_catcalls.h` `purr_cc_radio_t`, `PurrOS/main/purr_catrt_radio.c`; one holder, BUSY for others, lease ends when the app stops, `purr_radio_dev_take_back()` for the services screen
- [x] 5a `Apps/meshtastic` service (daemon app, 13 KB) — DONE: `meshtastic.c`, host test `test_mt_app.c` (fake radio + fake Messages + a second node): channel and direct texts both ways, acks, failure on a bad peer, yield, lease retry. Settings come from the manifest list (`[[provides.messaging.setting]]`) drawn by Messages; reading them back from config.bin is in place, applying a change at runtime is TODO
- [x] 5b the app's own screens — DONE (host-tested with a fake `ui`, not seen on a device): home (status, nodes heard), Nodes (tap one to open its thread in Messages), Channels (tap one for its share link; leave a secondary one), Join a group (paste a link), Settings (name, region, preset, hop limit, pass-on, sealed DMs; a change restarts the node and asks the radio again). The app is now `kind = "ui"` with background on, version 0.2.0, one app for both the service and the screens
- [ ] 6 Messages / Contacts wiring (channels list, save to contacts)
- [ ] 7 on-air test with a real node (needs the owner; T-Deck Plus has an SX1262)

**Status 2026-11-25 (late night), Meshtastic:** items 1, 2, 4 and 5a are built and host-tested (`test_mt` 74, `test_mt_node` 40, `test_mt_app` 39 checks); channel share links (https://meshtastic.org/e/#...) parse and are made in the core, and the app joins a group from a `join.txt` left in its folder and writes `link.txt` with its own channels. Nothing has been on the air yet. **The T-Deck Plus LoRa driver (`lora-sx126x`) has never been verified on a device** (docs/19-boards.md): the first real test is the radio itself. Signed test files (developer key id 4): `~/p-private/mesh-test/meshtastic.cat` (the service app) and `~/p-private/mesh-test/lora-sx126x.cat` (the driver). To try it on the T-Deck Plus (needs the owner): get the driver (`hwdev get lora-sx126x` from a dev server that lists it, or plant the file in /boot/drivers), install the app, grant the radio and messaging permissions, then watch the console for "meshtastic: on the air" and send from Messages to #LongFast; a second Meshtastic node (any official device on US LongFast) should show the text and its replies should appear in Messages.

**Direct messages with public keys (2026-11-26, early):** `purr_mt_pkc.c` (X25519 checked against RFC 7748, AES-CCM against RFC 3610; `test_mt_pkc` 19 checks) and the node side (key in our node info, sealed packets received, sealed sends when the other node's key is known and `pkc_send` is on; `test_mt_node` 52 checks). The way the primitives are combined for a mesh packet (nonce, hashing the shared secret, tag and extra nonce at the end, channel hash 0) is from memory of the firmware: **sending sealed is off by default** until a real 2.5+ node confirms it (manifest setting "Sealed direct messages (experimental)"); receiving sealed is on.

**Not done yet:** position and telemetry broadcasts (GPS class exists), traceroute reply, listen-before-talk (CAD) in the driver, a screen for the app itself (nodes, channels, keys), saving a contact from Messages, the map. The settings in the manifest are read at the start; Messages applying a change live is not built.

**Status 2026-11-26 (night, later):** the app has a node page too (tap a node: its details, Message opens its thread in Messages, Save to contacts adds it to Contacts with the address "!xxxxxxxx" on the service "meshtastic"; needs the `contacts` permission). Version 0.2.1. The whole host suite passes (`python3 run.py`, all files). Signed bundle for the morning in `~/p-private/mesh-test/`: `meshtastic.cat` (developer key 4), `lora-sx126x.cat` (developer key 4), `tdeck_kernel_slot.bin` (kernel 1.2.0 with the `radio` catcall, boot key 3, flash at 0x1E0000; also carries the proximity link fixes). **Blocked on the owner from here:** flashing the T-Deck Plus, getting the LoRa driver and the app onto it, and an on-air test with a second Meshtastic node. After that, in order: fix whatever the first on-air test shows (the radio driver is the unknown), check the sealed direct message layout against a 2.5+ node and turn it on, then position and telemetry (needs a `location` catcall), listen-before-talk, saving the node list across restarts, Messages applying settings live.

**Status 2026-11-26 (overnight):** committed in four chunks on PURR2.0 (Click Wheel + media catcall; proximity link and remote display; PURR OS Mini; Meshtastic). **The push did not happen: this container has no GitHub credentials** (no gh, no SSH key, no stored login); run `git push origin PURR2.0` from a machine that has them. The T-Deck Plus was flashed (kernel slot, 2026-11-26 about 00:00) with the LoRa driver and the Meshtastic app carried in the image (planted to /boot/drivers/lora-sx126x.cat and /apps/meshtastic/package.cat); nothing could be tried on it because a person has to sign in first (the serial console does not take input once the graphical shell runs) and a second Meshtastic node is needed on the air. After sign-in the automatic put-back should load the drivers that were set aside by the earlier proximity crash; check Device Manager > Drivers.

**2026-11-26, the second node:** a Heltec WiFi LoRa 32 V3 was flashed with the official Meshtastic firmware 2.7.26 (the newest release GitHub marks stable; factory image at 0x0, the OTA loader at 0x340000, littlefs at 0x670000, all hashes verified) and set to region US with the `meshtastic` CLI (v2.7.11, in a throwaway venv): node `!f6fcdfe0`, LongFast, default key, hop limit 3. It is the other end for the first on-air test. A fresh device has region UNSET and does not transmit until a region is set.

**2026-11-26, first on-air findings and fixes:** the Heltec (official 2.7.26, node `!f6fcdfe0`) heard the T-Deck cleanly (RSSI about -24 dBm, channel hash 8, node info), so transmit, frequency, sync word and keys are right; the T-Deck heard nothing. Cause found in `lora_sx126x_module.c`: `l_receive` put the chip through standby and cleared its flags on every call and went back to standby after 60 ms, so any packet longer than 60 ms (all of them: LongFast is 0.3 s and up) was cut off or its flag was cleared before the next call. Now the chip stays in continuous receive between calls, is taken out of it only to transmit, and the boosted receive gain (register 0x08AC) is on. `test_drv_lora.c` models the chip's behaviour (a packet is heard only if receive is not interrupted) and failed before the fix. Also: the app's home and Nodes screens refresh as nodes are heard; the app now starts with "pass others' messages on" OFF (config magic MTS2, so old stored settings are dropped once); the image's post-sign-in install replaces the installed app when the carried one differs; a crash after sign-in was an old bug in the driver put-back task (it built LVGL widgets from its own task; it now uses purr_shell_notify). **One text ("hello from the heltec") was sent to everyone on the town's live LongFast channel by mistake**; the owner asked that nothing more be sent there. For tests use a private channel on both nodes (own key, own name, so a different slot), not LongFast.

**2026-11-26, receive confirmed on the T-Deck Plus:** with the continuous-receive driver the app heard 21 packets and listed 13 nodes (some with positions) in about four minutes on the town's LongFast, no receive errors, one packet sent (its own node info), pass-on off. The app writes a status line to the console every 20 s ("meshtastic: heard N, receive errors E, sent S, nodes M, radio held"). Still to check on air: texts both ways with the Heltec (use a private channel on both, not LongFast), acks, and whether a real 2.5+ node accepts a sealed direct message from us.

## The Heltec V3 as a PURR OS node, managed over the mesh (owner's priority, 2026-11-26)

**The original idea (owner):** PURR OS began as a bootloader that picks between firmwares with a small way out to handle hardware, aimed at Meshtastic nodes that are far away and hard to manage. So the goal is: a Heltec WiFi LoRa 32 V3 (ESP32-S3, 8 MB flash, **no PSRAM**, SX1262, 128 x 64 OLED, one button) runs PURR OS as its base platform, runs the Meshtastic service as an app, can take updates and drivers like the rest of the system, and a trusted person can **log in as an administrator and send commands to it over Meshtastic, in a private encrypted channel**, from anywhere on the mesh.

**Design:**
1. **Transport:** Meshtastic's private application port (PRIVATE_APP, 256) on a **private channel** with its own key (so only its members can even read the traffic), carried by the same mesh service as any message. Packets are about 200 bytes; commands and answers are cut into pieces with a short header and acknowledged (the mesh gives each piece an ack; the admin layer adds the sequence).
2. **Who may command:** a command is **signed with the administrator's key** (P-256, the same curve and code as the proximity link, `purr_px_ecc_t`) and carries a strictly increasing counter. The node holds the public key(s) of its administrators, enrolled while the two devices are together (the same close-range pairing and six digits as the link), and refuses an unsigned, wrongly signed or replayed command. Being on the private channel is not enough. A password (the PURR account's) can be asked for on top of the key for the commands that change keys or accounts. One trusted person means one enrolled key, and revoking is deleting it on the node.
3. **What a command is:** a line of the PURR command line (the same one the serial console and the planned remote terminal use), run with the administrator's rights, with the output returned in pieces. Updates and drivers travel the same way the system already moves them (signed items), in chunks, only on an administrator's command.
4. **The node:** a `heltec_v3` board profile (no PSRAM, the OLED, the radio on its own pins, Vext power), a kernel and image with no LVGL shell (the OLED gets a small status screen), and an **app loader that places small apps in internal RAM** (today the loader is PSRAM-only: `commands.c`, `esp_mmu_map(... MMU_TARGET_PSRAM0 ...)`). The ESP32-S3's internal SRAM has a fixed offset between its data and instruction addresses (`SOC_I_D_OFFSET`), and the relocator already works with two addresses (data and code), so an app can sit in SRAM and run through the instruction alias. Only small apps fit (a few tens of KB); the Meshtastic service has to be one of them.
5. **Bootloader role kept:** the PURR bootloader already picks between images and recovers; the node keeps a way back to stock Meshtastic (a second slot).

**Order:** (a) `Modules/meshtastic/purr_mt_admin.{c,h}`: the admin message format, signing and checking, replay protection, pieces and reassembly, host-tested; (b) the Heltec board profile and a no-PSRAM kernel build; (c) the internal-RAM app loader; (d) the Meshtastic app on the Heltec and the admin channel end to end between the T-Deck (administrator) and the Heltec (node), on a private channel, never on LongFast; (e) updates and driver pushes over it.

**Heltec V3 drivers (owner, 2026-11-26):** the node keeps a `drivers` partition so drivers can be updated in place, and carries only what it needs: the LoRa driver (`lora-sx126x.cat`, 4.9 KB, fits one 8 KB slot) and at most four more, five in all (`sdk/purrsdk/drivers.py`: `MAX_DRIVERS_BY_BOARD`, default list for `heltec_v3`). The partition itself stays the standard ten 8 KB slots at the end of the flash (0x7EC000, `partitions/heltec_v3.csv`) because every kernel expects that size; the other slots stay blank.
