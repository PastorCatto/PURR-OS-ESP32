# CoreOS spec (draft 0.4)

CoreOS is the **generic base platform** that every image is built on: the normal system,
KittenOS and, in a minimal form, the recovery loader. It is one source tree, built as a
**profile** for each case (section 1.1). It does the basics: networking (Wi-Fi), reading the
handoff, verification, and a small environment to work in, meaning the shell. It is a mutable
base that is adapted for each case, and it receives updates.

In the normal system it runs on the kernel, loads AppManager and the runtimes, hosts the shell
engine, the runtime manager and the memory pressure service, and hands over to KittenOS when
things go wrong. Layering and boot order are in `../../SPEC.md`.

## 1. Responsibilities

1. Bring up logging and the boot report. The kernel already provides the console and the
   filesystem.
2. Read the boot package's result (the handoff).
   Networking basics: the Wi-Fi connection service (`../../../Network/SPEC.md`).
3. Verify its own files.
4. Decide: continue, continue with a warning, or fall back to recovery.
5. Load AppManager, then the runtimes, from `/boot`, and check that each is compatible.
6. Host the runtime manager (`AppRuntime/SPEC.md`) and the command-line engine.
7. Watch memory and decide which app to stop when it runs short.
8. If it cannot continue, or the system keeps failing, ask for KittenOS.

Non-goals for now: user accounts, UI and remote access. In the normal system the drivers and
the filesystem belong to the kernel.

### 1.1 Profiles

One source tree, three profiles. The profile decides which modules are compiled in, and adapting
for a board or a case is a matter of choosing the profile and the board.

| Profile | Built into | Holds |
|---------|------------|-------|
| **minimal** | the recovery loader | Wi-Fi and TLS, the keys, verification and a flash writer, nothing else |
| **recovery** | KittenOS | minimal, plus a filesystem reader, the shell, the file-swap routine, a small driver set and mini-apps |
| **full** | the normal system | recovery's platform, plus everything on the kernel: AppManager, the runtimes, the runtime manager and the memory pressure service |

**Each image carries its own copy** of the CoreOS code. KittenOS's copy stays frozen and is
refreshed through the recovery loader, which downloads the newest KittenOS (the boot menu
gets an "update KittenOS" entry); the loader's own copy almost never changes. Both stay
monolithic, linked straight into their own raw partition, same as always. Keeping the copies
separate means recovery never depends on what it might be recovering.

**The normal system's copy is different on modular boards** (`PurrOS/SPEC.md` section 1,
updated 2026-09-28): it is a file in `/boot`, loaded into PSRAM by the **kernel** -- no longer
baked into the same flashed image as the kernel. See section 4.1. On monolithic boards
nothing changes here: kernel and CoreOS are still linked into one packed image.

## 2. Shared ABI

One header, `purr_abi.h`, defines every structure shared with the bootloader:

- the image container header
- `purrcfg`
- the bootloader handoff struct

Rules:

- Plain C, standard integer types only, no ESP-IDF includes. The bootloader
  includes the same file, so there is exactly one definition of each layout.
- Little-endian, packed, fixed-size, with explicit reserved bytes.
- Every struct has a version field and a `static_assert` on its size.
- Field lists and meaning come from `bootloader/SPEC.md`. Exact sizes and
  offsets are fixed when the header is written, and then that header is the
  authority for layout. The bootloader spec keeps authority for behavior.

## 3. Modules

Every module has one public header and one source file. Return types are
`esp_err_t`, except verification, which returns a `purr_verify_result_t` so
callers can tell why an image failed.

### 3.1 purr_log (console and boot report)

- Thin wrapper over `esp_log` with one tag per module, so output is uniform.
  AppManager, the runtimes and everything above use it.
- `purr_boot_report()` prints one block: chip, board, flavor, versions,
  handoff state, `secure_mode`, active flags, self-check result, key bag summary
  (IDs and roles, never key material), and the kernel result.
- A hook lets a later UI register a callback to show boot warnings on screen.
  Until then the warning is only logged.
- **Log buffer:** an in-RAM ring buffer (default 8 KB) keeps recent lines and is
  read with the `log` command. Output from apps that are not in the foreground
  goes into it, with the instance id and name in front. Whether it is also
  written to flash is open (section 10).

