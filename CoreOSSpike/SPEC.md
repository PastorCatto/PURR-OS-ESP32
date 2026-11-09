# Freestanding-CoreOS spike (draft 0.1)

An experiment, not a design. It answers one question on real hardware: can a
freestanding, relocatable blob that calls real FreeRTOS and heap functions --
not just a passed-in `puts`/`printf`, but a heap allocator and the task
scheduler itself -- run safely from a PSRAM copy the same way a module does?
Its result decides whether `PurrOS/SPEC.md` section 6's "CoreOS is a file the
kernel loads into PSRAM" is buildable as written, or needs a different shape.

## 0. Why this spike exists

`Modules/SPEC.md`'s mechanism is proven, but only for code that calls
**nothing** by name outside itself -- every module built so far
(`about`/`apps`/`wifi`/`netinstall`) is compiled `-nostdlib -ffreestanding`
and reaches the outside world only through `purr_core_table_t` function
pointers. That's why the relocation trick works: there are no external
symbols to resolve, only internal self-references.

CoreOS today is nothing like that -- it calls `malloc`, `vTaskDelay`,
`esp_partition_*`, `mbedtls_*`, `heap_caps_*`, `snprintf`, and more, directly,
by name, throughout its source. For CoreOS to become a relocated PSRAM blob
loaded by the kernel, it can't bring its own copy of FreeRTOS/libc (that
needs the same full `esp_system` boot sequence that's exactly why the kernel
itself can't be a relocatable file either -- the same wall, one layer down).
It has to call into the kernel's **already-running** FreeRTOS/heap instead,
through a table -- the same discipline modules already follow, just for
much heavier things than `puts`.

This spike tests that discipline directly, reusing the existing, already-
working module loader and CoreOS-hosted `purr_core_table_t` mechanism as the
harness -- not a new stub binary. The relocate/map/call primitive doesn't
care which two layers are on each end of it; proving "a freestanding blob
can drive FreeRTOS task creation and heap allocation, only through a passed
table" at the CoreOS-to-module layer is the same evidence for the
kernel-to-CoreOS layer, since it's the identical mechanism either way.
Throwaway spike code only -- a separate table, a separate loader function,
never touching the real `purr_core_table_t`/ABI or the four real modules.

## 1. Scope

Modular boards only, T-Deck Plus first, same as `ModuleSpike/SPEC.md`. Not
the real kernel/CoreOS split -- that's follow-up work once this passes
(`PurrOS/SPEC.md` section 11's "not yet built" list).

## 2. The method

A temporary `spikecoreos` shell command loads one spike test module through a
copy of the existing loader (`load_one_module_file`), verify/relocate/map
unchanged, but calling `entry()` with a **separate, spike-only table**
(`purr_kspike_table_t`) instead of the real `purr_core_table_t` -- so nothing
about the real module ABI, the real four modules, or the production loader
changes. The spike table offers:

- `puts`/`printf` (already proven; kept for readable spike output)
- `heap_alloc`/`heap_free` (`heap_caps_malloc`/`free` under the hood)
- `task_create` (`xTaskCreate` under the hood) -- the real test: can
  relocated PSRAM code become the entry point of a *new* FreeRTOS task, not
  just a synchronous call?
- `task_delay` (`vTaskDelay`) -- called from inside that new task
- one synthetic "driver-like" call (reads/writes a fixed in-memory struct
  standing in for a device register) -- proves the pattern generalizes past
  heap/scheduler to arbitrary hardware access, without needing a real driver

The spike test module (`CoreOSSpike/test/kspike_module.c`) is built and
signed the same way every real module is (`purrstrap modules build`), just
against the spike table's header instead of `purr_module_abi.h`.

## 3. Pass criteria

1. The spike module loads, relocates and maps executable through the
   existing, unchanged mechanism -- confirms adding heavier table calls
   doesn't disturb what's already proven.
2. `heap_alloc` returns real, usable PSRAM/heap memory: the module writes a
   pattern into it and reads it back correctly.
3. `task_create` succeeds: a *new* FreeRTOS task is created whose entry point
   is itself relocated PSRAM code (not the calling task continuing
   synchronously) and that new task runs.
4. The new task calls `task_delay` and the spike table's `puts`/`printf` and
   the synthetic driver call from *inside itself*, repeatedly, for some
   duration (tens of iterations), without crashing, corrupting the running
   shell, or triggering a cache-safety trap -- running concurrently with
   CoreOS's own tasks the whole time.
5. The synthetic driver-like call reads and writes the stand-in "register"
   correctly across several calls.

The spike passes when all five pass. This is a smaller, sharper spike than
`ModuleSpike/SPEC.md` (no rollback/tamper tests -- those are already proven
in `Modules/SPEC.md`; this is only checking the new thing: real
FreeRTOS/heap calls through a table, not just `puts`).

## 4. Non-goals

The real kernel/CoreOS split, a real driver call, purrstrap changes for
building CoreOS itself, the module loader's page-size cap growing, or any
partition table change. All throwaway spike code, cleaned up once this
spike's result is recorded here (same pattern `ModuleSpike`'s `cmd_spike*`
commands followed).

## 5. Open questions

Answered by running the spike:

- Does a relocated PSRAM blob's code work correctly as an `xTaskCreate` entry
  point, including its own stack (task stacks are heap-allocated by
  `xTaskCreate` itself, not part of the relocated blob, so this should be
  fine in principle -- confirmed or not by running it)?
- Does anything about running concurrently with CoreOS's own tasks (shared
  scheduler, shared heap) surface a problem the single-threaded, one-shot
  module calls made so far never could?

## 6. Outcome

**It passed**, run on the real T-Deck Plus (ESP32-S3), 2026-09-28. `spikecoreos` loaded and
relocated `kspike.cat` through the existing, unmodified mechanism, then reported `PASS` on
screen: heap alloc/write/read through the table succeeded, `task_create` succeeded, and the
spawned task -- itself relocated PSRAM code running as a real FreeRTOS task's entry point --
completed all 20 iterations with zero register-read/write mismatches, running concurrently
with CoreOS's own tasks the whole time. Device stayed up and stable afterward (~8+ minutes,
no crash, no watchdog trip, checked over serial).

**Conclusion:** a freestanding, relocated PSRAM blob can drive real FreeRTOS task creation
and real heap allocation through a passed-in table, not just `puts`/`printf`. The wall that
stops the *kernel* itself from being a relocatable file (needs its own `esp_system`/FreeRTOS
boot sequence) does not apply to CoreOS, since CoreOS only ever needs to call into an
*already-running* FreeRTOS/heap -- exactly what this spike exercised. `PurrOS/SPEC.md`
section 6's "CoreOS is a file the kernel loads into PSRAM" is buildable as written, not just
plausible. What's still real, unstarted work: refactoring CoreOS's actual call sites (today
direct calls to `malloc`/`vTaskDelay`/`esp_partition_*`/`mbedtls_*`/etc.) to go through a real
kernel-provided table instead, at CoreOS's actual size and breadth -- this spike proved the
*mechanism* with a small, hand-picked set of calls, not that every real CoreOS call site
converts cleanly.
