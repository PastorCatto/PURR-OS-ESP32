# PURR OS bootloader spec (draft 0.1)

Single source of truth for the bootloader, the image container and the boot
flow. If code and this document disagree, fix one of them in the same commit.

## 1. Purpose and threat model

The bootloader loads exactly two kinds of image: the **kernel** (PURR OS) and
the **recovery** image (**KittenOS**). It verifies each against a public key it holds, applies a
small policy from flags in flash, and hands off.

**What this is:** an authenticity and integrity check with a recovery policy.
It catches corrupted images, wrong-vendor builds, bad updates and accidental
mistakes, and it lets the OS warn the user.

**What this is not:** protection against someone who can write flash. Without
eFuses nothing verifies the bootloader itself, so a physical attacker can
replace it, change the flags or change the keys. eFuse support (section 8) is
optional and adds a real root of trust underneath this layer.

**KittenOS** is the recovery OS. It is a barebones PURR OS build with recovery
functions added on top (updating, key management, diagnostics), built from the
same tree as PURR OS. It uses the same container format, the same driver
interface and the same signing as the kernel image, so recovery is just another
image to the bootloader. Handling a key or verification failure is KittenOS's
job, not the bootloader's.

Non-goals for v1: UI, networking, filesystem access, loadable driver modules.
The bootloader stays small and has no FreeRTOS.

## 2. Flash layout (example, 16 MB)

| Name       | Type/Subtype | Size   | Notes |
|------------|--------------|--------|-------|
| bootloader | -            | ~32 KB | Second stage. |
| purrcfg    | data/0x40    | 8 KB   | Two 4 KB copies (A/B) of the config struct. |
| otadata    | data/ota     | 8 KB   | Standard IDF slot selection. |
| nvs        | data/nvs     | 24 KB  | Used by the OS, never by the bootloader. |
| kittenos   | app/factory  | 2 MB   | KittenOS recovery image, updated separately. Sized as a barebones PURR OS plus recovery tools; revisit once a build exists. |
| kernel_0   | app/ota_0    | rest/2 | Kernel image. |
| kernel_1   | app/ota_1    | rest/2 | Kernel image. |

Sizes are placeholders per device. The bootloader binary must fit before the
partition table, so budget its size and move the table if it grows.

## 3. Image container

Every image starts with a `purr_image_header_t`, followed by the payload. For
kernel and recovery the payload is a standard ESP-IDF app image.

| Field            | Size | Meaning |
|------------------|------|---------|
| magic            | 4    | `0x50555252` ('PURR') |
| header_version   | 1    | Container version, starts at 1 |
| header_size      | 2    | Bytes, lets the header grow |
| chip_id          | 2    | `esp_chip_id_t` value; bootloader rejects a mismatch |
| image_type       | 1    | 1 = kernel (PURR OS), 2 = recovery (KittenOS), 3+ reserved for loadable modules |
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
| role     | 1 = PURR OS (kernel images), 2 = KittenOS (recovery images) |
| pubkey   | P-256 public key, 64 bytes |
| revoked  | Bit in `revoked_keys` |

- An image verifies only against a key whose role matches its `image_type`.
  A kernel image signed with a recovery-role key fails, and the reverse.
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
| efuse_arm      | `arm_stage`, per-flag `set_at_seq`, and the request (section 8) |
| crc32          | Over everything above |

Flags:

| Flag             | Persistent | Effect |
|------------------|------------|--------|
| IGNORE_ONCE      | one boot   | Skip verification failure for the next boot only. Cleared before handoff. |
| UPDATE_KEY       | one boot   | Apply the signed `key_update` block, then clear. |
| SECURE_OFF_ONCE  | one boot   | Treat `secure_mode` as off for the next boot only. |
| FORCE_RECOVERY   | one boot   | Boot recovery instead of the kernel. |

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

1. Init, load the partition table, read `purrcfg`. If both copies are invalid,
   use defaults (secure_mode = warn) and continue.
2. If `UPDATE_KEY` is set, validate and apply the key update, then clear it.
3. If `FORCE_RECOVERY` is set, jump to step 5 with recovery selected.
4. Pick the kernel slot from otadata.
5. Verify the selected image:
   chip ID, magic and version, payload SHA-256, signature against its `key_id`,
   revoked bit, and IDF's own image check.
6. Apply the policy:

| Result | off | warn | enforce |
|--------|-----|------|---------|
| Verified | boot | boot | boot |
| Failed | boot | boot, warning state set | boot recovery |
| Failed, and `IGNORE_ONCE` set | boot | boot, warning state set | boot, warning state set |

7. If recovery is the selected image and fails verification: in `off` and `warn`
   boot it with the warning; in `enforce` stop and wait. Open item: decide
   whether "wait" means a reset loop or a halt.
8. Clear one-shot flags, write `purrcfg` if it changed, write the handoff
   struct, jump.

## 7. Handoff to the OS

A small struct in RTC no-init memory (survives a soft reset, not power loss):

| Field         | Meaning |
|---------------|---------|
| magic, crc    | Validity |
| boot_state    | verified, failed_warned, forced_recovery, config_default |
| key_id_used   | Which key verified the image, or 0xFF |
| flags_used    | Which one-shot flags were consumed |
| bootloader_v  | Bootloader version |

The kernel or recovery reads this and shows any warning. The bootloader itself
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
- Preconditions at burn time: `secure_mode` = enforce, the kernel and recovery
  images both verify, and a dry-run report has been written to the log.
- One action per arming cycle. v1 defines only "program secure boot key
  digest". Other actions (disable JTAG, disable ROM download, flash encryption)
  are reserved and not implemented.
- After a burn, read the eFuse back, verify it, record the result in the
  handoff struct and reset `arm_stage` to 0.
- Develop and test on a sacrificial board first.

## 9. Open questions

- Header-first container vs. trailer, pending the IDF loader check (section 3).
- Recovery failing verification in enforce mode: reset loop or halt.
- Which role's key may sign each kind of request (key update, `secure_mode`
  change, eFuse request). The obvious rule is that only the KittenOS role can,
  so a compromised kernel-signing key cannot touch trust state.
- Whether KittenOS may replace the PURR OS image and edit `purrcfg` directly
  when secure boot is disabled, or whether that also needs a request.