### 3.2 purr_handoff (bootloader state)

- Read the struct the bootloader and the boot package left for the OS.
- Validate its magic and CRC. Missing or invalid is reported as `no handoff`,
  which is the normal case under a stock bootloader.
- Uses ESP-IDF's reserved RTC FAST memory
  (`CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC`), which has a fixed address for the
  bootloader and the app and survives reboots but not power loss. Both builds
  must use the same option and size, and the size must fit the handoff struct.
  The app side reads it through `bootloader_common_get_rtc_retain_mem()`.
  Confirm that access from an app build at implementation time.

### 3.3 purr_cfg (`purrcfg` access)

- Load the newer valid copy of two A/B sectors (highest `seq`, valid CRC).
  Fall back to defaults if neither is valid.
- Write the other sector, bump `seq`, read back and verify, so power loss never
  leaves zero valid copies.
- Typed getters for `secure_mode`, flags, revoked keys and the key bag.
- Typed setters only for fields runtime code may change. Requests that change
  trust state (key update, `secure_mode`, eFuse request) go through one entry
  point that checks the signature when secure boot is enabled and accepts it
  unsigned when disabled, per `bootloader/SPEC.md` 5.1.
- CoreOS never writes `boot_seq`. Only the bootloader does.
- CoreOS stores eFuse arm flags and requests but never burns anything.
- Two fields are added to `purrcfg`: `boot_fail_count` (section 7) and `update_state`
  (`PurrOS/SPEC.md` section 6.1). Both are in `bootloader/SPEC.md`.

### 3.4 purr_keybag

- Builds the effective key bag: compiled-in defaults with overrides from
  `purrcfg` applied slot by slot.
- Lookup by `key_id` and role, with revoked checks. Read-only. Changing a key
  goes through `purr_cfg` as a request.
- Default keys live in a generated source file. Until a real key exists the
  default set is empty, and a build with secure boot enabled fails at boot
  rather than silently trusting nothing.

### 3.5 purr_image (container reading and verification)

- Read the container header from a partition or flash offset.
- Check the header without hashing: magic, `header_version`, `header_size`,
  `chip_id`, `image_type`, `min_boot_version`, and, in `enforce` mode, the version floor for that
  component from `purrcfg`.
- Hash the payload in chunks (bounded RAM, no full-image buffer).
- Verify the signature over the header against the key bag, using a key whose
  role matches `image_type`.

Result codes: OK, BAD_MAGIC, BAD_VERSION, BAD_CHIP, BAD_TYPE,
TOO_OLD_BOOTLOADER, BELOW_FLOOR, BAD_HASH, BAD_SIGNATURE, NO_KEY, ROLE_MISMATCH, REVOKED,
IO_ERROR.

Verification is split so the bootloader can reuse it:

- `purr_verify` (plain C, no ESP-IDF): all the policy and checks above.
- A crypto backend behind one function, `verify_p256(pubkey, hash, signature)`.
  CoreOS uses mbedtls. The bootloader uses micro-ecc, which it already links.

### 3.6 purr_mem (memory pressure service)

CoreOS decides when memory is short and which app is stopped. It does not stop
apps itself, and it does not know how apps are run. The runtime manager
(`AppRuntime/SPEC.md` sections 6 and 7) registers with it, and CoreOS only calls
the callbacks it was given.

- Watches free internal memory and the largest free block against two thresholds,
  `low` and `critical`, with hysteresis. Checks on allocation failure if the
  platform offers a hook, and on a slow timer.
- **Registration:** the manager supplies `on_pressure(level)`, a way to read its
  registry (importance, size, last use per instance), and `stop(instance, reason)`.
- At `low`: calls `on_pressure(low)`, and the manager tells apps to drop caches.
- At `critical`: picks a victim by the order in `AppRuntime/SPEC.md` section 7,
  calls `stop`, and repeats until free memory is above `critical` plus a margin.
  A minimum interval between stops prevents a kill loop.
- **Never stops** CoreOS, the kernel, AppManager, the manager, runtime modules or
  anything flagged persistent. If only protected memory users are left, it logs
  that and does not stop innocent apps.
- Static tables, no heap use, and it works when no manager is registered (it then
  only logs).

