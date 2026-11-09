/* purr_reason.c - see purr_reason.h. */
#include "purr_reason.h"

#include <stddef.h>

#include "purr_abi.h"
#include "purr_verify.h"

static const char *const k_text[PURR_RR_COUNT] = {
    "The system isn't signed by a key this device trusts.",           /* UNSIGNED */
    "Something changed the system after it was signed. It might have been tampered with.",   /* ALTERED */
    "Some system files are damaged and can't be read.",                 /* DAMAGED */
    "The system is older than this device allows.",                     /* ROLLBACK */
    "The device couldn't start a few times in a row.",               /* REPEATED */
    "We couldn't find a working system.",                                      /* NO_SYSTEM */
    "Someone asked for a recovery.",                                         /* REQUESTED */
    "We couldn't read the boot settings.",                              /* CONFIG */
    "The system hit a problem and asked for a fresh install.",         /* SYSTEM */
    "Nothing is installed on this device yet.",                          /* BLANK */
};

const char *purr_reason_text(int bit)
{
    return bit >= 0 && bit < PURR_RR_COUNT ? k_text[bit] : NULL;
}

uint16_t purr_reason_from_verify(int r)
{
    switch (r) {
    case PURR_V_OK:
        return 0;
    case PURR_V_NO_KEY:
    case PURR_V_REVOKED:
    case PURR_V_ROLE_MISMATCH:
        return PURR_RR_UNSIGNED;
    case PURR_V_BAD_HASH:
    case PURR_V_BAD_SIGNATURE:
        return PURR_RR_ALTERED;
    case PURR_V_TOO_OLD_BOOTLOADER:
    case PURR_V_BELOW_FLOOR:
        return PURR_RR_ROLLBACK;
    default:                                    /* bad magic, version, layout, chip, type, or an I/O error */
        return PURR_RR_DAMAGED;
    }
}

int purr_reason_lines(uint16_t reasons, const char **out, int max)
{
    int n = 0;
    for (int bit = 0; bit < PURR_RR_COUNT && n < max; bit++) {
        if (reasons & (1u << bit)) {
            out[n++] = k_text[bit];
        }
    }
    return n;
}

int purr_reason_is_first_run(uint16_t reasons)
{
    return (reasons & PURR_RR_BLANK) != 0 &&
           (reasons & (PURR_RR_UNSIGNED | PURR_RR_ALTERED | PURR_RR_DAMAGED | PURR_RR_ROLLBACK | PURR_RR_REPEATED)) == 0;
}

int purr_reason_needs_full_restore(uint16_t reasons)
{
    return (reasons & (PURR_RR_UNSIGNED | PURR_RR_ALTERED | PURR_RR_DAMAGED | PURR_RR_ROLLBACK | PURR_RR_NO_SYSTEM |
                PURR_RR_BLANK)) != 0;
}
