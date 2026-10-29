# plan3: hardening

**Source of truth for this work.** Re-read after any context compaction. Update the Status table
and the Session log as work happens. Builds on `plan1.md` (boot chain, signature trailer; now
running on the T-Deck Plus) and `SECURITY.md` (the public description of the model).

Started 2026-10-25. Nothing here is committed.

## Method

Think like the attacker, per `SECURITY.md` section 11, then fix in order of value. Every finding
below is backed by code or by the device, not by assumption. Where a fix changes policy (what
the device allows), the decision is written down here before it is built.

## Baseline (what is true today)

- The device runs the PURR bootloader, a signed boot package, and signed `kernel`, `kittenos`,
  `loader` slots with signature trailers. `secure_mode` is `warn`. The kernel slot verifies on
  boot, taking about 2.0 s. Details in `plan1.md`.
- Host tests: 18 files pass (`PurrOS/components/coreos/test/host/run.py`).

## Findings from the audit

| # | Finding | Evidence | Severity |
|---|---|---|---|
| F1 | `purrcfg` trust fields are not authenticated. The bootloader accepts `secure_mode`, the revoked-key mask, and up to N **override keys of any role** from `purrcfg` on a CRC alone. Anyone who can write the `purrcfg` partition can add a boot-role key and sign arbitrary images, or set `off`. The one-shot flags `SECURE_OFF_ONCE` and `IGNORE_ONCE` are also honored from unsigned `purrcfg`. | `purr_keybag_build()` applies `cfg->keys[]` when `valid == 1`; `purr_boot.c` honors the flags; nothing signs `purrcfg` | High for any attacker who gets a flash write; physical attackers can do this anyway (tier 3), so it matters most as defence in depth against a future bug |
| F2 | **There is no way to reach `enforce`.** No command, tool or request path writes `secure_mode`, a key slot or the revoked mask. They change only by flashing a hand-made `purrcfg`. So `enforce` is untested on a device and unusable in practice. | grep of `PurrOS/main` and `components` finds no writer; the signed-request design (`bootloader/SPEC.md` 5.1) is unbuilt | High (the whole policy is effectively `warn` forever) |
| F3 | The bootloader does not enforce version floors, so a validly signed **older** kernel/kittenos/loader boots in every mode (a rollback). The OS install paths do enforce floors (`commands.c` `find_floor`). | `purr_slotverify.c` sets `enforce_floor = 0` | Medium |
| F4 | Verifying a slot takes about 2.0 s at every boot (software SHA-256, 1 KB flash windows). The bootloader has 432 bytes free. Both are fixed by the same change (hardware SHA). | measured on the device 2026-10-25 | Medium (usability, and it blocks adding bootloader features) |
| F5 | In `enforce` with no usable slot the bootloader resets in a loop. | `purr_boot.c` final gate | Medium (a policy decision, listed below) |
| F6 | Parsers of untrusted input (container header, trailer, relocation tables, recovery manifest, `purrcfg`, `.cat`/module headers) have unit tests but no fuzzing. | `test/host` | Medium |
| F7 | Boot menu: anyone at power-on can pick KittenOS or "internet recovery". | `pkg_main.c` | Policy decision |
| F8 | The debug serial console (`debug.cat`) only answers `ping` and echoes; `serial write/read` sits behind the shell login. | `Modules/test/debug_module.c` | Low, closed |
| F9 | Wi-Fi credentials are plaintext in `netrec` and the saved-networks file; user passwords are PBKDF2-hashed. | `purr_netrec.c` | Low until flash encryption exists |
| F10 | Modules can call `fs_*` and `login_*` through the kernel table (full root-filesystem access, user management). The model is "signed code is trusted"; there is no per-module permission. | `purr_kernel_table.h` | Design limit, not a bug |
| F11 | The driver module ABI had no version check (found 2026-10-25, fixed: `bus_abi`). Kernelmods and modules already check their tables' ABI. | `plan2.md`, `Drivers/SPEC.md` 4.1 | Fixed |

## Work items, in order

1. **Fuzz the parsers (F6).** A deterministic mutation fuzzer under ASan/UBSan for
   `purr_image_verify`, `purr_trailer_verify`, the relocator, `purr_manifest_parse`,
   `purr_cfg_load`. Host-only, no device risk. Fix whatever it finds.
