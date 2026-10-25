# plan1: boot chain, per-image signature trailer

**Source of truth for this work.** Re-read this file after any context compaction before doing
anything. Update the Status table and append to the Session log as work happens, and before a
compaction dump everything new that is not already here. If this file and the code disagree,
find out which is right and fix one of them in the same step.

Last updated: 2026-10-25. Nothing in this plan is committed. **The new bootloader, boot package
and signed slots are running on the T-Deck Plus** (full flash done, see Hardware results below).

## Goal

The bootloader launches `kernel`, `kittenos` and `loader` after only a shallow check (first byte
is `0xE9`). Make it verify a real PURR signature on each, with the `off`/`warn`/`enforce` policy
from `bootloader/SPEC.md` section 6, and report the result in the handoff. Then SECURITY.md's
main gap closes.

## Design (decided)

**Trailer, not header.** These slots are real ESP-IDF app images and the stock loader needs the
ESP magic at partition offset 0, so the PURR header cannot go first.

- The signature lives in the **last 4096 bytes (one sector) of each partition**: a verbatim copy
  of the signed container header that the release file (`.cat`/`.kitt`) already carries, then
  `0xFF`.
- The ESP image (the container's payload) stays at offset 0, untouched.
- Container facts: `purr_image_header_t` is 173 bytes; `header_size == payload_offset == 173`
  for everything `purrstrap` builds (`purrstrap/lib/image.py` `make_image`).
- Verification treats the partition as the container it was cut from: header bytes come from the
  trailer, payload bytes from partition offset 0. The ordinary `purr_image_verify()` then does
  the rest (chip, type/role, version floor, key, revocation, payload hash, signature).
  Implemented in `PurrOS/components/coreos/src/purr_trailer.c` (virtual reader).
- Max image size per slot = `part_size - 4096`.
- The partition decides what it may hold, not the header: kernel = `PURR_IMG_MODULE` +
  `PURR_MOD_KERNEL`; kittenos = `PURR_IMG_RECOVERY`; loader = `PURR_IMG_MODULE` +
  `PURR_MOD_LOADER`. All boot-role keys (`purr_role_may_sign`).
- `PURR_V_BAD_MAGIC` from the trailer verifier means "no trailer" (slot flashed straight from a
  build); anything else means a trailer that is present and wrong.

**Full payload hash, not "sign IDF's appended digest".** The earlier pitch bound the signature to
the SHA-256 that IDF appends to every app image, to avoid hashing ~1 MB at boot. Decided against
for the first cut: it would depend on IDF's loader verifying that digest, which has not been
checked in 5.3.5, and reusing `purr_image_verify` unchanged is host-testable. Cost: software
SHA over the image in the bootloader (estimate 100 to 400 ms for ~1 MB, not measured). Revisit
as an optimization once measured (digest binding, or the hardware SHA block).

**Policy** (`secure_mode` from `purrcfg`; `SECURE_OFF_ONCE` treats it as `off` for this boot):

| Slot result | off | warn (default) | enforce |
|---|---|---|---|
| verified | boot | boot | boot |
| not verified (including no trailer) | boot | boot, warning recorded | slot unusable (falls down the ladder); usable only if `IGNORE_ONCE`, with warning |

- `IGNORE_ONCE` and `SECURE_OFF_ONCE` are consumed (cleared) this boot, before the jump.
- Default stays `warn`, so the dev flow (`idf.py flash` of raw images) keeps booting with a
  warning.
- Every path is gated, including `FORCE_RECOVERY`/`FORCE_LOADER`/`CONTINUE_INSTALL` and the
  boot menu's choice: a final check on the slot actually being launched.
- The boot package's menu must not offer a slot that fails the same predicate in `enforce`.
- **Known risk:** `enforce` with unsigned slots gives "no bootable app found" and a reset loop
  (`boot_fail_count` keeps climbing). The `bootloader/SPEC.md` open question (wait vs halt)
  decides this; do not default release builds to `enforce` until the writers sign every slot.

**Handoff state precedence** (`bootloader/SPEC.md` section 7): `FAILED_WARNED` (chosen slot
failed but policy let it boot) > `AUTO_REINSTALL` > `FORCED_RECOVERY` > `VERIFIED` (chosen slot
verified; `key_id_used` = its key) > `CONFIG_DEFAULT` > `UNCHECKED`.

**Writers** (all must write the trailer after the payload; each already erases the whole slot
first, so the trailer sector is clean):

- `PurrOS/main/commands.c` netinstall, around lines 1000-1060 (kernel, loader; bootpkg writes
  the whole container and is NOT part of this change).
- `PurrOS/main/recovery_loader.c`, `install_kittenos()` around line 365, and the component
  installer around line 564.
- Each: check `payload_size <= slot->size - 4096`, write the header copy at
  `slot->size - 4096`, read it back and compare.
- `purrstrap`: a way to produce a flashable slot image (payload, `0xFF` padding, trailer) for
  development flashing with esptool.

## Status

| # | Item | State |
|---|---|---|
| 0 | Ladder spec matches code; handoff written (bootloader) and read (kernel, `purrcfg` command); `bootpkg` 56 KB, `purrcfg` moved to 0xD000, `nvs` 16 KB; mmap fix; one shared bootloader component for `bootloader/`, `PurrOS/`, `Kernel/` | done, uncompiled with IDF, untested on hardware |
| 1 | `purr_trailer.h/.c` (shared verifier) | done, host-tested |
| 2 | `test_trailer.c` host test (42 checks) | done, passes |
| 3 | Bootloader: `purr_slotverify.c/.h` (aligned flash reader, key bag) | done, compiles; not run on hardware |
| 4 | `purr_boot.c`: policy, flag consumption, `purr_slot_usable()`, final gate, handoff VERIFIED/FAILED_WARNED + key id | done, compiles; not run on hardware |
| 5 | `purr_bootpkg_run`: menu takes the same predicate | done, compiles |
| 6 | Writers write + read back + verify the trailer: `main/purr_slot_trailer.c/.h` helper; `net_install` (commands.c), `install_kittenos` and `fetch_and_install_module` (recovery_loader.c); the loader's "already good" checks for kernel/kittenos now also require a valid trailer unless `secure_mode` is off | written; compile check pending (PurrOS build) |
| 7 | `purrstrap coreos slot-image` (whole-partition image with trailer, sizes from the board CSV) | done; checked end to end with the real boot key and the C verifier (ok / bad-hash / no-key on tampering) |
| 8 | Specs: `bootloader/SPEC.md` sections 2.1, 3.1, 6, 7; `SECURITY.md` (section 4.1, tables) | done; `PurrOS/SPEC.md` mention still todo |
| 9 | Full firmware builds (PurrOS tdeck_plus full/recovery/minimal built; Kernel/ not built); hardware checks | PurrOS builds done; first hardware boot done (below); remaining checks listed below |

## Findings from the first real IDF build (2026-10-25)

- **CMake and Ninja** are not installed by IDF's `install.sh` on macOS (it expects Homebrew,
  which Intel Macs no longer have). Fixed with `python tools/idf_tools.py install cmake ninja`
  from `~/esp/esp-idf-v5.3.5` after `. ./export.sh`; they live in `~/.espressif/tools`.
