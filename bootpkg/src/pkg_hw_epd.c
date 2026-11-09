/*
 * pkg_hw_epd.c - the hardware the boot menu needs on the Waveshare ESP32-S3-ePaper-1.54
 * ("InkyTuxedo"): an SSD1681-family e-paper panel driven by bit-banged SPI, and two plain
 * buttons (BOOT, PWR) instead of a keyboard. Implements the same pkg_hw.h interface pkg_hw.c
 * (the T-Deck Plus's ST7789 + I2C keyboard) does, so pkg_main.c needs no board-specific code
 * beyond the small BOARD_HAS_KEYBOARD branches it already has.
 *
 * Protocol -- command bytes, LUTs, BUSY/PWR polarity, partial-refresh ghosting mitigation --
 * ported from PurrOS/components/kernel/src/epd1in54.c (itself ported from the archive's
 * epd1in54.c, a faithful C port of Waveshare's own vendor BSP). Same duplication-between-
 * kernel-and-bootpkg tradeoff pkg_hw.c's own ST7789 copy already accepts (F-02's own note on
 * this) -- bootpkg cannot link the kernel's driver (no driver-module loading into the boot
 * package yet), so the protocol bytes live twice, not shared code.
 *
 * -- Why hw_fill()/hw_text() don't refresh immediately --
 * A refresh here is ~0.3s (partial) to ~2s (full). pkg_main.c's own draw_* functions call
 * hw_fill()/hw_text() several times each (a background fill plus one or more glyphs); doing
 * a full hardware refresh per call would mean several seconds per screen, the exact mistake
 * the archive's own epaper_ui.c top comment documents measuring (one refresh per glyph,
 * ~58s for three short lines). Instead, both functions draw into a local shadow buffer and
 * pkg_main.c calls hw_flush() once at the end of each logical screen update -- one refresh
 * per screen, not per draw call.
 *
 * -- Invert --
 * EPD_INVERT below flips the black/white threshold panel-wide, same reasoning and same
 * constant name as the kernel's own epd1in54.c: pkg_main.c's menu draws a mostly-black
 * screen (BLACK fills with WHITE/ORANGE text and borders), which without this would flip
 * most of the panel's pixels on every refresh -- costlier on this panel than a mostly-white
 * screen with only the text/border pixels flipped. pkg_main.c's own colors are unchanged;
 * only the panel's drive direction is.
 */
#include "pkg_hw.h"

#include "board.h"
#include "purr_font.h"

/* ------------------------------------------------------------ GPIO (ESP32-S3) */

#define GPIO_BASE 0x60004000u
#define REG(off) (*(volatile uint32_t *)(GPIO_BASE + (off)))

static inline void pin_hi(int p) { if (p < 32) REG(0x08) = 1u << p; else REG(0x14) = 1u << (p - 32); }
static inline void pin_lo(int p) { if (p < 32) REG(0x0C) = 1u << p; else REG(0x18) = 1u << (p - 32); }
static inline int pin_read(int p)
{
    return p < 32 ? (int)((REG(0x3C) >> p) & 1u) : (int)((REG(0x40) >> (p - 32)) & 1u);
}

/* ------------------------------------------------------------ eFuse (read-only) */
/* Same bit pkg_hw.c's own copy reads (chip-level, not board-specific); duplicated rather
 * than shared for the same reason the rest of this file is -- see this file's top comment. */
#define EFUSE_BASE 0x60007000u
#define EFUSE_RD_REPEAT_DATA2 (*(volatile uint32_t *)(EFUSE_BASE + 0x38))
#define EFUSE_SECURE_BOOT_EN_BIT (1u << 20)

int hw_secure_boot_enabled(void)
{
    return (EFUSE_RD_REPEAT_DATA2 & EFUSE_SECURE_BOOT_EN_BIT) != 0;
}

static const purr_boot_services_t *g;

void hw_delay_ms(uint32_t ms) { while (ms--) g->delay_us(1000); }

/* ------------------------------------------------------------ SPI, bit-banged */

static inline void spi_byte(uint8_t v)
{
    for (int i = 7; i >= 0; i--) {
        pin_lo(PIN_SCLK);
        if ((v >> i) & 1) pin_hi(PIN_MOSI); else pin_lo(PIN_MOSI);
        pin_hi(PIN_SCLK);
    }
}

static void epd_cmd(uint8_t c)
{
    pin_lo(PIN_DC); pin_lo(PIN_CS);
    spi_byte(c);
    pin_hi(PIN_CS);
}

