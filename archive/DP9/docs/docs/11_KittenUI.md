# 11 — KittenUI

> **Archived.** Moved to `archive/ui_backends_v1/` alongside every other UI
> backend for the protected-processes/Unix-boot rewrite's console-only
> test phase — see that folder's own `README.md`.

KittenUI is PURR OS's small-screen UI module, built on LVGL. It replaces MiniWin on all displays ≤ 320×240. Larger displays (480×320+) continue to use MiniWin. The heltec's 128×64 OLED uses a separate text-mode `oled_ui` module.

---

## Device UI Assignment

| Device | Screen | UI Module | Theme |
|---|---|---|---|
| cyd | 320×240 ILI9341 | KittenUI | WCE Classic |
| cyd_s024c | 240×320 ILI9341 | KittenUI | WCE Classic |
| cyd_s028r | 320×240 ILI9341 | KittenUI | WCE Classic |
| tdeck | 320×240 ST7789 | KittenUI | WCE Classic |
| tdeck_plus | 320×240 ST7789 | KittenUI | WCE Classic |
| waveshare169 | 240×280 ST7789 | KittenUI | WCE Classic |
| jc3248w535 | 480×320 AXS15231B | MiniWin | WCE Classic |
| heltec | 128×64 SSD1306 | oled_ui | (text-mode) |

Theme is set per-device in `device.pcat` under `[ui] theme = <id>`. It can also be overridden at runtime and persisted to NVS.

---

## Architecture

```
App code / modules
        │ LVGL widget API
        ▼
kittenui_module.c    ← module init, theme registry, LVGL task
kittenui_hal.c       ← flush_cb, touch_read_cb (catcall → LVGL)
        │
        ▼ catcall_display_t.push_pixels()
        ▼ catcall_touch_t.is_pressed() / read_point()
Hardware driver (.purr)
```

KittenUI never touches hardware directly. All display and touch goes through catcalls. This means themes, widgets, and apps are fully device-agnostic.

---

## Theme System

### Built-in Themes

| ID | Name | Style |
|---|---|---|
| `wce` | WCE Classic | Silver/navy, raised 3D buttons, square corners |
| `dark` | Dark | Near-black surfaces, muted text, blue accent |

### Switching Themes

**At build time** — set in `device.pcat`:
```toml
[ui]
theme = dark
```

**At runtime** — via NVS (persisted across reboots):
```c
// From any app or module:
nvs_handle_t h;
nvs_open("purr_settings", NVS_READWRITE, &h);
nvs_set_str(h, "theme", "dark");
nvs_commit(h);
nvs_close(h);
// Then call:
kittenui_apply_theme();
```

---

## Writing a Custom Theme

You only need `kittenui_theme.h`. No other KittenUI internals required.

```c
// my_theme.c
#include "kittenui_theme.h"
#include "lvgl.h"

static kittenui_theme_t s_my_theme;
static bool s_init = false;

const kittenui_theme_t *my_theme(void) {
    if (!s_init) {
        kittenui_palette_t *p = &s_my_theme.palette;
        p->window_bg     = lv_color_make(0xFF, 0xF0, 0xE0); // warm white
        p->titlebar      = lv_color_make(0x8B, 0x00, 0x00); // dark red
        p->titlebar_text = lv_color_make(0xFF, 0xFF, 0xFF);
        p->selected      = lv_color_make(0x8B, 0x00, 0x00);
        p->selected_text = lv_color_make(0xFF, 0xFF, 0xFF);
        p->text          = lv_color_make(0x00, 0x00, 0x00);
        p->surface       = lv_color_make(0xFF, 0xF0, 0xE0);
        p->border        = lv_color_make(0x60, 0x60, 0x60);
        p->border_light  = lv_color_make(0xFF, 0xFF, 0xFF);
        p->border_dark   = lv_color_make(0x40, 0x40, 0x40);
        p->accent        = lv_color_make(0x8B, 0x00, 0x00);
        p->danger        = lv_color_make(0xFF, 0x00, 0x00);
        p->success       = lv_color_make(0x00, 0x80, 0x00);
        // ... fill remaining fields ...

        s_my_theme.name = "My Theme";
        s_my_theme.id   = "mytheme";   // used in NVS / device.pcat

        s_my_theme.fonts.body    = &lv_font_montserrat_14;
        s_my_theme.fonts.heading = &lv_font_montserrat_16;
        s_my_theme.fonts.small   = &lv_font_montserrat_10;
        s_my_theme.fonts.mono    = &lv_font_unscii_8;

        s_my_theme.flags.raised_buttons  = true;
        s_my_theme.flags.rounded_corners = true;
        s_my_theme.flags.corner_radius   = 4;
        s_my_theme.flags.padding         = 4;
        s_my_theme.flags.item_height     = 22;

        // Optional: do anything LVGL exposes beyond the struct
        s_my_theme.apply_fn = NULL;

        s_init = true;
    }
    return &s_my_theme;
}
```

Register it before `kittenui_init()` is called (e.g. in your module's init):

```c
kittenui_register_theme(my_theme());
```

Then select it in `device.pcat`:
```toml
[ui]
theme = mytheme
```

The `apply_fn` field is an escape hatch for anything the struct doesn't cover — gradient fills, custom widget styles, animation tweaks — it receives the full theme pointer and runs after the palette is applied.

---

## LVGL Draw Buffer

KittenUI uses a double-buffer strategy: two buffers of `KITTENUI_BUF_LINES` rows each (default 20). This gives partial refresh without requiring a full framebuffer in RAM — important on devices without PSRAM.

To tune for your device, set `KITTENUI_BUF_LINES` in `menuconfig` or override it in CMakeLists:
```cmake
target_compile_definitions(kittenui PRIVATE KITTENUI_BUF_LINES=10)
```

Devices with PSRAM (tdeck_plus, jc3248w535) can use a full framebuffer if needed by setting `full_refresh = 1` in the LVGL driver config.

---

## Files

| File | Purpose |
|---|---|
| `kittenui_theme.h` | Public theme API — all you need to write a custom theme |
| `kittenui.h` | Public KittenUI API |
| `kittenui_module.c` | Module init, theme registry, LVGL task |
| `kittenui_hal.c` | LVGL ↔ catcall_display/touch bridge |
| `themes/theme_wce.c` | WCE Classic built-in |
| `themes/theme_dark.c` | Dark built-in |
| `module.pcat` | Module metadata + build manifest |
