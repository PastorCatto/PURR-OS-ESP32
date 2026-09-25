// purr_probe_guard.h — MMIO access gate for the hardware probe kernel
//
// THIS FILE AND ITS .c ARE THE SAFETY BOUNDARY OF THE PROBE KERNEL.
// Nothing else in kernel_tdeck_plus_probe/ is permitted to dereference a
// host-supplied address. Every read and every write funnels through
// probe_guard_read32() / probe_guard_write32() below, each of which
// re-validates its own address immediately before touching the bus.
//
// The gate is ALLOWLIST-FIRST: an address that is not inside an explicitly
// enumerated allow window is refused. Denial is therefore the default for the
// entire 4 GB space, including every peripheral nobody thought to name.
//
// On top of that there is an independent DENY list covering the ranges whose
// misuse is irreversible or security-relevant — eFuse above all. It is checked
// BEFORE the allowlist and it wins unconditionally. It exists so that a future
// widening of the allowlist (a careless `{0x60000000, 0x10000}` bulk window,
// say) cannot silently expose eFuse: probe_guard_selftest() proves at boot that
// no allow window intersects any deny window, and the command loop refuses to
// start if that proof fails. Fail-closed, not fail-open.
//
// Why eFuse specifically. eFuse bits burn ONE WAY. There is no erase, no
// reflash, no recovery. A single stray word written into the eFuse programming
// registers can permanently enable flash encryption or secure boot on the
// board, or blow the JTAG/download-mode disable bits, turning a development
// T-Deck into a brick that cannot be reflashed or debugged again. That is not a
// risk worth carrying in a tool whose whole job is to poke registers by hand
// from a host script, so this kernel simply cannot reach them.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Result of an access check. Anything other than PROBE_ACCESS_OK means the
// bus was NOT touched.
typedef enum {
    PROBE_ACCESS_OK = 0,
    PROBE_DENY_FORBIDDEN,    // inside an explicit deny window (eFuse et al.)
    PROBE_DENY_NOT_ALLOWED,  // outside every allow window
    PROBE_DENY_UNALIGNED,    // not 4-byte aligned
    PROBE_DENY_SPAN,         // read length leaves the window / wraps
    PROBE_DENY_NOT_ARMED,    // selftest never passed — gate is sealed shut
} probe_access_t;

// Stable machine-readable token for a result, for the wire protocol.
// e.g. "efuse-forbidden", "not-allowed". Never NULL.
const char *probe_guard_reason(probe_access_t r);

// A named MMIO window. `len` is in bytes.
typedef struct {
    uint32_t    base;
    uint32_t    len;
    const char *name;
    const char *note;
} probe_window_t;

// Introspection, so the host side (and the emulator author reading a trace)
// can print exactly what this build permits rather than guessing.
size_t                 probe_guard_allow_count(void);
const probe_window_t  *probe_guard_allow_at(size_t i);
size_t                 probe_guard_deny_count(void);
const probe_window_t  *probe_guard_deny_at(size_t i);

// Boot-time proof obligation. Verifies that:
//   1. no allow window intersects any deny window,
//   2. no window has zero length or wraps the address space,
//   3. the eFuse block in particular resolves to FORBIDDEN for both a read
//      and a write probe at its base, its last word, and its midpoint.
//
// Returns true only if all of that holds, and ONLY THEN is the gate armed.
// Until it is armed every access returns PROBE_DENY_NOT_ARMED, so a failed or
// skipped selftest leaves the probe inert rather than permissive.
bool probe_guard_selftest(void);
bool probe_guard_is_armed(void);

// Check without touching the bus. `words` is the number of 32-bit words the
// caller intends to read starting at `addr`.
probe_access_t probe_guard_check_read(uint32_t addr, uint32_t words);
probe_access_t probe_guard_check_write(uint32_t addr);

// The ONLY sanctioned accessors. Each re-checks its own address; callers may
// not hoist the check. On refusal *out is left untouched / no write occurs.
probe_access_t probe_guard_read32(uint32_t addr, uint32_t *out);
probe_access_t probe_guard_write32(uint32_t addr, uint32_t value);

#ifdef __cplusplus
}
#endif
