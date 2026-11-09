# 15. Adding a second display

**The system supports exactly one display. Adding a second means changing the kernel.** This chapter says why, gives the
smallest set of changes that works, and covers the three situations people actually mean by "a second display".
Read [13](13-writing-drivers.md) and [14](14-adding-a-display.md) first.

**Status.** None of this is built into the repo ([F-03](FINDINGS.md#f-03)). The code below was **written and compiled** in a
scratch copy of the tree (a clean `purrstrap coreos build --board tdeck_plus --profile full`, no new warnings) and is saved
in [examples/kernel/](examples/kernel/). **It has not been run on hardware**, and the SSD1306 init sequence and the example pin
numbers are the common ones, not ones verified on a particular board.

## Why it does not work out of the box

| Obstacle | Where |
|----------|-------|
| The board profile has one `display` and one `keyboard`; there is nowhere to describe a second panel | `purr_board.h` |
| `purr_kernel_init()` brings up exactly one display and `purr_kernel_display()` returns exactly one | `purr_kernel.c` |
| The ST7789 driver keeps all its state in one file-static struct, so a second `purr_st7789_init()` **overwrites the first** | `st7789.c` |
| `purr_display_v2_t`'s functions take **no context pointer**, so one set of functions cannot serve two panels | `purr_display.h` |
| The console (`purr_console_init`) binds to one display and keeps its own state in statics | `purr_console.c` |
| The bus's maximum transfer size is computed from the first display's width | `purr_kernel_init()` |
| The boot menu and the recovery loader draw on the one display only | `bootpkg/`, `recovery_loader.c` |
| Modules and kernelmods cannot draw at all: there is no display entry in either call table | `purr_kernel_table.h` |

## What "second display" can mean

| You want | Difficulty | What it takes |
|----------|-----------|---------------|
| **A. A different kind of second display** (for example a small OLED for status) next to the main one | easy | a second driver with its own state, a profile entry, a kernel getter |
| **B. A second panel of the same controller** (two ST7789s) | medium | per-instance driver state **and** per-instance function tables (no context pointer) |
| **C. The same console on both screens** (mirror) | easy once B or A works, if the sizes match | a "tee" display that forwards each call twice |
| **D. Two independent consoles / a window system** | large | the console and the shell are single-instance; this is the UI phase's job |

The patch described below implements A, B and C. It does not do D.

## The changes

The whole set is `examples/kernel/second-display.patch` plus the two new/replaced driver files. Apply from the repository root
and review it. The pieces:

### 1. Describe the hardware (`purr_board.h`)

```diff
+/* An optional monochrome OLED on the I2C bus the keyboard already uses (addr 0 = none). */
+typedef struct {
+    const char *name;
+    const char *compatible;       /* "ssd1306" */
+    uint8_t addr;                 /* 7-bit address, usually 0x3C; 0 if the board has no OLED */
+    uint16_t width, height;       /* 128 x 64 or 128 x 32 */
+    uint32_t hz;
+} purr_oled_cfg_t;
...
     purr_display_cfg_t display;
+    purr_display_cfg_t display2;  /* a second SPI display on the same bus; width 0 = none */
+    purr_oled_cfg_t oled;         /* an optional I2C OLED; addr 0 = none */
     purr_keyboard_cfg_t keyboard;
```

Designated initializers leave omitted fields zero, so existing board files need no change: `display2.width == 0` and `oled.addr == 0`
mean "not present". A board that has them fills them in (example values, **not** a real T-Deck Plus configuration):

```c
.display2 = {
    .name = "ST7789 240x240", .compatible = "st7789",
    .cs = 5, .dc = 11, .rst = PURR_PIN_NONE, .backlight = PURR_PIN_NONE,
    .width = 240, .height = 240,
    .col_off = 0, .row_off = 80,
    .madctl = 0x00, .bgr = 0, .invert = 1,
    .spi_hz = 40 * 1000 * 1000,
},
.oled = {.name = "SSD1306 128x64", .compatible = "ssd1306", .addr = 0x3C,
         .width = 128, .height = 64, .hz = 400000},
```

A second SPI panel needs its **own chip select**; the data/command pin can be shared or separate. List any chip selects of
idle devices in `idle_high_pins` ([13](13-writing-drivers.md)) so they do not fight the bus.

### 2. Make the ST7789 driver multi-instance (`st7789_multi.c`, replaces `st7789.c`)

Two ideas, both in the file:

- **State per slot.** The old `static struct {...} s;` becomes `st_dev_t g_dev[MAX_SLOTS]`; every helper takes the device pointer.
  The one thing that was implicit, the D/C pin in the SPI `pre_cb`, now travels in the transaction's `user` pointer (a small struct on the
  caller's stack, valid because transactions are blocking).
- **Functions per slot.** `purr_display_v2_t` has no context pointer, so a macro generates a set of wrappers per slot:

  ```c
  #define SLOT_FUNCS(n)                                                                         \
      static esp_err_t get_info_##n(purr_display_info_t *o) { return do_get_info(&g_dev[n], o); } \
      static esp_err_t blit_##n(int x, int y, int w, int h, const uint16_t *p)                  \
      { return do_blit(&g_dev[n], x, y, w, h, p); }                                             \
      static esp_err_t fill_##n(int x, int y, int w, int h, uint16_t c)                         \
      { return do_fill(&g_dev[n], x, y, w, h, c); }                                             \
      static esp_err_t brightness_##n(uint8_t l) { return do_set_brightness(&g_dev[n], l); }
  SLOT_FUNCS(0)
  SLOT_FUNCS(1)
  ```

  Adding a third panel means `SLOT_FUNCS(2)`, one more entry in the table, and `MAX_SLOTS 3`.

  Each slot also gets its own backlight LEDC channel (`LEDC_CHANNEL_2 + slot`).

The new entry point is `purr_st7789_init_slot(slot, cfg, spi_host, &ops)`. The old `purr_st7789_init()` still exists and calls slot 0, so
every existing caller is unchanged.

The honest alternative is to give the display API a context pointer (`blit(void *ctx, ...)`). That is a **major version bump** of the
display interface (the spec's matching rule: major must be equal), touches the console, graphics and boot code, and is the right fix once
a driver registry exists. The macro is the smallest change that works today.

### 3. An OLED driver for case A (`ssd1306.c`)

A different controller on a different bus needs no tricks, since it has its own file-static state. `examples/kernel/ssd1306.c`:

- uses the **I2C bus the kernel already created** for the keyboard (the kernel now keeps that handle: `s_i2c_bus`) and adds a device at the
  OLED's address,
- keeps a private 1 KB framebuffer (128 x 64, one bit per pixel, page-major) because the panel cannot be written a pixel at a time,
- converts each RGB565 pixel to on or off by brightness (`r + g/2 + b` over about a quarter), so `purr_gfx_text` with a white foreground on
  black just works,
- sends only the pages a draw touched.

It is a `purr_display_v2_t` like any other, so `purr_gfx_text`, `fill` and `blit` work on it unchanged.

### 4. Bring them up and expose them (`purr_kernel.c`, `purr_kernel.h`)

After the main display comes up, `purr_kernel_init()` tries the extras. **A failure is logged and not fatal**: the console display is what the
system needs.

```c
if (b->display2.width != 0) {
    esp_err_t e2 = purr_st7789_init_slot(1, &b->display2, b->spi.host, &s_display2);
    if (e2 != ESP_OK) { s_display2 = NULL; ESP_LOGW(TAG, "second display did not come up: %s", esp_err_to_name(e2)); }
}
if (b->oled.addr != 0 && s_i2c_bus != NULL) {
    esp_err_t e3 = purr_ssd1306_init(s_i2c_bus, &b->oled, &s_oled);
    if (e3 != ESP_OK) { s_oled = NULL; ESP_LOGW(TAG, "OLED did not come up: %s", esp_err_to_name(e3)); }
}
```

and two getters, `purr_kernel_display2()` and `purr_kernel_oled()`, return the handle or `NULL`. The SPI bus's maximum transfer size now uses the wider
of the two panels. The sizes matter: `max_transfer_sz = max(width) * 16 * 2`.

### 5. Use them

In `PurrOS/main/main.c`, after `purr_console_init(d)` (this is in the patch):

```c
const purr_display_v2_t *d2 = purr_kernel_display2();
if (d2 != NULL) {
    purr_gfx_text(d2, 8, 8, "PURR OS: display 2", PURR_RGB565(255, 180, 0), 0, 2);
}
const purr_display_v2_t *oled = purr_kernel_oled();
if (oled != NULL) {
    purr_gfx_text(oled, 0, 0, "PURR OS", 0xFFFF, 0, 2);
}
```

`purr_gfx_text(d, x, y, string, fg, bg, scale)` draws one line with the 8x8 font on any display. The 128x64 OLED holds 16 characters per line at
scale 1 and 8 at scale 2.

For a **live status** (uptime, IP address) create a small FreeRTOS task in the kernel or in `main.c` that wakes once a second and redraws a line;
there is no framework for that yet. Call the draw functions only from one task per display ([13](13-writing-drivers.md): no bus lock).

### 6. Mirroring the console (case C)

If both panels have the **same width and height**, the console can drive both. `purr_display_tee(a, b)` (in the patch, in `purr_gfx.c`) returns a
display whose `blit` and `fill` forward to both:

```c
const purr_display_v2_t *mirror = purr_display_tee(purr_kernel_display(), purr_kernel_display2());
purr_console_init(mirror != NULL ? mirror : purr_kernel_display());
```

Limits: only one tee at a time (no context pointer again); sizes must match exactly or it returns `NULL`; brightness stays per panel; both
panels are written one after the other, so drawing takes twice as long. A mismatch is not scaled.

## Boot menu, recovery loader, KittenOS

- The **boot menu** draws on the one panel its own `pkg_hw.c` code is written for ([14](14-adding-a-display.md)). A second display stays blank until the
  kernel starts. That is fine for most uses.
- **KittenOS** and the **recovery loader** run the same kernel code, so they get the same extra displays if the profile has them. Their screens
  (`purr_console` text) appear only on the main display unless you mirror.

## Modules and kernelmods cannot draw

Neither call table has a display entry, so a command cannot draw on a second screen. Two ways, both extending the kernel table (and bumping
`PURR_KERNEL_TABLE_ABI_VERSION`, [12](12-writing-modules.md)):

- an opaque `display2_text(x, y, string)` call that does the drawing kernel-side (the pattern the existing tables follow), or
- `display2_blit` and `display2_fill` forwarding calls.

Prefer the first: a module is never handed raw drawing or hardware access.

## Checking it

1. Serial log: `st7789: slot 0: ST7789 320x240 up, 320x240`, then `st7789: slot 1: ST7789 240x240 up, 240x240` and/or
   `ssd1306: SSD1306 128x64 up, 128x64`.
2. The main console works exactly as before. If it does not, the refactor broke slot 0: compare `st7789_multi.c` with the original.
3. The second panel shows its label. A blank panel with a clean log is usually the backlight, a chip-select clash or `col_off`/`row_off`
   ([14](14-adding-a-display.md) troubleshooting table).
4. If the SPI bus is shared, make sure LoRa and SD chip selects are in `idle_high_pins` or the bus misbehaves.

## What this chapter does not give you

Touch on either panel, two consoles with their own shells, a windowing layer, per-display brightness UI, power management of the extras, hot-plug,
and a driver registry that would make all of this a profile entry plus a driver file instead of five edited files. Those are the `Catcalls`,
`UI` and kernel driver specs ([24](24-not-built-yet.md)).

Next: [16 Adding a board](16-adding-a-board.md).
