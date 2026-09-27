# Loading-from-files spike (draft 0.2)

An experiment, not a design. It answers one question on real hardware: can a modular
board load the kernel and CoreOS from files in the root filesystem, run them from PSRAM,
and fall back safely? Its result decides how modular boards are packaged
(`../PurrOS/SPEC.md` sections 4 and 6).

## 1. Scope

- **Modular boards only:** at least 4 MB flash, at least 2 MB PSRAM, and MTP support
  (`../PurrOS/SPEC.md` section 1.1). The spike runs first on the **T-Deck Plus** (ESP32-S3).
- Monolithic boards such as the CYD 2.4C pack everything into one image and never load a
  system file, so they are not part of this.

## 2. Two methods, in this order

Every user-facing feature needs a fallback, and this one has two ways to work before it has
a real fallback.

**A. Load into PSRAM.** The boot package reads the kernel file from the LittleFS root
filesystem, copies it into PSRAM, applies relocations with the same loader design as apps
(`../CatFormat/SPEC.md`), maps the memory so the CPU can execute it, and runs it. The kernel
then does the same for CoreOS. On the ESP32-S3, ESP-IDF appears to allow mapping PSRAM as
executable. That has to be checked (section 5).

**B. A raw cache.** If PSRAM execution turns out to be slow or awkward, the kernel and CoreOS
run in place from raw flash slots, and the files in the root filesystem stay the master copy.
The boot package or KittenOS refreshes a slot when a newer file is there. The mapping API is
public but chooses the virtual address itself, so this needs a fixed-window scheme
(`../PurrOS/SPEC.md` section 6).

Method A is tried first, because it is the Linux way and reuses the app loader.

## 3. Pass criteria

1. A signed test kernel file in LittleFS is read, verified and loaded into PSRAM by the boot
   package, and its entry runs and prints over serial.
2. The test kernel starts a test CoreOS file from LittleFS the same way.
3. After the kernel or CoreOS is rebuilt with an unrelated change, the other still loads.
   This shows the call tables between them are stable, so each is independently updatable.
4. A tampered file is rejected.
5. A failed update rolls back: KittenOS renames the bad file aside and restores the `.bak`
   copy, and a power cut in the middle of the swap still ends in a working system.
6. The speed of running from PSRAM is measured, against running in place from flash.

The spike passes when criteria 1 to 5 pass for at least one method. Criterion 6 decides which
method is used.

## 4. The test files

- A tiny test kernel and a tiny test CoreOS for the ESP32-S3, each with an entry table
  following the contracts in `../PurrOS/components/kernel/SPEC.md` and
  `../PurrOS/components/coreos/SPEC.md`.
- The tables for the spike are minimal: log, memory allocation, and a tick counter for timing.
- They are signed with a test key and wrapped in the module container
  (`../bootloader/SPEC.md` section 3). purrstrap needs a minimal subscript to do that.

## 5. What to find out

Questions the spike answers by trying, not by reading:

- Does ESP-IDF 5.3.5 let PSRAM be mapped as executable on the ESP32-S3, and through which
  call?
- What are the alignment and byte-access rules for executing from PSRAM? Do code and read-only
  data need separate mappings?
- Does Xtensa code from an ordinary build relocate correctly with the link-twice method
  (`../CatFormat/SPEC.md` section 8)?
- How much slower is PSRAM code than flash code, and how much does the cache change that?
- How much PSRAM does each file use, and what is the load time?
- Does interrupt or cache-disabled code (flash writes) cause trouble for code in PSRAM?
- How small can a read-only LittleFS reader be in the boot package?
- For method B: does the fixed-window scheme work, and where does ESP-IDF allow the window?

## 6. The fallback tests

Simulate each failure and check what happens:

1. A file is missing, corrupted (bad hash), or has a wrong call-table version: KittenOS starts
   and rolls back from the `.bak` copy where one exists, and the boot report says why.
2. No usable copy exists: KittenOS stays running so the user can repair it.
3. KittenOS itself fails: the bootloader's serial prompt.
4. A power cut at every step of the file swap (`../PurrOS/SPEC.md` section 6.1).

## 7. Outcomes

- **Method A works:** modular boards boot from files loaded into PSRAM, with no raw copies of
  the system.
- **Only method B works:** modular boards keep raw cache slots that the boot package or
  KittenOS refreshes from the filesystem, and the fixed-window scheme becomes part of the
  interface.
- **Neither works:** modular boards package like monolithic ones, one image, and the
  files-in-the-filesystem model is dropped for now.

In every case the numbers from section 5 are written into the specs.

## 8. Non-goals

The real kernel, drivers, CoreOS, AppManager or runtimes. This uses tiny test files. Any
monolithic board work.

## 9. Open questions

- Which test key signs the files.
- Whether KittenOS exists yet at that point, or the spike stops the chain at a stub.
- How speed is measured: a fixed loop and a call through the tables, timed with a hardware
  counter.
- What the T-Deck Plus needs to be flashed and monitored during the spike.
