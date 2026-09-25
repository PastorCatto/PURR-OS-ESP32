# UI backends v1 (archived)

Every UI backend PURR OS shipped before the "console-only test" phase of
the protected-processes/Unix-boot rewrite, moved here as-is — not deleted,
not modified, just set aside. Same convention this repo already uses for
`archive/WIP_shells/`, `archive/SDK_legacy/`, `archive/releases_legacy/`.

## What's here

`modules/`: `mochi`, `cupcake`, `cardstack`, `kittenui`, `lvgldebug`,
`tabby`, `cheetah`, `nougat`, `pounce`, `blackpurr`, `miniwin`, `systemui`
— every implementation of `catcall_ui_t`/the portable `purr_win_*` widget
API (`source/kernel/catcalls/purr_win.h`, which stays in the live tree),
plus `systemui` (status bar/lock/login chrome hosted by several of the
backends above).

## Why

T-Deck, T-Deck Plus, and Tab5 (the three devices this rewrite is actively
verifying) now have a real Unix-style boot → protected login console
(`source/modules/purr_console/`, `purr_console_login/`,
`purr_kernel_start_protected()`) that needs to be proven solid on its own,
with nothing else running, before a new UI gets built on top of it. Every
app that calls `purr_win_*` (`calculator`, `diagnostics`, `fileman`,
`milkbar`, `msn`, `oobe`, `reticulum_app`, `server_manager`, `settings`,
`terminal`, plus `lua_runtime`'s Lua window-API bindings) does so
unconditionally with no compile guard — ESP-IDF links every selected
component into one image, so any of them being selected while no backend
provides those symbols fails the WHOLE firmware's link, not just that
app. Those apps/modules are therefore **deselected** (not moved) in the
three in-scope devices' `device.pcat` files for now — their source is
untouched, they're just not part of the current build.

Every other device (`cyd*`, `waveshare169`, `jc3248w535`, `heltec`,
`tdeck_plus_pounce`, `tdeck_plus_arduino`, the probe/test kernels) still
selects one of these backends and was deliberately left untouched — their
builds will fail until either a UI returns or their own `device.pcat` gets
the same deselection treatment. Not an oversight: this rewrite's scope has
been T-Deck/T-Deck Plus/Tab5 only, with Pounce and the Arduino kernel
already ruled out as deprecated.

## What comes back, and how

Phase 3 of the rewrite builds a genuinely new UI — not a restoration of
any of these as-is. The one design decision already settled for that
phase: `systemui` (status bar, lock screen, login UI) becomes a
`purr_kernel_start_protected()` process itself — never silently
strike-disabled the way a misbehaving app can be — while the launcher and
regular apps stay ordinary, disable-able P2/P3 modules. Nothing here
should be treated as a starting point for that without re-evaluating it
against the new boot/login contract this rewrite establishes first.