static void epd_data(uint8_t d)
{
    pin_hi(PIN_DC); pin_lo(PIN_CS);
    spi_byte(d);
    pin_hi(PIN_CS);
}

static void epd_data_buf(const uint8_t *buf, int n)
{
    pin_hi(PIN_DC); pin_lo(PIN_CS);
    for (int i = 0; i < n; i++) spi_byte(buf[i]);
    pin_hi(PIN_CS);
}

/* BUSY is active HIGH (confirmed against the vendor BSP). A ceiling, not an unbounded spin --
 * see epd1in54.c's own identical comment. */
static void epd_wait_busy(void)
{
    int waited_ms = 0;
    while (pin_read(PIN_BUSY) == 1) {
        hw_delay_ms(5);
        waited_ms += 5;
        if (waited_ms > 5000) {
            break;
        }
    }
}

/* Full-refresh LUT -- ported byte-for-byte, same bytes as epd1in54.c's WF_FULL_1IN54. */
static const uint8_t WF_FULL_1IN54[159] = {
    0x80,0x48,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x40,0x48,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x80,0x48,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x40,0x48,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0A,0x00,0x00,0x00,0x00,0x00,0x00,
    0x08,0x01,0x00,0x08,0x01,0x00,0x02,
    0x0A,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x22,0x22,0x22,0x22,0x22,0x22,0x00,0x00,0x00,
    0x22,0x17,0x41,0x00,0x32,0x20,
};

/* Partial-refresh LUT -- ported byte-for-byte, same bytes as epd1in54.c's WF_PARTIAL_1IN54. */
static const uint8_t WF_PARTIAL_1IN54[159] = {
    0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x80,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x40,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0F,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x01,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x22,0x22,0x22,0x22,0x22,0x22,0x00,0x00,0x00,
    0x02,0x17,0x41,0xB0,0x32,0x28,
};

/* How many partial refreshes before a full-refresh baseline clears accumulated ghosting --
 * same measured value as epd1in54.c, which runs continuously; this boot menu is on screen
 * far more briefly, but the same constant is the right default until real use says otherwise. */
#define FULL_REFRESH_EVERY 10

#define EPD_STRIDE (LCD_W / 8)
#define EPD_BUF_LEN (EPD_STRIDE * LCD_H)

static uint8_t s_fb[EPD_BUF_LEN];     /* 1bpp shadow buffer -- bit=1 WHITE, bit=0 BLACK */
static int s_partial_mode;
static int s_partial_count;

static void epd_set_lut(const uint8_t *lut)
{
    epd_cmd(0x32);
    epd_data_buf(lut, 153);
    epd_wait_busy();

    epd_cmd(0x3F); epd_data(lut[153]);
    epd_cmd(0x03); epd_data(lut[154]);
    epd_cmd(0x04);
    epd_data(lut[155]); epd_data(lut[156]); epd_data(lut[157]);
    epd_cmd(0x2C); epd_data(lut[158]);
}

static void epd_hw_reset(void)
{
    pin_hi(PIN_RST); hw_delay_ms(50);
    pin_lo(PIN_RST); hw_delay_ms(20);
    pin_hi(PIN_RST); hw_delay_ms(50);
}

