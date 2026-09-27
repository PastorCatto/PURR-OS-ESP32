#pragma once
// mesh_ble.h — Meshtastic BLE phone-API companion service
//
// Implements Meshtastic's published BLE GATT service (toradio/fromradio/
// fromnum characteristics) so the official Meshtastic phone app can connect
// to this device exactly as it would to a real Meshtastic node — this is
// what makes MeshChat's mesh a "real" Meshtastic mesh from a phone's point
// of view too, not just PURR-to-PURR.
//
// IMPORTANT: the service/characteristic UUIDs below are reproduced from
// memory of Meshtastic's public BLE spec, not fetched from a live copy of
// the Meshtastic firmware source during this session — verify them against
// current github.com/meshtastic/firmware (BluetoothPhoneAPI / NimbleBluetooth)
// before relying on this for real phone interop. A single wrong hex digit
// makes the whole service invisible to the app.

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Registers this module's rx callback and hands bt_mgr a callback
// (bt_mgr_register_gatt_provider()) to queue this service's GATT table —
// NimBLE's controller/host don't come up at boot anymore (see bt_mgr.h),
// so unlike the old "called once after bt_mgr_init()" design, the actual
// ble_gatts_add_svcs() call happens much later, whenever bt_mgr_ensure_
// active() first runs. Advertising is a separate step
// (mesh_ble_set_advertising), gated on the user enabling Bluetooth — and
// is also what triggers that first activation if Settings' Bluetooth
// toggle was never touched.
int  mesh_ble_init(void);
void mesh_ble_deinit(void);

void mesh_ble_set_advertising(bool on);

#ifdef __cplusplus
}
#endif
