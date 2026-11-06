# 06. The app runtime, the app manager, the sandbox and permissions

## Short version

- **AppManager** keeps signed `package.cat` files on the filesystem and checks them. **The runtime** loads one into PSRAM, gives it a task and a queue, and drives it through a lifecycle. **Permissions** gate the dangerous catcalls.
- The manager logic is plain C in [purr_runtime.c](../PurrOS/components/coreos/src/purr_runtime.c), tested on a PC. The device glue is [purr_catrt.c](../PurrOS/main/purr_catrt.c).
- At most 8 instances exist at once. On an 8 MB PSRAM board, 3 user apps may stay frozen in the background and 2 services may run.
- There is **no memory protection** between apps on the ESP32-S3. The "sandbox" is signing, the catcall-only link rule, permissions and watchdogs.
- Control is Hyprland-style: `/etc/apps.conf`, dispatchers, queries, and an event stream, from the `purrctl` shell command.

## What it is for

The runtime lets several small apps share a device with very little RAM, switch between them, and survive a misbehaving one. It decides when an app may start, who is in front, what to freeze or stop when memory runs low, and what an app must ask the user before it may do something with consequences.

## The files

| File | What it is |
|---|---|
| [PurrOS/components/coreos/src/purr_runtime.c](../PurrOS/components/coreos/src/purr_runtime.c), [include/purr_runtime.h](../PurrOS/components/coreos/include/purr_runtime.h) | The manager: states, launch, admission, focus, rules, binds, out-of-memory killer, events |
| [PurrOS/main/purr_catrt.c](../PurrOS/main/purr_catrt.c) | Device side: a task and queue per app, the manager task, catcall lookup, `purrctl` back end |
| [PurrOS/components/coreos/src/purr_appmgr.c](../PurrOS/components/coreos/src/purr_appmgr.c), [include/purr_appmgr.h](../PurrOS/components/coreos/include/purr_appmgr.h) | App storage and the registry (a rebuildable cache) |
| [PurrOS/components/coreos/src/purr_perm.c](../PurrOS/components/coreos/src/purr_perm.c), [include/purr_perm.h](../PurrOS/components/coreos/include/purr_perm.h) | The permission table and its text format |
| [PurrOS/main/purr_catrt_power.c](../PurrOS/main/purr_catrt_power.c) | The permission gate and prompt queue; the `power` catcall |
| [PurrOS/main/purr_w10m_perm.c](../PurrOS/main/purr_w10m_perm.c) | The permission prompt on the screen |
| [PurrOS/main/commands.c](../PurrOS/main/commands.c) | Loading apps (`catrt_platform_load`), the `purrctl` and app commands |
| [PurrOS/components/kernel/src/purr_io.c](../PurrOS/components/kernel/src/purr_io.c) | The flash task that serves apps with PSRAM stacks |
| [AppRuntime/SPEC.md](../AppRuntime/SPEC.md), [AppManager/SPEC.md](../AppManager/SPEC.md), [Permissions/SPEC.md](../Permissions/SPEC.md) | The specs (drafts) |
| [PurrOS/components/coreos/test/host/test_runtime.c](../PurrOS/components/coreos/test/host/test_runtime.c) | 273 checks on the manager |
| [Apps/](../Apps/) | The sideloaded apps; see [chapter 16](16-apps.md) |

## Where an app lives and how it is found

An installed app is a folder with one file, `package.cat`. When an app is asked to open, `catrt_platform_load()` looks in this order:

1. `/home/<user>/devapps/<name>/package.cat`, **only while developer mode is active**, so a build under test can stand in for an installed app.
2. `/home/<user>/apps/<name>/package.cat`.
3. `/apps/<name>/package.cat` (shared by all users; owner decision, 2026-10-28, [plan9.md](../plan9.md)).

A refused user copy is not quietly replaced by the system one. App names are letters, digits, `-` and `_` only, so a name cannot escape the apps folder. The registry holds up to 32 apps ([purr_appmgr.h](../PurrOS/components/coreos/include/purr_appmgr.h)).

Every load verifies the container with `purr_image_verify()` in every secure mode ([chapter 03](03-trust-and-keys.md)). An unsigned app runs only if all four hold: developer mode is active this boot, an administrator turned on "allow unsigned apps", the app is in the user's own folder, and its payload hash still matches. A badge shows while this is allowed.

