// purr_probe_dma.h — SPI2 bulk transfer over GDMA, with descriptor writeback
//
// The CPU-driven path (purr_probe_spi.c) tops out at 64 bytes through W0..W15.
// The real st7789 driver does not use it — it pushes framebuffers through GDMA
// with linked descriptors, so that is the path PURR OS actually runs and the
// one an emulator has to model to boot the real firmware.
//
// What a write-log cannot tell you, and this can:
//
//   - Does the DMA engine WRITE BACK to the descriptor when it finishes? On
//     this family, RX descriptors get `owner` cleared and `length` updated;
//     TX descriptors are widely assumed not to. Assumption is not measurement,
//     and firmware that polls `owner` to decide a buffer is reusable will spin
//     forever against an emulator that guesses wrong in either direction.
//   - Which of SPI's own DMA_CONF bits must be set, and which self-assert.
//   - Whether TRANS_DONE still marks completion, or DMA has its own signal.
//
// Channel allocation and the peripheral-to-channel connection go through IDF's
// gdma driver for the same reason spi_bus_initialize() does the pin matrix:
// that part is well understood, and a hand-rolled version would only risk a
// broken baseline. The descriptor and the SPI-side DMA registers are driven
// raw, because those are what is being characterised.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROBE_DMA_MAX_XFER 2048

// A descriptor's three words, captured verbatim before and after the transfer.
typedef struct {
    uint32_t dw0;       // size / length / suc_eof / owner, packed
    uint32_t buffer;
    uint32_t next;
    // dw0 decoded, so a reader does not have to do bit surgery by hand.
    uint32_t size, length;
    uint8_t  suc_eof, owner;
} probe_dma_desc_t;

typedef struct {
    probe_dma_desc_t before;
    probe_dma_desc_t after;
    bool     owner_cleared;      // did the engine hand the buffer back?
    bool     length_updated;
    uint32_t spi_dma_conf_after;
    uint32_t spi_dma_int_raw_after;
    uint32_t cycles_to_done;
    bool     trans_done_seen;
    bool     timed_out;
    uint32_t bytes;
} probe_dma_result_t;

// Full-duplex result: the TX descriptor above, plus the RX side.
//
// RX is the half that matters for a panel READ, and the half where the
// writeback question has a different answer. The TX measurement showed `owner`
// staying 1 and `length` untouched; RX descriptors on this family are supposed
// to be handed back — `owner` cleared and `length` set to what actually
// arrived. "Supposed to" is why this exists.
//
// It also decides how firmware learns a read completed. If RX writeback works,
// polling `owner` is viable; if it does not, the only signal is the interrupt,
// and a driver waiting on the wrong one hangs — which is the shape of a
// display read that times out.
typedef struct {
    probe_dma_desc_t rx_before;
    probe_dma_desc_t rx_after;
    bool     rx_owner_cleared;
    bool     rx_length_updated;
    uint32_t rx_length;          // what the engine reported receiving
    uint8_t  rx_first[8];        // first bytes landed in the RX buffer
    bool     rx_buffer_changed;  // did anything at all get written?
} probe_dma_rx_t;

// What IDF thinks it allocated, and the descriptor addresses actually handed to
// gdma_start(). Pairing these with the GDMA channel registers is what separates
// "the driver never programmed the engine" from "we are reading the wrong
// channel" — the two look identical from a descriptor that never advances.
//
// Any of the out pointers may be NULL. Returns ESP_ERR_INVALID_STATE if the
// channels are not open.
esp_err_t probe_dma_channel_info(int *tx_id, int *rx_id,
                                 uint32_t *tx_desc_addr, uint32_t *rx_desc_addr);

esp_err_t probe_dma_open(void);
void      probe_dma_close(void);
bool      probe_dma_is_open(void);

// One DMA transmit of `len` bytes. `pattern` fills the buffer (so a host does
// not have to send 2 KB over the wire to exercise a 2 KB transfer).
esp_err_t probe_dma_xfer(uint8_t pattern, size_t len, int dc_level,
                         probe_dma_result_t *out);

// Full duplex: TX and RX descriptors on separate GDMA channels, both armed for
// the same transfer. `out` carries the TX side, `rx` the RX side.
esp_err_t probe_dma_xfer_duplex(uint8_t pattern, size_t len, int dc_level,
                                probe_dma_result_t *out, probe_dma_rx_t *rx);

#ifdef __cplusplus
}
#endif
