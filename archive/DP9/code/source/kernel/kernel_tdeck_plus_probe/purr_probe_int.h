// purr_probe_int.h — SPI2 interrupt characterisation
//
// Prioritised ahead of DMA on a report from the emulator side that a live bug
// sits in the interrupt path. It is also the cheaper of the two to build, so
// there was no reason to argue.
//
// Everything the probe did before this was POLLED. That leaves an emulator
// guessing at the entire interrupt story, and the guesses are the expensive
// kind: firmware that arms an interrupt and blocks waiting for it does not fail
// visibly when the model is wrong, it simply never wakes up.
//
// The specific unknowns:
//
//   - Is INT_ST just (INT_RAW & INT_ENA), or is it latched independently?
//   - Does writing INT_ENA retroactively surface an ALREADY-SET raw bit, or
//     only ones that arrive afterwards? (This decides whether firmware that
//     arms late misses the event — a classic lost-wakeup.)
//   - Does clearing INT_ENA deassert the CPU interrupt without touching RAW?
//   - Does the interrupt actually reach the CPU, on which core, and how many
//     cycles after the hardware event?
//   - ETS_SPI2_INTR_SOURCE is documented LEVEL-triggered. Confirm it: a level
//     interrupt whose handler neither clears RAW nor masks ENA re-enters
//     immediately and livelocks the core.
//
// The first three are answered by probe_int_matrix() with no CPU interrupt
// involved at all — pure register observation, nothing to wedge. Only
// probe_int_fire() installs a real handler.

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// One row of the ENA/ST truth table, gathered without routing anything to the
// CPU. `raw_preset` records whether TRANS_DONE was already set in RAW when ENA
// was written — the lost-wakeup question.
typedef struct {
    uint32_t ena_written;
    uint32_t raw_before, st_before;
    uint32_t raw_after,  st_after;
    bool     raw_preset;
} probe_int_row_t;

#define PROBE_INT_MAX_ROWS 8

// Observe INT_RAW / INT_ENA / INT_ST interactions. Never enables the CPU
// interrupt, so this is safe to run on a board nobody is watching.
esp_err_t probe_int_matrix(probe_int_row_t *rows, size_t max, size_t *out_n);

// What a real ISR saw.
typedef struct {
    uint32_t count;        // ISR entries for one transfer (>1 means re-entry)
    uint32_t cyc_isr;      // cycle at first ISR entry
    uint32_t core;         // core the ISR ran on
    uint32_t st_at_entry;  // INT_ST read inside the ISR
    uint32_t raw_at_entry; // INT_RAW read inside the ISR
    uint32_t raw_after;    // INT_RAW after the ISR masked ENA — still set?
    uint32_t st_after;     // INT_ST likewise
    uint32_t cyc_usr_set;  // cycle CMD.USR was written, for the latency delta
    bool     fired;
} probe_int_result_t;

// Install a real handler, arm TRANS_DONE, run one transfer, report what
// happened. The handler MASKS (clears INT_ENA) rather than clearing INT_RAW:
// that deasserts a level-triggered line safely while leaving RAW intact, so the
// caller can then observe whether RAW survived independently of the mask —
// which is the question. Clearing RAW in the handler would destroy the evidence.
esp_err_t probe_int_fire(const uint8_t *tx, size_t len, int dc_level,
                         probe_int_result_t *out);

void probe_int_uninstall(void);

#ifdef __cplusplus
}
#endif
