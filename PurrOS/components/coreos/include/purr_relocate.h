/*
 * purr_relocate.h - applies a module's relocation list to its loaded payload
 * (Modules/SPEC.md section 4).
 *
 * Plain C with no ESP-IDF dependency, so it is tested on the PC like everything else pure in
 * this codebase. One relocation kind covers every case (a string literal's address, a global's
 * address, a function pointer in an exported call table): the module was linked at base
 * address 0, so a relocatable word's stored value already equals its target offset from the
 * start of the module -- applying it at load time is just adding the real load address.
 */
#ifndef PURR_RELOCATE_H
#define PURR_RELOCATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A hard cap on how many relocations one module may carry, so a malformed or hostile
 * manifest can't force an unbounded amount of work before individual entries are even
 * checked. purrstrap-built modules will be nowhere near this. */
#define PURR_RELOC_MAX 4096

typedef enum {
    PURR_RELOC_OK = 0,
    PURR_RELOC_TOO_MANY,     /* count over PURR_RELOC_MAX */
    PURR_RELOC_MISALIGNED,   /* an offset is not a multiple of 4 */
    PURR_RELOC_BAD_OFFSET,   /* an offset (or offset+4) falls outside the buffer */
} purr_reloc_result_t;

/*
 * Applies `count` relocations to `buf` (of `buf_len` bytes): for each offset in `offsets`,
 * adds `base` to the 32-bit little-endian word at that offset.
 *
 * Every offset is checked against buf_len and 4-byte alignment before anything is written --
 * on any bad offset the whole call fails and buf is left completely untouched, so a caller
 * never has to reason about a partially-relocated buffer.
 */
purr_reloc_result_t purr_relocate(uint8_t *buf, uint32_t buf_len,
                                  const uint32_t *offsets, uint32_t count,
                                  uint32_t base);

const char *purr_reloc_result_name(purr_reloc_result_t r);

#ifdef __cplusplus
}
#endif

#endif /* PURR_RELOCATE_H */
