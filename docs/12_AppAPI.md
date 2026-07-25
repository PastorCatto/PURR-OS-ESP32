# 12 — App API & Unified Windowing

PURR OS apps never call LVGL or MiniWin directly. All UI goes through a single unified API — `purr_win.h` — which dispatches to whichever UI module is active at runtime.

---

## Layer Stack

```
App code   (terminal.c, calculator.c, your_app.c)
    │  #include "purr_win.h"
    │  purr_win_create(), purr_win_button(), purr_win_textarea() ...
    ▼
catcall_ui_t  ← registered by whichever UI module loaded first
    │
    ├── kittenui_win.c  (LVGL backend — small screens ≤320×240)
    └── miniwin_win.c   (MiniWin backend — large screens 480×320+)
    │
    ▼  catcall_display_t.push_pixels()
Display driver (.purr)
    ▼
Physical screen
```

Catcalls are the **hardware** layer (display pixels, touch points, radio packets). The app API is the **UI widget** layer above them. Apps sit above both.

---

## Choosing a Tier

| Tier | File ext | Gets `purr_win.h` | Gets kernel API | Use for |
|------|----------|-------------------|-----------------|---------|
| `.meow` | Lua script | via Lua bindings | No | Scripted tools, games |
| `.paws` | Compiled C | Yes | No | Calculators, viewers, utilities |
| `.claw` | Compiled C | Yes | Yes | Terminal, system tools, emulators |

Set `tier` in `app.pcat`. The build system enforces it.

---

## Quick Start

```c
#include "purr_win.h"   // that's it — no LVGL, no MiniWin

static purr_win_t win;

void my_app_init(void) {
    win = purr_win_create("My App");

    purr_wid_t lbl = purr_win_label(win, "Hello, PURR OS!");
    purr_win_label_align(lbl, PURR_ALIGN_CENTER);

    purr_wid_t btn = purr_win_button(win, "Tap me", on_tap, NULL);

    purr_win_show(win);
}

static void on_tap(purr_wid_t wid, purr_event_t event, void *user) {
    purr_win_label_set(lbl, "Tapped!");
}
```

---

## API Reference

### Windows

```c
purr_win_t purr_win_create  (const char *title);   // 0 if no UI backend registered
void       purr_win_show    (purr_win_t win);
void       purr_win_hide    (purr_win_t win);
void       purr_win_clear   (purr_win_t win);  // remove all child widgets
void       purr_win_destroy (purr_win_t win);
void       purr_win_on_close(purr_win_t win, purr_win_cb_t cb, void *user);
```

`purr_win_on_close()` fires when the backend's own close affordance is used, in
addition to its default hide behaviour — it's how `app_manager` learns the user
asked to *close* rather than *minimise*. Optional; a no-op on backends that
don't implement it.

### Labels

```c
purr_wid_t purr_win_label        (purr_win_t win, const char *text);
void       purr_win_label_set    (purr_wid_t wid, const char *text);
void       purr_win_label_align  (purr_wid_t wid, purr_align_t align);
void       purr_win_label_set_big(purr_wid_t wid, const char *text);
// align: PURR_ALIGN_LEFT | PURR_ALIGN_CENTER | PURR_ALIGN_RIGHT
```

`purr_win_label_set_big()` re-renders an existing label in a banner-size font.
**Always safe to call** — it falls back to `purr_win_label_set()` on backends
without it, so the text still appears, just not enlarged.

### Buttons

```c
purr_wid_t purr_win_button       (purr_win_t win, const char *label,
                                   purr_win_cb_t cb, void *user);
void       purr_win_button_enable(purr_wid_t wid, bool enabled);
```

Callback signature:
```c
typedef void (*purr_win_cb_t)(purr_wid_t wid, purr_event_t event, void *user);
// event: PURR_EVENT_CLICKED | CHANGED | FOCUSED | DEFOCUS | SELECTED | ACTIVATED
```

### Textarea

```c
purr_wid_t   purr_win_textarea          (purr_win_t win, uint16_t w_pct, uint16_t h_pct);
void         purr_win_textarea_append   (purr_wid_t wid, const char *text);
void         purr_win_textarea_set      (purr_wid_t wid, const char *text);
void         purr_win_textarea_clear    (purr_wid_t wid);
const char  *purr_win_textarea_get      (purr_wid_t wid);   // backend-owned, copy if needed
void         purr_win_textarea_focus    (purr_wid_t wid);   // show keyboard / cursor
void         purr_win_textarea_on_change(purr_wid_t wid, purr_win_cb_t cb, void *user);
```

