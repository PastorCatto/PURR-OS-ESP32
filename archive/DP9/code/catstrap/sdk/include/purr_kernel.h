#pragma once
// purr_kernel.h — PURR OS kernel spine API
//
// This is what modules call into the kernel for. The kernel owns these
// registries. Modules register their catcalls here at init time. Other
// modules call through here — never directly into another module.

#include <stdint.h>
#include <stdbool.h>
#include "purr_module.h"
#include "../catcalls/catcalls.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Version ───────────────────────────────────────────────────────────────────

#define PURR_KERNEL_VERSION  "0.12.0"
#define KITT_VERSION         "0.9.0"

// ── Module loader ─────────────────────────────────────────────────────────────

// Scan flash dir for .purr files sorted by priority, with SD fallback.
// For PURR_PRIORITY_REQUIRED modules: if both flash and sd_fallback_dir fail,
// purr_kernel_panic() is called. Pass NULL for sd_fallback_dir to disable fallback.
int  purr_kernel_scan_modules(const char *flash_dir, const char *sd_fallback_dir);

// Load a single .purr by path.
int  purr_kernel_load_module(const char *path);

// Unload by name.
void purr_kernel_unload_module(const char *name);

// Get loaded module header by name (NULL if not loaded).
const purr_module_header_t *purr_kernel_get_module(const char *name);

// ── Catcall registry ──────────────────────────────────────────────────────────
// Modules call these at init to register what they provide.
// The kernel stores one pointer per catcall type. Last registered wins,
// which allows override (e.g. fallback display replaced by real driver).

void purr_kernel_register_display(const catcall_display_t *drv);
void purr_kernel_register_touch  (const catcall_touch_t   *drv);
void purr_kernel_register_input  (const catcall_input_t   *drv);
void purr_kernel_register_radio  (const catcall_radio_t   *drv);
void purr_kernel_register_gps    (const catcall_gps_t     *drv);

// Retrieve registered catcall implementations (NULL if none registered).
const catcall_display_t *purr_kernel_display(void);
const catcall_touch_t   *purr_kernel_touch(void);
const catcall_input_t   *purr_kernel_input(void);
const catcall_radio_t   *purr_kernel_radio(void);
const catcall_gps_t     *purr_kernel_gps(void);

// ── System info ───────────────────────────────────────────────────────────────

uint32_t purr_kernel_free_ram(void);
uint64_t purr_kernel_uptime_ms(void);
bool     purr_kernel_sd_available(void);
bool     purr_kernel_wifi_connected(void);
void     purr_kernel_reboot(void);

// Called by SD / WiFi modules when their state changes
void     purr_kernel_set_sd_available(bool v);
void     purr_kernel_set_wifi_connected(bool v);

// ── Panic ─────────────────────────────────────────────────────────────────────

// Kernel panic — logs reason to serial, draws panic screen if display is up,
// then halts. Never returns.
void __attribute__((noreturn)) purr_kernel_panic(const char *reason);

#ifdef __cplusplus
}
#endif
