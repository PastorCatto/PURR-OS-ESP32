# Catcalls spec (draft 0.1)

The device interfaces that apps and the layers above the kernel use, beyond display. The
registry, the two ABIs and the display interface are in
`../PurrOS/components/kernel/SPEC.md` sections 7 and 8. Permissions are in
`../Permissions/SPEC.md`. **Each catcall here follows the native rules** (versioned table,
optional functions marked by feature bits), and the legacy adapters map the old catcalls onto
these where they overlap.

## 1. The catcalls

| Catcall | Purpose | Permission |
|---------|---------|------------|
| `display` | Draw pixels (kernel spec) | none for apps, `ui` later |
| `input` | Keys, touch and scroll events | foreground only, `input-monitor` otherwise |
| `console` | Standard input, output and error | normal |
| `sched` | Threads, sleep, timers | normal |
| `app` | An app's own info and requests | normal |
| `storage` | Files in the app's own folder | normal, `shared-storage` for anything else |
| `net` | TCP and UDP sockets, DNS | `internet`, normal |
| `wifi` | Scan and connect | `wifi` |
| `radio` | Raw radio packets | `radio` |
| `gps` | Parsed position fixes | `gps` |
| `gps_raw` | The raw GPS stream | `gps-raw` |
| `gpio`, `i2c`, `spi` | Direct hardware | `hardware-raw` |
| `system` | Battery, time, memory, uptime (read-only) | normal |
| `audio` | Audio output | normal (following Android) |
| `appmgr` | Install, remove and export apps | `device-admin` |
| `update` | Stage an update and request it | privileged apps only |
| `ui` | Widgets and a canvas, backend-neutral (`../UI/SPEC.md`) | none |

A catcall exists only where a driver provides it. An app that asks for one the board does not
have gets a clear "not available" answer.

## 2. input

One catcall delivers all input as typed events from every device. Each event says which
device it came from.

**Event types**

- **key:** the key's USB HID usage code, the typed character in Unicode if the key produces
  one, the modifier state, the action (press, release or repeat), and a timestamp
- **pointer:** touch or pointer, with x and y in **screen pixels in the display's current
  orientation**, already calibrated and rotated by the driver, the action (down, move, up),
  and a pointer id
- **scroll:** a trackball or wheel, with a horizontal and vertical step

**Delivery.** Each app has a small queue that it reads, waiting for the next event with a
timeout. When an app is too slow and the queue fills, the oldest events are dropped. Only the
foreground app receives events. `input-monitor` allows keys while the app is not focused.

**Legacy.** The old `touch` and `input` catcalls are mapped onto this one.

## 3. storage

- Ordinary Unix-style calls: open, read, write, seek, close, directory listing, stat, delete,
  make directory and rename.
- The app sees only its own data folder as its root, so it cannot reach other apps' files. The
  folder is under the logged-in user's home, so each user has their own (`../Users/SPEC.md`).
- Anything outside that folder, including the SD card, needs the `shared-storage` permission.

## 4. sched

- **Threads.** Apps may create threads. The default cap is a small number (4). An app that
  needs more declares it in its manifest, and the runtime checks the request against the
  available memory when the app starts. Each thread's stack size follows the manifest.
- Creating a thread past the cap fails with an error and never affects other apps.
- Sleep and timers are here too, and everything created through the catcall is owned by the
  calling app, so stopping the app releases it.

## 5. Network

- **`net`:** sockets for TCP and UDP, and DNS. Following Android, plain internet access is a
  normal permission, `internet`, granted at install.
- **`wifi`:** scanning and joining networks, under the `wifi` permission.
- Bluetooth is deferred to its own chunk.

## 6. radio

Raw, and only raw: set the parameters (frequency, bandwidth, spreading factor, power) and send
and receive packets. Mesh protocols live above it, in apps or in vendor-signed modules, so the
OS stays neutral and each vendor can implement their own protocol on the same hardware.

## 7. GPS

- **`gps`:** parsed fixes: latitude, longitude, altitude, speed, time, satellite count and fix
  quality.
- **`gps_raw`:** the raw receiver stream (NMEA text), a separate catcall behind its own
  permission, `gps-raw`. An app with plain `gps` does not get it.

## 8. Direct hardware

`gpio`, `i2c` and `spi` are separate catcalls, so each one is small and easy to describe in a
permission prompt. The kernel's pin registry refuses any pin a driver already owns and any bus
a driver shares, so an app cannot break the display, radio or storage.

## 9. system

Read-only: battery level and state, the time, free memory and uptime. Nothing sensitive is in
it, so it is a normal permission. Changing anything (brightness, time) stays under
`system-settings`.

## 10. audio

- **Output only for now.** The microphone is a separate later catcall with its own
  `audio-input` permission, because recording has privacy implications.
- **Format:** 16-bit PCM. The app picks the sample rate and mono or stereo, and the system
  converts to what the codec supports.
- **One owner at a time.** The foreground app, or the one that started playing most recently,
  has the speaker. The interface is designed so a **mixer** can be added later without
  changing it: each app opens its own stream, and the audio service decides what to play.
- **Optional per board.** Some boards have audio and some do not. A codec such as the ES8311 on
  the Waveshare ESP32-S3-ePaper-1.54 is controlled over I2C and streams over I2S, which is two
  buses, so under the driver rule it needs a code driver.

## 11. Testing

- Fake drivers for each catcall, with the version matching and the legacy adapters
  (`../PurrOS/components/kernel/SPEC.md` section 12).
- Input: event ordering, queue overflow drops the oldest, foreground-only delivery, and the
  legacy `touch` mapping.
- Storage: an app cannot escape its own folder, including with `..` and symbolic paths.
- Thread cap: the default, a manifest request, and refusal when memory is short.
- Pin registry: an app cannot take a pin a driver owns.
- Permission checks for every row in the table in section 1.

## 12. Open questions

- **TLS.** Whether apps get an HTTPS client through a catcall, or bring their own.
- **Storage quotas** per app.
- **Multi-touch and gestures,** and key repeat timing.
- **Keyboard layouts and remapping,** possibly through data-description drivers.
- **Audio buffer sizes and latency.**
- **Sensors,** such as the SHTC3 temperature and humidity chip on the Waveshare board, and
  whether they get a catcall.
- **Notifications and vibration.**
- **The e-paper display,** which needs its own refresh rules on the display catcall.
- **The Bluetooth and microphone catcalls,** both deferred.
