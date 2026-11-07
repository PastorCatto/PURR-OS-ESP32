# 09. purrstrap, the build and signing tool

## Short version

- `purrstrap` is a Python tool that builds, packages, signs and serves PURR OS parts. Run it as `python3 purrstrap/purrstrap.py <script> <action> [--option value]`.
- It is a small core plus **subscripts**, one file each in [purrstrap/scripts/](../purrstrap/scripts/). Each subscript declares its actions and options once; the command line and the text menu are both generated from that.
- Standard library only, except `keys`, which needs the `cryptography` package, and `apps`, which needs Python 3.11 (`tomllib`). A subscript that cannot load is shown as broken or unavailable; the others still work.
- Passwords for signing come from `PURRSTRAP_KEY_PASSWORD` or a prompt. Never as an argument, never saved.
- Run `python3 purrstrap/purrstrap.py list` to see every action and option. Tests: `cd purrstrap && python3 -m unittest discover -s tests`.

## What it is for

purrstrap is the one place where a build output becomes something a device accepts: wrapped in the PURR container, signed with the right role's key, cut into a slot image, or placed in a manifest. It replaces an older single 4000-line tool whose menu and command line had drifted apart ([purrstrap/SPEC.md](../purrstrap/SPEC.md) section 1).

## The files

| File | What it is |
|---|---|
| [purrstrap/purrstrap.py](../purrstrap/purrstrap.py) | Entry point |
| [purrstrap/lib/app.py](../purrstrap/lib/app.py) | Chooses the command line or the text menu |
| [purrstrap/lib/cli.py](../purrstrap/lib/cli.py) | Builds `argparse` from the declarations; prints `list` |
| [purrstrap/lib/registry.py](../purrstrap/lib/registry.py) | Finds and validates subscripts; a bad one never stops the rest |
| [purrstrap/lib/model.py](../purrstrap/lib/model.py) | `Script`, `Action`, `Param` |
| [purrstrap/lib/context.py](../purrstrap/lib/context.py) | Output helpers, a process runner, remembered values |
| [purrstrap/lib/ui.py](../purrstrap/lib/ui.py) | The text menus and forms |
| [purrstrap/lib/image.py](../purrstrap/lib/image.py) | The PURR container layout, packed and parsed in one place |
| [purrstrap/lib/purrcfg.py](../purrstrap/lib/purrcfg.py) | The `purrcfg` layout and trust-block bytes |
| [purrstrap/scripts/coreos.py](../purrstrap/scripts/coreos.py) | Build, package and slot-image the OS |
| [purrstrap/scripts/keys.py](../purrstrap/scripts/keys.py) | Keys, signing, verification, trust blocks |
| [purrstrap/scripts/bootpkg.py](../purrstrap/scripts/bootpkg.py) | Build the boot package |
| [purrstrap/scripts/modules.py](../purrstrap/scripts/modules.py) | Build modules, drivers and apps; build the module index |
| [purrstrap/scripts/apps.py](../purrstrap/scripts/apps.py) | Scaffold and build an app project from `manifest.toml` |
| [purrstrap/scripts/dev.py](../purrstrap/scripts/dev.py) | The developer repository and server |
| [purrstrap/scripts/uiassets.py](../purrstrap/scripts/uiassets.py) | Pack the picture pack |
| [purrstrap/scripts/games.py](../purrstrap/scripts/games.py) | Package a game; generate a Retro-Go target |
| [purrstrap/scripts/netrec.py](../purrstrap/scripts/netrec.py) | Make a saved-Wi-Fi image for a board with no keyboard |
| [purrstrap/freestanding/purr_freestanding_libc.c](../purrstrap/freestanding/purr_freestanding_libc.c) | The small libc subset linked into freestanding builds |
| [purrstrap/tests/](../purrstrap/tests/) | The unit tests (128 test functions by count) |
| [purrstrap/SPEC.md](../purrstrap/SPEC.md) | The tool's contract (draft 0.1) |

## The subscripts

