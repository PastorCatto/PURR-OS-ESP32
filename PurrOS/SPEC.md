# PurrOS base system spec (draft 0.6)

Overview of `PurrOS/`, the base system: how it boots, how it is layered, where things
live, and in what order it gets built. Each part has its own spec:

- `components/kernel/SPEC.md`: hardware, drivers and the filesystem. Starts first.
- `components/coreos/SPEC.md`: runs on the kernel. Handoff, self-verification,
  decisions, the shell engine, memory pressure.
- `../bootloader/SPEC.md`: the bootloader, the boot package and the shared formats.
- `../KittenOS/SPEC.md`: the recovery system, and the one that applies updates.
- `../purrstrap/SPEC.md`: the build and packaging tool.
- `../AppManager/SPEC.md`: installing and removing apps.
- `../AppRuntime/SPEC.md`: running apps: the manager, runtime modules, multitasking.
- `../AppSDK/SPEC.md`: writing and building apps.
- `../CatFormat/SPEC.md`: the `.cat` app file format.
- `../Permissions/SPEC.md`: allowing and denying access to hardware.
- `../OTA/SPEC.md`: getting updates onto a device.
- `../Keys/SPEC.md`: keys, roles, vendor certificates and signing.
- `../Catcalls/SPEC.md`: the device interfaces apps use.
- `../Network/SPEC.md`: Wi-Fi and the network service.
- `../UI/SPEC.md`: the `ui` catcall, themes and the system UI.
- `../MicroPython/SPEC.md`: the second runtime, for Python apps.
- `../Transfer/SPEC.md`: moving apps between devices, and the app storage app.
- `../Boards/SPEC.md`: the supported boards and how a board is added.
- `../Install/SPEC.md`: network install and hardware discovery.
- `../Drivers/SPEC.md`: driver packs, individual drivers and pinned drivers.
- `../Users/SPEC.md`: user accounts, login and remote access identity.
- `../RecoveryLoader/SPEC.md`: internet recovery of KittenOS.
- `../ModuleSpike/SPEC.md`: the experiment that proves loading from the filesystem.

This file holds only what spans parts. If something is specified in a part's spec, it
is not repeated here.

## 1. The model

The layout follows Linux. A small **boot area** in raw flash holds just enough to start
and to recover, like an EFI partition. **Almost everything else lives in one root
filesystem** (LittleFS): CoreOS, the drivers bundle, AppManager, the runtimes, apps,
configuration, logs, and the backup and staged copies used for updates. Anything in the
root filesystem can be replaced independently.

**Updated 2026-09-28 (draft 0.6):** the kernel moved back into the boot area, as its own
flash partition, alongside KittenOS and the recovery loader. Earlier drafts of this
document (and `ModuleSpike`/`Modules` SPEC.md, when they were written) assumed the kernel
itself could be a relocatable PSRAM-loaded file like CoreOS and every module built since
— it can't: the kernel's job is bringing up FreeRTOS and the hardware in the first place,
and a full FreeRTOS/ESP-IDF app cannot execute from a PSRAM copy the way a module can
(settled earlier in the rewrite; see `Modules/SPEC.md`). CoreOS is not affected by this —
it already only ever loads *after* the kernel has FreeRTOS and hardware running, the same
precondition every module already relies on, so CoreOS-as-a-PSRAM-blob stays exactly the
plan, just now explicitly built on the same mechanism `Modules/SPEC.md` proved on
hardware, not a separate unproven idea.

| Thing | Where it lives | Loaded by | Updated |
|-------|----------------|-----------|---------|
| Bootloader | raw flash, fixed | ROM | rarely |
| Boot package | boot area | bootloader | rarely |
| KittenOS | boot area | bootloader, when triggered | not in normal use |
| Recovery loader | boot area (modular boards) | bootloader, when KittenOS fails | almost never |
| Kernel | boot area (modular boards), its own flash partition | boot package | rarely; a raw partition write from either full PURR OS or KittenOS (`net_install kernel`, `../OTA/SPEC.md` section 3), the same partition-level path the boot package and the recovery loader use, not the file-swap in section 6.1 |
| CoreOS | root filesystem, `/boot` | kernel | independently |
| Device config bundle | root filesystem, `/boot` | kernel and CoreOS | with the drivers it holds; locked to the release keys (`components/kernel/SPEC.md` section 13) |
| AppManager | root filesystem, `/boot` | CoreOS | rarely |
| Runtime modules | root filesystem, `/boot` | CoreOS | independently: `catrt` first, later `mpyrt` |
| Apps (`.cat`) | root filesystem, `/apps` | app runtime | independently, and copied between devices |

