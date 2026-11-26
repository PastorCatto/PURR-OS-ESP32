/*
 * purr_rdisp.h - the remote display wire code (RemoteDisplay/SPEC.md): the message layouts, the three pixel codecs, splitting a rectangle into pieces, and merging the
 * rectangles a screen redrew. Plain C with no dependencies, shared by the host (an S3 running PURR) and the client (a CYD with a tiny image), and tested on the host.
 *
 * All numbers on the wire are little-endian. Every decoder here is given the exact number of bytes it must produce and refuses anything that would write past it.
 */
#ifndef PURR_RDISP_H
#define PURR_RDISP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_RD_MAJOR 1
#define PURR_RD_MINOR 0
#define PURR_RD_HDR_LEN 8
#define PURR_RD_MAX_PAYLOAD 65536
#define PURR_RD_TCP_PORT 47803
#define PURR_RD_DISCOVERY_PORT 47802

/* message types */
#define PURR_RD_T_HELLO   0x01
#define PURR_RD_T_WELCOME 0x02
#define PURR_RD_T_PAIR    0x03
#define PURR_RD_T_AUTH    0x04
#define PURR_RD_T_RECT    0x10
#define PURR_RD_T_COPY    0x11
#define PURR_RD_T_PRESENT 0x12
#define PURR_RD_T_FILL    0x13
#define PURR_RD_T_INPUT   0x20
#define PURR_RD_T_ACK     0x21
#define PURR_RD_T_PING    0x30
#define PURR_RD_T_PONG    0x31
#define PURR_RD_T_BYE     0x3F
#define PURR_RD_T_CACHE_STORE 0x40      /* reserved */
#define PURR_RD_T_CACHE_BLIT  0x41      /* reserved */
#define PURR_RD_T_ERROR   0x7F

/* codecs and pixel formats */
#define PURR_RD_CODEC_RAW   0
#define PURR_RD_CODEC_RLE16 1
#define PURR_RD_CODEC_LZ4   2
#define PURR_RD_CODEC_BIT(c) (1u << (c))
#define PURR_RD_FMT_RGB565 0
#define PURR_RD_FMT_MONO1  1

/* HELLO caps */
#define PURR_RD_CAP_FILL  (1u << 0)
#define PURR_RD_CAP_COPY  (1u << 1)
#define PURR_RD_CAP_CACHE (1u << 2)
#define PURR_RD_CAP_TOUCH (1u << 3)
#define PURR_RD_CAP_KEYS  (1u << 4)

/* INPUT kinds */
#define PURR_RD_IN_TOUCH_DOWN 0
#define PURR_RD_IN_TOUCH_MOVE 1
#define PURR_RD_IN_TOUCH_UP   2
#define PURR_RD_IN_KEY_DOWN   3
#define PURR_RD_IN_KEY_UP     4

/* ---------------------------------------------------------------- messages */

typedef struct {
    uint8_t type, flags;
    uint16_t seq;
    uint32_t len;
} purr_rd_hdr_t;

/* 8 bytes written, or -1 (short buffer, or len over PURR_RD_MAX_PAYLOAD). */
int purr_rd_hdr_put(const purr_rd_hdr_t *h, uint8_t *out, int cap);
/* 8, -1 for fewer than 8 bytes, -2 for a len over PURR_RD_MAX_PAYLOAD (the session ends). */
int purr_rd_hdr_get(const uint8_t *in, int n, purr_rd_hdr_t *h);

typedef struct { uint16_t x, y, w, h; } purr_rd_rect_t;

typedef struct { purr_rd_rect_t r; uint8_t codec, fmt; } purr_rd_rect_msg_t;     /* then the data */
#define PURR_RD_RECT_HEAD 10
int purr_rd_rect_put(const purr_rd_rect_msg_t *m, uint8_t *out, int cap);
int purr_rd_rect_get(const uint8_t *in, int n, purr_rd_rect_msg_t *m);

typedef struct { purr_rd_rect_t r; uint16_t colour; } purr_rd_fill_t;
#define PURR_RD_FILL_LEN 10
int purr_rd_fill_put(const purr_rd_fill_t *m, uint8_t *out, int cap);
int purr_rd_fill_get(const uint8_t *in, int n, purr_rd_fill_t *m);

typedef struct { uint16_t sx, sy, w, h, dx, dy; } purr_rd_copy_t;
#define PURR_RD_COPY_LEN 12
int purr_rd_copy_put(const purr_rd_copy_t *m, uint8_t *out, int cap);
int purr_rd_copy_get(const uint8_t *in, int n, purr_rd_copy_t *m);

