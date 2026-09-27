# 10 — Module Loading, Priority, and SD Fallback

> **Accurate as of v1.0.0-dp9.** Read the section immediately below before the
> rest of this document — the primary loading mechanism is **static
> registration**. The `.purr` file scanning this document also describes is
> not a working secondary route, and should not be relied on as one: its
> loader (`purr_kernel_load_module()`/`peek_module_header()` in
> `purr_kernel.c`) `fread()`s a `purr_module_header_t` — including its raw
> `init()`/`deinit()` function pointers — directly off disk. That can only
> produce a valid, callable address if the file is a byte-exact dump of the
> exact firmware build reading it; it cannot load an independently compiled
> object, and never could. There are no `.purr` files anywhere in this tree
> on any device, on any build. The functions are kept (six `kernel_*_boot.c`
> files still call `purr_kernel_scan_modules()` at boot, and removing that
> is its own tracked boot-sequence change) but they should be read as
> historical/aspirational, not as a real extension point. `source/modules/
> driver_manager/` — the module this scan was originally built to feed — is
> in the same state; see its own top-of-file comment.
>
> The real loadable-module mechanism, proven on real hardware, is
> `source/modules/claw_loader/` (`claw_elf.c`'s genuine ELF32/Xtensa
> relocation). It's scoped to one loaded personal-app slot today; extending
> it to drivers/system modules is the intended direction, not building out
> the `.purr` scan further.

This document covers how PURR OS loads kernel modules at boot: static
registration, the priority system, SD fallback, the panic screen, and the SD
card directory layout.

---

## How modules actually get loaded

Modules selected in a device's `device.pcat` are **compiled into the firmware
and registered statically**. They are not shipped as `.purr` files and are not
discovered by scanning.

purrstrap generates `purr_device_glue.c` at build time from `device.pcat`'s
`[modules]`, `[drivers]` and `[apps]` sections. For each entry it emits an
`extern` declaration and a registration call:

```c
// auto-generated — do not edit
extern purr_module_header_t purr_module_mochi;
extern purr_module_header_t purr_module_systemui;
extern purr_module_header_t purr_module_app_manager;

void purr_register_static_modules(void) {
    purr_kernel_register_module_static(&purr_module_mochi);
    purr_kernel_register_module_static(&purr_module_systemui);
    purr_kernel_register_module_static(&purr_module_app_manager);
}
```

The kernel's boot then does:

```c
purr_register_static_modules();      // populate the registry
purr_kernel_load_static_modules();   // call each init() in priority order
```

Two consequences worth knowing:

- **Every value under `[modules]` becomes a module symbol.** A key whose value
  isn't a real module directory produces an `extern purr_module_<value>;` that
  fails at link. This is why presentation choices that select between
  *implementations* live in a separate `[ui]` section — see `09_SystemUI.md`'s
  `systemui_style`.
- **Adding a module means editing `device.pcat`**, not dropping a file on the
  device.

`.purr` scanning still runs afterwards for `/sdcard/modules` and
`/sdcard/drivers`, but — see the banner at the top of this document — it
cannot actually load a real out-of-tree module, so an SD card cannot add
extras this way today. Nothing in a normal build arrives via this path.

---

## Module Priority

Every module declares a `load_priority` in its `purr_module_header_t`,
regardless of whether it is registered statically or loaded from a `.purr`:

| Level | Constant | Value | Meaning |
|---|---|---|---|
| **REQUIRED** | `PURR_PRIORITY_REQUIRED` | `1` | Kernel **panics** if this fails to load from both flash and SD. Use for display driver, touch driver — anything without which the OS cannot function. |
| **IMPORTANT** | `PURR_PRIORITY_IMPORTANT` | `2` | Loaded before apps. Kernel logs a warning and continues if missing. Use for radio, GPS, secondary input. |
| **OPTIONAL** | `PURR_PRIORITY_OPTIONAL` | `3` | Silent skip if missing. Use for non-essential add-ons. |

Setting priority in a module's header:

```c
purr_module_header_t purr_module = {
    .magic         = PURR_MODULE_MAGIC,
    .abi_version   = PURR_MODULE_ABI_VERSION,
    .module_type   = PURR_MOD_DRIVER,
    .load_priority = PURR_PRIORITY_REQUIRED,   // ← kernel panics without this
    .name          = "ili9341",
    .version       = "1.0.0",
    .kernel_min    = "0.9.0",
    .provided_catcalls = CATCALL_FLAG_DISPLAY,
    .init   = ili9341_init,
    .deinit = ili9341_deinit,
};
```

---

## Boot Load Sequence

Real sequence, as implemented in `source/kernel/kernel_tdeck_plus/kernel_tdp_boot.c`
(other kernels follow the same shape):

```
app_main()
  │
  ├─ NVS init
  ├─ purr_device_init()          // board power rails, e.g. Heltec's Vext
  ├─ Baked-in Layer 0 drivers    // display, touch, input, radio — direct calls,
  │                              //   not modules; a specialized kernel needs a
  │                              //   screen before anything else can report a fault
  ├─ boot_splash_show()          // raw framebuffer, before any UI backend exists
  ├─ Mount /flash  (SPIFFS)
  ├─ Mount /sdcard (FAT — optional)
  │
  ├─ purr_register_static_modules()     // generated glue; populates the registry
  ├─ purr_kernel_load_static_modules()  // init() each, in load_priority order
  │
  └─ purr_kernel_scan_modules("/sdcard/modules")   // optional extras
     purr_kernel_scan_modules("/sdcard/drivers")   // all treated as optional
```