2. **Hardware SHA-256 backend for the bootloader (F4).** Make the hash pluggable in
   `purr_verify_env_t`; the bootloader supplies `bootloader_sha256_*`; drop the software SHA from
   the bootloader image. Target: well under 1 s for the kernel slot, and room freed.
3. **Version floors in the bootloader (F3).** Pass the per-component floor from `purrcfg` and
   enforce it in `enforce` mode.
4. **Authenticated trust block (F1) and a path to `enforce` (F2).** Design below; needs sign-off
   on the decisions listed.
5. **Hardware tests of the policy**: tampered slot in `warn`, refusal in `enforce`,
   `IGNORE_ONCE`, the failure ladder, `reboot recovery`/`loader`, the handoff read-back
   (`plan1.md` checklist).
6. **Policy decisions** F5, F7.

## Design: trust block, development mode, secure status (item 4) -- DECIDED 2026-10-25

Decisions from the owner:
1. **Tighten unsigned, loosen only with a signature.** Accepted.
2. **Development mode** exists for everything, is **off by default**, and can be turned on either
   (a) before flashing, in a config baked into the device's `purrcfg`, or (b) at runtime by a
   toggle in Settings. When it is on, the bootloader relaxes `enforce` to `warn` (a failed or
   unsigned image boots with a warning), and **apps that need a security model can see it** and
   refuse. The aim is commercial partnerships: an app (a DRM-protected service, say) must be able
   to ask "is this device genuinely in a trusted state?" and get an honest yes or no.
3. (Not answered; decided here, revisit freely.) The **boot key** signs trust blocks, no new role.

### Model

- `purrcfg` v2 splits the trust state into a **signed part** and **unsigned, tighten-only parts**.
  - Signed (covered by a 64-byte ECDSA signature, checked by the bootloader against the
    compiled-in default boot-role keys only, never the overridden bag):
    `trust_mode` (off / warn / enforce), `trust_flags` (bit 0 `DEV_LOCKED`: a production device
    refuses development mode outright), `revoked_keys`, and the override `keys[]`.
  - Unsigned, written by the OS: `tighten` (0, warn or enforce: a floor on the mode), the
    persistent flag `DEV_MODE`, the version floors (raised by the OS on healthy boots), and the
    existing one-shot flags.
- **Effective mode**, computed by the bootloader:
  `mode = max(signature_ok ? trust_mode : WARN, tighten)`, and
  `if (DEV_MODE && !(signature_ok && DEV_LOCKED)) mode = min(mode, WARN)`.
  An unsigned writer can therefore only raise the mode (D1); it can lower it only through
  development mode, which is explicit, visible, and lockable by a production signature.
- **Override keys and revocations count only when the signature is valid.** An invalid or missing
  signature means "defaults": the compiled-in keys and `WARN`, exactly what a fresh device does.
- **Development mode is never silent.** It shows on the boot menu, in the handoff, and in the
  secure status below. It never goes below `warn` (nothing lets a writer reach `off`; only a
  signed block can set `off`).
- The one-shot flags `SECURE_OFF_ONCE` / `IGNORE_ONCE` are honored only in development mode.

### Secure status for the OS and apps

- The handoff gains a `secure_state` byte: effective mode, `dev` bit, `signature_ok` bit.
- A kernel-table call `secure_status()` returns: effective mode, dev mode, whether the running
  slot verified (and with which key), whether the trust block is signed, bootloader version.
  Shell: `secure` (shows it). `secure enforce` tightens (admin). `secure dev on|off` needs an
  admin login and a typed confirmation, and is refused when the signed block says `DEV_LOCKED`.
- Apps reach it through a catcall (`Catcalls/SPEC.md`, to be added): a small answer such as
  **GENUINE** (mode `enforce`, not dev, running slot verified), **DEVELOPMENT**, or
  **UNVERIFIED**.
- **Honest limit, for any partner conversation:** this is a *software* attestation. Without the
  hardware root of trust (eFuse Secure Boot, `bootloader/SPEC.md` section 8) someone who can write
  flash can still make a device report GENUINE. A DRM partner will want that hardware layer; the
  design leaves room for a third level (**HARDWARE-ROOTED**) when it exists.

