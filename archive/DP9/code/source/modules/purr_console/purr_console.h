#pragma once
// purr_console.h — PURR OS console core (Phase 1c of the modular-core/
// Unix-boot plan — see the plan's own Context section for the full
// picture).
//
// Named purr_console, not console: ESP-IDF ships its own built-in
// component literally called "console" — see purr_console.c's own top
// comment for exactly how that collision silently broke the first real
// build of this module.
//
// One command table + one line-editing/dispatch loop, bound to whichever
// raw byte transport a caller provides via purr_console_io_t — UART0,
// USB-CDC, or (a later phase) an on-screen catcall_ui backend — without
// the interpreter itself knowing which. Replaces the four disconnected
// command interfaces that existed before this: each kernel_*_boot.c's own
// serial_console_task() (2 commands, its own if/else chain), terminal.c
// (the real command set, but built on purr_win — needs a full UI backend
// to exist at all), blackpurr_shell.c (bypasses catcall_ui entirely, and
// is an app launcher, not a command shell), and purr_probe_cmd.c (the
// most mature of the four, but siloed in the hardware-probe-only kernel).
//
// Only one console session is ever active at a time on a given device
// today — mirrors this codebase's existing single-session conventions
// (one Lua VM, one loaded .claw module at a time) — so command handlers
// print through the one currently-running session's own I/O rather than
// a handle threaded through every call.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Returns a byte 0-255 read within timeout_ms, or -1 if none arrived
    // in time. Must never block longer than timeout_ms — the console loop
    // uses the timeout to stay responsive rather than to rate-limit.
    int (*read_byte)(uint32_t timeout_ms);

    // Raw byte write — no assumption about line endings. Callers that
    // want CRLF write it themselves; the built-in commands already do,
    // matching this codebase's existing "\r\n" convention on UART0 (see
    // the serial console this module replaces).
    void (*write)(const void *data, size_t len);

    void (*flush)(void);
} purr_console_io_t;

// One command's implementation. `args` is the remainder of the line after
// the verb, trimmed, never NULL (may be ""). Handlers print their output
// via purr_console_print()/purr_console_println() — those write to
// whichever I/O purr_console_run() is currently bound to; a handler never
// touches a purr_console_io_t directly.
typedef void (*purr_console_cmd_fn)(const char *args);

typedef struct {
    const char          *name;
    purr_console_cmd_fn  fn;
    const char          *help;   // one line, shown by the built-in "help" command
} purr_console_cmd_t;

// Registers additional commands beyond the built-in set (see console.c's
// own s_builtin_cmds[] for exactly what that is). For commands that are
// genuinely device-specific — e.g. an I2C bus scan, which needs a
// per-device SDA/SCL pin pair and has no business being baked into this
// shared, device-agnostic module — a specialized kernel's own boot.c
// calls this once at startup instead. `cmds` must stay valid for as long
// as the console might run (a static const array, same convention every
// other registration table in this codebase already uses) — not copied.
// Safe to call more than once to add more than one extra table.
void purr_console_register_commands(const purr_console_cmd_t *cmds, int count);

// A login gate — called once, before purr_console_run()'s REPL loop
// starts, when that call passed with_login=true (see below). Expected to
// block, retrying internally, until some identity is actually
// authenticated (a real getty/login never "fails" outward — it just asks
// again), then return.
//
// This is a REGISTERED HOOK, not a direct purr_console.c call into
// user_mgr/app_manager, and that is deliberate: this component must stay
// linkable with ZERO dependency on either, for the same reason
// rnode_module.c reaches pairing.c through a kernel callback instead of a
// direct #include (see purr_kernel.h's own "Radio-companion offload"
// comment) — linking user_mgr/app_manager symbols in unconditionally
// would drag both into any build that links this console core at all,
// including a future minimal/recovery-only one that must never depend on
// either. The actual login logic (user_mgr_verify() etc.) lives in
// whichever kernel_*_boot.c registers it, which already links those
// components for its own normal-boot purposes.
typedef void (*purr_console_login_fn)(const purr_console_io_t *io);

// Registers the login gate. NULL (the default, and the state this is left
// in for the whole life of a program that never calls this) means
// purr_console_run(..., with_login=true) skips authentication entirely —
// a device with no login concept at all is still a valid configuration,
// not an error.
void purr_console_set_login_fn(purr_console_login_fn fn);

