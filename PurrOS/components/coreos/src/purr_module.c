#include "purr_module.h"

#include <string.h>

#include "purr_relocate.h"

#define FIXED_HEADER_SIZE 8u  /* entry_offset (4) + data_reloc_count (4) */

purr_module_layout_result_t purr_module_parse_layout(const uint8_t *payload, uint32_t payload_size,
                                                      purr_module_layout_t *out)
{
    if (payload_size < FIXED_HEADER_SIZE) {
        return PURR_MOD_LAYOUT_TOO_SHORT;
    }

    uint32_t entry_offset, data_reloc_count;
    memcpy(&entry_offset, payload + 0, 4);
    memcpy(&data_reloc_count, payload + 4, 4);
    if (data_reloc_count > PURR_RELOC_MAX) {
        return PURR_MOD_LAYOUT_TOO_MANY;
    }

    uint32_t data_reloc_table_offset = FIXED_HEADER_SIZE;
    uint64_t after_data64 = (uint64_t)data_reloc_table_offset + (uint64_t)data_reloc_count * 4u;
    /* Need 4 more bytes here for code_reloc_count itself. */
    if (after_data64 + 4u > payload_size) {
        return PURR_MOD_LAYOUT_BAD_SIZE;
    }
    uint32_t code_reloc_count_offset = (uint32_t)after_data64;

    uint32_t code_reloc_count;
    memcpy(&code_reloc_count, payload + code_reloc_count_offset, 4);
    if (code_reloc_count > PURR_RELOC_MAX) {
        return PURR_MOD_LAYOUT_TOO_MANY;
    }

    uint32_t code_reloc_table_offset = code_reloc_count_offset + 4u;
    uint64_t code_offset64 = (uint64_t)code_reloc_table_offset + (uint64_t)code_reloc_count * 4u;
    if (code_offset64 > payload_size) {
        return PURR_MOD_LAYOUT_BAD_SIZE;
    }
    uint32_t code_offset = (uint32_t)code_offset64;
    uint32_t code_size = payload_size - code_offset;

    if (entry_offset >= code_size) {
        return PURR_MOD_LAYOUT_BAD_ENTRY;
    }

    out->entry_offset = entry_offset;
    out->data_reloc_count = data_reloc_count;
    out->data_reloc_table_offset = data_reloc_table_offset;
    out->code_reloc_count = code_reloc_count;
    out->code_reloc_table_offset = code_reloc_table_offset;
    out->code_offset = code_offset;
    out->code_size = code_size;
    return PURR_MOD_LAYOUT_OK;
}

void purr_module_read_data_relocs(const uint8_t *payload, const purr_module_layout_t *layout,
                                  uint32_t *out_offsets)
{
    for (uint32_t i = 0; i < layout->data_reloc_count; i++) {
        memcpy(&out_offsets[i], payload + layout->data_reloc_table_offset + i * 4u, 4);
    }
}

void purr_module_read_code_relocs(const uint8_t *payload, const purr_module_layout_t *layout,
                                  uint32_t *out_offsets)
{
    for (uint32_t i = 0; i < layout->code_reloc_count; i++) {
        memcpy(&out_offsets[i], payload + layout->code_reloc_table_offset + i * 4u, 4);
    }
}

const char *purr_module_layout_result_name(purr_module_layout_result_t r)
{
    switch (r) {
    case PURR_MOD_LAYOUT_OK:        return "ok";
    case PURR_MOD_LAYOUT_TOO_SHORT: return "too-short";
    case PURR_MOD_LAYOUT_TOO_MANY:  return "too-many-relocs";
    case PURR_MOD_LAYOUT_BAD_SIZE:  return "bad-size";
    case PURR_MOD_LAYOUT_BAD_ENTRY: return "bad-entry";
    default:                        return "unknown";
    }
}