### Tooling

- `purrstrap keys trust-block` builds and signs a block (mode, flags, keys to add or revoke).
- `purrstrap` can also write an initial `purrcfg` image containing a block, for flashing at the
  `purrcfg` partition: this is the "set it prior to flash" path (including a dev-mode-on config for
  development boards).
- An OS command applies a verified block to the device (a loosening that needs a signature).

### Order of work

1. **3b (now):** drop `otadata`, table to `0xA000`, 40 KB bootloader. Needs a full reflash.
2. 4a: `purrcfg` v2 structure, the effective-mode function (shared plain C, host-tested), the
   bootloader using it, handoff `secure_state`.
3. 4b: `purrstrap keys trust-block` and the initial-`purrcfg` image tool.
4. 4c: `secure_status()`, the `secure` shell command, the Settings toggle hook, the catcall.
5. 5: hardware tests, now including `enforce` refusal and development mode.

## Status

| # | Item | State |
|---|---|---|
| 0 | Audit and this plan | done |
| 1 | Parser fuzzing | **done**: `test/host/fuzz/` (6 targets: image verify, trailer verify, module layout + relocations, manifest, purrcfg + key bag overrides, Wi-Fi list). 5.4 million executions over 3 seeds under ASan + UBSan found nothing; the harness catches a deliberately planted bounds bug at once on every seed. Not covered: the `.cat`/appmgr manifest, user store, CLI, and the ESP-IDF glue in `load_relocatable_file()` (needs the device or a mock) |
| 2 | Hardware SHA backend | **done and verified on the device**: slot verify 1954 ms -> 656 ms. Measured breakdown before: 328 ms flash reads, about 1300 ms software SHA-256 (the bootloader runs at only 80 MHz), about 250 ms the signature check. Now `purr_hash_t` is an optional hook in `purr_verify_env_t`; the bootloader plugs in IDF's `bootloader_sha256_*` (ROM/hardware SHA, whole words only, so a payload whose size is not a multiple of 4 falls back to software) and reads flash in 4 KB read-ahead windows. 21 new host checks. Bootloader is 32,544 bytes, **224 free** (the software SHA is still linked for the 109-byte header hash). Remaining time is about 250 ms signature check plus flash reads; the bootloader's 80 MHz clock is the next lever |
| 3 | Floors in the bootloader | **done and verified on the device (warn mode)**. Bootloader: each slot is checked against its `purrcfg` floor (always as `enforce_floor`, so below-floor is a failed verification: warn boots with a warning, enforce refuses). OS: `purr_mark_boot_healthy()` raises the floor for the running slot (kernel / kittenos / loader) to the version in its signature trailer, **only if that trailer verifies right then** and the version is above the current floor. Device test: booted 0.2.1 (floor raised), flashed the older validly signed 0.2.0 back: `slot @0x1a0000: below-version-floor` + `booting slot 0 although it did not verify`. Restored to 0.2.1. `enforce` refusal itself is still untested on hardware (needs item 4). No tool yet to LOWER a floor deliberately (a downgrade needs the signed trust block, item 4) |
| 3b | Make room in the bootloader | **DONE and running on the device (2026-10-25).** Bootloader limit is now 40 KB; it is 32,608 bytes, **8,352 free**. Table at 0xA000, `otadata` gone, `nvs` 0xB000, `purrcfg` 0xF000. All three OS images were rebuilt (the app reads the table at the compiled offset) and reflashed as 0.2.2 with the new bootloader and table; the first boot verified the boot package and kernel slot (647 ms) and mounted the filesystem. Floors reset (purrcfg moved and was erased). Original notes: CSVs and `sdkconfig.defaults` changed, `esp_ota_set_boot_partition` removed, three clean builds running; then full reflash. Original plan: Bootloader is 32,608 bytes, 160 free of 32,768. Plan: drop `otadata` (the PURR bootloader ignores it; only `esp_ota_set_boot_partition` in netinstall uses it, which is meaningless here) and move the partition table to `0xA000`, giving the bootloader 40 KB. New layout: `nvs` 0xB000/16K, `purrcfg` 0xF000/8K, `phy_init` 0x11000, `bootpkg` 0x12000/56K unchanged. Needs `CONFIG_PARTITION_TABLE_OFFSET=0xA000` in all three `sdkconfig.defaults`, the CSVs, dropping the `esp_ota_set_boot_partition` call, and another full reflash of bootloader + table (erase nvs/purrcfg) |
| 4 | Trust block, development mode, secure status, path to `enforce` | **built and tested on the device (2026-10-25).** 4a: `purrcfg` keeps its size/version (spare bytes became `tighten` and `trust_flags`, `reserved1` became `trust_sig`), new shared `purr_trust.c` (effective mode, 85 host checks incl. forged/wrong-key/self-vouching/dev-lock cases), bootloader evaluates it once per boot, one-shot loosening flags only in dev mode, handoff v2 with `secure_state`, boot menu shows the policy and `DEVELOPMENT MODE`, the OS reads the effective view everywhere (a view can never be stored back). 4b: `purrstrap keys trust-block` (+ `lib/purrcfg.py`, 11 Python tests, byte-for-byte cross-checked against the C evaluator with the real boot key). 4c: `secure` (status; admin: `enforce`, `relax`, `dev on|off`), `trust apply <file>`, `purr_secure_status()` (GENUINE / DEVELOPMENT / UNVERIFIED). **Device tests** (blocks flashed to `purrcfg`, kernel slot tampered by one byte): signed enforce boots clean; enforce + tampered kernel -> `REFUSED`, falls back to KittenOS; signed enforce + dev flag -> `warn (DEVELOPMENT MODE)`, tampered kernel boots with a warning; `DEV_LOCKED` + dev flag -> enforce holds; a block signed by the developer key (mode off) ignored; a signed enforce block forged to `off` with a valid checksum ignored. Device restored to unsigned/warn afterwards |
| 4d | Remaining from item 4 | the app-facing catcall for the secure status and the Settings toggle (UI/app work; the API `purr_secure_status()` and the setter logic exist); replay protection for signed blocks (needs a spare `purrcfg` field, a format change); signed or otherwise protected version floors (needs the hardware root); try `secure` / `trust apply` at the keyboard (not exercised by hand yet) |
| 5 | Hardware policy tests | **mostly done** (see item 4): enforce refusal + fallback, tamper, dev mode, production lock, wrong-key and forged blocks, rollback (item 3). Still to do by hand: the failure ladder (seven power cycles), `reboot recovery` / `reboot loader`, menu choices, `secure` / `trust apply` at the keyboard, `net_install` writing a trailer |
| 6 | Policy decisions (F5 `enforce` with nothing usable; F7 boot menu) | open |

