# PURR OS — Catcalls

> **Accurate as of v1.0.0-dp8.** Contract versions in this document were
> verified against the headers in `source/kernel/catcalls/`, not carried
> forward. If you are adding a member to a catcall, bump its version macro and
> update the matching section here in the same change.

Catcalls are PURR OS's version of syscalls. They are the only way any module or app communicates with hardware — nothing ever calls a driver function directly. The kernel holds one registered implementation per catcall type. Drivers register; everyone else calls through the kernel accessor.

**Pattern:**
```c
// Driver registers its implementation once at init:
purr_kernel_register_display(&my_ili9341_catcall);

// Everything else reads through the kernel — never calls the driver directly:
const catcall_display_t *disp = purr_kernel_display();
if (disp) disp->fill_rect(0, 0, 320, 240, 0x0000);
```

All catcalls follow this pattern. Always null-check the accessor — if no driver has registered, it returns `NULL`.

---

## Catcall Registry

| Catcall | Flag constant | Register fn | Accessor fn |
|---------|--------------|-------------|-------------|
| display | `CATCALL_FLAG_DISPLAY` (`1<<0`) | `purr_kernel_register_display()` | `purr_kernel_display()` |
| touch   | `CATCALL_FLAG_TOUCH` (`1<<1`)   | `purr_kernel_register_touch()` | `purr_kernel_touch()` |
| input   | `CATCALL_FLAG_INPUT` (`1<<2`)   | `purr_kernel_register_input()` | `purr_kernel_input()` |
| radio   | `CATCALL_FLAG_RADIO` (`1<<3`)   | `purr_kernel_register_radio()` | `purr_kernel_radio()` |
| gps     | `CATCALL_FLAG_GPS` (`1<<4`)     | `purr_kernel_register_gps()` | `purr_kernel_gps()` |
| ui      | `CATCALL_FLAG_UI` (`1<<5`)      | `purr_kernel_register_ui()` | `purr_kernel_ui()` |

### Registration semantics — input is the exception

**Five of the six catcalls hold exactly one implementation, and re-registering
overwrites it — last registered wins.** `purr_kernel_register_display()` and
friends are a plain assignment, not a first-wins guard. This matches hardware
reality (there is one display) and permits hot replacement.

**`input` is a list, not a slot.** A device can have several input devices at
once, and the T-Deck Plus does — a trackball *and* a BBQ20 keyboard, both
registered:

```c
void purr_kernel_register_input(const catcall_input_t *drv) {
    if (s_input_count < MAX_INPUTS) s_inputs[s_input_count++] = drv;   // appended
}
```

Accessors:

| Function | Returns |
|---|---|
| `purr_kernel_input()` | `s_inputs[0]` — the **first registered**, kept for legacy callers |
| `purr_kernel_input_count()` | how many are registered |
| `purr_kernel_input_at(i)` | the i-th |

> **Do not use `purr_kernel_input()` on a multi-input device.** On the T-Deck
> Plus the trackball registers before the keyboard, so it returns the trackball
> — a consumer wanting keystrokes must enumerate with `input_count()`/
> `input_at()` and pick by capability. The established test is that a
> keyboard-class driver implements `set_backlight` and a trackball does not.
>
> `purr_kernel_keyboard_set_backlight()` does exactly this internally: it walks
> every registered input and dispatches to whichever implements `set_backlight`,
> so callers never need to know which driver has one.

---

## catcall_display_t

**Source:** `source/kernel/catcalls/catcall_display.h`
**Flag:** `CATCALL_FLAG_DISPLAY` (`1<<0`)
**Version:** `CATCALL_DISPLAY_VERSION 1`

The display catcall is required for all visual modules. KittenUI and MiniWin will refuse to start without it.

