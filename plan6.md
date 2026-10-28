# plan6: the app runtime, API v2, and a Hyprland-style control surface

**Source of truth for this work.** Re-read after any context compaction. Update the Status table and
the Session log as work happens. Started 2026-10-26. It feeds plan5 (the developer kit): the kit can only
offer what the runtime has, and today the app ABI is a stub (`on_create`, `on_destroy`, one `console_print`).

## Goal

Build the real app system that `AppRuntime/SPEC.md` (v0.3) describes, and give it a control surface in the
spirit of Hyprland: **text configuration, rules, binds, dispatchers, queries and an event stream.**

- Extend the app API: the full lifecycle (create, resume, pause, save-state, low-memory, event, destroy) and
  a `catcall_get` dispatcher with real catcalls (console, app, sched, storage, system, an event bus).
- Build the manager: instances and states, foreground and background, switching, admission, the
  out-of-memory killer, teardown, unresponsive detection.
- Control it the Hyprland way, so a person at a shell, a config file, or a privileged app can do the same things.

## What "Hyprland style" means here (owner to confirm)

Hyprland is configured by a plain text file of `keyword = a, b, c` lines, controlled by **dispatchers**
(`hyprctl dispatch <name> <args>`), inspected by **queries** (`hyprctl clients`, `activewindow`, `-j` for JSON)
and observed through an **event stream** of `name>>data` lines. The mapping:

| Hyprland | PURR |
|---|---|
| `hyprland.conf` | `/etc/apps.conf` (system) and the user's `~/.config/apps.conf` |
| `general:option = value` | `general:max_apps = 8`, `general:reserve_kb = 64`, ... |
| `windowrule = float, class:^(foo)$` | `rule = background, name:sync*` / `rule = persistent, class:daemon` |
| `bind = SUPER, Q, killactive` | `bind = SUPER, Q, close active` |
| `exec-once = app` | `exec-once = launcher` (started once at boot, in order) |
| `hyprctl dispatch focuswindow x` | `purrctl dispatch focus <app>` |
| `hyprctl clients` / `activewindow` | `purrctl apps` / `purrctl activeapp` (both take `-j`) |
| socket2 events `openwindow>>...` | `purrctl events`, one line each: `openapp>>3,notes` |

"Create, destroy, and a bunch of other jazz" is the lifecycle plus the dispatchers. If the owner meant
something else by "Hyprland style" (for example the tiling layout of windows), say so: layout belongs to the
`ui` catcall and is not part of this plan.

## App API v2 (`purr_app_abi.h`, `PURR_APP_ABI_VERSION` 2)

The entry point still returns a table. v1 apps (two callbacks, one print function) keep loading: the runtime
sees `abi_version == 1` and adapts.

```
purr_app_table_t {            (v2)
    abi_version,
    on_create(env, state, len)   first start; state saved from the last run, or NULL
    on_resume()                  came to the foreground
    on_pause()                   left the foreground
    on_save_state(buf, cap)      optional; returns the length written
    on_low_memory(level)         1 = low, 2 = critical: free what you can
    on_event(ev)                 bus events the app subscribed to, timers, posted events
    on_destroy(reason)           user closed / update / killed for memory / unresponsive / shutdown
}
purr_app_env_t { env_version, console_print (kept for v1), catcall_get(name, min_version) }
```

Catcalls handed out by `catcall_get` (each a versioned table; an app that asks for one it was not granted, or
the board lacks, gets NULL):

| Catcall | Calls |
|---|---|
| `console` | `write`, `print`, `read_line` (foreground only) |
| `app` | `info`, `close_self`, `request_focus`, `post(name, data)`, `subscribe(prefix)`, `unsubscribe` |
| `sched` | `uptime_ms`, `sleep_ms`, `timer_start(ms, repeat)`, `timer_cancel` (timers arrive as events) |
| `storage` | open, read, write, seek, close, mkdir, remove, list, stat, rooted at the app's own folder |
| `system` | uptime, free heap, largest free block, battery, time |
| `event` | `emit`, `subscribe` (the bus behind the text event stream) |
| `ctl` | `dispatch(line, out, cap)`, `query(line, out, cap)` (privileged apps only) |

## The manager (`purr_runtime.c`, in CoreOS, host-testable)

