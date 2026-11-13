/* Gamepad module driver (Playtime): a MCP23017 I2C expander scanning a 2x4 button matrix plus one function button, with an LED for every button.
 *
 * What the hardware is (from the maker's public library, read in full; local/playtime-specs.md has the sources):
 *   - I2C address 0x20, on the board's main I2C bus. The module is "attached" when something answers there. It can be absent at start-up and plugged in later.
 *   - Matrix: GPB0 and GPB1 are the rows, GPB2..GPB5 the columns. Every switch has a diode from its column to its row, so a button can only pull its COLUMN low
 *     while its ROW is driven low. Row 1 holds UP, DOWN, LEFT, RIGHT (columns 1..4); row 2 holds A, B, X, Y. FUNC is on its own pin, GPB6.
 *   - Scan: the columns and FUNC (GPB2..GPB6) have their pull-ups on. For each row, IODIRB makes only that row an output (it drives low, because OLATB holds 0 there),
 *     then GPIOB is read: a column bit reading 0 is a pressed button. FUNC pressed = GPB6 reads 0.
 *   - LEDs are active low. GPA0..GPA7 are the LEDs of DOWN, LEFT, RIGHT, A, B, X, Y and FUNC; the LED of UP is GPB7. A pin driven low lights its LED.
 *   - INT and RESET are not used.
 *
 * What it gives the system: it is the "keyboard" device. Every read() returns one key code, or 0 for none, the way the T-Deck's keyboard controller does, and
 * the shell reads it the same way (purr_kernel_key() -> LVGL keypad):
 *     UP 17, DOWN 18, RIGHT 19, LEFT 20 (LVGL's arrow keys), A Enter (10), B Esc (27, the Back key), X Menu (1), Y Home (2), FUNC Tab (9, next field).
 * A direction repeats while it is held. The button's LED lights while it is down.
 * It is also a gamepad-class driver (PURR_DRV_CLASS_GAMEPAD): a shell that needs the raw state (FUNC held, FUNC plus a face button) or the lights (a hint for each
 * button that does something on the current screen) uses that through purr_class_ops(); the keys above keep working for everything else.
 *
 * Freestanding module: no libc, only the bus table. The state lives in this file's statics (one gamepad per board). */
#include "purr_driver.h"
#include "purr_driver_bus.h"
#include "purr_driver_class.h"

#define MCP_IODIRA 0x00
#define MCP_IODIRB 0x01
#define MCP_GPPUA  0x0C
#define MCP_GPIOB  0x13
#define MCP_OLATA  0x14
#define MCP_OLATB  0x15

#define ROW_MASK   0x03u         /* GPB0, GPB1 */
#define COL_SHIFT  2             /* GPB2..GPB5 are columns 1..4 */
#define COL_MASK   0x3Cu
#define FUNC_BIT   0x40u         /* GPB6 */
#define LED_UP_BIT 0x80u         /* GPB7, active low */

#define N_BUTTONS  9             /* 0..3 UP DOWN LEFT RIGHT, 4..7 A B X Y, 8 FUNC */
#define BTN_FUNC   8

#define SCAN_US        12000     /* at most one scan this often while a button is down, however often read() is called */
#define SCAN_IDLE_US   24000     /* and this often while none is: one bus read per look, 40 a second, not hundreds (an emulated I2C controller crashed the guest under load) */
#define ATTACH_US      500000    /* how soon an absent module is looked for again, the first time: it doubles after each miss ... */
#define ATTACH_MAX_US  8000000   /* ... up to this, so a board with no module costs a probe every eight seconds, not every half second */
#define REPEAT_FIRST_US 400000
#define REPEAT_NEXT_US  120000
#define I2C_MS         250       /* a real transfer takes a millisecond; the rest is for an emulator on a busy computer, where a timeout leaves a late interrupt behind */
#define PROBE_MS       30        /* an absent module answers with a NACK at once; the probe only needs to wait out a stuck bus */

static const uint8_t s_key[N_BUTTONS] = {17, 18, 20, 19, '\n', 27, 1, 2, '\t'};

static const purr_driver_bus_table_t *s_bus;
static uint32_t s_h;
static int s_present;
static uint64_t s_next_attach_us;
static uint32_t s_attach_gap_us = ATTACH_US;
static uint64_t s_last_scan_us;
static uint16_t s_down;          /* bit n: button n is down, from the last scan */
static uint16_t s_raw;           /* what the last scan read: a change is believed only when two scans in a row agree (switch bounce, and a bad read on the bus) */
static int s_settling;           /* the last scan disagreed with what is believed: look again soon */
static uint64_t s_repeat_at_us;
static uint8_t s_q[8];
static uint8_t s_q_head, s_q_count;
static uint16_t s_led;           /* bit n: the LED of button n is lit */
static int s_led_override;       /* the shell drives the lights (set_leds); otherwise a light is on while its button is down */
static uint16_t s_led_want;      /* what the shell asked for */