typedef struct { uint8_t kind, id; uint16_t x, y; } purr_rd_input_t;
#define PURR_RD_INPUT_LEN 6
int purr_rd_input_put(const purr_rd_input_t *m, uint8_t *out, int cap);
int purr_rd_input_get(const uint8_t *in, int n, purr_rd_input_t *m);

typedef struct {
    uint8_t major, minor;
    uint16_t width, height;
    uint8_t formats, codecs;
    uint16_t caps, max_px, rx_buf;
    char board[16];
    uint8_t pubkey[64];
} purr_rd_hello_t;
#define PURR_RD_HELLO_LEN 94
int purr_rd_hello_put(const purr_rd_hello_t *m, uint8_t *out, int cap);
int purr_rd_hello_get(const uint8_t *in, int n, purr_rd_hello_t *m);

typedef struct {
    uint8_t major, minor, format, codecs;
    uint16_t width, height, max_px;
    uint8_t window, reserved;
    uint32_t session;
} purr_rd_welcome_t;
#define PURR_RD_WELCOME_LEN 16
int purr_rd_welcome_put(const purr_rd_welcome_t *m, uint8_t *out, int cap);
int purr_rd_welcome_get(const uint8_t *in, int n, purr_rd_welcome_t *m);

/* ---------------------------------------------------------------- codecs */

/* Bytes of raw pixel data for a w x h rectangle in a format (RGB565: 2 per pixel; MONO1: rows of ceil(w/8)); -1 for an unknown format. */
int purr_rd_raw_bytes(int fmt, int w, int h);

/* RLE16 works in 2-byte units (n_bytes must be even): a control byte, high bit set = a run of (low 7 bits + 1) copies of the next unit; clear = that many literal units follow.
 * Encode returns the bytes written or -1 (odd length, or it does not fit in cap). Decode returns 0 only if it produced exactly n_bytes; -1 otherwise (nothing past n_bytes is written). */
int purr_rd_rle16_encode(const uint8_t *src, int n_bytes, uint8_t *dst, int cap);
int purr_rd_rle16_decode(const uint8_t *src, int n, uint8_t *dst, int n_bytes);

/* The standard LZ4 block format (no frame). n at most 65534. `table` is scratch for the encoder: 4096 entries (8 KB). Encode returns the bytes written or -1 (too long, or does not fit).
 * Decode returns 0 only if it produced exactly n_bytes; it never writes past n_bytes and never reads past src + n. */
#define PURR_RD_LZ4_TABLE 4096
int purr_rd_lz4_encode(const uint8_t *src, int n, uint8_t *dst, int cap, uint16_t *table);
int purr_rd_lz4_decode(const uint8_t *src, int n, uint8_t *dst, int n_bytes);

/* Tries the codecs in `codec_mask` (bits of PURR_RD_CODEC_BIT; RLE16 only for RGB565) and writes the smallest result to out; RAW (a copy, n bytes) when nothing is smaller than n.
 * Returns the bytes written and the codec in *codec, or -1 if even RAW does not fit in cap. `lz_table` may be NULL when LZ4 is not in the mask. */
int purr_rd_encode_best(const uint8_t *raw, int n, int fmt, unsigned codec_mask, uint8_t *out, int cap, uint16_t *lz_table, uint8_t *codec);
/* Decodes one RECT's data into dst (exactly n_bytes = purr_rd_raw_bytes(...)); 0, or -1 for an unknown codec, bad data, or the wrong length. */
int purr_rd_decode(int codec, const uint8_t *in, int n, uint8_t *dst, int n_bytes);

/* ---------------------------------------------------------------- rectangles */

/* Cuts a rectangle into pieces of at most max_px pixels (full-width bands, or, for a rectangle wider than max_px, pieces one row high). Returns the number of pieces written to out,
 * or -1 if there would be more than max_out (or max_px < 1). Together the pieces cover the rectangle exactly, with no overlap. */
int purr_rd_split(purr_rd_rect_t in, int max_px, purr_rd_rect_t *out, int max_out);

#define PURR_RD_DAMAGE_MAX 8
typedef struct {
    purr_rd_rect_t r[PURR_RD_DAMAGE_MAX];
    int n;
    int w, h;
} purr_rd_damage_t;

void purr_rd_damage_init(purr_rd_damage_t *d, int screen_w, int screen_h);
/* Adds what was redrawn (clipped to the screen; an empty rectangle is ignored). Rectangles that overlap, or whose union costs little more than the two apart, are merged; when there are
 * too many the two that cost least to join are joined, so at most PURR_RD_DAMAGE_MAX remain and every pixel added is still inside one of them. */
void purr_rd_damage_add(purr_rd_damage_t *d, int x, int y, int w, int h);
/* Copies the rectangles out (up to max) and empties the set; returns how many. */
int purr_rd_damage_take(purr_rd_damage_t *d, purr_rd_rect_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* PURR_RDISP_H */
