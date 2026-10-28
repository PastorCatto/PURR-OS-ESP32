# 17. Keys and signing

Everything that holds code is signed, and the device checks the signature before it uses the file. This chapter
explains the trust model as built, how to make and use keys with `purrstrap keys`, and how to rotate them.
Source: `purrstrap/scripts/keys.py` (and `lib/image.py`), `PurrOS/components/coreos/src/purr_keybag.c`,
`purr_verify.c`, `keys/purr_default_keys.c`, `bootloader/bootloader_components/main/purr_crypto_uecc.c`.
Design: `Keys/SPEC.md`.

**Status: [WORKS]** generate, export, sign, verify, inspect; device-side verify (52 host checks, 55 on the key bag,
all signed by the same Python the tool uses). **[DESIGNED]** and absent: vendor certificates, revocation lists and
tooling, certificate expiry and the time floor, the owner key, the system key in practice, YubiKey signing,
key updates applied by the bootloader. See [F-19](FINDINGS.md#f-19) and [F-08](FINDINGS.md#f-08).

## The model

- **Algorithm:** ECDSA over P-256 with SHA-256. Public keys are 64 raw bytes (X then Y), signatures 64 raw bytes
  (r then s).
- **What is signed:** the 109 bytes of the image header that come before the signature field. The header contains the
  SHA-256 of the payload, so the signature covers the payload too. Layout in
  [21](21-reference-formats-abis-limits.md#image-container).
- **Key ids** (0 to 31) are numbers in the image header. The device looks the key up by id in its **key bag**. The id
  is not a secret and not derived from the key; you assign it.
- **Roles:** every key in the bag has a role. A file only verifies if its key's role may sign that kind of file:

| File kind | Roles allowed |
|-----------|---------------|
| PURR OS image, KittenOS image (types 1, 2) | boot |
| module subtype kernel, coreos, bootpkg, loader | boot |
| module subtype appmanager, runtime, devbundle | system |
| module subtype driver (all current `/system` and `/kernelmods` modules) | system, vendor, developer, owner |
| app (type 4) | system, owner, developer, vendor |

- **The key bag** is the compiled-in defaults (`purr_default_keys.c`) with up to 8 override slots from `purrcfg`
  applied by key id, and a 32-bit revocation mask. 16 keys at most. **Nothing writes the override slots or the
  mask today**, so the bag is exactly what is compiled in.
- **This repo's default bag has two keys:** `boot` id 1 and `developer` id 2. There is no `system` key, so
  AppManager, runtime and devices-bundle modules cannot currently be signed for a device built from this tree.
  There is no `owner` key either.

### How a device verifies a file

`purr_image_verify()` checks in this order and returns the first failure:

| Result name | Meaning |
|-------------|---------|
| `bad-magic` | not a PURR image |
| `bad-version` | header version is not 1, or the version text is malformed |
| `bad-layout` | sizes or offsets do not fit the file |
| `bad-type` | image type outside 1 to 4 |
| `bad-chip` | built for another chip (apps may say "any") |
| `too-old-bootloader` | `min_boot_version` is above the bootloader's version (always 0 today) |
| `below-version-floor` | in `enforce` mode, older than the confirmed floor |
| `no-key` | the key id is not in the bag |
| `revoked` | the key id's bit is set in the revocation mask |
| `role-mismatch` | the key's role may not sign this kind of file |
| `bad-hash` | payload SHA-256 does not match the header |
| `bad-signature` | the signature does not verify |
| `io-error` | could not read |
| `ok` | verified |

Only a failure of the full check matters. The bootloader additionally insists the boot package really is a boot
package, even if validly signed as something else.

### secure_mode

`purrcfg.secure_mode` is `off`, `warn` (default) or `enforce`. As built:

- the **install** paths (`appinstall`, `netinstall` for partitions, staged files and module files, the loader's installs) and the bootloader's load of the **boot package** accept a
  file that does not verify **only in `off`**, with a warning;
- the **module loader never accepts an unverified module, in any mode** (`off` lets `netinstall modules` write an unsigned file, and the next boot quarantines it);
- `enforce` adds the version-floor check to staged system updates and nothing else. There is no shell command or tool to change it; write a `purrcfg`
image ([05](05-boot-process-and-flash-layout.md#changing-purrcfg-from-the-pc)). The three bypass flags
(`IGNORE_ONCE`, `SECURE_OFF_ONCE`, `UPDATE_KEY`) and the eFuse flow are not implemented.

## Making keys

You need `pip install cryptography` ([03](03-development-environment.md)). Real output from a run:

```powershell
$env:PURRSTRAP_KEY_PASSWORD = "choose-a-long-passphrase"     # or omit it and be prompted
python purrstrap/purrstrap.py keys generate --role boot      --out signing_keys --key-id 1
python purrstrap/purrstrap.py keys generate --role developer --out signing_keys --key-id 2
```

```
[ok] boot key pair generated (id 1, fingerprint eda999b8f486d160)
    private (password-protected): signing_keys\boot.key
    public:                       signing_keys\boot.pub
Keep the private key offline. It is never needed to verify anything.
```

- Files: `<role>.key` (PKCS#8 PEM, encrypted with your password) and `<role>.pub` (SubjectPublicKeyInfo PEM).
- The **password** is asked for (twice) or read from `PURRSTRAP_KEY_PASSWORD`. It is never accepted as a command-line
  argument and never stored. An empty password is refused.
- **`--key-id` here is cosmetic.** It is only printed. The id that matters is the one you give to `export-public` and
  `sign` ([F-19](FINDINGS.md#f-19)). Use the same number everywhere.
- Re-running `generate` for the same role **overwrites** the files without asking.
- **The fingerprint** is the first 8 bytes of SHA-256 over the raw 64-byte public key. It identifies a key in logs
  and `keys inspect`.
- A **lost password cannot be recovered.** The project has already lost one this way and rotated: the old pair sits in
  `signing_keys/old/`. Treat it as a routine, not a disaster, but it means rebuilding and reflashing everything
  (below).

### Put the public keys into the build

```powershell
python purrstrap/purrstrap.py keys export-public --pub signing_keys/boot.pub      --role boot      --key-id 1 --out PurrOS/components/coreos/keys
python purrstrap/purrstrap.py keys export-public --pub signing_keys/developer.pub --role developer --key-id 2 --out PurrOS/components/coreos/keys
```

Each call writes `<role>_<id>.pub.bin` (the raw 64 bytes) and then **regenerates `purr_default_keys.c` from every
`*_<id>.pub.bin` in that folder**, so you add keys one at a time. Delete a `.pub.bin` to remove a key. The generated
file is tracked in git (public keys only) and compiled into **both the bootloader and PurrOS**.

The private keys must never be committed. `signing_keys/` is in `.gitignore`; check before you `git add`.

### Sign

```powershell
python purrstrap/purrstrap.py keys sign --key signing_keys/boot.key --image bootpkg/build/tdeck_plus/bootpkg.bin --key-id 1
# [ok] signed bootpkg 0.1.0 with key id 1
```

`--image` is signed in place unless you pass `--out`. Signing recomputes the payload hash, so you can edit the payload
and re-sign. **`--key-id 0` (the default) means "leave the header's id as it is"**, and a freshly built image has id 0,
which then fails `no-key` on the device. Always pass the id.

### Verify and inspect

```powershell
python purrstrap/purrstrap.py keys verify  --key signing_keys/boot.pub --image bootpkg.bin
# [ok] bootpkg 0.1.0: payload hash and signature both check out
python purrstrap/purrstrap.py keys inspect --path bootpkg.bin
```

`inspect` accepts an image (prints header version, chip id, type, flags, payload offset and size, payload SHA-256,
key id, signature), a public key (fingerprint and raw hex), a private key (asks for the password, prints the public
fingerprint) or a 64-byte raw key file.

## Which key signs what, in practice

| You are releasing | Sign with | `--key-id` |
|-------------------|-----------|-----------|
| bootpkg, kernel, loader, KittenOS, PURR OS image, coreos | boot key | 1 |
| shell command modules, kernelmods | developer key | 2 |
| apps | developer key | 2 |
| appmanager, runtime, devbundle | **a system key, which does not exist yet** | |

That the developer key signs `netinstall`, which writes raw partitions, is the author's own open question
(`Modules/SPEC.md` section 10): the permissive `driver` subtype covers it for now.

## Rotating keys (what the author did, 2026-09-30)

Whichever key is in `purr_default_keys.c` is the trusted one. Rotating means replacing it and redoing everything
that was signed or built with the old one:

1. Move the old pair aside: `signing_keys/old/`.
2. `keys generate` new `boot` and `developer` keys.
3. `keys export-public` both into `PurrOS/components/coreos/keys/` (same ids, so the files are replaced).
4. **Rebuild the bootloader** (it embeds the key bag) and flash it by wire at `0x0`.
5. Rebuild PurrOS in every profile you ship, and the standalone kernel if you use it.
6. Re-sign the boot package with the new boot key and reflash it at `0x12000`. Until you do, the bootloader rejects it
   and you have no menu (the system still boots).
7. Rebuild, **re-sign and replace** every module and kernelmod. If they are embedded in `commands.c`, regenerate the
   byte arrays.
8. Re-sign and republish every released file: KittenOS, kernel, loader, bootpkg, modules, manifests.

A device still running the old bootloader will refuse everything signed by the new keys. There is no remote key update:
`purrcfg` has the slots and the bootloader's `UPDATE_KEY` step is not built, so rotation means reflashing by wire.

## Common mistakes

| Mistake | Effect |
|---------|--------|
| signed with `--key-id 0` | `no-key` |
| signed with the right key but wrong id | `no-key` or `role-mismatch` |
| rebuilt PurrOS but not the bootloader after a key change | the bootloader rejects the boot package |
| left the old `.pub.bin` in `keys/` | the old key stays trusted; delete it |
| committed `signing_keys/` | the private key is exposed; rotate immediately |
| signed a module with the `boot` key | works for `driver` modules too, but widens damage if that key leaks |

## Not built

Vendor certificates and per-vendor scope, the revocation list and `keys revoke`, certificate expiry and the
authenticated time floor (`latest_time` is never written), a published test owner key for developer previews,
`keys cert`, YubiKey (PIV) signing, signing in CI. See [24](24-not-built-yet.md).

Next: [18 Making your own PURR OS](18-making-your-own-purr-os.md).