## The lifecycle

States: `STARTING`, `FOREGROUND`, `BACKGROUND`, `SUSPENDED`, `STOPPING`, `STOPPED`.

1. **Launch.** The manager checks the slot count, then **admission**: the app's memory block plus the system reserve must fit, and there must be internal RAM for its task. If not, it stops what may be stopped (frozen apps, then background apps, oldest first) and tries again; if nothing can go it fails with `PURR_RT_E_MEMORY` or `PURR_RT_E_INTERNAL` without touching the foreground app.
2. **Create.** `on_create` runs on the app's own task, with any saved state.
3. **Focus.** One app is in front. A UI app gets a window from the shell; a game takes the screen.
4. **Leave the screen.** `on_pause`, then `on_save_state` (capped at 4 KB). An app that may not run in the background is **frozen**: its task parks itself on a semaphore when it reaches a safe point. The runtime never suspends a task from outside, because it could be holding a LittleFS or UI lock.
5. **Eviction.** Over the frozen cap, the app unused longest is closed, after an `evicted` event and a notice ("Closed Notes to free memory"). Persistent and pinned apps are skipped.
6. **Teardown.** Mark stopping, deliver `on_destroy`, wait for in-flight calls (2 seconds, then force), release timers and handles, free the block, emit `closeapp`.

Instance limits come from `purr_rt_caps_for_psram()`: 8 MB class gets 3 frozen apps and 2 services, 4 MB class 2 and 1, smaller 1 and 1. `/etc/apps.conf` can lower them, never raise them.

### Memory and the out-of-memory killer

Pressure levels: `low` sends `on_low_memory`; `critical` kills victims, in order suspended, background, least recently used, largest first among equals. It never kills a system, `persistent` or foreground app, and it keeps a minimum time between kills. An app that does not answer a callback for three seconds is reported `unresponsive` and can be force-stopped.

### Stacks and the PSRAM rule

Each app task needs a stack. Since 1.0.22 (2026-11-05, [plan7.md](../plan7.md)) the task, its queue and its semaphores are created **in PSRAM** (12 KB stack), so an open app holds about 1 KB of internal RAM instead of about 10. A task with a PSRAM stack must not touch flash itself, because flash operations switch the cache off. All app file and settings paths therefore end in `purr_io_partition_read/write/erase_range`, which hand the access to a small task with an internal stack and wait. The setting `apps.psram_stacks = 0` returns to 8 KB internal stacks.

**This differs from `CLAUDE.md`**, which says apps cannot use PSRAM stacks and each takes an 8 KB internal stack. That was true before 1.0.22 and is still true for the fallback and for tasks that start hardware (a driver load keeps a fast-RAM stack). The code and [plan7.md](../plan7.md) are newer; the CLAUDE.md line needs updating by the owner. Not verified on a device.

## The control surface

`/etc/apps.conf` is planted with every line commented out. Keywords:

| Keyword | Meaning |
|---|---|
| `general:max_apps`, `reserve_kb`, `low_kb`, `critical_kb` | Limits |
| `rule = effect[ arg], matcher` | Effects: `background`, `persistent`, `suspend`, `memory <kb>`, `focus`, `nofocus`. Matchers: `name:<glob>`, `class:<daemon|command-line|ui>` |
| `bind = MODS, key, dispatcher` | A key binding (up to 16) |
| `exec-boot = app` | Started at boot with nobody signed in |
| `exec-once = app` | Started after the first sign-in of a boot |

Shell: `purrctl dispatch open|close|kill|focus|cycle|foreground|background|suspend|resume|pin|unpin|post ...`, `purrctl apps`, `activeapp`, `memory`, `rules`, `binds`, `version` (add `-j` for JSON), `purrctl events` for the stream of `name>>data` lines, and `purrctl reload`. A bad config line is reported, never fatal.

## Permissions

Dangerous permissions follow Android's model: the app asks when it first reaches for the feature, and the user answers.

| Permission | What it allows | Who |
|---|---|---|
| `keep-awake` | Hold the screen or CPU awake | any |
| `wake-alarm` | Wake the device on a schedule | any |
| `shared-storage` | See and change files in the user's home | any |
| `system-files` | Read system files (read-only) | administrators |
| `manage-devices` | Hardware, drivers, system updates | administrators |
| `install-apps` | Install, update and remove apps | any |
| `messaging`, `contacts`, `radio` | Messages, address book, LoRa lease | any |

