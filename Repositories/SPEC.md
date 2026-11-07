# Repositories and the Store (spec draft 0.1, 2026-10-29)

Decided by the owner on 2026-10-29. Builds on plan8 (the repository client, the Developer Portal) and `Keys/SPEC.md`.

## 1. What it is

- A **repository** is a set of static files on any web host (a GitHub repository is the model). It lists apps, assets and extras, and holds their packages.
  Anyone can run one. There is one official repository, and a **separate GitHub repository that holds only apps** (pre-compiled, each linking to its source).
- Like F-Droid: people add more repositories, each one its own source with its own rules. **Whoever runs a repository signs it.**
- The **Store** is an app that browses repositories. It comes pre-installed (section 9). Its **companion service** does the work: installing, updating and removing
  apps, and fetching **assets and extras** (fonts, wallpapers, sound, data) that live in flash or on an SD card.
- Trust comes from the key a repository publishes in its **release file** (`repo.json`). The OS reads that file and decides how far to trust the repository (section 4).
  If a repository is not trusted, developer mode has to be on to use it.

## 2. Layout (all paths relative to the repository's base URL)

Two layouts, one format. A client tries the flat one first, then the tree.

**Flat** (a GitHub **release**: its assets are one list of files, and an asset name is unique inside a release). The base URL is the release's download address.
The official repository is the release **JumpingJaguar3** of the project's repository (JumpingJaguar1 is the recovery images, JumpingJaguar2 the drivers and modules):

```
<base>/repo.json            the release file: who runs it, its public key, its rules
<base>/repo.json.sig        detached signature of repo.json's exact bytes, by the repository's own key
<base>/index.json           what it offers (format 2, section 3)
<base>/index.json.sig       detached signature of index.json's exact bytes, by the repository's key
<base>/<name>-<version>.cat       an app package            (the index's "url" is just this file name)
<base>/<name>-<version>.<ext>     an asset or an extra
```

