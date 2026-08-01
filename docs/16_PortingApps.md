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
Demon (§7).

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

Most ports need the same five, and they are the whole job:

| Seam | PURR OS gives you | Section |
|---|---|---|
| Display | `catcall_display_t` | §5 |
| Input | `catcall_input_t` | §6 |
| Files | Standard `stdio` on `/sdcard` | §4.1 |
| Time | `esp_timer_get_time()`, `gettimeofday` | — |
| Memory | `heap_caps_malloc` | §8 |

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

---

## 8. Memory

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

## 9. Checklist

- [ ] Tier chosen; `.claw` if it has its own renderer
- [ ] `app.pcat` + `CMakeLists.txt`; enabled via `modulestrap enable`
- [ ] Vendored source unmodified, globbed, `-w` **per-source** only
- [ ] `.inl`/`.dat`/`COPYING` copied, not just `.c`/`.h`
- [ ] Provenance + licence in a `README.md`; no assets committed
- [ ] Upstream platform layer deleted, not adapted
- [ ] Screen size from `get_info()`, not hardcoded
- [ ] No double byte-swap in the pixel path
- [ ] Keyboard found by capability, not `purr_kernel_input()`
- [ ] Key releases synthesised if your input model needs them
- [ ] Speed Demon: internal stack, heartbeat in every slow loop
- [ ] Every failure path draws something and exits cleanly
- [ ] `app_manager_notify_exited()` on **every** path out
- [ ] Allocation sized against largest block, not total free

---

## 10. Worked example

`source/apps/exclusive/doom/` is the reference port and is deliberately
commented for this purpose. Its `README.md` covers provenance, and the long
comments in `compat/i_system.c` (WAD access), `compat/i_video.c` (display and
the synthesised key-up) and `doom_app.c` (Speed Demon lifecycle) each explain
why the code is shaped the way it is rather than what it does.

`source/apps/exclusive/magidos/` is a second example with a different shape: a
host-side shell rather than a vendored engine.
