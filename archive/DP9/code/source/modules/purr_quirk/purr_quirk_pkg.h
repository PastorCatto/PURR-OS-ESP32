#pragma once
// purr_quirk_pkg.h — .purr v2: device quirk-data package format.
//
// NOT the Gen-1 code-loading format (see docs/10_ModuleLoading.md and
// driver_manager.c's/purr_kernel.c's own history for why that one never
// worked). This format carries DATA ONLY — no code, no relocation, no
// execute-from-flash mapping, no capability boundary to design. A driver
// reads a named block and interprets it as a plain struct, exactly the
// same trust model catcall_display_t's own display_config_t already has
// for st7789_init(cfg). It shares the .purr extension with the dead
// format — same idea, "one file per device's specialness," borrowed from
// how postmarketOS bundles a per-device package (deviceinfo + kernel
// config + patches) alongside a shared, generic base OS — but a
// DIFFERENT magic number, so a loader can never confuse this for the
// old, broken format, or vice versa.
//
// The package this generalizes: this codebase already has an established
// pattern of a generic driver (st7789, gt911, sx1262_rl, sx1262, ssd1306,
// ili9341, adc_battery — confirmed, all seven) exposing a plain
// <driver>_configure(pin, pin, ...) function that a device's own
// specialized kernel boot.c calls with HARDCODED literal values (or, for
// adc_battery alone today, purrstrap-generated ones baked into
// purr_device_glue.c). That is a real, working pattern — this format does
// not replace it, it gives it a loadable, swappable data source: a boot
// file checks for a named quirk block FIRST and falls back to its own
// hardcoded defaults when none is loaded or a device never shipped one,
// so every existing device's behavior is completely unchanged unless it
// actually gains a .purr quirk package.

#include <stdint.h>

#define PURR_QUIRK_MAGIC       0x51525550u  // 'PURQ' — deliberately not 'PURR'
#define PURR_QUIRK_ABI_VERSION 1
#define PURR_QUIRK_MAX_BLOCKS  16
#define PURR_QUIRK_NAME_MAX    24

// One named data block — e.g. "st7789.pins", "st7789.panel_profile",
// "gt911.pins", "adc_battery.config". `offset` is a byte offset from the
// START OF THE FILE (not from the end of the header) to this block's
// payload, so a reader can fread()/seek directly to it without needing to
// have parsed every earlier block's payload first.
typedef struct {
    char     name[PURR_QUIRK_NAME_MAX];
    uint32_t offset;
    uint32_t size;
} purr_quirk_block_t;

// Fixed-size header — blocks[] is inline, not a separate variable-length
// table, matching this codebase's existing convention of reading a plain
// struct directly off disk (purr_module_header_t, display_config_t) with
// no separate parsing pass. A package with fewer than PURR_QUIRK_MAX_BLOCKS
// entries simply leaves the remaining slots zeroed (size == 0, meaning
// "unused" — purr_quirk_get_block() skips those).
//
// abi_version and block_count are plain uint32_t here, not the uint8_t/
// uint16_t their actual value ranges would need — deliberately, so every
// field in this header lands on a natural 4-byte boundary with ZERO
// implicit compiler padding anywhere (magic=4, abi_version=4, device_name
// is a 32-byte char array with no alignment need of its own, block_count=4
// landing at offset 4+4+32=40, already 4-aligned, then blocks[] whose own
// 24-byte name field is also a multiple of 4). This is the actual point:
// purrstrap's Python-side generator has to produce byte-for-byte identical
// layout with struct.pack(), and a format string that has to replicate
// this compiler's implicit padding rules is a real, easy way to get that
// silently wrong. Choosing field widths that make the padding question
// not arise at all is safer than computing it correctly once and hoping
// it never needs to be recomputed.
typedef struct {
    uint32_t magic;             // PURR_QUIRK_MAGIC
    uint32_t abi_version;       // PURR_QUIRK_ABI_VERSION
    char     device_name[32];   // which device.pcat this was generated from — informational, not checked against the running device
    uint32_t block_count;
    purr_quirk_block_t blocks[PURR_QUIRK_MAX_BLOCKS];
    // block payloads follow in the file, at each block's own `offset`
} purr_quirk_pkg_header_t;
