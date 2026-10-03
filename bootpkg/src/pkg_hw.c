#include "pkg_hw.h"

#include "board.h"
#include "purr_font.h"

/* ------------------------------------------------------------ GPIO (ESP32-S3) */

#define GPIO_BASE 0x60004000u
#define REG(off) (*(volatile uint32_t *)(GPIO_BASE + (off)))

static inline void pin_hi(int p)
{
    if (p < 32) REG(0x08) = 1u << p; else REG(0x14) = 1u << (p - 32);
}
static inline void pin_lo(int p)
{
    if (p < 32) REG(0x0C) = 1u << p; else REG(0x18) = 1u << (p - 32);
}
static inline void pin_drive(int p)         /* output enable on */
{
    if (p < 32) REG(0x24) = 1u << p; else REG(0x30) = 1u << (p - 32);
}
static inline void pin_release(int p)       /* output enable off */
{
    if (p < 32) REG(0x28) = 1u << p; else REG(0x34) = 1u << (p - 32);
}
static inline int pin_read(int p)
{
    return p < 32 ? (int)((REG(0x3C) >> p) & 1u) : (int)((REG(0x40) >> (p - 32)) & 1u);
}

/* ------------------------------------------------------------ eFuse (read-only) */

/* The ESP32-S3's own hardware Secure Boot status (EFUSE_SECURE_BOOT_EN, eFuse block 0,
 * EFUSE_RD_REPEAT_DATA2_REG bit 20 -- same bit esp_secure_boot_enabled() reads via the HAL,
 * not available to this standalone freestanding build). Read-only: this never programs
 * anything. Shown on the boot menu so "is it enabled" has a real, zero-risk answer, whether
 * or not the fuse is ever actually burned (bootloader/SPEC.md section 6). */
#define EFUSE_BASE 0x60007000u
#define EFUSE_RD_REPEAT_DATA2 (*(volatile uint32_t *)(EFUSE_BASE + 0x38))
#define EFUSE_SECURE_BOOT_EN_BIT (1u << 20)

int hw_secure_boot_enabled(void)
{
    return (EFUSE_RD_REPEAT_DATA2 & EFUSE_SECURE_BOOT_EN_BIT) != 0;
}

static const purr_boot_services_t *g;

static void delay_us(uint32_t us) { g->delay_us(us); }
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

static void lcd_cmd(uint8_t c)
{
    pin_lo(PIN_CS);
    pin_lo(PIN_DC);
    spi_byte(c);
    pin_hi(PIN_DC);
    pin_hi(PIN_CS);
}

static void lcd_data(const uint8_t *d, int n)
{
    pin_lo(PIN_CS);
    for (int i = 0; i < n; i++) spi_byte(d[i]);
    pin_hi(PIN_CS);
}

static void lcd_cmd_data(uint8_t c, const uint8_t *d, int n)
{
    lcd_cmd(c);
    if (n) lcd_data(d, n);
}

static void set_window(int x, int y, int w, int h)
{
    int x1 = x + w - 1, y1 = y + h - 1;
    uint8_t cx[4] = {(uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(x1 >> 8), (uint8_t)x1};
    uint8_t cy[4] = {(uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(y1 >> 8), (uint8_t)y1};
    lcd_cmd_data(0x2A, cx, 4);
    lcd_cmd_data(0x2B, cy, 4);
    lcd_cmd(0x2C);
}

void hw_fill(int x, int y, int w, int h, uint16_t c)
{
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > LCD_W || y + h > LCD_H) return;
    set_window(x, y, w, h);
    uint8_t hi = c >> 8, lo = (uint8_t)c;
    pin_lo(PIN_CS);
    for (int n = w * h; n > 0; n--) {
        spi_byte(hi);
        spi_byte(lo);
    }
    pin_hi(PIN_CS);
}

/* This panel already draws immediately -- nothing to batch. */
void hw_flush(void) {}

/* The keyboard reads immediately too -- nothing here needs a clock. */
void hw_tick(uint32_t dt_ms) { (void)dt_ms; }

int hw_text_width(const char *s, int scale)
{
    int n = 0;
    while (s[n]) n++;
    return n * PURR_FONT_W * scale;
}

void hw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    int n = 0;
    while (s[n]) n++;
    if (n == 0) return;
    int w = n * PURR_FONT_W * scale, h = PURR_FONT_H * scale;
    if (x < 0 || y < 0 || x + w > LCD_W || y + h > LCD_H) return;
    set_window(x, y, w, h);
    pin_lo(PIN_CS);
    for (int row = 0; row < PURR_FONT_H; row++) {
        for (int sy = 0; sy < scale; sy++) {
            for (int i = 0; i < n; i++) {
                uint8_t bits = purr_font_glyph(s[i])[row];
                for (int col = 0; col < PURR_FONT_W; col++) {
                    uint16_t c = (bits & (0x80 >> col)) ? fg : bg;
                    for (int sx = 0; sx < scale; sx++) {
                        spi_byte(c >> 8);
                        spi_byte((uint8_t)c);
                    }
                }
            }
        }
    }
    pin_hi(PIN_CS);
}

