# Findings

Things found while reading every spec and the source to write this documentation: places where the specs and the code disagree, things that look wrong,
and rough edges worth fixing before the user-interface phase. **Nothing here was fixed.** The documentation follows the code and links here for each item.

Evidence levels used below:

- **read** found by reading the code. Not run.
- **run** reproduced by running a tool on the PC during this pass.
- **built** compiled with the real toolchain in a scratch copy of the tree.
- Nothing was exercised on hardware.

## Summary

| ID | Severity | Area | One line |
|----|----------|------|----------|
| [F-01](#f-01) | medium | boards | The CYD 2.4C is named everywhere as a supported board and cannot be built. |
| [F-02](#f-02) | medium | kernel | The driver registry in the kernel spec does not exist; hardware init is hard-wired, and the boot package keeps a second copy of the pins. |
| [F-03](#f-03) | medium | kernel | A second display is impossible without kernel surgery. |
| [F-04](#f-04) | medium | shell | The shell is a tokenizer and a command table; most of the specified shell does not exist. |
| [F-05](#f-05) | medium | CoreOS | The handoff, the decision table, the boot report and the self-check are not wired in. |
| [F-06](#f-06) | low | spec | The boot-failure ladder is built, but three specs say it is not, with different thresholds. |
| [F-07](#f-07) | low | spec | The partition names and sizes in the specs differ from the partition tables. |
| [F-08](#f-08) | medium | security | `secure_mode` cannot be changed, and four `purrcfg` fields and three flags do nothing. |
| [F-09](#f-09) | low | users | Apps are installed per user; the spec says once per device. |
| [F-10](#f-10) | **high** | security | There is no file permission system: any user can read password hashes, wipe the disk or overwrite the kernel. |
| [F-11](#f-11) | medium | users | Account and app names are not validated; naming the first account `root` locks the device. |
| [F-12](#f-12) | low | users | The login delay leaks which names exist; `next_allowed_time` is unused. |
| [F-13](#f-13) | **high** | updates | Installing `bootpkg` over the network strips the container the bootloader needs, which removes the boot menu. |
| [F-14](#f-14) | medium | updates | The recovery loader's "already good" check can never succeed for `kernel` or `kittenos`. |
| [F-15](#f-15) | medium | security | The bootloader checks one magic byte on `kernel`, `kittenos` and `loader`. |
| [F-16](#f-16) | low | naming | `.cat` means an app and a system module; the specs say `.kitt` for modules. |
| [F-17](#f-17) | low | code | Stale comments and a warning that point at things that no longer exist. |
| [F-18](#f-18) | low | build | Three unrelated version numbers for the same system. |
| [F-19](#f-19) | medium | tooling | Easy ways to produce an image the device rejects; no `system` key exists. |
| [F-20](#f-20) | medium | tooling | Half of purrstrap's specified scripts do not exist; the release flow is manual. |
| [F-21](#f-21) | low | boot package | The boot package is ESP32-S3 only and assumes pins every board may not have. |
| [F-22](#f-22) | low | forks | Several fork hazards: author URLs in two places, README, key ids. |
| [F-23](#f-23) | low | security | Wi-Fi passwords in plain text; the password is typed in the clear. |
| [F-24](#f-24) | low | limits | Silent caps that fail quietly. |
| [F-25](#f-25) | medium | updates | `netinstall` with no argument overwrites the kernel slot, with no confirmation. |
| [F-26](#f-26) | low | spec | The kernel spec still describes the old design. |
| [F-27](#f-27) | low | spec | The display API in the spec has functions the header does not. |
| [F-28](#f-28) | medium | boot | With nothing bootable the bootloader resets forever; the loader's failures halt. |
| [F-29](#f-29) | low | tooling | Builds fail when the repo path is long on Windows. |
| [F-30](#f-30) | low | boards | The bootloader project has one partition table, the T-Deck's. |
| [F-31](#f-31) | **high** | updates | A network-restored device does not end at a working PURR OS. |
| [F-32](#f-32) | low | boot | The boot menu offers Internet recovery only when nothing else boots. |
| [F-33](#f-33) | low | kernel | The filesystem wrapper and the display driver have no locking. |
| [F-34](#f-34) | medium | apps | Updating an app is not power-cut safe. |
| [F-35](#f-35) | low | tooling | `modules build` cannot detect hard-coded addresses, contrary to its spec. |
| [F-36](#f-36) | low | kernel | The kernel table has unset entries and no guard against calling them. |
| [F-37](#f-37) | **high** | data | KittenOS formats `root` on any mount failure, including a transient one. |
| [F-38](#f-38) | low | flash | `purrcfg` is erased about twice per boot. |
| [F-39](#f-39) | low | tooling | Every build prints a "To flash, run" command that is wrong for this device. |

## Before the user-interface phase

The items that will bite first, in the order a UI will meet them:

1. **One display, one console, one input source.** [F-02](#f-02), [F-03](#f-03), [F-27](#f-27), and the console in `purr_console.c` are all single-instance and use file statics.
2. **No locking** in the display driver or the filesystem wrapper. A UI task plus the shell plus Wi-Fi will overlap. [F-33](#f-33)
3. **Input is a `char`.** Touch, trackball and scrolling have no path to the system. [F-02](#f-02)
4. **No authorization.** A UI that lets any user reach a settings screen inherits [F-10](#f-10).
5. **Modules and kernelmods cannot draw or read input** (no display or key entry in either table, except `read_key`). Any UI-facing extension means growing the tables and re-signing everything.
6. **The spec for the UI** (`UI/SPEC.md`) depends on the app runtime, catcalls and permissions, none of which exist ([24](24-not-built-yet.md)).

---

<a id="f-01"></a>
## F-01 The CYD 2.4C cannot be built

- **Where:** `PurrOS/components/kernel/Kconfig`, `CMakeLists.txt`, `purr_kernel.c`; `purrstrap/scripts/coreos.py` (`BOARDS`); `bootloader/`; `bootpkg/`.
- **What:** The Kconfig choice lists `PURR_BOARD_CYD_24C`, purrstrap's `BOARDS` tables list `cyd_24c`, and the specs call the CYD the first and the monolithic board. But there is no
  `boards/cyd_24c.c`, no `PurrOS/sdkconfig.board.cyd_24c`, no `PurrOS/partitions/cyd_24c.csv`, no ILI9341 driver (`purr_kernel_init()` calls the ST7789 driver unconditionally), no
  boot package for the original ESP32 (`load_package()` returns 0 on non-S3), and `bootloader/partitions.csv` is the 16 MB T-Deck layout, which does not fit a 4 MB chip.
  `purrstrap coreos check` prints three errors for it. **[run]**
- **Why it matters:** the first-board story in `PurrOS/SPEC.md` and `kernel/SPEC.md` is not true of the repo. The monolithic tier has no code at all.
- **Fix:** either build the CYD (profile, ILI9341 driver, partitions, packed image) or remove it from the tables and mark the monolithic tier as designed.
  [14](14-adding-a-display.md) has a compiled ILI9341 driver as a start.

<a id="f-02"></a>
## F-02 No driver registry; hard-wired hardware; a second copy of the pins

- **Where:** `PurrOS/components/kernel/` against `kernel/SPEC.md` sections 2 to 7, 13, 14; `bootpkg/boards/tdeck_plus.h`.
- **What:** The spec describes a registry, a pin registry, bus ownership, catcall registries and a data-driven board profile. The code has `purr_board_t` with exactly one display and
  one keyboard and `purr_kernel_init()` that brings them up in a fixed order. `compatible` is stored and never read. Input is one character from an I2C read. The boot package
  has its own pin table and its own copy of the ST7789 init sequence, so the same hardware is described twice and must be edited twice. **[read]**
- **Why it matters:** every new device is a five-file edit ([13](13-writing-drivers.md)), and the two pin tables drift.
- **Fix:** at minimum generate the boot package header from the kernel profile. Better, build the registry before the UI needs touch and a second display.

<a id="f-03"></a>
## F-03 A second display needs kernel changes

- **Where:** `purr_board.h`, `purr_kernel.c`, `st7789.c`, `purr_display.h`, `purr_console.c`.
- **What:** one `display` in the profile, one `purr_kernel_display()`, driver state in a file-static (a second init overwrites the first), function pointers with no context argument,
  one console bound to one display. **[read]**
- **Fix:** a compiled patch for same-controller and different-controller second displays and a mirror helper is in [15](15-adding-a-second-display.md) and `documentation/examples/kernel/`
  **[built]**. The proper fix is a context pointer in the display API, a major version bump.

<a id="f-04"></a>
## F-04 The shell is much smaller than specified

- **Where:** `PurrOS/components/coreos/src/purr_cli.c` against `coreos/SPEC.md` 3.7 and `AppManager/SPEC.md` 8.1.
- **What:** built: tokenizing with quotes, line editing (backspace, Ctrl-U), a command table. Missing: pipes, redirection, `;`, `&&`, `||`, `&`, variables, scripts, comments, history, tab completion,
  `log`, `status`, `report`, `clear-failures`, `sh`, `ps`, `top`, `kill`, `run`, job control. The spec says `status`, `report`, `clear-failures` are shell commands; they are not. `help` formats names
  with `%-8s`, so names over 8 characters misalign. Return codes are discarded. `purr_cli.h` itself says "Pipes come later". **[read]**
- **Fix:** none needed for the core; mark the spec sections as designed so nobody plans around them.

<a id="f-05"></a>
## F-05 Handoff, decision table, boot report and self-check are not connected

- **Where:** `purr_decision.c`, `purr_abi.h` (`purr_handoff_t`); `PurrOS/main/main.c`; `bootloader/`.
- **What:** `purr_decide` is covered by 460 host checks and is called by nothing. The bootloader never writes the handoff and no sdkconfig reserves RTC memory for it. There is no
  `purr_coreos_boot()`, no boot report and no self-check of the running image. The documented `coreos/SPEC.md` section 4 and 5 flow is therefore not what runs; `main.c` just
  sets things up in order and drops to login. **[read]**
- **Why it matters:** `secure_mode=enforce` has no effect on what boots, and a failed self-check never triggers recovery.
- **Fix:** wire it in, or delete the dead code and the spec sections. See also [F-08](#f-08), [F-15](#f-15).

<a id="f-06"></a>
## F-06 The boot-failure ladder is built; the specs say it is not

- **Where:** `bootloader/SPEC.md` 6 (3a), `coreos/SPEC.md` 7, `KittenOS/SPEC.md` 5, `RecoveryLoader/SPEC.md` 2.1 against `purr_boot.c` and `recovery_loader.c`.
- **What:** all four say "not yet built". The ladder and the loader's auto mode are built. The specs put the **boot package** in charge of counting and use thresholds 0-2, 3-5, 6+;
  the code counts **in the bootloader** on every boot and uses 1-3, 4-6, 7+ (the count is incremented before it is read). **[read]**
- **Fix:** update the specs ([05](05-boot-process-and-flash-layout.md) has the real numbers).

<a id="f-07"></a>
## F-07 The specs and the partition tables disagree

- **Where:** `PurrOS/SPEC.md` 4.1, `bootloader/SPEC.md` 2 against the three identical `partitions` CSVs.
- **What:** the specs say `rescue` (the CSV says `loader`), `kittenos` 1 MB (CSV 1.5 MB), the loader about 1 MB (CSV 1.5 MB), `bootpkg` 56 KB (CSV 48 KB), and omit `netrec` and `otadata`. **[read]**
- **Fix:** update the specs; keep the three CSV copies identical (there is no check that they are).

<a id="f-08"></a>
## F-08 `secure_mode` is fixed; several purrcfg fields and flags are inert

- **Where:** `purr_abi.h`, `purr_cfg.c`, `bootloader/`, `PurrOS/main/commands.c`.
- **What:** nothing sets `secure_mode`, so every device runs the default `warn` unless someone writes the partition by hand ([05](05-boot-process-and-flash-layout.md)). `IGNORE_ONCE`, `UPDATE_KEY`,
  `SECURE_OFF_ONCE` are defined and never read. `boot_seq`, `latest_time`, `keys[]`, `revoked_keys`, `efuse_arm` and `boot_target` are never written. The spec's signed-request path for
  changing trust state does not exist. In practice: key rotation needs a reflash, `enforce` is unreachable without a PC, and there is no way to revoke a key. **[read]**
- **Fix:** decide whether trust state is shell-changeable (signed request) or PC-only, and delete or implement accordingly.

<a id="f-09"></a>
## F-09 Apps are per user, not per device

- **Where:** `commands.c` (`user_apps_root`, `kernel_table_app_*`) against `Users/SPEC.md` 1, 5.
- **What:** each account has its own copy under `/home/<name>/apps`; the spec says one copy under `/apps` with per-user data and grants. After `su`, the folder is still the admin's own. `userdel` leaves
  the home and apps behind. **[read]**
- **Fix:** pick a model. The per-user one wastes flash on a 11 MB filesystem and complicates updates.

<a id="f-10"></a>
## F-10 No file permissions or authorization

- **Where:** `fs_module.c`, `login_module.c`, `kernel_table_*` in `commands.c`; `Users/SPEC.md` 2.
- **What:** the only admin-gated commands are `useradd`, `userdel`, `usermod`, `su` and `passwd` for another user. Every other command is available to a **standard** user: `cat /etc/shadow`
  (the password hashes), `write`/`rm` over `/etc/passwd` and `/system`, `format --yes` (wipes everything), `reboot recovery|loader`, `netinstall` (including `kernel`, which writes a raw
  partition), `wifi forget`, `appformat`, `purrcfg`. The spec's "root-only files" are plain files. `su` adds nothing a standard command could not already do. **[read]**
- **Why it matters:** the account system gives an appearance of separation that the system does not enforce. A UI that exposes a settings screen inherits this.
- **Fix:** a minimal owner and mode on files, checked in the kernel's filesystem entry points, and a role check in each destructive kernel-table call (the code already re-checks accounts kernel-side, which
  is the right pattern).

<a id="f-11"></a>
## F-11 Names are not validated

- **Where:** `login.c` (`first_time_setup`), `purr_users.c` (`has_bad_char`), `purr_appmgr.c` (`purr_appmgr_add`).
- **What:** an account name only has to be non-empty and tab-free. The first account named `root` is created, and then login always refuses it, locking the device out until `root` is wiped
  from the PC. `first_time_setup` also ignores the return value of the add. Names are pasted into paths (`/home/<name>/apps`, `<app>.tmp`), so `/` and `..` are accepted. App names (from a signed
  header) go into folder names the same way. **[read]**
- **Fix:** one validator: letters, digits, `-`, `_`, not `root`, bounded length, used by setup, `useradd` and `appinstall`.

<a id="f-12"></a>
## F-12 Login delay details

- **Where:** `login.c`, `purr_users.h`.
- **What:** `next_allowed_time` is stored and never used; the delay is applied only when the typed name exists, so a name that exists is distinguishable by the pause. The delay sleeps before
  the password prompt, after the name is typed. **[read]**

<a id="f-13"></a>
## F-13 Network-installing the boot package removes the boot menu

- **Where:** `commands.c` (`purr_net_install_run`), `recovery_loader.c` (`fetch_and_install_module`) against `purr_bootpkg.c` (`load_package`) and `bootpkg.py`.
- **What:** both installers verify the PURR container and then write only `image + payload_offset`, the payload, into the partition. For `kernel`, `kittenos` and `loader` that is correct (plain
  ESP images). For `bootpkg` the partition must hold the **whole container**, because the bootloader reads and verifies the header from the partition (`h.magic != PURR_IMAGE_MAGIC` gives
  "no PURR image"). After `netinstall bootpkg`, or a loader full restore that rewrites it, the bootloader no longer finds a valid package and the menu is gone. The system still boots through
  the fallback ladder. **[read]** The code path was not run.
- **Fix:** write the container for `bootpkg`. Until then, do not list it in a manifest ([18](18-making-your-own-purr-os.md)).

<a id="f-14"></a>
## F-14 "Already good" never matches for `kernel` and `kittenos`

- **Where:** `recovery_loader.c` (`partition_verify_in_place`).
- **What:** it verifies a partition as a PURR container, but `kernel` and `kittenos` hold the stripped payload, so it always reports not good and every restore rewrites them. A restore of
  healthy slots still erases and rewrites them, with the usual power-cut risk. **[read]**

<a id="f-15"></a>
## F-15 Slot verification at boot is one byte

- **Where:** `purr_looks_bootable()` in `purr_bootpkg.c`; `bootloader/SPEC.md` 3.
- **What:** `kernel`, `kittenos` and `loader` are accepted if the first byte is `0xE9`. A modified image boots. The spec records the two designs for real verification. Known and written down; listed
  because it is the main gap in the "everything is signed" claim, and `secure_mode=enforce` cannot affect it ([F-08](#f-08)). **[read]**

<a id="f-16"></a>
## F-16 One extension, two things; and a spec that names another

- **Where:** `modules.py`, `coreos.py`, `commands.c` (`".cat"` match), `purr_swap.c`.
- **What:** `purrstrap modules build` writes `.cat`, and the loader accepts only `.cat`. Apps are also `.cat`. `package-component` writes `.cat`; `coreos package` and the swap files use `.kitt`.
  The specs call modules `.kitt`. The extension never tells you which kind of file you have; `keys inspect` does. **[run]**
- **Fix:** pick extensions by image type. `.cat` for apps only is the spec's intent.

<a id="f-17"></a>
## F-17 Stale comments and one warning

- `commands.c`: the module loader comment says `/modules`; the folder is `/system`. The `net_install` table comment says file-staged components are "not built yet"; they are, a few lines later.
- `purr_module_abi.h` says `net_install` writes `ota_0`; it writes `kernel`.
- `purr_menu.h` says the preferred entry is "the one otadata points at"; the bootloader ignores `otadata`.
- `main.c` `purros_installed()` also checks `ota_1`, which no longer exists.
- `Kconfig.projbuild` and `recovery_loader.c` comments say the URL options exist only in the `minimal` profile; they are unconditional.
- `recovery_loader.c` line 36: `warning: 'TAG' defined but not used` in every non-`minimal` build. **[built]**
- The specs mention shell commands `spikecoreos`, `testrelocmulti`, `testbigpage` that no longer exist.
- `Modules/SPEC.md` still says `PURR_RELOC_MAX` is 4096; it is 65536.
- `esp_ota_set_boot_partition` is still called after installing `kernel`; nothing reads `otadata`.

<a id="f-18"></a>
## F-18 Three version numbers

`version` prints a hard-coded `0.1.0` (`commands.c`). purrstrap embeds `internaldirty0.2.0-<N>` as the ESP-IDF project version. Released images carry whatever `--version` was given to `package`. Modules say `0.1.0`.
Nothing relates them. **[read]**

<a id="f-19"></a>
## F-19 Key tool footguns, and no system key

- **What:** `keys generate --key-id` is only printed. `keys sign --key-id 0` (the default) leaves the header id at 0, which the device rejects as `no-key`; there is no check that the id you give matches
  your exported key or its role. Re-running `generate` overwrites existing keys silently. The default bag has `boot` (1) and `developer` (2) only, so AppManager, runtime and devices-bundle modules (system
  role) cannot be signed. `cert` and `revoke` from the spec do not exist. **[run]**
- **Fix:** make `sign` take the id from the public key export (or refuse 0), refuse to overwrite, and decide on a `system` key.

<a id="f-20"></a>
## F-20 purrstrap is a third of its spec

Built: `coreos`, `bootpkg`, `keys`, `modules`. Not built: `bootloader`, `flash`, `image`, `repo`, `apps`. The bootloader, the standalone kernel, flashing, the recovery manifest, the app index and `purrcfg`
changes are all manual ([22](22-purrstrap-reference.md)). The trap in [04](04-building-and-flashing.md) (flashing the wrong bootloader) exists because purrstrap has no `flash` that knows which one is real. **[run]**

<a id="f-21"></a>
## F-21 The boot package's assumptions

`load_package()` is compiled out except on the ESP32-S3. `hw_init()` drives `PIN_POWER`, two idle pins, a backlight pin and the keyboard pins unconditionally, so a board without one of them
has to invent a pin or edit the code (`pin_hi(-1)` is undefined). The menu coordinates assume a 320x240 screen. `BOARDS` in `bootpkg.py` holds one board. **[read]** See [16](16-adding-a-board.md).

<a id="f-22"></a>
## F-22 Fork hazards

The release URLs (`github.com/PastorCatto/...`, tags `JumpingJaguar1`/`2`) are in two files each ([04](04-building-and-flashing.md)). The root `README.md` is the reset notice, with no build instructions.
Key ids (1 boot, 2 developer) are conventions repeated across tools and docs, not constants. `signing_keys/old/` holds the previous private keys. The licence is GPL v3.

<a id="f-23"></a>
## F-23 Plain-text Wi-Fi passwords

`/etc/wifi` and the `netrec` partition store passwords in clear (documented as deferred until flash encryption); both are readable by any logged-in user ([F-10](#f-10)); `wifi connect` echoes the password on screen
and keeps it in the shell's screen buffer. `wifi forget` leaves the `netrec` copy. **[read]**

<a id="f-24"></a>
## F-24 Caps that fail quietly

8 modules and 8 kernelmods; each directory sweep keeps only its **first 8 entries including subfolders** (so 7 files once `/system/.rejected` exists); the manifest parser keeps 16 entries and drops bad stanzas
without a message; 16 accounts; 8 saved networks (a full list still connects but does not save); a 64-column, 40-row console; packages over 512 KB. Several fail with no message beyond a serial log line
([21](21-reference-formats-abis-limits.md#limits)). **[read]**

<a id="f-25"></a>
## F-25 `netinstall` with no argument writes the kernel

`netinstall` defaults to `kernel`, erases the whole partition and writes it, with no confirmation, available to every user ([F-10](#f-10)). **[read]**

<a id="f-26"></a>
## F-26 The kernel spec describes the old layout

`kernel/SPEC.md` says the kernel is a file the boot package loads into PSRAM, and that it "talks to the platform only through the host API table, never calls ESP-IDF". The code is a flash partition
that links ESP-IDF directly. `PurrOS/SPEC.md` (draft 0.6) has the current design. **[read]**

<a id="f-27"></a>
## F-27 The display API in the spec has more than the header

`kernel/SPEC.md` 8 lists `set_rotation`, `blit_async` and `wait_idle` for version 2.0, and says the kernel serialises `blit` calls with a bus lock. `purr_display.h` has none of the three and there is no bus lock
([F-33](#f-33)). **[read]**

<a id="f-28"></a>
## F-28 Dead ends

The bootloader logs `no bootable app found` and resets, forever, when none of the three slots looks bootable, with nothing on screen (the open question in `bootloader/SPEC.md` 10). The recovery loader's
failures `halt` with a message and need a power cycle. KittenOS cannot ask for anything when it is the last resort. **[read]**

<a id="f-29"></a>
## F-29 Long paths break the build on Windows

ESP-IDF fails with `fatal error: opening dependency file ...: No such file or directory` when the build directory path is too long. Found by building in a deep scratch folder. Keep the repo near a drive root. **[built]**

<a id="f-30"></a>
## F-30 One bootloader partition table

`bootloader/partitions.csv` is the T-Deck's 16 MB layout and every board in `bootloader/` uses it. A board with 8 MB or 4 MB of flash needs its own CSV selected in its `sdkconfig.board.<board>`
([16](16-adding-a-board.md)). **[read]**

<a id="f-31"></a>
## F-31 A network-restored device does not end at PURR OS

- **Where:** `commands.c` (`s_net_install_components`, `purr_continue_install`), `PurrOS/SPEC.md` 11, the published manifest.
- **What:** After the retarget away from `ota_0`, `netinstall` has no component that installs the full PURR OS image (`coreos package` for `full` makes an `IMG_OS` that nothing consumes). Stage 2 installs
  `kernel` and `modules`. The `kernel` entry in the author's manifest is the standalone `Kernel/` binary, which has no shell and idles. File-staged components (`coreos`) are swapped but nothing loads them.
  So a device restored over the network lands in KittenOS (healthy), with an idle kernel in `kernel` that the bootloader boots first. **[read]**
- **Fix:** ship the full build as the `kernel` component until CoreOS is loaded from a file ([04](04-building-and-flashing.md), [18](18-making-your-own-purr-os.md)), or make the standalone kernel load a real CoreOS.

<a id="f-32"></a>
## F-32 Internet recovery is not in the normal menu

The menu adds the `Internet recovery` entry only when nothing else is bootable (`purr_menu_init`); the spec lists it, and an "update KittenOS" entry, as menu items. Normal access is `reboot loader`. **[read]**

<a id="f-33"></a>
## F-33 No locking

`purr_fs_t` has one shared file buffer and no mutex; `st7789.c` shares one DMA buffer and SPI device with no lock; `purr_console` keeps static state. Today the overlap is rare (the shell task draws and writes
files; the Wi-Fi task writes `/etc/wifi` only on a connect made from the shell). A UI adds concurrency. **[read]**

<a id="f-34"></a>
## F-34 An app update can lose the app

`purr_appmgr_add` for an existing app deletes `final_dir/package.cat` and `final_dir`, then renames `<name>.tmp` into place. A power cut between the two leaves only `<name>.tmp`, which `purr_appmgr_recover`
deletes at the next boot. The spec promises one complete version always survives. First installs are safe. **[read]**

<a id="f-35"></a>
## F-35 `modules build` misses hard-coded addresses

The relocation finder links twice and compares. A hard-coded constant (`0x3FC88000`) is identical in both links, so it is not flagged, and the build reports `0 data + 0 code relocation(s)` and succeeds. Only words
that differ for another reason are rejected. `Modules/SPEC.md` 4 says anything non-relocatable is an error. **[run]**

<a id="f-36"></a>
## F-36 Unset kernel-table entries

`purr_kernel_table_t` has no size or capability field. The standalone `Kernel/` binary leaves most entries `NULL`; a kernelmod that calls one crashes. The ABI version is the only guard, and it does not say
which entries a given kernel provides. **[read]**

<a id="f-37"></a>
## F-37 KittenOS formats `root` on any mount failure

`purr_fs_setup()` (recovery profile) formats the filesystem whenever `purr_fs_mount` returns anything but 0, including a transient I/O error, an out-of-memory, or a partly corrupt filesystem that LittleFS could
still recover from. The spec says "blank or damaged". Accounts, apps, modules and saved Wi-Fi are lost with no prompt. **[read]**
- **Fix:** format only on `LFS_ERR_CORRUPT`, and only after retrying the mount once.

<a id="f-38"></a>
## F-38 `purrcfg` wear

The bootloader erases and writes a `purrcfg` sector on **every boot** (the failure counter), and the OS erases and writes another when it resets it, so about two sector erases per boot, alternating between the
two sectors. At a typical 100,000-cycle endurance that is on the order of tens of thousands of boots. Fine for a dev kit, worth knowing for a product. **[read]**

<a id="f-39"></a>
## F-39 The flash commands the builds print are wrong here

`idf.py` ends every build with a ready-to-paste `esptool write_flash` line. For `bootloader/` it writes the **placeholder stub app** at `0x20000` over KittenOS. For `Kernel/` it writes `purr_kernel.bin` at `0x20000`, also the
KittenOS slot, plus a bootloader and a partition table from the wrong project. For `PurrOS/` it writes the stock bootloader at `0x0`. All three are correct for a normal single-project ESP-IDF layout and wrong for this one
([04](04-building-and-flashing.md)). **[built]**
- **Fix:** a `purrstrap flash` action that knows the real address of each component (it is also the missing item in [F-20](#f-20)).
