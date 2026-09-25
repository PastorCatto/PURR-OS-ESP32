// rnode_kiss.c — see rnode_kiss.h for the framing this implements and why
// it's a standalone, transport-agnostic byte-at-a-time state machine.

#include "rnode_kiss.h"
#include <string.h>

void rnode_kiss_decoder_reset(rnode_kiss_decoder_t *dec) {
    dec->len = 0;
    dec->in_frame = false;
    dec->escaped = false;
    dec->overflowed = false;
    dec->pending_clear = false;
}

// Clears len/escaped/overflowed for a fresh frame — split out from
// rnode_kiss_decoder_reset() (which also clears in_frame) because this
// needs calling from two different places below without touching
// in_frame both times.
static void clear_for_next_frame(rnode_kiss_decoder_t *dec) {
    dec->len = 0;
    dec->escaped = false;
    dec->overflowed = false;
    dec->pending_clear = false;
}

bool rnode_kiss_decoder_feed(rnode_kiss_decoder_t *dec, uint8_t byte) {
    // Deferred from the PREVIOUS call's completed frame (see rnode_kiss.h's
    // own comment on pending_clear) — do this first, before anything else
    // touches buf/len, now that the caller has had its one chance to read
    // the previous frame right after that call returned.
    if (dec->pending_clear) clear_for_next_frame(dec);

    if (byte == RNODE_KISS_FEND) {
        // Closing FEND of a real frame — deliver it, unless it was
        // actually the opening FEND of the next one (dec->len == 0, the
        // ordinary "consecutive FEND" case) or the frame overflowed and
        // has to be discarded. buf/len are deliberately left AS-IS here
        // when had_frame is true — the caller reads them right after this
        // returns; clearing happens lazily above, on the next call.
        bool had_frame = dec->in_frame && dec->len > 0 && !dec->overflowed;
        dec->in_frame = true;   // this FEND also opens the next frame
        if (had_frame) {
            dec->pending_clear = true;
        } else {
            clear_for_next_frame(dec);
        }
        return had_frame;
    }

    if (!dec->in_frame) {
        // Stray byte before any FEND was ever seen — ignore (matches
        // every real KISS decoder's own "sync to the next FEND" behavior).
        return false;
    }

    if (dec->overflowed) return false;   // discarding until the next FEND

    if (dec->escaped) {
        dec->escaped = false;
        uint8_t real_byte;
        if (byte == RNODE_KISS_TFEND) real_byte = RNODE_KISS_FEND;
        else if (byte == RNODE_KISS_TFESC) real_byte = RNODE_KISS_FESC;
        else real_byte = byte;   // malformed escape — pass the byte through rather than drop the frame
        if (dec->len >= RNODE_KISS_MAX_FRAME) { dec->overflowed = true; return false; }
        dec->buf[dec->len++] = real_byte;
        return false;
    }

    if (byte == RNODE_KISS_FESC) {
        dec->escaped = true;
        return false;
    }

    if (dec->len >= RNODE_KISS_MAX_FRAME) { dec->overflowed = true; return false; }
    dec->buf[dec->len++] = byte;
    return false;
}

size_t rnode_kiss_encode(uint8_t cmd, const uint8_t *data, size_t data_len,
                          uint8_t *out, size_t out_size) {
    size_t pos = 0;
    if (pos >= out_size) return 0;
    out[pos++] = RNODE_KISS_FEND;

    // The command byte itself is escaped exactly like any other data byte
    // would be if it happened to equal FEND/FESC — real KISS decoders
    // (RNodeInterface.py included) don't special-case the first byte, so
    // this encoder doesn't either.
    uint8_t all[1 + 600];
    if (data_len > sizeof(all) - 1) return 0;   // caller handed us more than any real frame should ever be
    all[0] = cmd;
    if (data_len) memcpy(all + 1, data, data_len);
    size_t all_len = 1 + data_len;

    for (size_t i = 0; i < all_len; i++) {
        uint8_t b = all[i];
        if (b == RNODE_KISS_FEND) {
            if (pos + 2 > out_size) return 0;
            out[pos++] = RNODE_KISS_FESC;
            out[pos++] = RNODE_KISS_TFEND;
        } else if (b == RNODE_KISS_FESC) {
            if (pos + 2 > out_size) return 0;
            out[pos++] = RNODE_KISS_FESC;
            out[pos++] = RNODE_KISS_TFESC;
        } else {
            if (pos + 1 > out_size) return 0;
            out[pos++] = b;
        }
    }

    if (pos >= out_size) return 0;
    out[pos++] = RNODE_KISS_FEND;
    return pos;
}