`w_pct` and `h_pct` are percentages of the window content area (0–100).

### Lists

```c
purr_wid_t purr_win_list             (purr_win_t win, uint16_t w_pct, uint16_t h_pct);
void       purr_win_list_set_items   (purr_wid_t wid, const char **items, int count);
void       purr_win_list_set_items_icon(purr_wid_t wid, const char **items,
                                         const char **icons, int count);
void       purr_win_list_clear       (purr_wid_t wid);
int        purr_win_list_get_selected(purr_wid_t wid);   // -1 if none
void       purr_win_list_set_selected(purr_wid_t wid, int index);
void       purr_win_list_on_select   (purr_wid_t wid, purr_win_cb_t cb, void *user);
```

The callback receives `PURR_EVENT_SELECTED` (highlight moved) and
`PURR_EVENT_ACTIVATED` (entry confirmed). Backends without real selection
navigation fire both together on tap.

`purr_win_list_set_items_icon()` adds a per-row glyph — an `LV_SYMBOL_*`-style
string, not a bitmap. **Always safe to call**; falls back to the icon-less
version, so rows simply lose their glyphs.

> **Lifetime contract, both variants:** backends may defer the rebuild to their
> next render tick. The arrays you pass must stay valid until then — do not
> build them on the stack of a function that returns immediately.

### Tile grid — *check the return value*

```c
purr_wid_t purr_win_tile_grid         (purr_win_t win, uint16_t w_pct, uint16_t h_pct);
void       purr_win_tile_grid_set_items(purr_wid_t wid,
                                         const char **labels, const char **symbols,
                                         const uint32_t *colors,
                                         purr_win_cb_t *cbs, void **users, int count);
```

Scrollable grid of icon+label tiles, for an app's own internal navigation menu.

**This is the one optional API that needs handling.** It returns `0` on any
backend without tile grids, and only you know what a sensible fallback is for
your content — so the dispatch layer does not guess:

```c
purr_wid_t grid = purr_win_tile_grid(win, 100, 80);
if (grid) {
    purr_win_tile_grid_set_items(grid, labels, symbols, colors, cbs, users, n);
} else {
    purr_wid_t list = purr_win_list(win, 100, 80);
    purr_win_list_set_items(list, labels, n);
}
```

See `source/apps/system/msn/msn.c`'s Home screen for the reference use.

### Layout

```c
purr_wid_t purr_win_row       (purr_win_t win, uint8_t padding);
purr_wid_t purr_win_col       (purr_win_t win, uint8_t padding);
purr_wid_t purr_win_row_grow  (purr_win_t win, uint8_t padding);
purr_wid_t purr_win_col_grow  (purr_win_t win, uint8_t padding);
void       purr_win_layout_end(purr_wid_t container);
```

Widgets created after `purr_win_row()` / `purr_win_col()` and before
`purr_win_layout_end()` are placed inside that container. On LVGL backends this
uses flex layout; on MiniWin, simple vertical stacking.

**Use the `_grow` variants whenever the container holds percentage-sized
children** (a list, a textarea, a split view). A hug-content parent cannot
resolve a percentage-sized child, so those children collapse to 0px — this is
what made File Manager's list+preview split render as nothing.

### Keyboard (on-screen)

```c
void purr_win_keyboard_show(purr_win_t win, purr_wid_t target_textarea);
void purr_win_keyboard_hide(purr_win_t win);
```

On LVGL backends: shows LVGL's built-in keyboard attached to the target
textarea. On MiniWin: no-op — a physical keyboard is handled via
`catcall_input` automatically.

Backends targeting devices with a real keyboard (Cupcake, Tabby, Mochi)
suppress the on-screen keyboard entirely when one is present, rather than
letting it eat a third of a short panel. They detect this by capability — a
keyboard-class input driver implements `set_backlight`, a trackball does not —
not by driver name.

### Canvas — MiniWin only, not portable

