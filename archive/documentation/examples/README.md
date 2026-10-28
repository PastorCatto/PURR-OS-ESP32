# Examples

Source files used by the documentation. **Everything here compiles**: the kernel files were built into the real PurrOS
`full` profile in a scratch copy of the tree, the modules were built with `purrstrap modules build`, and the tools were run.
**None of it was run on hardware.** These are not part of any build; copy what you need.

| File | Used by | What it is |
|------|---------|-----------|
| `modules/greet_module.c` | [12](../12-writing-modules.md) | the smallest `/system` module (adds `greet`) |
| `modules/count_kmod.c` | [12](../12-writing-modules.md) | the smallest `/kernelmods` kernelmod (adds `count`) |
| `modules/i2cscan_module.c` | [13](../13-writing-drivers.md) | the `i2cscan` command; needs the kernel-table change shown there (ABI 7) |
| `kernel/ili9341.c` | [14](../14-adding-a-display.md) | an ILI9341 display driver derived from `st7789.c`. Put in `PurrOS/components/kernel/src/`. |
| `kernel/st7789_multi.c` | [15](../15-adding-a-second-display.md) | a **replacement** for `st7789.c` that supports two panels at once |
| `kernel/ssd1306.c` | [15](../15-adding-a-second-display.md) | an I2C SSD1306 OLED driver |
| `kernel/second-display.patch` | [15](../15-adding-a-second-display.md) | the edits to existing kernel files (`git apply`-style unified diff) |
| `board/new-board.patch`, `board/files/` | [16](../16-adding-a-board.md) | the edits and new files that add a made-up board `myboard` (ESP32-S3, 8 MB flash, SPI ST7789). The full profile, the boot package and the bootloader were built for it. |
| `tools/mkmanifest.py` | [18](../18-making-your-own-purr-os.md) | writes a recovery manifest from files in a folder |
| `tools/mkcfg.py` | [05](../05-boot-process-and-flash-layout.md) | builds a `purrcfg` flash image with a chosen `secure_mode` |
| `tools/cat2c.py` | [12](../12-writing-modules.md) | turns a signed `.cat` into a C byte array for `commands.c` |

Paths inside the patch start with `PurrOS/`, so apply from the repository root. Check it applies to your tree first:
`git apply --check documentation/examples/kernel/second-display.patch`. It was generated against the tree state of
2026-09-30; later edits to those files may need a manual merge.
