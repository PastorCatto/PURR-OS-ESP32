# plan11 - the display link: mirror a device's screen to a second ESP32 (or the Mac), pull a UI from a host, and control a device from outside

> **Note (owner, 2026-11-05): this plan is a manually approved override of the plan series.** The plan files normally stop at plan10 (plan11 is not supposed to exist); the owner approved this one by hand. It is intended for initial development now and for consumer consumption later: the design, the protocol and the wording here are written to be read by developers and, eventually, by the people who use the product, so keep it clear of internal shorthand and of anything private (no keys, passphrases or private hardware details).


Status: design only, written 2026-11-05. **plan11 exists by an explicit owner override** of the rule that stops the plan files at plan10 (the owner: "No, write it as plan 11. I'm allowing an override."). The rule stands for everything else: do not create plan12, and fold other new work into plans 1-10. Nothing here is built. The owner has already started working on the long-term part (two ESP32 devices running at once) elsewhere; this file is the shared design and the first, small steps.

## Why
Three wishes that need the same piece, a link that carries a screen one way and input the other:
1. **Mirror**: a second ESP32 with a display shows what the first one shows, with very little transmitted (over UART or Wi-Fi). Built on the S3 first; the ESP32-P4 later as a bigger, more capable sink.
2. **Remote UI** (long term): the sink does not have to run the same UI as the source. It runs *any* UI and adapts to it, or pulls the UI from a host over Wi-Fi or UART, so two ESP32 devices can run at the same time as one system.
3. **Control from outside**: the same link, with a computer at one end, lets a developer (and Claude) see the device's screen and press its keys without a camera, and run it through a scripted test. This is the first thing worth building, because it needs no second board.

## What exists (checked 2026-11-05)
- The shell draws with LVGL through `purr_lvgl.c` into the panel; the display is a loadable driver module (`purr_display_v2_t`: blit, fill, set_power, set_brightness).
- The `ui` catcall (plan9) already is a serialised operation stream with small integer handles: an app's window is a list of operations (label, button, row, grid, set_text, set_value...) the OS executes, and events come back by handle. That is a ready wire format for remote UI.
- The USB serial link to a plugged-in T-Deck measures about 78 KB/s sustained (a 6.3 MB read in 80 s), and the board file already describes UART pins; Wi-Fi on an S3 gives 1 to 2 MB/s in practice; a UART at 3 Mbaud is about 300 KB/s.
- The VM (QEMU) can hand an emulated UART to a host socket and give the guest user-mode networking (to be checked in `tools/vm`; the owner asked to run hardware checks on the real device, not in the VM, so this is for later).

## Design

