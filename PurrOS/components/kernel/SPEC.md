# Kernel spec (draft 0.2)

The kernel handles hardware, drivers and the filesystem, and nothing else. It is a full
rewrite. It starts first: on modular boards it is a file in `/boot` that the boot package
loads into PSRAM and runs, and on monolithic boards it is part of the packed image. It
starts CoreOS, which runs on top of it, and the app runtime and the product use it to
reach the display, touch, storage and so on. Layering and boot order are in
`../../SPEC.md`.

## 1. Responsibilities

- Describe the hardware: a **board profile** lists the buses and devices.
- Load drivers and bind each to its device (**plug and play**).
- Own the shared resources: pins and buses.
- Mount the root filesystem (LittleFS) and give the layers above file access.
- Publish what each driver offers through **catcalls**, the interfaces the
  rest of the system calls.
- Report what came up and what failed, without deciding what a failure means.

Non-goals: security decisions, policy, user accounts, UI, applications, the
network stack. Anything that is not hardware belongs in a layer above or below.

## 2. Plug and play

"Plug and play" here means adding hardware support never edits the kernel:

- **Add a driver** = add one source file and register it. The kernel finds it,
  matches it to a device in the board profile and binds it.
- **Add a board** = add one board profile file. No driver changes.
- Drivers never hard-code pins or buses. They receive them from the profile
  through the kernel.
- The kernel detects conflicts (two devices claiming the same pin) at start and
  reports them instead of letting them misbehave.

Acceptance test for this rule: a fake driver and a fake board are added in the
host tests with no change to kernel code.

Finding hardware on a board the kernel does not know is covered in `../../../Install/SPEC.md`.

Hot-plug (SD card inserted, USB) is a later step. The driver contract leaves
room for it (section 5).

## 3. Board profile

A typed C table, checked at compile time. One profile per board, chosen by the
`PURR_BOARD` option. It lists:

- **Buses**: type (SPI or I2C), host, pins, default speed.
- **Devices**: name, `compatible` string (which driver), bus, chip-select or
  address, extra pins (reset, interrupt, backlight), and a priority.
- **Config values** drivers need (display size, rotation, and so on).

Priority reuses the old levels: `required` (the product cannot run without it),
`important`, `optional`. The kernel only records the level and reports failures
against it. CoreOS and the product decide what to do.

A later purrstrap step may generate the table from a declarative file. It is
written by hand until then. The direction is that the profile becomes data in the
device config bundle (section 13), so a new board needs no kernel change.

## 4. First board: CYD 2.4C (ESP32-2432S024C)

Original ESP32, 4 MB flash, no PSRAM. From the old archive profile
(`archive/DP9/code/source/devices/cyd_s024c/device.pcat`):

| Device | Bus | Pins (GPIO) |
|--------|-----|-------------|
| Display, ILI9341-compatible, 240x320 | SPI | CS 15, DC 2, MOSI 13, SCLK 14, RST none, backlight 27 |
| Touch, CST816S capacitive | I2C | SDA 33, SCL 32, INT 21, RST 25 |
| SD card (later) | SPI | CS 5, MOSI 23, MISO 19, SCLK 18 |

Status of this data:

- The archive marks backlight 27 as verified (not 21 as on other CYD boards).
  Everything else is carried over and is **to be confirmed on the board** at
  bring-up. Nothing here has been checked against hardware in this rewrite.
- The display MISO pin is not in the archive. The usual CYD wiring uses GPIO 12,
  but it is only needed if the driver reads the controller ID back.
- Some CYD batches ship a different display controller than the listing says.
  The driver should read the controller ID at probe time and report a mismatch,
  not assume.

## 5. Driver modules

A driver is a descriptor plus functions:

```
purr_driver_t {
    name,
    compatible[],          strings it can drive
    abi_version,
    probe(device, kernel_services)   -> handle or error
    remove(handle)
}
```

- Registered at build time through a linker section, so a driver only has to
  declare itself with one macro. There is no central list to edit.
- The kernel walks the board profile in priority order. For each device it
  finds a driver whose `compatible` matches, claims the pins, adds the device
  to its bus and calls `probe`. On success the driver registers its catcall.
- Every device ends in one recorded state: `bound`, `no_driver`,
  `probe_failed` (with the error) or `pin_conflict`.
- The kernel never panics on a failed driver. It records the failure and goes on.
- `remove` exists from the start so hot-plug and driver reload can be added
  without changing the descriptor.

v1 drivers are compiled into the kernel. Separate driver modules, with
the same container, come later. KittenOS does not load the kernel file. It links a
small fixed driver set from the same driver sources.

## 6. Buses and pins

- The kernel owns the SPI and I2C hosts. A driver asks the kernel for a device
  on a bus and gets a handle. It never initialises a host itself.
- Shared buses are locked by the kernel, so two drivers on one bus cannot
  interleave a transaction.
