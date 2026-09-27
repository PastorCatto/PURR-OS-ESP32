#ifndef PURR_BOOTCFG_H
#define PURR_BOOTCFG_H

#include <stdbool.h>

/*
 * Read purrcfg. If the one-shot FORCE_RECOVERY flag is set, clear it (writing purrcfg back)
 * and return true: the caller should start KittenOS this once. Returns false if there is
 * no purrcfg, it holds no request, or anything went wrong (then it never blocks booting).
 */
bool purr_bootcfg_take_recovery(void);

#endif
