# PURR OS bootloader spec (draft 0.2)

Single source of truth for the bootloader, the image container and the boot
flow. If code and this document disagree, fix one of them in the same commit.

## 1. Purpose and threat model

The bootloader loads exactly two things: the **boot package**, and **KittenOS** when the
boot target says so. On monolithic boards, and on modular boards' normal path, it also
loads the packed OS image or the **kernel** (respectively) after the boot package's menu
returns a choice -- the same mechanism either way (section 6, step 8). It verifies each
against a public key it holds, applies a small policy from flags in flash, and hands off.
Everything above the kernel (CoreOS, drivers, apps) is a file in the root filesystem,
loaded later and not by the bootloader. The kernel itself is loaded by the bootloader,
not a root-filesystem file -- see `PurrOS/SPEC.md` section 1 (updated 2026-09-28: the
kernel moved back into the boot area, its own partition).

**What this is:** an authenticity and integrity check with a recovery policy.
It catches corrupted images, wrong-vendor builds, bad updates and accidental
mistakes, and it lets the OS warn the user.

**What this is not:** protection against someone who can write flash. Without
eFuses nothing verifies the bootloader itself, so a physical attacker can
replace it, change the flags or change the keys. eFuse support (section 8) is
optional and adds a real root of trust underneath this layer.

**KittenOS** is the recovery system: a small (about 1 MB) fixed system in a raw partition,
not updated in normal use. It is a small real OS with its own drivers, a filesystem reader
and the shell, and it applies updates. It uses the same signing as everything else.
Handling a key or verification failure is KittenOS's job, not the bootloader's.

Non-goals for v1: networking, filesystem access, and loading anything except the boot
package, KittenOS and, on monolithic boards, the packed image. The boot menu and reading the
root filesystem belong to the boot package (section 9), not the bootloader. The bootloader
stays small and has no FreeRTOS.

## 2. Flash layout (example)

The full layouts per board are in `PurrOS/SPEC.md` section 4. The raw partitions the
bootloader knows about:

| Name       | Type/Subtype | Size   | Notes |
|------------|--------------|--------|-------|
| bootloader | -            | 40 KB (0x0, up to the table at 0xA000) | Second stage. |
| nvs        | data/nvs     | 16 KB  | Used by the OS, never by the bootloader. |
| purrcfg    | data/0x40    | 8 KB   | Two 4 KB copies (A/B) of the config struct, at 0xD000. Moved there (from 0x1E000) when `bootpkg` grew to 56 KB, with `nvs` giving up 8 KB to make room. |
| bootpkg    | data/0x40    | 56 KB (0xE000) | The boot package. Sized for growth: its menu will get more features. |
| kittenos   | app/factory  | 1 MB   | KittenOS, recovery only. Not updated in normal use. |
| rescue     | app/test     | ~1 MB  | Modular boards: the recovery loader (Wi-Fi, TLS, keys, minimal CoreOS). Almost never changed. |
| kernel     | app, single slot, `ota_0` subtype | 0x1E0000 (T-Deck Plus) | Modular boards only: hardware, drivers, FreeRTOS, mounts `root`, loads CoreOS from it (`PurrOS/SPEC.md` sections 1, 6). Built and flashed 2026-09-28: it keeps the `ota_0` *subtype* (not a genuinely distinct one, despite the original plan here) purely so ESP-IDF's own stock `bootloader_utility_load_boot_image()` -- which only knows `factory`/`test`/`ota_0..15` -- can actually jump to it; `esp_ota_*` selection semantics (otadata, `esp_ota_set_boot_partition`) are still never used. The bootloader picks it by trying it first in its own fallback chain (section 6), not through otadata. |
| os         | app/ota_0    | rest   | Monolithic boards only: the packed OS image, one slot. |
| root       | data/littlefs | rest  | The root filesystem: CoreOS and everything else on modular boards (the apps filesystem, 1 MB, on monolithic ones). |

The bootloader binary must fit before the partition table, so budget its size and move the
table if it grows. There is no OTA slot switching: which target boots is recorded in
`purrcfg`.

### 2.1 Sizing