### 3.7 purr_cli (command-line engine and shell syntax)

A small shell engine on the console, used by CoreOS's own shell, by KittenOS, and by the shell in
AppManager (`AppManager/SPEC.md` section 8.1).

**Engine**

- Reads a line from the console with basic editing, splits it into arguments, and
  runs the matching command.
- **Command registry:** `register(name, help, fn)`, where `fn(argc, argv, io)`
  returns an exit code. Modules register commands through the host API, so the
  kernel, the runtime manager and AppManager each add their own without CoreOS
  knowing them.
- **`io` handle:** standard input, output and error for a command or an app. It can
  be the console, the system log, a pipe or a file. Commands never write straight
  to the UART, so the same command works on the serial console now and on an
  on-screen text console later.
- Built-in commands: `help`, `version`, `reboot`, `log`, and `sh`.
- Fixed-size tables (registry 64 entries, line buffer, job pool), no heap use, so it
  works in KittenOS and in a degraded CoreOS when the rest of the system is broken.
- Commands that change trust state (keys, `secure_mode`, eFuse requests) are not
  available through it. Those go through `purr_cfg`'s signed request path.

**Shell syntax** (basic and Unix-style)

- `a b "c d"`: arguments, with single and double quotes.
- `a ; b`: run in order.
- `a | b`: pipe. The output of `a` becomes the input of `b`.
- `a > file`, `a >> file`, `a < file`: redirect output, append, redirect input.
- `a && b`, `a || b`: run `b` depending on the exit code of `a`.
- `a &`: run in the background. For an app this starts an instance and leaves the
  console with the shell.
- `NAME=value`, `$NAME`, `$?`: simple variables and the last exit code.
- `# comment`, and `sh file` to run a script with one command line per line.

Not in the first version: loops, functions, conditionals beyond `&&` and `||`,
wildcards, command substitution.

**Pipes**

- A pipe is a bounded in-RAM buffer (default 1 KB) with blocking reads and writes
  and an end-of-input signal. Each stage of a pipeline runs as its own short task,
  so a producer can produce more than the buffer holds.
- Fixed limits: 4 stages per pipeline and 4 jobs at once, from a static pool.
- A stage can be a command or an app instance. An app started in a pipeline has its
  `console` bound to the pipe instead of the terminal.

**Redirection and scripts need a filesystem,** which the kernel provides. If it is not
mounted, only pipes, `;` and `&&`/`||` work, and redirection reports that there is no
filesystem.

**Where output goes by default.** A foreground job writes to the console. A
background app's standard output and error go to the system log, with `[id name]`
in front of each line, and its input is empty. Anything redirected explicitly goes
where it was told.

## 4. Boot flow: `purr_coreos_boot()`

Called by the kernel when it starts CoreOS.

1. Logging.
2. Read the handoff.
3. Load `purrcfg`, build the key bag.
4. Self-check (section 5).
5. Decide (section 5).
6. Print the boot report.
7. If the decision allows, load AppManager and the runtimes (section 6).
8. Mark the boot healthy: reset `boot_fail_count` in `purrcfg` and, if an update is
   `unconfirmed`, confirm it (`PurrOS/SPEC.md` section 6.1).
9. Return a summary: handoff state, self-check result, decision, module results, and
   whether a boot warning is active.

If any step fails, the function still returns a summary marked degraded and does not
panic. If it cannot continue at all, it records KittenOS as the boot target in `purrcfg`
and restarts. It starts no tasks except the guarded module starts in section 6.

### 4.1 The kernel table (modular boards)

`purr_coreos_boot()` on a modular board doesn't just get called by the kernel -- it's
handed a table, `purr_kernel_table_t`, the same shape of contract section 6 already uses
one layer up (CoreOS to AppManager/modules): CoreOS never calls the kernel by name, only
through this table. Settled by `CoreOSSpike/SPEC.md` passing: this has to be true because
CoreOS is now a freestanding, relocated PSRAM file (`-nostdlib -ffreestanding`, no libc or
FreeRTOS linkage of its own), and everything it needs that depends on the kernel's
already-running state -- the heap, the task scheduler, real hardware, the mounted
filesystem -- has to be reached through a table for the same reason a module reaches
CoreOS's services through `purr_core_table_t`.

