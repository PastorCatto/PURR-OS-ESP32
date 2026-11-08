# 14. The Store and repositories

## Short version

- A **repository** is a set of static files on any web host: a signed `repo.json`, a signed `index.json`, and packages. Whoever runs it signs it. The model is F-Droid.
- The **Store** (app) and its **companion** (`storesvc`, a background app) are built into the firmware. The OS's own **store engine** (`purr_store.c`, the `store` catcall) does every install.
- The official repository is the GitHub release **JumpingJaguar3**. Its address in `purr_repo.h` is a **placeholder** until that release is published.
- Make a repository with `purr repo init|add|build|verify` from the SDK kit; see [sdk/docs/11-repositories.md](../sdk/docs/11-repositories.md).
- Everything here is **compiled and host-tested, and not run on a device** ([plan8.md](../plan8.md)).

## What it is for

CLAUDE.md says apps are sideloaded: only Settings is pre-installed in the shell, apart from the Store and its companion. The repository system is how apps, assets (wallpapers, fonts, sound) and extras reach a device after that. Two kinds of repository exist:

| Kind | Used for | Network rule | Package rule | Index |
|---|---|---|---|---|
| **SDK** | A developer's own build, from `purrstrap dev serve` | Local network only | Signed by the developer key, or unsigned in developer mode | Not signed |
| **Store** | Published apps | https only, any public host | Signed, or vouched for by the repository | Signed |

## The files

| File | What it is |
|---|---|
| [Repositories/SPEC.md](../Repositories/SPEC.md) | The spec (draft 0.1, decided by the owner 2026-10-29) |
| [plan8.md](../plan8.md) | Plan: foundation, Developer Portal, Store, SD card, drivers |
| [purr_repo.h](../PurrOS/components/coreos/include/purr_repo.h), [purr_repo.c](../PurrOS/components/coreos/src/purr_repo.c) | Index parsing and per-kind policy; pure C, host-tested |
| [purr_repo_trust.c](../PurrOS/components/coreos/src/purr_repo_trust.c) | Release file, signatures, endorsement, tiers, trusted list |
| [purr_store.c](../PurrOS/main/purr_store.c) | The store engine: jobs, receipts, settings, seeding |
| [purr_repo_dev.c](../PurrOS/main/purr_repo_dev.c) | The SDK-server client behind the Developer Portal |
| [purr_w10m_devportal.c](../PurrOS/main/purr_w10m_devportal.c) | The Developer Portal pages |
| [purr_builtin_apps.c](../PurrOS/main/purr_builtin_apps.c) | The Store and companion packages, generated into the firmware |
| [tools/release/gen_builtin_apps.py](../tools/release/gen_builtin_apps.py) | Regenerates that file; run it when the Store apps change |
| [Apps/store/](../Apps/store/manifest.toml) | The Store app source |
| [Apps/storesvc/](../Apps/storesvc/manifest.toml) | The companion source |
| [tools/release/make_jj3.sh](../tools/release/make_jj3.sh) | Builds the JumpingJaguar3 release folder |
| [sdk/docs/11-repositories.md](../sdk/docs/11-repositories.md) | How to run your own repository |
| [PurrOS/components/coreos/test/host/test_repo.c](../PurrOS/components/coreos/test/host/test_repo.c), [test_repo_trust.c](../PurrOS/components/coreos/test/host/test_repo_trust.c) | Host tests |

## How it works

### Layout of a repository

Two layouts carry the same format. A client tries the flat one first.

| Layout | Used by | Files |
|---|---|---|
| Flat | A GitHub release (asset names are unique within a release) | `repo.json`, `repo.json.sig`, `index.json`, `index.json.sig`, `<name>-<version>.cat` |
| Tree | A plain web host; what `purrstrap dev serve` makes | `purr/v1/repo.json` ..., `purr/v1/apps/<name>/<version>/package.cat`, `assets/`, `extras/` |

The three project releases: JumpingJaguar1 is the recovery images, JumpingJaguar2 the drivers and modules, JumpingJaguar3 the apps. Signatures are raw ECDSA P-256 over SHA-256, 64 bytes, written as 128 hex digits.

### Trust tiers

| Tier | What it is | How a device reaches it |
|---|---|---|
| official | The repository's key is endorsed by a key in the device's key bag | Added at "Set up the Store" (administrators) |
| community | An administrator added it and confirmed its fingerprint | Typed address, read name and fingerprint, confirm |
| yours | A repository on the local network | The dev server |
| untrusted | Bad signature, changed key, unconfirmed address | **Developer mode must be on** |

