# moy on PURR OS

A host implementation of [moy core 0.1](https://github.com/moybyte-org/moy-spec)
— a portable console spec: 320×240 palette-indexed, 64 colours, a 512-tile
sheet, a tilemap, and a sandboxed Lua 5.4 cart. Carts are folders and run
unmodified on any conforming host.

This port exists partly to **test `apps/common/purr_port.h`**, the translation
layer described in [docs/16_PortingApps.md](../../../../docs/16_PortingApps.md).
It is the first app written against that layer rather than against catcalls
directly.

## Running a cart

Copy a cart folder to `/sdcard/moy/` on the SD card:

```
/sdcard/moy/brick_siege.moy/
    manifest.json
    main.lua
    sprites.moygfx
    map.moymap
    config.json
```

Two example carts from the spec repo are staged in `sdcard_staging/moy/` at the
root of this repository. The first `*.moy` folder found is played; a cart picker
belongs in the launcher, not here.

If nothing is found the app says so on screen and waits for a keypress before
exiting cleanly — it does not leave a black screen. That matters because Speed
Demon has already unloaded the launcher by then.

## Controls

| Hardware | moy button |
|---|---|
| `W` `A` `S` `D` | up / left / down / right |
| Trackball roll | up / left / down / right |
| `Space`, trackball click | `a` |
| `B` | `b` |
| `R` | `run` |
| Touchscreen | `touch()` |
| Any key | `key()` / `keyp()` |

The mnemonic keys (`B`, `R`) were chosen so they cannot collide with the WASD
pad.

## Why it fits

The console's shape happens to match what PURR OS already provides, which is why
it was picked to exercise the translation layer:

| spec | PURR OS |
|---|---|
| 320×240 | the T-Deck Plus panel, 1:1, no scaling |
| palette-indexed 8bpp | `purr_port_open(&p, 8)` — the `fb8` path |
| 64-entry RGB palette | `purr_port_set_palette_rgb888()` |
| present a frame | `purr_port_present()` |
| buttons, held | `purr_port_key_next()`'s synthesised key-up |
| Lua 5.4 | `source/lib/lib_lua`, already vendored |

`moy_app.c` is the entire platform binding and is ~200 lines, most of it
comments. Contrast `doom/compat/`, which predates the layer and hand-rolls the
same five things.

## Speed Demon and Lua

Speed Demon unloads the `lua_runtime` **module** (it is `PURR_MOD_SYSTEM`),
which would normally rule out running a Lua cart with the panel to ourselves.

This component vendors Lua 5.4 **privately**, exactly as `lua_runtime` itself
does — see `CMakeLists.txt`. The interpreter here belongs to the app, nothing
unloads it, and a cart gets the whole machine. It is also the right design on
the spec's own terms: moy requires a sandboxed VM with per-cart state and its
own tick, which is not what a shared system-wide runtime is for.

`liolib.c`, `loslib.c` and `loadlib.c` are **excluded from the build**, not
merely nil'd at runtime: spec 4.1 forbids `io`, `os` and `package`, and leaving
them unlinked means a bug in the sandbox setup cannot reach them.

## Conformance

Implemented (core): `cls` `pix` `line` `rect` `rectb` `circ` `circb` `print`
`clip` `camera` `pal` `palt` `spr` `sspr` `map` `mget` `mset` `btn` `btnp`
`players` `touch` `key` `keyp` `textmode` `time` `rnd` `flr` `cfg` `pmem`
`quit`, plus `view` (EXTENSION: viewport).

**Audio verbs are bound but silent.** `sfx` `beep` `music` `music_stop`
`sound_stop` `volume` accept their arguments and do nothing — T-Deck Plus has no
configured audio output in PURR OS yet. They are bound rather than left
undefined deliberately: spec 8 makes audio core, so a cart is entitled to assume
the verb exists. A silent console is degraded; a `nil` global is a crashed cart.

**Not implemented:** the DRAFT 6.1 verbs (`tri` `trib` `rect_batch` `spans`
`spr_batch`) — the spec marks them provisional and says they may be dropped
entirely — and the `layers` extension (`make_layer` `draw_layer` `background`).

`textmode()` is a no-op, which spec 7.3 explicitly permits on a host whose
keyboard has only one mode.

## Known gaps

- Cart-supplied palettes (spec 2.2) are not read yet; the baked 64-entry
  palette is always used.
- `sounds.json` is not parsed.
- No cart picker — first cart found wins.
