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

| Metric | How | **Baseline** | After 1 | After 2 | After 3 |
|---|---|---|---|---|---|
| Actual SPI clock (kHz) | `spi_device_get_actual_freq()` | **80 000 — not clamped** | | | |
| `lv_timer_handler()`, idle (ms) | warn hook at 16 ms | **18–19, every 200 ms** | | | |
| `lv_timer_handler()`, shade + switcher (ms) | same | **median 24, p95 165, max 290** | | | |
| SPI transactions per flush — idle / active | counter in `push_pixels` | **34 / 66** | **6 / 7** | | |
| µs per flush, avg — idle / active | counter in `push_pixels` | **3251 / 10 444** | **1446 / 7465** | | |
| px per flush — idle / active | counter in `push_pixels` | **2740 / 18 387** | 2699 / 19 140 | | |
| µs per transaction | derived | **~79 idle, ~102 active (only ~18 is wire)** | n/a — no longer the bottleneck | | |
| Full-buffer flush (µs) | max, active windows | **~15 800** | **~10 100** | | |
| Transfer failures | log scan | n/a | **0** | | |

Captured 2026-07-25 on a physical T-Deck Plus.

**Idle**, two 120-flush windows: `41 trans / 3712 µs / 13909 µs max` (settling),
then `34 trans / 3251 µs / 3809 µs max` (steady).

**Active** — notification shade open/close and the multitasking app switcher, the
two worst offenders, five consecutive windows:

```
66 trans/flush, 10444 us/flush avg, 15924 us max, 18387 px/flush
57 trans/flush,  8851 us/flush avg, 15842 us max, 14937 px/flush
68 trans/flush, 10799 us/flush avg, 15710 us max, 19028 px/flush
65 trans/flush, 10562 us/flush avg, 15805 us max, 18692 px/flush
63 trans/flush,  9815 us/flush avg, 15707 us max, 17071 px/flush
```

Handler distribution over the same 60 s, n=313:

```
   0- 19ms : 124  ############################################################
  20- 39ms :  37  #####################################
  40- 59ms :   6  ######
  60- 79ms :   9  #########
  80- 99ms :   7  #######
 100-119ms :  14  ##############
 120-139ms :  12  ############
 140-159ms :  83  ############################################################
 160-179ms :  18  ##################
 180-199ms :   2  ##
 280-299ms :   1  #
```

**This is bimodal, and that matters.** The 0–19 ms mass is idle (F8's periodic tick
plus light frames). The 140–159 ms cluster — 83 samples, 27% of all frames — is a
distinct mode: **~6.7 fps while the shade or switcher is animating.** That cluster,
not the average, is what "absolutely brutal" is.

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

**Confirmed by measurement, and worse than estimated.** Steady-state idle:

```
[perf] 120 flushes: 34 trans/flush, 3251 us/flush avg, 3809 us max,
                    2740 px/flush, row-by-row
```

2740 px is 5480 bytes — at the measured 80 MHz that is **0.55 ms of wire time**. The
flush takes **3.25 ms**. So:

> **~83% of display time is per-transaction overhead, not data transfer.**
> ~95 µs per transaction, of which only ~18 µs is the bus actually moving bytes.

The original estimate assumed 20–50 µs of overhead; it is nearer 77 µs. That makes
the bulk path worth **more** than first thought, not less — collapsing 34 transactions
to ~4 should take a 3.25 ms flush to roughly 0.9 ms, about **3.8×**, because these
flushes are overhead-dominated rather than wire-dominated.

The largest flush seen during boot was **13.9 ms**, consistent with a full-width
80-line buffer (~83 transactions). Three of those per full-screen redraw ≈ 35–40 ms,
i.e. a **~25 fps ceiling before any blending cost** — which is the scroll lag.

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

### ~~F6 — The requested SPI clock may not be the achieved one~~ — **DISPROVEN**

Measured 2026-07-25: `requested 80000 kHz, actual 80000 kHz`. No clamp.

The concern was that MOSI=41 / SCLK=40 are not SPI2 IOMUX pins on the ESP32-S3, so
the signals route through the GPIO matrix — which is true, but it is evidently
carrying 80 MHz fine here. **Good news:** none of the other estimates need doubling,
and there is no easy win hiding in the clock. The time is going somewhere else.

Closed. No action.

### F8 — Idle costs an 18–19 ms stall every 200 ms *(found during Step 0)*

Not predicted; it fell out of the measurement. At idle, on the springboard, with
nothing being touched:

```
W (30273) mochi: lv_timer_handler() took 19ms (tick=2360)
W (30496) mochi: lv_timer_handler() took 18ms (tick=2400)
W (30719) mochi: lv_timer_handler() took 18ms (tick=2440)
```

Every 40 ticks, exactly, forever. `mochi_module.c`'s loop runs
`mochi_springboard_tick()` on `++tick % 40 == 0`, and the **next** `lv_timer_handler()`
call is the one that pays — so the tick dirties something (near-certainly the status
bar clock) and the following handler spends 18–19 ms redrawing it.

