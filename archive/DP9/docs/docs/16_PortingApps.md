# 16 — Porting Apps to PURR OS

For bringing **existing code** — a game, an emulator, an engine, a C library —
onto PURR OS. If you are writing something new, start at
[12_AppAPI.md](12_AppAPI.md) instead; this document assumes you have a codebase
that already works somewhere else and wants to keep working.

Everything here is drawn from real ports in this tree, chiefly PrBoom (DOOM,
`source/apps/exclusive/doom/`) and MagiDOS. Where a number appears it was
measured on T-Deck Plus, not estimated.

---

## 1. Pick a tier first

The tier decides what your code is allowed to touch, and it is not easily
changed later.

| Tier | Language | Gets | Port this way when |
|---|---|---|---|
| `.meow` | Lua | `win.*`, `sd.*` | The app is small and UI-shaped |
| `.paws` | C | `purr_win.h`, `sd.*` | It draws through windows and needs speed |
| `.claw` | C | Everything — full `purr_kernel_*` | **Anything with its own renderer** |

**A ported game or emulator is almost always a `.claw`.** It has its own frame
loop, its own framebuffer and its own idea of input, so the windowing API is not
what it wants — it wants the panel. That also makes it a candidate for Speed
Demon (§7) — **unless it uses the network**, which is an outright exclusion. See
§7.1 before you design around it.

Note the hard limit: **Lua tiers cannot use Speed Demon**, structurally — see
[15_SpeedDemon.md](15_SpeedDemon.md). If your port needs the whole machine, it
must be a `.claw`.

---

## 2. Lay out the component

```
source/apps/exclusive/<name>/
  app.pcat            manifest — modulestrap/catstrap discover the app by this
  CMakeLists.txt      an ESP-IDF component
  <name>_app.c        YOUR code: entry, task, lifecycle
  compat/             YOUR code: the platform seams (§4)
  <upstream>/         VENDORED third-party source, unmodified
```

`app.pcat` is what makes the directory visible to the build:

```ini
name        = "doom"
version     = "0.1.0"
tier        = "claw"
author      = "id Software / PrBoom team — ported to PURR OS"
description = "DOOM (PrBoom). Runs under speed demon; reads a WAD from /sdcard/doom/."

idf_requires = "esp_common driver freertos speed_demon boot_splash fatfs vfs"
```

Then turn it on for a device:

```bash
python3 modulestrap/modulestrap.py enable <name> <device>
python3 purrstrap/purrstrap.py build <device>
```

Since DP8, `device.pcat` decides what compiles — a component nothing references
is not built at all. See [07_Build_Tools.md](07_Build_Tools.md).

---

## 3. Vendoring third-party source

**Keep upstream code unmodified and in its own directory.** The point is that
re-vendoring a newer version is a directory copy rather than a merge.

Two things make that practical:

**Glob it, don't list it.** A hand-maintained list of 70 files goes stale on the
first re-vendor and fails confusingly.

```cmake
file(GLOB DOOM_PRBOOM_SRCS ${CMAKE_CURRENT_LIST_DIR}/prboom/*.c)
```

**Silence warnings on vendored files only.** This project builds with
`-Wall -Wextra -Werror=all`. Decades-old C will not survive that, and is not
going to be made to.

```cmake
set_source_files_properties(
    ${DOOM_PRBOOM_SRCS}
    PROPERTIES COMPILE_OPTIONS "-w"
)
```

Use `-w`, and be aware of what does *not* work — both were tried on the DOOM
port and both failed:

- **A list of `-Wno-error=<name>`.** GCC 13 against 1999 C produced a new batch
  every rebuild (`duplicate-decl-specifier`, `nonnull`, `address`,
  `dangling-else`, `sizeof-pointer-div`, …). Ninja stops at the first failure,
  so no single build ever reveals the whole set, and the list would be specific
  to one compiler version.
- **Blanket `-Wno-error`.** IDF's own `-Werror=all` wins and the warnings come
  back as errors.

