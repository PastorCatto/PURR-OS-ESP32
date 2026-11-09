# 12. Writing modules

A **module** is how you add a shell command (or a group of them) without touching the firmware image.
It is a small, freestanding, relocatable program, signed, copied to the filesystem and loaded at boot.
Source of the mechanism: `Modules/SPEC.md`, `PurrOS/main/commands.c` ("Modules (real)" section),
`PurrOS/components/coreos/src/purr_relocate.c`, `purr_module.c`, `purrstrap/scripts/modules.py`.

**Status: [WORKS].** Eight real modules and kernelmods load from the filesystem at boot on the T-Deck Plus,
and the build, sign, verify and index steps below were run end to end while writing this chapter.
**What a module cannot be:** a hardware driver. A module never touches hardware, flash, the key bag or
the filesystem object. It can only call what the call table offers. For hardware see
[13](13-writing-drivers.md).

## Two kinds, two boundaries

| | **Module** | **Kernelmod** |
|--|-----------|---------------|
| Folder | `/system/*.cat` | `/kernelmods/*.cat` |
| Table it receives | `purr_core_table_t` (`purr_module_abi.h`, version 4) | `purr_kernel_table_t` (`purr_kernel_table.h`, version 6) |
| Meaning | the CoreOS-to-module boundary | the prototype kernel-to-CoreOS boundary |
| Examples | `about`, `apps`, `wifi`, `netinstall` | `sysinfo`, `fs`, `login`, `appmgr` |
| Bad file | quarantined to `/system/.rejected/` | skipped and logged to serial, **not** quarantined |
| Use it for | commands built on domain-level services (Wi-Fi, apps, install) | commands that need kernel facilities (heap, uptime, files, accounts, reboot) |

If you just want a new command that lists files or prints system numbers, write a **kernelmod**: its table has
the filesystem and heap functions. A **module** table has Wi-Fi, app listing and `net_install`.
Both are loaded into PSRAM, so both need a board with PSRAM.

## The rules of freestanding code

A module is compiled with `-ffreestanding -fno-builtin -nostdlib` and linked with `-lgcc`:

- **No libc.** No `printf`, `strlen`, `memcpy`, `malloc`. The compiler is told not to invent them. Use the table's
  `puts` and `printf`, and write tiny helpers yourself (every existing module has a local `streq()`).
  64-bit division and shifts work, thanks to `-lgcc`.
- **No ESP-IDF, no FreeRTOS.** You get a pointer to a table of function pointers. That is your whole world.
- **Warnings are errors** (`-Wall -Wextra -Werror`). An unused static function fails the build.
- **Writable globals and tables of function pointers are fine.** Static initialised data, string literals, `const`
  tables and function pointers inside them are all relocated. Zero-initialised data (`.bss`) is cleared at load.
- **One 64 KB page.** The loader maps a module into a single page of PSRAM, so the code, data and relocation-adjusted
  image must fit in 64 KB. The file read limit is 128 KB. (The loader itself supports more pages; the module
  and kernelmod callers pass 1.)
- **Entry point.** One exported function, marked `__attribute__((used))`, that receives the table, saves the pointer
  in a static, and returns your module's own table.
- **Hardcoded addresses are not caught.** The build detects non-relocatable words by linking twice and comparing,
  but a constant like `0x3FC88000` is identical in both links, so it passes silently. Do not use them.

## Anatomy

```c
/* greet_module.c - a minimal /system module: adds a `greet` command. */
#include "purr_module_abi.h"

static const purr_core_table_t *s_core;

static int cmd_greet(purr_cli_t *cli, int argc, char **argv)
{
    s_core->printf(cli, "hello, %s!\n", argc > 1 ? argv[1] : "world");
    return 0;
}

static const purr_cmd_t s_cmds[] = {
    {"greet", "say hello", cmd_greet},
};

static const purr_module_table_t s_table = {
    .abi_version = PURR_MODULE_ABI_VERSION,
    .cmds = s_cmds,
    .cmd_count = 1,
};

__attribute__((used))
const purr_module_table_t *greet_module_entry(const purr_core_table_t *core)
{
    s_core = core;
    return &s_table;
}
```

- A command is `int fn(purr_cli_t *cli, int argc, char **argv)`. `argv[0]` is the command name. Return 0 for success.
  The shell does nothing with the return value yet.
- All output goes through the table: `s_core->puts(cli, "text")` and `s_core->printf(cli, "fmt", ...)`. `printf` uses a
  **160-byte** buffer; a longer line is cut.
- `help` text is the second field. Keep it short: `help` prints `  %-8s %s`.
- The ABI check: if `abi_version` is not exactly the core's, the module is refused.

A kernelmod is the same shape with the other header and table:

