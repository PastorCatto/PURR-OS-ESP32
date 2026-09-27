# MagiDOS

MagiDOS is a PURR OS application that runs a DOS emulator inside a MiniWin window. It uses 8086tiny as the CPU/emulator core. When opened, it scans the SD card for `.COM` and `.EXE` files and lets the user tap one to run it. DOS programs can call PURR OS kernel services (LoRa, WiFi, Bluetooth, notifications) via a custom software interrupt (`INT 0xE0`).

Unlike MagicMac, MagiDOS is not a shell. It runs under `purr_wm` as a regular app alongside whatever other windows the user has open.

---

## How it works

```
purr_wm (MiniWin window manager)
    |
magidos_app  (purr_wm_app/magidos/)
    |
drv_8086  ----  8086tiny emulator
    |                 |
lib_purr_dos_ipc    8086 address space
    |              INT 0xE0 = PURR kernel
PURR kernel               |
(LoRa / WiFi / BT)   DOS app executes INT 0xE0
                      AH = command code
                      DS:SI / ES:DI = args/results
                      ESP32 dispatches and returns
```

When a DOS app wants a PURR service it sets `AH` to a command code and executes `INT 0xE0`. The emulator intercepts the interrupt, calls `purr_dos_ipc_dispatch()`, which reads the registers and memory, dispatches to the correct PURR kernel driver, writes the result back into emulated memory, and returns. From the DOS app's perspective it looks and feels exactly like a standard DOS INT 21h call.

CGA text mode output (80x25 characters) is rendered into the MiniWin window each frame using a software character rasterizer and the standard IBM CGA colour palette.

---

## Project structure

```
magidos/
  CoreOS/components/
    drv_8086/               8086tiny emulator IDF component
      drv_8086.h            public API (init, load, step, key inject, frame callback)
      drv_8086.c            implementation, hooks for INT 0xE0 and VRAM updates
      8086tiny.c            (vendored — see setup step 2)
      CMakeLists.txt

    lib_purr_dos_ipc/       INT 0xE0 bridge to PURR kernel
      purr_dos_ipc.h        command codes, register convention, structs
      purr_dos_ipc.c        dispatch table, LoRa/WiFi/BT/notification handlers
      CMakeLists.txt

  purr_wm_app/magidos/      MiniWin app
    magidos_app.h           magidos_register() declaration
    magidos_app.cpp         WM registration, window lifecycle, emulator task
    magidos_filepicker.h    SD card file picker interface
    magidos_filepicker.cpp  Scans /sdcard for .COM/.EXE, touch-selectable list
    magidos_cga.h           CGA text renderer interface
    magidos_cga.cpp         char+attr to RGB565, IBM CGA palette
    CMakeLists.txt

  SDK/openwatcom/           DOS-side developer kit
    PurrDOS.h               C header (structs, function declarations)
    PurrDOS.asm             NASM real-mode INT 0xE0 stubs
    apps/
      purr_lora_chat/       Example app: LoRa chat in a DOS text-mode window
        main.c
```

---

## Setup

### 1. Prerequisites

**On the ESP32 side (IDF firmware):**
- ESP-IDF v5.3.5
- SD card mounted at `/sdcard` (SPI bus, already set up by PURR OS `pm_init`)
- `purr_wm` component already integrated (lives in `Shells/purr_wm/`)

