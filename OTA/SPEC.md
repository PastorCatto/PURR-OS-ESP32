# OTA spec (draft 0.1)

How new system images, modules and apps get onto a device and are swapped in. The
staging slot and the move procedure are defined in `../PurrOS/SPEC.md` section 6.1.
Installing apps is AppManager's job (`../AppManager/SPEC.md`).

## 1. Decisions so far

- **Two ways to get an update onto the device,** with one shared install step:
  - a **file** you copy on: SD card, MTP (T-Deck Plus only), or serial
  - a **download over Wi-Fi**
- **Only the user starts an update for now,** with a shell command. Automatic checks
  are not built yet.
- **Later, an OTA app** holds the update check itself and handles it. It is a
  **`.cat` file**, a privileged app installed in the apps area like any other, which
  is one reason the SDK has a privileged template.
- **System updates come from the main repo's Releases tab.** Each release has the
  images attached, plus a small manifest describing the latest versions.
- **Apps come from a separate GitHub profile and repo,** a folder of `.cat` files with
  a generated index. Apps are only **listed** there, and are downloaded when the user
  asks. They are not bundled into system releases.
- **purrstrap builds the app index.** It walks a folder of pre-compiled `.cat` files,
  reads each one, and writes the index for the repo. The index is also the record of
  what was packed (`../purrstrap/SPEC.md` section 6).

## 2. Sources and trust

- The device fetches the **manifest as a raw file** from the repo, not through
  GitHub's API, which has low rate limits for anonymous use and returns large
  responses.
- Downloads use **HTTPS with certificate checking**, using the certificate bundle that
  ESP-IDF ships.
- **The download source is not trusted for integrity.** Every image and app is
  signed, and the device verifies it after download exactly as it does for a file
  copied on. A bad download can never be installed.

## 3. Staging and handoff

System files split into two mechanisms, not one, since the kernel/CoreOS split
(`../PurrOS/SPEC.md`): the kernel is its own raw, directly-bootable partition now, not a file a
running system swaps in.

**Partition-level components** (`kernel`, `loader`, `bootpkg`): a raw `esp_partition` erase and
write, straight from whichever system is running (full PURR OS or KittenOS -- both carry
`net_install`, `PurrOS/main/commands.c`). Each is a `PURR_IMG_MODULE` image with its own subtype
(`PURR_MOD_KERNEL`, `PURR_MOD_LOADER`, `PURR_MOD_BOOTPKG`), and every one of those subtypes
requires a `PURR_ROLE_BOOT` signature (`purr_keybag.c`) -- the same trust tier as the boot
package itself, regardless of which system happened to run the install.

- Fetch and verify in memory first: container, chip, type and subtype, signature, hash.
- Erase and write the target partition, then read it back and compare the hash rather than trust
  the write.
- **Packaged by `purrstrap coreos package-component --component kernel|loader --file <bin>
  --version <v>`** (built 2026-09-30, alongside the standalone kernel binary itself, `Kernel/`):
  wraps an already-built raw binary into an unsigned `PURR_IMG_MODULE` with the matching
  `PURR_MOD_*` subtype, ready for `keys sign --key boot.key`. The payload is never parsed by
  the installer either way -- only `purr_image_verify`'s header checks and, later, the
  bootloader when it actually tries to boot the partition -- so this works for a stock
  ESP-IDF app image (`kernel`) exactly as it would for any other opaque blob.
