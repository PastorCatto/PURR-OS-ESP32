# Glossary

## Short version

- Every PURR word in one place, in alphabetical order, each with a link to the chapter where it is used.
- If a word is missing, it is a gap in this file: add it in the same commit as the code that introduced it.
- Terms are explained the way they are used in this repository, not the way other projects use them.

## A to C

| Term | Meaning | Read more |
|------|---------|-----------|
| **Admission** | The app runtime's check that there is enough memory before it starts another app. Since 1.0.22 app stacks are in PSRAM by default (with a way back to internal RAM), but internal RAM is still the scarce resource. | [06-app-runtime.md](06-app-runtime.md) |
| **App** | A freestanding module (no libc) loaded into PSRAM at run time. Apps are sideloaded through the Developer Portal or the Store; only Settings is built into the shell. | [06-app-runtime.md](06-app-runtime.md), [16-apps.md](16-apps.md) |
| **Board** | A description of a piece of hardware (pins, display, keyboard, and so on), written as a `board.toml` and turned into a board file and a device profile. The reference board is `tdeck_plus`. | [19-boards.md](19-boards.md) |
| **Boot key** | The signing key (id 1) that signs the boot package, KittenOS, the loader, the kernel and the trust block. Its public half is compiled into the bootloader and the firmware. | [03-trust-and-keys.md](03-trust-and-keys.md) |
| **Boot package** (`bootpkg`) | The signed image in its own flash slot that holds the boot menu's branding. Built with `purrstrap bootpkg build`. | [01-boot-chain.md](01-boot-chain.md) |
| **Bootloader** | The PURR bootloader at the start of flash. It verifies the boot package and the slots before running them. Its over-the-air update is switched off. | [01-boot-chain.md](01-boot-chain.md) |
| **`.cat` file** | A signed package: a kernel, a loader, an app, a driver or a module, with a header and a signature trailer. | [05-catcall-abi.md](05-catcall-abi.md), [09-purrstrap.md](09-purrstrap.md) |
| **Catcall** | A call from an app into the system: the application binary interface (ABI) between apps and the kernel. Catcalls are versioned (for example the ui catcall), and a driver or app states the lowest ABI it needs. | [05-catcall-abi.md](05-catcall-abi.md) |
| **Channel** | A named update source with rules: `stable` enforces the version floor, `beta` allows rollback. | [08-updates-and-release-channels.md](08-updates-and-release-channels.md) |
| **Companion** | The small service that runs next to the Store (for example for the hourly update check). | [14-store-and-repositories.md](14-store-and-repositories.md) |

## D to K

| Term | Meaning | Read more |
|------|---------|-----------|
| **Developer key** | The signing key (id 2) for apps, modules and drivers. After the planned key rotation it becomes developer-mode only (see [plan3.md](../plan3.md)). | [03-trust-and-keys.md](03-trust-and-keys.md) |
| **Developer mode** | A device setting that allows sideloading from a dev server and the recovery loader's dev-server source. It is off after an unattended install, on purpose. | [03-trust-and-keys.md](03-trust-and-keys.md) |
| **Developer Portal** | The app on the device that finds an SDK server on the local network, asks you to trust it, and installs apps from it. | [17-sdk.md](17-sdk.md) |
| **Dev-locked** | A device whose trust block says it may never be loosened. A locked device is never weakened on the device. | [03-trust-and-keys.md](03-trust-and-keys.md) |
| **Dev server** | `purrstrap dev serve`: serves a folder (apps, recovery images, modules, drivers) to a device on the local network. The emulated device reaches it at `10.0.2.2`. | [08-updates-and-release-channels.md](08-updates-and-release-channels.md), [runbook.md](runbook.md) |
| **Driver** | A signed module that talks to hardware (sensor, radio, backlight) over the driver bus. The display, touch and keyboard drivers are built in and planted at start-up. | [07-drivers.md](07-drivers.md) |
| **Driver bus** | The table of functions a driver may call (I2C, SPI, UART, ADC and so on). A change to it means the embedded drivers must be rebuilt and signed again. | [07-drivers.md](07-drivers.md) |
| **Emulator** | A Rust application around Espressif's QEMU fork that runs the ESP32-S3 image with a display, touch, keyboard and SD card. It is a separate project; the kit bundles it. | [18-emulator-and-vm.md](18-emulator-and-vm.md) |
| **Flavor** | A build variant on top of a board and a profile: `vm` (wired network for the emulator), `vmdebug` (automatic sign-in). | [runbook.md](runbook.md), [18-emulator-and-vm.md](18-emulator-and-vm.md) |
| **Floor** | A version number a device records for a component. It refuses to install anything below it. Raised on healthy boots on the stable channel. | [08-updates-and-release-channels.md](08-updates-and-release-channels.md) |
| **Headless runner** | The emulator without a window, driven by a command file (`tap`, `drag`, `shot`, `dump`). Used for scripted runs. | [18-emulator-and-vm.md](18-emulator-and-vm.md) |
| **JumpingJaguar1, 2, 3** | The names of the official GitHub releases: 1 holds the recovery images, 2 the modules and drivers, 3 the official app repository. | [08-updates-and-release-channels.md](08-updates-and-release-channels.md), [14-store-and-repositories.md](14-store-and-repositories.md) |
| **Kernel** | The full operating system image in the `kernel` slot. It is what runs after the boot menu. | [04-kernel-and-services.md](04-kernel-and-services.md) |
| **KittenOS** | The small recovery system in the `kittenos` slot. It starts when the kernel cannot, and can install the system from the network. | [01-boot-chain.md](01-boot-chain.md) |
| **Kit** (the SDK kit) | The self-contained developer download: Python, the toolchain, `purrstrap`, the `purr` command and PURR Studio, and (from 1.1.0) the emulator. | [17-sdk.md](17-sdk.md) |

