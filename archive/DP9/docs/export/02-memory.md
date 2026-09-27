# Persistent memory (auto-memory system)

This is a copy of the assistant's cross-session memory for this project, as
of export time. The live copy lives at
`~/.claude/projects/-home-PastorCatto-Projects-PURR-OS-ESP32/memory/` on
whatever machine runs Claude Code — that directory is NOT part of this repo
and will need to be rebuilt (or manually restored from this file) on a new
machine/profile. Claude Code will regenerate individual memory files over
time as it re-learns these facts through conversation, but starting fresh
loses this accumulated context unless it's re-told or this file is used to
seed it back in.

## Index

- **OOBE node identity** — Meshtastic name/hw_model hardcoded on purpose,
  deferred to future v1.0 first-run setup flow.
- **Perf debug mode idea** — future cupcake/MiniWin perf-debugging mode,
  talking stage only, scope not decided.

## oobe-node-identity

`mesh_router_encode_nodeinfo()` in `source/modules/meshtastic/mesh_router.c`
hardcodes the Meshtastic node identity: `long_name = "PURR-XXXXXXXX"`,
`short_name = "PRR"`, `hw_model = HELTEC_V3` (wrong for T-Deck Plus,
cosmetic only — wrong device icon on real Meshtastic clients).

**Why:** User confirmed live testing works correctly with these placeholder
values (visible to other real nodes, hw_model shows as Heltec V3 as expected
given the hardcoded value) and explicitly does not want this fixed now — it's
planned to be part of a future out-of-box-experience (OOBE) onboarding flow
for PURR OS v1.0: on first flash, walk the user through a setup process
(presumably including device name/identity configuration) before dropping
them to the desktop.

**How to apply:** Don't proactively "fix" the hardcoded name/hw_model as a
bug — it's known and intentionally deferred. When OOBE/first-run setup work
actually starts, this is one of the things it should configure. Don't
re-raise this as an issue unless the user brings up OOBE/setup work
specifically.

## perf-debug-mode-idea

Idea floated, still at the talking stage — not implemented, no design
decided yet: add a "performance bugs" debug mode to both the cupcake and
MiniWin UI shell modules (`source/modules/cupcake/`, `source/modules/miniwin/`),
meant to help diagnose UI performance issues (jank, slow redraws, etc.) in
those two shells specifically.

**Why:** user wants to shift focus toward Meshtastic-side work next, but
flagged this as something to come back to and add more debug tooling for
later.

**How to apply:** When next asked to build this out, don't assume scope —
clarify whether it means an on-screen debug overlay (FPS/frame time/heap/task
stack high-water marks), serial-log-only perf output, or both. That question
was asked once already and the answer at the time was "just make a note for
now," so the actual design is still open.

## Standing instructions not (yet) captured as discrete memory files

These have come up repeatedly in-session but weren't formally saved as
memory entries — worth re-establishing on a fresh machine/session if the
user doesn't repeat them explicitly:

- **Switch the UI to WinCE/MiniWin, avoid the Arduino kernel at all
  costs.** `tdeck_plus_arduino` is a compile-only verification target,
  never flashed to real hardware. `tdeck_plus` (module-based MiniWin
  desktop) is the only target flashed to the physical device.
- Every WinCE-desktop-shell change this session was mirrored into two
  near-duplicate files: `source/modules/miniwin/miniwin_wince_desktop.c`
  (the real, flashed target) and
  `source/kernel/kernel_tdeck_plus_arduino/wince_shell.cpp` (compile-only
  parity target). Keep doing this unless told otherwise.
