# 20. Testing

There are two automated suites, both runnable on a PC with no hardware, and a set of hardware checks that exist only as
procedures recorded in the specs. Run the two suites before and after any change.

## 1. The CoreOS host C tests

Plain C compiled with your PC's gcc, against fake flash, a RAM-disk block device and `micro-ecc` as the P-256 backend.
They cover the logic that has no ESP-IDF dependency.

```powershell
python PurrOS/components/coreos/test/host/run.py            # everything
python PurrOS/components/coreos/test/host/run.py verify     # only tests whose file name contains "verify"
```

Needs a host `gcc` (found on `PATH`, `PURR_GCC`, or a WinLibs install under `%LOCALAPPDATA%`) and the `micro-ecc` source that ships inside
ESP-IDF (found through `PURR_UECC_DIR`, `IDF_PATH` or the installer manifest). It compiles with `-std=c11 -Wall -Wextra -Werror`,
so a warning is a failure. Output goes to `test/host/build/` (git-ignored).

Result recorded for this documentation (16 suites, 1,877 checks, all passing):

| Suite | Checks | What it covers |
|-------|--------|----------------|
| `test_appmgr` | 95 | app registry scan, add, update rules, remove, `.tmp` recovery, through the `purr_appmgr_fs_t` interface |
| `test_cfg` | 456 | `purrcfg` A/B sectors, CRC, power-cut at each step, flags |
| `test_cli` | 86 | tokenizer, quoting, line editing, command dispatch |
| `test_decision` | 460 | the boot decision table (every row and mode). **Not called by the running system** ([F-05](FINDINGS.md#f-05)). |
| `test_fs` | 85 | the filesystem wrapper on a RAM disk (create, read, rename, list, space) |
| `test_keybag` | 55 | key bag build, overrides, revocation, `purr_role_may_sign` |
| `test_manifest` | 88 | manifest parsing, dropped stanzas, `type=`, find by chip and board |
| `test_menu` | 43 | boot menu logic (countdown, keys, empty case) |
| `test_module` | 38 | module payload layout parsing and bounds checks |
| `test_pbkdf2` | 197 | PBKDF2-HMAC-SHA256 against known vectors |
| `test_relocate` | 26 | the relocation applier |
| `test_swap` | 22 | the staged-update state machine |
| `test_users` | 97 | accounts, shadow records, constant-time check, delay schedule |
| `test_util` | 28 | CRC-32, SHA-256, version parsing |
| `test_verify` | 52 | full image verification, signed by the same P-256 code the tool uses |
| `test_wifi` | 49 | saved-network list and strongest-network pick |

The runner prints each suite's own count and a final `ALL PASSED`. If your numbers differ from this table, trust the runner:
suites grow as the code does.

### Adding a test

1. Create `test_<name>.c` in `PurrOS/components/coreos/test/host/`. The runner picks up every `test_*.c`.
2. Use the kit (`testkit.h`): `CHECK(cond)`, `CHECK_EQ(a, b)`, and end `main` with `TK_DONE("test_<name>")`.
3. The runner links **every** `.c` in `coreos/src` plus `kernel/src/purr_fs.c` and LittleFS, so a new source file you add to
   `coreos/src` is compiled in automatically. It must be plain C with no ESP-IDF headers.
4. Keep code that touches hardware out of `coreos/src`. That is the rule that makes it testable.

```c
#include "purr_util.h"
#include "testkit.h"

int main(void)
{
    CHECK_EQ(purr_crc32(0, "123456789", 9), 0xCBF43926u);   /* the standard check value */
    TK_DONE("test_mything");
}
```

## 2. The purrstrap tests

```powershell
python -m unittest discover -s purrstrap/tests
```

72 tests, standard library only, about half a second. They cover the subscript registry (good, broken and missing-requirement
scripts), parameter coercion and the generated CLI, the TUI driven by scripted key sequences, the `coreos` subscript against a
fake project tree and a fake runner (ESP-IDF discovery order, missing-file report, exact commands), the boot package packer, and
sign and verify round trips. A harmless `ResourceWarning: unclosed file` appears from `test_keys.py`.

## 3. What is not covered automatically

| Area | How it is checked instead |
|------|---------------------------|
| bootloader (`purr_boot.c`, ladder, flags) | flash and watch the serial log |
| boot package drawing and keys | flash and look at the screen |
| kernel, display driver, keyboard | flash |
| `commands.c` (kernel table, module loader, `netinstall`, swap I/O, accounts back end) | flash, then use the shell |
| recovery loader | break the system on purpose, flash the loader, watch it |
| Wi-Fi and HTTPS | a real network |
| modules on a real PSRAM mapping | boot and run the command |

The specs record the hardware results ("confirmed on hardware"). This documentation pass did not reproduce them.

## 4. A manual checklist after a significant change

Run in order on a device with the real bootloader ([04](04-building-and-flashing.md)):

1. Serial shows `purr_boot: PURR OS bootloader` and `bootpkg ... loaded (key id 1, ok)`.
2. The boot menu appears and its countdown boots the right slot; any key stops it.
3. Login works, wrong passwords wait (`waiting 1 second...`), `help` lists every module's commands.
4. `mem`, `parts`, `purrcfg` look sane; `purrcfg` shows `fails 0` after a healthy boot.
5. `wifi scan`, `wifi connect`, `net`.
6. `netinstall modules` (or `plantmodules`), `reboot`, commands are back and nothing was quarantined.
7. `reboot recovery` lands in KittenOS; `reboot loader` lands in the recovery loader.
8. Corrupt one module (flip a byte with a modified copy) and confirm it is quarantined on screen.
9. Zero the first word of `kernel` (`esptool write_flash 0x1a0000` of four zero bytes) and confirm the bootloader falls back to KittenOS.
10. Re-run both automated suites.

## Continuous integration

None. There is no CI configuration in the repo. The `Keys/SPEC.md` rule is that signing happens on the owner's machine, never in CI.

Next: [21 Reference](21-reference-formats-abis-limits.md).