**Not everything needs to cross this boundary.** Pure, self-contained computation that
doesn't touch the kernel's runtime state can be compiled straight into CoreOS's own
freestanding blob, the same way a module's own local `streq()`-style helpers already are
today: `purr_relocate`/`purr_module` (already plain, host-tested C), string formatting
(CoreOS needs its own small freestanding `snprintf`-equivalent, not libc's), and the
manifest parser. **Genuinely open, not yet decided:** whether the crypto stack
(`mbedtls`, used by `purr_verify`/`purr_keybag`) can compile freestanding as-is or needs
its own libc shims/table entries -- real work to find out, not assumed either way.

**What has to cross the table** (first cut, grows as real call sites convert, the same
way `purr_core_table_t` grew field by field this rewrite): a representative shape, not
exhaustive --

```c
typedef struct {
    /* Heap: proven by CoreOSSpike -- a relocated blob calling into the kernel's
     * already-running heap works. */
    void *(*heap_alloc)(size_t n);
    void (*heap_free)(void *p);

    /* Tasks and locks: proven by CoreOSSpike for task_create; mutexes/semaphores are the
     * same idea, not yet built. */
    int (*task_create)(void (*entry)(void *arg), const char *name, uint32_t stack_words,
                       void *arg, int priority);
    void (*task_delay)(uint32_t ms);

    /* The mounted root filesystem -- the kernel owns the flash/LittleFS driver
     * (responsibility #1), CoreOS never touches it directly. Same shape purr_fs_t's
     * calls already have today, reached through the table instead. */
    int (*fs_read)(const char *path, void *ctx, purr_fs_read_cb cb);
    int (*fs_write)(const char *path, const void *data, uint32_t len);
    /* ...list/mkdir/rename/stat, same set purr_fs.h already defines */

    /* Raw flash/partition access, for the kernel's own update path and (once retargeted)
     * netinstall. */
    int (*partition_read)(const char *label, uint32_t off, void *buf, uint32_t len);
    int (*partition_write)(const char *label, uint32_t off, const void *buf, uint32_t len);
    int (*partition_erase)(const char *label, uint32_t off, uint32_t len);

    /* Hardware the kernel already owns display/keyboard drivers for
     * (purr_kernel_display()/purr_kernel_key() today) -- CoreOS reaches them here instead
     * of calling into kernel code directly. */
    const purr_display_v2_t *(*display)(void);
    char (*key)(void);

    /* Wi-Fi: responsibility #2 lists this as CoreOS's job, but the actual esp_wifi_*
     * driver calls are ESP-IDF/kernel-side -- CoreOS drives the connection *policy*
     * (purr_net.c today) through calls the kernel exposes here. */
    int (*wifi_scan)(/* ... */);
    int (*wifi_connect)(/* ... */);
} purr_kernel_table_t;
```

This is a design placeholder, not a frozen ABI -- it gets filled in properly, field by
field, as CoreOS's real call sites actually convert (section 10).

## 5. Self-check and the bootloader's result

**Inputs**

- The handoff `boot_state`: verified, failed_warned, forced_recovery,
  config_default, or none.
- CoreOS's own check: verify the running image (its container header, payload
  hash and signature) with `purr_image`, the same rules the bootloader used.
- `secure_mode` from `purrcfg`: off, warn or enforce.

The self-check exists because the bootloader may not have verified this image
at all (stock bootloader, or a bootloader that was bypassed). CoreOS does not
just trust the handoff.

**Decision**

| Bootloader | Own check | off | warn | enforce |
|------------|-----------|-----|------|---------|
| verified | pass | continue | continue | continue |
| verified | fail | continue | continue with warning | recovery |
| failed_warned | any | continue | continue with warning | recovery |
| none (stock bootloader) | pass | continue | continue | continue |
| none | fail | continue | continue with warning | recovery |
| forced_recovery | any | continue | continue | continue |

"Recovery" means: set the `FORCE_RECOVERY` flag and reboot into KittenOS. Rules:

- KittenOS never falls back to itself. If KittenOS fails its own check in
  enforce mode it falls back to a serial prompt and does not restart.
