#ifndef KERNEL_LOADER_H
#define KERNEL_LOADER_H

#include <stdint.h>

#include "purr_fs.h"

typedef enum {
    KERNEL_LOAD_OK = 0,
    KERNEL_LOAD_TRANSIENT,
    KERNEL_LOAD_UNTRUSTED,
} kernel_load_result_t;

/* Reads, verifies, relocates and maps a PURR_IMG_MODULE file's payload as one executable
 * PSRAM block -- the same mechanism Modules/SPEC.md and commands.c's load_relocatable_file()
 * already prove, ported here since this kernel doesn't link PurrOS/main at all. On
 * KERNEL_LOAD_OK, the databuf and exec_ptr outputs are two aliases of the same mapped block,
 * and entry_offset is the entry point's byte offset into it; the caller owns cleanup
 * (esp_mmu_unmap + free) once it has these three values. */
kernel_load_result_t kernel_load_relocatable_file(purr_fs_t *fs, const char *path,
                                                   uint32_t read_cap, uint32_t max_pages,
                                                   uint8_t **out_databuf, void **out_exec_ptr,
                                                   uint32_t *out_entry_offset);

#endif
