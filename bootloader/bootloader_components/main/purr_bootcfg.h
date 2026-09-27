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
 * Read purrcfg. If the given one-shot flag (PURR_CFGF_FORCE_RECOVERY or
 * PURR_CFGF_FORCE_LOADER) is set, clear it (writing purrcfg back) and return true: the
 * caller should start that target this once. Returns false if there is no purrcfg, it
 * holds no such request, or anything went wrong (then it never blocks booting).
 */
bool purr_bootcfg_take_flag(uint32_t flag);

#endif
