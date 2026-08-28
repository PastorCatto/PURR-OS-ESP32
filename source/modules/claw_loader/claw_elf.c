// claw_elf.c — see claw_elf.h for the full picture.
//
// Byte layouts below are ELF32 (LSB) exactly as the ABI defines them —
// verified field-by-field against a real compiled object's actual bytes
// before this file was written (cross-checked against readelf's own
// output, not copied from a reference without checking). __attribute__
// ((packed)) because e_ident's 16 bytes keep every following field
// naturally misaligned relative to a struct start that isn't itself
// 4-aligned in the source buffer — this parser only ever reads through
// memcpy into these structs (never casts a raw pointer to one and
// dereferences it), so packing doesn't cost the unaligned-access trap some
// other Xtensa code in this codebase has hit; see purr_port.h for that.

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "claw_elf.h"

static const char *TAG = "claw_elf";

#define ET_REL      1
#define EM_XTENSA   94
#define ELFCLASS32  1
#define ELFDATA2LSB 1

#define SHT_SYMTAB  2
#define SHT_STRTAB  3
#define SHT_RELA    4
#define SHT_NOBITS  8

#define R_XTENSA_32        1
#define R_XTENSA_SLOT0_OP  20   // self-relocating, see claw_elf.h's header comment — never returned as a patch

