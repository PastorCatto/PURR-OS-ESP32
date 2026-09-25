# PURR OS v0.13.0 — Architecture Revision

> This document captures the architectural direction agreed upon for v0.13.0.
> v0.13.0 is considered the final major restructuring before v1.0.0.

---

## Goal

Stabilize the platform with a clear separation between the generic kernel and
device-specialized kernels, restore proven embedded driver stability, and formalize
the plug-and-play modular system. The esp_lcd upstream wrapper approach is deferred
to a future release — stability comes first.

---

## Implementation Order

1. **Revert to embedded drivers** (v0.11.0 style) — hand-rolled SPI/I2C, compiled
   directly in, no module loading for display and touch. Proven stable.
2. **Get T-Deck Plus fully working** — display, touch, trackball, keyboard all
   confirmed functional under the specialized kernel.
3. **Introduce specialized kernel layout** — `kernel_tdp` and `kernel_td` folders
   under `source/kernel/`, inheriting from `core/`.
4. **Validate plug-and-play** on top of the specialized kernel.
5. **Upstream driver migration** (esp_lcd, esp_lvgl_port) deferred to v0.14.0+
   once the kernel structure is proven.

---

## Layer Model

```
┌─────────────────────────────────────────────┐
│  Layer 4 — Modules + Apps                   │
│  Plug-and-play drivers, .paws/.claw apps,   │
│  SD-loaded extras. Unchanged from today.    │
├─────────────────────────────────────────────┤
│  Layer 3 — PURR Wrapper                     │
│  Extends upstream UI frameworks. Adds OS    │
│  integration, theming hooks, window mgmt    │
│  conventions. Other UI frameworks plug in   │
│  here by implementing the wrapper interface.│
├─────────────────────────────────────────────┤
│  Layer 2 — UI Framework (upstream source)   │
│  LVGL and MiniWin pulled in as full         │
│  upstream source. Full native API exposed.  │
│  Fork if patching is needed. Drop-in        │
│  upgradeable when upstream releases.        │
├─────────────────────────────────────────────┤
│  Layer 1 — Catcall Interface                │
│  Unchanged. Same header files, same         │
│  function pointer tables. Both specialized  │
│  kernel drivers and plug-and-play modules   │
│  implement the same interface.              │
├─────────────────────────────────────────────┤
│  Layer 0 — Kernel (generic or specialized)  │
│  Generic: core/ — works for any device.     │
│  Specialized: kernel_tdp/, kernel_td/ etc.  │
│  Device-specific drivers compiled straight  │
│  in. No module loading for these.           │
└─────────────────────────────────────────────┘
```

---

## Specialized Kernel Layout

Specialized kernels live alongside the generic core in `source/kernel/`:

```
source/kernel/
    core/           ← generic kernel (purr_kernel.c, boot.c, catcalls)
    kernel_tdeck_plus/  ← T-Deck Plus specialized kernel
    kernel_tdeck/       ← T-Deck specialized kernel
```

purrstrap checks for a specialized kernel folder matching the device. If found,
it builds that variant. If not, it falls back to `core/`. The pcat file still
drives all pin assignments and module selections — the kernel variant just changes
what gets compiled in at Layer 0.

### When to use a specialized kernel

A device gets a specialized kernel when:
- Its hardware doesn't behave under generic module loading
- It needs drivers compiled in a specific order or with tight coupling
- It has peripherals that require init sequences not expressible in a generic driver

Generic kernel remains the default for well-behaved devices.

---

## Specialized Kernel: kernel_tdp (T-Deck Plus)

**Compiled in (Layer 0 — no module loading):**
- ST7789 display — hand-rolled SPI, embedded directly (v0.11.0 style)
- GT911 touch — I2C, embedded directly
- Trackball — GPIO x4 + click, embedded directly
- Keyboard — BB Q20 over I2C (LilyGO reference), embedded directly

**Plug-and-play modules (Layer 4):**
- KittenUI (LVGL UI framework)
- app_manager
- SX1276 LoRa radio
- GPS (generic NMEA)
- SD card

**Does not include:** anything the T-Deck does not have (no keyboard differences
between TD and TDP at this time — both have trackball + keyboard).

---

## Specialized Kernel: kernel_td (T-Deck)

**Compiled in (Layer 0):**
- ST7789 display — same as TDP
- GT911 touch — same as TDP
- Trackball — same as TDP
- Keyboard — same as TDP

**Plug-and-play modules:**
- KittenUI
- app_manager
- SX1276 LoRa radio
- SD card

**Does not include:** GPS (T-Deck does not have GPS hardware).

---

## Driver Approach (v0.13.0)