```c
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  bits_per_pixel;    // always 16 (RGB565) in current drivers
    const char *name;           // e.g. "ILI9341"
} display_info_t;

typedef struct {
    bool     landscape;
    uint8_t  rotation;          // 0/1/2/3
} display_config_t;

typedef struct {
    const char  *name;
    uint8_t      catcall_version;

    esp_err_t  (*init)(const display_config_t *cfg);
    esp_err_t  (*push_pixels)(int x, int y, int w, int h, const uint16_t *data);
    esp_err_t  (*fill_rect)(int x, int y, int w, int h, uint16_t color);
    esp_err_t  (*set_brightness)(uint8_t level);   // 0-255
    void       (*get_info)(display_info_t *out);
    esp_err_t  (*deinit)(void);
} catcall_display_t;
```

### Functions

| Function | Description |
|----------|-------------|
| `init(cfg)` | Initialise display hardware with orientation from device.pcat. Called by driver_manager. |
| `push_pixels(x,y,w,h,data)` | Blit a rectangle of RGB565 pixels. `data` is `w*h` uint16_t values, row-major. |
| `fill_rect(x,y,w,h,color)` | Fill a rectangle with a single RGB565 colour. Faster than push_pixels for solid fills. |
| `set_brightness(level)` | Set backlight brightness 0-255. No-op on drivers without PWM. |
| `get_info(out)` | Fill `display_info_t` with dimensions and capabilities. |
| `deinit()` | Shut down display hardware. |

### RGB565 format

All pixel data is RGB565, big-endian (byte-swapped for direct SPI DMA):
```
bits 15-11: red (5 bits)
bits 10-5:  green (6 bits)
bits 4-0:   blue (5 bits)
```

### Existing display drivers

| Slug | Panel | Interface | Devices |
|------|-------|-----------|---------|
| `ili9341` | ILI9341 320x240 | SPI + DMA | cyd, cyd_s024c, cyd_s028r |
| `st7789` | ST7789 320x240 | SPI + DMA | tdeck, tdeck_plus |
| `axs15231b` | AXS15231B 480x320 | QSPI | jc3248w535 |
| `ssd1306` | SSD1306 128x64 | I2C | heltec |

---

## catcall_touch_t

**Source:** `source/kernel/catcalls/catcall_touch.h`
**Flag:** `CATCALL_FLAG_TOUCH` (`1<<1`)
**Version:** `CATCALL_TOUCH_VERSION 1`

```c
typedef struct {
    uint8_t  i2c_port;
    uint8_t  sda_pin;
    uint8_t  scl_pin;
    uint8_t  int_pin;    // 0xFF = no interrupt pin
    uint8_t  rst_pin;    // 0xFF = no reset pin
} touch_config_t;

typedef struct {
    const char *name;
    uint8_t     catcall_version;

    esp_err_t (*init)(const touch_config_t *cfg);
    bool      (*read_point)(uint16_t *x, uint16_t *y);
    bool      (*is_pressed)(void);
    esp_err_t (*deinit)(void);
} catcall_touch_t;
```

### Functions

| Function | Description |
|----------|-------------|
| `init(cfg)` | Initialise touch controller with I2C/pin config from device.pcat. |
| `read_point(x,y)` | Read current touch point in screen pixels. Returns `true` if a touch is active. Non-blocking. |
| `is_pressed()` | Returns `true` if currently touched. Cheaper than `read_point` when coordinates are not needed. |
| `deinit()` | Shut down touch controller. |

### Notes

- Coordinates are already mapped to screen pixels (no raw digitiser values).
- XPT2046 (resistive) applies an internal 3-sample median filter.
- CST816S, GT911, AXS15231B (capacitive) read directly from the IC register.

### Existing touch drivers

| Slug | Type | Interface | Devices |
|------|------|-----------|---------|
| `xpt2046` | Resistive | SPI (shared bus) | cyd, cyd_s028r |
| `cst816s` | Capacitive | I2C | cyd_s024c, waveshare169 |
| `gt911` | Capacitive | I2C | tdeck_plus |
| `axs15231b` | Capacitive | I2C | jc3248w535 |

---

## catcall_input_t

**Source:** `source/kernel/catcalls/catcall_input.h`
**Flag:** `CATCALL_FLAG_INPUT` (`1<<2`)
**Version:** `CATCALL_INPUT_VERSION 2` (bumped from 1 — added `set_backlight`)