### Roles
The **source** is the device being used. The **sink** shows its screen and sends input back. A sink is a build profile of this tree (`sdkconfig.profile.mirror`): display driver, link, input, nothing else (any S3 board with a screen, a second T-Deck first), a **host program on the Mac** (the simulator, also the reference decoder), or later a P4 board. A device can be both (two full devices that each show the other's apps) once remote UI exists.

### One framed link, two transports
A byte-stream protocol that does not care what carries it: COBS framing, CRC-16, sequence numbers, a small window of acks, a handshake that names the modes both ends speak. UART (up to about 3 Mbaud) or TCP over Wi-Fi (the sink announces itself by UDP broadcast, as the dev server does; pairing with a code shown on the sink; frames authenticated with the pairing key). The USB serial link to a computer is a UART for this purpose. A cable needs no encryption; Wi-Fi does.

### Three things that travel
1. **Tiles** (mode 1, first): only the rectangles LVGL redrew, RGB565, run-length and delta coded against the last frame. A still screen costs nothing, a tap a few KB, a full 320 x 240 frame about 15 to 40 KB coded (150 KB raw). Hooked in the LVGL flush callback: after the panel copy the changed area is queued to an encoder worker (PSRAM stack, no flash access; newest frame wins when the link is slow). Works with a sink that has no UI code, including the Mac.
2. **UI operations and shell state** (mode 2, the minimal one, and the road to remote UI): app windows as the `ui` operation stream, a few bytes per change, drawn by the sink's own LVGL in the sink's own skin and resolution; the shell's chrome (status strip, home, notifications, lock) as a small state record (time, battery, radio bars, Wi-Fi, accent, wallpaper, current page). Because the sink draws it, it adapts to its own screen; and in the other direction a host can *send* a UI to a sink, which is "pull in the UI from the host".
3. **Input and control** (back channel): keys, touch and trackball from the sink to the source; and, for a developer, **control commands**: press a key, tap a point, take a screenshot (a full tile frame), read the Task Manager numbers, run a shell command. Control is a setting of its own, administrator only, off by default, and refused on a locked device.

### Privacy and the locked-device rules
Mirroring sends the screen out of the device, so it is an administrator setting, off by default, with a visible "mirroring to X" mark in the status strip. Password fields are masked already and never sent as text. A locked device is never weakened: the lock screen can be mirrored, but input and control from outside are ignored while it is locked, and a sink cannot turn developer mode or any key or trust setting on.

### Bandwidth in one line
Raw frames over UART are about half a second each, too slow; tiles make it comfortable. Over USB to the Mac a screenshot is a fraction of a second.

### ESP32-P4 later
It has no Wi-Fi of its own (boards pair it with a C6 over ESP-Hosted; to be verified), so UART or the hosted link. As a sink it brings a large MIPI display, hardware scaling (320 x 240 up to a big panel) and an H.264 decoder, which is also the way to a video player the S3 cannot do alone.

## The virtual GPU (the owner's name for the whole thing, 2026-11-05)
Ideas recorded while the design was still open; none of it is decided except where it says "decision".
**Three layers, each useful alone.**
1. **A display manager in the OS.** Today the OS assumes one display (`purr_kernel_display()`, and the shell sizes itself from `w10m_disp_w()` / `w10m_disp_h()`). It becomes a registry of displays, each with a profile: width, height, orientation, density, and whether it has touch, a keyboard or both. LVGL 9 supports several displays natively (one `lv_display_t` each, with its own draw buffers, flush callback and input devices), so the work is in the shell's single-instance globals, not in LVGL.
2. **A virtual display backend (the link).** A display whose "panel" is a link to a sink. The OS sees an ordinary display; the backend turns draws into frames on the wire.
3. **An adaptive UI.** The shell and the apps lay themselves out from the display's profile (size, density), not from 320 x 240.
**Pushing pixels or a VNC-style remote framebuffer: the recommendation is to build it around damage rectangles with pull.** The source keeps a framebuffer in PSRAM and a list of what changed; the sink asks for updates (the way the remote framebuffer protocol behind VNC works), so a slow link or a big screen simply gets fewer, coalesced updates, flow control is built in, and several encodings (raw, run-length, tile-delta) can sit behind one request. A plain push to a local panel is the special case where the sink always asks at once. The internal link protocol stays small; a **host bridge on the Mac speaks the real remote framebuffer protocol to any VNC viewer**, so the Mac's own viewer works as a sink with no custom host software, and the bridge is also where input comes back from. (Unauthenticated screen sharing must not be offered over Wi-Fi: pairing and authentication belong to the link, not to a viewer.)
**Two displays at once.** *Mirror* first: the second display shows the first one's content, scaled or letterboxed (a tee in the flush path, as in the phases). *Extend* later: apps' windows (the `ui` catcall windows) placed on a chosen display; the shell's own chrome stays on the primary until its single-instance state is split, which is the large step.
**A development platform without the hardware (the owner: purely for QEMU and the host tools, not part of the on-device link).** The idea is to adapt to different display sizes (the M5Stack Tab5 is the example: an ESP32-P4 board with, as I remember it, a 5-inch 1280 x 720 touch display and a C6 for Wi-Fi; to be checked against M5Stack's own documents) without owning the board and without reverse engineering it:
- **Display profiles as data**, next to the board files in `sdk/boards/` (a `tab5` profile: 1280 x 720, density, capacitive touch, no keyboard). The OS and the host tools read the same profile.
- **The host simulator** (`tools/lvgl_host`, which already builds the real LVGL, theme and wizard pages on the Mac and renders them to PNG) grows into a viewer with selectable profiles and mouse as touch, so layouts for a 1280 x 720 screen can be worked on with no device at all. This is the cheapest and most useful first step.
- **Density buckets for the pictures**, as Android does (the Froyo icon pack at 1x and 2x, chosen by profile), and larger fonts than today's 12 to 40 pt (LVGL's scaling or a vector font).
- **Chip support is separate**: the ESP32-P4 is a different IDF target (RISC-V, MIPI-DSI display, no Wi-Fi of its own). Vendor board support and schematics come from the vendor; this plan does not depend on it.
**Decision:** the name is the PURR OS virtual GPU (the owner wrote "PerOS"; the product name is used in the plans), and the first sink is a program on the Mac, because it needs no second board.

## How hard is a mirror with shared touch? (estimate, 2026-11-05)
Two identical S3 devices, one screen mirrored to the other, a touch on either one acting on the same UI, over UART first. **Moderate: roughly 1,200 to 1,800 lines of C plus host tests, and the cost is dominated by testing on the two real boards, not by writing.**
- *Easy parts* (clean seams already exist): the tile codec and framing (plain C, host-testable); a tee in the LVGL flush callback into a PSRAM shadow framebuffer; the sink (display driver plus tile blit, no LVGL needed); injecting remote touch and keys through an LVGL input device.
- *Medium:* the UART transport with flow control (3 Mbaud is about 300 KB/s, a coded full frame 15 to 40 KB, so scrolling needs a frame-rate cap with newest-wins); pairing and authentication for Wi-Fi; arbitration when both ends touch at once (last writer wins); sleep and lock interplay (what the sink shows when the source sleeps; nothing from outside acts on a locked device).
- *Hard, and not part of the GPU:* making a UI that looks right on a *different* size or density. Scaling a mirror is easy and ugly; adapting the layout is the adaptive-UI layer and is the large piece.
- *Feel:* a tap round trip over UART (event out, redraw, tile back) is roughly 50 to 150 ms: fine for taps and typing, noticeably laggy for dragging and flick scrolling, because the source cannot animate ahead of the finger.
- *Cheapest way to prove most of it:* phase 2 with the Mac as the sink, over the USB link that is already there. It exercises the flush tee, the codec, the link and the input injection; a second board then adds only the sink profile.

## The emulator as the second device (owner's idea, 2026-11-05)
Run the `mirror` sink firmware in the owner's emulator (`ESP32-Emulator`, a Rust application around Espressif's QEMU) and connect a real T-Deck to it, over Wi-Fi or UART: the emulated device is the "second ESP32". What the emulator already offers, from its README and `boards/t-deck-plus-vm.toml`: boards are data (a display with width and height, touch, SD card, LoRa, GPS as a UART device), a serial console, and a virtual Ethernet controller; the Wi-Fi radio is **not** emulated (only scanning is simulated), so in the emulator the OS reaches a network over Ethernet. So:
- **Over the network:** the sink listens on the emulator's Ethernet, with a host port forward; the real T-Deck connects to the Mac's address and port (its Wi-Fi to the LAN, then through the forward). No radio emulation needed.
- **Over a UART:** a new `uart` device in the board file for the link, bound by the emulator to a host serial port, pty or socket. A real T-Deck's USB port is its *console*, so a dedicated link means either wiring two of its GPIO pins to a USB-UART adapter on the Mac, or multiplexing the link onto the USB console with the framing (the protocol already has sequence numbers and CRCs, so log lines and frames can share the stream; esptool and the link then cannot use the port at the same time).
- **A virtual display of any size** is a change to the board file (the `[display]` width and height), if the emulator's display model allows it: the natural place for a virtual 1280 x 720 "Tab5" profile, which is the "purely for QEMU" part of the idea.
- **What it buys:** the sink and the protocol tested end to end with no second board, the real T-Deck's mirror tee and touch injection tested against it, and a screen in a window for screenshots. **What it costs:** the emulator is a separate project (its changes are the owner's to approve), emulated timing is not the real board's, and the standing rule is that the emulator is only run when the owner asks.
- **What the emulator's own session notes add** (read from the earlier Claude sessions for `ESP32-Emulator` and its `docs/vm-hardware.md`, 2026-11-05; the notes themselves say nothing in the hardware contract is implemented or verified yet):
  - *Architecture:* Espressif's QEMU fork emulates the CPU, memory and flash; the project adds the missing SPI and I2C bus controllers to QEMU; the display, touch, keyboard and SD card are Rust devices that talk to QEMU over a socket protocol called vpb. Scope was set to the **ESP32-S3 only** (no P4); everything is S3 registers, ROM image and bit-banged-bus decoders.
  - *Networking, already working:* the VM board (`t-deck-plus-vm`) has a **W5500 Ethernet controller on its own SPI bus** (SPI3, chip select GPIO 9), modelled in the emulator's Rust shell in MACRAW mode so ESP-IDF's own `esp_eth` driver runs unchanged; PURR OS's VM build is configured for it (`PURR_ETH_CS_GPIO`) and a small VM-info device on I2C tells the OS it is a VM. Frames cross to QEMU's user-mode network (DHCP, DNS, NAT) as UDP datagrams, and the emulator's own hand-off note records the guest getting `10.0.2.15`. There is no Wi-Fi radio in the VM: it is wired Ethernet only. User-mode NAT is outbound, so a real T-Deck reaching a sink in the VM needs a host port forward to the guest's listening port (or the sink dials out to the T-Deck, which is the simpler direction).
  - *A link without a second board:* the design also lists **USB host passthrough** (the S3's DWC2 host mapped in QEMU, a physical device passed through with `usb-host`). Passing the real T-Deck's USB serial into the VM would give a UART-like link with no adapter, if the VM firmware has a CDC host driver. Design only for now.
  - *Display:* the panels are SPI devices behind vpb (the board file sets the size); a large MIPI-DSI panel is a different class of device and belongs to the P4, whose QEMU support is unknown (the notes say to check `qemu-system-riscv32 -machine help`), PURR OS has no P4 target yet, and IDF 5.3.5 treats the P4 as a preview target. The advice there: keep the adapter contract chip-neutral (frames, transfers) so a later P4 reuses it.
  - *So:* the VM can already be the second device over TCP, through its Ethernet. What it lacks is a **UART/serial path to the outside** for the T-Deck, which is the USB-host passthrough above or a host-side bridge. The emulator's change history is the owner's, and the emulator is only run when the owner asks.
- **Order:** the Mac-side Python sink first (fast, no emulator), then the emulator as sink, then a second real board.

## Phases
| # | Phase | Proves |
|---|-------|--------|
| 0 | Display profiles as data, and the host simulator drawing the real shell at a chosen size (320 x 240, then 1280 x 720) | the layout can adapt to other sizes, no hardware |
| 1 | Framing and the tile codec in coreos (plain C, host tests) and a Mac-side sink/controller (Python): decode tiles, write PNGs, send keys | the codec, with no hardware |
| 2 | The flush hook and a UART/USB transport on the device; a screenshot and a key press from the Mac on a real T-Deck | control from outside, the first useful result |
| 3a | The `mirror` profile running in the emulator as the sink (Ethernet or a link UART), a real T-Deck as the source | the sink and the protocol end to end, no second board |
| 3b | The `mirror` profile on a second S3 (UART between two boards) | mirror to a second device |
| 4 | Wi-Fi transport, discovery, pairing, authentication | the same over the air |
| 5 | UI-operations mode and shell state; the sink in its own skin | remote UI, adapting to the sink |
| 6 | A UI pulled from a host; two devices running at once | the long-term goal |
| 7 | P4 sink, scaling, video | the bigger sink |

## Decisions (owner, 2026-11-05)
1. A second ESP32 with a display is the first sink; minimal transmission is the point.
2. Wi-Fi and UART both, one protocol.
3. S3 first, the P4 later.
4. Long term: any UI on the sink, or a UI pulled from the host, two devices running at once (the owner has started on this elsewhere).
5. It also gives a way to control a device from outside (for development, and for Claude).
6. plan11 is allowed as an override.

## Open
Which second board first (another T-Deck, or an S3 board with a bigger screen); resolution and orientation (scale or letterbox); mirror versus "extend" (a second screen that shows something different); what the control commands may run (the terminal's rule applies: no flashing, no key or trust commands); how this meets the work the owner has already started.

## Status
| Phase | State |
|-------|-------|
| 0 to 7 | not started |
