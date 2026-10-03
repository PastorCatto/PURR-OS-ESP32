# 22. purrstrap reference

purrstrap is the project's build and packaging tool: a text menu and a command line generated from the same declarations.
Standard library only (Python 3.9+), except the `keys` script, which needs `cryptography`. Source: `purrstrap/`. Design:
`purrstrap/SPEC.md`. **72 unit tests** (`python -m unittest discover -s purrstrap/tests`).

## Running it

```powershell
python purrstrap/purrstrap.py list                         # every script, action and parameter, with status
python purrstrap/purrstrap.py <script> <action> [--param value ...]
python purrstrap/purrstrap.py                              # on a terminal: the interactive menu
```

- **CLI:** parameter names become flags with `_` turned into `-` (`key_id` is `--key-id`). Booleans are `--flag` and `--no-flag`.
  Choices are validated. The exit code is the action's. A CLI run never reads remembered values, so a command line always
  means the same thing.
- **Menu (TUI):** run with no arguments in a real terminal. Arrow keys move, Enter selects or edits, Left/Right changes a choice,
  Space toggles, Esc or `q` goes back. Form fields are prefilled from your last values for that action, stored in
  `purrstrap/.state.json` (not tracked). With no terminal it prints `list`.
- **Broken or unavailable scripts:** a script that fails to import is listed `broken` with the error; one whose `requires` packages are
  missing is `unavailable`. Neither stops the others. Files in `scripts/` starting with `_` are ignored.
- **Passwords** for `keys` come from a prompt or `PURRSTRAP_KEY_PASSWORD`, never an argument ([17](17-keys-and-signing.md)).
- Every command run by an action streams its output. `coreos build` also writes `purrstrap-build.log` in the build directory.

Working directory: run from the repository root; paths in examples are relative to it. The tool finds the repo root from its own location.

## coreos: build the base system

`PurrOS/` through ESP-IDF 5.3.5. Details in [04](04-building-and-flashing.md).

| Action | Parameters | Does |
|--------|------------|------|
| `check` | none | reports ESP-IDF (path, how found, version) and, for every board and profile, whether the settings files exist. Exit 1 if anything is missing. |
| `build` | `--board` (`tdeck_plus`, `cyd_24c`, default `tdeck_plus`), `--profile` (`minimal`, `recovery`, `full`, default `full`), `--clean` | builds one combination into `PurrOS/build/<board>-<profile>/`, passes `PROJECT_VER=internaldirty<base>-<N>`, prints the artifact paths |
| `clean` | `--board`, `--profile` | deletes that build directory |
| `size` | `--board`, `--profile` | `idf.py size` on an existing build |
| `package` | `--board`, `--profile` (`recovery` or `full` only), `--version` (required), `--out` | wraps `purros.bin` into an **unsigned** `PURR_IMG_RECOVERY` (`kittenos`) or `PURR_IMG_OS` (`purros`) container, default output `<build dir>/<name>.kitt` |
| `package-component` | `--component` (`kernel` or `loader`), `--file` (required), `--version` (required), `--board`, `--out` | wraps an already-built raw binary into an unsigned `PURR_IMG_MODULE` with subtype kernel or loader, default output `<component>.cat` |