Priority ordering guarantees that `PURR_PRIORITY_REQUIRED` (P1) modules are
initialised — and have registered their catcalls — **before** any P2 or P3
module tries to use them.

Note that on a specialized kernel the display/touch/input/radio drivers are
**baked directly into boot**, ahead of the module system entirely, rather than
being loaded as modules. purrstrap detects this (`_has_specialized_kernel()`)
and omits them from the generated glue to avoid double-init. See `13_Kernels.md`.

---

## Flash Partition — What Goes In

The SPIFFS flash partition (`/flash`) is built by **purrstrap** using `spiffsgen.py` from the IDF. It contains only what the `[flash]` section of `device.pcat` lists:

```toml
[flash]
# name = priority
display/ili9341   = 1    # P1: required — kernel panics without display
touch/cst816s     = 1    # P1: required — no touch = can't use UI
miniwin           = 2    # P2: important — UI framework
app_manager       = 2    # P2: important — without this no apps run
radio/sx1262      = 3    # P3: optional
```

The flash layout inside SPIFFS:

```
/flash/
├── modules/
│   ├── miniwin.purr
│   └── app_manager.purr
└── drivers/
    ├── display/
    │   └── ili9341.purr
    ├── touch/
    │   └── cst816s.purr
    └── radio/
        └── sx1262.purr
```

---

## SD Card — Layout and Fallback

The SD card serves two roles:

1. **Fallback** for P1 modules missing from flash (corrupt flash, dev workflow, field update)
2. **Extension** — additional modules, user apps, ROM files, config

```
/sdcard/
├── modules/              ← P1/P2 fallbacks and extra system modules
│   └── *.purr
├── drivers/              ← P1/P2 driver fallbacks and extras
│   ├── display/
│   ├── touch/
│   ├── input/
│   ├── radio/
│   └── gps/
├── apps/                 ← user apps loaded by app_manager
│   ├── *.meow            — Lua scripts
│   ├── *.paws            — compiled userland apps
│   └── *.claw            — compiled kernel-access apps
├── magicmac/             ← MagicMac emulator files
│   ├── mac.rom           — 512KB Mac Plus ROM (user-supplied)
│   └── mac.conf          — override config (optional)
├── magidos/              ← MagiDOS emulator files
│   ├── disk.img          — DOS disk image
│   └── magidos.conf
└── system/
    └── logs/             ← kernel + module log files (future)
```

The `boot.c` `ensure_sd_dirs()` call creates any missing directories when the SD mounts, so a freshly-formatted card is immediately usable.

---

## Kernel Panic Screen

> **Caveat:** the "or SD" half of "fails to load from both flash and SD"
> below describes the intended design, not working behavior — see the
> banner at the top of this document. `modulestrap` does produce real,
> independently compiled `.purr` blobs (`cattobaked/modules/*.purr`), but
> `purr_kernel_load_module()` cannot actually load one back at runtime.
> Today, a P1 module missing from flash panics; SD is not a real fallback.

`purr_kernel_panic(const char *reason)` is called when a P1 module fails to load from both flash and SD. It:

1. Logs `KERNEL PANIC` + reason to serial (always — serial works even with no display)
2. If a display catcall is registered (meaning the display driver itself loaded fine but something else failed): draws a red panic screen with white stripes so the user knows the device has halted — not just a blank screen
3. Halts in an infinite `vTaskDelay` loop

> **If the display driver is the P1 module that failed**, the panic screen will not draw (no display catcall registered). Serial log is the only output. Connect a serial monitor to diagnose.

---

## Declaring a Module as Required

Both the module header and `device.pcat` participate:

- **Module header**: set `load_priority = PURR_PRIORITY_REQUIRED` — this is the module's self-declaration
- **device.pcat** `[flash]` section: set `display/ili9341 = 1` — this tells purrstrap to include it in the flash image at all

A module not listed in `[flash]` won't be in the SPIFFS image. If it's P1 and missing from flash, the kernel will attempt the SD fallback. If SD also doesn't have it, it panics.

---

## Adding a New Required Driver

1. Set `load_priority = PURR_PRIORITY_REQUIRED` in the driver's `purr_module_header_t`
2. Add it to the device's `device.pcat` under `[flash]` with priority `1`
3. Run `purrstrap build <device>` — modulestrap builds the blob, it gets staged into SPIFFS, flash.bin is produced
4. Flash with `purrstrap flash <device>`

If you want it on SD as fallback only (e.g. for dev/testing) — **this does
not currently work.** `modulestrap` will happily compile the `.purr` blob
and it can be copied to `/sdcard/drivers/<type>/`, but
`purr_kernel_load_module()` cannot load it back (see the banner at the top
of this document). Ship it in `[flash]` for now.

---

*DP8 documentation pass performed by Claude Opus 5 in agentic/auto mode. The
boot sequence above was traced against `kernel_tdp_boot.c` and purrstrap's
`_generate_glue()`.*