typedef struct __attribute__((packed)) {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf32_ehdr_t;

typedef struct __attribute__((packed)) {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} elf32_shdr_t;

typedef struct __attribute__((packed)) {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
} elf32_sym_t;

typedef struct __attribute__((packed)) {
    uint32_t r_offset;
    uint32_t r_info;
    int32_t  r_addend;
} elf32_rela_t;

#define R_SYM(info)  ((info) >> 8)
#define R_TYPE(info) ((info) & 0xff)

// All reads go through this — bounds-checks every access against the
// buffer this ELF file actually is, so a truncated/malformed/hostile input
// fails cleanly instead of reading past the end of `data`.
static bool read_at(const uint8_t *data, size_t len, size_t off, size_t sz, void *out)
{
    if (off > len || sz > len - off) return false;
    memcpy(out, data + off, sz);
    return true;
}

static const char *str_at(const uint8_t *data, size_t len, size_t strtab_off, size_t strtab_size, uint32_t name_off)
{
    if (name_off >= strtab_size) return NULL;
    size_t abs_off = strtab_off + name_off;
    if (abs_off >= len) return NULL;
    // Caller only ever compares this against known-short constant strings —
    // safe to hand back a raw pointer into `data` rather than copying, as
    // long as `data` outlives the comparison (true for every call site
    // below, all synchronous within one parse).
    size_t max = len - abs_off;
    const char *s = (const char *)(data + abs_off);
    size_t n = 0;
    while (n < max && s[n] != '\0') n++;
    return (n < max) ? s : NULL;   // NULL if the string runs off the buffer unterminated
}

static bool find_section(const uint8_t *data, size_t len, const elf32_ehdr_t *eh,
                          const char *name, elf32_shdr_t *out, int *out_idx)
{
    elf32_shdr_t shstrtab;
    if (!read_at(data, len, eh->e_shoff + (size_t)eh->e_shstrndx * eh->e_shentsize,
                 sizeof(shstrtab), &shstrtab)) return false;

    for (int i = 0; i < eh->e_shnum; i++) {
        elf32_shdr_t sh;
        if (!read_at(data, len, eh->e_shoff + (size_t)i * eh->e_shentsize, sizeof(sh), &sh)) return false;
        const char *sn = str_at(data, len, shstrtab.sh_offset, shstrtab.sh_size, sh.sh_name);
        if (sn && strcmp(sn, name) == 0) {
            *out = sh;
            if (out_idx) *out_idx = i;
            return true;
        }
    }
    return false;
}

static bool find_symbol(const uint8_t *data, size_t len,
                         const elf32_shdr_t *symtab, const elf32_shdr_t *strtab,
                         const char *name, elf32_sym_t *out)
{
    if (symtab->sh_entsize == 0) return false;
    uint32_t count = symtab->sh_size / symtab->sh_entsize;
    for (uint32_t i = 0; i < count; i++) {
        elf32_sym_t sym;
        if (!read_at(data, len, symtab->sh_offset + (size_t)i * symtab->sh_entsize, sizeof(sym), &sym)) return false;
        const char *sn = str_at(data, len, strtab->sh_offset, strtab->sh_size, sym.st_name);
        if (sn && strcmp(sn, name) == 0) {
            *out = sym;
            return true;
        }
    }
    return false;
}

// Shared validation + section lookup for both public entry points below.
static bool validate_and_find(const uint8_t *data, size_t len, elf32_ehdr_t *eh_out,
                               elf32_shdr_t *text_out, int *text_idx_out,
                               elf32_shdr_t *symtab_out, elf32_shdr_t *strtab_out)
{
    if (!read_at(data, len, 0, sizeof(*eh_out), eh_out)) {
        ESP_LOGE(TAG, "file too short for an ELF header (%u B)", (unsigned)len);
        return false;
    }
    if (eh_out->e_ident[0] != 0x7f || eh_out->e_ident[1] != 'E' ||
        eh_out->e_ident[2] != 'L'  || eh_out->e_ident[3] != 'F') {
        ESP_LOGE(TAG, "bad magic");
        return false;
    }
    if (eh_out->e_ident[4] != ELFCLASS32 || eh_out->e_ident[5] != ELFDATA2LSB) {
        ESP_LOGE(TAG, "not ELF32 LSB (class=%u data=%u)", eh_out->e_ident[4], eh_out->e_ident[5]);
        return false;
    }
    if (eh_out->e_type != ET_REL) {
        ESP_LOGE(TAG, "not a relocatable object (e_type=%u) — must be a plain `-c` compile, never linked", eh_out->e_type);
        return false;
    }
    if (eh_out->e_machine != EM_XTENSA) {
        ESP_LOGE(TAG, "wrong machine (e_machine=%u, expected EM_XTENSA=%u)", eh_out->e_machine, EM_XTENSA);
        return false;
    }
    if (eh_out->e_shentsize < sizeof(elf32_shdr_t) || eh_out->e_shstrndx >= eh_out->e_shnum) {
        ESP_LOGE(TAG, "malformed section header table");
        return false;
    }
    if (!find_section(data, len, eh_out, ".text", text_out, text_idx_out)) {
        ESP_LOGE(TAG, "no .text section");
        return false;
    }
    if (!find_section(data, len, eh_out, ".symtab", symtab_out, NULL)) {
        ESP_LOGE(TAG, "no .symtab section");
        return false;
    }
    if (!find_section(data, len, eh_out, ".strtab", strtab_out, NULL)) {
        ESP_LOGE(TAG, "no .strtab section");
        return false;
    }
    return true;
}

bool claw_elf_find_offset(const uint8_t *data, size_t len, const char *symbol, uint32_t *out_off)
{
    elf32_ehdr_t eh;
    elf32_shdr_t text_sh, symtab_sh, strtab_sh;
    int text_idx = -1;
    if (!validate_and_find(data, len, &eh, &text_sh, &text_idx, &symtab_sh, &strtab_sh)) return false;

    elf32_sym_t sym;
    if (!find_symbol(data, len, &symtab_sh, &strtab_sh, symbol, &sym)) {
        ESP_LOGE(TAG, "symbol '%s' not found", symbol);
        return false;
    }
    if ((int)sym.st_shndx != text_idx) {
        ESP_LOGE(TAG, "symbol '%s' is not in .text (shndx=%u)", symbol, sym.st_shndx);
        return false;
    }
    *out_off = sym.st_value;
    return true;
}

bool claw_elf_load(const uint8_t *data, size_t len, const char *entry_symbol, claw_module_t *out)
{
    memset(out, 0, sizeof(*out));

    elf32_ehdr_t eh;
    elf32_shdr_t text_sh, symtab_sh, strtab_sh;
    int text_idx = -1;
    if (!validate_and_find(data, len, &eh, &text_sh, &text_idx, &symtab_sh, &strtab_sh)) return false;

    elf32_shdr_t rodata_sh, data_sh, bss_sh, rela_sh;
    int rodata_idx = -1, data_idx = -1, bss_idx = -1;
    bool have_rodata = find_section(data, len, &eh, ".rodata", &rodata_sh, &rodata_idx);
    bool have_data   = find_section(data, len, &eh, ".data",   &data_sh,   &data_idx);
    bool have_bss    = find_section(data, len, &eh, ".bss",    &bss_sh,    &bss_idx);

    elf32_sym_t entry_sym;
    if (!find_symbol(data, len, &symtab_sh, &strtab_sh, entry_symbol, &entry_sym)) {
        ESP_LOGE(TAG, "symbol '%s' not found", entry_symbol);
        return false;
    }
    if ((int)entry_sym.st_shndx != text_idx) {
        ESP_LOGE(TAG, "symbol '%s' is not in .text (shndx=%u)", entry_symbol, entry_sym.st_shndx);
        return false;
    }

    // .rela.text is optional — a module with no cross-section references
    // legitimately has none.
    bool have_rela = find_section(data, len, &eh, ".rela.text", &rela_sh, NULL);

    int patch_count = 0;
    if (have_rela) {
        if (rela_sh.sh_entsize == 0) { ESP_LOGE(TAG, "malformed .rela.text"); return false; }
        uint32_t rel_n = rela_sh.sh_size / rela_sh.sh_entsize;
        for (uint32_t i = 0; i < rel_n; i++) {
            elf32_rela_t rel;
            if (!read_at(data, len, rela_sh.sh_offset + (size_t)i * rela_sh.sh_entsize, sizeof(rel), &rel)) {
                ESP_LOGE(TAG, "truncated .rela.text");
                return false;
            }
            if (R_TYPE(rel.r_info) == R_XTENSA_32) patch_count++;
        }
    }

    claw_patch_t *patches = NULL;
    if (patch_count > 0) {
        patches = heap_caps_malloc((size_t)patch_count * sizeof(claw_patch_t), MALLOC_CAP_8BIT);
        if (!patches) { ESP_LOGE(TAG, "patch array alloc failed (%d entries)", patch_count); return false; }
    }

    int fill = 0;
    if (have_rela) {
        uint32_t rel_n = rela_sh.sh_size / rela_sh.sh_entsize;
        for (uint32_t i = 0; i < rel_n; i++) {
            elf32_rela_t rel;
            if (!read_at(data, len, rela_sh.sh_offset + (size_t)i * rela_sh.sh_entsize, sizeof(rel), &rel)) goto fail;
            if (R_TYPE(rel.r_info) != R_XTENSA_32) continue;

            elf32_sym_t sym;
            if (!read_at(data, len, symtab_sh.sh_offset + (size_t)R_SYM(rel.r_info) * symtab_sh.sh_entsize,
                         sizeof(sym), &sym)) goto fail;

            claw_sec_kind_t kind;
            if ((int)sym.st_shndx == text_idx)          kind = CLAW_SEC_TEXT;
            else if (have_rodata && (int)sym.st_shndx == rodata_idx) kind = CLAW_SEC_RODATA;
            else if (have_data   && (int)sym.st_shndx == data_idx)   kind = CLAW_SEC_DATA;
            else if (have_bss    && (int)sym.st_shndx == bss_idx)    kind = CLAW_SEC_BSS;
            else {
                ESP_LOGE(TAG, "relocation at .text+%u targets an unsupported section (shndx=%u) — "
                              "only .text/.rodata/.data/.bss are handled", (unsigned)rel.r_offset, sym.st_shndx);
                goto fail;
            }

            patches[fill].text_off    = rel.r_offset;
            patches[fill].target_kind = kind;
            patches[fill].target_off  = sym.st_value + (uint32_t)rel.r_addend;
            fill++;
        }
    }

    out->text        = data + text_sh.sh_offset;
    out->text_size   = text_sh.sh_size;
    out->rodata      = have_rodata ? data + rodata_sh.sh_offset : NULL;
    out->rodata_size = have_rodata ? rodata_sh.sh_size : 0;
    out->data        = have_data ? data + data_sh.sh_offset : NULL;
    out->data_size   = have_data ? data_sh.sh_size : 0;
    out->bss_size    = have_bss ? bss_sh.sh_size : 0;
    out->entry_off   = entry_sym.st_value;
    out->patches     = patches;
    out->patch_count = fill;

    ESP_LOGI(TAG, "parsed: text=%uB rodata=%uB data=%uB bss=%uB entry('%s')=+%u patches=%d",
             (unsigned)out->text_size, (unsigned)out->rodata_size, (unsigned)out->data_size,
             (unsigned)out->bss_size, entry_symbol, (unsigned)out->entry_off, out->patch_count);
    return true;

fail:
    if (patches) heap_caps_free(patches);
    return false;
}

void claw_elf_free(claw_module_t *m)
{
    if (m->patches) heap_caps_free(m->patches);
    m->patches = NULL;
    m->patch_count = 0;
}
