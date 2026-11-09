# PURR OS security

How PURR OS decides what to trust, what it protects against, what it does not, and what you
need to do if you build on it. This is a guide you can read front to back or look things up in.
The detailed, normative specs are linked from each section; if this file and a spec disagree,
the spec wins and this file is a bug.

Status as of 2026-10-25. Each feature below is marked **built**, **partly built** or **designed,
not built**, so you can tell a promise from a fact.

- [1. The idea in one page](#1-the-idea-in-one-page)
- [2. What this protects against, and what it does not](#2-what-this-protects-against-and-what-it-does-not)
- [3. The chain of trust](#3-the-chain-of-trust)
- [4. Signed containers](#4-signed-containers)
- [5. Keys and roles](#5-keys-and-roles)
- [6. The policy: `secure_mode`](#6-the-policy-secure_mode)
- [7. The boot flow and the failure ladder](#7-the-boot-flow-and-the-failure-ladder)
- [8. The handoff](#8-the-handoff)
- [9. Updates and recovery](#9-updates-and-recovery)
- [10. Hardware protection (eFuses): optional, dangerous, off by default](#10-hardware-protection-efuses-optional-dangerous-off-by-default)
- [11. Threat model, from the attacker's side](#11-threat-model-from-the-attackers-side)
- [12. Status: what is real today](#12-status-what-is-real-today)
- [13. For app developers (SDK)](#13-for-app-developers-sdk)
- [14. For people building their own flavor of PURR OS](#14-for-people-building-their-own-flavor-of-purr-os)
- [15. Hardening checklist](#15-hardening-checklist)
- [16. Reporting a problem](#16-reporting-a-problem)
- [17. Where the details live](#17-where-the-details-live)

---

## 1. The idea in one page

PURR OS borrows the shape of a PC's verified boot and shrinks it to a microcontroller.

- Every piece of code that the device runs is a **signed container**: a small header that says
  what the file is, who signed it, and a hash of its contents, followed by the contents.
- Each layer **checks the next layer** before running it. The bootloader checks the boot
  package. The kernel and CoreOS check the modules, drivers and apps they load.
- The device holds only **public keys**. Private keys stay on the build machine of whoever
  signs. A device can check a signature but cannot make one.
- A small **policy flag**, `secure_mode`, decides what happens when a check fails: carry on
  quietly (`off`), carry on and warn (`warn`), or refuse and fall back to recovery (`enforce`).
- If the normal system will not start, the device falls back through **KittenOS** (an
  on-device recovery system) and then the **recovery loader** (downloads a fresh KittenOS over
  Wi-Fi), so a bad update or a corrupted slot should not brick it.

This is a **software** verified-boot design. It is not a hardware root of trust, and it is
honest about that (section 2). An optional hardware layer using the chip's eFuses is designed
but gated and unbuilt (section 10).

---

## 2. What this protects against, and what it does not

**It is designed to catch:**

- corrupted images and failed or interrupted updates
- an image built for the wrong board or chip
- an image signed by the wrong party, or by a key whose role does not allow that kind of file
- a downgrade to an older, known-bad version (in `enforce` mode, once version floors are in use)
- a bad or malicious download, since downloads are verified in RAM before anything is erased
- a repeatedly failing boot, by escalating to recovery automatically

**It does not protect against someone who can write the device's flash.** Without eFuses,
nothing verifies the bootloader itself. A person with a flash programmer or a way to write
flash can replace the bootloader, change `purrcfg` flags, or swap the public keys, and the
device will trust the result. This is a property of the design, not an oversight, and the
design says so in its own threat model (`bootloader/SPEC.md` section 1).

**It also does not protect against:**

- a signed app misbehaving. The ESP32-S3 has no memory protection between tasks, so a signed
  app that is buggy or hostile can corrupt other apps or the OS. Safety comes from signing, the
  catcall-only rule enforced by the toolchain, and watchdogs, not from hardware isolation
  (`PurrOS/SPEC.md` section 5).
- reading secrets from flash. Wi-Fi credentials in the `netrec` partition are plain data, and
  real protection needs flash encryption, which is a later, hardware-level step.
- fault injection, glitching and side channels. These are out of scope.

---

## 3. The chain of trust

```
  ROM bootloader        in the chip; not ours; trusts nothing
      |
  PURR bootloader       reads purrcfg, picks a slot, checks the boot package, writes the handoff
      |
  boot package          the boot menu (signed; verified by the bootloader)
      |
  kernel                its own flash partition; brings up hardware and the filesystem
      |
  CoreOS                a signed file in the root filesystem; verifies what it loads
      |
  modules, drivers, AppManager, runtimes     signed files, each verified before use
      |
  apps (.cat)           signed; use catcalls only
```

Recovery sits beside this, not underneath it:

```
  PURR bootloader  -->  KittenOS (recovery OS, has its own copy of CoreOS and never
                                   loads the kernel or CoreOS file it might be repairing)
                  -->  recovery loader (Wi-Fi + TLS + keys; downloads a new KittenOS)
```

**Who checks whom, and how much of this is real today** (details in section 12):

| Link | Checked by | Built? |
|---|---|---|
| bootloader -> boot package | bootloader, full signature check | built |
| bootloader -> kernel, KittenOS, recovery loader | bootloader, signature trailer in the slot's last sector (section 4.1) | built; **verified on a real device for the kernel slot (2026-10-25)**. In `warn` mode (the default) a slot that fails still boots, with a warning; `enforce` refuses it |
| kernel -> CoreOS and modules | kernel / CoreOS verifier | built for modules and kernelmods; the full CoreOS-as-a-file split is in progress |
| CoreOS -> apps | AppManager / runtime | container verified; permission enforcement not built |

The bootloader and the shared verifier are small and plain C so they can be read and tested
on a host machine (`PurrOS/components/coreos/test/host`).

---

## 4. Signed containers

Every file that holds code starts with a `purr_image_header_t`
(`PurrOS/components/coreos/include/purr_abi.h`, full table in `bootloader/SPEC.md` section 3).

| Field | What it is for |
|---|---|
| `magic`, `header_version`, `header_size` | recognise and version the container |
| `chip_id` | the chip this was built for; a mismatch is rejected (code is chip-specific) |
| `image_type`, subtype | what it is: OS, recovery, system module (`.kitt`), app (`.cat`) and, for modules, kernel, CoreOS, driver, boot package, and so on |
| `key_id` | which key in the key bag signed it |
| `name`, `version`, `min_boot_version` | human name, semantic version, lowest bootloader allowed to load it |
| `payload_offset`, `payload_size`, `payload_sha256` | where the contents are, and their SHA-256 |
| `signature` | ECDSA P-256 over SHA-256 of every header byte before this field |

Verification checks, in order: the magic and version, the chip, the layout, the payload hash,
and the signature against the key named by `key_id`, then that key's **role** allows this
**type** of file, and that the key is not revoked. The crypto is ECDSA P-256 with SHA-256
(micro-ecc in the bootloader, mbedTLS in the OS). ESP-IDF's own image checksum and SHA-256
stay on in every mode as an additional integrity check.

---

### 4.1 Signatures on raw app slots: the trailer

`kernel`, `kittenos` and `loader` are ordinary ESP-IDF app images, and the chip's stock loader
needs the ESP magic byte at the very start of the partition, so the PURR header cannot go first.
The signature goes at the **end** of the partition instead:

```
partition offset 0                       the ESP app image, untouched
...                                      erased flash (0xFF)
last 4096 bytes of the partition         a copy of the signed container header, then 0xFF
```

- The trailer is a verbatim copy of the signed header the release file already carries, so
  signing works exactly as for every other container.
- The bootloader checks the image against it with the same verifier CoreOS uses: chip, key role
  against what the *partition* may hold (a signed loader cannot be passed off as a kernel),
  revocation, the SHA-256 of the whole image, and the signature.
- A slot flashed straight from a build has no trailer. It boots in `off` and `warn` (with a
  warning in `warn`) and is refused in `enforce`. `net_install`, the recovery loader and
  `purrstrap coreos slot-image` all write the trailer.
- The cost is one hash over the whole image at boot (measured at about 0.65 seconds for a 1.4 MB kernel on a T-Deck Plus, using the chip's hardware SHA), and the
  last 4 KB of each of these partitions is no longer available to the image.

## 5. Keys and roles

The device keeps a **key bag**: a small table of public keys, each with a role.
(`bootloader/SPEC.md` section 4, `Keys/SPEC.md`.)

| Role | Signs |
|---|---|
| **boot** | the bootloader, boot package, KittenOS, kernel, CoreOS, recovery loader |
| **system** | the drivers bundle, AppManager, the runtimes, official system apps |
| **owner** | apps that hold the `superuser` permission |
| **developer** | third-party apps and user code drivers |
| **vendor** | modules and apps from a third party, signed with that vendor's own key (certificate-based, designed) |

Rules that matter:

- **A key only verifies files its role allows.** An app signed with a developer key cannot pass
  as a boot-role file. A leaked developer key does not give anyone boot-level trust.
- **Default keys are compiled in** (`PurrOS/components/coreos/keys/purr_default_keys.c`, public
  keys only, tracked in git). The bag stored in `purrcfg` can override them slot by slot.
- **Private keys never leave the signer and are never committed.** In this repository they live
  in the gitignored `signing_keys/`. Whoever holds the private keys decides what the device
  trusts, so protect them like the device's identity.
- **Key changes are requests.** Replacing a key is a signed request when the policy is `warn`
  or `enforce`. A bare flag cannot change a key.
- **Revocation.** Keys can be revoked with a bit in `revoked_keys`; certificate serials are
  designed to be revocable too.
- **Expiry** of vendor certificates is checked against the latest time the device has seen
  (`latest_time`), which only moves forward and is raised only by verified sources: the RTC,
  the `Date` header of an HTTPS response, or the build time of a verified image. Setting the
  clock backwards cannot revive an expired certificate. (Designed; see `Keys/SPEC.md` section 5.)

**Developer-preview builds** trust a published test key whose private half is public on
purpose. The boot report shows when a test key is trusted so a preview device is never mistaken
for a protected one. **Official releases carry no test key.**

The key tool is `purrstrap keys` (`purrstrap/SPEC.md`, `Keys/SPEC.md` section 3). Private keys are
password-protected on disk; the password is asked for at signing time and is never accepted as a
command-line argument.

---

## 6. The policy: `secure_mode`

`secure_mode` lives in `purrcfg` (a small raw partition holding two 4 KB copies; the newer copy
with a valid CRC wins, so a power cut during a write is survivable).

| Verification result | `off` | `warn` (default) | `enforce` |
|---|---|---|---|
| verified | boot | boot | boot |
| failed | boot | boot, warning recorded | fall back to KittenOS |
| failed, `IGNORE_ONCE` set | boot | boot, warning recorded | boot, warning recorded |

Notes:

- The boot package is **rejected** on a failed check whenever the mode is `warn` or `enforce`,
  and accepted unverified only in `off`.
- **Requests that change trust state** (key updates, changing `secure_mode`, eFuse requests)
  are accepted unsigned only while the mode is `off`; once it is `warn` or `enforce` they must
  be signed by a valid key. Turning verification on unsigned is allowed only if the key bag
  already holds a valid key for each role, so you cannot lock yourself out.
- **One-shot flags** (`FORCE_RECOVERY`, `FORCE_LOADER`, `IGNORE_ONCE`, `SECURE_OFF_ONCE`,
  `UPDATE_KEY`) apply to a single boot and are cleared before the bootloader acts on them, so a
  crash cannot trap the device in a loop.
- `purrcfg` is integrity-checked with a **CRC**, which catches accidents but is not an
  authenticator; the trust-relevant fields are separately signed (section 6.1).

### 6.1 The trust block, tightening, and development mode (built 2026-10-25)

The table above says what each mode does with a failed image. What decides the mode is stored in
`purrcfg`, and `purrcfg` is only checksummed, so it is split into a part that is **signed** and
parts anyone with a write can touch but that can only make the device **stricter**.

| Part | Who can change it | What it holds |
|---|---|---|
| **Signed trust block** | Only the owner's boot key (an ECDSA signature the bootloader checks against the keys compiled into it) | the mode (`off`/`warn`/`enforce`), the production lock `DEV_LOCKED`, revoked keys, override keys |
| **`tighten`** | The OS, for an admin | a floor on the mode: raises it, never lowers anything |
| **Development mode flag** | The OS, for an admin who confirms at the keyboard (or a config baked in before flashing) | relaxes `enforce` to `warn`, nothing further |

The mode the bootloader enforces is:

```
base = the signed mode if the signature verifies, otherwise warn
mode = the stricter of base and tighten
if development mode is on and the signed block does not lock it: mode = warn (if it was enforce)
```

What follows from that, and was checked on a real device (section 12):

- **A device with no trust block behaves as `warn`**, exactly like a fresh device. You can turn
  `enforce` on yourself with `secure enforce`; it refuses unless every boot slot verifies first,
  so it cannot leave the device with nothing it may boot.
- **Editing `purrcfg` cannot turn verification off.** A block signed by the wrong key, or a signed
  block edited afterwards (even with a corrected checksum), is simply ignored: the device falls
  back to `warn`, and a tampered image is still flagged.
- **Override keys and revocations count only inside a valid signed block.** A key cannot vouch for
  itself: only the keys compiled into the bootloader can sign a block.
- **Development mode is explicit and visible.** It is off by default, never goes below `warn`,
  shows on the boot menu (`DEVELOPMENT MODE`), in the handoff and in the secure status, and the
  one-shot loosening flags (`IGNORE_ONCE`, `SECURE_OFF_ONCE`) are honoured only while it is on.
  A production build signs a block with `DEV_LOCKED`, after which the flag is ignored and
  `secure dev on` refuses.
- **Loosening without development mode needs the owner's key**: build a block with
  `purrstrap keys trust-block` and apply it with `trust apply <file>` (or flash it in the
  `purrcfg` partition before first boot).

Known limits of this design, so nobody is surprised:

- **A signed block can be replayed.** There is no counter inside it, so an old block (say, one
  that said `off`) that the owner once signed stays valid. A signed epoch needs a spare field in
  `purrcfg`, which is a future format change (`plan3.md`).
- **The version floors are not signed**, because the OS raises them; someone who can write
  `purrcfg` can lower one and roll back. Closing that needs the hardware root of trust.
- A flash programmer still beats all of this (section 2): the bootloader itself is not verified.

### 6.2 Unsigned apps in developer mode (built 2026-10-26)

By default no unsigned app runs, in any secure mode. A developer can opt in, in Settings > Privacy &
security > Developer options, or with `settings set dev.unsigned_apps on`. All of these must hold:

- **Developer mode is active for this boot**, as the bootloader's handoff reports it. A flag in a file
  cannot fake that, and a device whose signed trust block locks developer mode out can never satisfy it.
- **An administrator switched "allow unsigned apps" on** (stored in `/etc/system.conf`). Turning
  developer mode off turns it off too.
- **The app is in the signed-in user's own folder**, `/home/<name>/apps/<app>/package.cat`. The loader
  checks the path itself: the system's `/apps`, `/system`, `/kernelmods` and `/boot/drivers` always need a
  signature, and so does every module and driver, whatever the settings say.
- **The payload hash still has to match.** The verifier stops at the key lookup, before it has read the
  payload, so the loader checks the hash itself; an unsigned app may not be a damaged one. A revoked key is
  refused outright.

While unsigned apps are allowed the status strip shows a badge, the log carries a warning for every unsigned
app loaded, and the Apps page marks them `UNSIGNED (developer mode)`. An unsigned app runs with the same
access as the system: there is no sandbox yet (section 2), so this is a switch for people building their
own apps and nothing else. Known gap: the installer, with the switch on, accepts a package it could not
verify, including one with a bad hash; the loader still refuses to run such a package.

### 6.3 The secure status

The OS exposes one honest answer to "is this device in a trusted state?" (`purr_secure_status()`,
shell command `secure`), for the system, services and, through a catcall to be added, apps:

| Level | Meaning |
|---|---|
| **GENUINE** | the bootloader enforced `enforce` this boot, the slot it started verified, and development mode is neither on now nor was it at boot |
| **DEVELOPMENT** | development mode is on (now, or it was at boot) |
| **UNVERIFIED** | anything else: not enforcing, the slot did not verify, or there is no handoff |

This is **software attestation**. Without the hardware root of trust (section 10) someone who can
write flash can make a device report GENUINE, so it is the right input for an app's own policy
(a store, a license check) but not, on its own, for content protection that has to resist a
determined owner. The status leaves room for a hardware-rooted level when that exists.

---

## 7. The boot flow and the failure ladder

1. The bootloader reads `purrcfg` and **increments `boot_fail_count`** on every boot.
2. It picks a slot using a three-rung ladder on that count:

   | `boot_fail_count` | Prefers | Then tries |
   |---|---|---|
   | 1-3 | `kernel` | KittenOS, then recovery loader |
   | 4-6 | KittenOS | recovery loader, then kernel |
   | 7 and up | recovery loader | KittenOS, then kernel |

3. Explicit one-shot requests (`reboot recovery`, `reboot loader`) override the ladder once.
4. Otherwise the **boot package** shows a short menu (3 seconds, any key stops it; if ignored,
   the system boots normally) and may choose another slot.
5. When the ladder itself lands on the recovery loader, the bootloader skips the menu and sets
   an auto-reinstall request, so the loader runs its silent restore instead of a menu nobody is
   there to answer.
6. The OS **resets `boot_fail_count` to zero** once it reaches a healthy state. The count
   therefore only climbs across boots that never got that far.
7. The bootloader writes the handoff (section 8) and jumps.

A slot is *usable* if its first byte is the ESP app-image magic (`0xE9`) and, in `enforce`
mode, its signature trailer verifies (section 4.1). The slot actually being launched is always
checked once, so the handoff can report the result, in every mode except `off`. Whatever picked
the slot -- the ladder, a `reboot recovery` request or the boot menu -- the launched slot has to
pass this gate, so an explicit request is not a way around `enforce`.

If KittenOS also fails, the recovery loader downloads a new one. If that fails too, the device
reaches a serial prompt. Whether "wait at the prompt" means a reset loop or a halt in `enforce`
mode is an open decision (`bootloader/SPEC.md` section 6).

---

## 8. The handoff

The bootloader leaves the OS a 20-byte block in RTC memory (handoff version 2) describing why it is running. It has
its own magic and CRC, survives a soft reset and does not survive power loss, so a cold boot or
a stock bootloader reads as "no handoff". **Written by the bootloader and read by the kernel;
confirmed on a T-Deck Plus (2026-10-25): the shell's `purrcfg` command shows `verified`.**

| `boot_state` | Meaning |
|---|---|
| `VERIFIED` | the launched slot's signature trailer verified; `key_id_used` says which key |
| `FAILED_WARNED` | the launched slot did not verify and the policy let it boot anyway (`warn`, or `enforce` with `IGNORE_ONCE`) |
| `FORCED_RECOVERY` | a `FORCE_RECOVERY` or `FORCE_LOADER` request was consumed |
| `CONFIG_DEFAULT` | `purrcfg` was unreadable, defaults used |
| `AUTO_REINSTALL` | the failure ladder sent the boot to the recovery loader |
| `UNCHECKED` | nothing was verified: `secure_mode` is `off` |

`UNCHECKED` exists on purpose: the handoff never claims `VERIFIED` for something that was not
actually signature-checked. `FAILED_WARNED` takes precedence over the others, then
`AUTO_REINSTALL`, `FORCED_RECOVERY`, `VERIFIED`. The shell's `purrcfg` command prints it. CoreOS is meant to read it
first and decide what to warn about.

---

## 9. Updates and recovery

**Two update mechanisms, by what is being updated:**

- **Partition-level writes** for the boot area: `bootpkg`, `kernel`, `kittenos`, `loader`. The
  new image is **downloaded into RAM, verified, and only then written**, so a bad download never
  damages what is already there. A power cut during the write can leave a slot broken, but the
  recovery tiers are untouched and run again.
- **File swap** for everything in the root filesystem (CoreOS, drivers, AppManager, runtimes).
  KittenOS performs it: stage as `<name>.new`, verify, record the request in `purrcfg`, rename the
  current file to `.bak`, rename the new one into place, and boot. If the new version is not
  healthy after N attempts (default 3), it is renamed `.bad` and the `.bak` restored.
  Renames are atomic in LittleFS and each step is recorded, so a power cut resumes cleanly.

**Version floors.** `purrcfg` records, per component, the newest version confirmed on this
device. A floor only rises when an update is confirmed healthy. In `enforce` mode a file below
its floor is rejected; in `warn` and `off` it is allowed with a warning.

**The recovery loader** is a small separate image holding only Wi-Fi, TLS with a certificate
bundle, the public keys and a minimal CoreOS. It has no shell and no filesystem and never
touches LittleFS. It uses a saved recovery network, or asks you to type one. It fetches the
recovery manifest over HTTPS, **but trusts the signature, not the download source**
(`RecoveryLoader/SPEC.md`, `OTA/SPEC.md`).

---

## 10. Hardware protection (eFuses): optional, dangerous, off by default

The chip's eFuses can anchor the whole chain (a burned key digest that the ROM checks), which
closes the "someone replaced the bootloader" hole in section 2. eFuse writes are **permanent and
can brick a board**, so this repository treats them with deliberate friction:

- Compiled in only with `CONFIG_PURR_EFUSE_SUPPORT=y`. A build without it cannot burn anything.
- A burn needs **three separate arm flags, set in three different boots, in order**, plus a
  signed request, plus `enforce` mode, plus a dry-run report in the log. A watchdog or brownout
  reset resets the sequence. (`bootloader/SPEC.md` section 8.)
- The only action defined is "program the secure boot key digest". Disabling JTAG, disabling ROM
  download and flash encryption are reserved and not implemented.
- **Nothing in this repository burns an eFuse today.** The boot menu shows a read-only line for
  the chip's hardware Secure Boot status so you can always see whether it is enabled.
- Develop and test on a sacrificial board first.

If you want hardware-rooted security you take on these risks yourself. Read the ESP-IDF Secure
Boot V2 and flash-encryption documentation for your exact chip and revision first.

---

## 11. Threat model, from the attacker's side

The useful question is not "is it secure" but "who can do what, with what access". Tiers from
easiest to hardest for an attacker:

### Tier 1: remote or app-level

| Attack | State | Mitigation |
|---|---|---|
| Tampered or malicious download (`net_install`, recovery loader, OTA) | HTTPS plus signature, verified in RAM before erase. Strong. | Keep the certificate bundle current; reject downgrades in `enforce`; fuzz the manifest parser |
| Malicious signed app | Signed, but permissions are not enforced and the S3 has no memory protection | Build the permission model; treat any signed app as fully trusted until then |
| Malformed container or relocation data | Bounds are checked (for example `PURR_RELOC_MAX`) | Fuzz and add bounds tests for every parser of untrusted input |
| Network services | Wi-Fi station mode only | Review any listening service when one is added |

### Tier 2: local, no tools

| Attack | State | Mitigation |
|---|---|---|
| Boot menu or text shell at power-on | The shell is behind login; the menu is open | Decide whether the menu may start recovery without credentials |
| Serial or USB console | Open | Confirm nothing sensitive is reachable before login |
| Forcing the failure ladder by power-cycling | Ends in the loader's silent mode, which only installs signed images | Acceptable; consider logging or rate-limiting |

### Tier 3: physical, with a flash programmer

| Attack | State | Mitigation |
|---|---|---|
| Replace the bootloader, keys or `purrcfg` | **Not defended** without eFuses | Hardware root of trust (section 10) |
| Read stored secrets (Wi-Fi record, hashes) | Plain flash; user passwords are PBKDF2-hashed | Flash encryption; strong hashes |
| Roll back version floors in `purrcfg` | CRC only, no authenticator | Sign or MAC the trust fields (key slots, `secure_mode`, floors) |

### Tier 4: lab-grade (glitching, side channels)

Out of scope.

---

## 12. Status: what is real today

| Capability | Status |
|---|---|
| Signed container format and verifier (host-tested) | built |
| Signature check of the boot package by the bootloader | built, checked on a T-Deck Plus |
| `secure_mode` policy for the boot package | built |
| Key bag, roles, default keys, `purrstrap keys` | built (not every tool action in the spec exists yet) |
| Module and kernelmod verification before load | built |
| Failure-count ladder and one-shot requests | built, kernel-to-KittenOS step proven on hardware; the loader tier untested |
| File-swap update with rollback (KittenOS) | built, mechanism proven; end-to-end CoreOS-as-a-file not yet |
| Recovery loader end to end against a real release | built |
| Handoff (bootloader to kernel) | built and verified on hardware (2026-10-25) |
| Signature check of `kernel`, `kittenos`, `loader` slots (trailer) | built (bootloader, `net_install`, recovery loader, `purrstrap coreos slot-image`); host-tested; the kernel slot verified on real hardware (about 0.65 s at boot). Version floors are enforced (an older signed kernel was flagged on the device); `enforce` refusal itself and the other two slots' boots are not yet exercised |
| Signed trust block, `tighten`, development mode, `secure` / `trust apply` commands, `purrstrap keys trust-block` | built; checked on a real device (2026-10-25): signed `enforce`, refusal of a tampered slot with fallback to KittenOS, development mode relaxing to `warn`, the `DEV_LOCKED` production lock, a block signed with the wrong key and a forged block both ignored |
| Override keys and revocations inside the signed block | built (host-tested; override keys are honoured only in a valid signed block); not exercised on the device |
| Replay protection for signed blocks | not built (needs a spare `purrcfg` field) |
| Secure status API and shell view | built (`purr_secure_status()`, `secure`); the app-facing catcall and the Settings toggle are not built |
| Vendor certificates, expiry, `latest_time` | designed, not built |
| App permission enforcement | designed, not built |
| eFuse support (arming flow, secure boot digest) | designed, not built, off by default |
| Flash encryption | not planned yet |

---

## 13. For app developers (SDK)

*(The app SDK is still being built; this is what the design already commits to.)*

- Apps are **pre-compiled, signed `.cat` images**. There are no scripts and no interpreter in the
  first runtime. A MicroPython runtime is planned as a second, separate runtime.
- Apps talk to the system **only through catcalls**, the stable device interfaces that are the
  same on every board. An app must never touch hardware, the OS or ESP-IDF directly; the
  toolchain enforces this.
- Apps sign with the **developer** role. Only apps that need the `superuser` permission are
  owner-signed. A leaked developer key cannot sign boot or system files.
- **Declare what you need.** The manifest (`manifest.toml`) lists permissions. Enforcement is not
  built yet, so do not assume it protects your users, and do not assume a sibling app is
  sandboxed from yours. There is no memory protection between apps on the ESP32-S3.
- Each running app is its own task with its own stack and RAM block, both declared in the header.
- Keep your signing key private and offline. If it leaks, tell your users: it can be revoked.
- Do not ship secrets inside an app. The image is not encrypted.

---

## 14. For people building their own flavor of PURR OS

*(Designed for; the SDK and flavor tooling come later.)* If you ship your own build, **you** own
the trust decisions for your devices. At minimum:

1. **Generate your own keys.** Do not ship the repository's default or test keys. Use
   `purrstrap keys` to generate a key per role, regenerate the default key source
   (`PurrOS/components/coreos/keys/purr_default_keys.c`) from your public keys, and keep the
   private keys off the build host's working tree, ideally on an offline machine or hardware
   token.
2. **Never include the developer-preview test key** in a release. The boot report should never
   say a test key is trusted on a shipped device.
3. **Ship in `enforce` mode** for releases. `warn` is for development.
4. **Keep roles separate.** Use a different key for boot, system, owner and developer roles, so one
   leak is contained.
5. **Own your recovery manifest.** The recovery loader and `net_install` fetch a manifest URL
   that you set (`CONFIG_PURR_RECOVERY_MANIFEST_URL`, `CONFIG_PURR_MODULE_INDEX_URL`). Point them
   at releases you sign and control. Releases are signed locally and the signed files uploaded;
   do not keep private keys in CI.
6. **Rotate deliberately.** Rotating the boot key is non-trivial because the bootloader and boot
   package embed public keys. Plan the rotation path (an open design question,
   `Keys/SPEC.md` section 8) before you need it, and keep backups of the keys.
7. **Decide your hardware story.** If you want a hardware root of trust, test the eFuse flow
   (section 10) on sacrificial boards first. Do not burn anything on a unit you cannot afford to lose.
8. **Boards.** A new board is a board profile, a partition file and a boot-package backend
   (`Boards/SPEC.md`). The board decides the chip, and `chip_id` stops an image for one chip from
   loading on another.

---

## 15. Hardening checklist

For anyone deploying PURR OS on devices that matter:

- [ ] `secure_mode` is `enforce` in release builds: sign a block with `purrstrap keys trust-block --mode enforce --dev-locked`
- [ ] default and test keys replaced with your own; no test key trusted
- [ ] private keys offline, backed up, and passphrase-protected
- [x] per-image verification enabled for `kernel`, `kittenos` and `loader` (section 4.1)
- [x] `purrcfg` trust fields authenticated (section 6.1)
- [ ] recovery manifest URLs point at releases you control and sign
- [x] downgrades blocked in `enforce` (version floors, section 9); note the floors themselves are unsigned
- [ ] boot-menu and serial-console exposure reviewed for your threat model
- [ ] parsers of untrusted input (containers, relocations, manifests, app headers) fuzzed or table-tested
- [ ] no secrets in apps; Wi-Fi record sensitivity understood (flash encryption if it matters)
- [ ] hardware options (eFuses, flash encryption) decided and tested on a sacrificial board

---

## 16. Reporting a problem

If you find a way past any of the checks above, please report it privately before publishing.
Use the repository's private security advisory option if it is enabled, otherwise contact the
maintainer through the project's GitHub page. Please include the version or commit, the board,
the `secure_mode`, and steps to reproduce. A design weakness that the docs already list as
unbuilt (section 12) is still useful to hear about, especially if it is worse than described.

---

## 17. Where the details live

| Topic | Document |
|---|---|
| Bootloader, container, `purrcfg`, boot flow, handoff, eFuse gating | `bootloader/SPEC.md` |
| System overview, layers, flash layout, updates | `PurrOS/SPEC.md` |
| Keys, roles, vendor certificates, time floor | `Keys/SPEC.md` |
| KittenOS (recovery) | `KittenOS/SPEC.md` |
| Internet recovery | `RecoveryLoader/SPEC.md` |
| Update delivery | `OTA/SPEC.md` |
| App format | `CatFormat/SPEC.md` |
| App permissions | `Permissions/SPEC.md` |
| App runtime and SDK | `AppRuntime/SPEC.md`, `AppSDK/SPEC.md` |
| Build and signing tool | `purrstrap/SPEC.md` |
| Boards | `Boards/SPEC.md` |
