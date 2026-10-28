# 13. Writing drivers

**Read this first.** The kernel spec (`PurrOS/components/kernel/SPEC.md`) describes a plug-and-play driver system:
a driver registry filled by a linker section, a pin registry that refuses conflicts, a bus layer, catcall registries,
board profiles as data, a signed devices bundle, and user-installable data-description drivers. **None of that is
built** ([F-02](FINDINGS.md#f-02)). What exists is simpler, and this chapter documents it as it is, then shows how to add
hardware inside that structure. Everything labelled **[DESIGNED]** is the spec's plan, not code you can use.

## What a driver is today

| Piece | File | What it is |
|-------|------|-----------|
| **Board profile** | `PurrOS/components/kernel/include/purr_board.h`, `boards/<board>.c` | one `const purr_board_t` describing the board's power pin, SPI bus, **one** display and **one** keyboard. Data only. |
| **Kernel bring-up** | `PurrOS/components/kernel/src/purr_kernel.c` | `purr_kernel_init()`: switch on the power rail, hold other chip selects high, start the SPI bus, start the keyboard I2C bus, start the display. Hard-wired, in that order. |
| **Display driver** | `src/st7789.c` | `purr_st7789_init(cfg, spi_host, &ops)` returns a `purr_display_v2_t` of function pointers. |
| **Input** | in `purr_kernel.c` | `purr_kernel_key()` reads one byte from the keyboard controller over I2C. |
| **Layers above** | `purr_gfx.c`, `purr_console.c`, `purr_term.c` | text drawing and the console, written against `purr_display_v2_t`. They know nothing about ST7789. |

Selecting a board is a Kconfig choice (`PURR_BOARD`, default `PURR_BOARD_TDECK_PLUS`) and a line in
`components/kernel/CMakeLists.txt` that adds `boards/tdeck_plus.c` when it is chosen. Selecting the display driver is a
**direct call** in `purr_kernel_init()`. There is no search, no matching, no probing.

Modules and kernelmods ([12](12-writing-modules.md)) are **not** drivers: they reach hardware only through functions the
kernel table already provides.

## The board profile

`purr_board_t` (`purr_board.h`), fully:

| Field | Meaning |
|-------|---------|
| `name` | codename, e.g. `"tdeck_plus"`. Matched against manifests, so keep it identical to the release board name. |
| `power_pin`, `power_settle_ms` | a GPIO that switches the peripheral power rail (`PURR_PIN_NONE` = -1 if none). Set high before anything else. On the T-Deck Plus this is GPIO 10: display, touch, keyboard, SD and LoRa all sit behind it and none answers until it is on. |
| `idle_high_pins[4]` | chip selects of **other** devices on the shared SPI bus, driven high (deselected) so they stay quiet while the display talks. T-Deck Plus: LoRa 9, SD 39. |
| `spi` | `{host, mosi, miso, sclk}`. T-Deck Plus: SPI2, 41, 38, 40. |
| `display` | `purr_display_cfg_t`: `name`, `compatible`, `cs`, `dc`, `rst`, `backlight`, `width`, `height`, `col_off`, `row_off`, `madctl`, `bgr`, `invert`, `spi_hz` |
| `keyboard` | `purr_keyboard_cfg_t`: `port`, `sda`, `scl`, `addr` (0 = no keyboard), `hz` |

The whole T-Deck Plus profile is `components/kernel/boards/tdeck_plus.c`. Its pins came from the old DP9 archive profile, which
was checked against LilyGO's headers, and are "not yet checked in this rewrite except by the display coming up" (the file
says so). `compatible` is carried but nothing reads it.

## The display contract

`purr_display.h`. A driver provides a `purr_display_v2_t`:

```c
typedef struct {
    uint16_t struct_size;
    uint8_t  major, minor;            /* 2, 0 */
    uint32_t features;                /* PURR_DISPLAY_F_BRIGHTNESS | PURR_DISPLAY_F_POWER */

    esp_err_t (*get_info)(purr_display_info_t *out);                       /* required */
    esp_err_t (*blit)(int x, int y, int w, int h, const uint16_t *pixels);  /* required */
    esp_err_t (*fill)(int x, int y, int w, int h, uint16_t color);          /* required */
    esp_err_t (*set_brightness)(uint8_t level);                             /* optional, NULL if absent */
    esp_err_t (*set_power)(bool on);                                        /* optional, NULL if absent */
} purr_display_v2_t;
```

The rules a driver must follow:

- **Pixels are host-endian RGB565.** If the panel wants bytes swapped, the driver swaps. Callers never know. Build colours with
  `PURR_RGB565(r, g, b)`.
- **Clip.** Coordinates outside the panel are clipped by the driver. A rectangle with nothing visible succeeds and does nothing.
- **`blit(x, y, w, h, pixels)`**: `pixels` is `w * h` values, row by row, **un-clipped dimensions**: if the rectangle hangs off the edge, the
  source stride is still `w`. The driver must skip the clipped part, not shift the data. (`st7789.c` does: `skip_x`, `skip_y`.)
- **No frame buffer.** The API has none. Callers draw in chunks (`get_info().max_chunk_pixels` says how many pixels the driver
  likes per call) and the driver copies each to the panel. This is what keeps it usable on boards with no PSRAM.
- `get_info` reports `width` and `height` in the **current orientation**, `max_chunk_pixels`, and a 24-byte `name`.
- Optional functions are `NULL` **and** their feature bit is clear when absent. Callers must check.
- Return `esp_err_t`.

The spec's version 2.0 also lists `set_rotation`, `blit_async` and `wait_idle`. The header does not have them
([F-27](FINDINGS.md#f-27)). Rotation is set once by the board profile (`madctl`).

**Thread safety:** the spec says the kernel's bus lock serialises concurrent `blit` calls. There is no bus lock. The ST7789 driver
shares one DMA buffer and one SPI device, so two tasks drawing at once would corrupt each other. Today only the shell task draws
(`purr_console_flush`); keep it that way, or add a mutex in the driver.

## Anatomy of the ST7789 driver

`st7789.c` (297 lines) is a good template. Its parts:

1. **Init sequence as data.** `s_init[]` is a table of `{command, delay_ms, length, data[14]}` steps: software reset, sleep out,
   16 bits per pixel, orientation, porch, gate, VCOM, power, gamma. The orientation step takes its byte from the board profile
   (`madctl | (bgr ? 0x08 : 0)`). Everything that differs per panel or per board is in the profile, so a variant is a data change.
2. **Reset and power-on.** Optional hardware reset pulse (`rst`), init steps with their delays, optional `INVON` if the profile asks,
   `NORON`, clear the screen black, then `DISPON`.
3. **SPI.** One device on the board's SPI bus at the profile's `spi_hz` (40 MHz on the T-Deck Plus), mode 0, with the D/C pin driven
   from a `pre_cb` callback per transaction (command or data). Pixel data is sent in chunks of 16 lines from one internal DMA buffer
   (`width * 16 * 2` bytes).
4. **Window and write.** `CASET`, `RASET`, `RAMWR`, honouring `col_off`/`row_off` for panels whose RAM is bigger than the glass.
5. **Backlight.** If a backlight pin is given, LEDC timer 2 and channel 2 at 5 kHz, 8-bit; on success `set_brightness` is filled in
   and `PURR_DISPLAY_F_BRIGHTNESS` is set. `set_power` is not implemented.
6. **State** is file-static (`static struct {...} s`). One display per binary ([F-03](FINDINGS.md#f-03)).

## The keyboard contract

`purr_kernel_key()` returns the next key as one `char`, or `0` if none is waiting (or there is no keyboard). The T-Deck keyboard is a small
controller at I2C address `0x55` that answers a one-byte read with the next key as ASCII, or `0`. The read has a 20 ms timeout.

What the rest of the system expects from "a key":

- printable ASCII (`0x20` to `0x7E`) is inserted,
- `\r` or `\n` is Enter,
- `\b` (`0x08`) or `0x7F` is Backspace,
- `0x15` (Ctrl-U) clears the line,
- the boot menu uses `w`, `s`, `d` and Enter (no arrow keys).

Anything else is ignored. A different keyboard or a USB or serial input only has to produce the same characters from
`purr_kernel_key()`. There is no input event system (the spec's `input` catcall with key, pointer and scroll events is **[DESIGNED]**), no
touch, no trackball, and no key repeat handling beyond what the controller does.

## Adding hardware: the recipe

Because there is no registry, adding a device means editing the kernel in five places. The pattern, in order:

1. **Describe it.** Add its pins and address to `purr_board_t` and fill them in `boards/<board>.c`.
2. **Bring it up.** In `purr_kernel_init()`, after the bus it sits on exists, create the device handle. Keep the handle in a file-static.
   Record failures as warnings and carry on; do not make the whole boot depend on an optional part.
3. **Give it a kernel API.** Declare a function in `purr_kernel.h` (for example `purr_kernel_i2c_probe(addr)`) and implement it next to
   `purr_kernel_key()`.
4. **Expose it to commands, if you want a shell command.** Add a field to `purr_kernel_table_t`, **bump `PURR_KERNEL_TABLE_ABI_VERSION`**,
   implement the entry in `PurrOS/main/commands.c` (`s_kernel_table`), and, if the standalone `Kernel/` binary should have it,
   in `Kernel/main/kernel_table.c`. Everything that loads against that table must then be rebuilt and re-signed
   ([12](12-writing-modules.md)).
5. **Write the command** as a kernelmod, build, sign and install it ([12](12-writing-modules.md)).

Design guidance carried over from the existing code: expose **what a command needs to do**, not raw handles. A module is never given a bus
or a pin. If the operation can damage hardware (driving unknown pins), make it one opaque call that does its own checks.

### Worked example

The next section is a complete, compiled example of those five steps: an `i2cscan` command.

## Worked example: an `i2cscan` command

The Install spec lists `i2cscan` as a discovery tool. It is a good first driver-level change because it touches every layer and
needs no new hardware. **Verified:** every edit below was applied to a scratch copy of the tree and compiled with the real toolchain
(`purrstrap coreos build`, and `purrstrap modules build` for the kernelmod). It was not run on a device.

### Step 1 and 2: keep the bus handle (kernel)

`purr_kernel_init()` already creates the I2C bus for the keyboard but throws the handle away. Keep it, and add a probe next
to `purr_kernel_key()`. In `PurrOS/components/kernel/src/purr_kernel.c`:

```diff
 static i2c_master_dev_handle_t s_kbd;
+static i2c_master_bus_handle_t s_i2c_bus;
...
-        i2c_master_bus_handle_t bus_h;
+        i2c_master_bus_handle_t bus_h = NULL;
...
             s_kbd = NULL;
             ESP_LOGW(TAG, "keyboard bus did not start");
         }
+        s_i2c_bus = bus_h;
     }
...
+int purr_kernel_i2c_probe(int addr)
+{
+    if (s_i2c_bus == NULL) {
+        return -1;                               /* no I2C bus on this board */
+    }
+    return i2c_master_probe(s_i2c_bus, (uint16_t)addr, 20) == ESP_OK ? 1 : 0;
+}
```

### Step 3: declare it

`PurrOS/components/kernel/include/purr_kernel.h`:

```diff
+/* Does a device answer at this 7-bit address on the board's I2C bus? 1 yes, 0 no, -1 no bus. */
+int purr_kernel_i2c_probe(int addr);
+
 /* The next key as a character, or 0 if none is waiting (or there is no keyboard). */
```

### Step 4: put it in the kernel table and bump the version

`PurrOS/components/coreos/include/purr_kernel_table.h`:

```diff
-#define PURR_KERNEL_TABLE_ABI_VERSION 6u
+#define PURR_KERNEL_TABLE_ABI_VERSION 7u
...
     int (*login_take_logout)(void);        /* 1 if "logout" was run, and clears the request */
+
+    /* Does a device answer at this 7-bit I2C address? 1 yes, 0 no, -1 no bus. Read-only. */
+    int (*i2c_probe)(int addr);
 } purr_kernel_table_t;
```

`PurrOS/main/commands.c`, beside the other `kernel_table_*` wrappers and in `s_kernel_table`:

```diff
 static void kernel_table_console_flush(void) { purr_console_flush(); }
+static int kernel_table_i2c_probe(int addr) { return purr_kernel_i2c_probe(addr); }
...
     .login_take_logout = kernel_table_login_take_logout,
+    .i2c_probe = kernel_table_i2c_probe,
 };
```

The standalone `Kernel/main/kernel_table.c` is left alone: the field stays `NULL` there, and a kernelmod calling it would crash. Only
wire it up if that kernel should provide it.

### Step 5: the command (a kernelmod)

`Modules/coreos/i2cscan_module.c`:

```c
#include "purr_kernel_table.h"

static const purr_kernel_table_t *s_k;

static int cmd_i2cscan(purr_cli_t *cli, int argc, char **argv)
{
    (void)argc; (void)argv;
    int found = 0;
    s_k->puts(cli, "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    for (int row = 0; row < 8; row++) {
        s_k->printf(cli, "%02x:", row * 16);
        for (int col = 0; col < 16; col++) {
            int addr = row * 16 + col;
            if (addr < 0x08 || addr > 0x77) {
                s_k->puts(cli, "   ");
                continue;
            }
            int r = s_k->i2c_probe(addr);
            if (r < 0) {
                s_k->puts(cli, "\nno I2C bus on this board\n");
                return 1;
            }
            if (r > 0) {
                found++;
                s_k->printf(cli, " %02x", addr);
            } else {
                s_k->puts(cli, " --");
            }
        }
        s_k->puts(cli, "\n");
    }
    s_k->printf(cli, "%d device(s) found\n", found);
    return 0;
}

static const purr_cmd_t s_cmds[] = {
    {"i2cscan", "list devices on the I2C bus", cmd_i2cscan},
};

static const purr_kernel_module_table_t s_table = {
    .abi_version = PURR_KERNEL_TABLE_ABI_VERSION,
    .cmds = s_cmds,
    .cmd_count = 1,
};

__attribute__((used))
const purr_kernel_module_table_t *i2cscan_module_entry(const purr_kernel_table_t *kernel)
{
    s_k = kernel;
    return &s_table;
}
```

### Build and install

```powershell
python purrstrap/purrstrap.py coreos build --board tdeck_plus --profile full
python purrstrap/purrstrap.py modules build --source Modules/coreos/i2cscan_module.c --entry i2cscan_module_entry `
  --kind driver --name i2cscan --version 0.1.0 --out rel/i2cscan.cat
python purrstrap/purrstrap.py keys sign --key signing_keys/developer.key --image rel/i2cscan.cat --key-id 2
```

The firmware build finished clean (one pre-existing, unrelated warning: `TAG` unused in `recovery_loader.c`) and the kernelmod
built as `657 bytes, 13 data + 1 code relocation(s)`.

**You are not done: the ABI went from 6 to 7.** Every other kernelmod (`sysinfo`, `fs`, `login`, `appmgr`) is now refused as an
ABI mismatch. Rebuild and re-sign all four, then get all five onto the device ([12](12-writing-modules.md) section 3, either by
embedding them in `commands.c` or by publishing a module index). After the app image is flashed, the old files on `root` are
still there and are rejected, which is why `help` will be short until you replace them.

Expected output (addresses depend on the board; the T-Deck keyboard is `55`):

```
alice@PURR OS> i2cscan
     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
00:                       -- -- -- -- -- -- -- -- --
10: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
...
50: -- -- -- -- -- 55 -- -- -- -- -- -- -- -- -- --
...
1 device(s) found
```

(Only the shape is shown; this was compiled, not run.)

This is the five-step pattern for any new capability. Note how little of it is the "driver": the rest is plumbing that a real
registry would remove.


## What the spec promises instead

All **[DESIGNED]**. Listed so you know what a future driver author will do, and what you must not assume exists today.

| Promised | Where | Not built |
|----------|-------|-----------|
| `purr_driver_t` with `name`, `compatible[]`, `probe`, `remove`, registered by one macro into a linker section | kernel SPEC section 5 | yes |
| device states `bound`, `no_driver`, `probe_failed`, `pin_conflict` | section 5 | yes |
| kernel-owned SPI and I2C hosts, bus locks, a pin registry that names both owners on conflict | section 6 | yes |
| catcall registry with generations, bridging adapters and feature bits | section 7 | yes (only the display struct exists) |
| the board profile as data inside a signed devices bundle in `/boot` | sections 3 and 13, `Drivers/SPEC.md` | yes |
| data-description drivers installed from an SD card, run by generic engines | section 14 | format undecided |
| code drivers as signed native modules | section 14 | no module can reach hardware |
| a per-board driver pack: signed manifest pinning each driver's hash and pins | `Drivers/SPEC.md` | yes |
| hardware discovery: I2C/SPI/UART walks, `i2cscan`, `spiprobe`, a saved discovered profile | `Install/SPEC.md` | yes |
| touch (CST816S), SD card, LoRa, GPS, audio, e-paper drivers | kernel SPEC, `Boards/`, `Catcalls/` | yes |

Next: [14 Adding a display](14-adding-a-display.md).
