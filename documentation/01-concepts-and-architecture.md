# 01. Concepts and architecture

This chapter is the map. Read it once and the rest of the documentation will make sense.

- [What PURR OS is](#what-purr-os-is)
- [The layers](#the-layers)
- [What is real today versus the target design](#what-is-real-today-versus-the-target-design)
- [Names that are easy to mix up](#names-that-are-easy-to-mix-up)
- [Profiles: one source tree, three personalities](#profiles-one-source-tree-three-personalities)
- [Where code lives in flash](#where-code-lives-in-flash)
- [The two call tables: how modules reach the system](#the-two-call-tables-how-modules-reach-the-system)
- [The trust model on one page](#the-trust-model-on-one-page)
- [Board tiers](#board-tiers)
- [Design rules the project follows](#design-rules-the-project-follows)

## What PURR OS is

PURR OS is an operating system for ESP32-family boards, rewritten from scratch in September 2026
after the earlier line (the "DP" releases, now in `archive/` and `CatReleases/`) became messy.
The rewrite has three governing ideas:

1. **Design first.** Each part has a `SPEC.md` next to its code, written before the code. The code
   and the spec are supposed to move together.
2. **Own the whole chain.** The project controls everything from the second-stage bootloader up,
   and everything that runs is signed.
3. **A small fixed core with everything else replaceable.** Commands are loadable modules,
   apps are separate signed files, and a recovery path exists for every failure.

The first and best-supported device is the **LilyGO T-Deck Plus**. Everything in these documents
that mentions hardware is about that board unless it says otherwise.

## The layers

This is the **target** architecture, from `PurrOS/SPEC.md`. Section
[What is real today](#what-is-real-today-versus-the-target-design) says how far along each
layer is.

```
  ROM boot loader            (in the chip; not ours)
      |
  PURR bootloader            second stage. Verifies and loads the boot package, picks
      |                      what to boot, applies the failure-count ladder.
      |
  boot package (bootpkg)     signed menu program. Draws the boot menu, returns a choice.
      |
  kernel   OR   KittenOS   OR   recovery loader
      |           (recovery)       (internet recovery)
      |
  CoreOS                     shared platform: shell engine, accounts, Wi-Fi, verification,
      |                      module loader, update logic
      |
  modules                    one signed file per group of commands, loaded at boot
      |
  apps (.cat)                signed app packages (installable today, not yet runnable)
```

Two ideas to hold on to:

- **Recovery must not depend on what it recovers.** KittenOS carries its own copy of the
  platform code and never loads the kernel or CoreOS files, so it works when they are broken.
  The recovery loader carries even less and can fetch a new KittenOS from the internet.
- **Everything that holds code is a signed container.** A single header format (the "PURR image
  container", see [21](21-reference-formats-abis-limits.md#image-container)) is used for the boot
  package, kernel, CoreOS, modules, KittenOS, and apps.

## What is real today versus the target design

The most important thing to understand before working on the system is that the **kernel/CoreOS
split is partly built**. The specs describe the finished design in the present tense in several
places. This table says what actually exists.

| Layer | Target design | State on 2026-09-30 |
|-------|---------------|---------------------|
| Bootloader | Custom second stage, verifies images, failure-count ladder | **[WORKS]** Custom bootloader runs on the T-Deck Plus. Verifies the **boot package** by signature. For `kernel`, `kittenos`, and `loader` it only checks that the first byte looks like an ESP app image (`0xE9`); real signature verification there is **[DESIGNED]**. |
| Boot package | Signed menu, picks a slot | **[WORKS]** Draws the menu on the real display with a bit-banged driver. |
| Kernel | Small flashed partition: hardware + filesystem, then loads CoreOS from a file | **[PARTIAL]** A standalone kernel binary exists (`Kernel/`, about 330 KB) and was proven to load a tiny CoreOS file into PSRAM and run it. It does **not** load the real CoreOS. Today the real system is built as **one monolithic image**: kernel, CoreOS, shell, and modules loader linked together (`PurrOS/`, about 1.08 MB). |
| CoreOS | Relocatable file in `/boot`, loaded by the kernel | **[PARTIAL]** All 16 source files were built as one relocatable blob and proven loadable. The running shell is still the monolith. The file-swap mechanism for updating it (`purr_swap`) is built and host-tested. |
| Shell | Pipes, redirection, variables, scripts, job control | **[PARTIAL]** A plain command line: quoted words and a command table. No pipes, redirects, `;`, `&&`, variables, or scripts ([F-04](FINDINGS.md#f-04)). |
| Modules | Loadable commands, verified, quarantined if bad | **[WORKS]** Eight modules in two groups load from `/system` and `/kernelmods` at boot. |
| Accounts | Multi-user, root locked, `su` for admins | **[WORKS]** Create accounts, log in, `su`, `passwd`, growing lockout delay. Gaps in [07](07-user-accounts.md). |
| Wi-Fi | Saved networks, auto-reconnect | **[WORKS]** |
| Update/recovery | Network install, file swap, internet recovery | **[WORKS]** in code; the pieces are individually proven. A full live end-to-end run against a published release is recorded as not yet done. |
| Apps | `.cat` packages run by a runtime with catcalls | **[PARTIAL]** You can install, list, inspect, and remove signed `.cat` packages. **Nothing can run them**: the runtime, SDK, and catcalls are **[DESIGNED]**. |
| Drivers | Plug-and-play registry, board data files, user drivers | **[DESIGNED]**. Today there is one hardwired display driver (ST7789) and one keyboard reader. See [13](13-writing-drivers.md). |
| UI | LVGL/MiniWin, `ui` catcall, themes | **[DESIGNED]**. There is a text console only. |
| Second board (CYD 2.4C) | Monolithic tier | **[PROBLEM]** Listed everywhere, **not buildable** ([F-01](FINDINGS.md#f-01)). |

## Names that are easy to mix up

| Name | What it is |
|------|-----------|
| **PURR OS** | The normal system: the `full` profile build of `PurrOS/`. Shell, Wi-Fi, accounts, apps, everything. Prompt reads `name@PURR OS>`. |
| **KittenOS** | The recovery system: the `recovery` profile of the **same** source tree. Smaller feature set, never normally updated, prompt reads `name@KittenOS>`. It is a *different image*, not a mode of PURR OS. |
| **PURR Loader / recovery loader** | The `minimal` profile. No shell, no filesystem. Connects to Wi-Fi and installs KittenOS. Shown as "PURR internet recovery". Lives in the `loader` partition. |
| **Kernel** | In the target design: the small image in the `kernel` partition. In practice the word is used for two things: the standalone `Kernel/` project, and the partition (which today may hold a full monolithic PURR OS). |
| **CoreOS** | The shared platform code: `PurrOS/components/coreos`. Compiled into every profile. In the target design also a relocatable file in `/boot`. |
| **Module** | A signed relocatable file in `/system` that adds commands. Talks to CoreOS through the *core table*. |
| **Kernelmod** | A signed relocatable file in `/kernelmods` that adds commands. Talks through the *kernel table*. A prototype of the kernel/CoreOS boundary, living in the same monolith for now. |
| **App (`.cat`)** | A signed package for end-user programs. Installed under `/home/<user>/apps`. |
| **Profile** | One of `full`, `recovery`, `minimal`. Chosen at build time. |
| **Board** | A concrete device (`tdeck_plus`). Decides the chip, pins, partition table. |
| **Tier** | *Modular* (>= 4 MB flash, >= 2 MB PSRAM, native USB) or *monolithic* (everything linked into one image). |

> **File extension warning [PROBLEM, [F-16](FINDINGS.md#f-16)].** Modules and kernelmods are
> stored with the extension `.cat`, which is *also* the app package extension. The specs call
> system modules `.kitt`. The loader matches on `.cat`. They are different things inside: a
> module is container `image_type 3`, an app is `image_type 4`. The extension does not tell you
> which you are holding; use `purrstrap keys inspect --path <file>`.

## Profiles: one source tree, three personalities

All three profiles are built from the single project `PurrOS/`. `purrstrap coreos build` selects
a profile by passing `sdkconfig.profile.<name>` to ESP-IDF. The Kconfig option is
`PURR_PROFILE`. In code you will see `CONFIG_PURR_PROFILE_FULL`, `_RECOVERY`, and `_MINIMAL`.

| | `full` | `recovery` | `minimal` |
|--|--------|------------|-----------|
| Personality | PURR OS | KittenOS | PURR Loader |
| Output file | `build/tdeck_plus-full/purros.bin` | `build/tdeck_plus-recovery/purros.bin` | `build/tdeck_plus-minimal/purros.bin` |
| Display + keyboard | yes | yes | yes |
| Login | yes | yes, except it is skipped when no kernel image is present | no |
| Shell + modules + accounts | yes | yes | no |
| Wi-Fi | yes | yes | yes |
| Swap of staged update files | no (`netinstall` only *stages*) | yes, runs at boot | no |
| Auto-format blank `root` | no | yes | no |
| Stage 2 of recovery install | runs if flagged | runs if flagged | no |
| Recovery menu (press S / F) | no | no | yes |

Every output is named `purros.bin` regardless of profile, because the ESP-IDF project name is
`purros`. Keep the profile directory name in your head, not the file name.

## Where code lives in flash

The T-Deck Plus partition table (`PurrOS/partitions/tdeck_plus.csv`, kept identical to
`bootloader/partitions.csv` and `Kernel/partitions.csv`):

| Name | Offset | Size | Holds |
|------|--------|------|-------|
| bootloader | `0x0` | n/a | The custom second stage (flashed at 0 on ESP32-S3). |
| (partition table) | `0x8000` | | |
| `nvs` | `0x9000` | 24 KB | ESP-IDF NVS. Used by Wi-Fi internals; not by the bootloader. |
| `otadata` | `0xF000` | 8 KB | Left over for ESP-IDF compatibility. **Not used for selecting what to boot.** |
| `phy_init` | `0x11000` | 4 KB | RF calibration data. |
| `bootpkg` | `0x12000` | 48 KB | The signed boot package **container** (header included). |
| `purrcfg` | `0x1E000` | 8 KB | Boot configuration, two 4 KB A/B copies. |
| `kittenos` | `0x20000` | 1.5 MB | App image, type `factory`. The safe slot. |
| `kernel` | `0x1A0000` | 1.875 MB | App image, subtype `ota_0` (kept so the stock IDF loader can jump to it). The primary boot slot. |
| `loader` | `0x380000` | 1.5 MB | App image, type `test`. The recovery loader. |
| `netrec` | `0x500000` | 4 KB | The "recovery network" record: last Wi-Fi network, in plain text. |
| `root` | `0x501000` | about 11 MB | LittleFS. Everything else: `/system`, `/kernelmods`, `/etc`, `/home`, `/boot`. |

> The specs describe `rescue` (the loader) as about 1 MB and `kittenos` as 1 MB, `bootpkg` as
> 56 KB, and do not list `netrec` at all. The table above is what the CSV says
> ([F-07](FINDINGS.md#f-07)). Full explanation of each partition is in
> [05](05-boot-process-and-flash-layout.md).

**What goes in `kernel` and `kittenos` is in flux.** Each is just an ESP-IDF app slot, and any
profile build of `PurrOS/` will boot from either. A coherent arrangement that matches the
code's intent today:

| Slot | Intended content | Why |
|------|------------------|-----|
| `kernel` (primary) | A full-profile PURR OS monolith | The bootloader prefers it. Network install (`netinstall kernel`) writes here. |
| `kittenos` (safe) | The recovery-profile build | The bootloader falls back to it. Only the recovery loader may rewrite it. |
| `loader` | The minimal-profile build | Last resort. |

During development the fastest loop is to flash a full build directly into the `kittenos` slot
with `idf.py flash`, leaving `kernel` empty. That works because an empty `kernel` slot fails the
bootable check and the bootloader falls through to the factory slot. Just remember that you have
then replaced your recovery system. Details and recipes are in
[04](04-building-and-flashing.md).

## The two call tables: how modules reach the system

A module is compiled freestanding (`-nostdlib -ffreestanding`): no libc, no ESP-IDF, no FreeRTOS.
It cannot call anything by name. Instead its entry function receives a **pointer to a table of
function pointers** and keeps it. Everything the module can do is whatever that table offers.

There are two tables, one per boundary:

| Table | Header | Used by | Loaded from | Version constant |
|-------|--------|---------|-------------|------------------|
| **Core table** (`purr_core_table_t`) | `purr_module_abi.h` | *Modules*: `about`, `apps`, `wifi`, `netinstall` | `/system/*.cat` | `PURR_MODULE_ABI_VERSION` (4) |
| **Kernel table** (`purr_kernel_table_t`) | `purr_kernel_table.h` | *Kernelmods*: `sysinfo`, `fs`, `login`, `appmgr` | `/kernelmods/*.cat` | `PURR_KERNEL_TABLE_ABI_VERSION` (6) |

The kernel table is the prototype of the boundary between kernel and CoreOS. The idea: the day
CoreOS becomes a separately loaded file, it will call the kernel through exactly this table, so
the commands were converted onto it *first*, while still living in one binary. Thirty-one shell
commands have been moved this way; none remain inline in `commands.c` except `help` and
`plantmodules`.

Two rules shape both tables:

1. **Domain-level calls, not primitives.** A module is never handed a filesystem object, the
   key bag, a raw flash partition, or the keyboard. Things that touch secrets or hardware are
   one opaque call (`login_su`, `fs_format`, `net_install`, `app_install`, ...), and the real
   permission checks happen on the kernel side every time.
2. **Strict versioning.** The table version is checked when a module loads. A mismatch means the
   module is refused (and, for `/system` modules, quarantined). Every time the table grows, the
   version is bumped and every module must be **rebuilt and re-signed**. See
   [12](12-writing-modules.md).

## The trust model on one page

Detailed in [17](17-keys-and-signing.md). In brief:

- Every signed file is an **ECDSA P-256 / SHA-256** signature over a 173-byte header, which
  contains the SHA-256 of the payload.
- The signing key has a **role**: `boot`, `system`, `owner`, `developer`, `vendor`. A file only
  verifies if the key's role is allowed to sign that *kind* of file.
- Devices carry a **default key bag** compiled in (`purr_default_keys.c`): today exactly two
  public keys, `boot` (id 1) and `developer` (id 2).
- `purrcfg` holds `secure_mode`: `off` (accept unsigned), `warn` (the default), `enforce`. In
  practice, for **installs**, anything that fails verification is rejected unless mode is `off`; the
  **module loader always requires a valid signature**, in every mode; `enforce` additionally enforces version floors for staged updates.

What verifies what:

| Thing | Verified by | When | How strong |
|-------|-------------|------|-----------|
| Boot package | Bootloader | Every boot | Full signature (boot role) |
| `kernel`, `kittenos`, `loader` images | Bootloader | Every boot | **Magic byte only** |
| `kernel`/`loader`/`bootpkg` downloads | `netinstall` / recovery loader | Before writing | Full signature + hash + read-back |
| KittenOS download | Recovery loader | Before writing | Full signature + hash + read-back |
| Staged system files (`/boot/*.new`) | `netinstall`, then KittenOS again | Before and at swap | Full signature + version floor |
| Modules and kernelmods | CoreOS | Every boot | Full signature; bad ones are quarantined |
| Module downloads | `netinstall modules` | Before writing | Full signature + hash |
| Apps | `appinstall` | Before install, and on every `apps` scan | Full signature |

## Board tiers

| Board | Chip | Tier | Status |
|-------|------|------|--------|
| **LilyGO T-Deck Plus** | ESP32-S3, 16 MB flash, 8 MB octal PSRAM | modular (primary) | **[WORKS]** |
| **CYD 2.4C** (ESP32-2432S024C) | ESP32, 4 MB, no PSRAM | monolithic | **[PROBLEM]** named everywhere, not buildable ([F-01](FINDINGS.md#f-01)) |
| **Waveshare ESP32-S3-ePaper-1.54** | ESP32-S3 | modular | **[DESIGNED]**, spec only |

*Modular* boards load code from files into PSRAM. *Monolithic* boards have no PSRAM and cannot
do that, so everything is one image. Because modules are PSRAM-loaded, **modules and kernelmods
work only on boards with PSRAM** (the loader allocates with `MALLOC_CAP_SPIRAM`).

## Design rules the project follows

These come from the specs and the project's own working rules. They explain choices you will run
into.

- **Spec first.** If you change behaviour, update the `SPEC.md` in the same change.
- **Every user-facing feature needs a fallback.** A failed normal boot goes to KittenOS; a failed
  KittenOS goes to the recovery loader; a failed loader halts with a message.
- **No memory protection on ESP32 (original).** Apps and modules share one address space.
  Safety comes from signing, not isolation.
- **Unix-like by intent.** Small commands, text output, `/etc`, `/home`, exit codes.
- **Keys are cheap to swap.** Whichever key is in `signing_keys/` *is* the trusted one for your
  build. Nothing is permanently baked in except by your own releases. See
  [18](18-making-your-own-purr-os.md).