`cyd_24c` appears in the choices but its files do not exist, so `check` reports it as broken and `build` refuses ([F-01](FINDINGS.md#f-01)).
`package` for `minimal` is refused ("flashed directly, never downloaded").

What each profile build needs in `PurrOS/`: `CMakeLists.txt`, `sdkconfig.defaults`, `sdkconfig.board.<board>`, `sdkconfig.profile.<profile>`,
`partitions/<board>.csv`. The IDF command is
`idf.py -B build/<board>-<profile> -D IDF_TARGET=<chip> -D SDKCONFIG=<dir>/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.board.<board>;sdkconfig.profile.<profile>" -D PROJECT_VER=... build`.

## bootpkg: the boot menu

| Action | Parameters | Does |
|--------|------------|------|
| `build` | `--board` (`tdeck_plus` only) | compiles `bootpkg/src/*.c` plus `purr_menu.c` and `purr_font_data.c` with the Xtensa compiler, links for the SRAM window, wraps in an **unsigned** container: `bootpkg/build/<board>/bootpkg.bin`. Sign it with `keys sign --key-id 1`. |

Fails if the package outgrows 32 KB of code or 32 KB of data plus bss.

## keys: sign and verify

Needs `cryptography`. Full walkthrough in [17](17-keys-and-signing.md).

| Action | Parameters | Does |
|--------|------------|------|
| `generate` | `--role` (boot, system, owner, developer, vendor; required), `--out` (required), `--key-id` (cosmetic) | writes `<role>.key` (encrypted PKCS#8 PEM) and `<role>.pub` (PEM); asks for a password twice |
| `export-public` | `--pub`, `--role`, `--key-id` (all required), `--out` (required) | writes `<role>_<id>.pub.bin` (raw 64 bytes) and **regenerates `purr_default_keys.c`** from every such file in `--out` |
| `sign` | `--key`, `--image` (required), `--out` (default: overwrite), `--key-id` (0 = leave as is) | recomputes the payload hash, sets the key id, signs the header |
| `verify` | `--key` (public PEM or 64 raw bytes), `--image` (required) | checks payload hash and signature |
| `inspect` | `--path` (required) | prints an image header, a public key, a private key (asks the password) or a raw key |

Not built: `cert`, `revoke`.

## modules: loadable commands

| Action | Parameters | Does |
|--------|------------|------|
| `build` | `--source` (required, one file or comma-separated), `--entry` (required), `--kind` (`driver` default, `appmanager`, `runtime`, `devbundle`, `coreos`), `--name` (required), `--version` (required), `--board`, `--out` | compile twice at two bases, diff, classify relocations, pack an unsigned `PURR_IMG_MODULE`. Scratch files: `Modules/build/<name>/`. Default output `Modules/build/<name>/<name>.cat`. |
| `index` | `--board`, `--modules-dir`, `--kernelmods-dir`, `--coreos-version`, `--key` (default `developer`), `--out` (default `modules.manifest`) | writes a module index from already-built, already-signed `.cat` files |

Details and rules: [12](12-writing-modules.md). `index` writes `component`, `type`, `version`, `board`, `file`, `size`, `sha256`, `key` and, if given, `min_coreos`
(it does not write `chip`, which defaults to `any`).

## What purrstrap does not do

These are in `purrstrap/SPEC.md` section 6 and absent ([F-20](FINDINGS.md#f-20)):

| Missing | Do it by hand |
|---------|---------------|
| build the bootloader | `idf.py` inside `bootloader/` ([04](04-building-and-flashing.md)) |
| build the standalone kernel | `idf.py` inside `Kernel/` |
| flash and monitor | `python -m esptool ...`, a serial terminal |
| write the recovery manifest | [examples/tools/mkmanifest.py](examples/tools/mkmanifest.py) |
| build or index apps (`apps`, `repo` scripts), the SDK | nothing exists |
| `image`: build, sign and inspect containers generally | `lib/image.py` from Python, `keys inspect` |
| change `purrcfg` | [examples/tools/mkcfg.py](examples/tools/mkcfg.py) |

## Adding a script

A script is one file in `purrstrap/scripts/` defining `SCRIPT = Script(name=<file name>, title, description, actions=(...), requires=(...))`; each
`Action(name, title, run, help, params)` where `run(ctx, **params)` returns an int exit code and `Param(name, type, default, choices, help, required)` has type
`str`, `int`, `bool`, `choice` or `path`. The context (`ctx`) offers `info`, `ok`, `warn`, `error`, `run(argv, cwd, log_path, env)` and `repo_root`. The CLI flags
and the menu forms are generated from the declaration, so there is nothing else to write. See `scripts/bootpkg.py` for the smallest example.

Next: [23 Cookbook](23-cookbook.md).
