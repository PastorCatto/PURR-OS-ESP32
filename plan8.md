# plan8: the repository foundation, the Developer Portal and the SDK server

**Source of truth for this work.** Re-read after any context compaction. Update the Status table and the
Session log as work happens. Started 2026-10-27. Builds on plan7 (the W10M shell, Settings) and SECURITY.md 6.2
(unsigned apps in developer mode).

## Goal

1. A **repository** concept in the app manager: any source of apps (an index of what exists, and the packages)
   behind one client, so a local SDK server today and an official app store later use the same backend code.
2. The **Developer Portal**: a separate app on the Home screen, visible only while developer mode is active,
   that finds the local SDK server and installs its pre-compiled (unsigned or dev-signed) apps like a
   makeshift app store. Local connections only, never the internet.
3. The **developer side**: a host tool that serves a folder of built apps as a repository and announces itself
   on the local network, so shipping an app to the device is one command and a tap, with no long URL.

The **Settings app is left alone** (owner, 2026-10-27): no pages are removed or moved, and its Developer options
page keeps the "Allow unsigned apps" switch. The portal is purely additive.

## Decisions (owner, 2026-10-27)

1. The portal is its own app with its own window, not a Settings page. Settings is untouched.
2. The portal installs into `/home/<user>/devapps`, signed by the developer key or unsigned when the existing
   "Allow unsigned apps" switch (in Settings, unchanged) is on. The loader accepts an unsigned app from the
   user's `apps` or `devapps` folder, nowhere else, and only with developer mode active and the switch on.