`-w` wins regardless of flag order because the diagnostic is never emitted, so
there is nothing to promote. Apply it **per-source**, not to the component: your
own code in `compat/` and `<name>_app.c` must stay under full warnings, and
that is precisely what the per-source form buys.

**Copy more than `*.c` and `*.h`.** The DOOM port failed on
`r_drawflush.inl: No such file or directory` because `.inl` files were missed.
Check the upstream directory for `.inl`, `.dat`, `COPYING`, `AUTHORS`.

**Record provenance and licence** in a `README.md` beside the code — origin,
version, licence of each directory, and what you changed. This repo is GPL-3.0;
GPL-2.0-**or-later** code (which is what the DOOM source release is) is
compatible, plain GPL-2.0-only is not. Never commit game assets or ROMs.

---

## 4. The seams you have to implement

Most ports need the same five, and they are the whole job. **§4.2 implements
all of them for you** — read this table to know what is being translated, then
use the layer rather than writing it again.

| Seam | PURR OS gives you | Section |
|---|---|---|
| Display | `catcall_display_t` | §5 |
| Input | `catcall_input_t` | §6 |
| Files | Standard `stdio` on `/sdcard` | §4.1 |
| Time | `esp_timer_get_time()`, `gettimeofday` | — |
| Memory | `heap_caps_malloc` | §9 |

Delete the upstream platform layer rather than adapting it. On the DOOM port the
originals for SPI LCD and PSX gamepad were thrown away entirely; keeping them
would have meant porting IDF v3 APIs to v5 for code that was going to be
replaced anyway.

### 4.1 Files

`/sdcard` is a normal FATFS mount — `fopen`/`fread`/`opendir` all work.

**Scan a directory rather than hardcoding a filename.** The DOOM port looks for
`/sdcard/doom/*.wad` so that a shareware WAD, a cut-down one, or DOOM2 all work
by being dropped in.

**You cannot `mmap` a file on FAT.** If upstream memory-maps its data (PrBoom
does — every lump read is a pointer into mapped storage), load the file into
PSRAM once at startup and make the map function pointer arithmetic. That keeps
upstream's zero-copy design intact and takes the SD card off the shared SPI bus
for the whole run.

### 4.2 Use the translation layer — `purr_port.h`

**Do not implement the seams from scratch.** `source/apps/common/purr_port.h`
already does, and it encodes every trap in §5–§9.

This is the same wrapper technique PURR OS already uses twice. A foreign library
keeps its own API, and a small HAL fills that library's hooks with callbacks that
translate to catcalls — `mochi_hal.c` and `cupcake_hal.c` do it for LVGL
(`lv_disp_drv_t::flush_cb` → `push_pixels`, `lv_indev` read callbacks →
`poll_event`), and `miniwin` does it for MiniWin. `purr_port.h` is that technique applied
to applications: **your port keeps its own platform API and implements it in
terms of these calls.**

Header-only, like `modules/common/purr_lv_flush.h` and for the same reason — no
lifecycle, nothing to register, so no `.pcat` and no build-system change. Include
it with a relative path.

```c
#include "../../common/purr_port.h"

static purr_port_t s_port;

// 8 = paletted (expanded through pal[] on present), 16 = direct RGB565
if (!purr_port_open(&s_port, 8)) { /* no display, or no memory */ }

purr_port_set_palette_rgb888(&s_port, playpal, 256);

// render into s_port.fb8 (or .fb16), then:
purr_port_present(&s_port);

purr_port_key_t k;
while (purr_port_key_next(&s_port, &k)) {
    // k.down == false is a SYNTHESISED release — see §6
}

purr_port_heartbeat();          // throttled to 1Hz, safe in a hot loop
purr_port_close(&s_port);
```

| Call | Replaces | Handles for you |
|---|---|---|
| `purr_port_open` | display lookup + `malloc` | panel size, internal→PSRAM fallback, logs which it got |
| `purr_port_set_palette_rgb888` | palette conversion | native-endian RGB565, all 256 entries |
| `purr_port_present` | expand + `push_pixels` | unrolled 8bpp→RGB565 expansion |
| `purr_port_key_next` | `poll_event` | **synthesised key-up**, repeat swallowing |
| `purr_port_heartbeat` | `purr_speed_demon_heartbeat` | 1 Hz throttle; no-op when windowed |
| `purr_port_fail_screen` | — | on-screen error + key-to-exit |
| `purr_port_find_keyboard` | `purr_kernel_input()` | resolves by capability, not index |

