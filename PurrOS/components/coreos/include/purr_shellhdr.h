/*
 * purr_shellhdr.h - the header at the start of a loadable shell's code, and what the loader does with it (plan7.md, purr_shell_abi.h).
 *
 * Plain C with no ESP-IDF dependency, tested on the PC like the module layout and the relocator. The shell is an ordinary module (purr_module.h), so the kernel's
 * one module loader verifies, relocates and maps it; on top of that the shell loader uses the header (purrstrap's lib/shellabi.py writes it):
 *
 *   1. purr_shellhdr_parse(): structure and bounds; refuses a shell built for another ABI number;
 *   2. purr_shellhdr_check(): is the shell compatible with THIS kernel? Every OS name it uses must exist in the kernel's export list (a shell finds what it uses by NAME,
 *      so a kernel that has added or reordered exports still runs it), and every layout fact it carries (sizes of structures, constants: purr_shell_layout.def) must equal the
 *      kernel's value for the same id. A kernel may know more ids than the shell carries; a shell with an id the kernel does not know is refused. A mismatch is not loaded:
 *      reading a structure at the wrong offset corrupts memory silently. This does not look at signatures: trust is the loader's other half and does not depend on the build;
 *   3. purr_shellhdr_patch(): writes the kernel's real address of each name the shell uses over the placeholder word at its import site.
 *
 * Layout in the code (word = 4 bytes, little endian), purrstrap reserves the capacities and fills what is used:
 *   word 0 magic "PSHL", 1 ABI version, 2 bss size, 3 import count, 4 layout count, 5 names size, 6 layout capacity, 7 import capacity, 8 names capacity,
 *   bytes 36..51 the kernel version the shell was built against (text), then 3 reserved words (header bytes 0..63);
 *   then layout pairs (id, value) x layout capacity, import pairs (offset of the word in the code, offset of the name in the names block) x import capacity,
 *   then the names block (NUL-terminated strings), names capacity bytes rounded up to a word.
 */
#ifndef PURR_SHELLHDR_H
#define PURR_SHELLHDR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_SHELLHDR_MAGIC 0x4C485350u                 /* "PSHL" */
#define PURR_SHELLHDR_FIXED 64u

typedef enum {
    PURR_SHELLHDR_OK = 0,
    PURR_SHELLHDR_TOO_SHORT,        /* the code is shorter than its header says */
    PURR_SHELLHDR_BAD_MAGIC,        /* not a shell */
    PURR_SHELLHDR_BAD_ABI,          /* built for another ABI version */
    PURR_SHELLHDR_DAMAGED,          /* counts above capacities, a name that is not terminated, ... */
    PURR_SHELLHDR_BAD_IMPORT,       /* an import site is misaligned or outside the code */
    PURR_SHELLHDR_NEEDS_NEWER,      /* the shell uses an OS name this kernel does not have */
    PURR_SHELLHDR_LAYOUT_MISMATCH,  /* a layout fact differs from the kernel's, or the shell carries one the kernel does not know */
} purr_shellhdr_result_t;

typedef struct {
    uint32_t id;
    uint32_t value;
} purr_shellhdr_pair_t;             /* the same shape as purr_shell_layout_t */

typedef struct {
    uint32_t abi_version;
    uint32_t bss_size;
    uint32_t import_count;
    uint32_t layout_count;
    uint32_t names_size;
    uint32_t layout_offset;         /* into the code */
    uint32_t imports_offset;
    uint32_t names_offset;
    uint32_t header_size;
    char built_for[17];             /* the kernel version the shell was built against, for display */
} purr_shellhdr_t;

purr_shellhdr_result_t purr_shellhdr_parse(const uint8_t *code, uint32_t code_size, uint32_t abi_version, purr_shellhdr_t *out);

/* On NEEDS_NEWER, *problem is the first missing name (it points into the code); on LAYOUT_MISMATCH it is NULL. */
purr_shellhdr_result_t purr_shellhdr_check(const uint8_t *code, uint32_t code_size, const purr_shellhdr_t *h, const char *const *kernel_names, uint32_t kernel_name_count,
                                           const purr_shellhdr_pair_t *kernel_layout, uint32_t kernel_layout_count, const char **problem);

/* Writes exports[i] (as a 32-bit address) over every import site, where i is the position of the site's name in kernel_names. Checks all sites first; on any problem nothing is written. */
purr_shellhdr_result_t purr_shellhdr_patch(uint8_t *code, uint32_t code_size, const purr_shellhdr_t *h, const char *const *kernel_names, uint32_t kernel_name_count,
                                           const void *const *exports);

const char *purr_shellhdr_result_name(purr_shellhdr_result_t r);

#ifdef __cplusplus
}
#endif

#endif /* PURR_SHELLHDR_H */