| Script | Action | What it does |
|---|---|---|
| `coreos` | `check` | Reports ESP-IDF and project status; builds nothing |
| | `build` | Builds a board and profile (`--board`, `--profile minimal\|recovery\|full`, `--flavor standard\|vm`, `--clean`) |
| | `clean`, `size` | Deletes a build folder; runs the size report |
| | `package` | Wraps a built recovery or full binary into an unsigned PURR image (`--version`) |
| | `package-component` | Wraps a built `kernel`, `loader` or `bootloader` binary into an unsigned module |
| | `slot-image` | Cuts a signed container into a whole-partition image with its trailer, for esptool |
| `keys` | `generate`, `export-public`, `sign`, `verify`, `inspect` | Key pairs per role, signing, checking |
| | `trust-block` | Builds a signed `purrcfg` image: mode, `DEV_LOCKED`, revocations, extra keys, flags |
| `bootpkg` | `build` | Compiles, links and wraps the boot package for a board; `--brand` takes a brand header |
| `shells` | `build` | Builds a loadable shell (plan7): the kernel's own headers and defines, linked against the placeholder addresses of `PurrOS/main/purr_shell_exports.def`, relocated like a module, packed as subtype `MOD_SHELL` with the import sites the loader patches; a name the shell uses that is not exported is a link error that lists it. Run it with `--kernel-build <build dir of a kernel built from this tree>` |
| `modules` | `build` | Compiles a module twice at two base addresses, finds relocations, packs it (`--kind driver\|appmanager\|runtime\|devbundle\|coreos\|app`) |
| | `index` | Writes `modules.manifest` from already-signed files |
| `apps` | `new`, `build` | Scaffold and build an app project |
| `dev` | `recovery` | Copies images into the server folder and regenerates both manifests |
| | `publish`, `index`, `list` | Add an app to the repository folder; write `purr/v1/index.json`; list it |
| | `serve`, `find` | Serve the folder and announce it on the network; listen for servers as a device would |
| | `withdraw`, `unwithdraw` | Mark a recovery version bad, or take the mark off |
| `uiassets` | `import-lvgl`, `build`, `package` | Pack pictures; wrap the pack for stage 2 |
| `games` | `package`, `inspect`, `target` | Game packages and Retro-Go targets |
| `netrec` | `write` | Build the saved-network image for a board |

## How it works

### The contract

A subscript is a file whose name equals `SCRIPT.name`. The loader imports every file in `scripts/` not starting with `_`, checks the declaration (duplicate action names, bad defaults, a choice with no choices), and checks `requires`. Results: `ok`, `broken` (import or declaration error, with the traceback) or `unavailable` (a missing package). An action returns an integer exit code; an exception is printed with its traceback and counts as failure.

### Two front ends, one definition

With arguments, purrstrap runs the CLI. With no arguments on a terminal it opens the text menu (arrows, Enter, Space, Esc), prefilled from the last values used, which are stored in `purrstrap/.state.json` (not tracked, and a convenience only). With no terminal it prints `list`. A command line never reads remembered values, so it always means the same thing.

### What a signed file is made of

Every subscript that writes a container uses [image.py](../purrstrap/lib/image.py): the 173-byte header ([chapter 03](03-trust-and-keys.md)), the payload, the SHA-256 of the payload, and (after `keys sign`) the signature. Signing is a separate step because the passphrase belongs to the signing command only. To sign, `keys sign --key <private key> --image <file> --key-id <id>`; to check, `keys verify --key <public key> --image <file>`.

### How a module is built

`modules build` links the sources twice, at base 0 and at base 0x100000, with a merged single-segment linker script. It compares the two binaries word by word. A word that differs by exactly the base difference is a relocation; any other difference is a build error. Using the compiler's own symbol types (`readelf -s`), it sorts relocations into **code** (a function address, resolved against the executable alias) and **data** (everything else, against the writable alias). Every address *inside* a function counts as code, which is what makes a `switch` jump table work (fixed 2026-11-01, tested in `test_modules_relocs.py`). The result is a prefix (entry offset, two relocation lists) plus the code blob ([Modules/SPEC.md](../Modules/SPEC.md) sections 4 and 5). Apps, drivers and command modules all share this pipeline; they differ only in `--kind`.

### The developer server

`dev serve` serves a folder over HTTP (default port 47801) and announces itself on the network every three seconds so a device can find it. `/recovery/<file>` serves plain names only; there is no way out of the folder. `dev recovery` writes `recovery.manifest` and `modules.manifest` from the files present, and refuses an image whose payload does not fit its partition. Apps go in `purr/v1/` with an index.

## Adding a subscript

1. Create `purrstrap/scripts/<name>.py`. The file name must equal `SCRIPT.name`.
2. Define `SCRIPT = Script(name=..., title=..., description=..., requires=(...), actions=(Action(...),))`. Each `Action` has a name, a title, help text, `params` and a `run(ctx, **params)` function that returns 0 on success. Param types are `str`, `int`, `bool`, `choice` (needs `choices`), `path`.
3. Use `ctx.info`, `ctx.ok`, `ctx.warn` and `ctx.error` for output, and `ctx.run(argv, cwd=...)` to run a program and stream its output.
4. Put anything the script needs that is not in the standard library in `requires`, so a machine without it sees "unavailable" and not a crash.
5. Run `python3 purrstrap/purrstrap.py list`. The new script must appear with status ok and its options listed; the options on the command line and in the menu come from the same declaration.
6. Add a test in `purrstrap/tests/` using `make_ctx()` and `write_script()` from [helpers.py](../purrstrap/tests/helpers.py), which give a capturing context and a throwaway scripts folder.
7. Write the "why" in the script's docstring. The spec's rule is that documentation lives with the code.

## A worked example: a kernel to a flashable slot

This is the chain a kernel image goes through, with each step's tool. The signing step needs the passphrase in the environment of that one command and nothing else.

