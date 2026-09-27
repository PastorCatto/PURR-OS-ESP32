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
#define PURR_MODULE_ABI_VERSION 1

// Module types
#define PURR_MOD_DRIVER   0x01
#define PURR_MOD_SYSTEM   0x02
#define PURR_MOD_UI       0x03
#define PURR_MOD_APP      0x04

// Load priority levels
#define PURR_PRIORITY_REQUIRED  1   // must load — panic if missing
#define PURR_PRIORITY_IMPORTANT 2   // load before apps — warn if missing
#define PURR_PRIORITY_OPTIONAL  3   // best-effort — silent if missing

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
    uint8_t  _reserved;         // pad to 4-byte alignment
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

// Every .purr module must export this symbol
extern purr_module_header_t purr_module;
