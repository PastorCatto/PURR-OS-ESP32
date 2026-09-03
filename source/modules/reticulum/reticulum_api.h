#pragma once
// reticulum_api.h — C-callable bridge into reticulum_module.cpp (the vendored
// Reticulum Network Stack, see vendor/VENDORED.md), for plain-C consumers
// like source/apps/system/reticulum/ (the standalone chat app). Same
// "C++ module, C-callable header" shape meshcore/meshtastic already use
// to expose themselves to msn.c.
//
// Scope matches the plan doc's Stage 3: one active chat Link at a time,
// plain text over it — not LXMF, not multi-conversation. Peers are
// whatever's been heard via Announce since this module started (in-RAM
// only, not persisted).

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RETICULUM_HASH_HEX_LEN 33   // 16-byte destination hash, hex + NUL
#define RETICULUM_PEER_NAME_MAX 32

// This device's own destination hash, hex-encoded — "" if the module
// hasn't finished starting yet.
void reticulum_own_hash(char *out, size_t out_len);

// Peers heard via Announce since boot, most-recently-heard order isn't
// guaranteed — just a flat list. name is whatever the peer's own
// hostname was at announce time (purr_kernel_hostname_get() on their
// side — see reticulum_module.cpp), "(unnamed)" if they didn't send one.
int  reticulum_peer_count(void);
bool reticulum_peer_at(int idx, char *hash_hex_out, size_t hash_hex_len,
                        char *name_out, size_t name_len);

typedef enum {
    RETICULUM_LINK_NONE      = 0,   // no link attempted, or it fully closed
    RETICULUM_LINK_PENDING   = 1,
    RETICULUM_LINK_HANDSHAKE = 2,
    RETICULUM_LINK_ACTIVE    = 3,
    RETICULUM_LINK_STALE     = 4,
} reticulum_link_status_t;

// Opens (or re-opens) the one active chat link, to a peer identified by
// its hex destination hash — see reticulum_peer_at() above. False if the
// peer's Identity hasn't been recalled yet (no announce heard from them)
// or a link is already open. Asynchronous — poll reticulum_link_status()
// for HANDSHAKE -> ACTIVE.
bool reticulum_link_open(const char *peer_hash_hex);
void reticulum_link_close(void);
reticulum_link_status_t reticulum_link_status(void);

// True once an incoming link request has been accepted (this device is
// the one being connected TO, not the initiator) — same status/send/poll
// calls work either way once a link is ACTIVE.
bool reticulum_link_is_incoming(void);

// Sends one message over the currently-active link. False if no link is
// ACTIVE yet.
bool reticulum_chat_send(const char *text);

// Poll-based receive, same "safe to call from a background task" shape
// purr_win's own setters already follow elsewhere in this codebase — call
// periodically from the app's own refresh task. Drains one pending
// message per call (text_out NUL-terminated, truncated to text_len if
// needed); returns false once nothing's left pending.
bool reticulum_chat_poll(char *text_out, size_t text_len);

#ifdef __cplusplus
}
#endif
