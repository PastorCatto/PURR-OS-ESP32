# Modules (draft 0.1)

The real core/module system: a tiny fixed core plus loadable, relocatable modules from a
LittleFS system folder — `/system` on the root filesystem, every `.cat` file in it loaded at
boot. `ModuleSpike/SPEC.md` proved the underlying mechanism works on real hardware; this is
the design for the real thing, not a throwaway test. See the pinned module-loader project
memory for how this got here, and `CatFormat/SPEC.md` for the sibling format this borrows its
relocation method from.

## 1. Scope

Modular boards only (`PurrOS/SPEC.md` section 1.1), same as the spike. This spec covers the
module container, its relocation, and the core/module call table. It does not cover which
real subsystems move into modules first (a separate decision once this exists) or the core's
own OTA update path (already decided in `ModuleSpike/SPEC.md` section 0.1: reuse the existing
BOOT-role signing gate, nothing new).

## 2. Container

A module is a `PURR_IMG_MODULE` image (`bootloader/SPEC.md` section 3, `purr_image_header_t`)
— the same container every other signed thing in this codebase uses. No new header format.

**Correction from draft 0.1's first pass:** that draft said the relocation list would ship in
"the container's existing manifest area (`CatFormat/SPEC.md` section 5)". Checked against the
actual code before building anything on top of it: `CatFormat/SPEC.md`'s payload table and
manifest block are still aspirational — `purr_appmgr.c` doesn't implement any of it today,
and neither does anything else. A module's payload is just the one flat payload the common
header already points to (`payload_offset`/`payload_size`), like every other image type. The
relocation list and entry point live inside that payload, in a small prefix purrstrap writes
(section 4), not in infrastructure that doesn't exist yet.

## 3. One segment, not several

`CatFormat/SPEC.md` section 6 left segment tags open ("keeps both loading models possible...
because the loading model is still open"). The spike settled the loading model: PSRAM copy,
one contiguous buffer, one base address. So a module's payload is built as **one merged
segment** — code, literals, rodata, and initialized data all placed together by a dedicated
linker script, instead of GCC's normal separate `.text`/`.rodata`/`.data` layout. This means
every relocatable word in the payload needs the exact same fix-up: add the buffer's actual
runtime address. No segment tags, no per-kind handling.

Zero-initialized data (`.bss`) is not stored in the file, same as CatFormat — just a size the
loader clears after copying.

## 4. Relocation

- The module is linked twice by purrstrap, once at base address `0x00000000` and once at a
  second base far enough away that a real address difference is unambiguous (e.g.
  `0x00100000`), using the same merged-segment linker script both times.
- The two flat binaries are compared word by word (32-bit aligned). A word that differs by
  exactly the base difference is a relocation: record its offset. Any other difference is a
  build error — it means something in the module isn't relocatable this way (inline assembly
  with a hardcoded address, for instance) and purrstrap refuses to package it.
- Because the link base was `0x00000000`, a to-be-relocated word's stored value **is already
  the target offset from the start of the module**. Applying a relocation at load time is
  just: `word += actual_load_address`. No per-relocation base lookup.
- `purr_relocate()` (`PurrOS/components/coreos/`) is the pure function that applies a
  relocation list to a loaded buffer — built and host-tested (26 checks): given a buffer, an
  array of 4-byte-aligned offsets, and a base value, it adds the base to the word at each
  offset, or fails leaving the buffer completely untouched if any offset is bad.

**Two lists, not one (revised after building the first real module — section 11's postmortem
has the full story).** A first version used one list and one base for everything, on the
theory that "a word that needs the load address added" covers every case uniformly. It
doesn't: a word holding a **function's** address must resolve to wherever the module ends up
mapped *executable*, since that's the only alias anything is ever called through, and that
mapping is typically exec+read only, not writable. A word holding an **object's** address — a
global that gets written to, a string, a const table, read or written — must resolve to the
plain *writable* copy instead, since it supports both, and the executable alias might not
support writes at all. A module with a global it initializes at load time crashed the device
(a real ESP32 hardware cache-safety trap) the instant it tried, because a single-base scheme
had put that global's address on the executable alias.

