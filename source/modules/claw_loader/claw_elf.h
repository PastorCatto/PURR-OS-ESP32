#pragma once
// claw_elf.h — minimal ELF32/Xtensa relocatable-object (ET_REL) parser.
//
// Promoted out of kernel_tdeck_plus's R&D-spike scratch territory once the
// spike itself confirmed the whole approach on real hardware (see this
// module's own module.pcat and git history for the three-round PoC that
// proved it) — this is now real, reusable infrastructure, not throwaway
// test code.
//
// Given the RAW compiled .o bytes for a never-linked object (a plain
// `xtensa-esp32s3-elf-gcc -c` compile), walks the real section/symbol/
// relocation tables at RUNTIME and produces a load plan: which bytes are
// .text/.rodata/.data, how big .bss needs to be, and exactly which
// relocations need patching once those pieces have real addresses.
//
// Section layout convention: .text holds code + any literal pool
// (-mtext-section-literals), .rodata/.data/.bss hold static storage — true
// of any plain compile, nothing PoC-specific.
//
// Relocation handling — confirmed via objdump before this was written, not
// assumed: R_XTENSA_SLOT0_OP entries (call8 targets, l32r operands
// referencing a literal-pool slot within .text) are already correctly
// PC-relative as compiled, self-relocating as long as .text loads as one
// contiguous blob — never returned as something to patch. Only R_XTENSA_32
// entries in .text (an absolute address baked into a literal pool slot,
// pointing at something in a possibly-different section — including .text
// itself, e.g. a function pointer to a sibling function) need patching at
// load time.
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLAW_SEC_TEXT = 0,
    CLAW_SEC_RODATA,
    CLAW_SEC_DATA,
    CLAW_SEC_BSS,
} claw_sec_kind_t;

typedef struct {
    uint32_t        text_off;     // byte offset within .text to patch — only .text ever needs patching, it's the only section with code/literals
    claw_sec_kind_t target_kind;  // which section the patched 4-byte value should point into
    uint32_t        target_off;   // offset within that section (symbol's st_value + relocation addend)
} claw_patch_t;

typedef struct {
    const uint8_t *text;    size_t text_size;
    const uint8_t *rodata;  size_t rodata_size;
    const uint8_t *data;    size_t data_size;
    size_t         bss_size;
    uint32_t       entry_off;    // requested symbol's offset within .text
    claw_patch_t  *patches;      // heap_caps_malloc'd (MALLOC_CAP_8BIT) — free with claw_elf_free()
    int            patch_count;
} claw_module_t;

// Parses `data` (len bytes) as an ELF32/Xtensa ET_REL object. text/rodata/
// data point INTO `data` (zero-copy — `data` must stay valid as long as
// `out` is used); bss has no file content by definition (SHT_NOBITS), only
// a size. Looks up entry_symbol's offset within .text.
//
// Returns false (out->patches left NULL, nothing to free) on any parse
// failure: bad magic/class/machine/type, a missing expected section, or the
// entry symbol not found — logs the specific reason via ESP_LOGE.
bool claw_elf_load(const uint8_t *data, size_t len, const char *entry_symbol, claw_module_t *out);

void claw_elf_free(claw_module_t *m);

// Lightweight second symbol lookup against an already-validated object —
// for a loader that needs more than one well-known entry point (e.g. an
// init AND a deinit) without re-walking the relocation table a second time.
// Returns false (out_off untouched) if the object doesn't parse or the
// symbol isn't in .text — same validation claw_elf_load() does, just
// without building a patch list.
bool claw_elf_find_offset(const uint8_t *data, size_t len, const char *symbol, uint32_t *out_off);

#ifdef __cplusplus
}
#endif