A repository's `id` is the first 16 hex digits of the SHA-256 of its public key; the OS checks it, and it is what a person compares. A new key is a different repository: if `repo.json` carries a different key, the repository drops to untrusted until an administrator confirms the new fingerprint. The device keeps its list in `/etc/repos.conf` (administrators only).

### What the loader accepts

An app installed from a trusted repository lives in `/apps/store/<repository id>/<name>/` with a receipt (a copy of the signed index). The loader accepts it if its own signature verifies against the key bag, **or** the receipt's signature verifies against the repository's remembered key, that repository is still trusted and the package hash matches. The second path is the community path, and it always yields an **unprivileged** app: never `superuser`, never a driver or module. Drivers keep their own rules ([Drivers/SPEC.md](../Drivers/SPEC.md)). A signature proves who published something and that it is unchanged; it does not prove it is safe.

### The engine and its two clients

The store engine runs one job at a time. `start(what, a, b)` returns at once and, when the job ends, the app receives a `job` UI event with 0 (worked), 1 (failed) or 2 (worked and needs a confirmation). Jobs:

| Job | What it does |
|---|---|
| `repo-add` | Fetch and check a release file, hold it for confirmation |
| `repo-confirm` | Trust the pending repository |
| `repo-remove` | Forget a repository (its apps stay but are no longer vouched for) |
| `refresh` | Fetch and check signed indexes |
| `install` / `update-all` | Install an item, or every offered update |
| `remove` | Remove an item |
| `check` | Refresh, count updates, post a notification (what a schedule calls) |
| `bootstrap` | Add the official repository and install the Store and companion |

The Store app and the companion are two clients, so closing the Store never interrupts an install. Settings keys: `store.companion` (off by default; the companion holds about 10 KB of fast RAM), `store.auto_update` (default off), `store.check_hours` (default 12).

### The hub (`PURR-OS-HUB`)

The official repository is a plain GitHub repository, `PastorCatto/PURR-OS-HUB`, served from its raw address `https://raw.githubusercontent.com/PastorCatto/PURR-OS-HUB/main/repos/official` (`PURR_REPO_OFFICIAL_URL`). There is **one** official repository, because a device knows a repository by the fingerprint of its key and so two folders signed by one key would be the same repository. The layout:

```
repos/official/
  repo.json, repo.json.sig, index.json, index.json.sig    signed with the hub key (id 5), at the root
  repo.toml                                               name, maintainer, url, min_os, endorse_key_id = 5
  items/<name>.toml                                       what the Store shows: title, desc, category, perms, licence, source, commit
  apps/  store/  shells/                                  packages; the index entry's url is the path ("apps/clock-1.0.0.cat")
```

- A package may sit in a subfolder: the device accepts a `/` in an entry's `url` (no leading `/` or `.`, no `..`, no `//`, at most 95 characters) and joins it to the repository address.
- The index is built and signed offline by the owner with `tools/hubsign.py` (a single file; `keygen`, `check [--strict]`, `build`; building twice changes nothing). `check` also catches an entry whose package has moved. Only the signing key is ignored by git, so anyone can fork the hub and make their own store.
- The device's limits shape the items: 32 items per repository, 48 in the Store, 24 KB of index; title 31, description 79, category 15 characters; a version of at most 15 characters with no underscore. A too-long field makes the whole entry unusable, so the tool refuses it.
- A signed index vouches only for ordinary (`unprivileged`) apps. A shell must be signed by a key the device trusts for shells. The licence is a field of each item (the four hub apps GPL-3.0, the Store GPL-2.0, the Windows 10 Mobile style shell MIT); it is not in the signed index yet.
- Drivers are not Store items. They are read from a `drivers.manifest`, and are planned to get a folder of their own in the same repository.
- To update an app: `tools/vm/hub_app.sh <app>` builds and signs it with the developer key (id 4) and puts it in the hub; then `hubsign.py build` and a push.

### The Store keeps itself current

The kernel seeds the Store from its baked-in copy once per user and leaves an installed copy alone, so a new kernel never replaced it. Since 1.0.35 the Store is also a hub item (`store/`), and a kernel task installs a newer `store` or `storesvc` from the official repository only, a minute after sign-in and then every `store.check_hours`, but **only while `store.auto_update` is on** (off by default) and not while the companion runs. A running Store is left until the next sign-in. Not run on a device.

### The companion

`storesvc` is a daemon with no screen. It checks for updates every `store.check_hours` hours (the first check about 90 s after it starts), posts "n updates ready", and installs them if `store.auto_update` is on. Its one permission, `install-apps`, is granted by the Store's own seeding, because a background app cannot be asked. It will become the `[service]` entry of the Store's package when the service runtime exists ([plan5.md](../plan5.md)).