- **How a package step wraps a payload into its file** (the same for `package-component`, `bootpkg build`,
  `uiassets package`; `purrstrap/lib/image.py`, `make_image`, and `purr_image_header_t` in `purr_abi.h`). The file is
  one header followed by the payload bytes, unchanged:

  | field | notes |
  |---|---|
  | `magic` `'PURR'`, `header_version`, `header_size` | the layout is declared once, in `lib/image.py` |
  | `chip_id`, `image_type` (`PURR_IMG_MODULE`) | the chip is checked against the device |
  | `key_id`, `signature[64]` | zero in the packaged file; `keys sign` fills them (P-256 over every byte before the signature) |
  | `flags` low byte | the subtype: `PURR_MOD_KERNEL`, `LOADER`, `BOOTLOADER`, `BOOTPKG`, `UIASSETS` ... |
  | `name[32]`, `version[12]`, `min_boot_version[12]` | `name` is the component; the manifest names the file `<component>-<board>.cat` |
  | `payload_offset`, `payload_size`, `payload_sha256` | the payload is the bytes that reach the partition (kernel, loader, ui_assets) or the whole file is kept (bootpkg) |

  Package, sign, then `dev recovery` copies the file in and writes its size and sha256 (and the payload's) into
  `recovery.manifest`; the device checks the file hash, the signature and the subtype's allowed key, writes, and reads
  back the payload hash. A new component therefore needs one subtype, one row of `purr_role_may_sign`, one install
  branch in `commands.c`, and one name in `RECOVERY_COMPONENTS` (`purrstrap/scripts/dev.py`).
- **`uiassets`** (`PURR_MOD_UIASSETS`, 10): the picture pack for the `ui_assets` data partition, one pack per shell
  (the Windows-style and the Android-style packs differ). Signed by the boot key (a dev server) or the system key (a
  release). Only the used part of the pack is sent, not the padding to the partition's size. Stage 2 (and
  `netinstall uiassets`) writes only that part, skips the write when the partition already holds the same payload, and
  needs no reboot beyond the next UI start. Packaged by `purrstrap uiassets package`, hosted by `dev recovery --uiassets`.
- `kernel` also becomes the boot target immediately (no reboot needed to select it); `loader` and
  `bootpkg` are read directly by the bootloader's own fallback chain
  (`../bootloader/SPEC.md`), so the write above is the whole install.
- **`kittenos` is never written this way.** Only the recovery loader's own `install_kittenos()`
  (`../RecoveryLoader/SPEC.md`) may write it, and that only runs from the `minimal` profile, which
  never shares a boot with a general shell -- so the fallback tier can't be overwritten by
  whatever's running above it, even a compromised or buggy one.

**File-staged components** (CoreOS, AppManager, the runtimes, the drivers bundle) -- built
2026-09-29, `net_install`'s `s_net_install_staged` table and `purr_swap_setup()`
(`PurrOS/main/commands.c`), decision logic in `purr_swap_decide()`
(`PurrOS/components/coreos/{include,src}/purr_swap.{h,c}`, host-tested):

- **Stage.** The new file is written into `/boot` as `<name>.new` as it arrives (`net_install
  coreos`/`appmanager`/`runtime`/`devbundle`), verified the same way partition-level components
  are (container, chip, type and subtype, signature, hash, and now also a version-floor check
  against `purrcfg`'s `floors[]` -- the one thing the partition-level path still doesn't enforce).
  The running system is not touched.
- **Hand off.** A verified file staged this way records the request in `purrcfg` (`update.state
  = REQUESTED`) and sets `FORCE_RECOVERY`, the same one-shot flag `reboot recovery` already
  used. Nothing else replaces system files.
- **Swap.** KittenOS re-verifies the staged file by itself (never trusting a file just because
  it verified once before landing on flash), records `MOVING` before touching anything (crash
  safety: a power cut here resumes correctly next boot instead of restarting from scratch),
  renames the current file to `<name>.bak`, renames `<name>.new` into place, sets
  `UNCONFIRMED`, and restarts.
- **Confirm.** `purr_mark_boot_healthy()` (the same function that resets the boot-failure
  ladder, `bootloader/SPEC.md` section 6) now also confirms an `UNCONFIRMED` update once a
  normal boot reaches healthy: raises that component's floor, sets `CONFIRMED`.
- **Roll back.** Retries up to 3 times (`update.attempts`) before giving up: renames the bad
  file to `<name>.bad`, restores `<name>.bak`, marks `FAILED`.
- **What "swapped" doesn't mean yet:** the kernel/CoreOS split (`../PurrOS/SPEC.md` section 6)
  that would actually *load* `/boot/coreos.kitt` isn't built -- `full`/`recovery` are still one
  monolithic binary each. This mechanism correctly stages, verifies, swaps, confirms and rolls
  back the file itself; nothing yet reads it back out and runs it.