Every file in the root filesystem that holds code is a signed container
(`bootloader/SPEC.md` section 3) with a type, and is verified before it is used. The
kernel partition holds the same kind of signed container, verified by the boot package
the same way any image is, just read from a flash partition instead of a file.

Why apps are separate: they are meant to move between PURR OS devices, so they use only
catcalls, which are the same on every device. They are pre-compiled, so what limits them
is the instruction set, not the board.

### 1.1 Board tiers

The same source builds for two kinds of board. The difference is how it is packaged.

- **Modular board:** at least 4 MB of flash, at least 2 MB of PSRAM, and MTP support
  (native USB). The kernel is a small flashed partition (hardware, drivers, FreeRTOS,
  mounting the root filesystem); CoreOS is a file in the root filesystem that the kernel
  loads into PSRAM. The T-Deck Plus is the primary example.
- **Monolithic board:** anything that does not meet that bar, such as the CYD 2.4C (no
  PSRAM, no native USB). It cannot load code from a file into RAM, and code must run in
  place from contiguous flash. So **everything is packed into one image** (kernel, CoreOS,
  AppManager and the runtime linked together) in a raw slot, with **1 MB left for apps.**
  KittenOS is the only safety net: it writes a new image into the single slot from an SD
  card file, and keeps no rollback copy. KittenOS has no network stack on these boards, so
  the running system downloads a Wi-Fi update to the SD card first. Only apps are independent.

**Every user-facing feature needs a fallback.** Failure of the normal system leads to
KittenOS. Failure of KittenOS leads to the recovery loader on boards that have one (Wi-Fi
and TLS, it downloads a new KittenOS), and then to a serial prompt (section 3).

**The `kernel` partition needs no fallback of its own** -- it already falls into this same
chain for free. It is just another bootloader-verified image slot (`bootloader/SPEC.md`
section 6), so a `kernel` that fails verification in `enforce` mode already falls back to
KittenOS the same way a bad `kittenos` or packed image would. KittenOS can already write
straight into `kernel` if it needs repairing, the same way it already writes `bootpkg`
(section 4.1) -- no separate recovery partition needed for it.

**Design style: Unix-like.** Small single-purpose parts, text-first interfaces (a shell
with pipes before any graphical UI), processes with ids that can be listed and stopped,
and a conventional directory hierarchy on storage. A graphical UI comes after the shell
and builds on the same services.

## 2. Layers

**Normal boot** (modular board):

```
  bootloader        verifies and loads the boot package
      |
  boot package      the boot menu; verifies and jumps into the kernel partition
      |             (a normal flash-resident image boot, no relocation needed).
      |             Starts KittenOS or the recovery loader instead if triggered.
      |
  kernel            hardware, drivers, FreeRTOS, mounts the root filesystem; then
      |             verifies CoreOS from the root filesystem, relocates it into
      |             PSRAM, maps it executable and starts it -- the same
      |             mechanism `Modules/SPEC.md` proves for every module, just for
      |             something kernel-sized instead of one command's worth
      |
  CoreOS            handoff, self-verification, decisions, the shell engine,
      |             the memory pressure service; hosts the runtime manager and
      |             the module loader
      |
  appmanager        installs, removes and updates apps; the command-line
      |             shell (launcher and task manager commands)
      |
  app runtime       a manager plus runtime modules: runs apps, gives them catcalls
      |
  apps              `.cat` app images, catcalls only
```

**Recovery:**

```
  bootloader        loads the boot package, or KittenOS directly if asked
      |
  KittenOS          its own small fixed driver set, a filesystem reader, the
                    command-line shell, and the routine that applies updates
```

On modular boards a small **recovery loader** sits beside KittenOS. It holds only Wi-Fi, TLS,
the keys and a very minimal CoreOS, and downloads a new KittenOS if KittenOS itself is broken.

Rules for the boundaries:

- **The kernel comes first and knows hardware and the filesystem, nothing else.** No
  security decisions, no policy, no UI. It reports what it found and what failed. On
  modular boards it is the only layer besides the boot area itself that lives in its own
  flash partition, not a file -- it has to, since it is what brings FreeRTOS and the
  hardware up in the first place, and nothing can load a file into executable memory
  before that has happened.
- **CoreOS runs on the kernel and owns the decisions.** It reads the boot package's
  result, verifies its own files, decides whether to continue, warn or fall back to
  recovery, and loads the rest. It is a file, not a partition, because by the time it
  loads, FreeRTOS and hardware are already running -- the same precondition every
  module already needs.
- **Recovery must not depend on what it recovers.** KittenOS carries its own drivers and
  filesystem reader and never loads the kernel partition or the CoreOS file, so it works
  when they are missing or broken.
