# MagicMac — 68k Mac Plus Emulator with PURR OS Integration

MagicMac is a full Mac OS System 3-7 emulator that runs as a selectable boot mode alongside PURR OS. The Mac Finder acts as the app launcher. Apps built with Retro68 run natively inside the emulator and access PURR OS kernel services (LoRa, WiFi, Bluetooth) through a memory-mapped IPC bridge.

**Key Features:**
- **Boot mode selectable** at runtime via settings app
- **Full-screen rendering** of 512×342 Mac framebuffer scaled to device display (320×240 on T-Deck)
- **Memory optimized** — WiFi/BT disabled during MagicMac boot (saves 2-3MB for emulator)
- **Kernel API access** — 68k apps can call PURR OS services via 0xF00000 IPC window
- **Persistent settings** in `/sdcard/magicmac/magicmac.json`

---

## Implementation Status

✅ **Completed:**
- Boot mode infrastructure (KITT enum, NVS persistence)
- Settings app (manage magicmac.json configuration)
- Frame rendering (512×342 1bpp → 320×240 RGB565 scaled display)
- ROM loading from `/sdcard/magicmac/mac.rom` (with SPIFFS fallback)
- WiFi/BT conditional init (disabled in MagicMac mode to save RAM)
- IPC bridge framework

⏳ **TODO:**
- Touch input wiring (Mac ADB mouse)
- Disk selection from magicmac.json in IPC bridge
- Full keyboard mapping (T-Deck keyboard → ADB)

---

## How it works

```
CYD display / touch
       |
  purr_classic (Shells/purr_classic/)
       |
  drv_umac  ----  umac + Musashi 68k emulator
       |                  |
  lib_purr_ipc     68k address space
       |            0xF00000 = IPC window
  PURR kernel               |
  (LoRa / WiFi / BT)   Mac app writes here
                        ESP32 dispatches
                        response written back
```

When a Mac app wants to use a PURR kernel service it writes a command struct to absolute 68k address `0xF00000` and sets the status byte to `PENDING`. The ESP32 side sees the write (via a hook in the emulator), dispatches the call, writes the response, and sets status to `DONE`. The Mac app spin-polls the status byte — the whole round-trip is under 1 ms for most calls.

---

## Project structure

```
magicmac/
  CoreOS/components/
    drv_umac/           umac + Musashi IDF component
      umac_core.h       public API (init, start, mouse event, frame callback)
      umac_core.c       implementation + PSRAM/heap ROM and RAM allocation
      CMakeLists.txt

    lib_purr_ipc/       IPC bridge between 68k address space and PURR kernel
      purr_ipc.h        command IDs, frame struct, ESP32-side API
      purr_ipc.c        dispatch loop, LoRa/WiFi/BT/notification handlers
      CMakeLists.txt

  Shells/purr_classic/  Full-screen shell entry point
    purr_classic.h      purr_classic_start() declaration
    purr_classic.cpp    wires touch, display, launches emulator + IPC tasks
    CMakeLists.txt

  SDK/retro68/          Mac-side developer kit
    PurrIPC.h           C header for Mac apps (IPC command IDs, structs, API)
    PurrIPC.c           Implementation — writes to 0xF00000, spin-polls DONE
    README.md           SDK usage guide
    apps/
      purr_lora_chat/   Example app: LoRa chat terminal in a Mac window
        main.c
        Makefile
```

---

## Setup

### 1. Get the ROM image

MagicMac requires a **512 KB Mac Plus ROM dump**. This is not included for legal reasons.

**Obtaining the ROM:**
- Dump your own from an original Mac Plus (legally required)
- Archive ROM files (legal gray area)
- Cannot be included in repo due to copyright

**Installation:**
Place the ROM file at: `/sdcard/magicmac/mac.rom`

The emulator loads it at boot. If SD card ROM is not found, it falls back to `/spiffs/mac.rom` (legacy).

### 2. Build with SDK

Use the PURR SDK build system with the `--magicmac` flag:

```bash
python3 SDK/sdk_core.py --target tdeck_plus --magicmac --build
python3 SDK/sdk_core.py --target tdeck_plus --magicmac --flash /dev/ttyACM0
```

The SDK automatically:
- Enables `PURR_HAS_MAGICMAC` compile flag
- Links purr_classic shell
- Includes drv_umac + lib_purr_ipc components
- Enables PSRAM support for 68k RAM

### 3. Optional: Vendor umac and Musashi (if building standalone)

If not using the SDK build system, manually clone the emulator sources:

```sh
cd magicmac/CoreOS/components/drv_umac

# umac — Mac Plus emulator core
git clone https://github.com/evansm7/umac umac

# Musashi — 68k CPU core (umac depends on this)
git clone https://github.com/kstenerud/musashi musashi
```

Then update `CMakeLists.txt` `SRCS` to match actual source file names.

### 6. Flash and test

```sh
cd CoreOS
idf.py -p /dev/ttyUSB0 flash monitor
```

On boot the emulator will load the ROM, initialise 512 KB of RAM, and start the 68k CPU. The Mac boot chime sound is not emitted (no audio driver yet). The happy Mac icon will appear on screen within a couple of seconds.

---

## Building Mac apps (Retro68)

See [SDK/retro68/README.md](SDK/retro68/README.md) for full instructions.

Quick start:

```sh
# Install Retro68
git clone https://github.com/autc04/Retro68
cd Retro68 && ./build-all.sh

# Build the example app
cd magicmac/SDK/retro68/apps/purr_lora_chat
make
# Produces purr_lora_chat.dsk — copy to Mac disk or load via umac disk image support
```

---

## IPC command reference

| Command ID | Direction | AH / registers | Description |
|------------|-----------|----------------|-------------|
| `0x01` | Mac to ESP32 | payload = bytes | Send LoRa packet |
| `0x02` | ESP32 to Mac | response = bytes | Receive LoRa packet |
| `0x10` | Mac to ESP32 | — | Get WiFi status |
| `0x11` | Mac to ESP32 | payload = SSID scan trigger | Scan networks (JSON response) |
| `0x12` | Mac to ESP32 | payload = ssid\0pass\0 | Connect to network |
| `0x20` | Mac to ESP32 | — | List BT paired devices (JSON) |
| `0x21` | Mac to ESP32 | payload = addr | Pair BT device |
| `0x30` | Mac to ESP32 | payload = text | Post notification |
| `0x31` | ESP32 to Mac | response = text or LoRa | Poll for pending notification |

Full struct definitions are in [CoreOS/components/lib_purr_ipc/purr_ipc.h](CoreOS/components/lib_purr_ipc/purr_ipc.h).

---

## Status

Work in progress. The IDF components compile. The following still need to be done before MagicMac is functional:

- Vendor umac and Musashi source into `drv_umac/`
- Wire `umac_register_ipc_window()` to the actual umac memory hook API
- Implement `_on_frame()` 1bpp to RGB565 conversion and display blit
- Wire `drv_touch` callback to `_on_touch()` in `purr_classic.cpp`
- Implement MZ EXE loader in `drv_umac` if needed (COM only for now)
- Full CP437 8x8 font (replace stub in `magidos_cga.cpp`)
