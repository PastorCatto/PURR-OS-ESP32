#pragma once
// purr_module.h — .purr kernel module ABI
//
// Every .purr binary begins with a purr_module_header_t at a fixed symbol
// named `purr_module`. The kernel reads this at load time to identify the
// module, validate compatibility, and wire catcalls.
//
// Module types:
//   PURR_MOD_DRIVER     — implements one or more catcalls (display, touch, etc.)
//   PURR_MOD_SYSTEM     — kernel service (driver_manager, app_manager, etc.)
//   PURR_MOD_UI         — UI framework (miniwin, headless)
//   PURR_MOD_APP        — compiled application (.claw tier)
//
// Priority levels (load_priority field):
//   PURR_PRIORITY_REQUIRED  (1) — Kernel PANICS if this fails to load from both
//                                  flash and SD. Use for display driver, touch driver —
//                                  anything the OS cannot function without.
//   PURR_PRIORITY_IMPORTANT (2) — Loaded before apps. Kernel logs a warning and
//                                  continues if missing. Use for radio, GPS, input.
//   PURR_PRIORITY_OPTIONAL  (3) — Loaded opportunistically. Silent skip if missing.
//                                  Use for non-essential modules.
//
// Catcall flags (required_catcalls / provided_catcalls bitmask):

#include <stdint.h>

#define PURR_MODULE_MAGIC       0x50555252u   // 'PURR'
#define PURR_MODULE_ABI_VERSION 3

// Maximum other modules a single module can name as a hard dependency in
// its own `depends` field below. 4 covers every real fan-out seen in this
// tree so far (app_manager's own CMakeLists PRIV_REQUIRES speed_demon,
// user_mgr, sig_mgr, claw_loader — the largest today) with one slot of
// headroom. Fixed-size, matching every other string field in this header
// (name/version/kernel_min/kernel_max) — this header is read as a raw
// struct off flash for a loaded module (see claw_elf.h), so it can't hold
// a variable-length list the way a heap-allocated config could.
#define PURR_MODULE_MAX_DEPS 4

// Module types
#define PURR_MOD_DRIVER   0x01
#define PURR_MOD_SYSTEM   0x02
#define PURR_MOD_UI       0x03
#define PURR_MOD_APP      0x04

// Load priority levels
#define PURR_PRIORITY_REQUIRED  1   // must load — panic if missing
#define PURR_PRIORITY_IMPORTANT 2   // load before apps — warn if missing
#define PURR_PRIORITY_OPTIONAL  3   // best-effort — silent if missing

// init() return value meaning "chose not to start, this is not a fault" —
// e.g. a module that refuses to activate because a mutually-exclusive
// peer already owns a shared resource (meshcore_module.cpp declining
// while meshtastic holds the radio). Distinct from any nonzero failure
// code: the kernel's static module loader (purr_kernel.c's
// load_one_static()) skips the crash-guard strike for this specific
// value, since retrying an intentional decline on every boot isn't a
// crash loop — logging it as one and eventually disabling the module via
// purr_crash_guard would be wrong. The module still doesn't load this
// boot (same as any other nonzero return); this only affects whether the
// attempt counts against the crash guard's failure budget.
#define PURR_MODULE_INIT_DECLINED  2

// Catcall bitmask flags
#define CATCALL_FLAG_DISPLAY  (1u << 0)
#define CATCALL_FLAG_TOUCH    (1u << 1)
#define CATCALL_FLAG_INPUT    (1u << 2)
#define CATCALL_FLAG_RADIO    (1u << 3)
#define CATCALL_FLAG_GPS      (1u << 4)

typedef struct {
    uint32_t magic;             // must equal PURR_MODULE_MAGIC
    uint8_t  abi_version;       // must equal PURR_MODULE_ABI_VERSION
    uint8_t  module_type;       // PURR_MOD_*
    uint8_t  load_priority;     // PURR_PRIORITY_REQUIRED / IMPORTANT / OPTIONAL
    // speed_demon — declare that this app needs the machine to itself.
    //
    // Set to 1 and app_manager takes care of everything: before the app's
    // init() runs it unloads the launcher, the system UI, the mesh stack and
    // every other non-essential service, and when the app's task ends it puts
    // all of them back. The app itself calls nothing.
    //
    // One line, in the app's own PURR_MODULE_REGISTER block:
    //     .speed_demon = 1,
    //
    // Ignored for anything that is not PURR_MOD_APP. Takes the former _reserved
    // pad byte, so the header layout and ABI version are unchanged.
    // See modules/speed_demon/speed_demon.h and docs/15_SpeedDemon.md.
    uint8_t  speed_demon;
    char     name[32];          // human-readable module name
    char     version[12];       // module semver string e.g. "1.0.0"
    char     kernel_min[12];    // minimum KITT version e.g. "0.9.0"
    char     kernel_max[12];    // max KITT version, empty = no ceiling
    uint32_t provided_catcalls; // bitmask of CATCALL_FLAG_* this module provides
    uint32_t required_catcalls; // bitmask of CATCALL_FLAG_* this module needs

    // Other modules this one needs ALREADY LOADED before its own init()
    // may run — by name (purr_kernel_get_module()'s own lookup key), same
    // as kernel_min/kernel_max are checked by name/version rather than by
    // pointer. Empty string ("") in a slot means unused; slots need not be
    // contiguous from index 0. This is deliberately narrower than a real
    // dependency-resolution system: it is a LOAD-TIME GATE ("refuse to
    // init if X isn't already up"), not a solver that reorders anything
    // to satisfy it — a module still has to be started after its own
    // dependencies by whoever is starting it (device.pcat's [modules]
    // priority ordering, for a static module; the caller's own sequencing,
    // for a loaded one). See claw_loader.c's check_dependencies() for the
    // one real consumer today.
    //
    // ABI v2 -> v3: additive at the end of the struct, so a v2 header
    // (shorter) read as v3 would leave these slots as whatever bytes
    // followed it in memory — which is exactly why abi_version is checked
    // BEFORE anything past it is trusted (see purr_kernel_load_module()'s
    // own check, and claw_loader's mirror of it).
    char     depends[PURR_MODULE_MAX_DEPS][32];

    // Lifecycle — called by kernel module loader
    int  (*init)(void);         // 0 = success
    void (*deinit)(void);
} purr_module_header_t;

// Declare a static module descriptor.
//
// Usage (in exactly one .c file per module):
//   PURR_MODULE_REGISTER(my_driver) = {
//       .magic = PURR_MODULE_MAGIC,
//       ...
//   };
//
// purrstrap reads device.pcat and generates purr_register_static_modules() in
// the device glue file, which explicitly calls purr_kernel_register_module_static()
// for each module the device needs. No linker tricks required.
#ifdef __cplusplus
extern "C" {
#endif
void purr_kernel_register_module_static(const purr_module_header_t *hdr);
#ifdef __cplusplus
}
#endif

#define PURR_MODULE_REGISTER(id) \
    purr_module_header_t purr_module_##id
