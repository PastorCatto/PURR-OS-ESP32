/*
 * purr_resetnote.h - the data-reset notice: why the data on this device was wiped or replaced (plan7.md, owner request 2026-11-06).
 *
 * Whatever wipes the data partition (or creates the default account because no account list was found) first records WHY in a small record that is outside the
 * data partition (purr_netrec.h) and sets PURR_CFGF_DATA_RESET in purrcfg. The OS shows the reason at sign-in, as a notification and in Settings > About until the
 * owner dismisses it. This is the wording, kept in plain C so a host test covers it.
 */
#ifndef PURR_RESETNOTE_H
#define PURR_RESETNOTE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PURR_RESET_NONE = 0,
    PURR_RESET_AUTO_FORMAT = 1,          /* the recovery system formatted storage that would not mount (it said "damaged") */
    PURR_RESET_USER_ERASE = 2,           /* "erase everything" was chosen when reinstalling */
    PURR_RESET_WEAKEN_WIPE = 3,          /* the wipe that comes with turning developer mode on */
    PURR_RESET_DEFAULT_OVER_HOMES = 4    /* no account list was found although user folders exist: the default account was created */
} purr_reset_why_t;

/*
 * Fills `brief` (one line, for the sign-in screen) and `full` (a few sentences, for About and the notification) for a notice. `detail` is the file system's
 * error code (0 if none), `had_data` says the storage held a file system. Both are always terminated, and truncated cleanly if the buffers are small.
 * Returns 1 for a known reason, 0 otherwise (a generic text is written then).
 */
int purr_resetnote_text(int why, int detail, int had_data, char *brief, size_t bcap, char *full, size_t fcap);

#ifdef __cplusplus
}
#endif

#endif /* PURR_RESETNOTE_H */