### Pre-installed

The Store and companion ride inside the kernel image, which the boot key checks, so they carry no signature of their own. At an account's first sign-in they are installed into its apps folder; a marker in the home folder makes removal stick. The official repository is **not** added silently: the Store's first page offers "Set up the Store", which adds the hub (also on a device that already lists other repositories).

### Assets and extras

An index entry has a `type` (`app`, `asset`, `extra`, `theme`, `shell`: a theme is one `pack.bin` up to 4 MB under `/ui/themes/<name>/`; a shell is one signed `shell.cat` up to 1 MB under `/ui/shells/<name>/` that carries its own pictures and is chosen in Settings, Interface), a `target` (`flash`, `sd`, `any`), a `dest` (no `..`, no absolute path), `needs_sd`, `source`, `perms`, `category`, `min_os`, and `removed`. Flash targets go to `/home/<user>/assets/<name>/<dest>`; SD targets to `/sd/purr/<name>/<dest>`. A download is refused if it would take the file system past 90 percent. Fonts are listed in the spec as assets, which is how Part D plans to deliver them ([chapter 12](12-picture-pack-and-fonts.md)).

### The SD card

The `sd-spi` driver (class BLOCK) and `purr_sd.c` mount FAT at `/sd`, lazily, with a 3 s retry. FAT12/16/32 only; a 64 GB or larger card must be formatted FAT32. The driver is not baked in: Device Manager's "Get missing drivers" downloads it. Host-tested against a byte-level fake card; **not run on a device**. The Files app does not browse `/sd` yet.

## How to build and test it

1. Make a repository: `bin/purr repo init myrepo --name "My Apps" --maintainer "me" --url "https://github.com/me/my-apps"`, then `bin/purr repo add myrepo build/clock.cat --title "Clock" --desc "Tells the time" --category tools --source https://github.com/me/clock --perms keep-awake`.
2. Build it: `bin/purr repo build myrepo` (flat, for a GitHub release) or add `--layout tree`. Check it: `bin/purr repo verify myrepo/out`.
3. The private key never goes into the repository. CI is given it as a secret, or the maintainer signs locally. The password is the maintainer's, entered per command; never written down.
4. For the official release use `tools/release/make_jj3.sh`. It makes the key the first time (it asks for a password), adds each app in `out/apps` and builds `out/JumpingJaguar3`. Upload every file in it as an asset of the release tag. Endorsing needs the boot key's password and is the owner's step.
5. After upload, fetch every file and compare size and SHA-256.
6. Host tests: `cd PurrOS/components/coreos/test/host && IDF_PATH=$HOME/esp/esp-idf-v5.3.5 python3 run.py repo`. Plan8 records 165 checks for the foundation library.
7. The SDK path (dev server and Developer Portal): [chapter 13](13-networking.md) and [chapter 17](17-sdk.md).

## What goes wrong

| Symptom | Cause | Fix |
|---|---|---|
| Install fails as a second user with "filesystem error" | The second user's apps folder missing or not writable | The owner decided on 2026-10-28: shared `/apps` for dev-mode apps, data per user; the owner reproduces it ([plan9.md](../plan9.md)) |
| Repository shows "this repository changed its key" | `repo.json` carries a different key | An administrator confirms the new fingerprint |
| Install refused: busy | One job at a time | Wait for the job |
| Download refused | File system would pass 90 percent | Free space |
| An asset says it needs an SD card | `needs_sd` and no card mounted | Insert a FAT32 card |
| Companion does not run | It is off by default | `settings set store.companion on` |

## What is not finished

- **Official repository:** it is live at the hub's raw address, signed by the hub key (id 5) and verified from that address (official, 6 items) on 2026-11-07. Not verified: that a T-Deck fetches it over TLS on every network. Revocation and bundles are open ([Repositories/SPEC.md](../Repositories/SPEC.md) section 10).
- **Drivers in the hub, the details page** (author, licence, longer text, version, size on tapping an item) and a **licence prompt** are not built; the owner decided there is no prompt before installing, only the page.
- **Companion installs missing drivers by itself** (`store.auto_drivers`): designed on 2026-11-05, not built ([plan8.md](../plan8.md)).
- **Plan status table is stale:** the table in plan8 lists item 5 (Store backend) as "todo" while the session log records the Store, engine and companion as built. The log is newer.
- Not verified on a device: the Store app, the companion, community-path installs, the SD card driver.
- Open: whether community apps may request the more dangerous permissions at all.

Next: [15 Power](15-power.md).
