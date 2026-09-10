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

// Two independent flash pools, each with its own CLAW_MAX_SLOTS sub-regions
// — see partitions_16mb_ota.csv's own comment on claw_slot/sys_claw for the
// sizing story:
//
//   CLAW_POOL_DYNAMIC — the original "claw_slot" partition (64KB / 2 =
//   32KB/slot). Personal/user-pushed apps and every claw_loader_selftest.c
//   proof object. Small, by design — nothing loaded here has ever needed
//   more.
//
//   CLAW_POOL_SYSTEM — the "sys_claw" partition (256KB / 2 = 128KB/slot).
//   Core/system-owned packages (loginUI, later systemui_launcher —
//   source/apps/system/login_ui/) that need real room for a UI screen,
//   shipped through the same app download/transfer system already proven
//   for personal apps rather than a bespoke mechanism (server_mgr's push,
//   app_manager_remote's download both already handle files up to 2MB) —
//   see claw_loader_system_install()/_load() below.
//
// A module loaded from one pool is completely independent of the other —
// loading two system packages never contends with a personal app load, and
// vice versa.
typedef enum {
    CLAW_POOL_DYNAMIC = 0,
    CLAW_POOL_SYSTEM  = 1,
} claw_pool_t;

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
    // Which pool (see claw_pool_t above) and which of that pool's
    // CLAW_MAX_SLOTS sub-regions this module occupies — set by
    // claw_loader_load(), read back by claw_loader_unload() to free the
    // right one. Not meant to be read or set by the caller directly;
    // exposed only because this struct is caller-owned (stack or static
    // storage, no heap allocation of its own) rather than an opaque handle.
    claw_pool_t pool;
    int      slot;
} claw_loaded_module_t;

// Number of independent modules that can be loaded at once, PER POOL — see
// claw_loader.c's own comment on the partition-size tradeoff this number
// represents. Callers that want to know how many are free right now
// (rather than just trying and handling failure) can use
// claw_loader_slots_free() below.
#define CLAW_MAX_SLOTS 2

// Parses, relocates, and flash-maps `obj_bytes` (obj_len bytes — a
// standalone `xtensa-esp32s3-elf-gcc -c` compile, never linked) into the
// named `pool`, then resolves claw_personal_init/claw_personal_deinit
// within it. `out` is zeroed then filled; on failure (parse error, missing
// entry point, object too big for the pool's own per-slot size, or every
// slot in that pool already occupied — see CLAW_MAX_SLOTS above) returns
// false with nothing to free.
//
// Auto-allocates the first free slot WITHIN the given pool rather than
// taking one as a parameter — every existing caller already treats this as
// "load me a module" with no slot concept at all, and this keeps it that
// way; claw_loader_unload() reads which pool/slot it got back out of `out`
// itself, so nothing else needs to track slot numbers.
//
// Does NOT call init() — that's the caller's decision, same as app_manager
// deciding when to call a pre-linked module's own .init.
bool claw_loader_load(const uint8_t *obj_bytes, size_t obj_len, claw_pool_t pool, claw_loaded_module_t *out);

// How many of `pool`'s CLAW_MAX_SLOTS are currently free — for a caller
// that wants to know before trying (e.g. a future UI showing "1 of 2
// loader slots in use") rather than only finding out via a failed
// claw_loader_load().
int claw_loader_slots_free(claw_pool_t pool);

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

// ── System-space storage (CLAW_POOL_SYSTEM) ──────────────────────────────
// Core/system-owned packages — no per-username directory, one fixed name:
// <name>.claw. NOT personal_root()'s SD-preferred/flash-fallback ordering
// — a system package ships baked into /flash's SPIFFS image at build time
// (purrstrap stages it there — see build_flash_image()'s own comment), so
// /flash/system/<name>.claw is the guaranteed-present shipped default;
// /sdcard/system/<name>.claw is instead an OPTIONAL OVERRIDE, checked
// first, same "/sdcard copy overrides the shipped default" precedent
// purr_quirk_load() already established for device.purr. Confirmed this
// ordering matters on real hardware, not just in theory — see claw_
// loader.c's own comment on this section for the exact failure it fixed.
// claw_loader_system_install() below exists for the SAME reason
// claw_loader_personal_add() does: a later push over the existing app
// download/transfer system (server_mgr's push, app_manager_remote's
// download) lands here through the identical write-a-file convention, no
// new transport code needed — it always writes the OVERRIDE location,
// never touching the shipped-default SPIFFS image at runtime.

// The override root ("/sdcard/system"), or NULL if SD isn't available —
// for a caller staging an update INTO it (mirrors claw_loader_personal_
// root()'s role for personal apps). The shipped-default location
// ("/flash/system") is purrstrap's own concern at build time, not
// something runtime code needs to know the path of.
const char *claw_loader_system_root(void);

// Ensures the override root exists, writes obj_bytes to <name>.claw
// inside it (overwriting any existing file of that name). Never touches
// the shipped-default /flash/system location.
bool claw_loader_system_install(const char *name, const uint8_t *obj_bytes, size_t obj_len);

// Checks the override location first, the shipped-default location
// second; reads whichever is found fully into a temporary buffer and
// calls claw_loader_load(..., CLAW_POOL_SYSTEM, out) on it — same fixed import
// table every other loaded module gets. The temporary read buffer is freed
// before this returns; `out`'s own allocations follow claw_loader_load()'s
// normal ownership rules, freed via claw_loader_unload(). Returns false if
// the file doesn't exist, couldn't be read, or claw_loader_load() itself
// failed.
bool claw_loader_system_load(const char *name, claw_loaded_module_t *out);

#ifdef __cplusplus
}
#endif
