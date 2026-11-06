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

## Boot screens redesign (2026-10-27)
Decided with the user: one minimal frame for every boot screen. Cat's-eye mark, letter-spaced "PURR OS" wordmark, board
and version line; the entry rows in the middle (menu only); a thin progress bar low on the screen with one line of text
above it (stage and percentage, or the countdown and "any key stops"); the small status chips UNDER the bar. The bar is
always in the same place: it fills while something is verified or installed and drains while the menu counts down. Chips:
policy (ENFORCE / WARN / OFF), SIGNED, HW secure boot (only when on), input (kbd ok / NO KBD, or BOOT / PWR on e-ink);
DEV MODE is a filled amber chip. The sign-in-screen idea and the Lumia theme mockups were dropped (the OS already has its
UI; boot only complements it).
- Done: `bootpkg/src/pkg_ui.c/.h` (the look, hw_fill/hw_text only, 320x240 and 200x200 layouts), `pkg_main.c` menu rewritten
  on it, "Starting <slot>" hand-off screen, `purrstrap` builds it with `-DPKG_VERSION`. Package ~5.8 KB of 32 KB.
  Host renderer of the REAL code: `tools/boot/render_real.py` (PNGs in `out/boot-preview/`); `tools/boot/preview.py` is the
  original mockup. Ran in the VM (signed package loads, menu counts down, kernel boots); the look itself was checked on
  rendered PNGs, not on the VM screen.
- Done later the same day: (1) **verification progress**: bootloader services version 3; while it verifies the slots the
  bootloader calls the package's entry with `nparts < 0` (purr_abi.h, "a progress call"), only to a package whose version is
  0.2 or later; the package brings the display up on the first call and the menu continues on the same frame without
  clearing it. The bar steps per slot ("Verifying Kernel" 0 %, "Verifying KittenOS" 50 %, "Verified" 100 %); inside a slot
  there is no progress (`purr_slot_verify` is one call). Skipped on the e-ink board (each refresh takes seconds).
  (2) **re-install progress** in the recovery loader: `main/recovery_screen.c` compiles `bootpkg/src/pkg_ui.c` against the
  kernel display; stage line, bar by component (3 steps) plus bytes of the download in progress (new
  `purr_fetch_set_progress` hook in the kernel fetch code), RECOVERY / WIFI / SIGNED chips, a FAILED chip on halt.
  (3) **kernel start screen**: after the display is up the kernel shows the same frame with stage lines (starting services,
  network, preparing the shell, "Hold space: text shell", starting PURR OS) instead of log text; the text console returns
  for a text login, the space-bar escape, or a fallback from the graphical shell (which now also unmutes it).
  The bootloader is built in `out/boot_tdeck_plus` (the old `bootloader/build_*` folders came from Windows).
- Checked: host-rendered PNGs of the real code (T-Deck Plus and the 200x200 e-ink layout); all three boot packages build;
  bootloader, full, minimal (loader) and recovery profiles build; the VM boots through bootloader -> package 0.2.0 ->
  verification -> menu -> kernel screen -> automatic sign-in -> LVGL. NOT checked: how it looks on the VM or device screen,
  and the re-install screen has not run (it needs the recovery manifest server and a forced recovery boot).
