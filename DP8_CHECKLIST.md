# PURR OS — Developer Preview 8 Checklist

> ## 🔒 RELEASE GATE
>
> **DP8 does not ship until this document is complete and fully satisfied.**
>
> The version string is already `v1.0.0-dp8` and the CHANGELOG entry is written,
> but nothing has been baked — `CatReleases/` still ends at DP7. Mochi is the
> headline feature of this release, and shipping it in its current state would put
> the tearing and scroll lag in front of everyone at once.
>
> Steps 0–4 are the gate. Step 5 is explicitly deferred past DP8 (see its entry).
> Step 6 is an audit whose *findings* are required before release, not its fixes.

**Working tracking doc. Scope: Mochi / T-Deck Plus rendering performance.**

Community response to Mochi's look is good. The problems are lag while scrolling,
bad screen tearing during page swipes, and a suspicion — correct, see below — that
the translucent chrome makes both worse.

This document exists because these symptoms have been chased for several days
without landing. Every fix below is individually invisible: you can fix something
real and the UI still feels laggy, because two other things dominate. That is what
makes this class of problem loop.

> **The one rule: one change, one measurement.** Never change two of these at once.
> Each step is accepted or reverted on whether it moves the numbers in the baseline
> table — not on whether the screen subjectively feels better.

Target device is `tdeck_plus` and only `tdeck_plus`. It is the flagship and the
only board on hand. Other drivers are Step 6, deliberately last, and are an audit
rather than a rewrite because there is no hardware to verify them on.

---

## Baseline measurements

Fill this in at Step 0. Re-measure after every accepted step. If a step doesn't
move a number, revert it — it isn't the problem, and keeping it just adds noise to
the next measurement.

| Metric | How | Baseline | After 1 | After 2 | After 3 |
|---|---|---|---|---|---|
| Actual SPI clock (kHz) | `spi_device_get_actual_freq()` | | | | |
| `lv_timer_handler()` p95, idle (ms) | existing warn hook, threshold → 16ms | | | | |
| `lv_timer_handler()` p95, during page drag (ms) | same | | | | |
| SPI transactions per flush | counter in `push_pixels` | | | | |
| µs per flush | counter in `push_pixels` | | | | |
| Full-screen redraw (ms) | derived | | | | |

---

## Findings

All verified by reading the code, not inferred from symptoms.

### F1 — Perf mode is off, so every flush is row-by-row