```c
void purr_win_canvas_on_paint(purr_win_t win, purr_win_paint_cb_t cb, void *user);
void purr_win_canvas_on_touch(purr_win_t win, purr_win_touch_cb_t cb, void *user);
void purr_win_canvas_rect    (purr_win_t win, int16_t x, int16_t y,
                               int16_t w, int16_t h, uint32_t color);
void purr_win_canvas_text    (purr_win_t win, int16_t x, int16_t y,
                               const char *text, uint32_t color);
void purr_win_canvas_repaint (purr_win_t win);
void purr_win_canvas_size    (purr_win_t win, int16_t *w, int16_t *h);
```

Raw drawing, for widget-dense screens (calculator keypads, grids) that would
otherwise need dozens of native button widgets — MiniWin has a confirmed hang
when tearing down a window holding ~20 of them.

**Only MiniWin implements this, and no app currently uses it.** On every LVGL
backend these are silent no-ops, so a canvas-drawn screen renders *nothing at
all*. Do not adopt it without confirming your target backend, and prefer
ordinary widgets unless you are specifically working around that MiniWin
teardown issue.

Call `canvas_rect`/`canvas_text` only from inside the paint callback, where the
backend has a valid paint context. `canvas_repaint()` requests another paint
asynchronously — it does not draw immediately.

---

## Writing a Backend (for new UI modules)

If you're writing a new UI module (e.g. `oled_ui_win.c` for the text-mode OLED), implement `catcall_ui_t` and call `purr_kernel_register_ui()` from your module's `init()`:

```c
#include "catcall_ui.h"
#include "purr_kernel.h"

static const catcall_ui_t s_my_ui = {
    .name            = "my_ui",
    .catcall_version = CATCALL_UI_VERSION,
    .win_create      = my_win_create,
    .win_destroy     = my_win_destroy,
    // ... all function pointers
};

// Call from your module init():
purr_kernel_register_ui(&s_my_ui);
```

Any function pointer left NULL is a graceful no-op — `purr_win.h` checks before calling.

---

## Built-in System Apps

| App | Tier | File | Description |
|-----|------|------|-------------|
| terminal | `.claw` | `apps/system/terminal/` | Shell: ls, cat, echo, modules, mem, uptime, reboot |
| calculator | `.paws` | `apps/system/calculator/` | Basic arithmetic with decimal support |
| settings | `.claw` | `apps/system/settings/` | Theme, brightness, SD status, system reboot |
| about | `.claw` | `apps/system/about/` | OS/KITT version, chip info, free RAM, uptime, active drivers |
| fileman | `.claw` | `apps/system/fileman/` | Browse SPIFFS and SD card; preview text files |

`settings` and `about` are **staple** apps — always present. The rest follow the same unified API layer and can be excluded from a build if flash space is tight.

---

## File Manager Notes

The file manager (`fileman`) uses a two-panel layout:

```
[ SPIFFS ] [ SD ] [ Up ]
/spiffs
┌──────────────────┬────────────────┐
│ [dir/]           │ file preview   │
│  file.txt        │ (text content) │
│  config.json     │                │
└──────────────────┴────────────────┘
[ < Prev ]  [ Open ]  [ Next > ]
Status: [1/5] config.json
```

Use **Prev** / **Next** to cycle the selection cursor, **Open** to enter a directory or preview a file. Binary files are displayed with non-printable bytes replaced by `.`.

---

## Files

| File | Purpose |
|------|---------|
| `source/kernel/catcalls/catcall_ui.h` | Widget catcall contract — implement this for a new UI backend |
| `source/kernel/catcalls/purr_win.h` | App-facing unified API — include this in your app |
| `source/modules/kittenui/kittenui_win.c` | LVGL backend (small screens) |
| `source/modules/miniwin/miniwin_win.c` | MiniWin backend (large screens) |
| `source/apps/system/terminal/terminal.c` | Terminal app source |
| `source/apps/system/calculator/calculator.c` | Calculator app source |
| `source/apps/system/settings/settings.c` | Settings app source |
| `source/apps/system/about/about.c` | About screen source |
| `source/apps/system/fileman/fileman.c` | File manager source |

---

*DP8 documentation pass performed by Claude Opus 5 in agentic/auto mode. Every
function above was verified to exist in `source/kernel/catcalls/purr_win.h`.*
