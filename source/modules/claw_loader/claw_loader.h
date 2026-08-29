#pragma once
// claw_loader.h — loads a compiled-but-never-linked .claw object at
// runtime: parses it (claw_elf.h), relocates it, and maps it executable
// from flash. Promoted from the cross-device .claw loading R&D spike once
// three real-hardware rounds confirmed the whole approach — see this
// module's own module.pcat for that history.
//
// ── Entry point convention ──────────────────────────────────────────────
// A pre-linked .claw app declares PURR_MODULE_REGISTER(name) = { .init =
// X, .deinit = Y, ... } — a static struct the linker places in a known
// section, found by app_manager via purr_kernel_get_module(). That
// mechanism is inherently build-time: it doesn't exist for an object that
// was never linked into this firmware at all.
//
// A LOADED module instead names its two entry points directly:
// claw_personal_init() (int(void), same signature and same "0 = ok, error
// codes on failure" contract purr_module_header_t::init already has) and
// claw_personal_deinit() (void(void)). claw_loader_load() looks both up by
// symbol name — same idea, resolved by this loader instead of by the
// linker.
//
// ── One slot ─────────────────────────────────────────────────────────────
// Only one module loaded at a time — same constraint this codebase's Lua
// VM (.meow/.hiss, lua_runtime.c) already has for the same reason: nothing
// yet needs more, and it keeps the flash partition/RAM bookkeeping simple.
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int  (*claw_init_fn)(void);
typedef void (*claw_deinit_fn)(void);

typedef struct {
    claw_init_fn   init;
    claw_deinit_fn deinit;
    // Kept alive for the loaded module's whole lifetime — freed by
    // claw_loader_unload(), never touched by the caller directly.
    void    *rodata_ram;
    void    *data_ram;
    void    *bss_ram;
    // esp_partition_mmap_handle_t, kept as a plain uint32_t here so this
    // header doesn't need to pull in esp_partition.h — it's a typedef over
    // uint32_t upstream, passed back to esp_partition_munmap() unchanged.
    uint32_t mmap_handle;
} claw_loaded_module_t;

// Parses, relocates, and flash-maps `obj_bytes` (obj_len bytes — a
// standalone `xtensa-esp32s3-elf-gcc -c` compile, never linked), then
// resolves claw_personal_init/claw_personal_deinit within it. `out` is
// zeroed then filled; on failure (parse error, missing entry point, object
// too big for the loader's flash slot) returns false with nothing to free.
//
// Does NOT call init() — that's the caller's decision, same as app_manager
// deciding when to call a pre-linked module's own .init.
bool claw_loader_load(const uint8_t *obj_bytes, size_t obj_len, claw_loaded_module_t *out);

// Frees every resource claw_loader_load() allocated (RAM copies, the flash
// mapping) and zeroes `m`. Does NOT call deinit() — same reasoning as
// claw_loader_load() not calling init(): the caller already knows whether
// deinit() needs calling first (it doesn't, for a module that was never
// init()'d, or one that already reported itself done).
void claw_loader_unload(claw_loaded_module_t *m);

// ── Personal-space storage ──────────────────────────────────────────────
// Per-user storage for .claw app objects, at
// <root>/<username>/<appname>.claw — flat, one file per app, enumerated
// directly (no separate manifest for this first pass, matching
// app_manager.c's own scan_dir() precedent: readdir() order is whatever
// the filesystem gives, not alphabetical or insertion-order). <root> is
// /sdcard/personal when SD is available (preferred — more capacity), else
// /flash/personal on a device with no SD card at all (e.g. Heltec V3;
// see purr_kernel_flash_available()'s own doc comment) — genuinely small
// there (whatever's left of that device's SPIFFS partition after
// everything else that lands on /flash), fine for small pushed apps, not
// a general-purpose store.
//
// username/appname are trusted as already-validated by the caller
// (app_manager, once piece 3 of the personal-space work lands) —
// this layer does no username-format checking of its own. user_mgr.h's
// user_mgr_valid_username() is the real gate; pulling user_mgr in here
// would be a needless dependency for a module that's otherwise
// deliberately minimal (see this module's CMakeLists.txt REQUIRES).
//
// All four below return false (no-op) if NEITHER SD nor flash is
// available (purr_kernel_sd_available()/purr_kernel_flash_available()) —
// same gate kernel_tdp_boot.c's ensure_sd_dirs() uses before touching
// /sdcard at all, just widened to accept either root.

// The SD-preferred/flash-fallback root itself ("/sdcard/personal" or
// "/flash/personal"), or NULL if neither is available — for a caller that
// needs to stage files under this SAME root before they're a real,
// scannable personal app (e.g. server_mgr.h's pending-approval uploads,
// staged at <root>/pending/ until a human approves them), without
// duplicating this file's own SD-preferred/flash-fallback logic.
const char *claw_loader_personal_root(void);

// Ensures <root>/<username>/ exists, writes obj_bytes to
// <appname>.claw inside it (overwriting any existing file of that name).
bool claw_loader_personal_add(const char *username, const char *appname,
                               const uint8_t *obj_bytes, size_t obj_len);

// Number of .claw files currently stored for `username` (0 if none, no
// personal directory yet, or SD unavailable).
int  claw_loader_personal_count(const char *username);

// `idx`'th (0..count-1) app's display name (filename minus .claw), written
// into name_out. Returns false if idx is out of range or the directory
// couldn't be enumerated. Same "whatever readdir() order gives" caveat as
// claw_loader_personal_count() above — a caller wanting a stable order
// should sort after collecting every entry, not rely on index stability
// across calls.
bool claw_loader_personal_at(const char *username, int idx,
                              char *name_out, size_t name_out_sz);

// Deletes <appname>.claw from username's personal directory. Returns false
// if it didn't exist or the delete failed.
bool claw_loader_personal_remove(const char *username, const char *appname);

// Reads <appname>.claw fully into a temporary buffer and calls
// claw_loader_load() on it — same fixed import table (claw_loader.c's
// s_imports[]) every other loaded module gets, and the same "only one
// loaded module at a time" constraint as claw_loader_load() itself, see
// this header's top comment. The temporary read buffer is freed before
// this returns;
// `out`'s own allocations follow claw_loader_load()'s normal ownership
// rules, freed via claw_loader_unload(). Returns false if the file doesn't
// exist, couldn't be read, or claw_loader_load() itself failed.
bool claw_loader_personal_load(const char *username, const char *appname,
                                claw_loaded_module_t *out);

#ifdef __cplusplus
}
#endif
