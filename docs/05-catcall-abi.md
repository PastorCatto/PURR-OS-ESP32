# 05. Catcalls and the app ABI

## Short version

- An app is a freestanding module with no libc. Its only link to the system is `env->catcall_get(name, min_version)`, which returns a **versioned table of function pointers** or NULL.
- The tables are declared in [purr_catcalls.h](../PurrOS/components/coreos/include/purr_catcalls.h); the lifecycle and the event types are in [purr_app_abi.h](../PurrOS/components/coreos/include/purr_app_abi.h). The runtime serves 14 catcalls today.
- A table's first field is its version. A table only grows at its end. An app asks for the version of the newest field it uses.
- `ctl`, `display`, `input`, `net`, `wifi`, `radio`, `gps`, `gpio`, `i2c`, `spi` and `audio` are in the design ([Catcalls/SPEC.md](../Catcalls/SPEC.md)) but are **not served** by the runtime yet.
- To build an app today: `purr` (the kit) or `purrstrap modules build --kind app`. The manifest is checked at build time and is **not** carried into the signed image.

## What it is for

Catcalls let an app run on any board and any shell without knowing the hardware, the UI toolkit or ESP-IDF. The app asks for a service by name; the system decides whether it exists on this board, whether this app may have it, and which implementation answers. The same rule keeps apps portable: an app that imports nothing but `catcall_get` cannot call anything the system did not choose to give it.

## The files

| File | What it is |
|---|---|
| [PurrOS/components/coreos/include/purr_catcalls.h](../PurrOS/components/coreos/include/purr_catcalls.h) | Every catcall table, in one header shared by the runtime and by apps |
| [PurrOS/components/coreos/include/purr_app_abi.h](../PurrOS/components/coreos/include/purr_app_abi.h) | App lifecycle tables (v1, v2), the env struct, event types, destroy reasons |
| [PurrOS/main/purr_catrt.c](../PurrOS/main/purr_catrt.c) | `catcall_get` and the console, app, sched, system and event tables |
| [PurrOS/main/purr_catrt_ui.c](../PurrOS/main/purr_catrt_ui.c) | The `ui` table and the window host |
| [PurrOS/main/purr_catrt_storage.c](../PurrOS/main/purr_catrt_storage.c) | `storage` |
| [PurrOS/main/purr_catrt_files.c](../PurrOS/main/purr_catrt_files.c) | `files` |
| [PurrOS/main/purr_catrt_power.c](../PurrOS/main/purr_catrt_power.c) | `power`, and the permission gate used by the others |
| [PurrOS/main/purr_catrt_devices.c](../PurrOS/main/purr_catrt_devices.c) | `devices` |
| [PurrOS/main/purr_store.c](../PurrOS/main/purr_store.c) | `store` |
| [PurrOS/main/purr_catrt_game.c](../PurrOS/main/purr_catrt_game.c) | `game` |
| [PurrOS/main/purr_catrt_msg.c](../PurrOS/main/purr_catrt_msg.c) | `messaging` and `contacts` |
| [Catcalls/SPEC.md](../Catcalls/SPEC.md) | The design list (draft 0.1) |
| [AppSDK/SPEC.md](../AppSDK/SPEC.md) | What a developer writes and builds (draft 0.1) |
| [AppSDK/templates/unprivileged/](../AppSDK/templates/unprivileged/) | The one starter template |
| [UI/SPEC.md](../UI/SPEC.md) | The `ui` catcall design |
| [Apps/notes/](../Apps/notes/) | A small real app that uses `ui` and `storage` |

## How an app starts

An app image is a `PURR_IMG_APP` container ([chapter 03](03-trust-and-keys.md)) whose payload is a relocatable module ([chapter 07](07-drivers.md) explains the format, which apps share with drivers). The runtime loads it into PSRAM, relocates it, and calls its **entry function** with no arguments. The entry returns a table.

There are two table versions:

| Table | ABI | Callbacks |
|---|---|---|
| `purr_app_table_t` | 1 | `on_create(env, state, len)`, `on_destroy(reason)` |
| `purr_app_table2_t` | 2 | Version 1 first, then `on_resume`, `on_pause`, `on_save_state`, `on_low_memory`, `on_event`, plus `memory_needed`, `kind` and `flags` |

The v2 table starts with the v1 table, so a runtime that knows v2 accepts both and an old runtime refuses a v2 app at the version check. `kind` is `PURR_APP_KIND_COMMAND_LINE` (0), `DAEMON` (1), `UI` (2) or `GAME` (3). `flags` are in [purr_runtime.h](../PurrOS/components/coreos/include/purr_runtime.h): background-ok, saves-state, persistent, privileged.

Every callback runs on the app's own task and must return quickly. The runtime waits at most three seconds for a callback ([purr_catrt.c](../PurrOS/main/purr_catrt.c), `CALL_TIMEOUT_MS`), and marks a slow app unresponsive.

