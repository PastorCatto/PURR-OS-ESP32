#pragma once
// proximity_rpc.h — reliable request/response RPC layer over
// proximity_module.c's ESP-NOW transport (PROXIMITY_FRAME_RELAY).
//
// proximity_send_unicast() is fire-and-forget with a 249-byte hard cap and
// no reply/correlation of any kind — fine for pairing's tiny fixed-size
// handshake messages, not enough for anything that needs to move real data
// (an app list, a chat message) and get a matching answer back. This layer
// adds exactly that, and nothing else:
//
//   - Correlation: each call gets a locally-generated sequence ID; the
//     response is matched back to the waiting caller by that ID.
//   - Timeout: proximity_rpc_call() is a blocking call with a caller-
//     specified bound — ESP-NOW unicast has no app-level ack, so a lost
//     frame must not hang the caller forever (same "never trust an
//     unbounded wait" lesson as this project's SPI-bus work).
//   - Chunking: a logical request/response larger than one frame's payload
//     budget is split into numbered chunks and reassembled before dispatch.
//     Chunks are assumed to arrive in order (no reordering/bitmap
//     reassembly) — true for a single ESP-NOW unicast burst in practice on
//     this hardware, not a protocol-level guarantee; a genuinely reordered
//     or dropped chunk just times out the whole call, same as any other
//     lost frame.
//   - Authorization: every inbound frame is checked against
//     pairing_is_trusted() before being dispatched anywhere. This is the
//     entire security boundary (see pairing.h — no encryption exists);
//     enforced once, here, not re-implemented per action.
//
// Runs entirely on proximity_task()'s own thread for frame handling (same
// possibly-PSRAM-stack caveat as pairing_module.c's pairing_on_frame() —
// registered action handlers must not touch NVS/flash directly) plus
// whatever task calls proximity_rpc_call() itself, which blocks until a
// response arrives or the timeout elapses. Never call proximity_rpc_call()
// from proximity_task() itself (the response can never be dispatched while
// the caller is blocking that same task) or from cupcake_task (blocks the
// UI render loop for up to timeout_ms) — call it from a dedicated
// app-owned task, same convention as MSN's buddy_refresh_task/mesh_task
// pattern for anything that can take real wall-clock time.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int  proximity_rpc_init(void);
void proximity_rpc_deinit(void);

// Total reassembled message size budget (request or response), each
// direction independent. Comfortably covers a Remote Apps enumerate
// response (dozens of apps @ ~40 bytes each) without being large enough to
// make a malicious/malformed chunk_count claim expensive to buffer.
#define PROXIMITY_RPC_MAX_MSG 2048

// action_id namespacing is the caller's responsibility — Phase C (Remote
// Apps) and Phase D (per-app actions like msn.*) each own a distinct
// range/prefix by convention, not enforced here.
//
// req/req_len: the reassembled request payload. Write the response into
// resp_out (capacity resp_cap), set *resp_len_out, and return true — or
// return false to send back a generic failure status (caller's
// proximity_rpc_call() sees this as a failed call, not a timeout).
typedef bool (*proximity_rpc_handler_t)(const uint8_t mac[6], uint16_t action_id,
                                         const uint8_t *req, size_t req_len,
                                         uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out);

// Registers the handler for action_id. At most one handler per ID; a
// second registration replaces the first. Only takes effect for inbound
// requests from a pairing_is_trusted() mac — untrusted senders are
// dropped before this is ever consulted.
void proximity_rpc_register(uint16_t action_id, proximity_rpc_handler_t handler);

// Blocking call — see this header's top comment for which tasks must never
// call this. mac must already be pairing_is_trusted() (checked here;
// returns false immediately otherwise, no frame sent). Returns true and
// fills resp_out/*resp_len_out on a reply received within timeout_ms;
// false on timeout, an untrusted mac, or a handler-reported failure on the
// remote side.
bool proximity_rpc_call(const uint8_t mac[6], uint16_t action_id,
                         const uint8_t *req, size_t req_len,
                         uint8_t *resp_out, size_t resp_cap, size_t *resp_len_out,
                         uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