```c
#include "purr_kernel_table.h"

static const purr_kernel_table_t *s_k;

static void tally(void *ctx, const char *name, int is_dir, uint32_t size)
{
    (void)name; (void)is_dir; (void)size;
    (*(int *)ctx)++;
}

static int cmd_count(purr_cli_t *cli, int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/";
    int n = 0;
    int e = s_k->fs_list(path, tally, &n);
    if (e < 0) {
        s_k->printf(cli, "count: %s: %s\n", path, s_k->fs_strerror(e));
        return 1;
    }
    s_k->printf(cli, "%d entr%s in %s\n", n, n == 1 ? "y" : "ies", path);
    return 0;
}

static const purr_cmd_t s_cmds[] = {
    {"count", "count entries in a directory", cmd_count},
};

static const purr_kernel_module_table_t s_table = {
    .abi_version = PURR_KERNEL_TABLE_ABI_VERSION,
    .cmds = s_cmds,
    .cmd_count = 1,
};

__attribute__((used))
const purr_kernel_module_table_t *count_kmod_entry(const purr_kernel_table_t *kernel)
{
    s_k = kernel;
    return &s_table;
}
```

## What the tables offer

The full field lists are in [21](21-reference-formats-abis-limits.md#call-tables). In short:

**Core table** (modules): `puts`, `printf`, `apps_scan` (the installed apps registry, read-only), `net_scan`,
`net_connect`, `net_forget`, `net_saved`, `net_status` (Wi-Fi, already turned into plain structs and words),
`net_install` (the whole installer as one call).

**Kernel table** (kernelmods): `puts`, `printf`, `heap_alloc`, `heap_free`, heap and PSRAM statistics, `uptime_us`,
the filesystem (`fs_list`, `fs_read`, `fs_write`, `fs_mkdir`, `fs_remove`, `fs_rename`, `fs_usage`, `fs_mounted`,
`fs_strerror`, `fs_format`), `console_flush`, `console_clear`, `read_key`, the account calls (`login_*`), the app calls
(`app_*`), `print_version/info/parts/purrcfg`, `reboot_system`.

**Why so many calls are single opaque functions** (`login_su`, `fs_format`, `net_install`, `app_install`): anything that
touches a password, the shadow file, the signing key bag or a raw partition is one call that does all its own I/O and
re-checks permission **on the other side of the table**. A module never gets a primitive it could misuse. That is the
design rule; copy it when you extend a table.

## Build, sign, install

### 1. Build

```powershell
python purrstrap/purrstrap.py modules build --source path\greet_module.c --entry greet_module_entry `
  --kind driver --name greet --version 0.1.0 --out greet.cat
```

Real output, trimmed:

```
...
[ok] greet.cat: 345 bytes (greet 0.1.0, unsigned, 7 data + 1 code relocation(s), entry at code offset 0x30)
sign it: purrstrap keys sign --key <role>.key --image greet.cat --key-id <id>
```

What it did ([21](21-reference-formats-abis-limits.md#module-payload)): compiled your source twice, linked it at base
`0x0` and again at `0x100000`, compared the two images word by word, recorded every word that differs by exactly the
base difference as a relocation (splitting them into **code** relocations, whose target is a function, and **data**
relocations, everything else), and wrapped the result in a PURR container. Intermediate files are in
`Modules/build/<name>/` (git-ignored; delete freely).

Options: `--source` may be several comma-separated files (CoreOS itself is built this way). `--kind` decides who may sign it:

| `--kind` | Subtype | Signer role |
|----------|---------|-------------|
| `driver` (the default, and the right choice for a shell command) | 5 | system, vendor, developer or owner |
| `appmanager`, `runtime`, `devbundle` | 3, 4, 7 | system |
| `coreos` | 2 | boot |

`--name` becomes the **file name on the device** (`/system/<name>.cat`) when installed by `netinstall modules`, so choose it
once. `--version` is shown by tools; `--board` defaults to `tdeck_plus`.

### 2. Sign

```powershell
python purrstrap/purrstrap.py keys sign --key signing_keys/developer.key --image greet.cat --key-id 2
python purrstrap/purrstrap.py keys verify --key signing_keys/developer.pub --image greet.cat
```

The `--key-id` **must** be the id your device's key bag has for that key, or the device says `no-key`. `0` means
"leave as is", which for a fresh image is 0 and will not verify ([F-19](FINDINGS.md#f-19)). In this repo: boot = 1,
developer = 2. See [17](17-keys-and-signing.md).

### 3. Get it onto the device

Choose one. None is a USB copy; that does not exist yet ([09](09-filesystem.md)).

**A. Through your own module index (no firmware rebuild).** Host the file and an index on any web server, point the
firmware at the index, run `netinstall modules`.

```powershell
python purrstrap/purrstrap.py modules index --board tdeck_plus --modules-dir mods --coreos-version 0.2.0 --key developer --out mods\modules.manifest
cd mods ; python -m http.server 8000
```

Set `CONFIG_PURR_MODULE_INDEX_URL="http://<pc-ip>:8000/modules.manifest"` (both in `Kconfig.projbuild` and
`sdkconfig.defaults`, then `--clean` rebuild, see [04](04-building-and-flashing.md)). Plain `http://` works for
development. Then on the device:

```
wifi connect ...
netinstall modules
reboot
```

The index lists one stanza per `.cat` in the folders you give (`--modules-dir` for `/system`, `--kernelmods-dir` for
`/kernelmods`), and reads each file's own header for its name and version. A real index entry from the test:

```
component=doc_greet
type=module
version=0.1.0
board=tdeck_plus
file=greet.cat
size=345
sha256=6867...276f
key=developer
min_coreos=0.2.0
```

Note `netinstall` writes `/system/<component>.cat`, where `component` is the **header name**, not the file name.

**B. Embed it in the firmware (the author's current dev loop).** Convert the signed file to a C array, add it to the
tables in `PurrOS/main/commands.c`, rebuild, flash the app and run `plantmodules`:

```python
# cat2c.py <file.cat> <c_identifier>
import sys
data = open(sys.argv[1], "rb").read()
print(f"static const uint8_t {sys.argv[2]}[] = {{")
for i in range(0, len(data), 12):
    print("    " + ", ".join(f"0x{b:02x}" for b in data[i:i+12]) + ",")
print("};")
```

Paste the array next to the other `s_*_module` arrays and add `{"greet.cat", s_greet_module, sizeof(s_greet_module)}` to
`s_temp_modules[]` (modules) or `s_temp_kernelmods[]` (kernelmods). `plantmodules` rewrites **all** of them, then `reboot`.
The embedded copies must be re-signed and regenerated after any key rotation or ABI bump. This whole mechanism is
temporary scaffolding the author intends to retire.

## What happens at boot

Early in boot (after the filesystem mounts, before login) `purr_modules_setup()` sweeps `/system` then `/kernelmods`.
For each `*.cat` file (case-sensitive, files only):

1. read it (128 KB cap), verify the container, chip, role and signature,
2. parse the payload prefix and bounds-check every offset,
3. allocate a 64 KB-aligned PSRAM page, copy the code in, apply the **data** relocations against the writable address,
   map the same memory executable, apply the **code** relocations against the executable address,
4. flush caches, call the entry, check `abi_version`,
5. add its commands to the shell table.

Outcomes:

| Result | Module (`/system`) | Kernelmod (`/kernelmods`) |
|--------|-------------------|---------------------------|
| loaded | commands appear | commands appear |
| failed verification, bad layout, too big, bad relocation | moved to `/system/.rejected/<name>`, screen says `system: /system/x.cat rejected (failed verification) -- quarantined` | skipped; serial log only |
| ABI mismatch or `NULL` table | quarantined, `incompatible (abi mismatch)` | skipped; serial log only |
| out of memory, mapping failure | left in place, tried again next boot | skipped |

One bad file never blocks the others. With `CONFIG_PURR_VERBOSE_BOOT` you see a line per file on screen; always on the
serial log with tags `modules` and `kernelmods`.

**Command name clashes:** the shell's table is built as built-ins, then modules, then kernelmods, and the **first match
wins**. A new command named like an existing one is shadowed. `help` shows every entry, so a duplicate listing is the clue.

## Limits

8 loaded modules, 8 loaded kernelmods, and each directory listing sees only its **first 8 entries including
subfolders** ([F-24](FINDINGS.md#f-24)). Once `/system/.rejected/` exists that leaves 7 module files. Modules are never
unloaded.

## Troubleshooting a module

| Symptom | Cause |
|---------|-------|
| `compile failed (a)` | any warning, because of `-Werror` |
| `entry symbol 'x' not found. Known symbols: ...` | `--entry` does not match, or the function lacks `__attribute__((used))` and was garbage-collected |
| `not relocatable (base-A word ..., base-B word ...)` | a word differs between the two links by something other than the base difference, e.g. address arithmetic the linker cannot express |
| `the two builds came out different sizes` | something non-deterministic between links |
| `no-key` on device | wrong or missing `--key-id` |
| `role-mismatch` | `--kind` needs a role your key does not have (e.g. `coreos` needs boot) |
| `bad-chip` | built for the other board |
| quarantined as `incompatible (abi mismatch)` | the table version changed; rebuild and re-sign |
| `too big (N bytes, room for 1 page(s))` | over 64 KB |
| it loads but the command is missing | name clash, or `cmd_count` does not match the array |
| crash with `Cache disabled but cached memory region accessed` | a written global was relocated against the executable alias. This was fixed by the two-list split; if you see it, you hit a build path that bypassed `purrstrap`. |

## Changing the tables (for core developers)

Adding a call to `purr_core_table_t` or `purr_kernel_table_t`:

1. Append the field in `purr_module_abi.h` or `purr_kernel_table.h`. Appending is offset-compatible, but...
2. **bump the version constant.** Every existing module is then refused until rebuilt.
3. Implement it in `commands.c` (`s_core_table` or `s_kernel_table`) and in `Kernel/main/kernel_table.c` if the standalone
   kernel should provide it (unset fields stay `NULL`, and calling one crashes).
4. Rebuild, **re-sign and re-publish every module and kernelmod**, regenerate the embedded arrays, update the module index.

Forgetting step 4 is the classic failure: the app image is reflashed, the old `/system` files stay on the device
(flashing never touches `root`), and they are correctly quarantined as an ABI mismatch.

Next: [13 Writing drivers](13-writing-drivers.md).
