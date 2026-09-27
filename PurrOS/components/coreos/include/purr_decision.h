/*
 * purr_decision.h - what CoreOS does with the bootloader's result and its own
 * self-check. A pure function, the decision table in
 * PurrOS/components/coreos/SPEC.md section 5.
 */
#ifndef PURR_DECISION_H
#define PURR_DECISION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PURR_OWN_PASS = 0,      /* own image verified */
    PURR_OWN_FAIL,          /* own image failed verification */
    PURR_OWN_UNSIGNED       /* no container header (a dev build) */
} purr_own_check_t;

typedef enum {
    PURR_DECIDE_CONTINUE = 0,    /* carry on */
    PURR_DECIDE_CONTINUE_WARN,   /* carry on, with a boot warning */
    PURR_DECIDE_RECOVERY,        /* record KittenOS as the target and restart */
    PURR_DECIDE_PROMPT,          /* KittenOS itself failed: serial prompt, no restart */
    PURR_DECIDE_DEGRADED         /* recovery was already tried: stay in a degraded shell */
} purr_decision_t;

typedef struct {
    uint8_t          boot_state;              /* PURR_BOOT_* from the handoff */
    purr_own_check_t own;
    uint8_t          secure_mode;             /* PURR_SECURE_* */
    uint8_t          is_recovery_image;       /* nonzero when running as KittenOS */
    uint8_t          recovery_already_tried;  /* nonzero after one automatic restart */
} purr_decision_in_t;

/*
 * Rules:
 *  - forced_recovery always continues (the user asked to be here).
 *  - failed_warned counts as a failure whatever the own check says.
 *  - verified, config_default and none count as a failure only if the own
 *    check did not pass.
 *  - A failure: off continues, warn continues with a warning, enforce goes to
 *    recovery. An unknown secure_mode is treated as enforce (fail closed).
 *  - KittenOS never falls back to itself (PROMPT), and only one automatic
 *    recovery restart is allowed (DEGRADED afterwards).
 */
purr_decision_t purr_decide(const purr_decision_in_t *in);

const char *purr_decision_name(purr_decision_t d);

#ifdef __cplusplus
}
#endif

#endif /* PURR_DECISION_H */
