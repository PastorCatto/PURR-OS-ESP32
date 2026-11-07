/*
 * purr_shellhdr.h - the header at the start of a loadable shell's code, and the step that writes the real addresses into it (plan7.md, purr_shell_abi.h).
 *
 * Plain C with no ESP-IDF dependency, tested on the PC like the module layout and the relocator. The shell is an ordinary module (purr_module.h), so the kernel's
 * one module loader verifies, relocates and maps it; this is what the shell loader does on top, with the loaded copy:
 *
 *   1. purr_shellhdr_parse(): reads the header (purrstrap's lib/shellabi.py writes it: magic "PSHL", the ABI version, how many exports the shell was linked
 *      against, its bss size, and the list of import sites) and refuses a shell built for another ABI or for a longer export list than this kernel has;
 *   2. purr_shellhdr_patch(): writes the kernel's real address of export `index` over the word at every import site (offset into the code).
 */
#ifndef PURR_SHELLHDR_H
#define PURR_SHELLHDR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_SHELLHDR_MAGIC 0x4C485350u                 /* "PSHL" */

typedef enum {
    PURR_SHELLHDR_OK = 0,
    PURR_SHELLHDR_TOO_SHORT,        /* the code is shorter than its header says */
    PURR_SHELLHDR_BAD_MAGIC,        /* not a shell */
    PURR_SHELLHDR_BAD_ABI,          /* built for another ABI version */
    PURR_SHELLHDR_NEEDS_NEWER,      /* linked against more exports than this kernel has: it needs a newer kernel */
    PURR_SHELLHDR_DAMAGED,          /* more import sites than exports */
    PURR_SHELLHDR_BAD_IMPORT,       /* an import site is misaligned, outside the code, or names an export that does not exist */
} purr_shellhdr_result_t;

typedef struct {
    uint32_t abi_version;
    uint32_t export_count;          /* what the shell was linked against */
    uint32_t bss_size;              /* zeroed memory the loader maps after the code */
    uint32_t import_count;
    uint32_t imports_offset;        /* into the code: the (offset, index) pairs */
    uint32_t header_size;
} purr_shellhdr_t;

purr_shellhdr_result_t purr_shellhdr_parse(const uint8_t *code, uint32_t code_size, uint32_t abi_version, uint32_t kernel_export_count, purr_shellhdr_t *out);

/* Writes exports[index] (as a 32-bit address) over the word at each import site. Every site is checked first; on any bad one nothing is written. */
purr_shellhdr_result_t purr_shellhdr_patch(uint8_t *code, uint32_t code_size, const purr_shellhdr_t *h, const void *const *exports, uint32_t export_count);

const char *purr_shellhdr_result_name(purr_shellhdr_result_t r);

#ifdef __cplusplus
}
#endif

#endif /* PURR_SHELLHDR_H */
