# PURR OS — In-House Exclusives

The in-house exclusives are PURR OS's two flagship `.claw` apps: **MagicMac** and **MagiDOS**. Both are full computer emulators that run inside PURR OS on ESP32-S3 hardware, and both live at `source/apps/exclusive/`.

> **Build status, as of DP8 — they are not equals.**
>
> | | In the firmware? | Why |
> |---|---|---|
> | **MagiDOS** | **Yes** | Has an `app.pcat`, so `modulestrap`'s exclusive scan picks it up and it links in as a pre-linked `.claw`. Runs under [Speed Demon](15_SpeedDemon.md). |
> | **MagicMac** | **No** | Has no `app.pcat`. `modulestrap` only scans directories containing one, so it is never registered as a component and never enters a build — confirmed absent from `components_manifest.cmake`. |
>
> The sections below describe both designs as intended. Treat MagicMac's as a
> design document rather than a description of shipping behaviour until it gains
> an `app.pcat` and is verified on hardware.

---

## MagicMac

**Source:** `source/apps/exclusive/magicmac/`  
**Tier:** `.claw` (kernel-access, full catcall API)  
**Best device:** jc3248w535 (8MB PSRAM, 480×320 display)

MagicMac emulates a **Macintosh Plus** running Mac OS System 3–7. The Mac Finder acts as the app launcher. Apps built with the Retro68 toolchain run natively inside the emulator and can call PURR OS kernel services (LoRa, WiFi, Bluetooth) through a memory-mapped IPC bridge at 68k address `0xF00000`.

### How it works

```
catcall_display_t (PURR OS display)
       │
  purr_classic (.claw entry point)
       │
  drv_umac  ─────  umac + Musashi 68k emulator
       │                     │
  lib_purr_ipc         68k address space
       │              0xF00000 = IPC window
  purr_kernel_*                │
  (LoRa / WiFi / BT)      Mac app writes here
                           ESP32 intercepts
                           response written back
```

The Mac framebuffer (512×342, 1bpp) is scaled and converted to RGB565 on each frame and blitted to the display via `catcall_display_t->push_pixels()`. No direct SPI/GPIO calls — it all goes through the catcall.

### IPC Bridge

When a Mac app wants to call a PURR OS service:

1. Mac app writes a command struct to 68k address `0xF00000`
2. Sets the status byte to `PENDING`
3. The ESP32 side intercepts the write via a memory hook in the umac emulator
4. `lib_purr_ipc` dispatches to the appropriate kernel service
5. Response is written back into the IPC window
6. Status byte set to `DONE`
7. Mac app sees `DONE` and reads the response

Round-trip latency: under 1ms for most calls.

### IPC Command Reference

| Command | Direction | Description |
|---------|-----------|-------------|
| `0x01` | Mac → ESP32 | Send LoRa packet |
| `0x02` | ESP32 → Mac | Receive LoRa packet |
| `0x10` | Mac → ESP32 | Get WiFi status |
| `0x11` | Mac → ESP32 | Scan WiFi networks (JSON response) |
| `0x12` | Mac → ESP32 | Connect to WiFi network |
| `0x20` | Mac → ESP32 | List paired BT devices (JSON) |
| `0x21` | Mac → ESP32 | Pair BT device |
| `0x30` | Mac → ESP32 | Post notification |
| `0x31` | ESP32 → Mac | Poll for pending notification |

### Source Layout

```
magicmac/
  CoreOS/components/
    drv_umac/                 umac + Musashi 68k emulator IDF component
      umac_core.h             public API (init, start, mouse event, frame callback)
      umac_core.c             implementation (PSRAM/heap ROM + RAM allocation)
      CMakeLists.txt
    lib_purr_ipc/             IPC bridge
      purr_ipc.h              command IDs, frame struct, ESP32-side API
      purr_ipc.c              dispatch loop, LoRa/WiFi/BT/notification handlers
      CMakeLists.txt
  Shells/purr_classic/        .claw entry point
    purr_classic.h
    purr_classic.cpp          wires catcall_touch, catcall_display, launches emulator + IPC tasks
    CMakeLists.txt
  SDK/retro68/                Mac-side developer kit (68k apps)
    PurrIPC.h                 C header for Mac apps
    PurrIPC.c                 writes to 0xF00000, spin-polls DONE
    README.md
    apps/
      purr_lora_chat/         Example: LoRa chat in a Mac window
```