Display and touch drivers are embedded using the proven v0.11.0 hand-rolled
approach — direct SPI/I2C register writes, no upstream component dependency.
This is stable, predictable, and fully understood.

The esp_lcd / esp_lvgl_port upstream migration is documented below as the future
direction but is not implemented in v0.13.0.

| Driver | T-Deck | T-Deck Plus | CYD S028R | CYD S024C | JC3248W535 | Approach |
|--------|--------|-------------|-----------|-----------|------------|----------|
| Display | ST7789 | ST7789 | ILI9341 | ILI9341 | AXS15231B | Embedded, hand-rolled |
| Touch | GT911 | GT911 | XPT2046 | CST820 | AXS15231B | Embedded, hand-rolled |
| WiFi | ✓ | ✓ | ✓ | ✓ | ✓ | ESP-IDF native |
| Bluetooth | ✓ | ✓ | ✓ | ✓ | ✓ | ESP-IDF native |
| Keyboard | BB Q20 | BB Q20 | — | — | — | Embedded (specialized kernel) |
| Trackball | GPIO x4 | GPIO x4 | — | — | — | Embedded (specialized kernel) |
| LoRa | SX1276 | SX1276 | — | — | — | Plug-and-play module |
| GPS | — | NMEA | — | — | — | Plug-and-play module (TDP only) |
| SD Card | SPI | SPI | SPI | SPI | SPI | Plug-and-play module |

---

## Layer 1 — Catcall Interface

No changes. `catcall_display_t`, `catcall_touch_t`, `catcall_radio_t`, etc. remain
the contract between Layer 0 and everything above. Specialized kernel drivers
register catcalls exactly like plug-and-play modules do. Nothing above Layer 1
knows or cares which kernel variant is running.

---

## Layer 2 — UI Frameworks

LVGL pulled in as full upstream source via the component manager. Full native API
available. MiniWin follows the same pattern. Fork and patch if needed rather than
working around upstream in the wrapper.

---

## Layer 3 — PURR Wrapper

Extends UI frameworks rather than hiding them. Adds kernel integration, theming,
window management conventions, and portability hooks. Third parties implement the
wrapper interface to bring their own UI into PURR OS.

---

## Device Personality Files

Each device has a `.pcat` file under `source/devices/<name>/`. The pcat drives:
- Chip selection (esp32, esp32s3, etc.)
- Pin assignments
- Module inclusions
- Radio capabilities
- Flash layout

purrstrap reads the device name, finds the pcat, checks for a matching specialized
kernel, and builds accordingly. The pcat format is extended in future releases to
support driver source references and capability gating.

---

## Chip Capability Matrix

| Chip | WiFi | BT Classic | BLE | 802.15.4 |
|------|------|------------|-----|----------|
| ESP32 | ✓ | ✓ | ✓ | — |
| ESP32-S2 | ✓ | — | — | — |
| ESP32-S3 | ✓ | — | ✓ | — |
| ESP32-C3 | ✓ | — | ✓ | — |
| ESP32-C6 | ✓ (WiFi 6) | — | ✓ | ✓ |
| ESP32-H2 | — | — | ✓ | ✓ |

WiFi/BT init is gated by the pcat `[radio]` section and chip capability.
purrstrap excludes unsupported capabilities at build time.

---

## Future: Upstream Driver Migration (v0.14.0+)

Once the specialized kernel structure is proven and T-Deck Plus is stable, the
following upstream migration is planned:

- Display drivers → ESP-IDF `esp_lcd` panel API
- Touch drivers → `espressif/esp_lcd_touch_*` components
- LVGL integration → `espressif/esp_lvgl_port` (DMA, proper task, locking)
- XPT2046 → `atanisoft/esp_lcd_touch_xpt2046`
- CST820 → `kodediy/esp_lcd_touch_cst820`
- AXS15231B → `espressif/esp_lcd_axs15231b`

The catcall interface will gain optional `panel_handle` and `touch_handle` fields
to enable the `esp_lvgl_port` path without breaking existing drivers.

---

## What Changes in v0.13.0

- Specialized kernel folders introduced (`kernel_tdeck_plus`, `kernel_tdeck`)
- purrstrap updated to detect and build specialized kernels
- Display/touch drivers reverted to proven v0.11.0 embedded approach
- Keyboard (BB Q20) and trackball baked into specialized kernels
- GPS remains plug-and-play (TDP only)
- LoRa remains plug-and-play (both TD and TDP)

## What Does Not Change

- Catcall header interfaces
- Modular extension system and module loading
- `.paws` / `.claw` app tiers
- SD plug-and-play
- purrstrap as the build and flash tool
- Generic `core/` kernel for all other devices

---

*Target: v0.13.0 → v1.0.0 final stabilization*