**Monolithic boards** keep no filesystem copy of the system, and their 1 MB apps area is too
small to stage a whole packed image. The running system downloads the image to the **SD card**
and verifies it, then restarts into KittenOS, which applies it from there into the single raw
slot, with no rollback copy. KittenOS has no network stack on these boards, so a Wi-Fi update
needs an SD card. KittenOS's own UI for this comes later.

**Modular boards:** KittenOS carries a Wi-Fi stack with HTTPS, so it can also download a missing
or broken system file itself during recovery.

**Apps** go through AppManager's install steps, not this path.

**Individual command modules and kernelmods** (the `/system` and `/kernelmods` `.cat` files --
`about`/`apps`/`wifi`/`netinstall`, `appmgr`/`fs`/`login`/`sysinfo` today) are neither
partition-level nor file-staged: no erase-and-write, no `.new`/`.bak`/rollback dance. Each is
small, individually replaceable, and the module loader already tolerates a bad one at load time
(quarantining it rather than failing to boot, `Modules/SPEC.md` section 7.1) -- the staging
machinery above exists to protect things a bad write could brick (a raw partition) or that need
an all-or-nothing swap (one big CoreOS/AppManager file); neither risk applies here. Install is a
direct fetch-verify-write straight to `/system/<name>.cat` or `/kernelmods/<name>.cat`,
overwriting whatever was there -- the same `purr_fetch_into`/`purr_image_verify` steps
`appinstall` already uses for apps, just targeting these folders instead of a user's apps root.
Listed in the module index (section 6.1), not the recovery manifest: the recovery loader never
touches them (`../RecoveryLoader/SPEC.md` section 5), only KittenOS, once it has Wi-Fi and the
real filesystem, does -- the same tier as apps, not the boot-critical tier above.

This replaces `plant_temp_modules()` (`PurrOS/main/commands.c`), the temporary scaffolding that
embedded these same eight files directly in firmware and planted them on every boot since there
was no real distribution mechanism yet.

**Built, 2026-09-30: `netinstall modules`.** Fetches the module index, installs every entry
matching this chip/board straight to `/system`/`/kernelmods` as described above, and reports a
installed/skipped/failed count. `plant_temp_modules()`'s automatic call is disabled now that
this exists (the function and the manual `plantmodules` command stay, as a no-network local-dev
fallback -- `PurrOS/components/coreos/SPEC.md` section 10). Host-tested (the manifest's `type=`
field, section 6.1) and compiles clean on-device; **not yet exercised against a live device over
real Wi-Fi** -- that needs real network credentials and a reachable server, neither available
this session.

## 4. Getting an update by Wi-Fi

- Fetch the manifest, compare versions with what is installed, and list what is newer.
- Download with resume: a range request continues where it stopped, so a dropped
  connection does not restart a large image.
- Check free space before starting.
- Progress is printed on the console.

## 5. Two manifests, two formats

Attached to each release, small and readable. Per component (kernel, loader, boot package,
AppManager, runtimes, drivers bundle, CoreOS, KittenOS):

- component name and version
- which chip and board it is for
- file name, size and SHA-256
- which key role signed it
- the minimum bootloader and CoreOS versions it needs

**There are two manifests, not one, because their readers have very different means:**

- **The recovery manifest.** Read by the **recovery loader** (no filesystem, only what fits
  in its own small image) and by **KittenOS** doing a network install (`../Install/SPEC.md`),
  both size-constrained. **Flat text**, dependency-free to parse: stanzas of `key=value`
  lines separated by a blank line, `#` starts a comment, one stanza per component. Unknown
  keys are ignored, so it can grow. An entry missing a required field, or with a malformed
  size or hash, is dropped rather than trusted with a guessed value. `chip` and `board` may
  be `any`. Implemented in `PurrOS/components/coreos/{src,include}/purr_manifest.{c,h}`
  (`../RecoveryLoader/SPEC.md`). Example:

  ```
  release=1.2.0
  released=2026-09-27

  component=loader
  version=1.2.0
  chip=esp32s3
  board=tdeck_plus
  file=loader-tdeck_plus-1.2.0.kitt
  size=245760
  sha256=3b1c2f...(64 hex characters)
  key=boot
  min_bootloader=1.0.0
  min_coreos=1.0.0
  ```

  `key` names the role that must have signed the image (`../Keys/SPEC.md`), and it has to match
  what `purr_role_may_sign()` (`purr_keybag.c`) actually requires for that component's image type
  and subtype -- a wrong `key` here just means the device rejects a correctly-signed download, not
  a way to loosen the requirement. `kernel`, `loader`, `bootpkg`, `coreos` and `kittenos` all need
  `key=boot`; `appmanager`, the runtimes and the drivers bundle need `key=system` (section 3).

