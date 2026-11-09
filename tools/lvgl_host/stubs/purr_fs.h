#pragma once
#include <stdint.h>
typedef struct purr_fs purr_fs_t;
typedef int (*purr_fs_read_fn)(void *ctx, const void *data, uint32_t n);
static inline int purr_fs_mounted(purr_fs_t *fs) { (void)fs; return 0; }       /* no filesystem in the preview */
static inline int purr_fs_read(purr_fs_t *fs, const char *p, purr_fs_read_fn cb, void *ctx) { (void)fs; (void)p; (void)cb; (void)ctx; return -1; }
static inline int purr_fs_write(purr_fs_t *fs, const char *p, const void *d, uint32_t n) { (void)fs; (void)p; (void)d; (void)n; return -1; }
static inline int purr_fs_mkdir(purr_fs_t *fs, const char *p) { (void)fs; (void)p; return -1; }
typedef int (*purr_fs_list_fn)(void *ctx, const char *name, int is_dir, uint32_t size);
