#ifndef PURR_BOOTCFG_H
#define PURR_BOOTCFG_H

#include <stdbool.h>

#include "purr_cfg.h"
#include "purr_trust.h"

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

/*
 * The effective security state (purr_trust.h): evaluate cfg's trust block once, against the
 * compiled-in default keys, and keep the result. publish() is called by purr_boot.c right after
 * the first load; view() is the cfg with the effective values in place (decisions only, never
 * stored) and trust() the evaluation. The signature check costs about 250 ms at the bootloader's
 * 80 MHz, and only when a trust block is present, so it is done exactly once per boot and shared
 * by the boot package loader and the slot checks. Both fall back to loading purrcfg themselves if
 * publish() was not called.
 */
void purr_bootcfg_publish(const purr_cfg_t *cfg);
const purr_cfg_t *purr_bootcfg_view(void);
const purr_trust_t *purr_bootcfg_trust(void);

/*
 * Write cfg as the newest copy (bumping seq, sealing the CRC, reading back to check), the
 * same way purr_bootcfg_take_flag already does internally -- exposed here so purr_boot.c can
 * bump boot_fail_count and set flags that flow bootloader -> OS (PURR_CFGF_AUTO_REINSTALL),
 * the opposite direction from the one-shot request flags purr_bootcfg_take_flag clears.
 * Returns false if there is no purrcfg partition or the write failed; never blocks booting.
 */
bool purr_bootcfg_store(purr_cfg_t *cfg);

#endif
