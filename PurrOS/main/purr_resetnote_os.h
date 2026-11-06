/*
 * purr_resetnote_os.h - the data-reset notice on the device (purr_resetnote.h has the wording, purr_netrec.h the record, PURR_CFGF_DATA_RESET the flag).
 */
#ifndef PURR_RESETNOTE_OS_H
#define PURR_RESETNOTE_OS_H

#include <stddef.h>

/* Records why data was wiped or replaced: the netrec record, then the DATA_RESET flag in purrcfg. Call it BEFORE the wipe where there is one. why: PURR_RESET_*. */
void purr_resetnote_record(int why, int detail, int had_data);

/* 1 if a notice is pending (brief and full are filled, either may be NULL), 0 if not. */
int purr_resetnote_pending(char *brief, size_t bcap, char *full, size_t fcap);

/* The owner has read it: clears the record and the flag. */
void purr_resetnote_dismiss(void);

#endif