Pure logic over a host interface (load, unload, deliver, now, free memory, log, state storage). Nothing in it
touches FreeRTOS, so it is tested on the PC with fakes (`AppRuntime/SPEC.md` section 13) and then wired to the
device. On the device each instance gets a task and a queue, and `deliver` posts to the queue and waits for the
callback to return, with a timeout (a stuck app is marked unresponsive and force-stopped).

- **States:** starting, foreground, background, suspended, stopping, stopped (`AppRuntime/SPEC.md` section 4).
- **Admission:** memory needed plus the reserve must fit; otherwise stop the least important apps, retry, and
  fail without touching the foreground app.
- **OOM killer:** `low` sends `on_low_memory`; `critical` kills victims (suspended, then background, least
  recently used first, largest first among equals; never system, `persistent`, or the foreground app),
  with hysteresis and a minimum interval between kills.
- **Teardown:** mark stopping, deliver `on_destroy`, wait out in-flight catcalls (timeout, then force), release
  timers, handles and the memory block, remove from the registry, emit `closeapp`.
- **Saved state:** opt-in; saved whenever an app leaves the foreground, capped (4 KB), handed to `on_create`.

## The control surface

**Dispatchers:** `open <app> [args]` (alias `exec`), `close <ref>`, `kill <ref>`, `focus <ref>`, `cycle`,
`foreground`/`background`/`suspend`/`resume <ref>`, `pin`/`unpin <ref>`, `post <ref> <event> [data]`.
`<ref>` is an id, a name, `active`, `next` or `prev`.

**Queries:** `apps`, `activeapp`, `memory`, `rules`, `binds`, `version`; add `-j` for JSON.

**Events** (`name>>data`): `openapp>>id,name`, `closeapp>>id,name,reason`, `focus>>id,name`,
`state>>id,name,from,to`, `lowmemory>>level`, `killed>>id,name,reason`, `unresponsive>>id,name`,
`config>>reloaded`, plus whatever apps `emit` on the bus. Subscribers are registered sinks (the shell, the
system log, apps through the `event` catcall).

**Config** (`/etc/apps.conf`): `general:*` options, `rule = effect[ arg], matcher`, `bind = MODS, key,
dispatcher[, args]`, `exec-once = app`. Rule effects: `background`, `persistent`, `suspend`, `memory <kb>`,
`focus`, `nofocus`. Matchers: `name:<glob>`, `class:<daemon|command-line|ui>`. Reloadable at run time; a bad
line is reported, never fatal.

**`purrctl`** is the shell command (`purrctl dispatch ...`, `purrctl apps`, `purrctl events`, `purrctl reload`).

## UI model: apps do not know which UI they are on (owner decision, 2026-10-26)

Apps are written against an abstract surface and the `ui` catcall (`UI/SPEC.md`: widgets plus a canvas,
events through the app's normal queue), never against LVGL or a particular shell. The **card stack** (the
LVGL card shell: Home card, edge panels, carousel/overview/minimize gestures) is the primary backend, and the
text console is a second one, so an app written once runs on either.

The way the two meet the manager: **a UI is just a client of the control surface.** The card stack subscribes
to the event stream (`openapp`, `closeapp`, `focus`, `state`, `killed`) to create, reorder and remove cards,
and a gesture calls a dispatcher (swipe to dismiss = `close`, tap a card = `focus`, minimize = `background`).
The text shell does the same through `purrctl`. So the manager never imports a UI, and a new UI needs no
manager change. Each running app is a card whose title and icon come from its manifest and whose content area
is the app's own surface.

**How it is tested (by me, in the emulator):**
1. Host tests for the manager (fakes), run with `components/coreos/test/host/run.py`.
2. The text console on the VM image: `purrctl ...` typed through the emulated keyboard, output read from the
   serial log. This is the easy path to automate and is the first thing checked on the device.
3. The card stack: drive it with injected touch (the headless runner gets a `HEADLESS_TOUCH` script) and read
   the screenshot. No change of UI is needed; the text path covers logic and the card path covers the look.

## Phases