- The kernel keeps a pin registry. A claim names the owner, and a second claim
  on the same pin fails with a message naming both.
- The first version needs SPI (display) and I2C (touch). SD reuses SPI later.

## 7. Catcalls: two ABIs

The interfaces drivers publish. There are two ABIs and both keep working.

**Legacy ABI (generation 1).** The old catcalls from the previous OS: display,
touch, input, radio and GPS, each a typed struct with a `catcall_version` field,
plus the `purr_kernel_register_*` and `purr_kernel_*` entry points.

- The headers are copied into `include/legacy/` and then frozen. They are never
  edited. A change to a legacy interface is a new generation.
- The legacy entry points wrap the registry. They keep the old rule that the
  last registration wins.
- Purpose: code and drivers written for the old catcalls keep working.

**Native ABI (generation 2).** The catcalls designed for PURR OS.

- A catcall is an interface ID plus a table that begins with a header
  (`struct_size`, `major`, `minor`, `features`), followed by function pointers.
- `register(id, table, flags)`, `get(id, min_major, min_minor)`, `unregister`,
  and `list` for diagnostics.
- Matching: major must equal, minor must be at least the requested one.
  A mismatch returns an error, never a wrong-shaped table.
- `features` is a bitmask for optional functions. An optional function that a
  driver does not provide is NULL and its feature bit is clear, so consumers
  check the bit and the minor version does not have to change.
- A second registration for the same ID is an error unless the caller passes
  the replace flag. The legacy wrappers always pass it.
- Fixed-size table (16 entries), mutex-guarded, no heap use.

**Bridging.** The registry tags every entry with its generation. When a caller
asks for a generation that has no provider, the kernel builds an adapter from
the other one:

- A native provider can be used by legacy consumers, and the reverse.
- Adapters are per interface, small and written by hand. Features that exist
  only in the native version return `ESP_ERR_NOT_SUPPORTED` through the legacy
  view.
- Rule: a native interface is not accepted until its legacy adapter exists in
  both directions, or it is marked as having no legacy equivalent.
- If both generations have a provider, the requested one is returned as is.

Legacy compatibility covers catcall interfaces only. Loading old `.purr`
binaries is not part of it.

## 8. Display API (the first native interface)

Native display interface, id `display`, version 2.0.

**Types**

- Pixel format: `RGB565` only at first, with other values reserved. Pixel data
  is host-endian 16-bit. If the panel wants the bytes swapped, the driver does
  it, so callers never know.
- `display_info`: width and height in the panel's native orientation, pixel
  format, supported rotations, `max_chunk_pixels` (largest blit the driver
  wants in one call), and a name string.

**Functions**

| Function | Purpose | Optional |
|----------|---------|----------|
| `get_info(info*)` | Panel size, format and limits | no |
| `blit(x, y, w, h, pixels)` | Copy a rectangle, returns when the data is safely sent | no |
| `fill(x, y, w, h, color)` | Solid rectangle | no |
| `set_rotation(rotation)` | 0, 90, 180 or 270 | yes |
| `set_brightness(level 0..255)` | Backlight | yes |
| `set_power(on)` | Sleep in and out | yes |
| `blit_async(x, y, w, h, pixels, done_cb, arg)` | Start a copy and call back when the buffer can be reused | yes |
| `wait_idle()` | Block until all queued transfers are done | yes |

Rules:

- Coordinates outside the panel are clipped by the driver, and a rectangle with
  no visible area succeeds without doing anything.
- Every function returns `esp_err_t`. Optional functions the driver lacks are
  NULL, with the feature bit clear.
- `blit` may be called from several tasks. The kernel's bus lock serialises them.
  `done_cb` runs in a task or interrupt context that the header states, and
  must not block.
- There is no frame buffer in the API. On a board without PSRAM the caller draws
  in chunks (the graphics library on top renders partial buffers), and the
  driver DMA-copies each one.

**Legacy view (generation 1 `catcall_display_t`)**

| Legacy | Native |
|--------|--------|
| `push_pixels` | `blit` |
| `fill_rect` | `fill` |
| `set_brightness` | `set_brightness` |
| `get_info` | `get_info` |
| `init(cfg)` | Succeeds if the device is bound. Pins in the legacy config are ignored, since the board profile owns them. Rotation is applied. |
| `deinit` | Calls `remove` |

## 9. First driver: ILI9341 display

- SPI at the profile's speed, DMA, chunk size set by Kconfig (default 16 lines
  of 240 px RGB565 = 7,680 bytes, two buffers).
- Init sequence, rotation and colour order are data tables, so a controller
  variant is a table change.
- Backlight through the LEDC peripheral (PWM), so `set_brightness` gives real
  dimming. A plain GPIO on/off is the fallback.
- Reads the controller ID at probe when MISO is wired, and reports a mismatch
  with the expected ID.
- Ends with a **test pattern app**: colour bars, gradient, a moving rectangle,
  to check size, orientation, colour order and tearing on the real board.

