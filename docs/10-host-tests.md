# 10. Host tests

## Short version

- Most of the logic of PURR OS is plain C that compiles on a laptop. The host tests build it with the system C compiler and run it in seconds, with no device.
- Run all C tests: `cd PurrOS/components/coreos/test/host && IDF_PATH=$HOME/esp/esp-idf-v5.3.5 python3 run.py`. Run some: add a name part, for example `run.py trust`.
- 34 test programs (`test_*.c`) are in that folder. The last full inventory (2026-11-06) says all pass.
- Python tests: `cd purrstrap && python3 -m unittest discover -s tests` (plus tests for the SDK and the documentation tools). A fuzzer attacks six parsers under AddressSanitizer: `python3 PurrOS/components/coreos/test/host/fuzz/fuzz.py`.
- Host tests prove the logic. They do not prove anything about the radio, the display, flash timing or memory pressure on the real device.

## What it is for

The host tests are the fast safety net. A change to verification, the manifest parser, the runtime manager, the key bag or the permission table is checked in the time it takes to compile a few C files, long before a ten-minute firmware build or a flash. They also pin down behaviour that was hard to find on hardware, so it cannot silently come back.

## The files

| File | What it is |
|---|---|
| [run.py](../PurrOS/components/coreos/test/host/run.py) | Builds and runs the C tests |
| [testkit.h](../PurrOS/components/coreos/test/host/testkit.h) | `CHECK`, `CHECK_EQ`, `TK_DONE`: the whole assertion kit |
| [crypto_uecc.c](../PurrOS/components/coreos/test/host/crypto_uecc.c), [testcrypto.h](../PurrOS/components/coreos/test/host/testcrypto.h) | The P-256 backend for tests (micro-ecc from ESP-IDF) |
| [fakebus.h](../PurrOS/components/coreos/test/host/fakebus.h) | A fake driver bus: a scripted UART, SPI callback, GPIO levels, a clock |
| [esp_flash.h](../PurrOS/components/coreos/test/host/esp_flash.h), [esp_log.h](../PurrOS/components/coreos/test/host/esp_log.h) | Stubs so device files compile on a PC |
| [repo_fixture.h](../PurrOS/components/coreos/test/host/repo_fixture.h), [gen_repo_fixture.py](../PurrOS/components/coreos/test/host/gen_repo_fixture.py) | A repository made by the SDK's own tool, so the C code is checked against the real format |
| [fuzz/fuzz.py](../PurrOS/components/coreos/test/host/fuzz/fuzz.py), [fuzz_parsers.c](../PurrOS/components/coreos/test/host/fuzz/fuzz_parsers.c) | The parser fuzzer |
| [purrstrap/tests/](../purrstrap/tests/) | Tests of the build and signing tool |
| [sdk/tests/](../sdk/tests/) | Tests of the developer kit (113 test functions by count) |
| [tools/docs/test_docs_tools.py](../tools/docs/test_docs_tools.py) | Tests of the documentation checkers |

## The C tests, by area

| Area | Programs |
|---|---|
| Trust and boot | `test_verify`, `test_trailer`, `test_trust`, `test_keybag`, `test_cfg`, `test_reason`, `test_menu`, `test_bootimg`, `test_bootupdate`, `test_swap`, `test_decision` |
| Updates and repositories | `test_manifest`, `test_repo`, `test_repo_trust` |
| Modules and drivers | `test_module`, `test_relocate`, `test_driver`, `test_drv_gnss`, `test_hwprofile`, `test_hwscan` |
| Runtime and apps | `test_runtime`, `test_appmgr`, `test_perm`, `test_power`, `test_battmodel`, `test_statusicons` |
| System | `test_fs`, `test_users`, `test_pbkdf2`, `test_cli`, `test_util`, `test_wifi` |
| Messaging | `test_msgstore`, `test_contacts` |

Check counts that the plans and specs quote, which are the best size guides: `test_trailer` 42, `test_trust` 85 (including forged, wrong-key, self-vouching and dev-lock cases), `test_reason` 42, `test_manifest` 162, `test_runtime` 273, `test_relocate` 26, `test_module` 38.

