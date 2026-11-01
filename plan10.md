# plan10 - power: system sleep, and apps that keep the device awake or wake it on a schedule

Status: phases 1-4 written 2026-10-27; policy and permission table host-tested, rest compile-checked, nothing run in the VM yet. This is the last plan file (see memory plan-file-limit): when it is
done, no new plan opens until plans 1-10 are all finished.

## Why
Two jobs that are really one. (1) The device never sleeps today: the screen stays on, the CPU never idles down
(`CONFIG_PM_ENABLE` is off), and nothing times out. (2) A background app (the Meshtastic case) needs to say "keep me
awake while I work" or "wake me every N seconds, let me check messages, beacon, hear the reply, then let the device
go back to sleep". Both need one owner of "is the device allowed to sleep right now", and both need the permission
model that does not exist yet (`ctl` and everything privileged is still "comes with the permissions chunk").

## What exists (checked 2026-10-27)
- Backlight PWM only (`display.brightness`, 1-255). No timeout, no panel sleep (`set_power` is NULL for the EPD, and
  unused for the ST7789), no idle tracking.
- No `esp_pm`, no light sleep, no wake sources. Keyboard and touch are polled over I2C (touch `int_pin` unused).
- `sched` catcall has per-app timers (FreeRTOS timers). `background`/`persistent` rules exist in `/etc/apps.conf`.
- No permission model: `catcall_get` hands out console/app/sched/system/event/storage to everyone, `ui` by kind.

## Design

### Three sleep states (each is a layer, each works without the one below)
1. **Awake**: screen on.
2. **Screen off** (idle timeout, or the user presses sleep): backlight 0 and the panel put to sleep. CPU keeps
   running, apps keep running exactly as now. Any key or touch wakes the screen. This is what the user mostly sees,
   and it works everywhere including the VM.
3. **Light sleep** (screen off and nothing holds the CPU): the CPU halts between events, RAM and all app state kept,
   Wi-Fi stays associated (modem sleep). Wakes on: the next alarm, a key press, a touch, the radio interrupt.
   Needs real hardware to prove; behind a switch (`power.lightsleep`, default off until it has run on a T-Deck).
   Deep sleep is not used: it reboots and drops every app. It is only "power off".

### Wake locks (what an app can hold)
- `PURR_WAKE_SCREEN`: keep the screen on (a reader, a timer face). Released automatically when the app leaves the
  foreground, so a forgotten lock cannot keep the screen lit.
- `PURR_WAKE_CPU`: keep the CPU out of light sleep (a transfer, a radio exchange), screen may be off. Every CPU lock
  has a **lease**: it expires on its own (default 30 s, max 120 s) unless renewed, so a crashed or forgetful app
  cannot drain the battery. Held locks and owners are listed by `power` in the shell and in Settings > Battery.
- Locks are owned by the app and dropped when it stops (same ownership rule as timers and files).

### Alarms (wake in intervals)
- `alarm_set(interval_ms, flags)`: wake the device every interval; at most 2 per app; minimum interval 5 s
  (a floor stops a polling loop from becoming a busy loop). Flags: `ALIGN` lets the OS move the wake up to 20 % early
  or late so several apps' alarms land together (one wake, not five). The app gets `PURR_APPEV_WAKE` on its own task.
- **The wake window.** On `WAKE` the OS holds a CPU lock for the app for up to `window_ms` (default 10 s, max
  30 s) or until the app calls `power->done()`, whichever is first. The app checks messages, beacons, waits for the
  reply, calls `done()`. When every window is closed and nothing else holds the device, it goes back to sleep. An app
  that never calls `done()` loses its window at the limit and the OS counts the overrun; three overruns in a row and
  the alarm is switched off and the user told (a runaway app must not own the battery).
- Alarms survive screen-off and light sleep. They do not survive a reboot (the app sets them again in on_create).
- Only an app that is allowed to run in the background (`background` rule or kind daemon) can have an alarm.

