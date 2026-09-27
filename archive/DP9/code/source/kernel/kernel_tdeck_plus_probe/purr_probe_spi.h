// purr_probe_spi.h — register-level SPI2 transactions with a full access trace
//
// Exists to answer the questions a register write log cannot: what the hardware
// READS BACK. A host that has captured every write the firmware makes still has
// to guess what SPI_CMD_REG returns mid-transfer, whether USR self-clears or
// latches until acknowledged, how many cycles elapse before TRANS_DONE sets,
// and what MISO produces with nothing driving it. Each of those is a coin flip
// when writing an emulator, and one wrong guess is a hang.
//
// So this drives the transaction itself through raw register accesses and
// records EVERY access — address, value, direction, CPU cycle — in order.
// The bus and pin matrix are still brought up by the IDF driver, because
// nobody learns anything from a hand-rolled reimplementation of clock divider
// configuration, and getting it wrong would only produce a broken baseline.
//
// Every access here goes through purr_probe_guard.h like everything else.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROBE_SPI_MAX_EVENTS 256
#define PROBE_SPI_MAX_XFER   64      // CPU-driven via W0..W15, so 64 bytes

typedef struct {
    uint32_t cycle;   // esp_cpu_get_cycle_count() sampled at the access
    uint32_t addr;
    uint32_t value;   // value written, or value read back
    uint8_t  write;   // 1 = write, 0 = read
} probe_spi_ev_t;

// Timing landmarks for the last transaction, in CPU cycles. Deltas rather than
// absolutes are what an emulator needs to match.
typedef struct {
    uint32_t cyc_usr_set;       // cycle at which SPI_USR was written
    uint32_t cyc_usr_clear;     // cycle at which CMD.USR first read back 0
    uint32_t cyc_trans_done;    // cycle at which TRANS_DONE_INT_RAW first read 1
    uint32_t polls_until_clear; // how many CMD_REG reads that took
    bool     usr_cleared;       // false => timed out with USR still set
    bool     trans_done_seen;   // false => TRANS_DONE never asserted
} probe_spi_timing_t;

// Bring up SPI2 on the T-Deck Plus display bus and write a known-good master
// configuration, tracing every register access. Safe to call repeatedly.
esp_err_t probe_spi_open(uint32_t hz, uint8_t spi_mode);
void      probe_spi_close(void);
bool      probe_spi_is_open(void);

// One full-duplex transaction of `len` bytes with CS asserted around it and DC
// held at `dc_level` throughout. `rx` may be NULL. Clears the trace first, so
// the trace afterwards describes exactly this transaction.
esp_err_t probe_spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len, int dc_level);

// ── High-resolution timing ───────────────────────────────────────────────────
//
// probe_spi_xfer()'s poll loop reads CMD_REG and DMA_INT_RAW in the same
// iteration, ~480 cycles apart, with ~1400 cycles between iterations. That is
// far too coarse to order two events that occur within a few hundred cycles of
// each other, so the trace CANNOT establish whether TRANS_DONE asserts before
// or after CMD.USR clears — and firmware waiting on the wrong one of the two
// is exactly the bug that presents as an unexplained hang.
//
// This resolves it by removing the skew instead of measuring around it: it runs
// the same transfer twice, each time polling exactly ONE register in a tight
// untraced loop. Repeated over many iterations so jitter is visible rather than
// averaged away.
typedef struct {
    uint32_t iterations;
    uint32_t usr_min, usr_max, usr_mean;   // cycles from CMD.USR write to USR clear
    uint32_t td_min,  td_max,  td_mean;    // cycles from CMD.USR write to TRANS_DONE
    uint32_t usr_fail, td_fail;            // iterations that timed out
} probe_spi_timing_stats_t;

esp_err_t probe_spi_timing_probe(const uint8_t *tx, size_t len, int dc_level,
                                 uint32_t iterations, probe_spi_timing_stats_t *out);

// ── Async start, for interrupt characterisation ──────────────────────────────
//
// Loads the transfer, asserts CS, writes CMD.USR and RETURNS — no polling, so
// the caller can wait on an interrupt instead and time it. Kept here rather
// than duplicated in purr_probe_int.c so all SPI2 register knowledge stays in
// one file. `cyc_usr_set` is the cycle CMD.USR was written, which is the zero
// point every interrupt-latency figure is measured from.
esp_err_t probe_spi_start_async(const uint8_t *tx, size_t len, int dc_level,
                                uint32_t *cyc_usr_set);

// Wait for USR to clear (bounded), then release CS. Safe to call even if the
// transfer already finished.
void probe_spi_finish_async(void);

// Trace readout.
void                     probe_spi_trace_clear(void);
size_t                   probe_spi_trace_count(void);
const probe_spi_ev_t    *probe_spi_trace_at(size_t i);
const probe_spi_timing_t *probe_spi_last_timing(void);

// Name of a SPI2 register by absolute address, for readable traces
// ("CMD", "USER", "MS_DLEN", "W0"...). Returns NULL if not a known SPI2 reg.
const char *probe_spi_reg_name(uint32_t addr);

#ifdef __cplusplus
}
#endif
