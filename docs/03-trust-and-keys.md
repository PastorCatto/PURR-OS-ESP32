# 03. Trust, keys and signing

*Current as of PURR OS 1.1 (2026-11-08).*

## Short version

- Every file that holds code starts with (or, for the three boot slots, ends with) a signed header: an ECDSA P-256 signature over a SHA-256 hash, made by a key with a **role**. A key only counts for the kinds of file its role allows.
- Two public keys are compiled into the bootloader and the OS: a **boot** key (id 1) and a **developer** key (id 2). More can be added on a device through a signed trust block. Private keys never go in the repository.
- The device's policy is `off`, `warn` (default) or `enforce`. The part that can loosen it is **signed**; the part the OS can write can only make it stricter.
- Development mode relaxes `enforce` to `warn`. A signed `DEV_LOCKED` flag forbids it for good on a production device.
- This is software trust. Without eFuse Secure Boot, someone who can write the flash can replace the bootloader and the keys.

## What it is for

The trust model answers one question for the bootloader, the recovery loader and the OS: "may this file run on this device?" It must catch damage, wrong boards, wrong signers and old versions. It does not stop someone with a flash programmer ([SECURITY.md](../SECURITY.md) section 2 says so plainly). The public description of the model is SECURITY.md; this chapter ties it to the code.

## The files

| File | What it is |
|---|---|
| [SECURITY.md](../SECURITY.md) | The public security description: chain, roles, policy, threat model |
| [Keys/SPEC.md](../Keys/SPEC.md) | Roles, the key tool, vendor certificates (mostly design) |
| [PurrOS/components/coreos/include/purr_abi.h](../PurrOS/components/coreos/include/purr_abi.h) | `purr_image_header_t` (173 bytes), roles, `purrcfg`, handoff |
| [PurrOS/components/coreos/src/purr_verify.c](../PurrOS/components/coreos/src/purr_verify.c) | `purr_image_verify()`: the one verifier used by bootloader, loader and OS |
| [PurrOS/components/coreos/src/purr_keybag.c](../PurrOS/components/coreos/src/purr_keybag.c) | Key bag building, revocation, and `purr_role_may_sign()` |
| [PurrOS/components/coreos/src/purr_trailer.c](../PurrOS/components/coreos/src/purr_trailer.c) | Verifies a slot from its trailer |
| [PurrOS/components/coreos/src/purr_trust.c](../PurrOS/components/coreos/src/purr_trust.c) | The signed trust block and the effective mode |
| [PurrOS/components/coreos/keys/purr_default_keys.c](../PurrOS/components/coreos/keys/purr_default_keys.c) | The compiled-in **public** keys (ids 1 and 2) |
| [PurrOS/main/purr_trust_os.c](../PurrOS/main/purr_trust_os.c) | The OS's view of security: `purr_secure_status()` |
| [PurrOS/main/purr_slot_trailer.c](../PurrOS/main/purr_slot_trailer.c) | Writes and checks trailers when the OS installs a slot |
| [bootloader/bootloader_components/main/purr_slotverify.c](../bootloader/bootloader_components/main/purr_slotverify.c) | The bootloader's slot check |
| [purrstrap/scripts/keys.py](../purrstrap/scripts/keys.py) | `purrstrap keys`: generate, sign, verify, trust-block, inspect |
| [purrstrap/lib/purrcfg.py](../purrstrap/lib/purrcfg.py) | Python twin of the `purrcfg` layout, for building a signed block |
| `signing_keys/` | Git-ignored folder for private keys and their public halves. Described by role only (below) |

## Key roles

A key in the key bag has a role. `purr_role_may_sign()` decides which file types each role may sign:

