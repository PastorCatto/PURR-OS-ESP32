# purrstrap spec (draft 0.1)

purrstrap is the PURR OS build and packaging tool: a TUI and a CLI over the
same set of **subscripts**. This is a from-scratch replacement for the old
purrstrap, which grew into one ~4000-line file with a menu layer wrapping its
command line, so the two drifted apart.

## 1. Principles

- **Small core, everything else is a subscript.** The framework loads
  subscripts and runs their actions. It knows nothing about ESP-IDF, keys or
  images.
- **One definition, two front ends.** A subscript declares its actions and
  parameters once. The CLI flags and the TUI forms are both generated from that
  declaration, so they cannot disagree.
- **A broken subscript never breaks the tool.** Import errors, bad
  declarations and missing dependencies are shown per subscript and everything
  else keeps working.
- **Standard library only.** Nothing to install. Python 3.9 or newer.
- **Documentation lives with the code.** Each subscript's title, description
  and per-action help text is the documentation, and `list` prints it. This spec
  covers only the contract.

## 2. Layout

```
purrstrap/
  purrstrap.py      entry point
  SPEC.md
  lib/
    model.py        Script, Action, Param
    registry.py     discover and validate subscripts
    context.py      paths, output helpers, process runner, remembered values
    cli.py          argparse generated from the declarations
    ui.py           menus and forms (ANSI, stdlib)
    app.py          main(): choose CLI or TUI
  scripts/
    coreos.py       first subscript: build the base system (section 5)
  tests/            unittest
```

## 3. Subscript contract

A subscript is one Python file in `scripts/` that defines a module-level
`SCRIPT`:

```python
from lib.model import Script, Action, Param

SCRIPT = Script(
    name="coreos",                 # must equal the file name
    title="Core OS",
    description="...",
    requires=(),                   # pip packages the script needs
    actions=(
        Action(name="build", title="Build", help="...",
               params=(Param("target", type="choice",
                             choices=("esp32", "esp32s3"),
                             default="esp32s3"),),
               run=build),         # run(ctx, **params) -> int, 0 = success
    ),
)
```

Param types: `str`, `int`, `bool`, `choice` (needs `choices`), `path`. A param
may be `required` or carry a `default`.

Loading rules:

- Files in `scripts/` are loaded in name order. Files starting with `_` are
  ignored.
- A file that fails to import, or whose `SCRIPT` is invalid (wrong name,
  duplicate action, bad default, a choice with no choices), is listed as
  **broken** with the error.
- A script whose `requires` are not installed is listed as **unavailable** and
  names the missing packages. It is not imported.
- Actions return an integer exit code. An exception in an action is caught,
  printed with a traceback, and counted as failure.

## 4. Front ends

**CLI**

```
purrstrap.py list                         all scripts, actions, params, status
purrstrap.py <script> <action> [--param value ...]
```

Bool params become `--flag` / `--no-flag`. Choices are validated. The exit code
is the action's. CLI runs never read remembered values, so a command line always
means the same thing.

**TUI**: with no arguments on a terminal, purrstrap opens a menu: scripts,
then actions, then a form for the params, then runs and waits for a key. Form
fields are prefilled from the last values used for that action. With no
arguments and no terminal it prints `list`.

Keys: arrows to move, Enter to select or edit, Left/Right to change a choice,
Space to toggle, Esc or `q` to go back.

**Remembered values**: last-used params per action, stored in
`purrstrap/.state.json` (not tracked). They only prefill TUI forms and are never
a source of truth for anything.

## 5. First subscript: coreos

Builds the base system in `PurrOS/` with ESP-IDF.

| Action | Params | What it does |
|--------|--------|--------------|
| `check` | none | Reports what is found and what is missing, without building. |
| `build` | `target`, `profile`, `clean` | Builds one target and profile. |
| `clean` | `target`, `profile` | Deletes that build directory. |
| `size` | `target`, `profile` | Runs `idf.py size` for an existing build. |

`target` is `esp32` or `esp32s3`. `profile` is a CoreOS profile: `minimal`, `recovery` or `full`.

**Finding ESP-IDF**, in order:

1. `IDF_PATH` set and `idf.py` on `PATH` (an already-activated shell).
2. The install manifest written by the ESP-IDF installer:
   `%IDF_TOOLS_PATH%\eim_idf.json` or `C:\Espressif\tools\eim_idf.json` on
   Windows, `~/.espressif/eim_idf.json` elsewhere. The selected install
   supplies the framework path and activation script.
3. On Linux and macOS, `~/esp/*/esp-idf/export.sh`.

The required version is a constant in the script (5.3.5 today). A different
version is a warning, not an error.

**Running commands**: when the shell is not already activated, each command runs
through the activation script (PowerShell on Windows, bash elsewhere), so the
user never has to open an ESP-IDF terminal.
Commands run with the `MSYS*` and `MINGW*` environment variables removed. Under
Git Bash, `idf.py` otherwise prints a warning and exits 0 without doing
anything, which would look like a successful build.

**Project requirements** (`check` lists any that are missing, so a build fails
with a clear message instead of an obscure one): in `PurrOS/`, `CMakeLists.txt`,
`partitions.csv`, `sdkconfig.defaults`, `sdkconfig.defaults.<target>` for the
chosen target, and `sdkconfig.profile.<profile>`.

**Build layout**: each combination has its own directory and sdkconfig, so
switching target or profile never reconfigures:

```
PurrOS/build/<target>-<profile>/          IDF build dir and sdkconfig
```

The command is `idf.py -B <dir> -D IDF_TARGET=<target>
-D SDKCONFIG=<dir>/sdkconfig
-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.<target>;sdkconfig.profile.<profile>"
build`. Full output is streamed and also saved to `<dir>/purrstrap-build.log`.
Artifacts stay in the build directory and their paths are printed at the end.

## 6. Later subscripts (not in this version)

- `keys`: generate per-role keys (password-protected private key files plus public keys),
  sign, verify and inspect, issue vendor certificates, and write the default public-key
  source file. Uses the `cryptography` package. See `../Keys/SPEC.md`.
- `image`: build, sign, verify and inspect containers to the `purr_abi.h`
  layout; write sample images for core's tests. Needs a crypto package, so it
  declares `requires`.
- `bootloader`: build the bootloader project.
- `flash`: flash and monitor.
- `repo`: walk a folder of pre-compiled `.cat` files, read each one's header and
  manifest, verify it, and write the app index for the app repo (`../OTA/SPEC.md`
  section 6). The index is the record of what was packed. The action also prints what
  changed since the last index: added, updated and removed apps.

Each is a new file in `scripts/`. The framework does not change.

**Config.** Subscripts read their settings from a config file, separate from the
remembered values in `.state.json`. It is kept in the project and tracked in git, in
JSON so it reads on every Python version purrstrap supports. For `repo` it holds the
folder of `.cat` files, the output path for the index, and the base download URL of
the app repo. Where the file lives and its exact layout are open.

## 7. Testing

`python -m unittest discover -s purrstrap/tests` with no dependencies:

- The registry against good, broken and missing-requirement scripts.
- Param coercion and the generated CLI.
- The TUI menu and form driven by scripted key sequences.
- The coreos subscript against a fake project tree and a fake runner: the
  ESP-IDF discovery order, the missing-file report, and the exact commands
  built.

## 8. Non-goals

- Mouse support, colors beyond basic ANSI, or any dependency to install.
- Running builds in parallel.
- Being the source of truth for any file format. Formats live in the C headers
  and their specs.

## 9. Open questions

- Whether `flash` should list serial ports itself or leave that to `esptool`.
- Whether the remembered-values file should move to a per-user location.