- Still open: accent follows the OS accent (needs it stored in purrcfg; amber for now); a Montserrat subset for the
  wordmark (the pixel font differs from the shell's type); the display module re-initialises the panel at kernel start, so
  there can still be a brief blank between the package's hand-off and the kernel screen; progress inside a slot's
  verification; the image script patches only bootloader, package, kernel and trust block (loader and KittenOS slots keep
  the old builds, so the new re-install screen needs those two slots rebuilt into the image to be seen).

## KittenOS internet recovery: first-run setup (OOBE) (2026-10-27)
Decided with the user: the recovery experience is one wizard in the existing LVGL UI (W10M theme, the Settings app's own widgets,
now shared as `PurrOS/main/purr_w10m_widgets.c`), in two parts because recovery has two stages. Minimal: no header, no step
counter, one accent title and the page's controls.
- **Part 1, recovery loader (no filesystem):** (1) "Uh-oh, something went wrong. We need to recover this device." with the reasons
  the bootloader flagged, OK; (2) connect to Wi-Fi (scan list, a remembered network connects without a password, wired skips); (3) "The legal stuff & cool people" (legal and credits), a plain
  text file that is NOT built into the firmware: the loader has no filesystem, so it downloads `OTA/legal.txt` from the recovery
  server next to the manifest once Wi-Fi is up (lines starting with # are editor notes; if it cannot be fetched the page says so and
  carries on), and the installed system keeps it as `/etc/legal.txt` for Settings > About; (4) "Okay, getting this sorted out":
  progress while the restore runs. Restore everything (kernel too) when the reasons say the system cannot be trusted
  (`purr_reason_needs_full_restore`), otherwise only PURR OS.
- **Part 2, KittenOS first boot (has a filesystem):** pick a colour (accent grid + dark/light, restyles live), the account (replaces
  the text first-time setup), date and time, done. The rest of the system keeps installing in the background (a line and a thin
  bar in a small header) while the user answers; Finish waits for it, then restarts into PURR OS.
- **Reasons from the bootloader:** `purr_handoff_t.recovery_reasons` (PURR_RR_*: UNSIGNED, ALTERED, DAMAGED, ROLLBACK, REPEATED,
  NO_SYSTEM, REQUESTED, CONFIG, SYSTEM) in `purr_abi.h`; wording and the verify-result mapping in `purr_reason.c`;
  `purr_handoff_set_reasons`. Host tests: `test_reason` (34 checks) and `test_cfg` pass.
- **Done:** `purr_oobe.c/.h` (the pages, platform actions behind `purr_oobe_ops_t`, slow work as polled jobs), `legal.txt`,
  `purr_reason`, host LVGL renderer `tools/lvgl_host/render.py` (real LVGL 9.2 + the real theme and widgets + the real OOBE pages,
  PNGs in `out/lvgl_host/`; Settings now uses the shared widgets and the full firmware still builds).
- **Not done (needs approval of the look first):** the bootloader filling in `recovery_reasons` (purr_boot.c: slot verdicts, ladder,
  request, no system, config) and `purr_handoff_write`; the loader running this wizard (LVGL + theme in the minimal profile, real Wi-Fi
  scan/connect/remember, restore as a polled job reporting progress, replacing `recovery_screen.c`'s drawing); KittenOS first boot running
  part 2 (account creation job, the background stage-2 install as a job, restart); the loader's LEGAL job (fetch `legal.txt` beside the manifest, cap 16 KB), the restore also installing it as /etc/legal.txt,
  Settings > About > Legal reading /etc/legal.txt; the accent saved in purrcfg so the boot screens can follow it. None of it has run
  on the VM or a device.

### Update (same day): FRESH, reasons from the bootloader, tone
- **`PURR_CFGF_FRESH`** (purrcfg flag, bit 9) is ON BY DEFAULT: `purr_cfg_defaults()` and purrstrap's initial purrcfg
  (`keys trust-block`; `--installed` leaves it off, used by `tools/vm/mkimage_dbg.sh` because that image holds a working
  system). Meaning: "never reached a healthy boot of the full OS". Cleared only by the full OS in `purr_mark_boot_healthy`
  (not by the bootloader, the loader or KittenOS, so it survives the whole install). With FRESH set and no usable system the
  bootloader flags `PURR_RR_BLANK` and the loader's first page reads "Hi there, first time? This is a network install..." instead
  of "Uh-oh, something went wrong." (`purr_reason_is_first_run`: BLANK with no sign of damage or tampering).
- **Bootloader now fills `recovery_reasons`** (purr_boot.c `recovery_reasons()`): damaged / unsigned / altered / rollback from the two
  system slots' verdicts, NO_SYSTEM, BLANK, REPEATED (the failure ladder), REQUESTED (FORCE_LOADER or the boot menu's pick),
  CONFIG. Only computed when the loader is the slot being launched (it verifies the slots, so it is skipped on normal boots).
  `purr_handoff_write` takes the reasons. Builds; a normal boot in the VM is unchanged; the loader-launch path (the reasons
  themselves) has NOT been run: it needs a device or VM with the system slots erased and a key press at the boot menu.
- Copy is casual ("Let's get you connected", "We'll hop on Wi-Fi...", contractions) and the legal page is titled "The legal stuff &
  cool people". Host tests: the whole coreos suite passes (test_reason 42 checks); purrstrap trust-block and key tests pass (30).

### Update (same day): the wizard is wired in, and a dev server can host recovery
- **Wired in.** The recovery loader (minimal profile) now runs the setup wizard part 1 on LVGL (`recovery_loader.c`, jobs: Wi-Fi scan,
  connect + remember, dev server scan/verify, legal download, restore; `oobe_job.c` is the worker, `oobe_ui.c` the LVGL bring-up).
  KittenOS first boot (recovery profile, no accounts) runs part 2 (`oobe_firstboot.c`: colour, account via
  `purr_login_account_add_first`, date and time, done) while stage 2 installs in the background (`purr_stage2_take_pending` and
  `purr_stage2_step` in commands.c). The text flows stay as the fallback (waveshare e-ink, or LVGL failing). Unattended recovery
  (the ladder) opens on the install page and joins the saved network.
- **Found and fixed: the loader had no screen at all.** Since the display became a loadable module (commit 45c46edc) the loader, which has
  no filesystem, never bound a display ("display did not come up"). `purr_loader_load_drivers()` now loads the baked-in display, keyboard
  and touch modules from memory (`load_relocatable_image` split out of `load_relocatable_file`, verified like any module).
- **Dev server hosts recovery** (`purrstrap dev recovery`, `dev serve`): `/recovery/<file>` (plain names only, no way out of the folder),
  recovery.manifest + modules.manifest generated from the images in the folder (the device's C parser reads them), legal.txt.
  The server now announces itself network-wide every 3 s (`--announce`, 0 = off; limited broadcast plus each interface's), and
  `dev find` listens like a device (try it from another computer on the same AP). Devices (`purr_lan_discover.c`, also used by the
  portal) probe AND listen on the discovery port.
- **Gated by a bootloader flag.** `PURR_CFGF_DEV_RECOVERY` (purrcfg bit 10, `keys trust-block --dev-recovery`): the bootloader honours it
  only in development mode and reports `PURR_SECST_DEVREC` in the hand-off. Only then does the wizard add its extra page (after Wi-Fi,
  before the install): it finds servers, shows name + address + id, checks the server has recovery files for this board
  (`purr_recovery_verify`), the owner taps one and "Use it" (or "Use the internet instead"). The choice is remembered beside the
  saved network (netrec source record) so KittenOS stage 2 pulls from the same server (`purr_recovery_src.c` supplies the manifest,
  module index and legal URLs to the loader and commands.c). Images are still checked against the boot key before anything is written.
- **Checked:** whole C host suite and purrstrap tests pass (dev-recovery tests: manifests, safe names, the server serving only its folder,
  announcements); every profile and the bootloader build (loader 28 % free, KittenOS 23 %, full 14 %); in the VM the normal OS still
  boots, and a forced recovery shows the wizard (bootloader reasons 0x40, 3 drivers loaded from memory, LVGL, theme, "wizard part 1 is up").
  `tools/vm/mkimage_dbg.sh` has SCENARIO=normal|recover|first and DEV_RECOVERY=1; `out/vm-recover-devrec-flash.bin` is the recover image.
- **Not checked:** nothing has been clicked through: Wi-Fi scan/connect, the dev server page against a real server, the legal download, the
  restore itself (it needs a reachable server and writes flash), KittenOS first boot (account, stage 2 background install), the "first
  time?" page end to end (SCENARIO=first). The VM's Wi-Fi is simulated and may not reach the dev server (discovery also probes 10.0.2.2).

### Update (same day): recovery installs only KittenOS; release folder
- **Staged recovery.** The loader no longer restores everything at once. It installs KittenOS plus the boot package (best effort: a
  failure there never stops the restore) and restarts. KittenOS then asks for the account (setup wizard part 2) and installs the kernel
  and the modules in the background (stage 2). `restore_components(include_kernel)`: 0 by default and unattended, 1 only for the
  text flow's F key. The reasons from the bootloader no longer choose the scope.
- **Why a release folder.** The published OTA was stale (kernel 0.6.1, KittenOS 0.2.8, loader 0.1.0, no boot package), so a device
  that recovered from it went BACK to an old system. `tools/release/make_release.py` builds a signed, self-checking release:
  new tdeck_plus images at 1.0.0 (real-device builds out/stdfull, stdmin, stdrec: Wi-Fi, official URLs, no VM/debug flavor), the boot
  package 0.2.0, legal.txt, a recovery.manifest that also carries the other boards' entries unchanged, and the modules release
  unchanged. Output went to ~/Desktop/PURR-OS-release (JumpingJaguar1/ and JumpingJaguar2/ = the two release tags the firmware reads).
- Checked: every manifest entry matches its file (size + sha256); the device's C manifest parser reads the new manifest; each image
  verifies against the boot key. NOT checked: running the real-device images on a T-Deck (the VM runs the VM-flavor builds). Tight:
  the real KittenOS image has 3 % of its slot free, the loader 8 %.

## Recovery app, kitten user, user account control, Security tab (2026-10-27)
Built and compiled (vmrec, stdrec, vmfull); NOT run in the VM or on the device yet.
- The card-stack UI no longer starts in the recovery profile (KittenOS). `main/recovery_app.c` replaces it: W10M theme and
  Settings widgets, "Hi, welcome to recovery.", the bootloader's reasons, Restart / Reinstall (keep data) / Reinstall and erase
  everything (wipes root + saved Wi-Fi, sets FRESH, FORCE_LOADER) / Security.
- KittenOS signs in as a built-in `kitten` standard account (`purr_login_kitten`, uid 254, memory only), never nobody.
- User account control: `purr_login_elevate` (login.c). An administrator's name and password turn the session into theirs until
  the device restarts. Reinstall and erase also open without a password when the system is damaged/missing/repeatedly failing
  (`recovery_gate_open_for`) or there are no accounts; Security always needs an admin.
- Security tab: the only way to weaken the bootloader. "Weaken" = developer mode (relaxes enforce to warn), refused when the
  signed trust block says DEV_LOCKED. Default wipes root + saved Wi-Fi first; "keep my data" needs an "I understand" switch.
  "Restore full security" clears it. `secure dev on` (shell) and the Settings dev-mode switch no longer turn it on.
- Open: VM/hardware run; host test for `recovery_gate_open_for`; whether tighten (`secure relax`) should also move here.

## Hardware crash loop after a restart (2026-10-27, from the owner's serial log)
Log: `cpu_start: Multicore app`, then `Guru Meditation Error: Core 1 panic'ed (IllegalInstruction)`, PC 0x40375d9d,
backtrace 0x40375d9a = `esp_restart_noos` (the same address in all three std builds), then the bootloader and boot menu again, in a loop.
Reading: the previous run called `esp_restart()` from a task on core 1; IDF then resets core 0 first and core 1 last, and core 1 was
left spinning in the final `while (true)` of `esp_restart_noos`. The next app's `start_other_core` (cpu_start.c) only resets the app
CPU if its clock is off, so it was never restarted, and when the new image overwrote the IRAM under the loop, core 1 executed
garbage. The VM does not model this (the emulated reset is clean), which is why it never showed there.
Fixes: (1) the bootloader parks the app CPU first thing (`park_app_cpu()` in `purr_boot.c`: run-stall, reset, clock off), which is the
real fix and needs the bootloader reflashed (it does not ship in OTA slot updates); (2) the restart tasks of the loader, the first-boot
wizard and the recovery app are pinned to core 0. Both build; VM boot checked (both cores start, recovery app comes up). NOT yet
verified on the device: that needs the new bootloader flashed and a restart from the wizard.

## "Couldn't start that" on the device (2026-10-27, owner's report)
On the T-Deck, creating the account and every recovery action failed with "couldn't start that" / "Could not start that." Cause: every
background job (`oobe_job.c`) asked for a 40 KB task stack from INTERNAL RAM, one contiguous block; with Wi-Fi up that was not available
(the VM, wired, has plenty, which is why it never showed). Fix: per-job stack sizes (accounts, password check, flash erase 12 KB; Wi-Fi and
small downloads 16 KB; image installs 32 KB, measured use ~13 KB restore, ~22 KB stage 2), one retry, and if the task still cannot be
created the job fails at once with "Not enough memory to do that (N KB free)" instead of a generic message, plus a log line with free and
largest internal blocks. The wizard and the recovery app also log internal RAM at start, so the next device log shows the real numbers.
Not verified on the device. If the largest block is still too small for the 32 KB install stack, the next step is to free internal RAM in the
KittenOS profile (find who holds it) or run installs in a task with a smaller footprint.

The fixed images are release 1.0.2 (`~/Desktop/PURR-OS-release-1.0.2`; 1.0.1 had only the bootloader fix).

### Memory follow-up (2026-10-27, after the device showed "Not enough memory ... 30 KB free")
The largest free internal block on the real device was about 30 KB, just under the 32 KB install stack, so the per-job sizes were not enough.
Two changes: (1) `oobe_job_host_run()`: the main task (32 KB stack, idle while the wizard is up) runs the install-sized jobs (restore,
stage 2) itself, so they need no new internal RAM; `oobe_job_abort` long-jumps back to the host loop there instead of deleting the task;
smaller jobs still get their own 12 or 16 KB tasks. (2) `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` for the T-Deck Plus (`sdkconfig.board.tdeck_plus`):
TLS buffers in PSRAM (a session holds ~25 KB of internal RAM). PSRAM stacks were ruled out for jobs that write flash (cache is off during
a flash write). VM: the unattended recovery still completes with the restore on the main task. Release 1.0.3 holds these fixes
(`~/Desktop/PURR-OS-release-1.0.3`). Not yet confirmed on the device; the wizard logs internal RAM at start, so the next log gives the numbers.

Correction (2026-10-27): on the device the legal.txt download timed out right after a good TLS handshake ("Connection timed out before data was ready"),
and downloads had worked before the TLS buffers were moved to PSRAM. Reverted `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` (internal again); the job host already removed
the need for that memory. Release stays 1.0.3 (owner: no version bump; the earlier 1.0.3 folder is `PURR-OS-release-1.0.3-old` on the Desktop). Whether the
timeout was the PSRAM TLS buffers is a guess: not confirmed until a device run downloads legal.txt and the images.

## One way to restore: the loader, automatically (2026-10-27, owner's design)
Owner: the auto loader should be the only thing that restores; it has the RAM, nothing else can interfere with the download; KittenOS should not
download the image itself or force the interactive loader. Built:
- Reset = `PURR_CFGF_BAD_KERNEL | PURR_CFGF_BAD_KITTENOS` (new persistent purrcfg flags) + FORCE_LOADER + AUTO_REINSTALL, set by the recovery app's
  Reinstall (keeps data) and Erase (also FRESH). The bootloader refuses a slot marked bad (`marked_bad()` in `purr_boot.c`, only while the loader slot
  is present) so an interrupted reinstall can never fall back to the old system; the boot menu greys it out too. The mark is cleared per slot by
  `purr_slot_trailer_write` once the new image is in. The saved Wi-Fi is kept on erase so the automatic restore can run.
- Loader: every entry is a restore and runs automatically from the saved network or the wire, kernel included (`restore_components(1)`). With no saved
  network the wizard runs in setup mode (`PURR_OOBE_PART_LOADER_SETUP`: reasons, Wi-Fi, dev server page if allowed), saves it, sets AUTO_REINSTALL and
  restarts; the automatic restore then runs on a fresh heap. An in-place reset (kernel marked bad, not FRESH) sets no CONTINUE_INSTALL, so it goes
  straight to PURR OS with the data and modules untouched. After an erase or a first install KittenOS still carries on (account, colour, modules).
- KittenOS stage 2 only fetches the kernel if the slot is empty or marked bad (`purr_kernel_installed()`), so an older loader still works.
- Gone from the loader flow for now: the legal page (it needs the network, and the setup now has none). Put it in KittenOS part 2.
Verified in the VM: blank device, wired: loader installs the kernel and KittenOS, restarts into KittenOS stage 2. NOT run: the setup mode (needs Wi-Fi),
the BAD-flag path and the in-place reset (needs a device with accounts and a click on Reinstall), none of it on the T-Deck.
Follow-up for less duplicate code: commands.c `net_install` for the kernel is now only a fallback.

### Legal page moved to KittenOS (2026-10-28)
The loader no longer shows the legal page. It is page 2 of KittenOS's first-boot wizard (colour, legal, account, time, done). The text is
fetched by the stage 2 install job right after it gets the network (`fetch_legal` in `oobe_firstboot.c`, same `legal.txt` URL as before), so
there is no second TLS session; the page polls that job and falls back to "couldn't load, read it in Settings". Builds: stdrec, stdmin, vmrec
(stdrec ~8% free). Not run in the VM or on the device. Dead code left in the loader: `job_legal`/`op_legal`.

### Closed (2026-10-28, owner)
Owner verified the whole reset and restore flows on the device several times (that is how the bugs above were found). Plan1 is complete.
The one leftover, the bootloader reset crash-loop (core 1 not parked), moves to plan3 item 6.

## Flash map v2: LittleFS gets everything that is not a slot (owner, 2026-11-06; designed, nothing built)

**Owner's rule.** Only the kernel, the recovery loader and KittenOS keep dedicated slots, besides what the system itself requires (the bootloader, the partition table, `nvs`, `purrcfg`, `phy_init`, `bootpkg` and `netrec`: all read before or without a file system). Everything else (pictures, UI packs and themes, shells, apps, drivers, modules) lives in LittleFS; a small partition only if something cannot.

**Today (T-Deck Plus, 16 MB, `PurrOS/partitions/tdeck_plus.csv`).** `nvs` 0xB000 (16 KB), `purrcfg` 0xF000 (8 KB), `phy_init` 0x11000 (4 KB), `bootpkg` 0x12000 (56 KB), `kittenos` 0x20000 (1.5 MB), `kernel` 0x1A0000 (1.875 MB), `loader` 0x380000 (1.5 MB), `netrec` 0x500000 (4 KB), **`ui_assets` 0x501000 (5 MB)**, `root` (LittleFS) 0xA01000 (**0x5FF000, about 6 MB**).

**Proposed.** Drop `ui_assets`; `root` starts at 0x501000 and runs to the end of flash: **0xAFF000 bytes, about 10.75 MiB, nearly twice today's** (+5 MB). Nothing else moves. The raw picture partition and `netinstall uiassets` go away: the per-shell packs, themes and shells are files in LittleFS (plan7.md).

**What it costs.** (1) A partition-table change **cannot be an OTA** (the table is at 0xA000, bootloader OTA is off): a **cable flash, and a full erase of `root`** (its start moves from 0xA01000 to 0x501000), as with every layout change so far (the comment in the CSV). (2) Places that know the old layout: `purrstrap/scripts/uiassets.py` (it writes the pack for the raw partition), `main/purr_ui_assets.c` and `.h`, `main/main.c`, `main/commands.c` (`netinstall uiassets`), `tools/hw/make_hw_image.sh`, `tools/vm/mkimage_dbg.sh` (the root erase range for the blank-device scenarios), `bootloader/partitions.csv`, `partitions/tdeck.csv`, `docs/02-flash-map-and-partitions.md`, the SDK board profile and the emulator's image layout. (3) The LittleFS lookahead buffer (`look_buf`, 32 bytes = 256 blocks per pass) is small for 2,800 blocks; raise it to 64 or 128 bytes. (4) KittenOS is within 15 KB of its slot (1,557,184 of 1,572,864 bytes), so its slot is not a candidate for shrinking. The 8 MB Waveshare board keeps its own table.

### Flash map v2: approved for 1.0.29, and the contract everything is built to (owner, 2026-11-06)

The owner approved it ("everything in 0.29, all in one go, I don't mind a cable flash") together with the per-shell packs, the themes and the SDK. **Shared names, so the parts agree:**
- **New T-Deck Plus table:** `nvs` 0xB000/0x4000, `purrcfg` 0xF000/0x2000, `phy_init` 0x11000/0x1000, `bootpkg` 0x12000/0xE000, `kittenos` 0x20000/0x180000, `kernel` 0x1A0000/0x1E0000, `loader` 0x380000/0x180000, `netrec` 0x500000/0x1000, **`root` 0x501000/0xAFF000** (ends at 0x1000000). No `ui_assets`.
- **Pack file:** the format `purrstrap uiassets` writes today (12-byte header, 40-byte directory entries, raw RGB565 data, CRC32 over the directory and the data), unchanged, called `pack.bin`.
- **Where packs live (LittleFS):** `/ui/<shell>/pack.bin` is a shell's default pack (`<shell>` is `w10m` or `android`); `/ui/themes/<id>/pack.bin` and `/ui/themes/<id>/theme.conf` are installed themes (three at most); `/ui/active.conf` holds `theme=<id>` (absent or empty = none); on the SD card the same tree under `/sdcard/ui/themes/`. Lookup is layered: the active theme's pack first, then the shell's default pack.
- **Release components** (recovery manifest, signed by the system key, installed by the loader's second stage into the file above, written with `purr_fs_write_kept`): `ui_w10m` and `ui_android`. The old `uiassets` component and `netinstall uiassets` go away. The Windows pack is built by `tools/ui/w10m_pack.py` (from the upstream MIT project) and the Android one by `tools/ui/froyo_pack.py`.
- **Theme config** (`theme.conf`, `key=value` lines): `name`, `author`, `licence`, `version`, `shell` (`w10m`, `android` or `any`), `accent` (index or `#RRGGBB`, optional), `dark` (`1`/`0`, optional).
- **Not in 1.0.29:** loadable shells (plan7.md); they are the work after it.

**Revised the same day (owner: "a little more space for the kernel, the loader and KittenOS, so they can grow"): the final flash map v2.** Each of the three slots is 256 KB larger, and LittleFS starts later: `nvs` 0xB000/0x4000, `purrcfg` 0xF000/0x2000, `phy_init` 0x11000/0x1000, `bootpkg` 0x12000/0xE000, **`kittenos` 0x20000/0x1C0000 (1.75 MB)**, **`kernel` 0x1E0000/0x220000 (2.125 MB)**, **`loader` 0x400000/0x1C0000 (1.75 MB)**, `netrec` 0x5C0000/0x1000, **`root` 0x5C1000/0xA3F000 (about 10.2 MiB, to the end of flash)**. Headroom at the time (1.0.29 builds): kernel 323 KB free (it was 61 KB), KittenOS 277 KB (was 15 KB), loader 335 KB (was 73 KB). The numbers in the contract above (`root` 0x501000/0xAFF000 and the old slot sizes) are superseded by this paragraph; the slot offsets are not hard-coded in the firmware (the bootloader reads the table), only in the partition files, the image scripts, the SDK board profile and `purrstrap`'s `SLOT_BYTES`.
