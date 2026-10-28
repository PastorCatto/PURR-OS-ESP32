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
