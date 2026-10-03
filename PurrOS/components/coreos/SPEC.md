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

**Designed, not built** ([F-04](../../../documentation/FINDINGS.md#f-04)). What `purr_cli.c` actually
has today: tokenizing with quotes, line editing (backspace, Ctrl-U), and a command table -- real,
tested, and in daily use. Everything else below -- pipes, redirection, `;`/`&&`/`||`/`&`, variables,
scripts, `log`/`status`/`report`/`clear-failures`/`sh`/`ps`/`top`/`kill`/`run`, job control -- is the
target shape, not the current one; `purr_cli.h` itself says "pipes come later." Read this section as
a plan to build toward, not a description of what a command written against `purr_cli.c` can rely on
existing yet.

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

### 4.2 File ownership and permission enforcement

Design for [F-10](../../../documentation/FINDINGS.md#f-10): today every file is a plain
LittleFS path with no owner or mode at all, so any command a module can reach -- `cat
/etc/shadow`, `write`/`rm` over `/etc/passwd` or `/system`, `format --yes`, `netinstall
kernel` -- works for a standard user exactly as it would for root. The account-facing
decisions are `../../../Users/SPEC.md` section 2; this is the mechanism underneath them.

- **Storage: one LittleFS custom attribute per path**, using the attribute API `purr_fs.h`
  doesn't use yet (`lfs_setattr`/`lfs_getattr`/`lfs_removeattr`, already in the bundled
  `lfs.h`) -- no separate index file that could drift out of sync with the files it
  describes. One attribute type holds `{owner_uid: u8, mode: u8}`. `mode` is two bits,
  whether anyone other than the owner can read or write the file; there are no groups, so
  nothing more than that is meaningful. Root (uid 0) and an admin acting on their own
  behalf always have full access regardless of the stored mode, the same as `su`'s existing
  escalation (`Users/SPEC.md` section 1).
- **A path's effective owner/mode is its own attribute if set, otherwise the nearest
  ancestor directory's, otherwise root-only.** This is what lets a fixed rule on `/boot` or
  `/home/<name>` cover every file written under it later (`/boot/coreos.new`,
  `/home/alice/apps/...`) without needing its own attribute set at creation time, though one
  normally still is (below).
- **The enforcement point is `purr_kernel_table_t`'s `fs_*` entries in `commands.c` --
  nowhere else.** This is the one boundary every shell-command module already calls through
  with no other path to the filesystem (section 4.1, confirmed by the fs/accounts sweeps in
  section 10). Kernel-internal code that calls `purr_fs_*` directly instead of through the
  table -- `login.c`, `purr_appmgr.c`, the mount/format logic in `purr_fs_setup()` itself --
  never crosses this check. A wrong or corrupted permission can therefore never strand the
  OS; it can only ever stop a standard user from reaching something they don't own, which is
  the point.
- **Unset is root-only, not open.** A file with no stored attribute -- true of every file
  that exists before this ships -- is treated as owned by root with neither bit set.
  Defaulting the other way would mean nothing is actually protected until something
  remembers to lock it down by hand.
- **A fixed baseline, corrected and re-verified on every mount.** A short list built into
  firmware -- `/etc` and `/etc/passwd` (root, world-read), `/etc/shadow` and `/etc/wifi`
  (root, no outside access), `/boot`, `/system`, `/kernelmods`, `/home` (root; `/system` and
  `/kernelmods` world-read since every account's shell lists and runs from them, `/boot`
  world-closed since it only ever holds in-progress component swaps) -- gets checked against
  the file's actual attribute every time `root` mounts, in `purr_fs_setup()`
  (`PurrOS/main/commands.c`, the same place the F-37 mount-retry-then-format-on-real-
  corruption logic already runs). A mismatch is corrected with `lfs_setattr`, then read back
  with `lfs_getattr` to confirm the correction took -- the same verify-after-write pattern
  `purr_net_install_run` already uses for a downloaded image -- and logged, since a
  correction is also tamper evidence. A device that has never seen this feature gets every
  baseline path set for the first time through the exact same check: "missing" and "wrong"
  are the same case. This is both the migration path for every device already running
  (including real hardware already flashed) and the ongoing self-heal.
- **Existing per-user files are not on that fixed list.** Anything already under
  `/home/<name>/` is assigned to that account by the path itself during the same pass, not
  left root-only -- the one-time upgrade is seamless for ordinary account data, only the
  fixed system paths above are locked down by default.
- **Destructive whole-device commands are a second, separate check, not a file.**
  `format`, `netinstall`, `reboot recovery`/`reboot loader`, `wifi forget` and `appformat`
  aren't really about which file is touched, so file ownership doesn't cover them; they need
  the same admin check the accounts cluster already has. Today that check exists three
  times, slightly differently, for `useradd`/`userdel`/`usermod` (`require_admin()`,
  `commands.c`), `su` (its own inline pair) and `passwd` on another account (its own inline
  triple) -- consolidating these into one shared helper and applying it to the five commands
  above closes F-10's other half. One real wrinkle: `wifi`/`netinstall` are not on
  `purr_kernel_table_t` yet, they're still on the older `purr_core_table_t`
  (`purr_module_abi.h`), so the check has to land in both tables until that migration
  happens, not just one.

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
  enforce mode it falls back to a serial prompt and does not restart -- unless the
  `boot_fail_count` ladder (`../../../bootloader/SPEC.md` section 6, not yet built) has already
  climbed past its second threshold, in which case the boot package sends the *next* boot to
  the recovery loader's silent auto mode instead
  (`../../../RecoveryLoader/SPEC.md` section 2.1) rather than a prompt nobody may be watching.
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
  (default 3) the boot package starts KittenOS instead. If the count keeps climbing from there
  -- meaning KittenOS itself is not reaching healthy either -- a second threshold (default 6)
  sends the boot package to the recovery loader's silent auto mode instead
  (`../../../bootloader/SPEC.md` section 6, `../../../RecoveryLoader/SPEC.md` section 2.1; not
  yet built).
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
  became real modules -- not a single big-bang rewrite. **A real prerequisite for this is
  now built and proven, 2026-09-28:** `purrstrap modules build --source a.c,b.c,...`
  (comma-separated) links several source files into one relocatable blob -- needed since
  CoreOS will span many files, unlike every module built so far. Proven on real hardware
  through the unmodified production loader: a module built from four files (a new test
  file, a shared freestanding `memcpy`, and two pieces of real, unmodified CoreOS
  component code -- `purr_relocate.c` and `purr_module.c`, the very functions the loader
  itself uses to load it) loaded and ran correctly. This also directly confirms section
  4.1's "not everything needs to cross the table" claim with real code, not just an
  assertion: pure computation links straight into the blob with zero table entries needed.

  **The first real slice of the table itself is now built and proven too, 2026-09-28:**
  `purr_kernel_table.h` (permanent, not a placeholder sketch), backed by a real
  implementation in `commands.c`'s `s_kernel_table` (real `heap_caps_*`/`esp_timer_get_time`
  calls) and loaded via a dedicated `/kernelmods` folder, kept separate from `/system`
  (the CoreOS-to-module boundary) so the two stay conceptually distinct even though one
  binary hosts both roles until a real kernel binary exists. The first two real commands
  converted onto it: `mem` and `uptime` (`Modules/coreos/sysinfo_module.c`) -- their old
  inline `commands.c` versions are gone, not duplicated. Confirmed working end to end on
  real hardware. Found and fixed along the way: 64-bit division (`uptime`'s
  `/ 1000000`) needs `__udivdi3` from `libgcc` -- compiler-support, not libc, so
  `purrstrap modules build` now links `-lgcc` for every module, not just this one.

  **A much bigger sweep followed the same day: the whole filesystem command cluster.**
  `ls`/`cat`/`mkdir`/`rm`/`mv`/`write`/`df`/`format` -- 8 real commands, not one --
  converted in a single pass (`Modules/coreos/fs_module.c`), growing the table with
  `fs_list`/`fs_read`/`fs_write`/`fs_mkdir`/`fs_remove`/`fs_rename`/`fs_usage`/
  `fs_mounted`/`fs_strerror`/`fs_format`/`console_flush` (ABI 1 -> 2). `fs_list`/`fs_read`
  reuse `purr_fs.h`'s own `purr_fs_list_fn`/`purr_fs_read_fn` callback types directly --
  already proven safe in a freestanding build, since `purr_module_abi.h` already pulls in
  `purr_fs.h` transitively and every real module already compiles against it. `fs_format`
  stays one opaque call, not decomposed (same reasoning as `netinstall`'s `net_install`,
  `Modules/SPEC.md`): formatting needs the raw block device and an unmount first, real
  low-level access a module is never handed. Confirmed working end to end on real
  hardware, the full cluster at once. This is the proof that the incremental-conversion
  pattern scales past one command at a time to a whole coherent subsystem in one sweep,
  not just single trivial commands.

  **A third sweep, the same day again: the accounts cluster.** `whoami`/`id`/`su`/`passwd`/
  `useradd`/`userdel`/`usermod`/`logout` -- another 8 real commands at once
  (`Modules/coreos/login_module.c`), growing the table to ABI 3. Unlike the filesystem
  sweep, only `whoami`/`id` are plain read-only queries (`login_whoami`, into a
  module-safe `purr_klogin_who_t`, reusing `purr_users.h`'s `purr_user_role_t` directly --
  same safe tier as `purr_fs.h`). Every other command is security-sensitive (a password, an
  admin check, the shadow file), so each stays one opaque, `cli`-aware call
  (`login_su`/`login_passwd`/`login_useradd`/`login_userdel`/`login_usermod`) -- the same
  shape as `net_install`/`fs_format`: all I/O, including the masked password prompt (which
  needs the kernel's own raw keyboard read), and all permission/shadow-file logic stay
  kernel-side, unconditionally re-checked there regardless of what the module's own
  pre-check (e.g. "already root") already believed. The module ends up pure argument
  parsing and dispatch -- no password, admin check, or shadow-file access ever reaches
  module code. Confirmed working end to end on real hardware, all 8 commands plus the
  earlier two sweeps together (18 real commands total now off the monolith and onto the
  kernel table).

  **A fourth sweep, same evening: app management.** `appinfo`/`appinstall`/`appremove`/
  `appformat` (`Modules/coreos/appmgr_module.c`), growing the table to ABI 4
  (`app_info`/`app_install`/`app_remove`/`app_format`). Same shape as the accounts sweep --
  every one of these touches the signing key bag (installing/removing an app verifies
  against it) or the raw per-user apps filesystem, so all four stay one opaque, `cli`-aware
  call each, same reasoning as `net_install`/`fs_format`/`login_*`. Confirmed working end to
  end on real hardware alongside all three earlier sweeps. **22 real commands total** now
  converted off the monolith and onto the kernel table (`mem`/`uptime`, the 8-command
  filesystem cluster, the 8-command accounts cluster, the 4-command app-management
  cluster).

  **A fifth and last sweep, same evening: everything else in the built-in table.**
  `version`/`info`/`parts`/`echo`/`clear`/`reboot`/`purrcfg` -- folded into
  `Modules/coreos/sysinfo_module.c` alongside `mem`/`uptime` (growing it from 2 to 9
  commands), ABI 4 -> 5. `version`/`info`/`parts`/`purrcfg` are read-only display commands
  with no reason to decompose into structured data, so each `print_*` entry just prints
  directly onto `cli`, same as `net_install`/`fs_format`'s own progress messages. `reboot`
  touches the raw `purrcfg` partition (the one-shot boot-target flag), so it stays one
  opaque `reboot_system` call. **31 real commands total** now converted off the monolith --
  everything that was inline in `commands.c` is now either a real module
  (`about`/`apps`/`wifi`/`netinstall`, the CoreOS-to-module boundary) or a kernelmod (the
  kernel-to-CoreOS boundary), except the login gate itself and the temporary scaffolding
  (`plantmodules`/`spikecoreos`/`testrelocmulti`).

  **Confirmed working end to end on real hardware, with one separate, pre-existing finding
  along the way (not caused by this sweep):** `reboot`/`purrcfg` themselves work correctly,
  but `reboot recovery`/`reboot loader` don't actually show a boot menu or land anywhere,
  because the device has never had the real custom bootloader (`../bootloader/`, its own
  separate build producing `purr_bootloader.bin` with the actual menu/recovery-dispatch
  logic) flashed during this whole rewrite's fast dev-loop -- every flash all session has
  been `PurrOS/build/tdeck_plus-<profile>`'s own auto-generated stock ESP-IDF bootloader,
  which has no menu and doesn't act on the `purrcfg` flag at all. Not a regression: nothing
  in any sweep touched partitions, the bootloader, or the boot package. Real follow-up work,
  tracked separately from the kernel-table conversion. (This finding is now stale in its own
  right: the real bootloader was built and flashed for real starting 2026-09-28, see
  `bootloader/SPEC.md`.)

  **A sixth sweep, 2026-09-30: the login gate itself, not a shell command.** ABI 5 -> 6,
  `read_key`/`login_run`/`login_skip`/`login_take_logout` added (`login_whoami`, already in
  the table, doubles as "who is currently logged in" for this too). Different in kind from
  every earlier sweep: nothing here becomes a loaded module today -- `main.c`'s own
  `run_login()`/`run_shell()` (PurrOS/SPEC.md section 6's future CoreOS boot orchestration)
  now call through `purr_kernel_table()` instead of `purr_login_*()`/`purr_kernel_key()`
  directly, so that code is already written the way it would have to be once it actually
  moves into a separately loaded CoreOS, rather than needing a rewrite then. `purros_installed()`'s
  raw `esp_partition_*` check and `login.c`'s own internals (still real keyboard/console/
  shadow-file access, unchanged) deliberately stay outside this -- they're the *implementation*
  of `login_run`/`login_skip`, kernel-side, same as `login_su`/`login_passwd`'s bodies always
  were.

  **A real bug found and fixed along the way, unrelated to the ABI change itself:** every ABI
  bump this session requires the four kernelmods to be rebuilt, re-signed and re-embedded --
  but re-flashing the app image never touches what's already sitting on the device's root
  LittleFS, so the *old*, ABI-5-signed `/kernelmods/*.cat` files stayed in place and were
  correctly rejected as an ABI mismatch after this bump, since nothing had ever re-planted
  them. `plant_temp_modules()` (shared by the manual `plantmodules` command and, now,
  `purr_modules_setup()` itself) fixed this by replanting every embedded temp module/kernelmod
  automatically on every boot, not just on request -- acceptable only because this whole
  mechanism is already explicitly temporary scaffolding with no real distribution path yet;
  remove this call the same day a real one replaces it. Confirmed working end to end on real
  hardware: all four `/system` modules and all four `/kernelmods` load clean after the bump,
  with no further manual step needed.

  **The real replacement is now built, 2026-09-30:** `OTA/SPEC.md` sections 3 and 6.1 --
  `netinstall modules` fetches the module index, then each module/kernelmod it lists for this
  chip/board, verifies each the same way `appinstall` verifies an app, and writes it straight
  to `/system/<name>.cat` or `/kernelmods/<name>.cat`. `purr_modules_setup()`'s automatic call
  to `plant_temp_modules()` is now disabled; the function and the manual `plantmodules` command
  stay as a no-network local-dev fallback. Host-tested and compiles clean on-device; not yet
  run against a live device over real Wi-Fi (no network credentials or reachable server
  available this session) -- a real, open follow-up, not a known bug.
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