- **The system manifest.** Read by the **full, running system's own `update` commands**
  (section 7), which already carry plenty of flash, RAM and a JSON library. **JSON**, not
  yet designed — it can carry more (a changelog, channels) since nothing tiny has to parse
  it. Built when the `update` commands are, not part of this pass.

The two can list the same releases; they are separate documents for separate readers, not
separate content.

## 6. The app index

Generated by purrstrap. Each app's entry has:

- name, version and description
- file name, size, and the SHA-256 of the whole file
- class (unprivileged or privileged) and kind
- the permissions it requests
- the catcalls it needs, with minimum versions
- the runtime it needs and its minimum version
- the platform families its payloads cover
- the SDK version it was built with
- which key signed it
- an optional icon

## 6.1 The module index

The same pattern as the app index (section 6), one tier down: the individual command modules
and kernelmods (section 3), not whole system components. **A separate list from the recovery
manifest on purpose** -- the recovery loader is size-constrained and never reads it (section 3,
`../RecoveryLoader/SPEC.md` section 5); only KittenOS, once it has Wi-Fi and a filesystem, does,
the same as the app index. Pulled in **alongside the CoreOS release it was built against**, not
as its own independent release channel: a module is only ever meant for one specific kernel-table
or core-table ABI version (`Modules/SPEC.md`), and the module loader already refuses a mismatched
one at load time -- pairing the index to a CoreOS version means that mismatch is caught before a
pointless download, not just after.

Generated by purrstrap (the same `coreos`/`appmanager`-kind builds already produce the `.cat`
files; the index just lists what came out). Each entry:

- name (`about`, `fs`, ...) and which folder it belongs in: `module` (`/system`) or `kernelmod`
  (`/kernelmods`)