### Emulator Dependencies (not vendored — legal)

- **umac** — Mac Plus emulator core ([github.com/evansm7/umac](https://github.com/evansm7/umac))
- **Musashi** — 68k CPU core ([github.com/kstenerud/musashi](https://github.com/kstenerud/musashi))
- **Mac Plus ROM** — 512KB ROM dump (copyrighted Apple IP, not included)
  - Place at `/sdcard/magicmac/mac.rom` (SPIFFS fallback: `/flash/mac.rom`)

```bash
cd source/apps/exclusive/magicmac/CoreOS/components/drv_umac
git clone https://github.com/evansm7/umac umac
git clone https://github.com/kstenerud/musashi musashi
```

### Building Mac Apps (Retro68)

```bash
# Install Retro68
git clone https://github.com/autc04/Retro68
cd Retro68 && ./build-all.sh

# Build example app
cd source/apps/exclusive/magicmac/SDK/retro68/apps/purr_lora_chat
make
# → purr_lora_chat.dsk  (Mac disk image with the app inside)
```

Copy the `.dsk` to `/sdcard/magicmac/` — the Mac sees it as a floppy disk.

### Building with catstrap

```bash
catstrap build magicmac
# → cattobaked/apps/magicmac.claw
```

### Implementation Status

| Feature | Status |
|---------|--------|
| Boot mode infrastructure | ✅ Done |
| Frame rendering (1bpp → RGB565 scaled) | ✅ Done |
| ROM loading from SD | ✅ Done |
| WiFi/BT disable in MagicMac mode (saves RAM) | ✅ Done |
| IPC bridge framework | ✅ Done |
| Touch input (ADB mouse) | ⏳ TODO |
| Keyboard mapping (T-Deck → ADB) | ⏳ TODO |
| Vendor umac + Musashi | ⏳ TODO (legal — user must clone) |
| Wire `umac_register_ipc_window()` to actual umac API | ⏳ TODO |
| Disk selection from magicmac.json | ⏳ TODO |

---

## MagiDOS

**Source:** `source/apps/exclusive/magidos/`  
**Tier:** `.claw` (kernel-access, full catcall API)  
**Best device:** jc3248w535 (8MB PSRAM) or tdeck_plus (keyboard)

MagiDOS runs a **DOS 8086 emulator** inside a MiniWin window. It scans the SD card for `.COM` and `.EXE` files and presents a touch-selectable file picker. DOS programs run full-screen in the window with CGA text mode (80×25 characters, IBM CGA palette). DOS apps can call PURR OS kernel services via a custom software interrupt (`INT 0xE0`).

Unlike MagicMac, MagiDOS is not a shell — it runs alongside other MiniWin windows as a regular app.

### How it works

```
MiniWin window manager (miniwin .purr module)
       │
  magidos_app  (.claw entry point)
       │
  drv_8086  ─────  8086tiny emulator
       │                   │
  lib_purr_dos_ipc    8086 address space
       │              INT 0xE0 = PURR kernel
  purr_kernel_*               │
  (LoRa / WiFi / BT)     DOS app executes INT 0xE0
                          AH = command code
                          DS:SI / ES:DI = args/results
```

CGA VRAM at `0xB8000`–`0xBFFFF` is monitored by a hook in the emulator. On each write, `magidos_cga` converts character+attribute bytes to RGB565 using the standard IBM CGA palette and pushes the updated region via `catcall_display_t->push_pixels()`.

### INT 0xE0 Command Reference

| AH | Command | DS:SI (in) | ES:DI (out) | CX | Result |
|----|---------|-----------|------------|-----|--------|
| `10h` | LoRa send | payload ptr | — | payload length | — |
| `11h` | LoRa receive | — | buffer ptr | buffer size | AX = bytes |
| `20h` | WiFi status | — | `PurrDOSWiFiStatus*` | — | struct filled |
| `21h` | WiFi scan | — | buffer ptr | buffer size | JSON |
| `22h` | WiFi connect | `ssid\0pass\0` | — | — | — |
| `30h` | BT list | — | buffer ptr | buffer size | JSON |
| `40h` | Notify post | text ptr | — | — | — |
| `41h` | Notify poll | — | `PurrDOSNotif*` | — | ZF clear if present |

### Source Layout

```
magidos/
  CoreOS/components/
    drv_8086/                 8086tiny emulator IDF component
      drv_8086.h              public API
      drv_8086.c              hooks for INT 0xE0 + VRAM updates
      CMakeLists.txt
    lib_purr_dos_ipc/         INT 0xE0 → PURR kernel bridge
      purr_dos_ipc.h          command codes, register convention
      purr_dos_ipc.c          dispatch table, service handlers
      CMakeLists.txt
  purr_wm_app/magidos/        MiniWin app
    magidos_app.h/.cpp        window lifecycle, emulator task
    magidos_filepicker.h/.cpp SD card .COM/.EXE picker
    magidos_cga.h/.cpp        CGA text renderer (char+attr → RGB565)
    CMakeLists.txt
  SDK/openwatcom/             DOS-side developer kit
    PurrDOS.h                 C header (structs, function declarations)
    PurrDOS.asm               NASM real-mode INT 0xE0 stubs
    examples/
      hello.c / hello.asm
      purr_demo.c
    apps/
      purr_lora_chat/         Example: LoRa chat in a DOS text window
```

### Emulator Dependency (not vendored)

- **8086tiny** — 8086 CPU + PC emulator ([github.com/adriancable/8086tiny](https://github.com/adriancable/8086tiny))

```bash
cd source/apps/exclusive/magidos/CoreOS/components/drv_8086
git clone https://github.com/adriancable/8086tiny .
```

### Building DOS Apps (OpenWatcom)

```bash
# Install OpenWatcom v2: https://github.com/open-watcom/open-watcom-v2/releases
# Add to PATH: <install>/binl (Linux) or <install>\binnt (Windows)

# Assemble PURR stubs
cd source/apps/exclusive/magidos/SDK/openwatcom
nasm -f obj PurrDOS.asm -o PurrDOS.obj

# Build a COM app
cd apps/purr_lora_chat
wcl -mt -0 -os main.c ../../PurrDOS.obj -fe=purr_lora_chat.com
```

Copy `.com` or `.exe` to SD card root — MagiDOS file picker shows them automatically.

### Building with catstrap

```bash
catstrap build magidos
# → cattobaked/apps/magidos.claw
```

### Implementation Status

| Feature | Status |
|---------|--------|
| IDF components (drv_8086, lib_purr_dos_ipc, purr_wm_app) | ✅ Done |
| CGA character renderer (magidos_cga) | ✅ Done (stub font) |
| SD card file picker (magidos_filepicker) | ✅ Done (auto-selects first file) |
| Vendor 8086tiny + wire INT 0xE0 hook | ⏳ TODO (user must clone) |
| Wire register file into purr_dos_ipc_dispatch | ⏳ TODO |
| Touch-selectable file picker UI | ⏳ TODO |
| MZ EXE loader | ⏳ TODO (COM only currently) |
| Full CP437 8×8 font | ⏳ TODO (stub glyph table) |

---

## Comparing the Two

| | MagicMac | MagiDOS |
|-|---------|---------|
| Emulated hardware | Macintosh Plus | IBM PC (8086) |
| CPU core | Musashi (68000) | 8086tiny |
| Display | 512×342 1bpp framebuffer | CGA text 80×25 |
| Window style | Full-screen shell | MiniWin window |
| App format | 68k Mac apps (Retro68) | DOS `.com` / `.exe` |
| PURR API | Memory-mapped IPC @ 0xF00000 | `INT 0xE0` |
| Best device | jc3248w535 (PSRAM) | jc3248w535 / tdeck_plus |
| PSRAM required | Yes (ROM + 128KB RAM) | Recommended (8086 RAM) |
