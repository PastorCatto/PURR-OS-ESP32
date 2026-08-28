#pragma once
// claw_poc_blob.h — EXPERIMENTAL. Auto-extracted (by the R&D spike's
// scratch extract.py, not part of the repo's build) from a standalone
// compile of a tiny guest.c, never linked into this firmware. See
// claw_poc_test.c for what this actually proves and how it's used.
//
// guest.c, for reference (not compiled as part of this build):
//
//   static int counter = 0;
//   static const char tag[] = "PICOK";
//   __attribute__((noinline)) static int add_one(int x) { return x + 1; }
//   int entry(int x) {
//       volatile const char *t = tag;
//       counter += 1;
//       return add_one(x) + t[0] + counter;
//   }
//
// Compiled: xtensa-esp32s3-elf-gcc -mtext-section-literals -mlongcalls -O0 -c
//
// entry(10) should evaluate to: add_one(10)=11, t[0]='P'=80, counter=1
// (first call) -> 11 + 80 + 1 = 92.

#include <stdint.h>

#define GUEST_ENTRY_OFF 24
#define GUEST_BSS_SIZE  4

static const uint8_t guest_text[] = {
    0x36, 0x61, 0x00, 0x7d, 0x01, 0x29, 0x07, 0x88, 0x07, 0x1b, 0x88, 0x2d,
    0x08, 0x1d, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x36, 0x81, 0x00, 0x7d, 0x01, 0x29, 0x47, 0x81, 0xfc, 0xff, 0x89, 0x07,
    0x81, 0xfc, 0xff, 0x88, 0x08, 0x1b, 0x98, 0x81, 0xfa, 0xff, 0x99, 0x08,
    0xa8, 0x47, 0xe5, 0xfc, 0xff, 0x9d, 0x0a, 0x88, 0x07, 0xc0, 0x20, 0x00,
    0x82, 0x08, 0x00, 0x80, 0x80, 0x74, 0x8a, 0x99, 0x81, 0xf4, 0xff, 0x88,
    0x08, 0x8a, 0x89, 0x2d, 0x08, 0x1d, 0xf0,
};
static const uint8_t guest_rodata[] = {
    0x50, 0x49, 0x43, 0x4f, 0x4b, 0x00,
};

// Two relocations, both confirmed (via objdump, before this file was
// written — see the R&D spike's own findings) to be the ONLY ones needing
// patching: literal-pool slots in .text holding the absolute address of
// something in a different section. Every call8/l32r-to-an-intra-.text-
// target relocation (R_XTENSA_SLOT0_OP) is deliberately NOT here — it's
// already correctly encoded as long as .text loads as one contiguous blob,
// which this loader guarantees by construction.
typedef struct { uint32_t text_off; uint8_t target_section; int32_t addend; } guest_patch_t;
// target_section: 0 = .rodata, 1 = .bss
static const guest_patch_t guest_patches[] = {
    { 20, 1, 0 },   // .text+20 <- &counter (.bss)
    { 16, 0, 0 },   // .text+16 <- &tag     (.rodata)
};
#define GUEST_PATCH_COUNT 2
