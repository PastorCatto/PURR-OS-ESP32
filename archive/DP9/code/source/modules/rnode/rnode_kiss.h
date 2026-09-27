#pragma once
// rnode_kiss.h — KISS byte-stuffing codec, transport-agnostic.
//
// Real KISS framing (confirmed via direct fetch of Reticulum's own
// RNS/Interfaces/RNodeInterface.py — see the plan doc): a frame is
// FEND <command byte> <possibly-escaped data...> FEND. Only two bytes
// ever need escaping inside the body: FEND itself (0xC0) and FESC (0xDB).
// This file only knows about that byte-stuffing — it has no idea what the
// command byte or data mean (that's rnode_proto.c's job) and no idea
// where the bytes come from (BLE, UART, whatever — that's rnode_ble.c's
// job). Same "small, single-purpose, decoupled from any one transport"
// split this codebase already uses for rns_radio_adapter.cpp vs.
// reticulum_module.cpp.
//
// The decoder is a streaming byte-at-a-time state machine deliberately —
// a BLE central can (and will) split one logical KISS frame across
// several separate characteristic writes depending on its own MTU, so
// there is no guarantee a whole frame ever arrives in one call.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RNODE_KISS_FEND  0xC0
#define RNODE_KISS_FESC  0xDB
#define RNODE_KISS_TFEND 0xDC
#define RNODE_KISS_TFESC 0xDD

// Largest decoded (unescaped) frame this decoder will assemble, command
// byte included. 600 — comfortably above the 508-byte HW_MTU
// rns_radio_adapter.cpp already established for this codebase's own
// split/reassemble scheme (see rnode_proto.h), plus the one command byte
// and a little margin. A frame that would overflow this is dropped (see
// rnode_kiss_decoder_feed()'s own doc comment) rather than silently
// truncated.
#define RNODE_KISS_MAX_FRAME 600

typedef struct {
    uint8_t buf[RNODE_KISS_MAX_FRAME];
    size_t  len;        // bytes assembled so far — see pending_clear below for the one subtlety
    bool    in_frame;   // seen an opening FEND, still inside the frame
    bool    escaped;    // last byte was FESC — next byte is transposed
    bool    overflowed; // this frame exceeded RNODE_KISS_MAX_FRAME — discard until next FEND
    // Set the instant rnode_kiss_decoder_feed() returns true (a frame just
    // completed) — buf[0..len) is deliberately left untouched at that
    // point so the caller can still read it after the call returns; the
    // actual len/escaped/overflowed reset for the *next* frame happens
    // lazily, on this flag, at the top of the very next feed() call
    // instead. Never read/set this from outside rnode_kiss.c.
    bool    pending_clear;
} rnode_kiss_decoder_t;

void rnode_kiss_decoder_reset(rnode_kiss_decoder_t *dec);

// Feed one raw transport byte in. Returns true exactly when this byte
// completed a non-empty frame — dec->buf[0..dec->len) is then the decoded
// frame (buf[0] is the RNode command byte, buf[1..] is its data), valid
// until the next call. Consecutive FENDs (empty frames — common at the
// very start of a stream, or between back-to-back sends) are silently
// absorbed, matching every real KISS implementation's own behavior, not
// surfaced as empty completed frames.
bool rnode_kiss_decoder_feed(rnode_kiss_decoder_t *dec, uint8_t byte);

// Encodes one frame (cmd + data) into KISS wire format: FEND, cmd,
// escaped data, FEND. Returns the encoded length written to out, or 0 if
// it wouldn't fit in out_size (out is left in an undefined state in that
// case — callers size out generously, see RNODE_KISS_MAX_FRAME's own
// comment for the margin this needs: escaping can at most double the
// data portion).
size_t rnode_kiss_encode(uint8_t cmd, const uint8_t *data, size_t data_len,
                          uint8_t *out, size_t out_size);

#ifdef __cplusplus
}
#endif
