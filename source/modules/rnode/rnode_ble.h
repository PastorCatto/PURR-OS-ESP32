#pragma once
// rnode_ble.h — NimBLE transport for RNode mode: the real Nordic UART
// Service (NUS), not a PURR-invented GATT shape. See the plan doc's
// "Two protocol facts" section for why matching NUS's real UUIDs exactly
// (not inventing new ones) is what lets unmodified Sideband/`rns`/
// NomadNet connect to this device as if it were a real RNode.
//
// Structurally a near-mirror of meshtastic/mesh_ble.c — same lazy bring-
// up via bt_mgr_register_gatt_provider()/bt_mgr_ensure_active(), same
// bt_mgr_host_ready() guard before touching NimBLE, same drop-oldest
// outbound queue shape — adapted to NUS's 2-characteristic (RX write, TX
// notify) layout instead of meshtastic's 3.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int  rnode_ble_init(void);
void rnode_ble_deinit(void);

void rnode_ble_set_advertising(bool on);

#ifdef __cplusplus
}
#endif
