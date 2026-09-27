#pragma once
// rnode_proto.h — the RNode extended command state machine.
//
// Transport-agnostic on purpose (same "protocol logic decoupled from any
// one transport" split as rns_radio_adapter.cpp vs. reticulum_module.cpp
// — see the plan doc): this file knows nothing about BLE. It consumes
// already-KISS-decoded frames (cmd byte + data, see rnode_kiss.h) and
// drives the real radio via catcall_radio_t (purr_kernel_radio()) —
// exactly the same shared driver instance meshtastic/meshcore/reticulum
// already take turns owning, one at a time, via purr_kernel_mesh_
// backend_t's mutual exclusion (rnode_module.c's own init() guard).
//
// Every CMD_*/DETECT_RESP byte value and wire encoding here is confirmed
// directly from RNS's own live RNS/Interfaces/RNodeInterface.py (see the
// plan doc's "Stage 1 protocol constants" section) — not guessed.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called with one fully KISS-encoded frame (FEND ... FEND) ready to send
// to the host — the transport (rnode_ble.c) just needs to push these
// bytes out, chunked to whatever its own MTU is; no additional framing
// needed, KISS's own byte-stuffing already survives being split across
// multiple writes/notifies (see rnode_kiss.h's decoder).
typedef void (*rnode_proto_output_fn)(const uint8_t *kiss_frame, size_t len);

void rnode_proto_init(rnode_proto_output_fn output);
void rnode_proto_deinit(void);

// Called once per fully-decoded incoming frame (frame[0] = RNode command
// byte, frame[1..len) = its data) — rnode_ble.c's own RX-characteristic
// write handler runs bytes through an rnode_kiss_decoder_t and calls this
// each time it completes one. Safe to call from any task context (NimBLE
// host task included) — internally serialized against rnode_proto_poll_
// radio() below via this file's own radio-access mutex, same reasoning
// meshtastic_module.c's mesh_radio_lock()/unlock() already established
// for the identical hazard (two different tasks touching one physical
// radio).
void rnode_proto_handle_frame(const uint8_t *frame, size_t len);

// Drains one pending inbound radio packet, if any (data_available() +
// receive(), reassembling a split transmission first if needed — see
// rnode_proto.c's own header comment for the wire format), and pushes it
// to the host as a CMD_DATA frame via the output callback. Call this
// periodically from a dedicated task (rnode_module.c owns it) — not
// reentrant with itself, safe to call concurrently with rnode_proto_
// handle_frame() from a different task.
void rnode_proto_poll_radio(void);

#ifdef __cplusplus
}
#endif
