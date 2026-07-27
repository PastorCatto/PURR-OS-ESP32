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
#define PURR_MODULE_ABI_VERSION 2

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