- **KittenOS is only recovery, and it applies updates.** It is not updated in normal use.
- **Apps use catcalls and nothing else.** They never touch hardware, the OS or ESP-IDF
  directly.

## 3. Boot sequences

**Modular board, normal**

1. The bootloader verifies and loads the boot package and writes the handoff.
2. The boot package shows the boot menu for a short time. If nothing is pressed, it
   verifies and jumps into the `kernel` partition (a normal image boot; no PSRAM
   relocation at this stage, since the kernel is flash-resident).
3. The kernel brings up the hardware, FreeRTOS and the filesystem.
4. The kernel reads the root filesystem, verifies the CoreOS file, relocates it into
   PSRAM, maps it executable and starts it (`Modules/SPEC.md`'s mechanism).
5. CoreOS verifies itself, decides, then loads AppManager and the runtimes.

**Monolithic board, normal:** the bootloader loads the boot package for the menu, then
the packed image from its raw slot. Kernel and CoreOS start as one.

**Recovery.** KittenOS starts when:

- the user asks: the `reboot recovery` shell command, or the boot menu at power-on
- the kernel or CoreOS file is missing, fails verification in enforce mode, or does not
  start (a failure counter in `purrcfg`)
- `FORCE_RECOVERY` is set, or CoreOS asks for it after a failed decision

Starting KittenOS is done by recording the target in `purrcfg` and restarting, and the
bootloader honours it.

**If KittenOS fails too,** the bootloader starts the recovery loader on modular boards, which
downloads a new KittenOS. If that fails, or the board has no loader, it prints a prompt on the
serial console.

## 4. Flash layout

Per board, with its own partition file. Sizes are proposals until real images exist.

### 4.1 Modular board: T-Deck Plus (16 MB)

| Area | Purpose | Size |
|------|---------|------|
| Boot area (raw) | bootloader, partition table, `purrcfg`, `nvs` | ~128 KB |
| `bootpkg` (raw) | the boot package | ~64 KB |
| `kittenos` (raw) | KittenOS, recovery only | 1 MB |
| `rescue` (raw) | the recovery loader: Wi-Fi, TLS, keys, minimal CoreOS | ~1 MB, to be sized |
| `kernel` (app, single slot, no A/B) | hardware, drivers, FreeRTOS, mounts `root`, loads CoreOS | to be sized |
| `root` | LittleFS: CoreOS and everything else | the rest |

No `ota_0`/`ota_1`: that pattern (an OTA-selected app slot) is the monolithic-board
fallback (section 4.2), not the modular-board design. `kernel` is a single slot on
purpose -- the boot package picks it directly (`purrcfg`, not `esp_ota_*`), and it's
meant to change about as rarely as `bootpkg`/`rescue` do. When it does need updating,
that's KittenOS writing a verified image straight into the partition, the same way it
already updates `bootpkg` (`bootloader/SPEC.md` section 9, "Updating the package") --
not the `.new`/`.bak` LittleFS rename dance section 6.1 uses for CoreOS and everything
above it, since `kernel` isn't a LittleFS file. The exact partition subtype (distinct
from `factory`/`ota_*`, similar to how `rescue` already uses a plain, non-OTA subtype)
is a `partitions/<board>.csv` decision, not fixed here.

Directories in the root filesystem (proposed): `/boot` for CoreOS and the other system
files with their backups, `/apps` for `.cat` apps, `/data` for their data,
`/etc` for configuration, `/var/log` for logs and `/tmp` for scratch space.

### 4.2 Monolithic board: CYD 2.4C (4 MB)

| Area | Purpose | Size |
|------|---------|------|
| Boot area (raw) | bootloader, partition table, `purrcfg`, `nvs` | ~128 KB |
| `bootpkg` (raw) | the boot package (menu only) | ~64 KB |
| `kittenos` (raw) | KittenOS, recovery only | 1 MB |
| `os` (raw) | the whole packed image, one slot | ~1.8 MB, what is left |
| `apps` | LittleFS for `.cat` apps and their data | 1 MB |

The `os` slot is one slot with no backup. The packed image has to fit in whatever remains.

## 5. Apps

- **Pre-compiled `.cat` images**, signed with the same container and key system. No
  scripts and no interpreter.
- **Catcalls are the only interface**, so an app is portable across boards. The
  instruction set is the remaining limit, handled by an optional payload per
  instruction-set family.
- Managed by AppManager (`../AppManager/SPEC.md`): add, remove, list, and copy in from a
  PC over an MTP session. The first chunk is local apps only.
- The app runtime (`../AppRuntime/SPEC.md`) loads and runs them: a shared manager plus
  runtime modules, `catrt` first and a MicroPython runtime later. The app SDK and a `ui`
  catcall are needed before the first real app.
