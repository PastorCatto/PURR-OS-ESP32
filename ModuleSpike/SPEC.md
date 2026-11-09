# Loading-from-files spike (draft 0.3)

An experiment, not a design. It answers one question on real hardware: can a small,
position-independent module be loaded from a file in the root filesystem, run from PSRAM,
and fall back safely? Its result decides how the core/module split is built
(`../PurrOS/SPEC.md`, and the pinned module-loader idea in project memory).

## 0. Why this draft changed

Draft 0.2 asked whether **the kernel and CoreOS themselves** could load as files. They
cannot: a full FreeRTOS/ESP-IDF app needs the standard interrupt-vector/`esp_system` boot
sequence, which only runs when the app is loaded through the normal partition-table
mechanism. Copying it into PSRAM first does not fix that, and LittleFS's read API
reassembling a file's blocks into a contiguous stream does not change what kind of code can
run once it is in RAM.

What *can* load from a file and run: small position-independent code with no startup of its
own, called into (and calling back into) an already-booted core — the same trick the boot
package already uses for itself, and `.cat` apps will use for `run`. So the real shape is:

- **The core** stays a normal flashed app (FreeRTOS bring-up, flash driver, LittleFS mount,
  verify/key bag, catcall dispatch) — updated like any other OTA image (own signing role,
  section 0.1), never loaded from a file.
- **Modules** (drivers, the shell, AppManager, UI, eventually most of CoreOS) are small PIC
  files in a LittleFS system folder, loaded into PSRAM and run by the core.

This spike tests the module half only, with a throwaway stub core and throwaway test
modules — not real kernel/CoreOS code.

### 0.1 The core's own updates (not part of this spike)

Settled already, not tested here: the core is OTA-updatable the same way PURR OS is, but a
core image is only accepted if it is signed by the `BOOT` role — the same role already
reserved for kernel/CoreOS/bootpkg/loader (`purr_role_may_sign`). Modules are signed by a
lower role (`SYSTEM`/`DEVELOPER`/etc., matching `PURR_IMG_MODULE` today) and cannot satisfy
that check, so a module-signing key can never push a core update. No new mechanism — this is
the existing role-gate applied consistently. This only needs proving once the core has a real
OTA slot; it is not part of what the spike measures.

## 1. Scope

- **Modular boards only:** at least 4 MB flash, at least 2 MB PSRAM, and MTP support
  (`../PurrOS/SPEC.md` section 1.1). The spike runs first on the **T-Deck Plus** (ESP32-S3).
- Monolithic boards such as the CYD 2.4C pack everything into one image and never load a
  module, so they are not part of this.

## 2. The method

**Copy into PSRAM.** A stub core reads a module file from the LittleFS root filesystem,
verifies it (PURR image container, `../bootloader/SPEC.md` section 3), copies its payload
into PSRAM, applies relocations with the flat-image + relocation-list format `.cat` apps
already use (`../CatFormat/SPEC.md` sections 6-7), maps the memory so the CPU can execute it,
and calls its entry. On the ESP32-S3, ESP-IDF appears to allow mapping PSRAM as executable —
that has to be checked (section 5).

There is deliberately no fixed-flash-slot fallback in this draft: a scheme of raw partition
slots the core refreshes in place would work, but only for a fixed number of slots, which
gives up the point of an open, arbitrarily-named module folder in LittleFS. If PSRAM
execution turns out to be a dead end on this hardware, that is a result to redesign around
(section 7), not a reason to fall back to fixed slots.

## 3. Pass criteria

1. A signed test module file in LittleFS is read, verified and loaded into PSRAM by the stub
   core, and its entry runs and prints over serial.
2. A second, independent test module loads the same way and can call back into the stub
   core's own table (log, alloc) while the core calls into the module's table.
3. After either test module is rebuilt with an unrelated change, the other still loads. This
   shows the call table between core and module is stable, so each is independently
   updatable — the actual payoff (push one module file, not a whole rebuild+reflash).
4. A tampered module file is rejected.
5. A failed module update rolls back: the core renames the bad file aside and restores the
   `.bak` copy, and a power cut in the middle of the swap still ends in a working system.
6. The speed of running a module from PSRAM is measured, against the same code running in
   place from flash as an ordinary linked-in function.

The spike passes when criteria 1 to 5 pass. Criterion 6 is recorded either way, since it
affects which modules are worth splitting out first.

## 4. The test files

- A minimal **stub core** for the ESP32-S3: FreeRTOS bring-up, LittleFS mount, the PURR
  image verify/key-bag check, and just enough of a call table to test with (log, malloc, a
  tick counter for timing).
- Two tiny **test modules**, each importing that table and exporting one of their own, built
  and linked the same way `.cat` apps are (link-twice at two base addresses, diff to find
  relocations, `../CatFormat/SPEC.md` section 8) — purrstrap needs a minimal subscript to do
  that for modules too.
- Both signed with a test key and wrapped in the module container (`PURR_IMG_MODULE`,
  `../bootloader/SPEC.md` section 3).

## 5. What to find out

Questions the spike answers by trying, not by reading:

- Does ESP-IDF 5.3.5 let PSRAM be mapped as executable on the ESP32-S3, and through which
  call?