int hw_display_init(void)
{
    pin_hi(PIN_CS);
    pin_lo(PIN_EPD_PWR);   /* active LOW -- must be on before anything else talks to the panel */
    hw_delay_ms(10);

    epd_hw_reset();
    epd_wait_busy();

    epd_cmd(0x12);   /* SWRESET */
    epd_wait_busy();

    epd_cmd(0x01);   /* driver output control */
    epd_data(0xC7); epd_data(0x00); epd_data(0x01);

    epd_cmd(0x11);   /* data entry mode */
    epd_data(0x01);

    epd_cmd(0x44); epd_data(0x00); epd_data((LCD_W - 1) >> 3);
    epd_cmd(0x45);
    epd_data((LCD_H - 1) & 0xFF); epd_data(((LCD_H - 1) >> 8) & 0xFF);
    epd_data(0x00); epd_data(0x00);

    epd_cmd(0x3C); epd_data(0x01);     /* border waveform */
    epd_cmd(0x18); epd_data(0x80);     /* internal temperature sensor */
    epd_cmd(0x22); epd_data(0xB1);     /* load temperature + waveform setting */
    epd_cmd(0x20);

    epd_cmd(0x4E); epd_data(0x00);
    epd_cmd(0x4F); epd_data((LCD_H - 1) & 0xFF); epd_data(((LCD_H - 1) >> 8) & 0xFF);
    epd_wait_busy();

    epd_set_lut(WF_FULL_1IN54);

    for (int i = 0; i < EPD_BUF_LEN; i++) s_fb[i] = 0xFF;   /* all-white known state */

    /* Seed both RAM banks with one full activation -- the ghost-free baseline every future
     * partial refresh compares against. */
    epd_cmd(0x24); epd_data_buf(s_fb, EPD_BUF_LEN);
    epd_cmd(0x26); epd_data_buf(s_fb, EPD_BUF_LEN);
    epd_cmd(0x22); epd_data(0xC7);
    epd_cmd(0x20);
    epd_wait_busy();

    epd_hw_reset();
    epd_wait_busy();
    epd_set_lut(WF_PARTIAL_1IN54);
    epd_cmd(0x37);
    {
        static const uint8_t cfg37[10] = {0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x00,0x00,0x00};
        epd_data_buf(cfg37, sizeof(cfg37));
    }
    epd_cmd(0x3C); epd_data(0x80);
    epd_cmd(0x22); epd_data(0xC0);
    epd_cmd(0x20);
    epd_wait_busy();

    s_partial_mode = 1;
    s_partial_count = 0;
    return 0;
}

/* ------------------------------------------------------------ pixel buffer / drawing */

#define EPD_INVERT 1

static inline void set_pixel(int x, int y, int white)
{
    if ((unsigned)x >= (unsigned)LCD_W || (unsigned)y >= (unsigned)LCD_H) return;
    uint8_t *byte = &s_fb[y * EPD_STRIDE + (x >> 3)];
    uint8_t bit = (uint8_t)(1 << (7 - (x & 7)));
    if (white) *byte |= bit; else *byte &= (uint8_t)~bit;
}

void hw_fill(int x, int y, int w, int h, uint16_t c)
{
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > LCD_W || y + h > LCD_H) return;
    int white = (c != 0x0000) ^ EPD_INVERT;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            set_pixel(x + col, y + row, white);
        }
    }
}

int hw_text_width(const char *s, int scale)
{
    int n = 0;
    while (s[n]) n++;
    return n * PURR_FONT_W * scale;
}

/* Thresholded the same way epd1in54.c's blit() is: fg/bg are RGB565 colors from pkg_main.c's
 * palette (BLACK/WHITE/GREY/ORANGE), but this panel only ever shows two of them -- anything
 * non-zero reads as WHITE, same convention as every other text-drawing path in this project. */
void hw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    int n = 0;
    while (s[n]) n++;
    if (n == 0) return;
    /* No whole-string bounds bail-out: a label sized for a wider panel (T-Deck Plus, 320px)
     * used to vanish ENTIRELY on this 200px one instead of just running off the edge ("Boot
     * kittenos" at scale 2 alone is 208px, wider than the whole screen) -- found 2026-10-03,
     * reported as the boot option text being blank. set_pixel() already clips every pixel
     * to the real panel bounds (and already handles a negative x/y correctly: cast to
     * unsigned, which makes the >= comparison reject it same as too-large), so letting it do
     * that per pixel instead of refusing to draw anything is both simpler and correct. */
    int fg_white = (fg != 0x0000) ^ EPD_INVERT, bg_white = (bg != 0x0000) ^ EPD_INVERT;
    for (int row = 0; row < PURR_FONT_H; row++) {
        for (int i = 0; i < n; i++) {
            uint8_t bits = purr_font_glyph(s[i])[row];
            for (int col = 0; col < PURR_FONT_W; col++) {
                int white = (bits & (0x80 >> col)) ? fg_white : bg_white;
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        set_pixel(x + i * PURR_FONT_W * scale + col * scale + sx,
                                 y + row * scale + sy, white);
                    }
                }
            }
        }
    }
}

static void epd_refresh_full(void)
{
    epd_cmd(0x24); epd_data_buf(s_fb, EPD_BUF_LEN);
    epd_cmd(0x22); epd_data(0xC7);
    epd_cmd(0x20);
    epd_wait_busy();
}

static void epd_refresh_partial(void)
{
    epd_cmd(0x24); epd_data_buf(s_fb, EPD_BUF_LEN);
    epd_cmd(0x22); epd_data(0xCF);
    epd_cmd(0x20);
    epd_wait_busy();
}

