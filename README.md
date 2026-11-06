<!--
  THE OWNER'S QUOTE GOES HERE.
  A few sentences in the owner's own words, as a block quote, in this form:

  > "..."
  >
  > Name

  It is left empty on purpose: it is not written for the owner. Delete this comment when the quote is in.
-->

# PURR OS

An operating system for small ESP32 devices, built around one idea: a device this small should still be able to check that the code it runs is the code it was meant to run, recover by itself when something goes wrong, and let a person write and install their own apps without special tools.

The first target is the LilyGO T-Deck Plus (an ESP32-S3 with 16 MB of flash, 8 MB of PSRAM, a 320 by 240 touch display, a keyboard and a trackball). Other ESP32-S3 boards are supported through board files.

This page is the overview. Every section ends with links to the detailed specifications. If this page and a specification disagree, the specification wins and this page is the bug.

## Contents

- [Where the project stands](#where-the-project-stands)
- [What it does](#what-it-does)
- [How the device starts](#how-the-device-starts)
- [What happens when something fails](#what-happens-when-something-fails)
- [How security works](#how-security-works)
- [How apps are loaded and run](#how-apps-are-loaded-and-run)
- [How the system installs and updates itself](#how-the-system-installs-and-updates-itself)
- [The user interface](#the-user-interface)
- [The developer kit and the emulator](#the-developer-kit-and-the-emulator)
- [Building and testing](#building-and-testing)
- [A map of the repository](#a-map-of-the-repository)
- [Where to read next](#where-to-read-next)
- [Licence](#licence)
- [Thanks and credits](#thanks-and-credits)

## Where the project stands

PURR OS is in active development. The current series is 1.0.x; each of those numbers is a patch on the way to **1.1, which is the version that will count as the stable general release**.

Being straight about what has been proven:

| State | Meaning here |
|---|---|
| Built and tested on the host | The C host tests and the Python tests pass on a development computer (the manifest parser, the image format and signatures, the driver registry, the build and signing tool, the developer kit). |
| Compiled | It builds for the chip. It has not necessarily run. |
| Run on a T-Deck Plus | It has been seen working on a real device. |

The boot chain, the signed image format, recovery over Wi-Fi, the app runtime and the Android-style shell have all run on a real T-Deck Plus. Several newer pieces are compiled and host-tested but not yet seen on a device; the plans say which, item by item (start with [plan9.md](plan9.md) and [plan3.md](plan3.md)). Nothing in the documentation is claimed as working unless it has been run, and where it has only been read or compiled, the text says so.

## What it does

- **Boots in stages, each one checked by the one before.** A small bootloader checks a boot package, which starts the system, and every piece carries a signature. See [How the device starts](#how-the-device-starts).
- **Falls back and repairs itself.** If the main system will not start, the device falls back to a smaller recovery system (KittenOS), and then to a recovery loader that downloads a fresh copy over Wi-Fi. A bad update or a damaged slot should not leave the device unusable.
- **Runs apps that other people write**, loaded from signed packages into external memory, each in its own sandbox with a list of permissions that the user is asked about. See [How apps are loaded and run](#how-apps-are-loaded-and-run).
- **Has an app store in the style of F-Droid.** Anyone can run a repository; whoever runs it signs it; the device decides how far to trust it.
- **Has two interface styles**: one in the look of Android 2.2 (Froyo) and one in the feel of Windows 10 Mobile. Both draw with LVGL on a 320 by 240 screen.
- **Treats drivers as downloadable, signed modules.** The display, touch and keyboard drivers ship with the system. Others (storage, sensors, the LoRa radio, trackball) are installed on demand and set aside automatically if one of them ever stops the device.
- **Comes with a developer kit** that can build, sign and run an app, and an emulator that runs the system without the hardware.

## How the device starts

The flash is divided into fixed slots. The partition table is [bootloader/partitions.csv](bootloader/partitions.csv).

| Slot | Offset | Size | Holds |
|---|---|---|---|
| `nvs` | 0xB000 | 16 KB | Wi-Fi and other settings kept by ESP-IDF |
| `purrcfg` | 0xF000 | 8 KB | The trust block, the security mode, the boot failure count, one-shot flags (two copies, so a power cut during a write is survivable) |
| `phy_init` | 0x11000 | 4 KB | Radio calibration |
| `bootpkg` | 0x12000 | 56 KB | The boot package: the boot menu and the splash screens |
| `kittenos` | 0x20000 | 1.5 MB | KittenOS, the recovery system (also the factory slot) |
| `kernel` | 0x1A0000 | 1.9 MB | The full system |
| `loader` | 0x380000 | 1.5 MB | The recovery loader, which downloads and installs the others |
| `netrec` | 0x500000 | 4 KB | The saved network used during recovery |
| `ui_assets` | 0x501000 | 5 MB | The picture pack for the shell (icons, wallpapers) |
| `root` | 0xA01000 | about 6 MB | The LittleFS file system: accounts, apps, settings, downloaded drivers |

What happens, in order:

```
power on
   |
   v
Espressif ROM bootloader (in the chip, cannot be changed)
   |
   v
PURR bootloader (flash 0x0)
   |  1. read purrcfg; add 1 to boot_fail_count (every start, no exceptions)
   |  2. check the trust block's signature against the keys compiled into the bootloader
   |  3. consume the one-shot flags (they apply to this start only)
   |  4. pick a slot with the failure ladder (next section)
   |  5. show the boot package's menu; if nobody presses a key, carry on
   |  6. verify the chosen slot (its signature trailer; in "enforce" mode a bad slot is refused)
   |  7. write the handoff block (why this slot, what was checked) and jump
   v
kernel                      <- the normal case
KittenOS                    <- recovery system, or after repeated failures
recovery loader             <- nothing else works, or an explicit request
   |
   v
the system reaches a settled state  ->  boot_fail_count is set back to 0
```

The count is raised by the bootloader on every start and cleared only by the operating system once it is up and healthy. A start that crashes, hangs or is reset therefore leaves the count where it was, and the count climbs across repeated failures. That one number drives everything in the next section.

More: [bootloader/SPEC.md](bootloader/SPEC.md), [PurrOS/SPEC.md](PurrOS/SPEC.md), [KittenOS/SPEC.md](KittenOS/SPEC.md), [RecoveryLoader/SPEC.md](RecoveryLoader/SPEC.md).

## What happens when something fails

The failure ladder picks which slot to prefer from the count (the value after this start has been added):

| `boot_fail_count` | Prefers | Falls back to |
|---|---|---|
| 1 to 3 | kernel | KittenOS, then the recovery loader |
| 4 to 6 | KittenOS | the recovery loader, then the kernel |
| 7 and up | recovery loader | KittenOS, then the kernel |

So the full system gets three tries, then the smaller recovery system gets three, then the device goes to the loader. When the ladder itself lands on the loader, the menu is skipped and an auto-reinstall request is set, so the loader restores the system without anyone having to answer a menu.

What each kind of failure leads to:

| What goes wrong | What the device does |
|---|---|
| The kernel slot is empty or does not look like an application image | The bootloader goes to the next slot in the ladder. |
| A slot fails its signature check, mode `warn` | It still boots, and a warning is recorded and shown in the secure status. |
| A slot fails its signature check, mode `enforce` | The slot is refused and the device falls down the ladder. An explicit request cannot be used to get around this. |
| The system crashes or hangs before it settles | The count is not cleared, so the next start counts higher and the ladder moves on. |
| A downloaded driver stops the device while it is starting | The system writes a marker before each driver starts and clears it afterwards. If the marker is still there at the next start, that driver is moved to a "stopped the device" list and not loaded. Device Manager shows the list and can put a driver back on purpose. |
| Two starts in a row do not settle | All downloaded drivers are set aside (the ones that ship with the system, display, touch and keyboard, never are), so the device can come up and show what happened. |
| A driver stops the device soon after sign-in | Drivers other than the essential input ones load only after the first sign-in, behind a marker that clears after 30 seconds of running. A marker still present at the next start sets those drivers aside. |
| An install is damaged in transit | Every download is checked against its size and SHA-256 from the release's manifest, retried up to three times, written, then read back and compared. Nothing is erased until the new copy has passed. |
| A release turns out to be bad | The publisher marks it withdrawn in the manifests. Devices stop offering and installing it and are told if they are running it. |
| KittenOS cannot start either | The recovery loader downloads a new one over Wi-Fi, from the official release or, in developer mode, from a development server on the local network. |
| Nothing at all is bootable and there is no network | The device resets and tries again, and the count keeps rising. The prompt on the serial port stays available. Whether that should become a halt in `enforce` mode is an open decision recorded in the bootloader specification. |

More: [SECURITY.md](SECURITY.md) section 7 and [bootloader/SPEC.md](bootloader/SPEC.md) section 6 for the ladder; [OTA/SPEC.md](OTA/SPEC.md) for withdrawing a release; [Drivers/SPEC.md](Drivers/SPEC.md) and [plan9.md](plan9.md) for the driver safeguards (they are newer than their first compiled form; the plan lists what has been tried on a device).

## How security works

The design borrows the shape of a PC's verified boot and shrinks it to a microcontroller. It is a software design and is honest about that: it is not a hardware root of trust. The chip's eFuse protections are designed, gated and off by default.

The pieces:

- **Everything that runs is a signed container**: a small header (what it is, who signed it, a hash of the contents) followed by the contents.
- **Each layer checks the next before running it.** The bootloader checks the boot package and the slot it jumps to. The kernel checks the modules, drivers and apps it loads.
- **The device holds only public keys.** Private keys stay on the machine of whoever signs. A device can check a signature but cannot make one.
- **Keys have roles, and a key only verifies what its role allows.** A leaked developer key does not give anyone boot-level trust.

| Role | Signs |
|---|---|
| boot | the bootloader, boot package, KittenOS, kernel, recovery loader |
| system | the modules, the app manager, the runtimes, official system apps |
| owner | apps that hold the highest permission |
| developer | third-party apps and user drivers |
| vendor | modules and apps from a third party, by certificate (designed) |

The chain, from the top:

```
public keys compiled into the bootloader
   |
   +--> the trust block in purrcfg (signed): security mode, developer mode, locks
   |
   +--> boot package, KittenOS, kernel, recovery loader      (boot key)
           |
           +--> modules, drivers, system apps                 (system key)
                   |
                   +--> apps and user drivers                 (developer key, or a
                                                               repository's signed index)
```

The policy, `secure_mode`, decides what a failed check does:

| Check result | `off` | `warn` (default) | `enforce` |
|---|---|---|---|
| verified | boot | boot | boot |
| failed | boot | boot, warning recorded | fall back to KittenOS |

Other rules worth knowing:

- Changes to the trust state (replacing a key, changing the mode) are signed requests once the mode is `warn` or `enforce`. Turning verification on is allowed only if the key bag already holds a valid key for each role, so a person cannot lock themselves out.
- The one-shot flags are cleared before they are acted on, so a crash cannot trap the device in a loop.
- **Developer mode** is the controlled way to loosen things for development: a development server on the local network can supply the system, and apps may be accepted without a trusted signature if that switch is on. Developer mode is never allowed to touch modules, drivers or the kernel's own trust.
- **A locked device is never weakened from the device.** Once a device is dev-locked, no setting on it can undo that.
- Private keys are password-protected on disk and live outside the repository. The password is given to one signing command at a time and is never accepted as a command-line argument.

What this does not protect against: someone who can rewrite the flash with a programmer can replace the bootloader. The threat model is written out tier by tier (remote, local, physical, laboratory) in the security guide.

More: [SECURITY.md](SECURITY.md) (read it front to back, or look things up), [Keys/SPEC.md](Keys/SPEC.md), [Permissions/SPEC.md](Permissions/SPEC.md), [Users/SPEC.md](Users/SPEC.md).

## How apps are loaded and run

Apps are small freestanding programs (no C library; they use only what the system hands them) in a signed package, loaded into the device's external memory (PSRAM) and run in a sandbox.

```
  a signed package (.cat): header + relocatable code + the app's manifest
        |
        |   put on the device by: the Developer Portal in the shell, or the Store
        v
  launch
        |
        v
  admission     the app manager asks the runtime how much memory the app needs and
        |       whether it fits while a reserve stays free for the system; if not,
        |       the least important apps are stopped to make room; if it still does
        |       not fit, the launch fails and says why
        v
  verify        signature and role are checked; revoked keys are refused
        v
  load          the code is relocated into PSRAM and mapped as executable;
        |       the data block is allocated once and does not grow
        v
  start         the app gets its own task (a stack of 8 KB in fast internal memory)
        |       and its on_create() is called with an environment
        v
  run           the app asks for system services by name and version
        |       (catcalls): console, ui, files, sched, system, power, devices, ...
        |       events (taps, typing, timers, permission answers) arrive in on_event()
        v
  stop          every widget, timer and task the app made is torn down for it
```

Things that follow from that design:

- **Catcalls are versioned tables of functions.** An app asks for `ui` at version 3 and gets either that table or nothing on an older system, so a new app can degrade cleanly. The app never holds a pointer into the system; windows and widgets are small integer handles.
- **Apps cannot draw or touch hardware directly.** The `ui` catcall hands the system a request; the system owns every widget and applies the theme, so an app looks like the rest of the system without styling code.
- **Dangerous permissions are asked at first use.** A call that needs one answers "ask" until the user decides; the app then gets an event with the answer and tries again. Background apps are never asked: they are denied unless allowed earlier.
- **An app belongs to a class** (unprivileged or privileged) that decides what it may request.
- **Fast internal memory is the scarce resource.** Each app task takes 8 KB of it, which is why the number of apps that can run at once is whatever memory allows, with the least important stopped first.
- **Apps are installed, not built in.** Only Settings is built into the shell, apart from the Store and its companion. Everything else, including Files, Clock, Notes and Photos, is a package installed through the Developer Portal or the Store.

More: [AppRuntime/SPEC.md](AppRuntime/SPEC.md), [AppManager/SPEC.md](AppManager/SPEC.md), [Catcalls/SPEC.md](Catcalls/SPEC.md), [Permissions/SPEC.md](Permissions/SPEC.md), [AppSDK/SPEC.md](AppSDK/SPEC.md), [Repositories/SPEC.md](Repositories/SPEC.md); the catcall tables themselves are in [PurrOS/components/coreos/include/purr_catcalls.h](PurrOS/components/coreos/include/purr_catcalls.h). The apps shipped as examples are in [Apps/](Apps/).

## How the system installs and updates itself

A release is a folder of signed files plus manifests that describe them. Three manifests carry the same fields (component, version, file, size, SHA-256, and for drivers the bus ABI range):

| Manifest | Lists |
|---|---|
| `recovery.manifest` | bootloader, boot package, kernel, KittenOS, recovery loader, picture pack |
| `modules.manifest` | the system modules (about, appmgr, apps, fs, login, netinstall, sysinfo, wifi, and others) |
| `drivers.manifest` | the downloadable drivers |

The sequence on a blank or repaired device:

```
recovery loader  --(Wi-Fi)-->  read recovery.manifest
   |  for each component:  download -> check size and SHA-256 -> verify signature
   |                       -> erase the slot -> write -> read back and compare
   v
KittenOS (recovery system) first run  -> setup wizard: account, time zone, legal text
   |  stage 2: the modules, then the picture pack, then the Device Manager can add drivers
   v
restart into the kernel (the full system)
```

Where the files come from:

- **The official release**, over HTTPS, by default.
- **A development server** on the local network, when developer mode is on. The build and signing tool serves a folder: `python3 purrstrap/purrstrap.py dev serve --dir out/devrepo --port 47801`. This is how new builds are tried on a real device without a release.
- Developer mode is deliberately **off after a fresh install** and has to be switched on once the system is running. That is a safety property, not a bug: after a power loss during an install the device falls back to the official source.

A release can be **withdrawn** by its publisher; devices then stop installing it and report it if they are running it.

More: [OTA/SPEC.md](OTA/SPEC.md), [Install/SPEC.md](Install/SPEC.md), [RecoveryLoader/SPEC.md](RecoveryLoader/SPEC.md), [Network/SPEC.md](Network/SPEC.md), [Repositories/SPEC.md](Repositories/SPEC.md), [purrstrap/SPEC.md](purrstrap/SPEC.md), [plan3.md](plan3.md) (release channels and the rollback rules).

## The user interface

The shell draws with LVGL 9 on the 320 by 240 display, and the keyboard and trackball work throughout.

- **The Android-style shell** follows the look of Android 2.2 (Froyo): a launcher with a dock, a status strip with signal, Wi-Fi and battery, a notification shade, a lock screen with a slide-to-unlock control, a first-run wizard, and an app grid. Its icons come from the Android Open Source Project (see [Thanks and credits](#thanks-and-credits)).
- **The Windows 10 Mobile style shell** recreates the feel of that interface with tiles and a sign-in screen.
- **The picture pack** (icons, wallpapers) lives in its own partition and is installed by the setup wizard, so the firmware itself stays small.

More: [UI/SPEC.md](UI/SPEC.md), [UI/SHELL-OPTIMIZATION.md](UI/SHELL-OPTIMIZATION.md), [plan7.md](plan7.md).

## The developer kit and the emulator

The developer kit (`sdk/`) is a download that lets one person build, sign and package any part of the system without this repository or ESP-IDF: a command line (`purr.py`), a small desktop app (Studio), board files, drivers, templates and guides. Its design is in [sdk/DESIGN.md](sdk/DESIGN.md) and its first guides start at [sdk/docs/00-start.md](sdk/docs/00-start.md). The plan for bringing it up to release quality, with the emulator inside it, is [showtime1.md](showtime1.md).

The emulator is a separate companion project: a Rust application around Espressif's QEMU fork that runs the ESP32-S3 with a display, touch, keyboard, SD card and a virtual Ethernet controller, so the system and the apps can be run without the hardware. Wiring it into the kit's run button is the first part of the plan above.

## Building and testing

The firmware builds with ESP-IDF 5.3.5.

```
. $IDF_PATH/export.sh
ninja -C out/<build directory>      # a full build takes over ten minutes
```

Fast tests that need no device:

```
cd PurrOS/components/coreos/test/host && IDF_PATH=$IDF_PATH python3 run.py [name]   # the C host tests
cd purrstrap && python3 -m unittest discover -s tests                               # the build and signing tool
cd sdk && python3 -m unittest discover -s tests -t tests                            # the developer kit
```

Signing needs the private keys, which are not in the repository. The passphrase is supplied in the environment of the one command that signs, and is never written to a file, a script or a commit message.

## A map of the repository

| Folder | What is in it |
|---|---|
| [PurrOS/](PurrOS/) | The firmware: the kernel, the core services, the shells, the app runtime, the main program. |
| [bootloader/](bootloader/) and [bootpkg/](bootpkg/) | The second-stage bootloader and the boot package. |
| [purrstrap/](purrstrap/) | The build, packaging and signing tool, including the development server. |
| [Apps/](Apps/) | The sample and system apps (Clock, Files, Photos, Notes, Devices, the Store and others). |
| [Modules/](Modules/) | The sources of the system modules and the drivers. |
| [sdk/](sdk/) | The developer kit. |
| [UI/](UI/) | The interface specification and the Android-style artwork. |
| [tools/](tools/) | Release, hardware and virtual machine scripts. |
| [Keys/](Keys/), [Permissions/](Permissions/), [Users/](Users/) | Specifications for keys, permissions and accounts. |
| `*/SPEC.md` | One specification per area; the list is under [Where to read next](#where-to-read-next). |
| `plan1.md` to `plan11.md`, [showtime1.md](showtime1.md) | The working plans: what was decided, what was built, what is still open. |

## Where to read next

Specifications, one per area:

| Area | Document |
|---|---|
| Security overview | [SECURITY.md](SECURITY.md) |
| Bootloader and boot package | [bootloader/SPEC.md](bootloader/SPEC.md) |
| The system | [PurrOS/SPEC.md](PurrOS/SPEC.md) |
| Recovery system and loader | [KittenOS/SPEC.md](KittenOS/SPEC.md), [RecoveryLoader/SPEC.md](RecoveryLoader/SPEC.md) |
| Installing and updating | [Install/SPEC.md](Install/SPEC.md), [OTA/SPEC.md](OTA/SPEC.md) |
| Keys, permissions, accounts | [Keys/SPEC.md](Keys/SPEC.md), [Permissions/SPEC.md](Permissions/SPEC.md), [Users/SPEC.md](Users/SPEC.md) |
| Apps | [AppRuntime/SPEC.md](AppRuntime/SPEC.md), [AppManager/SPEC.md](AppManager/SPEC.md), [AppSDK/SPEC.md](AppSDK/SPEC.md), [Catcalls/SPEC.md](Catcalls/SPEC.md), [CatFormat/SPEC.md](CatFormat/SPEC.md) |
| Drivers and boards | [Drivers/SPEC.md](Drivers/SPEC.md), [Boards/SPEC.md](Boards/SPEC.md), [Modules/SPEC.md](Modules/SPEC.md) |
| Network and repositories | [Network/SPEC.md](Network/SPEC.md), [Repositories/SPEC.md](Repositories/SPEC.md), [Transfer/SPEC.md](Transfer/SPEC.md) |
| User interface | [UI/SPEC.md](UI/SPEC.md) |
| The build and signing tool | [purrstrap/SPEC.md](purrstrap/SPEC.md) |
| The developer kit | [sdk/DESIGN.md](sdk/DESIGN.md) |

The working plans record decisions and progress, and they are the place to see what is built, what is only compiled, and what is still open: [plan1.md](plan1.md) (boot chain), [plan3.md](plan3.md) (hardening and release channels), [plan5.md](plan5.md) (the developer kit), [plan6.md](plan6.md) (the app runtime), [plan7.md](plan7.md) (the shells), [plan8.md](plan8.md) (repositories and the Store), [plan9.md](plan9.md) (the interface, files and driver work), [plan10.md](plan10.md) (power), [plan11.md](plan11.md) (a virtual display link), and [showtime1.md](showtime1.md) (the developer kit release and the full documentation set).

A complete written guide to the whole repository, folder by folder, is part of [showtime1.md](showtime1.md) and will live under `docs/`.

To report a security problem, see [SECURITY.md](SECURITY.md) section 16.

## Licence

PURR OS is free software under the **GNU General Public License, version 3**. The licence text is in [LICENSE](LICENSE).

The Android 2.2 (Froyo) style shell is released under the **Apache License, Version 2.0**, the licence the Android Open Source Project itself uses, in respect of that project. That covers the shell's source (the `purr_android` files in [PurrOS/main/](PurrOS/main/), each marked with an SPDX line) and all the artwork in [UI/android-assets/](UI/android-assets/), both the icons that come from the Android Open Source Project and the original art made for PURR OS. The licence text and the Android notice are in [UI/android-assets/NOTICE](UI/android-assets/NOTICE). The look of the shell was recreated for PURR OS rather than copied from Android's source, and it is offered under the same licence by choice. The rest of the project stays under the GPL.

Third-party material that the system uses, with its licence and what was changed, is listed in [THIRD_PARTY.md](THIRD_PARTY.md).

## Thanks and credits

Thank you to everyone who has helped: the people who tested on real hardware, who reported what broke, and who shared what they knew. Much appreciated.

Particular thanks to the projects this one stands on, each used under its own licence:

- **The Android Open Source Project.** The icons of the Android-style shell are theirs, from Android 2.2 (Froyo), under the Apache License 2.0, copyright The Android Open Source Project. The shell is licensed under Apache 2.0 as well, out of respect for the project whose design it follows. Android's decision to be open is the reason a project like this can exist.
- **Espressif Systems.** ESP-IDF 5.3.5 and the components it carries (FreeRTOS, lwIP, mbed TLS and others, each under its own licence, most of them Apache 2.0 or MIT), and the ESP32-S3 itself. The emulator is built around Espressif's fork of QEMU (QEMU is GPL version 2).
- **LVGL**, the graphics library behind every screen (MIT), version 9.2.
- **Felix Biego**, for windows-10-mobile-lvgl (MIT), which the Windows 10 Mobile style shell is modelled on. Some of the imagery in that project belongs to others and is placeholder art in this repository, as [THIRD_PARTY.md](THIRD_PARTY.md) says plainly; it is to be replaced before any release.
- **littlefs**, the power-loss-safe file system (BSD 3-clause, copyright Arm Limited and the littlefs authors).
- **LilyGO**, for the T-Deck Plus hardware.
- **The Rust and Python communities**, whose libraries the build tool, the developer kit and the emulator rely on, each under its own licence.

If something you wrote or made is used here and is not credited properly, please say so and it will be fixed.
