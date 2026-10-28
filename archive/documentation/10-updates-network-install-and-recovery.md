# 10. Updates, network install and recovery

How new code reaches a device, how a broken device is brought back, and how to restore one over the
network. Source: `netinstall` and swap code in `PurrOS/main/commands.c`, the recovery loader in
`PurrOS/main/recovery_loader.c`, `PurrOS/components/coreos/src/purr_manifest.c`, `purr_swap.c`,
`bootloader/.../purr_boot.c`. Design: `OTA/SPEC.md`, `Install/SPEC.md`, `KittenOS/SPEC.md`,
`RecoveryLoader/SPEC.md`.

**Status:** **[WORKS]** in code, each piece host-tested or recorded as proven on hardware. **[PARTIAL]**
overall: the project's own notes say a full run against a live published release over real Wi-Fi has not
been done, and two of the paths have problems that make the result less useful than it sounds:
[F-13](FINDINGS.md#f-13), [F-14](FINDINGS.md#f-14), [F-31](FINDINGS.md#f-31). Read those before relying on
network restore.

## The five mechanisms

| # | Mechanism | Who | What it updates | Where it writes |
|---|-----------|-----|-----------------|-----------------|
| 1 | `netinstall kernel\|loader\|bootpkg` | PURR OS or KittenOS shell | one raw partition | the partition directly |
| 2 | `netinstall coreos\|appmanager\|runtime\|devbundle` | shell stages it; KittenOS swaps it | a file on `root` | `/boot/<file>.new`, then renamed in |
| 3 | `netinstall modules` | shell | the `/system` and `/kernelmods` command modules | the files directly |
| 4 | the recovery loader | boots by itself or on request | `kittenos` (and, in restore mode, `kernel` and `bootpkg`) | the partitions directly |
| 5 | stage 2 | KittenOS, automatically after the loader | `kernel` and the modules | as 1 and 3 |

`appinstall` updates apps and is separate ([11](11-apps.md)).

### What may write what

- **`kittenos` can only be written by the recovery loader.** Neither `netinstall` in PURR OS nor in KittenOS
  will do it. That keeps the fallback tier safe from a buggy or compromised system running above it.
- Every download is checked three ways before it is used: size and SHA-256 against the manifest, then the PURR
  container's signature and role, then (for partitions) a read-back hash after writing. The download host is
  not trusted.
- `secure_mode` governs failures: with `warn` (default) or `enforce`, anything that does not verify is
  rejected. Only `off` accepts it, printing `unverified (<reason>), accepted because secure mode is off`.

## The recovery manifest

A flat text file, stanzas separated by blank lines, `key=value`, `#` comments. Read by the recovery loader and by
`netinstall`. The parser is `purr_manifest.c` (88 host checks). Unknown keys are ignored; a stanza missing
`component`, `version`, `file`, `size` or a valid 64-hex `sha256`, or with size 0, is **dropped silently**
(counted in `dropped`, shown nowhere). At most **16** entries are kept.

```
release=1.2.0
released=2026-09-27

component=kernel
version=1.2.0
chip=esp32s3
board=tdeck_plus
file=kernel-tdeck_plus-1.2.0.cat
size=1130496
sha256=3b1c2f...(64 hex characters)
key=boot
min_bootloader=1.0.0
min_coreos=1.0.0
```

| Key | Required | Meaning |
|-----|----------|---------|
| `component` | yes | `kernel`, `kittenos`, `bootpkg`, `loader`, `coreos`, `appmanager`, `runtime`, `devbundle` |
| `version` | yes | shown in messages; staged files also get their version floor from the container header |
| `file` | yes | **file name only**, resolved against the directory of the manifest URL. A full URL is not supported. |
| `size`, `sha256` | yes | of the whole downloaded file |
| `chip`, `board` | no | default `any`. The device matches `esp32s3` and `tdeck_plus`. |
| `key` | no | informational. The real requirement is `purr_role_may_sign()`. |
| `min_bootloader`, `min_coreos`, `release`, `released` | no | informational today |

Which URL is used: `CONFIG_PURR_RECOVERY_MANIFEST_URL`, set at build time ([04](04-building-and-flashing.md),
[18](18-making-your-own-purr-os.md)). The module index (`netinstall modules`) uses
`CONFIG_PURR_MODULE_INDEX_URL` and the same format with `type=module` or `type=kernelmod`.

## 1. `netinstall` for a raw partition

```
alice@PURR OS> wifi connect HomeNet ********
alice@PURR OS> netinstall kernel
netinstall: kernel
fetching the recovery manifest...
found kernel 1.2.0 (1130496 bytes)
downloading kernel-tdeck_plus-1.2.0.cat...
verifying...
writing to kernel...
kernel 1.2.0 installed. Run: reboot
```

Steps in code: require a connection, fetch the manifest (32 KB limit), find the component for this chip and board,
download (2 MB limit), compare size and SHA-256, verify the container (`PURR_IMG_MODULE` with the matching subtype
and a **boot-role** signature), check the payload fits the partition, **erase the whole partition**, write the
payload, read it back and compare the hash.

- **No arguments means `kernel`.** Typing `netinstall` by itself overwrites your kernel slot with whatever the
  manifest says ([F-25](FINDINGS.md#f-25)). There is no confirmation.
- The **payload only** is written, not the PURR header. That is right for `kernel` (a plain ESP image) and
  **wrong for `bootpkg`**, whose partition must hold the whole container. See [F-13](FINDINGS.md#f-13).
- A power cut during the write leaves that partition broken. The bootloader then falls back to the next slot.
- Partition components do **not** check version floors (only file-staged ones do).

## 2. File-staged components and the swap

For CoreOS, AppManager, runtimes and the devices bundle. **[PARTIAL]** The staging, verifying, swapping,
confirming and rolling back all work and are tested, but **nothing loads these files afterward**: the running
system is the monolith, so a swapped `/boot/coreos.kitt` is just a file ([PurrOS/SPEC.md](../PurrOS/SPEC.md)
section 6.1 says so itself).

Flow, using `netinstall coreos` as the example:

1. **Stage.** The shell downloads, verifies (chip, subtype, boot-role signature for `coreos`, system-role for the
   others) and, in `enforce` mode, checks the version floor. It writes `/boot/coreos.kitt.new`, records
   `update = {target, REQUESTED, attempts 0, version}` in `purrcfg`, sets `FORCE_RECOVERY`, and prints
   `coreos 1.2.0 staged. Run: reboot`.
2. **Reboot.** The bootloader starts KittenOS (skipping the menu) because of `FORCE_RECOVERY`.
3. **Swap (KittenOS only, early in boot).** It re-verifies the staged file itself, records `MOVING` (so a power cut
   resumes), renames the current file to `.bak`, renames `.new` into place, records `UNCONFIRMED`, and restarts.
4. **Confirm.** When the normal system next reaches a healthy boot, `purr_mark_boot_healthy()` raises that
   component's version floor to the new version and records `CONFIRMED`.
5. **Roll back.** If the new file does not reach healthy, KittenOS retries up to 3 times, then renames the bad file
   to `.bad`, restores `.bak`, and records `FAILED`.

The state machine is `purr_swap_decide()` (22 host checks). States: none, staged, requested, moving, unconfirmed,
confirmed, failed.

## 3. `netinstall modules`

```
alice@PURR OS> netinstall modules
fetching the module index...
fetching about...
about 0.1.0 installed to /system/about.cat
...
modules: 8 installed, 0 skipped (chip/board), 0 failed
```

Fetches the module index, then for each `type=module` or `type=kernelmod` entry matching this chip and board:
downloads (256 KB limit), checks size and SHA-256, verifies the container (`PURR_IMG_MODULE`, any allowed signer for a
`driver`-subtype module) and **writes it straight to `/system/<component>.cat` or `/kernelmods/<component>.cat`**,
overwriting. There is no staging or rollback: a bad file is quarantined by the loader at the next boot.
`reboot` afterwards to load them. Modules built for a different ABI version are refused at load time, so
update the modules whenever the table version changes ([12](12-writing-modules.md)).

`PURR OS` does not run `plantmodules` on boot any more. That is deliberate: a fresh `root` has no modules until
you run `netinstall modules` or `plantmodules`.

## 4. The recovery loader

The `minimal` profile, in the `loader` partition. Shown on screen as `PURR internet recovery`.

### How it starts

| Trigger | Mode |
|---------|------|
| `reboot loader` | menu mode |
| Nothing else bootable, then choosing **Internet recovery** in the boot menu | menu mode |
| The ladder reaches 7 failed boots | **auto mode** (`AUTO_REINSTALL` set) |

### Menu mode

1. Starts Wi-Fi. Shows `press S within 3 seconds for a Wi-Fi shell, F for a full component restore...`.
   - **S**: a tiny shell with `help`, `wifi scan|connect|forget|list`, `net`, `exit`. Use it to check or set
     up Wi-Fi, then `exit` to continue.
   - **F**: full component restore instead of KittenOS only.
2. Connects: the saved `netrec` network if there is one, otherwise it asks you to type the network name and
   password (there is no saved-list; it loops until one connects).
3. Fetches the manifest. **Default:** finds the `kittenos` entry, downloads, verifies (`image_type` must be
   recovery), erases and writes the `kittenos` partition, reads it back, then restarts.
4. **Full restore (F):** checks `kernel`, `bootpkg` and `kittenos` in turn and rewrites each one that does not
   verify, from the manifest.

Both end by resetting `boot_fail_count` to 0, setting `CONTINUE_INSTALL`, and restarting.

### Auto mode

Used when nothing ever reaches a healthy boot. No menu, no prompts. It uses only the saved `netrec` network; with
none it halts with `no saved network: cannot recover unattended`. Then the same component restore as F, then restart.
Failures `halt` (the screen stays up with `halted. Power-cycle to try again, or use the boot menu.`).

**[PROBLEM]** The "is it already good" check reads the partitions as PURR containers, but `kernel` and `kittenos`
hold plain ESP images, so it never says "already good": a restore rewrites `kernel` and `kittenos` every time even if
they were fine ([F-14](FINDINGS.md#f-14)).

## 5. Stage 2: KittenOS finishes the install

When a normal-profile system boots with `CONTINUE_INSTALL` set (in practice KittenOS, right after the loader; it clears the flag first), after Wi-Fi starts it:

1. loads the `netrec` network and connects (15 seconds),
2. runs `netinstall kernel`,
3. runs `netinstall modules`.

Any failure just skips the rest. You still end at a normal login. Then `reboot`.

## Scenario walkthroughs

### A. PURR OS will not boot, but the device is otherwise fine

The failure counter climbs by one per attempt. At failure 4 the bootloader prefers KittenOS. Or force it:
hold a key at the boot menu and pick `KittenOS`. In KittenOS:

```
wifi connect HomeNet ********
netinstall kernel
reboot
```

### B. KittenOS and PURR OS both broken

The ladder reaches the loader at failure 7 (or the menu shows `Internet recovery` if nothing is bootable). With a
saved `netrec` network it recovers unattended. Otherwise press **S** to set up Wi-Fi or just let it ask for the
network name. When it restarts, KittenOS boots and stage 2 reinstalls the kernel and modules. Time: depends on
download speed; the loader holds the whole image in PSRAM.

### C. Blank flash with only bootloader, partition table and loader

Flash those three by wire ([04](04-building-and-flashing.md)). The bootloader finds nothing else bootable and boots
the loader. In menu mode it asks for the Wi-Fi name and password, installs KittenOS, restarts, and KittenOS formats
`root` and runs stage 2. This is the "network install" of `Install/SPEC.md`. The `bootpkg` is not installed on this
path (use **F**).

### D. Erase only the modules or `root`

Run `netinstall modules` in KittenOS or PURR OS (needs Wi-Fi), then `reboot`. If you have no network, run
`plantmodules`, then `reboot`.

### E. Roll a bad kernel back

There is no `.bak` for partition components. Reinstall the previous release's kernel with `netinstall kernel`
pointing at a manifest that lists it, or flash it by wire. Use the version floor and keep old releases published.

### F. Pin or downgrade

`secure_mode=enforce` plus raised floors stops downgrade of **file-staged** components. Partition components have no
floor check. There is no command to lower a floor; write a fresh `purrcfg` ([05](05-boot-process-and-flash-layout.md)).

## What a release must contain

See [18](18-making-your-own-purr-os.md) for the full procedure. In short: signed `kittenos`, `kernel`, `bootpkg`
(and optionally `loader`) files, a `recovery.manifest`, and a `modules.manifest` plus the module files, all
published at the two URLs compiled into the device.

## The shipped manifest and [F-31](FINDINGS.md#f-31)

The manifest the repo points at lists a `kernel` entry that is the standalone `Kernel/` binary (it idles with no
shell) and no full PURR OS image, and there is no `purros` component in `netinstall`. So a network-restored device
ends at **KittenOS** with working Wi-Fi and the modules, and an idle kernel in `kernel` that the bootloader prefers.
To end at PURR OS instead, publish the full build as the `kernel` component ([04](04-building-and-flashing.md)
layout B, [F-31](FINDINGS.md#f-31)).

Next: [11 Apps](11-apps.md).