The `env` handed to `on_create` has `console_print` (kept for v1), `env_version`, and `catcall_get`. Events arrive through `on_event` as `purr_app_event_t`:

| Event | Meaning |
|---|---|
| `PURR_APPEV_USER` | A bus event the app subscribed to, or one posted to it |
| `PURR_APPEV_TIMER` | A `sched` timer fired (arg = timer id) |
| `PURR_APPEV_KEY` | A key, when the app has input focus |
| `PURR_APPEV_UI` | A widget event: `click`, `changed`, `back`, `action`, or `job` |
| `PURR_APPEV_WAKE` | A `power` alarm fired |
| `PURR_APPEV_PERM` | The user answered a permission prompt |

## The catcalls the runtime serves

`catcall_get` is in [purr_catrt.c](../PurrOS/main/purr_catrt.c). The version is the table's own `version` field in the code; the header's comments can lag behind.

| Name | Table version | Who gets it | What it does |
|---|---|---|---|
| `console` | 1 | any app | `write`, `print`, `read_line` (only the app with console focus gets input) |
| `app` | 1 | any | Own info, `close_self`, `request_focus`, `post_self` |
| `sched` | 1 | any | `uptime_ms`, `sleep_ms`, up to 4 timers that arrive as events |
| `storage` | 1 | any | Files in `/home/<user>/appdata/<app>/`; relative paths only; no `..` |
| `system` | 2 | any | Uptime, free heap, battery, unix time; v2 adds `local_time()` |
| `event` | 1 | any | `emit` (namespaced `custom.<app>.<name>`), `subscribe` (4 per app) |
| `ui` | 5 | apps of kind `ui` only | Window, label, button, row, textarea, grid, input line, progress, slider, image, bubble, conversation row |
| `game` | 1 | apps of kind `game` only | `lcd_write`, `buttons`, `ticks_us`, `quit` |
| `power` | 1 | any; asks the user | `hold`, `renew`, `release`, `alarm_set`, `done` |
| `files` | 1 | any; asks the user | Absolute paths in the user's view: list, stat, read, mkdir, remove, rename, copy |
| `devices` | 1 | administrators; asks | Find hardware, drivers, updates, developer server (Device Manager) |
| `store` | 1 | any; asks | Repositories, install, update, remove (Store and companion) |
| `messaging` | 1 | any; asks | The shared message layer and the messaging services |
| `contacts` | 1 | any; asks | The address book |

"Asks" means a dangerous permission: the first call returns an `*_ASK` code, the system shows a prompt, and the app receives `PURR_APPEV_PERM` and calls again ([chapter 06](06-app-runtime.md)).

### Version rules

1. The table's first field is its version. An app calls `catcall_get("ui", 3)` to ask for at least version 3; an older system answers NULL.
2. A field is added **only at the end**, and the version is raised. An old app keeps working.
3. The header comment on `purr_cc_ui_t` still says "1 (the table says 4 now ...)". The code says 5. The comment is stale; the code is right.
4. `catcall_get` returns the *newest* table the system has, so an app must test the version it received before using a field added later.

### What an app owns

Everything an app creates through a catcall is owned by that app: timers, open storage handles, subscriptions, widgets. When the app stops, the runtime releases them. Once an app is closing, its catcall calls are refused (`purr_catrt_caller`), which fixed a freeze where a dying app created a ghost window ([plan9.md](../plan9.md), 2026-10-29).

## Designed, not served

[Catcalls/SPEC.md](../Catcalls/SPEC.md) lists many more: `display`, `input`, `net`, `wifi`, `radio`, `gps`, `gps_raw`, `gpio`, `i2c`, `spi`, `audio`, `appmgr`, `update`. None is in `catcall_get`. `ctl` (Hyprland-style control of the app system, for privileged apps only) is declared in the header with its table but is **not served**: a code comment says it comes with the permissions chunk. There is no `net` catcall, so an app cannot open a socket today; the Store and Device Manager reach the network through their own catcalls. There is no audio ([plan9.md](../plan9.md), "Apps to make").

## How to build and test an app

1. Make a project: `python3 purrstrap/purrstrap.py apps new --name myapp`. This copies the starter template, which uses the old v1 table ([AppSDK/templates/unprivileged/](../AppSDK/templates/unprivileged/)). For a real UI app, copy [Apps/notes/](../Apps/notes/) and its `manifest.toml` instead.
2. Build: `python3 purrstrap/purrstrap.py apps build --project Apps/myapp`. This needs Python 3.11 or newer (it reads TOML). On older Python the `apps` subscript shows as broken in `purrstrap list`.
3. Sign with the developer key ([chapter 03](03-trust-and-keys.md)); then publish to the dev server with `dev publish` ([chapter 09](09-purrstrap.md)) and install from the Developer Portal on the device.
4. The developer kit in [sdk/](../sdk/) wraps these steps as the `purr` command; start with [sdk/docs/13-first-hour.md](../sdk/docs/13-first-hour.md).

