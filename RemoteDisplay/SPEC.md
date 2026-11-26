# Remote display spec (draft 0.1, owner decision 2026-11-25)

A small board with a screen and no memory to spare (the CYD 2.4C, later the Heltec V3) boots a tiny image and behaves as the **screen and the touch panel of a PURR host**: a
T-Deck Plus or any stronger ESP32-S3 running PURR OS. The host draws; the client shows what it is sent and reports what is touched. Nothing of the UI runs on the client.

This is "option A" of the design talk: **pixels, not widgets**. The host sends the rectangles of its own screen that changed; the client writes them straight to its panel. It works
with any shell (Froyo, the Type shell, the Click Wheel) and needs no LVGL, no assets and no framebuffer on the client. A cache of static bitmaps and a copy-region command
(section 5) are in the protocol from the start so the traffic can be cut later without a new version.

## 1. Roles and what each needs

| | Host | Client |
|---|---|---|
| Chip | ESP32-S3 with PSRAM (T-Deck Plus first) | ESP32 (CYD 2.4C: 4 MB flash, no PSRAM), later an S3 with no PSRAM (Heltec V3) |
| Runs | a PURR shell and apps; the **remote display service** | the **client image**: boot, Wi-Fi, the link, the panel and touch drivers, the decoder, the crypto |
| Memory it must find | a copy of each changed rectangle (from LVGL's flush), a compress buffer | a receive buffer and a decode chunk of about 16 KB each; no framebuffer (the panel has its own) |
| Never | | LVGL, a file system, BLE (one radio: BLE and Wi-Fi together cost RAM and throughput) |

The first client has the **same resolution as the T-Deck's panel (320 x 240)**, so mirroring is 1:1 and a touch at (x, y) is a touch at (x, y) on the host. A client of another size gets a
host-side scale or its own layout; that is a later decision (Heltec's 128 x 64 monochrome panel cannot mirror a 320 x 240 shell).

## 2. Finding each other (two modes)

The host must stay connected to the user's Wi-Fi while it serves a client, so the default asks nothing of the host's radio.

**Mode 1, same network (default).** The client joins the same Wi-Fi as the host (its credentials are set once, over the pairing link or by the user). It finds the host with a UDP
broadcast probe on **port 47802** (the same pattern as `purr_lan_discover.c`, port 47800, which finds the dev server). The host answers with an ANNOUNCE (section 3.1). The session is TCP, **port 47803**.
Nothing extra runs on the host's radio.

**Mode 2, no shared network.** The host switches on its own access point named `PURR-RD-<id>` (the last four hex digits of its device id) and the client joins it. The S3 can run its client and access
point at once, but **both must use the same channel**: when the host is also connected to a router the access point takes the router's channel (the client just follows) and the radio's air
time is shared. With no router the access point runs alone. This mode is the one with a cost (some tens of KB of internal RAM and power; not measured): it runs only while a client is paired and asking for it.

**Proximity.** In Mode 2 the client scans for `PURR-RD-*` and joins the strongest one that it is paired with (or, unpaired, the strongest above a signal threshold). In Mode 1 the host and the
client can also announce themselves with ESP-NOW broadcasts, which work while the host is connected to a router and carry the received signal strength; the first version may use the LAN probe alone.
"Closest" is a hint: only a host the client is paired with is ever connected to without the user's say.

## 3. The session

Everything after discovery is one TCP connection (port 47803) of **messages**, each with an 8-byte header, all numbers little-endian.

```
u8  type
u8  flags      (0 for now; bit 0 = the payload is encrypted, section 4)
u16 seq        (a counter per direction, for the encryption nonce and for debugging)
u32 len        (payload bytes, at most 65536; a larger value ends the session)
```

### 3.1 Discovery (UDP 47802)

PROBE (client to broadcast): the 4 bytes `PRD1`, the client's device id (8 bytes). ANNOUNCE (host to the sender): `PRD1`, `u8` kind 1, `u16` tcp port, `u8` flags (bit 0: accepting
new pairings now), the host's device id (8 bytes) and a name (up to 24 bytes, NUL padded). A host that is not paired with the sender and is not accepting pairings stays silent.

### 3.2 Messages

| Type | Name | From | Payload |
|---|---|---|---|
| 0x01 | HELLO | client | `u8` major, `u8` minor, `u16` width, `u16` height, `u8` pixel formats (bit 0 RGB565, bit 1 MONO1), `u8` codecs (bit 0 RAW, bit 1 RLE16, bit 2 LZ4), `u16` caps (bit 0 FILL, bit 1 COPY, bit 2 CACHE, bit 3 touch, bit 4 keys), `u16` max pixels per rectangle, `u16` receive buffer bytes, 16-byte board name, 64-byte public key (P-256, x then y) |
| 0x02 | WELCOME | host | `u8` major, `u8` minor, `u8` chosen pixel format, `u8` codecs the host will use, `u16` width, `u16` height, `u16` max pixels per rectangle, `u8` frames the client may leave un-acknowledged (window), `u8` reserved, `u32` session id |
| 0x03 | PAIR | both | section 4.1 |
| 0x04 | AUTH | both | section 4.2 |
| 0x10 | RECT | host | `u16` x, y, w, h; `u8` codec (0 RAW, 1 RLE16, 2 LZ4); `u8` pixel format; then the data. RAW is w x h x 2 bytes (RGB565, little-endian), or MONO1: rows of ceil(w/8) bytes, most significant bit first |
| 0x11 | COPY | host | `u16` source x, y, width, height, destination x, y: move a region already on the panel (a scrolled list); the client needs no memory for it if the panel can read back, otherwise it refuses COPY in its caps |
| 0x12 | PRESENT | host | `u32` frame id: everything since the last PRESENT is one frame; the client may now show it |
| 0x13 | FILL | host | `u16` x, y, w, h, `u16` colour: a flat area (the commonest thing in a UI, and a few bytes) |
| 0x20 | INPUT | client | `u8` kind (0 touch down, 1 touch move, 2 touch up, 3 key down, 4 key up), `u8` touch id, `u16` x (or the key code), `u16` y |
| 0x21 | ACK | client | `u32` frame id: this frame has been drawn |
| 0x30 | PING | both | `u32` token |
| 0x31 | PONG | both | the same token |
| 0x3F | BYE | both | `u8` reason |
| 0x7F | ERROR | both | `u8` code, then text |
| 0x40 | CACHE_STORE | host | reserved (section 5) |
| 0x41 | CACHE_BLIT | host | reserved (section 5) |

**Flow control.** The host never has more than `window` frames un-acknowledged. A frame is the rectangles between two PRESENTs; the client draws each rectangle as it arrives and ACKs at PRESENT.
**The host splits every rectangle into pieces of at most `max pixels` before it compresses them**, so the client's decode chunk is the size it said. A client that is slow just ACKs late.
**Damage.** The host collects what LVGL redrew (its flush areas), merges rectangles that overlap or are close (a bounded set: when there are too many, the two that cost least to join are joined),
compresses each piece with the codec that comes out smallest (RAW if nothing helps), sends FILL for a flat area, and PRESENTs.
**Codecs.** RAW; RLE16 (a control byte: high bit set = a run of `n+1` copies of the next 2-byte pixel, clear = `n+1` literal pixels follow); LZ4 (the standard LZ4 block format, on the pixel bytes).
The decoder is told the exact pixel count it must produce, and refuses anything that overruns it.

## 4. Security

A paired display sees everything the host's screen shows and can touch it, so it is held to the host's own standard.

1. **First pairing (the client has no key for this host).** HELLO carries the client's public key. The host asks its user ("Allow *name* to use this screen?", on the host's own screen) and, if the user
   agrees, both sides run an ephemeral ECDH (P-256) and each shows the same **six digits** (a hash of both public keys and both nonces). The user compares them: this is what stops a third device from sitting between the two.
   The host then stores the client's public key and the client stores the host's. Pairing is only accepted while the host's user has asked for it (Settings, "Add a display", or a held key).