purrstrap tells the two apart per relocation using the compiler's own symbol types
(`readelf -s`'s FUNC/OBJECT column) — not `nm`'s section-letter classification, which the
merged single-segment layout (section 3) makes unreliable: a `const` struct or string placed
in `.text` by `-mtext-section-literals` still shows up as a "text" symbol even though it's
data, never executed. A relocation whose pre-fixup value exactly matches a known `FUNC`
symbol's address goes in the **code** list; everything else goes in the **data** list.

## 5. Payload layout (replaces the spike's hardcoded entry offset)

The spike hardcoded "the entry function is at payload offset 8" in a comment. The real thing
can't do that, and (per section 2's correction) there's no manifest block to carry it in
either. So purrstrap prepends a small, self-contained prefix to the payload — no cooperation
needed from the module's own source, no custom linker section for it:

```
[ entry_offset      : u32 ]                    -- offset of the entry function, into the code blob
[ data_reloc_count  : u32 ]
[ data_reloc_offsets: u32 * data_reloc_count ]  -- relocate against the writable base
[ code_reloc_count  : u32 ]
[ code_reloc_offsets: u32 * code_reloc_count ]  -- relocate against the executable base
[ code blob ]                                   -- the base-0 linked flat binary, byte for byte
```

`entry_offset` and every offset in both tables are relative to the start of the code blob (the
byte right after the last `code_reloc_offsets` entry), not the start of the payload — purrstrap
finds `entry_offset` from the base-0 build's symbol table (the same way a human read it off
`objdump` in the spike; this is that lookup automated) and it needs no alignment of its own,
since Xtensa's 16-bit "narrow" instructions can start on a 2-byte boundary.

Every fixed-size field is read with a byte-safe access (`memcpy`, not a struct overlay) since
the payload buffer's alignment isn't guaranteed — same caution `purr_relocate` itself already
takes. `purr_module_parse_layout()` (`PurrOS/components/coreos/purr_module.c`) does this,
host-tested (38 checks): rejects a truncated prefix, a reloc count over `PURR_RELOC_MAX`, a
reloc table that doesn't fit the payload, or an `entry_offset` outside the code blob, before
any of it is trusted.

## 6. The call table

Two-way, unlike `.cat`'s one-way `catcall_get`:

- The **core → module** direction is the payload prefix's `entry_offset`, called once at load
  time as `const void *module_entry(const purr_core_table_t *core);` — the module gets a pointer to
  the core's own table (log, alloc, driver/pin access, whatever the real core ends up
  exposing) and returns a pointer to its own table for the core to keep and call through
  later. Both tables are plain structs of function pointers, versioned by `abi_version`.
- The **module → core** direction is just calling through the function pointers in the table
  the module was handed. No relocation involved (it's a runtime argument, not a compiled-in
  address), proven directly in the spike.
- A module's returned table is itself sitting in the module's relocated memory. Its non-`fn`
  fields (if any) are data relocations; any function pointers inside it (a table of
  sub-commands, say) are code relocations — same split as everything else (section 4).

**The real shape of `purr_core_table_t`, as of the first real subsystem port:**

```c
typedef struct {
    void (*puts)(purr_cli_t *cli, const char *s);
    void (*printf)(purr_cli_t *cli, const char *fmt, ...);
    void (*apps_scan)(purr_app_registry_t *out_reg);
} purr_core_table_t;
```