static void q_push(uint8_t k)
{
    if (s_q_count < sizeof(s_q)) {
        s_q[(s_q_head + s_q_count) % sizeof(s_q)] = k;
        s_q_count++;
    }
}

static int q_pop(void)
{
    if (s_q_count == 0) {
        return 0;
    }
    uint8_t k = s_q[s_q_head];
    s_q_head = (uint8_t)((s_q_head + 1) % sizeof(s_q));
    s_q_count--;
    return k;
}

static int wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return s_bus->i2c_write(s_h, b, 2, I2C_MS);
}

static int rd(uint8_t reg, uint8_t *val)
{
    return s_bus->i2c_write_read(s_h, &reg, 1, val, 1, I2C_MS);
}

/* The LEDs follow s_led. OLATA bit k is the LED of button k+1 (DOWN..FUNC); UP is OLATB bit 7. Low = on. Rows stay low in OLATB. */
static int leds_write(void)
{
    uint8_t a = 0;
    for (int n = 1; n < N_BUTTONS; n++) {
        if (s_led & (1u << n)) {
            a |= (uint8_t)(1u << (n - 1));
        }
    }
    uint8_t b = (s_led & 1u) ? 0x00 : LED_UP_BIT;
    uint8_t both[3] = {MCP_OLATA, (uint8_t)~a, b};              /* OLATA and OLATB are neighbours: one transfer sets every light (the rows stay low in OLATB) */
    return s_bus->i2c_write(s_h, both, sizeof(both), I2C_MS);
}

/* Sets the expander up as the firmware does and says whether it answered. The first thing is a probe of the address (bus ABI 9): it makes no noise when nothing
 * is there, where a failed write is an ERROR line from the I2C driver every time. */
static int attach(void)
{
    if (s_bus->i2c_probe != NULL && s_bus->i2c_probe(s_h, PROBE_MS) != 0) {
        return -1;
    }
    if (wr(MCP_IODIRA, 0x00) != 0) {                      /* a module that answers its address but not a write is no use either */
        return -1;
    }
    uint8_t pu[3] = {MCP_GPPUA, 0x00, (uint8_t)(COL_MASK | FUNC_BIT)};
    if (s_bus->i2c_write(s_h, pu, sizeof(pu), I2C_MS) != 0) {
        return -1;
    }
    s_led = 0;
    if (leds_write() != 0) {
        return -1;
    }
    return wr(MCP_IODIRB, (uint8_t)~(ROW_MASK | LED_UP_BIT));   /* the rows are released until a scan drives one */
}

/* Reads the nine buttons into a bitmask, or returns -1 if the module stopped answering. With both rows driven low (the idle state attach() leaves), one read
 * of GPIOB says whether any column or FUNC is pulled low; with none, nothing is pressed and that is all it costs. Only a press starts the row-by-row scan
 * that tells which button it is, and the rows go back to idle afterwards. */
static int scan(void)
{
    uint16_t mask = 0;
    uint8_t any;
    if (rd(MCP_GPIOB, &any) != 0) {
        return -1;
    }
    if ((any & (COL_MASK | FUNC_BIT)) == (COL_MASK | FUNC_BIT)) {
        return 0;
    }
    if (!(any & FUNC_BIT)) {
        mask |= (uint16_t)(1u << BTN_FUNC);                  /* FUNC has a pin of its own: no row needed */
    }
    for (int r = 0; r < 2; r++) {
        if (wr(MCP_IODIRB, (uint8_t)~((1u << r) | LED_UP_BIT)) != 0) {
            return -1;
        }
        uint8_t v;
        if (rd(MCP_GPIOB, &v) != 0) {
            return -1;
        }
        for (int c = 0; c < 4; c++) {
            if (!(v & (1u << (COL_SHIFT + c)))) {
                mask |= (uint16_t)(1u << (r * 4 + c));
            }
        }
    }
    if (wr(MCP_IODIRB, (uint8_t)~(ROW_MASK | LED_UP_BIT)) != 0) {      /* both rows low again: the idle state */
        return -1;
    }
    return (int)mask;
}

static void lost(void)
{
    s_present = 0;
    s_down = 0;
    s_raw = 0;
    s_settling = 0;
    s_led = 0;
    s_q_count = 0;
}

/* Looks for the module while it is absent, scans it while it is there (at most once every SCAN_US, however often this is called), queues the new key presses
 * and keeps the lights right. 1 while the module is there. Called from read() (keys) and from the class ops (state), always on the UI task. */
