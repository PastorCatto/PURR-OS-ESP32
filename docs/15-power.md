# 15. Power

*Current as of PURR OS 1.1 (2026-11-08).*

## Short version

- One policy object decides "is the device allowed to sleep now": `purr_power.c` in CoreOS. It has no ESP-IDF code, so a host test runs it with a fake clock.
- What works in code: **screen timeout** (default 60 s), the **panel sleeps** with the backlight, **wake locks with leases**, **alarms with wake windows**, and the **permissions** that gate them.
- **Light sleep is not implemented.** The setting `power.lightsleep` is stored and logged and nothing else. `CONFIG_PM_ENABLE` is not turned on by this code. It needs a real T-Deck.
- Shell command: `power` (state, locks, alarms). Settings keys: `power.screen_timeout`, `power.lightsleep`.
- Read [plan10.md](../plan10.md). Nothing in it has been run on a device. A VM panic after screen-off is open (see below).

## What it is for

Two jobs that are really one ([plan10.md](../plan10.md)). First, the device used to stay lit for ever. Second, a background app (the radio-mesh case) needs to say "keep me awake while I work" or "wake me every N seconds, let me check messages, then let the device sleep again". Both need one owner of the sleep decision, and both need a permission model.

## The files

| File | What it is |
|---|---|
| [plan10.md](../plan10.md) | The plan, with design, phases and progress |
| [purr_power.h](../PurrOS/components/coreos/include/purr_power.h), [purr_power.c](../PurrOS/components/coreos/src/purr_power.c) | Policy: lock table, alarm table, wake windows, state machine |
| [purr_power_plat.c](../PurrOS/main/purr_power_plat.c), [purr_power_plat.h](../PurrOS/main/purr_power_plat.h) | The real platform: backlight, panel sleep, tick task, `power` settings |
| [purr_catrt_power.c](../PurrOS/main/purr_catrt_power.c) | The `power` catcall, permission prompt queue, `perms`, `grant`, `revoke` |
| [purr_perm.h](../PurrOS/components/coreos/include/purr_perm.h), [purr_perm.c](../PurrOS/components/coreos/src/purr_perm.c) | The grant table and `permissions.conf` format |
| [purr_w10m_perm.c](../PurrOS/main/purr_w10m_perm.c) | The permission prompt card (UI task) |
| [purr_battery.h](../PurrOS/main/purr_battery.h), [purr_battery.c](../PurrOS/main/purr_battery.c) | Smoothed battery reading and charging flag |
| [Modules/drivers/st7789_module.c](../Modules/drivers/st7789_module.c) | The panel driver with `set_power` |
| [Apps/beacon/](../Apps/beacon/manifest.toml) | Sample app: a 30 s aligned alarm, counts wakes, calls `done()` |
| [test_power.c](../PurrOS/components/coreos/test/host/test_power.c) | Host test of the policy |
| [test_perm.c](../PurrOS/components/coreos/test/host/test_perm.c) | Host test of the permission table |
| [test_battmodel.c](../PurrOS/components/coreos/test/host/test_battmodel.c) | Host test of the battery model |
| [Permissions/SPEC.md](../Permissions/SPEC.md) | The permission model |

Plan10 names the permission files as `PurrOS/main/purr_perm.c/.h`. In the code they are under `PurrOS/components/coreos`. The links above are the real ones.

## How it works

### Three sleep states

| State | What happens | In code? |
|---|---|---|
| Awake | Screen on | yes |
| Screen off | Backlight 0 **and the panel put to sleep**; CPU and apps keep running; any key or touch wakes it | yes (compile-checked; not run on a device) |
| Light sleep | CPU halts between events; RAM, apps and Wi-Fi association kept | **no**: setting stored, decision logged ("cpu may sleep: yes/no (light sleep is off)") |

Deep sleep is not used: it reboots and drops every app. It only means "power off".

### Screen off and on

