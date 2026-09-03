// purr_probe_cmd.h — the host-facing command loop.
//
// Line protocol over the T-Deck's native USB port. Runs forever; never returns.
// Refuses to start unless probe_guard_selftest() has armed the access gate.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void probe_cmd_loop(void);

#ifdef __cplusplus
}
#endif
