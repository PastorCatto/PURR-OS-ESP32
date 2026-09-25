# MagiDOS Example Programs

Simple DOS programs demonstrating the MagiDOS emulator and PURR OS kernel bridge (INT 0xE0).

## Build Setup

### Prerequisites

1. **OpenWatcom v2** — Download from [github.com/open-watcom/open-watcom-v2](https://github.com/open-watcom/open-watcom-v2/releases)
2. **NASM** — [nasm.us](https://www.nasm.us) (for assembly examples)
3. Add OpenWatcom `bin/` to PATH:
   - Linux: `<install>/binl/`
   - Windows: `<install>\binnt\`

Verify installation:
```bash
wcl --version
nasm --version
```

## Examples

### 1. hello.com (C version)

Simple "Hello World" program written in C.

**Build:**
```bash
wcl -mt -0 -os hello.c -fe=hello.com
```

**Test on MagiDOS:**
1. Copy `hello.com` to SD card root
2. Launch MagiDOS app in WCE shell
3. Select `hello.com` from file picker
4. Program prints "HELLO WORLD FROM C!" and waits for keypress

**Compile Flags:**
- `-mt` — tiny memory model (produces COM executable)
- `-0` — target Intel 8086 CPU (no 286+ instructions)
- `-os` — optimize for code size

### 2. hello.asm (Assembly version)

"Hello World" written in pure 8086 assembly.

**Build:**
```bash
nasm -f obj hello.asm -o hello.obj
wlink format dos com file hello.obj name hello.com
```

**Test:** Same as C version above.

### 3. purr_demo.com

Demonstrates PURR OS kernel bridge (INT 0xE0) and shows how DOS programs access WiFi, RAM, and notifications.

**Build:**
```bash
wcl -mt -0 -os purr_demo.c -fe=purr_demo.com
```

**Features:**
- Displays system info (8086 emulator, 640 KB memory, CGA mode)
- Shows how to call INT 0xE0 for WiFi status
- Demonstrates keyboard input handling
- Interactive prompts for user testing

**Test:** Copy to SD card and launch from MagiDOS file picker.

## Testing on PURR OS

### Setup

1. Build PURR OS with MagiDOS enabled:
   ```bash
   cd CoreOS
   idf.py -DPURR_ENABLE_MAGIDOS=1 build
   ```

2. Copy `.COM` files to SD card root (not subdirectories):
   ```bash
   cp hello.com /media/your-sd/
   cp purr_demo.com /media/your-sd/
   ```

3. Flash and run:
   ```bash
   idf.py flash monitor
   ```

### In the WCE Shell

1. Tap "Meow!" (start menu)
2. Tap "Programs >"
3. Tap "MagiDOS"
4. File picker shows `.COM` files on SD card
5. Tap filename to execute
6. Program runs in the window; use arrow keys/Enter for input
7. Tap window close (X) to terminate

## INT 0xE0 — PURR Kernel Bridge

DOS programs can access PURR OS services via software interrupt 0xE0:

```asm
mov ah, 0x20        ; WiFi status command
mov di, offset buf  ; ES:DI = buffer address
int 0xE0            ; Call PURR kernel
```

### Command Codes

| AH | Function | Example |
|----|----------|---------|
| `0x20` | WiFi status | Get connected SSID and RSSI |
| `0x21` | WiFi scan | List available networks |
| `0x22` | WiFi connect | Connect to network |
| `0x10` | LoRa send | Transmit packet |
| `0x11` | LoRa receive | Check for received packet |
| `0x40` | Notification post | Send notification to shell |

**Full documentation:** See `magidos/CoreOS/components/lib_purr_dos_ipc/purr_dos_ipc.h`

## Troubleshooting

### "No .COM/.EXE files found"
- Files must be in `/sdcard/` root, not subdirectories
- File extension must be `.com` or `.exe` (case-insensitive)
- Check SD card is mounted: try Files app first

### Program crashes or hangs
- Check for unsupported DOS INT calls (only safe BIOS interrupts work)
- Check memory usage — programs limited to ~64 KB
- Avoid INT 21h functions that touch hardware

### Screen corruption
- CGA text rendering uses simple character output; fullscreen graphics won't work
- Color attributes might not display correctly on all devices
- Try simpler programs first (hello.com)

## Writing Your Own Programs

### Minimal Template (C)

```c
#include <stdio.h>

int main(void)
{
    printf("Hello from DOS!\n");
    getch();
    return 0;
}
```

Compile: `wcl -mt -0 -os yourprog.c -fe=yourprog.com`

### Important Constraints

- **Memory model:** Tiny (`-mt`) — produces flat 64 KB COM binary
- **CPU:** 8086 only (no 286+ instructions)
- **Interrupts:** Only BIOS (INT 08h-0Fh) and DOS (INT 21h, partial) are safe
- **INT 0xE0:** PURR kernel bridge (custom, safe)
- **Graphics:** CGA text mode only (no VESA, no mode-X)
- **Devices:** No direct hardware access (I/O ports blocked)

### Testing INT 0xE0 Calls

To test WiFi status in your program:

```c
#include <dos.h>

typedef struct {
    unsigned char connected;
    char ssid[33];
    signed char rssi;
} wifi_status_t;

wifi_status_t wifi;

/* Set up registers and call INT 0xE0 */
asm {
    mov ah, 0x20           ; WiFi status
    mov di, offset wifi    ; ES:DI = buffer
    int 0xE0
}

if (wifi.connected) {
    printf("Connected to: %s (RSSI %d)\n", wifi.ssid, wifi.rssi);
}
```

## Resources

- **MagiDOS Architecture:** `magidos/README.md`
- **IPC Commands:** `magidos/CoreOS/components/lib_purr_dos_ipc/purr_dos_ipc.h`
- **8086tiny Emulator:** `magidos/CoreOS/components/drv_8086/8086tiny.c`
- **PURR OS Kernel:** `CoreOS/system/kernel/kitt.cpp`
