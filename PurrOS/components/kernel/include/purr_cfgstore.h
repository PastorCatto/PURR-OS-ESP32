/* purr_cfgstore.h - purrcfg on the device: the "purrcfg" flash partition as a purr_flash_t. */
#ifndef PURR_CFGSTORE_H
#define PURR_CFGSTORE_H

#include "purr_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fill `fl` for the purrcfg partition. Returns 0, or -1 if there is no such partition. */
int purr_cfgstore_open(purr_flash_t *fl);

#ifdef __cplusplus
}
#endif

#endif
