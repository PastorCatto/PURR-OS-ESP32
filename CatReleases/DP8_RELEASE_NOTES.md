# PURR OS — Developer Preview 8

**43 commits since DP7 · 73 files · +7,741 / −674 lines**

DP8 shipped in two passes. The first brought in the mobile UI generation — the
Mochi and Tabby backends and the `systemui` split. The second, described here, is
a performance and stability pass.

It began as "Mochi feels laggy" and turned into discovering that most of what we
believed about the display path had been measured with a broken instrument.
Almost every number below replaced an earlier number that was wrong — including
two decisions that were made, shipped, and later reversed on better data.

Two features shipped: **Speed Demon** (one app takes the whole machine) and the
**menu primitive** (a real list contract, so apps stop scattering buttons).

---

## Headline numbers

All measured on T-Deck Plus hardware, not estimated.

| | before | after |
|---|---|---|
| Scrolling a list | 6.2 fps | **9.7–10.1 fps** |
| Mean frame under load | 128 ms | **60 ms** |
| Idle repaint | 75–81 ms every 200 ms | **≈ none** (1 warning in 55 s) |
| Screen fills in bands on redraw | yes | **no** |
| Speed Demon round trips | never completed one | **4 consecutive, memory flat** |
| Memory lost per Speed Demon cycle | ~9,000 B | **~440 B** |
| Firmware optimisation | `-Og` | **`-O2`** (~3.3× frame time) |

---

## Display and rendering

**The black rectangular blocks are gone.** Two hardware limits, both found on
device: the SPI transfer-length register is 18 bits, so a single transaction
caps at 32,768 bytes — a full frame is 153,600 and was silently rejected, leaving
whatever was in GRAM. And a PSRAM-resident source forced a per-transaction
internal bounce buffer that failed at size. The driver now stages through
internal DMA RAM and chunks.

**`-O2` instead of `-Og`** — the single largest win of the release, ~3.3× on
frame time, and it was not in any version of the original plan.

**Asynchronous flush with real double buffering** (`catcall_display` v1 → v3).
`push_pixels()` blocked for the entire transfer, so rendering and flushing never
overlapped. There is now an optional `push_pixels_async` / `flush_done_cb` pair;
drivers that skip it keep the blocking path unchanged. The transfer is chunked
with ping-pong staging, so a caller pushing a whole frame still returns
immediately.

**Off-screen composition — no more band-by-band fill.** LVGL sizes a render pass
by how many rows fit the draw buffer, and flushed each band the moment it
finished, so a full redraw visibly painted top-to-bottom over ~150 ms. Bands are
now assembled into a full-screen mirror and the finished frame is sent once.

The obvious alternative — a full-screen draw buffer — was tried and rejected on
measurement: 153,600 bytes cannot sit in a 32 KB data cache, and mean frame time
went 23 ms → 44 ms.

**Shadows were the scroll bottleneck** — 40–50% of frame time. Nothing in the UI
code sets `shadow_width`; every shadow comes from LVGL's default theme, which is
why the existing "UI effects" toggle had never made a measurable difference. It
reaches them now, and the shadow corner cache is available as the alternative
(equal performance, keeps the look).

**Idle repaint eliminated.** The system UI rewrote the whole status row five
times a second whether or not anything had changed — LVGL's style setters do not
compare old against new, and the notification and task boxes were destroyed and
rebuilt object-by-object every tick. All three now compare first.

**Tearing: closed as a hardware limit.** Confirmed against LilyGo's own header —
the T-Deck Plus does not break out the ST7789's TE pin, and scanline readback is
impractical on a bus shared with the radio. Pushing fewer pixels was measured and
does not help: a 40% reduction changed nothing, because any asynchronous write
that crosses the scan produces a seam regardless of size. **Recorded as a
hardware requirement for the next board rather than an open task.**

---

## Speed Demon (formerly "game mode")

One app takes the entire machine: launcher, system UI, mesh stack and radios are
unloaded before it starts and restored when it exits, reclaiming **12.8–15.7 KB
of internal DRAM** on a device that has ~31 KB free.

Apps opt in with **one line** — `.speed_demon = 1` in the module registration.
`app_manager` owns both halves, so an app cannot forget the exit; the failure
mode for that would be a device with no launcher and no way back.

It had never completed a single clean round trip before this release. It now does
four consecutive cycles with memory flat. Six distinct faults, each found from a
decoded backtrace or a measurement rather than by reading code:

| Fault | Found by |
|---|---|
| UI catcall never released on unload — the restored backend hit its own "something else owns the screen" guard and came back with no render task | a log line |
| SPI bus race on entry (~90% failure) — the bus lock is held per *device*, not per task | backtrace |
| PSRAM stack in the health watchdog — writing NVS disables the flash cache, making its own stack unreachable | backtrace |
| Relaunch silently inert — `state == APP_STATE_RUNNING` never cleared for an app that exits itself | log |
| 8 KB leaked per unload — `xTaskCreateWithCaps` paired with plain `vTaskDelete` | per-module accounting; the leak appeared to move between modules, which is what gave it away |
| `mesh_persist_task` deleted mid-SD-write, corrupting newlib reentrancy state | backtrace |

**Documented in [docs/15_SpeedDemon.md](../docs/15_SpeedDemon.md).** Note it is
for pre-linked `.claw` apps only — Lua tiers cannot use it, because the flag
lives in a module header they do not have and Speed Demon unloads `lua_runtime`
itself.

---

## UI and contracts

**Menu primitive — `catcall_ui` v8.** A real grouped-list contract
(`purr_win_menu_*`) with sections, values and headers, so apps stop hand-rolling
button grids. Mochi renders it as an iOS-style grouped table; backends without a
native implementation fall back to label + list automatically. Settings' category
picker and General page are migrated.

**Shared display path between backends.** Every fix above originally landed in
Mochi only, and Cupcake — a near-copy of the same file — silently had none of
them. It now lives in one shared unit both use. Bringing Cupcake level also
exposed two of its own bugs: a draw buffer 1.6× the data cache, and a second
buffer allocated unconditionally that could never help while its flush was
synchronous.

**Notification shade** is half-height with a grab handle; overpulling drops to
the lock screen. The duplicate Control Center is gone.

**Flat buttons** in row layouts — no shadow, no gradient.

**Adaptive springboard grid**, derived from panel size instead of a hard-coded
4×2.

---

## Kernel and reliability

**Crash strikes reset on a new build.** The strike counter lives in NVS and
halted boot at 5. During this session it reached 13/5 and bricked boot three
separate times, each needing a manual `esptool erase_region 0x9000 0x6000` —
knowledge that lives nowhere on the device. Worse, it caused a full
misdiagnosis: a boot loop after a config change looked like the config change's
fault, when the device would have halted regardless. Strikes are evidence about a
specific build, so they now clear when the firmware's ELF hash changes.

**Module tasks unsubscribe from the task watchdog before deletion** — four
modules did not, and the watchdog then panicked ~4 s into every Speed Demon
session.

**Bounded module unloads**, so a `deinit()` that never returns cannot leave the
device with the UI already gone and nothing coming back.

**NVS settings load before the UI is built**, fixing a load-order race where the
About page reported a version no baked artifact matched.

---

## Instrumentation

Worth calling out, because it changed conclusions rather than just adding logs.

**The frame metric was wrong twice.** First it only logged frames slower than a
threshold — a censored sample whose "median" was the median of the slow frames
only, and which made a build with 239 uniformly mediocre frames look better than
one with a fast majority. Then it counted no-op loop iterations, overstating fps
by ~20×.

**"fps" was the wrong question.** `RENDER-fps` divides frames by the whole
window, idle included, so it describes neither the idle nor the moving case. The
release adds **BUSY-fps**, measured from the gap between consecutive rendered
frames — which is what "how fast is it while I'm dragging" actually means.

> **A caveat that belongs in the notes:** identical builds measured 9.7 and 7.7
> BUSY-fps in different sessions, because the number depends on what is being
> scrolled. Compare ranges, not single runs.

---

## Known issues

- **Screen tearing during motion.** Hardware limit on T-Deck Plus — no TE pin.
  Will not be fixed on this board.
- **MagicMac does not build.** It has no `app.pcat`, so it is never scanned into
  the firmware. `docs/08_Exclusives.md` previously implied otherwise and has been
  corrected; its sections are design intent, not shipping behaviour.
- **MagiDOS is the host shell only.** The INT 21h shim, `C:` mapping and the
  bundled DOS app are not in this release.
- **Per-module memory accounting is still compiled in** (`[mem] unload` /
  `[mem] restore` log lines) while the last Speed Demon asymmetry is confirmed.
- **Milkbar icons and the Tab5 grid are unverified on hardware.**

---

*Built and measured on LilyGO T-Deck Plus (ESP32-S3, 8 MB PSRAM, ST7789 320×240).
Other devices in this release are built but not hardware-verified this cycle.*