## Session log

- 2026-10-25: audit done from the code and the live device; plan written.
- 2026-10-25: item 4 built (4a-4c) and tested on the device; docs updated (SECURITY.md 6.1/6.2, bootloader/SPEC.md 5.2).
- 2026-10-25: 3b done and flashed (layout change, 40 KB bootloader).
- 2026-10-25: item 3 done (floors); 3b identified.
- 2026-10-25: item 2 done (timing instrumentation added, measured, removed; the hook flashed to the device).
- 2026-10-25: item 1 done. Run with `python3 PurrOS/components/coreos/test/host/fuzz/fuzz.py [target] [iterations] [seed]`.

### Item 6 decided (owner, 2026-10-28)
- KittenOS stays bootable even when policy is `enforce` and nothing else is usable; it gets restricted heavily before shipping so that it is
  purely a recovery menu (recovery app, Security tab, nothing else).
- If KittenOS itself is corrupt, the escape route is, in order: (1) the dev server path (local restore), driven by a desktop tool for
  Windows, macOS and Linux that triggers a safe restore over the network, no USB cable; (2) an emergency pull of the newest image from
  the official GitHub repository, whose address is hard-coded into the bootloader (so it needs the bootloader to speak Wi-Fi/HTTPS or hand
  off to a minimal fetcher; size budget is ~6.4 KB free, so likely a fixed loader slot does the fetching and the bootloader only stores the URL
  and trust key).