A fix in `purr_port.h` reaches every port. That is the whole point: it is the
same divergence `purr_lv_flush.h` was created to stop between the two UI
backends, where a display fix landed in Mochi and silently did not exist in
Cupcake.

It is a translation layer, **not a framework** — every function is something you
call, never something that calls you. Your port keeps its own frame loop and its
own event loop; those are why it is a `.claw` in the first place.

The rest of this document (§5–§9) describes what the layer is doing underneath.
Read it when you need something the layer does not cover, or when a port is
behaving oddly and you need to know what to blame.

---

## 5. Display

There is **no framebuffer to map**. `catcall_display_t` is push-only: you render
into your own buffer and hand it over.

```c
const catcall_display_t *d = purr_kernel_display();

display_info_t info;
d->get_info(&info);                  // never hardcode 320x240

d->push_pixels(0, 0, w, h, rgb565);  // blocks until sent
```

Three things that are not obvious:

**Pixels are RGB565, native-endian. Do not byte-swap.** The st7789 driver swaps
on the way to DMA. Upstream code that pre-swaps for its own SPI driver — as
PrBoom's palette did, `lcdpal[i] = (v>>8)+(v<<8)` — will swap twice and produce
colours that look like a corrupt palette rather than an endianness bug.

**`push_pixels_async` exists** and is optional. It returns immediately and calls
a completion callback, so you can convert or render frame N+1 while N is still
going out. `stride` lets you push a sub-rectangle of a larger buffer without
flattening it. Both are NULL on drivers that do not implement them — fall back
to `push_pixels`.

**Measured ceiling:** a full 320×240 frame is 153,600 bytes, ~15.4 ms of wire
time at 80 MHz. About 55–60 fps on display push alone, before your renderer.

---

## 6. Input

```c
const catcall_input_t *in = purr_kernel_input_at(i);
input_event_t ev;
while (in->poll_event(&ev)) { ... }   // non-blocking
```

**Do not use `purr_kernel_input()`.** It returns the *first* registered input,
which on T-Deck Plus is the trackball, not the keyboard. Resolve by capability —
a keyboard implements `set_backlight`, a trackball does not:

```c
static const catcall_input_t *find_keyboard(void)
{
    int n = purr_kernel_input_count();
    for (int i = 0; i < n; i++) {
        const catcall_input_t *in = purr_kernel_input_at(i);
        if (in && in->poll_event && in->set_backlight) return in;
    }
    return NULL;
}
```

**The bbq20 keyboard never sends key-up.** It only ever emits
`INPUT_EVENT_KEY_DOWN` — it polls the currently-pressed key over I²C every 20 ms
and posts an event when the byte is non-zero. Any port whose input model has
press *and* release — which is every game — has to synthesise the release:

> Treat a key as held while events keep arriving; emit the release once none has
> arrived for ~120 ms (six poll intervals).

This degrades sensibly either way: if held keys repeat, movement is continuous;
if each press reports once, you get a step per press. Stilted, not broken.

---

## 7. Speed Demon

If your port owns the screen, opt in with **one line** — it calls nothing:

```c
PURR_MODULE_REGISTER(mygame) = {
    .speed_demon = 1,
    ...
};
```

`app_manager` then unloads the launcher, system UI, mesh stack and radios
*before* your `init()`, and restores them when you exit. Full detail in
[15_SpeedDemon.md](15_SpeedDemon.md). Four rules matter when porting:

**Your task stack must be INTERNAL, not PSRAM.** Exiting writes NVS, writing NVS
disables the flash cache, and a task on a PSRAM stack then faults on its own
stack. Use `xTaskCreatePinnedToCore` (internal by default); do not route the
stack to PSRAM to save internal DRAM.