| # | Phase | Result |
|---|---|---|
| A | `purr_app_abi.h` v2 (tables, reasons, event types) and the manager core: instances, states, launch, close, focus, admission, saved state | host tests pass |
| B | Rules, binds, config parser, dispatchers, queries, event stream, OOM killer, unresponsive detection | host tests pass |
| C | Device runtime: a task and queue per app, the catcall implementations, console routing, `purrctl`, boot-time `exec-once` | a sample app runs, switches and gets killed in the VM |
| D | SDK side: v2 template(s), a header-only helper layer (`purr_app.h`) over the catcalls, `apps build` checks the v2 ABI, spec updates (`AppRuntime`, `Catcalls`, `AppSDK`) | `purr new app` produces a v2 app |
| E | Permissions hooks (which catcalls a class gets), then the `ui` catcall (own plan) | later |

## Status

| # | Item | State |
|---|---|---|
| A | ABI v2: `purr_app_abi.h` additions (reasons, events, `purr_app_table2_t`, env `catcall_get`), `purr_catcalls.h` (console, app, sched, storage, system, event, ctl); manager core in `purr_runtime.c` | done; host tests pass |
| B | rules, binds, config parser, dispatchers, queries (`-j` JSON), event stream, OOM killer with hysteresis and kill interval, unresponsive reporting, `exec-once` | done; `test_runtime.c` 206 checks pass, full CoreOS host suite passes |
| C | device runtime (`main/purr_catrt.c`: a task and queue per app, manager task, console/app/sched/system/event catcalls), platform glue and `purrctl` in `commands.c`, boot self-test (`purr_rt_selftest.c`, `Apps/rtdemo/`) | working in the emulator: 39 of 40 self-test steps pass (the 40th is a script ordering mistake); signed apps run, unsigned and tampered apps are refused; saved state, timers, events, config reload, binds and admission checked; `purrctl` typed at the shell not yet tried; `storage` and `ctl` catcalls not built |
| D | SDK side | todo |

## Open questions

- Whether "Hyprland style" is the control and config model above, or also window layout (see above).
- The key a `bind` uses as `SUPER` on a T-Deck (it has Alt, Sym and Mic keys, no Super).
- Importance classes beyond `persistent`; whether an app may declare itself more important.
- Which catcalls a background app may use (permissions chunk).
- State versioning for apps that update to a build that cannot read the old state.

## Session log

- 2026-10-26 (even later): `exec-boot` (essential apps, at boot) and `exec-once` (everything else, after the
  first login: `purr_catrt_user_logged_in()` from `purr_session_started()` in main.c) added and host-tested;
  default `/etc/apps.conf` documents both.

- 2026-10-26 (later still): phase C works on the device in the emulator (see Status). Owner asked that apps
  never run unsigned: enforced by `purr_image_verify` (key, role, hash and signature, in every secure mode),
  surfaced as `PURR_RT_E_UNTRUSTED`, a `refused>>name,untrusted` event and a clear dispatcher message, with
  host and VM tests. Owner plans to switch UIs and shared a Windows 10 Mobile LVGL shell: see plan7.md. The card
  stack integration planned above is therefore on hold; the runtime's event/dispatcher seam is what a new shell uses.

- 2026-10-26 (later): phases A and B written and tested on the host. New files: `coreos/include/purr_runtime.h`,
  `coreos/src/purr_runtime.c` (added to the coreos CMake list), `coreos/include/purr_catcalls.h`,
  `coreos/test/host/test_runtime.c`. Owner decided apps are UI-agnostic with the card stack as the primary backend
  (see the UI model section). Owner is demoing the emulator, so no emulator work until told; phase C needs it.

- 2026-10-26: owner asked to extend the app API first and build the full system, "Hyprland style". Read
  `AppRuntime/SPEC.md` and `Catcalls/SPEC.md` (most of the design is already there). Wrote this plan.

### Merged into plan5 (owner, 2026-10-28)
The app runtime is part of the developer kit, so plan6's remaining work (phase D, the SDK side) is tracked at the bottom of plan5 and
plan6 stays as the design reference. Added to the runtime: background lifecycle. An app that is not shown is unloaded, or frozen
(tasks suspended, memory kept) when it must stay open; a frozen app only runs for its alarm window (plan10).

### Background cap (owner, 2026-10-28)
See plan5: 5 resident background apps total, 3 user and 2 service (daemon) apps; services wake on a 30-60 s plan10 alarm on hardware.
