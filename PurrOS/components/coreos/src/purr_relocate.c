#include "purr_relocate.h"

#include <string.h>

purr_reloc_result_t purr_relocate(uint8_t *buf, uint32_t buf_len,
                                  const uint32_t *offsets, uint32_t count,
                                  uint32_t base)
{
    if (count > PURR_RELOC_MAX) {
        return PURR_RELOC_TOO_MANY;
    }

    /* Validate every entry before writing anything, so a bad relocation later in the list
     * never leaves buf half-modified. */
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = offsets[i];
        if ((off & 3u) != 0u) {
            return PURR_RELOC_MISALIGNED;
        }
        if (off > buf_len || buf_len - off < 4u) {
            return PURR_RELOC_BAD_OFFSET;
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t word;
        memcpy(&word, buf + offsets[i], 4);
        word += base;
        memcpy(buf + offsets[i], &word, 4);
    }

    return PURR_RELOC_OK;
}

const char *purr_reloc_result_name(purr_reloc_result_t r)
{
    switch (r) {
    case PURR_RELOC_OK:         return "ok";
    case PURR_RELOC_TOO_MANY:   return "too-many";
    case PURR_RELOC_MISALIGNED: return "misaligned";
    case PURR_RELOC_BAD_OFFSET: return "bad-offset";
    default:                    return "unknown";
    }
}