2. **Later sessions.** AUTH is a challenge and response signed with the stored keys (ECDSA P-256) over the transcript of the exchange, and a fresh ECDH gives the session key. A client the host does not know is refused with no
   screen data.
3. **Encryption.** After AUTH every message from the next one on has its payload encrypted and authenticated (AES-128-GCM; the nonce is the direction and `seq`). A message that fails the check ends the session. The header is not secret.
4. **Input.** A client can send touches and keys, nothing else: no commands, no files. While the host is **locked**, the client shows the lock screen and its input is ignored (display only) unless the host's setting "Unlock from a paired display" is on (off by default);
   the PIN applies as it does on the host's own screen.
5. **Revocation.** Settings lists the paired displays; removing one deletes its key on the host, and it is refused from then on. A client forgets a host from its own pairing screen.
6. **Pairing Wi-Fi (Mode 2 only).** Before a pairing the host's access point uses a published pairing passphrase (it protects against nobody: the six digits do that); after it, the passphrase is derived from the shared secret.

## 5. The cache (reserved, not in version 1.0 hosts)

CACHE_STORE (`u16` id, size, then the bitmap) lets the host leave an icon or wallpaper on the client; CACHE_BLIT (`u16` id, `u16` x, y) draws it. The client declares how much it has in HELLO caps bit 2 and a later
field. A host never sends either until the client's caps bit is set, and a client that sees one it did not ask for answers ERROR.

