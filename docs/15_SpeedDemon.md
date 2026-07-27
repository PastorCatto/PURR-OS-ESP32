# Speed Demon

*Formerly "game mode" — renamed 2026-07-27. The old `purr_game_mode_*` names and
`modules/game_mode/` are gone; nothing else changed.*

Speed Demon gives one app the entire machine. It unloads the launcher, the system
UI, the mesh stack, the radios and every other non-essential service, hands the
display and input straight to the app, and puts all of it back when the app
exits.

It exists because a T-Deck Plus running the full OS has roughly **31 KB of
internal DRAM free**, and an emulator or a game needs framebuffers, and headroom
that simply is not there while a launcher, an LVGL backend, a mesh stack and two
radios are resident. Speed Demon reclaims about **12.8 KB of internal DRAM** and,
just as importantly, gives the app uncontended use of the SPI bus and the CPU.

---

## Using it — one line

An app declares that it needs the machine. It does **not** call anything.

```c
PURR_MODULE_REGISTER(mygame) = {
    .magic       = PURR_MODULE_MAGIC,
    .abi_version = PURR_MODULE_ABI_VERSION,
    .module_type = PURR_MOD_APP,
    .name        = "mygame",

    .speed_demon = 1,        // <- this is the whole opt-in

    .init        = mygame_init,
    .deinit      = mygame_deinit,
};
```

`0` or omitted means a normal app that runs alongside everything else.

`app_manager` reads the flag and owns both halves of the lifecycle:

1. Before your `init()` runs, it enters Speed Demon.
2. When your app reports that it has finished, it exits Speed Demon and the OS
   comes back.

Centralising it is deliberate. An app that entered by hand could forget the
matching exit, and the failure mode for that is a device with no launcher and no
system UI — nothing on screen and no way back except a reboot.

### What your app must do

**Beat the heartbeat.** At least every 5 seconds:

```c
purr_speed_demon_heartbeat();
```

Two consecutive misses (10 s of silence) is treated as a hang, and the OS
restores itself rather than leaving you with a dead device. Call it from your
main loop; if a single frame can take longer than 5 s, call it mid-frame too.

**Report your exit.** Immediately before your task deletes itself:

```c
app_manager_notify_exited("mygame");
vTaskDelete(NULL);
```

This is what triggers the restore. It is also what lets the app be launched a
second time — `app_manager` refuses to relaunch anything it still believes is
running, and an app that owns the panel has no window to re-show, so skipping
this makes the second launch silently do nothing.

**Do not call `purr_speed_demon_enter()` or `_exit()` yourself.** In particular
never enter from `init()`: `init()` can run on the UI render task, and entering
unloads the UI backend — deleting the very task making the call.

---

## What survives, and what does not

Kept, by **type** rather than by a name list — a name list silently stops
covering new modules as they are added, and nothing notices:

| Kept | Why |
|---|---|
| `PURR_MOD_DRIVER` | display, touch, input, radio, GPS, battery. The app needs the display and input; the rest are inert once nothing drives them, and unloading a driver would tear down a catcall other code still holds pointers to. |
| `app_manager` | it is the thing performing the unload, and the thing that will restore everything afterwards. |
| the app itself | obviously. |

Everything else goes. On a T-Deck Plus that is 12 modules: the UI backend,
`systemui`, `meshtastic`, `msn_relay`, `wifi_mgr`, `bt_mgr`, `lua_runtime`,
`homebase`, `pairing`, `proximity`, `proximity_rpc`, `app_manager_remote`.

Order matters and is not incidental. The unload list is built by walking the
registry in **reverse load order**, because load order already respects
dependencies — so unloading in reverse means nothing is torn out from under
something still running. Concretely: the UI backend must go before `systemui`,
because the backend's render task calls `purr_systemui_tick()` every frame.

---

## Entering and leaving, from the user's side

**To enter:** launch an app that declares `.speed_demon = 1`. On a stock
T-Deck Plus that is **MagiDOS**. A splash appears listing each service as it is
unloaded — that screen exists because the launcher is already gone by then and
something has to be on the panel.

**To leave:** exit the app the way the app defines. In MagiDOS, type `EXIT` at
the prompt. A second splash appears, counting the services back in, and the
launcher returns.

**If the app hangs:** do nothing. The heartbeat stops, and after two missed
beats the OS restores itself.

---

## Guarantees, and what they cost

**Bounded unloads.** Each module's `deinit()` runs on a helper task with a 3 s
ceiling. A `deinit()` that never returns leaves that one module loaded and the
sequence continues, rather than leaving the device with the UI already gone and
nothing coming back. A module left loaded is recorded and deliberately *not*
"restored" later — it never left.

**The UI lock is held across teardown.** Restoring a UI backend starts its render
task, which begins drawing immediately; holding the lock keeps it queued behind
the restore splash instead of interleaving with it on the panel and on the SPI
bus.

**The display driver drains before synchronous use.** The SPI bus lock is held
per *device*, not per task, and the async flush path holds the bus across a
return. Without draining, the splash's `fill_rect` could release a bus that a
render still owned — see `async_wait_idle()` in `st7789.c`. This is what caused
a ~90% failure rate entering Speed Demon before it was fixed.

---

## Known state (DP8)

Verified on hardware: **three consecutive round trips**, entering and exiting
cleanly each time.

| | value |
|---|---|
| internal DRAM reclaimed on entry | ~12.8 KB |
| drift per round trip | ~470 bytes |
| beat interval / miss tolerance | 5 s / 2 beats |
| per-module unload ceiling | 3 s |

**Open:** `homebase` frees nothing on unload but costs ~2.9 KB to restore — its
`deinit()` does not release what its `init()` allocated, and it is the likely
home of the residual drift. Per-module accounting is currently compiled in
(logged as `[mem] unload` / `[mem] restore`) and stays until that is closed.

**History worth keeping**, because each of these was found from a decoded
backtrace rather than by reading code, and each looked like something else first:

- The UI catcall was never released on unload, so a restored backend hit its own
  "something else owns the screen" guard and came back registered but
  uninitialised — no render task, and a UI-unresponsive strike seconds later.
- `run_bounded()` paired `xTaskCreateWithCaps()` with plain `vTaskDelete()`,
  leaking 8 KB per unload. It presented as a different module leaking on every
  run, which is what gave it away — it was the task doing the unloading.
- `mesh_persist_task` was deleted mid-SD-write, corrupting newlib's per-task
  reentrancy state. It now stops on a flag and deletes itself at a safe point.