static int service(void)
{
    uint64_t now = s_bus->now_us();
    if (!s_present) {
        if (now < s_next_attach_us) {
            return 0;
        }
        if (attach() != 0) {
            s_next_attach_us = now + s_attach_gap_us;
            s_attach_gap_us = s_attach_gap_us * 2 > ATTACH_MAX_US ? ATTACH_MAX_US : s_attach_gap_us * 2;
            return 0;
        }
        s_attach_gap_us = ATTACH_US;
        s_present = 1;
        s_last_scan_us = 0;
    }
    if (s_last_scan_us == 0 || now - s_last_scan_us >= ((s_down != 0 || s_settling) ? SCAN_US : SCAN_IDLE_US)) {
        s_last_scan_us = now;
        int m = scan();
        if (m < 0) {
            lost();
            return 0;
        }
        uint16_t seen = (uint16_t)m;
        if (seen != s_down && seen != s_raw) {          /* a change nobody has confirmed: remember it, look again in a moment */
            s_raw = seen;
            s_settling = 1;
            return 1;
        }
        s_raw = seen;
        s_settling = 0;
        uint16_t down = seen;
        uint16_t fresh = (uint16_t)(down & ~s_down);
        for (int n = 0; n < N_BUTTONS; n++) {
            if (fresh & (1u << n)) {
                q_push(s_key[n]);
                if (n < 4) {
                    s_repeat_at_us = now + REPEAT_FIRST_US;
                }
            }
        }
        if ((down & 0x0Fu) && !(fresh & 0x0Fu) && now >= s_repeat_at_us) {     /* a direction is still held */
            for (int n = 0; n < 4; n++) {
                if (down & (1u << n)) {
                    q_push(s_key[n]);
                    break;
                }
            }
            s_repeat_at_us = now + REPEAT_NEXT_US;
        }
        s_down = down;
        uint16_t want = s_led_override ? s_led_want : down;
        if (s_led != want) {
            s_led = want;
            if (leds_write() != 0) {
                lost();
                return 0;
            }
        }
    }
    return 1;
}

static int pad_read(void *handle, uint8_t *data, size_t len)
{
    (void)handle;
    if (len == 0) {
        return -1;
    }
    if (!service()) {
        return 0;
    }
    int k = q_pop();
    if (k == 0) {
        return 0;
    }
    data[0] = (uint8_t)k;
    return 1;
}

/* The class ops (purr_driver_class.h, PURR_DRV_CLASS_GAMEPAD): the raw state and the lights, for a shell that needs more than keys. */
static uint32_t pad_buttons(void *handle)
{
    (void)handle;
    return service() ? s_down : 0;
}

static int pad_present(void *handle)
{
    (void)handle;
    service();
    return s_present;
}

static void pad_set_leds(void *handle, int mask)
{
    (void)handle;
    if (mask < 0) {
        s_led_override = 0;
    } else {
        s_led_override = 1;
        s_led_want = (uint16_t)(mask & 0x1FF);
    }
    s_last_scan_us = 0;                                   /* the next service() writes the lights now */
}

static const purr_gamepad_ops_t s_pad_ops = {
    .version = 1, .buttons = pad_buttons, .present = pad_present, .set_leds = pad_set_leds,
};

static int pad_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle)
{
    (void)board;
    const purr_kv_cfg_t *c = (const purr_kv_cfg_t *)dev->cfg;
    uint32_t h = s_bus->i2c_add_device((int)purr_kv(c, "port", 0), (int)purr_kv(c, "sda", 17), (int)purr_kv(c, "scl", 18),
                                       (uint16_t)purr_kv(c, "addr", 0x20), (int)purr_kv(c, "hz", 400000));
    if (h == 0) {
        return -1;
    }
    s_h = h;
    s_present = 0;                /* looked for on the first read, and again (after 0.5 s, then 1, 2, 4, up to 8 s) while it is absent */
    s_next_attach_us = 0;
    s_attach_gap_us = ATTACH_US;
    s_down = 0;
    s_raw = 0;
    s_settling = 0;
    s_led = 0;
    s_led_override = 0;
    s_q_head = 0;
    s_q_count = 0;
    *out_handle = (void *)(uintptr_t)h;
    return 0;
}

static void pad_remove(void *handle)
{
    (void)handle;
    if (s_present) {
        s_led = 0;
        leds_write();
    }
    s_present = 0;
}

static const purr_driver_t s_pad_driver = {
    .name = "gamepad-mcp23017", .compatible = "gamepad-mcp23017",
    .probe = pad_probe, .remove = pad_remove, .read = pad_read,
    .bus_abi = PURR_DRIVER_BUS_ABI_VERSION,
    .class_id = PURR_DRV_CLASS_GAMEPAD, .ops = &s_pad_ops,
};

__attribute__((used))
const purr_driver_t *gamepad_mcp23017_entry(const purr_driver_bus_table_t *bus)
{
    s_bus = bus;
    return &s_pad_driver;
}