- **`CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE` is a hex option.** `16` meant `0x16` and failed
  IDF's multiple-of-4 assertion. Now `0x10` in all three `sdkconfig.defaults`.
- **IDF wipes the whole RTC retain block** (custom area included) at the end of
  `bootloader_utility_load_boot_image()` if its CRC is invalid, which it is on a cold power-on.
  A handoff written before the jump would have been erased on cold boots. `purr_handoff_write()`
  now makes the block valid first (reset + CRC update when the reboot counter is 0), then writes
  `custom`. `CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC` must stay off (a `#error` checks it);
  the custom area is outside IDF's CRC, so no reseal is needed after writing it.
- **The RTC API guesses held:** `rtc_retain_mem_t.custom` is a `uint8_t[]`;
  `bootloader_common_get_rtc_retain_mem()` is usable from the app (IDF's own test app does).
- **Bootloader size:** the committed bootloader was already 32,688 bytes against the 32,768
  limit (80 bytes free). With the trailer verification and handoff it was 34,400. Setting IDF's
  own bootloader log level to WARN (`CONFIG_BOOTLOADER_LOG_LEVEL_WARN`) while the PURR files
  override back to INFO (`#define LOG_LOCAL_LEVEL ESP_LOG_INFO` at the top of each of their .c
  files) brings it to **32,336 bytes, 432 free**. That is almost nothing: the next bootloader
  feature will need the partition table to move (or software SHA replaced by the hardware/ROM
  SHA, which is also much faster). The bootloader already uses `-Os`.
  **Side effect:** IDF's own INFO lines (for example "Loaded app from partition at offset ...")
  no longer print; the PURR lines (`purr_boot: PURR OS bootloader`, `booting app slot N`,
  `slot @0x...: ok`) still do.
- The layout has no spare room before `kittenos` at 0x20000: table 4 KB, nvs 16, purrcfg 8,
  otadata 8, phy_init 4, bootpkg 56 = 96 KB exactly. Growing the bootloader means shrinking or
  dropping something (`otadata` is the candidate: the PURR bootloader ignores it, but
  `esp_ota_set_boot_partition` in netinstall needs it today).
- Scratch build dirs (outside the repo): `.../scratchpad/blbuild` (bootloader, tdeck_plus),
  `hbuild` (the committed bootloader, for comparison), `posbuild` (PurrOS tdeck_plus-full).

## Hardware results (2026-10-25, T-Deck Plus, first boot after the full flash)

Flashed (all hash-verified): PURR bootloader at 0x0 (32,336 bytes), new partition table at
0x8000, signed boot package at 0x12000, KittenOS (recovery profile) at 0x20000, the full OS in
`kernel` at 0x1A0000, the recovery loader (minimal profile) at 0x380000, each slot image with
its signature trailer. `nvs`/`purrcfg`/`otadata`/`phy_init` area (0x9000..0x20000) erased;
`root`, `netrec`, `ui_assets` untouched. Signed with the boot key (id 1). Images and slot
files are in the session scratchpad `.../scratchpad/flash/` (not in the repo).

Boot log, in order: `purr_boot: PURR OS bootloader`; `secure_mode: warn`; `bootpkg 0.1.0 loaded
(key id 1, ok)`; boot menu drawn (`BlackDeck+ (tdeck_plus)`, secure boot HW off); `slot
@0x1a0000: ok`; `booting app slot 0`; the full OS ran (`internaldirtytrailer-5`).

- **Trailer verification works on real hardware** (signed kernel slot verified).
- **Measured cost: about 2.0 s** at first (menu "booting the chosen slot" at 4873 ms, `slot ...
  ok` at 6857 ms). A timing build split it: 328 ms flash reads, about 1300 ms software SHA-256
  (the bootloader runs at 80 MHz), about 250 ms the signature check. **Fixed in `plan3.md` item 2:
  hardware SHA plus 4 KB read-ahead brings it to 656 ms.**  (My guess that hashing was a small
  part, and that the hardware SHA would also save bootloader space, was wrong on both counts:
  hashing was most of it, and the software SHA stays linked for the header hash.)
- Expected one-off noise: `phy_init: failed to load RF calibration data` (erased nvs), and
  `drivers: hello_driver.cat ... built for driver bus ABI 0` (a stale file on `root`).

Still to check on the device (needs the keyboard):
- [x] (2026-10-25, hardening) rollback: an older validly signed kernel is flagged `below-version-floor` in warn mode (`plan3.md` item 3).
- [x] `purrcfg` in the text shell shows the handoff as `verified` (2026-10-25, user-confirmed on the T-Deck Plus): the bootloader writes it and the kernel reads it back.
- [ ] `reboot recovery` boots the recovery-profile KittenOS from the `kittenos` slot (and that
      it verifies); `reboot loader` boots the loader.
- [ ] Menu: pick KittenOS and "internet recovery" from the boot menu.
- [ ] A tampered slot (flip a byte in flash) shows `FAILED_WARNED` in `warn`, and is refused in
      `enforce` (set `secure_mode` via a signed request / `IGNORE_ONCE`).
- [ ] Failure ladder: seven power-cycles before the OS marks healthy escalates to the loader.
- [ ] `net_install kernel` / the recovery loader write a working trailer (not exercised yet).

## Hardware / build checklist (earlier, now mostly done)

- [ ] Delete cached `sdkconfig` in each build dir (new RTC and log defaults do not apply otherwise).
- [x] Bootloader builds and fits (432 bytes free). Still to see on a device: `purr_boot: PURR OS bootloader` in the log.
- [x] RTC API: `custom`, size option and `get_rtc_retain_mem` verified in the IDF source.
- [ ] The app side links `bootloader_common_get_rtc_retain_mem` (PurrOS full build in progress).
- [ ] `purrcfg` shell command prints a handoff.
- [ ] Reflash: bootloader, partition table, bootpkg region; erase `nvs`; `purrcfg` resets to defaults.
- [ ] Signed slot boots `VERIFIED`; unsigned slot boots with a warning in `warn`; unsigned
      slot is refused in `enforce`; tampered byte in the image fails.
- [ ] Measure verify time at boot.

## Open decisions

- `enforce` with nothing bootable: reset loop vs halt vs serial prompt.
- Digest binding as a speed optimization, once the cost is measured.
- Whether to also sign `purrcfg` trust fields (separate follow-up, see SECURITY.md section 11).

## Commands

- Host tests: `cd PurrOS/components/coreos/test/host && IDF_PATH=$HOME/esp/esp-idf-v5.3.5 python3 run.py [name-filter]`.
  Baseline before the trailer work: 17 test files, all pass.
- IDF: `. ~/esp/esp-idf-v5.3.5/export.sh` (v5.3.5 installed at `~/esp/esp-idf-v5.3.5`).
- Standalone bootloader build (from `bootloader/SPEC.md` section 6): `idf.py -B build_<board> -D SDKCONFIG=... -D IDF_TARGET=... -D SDKCONFIG_DEFAULTS=... build`; flash only
  `build_<board>/bootloader/bootloader.bin` at `0x0`.

## Facts worth keeping

- Signing keys: `signing_keys/boot.key`, `developer.key` (encrypted, gitignored). The old keys
  were deleted. The passphrase is not stored in any file.
- Working tree has about 90 files of CRLF churn from the Windows to Mac move; do not commit it
  blindly.
- Notes from earlier this session: `BOOTLOADER_SESSION_NOTES.md`, `SECURITY.md`,
  `UI/DISPLAY_NOTES.md`. Display plan: `plan2.md`.

## Session log

- 2026-10-25 (later): trailer verifier, host test, bootloader slot verification and policy written
  and compiled. IDF installed (cmake/ninja gap fixed). Found and fixed the RTC hex-size and
  cold-boot wipe problems. Bootloader size handled with WARN log level. Writers, purrstrap,
  specs still to do.
- 2026-10-25: design settled with the user (trailer approach approved); full-hash decision made
  by me, user not yet told in detail. `purr_trailer.h/.c` written.
