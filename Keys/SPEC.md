# Keys and signing spec (draft 0.1)

Which keys exist, who may sign what, how third parties are trusted, and the tool that
generates and uses keys. The device side (the key bag, `purrcfg`, and verification) is in
`../bootloader/SPEC.md` sections 3 to 5 and `../PurrOS/components/coreos/SPEC.md`
section 3. The tool lives in purrstrap (`../purrstrap/SPEC.md`).

## 1. Decisions so far

- **Separate keys per role,** so that a leaked key only affects its own role.
- **Signatures are ECDSA P-256 with SHA-256,** as the bootloader spec defines.
- **Private keys are kept offline,** on the owner's machine or a USB drive, and possibly on
  a YubiKey later. They are never stored in CI. Releases are signed locally and the signed
  files are uploaded.
- **Third parties (vendors) are trusted through certificates,** with a user enrollment
  fallback (section 4).
- **Certificates expire,** checked against the latest time the device has seen (section 5).
- **Developer-preview builds trust a published test key.** Official builds trust only the
  release keys (section 6).

## 2. Roles

| Role | Signs |
|------|-------|
| **boot** | the bootloader, boot package, KittenOS, kernel and CoreOS |
| **system** | the drivers bundle, AppManager, the runtimes and official system apps |
| **owner** | apps that hold the `superuser` permission |
| **developer** | third-party apps and user code drivers |
| **vendor** | modules and apps from a third party, such as Meshtastic or MeshCore support, signed with that vendor's own key |

The key bag holds keys by role, and a file only verifies against a key whose role allows
that type of file.

## 3. The key tool

A purrstrap subscript, `keys`. It uses the `cryptography` package, declared as a
requirement of this subscript alone, so the rest of purrstrap works without it.

| Action | Does |
|--------|------|
| `generate` | Creates a key pair for a role and exports it to files: a **password-protected private key** and a plain public key |
| `export-public` | Writes a public key in the format the device embeds (raw 64 bytes), and the generated C source for the default key bag |
| `sign` | Signs a file with a private key |
| `verify` | Checks a signed file against a key file (a public key is enough) |
| `inspect` | Shows a key, a certificate, or the signature block of a file |
| `cert` | Issues a vendor certificate (section 4) |
| `revoke` | Adds a key or certificate serial to the revocation list |

**Passwords**

- The private key file is encrypted with a password.
- purrstrap **asks for it when it signs,** and never stores it or writes it to the
  remembered-values file.
- An **environment variable** (`PURRSTRAP_KEY_PASSWORD`) can supply it for scripted builds.
  It is opt-in.
- It is **never accepted as a command-line argument,** because arguments show up in process
  lists and shell history.

**Signer interface.** purrstrap computes the hash and hands it to a signer, which returns
the signature. The file signer is the first implementation. An external-command signer,
for a YubiKey's PIV mode (which supports P-256), comes later without changing anything else.
The signature is converted to the raw 64-byte form the device expects.

## 4. Vendors and certificates

- **A vendor certificate** is a small signed statement: the vendor's name, its public key,
  **what it may sign** (for example LoRa drivers and apps, and never boot files), a serial
  number, and a start and end date. The owner signs it.
- **The device trusts** anything signed by a key that has a valid certificate. You can approve
  a vendor without shipping a release, limit what each vendor signs, and revoke one vendor
  without touching the rest.
- **User enrollment (fallback).** A user can enroll a key on their own device with a warning.
  A user-enrolled key holds only the developer role, and never boot, system or owner.
- **Overwrite warning.** Whenever a key in the key bag would be replaced, by a key update, a
  vendor rotation or a re-enrollment, the system warns the user and names the old and new
  key. Changes still need a signed request when Secure Boot is on
  (`../bootloader/SPEC.md` section 5.1).
- **Revocation.** Keys and certificate serials can be revoked. The list ships with updates
  and is stored in the key bag.

## 5. Certificate expiry and the time floor

Devices do not always know the time, so expiry is checked against the **latest time the
device has seen**:

- It is stored in `purrcfg` (`latest_time`) and only ever moves forward.
- Sources: the real-time clock, **authenticated** network time (the `Date` header of an HTTPS
  response), and the build timestamp of each verified image. Plain NTP sets the clock but never
  the floor (`../Network/SPEC.md` section 1). A board with no clock and no network raises it only
  from image build dates.
- A certificate is expired when its end date is earlier than that time.
- Setting the clock backwards cannot make an expired certificate valid again.
- **Only verified sources can raise it.** A timestamp from an unverified file never moves the
  floor, so nobody can expire everything by supplying a far-future date.

**When a certificate has expired,** it is not trusted. The system boots KittenOS, warns the
user, and asks whether to disable Secure Boot.

## 6. The test key and developer previews

- Developer-preview builds (DP 10) trust a **published test key** for the owner and developer
  roles. The private key is public on purpose, so in those builds anyone can sign
  `superuser` apps and user code drivers.
- The boot report shows that a test key is trusted, so a preview device is never mistaken
  for a protected one.
- **Official releases carry no test key.** They carry only the release keys, one per role.

## 7. Testing

- Sign and verify round trips, with a password given by prompt and by environment variable.
- Test vectors: purrstrap signs and the C code verifies, and the reverse
  (the same approach as `../CatFormat/SPEC.md` section 10).
- Table-driven certificate cases: wrong role, scope not allowed, revoked serial, tampered
  certificate, expired, not yet valid.
- Time floor cases: no clock, clock set backwards, an image with a bad timestamp.
- Overwrite warnings appear for every kind of key replacement.
- A wrong password fails without leaking anything.

## 8. Open questions

- **The certificate encoding.** Compact binary like the container is likely, since a tiny C
  parser has to read it.
- **The scope vocabulary:** how a vendor's allowed file types and hardware are named.
- **How often `latest_time` is written,** to limit flash wear.
- **Where the owner and boot keys are kept,** and their backups.
- **Rotating the boot key,** given that the bootloader and boot package carry public keys.
- **What a user-enrolled developer key may do,** and whether it needs a warning each time it
  signs something new.
- **The format of the generated default key source,** for the bootloader and CoreOS.
- **A YubiKey signer:** which tool and which slot, once it is added.
