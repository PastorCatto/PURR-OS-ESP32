/*
 * purr_module.h - reads a module's payload prefix (Modules/SPEC.md section 5).
 *
 * Plain C with no ESP-IDF dependency, tested on the PC like everything else pure in this
 * codebase. Every field is read byte-safe (memcpy, no struct overlay) since a payload buffer
 * read off flash or LittleFS is never guaranteed to be 4-byte aligned.
 *
 * Two relocation lists, not one (section 4's real lesson): a word whose value will be
 * EXECUTED (the entry point, a function pointer stored in an exported table) must resolve to
 * wherever the module is mapped EXECUTABLE; a word whose value is the address of a global
 * that gets WRITTEN to (or just read) must resolve to the plain WRITABLE copy, since the
 * executable mapping is typically exec+read only, not writable. One combined list can't tell
 * these apart -- purrstrap does, using the compiler's own FUNC/OBJECT symbol types, not which
 * section the linker happened to place something in.
 *
 * Payload layout:
 *   [ entry_offset      : u32 ]                    -- into the code blob below
 *   [ data_reloc_count  : u32 ]
 *   [ data_reloc_offsets: u32 * data_reloc_count ]  -- relocate against the writable base
 *   [ code_reloc_count  : u32 ]
 *   [ code_reloc_offsets: u32 * code_reloc_count ]  -- relocate against the executable base
 *   [ code blob ]                                   -- the base-0 linked flat binary
 */
#ifndef PURR_MODULE_H
#define PURR_MODULE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PURR_MOD_LAYOUT_OK = 0,
    PURR_MOD_LAYOUT_TOO_SHORT,   /* payload too small to even hold the fixed-size fields */
    PURR_MOD_LAYOUT_TOO_MANY,    /* a reloc count is over PURR_RELOC_MAX */
    PURR_MOD_LAYOUT_BAD_SIZE,    /* the prefix (headers + both offset tables) doesn't fit */
    PURR_MOD_LAYOUT_BAD_ENTRY,   /* entry_offset falls outside the code blob */
} purr_module_layout_result_t;

typedef struct {
    uint32_t entry_offset;           /* into the code blob */
    uint32_t data_reloc_count;
    uint32_t data_reloc_table_offset;/* into the payload */
    uint32_t code_reloc_count;
    uint32_t code_reloc_table_offset;/* into the payload */
    uint32_t code_offset;            /* into the payload, where the code blob starts */
    uint32_t code_size;              /* payload_size - code_offset */
} purr_module_layout_t;

/* Reads and validates the prefix. Never trusts a field until it's checked against
 * payload_size, so a hostile or truncated payload is rejected before anything past that is
 * touched. */
purr_module_layout_result_t purr_module_parse_layout(const uint8_t *payload, uint32_t payload_size,
                                                      purr_module_layout_t *out);

/* Copies layout->data_reloc_count offsets out of payload into out_offsets (byte-safe), which
 * must have room for that many uint32_t. Call only after purr_module_parse_layout() OK. */
void purr_module_read_data_relocs(const uint8_t *payload, const purr_module_layout_t *layout,
                                  uint32_t *out_offsets);

/* Same, for layout->code_reloc_count code relocations. */
void purr_module_read_code_relocs(const uint8_t *payload, const purr_module_layout_t *layout,
                                  uint32_t *out_offsets);

const char *purr_module_layout_result_name(purr_module_layout_result_t r);

#ifdef __cplusplus
}
#endif

#endif /* PURR_MODULE_H */