| Role | May sign |
|---|---|
| **boot** | OS and recovery images; module subtypes kernel, CoreOS, boot package, recovery loader, bootloader; UI picture packs |
| **system** | App manager, runtimes, dev bundle, UI picture packs, drivers, apps |
| **owner** | Drivers and apps (the `superuser`-permission apps in the design) |
| **developer** | Drivers and apps |
| **vendor** | Drivers and apps (a third party's own key; certificate scheme designed, not built) |

Since 1.0.32 the compiled-in list holds five keys (boot, developer and, from the rotation, a hub key with the system role; see below), so owner and vendor roles still appear only when a signed trust block adds a key with that role. Modules in the module index are signed with the developer key at the moment ([OTA/SPEC.md](../OTA/SPEC.md) section 6.1).

What each key signs in practice: the boot key signs everything below the OS (bootloader image, boot package, KittenOS, kernel, loader, trust blocks). The developer key signs modules, drivers and apps. The roles exist so that a leaked developer key does not give anyone boot-level trust.

## How a file is checked

`purr_image_verify()` runs these checks in order and stops at the first failure:

1. Magic and header version.
2. Header size inside the image; image type is one of OS, recovery, module, app.
3. Chip id matches the device (an app may say "any chip").
4. Version strings parse; the bootloader is new enough (`min_boot_version`).
5. Optional: not below the version floor. The caller switches this check on; the bootloader always does ([plan3.md](../plan3.md) item 3).
6. Payload offset and size inside the image.
7. The key id exists in the bag, is not revoked, and its role may sign this type and subtype.
8. SHA-256 of the payload equals the header's hash (hardware SHA in the bootloader).
9. ECDSA P-256 signature over the header bytes before the signature field.

The result codes are `PURR_V_*` in [purr_verify.h](../PurrOS/components/coreos/include/purr_verify.h). SECURITY.md lists the role check after the signature; the code checks it before the hash. The code is the authority.

### The three boot slots

`kernel`, `kittenos` and `loader` are real ESP-IDF images, so the signed header sits in the last 4096 bytes of the partition (the trailer; see [chapter 02](02-flash-map-and-partitions.md)). The partition, not the header, decides what it may hold: kernel is a kernel-subtype module, kittenos is a recovery image, loader is a loader-subtype module, all boot-role. A slot flashed straight from a build has no trailer. It boots in `off` and `warn`, and is refused in `enforce`.

## The policy

`secure_mode` in `purrcfg` decides what happens to a slot that fails.

| Mode | A failed slot |
|---|---|
| `off` | Boots, not checked |
| `warn` (default) | Boots, a warning goes into the handoff |
| `enforce` | Refused; the ladder moves to the next slot |

An unknown value is treated as `warn`, never `off`.

### The signed trust block

`purrcfg` is only protected by a CRC, which catches accidents but not an attacker. So the trust fields are split:

| Part | Who can write it | Holds |
|---|---|---|
| **Signed trust block** | Only a holder of a boot-role private key | `secure_mode`, `trust_flags` (`DEV_LOCKED`), `revoked_keys`, override `keys[]` |
| **`tighten`** | The OS, for an administrator | A floor on the mode: raise only |
| **`DEV_MODE` flag** | The OS or a baked-in config | Relaxes `enforce` to `warn` |

The signature covers a fixed byte string beginning "PURRTRUST" ([purr_trust.c](../PurrOS/components/coreos/src/purr_trust.c)), and it is checked against the **compiled-in** boot keys only, never against the overridden bag. A key cannot vouch for itself. The rule, computed by the bootloader once per boot:

```
base = signed mode if the signature verifies, otherwise warn
mode = the stricter of base and tighten
if DEV_MODE is on and not (signature ok and DEV_LOCKED): mode = warn if it was enforce
```

Consequences, all host-tested in `test_trust` and checked on a real device on 2026-10-25 ([plan3.md](../plan3.md) item 4): editing `purrcfg` cannot turn checking off; a block signed with the wrong key, or edited afterwards, is ignored; override keys and revocations count only inside a valid signed block.

### Development mode

Development mode is off by default, never goes below `warn`, and shows on the boot menu, in the handoff and in the secure status. The one-shot flags `IGNORE_ONCE` and `SECURE_OFF_ONCE` are honoured only while it is on. `secure dev on` is refused at the keyboard; it can only be switched on from the Security tab in recovery, which asks an administrator and wipes data by default unless the user says keep it ([plan1.md](../plan1.md) "Recovery app", [plan3.md](../plan3.md)). A device whose signed block has `DEV_LOCKED` never gets it. **A locked device is never weakened on the device** (owner decision, recorded in [plan3.md](../plan3.md)).

After an install the device starts with developer mode off on purpose, and the owner asked that it not be restored automatically (2026-11-05, [plan3.md](../plan3.md)).

### The secure status

`purr_secure_status()` returns one honest word for the system and (later) for apps: **GENUINE** (enforced this boot, slot verified, no development mode), **DEVELOPMENT**, or **UNVERIFIED**. It is software attestation. The app-facing catcall and the Settings switch are not built ([plan3.md](../plan3.md) item 4d).

## How signing is done

The tool is `purrstrap keys` ([chapter 09](09-purrstrap.md)). The password for a private key is read from the environment variable `PURRSTRAP_KEY_PASSWORD` or asked for on the terminal. It is never accepted as a command-line argument, never saved in a state file, and never written in any file, script, commit message or plan. Give it in the environment of the one command that signs:

1. Build the unsigned image. For a kernel: `python3 purrstrap/purrstrap.py coreos package-component --component kernel --file <purros.bin> --version <x.y.z> --board tdeck_plus --out K.cat`.
2. Sign it: `PURRSTRAP_KEY_PASSWORD=<given for this command only> python3 purrstrap/purrstrap.py keys sign --key <boot-role private key> --image K.cat --key-id 1`.
3. Check it with the public half: `python3 purrstrap/purrstrap.py keys verify --key <boot-role public key> --image K.cat`.
4. For a slot you will flash with esptool, cut the whole-partition image: `python3 purrstrap/purrstrap.py coreos slot-image --file K.cat --board tdeck_plus --slot kernel --out kernel_slot.bin`.

The `keys` subscript needs the `cryptography` package. The system Python on a plain Mac does not have it; the ESP-IDF Python environment does ([chapter 09](09-purrstrap.md)).

To set a device's policy before flashing, build a signed purrcfg: `keys trust-block --key <boot-role private key> --mode enforce --out purrcfg.bin`, and flash it to the `purrcfg` partition, or hand the file to the device's `trust apply <file>` command (admin).

## The key rotation (2026-11, in progress)

The first keys were made for development. Five keys are now compiled into the bootloader and the firmware (`purr_default_keys.c`, one list for both):

| Id | Role | What it is |
|---|---|---|
| 1 | boot | the old boot key: **deprecated since 1.1** (still accepted, not revoked; nothing is signed with it from 1.1) |
| 2 | developer | the old developer key (still accepted until the last step) |
| 3 | boot | the new boot key: signs kernel, KittenOS, loader, boot package, trust blocks |
| 4 | developer | the new developer key: signs apps, drivers and modules |
| 5 | system | the hub key: signs the hub's repository index and endorses it, so the hub shows as "official" |

A sixth key (id 6, a nightly role delivered as a signed bundle with an expiry) is designed and not generated. The private keys stay on the owner's computer, in an ignored folder, never in the repository. A passphrase is given to each signing command in its environment and is never written into a file, a script, a commit or a plan.

The order, and where it stands ([plan3.md](../plan3.md)):

1. Make the keys. **Done.**
2. A bootloader (0.5.0) that carries all five and accepts old and new. **Done**, flashed by cable on the owner's T-Deck Plus, because bootloader updates over the air are switched off (an erase of flash 0x0 aborts in ESP-IDF).
3. A release that carries the five keys but is still signed by the old boot key (**1.0.32**), so every device learns the new keys. **Done.** From then on a release may be signed by key 3 (1.0.34 and 1.0.35 are).
4. Sign everything else with the new keys, and sign the trust block with key 3. **Not done.**
5. Revoke ids 1 and 2 with a signed request, and ship bootloader 0.6.0 without them, again by cable. **Not done**; after it a file signed only by an old key does not boot.
6. Update the documents. **This page is part of it.**

**The rule that follows:** a device checks a release against the keys *it* has. A device older than 1.0.32, or one just reset (it reinstalls from the official release, which is old), knows only ids 1 and 2, and so does its recovery loader. For such a device the dev server needs a **bridge set** signed by key 1 (1.0.33 was published this way). The loader slot has to be new enough too: an old loader refuses a kernel signed by key 3, and the cure is to write a newer loader to its slot (`0x400000`) by cable.

## What goes wrong and how to fix it

| Symptom | Cause | Fix |
|---|---|---|
| `keys` shows as "broken: No module named 'cryptography'" in `purrstrap list` | Wrong Python | Use the ESP-IDF Python environment |
| Wrong password | Passphrase mistyped | Re-run; nothing is stored. A wrong password fails without leaking anything |
| `bad-hash` on a module after signing | Image changed after signing | Re-sign after the last edit |
| `below-version-floor` in warn mode | An older signed image than the OS has run | Install a newer version; the floor rises on healthy boots |
| `secure enforce` refuses | A slot does not verify now | Reinstall it so it carries a valid trailer, then retry |
| Trust block "ignored" | Wrong signing key, or edited after signing | Rebuild with the boot-role key |
| A downgrade or an old block is accepted | Version floors and the block have no replay protection | Known limit, below |

## What is not finished

- **Key rotation** has reached its third step (see "The key rotation" above); re-signing everything with the new keys and revoking the old ones are not done. The earlier plan said: The steps (new ids in unused slots, a new bootloader carrying the new keys, revoke the old ones) are in [plan3.md](../plan3.md). [Keys/SPEC.md](../Keys/SPEC.md) still lists rotation as open. The owner decided that Showtime goes before the rotation.
- **No replay protection** for signed blocks, and **version floors are not signed**. Someone with flash access can restore an old block or lower a floor ([SECURITY.md](../SECURITY.md) section 6.1).
- **Vendor certificates, expiry and `latest_time`** are designed, not built. **eFuse Secure Boot** and **flash encryption** are designed or not planned; both are off.
- **The developer key becoming developer-mode only, and driver signing being restricted** (owner decision, [plan3.md](../plan3.md)) is not in `purr_role_may_sign()` yet: today a developer key may sign drivers.
- **Release channels** (a beta channel skipping the version floor, with a limit of five releases back) are designed and not built ([chapter 08](08-updates-and-release-channels.md), [plan3.md](../plan3.md)).
- **Fuzzing** covers six parsers (5.4 million executions, nothing found), not the app manifest, the user store or the CLI ([chapter 10](10-host-tests.md)).
- **Not verified on a device:** the `secure` and `trust apply` commands typed by hand, and the whole `enforce` refusal path for the `loader` slot.

Plan references: [plan3.md](../plan3.md) (hardening, key rotation), [plan1.md](../plan1.md) (the trailer).

**Deprecated, not revoked (owner, 2026-11-09).** Keys 1 and 2 stay in the list. Release 1.1 is signed again with keys 3 and 4 (`tools/release/resign_release.py`: the bootloader, boot package and picture packs with key 3, the modules and drivers with key 4; the 1.1 kernel, KittenOS and loader were already on key 3). Revoking 1 and 2 on devices is a later decision, and by then nothing a device fetches depends on them.
