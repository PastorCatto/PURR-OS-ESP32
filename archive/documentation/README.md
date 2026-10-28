# PURR OS documentation

This folder is the user and developer documentation for the **rewritten PURR OS core**: the
bootloader, boot package, kernel, CoreOS, KittenOS, the recovery loader, the module system,
accounts, networking, the update and recovery paths, and the tooling (`purrstrap`) that builds
and signs all of it.

It covers what exists on the `PURR2.0` branch as of **2026-09-30**. It deliberately does **not**
cover `archive/`, `PURR-OS-0.11/`, `CatReleases/`, `releases/` (the old DP-era code and
binaries), and it does not document a graphical UI, because none exists yet.

The per-chunk `SPEC.md` files in the repo remain the design record. These documents are different
in purpose: they tell you **how to use and extend what is actually built**, and they say plainly
when something in a spec is designed but not built.

## How to read this documentation

Every feature in these documents carries a status tag. Trust the tags.

| Tag | Meaning |
|-----|---------|
| **[WORKS]** | Implemented, and either verified while writing these documents (host tests, purrstrap runs) or recorded in the specs as proven on real T-Deck Plus hardware. |
| **[PARTIAL]** | Implemented but incomplete, or implemented with a known gap. The gap is stated next to it. |
| **[DESIGNED]** | Written in a `SPEC.md` but **not built**. You cannot use it today. It is described so you know where things are going. |
| **[PROBLEM]** | Something in the code or specs that looks wrong or inconsistent. Each one is numbered in [FINDINGS.md](FINDINGS.md). |

What was and was not independently verified is spelled out in
[Verification and limits](#verification-and-limits) below.

## Start here

| If you want to... | Read |
|-------------------|------|
| Understand what all the parts are and how they fit | [01 Concepts and architecture](01-concepts-and-architecture.md) |
| Find a file or folder | [02 Repository map](02-repository-map.md) |
| Set up a machine to build PURR OS | [03 Development environment](03-development-environment.md) |
| Build and flash a device | [04 Building and flashing](04-building-and-flashing.md) |
| Know what happens from power-on to the prompt | [05 Boot process, flash layout and purrcfg](05-boot-process-and-flash-layout.md) |
| Use the device shell | [06 Using the shell](06-using-the-shell.md) |
| Set up accounts, passwords, admins | [07 User accounts](07-user-accounts.md) |
| Connect Wi-Fi | [08 Networking](08-networking.md) |
| Work with files and the filesystem | [09 Filesystem](09-filesystem.md) |
| Update, install over the network, recover a dead device | [10 Updates, network install and recovery](10-updates-network-install-and-recovery.md) |
| Install and manage apps | [11 Apps](11-apps.md) |
| Add a command to the OS | [12 Writing modules](12-writing-modules.md) |
| Write or change a hardware driver | [13 Writing drivers](13-writing-drivers.md) |
| Add or change a display | [14 Adding a display](14-adding-a-display.md) |
| Add a second display | [15 Adding a second display](15-adding-a-second-display.md) |
| Port to a new board | [16 Adding a board](16-adding-a-board.md) |
| Understand keys, roles, and what may sign what | [17 Keys and signing](17-keys-and-signing.md) |
| Make your own branded/forked PURR OS with your own keys | [18 Making your own PURR OS](18-making-your-own-purr-os.md) |
| Fix something that broke | [19 Troubleshooting and scenarios](19-troubleshooting-and-scenarios.md) |
| Run the automated tests | [20 Testing](20-testing.md) |
| Look up a byte layout, ABI, or limit | [21 Reference: formats, ABIs, limits](21-reference-formats-abis-limits.md) |
| Look up a purrstrap command | [22 purrstrap reference](22-purrstrap-reference.md) |
| Find a recipe for a specific task | [23 Cookbook](23-cookbook.md) |
| See what is planned but not built | [24 Not built yet](24-not-built-yet.md) |
| See what I found wrong while writing this | [FINDINGS.md](FINDINGS.md) |
| Look up a term | [Glossary](glossary.md) |
| Copy a compiled example (module, driver, patch, tool) | [examples/](examples/README.md) |

## The short version

PURR OS is a ground-up rewrite for ESP32 boards. The primary board is the **LilyGO T-Deck Plus**
(ESP32-S3, 16 MB flash, 8 MB PSRAM, 320x240 display, keyboard). Its parts, in boot order:

1. A custom **bootloader** that decides what to boot and falls back automatically.
2. A signed **boot package** that draws the boot menu.
3. A **kernel** slot (hardware, filesystem), or in today's builds the whole PURR OS image.
4. **CoreOS**, the shared platform with the shell, accounts, networking, and the module loader.
5. **Modules**: individual commands loaded from the filesystem at boot, verified by signature.
6. **KittenOS**, a small recovery system, and a **recovery loader** that can download a new
   one over Wi-Fi when everything else is dead.

Everything that holds code is a signed container, verified before it runs. Keys are separated by
role, so a key that may sign a command module may not sign the bootloader.

## Verification and limits

These documents were written by reading every SPEC and the current source of the bootloader,
boot package, kernel, CoreOS, the shell, the module loader, the standalone kernel, the recovery
loader, and purrstrap, and by running what could be run on a PC.

**Actually run while writing:**

- `python purrstrap/purrstrap.py list` and `coreos check`.
- The purrstrap unit tests: 72 tests, all pass.
- The CoreOS host C tests (`python run.py`): 16 suites, all pass.
- The key workflow end to end in a scratch directory: `keys generate`, `export-public`, `sign`,
  `verify`, `inspect`, plus `modules build` and `modules index` on a sample module.
- A `purrcfg` image built in Python and loaded by the real C `purr_cfg_load` on the host
  (used in [05](05-boot-process-and-flash-layout.md)).

**Not re-verified here:** anything that needs the device. No board was flashed while these were
written. Statements like "confirmed on hardware" are quoted from the project's own specs and
history, not re-observed. Where the specs claim something that the source contradicts, the
documents follow the source and the contradiction is listed in [FINDINGS.md](FINDINGS.md).

**Compiled, not run:** the examples in [examples/](examples/README.md) (a second-display patch, an ILI9341 and an SSD1306 driver, an `i2cscan` command, a new-board patch). They were built into a
scratch copy of the tree with the real toolchain, in `C:\pscr`, and **have not been run on hardware**. Chapters 13 to 16 say which parts of each.

Nothing was committed, and no source file outside this `documentation/` folder was changed. (Temporary build directories under `Modules/build/` created by test builds were removed, and the scratch copy
was built outside the repository.)