**Beat the heartbeat, at least every 5 s.** Ten seconds of silence is treated as
a hang and reboots the device.

```c
purr_speed_demon_heartbeat();
```

This is the single most likely thing to bite a port, because it bites during
*startup*, not during play. DOOM's first hardware launch died here: the WAD load
takes 9,964 ms (3.1 MB at ~312 KB/s off SPI SD) with no beat, and the engine was
killed 170 ms after reaching `R_InitData` — everything working, watchdog
unconvinced. Beat inside any long load loop, and during engine init.

Prefer hooking the beat to *something that indicates progress* rather than to a
timer. DOOM beats from its lump-read function: it fires when the engine is
actually advancing and falls silent if it genuinely wedges. A periodic timer task
would keep beating through a real hang and disable the watchdog entirely.

**Every exit path must be recoverable.** The launcher is already unloaded, so an
app that logs an error and returns leaves a black screen and no way back short of
a power cycle. Put failures on screen and wait for a key:

```c
purr_splash_show("MYGAME", 1);
purr_splash_status("No data files found - press any key to exit");
```

`boot_splash` is the right tool: always compiled, no LVGL behind it (the UI
backend is one of the things just unloaded), and Speed Demon already uses it.

**Report your exit by name**, on every path out:

```c
app_manager_notify_exited("mygame");
```

This is what restores the OS. Skip it and the app can be launched exactly once
per boot — `app_manager` still believes it is running, and the second tap
silently does nothing.

### 7.1 Speed Demon kills the network — do not use it for a connected app

**A networked app must not declare `.speed_demon = 1`.**

Speed Demon's `is_kept()` spares only drivers, apps, `driver_manager` and
`app_manager`. `wifi_mgr` is `PURR_MOD_SYSTEM`, so it is unloaded like anything
else — visible in any Speed Demon log:

```
W speed_demon: [mem] unload  wifi_mgr              -8668 bytes
```

For an offline game that is free memory. For an SSH session, a tile downloader or
anything holding a socket, it tears the stack out from under a live connection
the moment the app starts.

There is no flag to exempt it, and adding one would be the wrong fix: the radios
are a large part of what Speed Demon reclaims, and an app that needs the machine
that badly is the last thing that should also be holding a TCP connection.

**So connected ports run as ordinary apps** — alongside the launcher and system
UI, drawing through `purr_win.h` rather than owning the panel. You keep the
network and you give up the whole screen. That is the trade, and it is not
negotiable in the current design.

If a port genuinely needs both — full screen *and* the network — the honest
options are to split it into an online phase (windowed: fetch, sync, download)
and an offline phase (Speed Demon: render), or to leave it windowed.

---

## 8. Two worked archetypes

Both of these come from real requests and both hit constraints §1–§7 do not
cover on their own.

### 8.1 A terminal / SSH client

Runs **windowed, not Speed Demon** (§7.1). Three further constraints, none of
them obvious until you are already building:

**80 columns does not fit, and pretending otherwise is the wrong fix.** The panel
is 320 px wide:

| Font cell | Columns | Verdict |
|---|---|---|
| 8×8 | 40 | Comfortable |
| 6×8 | 53 | The sweet spot |
| 5×7 | 64 | Dense but legible |
| 4×6 | 80 | Technically 80 columns; barely readable |

Height is not the problem — 240 px over 24 rows is 10 px per row.

Do **not** claim 80 columns and letterbox or pan. SSH negotiates the window size
in its pty request, so tell the remote the size you actually have and let it
reflow. `TERM`, `COLUMNS` and `LINES` all follow from that, and a well-behaved
remote will wrap correctly instead of you scrolling a fiction.

**The keyboard has no Ctrl, and reports no modifiers at all.** This is the real
blocker for a terminal — Ctrl-C, Ctrl-D and Ctrl-Z are not optional in an SSH
session. Two separate problems:

- The BBQ20 has no physical Ctrl key (it has `alt`, `shift`, `sym`).
- `bbq20.c` sets `.modifiers = 0` unconditionally and emits a single byte, so
  even shift state is not exposed through `catcall_input_t` today.