The idle timeout comes from `power.screen_timeout` (5 to 3600 seconds, or `never`; the default in `purr_power_plat.c` is 60 s, while the plan's menu lists 15 s to 5 minutes and Never). The input path marks activity. In order:

1. **Off:** the backlight goes dark first, then the panel is told to sleep (display off, sleep in).
2. **On:** the panel wakes (the driver waits the 120 ms the controller needs), the shell **repaints** everything (`purr_ui_service_repaint`), and only then does the backlight come up. A stale frame is never shown.
3. The first touch or key that only woke the screen is **swallowed** (`s_touch_swallow` in `purr_lvgl.c`), so it does not also tap something.

Why the panel sleeps too: the owner (2026-11-03) found that the backlight alone leaves the glass being driven with a static picture, and a low-battery brownout left artifacts.

The manager asks for the screen while holding its lock, but waking waits on the UI task, which may be blocked on that lock in its input callback. So the request is only written down, and the power task carries it out after the lock is dropped (it wakes on a task notification). While the panel is not on, drawing is dropped and reported as done, and the driver holds the SPI lock for a whole blit so a sleep command cannot land between a window and its pixels. A display without power control (the e-paper) keeps the backlight-only behaviour.

### Wake locks

| Lock | Meaning | Rule |
|---|---|---|
| `PURR_WAKE_SCREEN` | Keep the screen on | Released when the app leaves the foreground |
| `PURR_WAKE_CPU` | Keep the CPU out of light sleep | Has a **lease**: default 30 s, maximum 120 s, expires unless renewed |

Locks belong to the app and are dropped when it stops. At most 8 owners.

### Alarms and wake windows

| Rule | Value |
|---|---|
| Alarms per app | 2 |
| Minimum interval | 5 s |
| Alignment | `PURR_ALARM_ALIGN` lets the OS move a wake up to a fifth of the interval early so several alarms share one wake |
| Wake window | 10 s by default, 30 s at most, or until the app calls `power->done()` |
| Overruns | Three in a row and the alarm is cut off and the user told (`alarm-off`) |
| Who may | Only an app allowed to run in the background |

Alarms survive screen-off, not a reboot. The owner decided (2026-10-28) that only the service slots hold standing alarms, and a frozen app keeps only its alarm and loses its lock ([plan10.md](../plan10.md), [plan5.md](../plan5.md)).

### The `power` catcall

`hold`, `renew`, `release`, `alarm_set`, `alarm_cancel`, `done`, `screen_is_on`. Events: `PURR_APPEV_WAKE` and `PURR_APPEV_PERM`. A gated call from the app in front queues a prompt and returns `PURR_POWER_ASK`. The shell shows Allow always, Just once or Don't allow; the answer arrives as `PURR_APPEV_PERM`; the app calls again. A background app is never asked and is denied unless an "always" is on file.

### Permissions

Stored per user in `/home/<user>/permissions.conf` as `app:permission=always|never`. "Just once" lasts until the app stops and is never written. The table fails closed if the file is unreadable. Plan10 introduced two permissions (`keep-awake`, `wake-alarm`); `purr_perm.h` now has nine (adds `shared-storage`, `system-files`, `manage-devices`, `install-apps`, `messaging`, `contacts`, `radio`). Shell commands: `perms`, `grant <app> <permission>`, `revoke <app> <permission>` (admin).

### The battery

The `battery-adc` driver gives a raw reading. `purr_battery.c` smooths it, keeps the last good value and refreshes at most every 20 s. Since 2026-11-04: bit 0 "present" from 2.8 V up, bit 1 "charging" when over 4.3 V, so the icon no longer vanishes on USB power. The `battery` shell command prints the raw reading and flags. **The real voltage on USB is still unknown** ([plan7.md](../plan7.md)).

## How to build and test it

1. Host tests: `cd PurrOS/components/coreos/test/host && IDF_PATH=$HOME/esp/esp-idf-v5.3.5 python3 run.py power perm battmodel`. Plan10 records 68 checks for power and 56 for permissions.
2. On a build, `power` prints the state; `power screen off` and `power screen on` switch the screen; `power timeout 30` sets the timeout.
3. Install the beacon sample (see [chapter 16](16-apps.md)); Start sets a 30 s aligned alarm.
4. On a unit, check what plan10 lists: the screen goes fully black when idle; wake shows the right picture with no flash of the old one; a game that is drawing at timeout does not hang; the first key press after a wake is not delivered.

## What goes wrong

| Symptom | Cause | Fix |
|---|---|---|
| White or blank screen on the T-Deck | A stale `st7789` driver blob in `commands.c` (the 2026-10-25 white screen) | Regenerate the embedded array whenever `st7789_module.c` changes; the baked-in driver is planted at start |
| Screen stays dark after wake | Panel wake failed (logged as `panel wake failed`) | Check the log; a stuck wake drops all drawing until restart |
| Tap after screen-off panics the VM (core 0, I2C interrupt) | Open: probably a touch/keyboard I2C bus shut down across the screen-off | Next step in [plan7.md](../plan7.md): read the screen-off and wake path and the I2C setup |
| An app cannot hold a lock | Permission not granted | `grant <app> keep-awake` (admin) |
| Alarm stops | Three overruns | The app must call `done()` |

## What is not finished

- **Phase 5 (light sleep on hardware)** is open: `esp_pm`, GPIO wake, measured current. SPI DMA and the I2C polling of keyboard and touch must not stall in auto light sleep. The keyboard's interrupt line is not used by polling, so wake-on-key needs it wired. Wi-Fi in light sleep delays inbound packets by up to a beacon interval.
- A radio wake source (`PURR_WAKE_RADIO`) depends on the LoRa driver; the alarm path covers the case for now.
- Settings > Battery (what holds the device awake, next alarm): listed in the plan; I did not verify it in the Settings code.
- Not verified on a device: panel sleep and wake, the idle timeout, the permission card, the beacon.
- Recovery builds have about 2 percent of the slot free (plan10, 2026-10-31), so power code added there is tight.

Next: [16 The apps](16-apps.md).
