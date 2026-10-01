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

## 2.1 Auto vs. menu (not yet built)

Two ways in: the bootloader's failure-count ladder (`../bootloader/SPEC.md` section 6, also not
yet built) landing here on its own, or a human choosing "Internet recovery" from the boot menu,
or the shell's `reboot loader`. The loader tells them apart by the handoff `boot_state`
(`../bootloader/SPEC.md` section 7): `auto_reinstall` means the ladder sent it here; anything
else means a person or a deliberate command did.

- **Auto** (`boot_state == auto_reinstall`): no Wi-Fi shell offer (section 2 step 2), no menu --
  nobody is necessarily there to answer one. Connects using only the saved recovery network
  record (section 3); if that fails, it halts the same way any other failure does rather than
  prompting for credentials nobody summoned a person to type. Then, instead of only restoring
  KittenOS, it does a **component restore**: checks `kernel`, `kittenos` and `bootpkg` each
  against the recovery manifest and rewrites only the ones that fail verification -- the same
  generalized, per-component logic `net_install` uses (`../OTA/SPEC.md` section 3), ported here
  rather than duplicated. The loader still never touches LittleFS itself (section 1); it ends by
  booting into a restored `kittenos`, which has the real filesystem and Wi-Fi stack to restore
  CoreOS, AppManager, the runtimes, the drivers bundle and apps on its own
  (`../Install/SPEC.md` section 1).
- **Menu** (any other `boot_state`): shows a menu instead of assuming a choice -- restore
  KittenOS only (section 2, today's only behavior), the full component restore (the auto path
  above, run on request instead of automatically), or the Wi-Fi diagnostic shell (section 2 step
  2, already built).

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

**In menu mode** (section 2.1), update anything except KittenOS. Once KittenOS is back, KittenOS
downloads the rest, since on these boards it carries a full Wi-Fi stack -- including the loader
itself and the boot package (section 8, now resolved: `../OTA/SPEC.md` section 3).

**In neither mode does it ever touch LittleFS,** even in auto mode's component restore (section
2.1): `kernel`, `kittenos` and `bootpkg` are all raw partition writes, the same shape as
`kittenos` always was. CoreOS-as-a-file, AppManager, the runtimes, the drivers bundle and apps
stay KittenOS's job in every case (`../Install/SPEC.md` section 1) -- adding filesystem support
to `minimal` just to let the loader do that itself was considered and turned down, to keep the
last-resort profile small and simple.

**`kittenos` stays writable only from here.** Nothing that runs above it (KittenOS's own shell, a
full PURR OS session) can overwrite the kittenos partition through `net_install` -- only
`install_kittenos()` here can, and it only runs from the `minimal` profile, which never shares a
boot with a general shell. That keeps the fallback tier safe even from a compromised or buggy
system running above it.

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
- **Its minimal display and keyboard drivers** for typing a password, or serial only.
- **Captive portals** and Wi-Fi that needs a login page.
- **The certificate bundle:** how it stays current, since the loader is rarely changed.

Resolved: **how the loader itself is updated,** and **whether it can also restore the boot
package.** Both are `PURR_IMG_MODULE` subtypes that already existed for exactly this
(`PURR_MOD_LOADER`, `PURR_MOD_BOOTPKG`, both `PURR_ROLE_BOOT`-only, `purr_keybag.c`). KittenOS
writes them the same way it already wrote the kernel: `netinstall loader` / `netinstall bootpkg`,
over the network, not only by serial reflash (`../OTA/SPEC.md` section 3).