/* The same start-up sequence as the kernel's driver. */
int hw_display_init(void)
{
    static const struct { uint8_t cmd; uint8_t delay_ms; uint8_t len; uint8_t data[14]; } init[] = {
        {0x01, 150, 0, {0}},
        {0x11, 10, 0, {0}},
        {0x3A, 10, 1, {0x55}},
        {0x36, 0, 1, {LCD_MADCTL | (LCD_BGR ? 0x08 : 0)}},
        {0xB2, 0, 5, {0x0C, 0x0C, 0x00, 0x33, 0x33}},
        {0xB7, 0, 1, {0x35}},
        {0xBB, 0, 1, {0x28}},
        {0xC0, 0, 1, {0x0C}},
        {0xC2, 0, 1, {0x01}},
        {0xC3, 0, 1, {0x13}},
        {0xC4, 0, 1, {0x20}},
        {0xC6, 0, 1, {0x0F}},
        {0xD0, 0, 2, {0xA4, 0xA1}},
        {0xE0, 0, 14, {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44,
                       0x42, 0x06, 0x0E, 0x12, 0x14, 0x17}},
        {0xE1, 0, 14, {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54,
                       0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E}},
    };
    pin_hi(PIN_CS);
    pin_hi(PIN_DC);
    pin_lo(PIN_SCLK);
    for (unsigned i = 0; i < sizeof(init) / sizeof(init[0]); i++) {
        lcd_cmd_data(init[i].cmd, init[i].data, init[i].len);
        hw_delay_ms(init[i].delay_ms);
    }
    if (LCD_INVERT) lcd_cmd(0x21);
    lcd_cmd(0x13);                          /* normal display mode */
    hw_delay_ms(10);
    hw_fill(0, 0, LCD_W, LCD_H, 0x0000);    /* clear before turning the panel on */
    lcd_cmd(0x29);                          /* display on */
    hw_delay_ms(10);
    pin_hi(PIN_BACKLIGHT);
    return 0;
}

/* ------------------------------------------------------------ keyboard, I2C */

/* Open drain by hand: the pin is only ever driven low or let go. */
static inline void sda_lo(void) { pin_drive(PIN_KBD_SDA); }
static inline void sda_hi(void) { pin_release(PIN_KBD_SDA); }
static inline void scl_lo(void) { pin_drive(PIN_KBD_SCL); }
/* Letting SCL go, then waiting while the device holds it low (clock stretching). */
static void scl_hi(void)
{
    pin_release(PIN_KBD_SCL);
    for (int i = 0; i < 2000 && !pin_read(PIN_KBD_SCL); i++) {
        delay_us(1);
    }
}
#define QUARTER() delay_us(5)

static int s_kbd_ok;                        /* the keyboard answered its last poll */
int hw_kbd_ok(void) { return s_kbd_ok; }

static void i2c_start(void)
{
    sda_hi(); scl_hi(); QUARTER();
    sda_lo(); QUARTER();
    scl_lo(); QUARTER();
}

static void i2c_stop(void)
{
    sda_lo(); QUARTER();
    scl_hi(); QUARTER();
    sda_hi(); QUARTER();
}

static int i2c_write(uint8_t b)             /* returns 1 if acknowledged */
{
    for (int i = 7; i >= 0; i--) {
        if ((b >> i) & 1) sda_hi(); else sda_lo();
        QUARTER(); scl_hi(); QUARTER(); scl_lo();
    }
    sda_hi(); QUARTER(); scl_hi(); QUARTER();
    int ack = !pin_read(PIN_KBD_SDA);
    scl_lo(); QUARTER();
    return ack;
}

static uint8_t i2c_read_nack(void)
{
    uint8_t b = 0;
    sda_hi();
    for (int i = 0; i < 8; i++) {
        QUARTER(); scl_hi(); QUARTER();
        b = (uint8_t)((b << 1) | pin_read(PIN_KBD_SDA));
        scl_lo();
    }
    sda_hi(); QUARTER(); scl_hi(); QUARTER(); scl_lo(); QUARTER();   /* NACK: last byte */
    return b;
}

char hw_key(void)
{
    i2c_start();
    if (!i2c_write((KBD_ADDR << 1) | 1)) {
        i2c_stop();
        s_kbd_ok = 0;
        return 0;
    }
    s_kbd_ok = 1;
    uint8_t c = i2c_read_nack();
    i2c_stop();
    return (char)c;
}

/* ------------------------------------------------------------ init */

void hw_init(const purr_boot_services_t *svc)
{
    g = svc;
    svc->gpio_setup(PIN_POWER, PURR_GPIO_OUT);
    pin_hi(PIN_POWER);
    hw_delay_ms(50);                        /* nothing on the rail answers before this */

    svc->gpio_setup(PIN_IDLE_A, PURR_GPIO_OUT);
    svc->gpio_setup(PIN_IDLE_B, PURR_GPIO_OUT);
    pin_hi(PIN_IDLE_A);
    pin_hi(PIN_IDLE_B);

    svc->gpio_setup(PIN_MOSI, PURR_GPIO_OUT);
    svc->gpio_setup(PIN_SCLK, PURR_GPIO_OUT);
    svc->gpio_setup(PIN_CS, PURR_GPIO_OUT);
    svc->gpio_setup(PIN_DC, PURR_GPIO_OUT);
    svc->gpio_setup(PIN_BACKLIGHT, PURR_GPIO_OUT);
    svc->gpio_setup(PIN_KBD_SDA, PURR_GPIO_OPEN_DRAIN);
    svc->gpio_setup(PIN_KBD_SCL, PURR_GPIO_OPEN_DRAIN);
    pin_hi(PIN_CS);
    pin_hi(PIN_DC);
}
