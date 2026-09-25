# MagicMac — Retro68 App SDK

Build classic Mac OS apps that talk to the PURR OS kernel via the IPC memory window.

## Prerequisites

- [Retro68](https://github.com/autc04/Retro68) installed and on PATH
  (`RetroCC`, `Rez`, `MakePEF`, `hfsutils`)
- A Mac Plus ROM image (`mac.rom`, 512 KB) placed in `CoreOS/spiffs_image/mac.rom`

## Build an app

```sh
cd SDK/retro68/apps/purr_lora_chat
make
# outputs: purr_lora_chat.dsk  (HFS floppy image, drag into Mac disk window)
```

## IPC usage from a Mac app (C, Inside Macintosh conventions)

```c
#include "PurrIPC.h"

// Send a LoRa message
PurrLoRaSend("hello purr", 10);

// Poll for incoming LoRa / notifications
PurrNotification notif;
if (PurrNotifyPoll(&notif)) {
    // notif.type == kPurrNotifyLoRa or kPurrNotifyText
}

// Get WiFi status
PurrWiFiStatus st;
PurrWiFiGetStatus(&st);
```

## How it works

`PurrIPC.c` writes a `purr_ipc_frame_t` to absolute address `0xF00000` in the
68k address space, sets `status = IPC_STATUS_PENDING`, then spin-polls until
`status == IPC_STATUS_DONE`. The ESP32 side services the call in under ~1 ms
for most operations.

Do not call IPC routines from interrupt level.