3. The shell key `dev.unsigned_apps` and the Settings switch stay exactly as they are.
4. The portal tile exists only while developer mode is active this boot (the bootloader's handoff says so).
   Nothing is planted on disk. **Dev apps show on Home with a small DEV mark** (any app in `devapps`, and any
   unsigned app). A dev app's name wins over a signed app of the same name while developer mode is active; the
   signed one is untouched on disk.
5. **Pull, not push.** The device fetches from a repository it was told to trust. No inbound control channel,
   no custom crypto. (Direct push and log streaming can come later on top.)
6. **Local connections only** for the SDK repository kind: the host must resolve to a private (10/8, 172.16/12,
   192.168/16), loopback, or link-local address. A public address is refused. The store kind is the opposite:
   HTTPS only, signed packages only, signed index.
7. **Foundation for the official store**: the index format reserves the fields a store needs (signature,
   permissions, icon, category, changelog, minimum OS version); the app manager's repository client enforces
   per-kind policy; `purrstrap` gets a store-index builder that signs an index. Nothing here is "the store";
   it is the backend it will stand on.

## The repository format (v1)

    GET <base>/purr/v1/index.json
    {
      "format": 1,
      "repo":  {"id": "...", "name": "...", "kind": "sdk" | "store"},
      "apps":  [ {"name": "clock", "title": "Clock", "version": "1.0.0", "size": 1121,
                  "sha256": "<hex>", "chip": "any" | "esp32s3", "abi": 2, "signed": true,
                  "key_id": 2, "desc": "...", "url": "apps/clock/1.0.0/package.cat",
                  "updated": 1793000000} ],
      "index_sig": "<hex, store kind only, reserved>"
    }
    GET <base>/purr/v1/<url from the index>    -> the .cat package

The client checks size and sha256 against the index, then hands the package to `purr_appmgr_add`, which does
the real verification as before. The index is advisory; the package decides.

## Discovery (SDK kind)

UDP broadcast `PURR-REPO?` to port 47800; a server answers `PURR-REPO! <name> <port> <id>`. In the VM (user-mode
networking) a broadcast does not leave the guest, so the portal also tries `10.0.2.2:47801`, the last server
used, and a typed address. The first connection to a server asks the owner to confirm its name and id; the id
is remembered.

## Phases

| # | Phase | Result |
|---|---|---|
| 1 | Foundation in coreos (pure C, host-tested): index parser, URL policy, version compare, update check; spec | host tests pass |
| 2 | Device: devapps folder and loader rule, `purr_repo` fetch and install on top of purr_fetch, DEV mark on tiles | the shell can install from a repository |
| 3 | The Developer Portal app: server list and trust, browse, install, update, remove, run, unsigned switch, DEV marks | tap to install from the SDK server in the VM |
| 4 | Host tool: `purrstrap dev serve / publish / list`, discovery responder, live re-index | `purrstrap dev publish clock.cat` and it appears |
| 5 | Store backend pieces: `purrstrap store index` (signed index), Settings > Apps "Store" source stub | the format is proven end to end with a signed index |

## Security notes

- The portal and the dev folder exist only while developer mode is active this boot. A production-signed trust
  block that locks developer mode out can never run any of it.
- Local-only is enforced twice: by the URL policy before connecting, and on the resolved address.
- Every download is bounded (size cap) and hash-checked before the app manager sees it.
- A repository can offer apps; it cannot change settings, accounts, keys or the trust block.
- Open question: a malicious device on the same LAN could impersonate the SDK server. Confirming the server on
  first use, remembering its id, and showing the server name in the portal limits this; the package decides what
  runs, and unsigned packages run only from devapps with the switch on.

## Status

| # | Item | State |
|---|---|---|
| 1 | Foundation library (`purr_repo`) and host tests | **done**: 165 checks, whole host suite passes |
| 2 | Device integration: `purr_repo_dev` (discover, index, install), devapps folder, loader rule, no-redirect fetch, DEV marks | **written, compiles** (kernel 1.65 MB); not yet exercised on the VM |
| 3 | Developer Portal app (`purr_w10m_devportal.c`) | **written, compiles**; not yet exercised on the VM |
| 4 | Host tool `purrstrap dev publish / index / list / serve` + discovery responder | **done and cross-checked** against the device's C parser |
| 5 | Store backend pieces (signed index, Settings "Store" source) | todo |

## Test setup

- Host: `purrstrap dev publish --file X.cat --dir <folder>`, then `purrstrap dev serve --dir <folder>`. In the VM the
  server is `10.0.2.2:47801` (the portal tries it by itself when it looks for servers).
- Developer mode must be active this boot (Settings > Privacy & security > Developer options, restart), and the
  "Allow unsigned apps" switch on to install an unsigned app. The debug VM image has developer mode forced on and
  signs in as `dev` automatically (`sdkconfig.flavor.vmdebug`, assembled by `mkimage_dbg.sh` in the notes).
- Index format and rules: `PurrOS/components/coreos/include/purr_repo.h`. The index signature for the store kind
  is a detached `index.json.sig` over the exact bytes (not embedded), not yet implemented.

## Session log

- 2026-10-27 (build): `purr_repo` library + 165 host checks; `purrstrap dev` tool; device client, portal app,
  devapps folder (loader accepts an unsigned app only from the user's `apps` or `devapps` folder, with developer
  mode active and the switch on; a developer app wins over an installed one of the same name), DEV marks, portal
  tile only in developer mode, `purr_fetch_local_into` (a redirect is a failure). Also: automatic sign-in
  (developer mode only; `login.autologin`; the VM debug image's built-in account), and the self-test removed.

- 2026-10-27: design agreed (above), after the owner moved from a push model to a pull model with an SDK server
  as a local-only makeshift store, and asked for the app-manager and developer-side foundation to be built so the
  official store is mostly ready. Starting phase 1.

### Owner idea for phase 5 (2026-10-28), needs a decision
A separate GitHub repository that holds only the pre-compiled, signed app binaries, each linking to its source. The OS reads an index from
it and shows only apps (no source tree to sift through). Open questions: one repo with an `index.json` built by CI vs a release per app;
who signs (the portal key, role per `SECURITY.md` 6.2); how a user mirrors it (the same `purrstrap dev serve` layout?).

### Decided (owner, 2026-10-28): open repository, community keys, update service
- The app index is an open repository people can add to (pull requests), not a closed list. It holds pre-compiled packages plus links to source. A CI step may still
  build `index.json` from it, but the repo is the source of truth.
- Signing: a published **community key** anyone can sign with, or people generate their own keys and publish the public half. A package carries its signature; the
  store verifies it on download and the OS verifies it again on install.
- Trust tiers (needed so "has a signature" means something): official key = PURR-signed, community key = community, a user-added key = "your key"
  (shown by name, added deliberately in Settings > Store > Keys). An unknown key is refused unless dev mode allows it. A signature proves who and that it is
  unchanged, not that it is safe; the store shows the tier and the permissions the app asks for.
- **Update service** (Windows-Update style): one background check that handles app updates and **driver updates** (display and bus modules). Drivers stay on the
  official/community-reviewed tiers only and keep the ABI-version refusal already in place. Folded into this plan, not a new plan file.
Open: key revocation (a published list the store checks), and whether driver updates need a second confirmation on the device.

### Driver compatibility and the update page (owner, 2026-10-28)
- **Driver ABI as a range.** A driver declares the lowest and highest ABI it works with (`abi_min`, `abi_max`); the OS runs it when its own bus/ABI
  version falls inside that range (today the check is exact: a driver built for another ABI is refused). The driver's own version is independent of the ABI.
  If a driver breaks on a newer OS, the repo index marks that driver version as bad (flagged, not deleted), the update service stops offering it and
  offers the fixed version when there is one. The index entry carries the ABI range and a `blocked` list per OS version.
- **Settings > Devices (and a general Updates page):** software updates, driver updates, and "update from a file": the user gives the loader a
  pre-downloaded image to write. Scope rule: that path may write **only the kernel slot** (where the core OS lives). It never writes the bootloader,
  boot package, KittenOS (recovery) or the loader. The recovery and loader are updated only by the loader itself, on purpose, with its own
  signed-image checks. All images are still checked against the boot key before anything is written.
Open: how a driver's min/max is declared (module header fields vs manifest), and whether KittenOS may update itself in-place from the update page
(the owner says recovery is updated by the loader only, so no).

### Driver foundation + "Find hardware" (owner, 2026-10-28)
Goal: flash a base system on an ESP32 board whose only working part is the chip, then add drivers as time goes on, and update drivers independently of the OS.
Decided by the owner:
- Developer mode accepts **unsigned drivers** (and unsigned keys) as well as unsigned apps. Built 2026-10-28: `load_relocatable_image` (commands.c) loads an unsigned driver from
  `/boot/drivers` when developer mode is active and the "Allow unsigned apps and drivers" switch is on; the payload hash is still checked; revoked keys and DEV_LOCKED still win.
- **Updates panel > "Find hardware"**: one button. It scans without asking questions where it can, downloads the drivers for what it found, and maps each driver to the
  pins it found the hardware on. A guided mode (it asks before driving any output) stays for what a passive scan cannot find.
- Drivers are installed and updated on their own, separate from the OS (driver ABI range, plan8 section above).
Design (to build, in this order):
1. **Hardware report** (read-only, no outputs driven): board fingerprint match against the board database (power-enable pin first, e.g. T-Deck GPIO10), then per bus:
   I2C pins found by pull-up sensing on non-reserved GPIOs (never flash/PSRAM/USB/strapping pins), address scan + ID-register match; known SPI pin maps from the
   database with the read-ID command per chip (ST7789 RDDID, SX1262 GetStatus, W5500 version register, SD CMD0); UART listen at common baud rates. Output: a list of
   {device, bus, pins, confidence}.
2. **Device database** in the repo (signed, versioned): chip ID signatures -> driver name, compatible, bus, default pins; includes power-enable pins of known boards.
3. **Install**: for each match, fetch the driver from the index (ABI range checked, signature/tier shown; unsigned only in dev mode), write `/boot/drivers/<name>.cat`, write the
   pins into the **discovered board profile** (`/etc/board.conf`), load. The registry's pin-conflict rule still applies.
4. **Settings > Updates**: Find hardware, Drivers (installed, versions, update each), Software. The recovery client's `--dev` role serves unsigned/dev drivers
   (built 2026-10-28: `broken-kitten-recovery add-driver` + `serve --dev`, drivers.manifest with `type=driver`) for the developer's board bring-up.
Safety: passive first; anything that drives an output only on pins that appear in some known board's map and are not reserved, or after the owner confirms; a profile is saved only
after the owner confirms it once.
Cannot be found: parts wired straight to a pin with no bus and no known map (the owner maps them by hand: guided mode, or a hand-written board.conf shared back to the repo).

### Step 1 built: the hardware report (2026-10-28)
`components/coreos/src/purr_hwscan.c` (pure: reserved pins on the S3, candidate pins, an I2C address + ID-register table of ~25 chips; host test `test_hwscan.c`, 67 checks pass)
and `main/purr_hwscan_plat.c` (the part that touches pins). Shell command `hwscan [power <gpio>]`, full profile only. It prints what the board profile already set up, then senses
free pins as inputs with a weak pull-down (a pin that still reads high has a pull-up), tries each pair of pulled-up pins as SDA/SCL on I2C port 1 (the kernel uses port 0),
probes 0x08-0x77 and identifies each hit (ID register first, address as a labelled guess). Never touches reserved pins (straps 0/3/45/46, USB 19/20, flash/PSRAM 26-37), pins the board
profile names, or pins the registry has claimed. `power <gpio>` drives that one pin high first, only because the owner named it.
Built for vmfull and stdrec. NOT run: the VM's base image has no account so its text shell asks for one first, and the VM probably does not model pull-ups; needs the T-Deck.
Next: a Settings "Find hardware" button that calls `purr_hwscan_run`; the device database and driver install (steps 2-4); SPI/UART in the report.
Caveat to check on hardware: a free pin may be an output of another chip (LoRa BUSY/DIO1 on the T-Deck); sensing it as an input with a weak pull-down is harmless, but the I2C pair
probe briefly pulls both pins low as open-drain, which is the one thing that is not strictly passive.

### Steps 2-4 built (2026-10-28); to test on the T-Deck in one go
Built, compiled in vmfull/stdfull/stdrec/stdmin (stdrec still 5% free), host-tested where there is pure logic (test_hwprofile 21, test_hwscan 67, test_manifest +abi fields,
test_driver +bind_extra; the Rust client 13). NOT run on a device or in the VM (the VM has no I2C sensor and no way to type into the W10M shell):
- Settings > **Updates**: Find hardware (scan, list), **Set up N devices** (downloads the driver from the index, checks size+hash against the index, loads it through the normal
  loader, adds the device to /etc/board.conf, binds it), **Drivers** list with versions and **Update drivers** (newer versions download, old kept as .bak, apply after a restart).
- `purr_hwsetup.c`: sources = official `drivers.manifest` next to the module index; plus, in developer mode, the dev server the owner chose (`<base>/recovery/drivers.manifest`).
- `purr_hwprofile` (pure), `purr_drivers_bind_extra`, `purr_driver_registered`; the kernel's I2C bus table now keys by pin pair (two buses); driver ABI range check.
- First driver: `Modules/drivers/bme280_module.c` (temperature/pressure/humidity, forced mode, Bosch integer compensation), built and signed with the developer key; in the 1.0.3 release as
  JumpingJaguar2/drivers.manifest + bme280-tdeck_plus.cat, and in out/devrepo/recovery for the dev server.
- Release 1.0.3 repacked (old copy: PURR-OS-release-1.0.3-prev2), out/hw/tdeck_plus-blank-flash.bin rebuilt 2026-10-28 14:26.
Test plan (one session): flash the image; let it restore; sign in; Settings > Updates > Find hardware; with a BME280 on free GPIOs (SDA/SCL with pull-ups) expect "BME280 sensor ... driver bme280",
then Set up; restart; "found by scan ... driver bme280" without "(not running)". Without a sensor expect "Nothing new answered". Check the log line "N free pins looked at" and that keyboard/touch still work after a scan.
Known gaps: the sensor has no UI/shell reader yet (read() returns text; nothing calls it); no SPI/UART discovery; the index source in secure mode is the official server only; updates need a restart.

### Add by hand + keyboard backlight driver (2026-10-28)
- Settings > Updates > "Add a device by hand" (driver name, SDA, SCL, address) and the shell `hwdev [add <driver> <sda> <scl> <addr> | read <name> | remove <name>]`: for hardware whose wiring the
  owner knows, or that sits on a bus the scan skips because the board already uses its pins. Fetches the driver if needed (same index, same checks), writes /etc/board.conf, binds. Tapping a
  discovered device on the Updates page asks its driver for a reading (`purr_hwsetup_read_text`).
- `Modules/drivers/kbd_backlight_module.c` (kbd-backlight 1.0.0, signed with the developer key, in the 1.0.3 drivers.manifest and out/devrepo): the T-Deck keyboard controller at 0x55 takes [0x01, level]
  to set the key backlight (from LilyGO's published keyboard firmware, NOT verified on this unit). Each read steps the level off/64/128/255 so a tap shows the effect. Test: Updates > Add a device by hand:
  `kbd-backlight`, 18, 8, `0x55` (or `hwdev add kbd-backlight 18 8 0x55`), then tap it. If the keys stay dark, the command is wrong for this controller revision: nothing else is affected.
- Fixed while here: the Updates page kept stale pointers to its lists after leaving the page; they are cleared on every page change now.
- The two drivers are in out/devrepo/recovery for the dev server and in ~/Desktop/PURR-OS-release-1.0.3/JumpingJaguar2 (previous copy: -prev3); out/hw/tdeck_plus-blank-flash.bin rebuilt.

### Device Manager, 2026-10-29
Find hardware, Add by hand, Drivers (update), the developer server and System update/Reinstall moved from Settings to the **Device Manager** app (`Apps/devices/`), through the `devices` catcall
(`purr_catrt_devices.c`: one worker job at a time, `manage-devices` permission, administrators only). Phase 5 (the store backend and "main repo") is still open: it needs the owner's decisions listed in the
session notes (where the main repo lives, who signs its index, whether the store is an app or part of the shell).

### The Store and repositories, 2026-10-29 (owner decisions in `Repositories/SPEC.md`)
Decided by the owner: a separate GitHub repository of pre-compiled apps, F-Droid style; anyone can run a repository and add others; whoever runs one signs it, and the device trusts the key
in the repository's release file (`repo.json`); untrusted needs developer mode; the Store comes pre-installed and updates itself; a companion app handles installs, updates, assets and extras
(flash or SD). The official repository is the release **JumpingJaguar3** (placeholder address in `purr_repo.h`; the owner will test trusted apps from it).
Built (compile-checked in all three profiles, host-tested, **not run on a device**): `purr_repo_trust` (release file, signatures, endorsement tiers, trusted list, format-2 index with assets and
extras; cross-checked against the Python tooling), `purr_store.c` (the engine, the `store` catcall, the `store` shell command, receipts and the loader's vouch rule, seeding), the Store app (`Apps/store`),
its companion (`Apps/storesvc`, timers; becomes the `[service]` entry when services exist), `purr repo` and Studio's repository tool, `tools/release/make_jj3.sh`, `sdk/docs/11-repositories.md`.
The Store and its companion are **built into the firmware** (`tools/release/gen_builtin_apps.py`, regenerate when they change) and installed for each account at first sign-in.
Not decided / still open: the repository key and its passphrase (the owner makes it: `tools/release/make_jj3.sh`), endorsing it with the boot key, revocation, bundles, SD card driver.

### The SD card, 2026-10-29
- **Driver** `sd-spi` 1.0.0 (`Modules/drivers/sd_spi_module.c`, class BLOCK, bus ABI 7): SD/SDHC/SDXC in SPI mode, started lazily (400 kHz, then 20 MHz), chip select by hand, bus lock per conversation. Host-tested against `FakeSdCard`, a byte-level model of the card
  (`sdk/tests/test_drivers_host.py`, 16 tests: SDHC and SDSC, single and multi-block reads and writes, bounds, no card, card put back, 3.3 V refusal, lock balance, CS discipline). **Not run on the unit.**
- **Kernel**: `spi_lock/spi_unlock` in the bus table (a mutex `purr_kernel.c` also takes in `spi_transfer`), BLOCK class and `purr_block_ops_t`.
- **OS**: `main/purr_sd.c` mounts FAT at `/sd` (FatFs diskio over the block ops, LFN on the heap: `CONFIG_FATFS_LFN_HEAP` in sdkconfig.defaults), lazily and with a 3 s retry; shell `sd [status|mount|unmount|ls|test]`
  (`sd test` writes 64 KB, reads it back, compares, times it). The Store's engine goes through `x_read/x_write/...` which pick flash or `/sd` by path. Board device `sd0` (cs 39) in `tdeck_plus.c` and `tdeck_plus.toml`.
- **Getting the driver**: `sd-spi` is not baked into the firmware. Device Manager > Drivers > "Get N missing drivers" (job `get-drivers`, `hwdev get`) downloads the driver for each built-in device that has none, from the official
  server or the dev server, and `sd0` binds at once. It is in `out/devrepo/recovery/drivers.manifest` (unsigned: developer mode only; sign it with the developer key for the official server).
- FAT12/16/32 only (no exFAT in ESP-IDF's FatFs): a 64 GB+ card must be formatted FAT32. Files app does not browse `/sd` yet.

### Planned: the companion installs missing drivers by itself (owner, 2026-11-05)
Owner: once the device is installed and ready, it should check itself and fetch any missing drivers that are online, as part of the Store's companion. Pieces that exist: `purr_hwsetup_get_missing()` (devices catcall job `get-drivers`: downloads the driver of every built-in device that has none, checked like any driver, from the official server or the dev server in developer mode) and `info("missing")`. Nothing calls it by itself today; only Device Manager does, when asked.
Design: an OS-side Store job `drivers` (`purr_store.c`, next to `check`), so the companion only decides when (the same split as updates) and needs no devices permission; a setting `store.auto_drivers` (admin only, default on) beside `store.companion`; the companion runs it after its update check (first look about 90 s after it starts, then every `store.check_hours`) and posts a notification with what it got or what it skipped. Guards: only when `missing` is above 0; skip a driver when fast RAM would drop under the app reserve (the LoRa driver is the large one) and say so instead of loading it; never in the middle of another Store job. Catch: the companion is off by default (it holds about 10 KB of fast RAM), so on a fresh device the pass would not run; run it once from the Store's first sign-in setup as well (no app task needed). Not built yet.

### The apps repository (owner, 2026-11-05)
The owner is creating a new repository for the apps and will give this session access. The plan for moving the apps and publishing them there is in `showtime1.md` ("The apps repository").

## The hub: one repository for OTAs, apps, games and channels (owner decisions, 2026-11-06)

The owner's repository **PURR-OS-HUB** (`https://github.com/PastorCatto/PURR-OS-HUB`, public, MIT; today only a LICENSE and a README, its description still says "drivers") becomes the place everything is served from: the OS images (OTA), apps, games, drivers and extras, in **channels**. It replaces the apps repository of the previous section. The release process moves to it at **1.1**: the proper release is the first to be served from the hub.

**What goes where.** Small signed metadata (manifests, indexes, `hub.json`, the docs, CI checks) is in git. The big files (kernel, loader, KittenOS, app and game packages) are GitHub release assets or the Pages site, never committed. Signing stays offline on the owner's computer; CI only verifies hashes and signatures against public keys and never holds the boot key or any passphrase. GitHub is the host **for now, not forever**: the address the firmware uses is meant to become an address the owner controls, so the host can change.

**Layout (draft).** `hub.json` (signed list of channels: id, base URL, trust rule, floor rule), `os/stable|beta|dev/` (the three manifests), and one repository each (own `repo.json` and `index.json` with signatures) for `apps/`, `games/`, `drivers/`, `extras/`. A **game** is an app of kind "game" (the `game` permission exists); a separate catalogue lets third parties run theirs under their own key and rules.

**Who does the work.** The Store service (the companion) does the checks: the OS update check, the app and game updates, the hub's channel list. **The install of OS images stays in the recovery loader and KittenOS**, which keep their own copy of the source, so a broken OS can still repair itself. The Store service is the hub's client for now; that is a choice for now, not for good.

**Third-party apps and games** arrive as **pull requests** that add an index entry (source link, commit, package address, sha256). CI checks the hash and the permissions asked for; the owner merges and signs the index.

**Legacy.** Only the owner's development devices (and possibly one device sent to a friend) have the JumpingJaguar addresses compiled in; they keep working and are not the focus. **There is no bridge release** (owner, 2026-11-06): 1.0.28 is rebuilt with the move mechanism below, because it has not been installed or uploaded; the proper release, 1.1, is served from the hub.

### Moving the OTA address (built into 1.0.28)

The recovery addresses were compile-time constants (`CONFIG_PURR_RECOVERY_MANIFEST_URL`, `CONFIG_PURR_MODULE_INDEX_URL`), each image (kernel, loader, KittenOS) carrying its own copy. Now:
1. **A stored official source** (two manifest addresses, a sequence number) in the `netrec` partition next to the network record and the dev-server record (so the loader, which has no file system, can read it). `purr_recovery_manifest_url()`, `_modules_url()` and `_legal_url()` use, in order: a chosen dev server, the stored source, the compiled default.
2. **A signed redirect file**, `moved.txt` with `moved.txt.sig` (ECDSA P-256 over SHA-256, 128 hex digits, as the repository files), next to the manifest the device is asking. It is `key=value` lines: `moved=1`, `seq`, `key_id`, `from` (the exact manifest address being asked), `recovery`, `modules`, `message`. The device follows it only if the signature verifies against a key in its bag with the boot role (not revoked), `from` equals the address it asked, `seq` is above the stored one, both new addresses are https and end in `/recovery.manifest` and `/modules.manifest`. A file naming the compiled default clears the stored source (a way back).
3. **When:** before an OS update check (kernel) and before the loader fetches its manifest, only when no dev server is in use. A move shows a notice with the file's message, and Settings > Updates shows where updates come from.
4. **Safety:** a redirect cannot install code: images are verified against the boot key from any host, and the floors still apply. A wrong or hostile host can only withhold updates, which the way back and the signed file cover.
Tooling: `purr repo move` (the SDK, `sdk/purrsdk/repo.py`) makes and signs the file. **Built and checked in 1.0.28 (2026-11-06):** the host test `test_move.c` (31 checks, with files signed by the Python tool), and in the VM the recovery loader fetched a signed `moved.txt`, followed it ("recsrc: followed move 1") and, on the next boot, used the stored source and did not ask the old address again. Not run: the kernel's update check (`purr_sysupdate_check`) and the Settings page (compiled, not seen); not run on a device. Tests: host tests for the parse and the rules (including a Python-signed fixture); a VM check against the dev server. Not run on a device.

### Themes as a Store item (owner, 2026-11-06; for 1.0.29)
The repository format gets a fourth item type, `theme` (next to app, asset and extra): the shell it is for (`w10m`, `android` or `any`), the package size, a licence shown before installing, a preview picture, `target` flash (one of three theme slots in LittleFS) or SD. The Store lists themes in their own category; installing one into a full set of slots asks which theme to replace. How the device keeps and switches them: [plan7.md](plan7.md) ("Themes from the Store, three at a time"). Nothing is built.

### The hub, as decided with the owner (2026-11-06 night; designed, nothing built or signed)

Decisions: **(1) Hosting:** a Pages deploy branch. `main` holds only small, reviewable files; CI builds a static site (tree layout, the same one `purrstrap dev serve` makes) onto a deploy branch whose history is squashed on every deploy, so the repository does not grow with every image. The address devices use becomes one the owner controls later (a custom domain on the same site); nothing on a device changes when it does. **(2) Keys first:** the owner chose **"rotate first"**: the key rotation (plan3) comes before anything signed is built; the hub's folders, documents, schemas and CI are written now, but `keys/`, `hub.json` and every signature wait for the new keys. **(3) UI items:** official **shells in a repository `ui`** (signed by the hub key only), **themes in `extras`**; a third party's shells and themes live in the author's own repository, which a user adds, and a locked device accepts only project-endorsed shells. **(4) Submissions:** third-party apps and games arrive as **pull requests that add an index entry** (source link, commit, package address, sha256); CI checks the hash and the permissions asked for; the owner merges and signs. Never shells: those are native code.

**Proposed (not yet decided): the hub key takes the device's existing `PURR_ROLE_SYSTEM`** (the role that already signs modules, the apps repository index and picture packs; shells already verify for it, `purr_role_may_sign()`), so no firmware change is needed for it; the nightly key is the new role plan3 already designs.

**Layout of PURR-OS-HUB:** `README.md`, `LICENSE`, `HUB.md` (how it works); `hub.json` and `hub.json.sig` (the channel list); `keys/` (public keys and `KEYS.md`, the roles, fingerprints and rotation log; no private key ever); `os/stable|beta|dev/` (`recovery.manifest`, `modules.manifest`, `drivers.manifest`, `withdrawn.list`, `moved.txt` and its `.sig`: metadata, the images are on the site); `repos/apps`, `games`, `drivers`, `extras` (themes, fonts, sounds), `ui` (shells), each with `repo.json`, `index.json` and their signatures; `.github/PULL_REQUEST_TEMPLATE.md`, `submissions/` (entry schema, an example); `docs/`; later `ci/` and `tools/`. Handed to the hub session as files to commit (documentation, schemas and templates only).

**Order:** key rotation (plan3, phases 1 to 7, the owner's passphrases one command at a time, cable flashes of the owner's devices) -> `keys/` and `hub.json` -> the site builder and the checks (`tools/make_site.py`, `tools/verify_hub.py`, reusing `sdk/purrsdk/repo.py` and `tools/release/`) -> the first signed hub -> firmware 1.1 defaulting to it. Shells: the Windows shell is the first item of `repos/ui`.

**Hub hand-off, batches 1 and 2 (2026-11-07):** the hub session (`purr-os-hub-78`) received the five public keys and `keys/KEYS.md` (committed there, 0dad6b4 and 0255c77, not pushed; fingerprints re-checked, all five are valid P-256 points) and then the first item for `repos/ui`: the Windows 10 Mobile style shell `w10m` 1.0.32, built against the 1.0.32 kernel with its own pack (436,085 bytes, sha256 `960889df05c985fb98d489f6214dc3e10a9513de466550c0007ae1644c6da862`), **signed with the new hub key, id 5 (role SYSTEM)**, verified with its public half. The package and its `.toml` item description are in `out/hub-handoff/ui/packages/` (ignored, not in either git tree; the hub keeps only the small text). The owner allowed reusing the passphrases they typed in chat on condition that each signing names its key id. **Open decision for batch 3:** how the hub's indexes are signed. The SDK's `repo build` signs `repo.json`/`index.json` with the repository's own key (`keys/repo.key`) and a project key endorses it (`repo endorse`); the hub design said the hub key signs the indexes. Either make the hub key the repository key of each official repository (id 5 as `key.pub`, which a device already knows as SYSTEM) or keep a repository key per folder endorsed by the hub key. Not decided; nothing is signed except the shell package.

**Hub simplified (owner, 2026-11-07): the hub is one plain GitHub repository the Store points at; no release assets, no deploy branch, no site builder.** Supersedes the earlier "Pages deploy branch with squashed history" and "GitHub release asset" ideas. Reason (owner): the aim was always a repository that is pointed at, so that updating the repository updates the Store; a release asset would need a manual step for every addition; a pull request should put an app straight into our repository; the hub is a pre-built set of binaries for everything about the system, in one place, without layers.
- **Layout:** each `repos/<kind>/` folder is a *flat* repository (SPEC section 2): `repo.json`, `repo.json.sig`, `index.json`, `index.json.sig` and the packages next to them (`w10m-1.0.32.cat`). The base URL a device uses is the raw address of that folder on `main` (`https://raw.githubusercontent.com/PastorCatto/PURR-OS-HUB/main/repos/ui`); `os/<channel>/` is the same for the OS images. `hub.json` lists the repositories and channels.
- **Everything is committed in the repository, binaries included.** Size is not a problem at this scale (a shell is under 1 MB, an app is tens of KB, an OS set is about 6 MB per release); keep the current and the previous version of each item, remove older ones in the same commit, and squash history only if the repository ever gets heavy.
- **Signing stays offline and is the only manual step:** a pull request adds the package and a small item description (`items/<name>.toml`); CI only checks it (no secrets, the hub key and every passphrase never go to CI: size caps, format, sha256, the type rules, not signed by anything it must not be); the owner merges, runs ONE local command that rebuilds and signs `index.json` with the hub key (id 5), and pushes. A merged item is not offered to any device until that signed index is pushed, which is the review gate.
- **Third-party apps** come as packages in the pull request (built from a named source and commit, unprivileged, vouched for by the signed index, never trusted by author key); third-party shells and anything privileged still live in the author's own repository.
- **Device side to do later:** the official repository address (`PURR_REPO_OFFICIAL_URL`) moves from the JumpingJaguar3 release to the hub's raw address; the SDK `repo build` learns to sign with the hub key directly (hub key as the repository key, endorsement by a key the device already has).

**Hub signing tool (owner, 2026-11-07): the tools are committed, only the key is ignored, so anyone can fork the store.** `PURR-OS-HUB/tools/hubsign.py` is one self-contained file (Python 3.9+, `cryptography`; no dependency on this repository): `keygen`, `check [--strict]` (no key; for CI) and `build` (checks, then writes and signs `repo.json` and `index.json` per `repos/<kind>/`; building twice changes nothing). Each `repos/<kind>/repo.toml` holds the repository's name, maintainer, url, min_os and `endorse_key_id` (5 for the official hub: a device checks the endorsement against its own copy of the key by id, with no role test, so the hub key endorses itself and the repository is "official"; a fork deletes that line and is a "community" repository). The hub's `.gitignore` ignores `signing_keys/`, `*.key` and `*.pem` only. Tests: 11 self-tests (`tools/test_hubsign.py`, throwaway key); the real `w10m` package built by this tool and by the SDK's `repo build` gives byte-identical `repo.json` and `index.json`, and the SDK's verifier accepts it. Batch 3 handed the tool, `repos/ui/repo.toml` and the package (`repos/ui/w10m-1.0.32.cat`) to the hub session; the item records the firmware commit `07d79cd6e4f80af42ab39ee798ad3caa5f195db8`. **The owner signs:** `python3 tools/hubsign.py build` with the hub key (id 5) in `signing_keys/hub.key` or given by `--key`.
- **To do, in this order (owner: "once the store itself is slightly built, add our official URL to the store app"):** (1) the owner signs and pushes the first index; (2) check the raw address serves it (`purr repo verify <raw address>` from the SDK); (3) only then change `PURR_REPO_OFFICIAL_URL` in `purr_repo.h` to the hub's raw address (`https://raw.githubusercontent.com/PastorCatto/PURR-OS-HUB/main/repos/ui` for shells, one per kind) and make the Store list the hub's repositories; this is a kernel change, so it ships as the next number (1.0.33) and 1.0.32 stays as published. Not done: the device-side address, a repository list in `hub.json`, the other repository folders.

**Hub signed for the first time (owner, 2026-11-07): `repos/ui` is signed with the hub key (id 5) and verifies as official** (SDK verifier: endorsed by key 5, system role; the firmware's own index parser, run in a host harness, lists `w10m` 1.0.32 as a vouched shell, none dropped). Run by me at the owner's request with the IDF Python (the system Python has no `cryptography`), `--key` pointing at the key where it already is. Nothing is pushed; the hub session commits.
- **Found by running the device parser:** `purr_repo.c` drops a whole index entry if any text field is longer than its buffer (`desc` 79, `title` 31, `category` 15, `min_os` 11, `dest` 63, `chip` 11, a permission 23, `version` 15 characters and no underscore). The first signed index had a 144-character description and would have shown no shell on any device. `hubsign.py check` now enforces these limits (13 self-tests). The shell's description is cut to one line; the rest goes on the details page.
- **Owner decision, details page (answer to "should the Store show a licence before installing?"): no licence prompt before installing, but tapping an item opens an about/details page** with who made it, who licensed it and so on. Design (to do with the next kernel, 1.0.33, together with the official address): the index stays small (24 KB cap, 80-byte `desc`, and every entry sits in device RAM), so the details are one small signed-by-hash file per item, fetched only when the page is opened: the entry gets `about` (a relative path, `<name>-<version>.about.json`) and `about_sha256`; the device checks the hash before showing anything, so the signed index vouches for the text. The about file holds `author`, `licence` (a short name such as MIT, plus an optional address of the text), `summary` (longer than `desc`, a few hundred characters), `source`, `commit`, `homepage`, `changelog`, `perms` with a reason each, and for a shell the kernel it was built for. The Store shows it as a scrolling page with the install button; it is shown from the hub's own text, never run. Format goes into `Repositories/SPEC.md` section 3.2 before the code. Hub side: the contributor toml gains these fields (it already has `source`, `commit`, `licence`), `hubsign.py build` writes the about file next to the package and puts its hash in the entry, `check` limits the sizes. Not built; the hub session gets the field names when they are settled.

**First official apps in the hub (owner, 2026-11-07): `repos/apps` with devices 1.2.0, clock 1.0.0, notes 1.1.0 and messages 0.1.2** (owner: devices "handles some of the critical parts of the OS", messages so that updates can be pushed later; "it should just be a copy paste from the dev server and a re-sign"). They are the dev server's own packages (payloads re-checked by rebuilding each from source at 07d79cd6: identical), re-signed with the developer key **id 4** (verified with its public half), and the repository index is signed with the hub key **id 5**. Verified as official by the SDK, and the firmware's index parser lists all four as vouched apps, none dropped. Licence in the items: GPL-3.0 (README: the project is GPL v3; only the Android shell is Apache 2.0). Not included: contacts (messages uses the contacts service of the OS; its app can follow), the demo apps, the Store and its companion (they are part of the Store itself).
- **Update flow for an app:** `PURRSTRAP_KEY_PASSWORD=... tools/vm/hub_app.sh messages` (rebuild, sign with the developer key id 4, drop the package and set version and commit in `repos/apps/items/messages.toml`, keep the current and previous version), then in the hub `python3 tools/hubsign.py build` (hub key id 5) and commit and push. The Store then offers the update because the signed index changed. Tested on a copy of the hub: same payload, item updated.
- **Trust note:** a project app is signed by the developer key (id 4) and also vouched for by the hub's signed index. `devices` is an ordinary unprivileged app that asks the administrator for `manage-devices`; the signature does not grant that permission, the administrator's answer does.
- **Device side still to do (1.0.33):** the official address in the Store, `about` details page, and the Store must read the hub's `repos/apps` and `repos/ui` repositories (one address each).

**Licences are per item (owner, 2026-11-07).** The project is a mix: the Windows 10 Mobile style shell can be MIT (ported from an MIT reference project, `docs/11`), the Android-style shell and its art are Apache 2.0, other parts GPL 2 or GPL 3, and more; the README Licence section and `THIRD_PARTY.md` list what is recorded. The hub's `licence` is therefore a per-item field and is never a repository-wide default. Correction to the note above: the four apps (devices, clock, notes, messages) carry no licence line in their source, so `GPL-3.0` in their items is a placeholder from the README's sentence about "the rest of the project", **to be confirmed by the owner per app**; the `w10m` shell item has no licence yet (proposal: MIT, to be confirmed). The Apache licence is version 2.0.

**Licences confirmed (owner, 2026-11-07):** the four apps (devices, clock, notes, messages) are GPL (`GPL-3.0` in their items, the version the README names); the Windows 10 Mobile style shell follows its MIT licence (`licence = "MIT"` in `repos/ui/items/w10m.toml`). The field is for the reviewer and the future details page; it is not in the signed index yet, so nothing was re-signed.

**Hub pushed and verified live (owner pushed, 2026-11-07):** `main` of `PastorCatto/PURR-OS-HUB` is at `0f8b81a` locally and on GitHub (`git ls-remote` equal). Fetched from `https://raw.githubusercontent.com/PastorCatto/PURR-OS-HUB/main/repos/<ui|apps>`: the SDK verifier says **official** (endorsed by key 5) with every package's size and sha256 matching (1 shell, 4 apps), and the firmware's own index parser accepts both live indexes (1 and 4 entries, none dropped). Raw GitHub caches for a few minutes, so a new push can take that long to show. Next: OS 1.0.33 (the Store reads these two addresses as the official repositories, plus the details page).

**The hub is ONE official repository, `repos/official/` (found while starting OS 1.0.33, 2026-11-07).** A device identifies a repository by its key fingerprint (`purr_repos_t` is keyed by `id`, a stanza whose id is not its key's fingerprint is dropped, `/var/store/<id>/` holds its index), so the planned `repos/ui` and `repos/apps`, both signed with the hub key, would have been the same repository on a device, the second overwriting the first. Fix, in line with the owner's "all in one place without layers": one folder, one index, items told apart by `type` (app, shell, later theme) and `category`; supersedes the per-kind folders in the hub layout above (`repos/ui` for shells, `repos/extras` for themes). Signed with the hub key (id 5); verified official; the firmware's index parser lists all 5 items (4 apps, 1 shell), none dropped. A further repository (a different key, endorsed by the hub key) can still be added later if the device's limits are reached: **32 items per repository, 48 in the Store, 24 KB per index.** Device base address: `https://raw.githubusercontent.com/PastorCatto/PURR-OS-HUB/main/repos/official` (77 characters; the device keeps 95). Not yet pushed in this shape: the hub session commits, the owner pushes. Not tested: that the T-Deck's TLS certificate store trusts `raw.githubusercontent.com` (the previous placeholder was a GitHub release address).

**OS 1.0.33, step 1 done in the working tree (2026-11-07; not published, the owner has not OTA'd past 1.0.30 and asked for nothing on the dev server):** `PURR_REPO_OFFICIAL_URL` is the hub's raw address (`.../PURR-OS-HUB/main/repos/official`); `purr_store_bootstrap()` adds the official repository whenever the list does not hold it (before: only when the list was empty, so the owner's device, which has the dev server in its list, would never have got it); the Store's detail page names themes and shells (it said "Extra"). Compiles in a scratch kernel build (`andfull`) and as a Store app package. **Correction to the note above:** the live check (official, hashes equal) was run on the earlier `repos/ui` + `repos/apps` layout; `repos/official` is committed in the hub but NOT on GitHub until the owner pushes again, so its raw address does not exist yet. **Step 2, still to do:** the details page (the `about` file: the entry gains `about` and `about_sha256`, fetched when the page opens, hash-checked, shown by the Store; `Repositories/SPEC.md` first, then `purr_repo.c`, `purr_store.c`, a new store catcall function, `Apps/store/store.c`, `hubsign.py build` writes the about files, host tests).

**OS 1.0.33 published to the dev server for OTA bench testing (owner, 2026-11-07).** The owner is on 1.0.32 now, so the kernel, KittenOS and loader are 1.0.33, signed with the NEW boot key (id 3) (the 1.0.32 images are kept in `out/ota/recovery-1.0.32`); 25 files checked against the live server, 0 mismatches. Content: step 1 only (official address, bootstrap adds it when missing, theme and shell labels); the details page is not in it and becomes 1.0.34. Not tested on a device: the Store fetching from `raw.githubusercontent.com` (TLS trust of that host).
- **Hub packages may sit in subfolders (owner, 2026-11-07: "can't it look through subfolders?").** The device already accepts a `/` in an entry's `url` (`rel_ok`; no leading `/` or `.`, no `..`, no `//`, at most 95 characters) and joins it to the repository's address, so no firmware change. `hubsign.py` now searches the folder and its subfolders (not `items/`, not hidden folders) and writes the relative path into `url` (`apps/clock-1.0.0.cat`, `shells/w10m-1.0.32.cat`); the index, signatures and `repo.toml` stay at the root. `check` compares name, version, sha256 AND url, so a moved package makes a stale index fail (17 self-tests). `tools/vm/hub_app.sh` writes to `repos/official/apps/` and reads `repos/official/items/`. The hub index was rebuilt with the hub key (id 5) after the move: official, 5 items, every package at its indexed path matches size and sha256, the firmware's parser keeps all 5. The hub session commits; the owner decides when it is pushed.
- **Drivers in the hub (owner, 2026-11-07, later):** a driver is not a Store item (`hubsign.py` refuses modules and drivers, and the device reads drivers through `drivers.manifest`, made by `tools/release/make_drivers_index.py`). So they get their own folder in the same hub (a separate index, technically a separate repository, same GitHub repo), and the Devices app's driver server address would point at it. Not started; the owner wants the app downloads tested first.

**Hub pushed and verified live in the single-repository shape (owner told the hub session to push, 2026-11-07):** origin `main` of `PastorCatto/PURR-OS-HUB` is at `2c9427b`. Fetched from `https://raw.githubusercontent.com/PastorCatto/PURR-OS-HUB/main/repos/official`: the SDK verifier says official (id `adffd25c4d51651a`, endorsed by key 5, 5 apps), the firmware's index parser keeps all 5 entries, and the 5 packages (`apps/clock`, `devices`, `messages`, `notes`, `shells/w10m`) match size and sha256 at their indexed paths. Still untested: that the T-Deck's TLS store trusts `raw.githubusercontent.com` (the OTA test of 1.0.33 will show it).

**Bench test of 1.0.33 on the T-Deck (owner, 2026-11-07): the Store works as expected** (it reads the hub's `repos/official`, so the device does reach `raw.githubusercontent.com`). The owner finds the layout "a little whatever", probably button placement; to fix in 1.0.34 with the details page. Note: after a full reset the device reinstalls the OS from the OFFICIAL release (the developer server setting is wiped), and the old OS only knows the old keys, so the dev server must carry a set signed by the OLD boot key (id 1) until the device has 1.0.32 or later; 1.0.33 on the dev server is signed with key id 1 for that reason (the key-3 set is in `out/ota/recovery-1.0.33-newkey`).

**Bugs found while bench-testing 1.0.33 (owner, 2026-11-07), fixed in the working tree for 1.0.34, compiled in scratch, NOT run on a device and NOT published:** (1) after a Store install, update or remove the shell's app list did not follow (the Store's jobs never called `w10m_apps_changed_post()`); (2) tapping the icon of a running app started a second copy (both launchers sent `open`; they now send `focus` when `purr_catrt_slot_of()` says it runs; the W10M shell module must be rebuilt and re-signed for its half); (3) downloaded drivers were set aside after an OTA (suspected: `quarantine_all_drivers()` sees `boot_fail_count >= 2` after the several restarts of an update; cause not confirmed from a log). Changes for (3): `boot_fail_count` is cleared before the swap restart; after sign-in, once the 30 s settle window has closed, `recover_drivers_task()` puts the set-aside drivers back ONE AT A TIME with no restart (move back, `purr_driver_load_file()`, `purr_hwsetup_boot()`, 20 s of running before the next), posts "The system is still setting itself up in the background." and a summary; `/boot/drivers.retrying` names the driver being tried, so a start that follows a death blames it, sets it aside and lists it in `/boot/drivers.bad` (also every driver caught by the probe marker, e.g. the known-bad GPS one), and those are never put back by themselves. Open: whether a live-registered driver binds without a restart on every kind of device, and a log of the real cause.

**OS 1.0.34 published to the dev server (owner: "yes", 2026-11-07), signed with the NEW boot key (id 3)** because the device is on 1.0.33 and knows the five keys; 25 files checked against the live server, 0 mismatches; the 1.0.33 set (old-key bridge) is kept in `out/ota/recovery-1.0.33-final`. Contains: the shell's app list follows Store installs, a running app's icon focuses it, the one-at-a-time driver put-back after sign-in and the cleared fail count. Not in it: the details page (next), the W10M shell module rebuilt for the focus fix, the button layout of the Store. A device on an OS older than 1.0.32 cannot take this set (it does not know key 3): use `recovery-1.0.33-final` for that.

**The Store updates itself from the hub (owner, 2026-11-07; for OS 1.0.35, compiled in scratch, NOT run on a device, NOT published to the dev server).** Why: the kernel seeds the Store from its baked-in copy once per user (`.store-seeded`) and leaves an installed `package.cat` alone, so a new kernel never replaced the owner's Store. Now: (1) the Store (1.0.2, declares `ui 3.0`, Install/Remove side by side) is a hub item in its own folder, `repos/official/store/store-1.0.2.cat`, signed with the developer key (id 4), the index signed with the hub key (id 5); `tools/vm/hub_app.sh store` puts it there; verified: official, 6 items, the firmware's parser keeps all 6; the item has no `licence` line yet (the owner has not said which one the Store carries). (2) `purr_store.c` `self_update()`: only the OFFICIAL repository, installs a newer `store` and `storesvc` unless they are running; started by `self_update_task` ONLY while `store.auto_update` is on, a minute after sign-in and every `store.check_hours` (1-168, default 12), and not at all while the companion runs (it does the same); turning the switch on starts it, off ends it. `store.auto_update` is off by default, so by default a Store is updated by hand from the update list. Open: whether the Store can replace itself on a device (new code probably loads at its next start), a baked-in copy newer than the hub's is not touched, and the hub agent commits, the owner pushes.

**OS 1.0.35 published to the dev server (owner: "build and publish 35", 2026-11-07), signed with the NEW boot key (id 3);** 25 files checked against the live server, 0 mismatches; the 1.0.34 set is kept in `out/ota/recovery-1.0.34-final`. Contains the Store's side-by-side buttons (Store 1.0.2) and its self-update tied to `store.auto_update` / `store.check_hours`. The owner says the Store app is **GPL 2** (`licence = "GPL-2.0"` in its hub item) and will push the hub. To try the update: push the hub, turn on `store.auto_update` in Settings, wait a minute after signing in; the Store already on the device (1.0.1) should be replaced by the hub's 1.0.2.
