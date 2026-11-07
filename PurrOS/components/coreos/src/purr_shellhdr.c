#include "purr_shellhdr.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);               /* byte-safe: the code buffer is not guaranteed to be aligned to the word at an arbitrary offset */
    return v;
}

purr_shellhdr_result_t purr_shellhdr_parse(const uint8_t *code, uint32_t code_size, uint32_t abi_version, uint32_t kernel_export_count, purr_shellhdr_t *out)
{
    if (code_size < 24) {
        return PURR_SHELLHDR_TOO_SHORT;
    }
    if (rd32(code) != PURR_SHELLHDR_MAGIC) {
        return PURR_SHELLHDR_BAD_MAGIC;
    }
    uint32_t abi = rd32(code + 4), exports = rd32(code + 8), bss = rd32(code + 12), imports = rd32(code + 16);
    if (abi != abi_version) {
        return PURR_SHELLHDR_BAD_ABI;
    }
    if (exports > kernel_export_count) {
        return PURR_SHELLHDR_NEEDS_NEWER;
    }
    if (imports > exports) {
        return PURR_SHELLHDR_DAMAGED;
    }
    uint64_t size = 24u + 8u * (uint64_t)exports;
    if (size > code_size) {
        return PURR_SHELLHDR_TOO_SHORT;
    }
    out->abi_version = abi;
    out->export_count = exports;
    out->bss_size = bss;
    out->import_count = imports;
    out->imports_offset = 24;
    out->header_size = (uint32_t)size;
    return PURR_SHELLHDR_OK;
}

purr_shellhdr_result_t purr_shellhdr_patch(uint8_t *code, uint32_t code_size, const purr_shellhdr_t *h, const void *const *exports, uint32_t export_count)
{
    for (uint32_t i = 0; i < h->import_count; i++) {
        uint32_t off = rd32(code + h->imports_offset + 8u * i), idx = rd32(code + h->imports_offset + 8u * i + 4);
        if ((off & 3u) != 0 || (uint64_t)off + 4 > code_size || off < h->header_size || idx >= h->export_count || idx >= export_count) {
            return PURR_SHELLHDR_BAD_IMPORT;
        }
    }
    for (uint32_t i = 0; i < h->import_count; i++) {
        uint32_t off = rd32(code + h->imports_offset + 8u * i), idx = rd32(code + h->imports_offset + 8u * i + 4);
        uint32_t addr = (uint32_t)(uintptr_t)exports[idx];
        memcpy(code + off, &addr, 4);
    }
    return PURR_SHELLHDR_OK;
}

const char *purr_shellhdr_result_name(purr_shellhdr_result_t r)
{
    switch (r) {
    case PURR_SHELLHDR_OK: return "ok";
    case PURR_SHELLHDR_TOO_SHORT: return "the shell is shorter than its header";
    case PURR_SHELLHDR_BAD_MAGIC: return "not a shell";
    case PURR_SHELLHDR_BAD_ABI: return "built for another shell ABI";
    case PURR_SHELLHDR_NEEDS_NEWER: return "needs a newer kernel";
    case PURR_SHELLHDR_DAMAGED: return "damaged shell header";
    case PURR_SHELLHDR_BAD_IMPORT: return "bad import site";
    }
    return "?";
}