## 6. The client image ("remote" profile)

A classic-ESP32 PURR image in the same signed container, signed with the same boot key (chip id esp32), that boots, shows a **searching** screen, finds a host (section 2), pairs or reconnects, and then only decodes.
It carries Wi-Fi, the panel driver (ILI9341 over SPI, DMA, the next chunk received while the last is written), the touch driver (CST816S, I2C), the decoder, the crypto and the pairing keys (NVS). The CYD's 4 MB flash holds the
bootloader (which sits at 0x1000 on a classic ESP32, the S3 boards use 0x0; the same bootloader source, built with the board file `bootloader/sdkconfig.board.cyd_24c`), a partition table **at 0xC000** (the
current bootloader is 38 KB, too big to fit under 0xA000 from 0x1000), `nvs`, `purrcfg`, and one app slot of 1.6 MB (`kernel`); the 2.25 MB behind it is free for a second slot of the same size for an
update the host can push (the client checks the signature before it switches). The table is `PurrOS/partitions/cyd_24c.csv`.

## 7. Versions and tests

`major` changes when a message is no longer understood by an old peer; `minor` adds messages and flags an old peer can ignore (a peer ignores types it does not know, after reading their length).
The wire code (header, messages, the three codecs, rectangle splitting and damage merging) is plain C shared by the host and the client: `Modules/vdisplay/purr_rdisp.c`, host-tested
(`test_rdisp.c`). The link itself needs hardware (the emulator has no Wi-Fi): the first end-to-end test is a client on a real CYD and a script on a computer that streams frames, measuring throughput and latency
before the host service is written.

**First measurement (2026-11-25, `tools/rdisp/measure.py`, the real codecs on five emulator screenshots of the Click Wheel and Froyo shells):** a whole 320 x 240 screen, 153,600 bytes raw,
comes out at **4.4 to 12.9 KB** (12 to 35 times smaller), about 3 to 9 ms on a 1.5 MB/s link. The panel's own write (SPI at 40 MHz) is about 31 ms for a full screen, so **the panel, not the link, is the limit**.
Static pictures only: no measurement yet of a live session, of small changes (a highlight moving), or of the radio under load.

**Host service built and tested in the emulator (2026-11-25).** `PurrOS/main/purr_rd_host.c`: the LVGL flush callback (`purr_lvgl.c`) copies every redrawn rectangle into a shadow of the screen in PSRAM and records the damage;
a task (stack in PSRAM) answers discovery probes on UDP 47802, accepts one display on TCP 47803, sends the changed rectangles (FILL for flat areas, else the smallest of RAW, RLE16 and LZ4), waits for ACKs (window of 3), and turns the
client's touches into a second LVGL pointer device (ignored while the host is locked). It works with any shell. It is **off on a real device until Settings, System, "Remote display" is turned on** (administrators only; the setting is
`remote.display`), and on by default inside a VM (the one runtime flag), where it is a test fixture. Tested with `tools/rdisp/test_client.py` (a stand-in for the Mini: real codecs through ctypes) against the Froyo image in the
emulator, reached through a QEMU user-network port forward (a local patch to the emulator's `qemuctl`: the environment variable `PURR_QEMU_USER_OPTS`): WELCOME, 39 frames, 103 rectangles and 21 fills (370,507 pixels in 73.7 KB, a tenth of raw), and a tap at the Apps button opened the app drawer on the host.
**Still missing:** pairing and encryption (section 4), so "on" means any display on the same network may look at this screen and touch it (when unlocked); the host's pairing prompt; a "Displays" page.

## 8. Open

Several clients per host (the protocol allows it; the first host serves one). The Heltec V3 profile (MONO1, 128 x 64, one button, and a UI of its own). Whether mirroring needs a scale for other panels. Audio (none).