**For building DOS apps:**
- [OpenWatcom v2](https://github.com/open-watcom/open-watcom-v2/releases) — free, targets 16-bit real mode
- [NASM](https://www.nasm.us/) — to assemble `PurrDOS.asm`
- Optional: `hfsutils` or `mtools` if you need to pack files into a floppy image

### 2. Vendor 8086tiny

8086tiny is not included. Clone it into `CoreOS/components/drv_8086/`:

```sh
cd magidos/CoreOS/components/drv_8086
git clone https://github.com/adriancable/8086tiny .
```

Then open `CMakeLists.txt` and confirm `8086tiny.c` is listed in `SRCS`. The vendored file should be named `8086tiny.c` — if the clone uses a different name, update the CMakeLists accordingly.

### 3. Wire the INT 0xE0 hook

8086tiny calls a user-supplied function when it encounters an unhandled interrupt. In `drv_8086.c`, the hook is named `hook_purr_int()`. Wire it by editing 8086tiny's main loop:

Find the interrupt dispatch section in `8086tiny.c` (look for `INTF` or the `INT` opcode handler). Add a check before the normal BIOS dispatch:

```c
if (i_data0 == 0xE0) {   // i_data0 = interrupt number
    hook_purr_int();
    return;
}
```

The exact variable name for the interrupt number depends on the 8086tiny version. Check the source after cloning.

### 4. Wire the VRAM hook

After each write to the CGA VRAM region (`0xB8000`–`0xBFFFF`), call `hook_vram_update()` so the MiniWin window redraws. In 8086tiny, memory writes go through a single macro or function — add the check there:

```c
// After any memory write:
if (addr >= 0xB8000 && addr < 0xC0000)
    hook_vram_update();
```

### 5. Register MagiDOS with purr_wm

In `CoreOS/CMakeLists.txt`, add the MagiDOS component paths to `EXTRA_COMPONENT_DIRS`:

```cmake
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_SOURCE_DIR}/../magidos/CoreOS/components/drv_8086"
    "${CMAKE_SOURCE_DIR}/../magidos/CoreOS/components/lib_purr_dos_ipc"
    "${CMAKE_SOURCE_DIR}/../magidos/purr_wm_app/magidos"
)
```

In whichever file calls `purr_wm_init()` (typically `system/system/main.cpp` or the active shell), add:

```cpp
#include "magidos_app.h"
// after purr_wm_init():
magidos_register();
```

### 6. Flash and test

```sh
cd CoreOS
idf.py -p /dev/ttyUSB0 flash monitor
```

MagiDOS appears as an app in the `purr_wm` launcher. Tap it to open. If the SD card has no `.COM` or `.EXE` files the window shows a message and closes after 3 seconds.

### 7. Put a DOS program on the SD card

Copy any real-mode `.COM` or `.EXE` to the root of the SD card. Simple single-file DOS programs work best. Programs that require extended memory (XMS/EMS), protected mode (DOS4GW, DPMI), or CD-ROM access will not run.

---

## Building DOS apps for MagiDOS

### Toolchain setup (OpenWatcom)

```sh
# Download OpenWatcom v2 installer from:
# https://github.com/open-watcom/open-watcom-v2/releases
# Install, then add to PATH: <install>/binl (Linux) or <install>\binnt (Windows)

# Verify:
wcl --version
nasm --version
```

### Assemble the PURR stubs

```sh
cd magidos/SDK/openwatcom
nasm -f obj PurrDOS.asm -o PurrDOS.obj
```

### Build the example app

```sh
cd magidos/SDK/openwatcom/apps/purr_lora_chat

# Compile and link as a real-mode COM file
wcl -mt -0 -os main.c ../../PurrDOS.obj -fe=purr_lora_chat.com
```

Flags:
- `-mt` — tiny memory model (COM output)
- `-0` — target 8086 instructions only
- `-os` — optimize for size

Copy `purr_lora_chat.com` to the SD card root and it will appear in the MagiDOS file picker.

### INT 0xE0 command reference

| AH | Command | DS:SI (in) | ES:DI (in) | CX (in) | Result |
|----|---------|-----------|-----------|---------|--------|
| `10h` | LoRa send | payload ptr | — | payload length | — |
| `11h` | LoRa recv | — | buffer ptr | buffer size | AX = bytes (0 if none) |
| `20h` | WiFi status | — | `PurrDOSWiFiStatus*` | — | struct filled |
| `21h` | WiFi scan | — | buffer ptr | buffer size | JSON null-terminated |
| `22h` | WiFi connect | `ssid\0pass\0` | — | — | — |
| `30h` | BT list | — | buffer ptr | buffer size | JSON null-terminated |
| `40h` | Notify post | text ptr | — | — | — |
| `41h` | Notify poll | — | `PurrDOSNotif*` | — | ZF clear if present |

Full struct definitions are in [SDK/openwatcom/PurrDOS.h](SDK/openwatcom/PurrDOS.h).
ASM stubs for each call are in [SDK/openwatcom/PurrDOS.asm](SDK/openwatcom/PurrDOS.asm).

---

## Status

Work in progress. The IDF components and WM app compile. The following still need to be done before MagiDOS is functional:

- Vendor 8086tiny into `drv_8086/` and wire `hook_purr_int()` and `hook_vram_update()`
- Wire actual 8086tiny register file into `purr_dos_ipc_dispatch()` (replace `ah/ds/si/es/di/cx` stubs)
- Implement the touch-selectable file picker UI in `magidos_filepicker.cpp` (currently auto-selects first file)
- Implement MZ EXE loader in `drv_8086.c` (COM only for now)
- Load a full CP437 8x8 font into `magidos_cga.cpp` (replace the stub glyph table)
- Wire `purr_wm_window_draw_text()` and `purr_wm_window_blit()` to the actual `purr_wm` API
