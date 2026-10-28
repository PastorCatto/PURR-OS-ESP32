# 11. Apps

**Short version: you can install, list, inspect and remove signed `.cat` app packages today. Nothing
can run them.** The runtime, the catcalls, the app SDK, the permission system and the UI are all
**[DESIGNED]** (`AppRuntime/`, `AppSDK/`, `Catcalls/`, `Permissions/`, `CatFormat/`, `UI/`,
`MicroPython/`, `Transfer/`). This chapter documents the half that exists: the app manager's storage
core and its four shell commands. Source: `PurrOS/components/coreos/src/purr_appmgr.c` (95 host checks),
the app section of `PurrOS/main/commands.c`, `Modules/appmanager/apps_module.c`,
`Modules/coreos/appmgr_module.c`.

## What an app package is

A PURR image container with `image_type = 4` and a payload. The device accepts a package when:

1. it parses as a PURR image and is type 4 (`not an app image` otherwise),
2. its chip id equals this device's (`9` for ESP32-S3) **or `0xFFFF` (any)**, since apps are meant to carry
   their own per-platform payloads later,
3. it **verifies**: payload SHA-256 and a signature from a key in the key bag whose role may sign apps
   (system, owner, developer or vendor, never boot),
4. its name is 1 to 31 characters (it becomes a folder name and is **not** checked for `/` or `..`, [F-11](FINDINGS.md#f-11)),
5. no equal or newer version of that name is installed.

The payload format (`CatFormat/SPEC.md`: payload table, manifest block, relocation list) is **not**
parsed or checked at all. The package manager treats the payload as opaque bytes. A package with a payload the
runtime could never load installs fine.

## Where apps live

`/home/<user>/apps/<name>/package.cat`, **per user**. `apps`, `appinfo`, `appinstall`, `appremove` and
`appformat` all act on the logged-in user's own folder. The spec describes one shared copy under `/apps`
([F-09](FINDINGS.md#f-09)). A user's folder is created at boot for every registered account and on first
install. Removing an account leaves its apps behind ([07](07-user-accounts.md)).

## Commands

### Install

```
alice@PURR OS> appinstall /downloads/hello.cat
installed
alice@PURR OS> appinstall https://example.org/apps/hello.cat
fetching https://example.org/apps/hello.cat...
installed
```

- The argument is a path on `root` or an `http://` or `https://` URL.
- The package is read into a 512 KB buffer, so **packages over 512 KB are rejected** (a file reports a filesystem
  error; a URL reports `appinstall: fetch failed (ESP_ERR_NO_MEM)`).
- The install is staged: written to `<name>.tmp/package.cat`, its hash re-checked, then renamed into place. Half-finished
  `.tmp` folders are removed at the next boot. A first install is safe against a power cut. **An update is not**: it deletes
  the old copy before renaming the new one in, and a cut in that gap leaves only a `.tmp` folder that the next boot deletes,
  losing the app ([F-34](FINDINGS.md#f-34)).
- Failure messages (`appinstall: <reason>`): `not a PURR image`, `not an app image`, `no payload for this chip`,
  `failed verification`, `name too long`, `an equal or newer version is already installed`,
  `too many apps installed` (limit **32** per user), `filesystem error`.
- With `secure_mode = off`, an unverified package is accepted. With `warn` or `enforce` it is refused with
  `failed verification` ([17](17-keys-and-signing.md)).

### List and inspect

```
alice@PURR OS> apps
  hello            1.0.0           1K  verified
alice@PURR OS> appinfo hello
name:     hello
version:  1.0.0
size:     1K
chip:     matches this device
verified: yes (signer role 4)
```

`apps` re-reads and **re-verifies every installed package on every call**, so the status always reflects the
current key bag (and a package signed by a since-removed key shows `unverified`). With many large apps this is slow.
Signer roles: 1 boot, 2 system, 3 owner, 4 developer, 5 vendor.
`(wrong chip)` marks a package built for another chip.

### Remove

`appremove hello` deletes the app's folder. `appformat --yes` removes every app of the **current** user
only. Neither touches other users' apps, and neither asks which.

## Making a package to test the install path

There is no app build tool (`purrstrap apps` is **[DESIGNED]**). To exercise install, list, verify and remove
you can wrap any bytes in an app container yourself. This was run in a scratch directory with the repo's tools:

```python
# mkapp.py  (run from the purrstrap folder so `lib` imports)
import sys
sys.path.insert(0, ".")
from lib import image as pimg
payload = b"not runnable code\n"
img = pimg.make_image("hello", "1.0.0", pimg.IMG_APP, 0, payload, pimg.CHIP_ANY)
open("hello.cat", "wb").write(img)
```

```powershell
python purrstrap/purrstrap.py keys sign --key signing_keys/developer.key --image hello.cat --key-id 2
python purrstrap/purrstrap.py keys verify --key signing_keys/developer.pub --image hello.cat
```

Expected: `signed hello 1.0.0 with key id 2` then `hello 1.0.0: payload hash and signature both check out`.
The key id must be the id your device's key bag has for a key whose role may sign apps (`2`, the developer key,
in this repo). Then serve it and install:

```powershell
cd <folder with hello.cat>
python -m http.server 8000
# on the device:  appinstall http://<pc-ip>:8000/hello.cat
```

To install a bad one on purpose, edit a byte of the signed file: `appinstall` says `failed verification`.

## What is not here

All **[DESIGNED]**, with the spec that describes it:

| Missing | Spec |
|---------|------|
| running an app: `run`, `ps`, `kill`, `top`, `fg`, `bg`, `log` | `AppManager/` 8.1, `AppRuntime/` |
| the app runtime (`catrt`), multitasking, the out-of-memory killer | `AppRuntime/` |
| catcalls apps call: `console`, `input`, `storage`, `net`, `ui` ... | `Catcalls/` |
| the app file format beyond the container (payload table, manifest, relocations) | `CatFormat/` |
| the SDK, starter templates, `purrstrap apps` | `AppSDK/` |
| permissions: install-time list, first-use prompts, `perms`, `grant`, `revoke` | `Permissions/` |
| privileged apps and the `superuser` permission (owner key) | `Permissions/`, `AppSDK/` |
| MTP, SD and serial transports to bring packages in | `AppManager/` 7 |
| app index and OTA app, app storage app, device-to-device transfer | `OTA/`, `Transfer/` |
| MicroPython apps | `MicroPython/` |

Next: [12 Writing modules](12-writing-modules.md).
