#pragma once
// purr_icons.h — the LVGL launcher's icon library: a small, fixed set of
// named 32x32 icons, handed out as opaque image sources. Real code, NOT a
// claw package (unlike launcher_lvgl.c) — same reason purr_lv_style.h
// stays real: an lv_img_dsc_t's contents (pixel format, RGB565 byte order
// under CONFIG_LV_COLOR_16_SWAP) depend on the build's own LVGL config,
// which no claw object should have to get right by hand. So the struct is
// built here, and the loaded launcher only ever sees a `const void *` it
// passes straight to lv_img_set_src() without looking inside.
//
// A plain declaration header, not header-only/static-inline — purrstrap.py's
// _extract_public_functions() (what makes a function callable from claw
// code at all) scans for ordinary `type name(args);` prototypes.
//
// Names in the set: "calculator", "clock", "settings", "file_manager". Not
// every app has an icon; launcher_lvgl.c's own icon_name_for_app() decides
// the mapping, and an unmapped app just shows its label alone.
//
// Flash cost: 12KB fleet-wide (4 icons x 32x32 x RGB565+A) — deliberate,
// main already links lvgl__lvgl on every device regardless of its ui= flag.
//
// Not to be confused with the MiniWin icon library (source/modules/miniwin/
// icon_lib/purr_icon_lib.h), a separate Win 3.1-art RGB888 set for MiniWin's
// own desktop — its lookup is purr_miniwin_icon_get(), renamed so a device
// that bundles both (safe_mode_ui) doesn't get two different functions
// named purr_icon_get().

#ifdef __cplusplus
extern "C" {
#endif

// The lv_img source for `name`, or NULL for an unknown or NULL name.
// lv_img_set_src(obj, NULL) is a safe no-op, so callers may pass the
// result straight through.
const void *purr_icon_get(const char *name);

#ifdef __cplusplus
}
#endif