1. `ninja -C out/stdfull` produces `out/stdfull/purros.bin` (a raw ESP-IDF application image, not yet a PURR file).
2. `coreos package-component --component kernel --file out/stdfull/purros.bin --version <x.y.z> --board tdeck_plus --out K.cat` wraps it in the container as an unsigned module with the kernel subtype.
3. `keys sign` writes the key id and signature into the header. `keys verify` with the public half confirms it.
4. For a release, `make_release.py` repeats steps 2 and 3 for every image and builds the manifest ([chapter 08](08-updates-and-release-channels.md)). For a cable flash, `coreos slot-image` appends the padding and the trailer, and `esptool` writes the result at 0x1E0000 (the kernel slot; 0x1A0000 up to 1.0.28).
5. The device checks the trailer at every start ([chapter 01](01-boot-chain.md)); the recovery loader and `netinstall` check the same header when they install.

The same shape holds for each other image: KittenOS uses `coreos package` (the recovery image type), the boot package uses `bootpkg build`, and a driver or app uses `modules build`. Only the wrapping step differs.

## Which Python to use

| Python | `keys` | `apps` | The rest |
|---|---|---|---|
| The system Python 3.9 on this Mac | broken (no `cryptography`) | broken (no `tomllib`) | works |
| The ESP-IDF Python environment (3.9) | works | broken (3.9 has no `tomllib`) | works |
| Python 3.11 or newer with `cryptography` | works | works | works |

The release scripts call the ESP-IDF Python (`~/.espressif/python_env/idf5.3_py3.9_env/bin/python`) because they need `keys`. Apps are therefore built with `modules build --kind app` in that setup ([plan9.md](../plan9.md)). The developer kit in [sdk/](../sdk/) bundles its own Python so none of this matters to an app developer ([plan5.md](../plan5.md)). The tool's own spec says 3.11 is the floor (raised 2026-10-18).

`coreos build` calls `idf.py` itself. It builds into `PurrOS/build/<board>-<profile>[-vm]`, while the release scripts and the repository notes use `out/stdfull`, `out/stdmin` and `out/stdrec` from a direct `cmake` and `ninja`. These are two ways to the same images ([chapter 04](04-kernel-and-services.md)).

## How to test it

```
cd purrstrap
python3 -m unittest discover -s tests
```

Use the ESP-IDF Python for the full run: the `keys` subscript needs `cryptography`. [plan5.md](../plan5.md) (inventory, 2026-11-06) reports 128 passing and 4 skipped and does not say why those four skip. I did not run the tests while writing this chapter. A plain count of test functions per file: `coreos` 23, `frontends` 17, `keys` 18, `uiassets` 15, `games` 13, `trustblock` 12, `registry` 10, `devrecovery` 10, `bootpkg` 8, `modules_relocs` 2. The trust-block bytes were cross-checked byte for byte against the C evaluator when they were written ([plan3.md](../plan3.md) item 4). See [chapter 10](10-host-tests.md).

## What goes wrong and how to fix it

| Symptom | Cause | Fix |
|---|---|---|
| `keys: broken ... cryptography` | Wrong Python | Use the ESP-IDF Python environment |
| `apps: broken ... tomllib` | Python older than 3.11 | Use 3.11 or build with `modules build --kind app` |
| `no PURRSTRAP_KEY_PASSWORD and no terminal to ask on` | Non-interactive run with no password in the environment | Give it in the environment of that one command |
| `slot-image`: image does not fit | Image plus the 4096-byte trailer is over the slot | Shrink it, or change the layout ([chapter 02](02-flash-map-and-partitions.md)) |
| `dev recovery`: payload too large for the partition | The loader could not install it | Fix the size before publishing |
| `modules build`: a non-relocatable word | Inline assembly with a hard-coded address | Rewrite that part |
| `coreos build` does nothing under Git Bash | `MSYS*` variables make `idf.py` exit 0 silently | The tool removes them; run from a normal shell |
| `idf.py` not found | ESP-IDF 5.3.5 not found | Run `coreos check`; the search order is in [purrstrap/SPEC.md](../purrstrap/SPEC.md) section 5 |

## What is not finished

- **No `package`/`verify`/`inspect`/`install` for apps** as listed in [AppManager/SPEC.md](../AppManager/SPEC.md) section 6; `keys inspect` and `games inspect` exist instead ([plan5.md](../plan5.md)).
- **No `cert` or `revoke` actions** in `keys`, although [Keys/SPEC.md](../Keys/SPEC.md) lists them; vendor certificates are not built ([chapter 03](03-trust-and-keys.md)).
- **No external signer** (a YubiKey in PIV mode was planned as a later signer).
- **[purrstrap/SPEC.md](../purrstrap/SPEC.md) lags the code.** It documents only `coreos`; the other subscripts are described by `list` and by their own docstrings, as the spec's principle says.
- **Key rotation** will change the signing steps (new key ids, new bootloader) ([plan3.md](../plan3.md)).
- **Games**: `payload = "module"` is not built; the packaged game images do not run on a device yet ([plan5.md](../plan5.md)).

Plan references: [plan5.md](../plan5.md) (SDK and kit), [plan3.md](../plan3.md) (keys), [plan8.md](../plan8.md) (the `repo` tool in the kit).