Between those spikes the handler stays under the 16 ms threshold and never logs. So
this is not general slowness — it is a specific, periodic, four-to-five-times-a-second
hitch that exists **while the device is doing nothing at all**.

Worth being precise about the ownership: the *trigger* is Mochi's, but the *cost* is
the driver's — 18 ms to repaint a clock is only expensive because of F1/F2. Steps 1
and 2 should shrink this without anyone touching Mochi. If it survives them, it
becomes a Mochi invalidation-scope question and belongs to whoever owns that.

### F7 — `LV_COLOR_16_SWAP` is unset, so the driver swaps in software

Not set in `CoreOS/sdkconfig_tdeck_plus`, so it defaults to 0 and
`st7789_push_pixels` byte-swaps every pixel on the CPU, on the render task.

At 18,387 px per active flush that is a real cost inside the measured flush time —
roughly 0.5 ms per flush by rough cycle count, and it is neither wire time nor
transaction overhead, so it will *not* be removed by Steps 1 or 2.

### F9 — The LVGL draw buffers live in PSRAM *(hypothesis, found during Step 0)*

`mochi_hal.c` allocates both draw buffers with `MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA`.
That is correct and deliberate for the *flush* — GDMA can read PSRAM, and these are
large. But the draw buffer is not only a DMA source: **it is LVGL's render target**,
and LVGL blends into it with heavy read-modify-write.

PSRAM is roughly an order of magnitude slower than internal DRAM for that access
pattern. A full-screen composite — wallpaper, then a translucent panel over it,
then `lv_layer_top()` chrome — touches the buffer several times over, every pass a
PSRAM read plus a PSRAM write.

**Why this is a live hypothesis and not a footnote:** the arithmetic does not close
without it. A ~150 ms frame contains only ~47 ms of measured SPI. Something is
consuming the other ~100 ms, and a full-screen software blend of 76,800 px should
not cost that on a 240 MHz S3 unless memory bandwidth is the wall.

Cheap test: allocate a smaller buffer in **internal DRAM** (e.g. 320×40 = 25.6 KB)
and re-measure. More flushes, but each blend runs at internal-RAM speed. If frame
time drops sharply, this is the single largest win available and it is nearly free.

Caveat: this lives in `mochi_hal.c`. It is a buffer-placement decision rather than a
UI-design one, but it is still Mochi's file — needs a call on ownership.

### F10 — `lv_tick_inc(5)` lies about how much time passed

`mochi_module.c`'s loop does `lv_tick_inc(5)` then `vTaskDelay(pdMS_TO_TICKS(5))`
around `lv_timer_handler()`. That is only correct if the handler costs ~0 ms.

Measured, one iteration costs **10–11 ms at idle and 150 ms+ under load**, while
LVGL is told 5 ms elapsed. So LVGL's clock runs at under half real speed at best,
and ~1/30th real speed during a shade animation.

Two consequences, both matching the reported feel:

1. **Animations run in slow motion.** A 300 ms authored transition plays over
   multiple real seconds. This is separate from low frame rate and would persist
   even at 60 fps.
2. **LVGL cannot adapt.** It has no idea it is behind, so its own frame-skipping
   and refresh-period logic never engages.

The fix is to feed real elapsed time — measure the loop with `esp_timer_get_time()`
and pass the true delta, or give LVGL an `esp_timer`-backed tick source. Nearly
free, and independent of every other item here. Also in Mochi's file.

---

## Step 0 conclusion — recalibration

Step 0 did its job, including the part where it contradicts the plan.

**Confirmed:** F1 and F2, and worse than estimated — ~80–100 µs per transaction
against ~18 µs of wire time.

**Disproven:** F6. The clock is fine.

**Found:** F8, F9, F10 — none of which were in the original analysis.

**The uncomfortable part.** A worst-case frame is ~150 ms. Measured SPI inside it is
~47 ms, or **about 31%**. A perfect outcome from Steps 1 and 2 — collapsing 66
transactions to ~4 — takes that 47 ms to roughly 16 ms, and the 150 ms frame to
about **120 ms**. That is a real improvement and it is *not enough on its own*.

This does not change the order. Step 1 is still first: it is isolated, driver-only,
testable without touching Mochi, and it is ~100% of the idle hitch in F8. But it
does change what should be claimed for it, and it means **the DP8 gate cannot be
"Step 1 and 2 are done"** — it has to be a frame-time target.

**Proposed gate:** worst-case `lv_timer_handler()` under 60 ms (≈16 fps) with the
140–159 ms cluster gone. Reaching that needs F9 and F10 as well as Steps 1–2, and
F9 is likely the largest single contributor.

Both F9 and F10 live in Mochi files. Neither is a UI-design change — one is buffer
placement, one is a clock bug — but both need a call on who lands them.

---

## Ordered plan

### Step 0 — Instrument. Do not skip.

Zero risk, and it decides whether the arithmetic above is even right.

- [x] `spi_device_get_actual_freq()` on the display handle — resolves **F6**
- [x] Drop the `lv_timer_handler()` warn threshold in `mochi_module.c` from 50 ms to 16 ms
- [x] Counters in `push_pixels`: transactions per flush, µs per flush, px per flush
- [x] `esp_timer` added to the `st7789` component's `REQUIRES` (needed by the counters)
- [x] Builds clean — `purrstrap build tdeck_plus`, "Project build complete"
- [ ] **Flash and capture the baseline table above, idle and during a sustained page drag**