- What are the alignment and byte-access rules for executing from PSRAM? Do code and
  read-only data need separate mappings?
- Does Xtensa code from an ordinary build relocate correctly with the link-twice method?
- How much slower is PSRAM code than flash code, and how much does the cache change that?
- How much PSRAM does a module use, and what is the load time?
- Does interrupt or cache-disabled code (flash writes) cause trouble for code in PSRAM?
- How small can a read-only LittleFS reader be in the stub core?

## 6. The fallback tests

Simulate each failure and check what happens:

1. A module file is missing, corrupted (bad hash), or has a wrong call-table version: the
   core rolls back from the `.bak` copy where one exists, and says why over serial.
2. No usable copy of a required module exists: the core stays up in a degraded state so the
   problem can be fixed (matches KittenOS's role once modules are real).
3. The stub core itself fails to boot: falls through to the normal bootloader menu, same as
   any other bad app image today.
4. A power cut at every step of the file swap.

## 7. Outcomes

**It works.** All six pass criteria were run on a real T-Deck Plus (ESP32-S3) and passed:

1. A signed module file (`PURR_IMG_MODULE`), planted on the real root LittleFS filesystem,
   read back, verified through the same `purr_verify`/key-bag path everything else in this
   codebase uses, copied into a SPIRAM buffer, mapped executable a second time over the same
   physical memory (`esp_mmu_map(MMU_TARGET_PSRAM0, MMU_MEM_CAP_EXEC)`), and called: correct
   result.
2. & 3. A second, independently built and signed module (compiled separately, never linked
   against the first) called back into a core-supplied function pointer and made an
   intra-module near-call to its own helper, both without any relocation, and returned the
   right answer. Two independently-built modules agreeing on the same plain-C-struct call
   table is the actual proof that each is independently updatable.
4. A module with one payload byte flipped after signing was rejected at the hash check
   (`bad-hash`), before signature verification even ran.
5. A corrupted "update" was rejected, and rolling back via a plain LittleFS rename from a
   `.bak` copy restored a verifying file. (This proves the rename-based rollback logic, not
   an actual power cut mid-swap — that needs bench control over the device's power, not
   software, and stays untested.)
6. PSRAM-exec speed, 20000 calls each: a normal flash-linked call averaged **151.9 ns/call**;
   the identical code run from the PSRAM executable alias averaged **132.4 ns/call** —
   PSRAM-exec was not just acceptable, it was slightly *faster* in this test (likely cache
   behavior specific to this tiny function; not a claim that holds for every workload).

**Conclusion:** modules load from PSRAM-copied files, an arbitrary number of them, named
freely in a LittleFS system folder, no fixed slot count, no real performance penalty for at
least this class of small function. The core/module split from the pinned module-loader idea
in project memory is no longer just plausible, it is proven on the actual target hardware.

## 7.1 What is still open, deliberately, past this spike

This spike used a hand-picked, zero-relocation test function and one hardcoded entry offset.
Real modules will need, before this becomes the real system rather than a proof:

- **A real relocator.** Anything with a string, a global, or a jump table needs an absolute
  address (Xtensa `l32r`), which breaks the moment code is copied somewhere else (found this
  out directly: a first attempt at a call-table test used a string literal, and the `l32r`'d
  address pointed at a `.rodata` location that was never even copied into the payload). The
  link-twice method `CatFormat/SPEC.md` section 8 already describes for `.cat` apps is the
  answer, extended to two-way call tables instead of `.cat`'s one-way lifecycle table.
- **A container-declared entry point and table layout**, not a number typed into a comment.
  The payload table / manifest (`CatFormat/SPEC.md` sections 4-5) already has room for this.
- **Purrstrap tooling** to build and sign a module the way `coreos package` does for
  recovery/full images, instead of hand-invoking the cross compiler and `image.py`.
- **A real power-cut test at every step of the file swap**, which needs bench control over
  the device's power rail, not something a shell command can simulate.

None of that blocks moving forward — it blocks calling the *real* core/module system done.

## 8. Non-goals

The real kernel, drivers, CoreOS, AppManager or runtimes. This uses a throwaway stub core and
throwaway test modules. Any monolithic board work. The core's own OTA update path (section
0.1) — that is proven separately, against the real core, once it exists.

## 9. Open questions

Answered by running the spike (kept here for the record, not as open questions anymore):

- **Which key signs the spike's test modules:** the real `developer` key already in use for
  `.cat` apps, not a separate throwaway one — `PURR_IMG_MODULE`'s `MOD_DRIVER` subtype already
  accepts that role, so a new key wasn't needed.
- **How speed is measured:** `esp_timer_get_time()` around 20000 calls each way. See section
  7's numbers.
- **The stub core's call table format:** was pure throwaway scaffolding for this spike (one
  `void (*ping)(void)` entry) — the real shape is section 7.1's job (a container-declared,
  versioned table), not this one.

Genuinely still open, not answered by this spike (see section 7.1 for why):

- The real relocation format for a module's data/rodata references.
- Where entry points and table layout get declared (container manifest, not a hardcoded
  offset).
- The power-cut-mid-swap case, which needs bench hardware control this spike didn't have.
