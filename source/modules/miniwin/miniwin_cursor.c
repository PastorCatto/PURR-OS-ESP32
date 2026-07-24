// miniwin_cursor.c — trackball-driven cursor overlay for MiniWin
//
// Polls all registered input catcalls each tick. Trackball pointer events move
// the cursor; trackball click synthesizes a touch press at cursor position
// (read by hal_touch). Real screen touch hides the cursor automatically.
//
// Cursor is drawn directly via the display catcall after MiniWin repaints,
// so it always sits on top. No framebuffer readback needed — MiniWin will
// overdraw it on the next repaint, which is fine at ~50 fps.
//
// Arrow sprite (12x16): drawn with fill_rect calls in the foreground color.

#include "miniwin_cursor.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_input.h"
#include "../../kernel/catcalls/catcall_display.h"
#include "../../kernel/catcalls/catcall_touch.h"
#include "esp_log.h"

static const char *TAG = "cursor";

// TEMP: trackball cursor disabled while debugging touch calibration. The
// cursor's synthetic-touch fallback (trackball click -> "touch" at cursor
// position, consumed by hal_touch.c) means trackball click noise/bounce
// can register as a phantom tap wherever the cursor happens to be, which
// was confusing touch calibration ("mouse doing its own thing"). Flip back
// to 1 once touch calibration is solid.
#define CURSOR_ENABLED 0

// ── State ─────────────────────────────────────────────────────────────────────

static int     s_w = 320, s_h = 240;
static int     s_x = 160, s_y = 120;
static bool    s_pressed  = false;   // trackball click active
static bool    s_visible  = false;   // false = real touch hiding cursor
static bool    s_dirty    = true;    // needs redraw

// Arrow cursor drawn with fill_rect calls — tip at (x,y), ~11x16px.
static void draw_cursor(int x, int y, uint16_t color)
{
    const catcall_display_t *d = purr_kernel_display();
    if (!d || !d->fill_rect) return;

    // Arrow pointing up-left, tip at (x,y), 11w x 16h
    // Column 0 (left edge) — full height
    d->fill_rect(x,     y,      1, 16, color);
    // Row 0 (top edge) — 11 wide
    d->fill_rect(x,     y,      11, 1, color);
    // Diagonal: from (x+10, y+1) down to (x+1, y+10)
    for (int i = 1; i <= 10; i++) {
        d->fill_rect(x + (11 - i), y + i, 1, 1, color);
    }
    // Fill interior darker — draw a smaller filled triangle (inner solid)
    // Row 1..9: fill from x+1 to diagonal
    for (int r = 1; r <= 9; r++) {
        int right = 11 - r;  // diagonal x at this row
        if (right > 1) {
            d->fill_rect(x + 1, y + r, right - 1, 1, color);
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

bool miniwin_cursor_enabled(void) { return CURSOR_ENABLED; }

bool miniwin_cursor_handle_event(const input_event_t *ev)
{
#if !CURSOR_ENABLED
    // Still "consume" pointer/click events so they don't fall through to
    // keyboard routing as garbage key codes — just don't act on them.
    if (ev->type == INPUT_EVENT_POINTER) return true;
    if (ev->type == INPUT_EVENT_KEY_DOWN && ev->keycode == 0x0028) return true;
    if (ev->type == INPUT_EVENT_KEY_UP && ev->keycode == 0x0028) return true;
    return false;
#endif
    const catcall_touch_t *touch = purr_kernel_touch();
    bool real_touch = touch && touch->is_pressed();

    if (ev->type == INPUT_EVENT_POINTER && !real_touch) {
        int nx = s_x + ev->delta_x;
        int ny = s_y + ev->delta_y;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx >= s_w) nx = s_w - 1;
        if (ny >= s_h) ny = s_h - 1;
        if (nx != s_x || ny != s_y) {
            s_x = nx; s_y = ny;
            s_visible = true;
            s_dirty   = true;
        }
        return true;
    }
    if (ev->type == INPUT_EVENT_KEY_DOWN && ev->keycode == 0x0028) {
        if (!real_touch) { s_pressed = true; s_visible = true; }
        return true;
    }
    if (ev->type == INPUT_EVENT_KEY_UP && ev->keycode == 0x0028) {
        s_pressed = false;
        return true;
    }
    return false;  // not consumed — let keyboard handler see it
}

void miniwin_cursor_init(int display_w, int display_h)
{
    s_w = display_w;
    s_h = display_h;
    s_x = display_w / 2;
    s_y = display_h / 2;
    s_dirty = true;
    ESP_LOGI(TAG, "cursor init %dx%d, start (%d,%d)", s_w, s_h, s_x, s_y);
}

void miniwin_cursor_poll(void)
{
#if !CURSOR_ENABLED
    return; // disabled — see CURSOR_ENABLED comment above
#endif
    // Check real touch — if active, hide cursor
    const catcall_touch_t *touch = purr_kernel_touch();
    bool real_touch = touch && touch->is_pressed();
    if (real_touch) {
        if (s_visible) {
            s_visible = false;
            s_dirty   = true;
        }
        s_pressed = false;
    }

    // Input events are drained by miniwin_input_pump() which calls
    // miniwin_cursor_handle_event() for each event before routing to keyboard.
    // Nothing to drain here — poll() just redraws if dirty.

    // Redraw every tick while visible, not just after a position change —
    // MiniWin's own periodic repaints draw straight over the cursor sprite
    // (it isn't a tracked window), so without this it visually vanishes
    // every time the background gets redrawn until the cursor moves again.
    if (s_visible && !real_touch) {
        s_dirty = true;
    }

    // Draw cursor on top of MiniWin frame if dirty and visible
    if (s_dirty && s_visible && !real_touch) {
        // Black outline (draw 1px larger in black)
        draw_cursor(s_x - 1, s_y - 1, 0x0000);
        // White fill
        draw_cursor(s_x, s_y, 0xFFFF);
        s_dirty = false;
    }
}

uint16_t miniwin_cursor_x(void)       { return (uint16_t)s_x; }
uint16_t miniwin_cursor_y(void)       { return (uint16_t)s_y; }
bool     miniwin_cursor_pressed(void) {
#if !CURSOR_ENABLED
    return false; // disabled — see CURSOR_ENABLED comment above
#else
    return s_pressed;
#endif
}
bool     miniwin_cursor_visible(void) { return s_visible; }