**Tree** (a plain web host, or a GitHub repository's raw files or Pages; also what `purrstrap dev serve` makes):

```
<base>/purr/v1/repo.json, repo.json.sig, index.json, index.json.sig
<base>/purr/v1/apps/<name>/<version>/package.cat
<base>/purr/v1/assets/<name>/<version>/<file>
<base>/purr/v1/extras/<name>/<version>/<file>
```

Signatures are raw ECDSA P-256 over SHA-256, 64 bytes (r then s), written as 128 hex digits in the `.sig` file. HTTPS only (an SDK repository on the local network may
use http, as before). A release address redirects to GitHub's file host; the client follows redirects for the store kind (and only to https).

**Official address, a placeholder until the release exists:**
`https://github.com/PastorCatto/PURR-OS-ESP32/releases/download/JumpingJaguar3` (`PURR_REPO_OFFICIAL_URL` in `purr_repo.h`, `RELEASE3` in `tools/release/`).

## 3. The files

### 3.1 repo.json (the release file)

```json
{
  "format": 2,
  "id": "9f3a61c2d0b84e17",
  "name": "PURR Apps",
  "description": "Pre-compiled apps for PURR OS, each with a link to its source.",
  "maintainer": "PastorCatto",
  "url": "https://github.com/PastorCatto/purr-apps",
  "key": {"alg": "p256", "pub": "<128 hex digits: X then Y>"},
  "min_os": "1.0.0",
  "updated": 1793300000,
  "endorsement": {"key_id": 2, "sig": "<128 hex>"}
}
```

- `id` is the first 16 hex digits of SHA-256 of the 64 public-key bytes. The OS checks it; it is what a person compares ("fingerprint 9f3a 61c2 d0b8 4e17").
- `endorsement` is optional: a signature, by a key the device already has in its key bag (today the boot key, id 1; the system key once there is one), over
  `"PURRREPO\0" + SHA-256(pub)`. It marks the **official** repository (and any repository the project vouches for). `key_id` says which key signed it.
- A repository can move: the new `repo.json` is signed by the same key. A **new key is a different repository** (section 4).

### 3.2 index.json (format 2)

The v1 format of `purr_repo.h`, with these changes: `"format": 2`, `"repo.kind": "store"`, and each entry gains

| Field | Meaning |
|---|---|
| `type` | `"app"` (default), `"asset"`, `"extra"`, `"theme"` (one `pack.bin`, up to 4 MB, installed as `/ui/themes/<name>/`) or `"shell"` (one signed `shell.cat`, up to 1 MB, `dest` fixed to `shell.cat`, installed as `/ui/shells/<name>/`; `category` is the skin, `w10m` or `android`; it carries its own pictures) |
| `target` | where it may live: `"flash"`, `"sd"` or `"any"` (default `"flash"`) |
| `dest` | for an asset or extra: the path it is written to, under the user's or the system's asset folder (section 6). No `..`, no absolute path |
| `needs_sd` | `1` if it cannot work without an SD card |
| `source` | a link to the source (a web address; shown, never fetched) |
| `perms` | the permissions an app asks for, for the Store to show before installing |
| `category`, `changelog`, `icon` | what the Store shows |
| `min_os` | the oldest OS it works on |
| `abi_min`, `abi_max` | for anything that runs: the app/driver ABI range |
| `removed` | `1` marks a version as withdrawn (never offered; installed copies are flagged) |

The index is advisory in the old sense: size and SHA-256 are checked, then the package is checked as before. What changes is that **the index is signed**, so the
SHA-256 in it vouches for the package (section 5).

### 3.3 Packages

- `app`: a `.cat` container (`Modules/SPEC.md`). It may be signed by its author's key, or carry no signature at all when the repository vouches for it (section 5).
- `asset`, `extra`: one file each. Several files are several items (a bundle format can come later).

## 4. Trust

| Tier | What it is | How a device reaches it |
|---|---|---|
| **official** | the repository's key is endorsed by a key in the device's key bag | the default repository, added automatically at setup |
| **community** | a repository the owner added and confirmed | an administrator adds the address, reads the name and fingerprint, and confirms |
| **yours** | a repository on the local network (the SDK kind) | the developer server, as today |
| **untrusted** | anything else: a bad signature, a changed key, an unconfirmed address | **developer mode must be on**, and the Store says so on every screen |

- The device keeps its trusted repositories in `/etc/repos.conf` (administrators only): `id`, `name`, `url`, `pub`, `tier`, when it was added.
- **Adding** a repository fetches `repo.json` and `repo.json.sig`, checks the id against the key, the self-signature, the endorsement if any, and then shows the
  name, maintainer, address and fingerprint. The confirmation is what makes a community repository trusted. The key is remembered.
- **Every later fetch** checks `index.json.sig` against the remembered key. If `repo.json` ever carries a different key, the repository drops to **untrusted** until an
  administrator confirms the new fingerprint again (and the Store says "this repository changed its key").
- **Withdrawing trust:** removing the repository from the list. A published list of revoked repository ids (checked by the Store service) can come later.
- A signature proves who published something and that it is unchanged. It does not prove it is safe: the Store shows the tier, the maintainer and the
  permissions an app asks for.

## 5. What the loader accepts from a repository

An app installed from a trusted repository is stored under `/apps/store/<repository id>/<name>/` with a copy of the signed `index.json` and `index.json.sig` it came from
(a "receipt"). When the app is loaded, the loader accepts it if **either**

1. its own signature verifies against the key bag, as for any app; **or**
2. the receipt's signature verifies against the repository's key in `/etc/repos.conf`, that repository is still trusted, and the package's SHA-256 is the one in the receipt.

Rule 2 is the community path. Limits that always apply to rule 2: the app is **unprivileged** (never `superuser`, never a driver or module); permissions are asked
as usual; and the package must be an `app`. Drivers and the system keep the existing rules (`Drivers/SPEC.md`): the key bag, or developer mode.
In an **untrusted** repository the same install is allowed only with developer mode on, and the app is marked DEV like any other.

## 6. The Store's companion

A small background app, **storesvc** (a daemon with no screen), plus the OS's own **store engine** (the `store` catcall, `purr_store.c`) that does the work. The engine
owns every install; the Store app and the companion are two clients of it, so closing the Store never interrupts an install.

- **install, update, remove** apps (`/home/<user>/apps/<name>/`, with a receipt), asset and extra files, by request of the Store app or on the companion's schedule
  (a check every `store.check_hours` hours, 12 by default; the first one about 90 seconds after start);
- **updates by themselves:** the companion posts a notification ("3 updates ready in the Store"). If the owner turned on `store.auto_update`, it then installs them too;
- **assets and extras:** `target=flash` goes to `/home/<user>/assets/<name>/<dest>`; `target=sd` goes to `/sd/purr/<name>/<dest>` when an SD card is mounted. An item
  that needs an SD card when there is none says so and installs nothing. (There is no SD driver yet: `/sd` is simply absent until one exists.) A version file records what is installed;
- **space:** a flash download is refused if it would take the file system past 90 % full, and says how much it needs;
- **updates of the Store itself:** the Store app and the companion are items in the official repository like any other. An update replaces the package on disk; it takes
  effect the next time the app starts (the old copy stays running until then);
- **limits:** one job at a time, never writes outside its folders, never installs from an untrusted repository without developer mode.

A background app cannot be asked for a permission, so the companion's one permission ("install-apps") is granted by the Store's own seeding (section 9).
When the OS has a service runtime (`plan5.md`: the `[service]` entry of a package, a wake window on an alarm), the companion becomes the second entry of the Store's package
and its timers become an alarm; nothing else changes.

## 7. Maintainers

`purr repo` in the developer kit makes and runs a repository (section 3's files): `init` (key and `repo.json`), `add` (a package, with hash, size and source link),
`index` (writes and signs `index.json`), `verify`, `serve` (local test), `endorse` (done by the project, for the official repository). A GitHub Actions
workflow template that rebuilds and signs the index on every push is part of the kit. The private key never lives in the repository: CI is given it as a secret,
or the maintainer signs locally.

## 8. Specs a repository may define for itself

A repository chooses its own rules inside this format: categories, what it accepts, who may submit, how it builds. The device only cares about section 4 and 5.

## 9. Pre-installed

- The Store app and its companion are **built into the firmware** (`tools/release/gen_builtin_apps.py` writes `purr_builtin_apps.c`). They are trusted because they ride inside
  the kernel image, which is checked against the boot key before it runs; they carry no signature of their own. The first time an account signs in, they are installed into
  that account's apps folder (a receipt names the built-in table and their hash; a marker in the home folder remembers, so removing the Store sticks) and the companion is
  granted its permission and started. No network is needed, and setup (OOBE) stays as it is.
- The official repository is **not** added silently. The Store's first page offers "Set up the Store" (administrators), which adds the official address
  (`PURR_REPO_OFFICIAL_URL`, the release JumpingJaguar3), shows its fingerprint, and installs the Store's own updates from it. An endorsed repository becomes **official**
  at once; until the project endorses it, it is shown as **community**.
- Updates of both are ordinary items afterwards (section 6): a newer Store in the repository replaces the built-in one, with a receipt from that repository.

## 10. Open

- The shape of a bundle (several files as one item).
- Revocation of repositories and of single packages across repositories.
- Whether apps from a community repository may request the more dangerous permissions at all.
- SD card support (no driver yet): the service treats `/sd` as absent until one exists.
