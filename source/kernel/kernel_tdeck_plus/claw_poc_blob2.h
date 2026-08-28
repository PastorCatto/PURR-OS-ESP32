#pragma once
// claw_poc_blob2.h — EXPERIMENTAL. Round 2 of the cross-device .claw
// loading R&D spike: can loaded code call BACK into the host firmware?
// See claw_poc_test.c's claw_poc_run2() for what this proves.
//
// guest2.c, for reference (not compiled as part of this build):
//
//   typedef void (*host_log_fn)(const char *msg);
//   static const char greeting[] = "hello from loaded .claw code";
//   int entry2(int x, host_log_fn log) {
//       log(greeting);
//       return x * 2;
//   }
//
// Compiled: xtensa-esp32s3-elf-gcc -mtext-section-literals -mlongcalls -O0 -c
//
// entry2(21, poc_host_log) should call poc_host_log(greeting) and return 42.
//
// Only ONE relocation this time (the greeting string) — confirmed via
// objdump before this file was written: the call through `log` compiles to
// `callx8 a8` (an indirect call through a register loaded from this
// function's OWN stack, where the caller's argument was stored), with NO
// relocation entry at all. It's a pure runtime value, not a compile-time
// symbol reference — exactly how catcall_ui_t/purr_win.h already dispatch
// every app->kernel call (_ui->win_create(...) etc.), which is why this
// case matters more than round 1's arithmetic did.

#include <stdint.h>

#define GUEST2_ENTRY_OFF 4
#define GUEST2_BSS_SIZE  0

static const uint8_t guest2_text[] = {
    0x00, 0x00, 0x00, 0x00, 0x36, 0x61, 0x00, 0x7d, 0x01, 0x29, 0x07, 0x39,
    0x17, 0x91, 0xfc, 0xff, 0x88, 0x17, 0x90, 0xa9, 0x20, 0xe0, 0x08, 0x00,
    0x88, 0x07, 0x8a, 0x88, 0x2d, 0x08, 0x1d, 0xf0,
};
static const uint8_t guest2_rodata[] = {
    0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x66, 0x72, 0x6f, 0x6d, 0x20, 0x6c,
    0x6f, 0x61, 0x64, 0x65, 0x64, 0x20, 0x2e, 0x63, 0x6c, 0x61, 0x77, 0x20,
    0x63, 0x6f, 0x64, 0x65, 0x00,
};

// Reuses guest_patch_t from claw_poc_blob.h — include that header first.
// target_section: 0 = .rodata, 1 = .bss
static const guest_patch_t guest2_patches[] = {
    { 0, 0, 0 },   // .text+0 <- &greeting (.rodata)
};
#define GUEST2_PATCH_COUNT 1
