# Meshtastic Mesh Networking — Resurrection Plan

> **Status as of v1.0.0-dp8 — Phases 1–2 are done and hardware-verified.** This plan's
> own verification section (bottom of the file) is accurate and was written from a real
> boot log; it has been re-checked against the tree and stands.
>
> What has changed since the plan was written:
> - **The driver is now `sx1262_rl`, not `sx1262`.** `tdeck_plus` and `heltec` both select
>   the RadioLib-backed driver. The plain `sx1262.c` SPI driver described throughout Part 1
>   is still in tree and still used by `tdeck` and `tdeck_plus_pounce`, but it is no longer
>   the T-Deck Plus path. The pin fixes in 1.2/1.3 landed and are still correct.
> - **RX is interrupt-driven** as of dp7 (`7a7dfccc`), not polled.
> - **MeshChat became MSN** (`6ad8ad33`, dp5) — protocol-agnostic, with **MeshCore** as a
>   second selectable backend (`6aa8763b`) and a live switch that does not require a
>   reboot (`215941c5`).
> - **The shared-SPI2 hang** the plan worried about was real and was closed in dp7
>   (`23658e42`).
> - Two open items below are still open: `mesh_manager_send_text()`'s return value has no
>   console trigger, and the **two-device text exchange has still never been run** — there
>   has only ever been one LoRa board on hand.
>
> **Do not delete this file yet.** Phases 3–6 (position/telemetry, admin/config, the ATAK
> gateway, store-and-forward) are named-only and unbuilt, and the two-device test is
> outstanding. The parts that *are* done are documented in `docs/03_Modules.md`.

**Working tracking doc — delete once mesh is fully implemented and verified.**

## Context

PURR OS had a real, fairly detailed Meshtastic + ATAK gateway plan (`archive/docs_0.11/MESHTASTIC_ATAK_PLAN.md`) and a working ~400-line implementation (`PURR-OS-0.11/CoreOS/system/kernel/modules/purr_mesh.cpp`) from before the kernel/catcall architecture rewrite. Both were archived out in commit `9a7d124a` and dropped from the live tree entirely — no mesh/Meshtastic/ATAK code or docs exist in the current codebase. This resurrects it, but explicitly **not** as a straight port of the old architecture — it's built as a self-contained `PURR_MOD_SYSTEM` kernel module ("a driver type of deal") that owns all mesh logic internally and talks to the radio strictly through the existing `catcall_radio_t` contract, matching how every other subsystem in the current architecture is layered.

While investigating the radio path to plan this, we discovered `tdeck_plus` (the device on hand this session) is running the **wrong radio driver** — it's configured for `sx1276`, but the archived plan's own hardware table says T-Deck Plus uses **SX1262**, and this matches a live boot failure from earlier this session (`sx1276: unexpected version 0x00, expected 0x12`). A real SX1262 driver already exists in the tree but has never been wired to real per-device pins. This has to be fixed first — nothing about mesh works without a functioning radio.

We also found the mechanism that's supposed to carry `device.pcat` pins into any driver (`purr_device_glue.c`'s generated `#define`s + a `purr_device_init()` hook) is dead code — never called anywhere. The two drivers that work today (display, touch) only work because `kernel_tdp_boot.c` hardcodes their pins directly and calls their `_configure()` functions by hand. The radio fix has to follow that same real, working pattern.

### Confirmed pins (via LilyGo's official `utilities.h`/`TDECK_PINS.h`, not guessed)
- `RADIO_BUSY = 13`, `RADIO_CS = 9`, `RADIO_RST = 17`, `RADIO_DIO1/IRQ = 45`
- SPI bus is **shared** across display + SD + radio: `MOSI=41, MISO=38, SCK=40`
- `SDCARD_CS = 39`
- `GPS TX=43, RX=44` (unchanged — never actually in conflict)