- version, chip and board (`any` matches every chip or board, same as the recovery manifest)
- the CoreOS version it needs at minimum (`min_coreos`, the same field every other entry
  already has -- not a required exact match; the real compatibility gate is the ABI version
  baked into the `.cat` file's own header, already checked at load time)
- file name, size and SHA-256
- which key signed it (today, always `developer` -- `purr_role_may_sign()`, `purr_keybag.c`)

**Built, 2026-09-30: flat text, reusing the recovery manifest's parser.** `purr_manifest_entry_t`
gained one new field, `type` (empty for a whole-component entry, `module`/`kernelmod` for one of
these), rather than a second parser -- avoids pulling a JSON library into KittenOS's own small
build just for this. `purrstrap modules index --modules-dir <dir> --kernelmods-dir <dir>
--coreos-version <v> --key <role> --out modules.manifest` generates it from a folder of
already-built, already-signed `.cat` files, reading each one's own header for its name and
version.

## 7. Shell commands (placeholders)

- `update check`: fetch the manifest and list what is newer
- `update install <component|file>`: stage, verify and apply
- `update status`: what is staged, and what is waiting to confirm
- `update cancel`: discard a staged image
- `rollback`: return to the previous version where one is kept

## 8. The OTA app (later)

- A privileged `.cat` app, installed in the `apps` area like any other. It does the
  check on its own, and fetches apps from the app repo.
- KittenOS can carry the same `.cat` as a mini-app, so recovery can update too.
- A sibling app, the **app storage**, browses the app repo and moves apps between devices
  (`../Transfer/SPEC.md`). It is unprivileged.
- If the app is missing or broken, the shell's `update` commands still work.
- It stages and asks for the apply through a privileged `update` catcall. It never
  writes partitions itself.
- Installing apps from the repo goes through the `appmgr` catcall (the
  `device-admin` permission).

## 8.1 Withdrawing a release (2026-11-05)

A release that turns out to be bad must be stoppable at once, and a device already running it must be told. The manifest (section 5) carries it; nothing else changes.

**Format**, all optional (an older reader ignores unknown keys, so a manifest with them is still valid for it):

    withdrawn_versions=kernel@1.0.20,kittenos@1.0.20,*@1.0.20     release-wide: component@version, "*" is every component, comma separated, 160 characters at most
    withdrawn_reason=the setup wizard showed two Wi-Fi pages         one line the person sees, 64 characters at most

and, inside an entry, `withdrawn=yes` (the same version, marked on the entry that still names it). The release-wide list is what a device already ON that version needs, because the manifest only ever names the latest entry of each component.

**What a device does** (`purr_manifest.c`; all of it through the one function every reader uses):
1. `purr_manifest_find()` never returns a withdrawn entry (its own flag, or named by the list). The recovery loader, `netinstall`, stage 2, Settings > Updates, the module and driver installs and the dev-server check all go through it, so a withdrawn version is not offered and not installed. With nothing else for that component, the answer is "nothing to install".
2. `purr_manifest_find_any()` still returns it, to say why nothing is offered; `purr_manifest_is_withdrawn(m, component, installed_version, reason)` answers for the version a device is running.
3. The existing `blocked=yes` (driver index, plan8) stays what it was: a version that broke on a newer OS. Withdrawn is the publisher pulling a release.

**Publisher side** (`purrstrap dev withdraw --version 1.0.20 [--components kernel,kittenos] --reason "..."`, `dev unwithdraw`): the list lives in `recovery/withdrawn.list` and is written into both manifests on every `dev recovery`, so a later publish does not lose it. The tool says what the server still hosts. For the official release the same lines go into the release's manifest assets (`tools/release/make_release.py`, not wired yet).

**Not in the first step** (to build, in this order): (1) say it in words on the device: the loader and `netinstall` print the reason, Settings > Updates says "your version was withdrawn, update to X" and offers a lower version when the withdrawn one is installed (the update check only offers newer today); (2) re-read the manifest just before the last write in the loader, so a withdrawal during a download takes effect; (3) sign the manifest (`recovery.manifest.sig`, boot key) and remember the newest `release` stamp, so a replayed old manifest cannot hide a withdrawal and a forged one cannot withdraw (an unsigned flag can only block, never install, so the first step is safe without it); (4) the companion checks the manifest for a withdrawal of the running version every hour or so; (5) the Store's repository index (already signed) gets the same per-item flag; (6) a downgrade guard: an update source whose kernel is OLDER than the one installed (the official 1.0.11 against a 1.0.21 device) asks first.
**Limits:** a device only learns of it when it next reads the manifest (there is no push), and a device built before this cannot honour the flag: for those the only protection is the server not offering the version, which `dev withdraw` also does while the entry is replaced.

## 9. Failure handling

Every user-facing feature needs a fallback.

- Download fails or drops: the old version keeps running, and the download can resume.
- Verification fails: the staged image is discarded and the reason is shown.
- The new image does not start: it rolls back to the previous version automatically.
- No Wi-Fi: the file path still works.

## 10. Testing

- Host: manifest parsing, version comparison, resume logic, and staging with a fake
  flash that loses power at every point (the same harness as the move routine).
- Board: a full update from a test release over Wi-Fi, a tampered image, a dropped
  connection, and a deliberate bad image that must roll back.

## 11. Open questions

- **An SD card is required for Wi-Fi updates on 4 MB boards,** since the apps area cannot stage a
  whole image and KittenOS has no network stack there.
- **The recovery manifest's format is settled** (section 5). **The system manifest's JSON
  shape is still open,** along with the version naming scheme (semantic versions plus names
  such as `1.0.0-dp10`) and whether there are channels (stable, preview).
- **Who signs releases,** and whether that happens in CI.
- **Certificate handling:** the bundled certificate authorities, or pinning GitHub's.
- **How the app repo is trusted:** which keys may sign apps published there.
- **Whether the app index is also signed,** or only the apps in it.