### Wake sources the OS owns
Keyboard and touch (screen on), the alarm timer, and one named source for the radio (`PURR_WAKE_RADIO`) once the
LoRa driver exists: the driver raises it, the OS wakes the subscribed app. Apps never name GPIOs.

### The `power` catcall (v1, `purr_cc_power_t`, name `power`)
`hold(kind, lease_ms)`, `renew`, `release`, `alarm_set(interval_ms, window_ms, flags)`, `alarm_cancel(i)`, `done()`,
`screen_is_on()`; events `PURR_APPEV_WAKE` (`arg` = alarm index, `name` = "alarm", or "alarm-off" when the OS cut the
alarm) and `PURR_APPEV_PERM`. Holding and alarms need the permissions below; `done`, `release` and `screen_is_on`
need none. Alarms are only for an app allowed to run in the background; a screen lock only for the app in front.

### Permissions (the missing chunk, built here because power is the first thing that needs it)
Follows `Permissions/SPEC.md` as already decided: asked **at first use**, grants **per user**, background apps
**cannot prompt**, privileged apps have everything, a refusal returns an error and the app handles it.
- Two new dangerous permissions: `keep-awake` and `wake-alarm` (added to the spec's table). The manifest lists them
  under `[permissions] requests` for information only; nothing is decided at install.
- **Asking without blocking.** The first gated call from the app in front queues a prompt and returns
  `PURR_POWER_ASK`; the shell shows a card (Allow always / Just once / Don't allow, the last becoming "Don't ask
  again" after two refusals); the answer reaches the app as `PURR_APPEV_PERM` (arg 1 allowed, 0 not), and the app
  calls again. Nothing waits inside a callback (the runtime gives a callback 3 s).
- **A background app is never asked**: it is denied (and logged) unless an earlier "always" is on file. So an app
  with a screen asks the first time its user starts it; a pure daemon needs the switch in Settings or the shell.
- Stored in `/home/<user>/permissions.conf` (`app:permission=always|never`); "just once" lasts until the app stops
  and is never written. The table follows the signed-in user and fails closed (empty) if the file is unreadable.
- A revoke (Settings, or `power`-side reset) drops the app's locks and cancels its alarms at once.
- `purr_perm.c` (coreos) is the table and file format, host-tested; `purr_catrt_power.c` (main) is the glue, the
  prompt queue and the catcall.

### Where the code goes
- `PurrOS/components/coreos`: `purr_power.h/.c`, platform-independent policy: lock table, alarm table with alignment,
  wake windows, state machine. Takes a small platform struct (now_ms, set_screen, set_light_sleep, arm_timer), so a host
  test runs the whole policy with a fake clock. No IDF includes.
- `PurrOS/main/purr_power_plat.c`: the real platform: backlight and panel sleep, `esp_pm_configure` +
  `esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP)` for CPU locks, `gpio_wakeup_enable` for keyboard/touch, and the VM stub
  (the QEMU build has no light sleep, so there the policy still runs and the CPU lock is a no-op).
- `PurrOS/main/purr_perm.c/.h`: grant table and `/home/<user>/permissions.conf`.
- `purr_catrt.c`: `power` catcall, `PURR_APPEV_WAKE`, owner cleanup in `teardown`.
- Settings: Display > Screen timeout (15 s, 30 s, 1 m, 2 m, 5 m, Never); a Battery page (what is holding the device
  awake, next alarm); Apps > permissions switches. `settings` command keys: `power.screen_timeout`, `power.lightsleep`.
- Activity: the existing LVGL input path marks activity (`lv_display_get_inactive_time`), so the timeout needs no new
  input code; the first key or touch while the screen is off only wakes it (it is not delivered to the UI).
- Sample app `Apps/beacon/`: a small UI app (so it can be asked for its permission) that keeps running in the
  background; Start sets a 30 s aligned alarm, each wake it counts and calls `done()`. It is the Meshtastic loop
  without a radio, and it is what the VM can run.
- Prompt card: `main/purr_w10m_perm.c` (UI task), registered with `purr_catrt_perm_set_ui`.

## Phases
1. `purr_power` policy core + host test (locks, leases, alarms, alignment, windows, overrun cut-off) with a fake clock.
2. Screen sleep for real: platform layer, idle timeout, wake on first key/touch, Settings > Display > Screen timeout,
   `power` shell command. Runs in the VM.
3. Permissions: `purr_perm`, grant table + file, host test, `catcall_get` gating, install prompt in the portal,
   Settings > Apps > permissions. Runs in the VM.
4. `power` catcall + `PURR_APPEV_WAKE` + beacon sample app, signed and published to the dev repo. Runs in the VM.
5. Light sleep on hardware: `esp_pm`, GPIO wake, measured current. Cannot be done in the VM; needs the T-Deck.
   Until then the switch stays off and this phase stays open.

## Progress (2026-10-27)
- Phase 1 done: `purr_power.c`, host test `test_power.c` (68 checks).
- Phase 3 done in code: `purr_perm.c`, `test_perm.c` (56 checks); `purr_catrt_power.c` (grants file per user, prompt
  queue, `perms`/`grant`/`revoke`); prompt card `purr_w10m_perm.c`; Settings > Apps > Permissions switches.
- Phase 2 done in code: `purr_power_plat.c` (backlight off on idle, tick task, `power` command, settings keys
  `power.screen_timeout` and `power.lightsleep`), input path drops the press that only woke the screen,
  Settings > Display > "Screen off after".
- Phase 4 done in code: `power` catcall, `PURR_APPEV_WAKE` / `PURR_APPEV_PERM`, `Apps/beacon/`.
- Firmware (`vmdbg`) builds, 14 % of the kernel slot free; beacon and notes build as modules. NOT run in the VM.
- Rechecked 2026-10-31: stdmin, stdrec and stdfull all build. Free space in the slot: min 6 %, **recovery 2 % (0x8480 bytes)**, full builds (the usual too-small warning for the kittenos and loader slots). Recovery is nearly full.
- Panel sleep is not used: only the backlight goes dark (putting the ST7789 to sleep has to be sequenced with the UI
  task's flushes). Revisit with the hardware phase.
- Lost in a macOS hard lock and to rebuild: the scratchpad (vmdbg build dir was rebuilt; signed apps, dev repo and
  mkimage scripts still need rebuilding).

## Not decided / risks
- Light sleep and the display: SPI DMA and the I2C polling of keyboard/touch must not stall in auto light sleep. The
  keyboard has an interrupt line on the T-Deck (GPIO 46) that polling mode does not use; wake-on-key needs it wired.
  Verify on hardware before the switch defaults on.
- Wi-Fi in light sleep keeps the association but delays inbound packets by up to a beacon interval; an app that needs
  low latency holds a CPU lock for the exchange.
- A radio wake source depends on a LoRa driver that does not exist yet (SX1262 on the T-Deck Plus). The alarm path
  covers the Meshtastic loop until then.
- `ctl` (task-manager apps) stays out of scope; this plan only uses the permission mechanism.

### Decision (owner, 2026-10-28)
Plan10 stays open as written (light sleep needs the device). Added requirement from the owner's display notes: apps that are not on screen
must be unloaded, or frozen if they are not closed. That is a lifecycle rule for the app runtime (plan6) that this plan's wake locks and
alarms must respect (a frozen app keeps only its alarm; a lock is dropped on freeze).

### Service apps (owner, 2026-10-28)
Only the 2 service slots (plan5) hold standing alarms; the intended interval on hardware is 30-60 s. User apps in the background are frozen
and get no alarm. The `background` rule and daemon kind in `/etc/apps.conf` map to the service slots.

### Decided (owner, 2026-10-28)
Service slots follow the PSRAM-based cap in plan5 (2, 1, 1 for 8, 4, 2 MB). A service is the `[service]` entry in the app's own package.