The CST816S touch driver is next after the display, through the same driver
contract and a touch catcall designed the same way.

## 10. What the boot package and CoreOS get

`purr_kernel_entry_t` (the boot package calls it, and it starts CoreOS):

- `init(boot_info)`: run section 5 for every device in the profile, mount the root
  filesystem, and start CoreOS.
- `status(out)`: per-device records (name, priority, state, error code), the count of
  failures by priority, and the pin registry summary. A `required` device in any state
  other than `bound` is flagged so CoreOS can decide.
- `deinit()`.
- The kernel's API for CoreOS: the catcall registry, the filesystem, buses and pins, in a
  versioned table.

The kernel does not read `purrcfg`, keys or the handoff itself. It is given the boot
information by the boot package and passes it on to CoreOS.

## 11. Constraints

- **The kernel talks to the platform only through the host API table** CoreOS
  passes in (`../coreos/SPEC.md` section 6). It never calls ESP-IDF by name.
  The bus and pin code in section 6 sits on that table's SPI, I2C, GPIO and PWM
  entries.
- **No code that must run while flash is busy** lives in the module. Interrupt
  handlers stay in CoreOS, and the kernel gets callbacks from a normal task.
- Static allocation in init and the registries. Driver buffers are allocated
  once at probe from internal DMA-capable RAM and released at `remove`.
- No PSRAM assumed, and no full-frame buffers.
- No chip-specific code in the registries. Chip and board specifics live in
  the board profile and drivers.
- Each public function documents whether it is thread-safe.

## 12. Testing

Host-side, with fake buses, fake pins and fake drivers:

- Driver registry and matching, including no driver found.
- Pin conflicts: two claims on one pin, and a claim on a reserved pin.
- Priority reporting when a `required` device fails.
- Catcall registry: version matching, replace flag, table full.
- Adapters in both directions, including a native-only feature returning
  `ESP_ERR_NOT_SUPPORTED` through the legacy view.
- The plug-and-play acceptance test from section 2.

On the board: the test pattern app, run first with the display alone, then with
touch added.

## 13. The device config bundle

One signed bundle holds the hardware description and the drivers for a board. How it is
packaged and distributed is in `../../../Drivers/SPEC.md`: a signed list that pins each driver's
hash and gives it its pins, with the drivers pulled as individual files.

- **Locked to the release keys.** The core drivers cannot be modified or replaced.
  Their signature is checked before they are used.
- **Separate from the boot package.** The boot package is only for the boot menu and
  loading the kernel (`../../../bootloader/SPEC.md` section 9). The bundle is for CoreOS and
  the kernel.
- **Modular boards:** a separate signed file in `/boot`, updated by the swap in
  `../../SPEC.md` section 6.1.
- **Monolithic boards:** linked into the OS image, so the image's signature protects
  the drivers. The bundle's format is the same either way.
- **The board profile** (section 3) becomes data read from the bundle, not a table
  compiled into the kernel.

## 14. User drivers

Users can add drivers, in two levels. Core drivers in the bundle are never touched.

**Data-description drivers (a file, from an SD card).**

- A plain text file that describes one device, run by the kernel's built-in generic
  engines. No native code, so it cannot crash the system.
- **The rule:** a driver can be a data description if it is **one device on one bus**,
  plus that device's own extra pins (chip select, data/command, reset, backlight, an
  interrupt line). Display and keyboard drivers are the intended examples.
- **It needs real code** if it coordinates two or more devices or buses, or needs its
  own timing or a state machine beyond a fixed start-up sequence and register reads.
  A radio that uses SPI plus timing and interrupts is an example.
- Installed into the driver system from an SD card (or another transport) by the
  shell, after the user confirms. A description that is malformed or asks for pins the
  locked drivers already own is rejected with the reason. The pin registry
  (section 6) enforces the second part.
- The file format is not decided (section 15).

**Code drivers.** A driver that needs code is a native module. It is only loaded if it
is signed with a key the device trusts for that: the owner key or a developer key.
Official builds trust only your keys. In developer-preview builds the published test
key is trusted, so anyone can sign one there.

## 15. Open questions

- Pin details for the display MISO and the touch reset polarity, confirmed on
  the CYD 2.4C.
- Graphics library choice above the display catcall (the old system used LVGL
  and MiniWin). Not a kernel concern, but it decides the chunk size.
- How large the host API table gets, and which entries are needed for the first
  display and touch drivers. Keep it as small as the two drivers allow.
- Whether the board profile should be generated from a declarative file by
  purrstrap, and in what format.
- Which second native interface to design after display and touch.
- **The description file format** and which generic engines exist first (display,
  keyboard, simple sensors).
- **Who may install a data-description driver:** the shell's root authority with a
  confirmation, or a permission (`../../../Permissions/SPEC.md`).
- **Where installed user drivers live** on storage, and the order they load in.
- **How a driver in the locked bundle can be tested** during development, when it
  cannot be modified.
