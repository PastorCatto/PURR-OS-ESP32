# KittenOS spec (draft 0.2)

The recovery system, and the one that applies updates. It is a small, fixed system in
the boot area, and it is not updated in normal use. Where it sits is in
`../PurrOS/SPEC.md`. The bootloader's part in starting it is in `../bootloader/SPEC.md`.

## 1. What it is

- One of two things that can start after the bootloader. The other is the normal system
  (the boot package, then the kernel and CoreOS).
- About 1 MB, in its own raw `kittenos` partition. Not updated in normal use.
- A real small OS: FreeRTOS, its own small fixed driver set (display and keyboard, and
  what else recovery needs), a **filesystem reader** for the root filesystem, and the
  same command-line shell.
- **Built from CoreOS's recovery profile** (`../PurrOS/components/coreos/SPEC.md` section 1.1), so
  recovery looks and behaves like the normal system. It carries its **own copy** of the CoreOS
  code, which stays frozen and is refreshed through the recovery loader, not loaded from `/boot`.
- **It never depends on what it recovers.** It does not load the kernel or CoreOS files,
  so it works when they are missing or broken.
- It loads AppManager from `/boot` if that file is present and verifies, to get app and
  update handling from one implementation. If it is not, KittenOS still has the shell and
  the swap routine.
- **Wi-Fi.** On modular boards KittenOS carries a Wi-Fi stack with HTTPS, so it can download
  a missing or broken system file itself. On 4 MB (monolithic) boards it has no network stack,
  because Wi-Fi with HTTPS is too large for its 1 MB (`../Network/SPEC.md` section 4).

## 2. When it starts

- **The user asks for it on purpose:** the shell command `reboot recovery`, or choosing it
  in the boot menu at power-on (section 3). The menu works even when the normal system is
  too broken to reach a shell.
- **The normal system does not come up:** the kernel or CoreOS file is missing, fails
  verification in enforce mode, or does not start (a failure counter in `purrcfg`).
- **An update is waiting:** the running system recorded a request and restarted into
  KittenOS to have it applied (`../PurrOS/SPEC.md` section 6.1).
- `FORCE_RECOVERY` is set, or CoreOS asked for it after a failed decision.

Starting it means recording the target in `purrcfg` and restarting, and the bootloader
honours that.

If KittenOS itself fails its own check in enforce mode, it does not restart into itself. On a
modular board the bootloader starts the recovery loader (`../RecoveryLoader/SPEC.md`), and
otherwise it falls back to a serial prompt.

## 3. The boot menu

At power-on the boot package shows a prompt, like a PC's "press a key for setup". If nobody
presses anything before the timeout, the system boots normally. The menu is drawn by the
boot package (`../bootloader/SPEC.md` section 9). Entries are open (section 7). At least:

- boot normally (the default)
- boot KittenOS (recovery)

## 4. What it does

- **Applies updates by swapping files** (`../PurrOS/SPEC.md` section 6.1): renames the
  current file to `.bak`, renames the staged `.new` file into place, restarts the normal
  system, and rolls back if it does not come up healthy.
- **On monolithic boards, writes the whole packed image** into the single raw slot, from an
  SD card file, and keeps no rollback copy. If the update came over Wi-Fi, the running
  system downloaded it to the SD card first. Its own UI for this comes later.
- **Builds the system over Wi-Fi** during a network install: downloads the kernel, CoreOS,
  AppManager, the runtimes, the boot package and the board's driver pack, and formats the
  filesystem (`../Install/SPEC.md`). Modular boards only.
- **Adds and removes mini-apps** through AppManager when that file is available.
- **Runs the command-line shell,** with the same commands as PURR OS where they apply.
- **Diagnostics:** the boot report, the system log, and the state of `purrcfg`.

It cannot change trust state (keys, `secure_mode`, eFuse requests) except through the signed
request path, the same as everywhere else.

## 5. Fallback chain

Every user-facing feature needs a fallback. Starting the system goes through these steps:

1. The boot package loads the kernel, the kernel starts CoreOS, and PURR OS runs.
2. If a system file fails to load or start, or a fresh update does not come up healthy,
   KittenOS rolls it back from its `.bak` copy.
3. If there is no usable copy, KittenOS stays running so the user can repair or reinstall.
4. If KittenOS itself is missing, damaged or fails verification, the bootloader starts the
   recovery loader on modular boards, which downloads a new KittenOS.
5. If that fails too, or the board has no loader, the bootloader prints a prompt on the
   serial console.

The boot package has its own fallback: if it is missing, invalid or crashed the last boot,
the bootloader skips the menu.

## 6. On the two board tiers

- **Modular boards:** the swap works on files in `/boot`.
- **Monolithic boards** (the CYD): there is no filesystem copy of the system. KittenOS is the
  only safety net. If a new image does not start, KittenOS is still there to write it again.

## 7. First cut (T-Deck Plus)

What is built first, so the rest of CoreOS can be built on the same pieces.

- **A normal ESP-IDF app in the `factory` slot.** The bootloader already knows how to start it,
  so no loader of our own is needed. It has its own 1 MB partition (`kittenos`, type app,
  subtype factory, at `0x20000`) and is not overwritten by PURR OS updates. This replaces the
  "raw partition" wording above; the effect is the same.
- **The same project as PURR OS** (`PurrOS/`), built with the `recovery` profile, so the shell
  engine, console and commands are shared. The `full` profile adds to them.
- **The boot menu lists it.** The bootloader gives the boot package the app slots and the
  factory slot. When no PURR OS slot can boot but KittenOS can, the menu counts down into
  KittenOS, which is the fallback in section 5.
- **What it does at first:** a text console on the display, a keyboard, and the shell. The
  commands are the basics (help, version, info, parts, mem, clear, echo, reboot). Reading
  the filesystem, applying updates and Wi-Fi come after.
- **`reboot recovery` is not done yet.** It needs the boot target in `purrcfg` (section 2). For
  now KittenOS is reached from the boot menu.

## 8. Open questions

- **Which key or button opens the boot menu on boards without a keyboard** (the CYD).
- **What a mini-app is in KittenOS:** the same `.cat` apps run by the same runtime, or
  something smaller.
- **The minimal driver set per board:** display and keyboard, and what else (storage, USB)
  recovery needs.
- **Reinstalling the whole system from KittenOS:** which sources it may use (SD card, serial,
  MTP, Wi-Fi) and what a factory reset does.
- **How KittenOS itself is updated:** through the recovery loader, with an "update KittenOS" entry
  in the boot menu. A serial reflash is the last resort.
- **KittenOS's update UI,** which is planned and comes after the shell.
- **How many `.bak` copies to keep** and when to delete them.