- **Bootloader budget.** The bootloader lives between flash offset 0 and the partition table. It
  was 32 KB (table at `0x8000`) and ran out of room: on 2026-10-25 the table moved to **`0xA000`**
  (`CONFIG_PARTITION_TABLE_OFFSET` in every project's `sdkconfig.defaults`), giving the bootloader
  **40 KB**. The room came from dropping the `otadata` partition (8 KB): the PURR bootloader
  picks `kernel`, `kittenos` or `loader` itself and never read it, and nothing in the OS needs it
  any more (the `esp_ota_set_boot_partition()` call in `net_install` was removed).
  The area before `kittenos` at `0x20000` is now: bootloader 40 KB (0x0), table 4 KB (0xA000),
  `nvs` 16 KB (0xB000), `purrcfg` 8 KB (0xF000), `phy_init` 4 KB (0x11000), `bootpkg` 56 KB
  (0x12000). The two projects that build the bootloader and every app must agree on the offset:
  an app finds the partition table at the offset compiled into it, so changing it means
  rebuilding the OS images as well.
- **Measured 2026-10-25 (ESP32-S3, built with ESP-IDF 5.3.5, `-Os`):** the bootloader as committed
  before the trailer work was 32,688 bytes against the old 32,768 limit (80 free). With slot
  verification and the handoff it was 34,400, over the limit. IDF's own bootloader code now logs at
  WARN only (`CONFIG_BOOTLOADER_LOG_LEVEL_WARN` in every project's `sdkconfig.defaults`) while the
  PURR files set `LOG_LOCAL_LEVEL` back to INFO for their own lines (side effect: IDF's own INFO
  lines, such as "Loaded app from partition", no longer print). With slot verification, the
  hardware SHA hook, the read window and the version floors it is **32,608 bytes, 8,352 free of
  the new 40 KB**. The software SHA-256 stays linked (it hashes the 109-byte header, which the
  word-only hardware block cannot).
- The last 4096 bytes of `kernel`, `kittenos` and `loader` are reserved for the signature trailer
  (section 3.1), so an image may be at most `partition size - 4096` bytes.
- Layouts are per board, so each board carries its own partition file.
- **Codename.** For a board that has a driver pack, the bootloader is built with the board's
  codename inside (`Boards/SPEC.md` section 3, `Install/SPEC.md`).

## 3. Image container

Every image starts with a `purr_image_header_t`, followed by the payload. For
the OS and recovery the payload is a standard ESP-IDF app image.

| Field            | Size | Meaning |
|------------------|------|---------|
| magic            | 4    | `0x50555252` ('PURR') |
| header_version   | 1    | Container version, starts at 1 |
| header_size      | 2    | Bytes, lets the header grow |
| chip_id          | 2    | `esp_chip_id_t` value; bootloader rejects a mismatch |
| image_type       | 1    | 1 = OS (PURR OS), 2 = recovery (KittenOS), 3 = system module (`.kitt`: kernel, CoreOS, AppManager, runtimes, drivers, boot package, device bundle), 4 = app image (`.cat`), 5 and up reserved for other app kinds such as MicroPython. The bootloader only loads types 1 and 2. CoreOS and the app runtime handle 3 and 4. |
| key_id           | 1    | Which bootloader key signed this image |
| flags            | 2    | Reserved |
| name             | 32   | Human-readable name |
| version          | 12   | Semver string |
| min_boot_version | 12   | Lowest bootloader version allowed to load it |
| payload_offset   | 4    | From start of image |
| payload_size     | 4    | Bytes |
| payload_sha256   | 32   | SHA-256 of the payload |
| signature        | 64   | ECDSA P-256 (r,s) over SHA-256 of every header byte before this field |

Signature is ECDSA P-256 with SHA-256, using micro-ecc and the bootloader's
SHA support, both already linked into the bootloader. The image keeps IDF's own
checksum and SHA-256 too; that check stays on in every mode.

### 3.1 Signatures on raw app slots: the trailer (built 2026-10-25)

`kernel`, `kittenos` and `loader` are real ESP-IDF app images. The stock
`bootloader_utility_load_boot_image()` is what knows how to set up the MMU and load their segments,
and it insists on the ESP app magic byte at the very start of the partition, so a PURR header
cannot go first (the original reason real per-image verification was not built). The signature
therefore goes at the **end** of the partition:

```
partition offset 0                        ESP app image (the container's payload), untouched
...                                       0xFF
partition end - 4096 (PURR_TRAILER_SIZE)  purr_image_header_t (173 bytes), then 0xFF
```

- The trailer is a **verbatim copy of the signed container header** that the release file
  (`.cat` / `.kitt`) already carries. Nothing about signing changed; `header_size` and
  `payload_offset` are both 173 for everything `purrstrap` builds.
- Verification treats the partition as the container it was cut from: header bytes come from the
  trailer, payload bytes from partition offset 0, and the ordinary `purr_image_verify()` does the
  rest (chip, role against type, version floor, key, revocation, payload SHA-256, signature).
  This is `purr_trailer_verify()` (`PurrOS/components/coreos/src/purr_trailer.c`), shared with
  CoreOS and tested on the PC (`test_trailer.c`).
- The **partition decides what it may hold**, not the header: `kernel` takes `PURR_IMG_MODULE` /
  `PURR_MOD_KERNEL`, `kittenos` takes `PURR_IMG_RECOVERY`, `loader` takes `PURR_IMG_MODULE` /
  `PURR_MOD_LOADER`. A validly signed loader is rejected as a kernel. All are boot-role keys.
- The largest image a slot can hold is `part_size - 4096`. `PURR_V_BAD_MAGIC` from the
  trailer verifier means "no trailer" (a slot flashed straight from a build); anything else
  means a trailer that is present and wrong.
- **The whole payload is hashed** (software SHA-256 in the bootloader today). An earlier idea
  was to sign only the SHA-256 that IDF appends to every app image and rely on IDF's loader to
  check it against the contents; that was not taken, because it would rest on IDF behaviour
  not yet checked in 5.3.5. Hashing the image costs boot time: **measured 2026-10-25 on a
  T-Deck Plus, about 2.0 s for a 1.38 MB kernel slot with software SHA-256** (328 ms flash reads,
  about 1300 ms hashing at the bootloader's 80 MHz, about 250 ms the signature check). The
  bootloader now hashes with the chip's SHA block through an optional `purr_hash_t` hook in the
  verifier (whole words only; other payload sizes use the software hash) and reads flash in 4 KB
  windows: **about 0.65 s**. The signature check and the 80 MHz clock are what is left.
- **Writers.** Anything that writes one of these slots writes the payload at offset 0 and the
  header copy at `part_size - 4096`, then reads both back: `net_install` (`commands.c`) and the
  recovery loader (`recovery_loader.c`). For development flashing,
  `purrstrap coreos slot-image` cuts a signed container into a whole-partition image to flash with
  esptool.
- A slot flashed without a trailer still boots in `off` and `warn` mode (with a warning in
  `warn`); `enforce` refuses it (section 6).
- **Version floors (built 2026-10-25, checked on the device in `warn` mode).** The bootloader
  passes each slot's floor from `purrcfg` (kernel, KittenOS, loader) to the verifier, always as
  an enforced floor, so an older validly signed image is a failed verification: `warn` boots it
  with a warning and `FAILED_WARNED`, `enforce` refuses it. The OS raises a slot's floor when a
  boot from it reaches healthy (`purr_mark_boot_healthy()`), to the version in the slot's own
  trailer and only if that trailer verifies at that moment. There is no way yet to lower a floor
  on purpose; that belongs to the signed trust block (`plan3.md` item 4).

## 4. Keys and the virtual key bag

The bootloader keeps a **virtual key bag**: a small software key store with a
separate key for each role, so PURR OS and KittenOS are signed by different
keys.

| Field    | Meaning |
|----------|---------|
| key_id   | Referenced by an image header |
| role     | boot, system, owner, developer or vendor (`Keys/SPEC.md`). A file only verifies against a key whose role allows that type of file. |
| pubkey   | P-256 public key, 64 bytes |
| revoked  | Bit in `revoked_keys` |

- An image verifies only against a key whose role matches its `image_type`.
  An OS image signed with a recovery-role key fails, and the reverse.
- Default keys are compiled into the bootloader. The bag stored in `purrcfg`
  overrides them slot by slot. The effective bag is the defaults with the
  overrides applied.
- A key update replaces one slot. When secure boot is enabled it must be signed
  (section 5.1). The `update key` flag alone never changes a key.
- Private keys never leave the build machine and are never committed.

### eFuse key slot

The eFuse secure-boot key slot is **left open** (not burned) while secure boot
is disabled, and this design never burns it implicitly. It is only programmed
through the gated flow in section 8. Once burned, the eFuse key becomes the
root that anchors the key bag: key bag changes must be signed by it. Slot
count and supported algorithms vary by chip; check the ESP-IDF Secure Boot V2
docs for each target before implementing.

## 5. purrcfg (raw config partition)

Two 4 KB sectors. The bootloader writes the newer copy (higher `seq`) and only
trusts a copy with a valid CRC. This survives power loss during a write.

| Field          | Meaning |
|----------------|---------|
| magic, version | Identify and version the struct |
| seq            | Increments on every write |
| secure_mode    | 0 = off, 1 = warn, 2 = enforce |
| flags          | See below |
| revoked_keys   | Bitmask of revoked key slots |
| key_update     | Optional: slot, new public key, signature by an existing key |
| boot_seq       | Boot counter, incremented by the bootloader on every boot |
| version_floor | Per component, the newest version confirmed on this device. It only rises, when an update is confirmed. In `enforce` mode a file below its floor is rejected. In `warn` and `off` it is allowed with a warning. |
| update_state | Written by CoreOS and KittenOS: target file, state (`staged`, `requested`, `moving`, `unconfirmed`, `confirmed`, `failed`) and attempt count. The bootloader never writes it. See `PurrOS/SPEC.md` section 6.1. |
| efuse_arm      | `arm_stage`, per-flag `set_at_seq`, and the request (section 8) |
| crc32          | Over everything above |

Flags:

| Flag             | Persistent | Effect |
|------------------|------------|--------|
| IGNORE_ONCE      | one boot   | Skip verification failure for the next boot only. Cleared before handoff. |
| UPDATE_KEY       | one boot   | Apply the signed `key_update` block, then clear. |
| SECURE_OFF_ONCE  | one boot   | Treat `secure_mode` as off for the next boot only. |
| FORCE_RECOVERY   | one boot   | Boot recovery instead of the OS. |

Setting `secure_mode` permanently to off is done through the same signed
mechanism as a key update, not by a bare flag.

### 5.1 Requests: signed or unsigned

Requests that change trust state (key update, changing `secure_mode`, eFuse
requests) depend on whether secure boot is enabled. Here "enabled" means
`secure_mode` is warn or enforce.

| Secure boot | Requests | eFuse key slot | Key bag |
|-------------|----------|----------------|---------|
| Enabled | Must be signed by a valid key in the bag | Not burned unless the gated flow in section 8 runs | Consulted for images and requests |
| Disabled | Accepted unsigned | Left open | Stored and editable, but not consulted |

- Turning secure boot on is allowed unsigned while it is off, but only if the
  key bag holds a valid key for both roles. Otherwise the request is rejected,
  because enabling it would lock the system out.
- Turning it off while enabled needs a signed request.
- eFuse burning (section 8) is only available while secure boot is enabled, so
  a disabled system cannot burn anything.

### 5.2 The trust block and the effective mode (built 2026-10-25)

`purrcfg` stays the same size and version; two spare bytes become `tighten` and `trust_flags`, and
the 64 reserved bytes become `trust_sig`. `secure_mode`, `trust_flags`, `revoked_keys` and
`keys[]` are the **signed trust block**: `trust_sig` is an ECDSA P-256 signature, by a boot-role
key among the **compiled-in default keys only** (never the overridden bag), over

```
"PURRTRUST" 0x00 | secure_mode | trust_flags | revoked_keys (u32 LE) | keys[8] (68 bytes each, as stored)
```

(`purr_trust_signed_bytes()`; `purrstrap/lib/purrcfg.py` builds the same bytes; both sides pin the
field offsets in tests). `PurrOS/components/coreos/src/purr_trust.c` computes the **effective
state**, shared by the bootloader and the OS and tested on the PC:

```
base = signature_ok ? secure_mode : warn            (unknown values count as warn, never off)
mode = max(base, tighten)
dev  = DEV_MODE flag set and not (signature_ok and DEV_LOCKED)
if dev and mode > warn: mode = warn
```

- `tighten` and `PURR_CFGF_DEV_MODE` are unsigned and written by the OS; `PURR_TRUSTF_DEV_LOCKED`
  is signed.
- Override keys and `revoked_keys` take effect only when `signature_ok`; the OS gets the same
  through `purr_trust_view()`, a copy that cannot be stored back (`purr_cfg_store()` refuses it).
- `IGNORE_ONCE` and `SECURE_OFF_ONCE` are honoured only when `dev` is true; otherwise they are
  cleared and ignored.
- The signature is checked **once per boot**, only when a signature is present (about 250 ms at
  the bootloader's 80 MHz), and shared by the boot package loader and the slot checks.
- Not built: replay protection (no counter in the signed bytes); signing the version floors.

## 6. Boot flow

1. Init, load the partition table, read `purrcfg`. If both copies are invalid, use defaults
   (secure_mode = warn) and continue.
2. If `UPDATE_KEY` is set, validate and apply the key update, then clear it.
3. Pick a default target -- **built and proven on hardware, 2026-09-28, modular boards:** try
   `kernel` first; if it is not usable, fall back to `kittenos`; if that is not usable either,
   fall back to the recovery loader (`loader`/rescue) automatically, the same target "internet
   recovery" already uses. A slot is **usable** (`purr_slot_usable()` in `purr_boot.c`) when its
   first byte is the ESP app-image magic (`0xE9`) and, **in `enforce` mode only**, its signature
   trailer verifies (section 3.1). In `warn` and `off` a slot that merely looks like an app image
   is usable. Verification costs a hash over the whole image, so it runs only when it can change
   the answer: for every slot examined along the way in `enforce`, and in `warn` for the slot
   actually being launched (so the handoff can report it). In `off` nothing is verified.
   `SECURE_OFF_ONCE` treats the mode as `off` for this boot, and `IGNORE_ONCE` lets a failed slot
   boot even in `enforce`; both are cleared on this boot, before anything acts on them. A
   `secure_mode` value the bootloader does not recognise is treated as `warn`, never `off`.
3a. **Built: a two-tier failure-count ladder,** on `boot_fail_count`. The *bootloader* increments
   it (in `purr_boot.c`, before anything else reads it) on every single boot, not the boot package
   as earlier drafts said: the boot package never runs on a `FORCE_RECOVERY`/`FORCE_LOADER` boot,
   and the ladder needs every attempt counted the same way. CoreOS/KittenOS reset it to 0 once
   they mark themselves healthy (`coreos/SPEC.md` section 4 step 8), so the count only climbs
   across boots where nothing ever got that far. Because the increment comes first, the count a
   boot *sees* is already 1 on the first attempt, and the code's thresholds are `> 3` and `> 6`:
   **1-3, prefer `kernel`; 4-6, prefer `kittenos`; 7+, prefer `loader`.** (The code is the
   authority here; an earlier draft of this section said 0-2 / 3-5 / 6+.) Each tier falls through
   the other two slots in order if its preferred one is missing or does not look bootable --
   a missing loader at tier 3 falls back to `kittenos`, not back to the kernel that has just
   proven unreliable. Both thresholds are defaults, expected to need tuning once real hardware
   failure data exists. Reaching the `loader` rung this way -- through the count, not through a
   human picking "internet recovery" or the shell's `reboot loader` -- skips the boot menu and
   sets `PURR_CFGF_AUTO_REINSTALL` in `purrcfg` and the handoff `boot_state`
   `PURR_BOOT_AUTO_REINSTALL` (section 7), so the recovery loader can tell a repeated, undetected
   failure apart from a deliberate request and run its silent auto mode instead of a menu nobody
   is there to answer (`RecoveryLoader/SPEC.md` section 2.1).

4. `FORCE_RECOVERY`/`FORCE_LOADER` (the shell's `reboot recovery`/`reboot loader`) override
   that default once, unconditionally, without even the shallow check -- but see step 6: the
   chosen slot still goes through the final gate.
5. Otherwise the boot package's menu can override the default with an explicit choice,
   including "internet recovery" regardless of what the default fallback picked. The menu is
   given `purr_slot_usable()`, so in `enforce` mode it never offers a slot the bootloader would
   refuse.
6. **Final gate (built 2026-10-25).** Whatever chose the slot -- the ladder, a one-shot request,
   the continue-install override or the menu -- the slot actually being launched must be usable.
   An explicit request is not a way around `enforce`. If it is not usable, the bootloader takes
   the first usable slot in the ladder's order; if there is none it logs and resets (see step 7
   for what the reset means). Then, for any mode but `off`, the launched slot's trailer is
   verified (once; the result is cached) and the outcome goes into the handoff (section 7):

| Result | off | warn | enforce |
|--------|-----|------|---------|
| Verified | boot | boot | boot |
| Failed or no trailer | boot (not checked) | boot, `FAILED_WARNED` | slot unusable; falls down the ladder |
| Failed, and `IGNORE_ONCE` set | boot (not checked) | boot, `FAILED_WARNED` | boot, `FAILED_WARNED` |

7. If KittenOS is the target and is missing, damaged or fails verification: on a modular board start
   the recovery loader, which downloads a new KittenOS (`RecoveryLoader/SPEC.md`). On a board
   without one, in `off` and `warn` boot KittenOS with the warning, and in `enforce` print the
   serial prompt and wait. Open item: decide whether "wait" means a reset loop or a halt.
   **Today, when nothing is usable, the bootloader resets**, which keeps raising `boot_fail_count`
   and so ends up at the loader's rung. In `enforce` mode with unsigned slots that is a reset
   loop, so release builds should not use `enforce` until every writer signs its slot.
8. Clear one-shot flags, write `purrcfg` if it changed, write the handoff struct, jump.
9. When the boot package returns its menu choice, load the chosen slot through the same stock
   ESP-IDF `bootloader_utility_load_boot_image()` every slot already goes through -- the
   packed image on monolithic boards, the **kernel** partition on modular ones. Same
   mechanism both tiers; only which slot the package is allowed to choose differs.

**Tested for real, 2026-10-25, on a T-Deck Plus (the PURR bootloader, signed boot package and
signed kernel slot):** `purr_boot: PURR OS bootloader`, `secure_mode: warn`, `bootpkg 0.1.0 loaded
(key id 1, ok)`, the boot menu drawn, `slot @0x1a0000: ok`, `booting app slot 0`. The handoff reads back as `verified` in the shell's `purrcfg` command. The
`enforce` refusal and the failure ladder's higher rungs have not yet been exercised on the device.

**Tested for real, 2026-09-28, on a T-Deck Plus:** planted a real signed image at both
`kernel` and `kittenos`. Booted `kernel` by default with no menu interaction needed
(`booting app slot 0`). Zeroed `kernel`'s first word (simulating a dead slot) and rebooted:
correctly logged `kernel not bootable: falling back to KittenOS` and booted `kittenos`
instead (`Loaded app from partition at offset 0x20000`). The third level (both dead ->
`loader`) is written the same way but untested on this device, because `loader` is currently
blank (`0xFF`) -- the real RecoveryLoader has never actually been flashed onto it, a separate,
already-known gap, not a flaw in this chain.

The kernel and CoreOS files are verified with the same rules
(`PurrOS/components/coreos/SPEC.md` section 3.5).

**One bootloader, built from here (changed 2026-10-25).** `bootloader/` is the only place the
bootloader's source lives (`bootloader_components/main/`). `PurrOS/` and `Kernel/` each carry a
two-line `bootloader_components/main/CMakeLists.txt` that includes the same
`purr_bootloader_component.cmake`, so `idf.py flash` from `PurrOS/` or `Kernel/` now builds and
flashes *this* bootloader instead of IDF's stock one. Before this, an entire session of hardware
testing (the fallback chain, `purrcfg` flags, `boot_fail_count`) ran against the stock bootloader
without anyone noticing, because the two boot a plain app partition identically for the simple
cases -- the difference only shows for what `purr_boot.c` owns (the ladder, the one-shot flags,
the boot package and menu, the handoff). The tell, if it ever recurs, is a missing
`purr_boot: PURR OS bootloader` log line. Everything the bootloader needs from the
`sdkconfig` is set in every project's `sdkconfig.defaults` (notably
`CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC`, section 7); an already-configured build directory
does not pick new defaults up, so delete its `sdkconfig` after changing them.

`bootloader/` still builds standalone for the bootloader alone. In that case flash only
`build_<board>/bootloader/bootloader.bin` at `0x0`, never the whole suggested `flash_args`,
which would also overwrite the real app in the `kittenos` slot with that project's placeholder
stub `main/` app.

## 7. Handoff to the OS

**Built 2026-10-25 (written by `purr_handoff.c`, read by `purr_kernel_handoff()`; not yet
built or run on hardware).** A small struct in the RTC FAST memory area that ESP-IDF reserves for
custom use (`CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC`, 20 bytes since handoff version 2, which added `secure_state`). It has a fixed address shared by
the bootloader and the app, survives a soft reset but not power loss, and needs the same option
and size in both builds (every project's `sdkconfig.defaults` sets both). It carries its own magic
and CRC, so a cold boot, or a stock bootloader, reads as "no handoff". The bootloader writes it
once, right before jumping, then reseals IDF's own CRC over the retain block so the app side does
not see the block as corrupt:

| Field         | Meaning |
|---------------|---------|
| magic, crc    | Validity (`purr_handoff_valid()`, host-tested) |
| boot_state    | see below |
| secure_state  | what the bootloader enforced: effective mode (bits 0-1), development mode (bit 2), signed trust block (bit 3) |
| key_id_used   | Which key verified the image, or 0xFF. Always 0xFF until per-image verification exists (section 3). |
| flags_used    | Which one-shot flags were consumed this boot (`PURR_CFGF_*` bits) |
| bootloader_v  | Packed version, `PURR_BOOTLOADER_VERSION` in `purr_handoff.h` |

`boot_state`: `VERIFIED` (reserved, nothing sets it yet), `FAILED_WARNED` (reserved for the
enforce/warn policy in section 6 step 6), `FORCED_RECOVERY` (a `FORCE_RECOVERY`/`FORCE_LOADER`
flag was consumed), `CONFIG_DEFAULT` (`purrcfg` unreadable, defaults used), `AUTO_REINSTALL`
(the failure-count ladder sent the boot to the loader), and `UNCHECKED` -- an ordinary boot whose
slot passed only the shallow app-image magic check. `UNCHECKED` exists so the handoff never
claims `VERIFIED` for something no signature was checked on; it becomes `VERIFIED` when section
3's open item is built.

CoreOS reads this first, decides what to do with it, and records any warning
for the layers above to show. The shell's `purrcfg` command prints it. The bootloader itself
never shows anything.

## 8. eFuse support (optional, gated)

Off by default. Compiled in only when `CONFIG_PURR_EFUSE_SUPPORT=y`. A build
without it cannot burn anything.

eFuse writes are permanent and can brick a board, so a burn requires three
separate arm flags, each set in a different boot, in order:

1. Running code sets `EFUSE_ARM_1`, then reboots.
2. Bootloader sees ARM_1, advances `arm_stage` to 1, clears ARM_1. The running
   system then sets `EFUSE_ARM_2` and reboots.
3. Bootloader sees ARM_2, advances to stage 2, clears ARM_2. The running system
   sets `EFUSE_ARM_3` and reboots.
4. Bootloader sees ARM_3 with stage 2 and runs the burn on this boot.

Reboot enforcement:

- The bootloader increments `boot_seq` in `purrcfg` once per boot, before it
  looks at any arm flag. Only the bootloader writes `boot_seq`.
- When running code sets an arm flag it records the `boot_seq` it is running
  under in that flag's `set_at_seq`.
- A stage advances only if that flag's `set_at_seq` is exactly `boot_seq - 1`:
  it was set in the immediately preceding boot session, and at least one
  bootloader run has happened since. A flag set in the current session, or
  set several boots ago, is rejected.
- The stage-3 burn additionally requires that ARM_3 was set in the boot right
  before this one, so a full three-reboot sequence is always needed. Setting
  all three flags at once, or setting a flag and burning without a reboot,
  cannot work.
- The bootloader also requires the reset reason to be a software or power-on
  reset. A watchdog or brownout reset does not count as a deliberate reboot
  and resets the arm sequence.
- At most one stage advances per boot. A boot with a missing, extra,
  stale or out-of-order flag resets `arm_stage` to 0 and clears all arm flags.
- The system must have booted successfully between arms, since the next flag is
  set by running code.
- The burn needs a signed `efuse_request` in `purrcfg`: action ID plus its
  parameters, signed by a valid key. A bare flag edit cannot trigger a burn.
- Preconditions at burn time: `secure_mode` = enforce, the OS and recovery
  images both verify, and a dry-run report has been written to the log.
- One action per arming cycle. v1 defines only "program secure boot key
  digest". Other actions (disable JTAG, disable ROM download, flash encryption)
  are reserved and not implemented.
- After a burn, read the eFuse back, verify it, record the result in the
  handoff struct and reset `arm_stage` to 0.
- Develop and test on a sacrificial board first.

## 9. The boot package and the boot menu

The bootloader stays small and generic, one build per chip family. Everything specific to one
board that happens before the kernel lives in the **boot package**.

**Boot package**

- A signed `.kitt` (module role `bootpkg`) in its own raw partition, right after the
  bootloader. One per board.
- It holds the board's quirks it needs and the **boot menu**: starting the display, reading
  the board's keys or buttons, and drawing the menu.
- **On modular boards it picks the `kernel` slot** the same way monolithic boards pick their
  packed image (below): it returns the choice from the app-slot list the bootloader already
  handed it, and the bootloader verifies and jumps into it (section 6 step 8). No LittleFS
  read and no PSRAM relocation happen inside the package itself -- that machinery belongs to
  the kernel, one layer up, loading CoreOS (`PurrOS/SPEC.md` section 6). The
  bootloader, not the package, increments `boot_fail_count` (section 6 step 3a).
- **On monolithic boards it only shows the menu** and returns the choice to the bootloader,
  which loads the packed image. Modular and monolithic boards now share this exact
  mechanism -- the only difference is which slot is on offer.
- **It is only for boot.** The drivers and hardware description for the kernel and CoreOS are a
  separate bundle (`PurrOS/components/kernel/SPEC.md` section 13).
- It is small, tens of KB to be measured, so it does not repeat the bootloader's size problem.

**How it is loaded**

1. The bootloader verifies the package like any other image (signature, chip, role). An
   unsigned package is only accepted when `secure_mode` is off.
2. It copies the package into a fixed RAM window that the bootloader reserves. The package is
   linked for that address, so it needs no relocation.
   **Flash mapping rule (fixed 2026-10-25):** the bootloader stage allows exactly one
   `bootloader_mmap()` at a time ("tried to bootloader_mmap twice" was a real crash). The
   package's own partition is the only thing ever mapped, and `load_package()` takes that mapping
   last, after `purrcfg` is read. The partition table is read once into a RAM copy with the plain
   flash reader, so partition lookups never map and can happen in any order.
3. It calls the package with a small service table (flash read, delay, GPIO, log). The package
   offers a small set of calls back: start the display, run the menu with a timeout, read a
   key, and return the choice.

**Boot menu**

- Shown for a short, configurable time at power-on, like a PC's "press a key for setup".
- **If ignored, the system boots normally.**
- Entries are open. At least: boot normally, and boot KittenOS (recovery).
- The bootloader acts on the returned choice: it sets the boot target and continues.
- **Secure Boot status line (built 2026-09-30).** Reads the ESP32-S3's own hardware Secure Boot
  eFuse directly (`EFUSE_RD_REPEAT_DATA2_REG` bit 20, the same bit `esp_secure_boot_enabled()`
  reads through the HAL -- not usable here since the boot package is a standalone freestanding
  build with no ESP-IDF headers, so `pkg_hw.c` reads the register directly, matching its
  existing bit-banged-register style). Read-only, never programs anything. Shown so "is it
  enabled" always has a real, zero-risk answer, independent of whether the actual eFuse burn
  (this section's armed, three-reboot procedure) is ever used. Confirmed on hardware: reports
  `off` correctly (the eFuse has never been burned on the test device).

**Decided for the first cut (T-Deck Plus, built and checked on hardware)**

- **Partition:** a raw data partition named `bootpkg`, type data, subtype `0x40`, at `0x12000`,
  56 KB, right after `phy_init`. The bootloader finds it by name in the partition table.
- **RAM window (ESP32-S3):** the bottom of internal SRAM, which is free before the system
  loads. It is seen twice: as data at `0x3FC88000` and as code at `0x40378000` (the two are the
  same memory). The first 32 KB is code, the next 32 KB is data and bss. The package is linked
  for those addresses. The addresses and the preamble are in `purr_abi.h`. Other chips get their
  own window when their boards are done.
- **Payload:** a 16-byte preamble (code size, data size, bss size, entry address), the code,
  then the data. The payload length is a multiple of 4, because the bootloader's hardware SHA
  only takes whole words.
- **Checks:** the bootloader loads `purrcfg` (defaults to `secure_mode = warn` if it cannot),
  builds the key bag (the compiled-in defaults plus any valid `purrcfg` overrides), and calls
  `purr_image_verify` (magic, header version, chip, layout, payload SHA-256, and the ECDSA
  P-256 signature against the key bag; a boot-role key only) over `PurrOS/components/coreos`'s
  shared `purr_verify.c`/`purr_keybag.c`, with `uECC_verify` (the `micro-ecc` component ESP-IDF's
  own secure boot already vendors) as the P-256 backend. The bootloader separately checks the
  header claims to be a boot package (`PURR_MOD_BOOTPKG`) before trusting the payload as one,
  since a validly-signed image of a different subtype must not be run as if it were this one. An
  image that fails verification is only accepted while `secure_mode` is off; otherwise the
  package is treated as missing (the fallback below). Checked on the T-Deck Plus: an unsigned
  package is rejected under the default `warn` mode, and a package signed with the boot-role
  dev key (generated and applied with `purrstrap keys`, `../Keys/SPEC.md` section 3) loads and
  runs. The default key bag's C source lives at `PurrOS/components/coreos/keys/` (public keys
  only, tracked in git; the matching private keys are generated locally into the gitignored
  `signing_keys/`).
- **Service table** (`purr_boot_services_t`): a version, `log`, `delay_us` and `gpio_setup`.
  Flash reads are not offered, because the bootloader reads the partition table itself and hands
  the package the list of app slots and which of them start with a valid image.
- **Entry:** `purr_pkg_entry(services, parts, nparts, preferred)` returns the slot to boot, or
  -1 to leave it to the bootloader.
- **No drivers:** the package drives the panel with bit-banged SPI and the keyboard with
  bit-banged I2C, writing the GPIO registers directly. It is slow (a few MHz) and needs no code
  from the bootloader, so it stays small (about 4 KB). Bit-banging is only for the menu. The
  kernel has real drivers.
- **Menu rules:** in `PurrOS/components/coreos/src/purr_menu.c`, plain C with host tests. One
  entry per bootable slot, a 3 second countdown that any key stops, and, when nothing can boot,
  "Internet recovery" after 2 seconds. Internet recovery is a stub until Milestone 2, and the
  package says so on screen and shows the menu again.
- **Keys:** the T-Deck has no arrow keys, so menus use W and S to move and D or Enter to
  choose. Text fields (the Wi-Fi password, for example) take the raw characters instead.
- **Built by** `purrstrap bootpkg build`, which compiles, links, and wraps the result.

**Fallback.** If the package is missing, invalid, or crashed the last time (a crash counter in
`purrcfg`), the bootloader skips the menu. On a modular board it then starts KittenOS, which
has its own filesystem reader, because without the package nothing can read the filesystem. If
KittenOS fails too, it prints a prompt on the serial console.

**Updating the package.** It changes rarely. KittenOS writes it into its raw partition from a
verified file.

### 9.1 The quiet start (2026-10-28, boot package 0.3)

The boot menu used to be the front door: every start loaded the package, verified each slot behind a progress bar, and counted down three seconds. It is now only
shown when it is needed.

A start goes like this (keyboard boards; the e-ink board keeps the old menu):

1. The bootloader picks the slot as before (the failure ladder, the one-shot requests, the continue-install override). Nothing about that changed.
2. **Fast startup** (`PURR_CFGF_FAST_START`, Settings > Devices > Startup) with nothing wrong: no package is loaded and nothing is drawn. The slot is
   checked exactly as in every other mode and the bootloader jumps. Without Fast startup: the package draws the **splash** (logo, board and version, "Starting PURR OS",
   the status chips; no bar), the slot is checked behind it, and the keyboard is watched for 300 ms.
3. The **menu** appears instead when any of these holds (`purr_menu_reason`, `PURR_MENU_WHY_*`): Space was pressed (the key will become a setting; the SDK is to expose
   the mapping, plan5.md), the last start never reached a healthy boot (`boot_fail_count` above 1), the kernel is present but fails its check (the ladder would
   otherwise have quietly moved on), or Settings asked for it (`PURR_CFGF_SHOW_MENU`, cleared when read). Any of these turns Fast startup off for that start.
4. The menu lists what can start (PURR OS, KittenOS), greys what cannot ("unusable"), and describes the highlighted entry. On a failed check it states why in two lines
   and adds **Restart**. The internet reinstall is no longer a menu entry; it is reached from KittenOS. Nobody pressing a key for 20 seconds starts the DEFAULT entry
   (the ladder's pick), not whatever was highlighted last; a key press starts the 20 seconds again. A device with nothing bootable still offers the recovery loader.

Boot package versions: a package older than 0.3 is given the old menu with the progress calls as before; none at all means no screen and the ladder's slot. Boot package
0.3 asks for services version 4 (`menu_reason`, `menu_idle_ms`, `detail1`, `detail2`, the SPLASH and KEYCHECK stages, the RESTART choice).

Fast startup cannot skip the bootloader itself (the chip's ROM runs it on every power-up) and does not skip the signature check, so enforce means what it did. Time saved is the
package load, the display start-up and the countdown.

The logo is "PURR OS!" in bold italic, white with an orange "!", drawn from the 8 x 8 font (each pixel row is a run, leaned a quarter pixel per row, and one pixel wider).

## 10. Open questions

- Header-first container vs. trailer, pending the IDF loader check (section 3).
- Recovery failing verification in enforce mode: reset loop or halt.
- Which role's key may sign each kind of request (key update, `secure_mode`
  change, eFuse request). The obvious rule is that only the KittenOS role can,
  so a compromised OS-signing key cannot touch trust state.
- Whether KittenOS may replace the PURR OS image and edit `purrcfg` directly
  when secure boot is disabled, or whether that also needs a request.
- Where the package crash counter lives in `purrcfg`.
- The RAM windows for the ESP32 (CYD 2.4C) and whether the package is one program with two modes (menu only on monolithic boards,
  loader on modular ones).
- How the boot target is recorded in `purrcfg` and how KittenOS is started.