- Boot menu (F7): kept; no change decided yet beyond the BAD-slot greying already done.
- Also here: the bootloader reset crash-loop (park_app_cpu) hardware check, moved from plan1.
Open design for this item: the desktop restore tool (name, protocol: reuse `purrstrap dev serve` discovery, signed images only).

### Recovery tool and KittenOS scope (owner, 2026-10-28)
- Desktop restore tool: **broken-kitten-recovery** ("broken kitty recovery"), one self-contained executable for Windows, macOS and Linux that does the
  whole safe restore. Also allowed on consumer devices: during the recovery loader's update channels, the device looks on the network for a trusted
  source (a proxy, or a machine holding signed, verified images) and may use it. Trust is by signature, never by address. The GitHub emergency pull
  (Q5) waits until this tool has been tested.
- KittenOS must not touch apps: no app manager, no scan of installed apps at boot. Its job is to wipe, format the file system, verify (security,
  signatures) and restore. Done 2026-10-28: the recovery profile no longer runs `purr_apps_setup` or `purr_catrt_setup` at boot (stdrec 5% free).
- Still to do: the stage-2 module installs must reuse the kernel's own verification (the app manager and verifier belong to the kernel/CoreOS, not
  to KittenOS). Options: (a) stage 2 only downloads and the kernel verifies on first boot; (b) KittenOS keeps a minimal verify-and-write path for
  signed modules; (c) run the kernel's verifier from inside recovery (chroot-like). Recommendation: (a) for apps, (b) for the few modules the first
  boot needs.

### Must verify (owner, 2026-10-28)
KittenOS and the recovery loader must keep working signature verification for everything they write (kernel, modules, KittenOS, loader image
checks) after the app manager is gone from KittenOS. Add an explicit test: recovery profile boots, restores from a signed source, refuses a bad
signature and a tampered image. Run in the VM (unattended recovery with a corrupted image) before the next release.

### Emergency mode + verification test, VM (2026-10-28)
- Built: `purr_recovery_emergency()` (purr_recovery_src.c). When the official manifest cannot be fetched and the bootloader allowed a dev server, the loader
  finds a dev server on the LAN, checks it has recovery files for this board, and restores from it. Needs DEV_MODE + DEV_RECOVERY; not remembered across boots.
- Test 1 (VM, blank device, official URL set to `.../JumpingJaguar3/recovery.manifest`): "Couldn't reach the install server" -> "emergency mode: using dev
  server" -> kernel 1.0.0 and kittenos 1.0.0 installed. Pass.
- Test 2 (same, one byte of the served kernel flipped): "download does not match the manifest (size or hash)"; the kernel was not written; KittenOS (untouched
  file) installed; the loader ended with "Something went wrong while installing. Power it off and on to try again." and did not restart. Pass.
- Not covered yet: a correct hash with a bad signature (needs a re-signed image with a wrong key), a tampered KittenOS image, hardware, and the case where the
  dev server is also bad. After KittenOS has no app manager the stage 2 modules still go through the old verify path.

### Verified relay for secure mode (owner idea, 2026-10-28; design, not built)
A PC/Mac client (part of broken-kitten-recovery) downloads the official images, verifies them (hash and signature against the official key, plus the
version floor), and serves them on the LAN. The device may pull from it even in secure (enforce) mode, which today is refused because the dev-server
channel needs developer mode.
- **The device never trusts the relay's word.** The relay announces "verified" for the user's benefit, but the loader checks every image itself against
  the boot key, in enforce mode strictly (`purr_image_verify` + slot trailer check, already how restore works), whatever the source. A lying relay can only
  waste time or serve an older signed image, and a below-floor image is rejected by the trailer check ("the bootloader would reject it").
  That is why this is safe to allow without developer mode: the channel is a transport, not a trust source.
- **Needed on the device:** a recovery source kind "relay" separate from "dev server": allowed in any secure mode, local addresses only (`purr_repo.h` rule),
  verification forced strict for relay sources (never "accepted because secure mode is off"), and the saved source remembers the relay's id.
