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

#ifdef __cplusplus
}
#endif
