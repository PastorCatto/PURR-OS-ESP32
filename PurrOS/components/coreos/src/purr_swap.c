#include "purr_swap.h"

#include <stddef.h>

purr_swap_action_t purr_swap_decide(const purr_update_t *update, uint8_t max_attempts)
{
    switch (update->state) {
    case PURR_UPD_REQUESTED:
        return PURR_SWAP_DO_SWAP;
    case PURR_UPD_MOVING:
        return PURR_SWAP_RESUME_MOVE;
    case PURR_UPD_UNCONFIRMED:
        return (update->attempts >= max_attempts) ? PURR_SWAP_ROLLBACK : PURR_SWAP_RETRY;
    case PURR_UPD_NONE:
    case PURR_UPD_STAGED:
    case PURR_UPD_CONFIRMED:
    case PURR_UPD_FAILED:
    default:
        return PURR_SWAP_NOTHING;
    }
}

const char *purr_swap_filename(uint8_t component)
{
    switch (component) {
    case PURR_COMP_COREOS:     return "coreos.kitt";
    case PURR_COMP_APPMANAGER: return "appmanager.kitt";
    case PURR_COMP_RUNTIME:    return "runtime.kitt";
    case PURR_COMP_DEVBUNDLE:  return "devbundle.kitt";
    default:                   return NULL;
    }
}