- `puts`/`printf` are how a module produces any output at all: it can't call
  `purr_cli_puts`/`snprintf` itself (no libc, no linkage to the core's own copies). Calling a
  variadic function through a pointer works exactly like calling one directly — same ABI
  either way — so `printf` is just `purr_cli_printf` reached indirectly.
- `apps_scan` is deliberately a **domain-level** call, not raw `purr_fs`/`purr_appmgr` access:
  a module never gets a `purr_fs_t` or a signing key bag. Installing and removing apps is
  security-sensitive and stays entirely inside the core; a module only ever sees the
  read-only scan result. This is the pattern for anything else that moves into a module later
  — the core exposes what a module needs to *do*, not the primitives it would need to do it
  unsupervised.
- Grows as more subsystems move out of the monolith. `PURR_MODULE_ABI_VERSION` gets bumped
  whenever the table's shape changes (2, for this addition) so a module built against an
  older shape is refused rather than silently misreading fields — appending fields is
  offset-compatible in practice, but the version check makes that guarantee explicit rather
  than relying on it by luck.

## 7. Loading

Implemented in `commands.c`'s `load_one_module_file()`, proven on hardware (section 11):

1. Read the file from its LittleFS path, verify it (`purr_image_verify`, exactly the path
   already proven in the spike).
2. Parse the payload's prefix (`purr_module_parse_layout`), bounds-checked against
   `payload_size` before trusting any of it (section 5).
3. Allocate a load buffer 64KB-aligned (`esp_mmu_map`'s real constraint, found in the spike)
   and large enough for the code blob, rounded up to `CONFIG_MMU_PAGE_SIZE`. Copy just the
   code blob in (the payload minus the prefix); the rest starts zeroed (covers `.bss`).
4. **Data pass:** `purr_relocate(load_buffer, ..., data_offsets, data_count,
   (uint32_t)load_buffer)` — needs only the buffer's own address, available immediately.
5. `esp_cache_msync` the buffer (writeback), then map the same physical range executable via
   `esp_mmu_map` to get the executable alias's address.
6. **Code pass:** `purr_relocate(load_buffer, ..., code_offsets, code_count,
   (uint32_t)exec_alias)` — writes still go through the writable buffer; only the computed
   *value* uses the executable alias's address.
7. `esp_cache_msync` the buffer again (writeback the code-pass fixups), then the executable
   alias (invalidate, instruction type) — the sequence the spike proved works.
8. Call `((entry_t)(exec_alias + entry_offset))(&core_table)`, keep the returned module table.

## 7.1 Boot-time integrity sweep and quarantine

The user's own framing: a "secondary loader" that mounts the filesystem and watches `/system`
so nothing unsigned can end up running there. That thing already exists in spirit — CoreOS
(shared by KittenOS and PURR OS) is the only code that ever mounts LittleFS and holds the key
bag, and `load_one_module_file()` already refuses anything that doesn't verify, with no
bypass. What was missing was *eagerness* and *visibility*: verification only ran lazily, on
the first shell command after login, and a failure was one `ESP_LOGW` line nobody sees outside
a serial capture, then a silent skip. Decided (2026-09-28): strengthen CoreOS's own boot
sequence rather than add a second boot-stage binary/partition — a separate stage wouldn't add
a new trust boundary, since CoreOS is already the root of trust doing the verifying; it would
just be more flash and duplicated key-bag/verify code for the same guarantee.

**The sweep, called once during boot setup (right after LittleFS mounts, before login runs —
not lazily on first shell command anymore):**

1. List every file directly under `/system`.
2. Attempt to load each one exactly as `load_one_module_file()` already does (verify, parse
   layout, relocate, map executable, call `entry()`, check the returned ABI version).
3. **On success:** register its commands, same as today.
4. **On failure** (bad hash/signature/chip/role, or a bad/mismatched ABI table from `entry()`):
   print one clear line to the actual screen the user is looking at (the same console the boot
   banner uses, not just the serial log), then move the file into `/system/.rejected/<name>`
   (creating that directory if needed) so it is never retried or loaded again on a later boot.
   Quarantined, not deleted — it stays there as evidence of what was rejected and why, so
   "someone tried to inject something" is distinguishable after the fact from "a module build
   went stale." No command reads `.rejected` back yet; a later `system` or `modules` command
   listing what's quarantined is a natural follow-up, not required for this pass.
5. One bad file never blocks anything else: the sweep continues past it, other valid modules
   still load, and the shell still comes up. This is a backstop, not a gate on any single write
   path — it catches a bad file in `/system` regardless of how it got there (a future
   install command's mistake, a bug, or someone using the raw `write`/`mv`/`cp` shell commands
   at the physical keyboard), rather than trying to police every possible way onto the
   filesystem individually.

## 8. Building (purrstrap)

`purrstrap modules build` (`purrstrap/scripts/modules.py`), built and proven (section 11):
compiles the module source against the merged-segment linker script twice (the two base
addresses), diffs to find every relocatable word, classifies each one as data or code against
the base-0 build's `readelf -s` FUNC symbol addresses (section 4), looks up the entry symbol's
address, writes the prefix (section 5), packages as `PURR_IMG_MODULE`. Refuses to package a
module that has any non-relocatable word rather than silently miscompiling it.

**`--source` takes a comma-separated list**, not just one file (added 2026-09-28, needed
before CoreOS itself -- which will span many files -- can build this way,
`PurrOS/components/coreos/SPEC.md` section 10): each file compiles to its own object, then
all of them link together at each base address, same as before from there on. Proven on real
hardware first with four files -- new test code, a shared freestanding `memcpy`, and two
pieces of real, unmodified CoreOS code (`purr_relocate.c`/`purr_module.c`) -- and then, real
scale, 2026-09-30: **15 of the 16 real files in `PurrOS/components/coreos/src`** (everything
except `purr_appmgr.c`, which called the kernel's raw filesystem functions directly), built as
one module with the new `coreos` kind, signed with the boot-role key `PURR_MOD_COREOS` requires,
and loaded and run correctly through the same production loader every real module uses. That one
exclusion is also resolved now, same day: `purr_appmgr.c` converted onto a `purr_appmgr_fs_t`
function-pointer interface (`PurrOS/SPEC.md` section 6), and the retry proved it -- **all 16
files, same day**, built and signed the same way, loaded and run on real hardware with every one
of 16 address-taken representative functions resolving to a real, distinct, non-NULL symbol in
the combined relocated binary (`PurrOS/SPEC.md` section 6 has the full result). Needed `purrstrap/freestanding/
purr_freestanding_libc.c` (memcpy/memset/memcmp/memchr/strncmp/strtoul/strlen/strcmp/strcpy/
strncpy/snprintf/vsnprintf -- every libc call these 15 files actually make) `--source`'d
alongside them, and surfaced a real, previously-invisible bug: `purr_menu.h`'s `purr_key_t`
(a keyboard-key enum) collided with `purr_keybag.h`'s (a signing key record), invisible until
this was the first time anything included both headers together. Renamed to
`purr_menu_key_t`.

**Kinds:** `driver`, `appmanager`, `runtime`, `devbundle` (each `PURR_ROLE_*`-gated per
`purr_role_may_sign()`, `purr_keybag.c`), plus **`coreos`** (added 2026-09-30, `PURR_MOD_COREOS`,
`PURR_ROLE_BOOT`-only -- the same trust tier as `kernel`/`bootpkg`/`loader`). `kernel`, `loader`
and `bootpkg` have no build kind here on purpose: they're real ESP app images or the boot
package binary, not relocatable module payloads, and go through `coreos.py`'s packaging
instead.

**Links `-lgcc`** (added 2026-09-28, found building the first real kernel-table module,
`mem`/`uptime`): a plain 64-bit divide (`uptime_us() / 1000000`) compiles to a call to
`__udivdi3`, a compiler-support helper for 32-bit targets -- not libc, so still fine under
`-nostdlib`, but not linked by default either. Every module build links it now, not just
ones that happen to need it; unused helpers are dropped by `--gc-sections` regardless.

## 9. Testing

- Relocation apply/detect logic (`purr_relocate`, `purr_module`): pure C, host-tested with
  hand-built buffers and offset lists (26 + 38 checks), same pattern as
  `purr_verify`/`purr_appmgr`'s host tests.
- purrstrap's link-twice-diff-and-classify step: proven against two real compiled modules
  (`hello_module.c`, a string only; `about_module.c`, a written global *and* a stored,
  callable function pointer — the case that first exposed the two-base bug).
- The real pipeline end to end, on hardware: `about_module.c` loads automatically at boot and
  its command runs correctly from the actual shell dispatch table — not a spike diagnostic,
  the real `purr_commands()`/`load_one_module_file()` path.
- The first real subsystem port: `Modules/appmanager/apps_module.c` (the `apps` command,
  previously `cmd_apps` inline in `commands.c`) grows the core table (`apps_scan`, section 6)
  and confirms the domain-level-call pattern on hardware — `apps` correctly reports "no apps
  installed" through a loaded module, not the monolith.

## 10. Open questions

- Whether a module can depend on another module's table (a chain), or only ever the core's —
  not needed so far, but worth deciding before more than one real module exists.
- How a module gets onto the device for real (a `modinstall`-style URL fetch, or MTP once it
  exists) — every module so far has landed there via a temporary, hand-embedded `plantmodules`
  shell command, the same stopgap AppManager's `appinstall` has for now via URL fetch. Whenever
  this is built, it must verify before writing to `/system` (fail fast, at install time, with a
  clear message), same as `appinstall` already does for apps — the boot-time sweep (section
  7.1) is the backstop regardless, but a good install path should not rely on the backstop
  alone to catch its own mistakes.
- Whether currently-permissive `MOD_DRIVER` signing (any of `SYSTEM`/`VENDOR`/`DEVELOPER`/
  `OWNER`) is right for subsystems the user considers core and wants hardened, like `wifi` — it
  is loose on purpose for third-party/downloadable drivers, but today it also covers `wifi`,
  `apps`, `about`, and `netinstall` since a `SYSTEM` key doesn't exist yet, only `boot.key` and
  the dev-only `developer.key`. A real key rollout (generating `system.key`, deciding which
  modules should require it instead of the permissive `driver` gate, retiring the dev-only
  key/password from anything that isn't a dev build) is real work, not yet scheduled.

Resolved while building this: the relocation count cap (`PURR_RELOC_MAX`, 4096) already lives
in `purr_relocate.h` and is enforced before any offset is even read.

## 11. Real bugs found building the first real module — both resolved

Building the first genuine module (`about`, a real shell command loaded from LittleFS at
boot, `Modules/test/about_module.c`) hit two real bugs, both since fixed. The first real
module now loads automatically at boot and its command runs correctly, confirmed on hardware.

**Bug 1, fixed:** the loader relocated using the writable `databuf` pointer's address as the
base instead of the executable alias (`exec_ptr`)'s address. A string, read through the wrong
(but still readable) alias, worked by accident — a stored function pointer, relocated to that
same wrong alias, is an instruction-fetch fault the instant anything calls it.

**Bug 2, initially misdiagnosed as a boot-timing issue, actually the real one:** with bug 1's
fix in place, calling the module's entry point still panicked every time with `Cache disabled
but cached memory region accessed`, at the same instruction — a few bytes into the entry
function, right where it stores its argument into a `.bss` global. Two hypotheses (a WiFi/NVS
timing race; something specific to booting early) were tested and both ruled out: a 3-second
settling delay just shifted the crash by 3 seconds, and disabling WiFi entirely (confirmed
zero `wifi:` log activity that boot) didn't stop it either. A third test — triggering the
identical load manually, well after boot settled, via a `loadmodules` shell command — *also*
crashed, identically, ruling out boot-timing altogether.

The real cause, found by disassembling the module and cross-referencing real ELF symbol types
(`readelf -s`, not `nm`, whose section-letter classification wrongly tags read-only data
placed in `.text` by `-mtext-section-literals` as if it were code): **relocating everything
against one base is wrong.** A word holding a function's address needs the executable alias
(exec+read only); a word holding an object's address — a global that gets *written* to, in
particular — needs the plain writable copy, since the executable mapping has no write
permission. Writing a global's value through the executable alias is exactly what produced
this specific hardware trap. `hellomod`'s single relocation (a string, read-only) and the
spike's call-table test (a runtime argument, not a stored address) both happened to never
exercise this path — `about` was the first module with a *written* global.

**The fix:** two relocation lists, not one (revising section 4-5 below and `purr_module.h`'s
payload layout accordingly). purrstrap classifies each relocation by checking whether its
pre-relocation value exactly matches a `FUNC`-type symbol's address (from the base-0 build's
`readelf -s` output) — function addresses go in the code list (relocated against the
executable alias), everything else (globals, strings, const tables) goes in the data list
(relocated against the writable copy, which supports both reading and writing). For `about`:
6 data relocations, 1 code relocation (`s_cmds[0].fn`, the one stored, callable function
pointer) — found and split automatically, verified by hand against the disassembly before
trusting it.