// The `exec <app>` built-in's implementation — same registered-hook shape
// and reasoning as purr_console_login_fn above, and for the identical
// reason: launching a named app is app_manager_launch_by_name()'s job,
// and this component must not carry an unconditional link-time dependency
// on app_manager to expose that as a command. `args` is exactly what the
// user typed after "exec " (may be empty — the hook decides what that
// means, e.g. "usage: exec <app>"). Print any result via
// purr_console_print()/println(), same as any other command handler.
typedef void (*purr_console_exec_fn)(const char *args);

// NULL (the default) makes `exec` print a plain "not available on this
// build" instead of silently doing nothing — same "absent is a real,
// reportable state, not a crash" instinct as the login gate above.
void purr_console_set_exec_fn(purr_console_exec_fn fn);

// The line-reading primitive purr_console_run()'s own REPL loop uses —
// exposed so a registered login_fn (which runs on the same io, before the
// REPL loop starts) can prompt for a username/password with identical
// line editing (backspace) and echo-suppression (purr_console_set_echo())
// behavior, rather than reimplementing it. Blocks until '\r'/'\n', writes
// nothing but what's typed (no prompt text — the caller prints its own
// prompt via purr_console_print() first). Returns the number of bytes
// read into `buf` (NUL-terminated, truncated to bufsz-1 if a very long
// line is typed).
size_t purr_console_read_line(const purr_console_io_t *io, char *buf, size_t bufsz);

// Runs the read-eval-print loop on `io` until it returns (never, in
// practice — this is meant to be the entire body of a dedicated task, the
// same role each kernel_*_boot.c's own serial_console_task() played
// before). Handles line editing (backspace) and echo itself; `io` only
// has to move bytes.
//
// `with_login`: true runs the registered login gate (if any) first —
// this is the normal-boot case, "console as the head of boot" per the
// modular-core/Unix-boot plan. false skips it unconditionally, REGARDLESS
// of whether a login_fn is registered — the recoverable-panic case
// (purr_kernel_panic_ex()'s own use of this function): that path must
// never depend on user_mgr/app_manager being in a working state, since it
// exists specifically for when something elsewhere in the system already
// isn't. Passing false there is what guarantees that even if normal boot
// registered a login_fn earlier, a panic-context console never calls it.
void purr_console_run(const purr_console_io_t *io, bool with_login);

// Signals the CURRENTLY-RUNNING purr_console_run() loop to stop touching
// its bound io and park itself (an inert delay loop) at its own next safe
// point, instead of continuing to read/dispatch commands. A fresh
// purr_console_run() call clears this and starts normally.
//
// Exists specifically so a caller that needs to take over the same
// underlying peripheral (e.g. purr_kernel_set_panic_console_cb()'s
// registered implementation, handing a recoverable panic a live shell on
// the same UART/USB the normal-boot console was already using) does NOT
// have to vTaskDelete() the old console's task to do it. That was this
// project's first attempt, and it failed on real hardware: the old task
// can be anywhere at the instant of a panic, including mid-call inside
// its io's own driver (e.g. usb_serial_jtag_write_bytes()), and killing
// it there left that driver's internal locking state stuck — every
// subsequent call into it from the NEW console (including its own first
// banner print) then blocked forever, silently. This function instead
// only ever causes the old loop to stop at the top of its own read
// cycle — between io->read_byte() calls, never inside one — so the
// driver is always left in a clean, complete state. The caller still
// needs to wait a little (comfortably longer than one read_byte() poll
// interval) after calling this before assuming the old loop has actually
// parked; see purr_kernel_set_panic_console_cb()'s own doc comment and
// its real implementation for the exact shape.
void purr_console_request_stop(void);

// Suppresses echo of the line currently being typed — for password entry
// (a later phase's login prompt). The console still reads and assembles
// the line normally; it just doesn't write typed characters back to the
// bound I/O. Off by default. A caller turns this on immediately before
// prompting for a secret and off again immediately after — the same
// "don't let a secret linger longer than it has to" instinct
// systemui_login.c's own ctx->password memset already follows, applied
// here to what's echoed rather than to what's retained in RAM.
void purr_console_set_echo(bool enabled);

// Output helpers for command handlers — write to whichever io
// purr_console_run() is currently using. Harmless no-ops if called
// outside a running console loop.
void purr_console_print(const char *text);
void purr_console_println(const char *text);

#ifdef __cplusplus
}
#endif