- Automatic recovery reboots are limited to one per boot sequence, so a
  verification failure cannot cause an endless reboot loop. The second time,
  the system stays in a degraded shell.
- A dev build with no container header and `secure_mode` off is allowed: the own
  check reports "unsigned" and the decision is continue.

## 6. Loading AppManager and the runtimes

On modular boards these are signed files in `/boot`, loaded into RAM (PSRAM) by CoreOS
with the same relocation loader design as apps. On monolithic boards they are linked into
the packed image and started through the same entry contract.

**Entry contract.** Each module exposes one table:

```
purr_module_entry_t {
    abi_version, min_coreos_version,
    init(const purr_boot_info_t *, const purr_api_t *),
    status(purr_module_status_t *),
    deinit()
}
```

**What CoreOS passes in**

- `purr_boot_info_t` (read-only): handoff state, `secure_mode`, decision, flavor, CoreOS
  version.
- `purr_api_t`: a versioned table of everything the module needs: memory, logging, tasks
  and locks, the catcall registry, the filesystem, `purrcfg` getters, verify helpers and
  command registration. A module never calls CoreOS or the kernel by name, so CoreOS can
  change without rebuilding the module as long as the table's major version holds.

**Loading steps**

1. Verify the file with `purr_image`: chip ID, module type, key role, hash and signature.
2. Check the declared table version and `min_coreos_version`. A mismatch is a load
   failure, not a crash.
3. Allocate memory, load and relocate.
4. Find the entry table and check `abi_version`.
5. Call `init` inside a guarded task with a timeout and the task watchdog on.
6. Record the result. A missing or failed AppManager leaves the system running in a
   degraded mode with the shell only, and is shown in the boot report.

**System file updates.** CoreOS does not replace its own files or the others. A module
stages and verifies a new file and asks for the update; KittenOS applies it
(`PurrOS/SPEC.md` section 6.1). CoreOS confirms a fresh update once it starts healthy
(section 4, step 8).

**Module container additions** (for `bootloader/SPEC.md` section 3 when the loading
spike settles): entry offset, required table version, required CoreOS version, RAM needed
for `.data` and `.bss`.

## 7. Failure handling

CoreOS no longer hosts the last-resort support mode. That is KittenOS's job
(`KittenOS/SPEC.md`).

- **Failure counter.** The boot package increments `boot_fail_count` before it loads the
  kernel. CoreOS resets it when the boot is healthy (section 4, step 8). At the threshold
  (default 3) the boot package starts KittenOS instead.
- **Asking for KittenOS.** CoreOS records KittenOS as the boot target and restarts when
  the decision in section 5 says "recovery", or when it cannot continue.
- **Shell commands.** The status and repair commands are ordinary commands in the shell
  engine: `status`, `report`, `version`, `reboot`, `reboot recovery`, `clear-failures`.
  They do not change trust state. `secure_mode` and keys cannot be changed from the shell.

## 8. Constraints

- Static allocation in the boot path, so a failing boot does not depend on heap
  state.
- Each public function documents whether it is thread-safe.
- Builds for `esp32` and `esp32s3` on ESP-IDF v5.3.5. Chip-specific code is not
  allowed here. It goes in the per-board sdkconfig or the kernel.
- **On a modular board, the `full`-profile CoreOS build is freestanding** (`-nostdlib
  -ffreestanding`, section 4.1) -- no direct libc, FreeRTOS or ESP-IDF driver linkage.
  Everything needing the kernel's runtime state goes through `purr_kernel_table_t`; pure
  computation can still be linked straight in. This does not apply to the `minimal` or
  `recovery` profiles (the recovery loader, KittenOS), which stay normal linked binaries,
  or to any profile on a monolithic board.

## 9. Testing

- Pure C parts (header checks, key bag, `purrcfg` A/B logic, handoff validation,
  the decision table, the fail counter) get host-side tests with a fake flash.
  The decision table is tested exhaustively, every row and mode.
- Verification needs signed sample images from purrstrap in
  `test/vectors/`. Core's tests must verify what purrstrap signs, and
  purrstrap's tests must parse what the C code accepts.
- The update request, confirm and failure-counter logic is tested with a fake `purrcfg`:
  every state, and a restart at every step. The swap itself is tested under KittenOS.
