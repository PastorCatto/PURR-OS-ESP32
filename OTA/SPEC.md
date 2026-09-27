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

**System files** (modular boards: the kernel, CoreOS, the drivers bundle, AppManager, the
runtimes and the boot package):

- **Stage.** The new file is written into `/boot` as `<name>.new` as it arrives, with its hash
  computed on the way in. The running system is not touched.
- **Verify.** When complete, the running system checks the container, chip, version (must be
  newer), signature, and that it suits this device.
- **Hand off.** A verified file is handed to KittenOS: the running system records the request
  in `purrcfg` and restarts into it. Nothing else replaces system files.
- **Swap.** KittenOS renames the current file to `<name>.bak`, renames `<name>.new` into place,
  and restarts. If the new version does not come up healthy it rolls back
  (`../PurrOS/SPEC.md` section 6.1).

**Monolithic boards** keep no filesystem copy of the system, and their 1 MB apps area is too
small to stage a whole packed image. The running system downloads the image to the **SD card**
and verifies it, then restarts into KittenOS, which applies it from there into the single raw
slot, with no rollback copy. KittenOS has no network stack on these boards, so a Wi-Fi update
needs an SD card. KittenOS's own UI for this comes later.

**Modular boards:** KittenOS carries a Wi-Fi stack with HTTPS, so it can also download a missing
or broken system file itself during recovery.

**Apps** go through AppManager's install steps, not this path.

## 4. Getting an update by Wi-Fi

- Fetch the manifest, compare versions with what is installed, and list what is newer.
- Download with resume: a range request continues where it stopped, so a dropped
  connection does not restart a large image.
- Check free space before starting.
- Progress is printed on the console.

## 5. Two manifests, two formats

Attached to each release, small and readable. Per component (OS image, kernel,
AppManager, runtimes, drivers bundle, boot package, KittenOS):

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

  component=coreos
  version=1.2.0
  chip=esp32s3
  board=tdeck_plus
  file=coreos-tdeck_plus-1.2.0.kitt
  size=245760
  sha256=3b1c2f...(64 hex characters)
  key=system
  min_bootloader=1.0.0
  min_coreos=1.0.0
  ```

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
