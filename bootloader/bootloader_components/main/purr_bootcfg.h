#ifndef PURR_BOOTCFG_H
#define PURR_BOOTCFG_H

#include <stdbool.h>

#include "purr_cfg.h"

/*
 * Read-only. Fills `out` with the newest valid copy, or defaults (secure_mode = warn) if
 * there is no partition or nothing valid was written yet. Returns whether a stored copy
 * was read (false means `out` holds defaults).
 */
bool purr_bootcfg_load(purr_cfg_t *out);

/*
 * Read purrcfg. If the one-shot FORCE_RECOVERY flag is set, clear it (writing purrcfg back)
 * and return true: the caller should start KittenOS this once. Returns false if there is
 * no purrcfg, it holds no request, or anything went wrong (then it never blocks booting).
 */
bool purr_bootcfg_take_recovery(void);

#endif