Our current `tdeck_plus/device.pcat` has **two** pin bugs: `lora_mosi/miso/sclk` (currently 6/5/7 — copy-pasted from `sx1276.c`'s driver-default fallback values, never verified against real hardware) should be 41/38/40; and `sd_cs` (currently 13 — added earlier this same session for the new SD driver, before this research existed) collides with the real radio BUSY pin — must be 39. Both fixed in Part 1. The SD driver code written earlier this session (`kernel_tdp_boot.c`'s `mount_sd_vfs()`) needs its `TDP_SD_CS` constant corrected from 13 to 39 as part of this fix.

---

## Part 1 — Radio driver fix (prerequisite, blocks Part 2's hardware testing)

- [x] **1.1** Add real pin configuration to the SX1262 driver
  Files: `source/drivers/radio/sx1262/sx1262.c` (edit), `source/drivers/radio/sx1262/sx1262.h` (new — doesn't exist yet)

  Mirror `gt911_configure()`'s exact pattern (`source/drivers/touch/gt911/gt911.c:100-105,488-495`): replace the seven `#ifndef SX1262_PIN_*` compile-time-only macros with `SX1262_DEFAULT_*` constants plus `static int s_pin_*` file-statics, add:
  ```c
  void sx1262_configure(int mosi, int miso, int sclk, int cs, int rst, int busy, int irq);
  ```
  Update every use of the old `SX1262_PIN_*` macros inside `sx1262_init()` and the `wait_busy()` polling loop to read the new statics instead.

- [x] **1.2** Wire T-Deck Plus's real pins before the module loader runs
  File: `source/kernel/kernel_tdeck_plus/kernel_tdp_boot.c` (edit)

  Add a `TDP_LORA_*` pin macro block next to the existing `TDP_DISPLAY_*` one, and call `sx1262_configure(...)` right before the existing `purr_kernel_load_static_modules();` call — same hand-wired pattern this file already uses for display/touch. Confirmed values: **MOSI=41, MISO=38, SCLK=40** (shared bus with display+SD — not a separate bus), **CS=9, RST=17, IRQ/DIO1=45, BUSY=13**.

  Also fix the SD driver's CS pin while touching this file: `mount_sd_vfs()`'s `TDP_SD_CS` constant must change from `13` to `39` (LilyGo's `TDECK_SDCARD_CS`) or the SD card and LoRa radio will contend for the same GPIO.

- [x] **1.3** Update `tdeck_plus/device.pcat`
  - `[drivers] radio` and `[radio] lora`: `"sx1276"` → `"sx1262"`
  - `[flash]`: `radio/sx1276 = 2` → `radio/sx1262 = 2`
  - `[pins]`: `lora_mosi = 6 / lora_miso = 5 / lora_sclk = 7` → `41 / 38 / 40`; add `lora_busy = 13`; change `sd_cs = 13` → `sd_cs = 39`
  - (`lora_cs=9`, `lora_rst=17`, `lora_irq=45` are already correct, no change)

- [x] **1.4** Deferred, not this pass
  `tdeck` (plain, non-Plus) likely has the same bug — selects `sx1262` already but its `[pins]` block is a copy of `tdeck_plus`'s old (wrong) SX1276-shaped values, with no BUSY entry. Not the hardware on hand this session; leave it broken and flag it. `heltec`'s `device.pcat` is already correctly configured (`radio = "sx1262"`, `lora_busy = 13`) — good second target once T-Deck Plus is proven.

---

## Part 2 — New `meshtastic` system module

Directory: `source/modules/meshtastic/` (new). Template: `source/modules/app_manager/` (flat `module.pcat`, `PURR_MOD_SYSTEM`, plain-C public header other code calls directly — not a catcall).

- [x] **2.1** `meshtastic.h` — public API (C, not the old code's C++):
  ```c
  int      mesh_manager_init(void);      // PURR_MOD_SYSTEM init(), returns immediately
  void     mesh_manager_deinit(void);
  bool     mesh_manager_send_text(const char *text);
  typedef void (*mesh_rx_cb_t)(uint32_t from_node, int portnum, const uint8_t *payload, size_t len);
  void     mesh_manager_set_rx_callback(mesh_rx_cb_t cb);
  uint32_t mesh_manager_node_id(void);
  int      mesh_manager_node_count(void);
  bool     mesh_manager_ready(void);
  ```
- [x] **2.2** `mesh_radio.c` — LONG_FAST preset constants, region frequency table, PSK→AES-256 key expansion, AES-CTR via mbedtls, IV construction. Ported from `purr_mesh.cpp`'s equivalent sections (lines 39-45, 95-123).
- [x] **2.3** `mesh_router.c` — dedup ring buffer (32 slots), node table (16 nodes, RSSI + last-seen), nanopb encode/decode for `MeshPacket`/`Data`/`User`, `TEXT_MESSAGE_APP` + `NODEINFO_APP` handling, flood relay. Ported from `purr_mesh.cpp` lines 61-92, 129-259.
- [x] **2.4** `meshtastic_module.c` — `PURR_MODULE_REGISTER(meshtastic)` (`module_type = PURR_MOD_SYSTEM`), the FreeRTOS task, public API glue.
- [x] **2.5** `module.pcat` (flat, matches `app_manager`'s): `name/version/module_type/kernel_min/provides/required_catcalls=["radio"]`.
- [x] **2.6** `CMakeLists.txt` — lists its own 3 `.c` files plus the vendored nanopb + protobuf sources directly as extra `SRCS`/`INCLUDE_DIRS` (see Part 3 for why).

**Correction vs. the old plan doc**: the actual working `purr_mesh.cpp` code does NOT use the `[4B magic][16B IV][ciphertext]` wire framing the doc describes — it nanopb-encodes a `MeshPacket` directly as the LoRa payload, with the IV derived from the packet's own plaintext `id`/`from` fields already in the header, not transmitted separately. Port from the actual code, not the doc's abbreviated description.

**Key architectural change (the point of this whole plan)**: old code drove the SX1262 directly via a bespoke `lora_manager` singleton. New code goes through `purr_kernel_radio()` → `catcall_radio_t` exclusively. The mesh module must NOT call the catcall's `init()` itself (the radio driver's own module registration already does that with its own default config) — instead it retunes via `set_frequency()`/`set_power()`.

- [x] **2.7** Necessary small shared-infra addition: `catcall_radio_t` currently has no way to set spreading factor / bandwidth / coding rate / sync word after init — only `set_frequency`/`set_power`. LONG_FAST needs SF11/BW250kHz/CR4-5/sync 0x2B. Add two function pointers to `catcall_radio.h`:
  ```c
  esp_err_t (*set_modulation)(uint8_t sf, uint32_t bw_hz, uint8_t cr);
  esp_err_t (*set_sync_word)(uint8_t sync);
  ```
  Both `sx1262.c` and `sx1276.c` already have the internal logic for this (`sx1262.c`'s `set_modulation()`, `sx1276.c`'s `apply_modulation()`) — just needs exposing through the catcall struct and each driver's registration.

**Task lifecycle** (mirrors `cardstack_module.c`'s pattern — wait on the actual dependency, not just boot_ready):
```c
static void mesh_task(void *arg) {
    while (!purr_kernel_radio()) vTaskDelay(pdMS_TO_TICKS(20));
    // apply LONG_FAST via set_modulation/set_sync_word, compute node_id from esp_read_mac(),
    // broadcast initial NODEINFO_APP, then loop: data_available -> receive -> decode ->
    // dedup -> node table update -> rx callback -> relay; NODEINFO_APP every 10 min
}
```
`mesh_manager_init()` itself just creates the task (8192 stack, priority 4) and returns.

---

## Part 3 — Vendoring nanopb + Meshtastic protobufs

- [x] **3.1** Copy verbatim from the archive:
  - `source/lib/lib_nanopb/` (new) ← `PURR-OS-0.11/CoreOS/components/lib_nanopb/` (pb.h, pb_common/decode/encode .c/.h)
  - `source/lib/lib_mesh_pb/meshtastic/` (new) ← `PURR-OS-0.11/CoreOS/components/lib_mesh_pb/meshtastic/` (mesh, portnums, channel, config, device_ui, module_config, telemetry, xmodem — all 8, since `mesh.pb.h` transitively includes all of them even though logic only actively uses mesh/portnums)
  - **Correction found during build**: `atak.pb.h` is NOT actually unused — `module_config.pb.h` `#include`s it transitively, so the build fails without it. Copied it too (header-only, no matching `.c`, no CMakeLists change needed).
  - **Second correction found during build**: the archived `atak.pb.h` stub was itself missing `meshtastic_Team`/`meshtastic_MemberRole` enum definitions that `module_config.pb.h`'s (unused-by-us) `TAKConfig` struct needs to compile — a latent bug in the *original* 0.11 code, never caught because `PURR_ENABLE_MESH` defaulted off there so this file was never actually compiled. Added minimal placeholder enum definitions (values aren't guaranteed to match real Meshtastic firmware's exact ordinals — fine, since nothing in this codebase constructs a TAKConfig).

`modulestrap.py`'s component discovery only scans `source/modules/*/module.pcat` and `source/drivers/*/*/driver.pcat` — a bare `source/lib/` folder won't be auto-discovered. Rather than teaching modulestrap a new "lib" concept, the `meshtastic` module's own `CMakeLists.txt` lists the vendored files as extra `SRCS`/`INCLUDE_DIRS` with relative paths (`../../lib/...`) — they compile as part of `meshtastic`'s own component, fully private to it, no shared build infra touched. Add `mbedtls` to that CMakeLists' `REQUIRES` (for AES-CTR).

---

## Part 4 — Incoming messages → existing notification system (no new UI work)

- [x] **4.1** In `mesh_router.c`'s `TEXT_MESSAGE_APP` RX handler:
  ```c
  char title[PURR_NOTIFY_TITLE_LEN];
  snprintf(title, sizeof(title), "Mesh: %08lX", (unsigned long)from);
  purr_kernel_notify(title, text_truncated_to_63_chars, "meshtastic");
  ```
  Already rendered by all three UI backends from this session's earlier notification-parity work — zero additional UI plumbing.

- [x] **4.2** Call `purr_kernel_set_lora_available(true)` once the radio catcall is confirmed present in the mesh task's startup — this flag is currently only ever set by the legacy Arduino kernel, never by `tdeck_plus`'s native path, so the LoRa status icon shown in every UI backend today is permanently stale. Cheap, useful, unrelated-but-adjacent fix.

---

## Part 5 — Device wiring

- [x] **5.1** `source/devices/tdeck_plus/device.pcat`: add `mesh = "meshtastic"` under `[modules]`, `meshtastic = 3` under `[flash]` (after `radio/sx1262 = 2`).

Not this pass: `tdeck` (pin verification needed first, per Part 1.4), `heltec` (correct pins already, but not the hardware on hand — good next target after T-Deck Plus is proven).

---

## Explicitly deferred (name only, do not design)

- **Phase 3**: Position/telemetry (`POSITION_APP`, node map UI)
- **Phase 4**: Admin/config exchange (`ADMIN_APP`, NVS `mesh_cfg`)
- **Phase 5**: ATAK/CoT gateway — genuinely never built (only planned), would be a new sibling module (`atak_gateway`) consuming `mesh_manager_set_rx_callback()`. CoT XML template and TAK multicast group (`239.2.3.1:6969`) are already fully specified in `archive/docs_0.11/MESHTASTIC_ATAK_PLAN.md` lines 72-82/137-138 if/when this gets picked up.
- **Phase 6**: SPIFFS store-and-forward for offline nodes

The module's shape (own task, small public API) is designed so these bolt on later without touching the core send/receive/routing path. This pass = Phases 1-2 only: real text message send/receive, node table, dedup, flood routing.

---

## Verification

**Build (before any hardware):**
- [x] `modulestrap build all` — regenerate the components manifest so `meshtastic`'s new component is discovered (found + fixed a real, pre-existing Windows path-separator bug in `modulestrap.py` along the way — backslashes in a generated `.cmake` file broke CMake parsing entirely; see git history on `modulestrap/modulestrap.py`)
- [x] `purrstrap build tdeck_plus` — **compiles clean** (45/45, full firmware merged). Hit and fixed 2 real bugs in the vendored archive along the way: `atak.pb.h` turned out not to be unused after all (`module_config.pb.h` transitively includes it), and the archived stub was itself missing `meshtastic_Team`/`meshtastic_MemberRole` enum definitions its own `TAKConfig` struct needs — a latent bug in the original 0.11 code that was never caught because that build path was never compiled there either. Both fixed; see Part 3 notes above.

**Hardware — VERIFIED on real T-Deck Plus, boot log:**
- [x] `sx1262: init OK  freq=868000000Hz SF7 BW125 CR4/5 pwr=14dBm` — driver's default config applies cleanly with T-Deck Plus's real pins (not Heltec's) — the radio chip actually responds now (contrast with the earlier session's `sx1276: unexpected version 0x00` failure this same device hit before the driver swap)
- [x] `purr_kernel: radio registered: sx1262` then `meshtastic: ready id=F6CCE960  906875000Hz  SF11 BW250kHz` — `mesh_task`'s `purr_kernel_radio()` wait unblocked, and the retune from the driver's defaults to LONG_FAST (SF11/BW250kHz, US region 906.875MHz) via the new `set_modulation`/`set_sync_word`/`set_frequency` catcall methods succeeded
- [x] `purr_kernel: notify [meshtastic] Mesh ready: Meshtastic mesh online` — notification fired, proving Part 4's wiring works
- [x] `meshtastic: nodeinfo broadcast id=F6CCE960` fired ~6s after boot, no crash/hang — proves the full encode (nanopb + AES-256-CTR) → `catcall_radio_t.send()` path runs clean end-to-end
- [x] No asserts, no guru meditation, no crash anywhere in the boot log
- [ ] `mesh_manager_send_text()` explicit return value — not directly observable yet (no console command wired to trigger it manually); the periodic NODEINFO broadcast above already proves the same encode→send path works, so this is a formality, not a real open question
- [ ] Two-device text exchange — only one LoRa-capable board on hand this session, deferred until a second is available

**Incidentally confirmed harmless (not part of this verification, noted for completeness):** SD card mount failed (`ESP_ERR_INVALID_RESPONSE` — ​no card was inserted, not a wiring issue); the two `spi_bus_initialize(): SPI bus already initialized` and the I2C "already acquired" lines are the same benign shared-bus/shared-I2C-bus messages already diagnosed as expected earlier this session.

### Critical files
- `source/drivers/radio/sx1262/sx1262.c` (+ new `.h`)
- `source/kernel/kernel_tdeck_plus/kernel_tdp_boot.c`
- `source/devices/tdeck_plus/device.pcat`
- `source/kernel/catcalls/catcall_radio.h` (+ matching changes in `sx1262.c`/`sx1276.c`)
- `source/modules/meshtastic/{meshtastic.h, meshtastic_module.c, mesh_radio.c, mesh_router.c, module.pcat, CMakeLists.txt}` (all new)
- `source/lib/lib_nanopb/`, `source/lib/lib_mesh_pb/meshtastic/` (all new, vendored from `PURR-OS-0.11/`)
