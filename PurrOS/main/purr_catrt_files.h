/* purr_catrt_files.h - the `files` catcall (purr_catcalls.h): a file manager's view of the file system, gated by two permissions. */
#ifndef PURR_CATRT_FILES_H
#define PURR_CATRT_FILES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void purr_catrt_files_init(void);

/* The whole of a file into PSRAM with the calling app's permission (the ui catcall's pictures): 0 with *out (free it) and *len, or a PURR_FILES_* code. */
int purr_catrt_files_load(const char *path, uint8_t **out, size_t *len, size_t max);

#ifdef __cplusplus
}
#endif

#endif
