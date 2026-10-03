# 14. Adding a display

How to get the system's text console onto a different screen: a different panel on the same controller, or a different
controller altogether. Read [13](13-writing-drivers.md) first for the structure (board profile, display contract, kernel
bring-up). For **two** displays at once see [15](15-adding-a-second-display.md).

**Status.** The T-Deck Plus ST7789 is **[WORKS]**. The ILI9341 driver below was written and **compiled** into the real build
in a scratch copy of the tree; it has **not been run on any panel**, and its init table is the widely used Adafruit
sequence, not something verified on a CYD. Everything else (e-paper, OLED, RGB, touch) is **[DESIGNED]** or absent.

## What uses the display

| Consumer | Display code | Notes |
|----------|--------------|-------|
| PURR OS, KittenOS, the recovery loader | the kernel's `purr_display_v2_t` driver + `purr_console` | one driver serves all three profiles |
| **The boot menu** | **its own, separate** bit-banged ST7789 code in `bootpkg/src/pkg_hw.c`, pins in `bootpkg/boards/<board>.h` | a second copy of the pins and the init sequence ([F-02](FINDINGS.md#f-02)) |
| the bootloader | none | it never draws |

So a new screen means changing the kernel driver **and**, if you want a boot menu, the boot package.

## Case A: a different panel on the same controller (ST7789)

Only data changes: edit `components/kernel/boards/<board>.c`, the `display` block. Every field of `purr_display_cfg_t`:

| Field | What it does | Typical values and symptoms |
|-------|--------------|-----------------------------|
| `cs`, `dc`, `rst`, `backlight` | GPIOs; `PURR_PIN_NONE` (-1) if absent | no `rst` means no hardware reset pulse (the driver still sends a software reset) |
| `width`, `height` | pixels, **in the orientation you use** | the console size follows from these |
| `col_off`, `row_off` | start of the visible area inside the controller's RAM | an image shifted by a few pixels, or a bright line at one edge, means these are wrong. Common for 240x240 and 135x240 ST7789 panels. |
| `madctl` | memory access control byte **without** the colour-order bit | bit 7 MY (flip rows), bit 6 MX (flip columns), bit 5 MV (swap x and y), bit 4 ML, bit 2 MH. T-Deck Plus: `0x70` = MX, MY, MV. Mirrored or rotated output means change this. |
| `bgr` | `1` if the panel is wired blue-green-red | red and blue swapped on screen means flip this. The driver ORs in bit 3. |
| `invert` | `1` to send the display-inversion command | colours that look like a negative image (black background white) mean flip this |
| `spi_hz` | SPI clock | 40 MHz works on the T-Deck. Speckle, shifted lines or garbage often mean too fast; try 20 or 10 MHz. |

Also check `power_pin`, `idle_high_pins` and `spi` for the board ([13](13-writing-drivers.md)). Then rebuild and flash the full profile.
The **backlight** is brought up with LEDC and set to full by `main.c` (`set_brightness(255)`); a backlight that is active-low
needs a driver change.

## Case B: a different controller (worked example: ILI9341)

Most small SPI TFTs share the command set the ST7789 driver uses (`CASET 2A`, `RASET 2B`, `RAMWR 2C`, `MADCTL 36`, `COLMOD 3A`,
`SLPOUT 11`, `DISPON 29`). Only the **init sequence** and a few details differ, which is why the driver keeps the sequence as
data. Steps, with real diffs:

### 1. Copy the driver

`cp PurrOS/components/kernel/src/st7789.c PurrOS/components/kernel/src/ili9341.c`, then change it. The complete difference from
`st7789.c` that was compiled:

```diff
-static const char *TAG = "st7789";
+static const char *TAG = "ili9341";
...
-    uint8_t data[14];
+    uint8_t data[16];                       /* ILI9341's gamma tables are 15 bytes */
 } step_t;

-#define MADCTL_STEP_INDEX 3                  /* the step that takes its argument from the profile */
-
 static const step_t s_init[] = {
     {0x01, 150, 0, {0}},                                          /* software reset */
-    ...the other 14 ST7789 steps (sleep out, porch, gate, VCOM, power, gamma) removed...
+    {0xEF, 0, 3, {0x03, 0x80, 0x02}},
+    {0xCF, 0, 3, {0x00, 0xC1, 0x30}},
+    {0xED, 0, 4, {0x64, 0x03, 0x12, 0x81}},
+    {0xE8, 0, 3, {0x85, 0x00, 0x78}},
+    {0xCB, 0, 5, {0x39, 0x2C, 0x00, 0x34, 0x02}},
+    {0xF7, 0, 1, {0x20}},
+    {0xEA, 0, 2, {0x00, 0x00}},
+    {0xC0, 0, 1, {0x23}},                                         /* power control 1 */
+    {0xC1, 0, 1, {0x10}},                                         /* power control 2 */
+    {0xC5, 0, 2, {0x3E, 0x28}},                                   /* VCOM 1 */
+    {0xC7, 0, 1, {0x86}},                                         /* VCOM 2 */
+    {CMD_MADCTL, 0, 1, {0}},                                      /* orientation: from the profile */
+    {0x3A, 0, 1, {0x55}},                                         /* 16 bits per pixel */
+    {0xB1, 0, 2, {0x00, 0x18}},                                   /* frame rate */
+    {0xB6, 0, 3, {0x08, 0x82, 0x27}},                             /* display function control */
+    {0xF2, 0, 1, {0x00}},                                         /* 3-gamma off */
+    {0x26, 0, 1, {0x01}},                                         /* gamma curve 1 */
+    {0xE0, 0, 15, {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
+                   0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}},    /* positive gamma */
+    {0xE1, 0, 15, {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
+                   0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}},    /* negative gamma */
+    {CMD_SLPOUT, 120, 0, {0}},                                    /* sleep out, then wait 120 ms */
 };
...
-esp_err_t purr_st7789_init(const purr_display_cfg_t *cfg, int spi_host, const purr_display_v2_t **out)
+esp_err_t purr_ili9341_init(const purr_display_cfg_t *cfg, int spi_host, const purr_display_v2_t **out)
...
-        uint8_t data[14];
+        uint8_t data[16];
         memcpy(data, s_init[i].data, sizeof(data));
-        if (i == MADCTL_STEP_INDEX) {
+        if (s_init[i].cmd == CMD_MADCTL) {       /* find the orientation step by command, not position */
             data[0] = cfg->madctl | (cfg->bgr ? 0x08 : 0x00);   /* bit 3 = BGR order */
         }
```

Everything else (clipping, endian swap, chunked DMA, backlight) is identical, which is the point: only the table and the name
changed. Two things to check on your own panel: whether it needs `INVON` (set `invert`), and whether it needs the longer
`SLPOUT` delay (it is 120 ms here).

### 2. Register and select it

`PurrOS/components/kernel/CMakeLists.txt`:

```diff
     "src/st7789.c"
+    "src/ili9341.c"
```

`PurrOS/components/kernel/include/purr_kernel.h`:

```diff
+esp_err_t purr_ili9341_init(const purr_display_cfg_t *cfg, int spi_host,
+                            const purr_display_v2_t **out);
```

`PurrOS/components/kernel/src/purr_kernel.c`: add `#include <string.h>`, replace the hard-wired call with a small dispatch on the
profile's `compatible` string (which nothing read until now):

```diff
+/* Picks the display driver named by the board profile's `compatible` string. */
+static esp_err_t start_display(const purr_board_t *b, const purr_display_v2_t **out)
+{
+    const char *drv = b->display.compatible;
+    if (drv != NULL && strcmp(drv, "st7789") == 0) {
+        return purr_st7789_init(&b->display, b->spi.host, out);
+    }
+    if (drv != NULL && strcmp(drv, "ili9341") == 0) {
+        return purr_ili9341_init(&b->display, b->spi.host, out);
+    }
+    ESP_LOGE(TAG, "no display driver for \"%s\"", drv ? drv : "(none)");
+    return ESP_ERR_NOT_FOUND;
+}
...
-    e = purr_st7789_init(&b->display, b->spi.host, &s_display);
+    e = start_display(b, &s_display);
```

Build result: `[ok] build finished`, no new warnings (the one `TAG unused` warning in `recovery_loader.c` is pre-existing).

### 3. The profile

An illustration only (the pins are from the CYD 2.4C notes in `kernel/SPEC.md`; the spec itself says "to be confirmed on the board"):

```c
.display = {
    .name = "ILI9341 240x320", .compatible = "ili9341",
    .cs = 15, .dc = 2, .rst = PURR_PIN_NONE, .backlight = 27,
    .width = 240, .height = 320,
    .col_off = 0, .row_off = 0,
    .madctl = 0x00,            /* portrait; try 0x20 / 0x60 / 0xA0 / 0xE0 for the others */
    .bgr = 1, .invert = 0,
    .spi_hz = 40 * 1000 * 1000,
},
```

A CYD is an original ESP32 with no PSRAM, which the current build does not support at all (no PURR board entry, no partition
table, no sdkconfig, [F-01](FINDINGS.md#f-01)). The profile above is useful on a PSRAM board with an ILI9341 panel. The rest of the
board work is in [16](16-adding-a-board.md).

## The boot menu's own display code

`bootpkg/src/pkg_hw.c` drives the panel by writing GPIO registers directly (so the boot package needs no drivers). It is hard-wired
to an **ST7789**: `hw_display_init()` contains its own copy of the init table, and the pins and geometry come from
`bootpkg/boards/tdeck_plus.h` (`PIN_*`, `LCD_W`, `LCD_H`, `LCD_MADCTL`, `LCD_BGR`, `LCD_INVERT`).

- **Same controller, new pins or size:** edit `bootpkg/boards/<board>.h` (and add the board to `BOARDS` in
  `purrstrap/scripts/bootpkg.py`, which also names the linker script). Rebuild and re-sign the package.
- **Different controller:** replace the table in `hw_display_init()` and the `lcd_*` helpers' assumptions. It must stay free of libc
  and fit in 32 KB of code.
- **Do nothing:** if the package cannot draw, the boot still works. The bootloader loads it, the menu draws garbage or nothing,
  and the countdown boots the default slot. Worst case you cannot reach the menu to choose KittenOS.
- The package is only built for the **ESP32-S3** (its RAM window is S3-specific, [F-21](FINDINGS.md#f-21)). On an original ESP32 the
  bootloader logs `bootpkg: no RAM window defined for this chip yet` and boots without a menu.

The pin tables in the kernel profile and the boot-package header are **independent copies**. Change both.

## The console on your new screen

`purr_console_init(d)` sizes itself from `get_info()`:

- cell = 8 by 9 pixels (an 8x8 font plus a one-pixel gap), white on black, cursor an orange underline,
- columns = `width / 8`, rows = `height / 9`, **clamped to 64 columns and 40 rows** (`purr_term.h`).

| Panel | Console |
|-------|---------|
| 320 x 240 (T-Deck Plus) | 40 x 26 |
| 240 x 320 portrait | 30 x 35 |
| 480 x 320 | 60 x 35 |
| 800 x 480 | **64 x 40**, so about a quarter of the pixels stay black |
| 128 x 64 monochrome OLED | 16 x 7 (and see [15](15-adding-a-second-display.md)) |

Larger panels need a larger `PURR_TERM_MAX_COLS` and `PURR_TERM_MAX_ROWS` (the grid is a static array: 64 x 41 bytes per row
times 40 rows, about 2.6 KB, plus the dirty flags), and a scaled font in `purr_console.c` (`CELL_W`, `ROW_H` are constants).

## Bring-up checklist

Do these in order; each isolates one thing.

1. **Serial log first.** You should see `kernel: board: <name>`, `peripheral power on (GPIO n)`, and the driver's line,
   for example `st7789: ST7789 320x240 up, 320x240`. `display did not come up: <error>` means the SPI device or init failed.
2. **Power rail.** If nothing answers at all, the `power_pin` is wrong or missing. On the T-Deck it must be high first.
3. **Backlight.** A dark screen with the log saying "up" is usually the backlight pin or polarity.
4. **A test pattern.** There is no test pattern app ([DESIGNED]). Add this to `purr_gfx.c` (declared in `purr_gfx.h`); it was
   compiled in the scratch build:

   ```c
   void purr_display_selftest(const purr_display_v2_t *d)
   {
       static const uint16_t bars[8] = {
           PURR_RGB565(255, 255, 255), PURR_RGB565(255, 255, 0), PURR_RGB565(0, 255, 255),
           PURR_RGB565(0, 255, 0),     PURR_RGB565(255, 0, 255), PURR_RGB565(255, 0, 0),
           PURR_RGB565(0, 0, 255),     PURR_RGB565(0, 0, 0),
       };
       purr_display_info_t info;
       d->get_info(&info);
       int bw = info.width / 8;
       for (int i = 0; i < 8; i++) {
           d->fill(i * bw, 0, i == 7 ? info.width - 7 * bw : bw, info.height, bars[i]);
       }
       uint16_t white = PURR_RGB565(255, 255, 255);
       d->fill(0, 0, info.width, 1, white);
       d->fill(0, info.height - 1, info.width, 1, white);
       d->fill(0, 0, 1, info.height, white);
       d->fill(info.width - 1, 0, 1, info.height, white);
   }
   ```

   Call it from `app_main()` right after `purr_kernel_display()` succeeds, with a `vTaskDelay(pdMS_TO_TICKS(5000))` after it,
   and remove it when done.
5. **Read the pattern:**

   | You see | Fix |
   |---------|-----|
   | bars in the wrong order (right to left) | `madctl` MX bit |
   | upside down | `madctl` MY bit |
   | rotated or the bars are horizontal | `madctl` MV bit and swap `width` and `height` |
   | red and blue swapped | `bgr` |
   | inverted colours | `invert` |
   | 1 to 3 pixel shift, a coloured edge | `col_off` and `row_off` |
   | speckle or torn lines | lower `spi_hz` |
   | border not complete | wrong `width` or `height` |

6. Boot into the shell and check text is crisp and the cursor row is right.

## Not covered

Touch (CST816S and GT911), e-paper refresh policy, MIPI/RGB parallel panels, hardware rotation at run time, PWM polarity, and any
display without `fill` and `blit` semantics. The e-paper plan is in `Boards/SPEC.md` (the driver "manages refreshing" and the
display contract gains optional `refresh_now` and `set_refresh_mode`); none of it exists.

Next: [15 Adding a second display](15-adding-a-second-display.md).