- **Needed on the relay:** announce `relay` in the discovery line, serve `recovery.manifest` with hashes, its checks logged for the owner.
- **Open (owner):** attended recovery asks "Use <name> at <address>?" on the device before pulling anything; unattended automatic recovery only uses a relay that
  was paired earlier (its id saved while the owner was present), so a stranger's laptop on the network is never picked up silently.

### Relay decisions (owner, 2026-10-28)
- **Unattended recovery** may pull from a relay on its own, since the device verifies every image itself. Attended recovery always shows the relay's name and address
  and asks first.
- If verification fails in unattended mode: **dev mode** flashes anyway (existing "accepted because secure mode is off" behaviour); **not dev mode**, it fails and
  the log says why.
- The relay client also carries a **dev role**: when the system image is signed but drivers are not (onboarding new hardware), the client pushes the new drivers from
  the developer's machine, and the client does the verifying for them.
- The client is **broken-kitten-recovery**, written in Rust for Windows, macOS and Linux: `tools/broken-kitten-recovery/`.

### Relay support on the device, VM tests (2026-10-28)
Built: `relay-` discovery ids (purr_recovery_src.c: `purr_recovery_server_allowed`, `purr_recovery_dev_mode`, `purr_recovery_source_is_relay`); a relay may be used in any
secure mode, a developer's server only when the bootloader allowed one (it is ignored otherwise, and said so in the log); a relay is never remembered across boots.
Unattended recovery (auto mode) picks a relay by itself when the official server is unreachable (`s_emergency_ok`). Attended recovery (the wizard): a failed
restore shows "Use a recovery computer", which lists what was found with its name and address and uses it only after the owner picks it. Verification: an image that fails
the device's own check from a relay is refused unless developer mode is on (`unverified_ok` in recovery_loader.c); from other sources the old rule (secure mode off) stands.
Tests (VM, enforce mode, developer mode OFF, official URL set to a dead address):
- real broken-kitten-recovery serving the 1.0.3 release: found, used, kernel 1.0.3 and kittenos 1.0.3 installed. Pass.
- a developer server on the network in that state: ignored. Pass.
- a rogue "relay" serving images with the manifest hashes forged to match: "kernel verification failed: bad-hash", "verification failed: bad-hash"; nothing written. Pass.
Not run: the attended wizard screens (no touch input in the headless runner), dev mode flashing a failing image, hardware.
Found while testing: the unicast discovery probe to the VM host reaches only one program per UDP port, so the relay and `dev serve` cannot both answer in the VM.

## Quieter bootloader (owner approved the design, 2026-10-28)
Design: menu only when needed, "PURR OS!" bold italic logo (white, orange "!"), no progress bar, status chips kept, Fast startup, 20 s fall-through to the default, Space opens the menu
(a setting later; the SDK is to expose the mapping, plan5.md). Full description: `bootloader/SPEC.md` section 9.1.
- Built: config flags `FAST_START` (persistent) and `SHOW_MENU` (one-shot); package ABI v4 (splash, key check, menu reasons, restart); `purr_menu` idle timer with the default as the fall-through
  (host tests pass); boot package 0.3.0 (splash, menu with descriptions, problem screen, logo drawn from the 8x8 font); bootloader flow (`purr_bootpkg_start`, `purr_slot_failure_text`);
  Settings > Devices > Startup (Fast startup switch, Restart to the boot menu). Seen in the emulator: the splash. See below for the menu and problem screens.
- The loader now ends every install in KittenOS's second stage (also a reset that keeps the data). Needs one flash of the new loader.
- Not done: the crash during the old autorun countdown is not located (needs a serial log). The new flow has no countdown screen, but that is not proof the crash is gone.
- Not on a device yet. A bootloader change is never delivered by recovery: it needs a flash of 0x0 (the blank image has it).

## Bootloader OTA (owner, 2026-10-28)
Built: `PURR_MOD_BOOTLOADER`, the staged swap with a backup and rollback (`bootloader/SPEC.md` 9.2), the packaging (`make_release.py --bootloader`, `dev recovery --bootloader`), Settings > About lists
"bootloader X" when newer and says so on the install tap, the Rust client verifies the new component. Bootloader 0.3.0 is published with release 1.0.10; a board flashed earlier reports 0.2.0, so it is
offered the update (a first real test). Not run on a device: have the USB flasher ready for the first try.