## L to Z

| Term | Meaning | Read more |
|------|---------|-----------|
| **Loader** (recovery loader) | The minimal system in the `loader` slot that installs or repairs the other slots from a manifest. | [01-boot-chain.md](01-boot-chain.md) |
| **Manifest** | A text file that lists files with their size and sha256: `recovery.manifest` (kernel, KittenOS, loader, boot package), `modules.manifest`, `drivers.manifest`. A file named in a manifest is fetched from the same folder. | [08-updates-and-release-channels.md](08-updates-and-release-channels.md) |
| **Module** | A signed, relocatable piece of code loaded at run time (an app, a driver, or a system module such as `wifi` or `fs`). | [06-app-runtime.md](06-app-runtime.md) |
| **Picture pack** (`uiassets`) | The signed component that carries the images the shells draw. Up to 1.0.28 it lived in its own 5 MB flash partition (`ui_assets`); in flash map v2 (1.0.29) it is a file in the LittleFS `root` (`/ui/<shell>/pack.bin`), installed by the setup wizard's second stage. | [12-picture-pack-and-fonts.md](12-picture-pack-and-fonts.md) |
| **Profile** | How much of the OS a build contains: `full` (kernel), `minimal` (loader), `recovery` (KittenOS). | [runbook.md](runbook.md) |
| **`purrcfg`** | The small flash partition (at `0xF000`) that holds the trust block, key overrides and floors. | [02-flash-map-and-partitions.md](02-flash-map-and-partitions.md) |
| **`purrstrap`** | The build and signing tool: packages components, signs them, builds boot packages and modules, and runs the dev server. | [09-purrstrap.md](09-purrstrap.md) |
| **PURR Studio** | The kit's app: drag in an image to see what it is and who signed it; tools for every part of the OS. | [17-sdk.md](17-sdk.md) |
| **Relocator** | The code that fixes up a loaded module's addresses. A `switch` in a module makes a jump table, which the relocator must treat as code. | [06-app-runtime.md](06-app-runtime.md) |
| **Repository** | A signed index of apps (F-Droid style). The official one is JumpingJaguar3; anyone can run their own. | [14-store-and-repositories.md](14-store-and-repositories.md) |
| **Sandbox** | The limits an app runs under: its permissions, its memory, its files. | [06-app-runtime.md](06-app-runtime.md) |
| **Shell** | The user interface the kernel starts: Android-style, Windows 10 Mobile style ("W10M") or iOS-style. All use the same catcalls. | [11-ui-stack-and-shells.md](11-ui-stack-and-shells.md) |
| **Slot** | A region of flash that holds one signed component (`bootpkg`, `kittenos`, `kernel`, `loader`). | [02-flash-map-and-partitions.md](02-flash-map-and-partitions.md) |
| **Store** | The built-in app store; it reads repositories and installs apps. Its companion is a separate service. | [14-store-and-repositories.md](14-store-and-repositories.md) |
| **Trust block** | The signed record in `purrcfg` that sets the device's security mode (for example `enforce`), whether developer mode is on, and whether the device is dev-locked. | [03-trust-and-keys.md](03-trust-and-keys.md) |
| **ui catcall** | The versioned set of calls through which apps draw and read input. Its version number tells a shell what it must support. | [11-ui-stack-and-shells.md](11-ui-stack-and-shells.md) |
| **VM flavor** | The firmware build for the emulator. It has a virtual wired network chip instead of Wi-Fi, and its recovery address points at the host's dev server. | [18-emulator-and-vm.md](18-emulator-and-vm.md) |
| **Withdrawn** | A flag on a release entry that makes every reader skip it, so a bad update can be stopped at once. | [08-updates-and-release-channels.md](08-updates-and-release-channels.md) |
