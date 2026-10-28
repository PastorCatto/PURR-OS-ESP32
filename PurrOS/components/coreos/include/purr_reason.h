/*
 * purr_reason.h - why the device is in recovery, in words (plan10-era OOBE design, plan1.md).
 *
 * The bootloader flags the reasons it knows (PURR_RR_*, purr_abi.h) in the handoff; the recovery loader's first page lists
 * them. This is the wording and the mapping from a verification result to a reason, kept in plain C so a host test covers it.
 */
#ifndef PURR_REASON_H
#define PURR_REASON_H

#include <stdint.h>

#include "purr_abi.h"                   /* PURR_RR_* */

#ifdef __cplusplus
extern "C" {
#endif

/* The one-line explanation for bit `bit` (0..PURR_RR_COUNT-1), or NULL. */
const char *purr_reason_text(int bit);

/* Which reason a failed slot verification (a PURR_V_* result, purr_verify.h) stands for; 0 for PURR_V_OK. */
uint16_t purr_reason_from_verify(int purr_v_result);

/* Fills `out` with the explanations of every set flag, in a fixed order; returns how many (at most max). */
int purr_reason_lines(uint16_t reasons, const char **out, int max);

/* 1 if this looks like the first run of a device with nothing installed: blank, and no sign that something was damaged or
 * tampered with (a half-erased device is a failure, not a first run). */
int purr_reason_is_first_run(uint16_t reasons);

/* 1 if the flags mean the system itself cannot be trusted or is missing (so the loader restores everything, kernel included). */
int purr_reason_needs_full_restore(uint16_t reasons);

#ifdef __cplusplus
}
#endif

#endif /* PURR_REASON_H */
