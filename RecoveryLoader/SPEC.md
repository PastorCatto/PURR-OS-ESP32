# Recovery loader spec (draft 0.1)

An internet recovery for KittenOS, like the one on a PC. If KittenOS itself is missing or
damaged, this small loader connects to Wi-Fi, downloads the newest KittenOS, and installs it.
It is the last step before a serial prompt in the fallback chain (`../KittenOS/SPEC.md`
section 5).

## 1. Decisions so far

- **A small separate image in its own raw partition,** almost never changed.
- **It holds only what it needs:** the Wi-Fi stack, TLS with the certificate bundle, the
  public keys, and CoreOS's **minimal profile** (verification, `purrcfg`, a flash writer). It has
  no shell, no filesystem and no apps.
- **Boards with room for it only.** The 4 MB boards leave it out, in line with KittenOS having
  no network stack there (`../Network/SPEC.md` section 1). On them, recovering a broken KittenOS
  is a serial or USB reflash.
- **Started by the bootloader** when KittenOS is missing, damaged or fails verification, and
  from the boot menu on request.
- **It is also the first step of a network install** on a board flashed with only the bootloader,
  the partition table and this loader (`../Install/SPEC.md`).
- **Wi-Fi credentials come from two places:** a **remembered recovery network** that the
  running system saved, and **typing it in** on the keyboard or serial console as the fallback.
- **It takes the newest KittenOS** listed in the recovery manifest (../OTA/SPEC.md section 5). The version rule follows Secure
  Boot: in `enforce` mode **only the latest version is accepted** (nothing older than the newest
  the device has confirmed). In `warn` and `off` modes an older version is allowed to boot with a
  warning.

## 2. What it does

1. Show or print why it started.
2. Get a network: use the remembered recovery network, or ask the user to type the network and
   password.
3. Connect, and fetch the recovery manifest (../OTA/SPEC.md section 5) from the main repo's Releases.
4. Pick the newest KittenOS for this board.
5. Download it into PSRAM, then verify it before touching flash: container, chip, type, key role,
   signature, and hash. An unsigned image is only accepted when `secure_mode` is off. In `enforce` mode a
   version below the version floor is refused. In `warn` and `off` an older version is allowed, with
   a warning.
6. Write it into the `kittenos` partition, read it back to check the hash, and restart.

Because the image is verified in memory before the erase, a bad download never damages what is
there. A power cut during the write leaves a broken KittenOS, but the loader itself is untouched
and simply runs again.

## 3. The recovery network record

- The running system saves the last-used network in a small raw record. The loader reads it.
- It holds a network name and password in plain form in raw flash. Like the saved Wi-Fi file, real
  protection against reading the flash needs flash encryption, a later step.
- If it is missing or wrong, the loader asks for the details and offers to save them.

## 4. Trust and time

- The download is over HTTPS, and the image is verified by signature as well, so the download
  source is not trusted for integrity (`../OTA/SPEC.md` section 2).
- The `Date` header of the HTTPS response is authenticated time, so the loader may raise
  `latest_time` (`../Keys/SPEC.md` section 5).
- Plain NTP is not used by the loader.

## 5. What it does not do

Update anything except KittenOS. Once KittenOS is back, KittenOS downloads the rest, since on
these boards it carries a full Wi-Fi stack.

## 6. Size

A Wi-Fi and TLS stack plus the certificate bundle is probably several hundred KB, and not
measured yet. The partition is sized once a real image exists. It is only for boards with plenty
of flash.

## 7. Testing

- Against a fake network: fetching the manifest, choosing the newest version, a wrong signature, a
  truncated download, and an older version booting with its warning.
- The recovery network record: present, missing, and wrong.
- A power cut during the write.
- On a board: break KittenOS on purpose and recover it over Wi-Fi.

## 8. Open questions

- **The name:** recovery loader, or something else.
- **The menu entries** for internet recovery and "update KittenOS" in the boot menu, alongside
  boot normally and boot KittenOS.
- **How the loader itself is updated,** probably only by serial reflash.
- **Its minimal display and keyboard drivers** for typing a password, or serial only.
- **Captive portals** and Wi-Fi that needs a login page.
- **Whether it can also restore the boot package,** which is also a raw partition.
- **The certificate bundle:** how it stays current, since the loader is rarely changed.
