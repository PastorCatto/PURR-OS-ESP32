#include "purr_shellhdr.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);               /* byte-safe: the code buffer is not guaranteed to be aligned to the word at an arbitrary offset */
    return v;
}

purr_shellhdr_result_t purr_shellhdr_parse(const uint8_t *code, uint32_t code_size, uint32_t abi_version, purr_shellhdr_t *out)
{
    if (code_size < PURR_SHELLHDR_FIXED) {
        return PURR_SHELLHDR_TOO_SHORT;
    }
    if (rd32(code) != PURR_SHELLHDR_MAGIC) {
        return PURR_SHELLHDR_BAD_MAGIC;
    }
    uint32_t abi = rd32(code + 4), bss = rd32(code + 8), imports = rd32(code + 12), layouts = rd32(code + 16), names = rd32(code + 20);
    uint32_t lcap = rd32(code + 24), icap = rd32(code + 28), ncap = rd32(code + 32);
    if (abi != abi_version) {
        return PURR_SHELLHDR_BAD_ABI;
    }
    if (imports > icap || layouts > lcap || names > ncap || lcap > 4096 || icap > 4096 || ncap > 65536) {
        return PURR_SHELLHDR_DAMAGED;
    }
    uint64_t size = PURR_SHELLHDR_FIXED + 8ull * lcap + 8ull * icap + (((uint64_t)ncap + 3) & ~3ull);
    if (size > code_size) {
        return PURR_SHELLHDR_TOO_SHORT;
    }
    out->abi_version = abi;
    out->bss_size = bss;
    out->import_count = imports;
    out->layout_count = layouts;
    out->names_size = names;
    out->layout_offset = PURR_SHELLHDR_FIXED;
    out->imports_offset = PURR_SHELLHDR_FIXED + 8 * lcap;
    out->names_offset = PURR_SHELLHDR_FIXED + 8 * lcap + 8 * icap;
    out->header_size = (uint32_t)size;
    out->pack_offset = rd32(code + 52);
    out->pack_size = rd32(code + 56);
    if (out->pack_size != 0 && ((out->pack_offset & 3u) != 0 || out->pack_offset < out->header_size || (uint64_t)out->pack_offset + out->pack_size > code_size)) {
        return PURR_SHELLHDR_DAMAGED;                           /* the pack must lie inside the code, after the header, word aligned */
    }
    memcpy(out->built_for, code + 36, 16);
    out->built_for[16] = '\0';
    return PURR_SHELLHDR_OK;
}

/* The name at `off` in the names block, or NULL if it is outside it or not terminated inside it. */
static const char *name_at(const uint8_t *code, const purr_shellhdr_t *h, uint32_t off)
{
    if (off >= h->names_size) {
        return NULL;
    }
    const char *s = (const char *)code + h->names_offset + off;
    if (memchr(s, '\0', h->names_size - off) == NULL) {
        return NULL;
    }
    return s;
}

static int find_name(const char *const *names, uint32_t count, const char *name)
{
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static purr_shellhdr_result_t check_imports(const uint8_t *code, uint32_t code_size, const purr_shellhdr_t *h, const char *const *kernel_names, uint32_t kernel_name_count,
                                            const char **problem)
{
    if (problem != NULL) {
        *problem = NULL;
    }
    for (uint32_t i = 0; i < h->import_count; i++) {
        uint32_t off = rd32(code + h->imports_offset + 8u * i), noff = rd32(code + h->imports_offset + 8u * i + 4);
        if ((off & 3u) != 0 || (uint64_t)off + 4 > code_size || off < h->header_size) {
            return PURR_SHELLHDR_BAD_IMPORT;
        }
        const char *name = name_at(code, h, noff);
        if (name == NULL) {
            return PURR_SHELLHDR_DAMAGED;
        }
        if (find_name(kernel_names, kernel_name_count, name) < 0) {
            if (problem != NULL) {
                *problem = name;
            }
            return PURR_SHELLHDR_NEEDS_NEWER;
        }
    }
    return PURR_SHELLHDR_OK;
}

purr_shellhdr_result_t purr_shellhdr_check(const uint8_t *code, uint32_t code_size, const purr_shellhdr_t *h, const char *const *kernel_names, uint32_t kernel_name_count,
                                           const purr_shellhdr_pair_t *kernel_layout, uint32_t kernel_layout_count, const char **problem)
{
    purr_shellhdr_result_t r = check_imports(code, code_size, h, kernel_names, kernel_name_count, problem);
    if (r != PURR_SHELLHDR_OK) {
        return r;
    }
    for (uint32_t i = 0; i < h->layout_count; i++) {
        uint32_t id = rd32(code + h->layout_offset + 8u * i), value = rd32(code + h->layout_offset + 8u * i + 4);
        int found = 0;
        for (uint32_t k = 0; k < kernel_layout_count; k++) {
            if (kernel_layout[k].id == id) {
                if (kernel_layout[k].value != value) {
                    return PURR_SHELLHDR_LAYOUT_MISMATCH;
                }
                found = 1;
                break;
            }
        }
        if (!found) {
            return PURR_SHELLHDR_LAYOUT_MISMATCH;
        }
    }
    return PURR_SHELLHDR_OK;
}

purr_shellhdr_result_t purr_shellhdr_patch(uint8_t *code, uint32_t code_size, const purr_shellhdr_t *h, const char *const *kernel_names, uint32_t kernel_name_count,
                                           const void *const *exports)
{
    purr_shellhdr_result_t r = check_imports(code, code_size, h, kernel_names, kernel_name_count, NULL);
    if (r != PURR_SHELLHDR_OK) {
        return r;
    }
    for (uint32_t i = 0; i < h->import_count; i++) {
        uint32_t off = rd32(code + h->imports_offset + 8u * i);
        const char *name = name_at(code, h, rd32(code + h->imports_offset + 8u * i + 4));
        uint32_t addr = (uint32_t)(uintptr_t)exports[find_name(kernel_names, kernel_name_count, name)];
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
    case PURR_SHELLHDR_DAMAGED: return "damaged shell header";
    case PURR_SHELLHDR_BAD_IMPORT: return "bad import site";
    case PURR_SHELLHDR_NEEDS_NEWER: return "uses something this system does not have";
    case PURR_SHELLHDR_LAYOUT_MISMATCH: return "built for a different layout of this system";
    }
    return "?";
}