- The command-line engine is tested with a fake console and a fake filesystem:
  quoting, unknown commands, a full registry, long lines, commands registered by a
  fake module, each piece of syntax above, a pipe where the producer outputs more
  than the buffer holds, a full job pool, redirection with no filesystem, and
  background output reaching the log with its prefix.
- The memory pressure service is tested with a fake heap and a fake manager: the
  thresholds, hysteresis, victim order, protected users, the minimum interval,
  and running with no manager registered.
- Module loading is tested with fake module files: good, wrong table version, `init`
  returns an error, `init` hangs, bad signature.
- Hardware and boot-flow tests come after the bootloader implements the spec.

## 10. Open questions

- **`purr_kernel_table_t`'s real shape** (section 4.1): today's sketch is a placeholder.
  Filling it in for real means converting CoreOS's actual call sites (today's direct
  `malloc`/`vTaskDelay`/`esp_partition_*`/display-kernel calls throughout `commands.c`,
  `purr_appmgr.c`, `purr_net.c`, etc.) one subsystem at a time, the same incremental way
  `purr_core_table_t` grew field by field as `about`/`apps`/`wifi`/`netinstall` each
  became real modules -- not a single big-bang rewrite.
- **Whether `mbedtls` compiles freestanding as-is: tried, 2026-09-28, decisive result
  (exploratory only, not committed code -- CoreOSSpike/build, deleted after).** The actual
  ECDSA/ECP/ASN.1/HMAC-DRBG verify code (`ecdsa.c`, `ecp.c`, `asn1*.c`, `hmac_drbg.c`,
  `ecp_curves.c`, `constant_time.c`, `platform_util.c`) compiles `-nostdlib -ffreestanding
  -fno-builtin` cleanly with no language-level blocker, once the right ESP-IDF include
  paths and the project's generated `sdkconfig.h` are supplied. What it actually needs:
  - **Memory already goes through mbedtls's own pluggable hooks**
    (`mbedtls_calloc`/`mbedtls_free`), not raw libc `malloc` -- these map onto the kernel
    table's `heap_alloc`/`heap_free` with zero mbedtls source changes.
  - Basic `memcpy`/`memset`/`memcmp`/`memmove`/`strcmp`/`strlen` need local freestanding
    implementations, same as a module's own `streq()` today.
  - `__udivdi3` needs `libgcc` linked -- compiler-support, not libc, a build-flag fix.
  - Randomness needs an RNG source (`mbedtls_hardware_poll`) -- a `rng_bytes`-style kernel
    table entry, backed by the real HWRNG.
  - **The one real finding:** this project builds with `CONFIG_MBEDTLS_HARDWARE_MPI=1`,
    so the actual big-number math (`mbedtls_mpi_mul_mpi`/`exp_mod`/etc.) is compiled out of
    `bignum.c` (`#if !defined(MBEDTLS_BIGNUM_ALT)`) in favor of the ESP32's hardware
    MPI/RSA accelerator peripheral -- a real hardware dependency needing exclusive-access
    locking, exactly the kind of thing the kernel table exists to mediate. Two ways
    forward, not yet decided: expose the hardware MPI accelerator through the table too
    (keeps the speed, more table surface), or build CoreOS's mbedtls with hardware MPI
    turned off (pure software bignum, fully self-contained, no table entry -- verification
    isn't a hot path, so the slowdown likely doesn't matter).
  
  Not yet tried: an actual successful link+run (only individual compiles and one `ld`
  attempt with stub symbols were done) -- this answers "is it fundamentally freestanding-
  compatible" (yes), not "does it verify a real signature correctly from a relocated
  PSRAM blob."
- Whether the boot button (GPIO0) held at startup should start KittenOS.
- Whether the `safe` command is needed in v1 or is a later addition.
- The exact threshold and timeout defaults for kernel start.
- Whether all runtime writes to `purrcfg` go through CoreOS only.
- Whether the log is also written to LittleFS (rotated and size-capped to limit flash
  wear) so it survives a reset, or stays in RAM only.
- Where a boot counts as healthy: after AppManager loads, or earlier.
- Whether the failure counter is incremented by the boot package before loading, as assumed
  here, or by the kernel.