Handles non-touch input: keyboards, trackballs, rotary encoders.

```c
typedef enum {
    INPUT_EVENT_NONE = 0,
    INPUT_EVENT_KEY_DOWN,
    INPUT_EVENT_KEY_UP,
    INPUT_EVENT_POINTER,    // trackball / mouse delta
} input_event_type_t;

typedef struct {
    input_event_type_t type;
    uint16_t keycode;       // USB HID keycode for KEY_DOWN/UP
    int16_t  delta_x;       // trackball delta for POINTER
    int16_t  delta_y;
    uint8_t  modifiers;     // shift/ctrl/alt bitmask
} input_event_t;

typedef struct {
    const char *name;
    uint8_t     catcall_version;

    esp_err_t (*init)(void);
    bool      (*poll_event)(input_event_t *out);
    esp_err_t (*deinit)(void);

    // Optional — only implemented by drivers with a controllable backlight
    // (e.g. bbq20's under-key LEDs). NULL for drivers without one (trackball).
    esp_err_t (*set_backlight)(uint8_t brightness);
} catcall_input_t;
```

`purr_kernel_keyboard_set_backlight(uint8_t brightness)` (`purr_kernel.h`) dispatches to whichever registered input driver implements `set_backlight` — callers (Settings' keyboard backlight section) don't need to know which specific driver has one, mirroring how screen brightness goes through `catcall_display` rather than a specific display driver.

### Functions

| Function | Description |
|----------|-------------|
| `init()` | Initialise input hardware (keyboard I2C, trackball GPIOs, etc.). |
| `poll_event(out)` | Non-blocking poll. Returns `true` and fills `out` if an event is pending. Returns `false` if queue is empty. |
| `deinit()` | Shut down input hardware. |

### Keycode convention

Key events use USB HID keycodes. `modifiers` bitmask:
```
bit 0 - left ctrl     bit 4 - right ctrl
bit 1 - left shift    bit 5 - right shift
bit 2 - left alt      bit 6 - right alt
bit 3 - left GUI      bit 7 - right GUI
```

### Existing input drivers

| Slug | Type | Devices |
|------|------|---------|
| `trackball` | 4-dir GPIO trackball | tdeck, tdeck_plus |

---

## catcall_radio_t

**Source:** `source/kernel/catcalls/catcall_radio.h`
**Flag:** `CATCALL_FLAG_RADIO` (`1<<3`)
**Version:** `CATCALL_RADIO_VERSION 3`

This catcall covers SPI LoRa modules (SX1262, SX1276). Built-in WiFi and Bluetooth use ESP-IDF APIs directly.

```c
typedef struct {
    uint32_t frequency_hz;
    uint8_t  tx_power_dbm;
    uint8_t  spreading_factor;   // LoRa: 7-12, 0 = N/A
    uint32_t bandwidth_hz;       // LoRa: 125000/250000/500000
    uint8_t  coding_rate;        // LoRa: 5-8 (denominator of 4/x), 0 = N/A
} radio_config_t;

typedef struct {
    const char *name;
    uint8_t     catcall_version;

    esp_err_t (*init)(const radio_config_t *cfg);
    esp_err_t (*send)(const uint8_t *data, size_t len);
    int       (*receive)(uint8_t *buf, size_t max_len);
    bool      (*data_available)(void);
    int       (*rssi)(void);
    float     (*snr)(void);
    esp_err_t (*set_frequency)(uint32_t hz);
    esp_err_t (*set_power)(uint8_t dbm);
    esp_err_t (*set_modulation)(uint8_t sf, uint32_t bw_hz, uint8_t cr);   // v2
    esp_err_t (*set_sync_word)(uint8_t sync);                              // v2
    esp_err_t (*deinit)(void);
    bool      (*wait_rx_signal)(uint32_t timeout_ms);   // v3, OPTIONAL
    void      (*wake_rx_wait)(void);                    // v3, OPTIONAL
} catcall_radio_t;
```

### Functions

| Function | Description |
|----------|-------------|
| `init(cfg)` | Initialise radio with frequency, spreading factor, bandwidth, coding rate. |
| `send(data,len)` | Transmit a packet. Blocks until TX complete. |
| `receive(buf,max)` | Copy received packet into buf. Returns byte count, or -1 if nothing waiting. Non-blocking. |
| `data_available()` | Returns `true` if a received packet is in the driver buffer. |
| `rssi()` | Signal strength of the last received packet in dBm. |
| `snr()` | Signal-to-noise ratio of the last received packet. |
| `set_frequency(hz)` | Change operating frequency on the fly. |
| `set_power(dbm)` | Change TX power on the fly. |
| `set_modulation(sf,bw,cr)` | Retune modulation **after** `init()`. Required by anything matching a fixed radio preset — e.g. Meshtastic's LONG_FAST (SF11, BW250kHz, CR4/5). Without it, `radio_config_t`'s sf/bandwidth/coding-rate are only ever applied once, at init. |
| `set_sync_word(sync)` | Set the LoRa sync word (Meshtastic uses `0x2B`). |
| `deinit()` | Power down the radio. |

### Optional members (v3)

Both are `NULL` on drivers that don't support them, and the **caller must have
its own fallback** — these are behaviour 2 in the sense described under
`catcall_ui_t` above, except the check is "is the pointer NULL" rather than a
sentinel return.

| Function | Purpose | Fallback when NULL |
|---|---|---|
| `wait_rx_signal(timeout_ms)` | Blocks until the radio signals RX-ready (e.g. an IRQ pin edge) or the timeout expires. Returns `true` if signalled. | Caller polls `data_available()` on a fixed short interval — a full SPI transaction each time. |
| `wake_rx_wait()` | Unblocks a task sitting in `wait_rx_signal()` early, for when something *other* than an RX event needs it to run (e.g. a newly-queued outgoing message). **Task context only — not ISR-safe.** | No early wake; the waiter runs when its timeout expires. |

Reference implementation: `sx1262_rl.cpp`, consumed by `meshtastic_module.c`'s
`mesh_task()`.

### Built-in WiFi / Bluetooth

WiFi and BT are silicon peripherals, not exposed through `catcall_radio_t`. The `[radio]` section of `device.pcat` declares their presence:

```ini
[radio]
wifi = true
bt   = true
lora = "sx1262_rl"
```

purrstrap emits `CONFIG_PURR_WIFI`, `CONFIG_PURR_BT`, `CONFIG_PURR_LORA`, and `CONFIG_PURR_LORA_DRIVER` into the device glue layer for conditional compilation in kernel code.

### Existing LoRa drivers

| Slug | Chip | Devices |
|------|------|---------|
| `sx1262` | SX1262 (plain SPI driver) | `tdeck`, `tdeck_plus_pounce` |
| `sx1262_rl` | SX1262 (RadioLib-backed) | `tdeck_plus`, `heltec` |
| `sx1276` | SX1276 | `tdeck_plus_arduino` only — see note below |

> `tdeck_plus_arduino` is the only device still selecting `sx1276`. Real T-Deck
> Plus hardware carries an SX1262; this is a leftover from before the driver swap
> (see `meshplan.md`) and is one more reason that kernel is deprecated.

---

## catcall_gps_t

**Source:** `source/kernel/catcalls/catcall_gps.h`
**Flag:** `CATCALL_FLAG_GPS` (`1<<4`)
**Version:** `CATCALL_GPS_VERSION 1`

```c
typedef struct {
    double   latitude;     // decimal degrees, positive = north
    double   longitude;    // decimal degrees, positive = east
    float    altitude_m;
    float    speed_mps;
    float    hdop;         // horizontal dilution of precision
    uint8_t  satellites;   // satellites used in fix
    bool     valid;        // false until first fix acquired
} gps_fix_t;

typedef struct {
    const char *name;
    uint8_t     catcall_version;

    esp_err_t (*init)(void);
    bool      (*get_fix)(gps_fix_t *out);
    esp_err_t (*deinit)(void);
} catcall_gps_t;
```

### Functions

| Function | Description |
|----------|-------------|
| `init()` | Start the GPS UART + NMEA parser background task. Non-blocking. |
| `get_fix(out)` | Fill `out` with the latest GPS data. Returns `true` if `out->valid` is set (fix acquired). |
| `deinit()` | Stop the GPS task and UART. |

### Existing GPS drivers

| Slug | Protocol | NMEA sentences | Devices |
|------|----------|----------------|---------|
| `generic_nmea` | UART 9600 baud | $GPRMC, $GPGGA | tdeck_plus |

---

## catcall_ui_t  *(new in v0.12.0)*

**Source:** `source/kernel/catcalls/catcall_ui.h`
**Flag:** `CATCALL_FLAG_UI` (`1<<5`)
**Version:** `CATCALL_UI_VERSION 7`

The UI catcall is the widget/windowing abstraction layer added in v0.12.0. UI
modules register an implementation at boot; all apps call through `purr_win.h`,
which dispatches to the registered backend. Apps never touch LVGL or MiniWin
APIs directly, making them portable across every display/UI combination.

> **Read the "Optional members" section below before writing an app against
> this contract.** Roughly a third of `catcall_ui_t` is optional, and the three
> ways a missing member degrades are *not* interchangeable — one of them
> requires the caller to check a return value and provide its own fallback.

```c
typedef uint32_t purr_win_t;   // opaque window handle
typedef uint32_t purr_wid_t;   // opaque widget handle

typedef enum {
    PURR_EVENT_CLICKED   = 0,
    PURR_EVENT_CHANGED   = 1,   // textarea text changed
    PURR_EVENT_FOCUSED   = 2,
    PURR_EVENT_DEFOCUS   = 3,
    PURR_EVENT_SELECTED  = 4,   // list: highlight moved (no confirm)
    PURR_EVENT_ACTIVATED = 5,   // list: entry confirmed/entered
} purr_event_t;

typedef void (*purr_win_cb_t)(purr_wid_t wid, purr_event_t event, void *user);

// Canvas callbacks — see "Canvas" below.
typedef void (*purr_win_paint_cb_t)(purr_win_t win, void *user);
typedef void (*purr_win_touch_cb_t)(purr_win_t win, int16_t x, int16_t y,
                                     bool pressed, void *user);

typedef enum { PURR_ALIGN_LEFT=0, PURR_ALIGN_CENTER, PURR_ALIGN_RIGHT } purr_align_t;
typedef enum { PURR_LAYOUT_ROW=0, PURR_LAYOUT_COL } purr_layout_t;

typedef struct {
    const char *name;
    uint8_t     catcall_version;      // must equal CATCALL_UI_VERSION

    // ── Window lifecycle ──
    purr_win_t (*win_create) (const char *title);
    void       (*win_destroy)(purr_win_t win);
    void       (*win_show)   (purr_win_t win);
    void       (*win_hide)   (purr_win_t win);
    void       (*win_clear)  (purr_win_t win);   // remove all child widgets
    void       (*win_on_close)(purr_win_t win, purr_win_cb_t cb, void *user);  // OPTIONAL

    // ── Labels ──
    purr_wid_t (*label_create) (purr_win_t win, const char *text);
    void       (*label_set)    (purr_wid_t wid, const char *text);
    void       (*label_align)  (purr_wid_t wid, purr_align_t align);
    void       (*label_set_big)(purr_wid_t wid, const char *text);            // OPTIONAL

    // ── Buttons ──
    purr_wid_t (*btn_create)(purr_win_t win, const char *label,
                              purr_win_cb_t cb, void *user);
    void       (*btn_enable)(purr_wid_t wid, bool enabled);

    // ── Textarea ──
    purr_wid_t  (*textarea_create)(purr_win_t win, uint16_t w_pct, uint16_t h_pct);
    void        (*textarea_append)(purr_wid_t wid, const char *text);
    void        (*textarea_set)   (purr_wid_t wid, const char *text);
    void        (*textarea_clear) (purr_wid_t wid);
    const char *(*textarea_get)   (purr_wid_t wid);   // backend-owned string
    void        (*textarea_focus) (purr_wid_t wid);
    void        (*textarea_cb)    (purr_wid_t wid, purr_win_cb_t cb, void *user);

    // ── List (flat, non-nested, selectable) ──
    purr_wid_t (*list_create)      (purr_win_t win, uint16_t w_pct, uint16_t h_pct);
    void       (*list_set_items)   (purr_wid_t wid, const char **items, int count);
    void       (*list_clear)       (purr_wid_t wid);
    int        (*list_get_selected)(purr_wid_t wid);   // -1 if none
    void       (*list_set_selected)(purr_wid_t wid, int index);
    void       (*list_cb)          (purr_wid_t wid, purr_win_cb_t cb, void *user);
    void       (*list_set_items_icon)(purr_wid_t wid, const char **items,
                                       const char **icons, int count);        // OPTIONAL

    // ── Tile grid (scrollable icon+label tiles) ──
    purr_wid_t (*tile_grid_create)   (purr_win_t win, uint16_t w_pct, uint16_t h_pct);  // OPTIONAL
    void       (*tile_grid_set_items)(purr_wid_t grid_wid,
                                       const char **labels, const char **symbols,
                                       const uint32_t *colors,
                                       purr_win_cb_t *cbs, void **users, int count);    // OPTIONAL

    // ── Layout containers ──
    // grow: fills the remaining space in the parent's flex layout instead of
    // hugging its own content — required for a row/col holding percentage-sized
    // children (a list, a textarea, a split view), which otherwise collapse to
    // 0 size inside a content-sized parent.
    purr_wid_t (*layout_begin)(purr_win_t win, purr_layout_t dir, uint8_t pad, bool grow);
    void       (*layout_end)  (purr_wid_t container);

    // ── On-screen keyboard ──
    void (*kb_show)(purr_win_t win, purr_wid_t target_textarea);
    void (*kb_hide)(purr_win_t win);

    // ── Canvas (raw draw + touch) ──                                          // OPTIONAL
    void (*canvas_on_paint)(purr_win_t win, purr_win_paint_cb_t cb, void *user);
    void (*canvas_on_touch)(purr_win_t win, purr_win_touch_cb_t cb, void *user);
    void (*canvas_rect)    (purr_win_t win, int16_t x, int16_t y,
                             int16_t w, int16_t h, uint32_t color);
    void (*canvas_text)    (purr_win_t win, int16_t x, int16_t y,
                             const char *text, uint32_t color);
    void (*canvas_repaint) (purr_win_t win);
    void (*canvas_size)    (purr_win_t win, int16_t *w, int16_t *h);
} catcall_ui_t;
```

### Optional members — how a missing member degrades

This is the part most likely to bite you. Members marked `OPTIONAL` above may be
`NULL`, and **there are three different degradation behaviours**. They are not
interchangeable, and only one of them is detectable by the caller.

| Behaviour | Which members | What the caller sees |
|---|---|---|
| **1. Silent no-op** | `win_on_close`, `tile_grid_set_items`, all `canvas_*` | Nothing happens. The call returns normally and there is **no way to tell** it did nothing. |
| **2. Sentinel return** | `tile_grid_create` → `0`<br>`win_create` → `0`<br>`textarea_get` → `NULL`<br>`list_get_selected` → `-1` | A defined "didn't work" value. **The caller must check it** and supply its own fallback. |
| **3. Automatic fallback** | `label_set_big` → `label_set`<br>`list_set_items_icon` → `list_set_items` | `purr_win.h` silently substitutes the non-optional equivalent. Always safe to call; you simply lose the enhancement. |

Behaviours 1 and 3 come free — call the wrapper and ignore the question.
**Behaviour 2 does not.** `purr_win_tile_grid()` returning `0` on a backend
without tile grids is the case that actually requires app-side handling:

```c
// Correct: tile grid where available, list everywhere else.
purr_wid_t grid = purr_win_tile_grid(win, 100, 80);
if (grid) {
    purr_win_tile_grid_set_items(grid, labels, symbols, colors, cbs, users, n);
} else {
    purr_wid_t list = purr_win_list(win, 100, 80);
    purr_win_list_set_items(list, labels, n);
}
```

Only the caller knows what a sane fallback looks like for its own content, which
is why the dispatch layer does not guess. `source/apps/system/msn/msn.c`'s Home
screen is the reference implementation.

The general rule, enforced by `_UI_CALL`/`_UI_VOID` in `purr_win.h`: **every**
member is null-checked before dispatch, so no call can crash on an unimplemented
backend — but "does not crash" is not "did what you asked".

### Optional-member support matrix

Verified against each backend's `catcall_ui_t` initialiser as of DP8:

| Backend | `win_on_close` | `label_set_big` | `tile_grid_*` | `list_set_items_icon` | `canvas_*` |
|---|:--:|:--:|:--:|:--:|:--:|
| `cupcake` | ● | ● | ● | ● | — |
| `mochi`   | ● | ● | ● | ● | — |
| `tabby`   | ● | ● | ● | ● | — |
| `miniwin` | ● | — | — | — | ● |
| `cardstack` | ● | — | — | — | — |
| `nougat`  | ● | — | — | — | — |
| `pounce`  | ● | — | — | — | — |
| `kittenui` | — | — | — | — | — |

Two things worth noting from that table:

- **`canvas_*` is MiniWin-only, and currently unused by any app.** The header
  recommends it for widget-dense screens (calculators, keypads) to avoid a
  window-teardown hang seen with ~20 native button controls. That advice is
  sound for MiniWin but **not portable** — on any LVGL backend those calls are
  silent no-ops (behaviour 1), so a canvas-drawn app would render nothing at
  all. Do not adopt it without checking the target backend.
- **`kittenui` implements no optional members.** Anything you build against the
  optional surface degrades fully there.

### Registered implementations

Eight backends register a `catcall_ui_t`. Three more (`blackpurr`, `oled_ui`,
`lvgldebug`) are shell-tier and draw directly via `catcall_display_t` without
registering a UI catcall at all — apps using `purr_win.h` do not run on those.

| UI module | Renderer | Tier | Notes |
|---|---|---|---|
| `kittenui` | LVGL 8 | Windowed | Small-to-large SPI screens; no optional members |
| `miniwin` | Vendored MiniWin WM | Windowed | Only backend implementing `canvas_*` |
| `cupcake` | LVGL 8 | Windowed | Android 1.5-style launcher |
| `cardstack` | LVGL 8 | Windowed | Rabbit R1-style snap-scroll card UI |
| `mochi` | LVGL 8 | Windowed | iOS-style springboard — see `09_SystemUI.md` |
| `tabby` | LVGL 8 | Windowed | Keyboard-first type-to-filter launcher |
| `nougat` | LVGL 9 | Windowed | **Experimental**, parked — see note below |
| `pounce` | Raw framebuffer | Framebuffer | No LVGL; own widget model. Omits `label_set_big` and `tile_grid_*` |

**On `nougat`:** its manifest and Kconfig help both describe M5Stack Tab5 as its
"first and only device", but `devices/tab5/device.pcat` currently sets
`ui = "cupcake"`. Nougat was an experimental LVGL 9 port that was parked once
LVGL 8 proved stable on that hardware; Tab5 was moved back. Treat the
Tab5-specific wording in those two places as historical.

### Using the UI catcall from an app

Apps include `purr_win.h` — not `catcall_ui.h`:

```c
#include "purr_win.h"

static purr_win_t s_win;

static void on_tap(purr_wid_t w, purr_event_t e, void *u) {
    purr_win_label_set((purr_wid_t)(uintptr_t)u, "Tapped!");
}

int my_app_init(void) {
    s_win = purr_win_create("Demo");
    purr_wid_t lbl = purr_win_label(s_win, "Hello, PURR OS!");
    purr_win_button(s_win, "Tap me", on_tap, (void*)(uintptr_t)lbl);
    purr_win_show(s_win);
    return 0;
}
```

`purr_win.h` null-checks the registered backend before every call — the app silently no-ops if no UI module is loaded yet.

### Writing a new UI backend

Implement the function pointers in `catcall_ui_t`, then call
`purr_kernel_register_ui(&my_ui)` from your module's `init()`.

Set `.catcall_version = CATCALL_UI_VERSION` — use the macro, never a literal.
Every backend in the tree does this, which is why the 6→7 bump cost nothing.

**You do not have to implement everything.** The members marked `OPTIONAL` may
be left `NULL`; see "Optional members" above for exactly how each one degrades.
A minimal viable backend implements windows, labels, buttons, textareas, lists,
layout and the keyboard hooks — that is what every system app actually uses.

Reference implementations, in rough order of usefulness:

- `source/modules/tabby/tabby_win.c` — a current, fully-commented LVGL backend.
  Its header records several hazards any LVGL backend hits (deferred window
  teardown, unbinding the on-screen keyboard before freeing its textarea,
  deferred list rebuilds, per-callback context lifetime). Read that comment
  before writing your own; each item is a live-confirmed bug, not theory.
- `source/modules/miniwin/miniwin_win.c` — non-LVGL, and the only `canvas_*`
  implementation.
- `source/modules/pounce/pounce_win.c` — no LVGL and no vendored toolkit;
  useful if you are targeting raw framebuffer.

Note that `tabby_win.c`, `mochi_win.c` and `cupcake_win.c` are close relatives
(~95% shared). That duplication is known and is a candidate for extraction into
a shared LVGL window layer; treat it as three copies of one design rather than
three independent designs.

---

## Glue Layer — How Pins Get to Drivers

Drivers do not hardcode pin numbers. purrstrap generates `purr_device_glue.c` for each device with `#define` macros from `device.pcat [pins]`:

```c
#define CONFIG_DRV_DISPLAY_CS_PIN  15
#define CONFIG_DRV_DISPLAY_DC_PIN  2
#define CONFIG_DRV_DISPLAY_BL_PIN  27
#define CONFIG_PURR_WIFI           1
#define CONFIG_PURR_BT             1
#define CONFIG_PURR_LORA           1
#define CONFIG_PURR_LORA_DRIVER    "sx1262_rl"

const char *purr_flash_app_dir = "/flash/apps";
const char *purr_sd_app_dir    = "/sdcard/apps";
```

Drivers `#include` these defines — the same driver binary runs on any device that provides its catcall hardware.

## Implementing a New Catcall (Driver Author)

1. Implement all function pointers in the catcall struct (leave unused ones as `NULL`).
2. Define a static const instance of the struct.
3. In your module's `init()`, call the matching `purr_kernel_register_*()`.
4. Set `provided_catcalls` bitmask in your `purr_module_header_t`.

```c
static const catcall_display_t s_disp = {
    .name            = "my_display",
    .catcall_version = CATCALL_DISPLAY_VERSION,
    .init            = my_init,
    .push_pixels     = my_push_pixels,
    .fill_rect       = my_fill_rect,
    .set_brightness  = NULL,   // not supported on this panel
    .get_info        = my_get_info,
    .deinit          = my_deinit,
};

static int my_module_init(void) {
    purr_kernel_register_display(&s_disp);
    return 0;
}

purr_module_header_t purr_module = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .load_priority     = PURR_PRIORITY_REQUIRED,
    .name              = "my_display",
    .provided_catcalls = CATCALL_FLAG_DISPLAY,
    .required_catcalls = 0,
    .init              = my_module_init,
    .deinit            = my_module_deinit,
};
```

---

*DP8 documentation pass performed by Claude Opus 5 in agentic/auto mode. Every
contract version and struct member above was verified against the headers in
`source/kernel/catcalls/` at the time of writing.*
