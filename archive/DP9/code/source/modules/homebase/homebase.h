#pragma once
// homebase.h — presence watcher for the paired "home base" device (see
// pairing.h's pairing_set_home_base()/pairing_get_home_base()).
//
// Reuses proximity_module.c's own continuously-pruned nearby-device table
// as the sole source of truth: a device is "present" exactly when
// proximity_device_at() currently returns an entry for its MAC. Proximity
// already ages out a device whose beacons stopped arriving (STALE_MS,
// proximity_module.c), so this module invents no timeout of its own — it
// only adds edge-detection (present/absent transitions) and a cheap cached
// query so callers (MSN's home-base relay) don't have to scan the
// proximity table themselves on every send.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int  homebase_init(void);
void homebase_deinit(void);

// Cheap cached read — safe to call from any task, including cupcake_task
// (no blocking, no NVS/RPC access). False if no home base is set, or it's
// set but not currently in proximity range.
bool homebase_is_present(void);

#ifdef __cplusplus
}
#endif
