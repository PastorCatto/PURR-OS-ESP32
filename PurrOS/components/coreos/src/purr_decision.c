#include "purr_decision.h"

#include "purr_abi.h"

purr_decision_t purr_decide(const purr_decision_in_t *in)
{
    int failure;

    if (in->boot_state == PURR_BOOT_FORCED_RECOVERY) {
        return PURR_DECIDE_CONTINUE;
    }

    if (in->boot_state == PURR_BOOT_FAILED_WARNED) {
        failure = 1;
    } else {
        failure = (in->own != PURR_OWN_PASS);
    }
    if (!failure) {
        return PURR_DECIDE_CONTINUE;
    }

    if (in->secure_mode == PURR_SECURE_OFF) {
        return PURR_DECIDE_CONTINUE;
    }
    if (in->secure_mode == PURR_SECURE_WARN) {
        return PURR_DECIDE_CONTINUE_WARN;
    }

    /* Enforce, and any unknown mode: fail closed. */
    if (in->is_recovery_image) {
        return PURR_DECIDE_PROMPT;
    }
    if (in->recovery_already_tried) {
        return PURR_DECIDE_DEGRADED;
    }
    return PURR_DECIDE_RECOVERY;
}

const char *purr_decision_name(purr_decision_t d)
{
    switch (d) {
    case PURR_DECIDE_CONTINUE:      return "continue";
    case PURR_DECIDE_CONTINUE_WARN: return "continue-with-warning";
    case PURR_DECIDE_RECOVERY:      return "recovery";
    case PURR_DECIDE_PROMPT:        return "prompt";
    case PURR_DECIDE_DEGRADED:      return "degraded";
    }
    return "unknown";
}