void hw_flush(void)
{
    if (!s_partial_mode) {
        epd_refresh_full();
        return;
    }
    s_partial_count++;
    if (s_partial_count >= FULL_REFRESH_EVERY) {
        /* Re-seed both RAM banks and reload the partial LUT to clear accumulated ghosting --
         * same two-step sequence hw_display_init() does once at startup. */
        epd_cmd(0x24); epd_data_buf(s_fb, EPD_BUF_LEN);
        epd_cmd(0x26); epd_data_buf(s_fb, EPD_BUF_LEN);
        epd_cmd(0x22); epd_data(0xC7);
        epd_cmd(0x20);
        epd_wait_busy();

        epd_hw_reset();
        epd_wait_busy();
        epd_set_lut(WF_PARTIAL_1IN54);
        epd_cmd(0x37);
        {
            static const uint8_t cfg37[10] = {0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x00,0x00,0x00};
            epd_data_buf(cfg37, sizeof(cfg37));
        }
        epd_cmd(0x3C); epd_data(0x80);
        epd_cmd(0x22); epd_data(0xC0);
        epd_cmd(0x20);
        epd_wait_busy();
        s_partial_count = 0;
    } else {
        epd_refresh_partial();
    }
}

/* ------------------------------------------------------------ buttons */
/*
 * BOOT (short press): down. BOOT (held BTN_HOLD_MS): select. PWR (press): up -- the same
 * mapping the archive's epaper_ui.c button_task used on this exact board, translated onto
 * purr_menu.c's existing w/s/d key convention (purr_key_from_char()) instead of a new one,
 * since that convention already exists and already does exactly this job.
 *
 * This is a plain polled function, not a task: pkg_main.c already calls hw_key() once per
 * TICK_MS (50ms) tick, close to the archive's own 30ms poll, and nothing here needs tighter
 * latency than that -- a refresh already blocks for 0.3-2s regardless.
 */
static int s_boot_was_down, s_boot_hold_fired;
static uint32_t s_boot_down_ms;
static int s_pwr_was_down;
static uint32_t s_uptime_ms;

/* No uptime service in purr_boot_services_t -- pkg_main.c calls this once per tick
 * (pkg_hw.h's own comment on why) instead. Good enough for a hold-vs-short-press threshold. */
void hw_tick(uint32_t dt_ms) { s_uptime_ms += dt_ms; }

int hw_kbd_ok(void) { return 1; }    /* no keyboard concept on this board -- never "not answering" */

char hw_key(void)
{
    int boot_down = pin_read(PIN_BTN_BOOT) == 0;      /* active-low */
    int pwr_down = pin_read(PIN_BTN_PWR) == 0;
    char result = 0;

    if (boot_down && !s_boot_was_down) {
        s_boot_down_ms = s_uptime_ms;
        s_boot_hold_fired = 0;
    } else if (boot_down && s_boot_was_down && !s_boot_hold_fired &&
              (s_uptime_ms - s_boot_down_ms) >= BTN_HOLD_MS) {
        s_boot_hold_fired = 1;
        result = 'd';            /* hold -> select/enter */
    } else if (!boot_down && s_boot_was_down && !s_boot_hold_fired) {
        result = 's';            /* short press -> down */
    }
    s_boot_was_down = boot_down;

    if (pwr_down && !s_pwr_was_down) {
        result = 'w';             /* press -> up */
    }
    s_pwr_was_down = pwr_down;

    return result;
}

/* ------------------------------------------------------------ init */

void hw_init(const purr_boot_services_t *svc)
{
    g = svc;
    svc->gpio_setup(PIN_MOSI, PURR_GPIO_OUT);
    svc->gpio_setup(PIN_SCLK, PURR_GPIO_OUT);
    svc->gpio_setup(PIN_CS, PURR_GPIO_OUT);
    svc->gpio_setup(PIN_DC, PURR_GPIO_OUT);
    svc->gpio_setup(PIN_RST, PURR_GPIO_OUT);
    svc->gpio_setup(PIN_EPD_PWR, PURR_GPIO_OUT);
    svc->gpio_setup(PIN_BUSY, PURR_GPIO_OPEN_DRAIN);      /* input, pulled up, read-only */
    svc->gpio_setup(PIN_BTN_BOOT, PURR_GPIO_OPEN_DRAIN);  /* active-low buttons, same pattern */
    svc->gpio_setup(PIN_BTN_PWR, PURR_GPIO_OPEN_DRAIN);
    pin_hi(PIN_CS);
    pin_hi(PIN_RST);
}