So a terminal port needs either a **sticky-modifier scheme in the app** (press
`sym`, then a letter, to send the control code) or an extension to the bbq20
driver to report modifiers properly. The driver change is the better fix and
benefits every app; the app-side scheme is what unblocks you first.

**Crypto is already there.** mbedTLS ships with ESP-IDF, so an SSH client does
not need it vendored. `libssh2` is the usual embedded choice and has an ESP-IDF
component; prefer that over vendoring OpenSSL.

Note there is already a `terminal` system app, but it is a local shell, not a
VT100 emulator — treat it as a sibling, not a starting point. A VT100 port needs
a real escape-sequence parser and a character grid with scrollback.

### 8.2 A map app

The deciding question is **where the tiles come from**, because it settles the
Speed Demon question:

- **Offline tiles on the SD card** → may use Speed Demon, gets the whole panel.
- **Tiles fetched over WiFi** → windowed, per §7.1.

A split design works well here: download windowed, then render offline.

**GPS is a first-class catcall** — `catcall_gps.h`, with `generic_nmea` behind it
on T-Deck Plus. Resolve it through the kernel registry like any other; do not
open the UART yourself.

**Tiles belong in PSRAM, decoded.** Budget against §9: a 256×256 RGB565 tile is
131,072 bytes, so a 3×3 working set is ~1.2 MB — comfortable in PSRAM,
impossible in internal DRAM. Keep the SD reads batched at pan boundaries rather
than per-frame, for the same SPI-bus reason as §4.1.

---

## 9. Memory

Two pools, and the scarce one is not the one you expect.

| Pool | Free on T-Deck Plus | Use for |
|---|---|---|
| Internal DRAM | ~30–130 KB depending on build | Task stacks, hot buffers, anything DMA |
| PSRAM | ~8.0 MB | Framebuffers, asset data, everything large |

```c
heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
```

**Largest contiguous block matters, not total free.** DOOM's 76,800-byte 8-bit
framebuffer fails an internal allocation on a device reporting ~103 KB free,
because the largest block is 31,744. Check `largest_internal` in the kernel's
`heapwatch` line before assuming an allocation will land.

**Try internal, fall back to PSRAM, and log which you got** — rather than
asserting, which turns a slower app into a dead one:

```c
buf = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
if (!buf) buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
lprintf("fb in %s\n", internal ? "internal DRAM" : "PSRAM (slower)");
```

That log line is what tells you why frame times look wrong later.

---

## 10. Checklist

- [ ] Tier chosen; `.claw` if it has its own renderer
- [ ] `app.pcat` + `CMakeLists.txt`; enabled via `modulestrap enable`
- [ ] Vendored source unmodified, globbed, `-w` **per-source** only
- [ ] `.inl`/`.dat`/`COPYING` copied, not just `.c`/`.h`
- [ ] Provenance + licence in a `README.md`; no assets committed
- [ ] Upstream platform layer deleted, not adapted
- [ ] Seams go through `purr_port.h` (§4.2), not hand-rolled
- [ ] Screen size from `get_info()`, not hardcoded
- [ ] No double byte-swap in the pixel path
- [ ] Keyboard found by capability, not `purr_kernel_input()`
- [ ] Key releases synthesised if your input model needs them
- [ ] Speed Demon: internal stack, heartbeat in every slow loop
- [ ] Every failure path draws something and exits cleanly
- [ ] `app_manager_notify_exited()` on **every** path out
- [ ] Allocation sized against largest block, not total free

---

## 11. Reference ports

`source/apps/exclusive/doom/` is the reference port and is deliberately
commented for this purpose. Its `README.md` covers provenance, and the long
comments in `compat/i_system.c` (WAD access), `compat/i_video.c` (display and
the synthesised key-up) and `doom_app.c` (Speed Demon lifecycle) each explain
why the code is shaped the way it is rather than what it does.

`source/apps/exclusive/magidos/` is a second example with a different shape: a
host-side shell rather than a vendored engine.
