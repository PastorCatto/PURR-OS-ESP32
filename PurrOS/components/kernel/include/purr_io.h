/*
 * purr_io.h - flash access for tasks whose stack is not in internal RAM.
 *
 * A flash read, write or erase switches the cache off, and a task with its stack in PSRAM cannot run through that (IDF aborts with "stack in external RAM during a
 * flash operation"). The app runtime gives every app a PSRAM stack (purr_catrt.c) so an open app costs about 1 KB of fast RAM instead of 10 KB; the apps' own
 * file and settings calls still end in these three functions, so they hand the raw flash access to one small task with an internal stack and wait for it. A caller
 * whose stack is internal (every system task) goes straight to the partition API, as before.
 */
#ifndef PURR_IO_H
#define PURR_IO_H

#include <stddef.h>

#include "esp_err.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the flash task. Until it runs, every call goes straight through. */
void purr_io_start(void);

esp_err_t purr_io_partition_read(const esp_partition_t *p, size_t offset, void *dst, size_t size);
esp_err_t purr_io_partition_write(const esp_partition_t *p, size_t offset, const void *src, size_t size);
esp_err_t purr_io_partition_erase_range(const esp_partition_t *p, size_t offset, size_t size);

#ifdef __cplusplus
}
#endif

#endif