An app is compiled freestanding with `-nostdlib`, so a call to any function that is not in the app fails at link time. The manifest's `[catcalls]` and `[permissions]` sections are read by the build tool but are not stored in the signed image, because the container header has no room ([AppSDK/SPEC.md](../AppSDK/SPEC.md) section 5). The runtime reads what an app is from its own `purr_app_table2_t`.

Host tests: `test_runtime` (273 checks on the manager), `test_perm`, `test_appmgr`. The catcall implementations themselves run only on the device or in the VM ([chapter 10](10-host-tests.md)).

## A first app, step by step

[Apps/notes/notes.c](../Apps/notes/notes.c) is the smallest real example of a UI app. Reading it in this order shows the whole pattern:

1. **The entry function.** `notes_entry()` returns a pointer to a static `purr_app_table2_t`. It is marked `used` so the linker keeps it. Its `base.abi_version` is `PURR_APP_ABI_VERSION_2`, and the table also sets `memory_needed` (16 KB), `kind` (2, a UI app) and `flags` (0).
2. **`on_create`.** The app reads `env->catcall_get` and asks for the tables it needs: `console`, `ui` and `storage`, all at version 1. If `ui` or `storage` is NULL it prints a line to the console and returns -1, which fails the launch cleanly.
3. **Drawing.** It calls `ui->label`, `ui->row`, `ui->textarea` and the others. It never holds a pointer to an LVGL object, only the small integer handles the calls return. A bad handle is refused by the system.
4. **`on_event`.** Every touch or key becomes a `PURR_APPEV_UI` event whose `arg` is the widget handle and whose `name` is `click`, `changed`, `action` or `back`. The app compares the handle with the ones it kept and acts.
5. **`on_pause`.** When the app leaves the front, the editor saves what was typed, because a frozen or evicted app may not get another chance.
6. **Files.** Notes are written through `storage`, into the app's own folder, one file per note.

The app uses no libc. Where it needs a string compare or a copy, it carries its own few lines, which is why such files have a block of tiny helpers near the top. The mini-library that would replace them is designed and not built ([AppSDK/SPEC.md](../AppSDK/SPEC.md) section 2).

A constraint that surprises people: **callbacks must return quickly.** The `on_create` above does its first screen and returns; anything slow (a download, a long file copy) must be started as a job and finished when a later event arrives, the way the Device Manager and the Store use `start()` and the `job` event.

## What goes wrong and how to fix it

| Symptom | Cause | Fix |
|---|---|---|
| `catcall_get` returns NULL for `ui` | Not an app of kind `ui`, or the OS is older than the version asked | Declare kind `ui` in the table; ask for the lowest version you need |
| App crashes the first time it uses a `switch` | A jump table was relocated against the writable alias | Fixed 2026-11-01 in `purrstrap/scripts/modules.py`; rebuild any app or driver built before then that has a `switch` of about five dense cases |
| `*_ASK` forever | The app did not wait for `PURR_APPEV_PERM` | Handle the event and call again |
| `apps` shows "broken: tomllib" | Python older than 3.11 | Use Python 3.11 or newer |
| Permission asked in `on_create` is refused | The starting app was counted as background | Fixed 2026-11-01; UI apps starting up count as foreground |

## What is not finished

- **SDK side of the runtime** is tracked in [plan5.md](../plan5.md) (it absorbed plan6 phase D). The mini-library (`printf`, strings, containers), privilege enforcement, and four more templates in [AppSDK/SPEC.md](../AppSDK/SPEC.md) are not built; the one template is the v1 stub.
- **`ctl`, `net`, `audio`, `radio`, `gps`** are not served ([plan6.md](../plan6.md), [plan9.md](../plan9.md)).
- **Plan and code differ** on `ui`: the design section of [plan9.md](../plan9.md) describes v1, and its later notes mention v3. The code is at v5 (`purr_catrt_ui.c`); the comment on the struct in the header still says 4.
- **Not verified on a device:** `files`, `devices`, `store`, `messaging`, `contacts` and `game` have run in the VM at most. The 2026-11-05 list of new catcalls is "compiled and host-tested, not run on a device" ([plan9.md](../plan9.md)).
- **Open design:** the on-screen keyboard (a text area asks the OS, or the shell), and a `shell` catcall for a future Terminal app with a hard refusal list ([plan9.md](../plan9.md)).

Plan references: [plan6.md](../plan6.md) (runtime and the catcall design), [plan9.md](../plan9.md) (`ui` and `files`), [plan5.md](../plan5.md) (SDK), [plan10.md](../plan10.md) (`power`), [plan8.md](../plan8.md) (`store`).