[`kernel_tdp_boot.c:582`](source/kernel/kernel_tdeck_plus/kernel_tdp_boot.c#L582)
calls `st7789_set_perf_mode(false)`, disabled because the bulk path produced
visible black rectangular chunks. So every flush falls into the row loop at
[`st7789.c:576-582`](source/drivers/display/st7789/st7789.c#L576-L582), and each
row is a separate `calloc()` → `queue_trans` → block → `get_trans_result` →
`free()`.

A full-screen redraw is **240 SPI transactions and 240 malloc/free pairs**, all on
the LVGL render task.

Arithmetic at a nominal 80 MHz: a 320px row is 640 bytes ≈ 64 µs of wire time, but
per-transaction overhead on an ESP32-S3 is realistically 20–50 µs — so roughly half
efficiency, call it ~25 ms of SPI for one full frame. The bulk path is one
153,600-byte DMA at ~15 ms. **Worth about 1.7× on its own**, plus 240 heap
operations per frame disappear.

### F2 — `queue_size = 1` means zero pipelining

[`st7789.c:476`](source/drivers/display/st7789/st7789.c#L476). Every transaction is
queue-then-block. The SPI peripheral idles while the CPU byte-swaps the next row;
the CPU idles while DMA runs. Neither ever overlaps.

Note this interacts with the timeout-reclaim logic in `spi_transmit_bounded` —
that code explicitly reasons about a queue depth of 1, so raising it is not a
one-line change.

### F3 — The second draw buffer currently buys nothing

[`mochi_hal.c:88-97`](source/modules/mochi/mochi_hal.c#L88-L97). `flush_cb` calls
`push_pixels` synchronously and only then calls `lv_disp_flush_ready()`. LVGL's
dual-buffer scheme only helps when the flush is asynchronous — return immediately,
signal ready from the DMA completion ISR.

As written, `s_buf2` is **50 KB of PSRAM for zero benefit**.

### F4 — Tearing: nothing reads the TE pin

The ST7789 exposes a tearing-effect output that pulses at vertical blank. The
driver never touches it. Writing GRAM while the panel scans out tears by
construction, and partial-area flushes during a drag are the worst case.

Open hardware question before any code: **is TE actually wired on the T-Deck Plus?**

### F5 — Transparency: the suspicion is correct, and there are two stacked costs

1. **Blending is a slower code path.** Opaque fills take a fast memset-ish route;
   anything with `LV_OPA < 255` goes through `lv_draw_sw_blend_normal` doing
   per-pixel mixing.
2. **Translucency defeats occlusion.** LVGL normally skips redrawing what's hidden.
   A translucent status bar and dock over a full-screen wallpaper force the
   wallpaper to be redrawn underneath and then blended — and because systemui
   composites on `lv_layer_top()`, invalidating the springboard drags the chrome
   above it into the same redraw.

During a page drag that is: full-screen wallpaper redraw + blend + row-by-row SPI,
every frame.

### F6 — The requested SPI clock may not be the achieved one

[`kernel_tdp_boot.c:515`](source/kernel/kernel_tdeck_plus/kernel_tdp_boot.c#L515)
asks for 80 MHz. But MOSI=41 / SCK=40 are **not** SPI2 IOMUX pins on the ESP32-S3,
so the signals route through the GPIO matrix, which realistically tops out near
40 MHz. If the achieved clock is 40, every estimate above doubles and that alone
explains much of the symptom.

### F7 — `LV_COLOR_16_SWAP` is unset, so the driver swaps in software

Not set in `CoreOS/sdkconfig_tdeck_plus`, so it defaults to 0 and
`st7789_push_pixels` byte-swaps every pixel on the CPU, on the render task.

---

## Ordered plan

### Step 0 — Instrument. Do not skip.

Zero risk, and it decides whether the arithmetic above is even right.

- [ ] `spi_device_get_actual_freq()` on the display handle — resolves **F6**
- [ ] Drop the `lv_timer_handler()` warn threshold at [`mochi_module.c:67`](source/modules/mochi/mochi_module.c#L67) from 50 ms to ~16 ms
- [ ] Temporary counters in `push_pixels`: transactions per flush, µs per flush
- [ ] Capture the baseline table above, idle and during a sustained page drag

### Step 1 — Bulk buffer. Test the two hypotheses separately, cheapest first.

Highest leverage, and the code is already written — it just has a bug. Resolves
**F1**.

- [ ] **1a — alignment.** `heap_caps_aligned_alloc(64, …)` for `s_bulk_buf`, and
      round the DMA length up to a cache-line multiple. PSRAM DMA writeback wants
      cache-line-aligned extents, and a misaligned tail corrupts as rectangular
      blocks — which matches the reported symptom. Small and safe. **If this alone
      fixes it, stop here.**
- [ ] **1b — buffer lifetime.** Only if 1a doesn't fix it.
      `spi_transmit_bounded()` deliberately does not free a timed-out transaction
      and returns `false`, but `st7789_push_pixels` **ignores the return value**,
      releases the bus and returns. The next flush then overwrites `s_bulk_buf`
      while a live DMA may still be reading it. With `s_row_buf` that window is
      640 bytes and invisible; with a full-frame buffer it is exactly "black
      rectangular chunks."
- [ ] Re-enable `st7789_set_perf_mode(true)` and re-measure
- [ ] Expected: ~1.7× on full-screen redraw, 240 heap ops/frame gone

### Step 2 — Async flush + `queue_size = 2`

The structural fix that makes the second draw buffer real. Resolves **F2** and
**F3**.

> **Depends on Step 1.** With the bulk path there is exactly one transaction per
> flush to signal `lv_disp_flush_ready()` from. Row-by-row would mean chaining 80
> completions. Doing these in the other order is how another few days get lost.

- [ ] Move `lv_disp_flush_ready()` to the DMA completion path
- [ ] Raise `queue_size` to 2 and add a second bounce buffer
- [ ] Rework `spi_transmit_bounded`'s reclaim logic for a queue depth > 1
- [ ] Re-measure

### Step 3 — Re-measure, then decide about transparency

Steps 1 and 2 together could plausibly halve frame time. The community likes the
look — don't spend it until the numbers say you must. Resolves **F5**, or
declares it a non-issue.

- [ ] Re-measure during a page drag
- [ ] If still short: make dock / status-bar backgrounds opaque **during an active
      drag only**, restoring translucency on release
- [ ] If still short: skip wallpaper redraw under fully-covered chrome

### Step 4 — TE pin (answer the hardware question first)

Resolves **F4**.

- [ ] Check LilyGo's `utilities.h` / schematic for whether TE is wired
- [ ] If wired: gate flushes on the TE pulse
- [ ] If not wired: **close this** — the only remedy is short flushes, which Steps
      1 and 2 already delivered

### Step 5 — `LV_COLOR_16_SWAP` — **deferred past DP8**

Resolves **F7**. Worth only a millisecond or two, and it is a coordinated change
across the driver, both asset converters, and every backend on this device — the
wallpaper and icons invert until all of it lands together.

**Not a DP8 gate.** Landing an all-or-nothing asset-format change on the way out
the door is how you ship inverted wallpapers. Revisit after DP8 is out.

- [ ] Deferred — do not start before DP8 ships

### Step 6 — Sweep the other display drivers (audit, not rewrite)

There is no hardware to verify these on — 4 of 12 targets have ever been on a
bench. So the output is a written audit and possibly a shared SPI-transaction
helper, **not** speculative edits to drivers nobody can test.

Grep each for the same three tells:

| Driver | `calloc` per transaction | `queue_size = 1` | Synchronous flush | Notes |
|---|---|---|---|---|
| `st7789` | yes | yes | yes | the subject of this document |
| `ili9341` | | | | CYD ×3 |
| `axs15231b` | | | | jc3248w535, QSPI |
| `ssd1306` | | | | Heltec, I2C not SPI |
| `st7123` | | | | Tab5, MIPI-DSI — different class entirely |

**Gate:** the table must be filled in before DP8 ships. Acting on it must not be.
If another driver has the same bug, that is worth *knowing* at release time even
if the fix waits for hardware.

---

## Release gate — DP8 bake

Once Steps 0–4 are satisfied and Step 6's table is filled in:

- [ ] Full erase + flash of `tdeck_plus`, confirm no tearing on a sustained page swipe
- [ ] Confirm the numbers hold after a warm reboot, not just a fresh flash
- [ ] Re-check `heltec` still builds and boots — the ST7789 changes don't touch it,
      but it is the only other target that has been on a bench
- [ ] Update the dp8 CHANGELOG entry with what actually landed from this document;
      its current date (2026-07-24) is the commit date and will need to become the
      real bake date, since every other entry is anchored to `baked_at`
- [ ] `purrstrap bake` / package into `CatReleases/DP8/`
- [ ] Tick the corresponding hardware items in `PURR_OS_1.0_CHECKLIST.md` §1 — a
      real bench session is exactly what that list is short of

---

## Out of scope

- Anything on a device other than `tdeck_plus`, beyond the Step 6 audit
- The Mochi visual design — it is well received; this is purely about making it
  fast enough to keep
- `purrstrap clean`, the `module.pcat` migration, `CONFIG_PURR_UI_LVGL` — tracked
  in `PURR_OS_1.0_CHECKLIST.md`

## Note on the existing driver

The ST7789 driver is carefully written. The bus-acquire bracketing and the
bounded-transfer reasoning are both correct and non-obvious, and they close real
hangs that were diagnosed on hardware. Nothing here should be read as unpicking
that. Every problem above is "safe but serialised" — the shape you get from
correctly fixing a hang and then never revisiting what the fix cost.

---

*Analysis performed by Claude Opus 5 in agentic/auto mode, v1.0.0-dp8.*