- **The OS is multitasking,** phone-style: several apps loaded at once and one in the
  foreground. How many is limited by memory, with an out-of-memory killer (CoreOS decides,
  the runtime manager stops the app, see `../AppRuntime/SPEC.md`). Consequences:
  - Each running app is its own task with its own stack and RAM block, both declared in
    the `.cat` header.
  - The loading model has to give every running app its own code and data
    (`../AppManager/SPEC.md` section 3).
  - Shared hardware needs an owner policy: the display and touch follow a foreground app,
    and storage is shared with locking.
  - The runtime tracks what each app holds, so stopping an app releases all of it.
  - **There is no memory protection** on the original ESP32. Tasks share one address
    space, so a faulty app can corrupt another app or the OS. Safety comes from signing, the
    catcall-only rule enforced by the toolchain, and watchdogs, not from hardware isolation.

## 6. Loading CoreOS from a file (modular boards)

The kernel reads CoreOS from the root filesystem, once it has hardware, FreeRTOS and the
filesystem itself up. Code has to run from memory the CPU can execute, and this is now
proven, not an open question: it's the exact mechanism `Modules/SPEC.md` builds and
`ModuleSpike/SPEC.md` originally spiked, applied to CoreOS instead of a command module.

- **Load into PSRAM.** Copy the file into PSRAM, relocate it against the two bases
  (`Modules/SPEC.md` section 4 -- data relocations against the writable alias, code
  relocations against the executable one), map it executable and run it. It needs no
  fixed address. Proven working on real ESP32-S3 hardware for the module system, and
  separately proven for the heavier, kernel-style calls CoreOS itself makes --
  `CoreOSSpike/SPEC.md`, passed 2026-09-28: real FreeRTOS `xTaskCreate` and real heap
  allocation, driven from relocated PSRAM code through a passed-in table, not just
  `puts`/`printf`. What the spike explicitly did *not* prove: that CoreOS's actual call
  sites -- every real `malloc`/`vTaskDelay`/`esp_partition_*`/`mbedtls_*` call across its
  real source, not a small hand-picked set -- convert cleanly onto that table. That's
  still real, unstarted work.
- **The kernel does not need a raw-flash fallback for this.** The earlier idea of running
  CoreOS in place from a raw flash slot if PSRAM execution failed is dropped along with
  the kernel-as-a-file idea it was attached to -- PSRAM execution is proven, and if
  CoreOS genuinely can't load this way for some board, that board doesn't get the
  no-OTA modular design at all and stays on the monolithic tier's single packed slot
  (section 4.2) instead of inventing a third loading method.

**Built 2026-09-29: both loader caps that were blocking this.** `load_relocatable_file()`
(pulled out of the module and kernelmod loaders, which duplicated the same ~170 lines twice,
into one shared function both now call) takes a page budget instead of a hardcoded one
`CONFIG_MMU_PAGE_SIZE` block, rounding a payload up to as many pages as its caller allows --
existing callers still pass 1, so nothing about today's modules or kernelmods changed, proven
on hardware (all four `/system` modules and all four `/kernelmods` still load identically after
the refactor). `PURR_RELOC_MAX` rose from 4096 to 65536 (`purr_relocate.h`) -- applying a
relocation is a trivial per-word add, so the new ceiling costs nothing at boot; it stays a
safety bound against a corrupt file, not a performance limit.

**Resolved, 2026-09-30: the multi-page path itself, proven on real hardware, not just in code
review.** A signed, deliberately oversized (~90KB, needing 2 of the 64KB pages) test module,
built the normal way through `purrstrap modules build`, was loaded through the real,
unmodified `load_relocatable_file()` with a 4-page budget. Checked two independent ways: the
90000-byte payload read straight back out of the writable alias (across the page boundary, not
just the first page) checksummed to exactly what was compiled in, and the entry call through
the executable alias returned a valid table and ran its command successfully. Temporary test
code (`Modules/test/bigpage_test.c`, its embedded-blob file, and `commands.c`'s `testbigpage`
block), deleted once the proof was recorded, the same discipline every other spike this session
followed.

**Resolved, 2026-09-30: `purrstrap` builds CoreOS's real source as one relocatable module,
proven on real hardware.** 15 of the 16 files in `PurrOS/components/coreos/src` -- every one
except `purr_appmgr.c` (below) -- were built together through the normal `purrstrap modules
build --kind coreos` path (the new `coreos` kind, mapping to the already-existing
`PURR_MOD_COREOS` subtype), signed with the boot-role key `PURR_MOD_COREOS` actually requires,
loaded through `load_relocatable_file()`, and ran their entry point successfully. Two real bugs
surfaced and were fixed, not test artifacts:

