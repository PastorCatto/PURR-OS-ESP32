#pragma once
// purr_quirk.h — loads and serves a .purr v2 device quirk-data package
// (see purr_quirk_pkg.h for the format). Trivial compared to claw_loader:
// this is a plain data file — one heap buffer, no relocation, no
// flash-mapping, no execute permission ever involved.
//
// Only one package is ever loaded at a time — same "one at a time"
// shape as claw_loader's original single slot and the Lua VM, for the
// same reason: nothing yet needs more than the one device this firmware
// was built for.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reads `path` fully into a heap buffer and validates its header (magic,
// abi_version). Replaces any previously loaded package. Returns false
// (nothing loaded) on any I/O error, bad magic, or ABI mismatch — a
// missing quirk file is not treated as an error at this layer (every
// caller already falls back to its own hardcoded defaults when
// purr_quirk_get_block() returns NULL, so "no package for this device"
// and "package failed to parse" both just mean "use defaults").
bool purr_quirk_load(const char *path);

void purr_quirk_unload(void);

// True if a package is currently loaded (regardless of which blocks it
// contains) — mostly useful for a `modules`-style diagnostic listing.
bool purr_quirk_loaded(void);

// Looks up a named block by exact name match. Returns NULL (out_size
// untouched) if no package is loaded, or the package has no block with
// this name. On success, returns a pointer into the loader's own internal
// buffer (valid until the next purr_quirk_load()/purr_quirk_unload() —
// callers needing it longer must copy) and writes the block's byte size
// to *out_size if non-NULL.
//
// Callers MUST check the returned size against sizeof(their own expected
// struct) before casting and dereferencing — a stale or hand-edited
// package could carry a block of the wrong size for the struct layout
// this firmware build expects; this API does no struct-layout validation
// of its own; the ABI version was already checked at load time.
const void *purr_quirk_get_block(const char *name, size_t *out_size);

#ifdef __cplusplus
}
#endif
