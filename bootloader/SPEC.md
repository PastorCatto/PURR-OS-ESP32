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
| bootloader | -            | ~32 KB | Second stage. |
| purrcfg    | data/0x40    | 8 KB   | Two 4 KB copies (A/B) of the config struct. |
| nvs        | data/nvs     | 24 KB  | Used by the OS, never by the bootloader. |
| bootpkg    | data/0x44    | ~64 KB | The boot package, right after the bootloader. |
| kittenos   | app/factory  | 1 MB   | KittenOS, recovery only. Not updated in normal use. |
| rescue     | app/test     | ~1 MB  | Modular boards: the recovery loader (Wi-Fi, TLS, keys, minimal CoreOS). Almost never changed. |
| kernel     | app, single slot | to be sized | Modular boards only: hardware, drivers, FreeRTOS, mounts `root`, loads CoreOS from it (`PurrOS/SPEC.md` sections 1, 6). A distinct subtype from `factory`/`ota_*` -- picked by the boot package like any other slot (section 6 step 8), never through `esp_ota_*`. |
| os         | app/ota_0    | rest   | Monolithic boards only: the packed OS image, one slot. |
| root       | data/littlefs | rest  | The root filesystem: CoreOS and everything else on modular boards (the apps filesystem, 1 MB, on monolithic ones). |

The bootloader binary must fit before the partition table, so budget its size and move the
table if it grows. There is no OTA slot switching: which target boots is recorded in
`purrcfg`.

### 2.1 Sizing

- **Bootloader budget.** On original ESP32 the bootloader sits at 0x1000 and the partition
  table at 0x8000, leaving 28,672 bytes. The basic bootloader in this repo already builds to
  27,008 bytes on esp32 (21,728 on esp32s3). Signature checking and `purrcfg` handling will
  not fit, so the partition table has to move (`CONFIG_PARTITION_TABLE_OFFSET`) and every
  partition after it shifts.
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

Open item: the IDF loader normally expects the app image at the start of a
partition. Loading it at `payload_offset` needs `esp_image_load` with a custom
position. Confirm this works before freezing the header layout.

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

## 6. Boot flow

1. Init, load the partition table, read `purrcfg`. If both copies are invalid, use defaults
   (secure_mode = warn) and continue.
2. If `UPDATE_KEY` is set, validate and apply the key update, then clear it.
3. Choose the target: KittenOS if `FORCE_RECOVERY` is set or the recorded boot target says so,
   the recovery loader if that is what was asked for, otherwise the boot package.
4. Verify the chosen image: chip ID, magic and version, payload SHA-256, signature against
   its `key_id`, revoked bit, IDF's own image check, and, in `enforce` mode, that its version is not below the
   version floor.
5. Apply the policy:

| Result | off | warn | enforce |
|--------|-----|------|---------|
| Verified | boot | boot | boot |
| Failed | boot | boot, warning state set | boot KittenOS |
| Failed, and `IGNORE_ONCE` set | boot | boot, warning state set | boot, warning state set |

6. If KittenOS is the target and is missing, damaged or fails verification: on a modular board start
   the recovery loader, which downloads a new KittenOS (`RecoveryLoader/SPEC.md`). On a board
   without one, in `off` and `warn` boot KittenOS with the warning, and in `enforce` print the
   serial prompt and wait. Open item: decide whether "wait" means a reset loop or a halt.
7. Clear one-shot flags, write `purrcfg` if it changed, write the handoff struct, jump.
8. When the boot package returns its menu choice, verify and load the chosen slot the same
   way (steps 4 and 5) -- the packed image on monolithic boards, the **kernel** partition on
   modular ones. Same mechanism both tiers; only which slot the package is allowed to
   choose differs.

The boot package verifies the kernel and CoreOS files it loads with the same rules
(`PurrOS/components/coreos/SPEC.md` section 3.5).

## 7. Handoff to the OS

A small struct in the RTC FAST memory area that ESP-IDF reserves for custom
use (`CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC`). It has a fixed address shared by
the bootloader and the app, survives a soft reset but not power loss, and needs
the same option and size in both builds. It carries its own magic and CRC:

| Field         | Meaning |
|---------------|---------|
| magic, crc    | Validity |
| boot_state    | verified, failed_warned, forced_recovery, config_default |
| key_id_used   | Which key verified the image, or 0xFF |
| flags_used    | Which one-shot flags were consumed |
| bootloader_v  | Bootloader version |

CoreOS reads this first, decides what to do with it, and records any warning
for the layers above to show. The bootloader itself
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
  the kernel, one layer up, loading CoreOS (`PurrOS/SPEC.md` section 6). Before returning its
  choice it increments `boot_fail_count`.
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
3. It calls the package with a small service table (flash read, delay, GPIO, log). The package
   offers a small set of calls back: start the display, run the menu with a timeout, read a
   key, and return the choice.

**Boot menu**

- Shown for a short, configurable time at power-on, like a PC's "press a key for setup".
- **If ignored, the system boots normally.**
- Entries are open. At least: boot normally, and boot KittenOS (recovery).
- The bootloader acts on the returned choice: it sets the boot target and continues.

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