- **`purr_key_t` collided.** `purr_keybag.h` (a signing key record) and `purr_menu.h` (a
  keyboard-key enum) both defined a type by that name -- invisible until this was the first time
  anything included both headers in one translation unit. `purr_menu.h`'s enum is now
  `purr_menu_key_t`; nothing else needed to change, since every other call site used the enum
  constants or the functions, never the type name itself.
- **Freestanding builds need a real libc subset.** `-fno-builtin` with no libc linkage leaves
  `memcpy`/`memset`/`memcmp`/`memchr`/`strncmp`/`strtoul`/`strlen`/`strcmp`/`strcpy`/`strncpy`/
  `snprintf`/`vsnprintf` all unresolved -- real, shipping coreos/src files call every one of
  these by name. `purrstrap/freestanding/purr_freestanding_libc.c` (moved there from
  `Modules/test/`, since it's real reusable infrastructure now, not a throwaway) provides
  correct, if scoped, implementations of exactly what these files call -- not a general-purpose
  libc, and it says so in its own header.

**Resolved, 2026-09-30: `purr_appmgr.c`'s direct kernel-filesystem calls.** It called
`purr_fs_list()`/`purr_fs_read()`/`purr_fs_write()`/`purr_fs_mkdir()`/`purr_fs_remove()`/
`purr_fs_rename()`/`purr_fs_stat()` -- the kernel's own raw filesystem functions -- directly,
the one real architectural gap left in the 16 files (not a missing-libc problem, unlike
everything else this section already fixed). Fixed by giving `purr_appmgr.h` a narrow
`purr_appmgr_fs_t` function-pointer interface, the same path-based shape
`purr_kernel_table_t`'s own `fs_*` entries already use, and converting all four public
`purr_appmgr_*` functions to call through it instead of a raw `purr_fs_t*`. `commands.c` backs
it with `s_appmgr_fs`, built from the same `kernel_table_fs_*` wrappers the kernel table itself
uses; host tests back it with small adapters over the fake RAM-disk fixture. Proven on real
hardware, not just via the 95 host-test checks that already covered `purr_appmgr.c`'s logic: a
temporary boot-time check, added and deleted the same way every other proof in this section was,
drove `purr_appmgr_add`/`scan`/`find`/`remove` end to end through `s_appmgr_fs` against the real
device filesystem and logged
`add=0 before=0 after_add=1 found=1 rm=0 after_remove=0 -> PASS`; all four `/system` modules and
all four `/kernelmods` (including `appmgr.cat` itself) still load identically afterward.

Beyond that, CoreOS's *other* source -- `commands.c`/`main.c`/`login.c`, none of it freestanding
today -- still hasn't been converted, and nobody has picked which real files a genuine
`coreos.kitt` build should even contain.

**Resolved, 2026-09-30: the complete 16-file `purrstrap` build, proven on real hardware.** All
16 files in `coreos/src` -- `purr_appmgr.c` included, now that the fix above landed -- built
together through `purrstrap modules build --kind coreos`, alongside `purr_freestanding_libc.c`
and a temporary entry file (`Modules/test/coreos_link_test.c`, deleted once this was recorded)
that took the address of one real, representative function from each of the 16 files into a
`volatile` array and read every one back (the same dead-store-elimination fix this session
already needed once, section 6's first 15-file proof -- a write nothing reads is dead by `-O2`'s
own logic, and `--gc-sections` strips the "referenced" function right back out). Signed with the
boot key, loaded through the same `load_relocatable_file()` every real module and kernelmod
uses, and run: the entry call returned `linked_count=16 checksum=00000860`, meaning every one of
the 16 address-taken functions actually resolved to a real, non-NULL, distinct symbol in the
combined, relocated binary -- not just a successful compile. All four `/system` modules and all
four `/kernelmods` loaded identically alongside it, same boot.

This also forced a real, previously-overdue step: both the boot and developer signing keys were
rotated (the old boot key's passphrase was lost, and keys in this project are meant to be cheaply
swappable -- whichever key is in `signing_keys/` *is* the trusted one, by design, not a fixed
secret to protect indefinitely). The default key bag
(`PurrOS/components/coreos/keys/purr_default_keys.c`) was regenerated from the two new public
keys, and all eight real embedded modules/kernelmods (`commands.c`'s `s_about_module` etc.) were
re-signed with the new developer key and their embedded bytes regenerated in place -- same
payload bytes, new header/signature only. The old keys are kept at `signing_keys/old/` (not
deleted) in case anything else still needs them.

Between the layers there is a small versioned table of calls: the boot package gives the
kernel the boot information and a few services, and the kernel gives CoreOS its API
(the same shape as `purr_core_table_t` already is for CoreOS-to-module, one layer up).
The details are in the kernel and CoreOS specs.

### 6.1 Updating the system files

Apps are updated by AppManager. This is for the system files above the kernel: CoreOS,
the drivers bundle, AppManager and the runtimes. The kernel itself is
not a system *file* -- see section 4.1 for how it updates instead. The boot package turned out
to belong with the kernel's partition-level path instead (it lives in its own raw partition,
never in LittleFS) -- see `../OTA/SPEC.md` section 3, which now also has the up-to-date split
between the two mechanisms.

**Built 2026-09-29** (`purr_swap.{h,c}`, host-tested; `purr_swap_setup()` in
`PurrOS/main/commands.c`), steps 1-7 below exactly as speced, plus the crash-safety note in
"Power loss" further down. **Still not built:** the kernel/CoreOS split (section 6) that would
make a swapped `coreos.kitt` actually get loaded and run -- this mechanism correctly moves,
verifies, confirms and rolls back the file itself, but nothing reads it back out yet.

**KittenOS performs the swap.** It is never overwritten in normal use, so it can safely
replace anything else, including CoreOS itself.

1. **Stage.** The new file arrives in `/boot` as `<name>.new`, put there by the OTA app, the
   shell, or a transport. Nothing has changed yet.
2. **Verify.** The running system checks it fully: container, chip, type, key role, a higher
   version, signature.
3. **Request.** It records the request in `purrcfg` (state `requested`) and restarts into
   KittenOS.
4. **Swap.** KittenOS verifies the file again by itself. It renames the current file to
   `<name>.bak`, then renames `<name>.new` into place, and sets the state to `unconfirmed`.
5. **Boot.** It restarts the normal system.
6. **Confirm.** When the new version starts healthy, it sets the state to `confirmed`, and that
   file's version floor rises to the confirmed version. Restoring a `.bak` after a failed update is
   allowed, because the floor only rises on confirmation.
7. **Roll back.** If the new version does not come up healthy after N attempts (default 3),
   KittenOS renames the bad file to `<name>.bad`, renames `<name>.bak` back, and marks the
   update `failed`.

Since CoreOS updates are now a plain LittleFS file swap, not a flash-partition
operation, iterating on CoreOS itself -- which is most of what this rewrite has actually
been doing -- stops needing a reflash at all once the kernel partition is stable. That
was the whole motivation for the module system in the first place.

**Power loss.** LittleFS renames are atomic. Each step is recorded in `update_state`
(`bootloader/SPEC.md` section 5), so KittenOS resumes from wherever a restart caught it.
The backup exists before the new file is used, and is not removed before the new version is
confirmed.

**Monolithic boards** have no backup: KittenOS writes the new packed image into the single
raw slot from an SD card file (which the running system downloaded first, if the update came
over Wi-Fi).
A cut leaves the slot broken but KittenOS untouched, so it starts and redoes the write. The
UI KittenOS will use for this comes later.

## 7. Layout

```
PurrOS/
  CMakeLists.txt               IDF project
  sdkconfig.defaults           shared
  sdkconfig.board.<board>      per board: chip, flash size, PSRAM, partition file
  sdkconfig.profile.<profile>  CoreOS profile: minimal, recovery or full
  partitions/<board>.csv       per board
  components/
    kernel/                    see its SPEC.md
    coreos/                    see its SPEC.md
  main/                        thin app: start CoreOS on the kernel
```

One project builds every image. The **CoreOS profile** (`minimal`, `recovery` or `full`) is a
Kconfig option (`PURR_PROFILE`) and the board another (`PURR_BOARD`). On modular boards the
kernel builds to its own flashed image (a normal ESP-IDF app) and CoreOS builds to a
relocatable file for the root filesystem (`purrstrap`, the same way a module builds --
`Modules/SPEC.md` section 8 -- just bigger). On monolithic boards kernel and CoreOS are
linked into one image, same as before. KittenOS uses a small built-in driver set either
way.

## 8. Shared formats, tooling, first boards

- **Shared formats:** `purr_abi.h` (in `components/coreos/include/`) defines the container
  header, `purrcfg` and the handoff struct for the bootloader, the boot package and CoreOS.
- **Tooling:** purrstrap (`../purrstrap/`). The board replaces the raw chip choice in its
  `coreos` script, since the board decides the chip. Later subscripts: keys, image signing,
  module packaging, flashing.
- **The first iteration is a shell on both boards.** It boots and drops to a shell. Only the
  display and keyboard have to work.
- **T-Deck Plus (primary, modular).** ESP32-S3, 16 MB flash, 8 MB PSRAM, a 3.2 inch ST7789
  display, a keyboard, and more (touch, trackball, LoRa, GPS). The pin map comes from the old
  archive (`archive/DP9/code/source/devices/tdeck_plus/device.pcat`). It is where the memory
  protection options on the ESP32-S3 will be investigated.
- **CYD 2.4C (monolithic).** ESP32-2432S024C: 2.4 inch 240x320 ILI9341-compatible SPI display
  and CST816S capacitive touch (I2C). Original ESP32, 4 MB flash, no PSRAM, no keyboard. In the
  first iteration it only boots and shows the console. Its pin map comes from the archive too
  (`archive/DP9/code/source/devices/cyd_s024c/device.pcat`).

## 9. Constraints from the boards

- **CYD: no PSRAM.** No full-frame buffers, and no loading code from files. Display output is
  push-based in small DMA chunks. It also drives the app runtime choice.
- **Two display controllers** from the start (ILI9341 on the CYD, ST7789 on the T-Deck Plus),
  so the display driver contract keeps the controller in a table.
- **Bootloader size.** The custom bootloader already measures 27,008 bytes on esp32 against
  the 28,672 bytes available before the partition table.
- **Secure Boot V2 on original ESP32** needs chip revision 3 or newer. The eFuse path has to
  check the revision. The software path is unaffected.
- **Code is chip-specific.** An esp32 kernel or module does not run on esp32s3. The container's
  `chip_id` enforces this.

## 10. Build order

**First milestone: the local chain works.** On the T-Deck Plus first, then the CYD.

1. purrstrap with the `coreos` subscript. Done. Its `flavor` becomes the CoreOS `profile`.
2. CoreOS foundations: `purr_abi.h`, `purrcfg`, key bag, image verification, handoff, boot log,
   decision table, the command-line engine. Host tests.
3. **The bootloader shows a UI:** the boot package draws the boot menu on the display ("press a
   key", boots normally if ignored). Needs the board profiles and the display drivers.
4. **KittenOS works:** chosen from the menu, it comes up with its shell (CoreOS's recovery
   profile).
5. **CoreOS boots to a shell:** the normal path, with the kernel starting CoreOS (the full
   profile) and dropping to a shell. The CYD boots and shows the console, with the kernel and
   CoreOS packed into one image.

**Second milestone: internet recovery.**

6. The recovery loader (CoreOS's minimal profile): Wi-Fi, TLS, downloads the newest KittenOS and
   installs it. The boot menu's "update KittenOS" entry.

**After that**

7. Loading CoreOS from a file on the T-Deck Plus (section 6, proven by `../Modules/SPEC.md`),
   the new `kernel` partition, and the resulting partition table changes.
8. The `console` catcall, the runtime manager and `catrt`, the app SDK, then AppManager with its
   command-line shell and its transports.
9. The network install and driver packs (`../Install/SPEC.md`, `../Drivers/SPEC.md`).
10. The `ui` catcall, a graphical shell, KittenOS's update UI, a MicroPython runtime, and remote
    access come later.

## 11. Open questions

**Resolved 2026-09-28** (see section 1's update note and sections 2/3/4.1/6): who starts
CoreOS is the kernel, settled -- the kernel is its own flash partition (like a Linux
kernel running init), not a file the boot package loads. The boot package's role is
correspondingly simpler on modular boards than once thought: it verifies and jumps into
the kernel partition directly, the same plain image-boot it already does for
KittenOS/rescue, not a second relocate-into-PSRAM path -- so it is one program either
way, monolithic or modular, just with a different target.

- **Built, 2026-09-28:** the actual `kernel` partition exists now (`partitions/tdeck_plus.csv`
  and `bootloader/partitions.csv`, kept identical): `ota_0` renamed to `kernel` -- kept on the
  `ota_0` *subtype* only so ESP-IDF's stock bootloader-image loader can still jump to it, not
  because `esp_ota_*` selection is used (it isn't; `ota_1` was already gone). The bootloader's
  own fallback chain (`bootloader/SPEC.md` section 6) tries `kernel` first, then `kittenos`,
  then the recovery loader, automatically -- proven on real hardware, including the
  kernel-fails-over-to-kittenos case.
- **Resolved, 2026-09-29:** `netinstall`'s retargeting. It now writes `kernel` (and `loader`,
  `bootpkg`) as a real, named, partition-level component -- `esp_ota_set_boot_partition` only
  runs for `kernel` specifically, not unconditionally -- and stages CoreOS/AppManager/the
  runtimes/the drivers bundle through the file-swap path instead (section 6.1, `OTA/SPEC.md`
  section 3). The module loader's page-size and relocation-count caps have also grown (section
  6, above) -- both real blockers this bullet used to list are cleared.
- **Built, 2026-09-30: a genuinely standalone, separate kernel binary, proven on real
  hardware.** New top-level project (`Kernel/`, same shape as `bootloader/`), reusing
  `PurrOS/components/kernel` directly -- no `PurrOS/main` (no shell, login, modules, apps),
  not another PurrOS profile. `app_main()` does exactly the kernel's job and nothing else:
  `purr_kernel_init()` (board power, bus, display), mounts `root` via `purr_fs_flash_bd`/
  `purr_fs_mount`, then idles -- there is nothing to hand off to yet. Flashed to the real
  `kernel` partition and booted: `purr_boot: booting app slot 0`, `Project name: purr_kernel`,
  `ST7789 320x240 up`, `root filesystem: mounted`. **267 KB vs. the monolith's ~1.08 MB** --
  about a quarter the size, the real, measured proof that the separation is substantive, not
  just a partition-table reservation.
- **Built, same day, later: the kernel genuinely loads and runs real CoreOS code from a file,
  proven on real hardware.** `Kernel/main/kernel_table.c` gives this kernel its own real
  `purr_kernel_table_t` (console, filesystem, heap, uptime -- login/apps/reboot/etc. left
  NULL on purpose, since that's real CoreOS content that hasn't moved into this kernel's
  domain yet, not faked); `Kernel/main/loader.c` ports `load_relocatable_file()`'s exact
  mechanism (this kernel doesn't link `PurrOS/main` at all); `Kernel/coreos_min/
  coreos_min_entry.c` is the first real (not synthetic) CoreOS entry point -- no command
  dispatch table yet (commands.c's real and much bigger content, a separate conversion), but
  it genuinely uses the kernel table for console output and a real `fs_list` of the actual
  root filesystem, not a self-contained checksum like the 16-file proof. Confirmed on the
  physical display: the kernel mounts root, writes and loads `/coreos.cat` (embedded,
  `plant_temp_modules()`-style temporary scaffolding), relocates it into PSRAM, calls the
  real entry point with the real kernel table, and that code prints through it and lists the
  real filesystem correctly. (Serial logging mysteriously stops right after the entry call,
  cause not yet root-caused -- the display output is what actually confirms success.) Also
  hit and fixed: `Kernel/`'s `sdkconfig.defaults` was missing `CONFIG_ESP_MAIN_TASK_STACK_SIZE
  =16384`, causing a `StoreProhibited` crash inside ECDSA verification -- PurrOS's own
  defaults already carry this for the same reason.

  **Packaged for real distribution too:** `purrstrap coreos package-component` (new action)
  wraps an already-built raw binary into a signed-ready `PURR_IMG_MODULE` with the matching
  `PURR_MOD_KERNEL`/`PURR_MOD_LOADER` subtype -- the shape `netinstall kernel` already
  expects. Used to sign the real kernel binary and add a `component=kernel` entry to a
  published KittenOS release's recovery manifest; not yet tested against a live release
  (no real network credentials available this session).

  **Still not done:** nothing yet loads the full, real CoreOS -- only this one deliberately
  minimal entry point -- and converting CoreOS's real source to call through a kernel table
  instead of `malloc`/`vTaskDelay`/`esp_partition_*`/`mbedtls_*` directly
  (`CoreOSSpike/SPEC.md` proved the mechanism on a small hand-picked set of calls, not
  CoreOS's actual breadth) is still real, unstarted work. Today's `full`/`recovery` builds
  still link kernel and CoreOS into one flashed image on the T-Deck Plus, same as the
  monolithic tier -- the standalone kernel above is a real, working alternative path, not a
  replacement yet. The bootloader's own fallback chain also still only checks that a slot
  looks like a real app image (a magic byte), not a real PURR signature -- real per-image
  verification (`bootloader/SPEC.md` section 6, steps 4-5) is designed but not built for
  `kernel`/`kittenos`/`loader`, only for the boot package itself.
- **The `kernel` partition's own update path** is built (`net_install kernel`, above) but not
  yet tested against a real power-cut-mid-write case.
- **How KittenOS is started:** the assumption is a target recorded in `purrcfg` that the
  bootloader honours.
- **How many backups to keep** (`.bak`) and when to delete them.
- **App loading model:** relocate into RAM, or run in place from flash, and the resulting size
  limit (`../AppManager/SPEC.md` section 3).
- **Whether KittenOS ever updates itself,** for example through serial only. It is the recovery
  base, so an interrupted update would leave nothing to recover with.
- **Sizes** of the boot area, boot package and packed image, once real images exist.
- Whether the purrstrap subscript should be renamed, since it builds the whole base system.
  User accounts are in `../Users/SPEC.md`.
