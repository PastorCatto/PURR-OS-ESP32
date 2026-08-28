#pragma once
// claw_elf.h — EXPERIMENTAL, cross-device .claw loading R&D spike.
//
// A minimal ELF32/Xtensa relocatable-object (ET_REL) parser, replacing the
// hand-extraction script (extract.py, scratch-only, never in this repo)
// that rounds 1-2 of this spike relied on. Given the RAW compiled .o bytes
// — the exact same kind of file that script parsed on the host — this
// walks the real section/symbol/relocation tables at RUNTIME, on-device,
// and produces the same load plan a real loader needs.
//
// Section layout convention this parser assumes (true of any plain
// `xtensa-esp32s3-elf-gcc -c` compile, not something specific to the PoC
// guest files): .text holds code + any literal pool
// (-mtext-section-literals), .rodata/.data/.bss hold static storage.
//
// Relocation handling — confirmed via objdump before this file was
// written, not assumed: R_XTENSA_SLOT0_OP entries (call8 targets, l32r
// operands referencing a literal-pool slot within .text) are already
// correctly PC-relative as compiled, self-relocating as long as .text loads
// as one contiguous blob — this parser never returns those. Only
// R_XTENSA_32 entries in .text (an absolute address baked into a literal
// pool slot, pointing at something in a DIFFERENT section) need patching at
// load time, and that target can legitimately be .text itself too (e.g. a
// function pointer to a sibling function) — this parser handles that case
// generally, unlike round 1-2's hand-written two-case patch list.
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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