How a request goes:

1. A catcall calls `purr_catrt_perm_gate(perm)`. The result is yes, no or **ask**.
2. **Ask** queues a prompt (up to a small number pending) and returns an `*_ASK` code. A *background* app is never asked; it is refused unless it was allowed earlier.
3. The user picks *Allow always* (saved), *Just once* (until the app stops), or *Deny*. After two denials the prompt offers *Don't ask again* (saved).
4. The app receives `PURR_APPEV_PERM` and calls again.
5. Saved grants are in `/home/<user>/permissions.conf`, one `app:permission=always|never` per line. The Files catcall can read this file but never write it.
6. A **privileged** app has every permission and is never asked.

Settings > Apps > Permissions shows and resets them. `purr_perm_reset()` makes every permission of an app ask again. `purr_perm_forget()` (drop an app's grants on removal) exists in the table code, but no caller in `main/` uses it yet, so grants of a removed app stay in the file.

## The sandbox, honestly

What stops an app from doing harm is a short list, and it is not hardware isolation:

| Control | What it gives |
|---|---|
| Signature check at load | Only signed apps run (unsigned only in developer mode) |
| Freestanding link | An app cannot call a function the system did not hand it |
| Catcall-only access | Storage is rooted in the app's own folder; `..` and absolute paths are refused |
| Permission gate | Dangerous calls ask the user |
| Watchdog and teardown | A stuck app is stopped; its timers and handles are released |

**An app and the OS share one address space.** A signed app that is buggy or hostile can corrupt memory ([SECURITY.md](../SECURITY.md) section 2). Do not describe this as a sandbox in the sense of an MPU or a process.

## How to build and test

Host tests for the manager, permissions and app manager (seconds): `cd PurrOS/components/coreos/test/host && IDF_PATH=$HOME/esp/esp-idf-v5.3.5 python3 run.py runtime` (also `perm`, `appmgr`). The device side runs on the device or in the VM; `purrctl` typed at the shell is the first thing to try. Do not run the emulator unless the owner asks.

## What goes wrong and how to fix it

| Symptom | Cause | Fix |
|---|---|---|
| App will not open: out of memory | Not enough internal RAM above the 16 KB reserve | Close an app; check the Task Manager; the reserve is `INTERNAL_RESERVE` in `purr_catrt.c` |
| UI freezes after "back" in an app | A late event reached a dying app | Fixed in stages 2026-10-29 and 2026-10-31 ([plan9.md](../plan9.md)); if it returns, attach OpenOCD at the freeze |
| App dies on first tap in a `switch` | Jump table relocation | Rebuild; fixed 2026-11-01 ([chapter 05](05-catcall-abi.md)) |
| `Guru Meditation ... stack in external RAM` | A path reached flash directly on a PSRAM stack | Route it through `purr_io` |
| Second user sees "filesystem error" on install | Suspected: their apps folder is missing or not writable | Open ([plan9.md](../plan9.md), 2026-10-28) |
| Permission prompt never appears | The app asked while counted as background | Fixed 2026-11-01 for starting UI apps |

## What is not finished

- **Companion services** (a `[service]` entry in a package that carries on when the UI closes) and the autosave nudge before eviction are not built ([plan5.md](../plan5.md)).
- **Phase D, the SDK side**, is tracked in [plan5.md](../plan5.md). Importance classes beyond `persistent` and the `SUPER` key on the T-Deck are open questions in [plan6.md](../plan6.md).
- **App permission enforcement from the manifest** is not built: the runtime reads what the app declares in its own table, not the manifest ([chapter 05](05-catcall-abi.md)).
- **Not verified on a device:** frozen apps parking and thawing with a timer running, the full eviction path, ten apps open at once, and the permission queue with several apps at once ([plan5.md](../plan5.md), [plan7.md](../plan7.md)).
- **Plan and code differ** on the app stack rule (see above), and the older [AppManager/SPEC.md](../AppManager/SPEC.md) still describes a separate `apps` partition and a 1 MB area; apps live on `root` now.

Plan references: [plan6.md](../plan6.md) (runtime design), [plan5.md](../plan5.md) (SDK and lifecycle), [plan10.md](../plan10.md) (power and wake alarms), [plan8.md](../plan8.md) (the Store installs).