## How `run.py` works

1. It finds a C compiler (`PURR_GCC`, then `gcc` on the path; on a Mac that is Apple's clang).
2. It finds the micro-ecc source inside ESP-IDF (`PURR_UECC_DIR`, or `IDF_PATH`, or the IDF install manifest). It exits with code 2 and an error line if either is missing.
3. It compiles micro-ecc and LittleFS once, without our warning flags.
4. For each `test_*.c` it compiles that file with the coreos sources, `purr_fs.c` and `purr_driver.c` from the kernel component, the crypto backend, micro-ecc and LittleFS. The flags are `-std=c11 -O1 -g -Wall -Wextra -Werror`, so **a warning is a failure**.
5. It runs each program, prints its output, and counts a non-zero exit as a failed file. The summary is `ALL PASSED` or `N test file(s) FAILED`.

New `src/*.c` files under coreos are picked up by glob. The device build needs the file listed in its own CMake file as well.

A test is a `main()` that includes `testkit.h`, makes calls, uses `CHECK(cond)` or `CHECK_EQ(a, b)`, and ends with `TK_DONE("name")`, which prints `name: N checks, M failed` and returns 1 on any failure. Files under `PurrOS/main` can be tested too by including them directly, as `test_bootupdate.c` does with `purr_bootupdate.c` over a fake flash.

## The fuzzer

`fuzz.py` compiles `fuzz_parsers.c` with AddressSanitizer and UndefinedBehaviorSanitizer and attacks six parsers with mutated inputs from a fixed-seed generator, so a run is exactly repeatable:

| Target | Parser |
|---|---|
| `verify` | `purr_image_verify` |
| `trailer` | `purr_trailer_verify` |
| `module` | Module layout and relocations |
| `manifest` | `purr_manifest_parse` |
| `cfg` | `purrcfg` load and key-bag overrides |
| `wifi` | The saved Wi-Fi list |

Usage: `python3 PurrOS/components/coreos/test/host/fuzz/fuzz.py [target|all] [iterations] [seed]`, default 100,000 iterations and seed 1. A crashing input is saved as `crash-<target>.bin` next to the script. The first campaign ran 5.4 million executions over three seeds and found nothing; a deliberately planted bounds bug was caught at once on every seed ([plan3.md](../plan3.md) item 1). It does not cover the app manifest, the user store, the CLI, or the ESP-IDF glue in the module loader.

## The other suites

| Suite | Command | Notes |
|---|---|---|
| purrstrap | `cd purrstrap && python3 -m unittest discover -s tests` | 128 test functions by count; [plan5.md](../plan5.md) reports 128 passing and 4 skipped. Needs the ESP-IDF Python for `keys` ([chapter 09](09-purrstrap.md)) |
| SDK | `python3 -m unittest discover -s sdk/tests -t sdk/tests` | 111 pass per [plan5.md](../plan5.md). Includes real drivers run against a Python bus (`sdk/tests/driverhost.py`) |
| Docs tools | `python3 -m unittest discover -s tools/docs` | The link checker and the word counter |
| Link check | `python3 tools/docs/check_links.py [paths]` | Fails on a broken relative link or anchor |

`gen_repo_fixture.py` needs a Python with `cryptography`. Run it again only when the repository format changes; the keys it uses are thrown away.

## Writing a new C test

1. Pick the plain-C source to test. If it includes an ESP-IDF header, either move that part behind a function pointer (the way `purr_flash_t` and `purr_verify_env_t` do) or add a stub header next to `esp_log.h`.
2. Create `test_<name>.c` in the host folder. Include the header under test and `testkit.h`. The name must start with `test_` and end in `.c`, because `run.py` globs for it.
3. Write `int main(void)`. Build the inputs by hand: a fake flash that is a byte array, a fake clock, a manifest in a string. Call the function. Use `CHECK` and `CHECK_EQ`.
4. Cover the failures on purpose: a truncated input, a wrong key, a size that is one too big, a value of zero. The parsers read untrusted bytes, so the bad cases are the point.
5. End with `TK_DONE("test_<name>")`.
6. Run it alone: `IDF_PATH=$HOME/esp/esp-idf-v5.3.5 python3 run.py <name>`. Then run everything once before you stop.
7. If the code under test is a new `src/*.c` file in coreos, also list it in the device CMake file so the firmware builds it.

## Reading a failure

A failing check prints `FAIL <file>:<line>: <expression>` (and, for `CHECK_EQ`, both values). The line number is in the test, not in the code under test. Re-run just that program, add a `printf` of the values you doubt, and fix the code under test rather than the expectation, unless the expectation was wrong. Where a plan or spec and the code disagree, the plan says to fix one of them in the same step (`CLAUDE.md`).

For the fuzzer, a crash prints the sanitizer report and saves the input. Replay the same target with the same seed to get the same crash again, fix the bounds check, and keep the saved input as a seed for the unit test of that parser.

## A habit that pays

Run the host tests before every commit that touches `PurrOS/components/coreos` or `PurrOS/main/purr_bootupdate.c`, and run the fuzzer after any change to a parser. Some tests exist to keep a device bug from coming back: `test_swap` checks that a resumed bootloader swap stops after three attempts ([plan3.md](../plan3.md), 2026-11-05), and `test_manifest` checks that withdrawn entries are never offered. The opposite also happens: a bug that exists only with real flash and real timing passes every host test (the jump-table crash and the PSRAM-stack abort were both found on the VM or the device). A green run is a reason to go on to the next check, not a reason to stop.

## What the host tests do not cover

| Not covered | Where it is checked instead |
|---|---|
| Flash timing, cache behaviour, task stacks | The device |
| Wi-Fi, TLS, real downloads | The device, or the VM with the developer server |
| The display, touch and keyboard drivers' hardware side | The device |
| LVGL drawing | `tools/lvgl_host/render.py` draws real LVGL on the host for the look; the behaviour is checked in the VM |
| The shells, the catcall implementations in `main/` | The VM and the device |
| Memory pressure, internal RAM | The device (the Task Manager) |
| The bootloader's ESP-IDF glue | The device; the shared logic (`purr_slot_usable`, trust, trailer) is tested |

## What goes wrong and how to fix it

| Symptom | Cause | Fix |
|---|---|---|
| `error: no gcc found` | No compiler | Install one, or set `PURR_GCC` |
| `error: micro-ecc source not found` | `IDF_PATH` not set | Set `IDF_PATH=$HOME/esp/esp-idf-v5.3.5` for that command |
| `BUILD FAILED` on a warning | `-Werror` | Fix the warning; do not turn the flag off |
| A test builds on one machine and not on another | `-Werror` plus a newer or different compiler adds warnings | Fix the code the warning points at |
| `fuzz.py`: `error: no C compiler found` | No `cc`, `gcc` or `clang` on the path | Install one or set `PURR_GCC`; it must support the sanitizers (clang or a recent gcc) |
| `gen_repo_fixture.py` fails to import | No `cryptography` | Use the ESP-IDF Python |
| A test of an `esp_*` call does not compile | It needs a stub | Add one next to `esp_log.h` and `esp_flash.h` |

## What is not finished

- **Fuzzing gaps:** the app manifest, user store, CLI and the module loader's ESP-IDF glue ([plan3.md](../plan3.md)).
- **No coverage tool** is wired in, so the test lists above show intent, not measured coverage.
- **No CI.** The tests run when someone runs them; there is no hook that gates a commit.
- **The kernel catcall tables and the shells have no host test.** The plan to add a host test for the app `storage` path rules (`rel_ok`) and the `ui` handle limits is open ([plan9.md](../plan9.md)).
- **Tests of a recovery profile that refuses a bad signature and a tampered image** are a stated requirement ([plan3.md](../plan3.md), "Must verify", 2026-10-28). Two VM runs passed; a correct hash with a bad signature, and a tampered KittenOS image, are not covered.
- **The 2026-11-06 inventory counted 34 C programs passing.** I did not run them while writing this chapter.

Plan references: [plan3.md](../plan3.md) (fuzzing, trust tests), [plan6.md](../plan6.md) (the runtime's host tests), [plan5.md](../plan5.md) (the test inventory).
