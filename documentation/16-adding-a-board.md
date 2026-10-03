# 16. Adding a board

How to port PURR OS to a new board, and exactly which files to touch. **Verified by doing it:** a made-up board, `myboard` (an ESP32-S3 dev kit with 8 MB flash, 8 MB PSRAM and a
240x240 SPI ST7789), was added in a scratch copy of the tree and the full PurrOS profile, the boot package and the bootloader were built for it with the real
toolchain. All three built clean. The files are in [examples/board/](examples/board/). It was **not flashed**, and the pins are invented.

**What the design allows.** Only **modular** boards: an ESP32-S3-class chip, at least 4 MB of flash (8 MB or more in practice, see below) and PSRAM (modules and
the loader allocate in PSRAM). Anything on the **original ESP32**, the **monolithic tier**, and any **RISC-V chip** is not supported by the code
([F-01](FINDINGS.md#f-01), [F-21](FINDINGS.md#f-21)): the boot package is Xtensa-S3 only, modules are built with Xtensa flags, and there is no packed-image build.
The spec's plan for a "folder per board" (`Boards/SPEC.md` section 3) is **[DESIGNED]**; today a board is a handful of edits in several places.

## What a board has to provide

| Need | Why | If the board lacks it |
|------|-----|-----------------------|
| A display the kernel can drive (an ST7789 works as is; others need a driver, [14](14-adding-a-display.md)) | the shell is on screen | no UI at all (no serial shell) |
| **A key source** (`purr_kernel_key()` returning characters) | the shell and login read it | the system boots and then waits forever at login. There is no serial console input. A real port needs a keyboard, buttons or a serial reader behind `purr_kernel_key()`. |
| Flash for four app-sized slots and a root filesystem | `kittenos`, `kernel`, `loader` are 1.5, 1.875 and 1.5 MB slots | shrink the slots (below) |
| PSRAM | module loading, the loader's downloads, app scratch | no modules, no loader |
| A way into ROM download mode | flashing | |

Current binary sizes (T-Deck Plus build, 2026-09-30): PURR OS 1,085,536 bytes; KittenOS 1,087,344; loader 1,043,088; standalone kernel 331,744;
bootloader 32,560; boot package 4,361. Each app fits a 1.5 MB slot with room to grow.

## The checklist

New files, in the tree, **in all four places that carry a copy**:

| File | Purpose |
|------|---------|
| `PurrOS/components/kernel/boards/<board>.c` | the board profile: `purr_board()` returning a `purr_board_t` ([13](13-writing-drivers.md)) |
| `PurrOS/sdkconfig.board.<board>` | chip-level settings: flash size, partition CSV name, `CONFIG_PURR_BOARD_<X>=y`, PSRAM mode and speed |
| `PurrOS/partitions/<board>.csv` | the partition table |
| `bootpkg/boards/<board>.h` | the boot menu's own copy of the pins and panel |
| `bootloader/sdkconfig.board.<board>` | bootloader: flash size and **its own partition CSV** |
| `bootloader/partitions_<board>.csv` | a copy of the same table for the bootloader project |
| `Kernel/sdkconfig.board.<board>` and `Kernel/partitions.csv` | only if you use the standalone kernel |

Edits to existing files (see `examples/board/new-board.patch`):

| File | Edit |
|------|------|
| `PurrOS/components/kernel/Kconfig` | add `config PURR_BOARD_<X>` to the `PURR_BOARD` choice |
| `PurrOS/components/kernel/CMakeLists.txt` | `if(CONFIG_PURR_BOARD_<X>) list(APPEND srcs "boards/<board>.c") endif()` |
| `purrstrap/scripts/coreos.py` | `BOARDS`: `"<board>": "esp32s3"` |
| `purrstrap/scripts/modules.py` | `BOARDS`: `"<board>": ("xtensa-esp32s3-elf", pimg.CHIP_ESP32S3)` |
| `purrstrap/scripts/bootpkg.py` | `BOARDS`: `"<board>": ("xtensa-esp32s3-elf", 9, "esp32s3.ld")` |
| `bootpkg/src/pkg_hw.c` | only if the board lacks a pin the boot package assumes (see step 7) |

## Step by step

### 1. The board profile

`PurrOS/components/kernel/boards/myboard.c` (shortened):

```c
static const purr_board_t s_board = {
    .name = "myboard",
    .power_pin = PURR_PIN_NONE,
    .power_settle_ms = 0,
    .idle_high_pins = {PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE},
    .spi = {.host = 1 /* SPI2_HOST */, .mosi = 11, .miso = PURR_PIN_NONE, .sclk = 12},
    .display = {
        .name = "ST7789 240x240", .compatible = "st7789",
        .cs = 10, .dc = 9, .rst = 8, .backlight = 7,
        .width = 240, .height = 240, .col_off = 0, .row_off = 0,
        .madctl = 0x00, .bgr = 0, .invert = 1, .spi_hz = 40 * 1000 * 1000,
    },
    .keyboard = {.addr = 0},               /* addr 0 means no keyboard */
};
const purr_board_t *purr_board(void) { return &s_board; }
```

Check each pin against the board's schematic, not a datasheet's default. The `name` must equal the `board=` you will put in manifests.

### 2. Select it (Kconfig and CMake)

```diff
         config PURR_BOARD_CYD_24C
             bool "CYD 2.4C (ESP32-2432S024C)"
+
+        config PURR_BOARD_MYBOARD
+            bool "My board (generic ESP32-S3 dev kit)"
```

```diff
 if(CONFIG_PURR_BOARD_TDECK_PLUS)
     list(APPEND srcs "boards/tdeck_plus.c")
 endif()
+if(CONFIG_PURR_BOARD_MYBOARD)
+    list(APPEND srcs "boards/myboard.c")
+endif()
```

### 3. Chip settings: `PurrOS/sdkconfig.board.myboard`

```
# myboard: ESP32-S3, 8 MB quad flash, 8 MB octal PSRAM
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions/myboard.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions/myboard.csv"
CONFIG_PURR_BOARD_MYBOARD=y

CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
```

Quad or octal PSRAM, flash size and flash mode all depend on the module. Check your module's datasheet; a wrong PSRAM mode fails at boot with a PSRAM
initialisation error. The T-Deck Plus profile is the reference (`PurrOS/sdkconfig.board.tdeck_plus`).

### 4. The partition table

`PurrOS/partitions/myboard.csv` is the T-Deck table with the `root` size changed to fit 8 MB:

```
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     0x9000,  0x6000
otadata,  data, ota,     0xf000,  0x2000
phy_init, data, phy,     0x11000, 0x1000
bootpkg,  data, 0x40,    0x12000, 0xC000
purrcfg,  data, 0x40,    0x1E000, 0x2000
kittenos, app,  factory, 0x20000, 0x180000
kernel,   app,  ota_0,   0x1A0000, 0x1E0000
loader,   app,  test,    0x380000, 0x180000
netrec,   data, 0x41,    0x500000, 0x1000
root,     data, 0x83,    0x501000, 0x2FF000
```

Rules:

- **Keep the names, types and subtypes exactly**: the bootloader, installers and loader find partitions by label and by subtype (`factory`, `ota_0`, `test`, data `0x40`, `0x41`,
  `0x83`). Offsets and sizes may change.
- The sum must fit the flash: here `root` ends at `0x501000 + 0x2FF000 = 0x800000`, exactly 8 MB.
- `bootpkg` is verified from the partition and must keep its size at least the container plus payload (about 4.5 KB now, 48 KB allotted).
- App slots must be 64 KB aligned. They are sized for ESP-IDF images with room to grow. For **4 MB of flash** these slots do not fit (`0x20000 + 0x180000 + 0x1E0000 + 0x180000` is already 4.9 MB):
  shrink them to about 1.2 MB each and watch `purrstrap coreos size`. The monolithic tier in the spec solves this with a different layout that has no code.
- **Keep every copy identical.** There are three CSVs in the repo for one board in principle ([F-30](FINDINGS.md#f-30)): `PurrOS/partitions/`, the bootloader's, the kernel's.

### 5. Teach purrstrap about it

```diff
-BOARDS = {"tdeck_plus": "esp32s3", "cyd_24c": "esp32"}
+BOARDS = {"tdeck_plus": "esp32s3", "cyd_24c": "esp32", "myboard": "esp32s3"}      # coreos.py
```

and the matching lines in `modules.py` and `bootpkg.py` (the table above). Then:

```powershell
python purrstrap/purrstrap.py coreos check          # myboard/minimal, recovery, full: ready to build
python purrstrap/purrstrap.py coreos build --board myboard --profile full
```

Real result: `[ok] build finished`, `PurrOS/build/myboard-full/purros.bin`, and the binary contains the strings `myboard` and `ST7789 240x240`.

### 6. The bootloader project

The bootloader has its own build and **one** shared partition CSV, the T-Deck's. A board with another flash size needs its own:

`bootloader/sdkconfig.board.myboard`:

```
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_myboard.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions_myboard.csv"
```

and `bootloader/partitions_myboard.csv` as a copy of the table above. The file listed **last** in `SDKCONFIG_DEFAULTS` wins, so the board file overrides the shared default.

```powershell
cd bootloader
idf.py -B build_myboard -D SDKCONFIG=build_myboard/sdkconfig -D IDF_TARGET=esp32s3 `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.board.myboard" build
```

Real result: `bootloader.bin` 32,560 bytes, flash size 8 MB. As always, flash only that file, at `0x0` ([04](04-building-and-flashing.md)).

### 7. The boot package

The boot menu has **its own pin table** and its own bit-banged drivers, tuned to an ST7789 and an I2C keyboard. Create `bootpkg/boards/myboard.h` with the same pins as the kernel profile, and:

- **A pin the board lacks** is written as `-1`, but the shipped `pkg_hw.c` drives its pins unconditionally (`pin_hi(-1)` is undefined behaviour). The scratch build added `>= 0` guards: every pin helper ignores a
  negative pin, `hw_init()` skips absent parts, and `hw_key()` returns 0 with no keyboard. It is part of `examples/board/new-board.patch`; the T-Deck package still builds with it.
- A **different panel controller** needs a new init table and helpers in `hw_display_init()` ([14](14-adding-a-display.md)).
- The menu layout assumes a screen about 320x240. Smaller screens clip text (text beyond the edge is not drawn).
- Boards with **no keyboard** will show the menu, which then counts down and boots the default. There is no way to choose, since there are no keys.

```powershell
python purrstrap/purrstrap.py bootpkg build --board myboard
# [ok] bootpkg/build/myboard/bootpkg.bin: 3905 bytes (code 2335, data 1380, bss 8)
python purrstrap/purrstrap.py keys sign --key signing_keys/boot.key --image bootpkg/build/myboard/bootpkg.bin --key-id 1
```

### 8. The standalone kernel (optional)

`Kernel/` has its own `sdkconfig.board.<board>` and **its own `partitions.csv`**. Copy the table there and write the board settings file with `CONFIG_PURR_BOARD_<X>=y`. Not built in the scratch test.

### 9. Flash and bring up

Follow [04](04-building-and-flashing.md) section 4 with your `build_myboard` and `build/myboard-*` outputs, using your flash size where esptool needs it (`--flash_size 8MB` is in the bootloader build's printed command), then the display bring-up in
[14](14-adding-a-display.md). Do the boot menu last: it is the part most likely to misbehave and the one the system can do without.

### 10. Releases

Name components `...-<board>-...` and put `board=<board>` in manifests. The device matches `purr_board()->name` ([10](10-updates-network-install-and-recovery.md)).

## Time and effort, honestly

For a board that is an ESP32-S3 with PSRAM, an SPI ST7789 and a way to type, this is about eleven file edits and no new C beyond the profile. Everything else is hardware the code does not know about: touch, SD, radios, audio, a different panel, a
real keyboard. Those are the driver system's job ([13](13-writing-drivers.md), [24](24-not-built-yet.md)).

Next: [17 Keys and signing](17-keys-and-signing.md).