All of the above is **temporary** and comes out at the end of Step 2. Each piece is
commented as such at its site so none of it survives by accident.

#### What to look for on the serial monitor

**Once at boot**, from the driver's SPI setup:

```
W (nnn) st7789: [perf] SPI clock: requested 80000 kHz, actual NNNNN kHz  <-- CLAMPED
```

The `<-- CLAMPED` marker only prints if the achieved clock is below what was
requested. If it appears, **F6 is confirmed** and every throughput estimate in this
document doubles.

**Every 120 flushes**, aggregated so the logging cannot dominate what it measures:

```
W (nnn) st7789: [perf] 120 flushes: NN trans/flush, NNNN us/flush avg,
                       NNNN us max, NNNNN px/flush, row-by-row
```

How to read it:

| Field | Meaning |
|---|---|
| `trans/flush` | SPI round-trips per LVGL flush. Row-by-row this should track flush height (~80) plus 3 command writes. The bulk path should collapse it to ~4. **This is the number Step 1 is trying to move.** |
| `us/flush` | Wall time inside `push_pixels`. Compare against the pure wire time implied by the actual clock — the gap is per-transaction overhead, which is what Step 2 attacks. |
| `px/flush` | Guards against misreading the other two. A small dirty rect is cheap for real reasons and must not be mistaken for a win. |
| trailing tag | `row-by-row` or `BULK` — confirms which path is actually live, rather than assuming perf mode took. |

**From the render loop**, now at a 16 ms threshold instead of 50 ms:

```
W (nnn) mochi: lv_timer_handler() took NNms (tick=NNNN)
```

At 50 ms this was a tripwire; on an already-slow UI it fired constantly with no
shape. At 16 ms (one 60 fps frame) it becomes a distribution — how far past budget
each call lands, and how that changes between idle and an active drag.

### Step 1 — Bulk buffer — ✅ **PASSED** (2026-07-25, confirmed on hardware)

Resolves **F1**. Display confirmed visually clean; zero transfer failures in a
65-second log.

**Both original hypotheses were wrong.** It was not cache alignment and not buffer
lifetime. Two real causes, found only by turning perf mode on and reading the log:

- [x] **Cause 1 — the 18-bit transfer-length register.** `SPI_LL_DMA_MAX_BIT_LEN`
      is 2^18 bits = **32,768 bytes**. A full frame is 153,600 and even one 80-line
      LVGL flush buffer is 51,200, so both were rejected with *"txdata transfer >
      hardware max supported len"*. **A rejected transfer sends nothing**, so that
      region of GRAM kept its old contents — the black blocks. Small dirty rects
      stayed under the limit, which is exactly why the trackball cursor still
      worked while opening an app did not.
- [x] **Cause 2 — PSRAM source forced a bounce buffer.** With the bulk buffer in
      PSRAM, `spi_device_queue_trans()` could not DMA from it directly and tried to
      allocate a per-transaction **internal-RAM copy**. At 32 KB that fails with
      `ESP_ERR_NO_MEM` — same outcome, an unsent region — and it meant every pixel
      was being copied twice for nothing.
- [x] **Fix.** The bulk buffer now lives in **internal DMA-capable RAM** and is one
      *chunk* in size (16 KB, walking down to 4 KB if internal DRAM is short).
      `push_pixels` walks the source rect chunk by chunk, so no transaction can ever
      exceed the hardware limit regardless of rect size, there is no bounce buffer,
      and the byte-swap writes into internal RAM. The panel cannot see the seams:
      after RAMWR the ST7789 consumes a continuous pixel stream.

**Result at ~19,000 px/flush:** 66 → **7** transactions (9.4× fewer),
10,444 → **7,465 µs** (1.4× faster), 0 failures.

**Why 1.4× and not the predicted 3.8×** — and this is the important part. Breaking
down the remaining 7,465 µs:

| Component | µs | Share |
|---|---|---|
| Wire time at 80 MHz | 3830 | **51%** — irreducible, this is the floor |
| Transaction overhead | ~560 | **7.5%** — was ~5600. Solved. |
| Byte-swap + reading source from PSRAM | ~3075 | **41%** — this is F7 + F9 |

Transaction overhead was real and is now gone, but it was never the whole story.

### Replan after Step 1

- **Step 2 (async flush + `queue_size = 2`) is DEMOTED off the DP8 gate.** It
  attacks the 7.5%. The problem it was going to solve is already solved, and it is
  the most invasive change on the list. Keep it for later if the floor ever matters.
- **F7 (`LV_COLOR_16_SWAP`) is PROMOTED.** It was deferred past DP8 on a guess that
  it was worth "a millisecond or two". Measured, it is a large part of that 41%.
  That guess was wrong and the deferral was wrong with it.
- **F9 goes next**, because it hits the 41% twice over: LVGL blends faster *and*
  this driver's byte-swap stops reading its source out of PSRAM.

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
