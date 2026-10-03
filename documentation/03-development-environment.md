# 03. Dev environment

## What you need

| Tool | Version used | Needed for |
|------|--------------|------------|
| ESP-IDF | **5.3.5** exactly (a different version only warns) | building `PurrOS/`, `bootloader/`, `Kernel/`; also supplies the Xtensa compiler that `purrstrap modules build` and `bootpkg build` use |
| Python | 3.9 or newer (3.11 verified) | purrstrap, esptool |
| `cryptography` (pip) | any recent (50.0.1 verified) | **only** `purrstrap keys`. The rest of purrstrap needs nothing. |
| A host C compiler (gcc) | MinGW-w64 via WinLibs works | the host unit tests only |
| `esptool` | ships with ESP-IDF (`python -m esptool`) | flashing |

purrstrap itself is standard library only on purpose. If `cryptography` is missing, the `keys`
script shows as "unavailable" in `purrstrap list` and everything else still works.

## Windows setup (the author's machine)

1. Install ESP-IDF 5.3.5 with the ESP-IDF installer. Default locations:
   - framework: `C:\esp\v5.3.5\esp-idf`
   - tools and activation script: `C:\Espressif\tools\` (the manifest is `eim_idf.json`, the
     PowerShell activation script is `Microsoft.v5.3.5.PowerShell_profile.ps1`)
2. Install Python deps for signing: `pip install cryptography`
3. Check everything: `python purrstrap/purrstrap.py coreos check`

Expected: the `tdeck_plus` lines say `ready to build`. **[PROBLEM]** The three `cyd_24c` lines report
errors because the CYD files do not exist ([F-01](FINDINGS.md#f-01)). That is expected, not a broken install.

## How purrstrap finds ESP-IDF

In this order (from `purrstrap/scripts/coreos.py`):

1. `IDF_PATH` set and `idf.py` on `PATH` (an already activated shell).
2. The installer manifest: `%IDF_TOOLS_PATH%\eim_idf.json` or `C:\Espressif\tools\eim_idf.json` on
   Windows, `~/.espressif/eim_idf.json` elsewhere.
3. On Linux and macOS, `~/esp/*/esp-idf/export.sh`.

When the shell is not activated, purrstrap runs every `idf.py` call through the activation script, so
you never have to open an "ESP-IDF terminal" for `coreos build`.

For the **manual** builds (`bootloader/`, `Kernel/`, flashing) you do need an activated shell:

```powershell
. C:\Espressif\tools\Microsoft.v5.3.5.PowerShell_profile.ps1
```

## The Git Bash trap

Under Git Bash, `MSYSTEM=MINGW64` is set. `idf.py` then prints "MSys/Mingw is no longer supported"
and **exits 0 without building**, which looks like success. purrstrap strips `MSYS*` and `MINGW*`
variables from the environment it passes down, so `coreos build` is safe. Hand-typed `idf.py` in Git
Bash is not. Use PowerShell for manual builds.

## Keep the repository path short

ESP-IDF's build writes dependency files at very deep paths. On Windows, once the build directory path gets long, builds fail with errors like:

```
fatal error: opening dependency file esp-idf\wpa_supplicant\CMakeFiles\...\crypto_mbedtls.c.obj.d: No such file or directory
```

This was hit for real while writing these documents (a build under a deep temp folder). `D:\Projects\PURR-OS-ESP32` works; a copy several folders deeper may not. Build in a short path such as
`C:\purr` ([F-29](FINDINGS.md#f-29)).

## The toolchain for modules

`purrstrap modules build` and `bootpkg build` call `xtensa-esp32s3-elf-gcc`, `objcopy`, `nm` and
`readelf` directly. They look on `PATH`, then in
`<IDF_TOOLS_PATH or C:\Espressif or ~/.espressif>/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin`.
If they are not found you get `xtensa-esp32s3-elf tools not found. Install ESP-IDF, or put them on
PATH.`

## Git identity

No git identity is configured on the author's machine. If you commit, set
`GIT_AUTHOR_NAME`, `GIT_AUTHOR_EMAIL`, `GIT_COMMITTER_NAME`, `GIT_COMMITTER_EMAIL` (or
`git config user.name` and `user.email`). The project convention is to **not commit unless asked**.

## Line endings

Git warns that CRLF will be replaced by LF in `commands.c`. `.gitattributes` exists. The generated C
source files are written with `\n` explicitly.

## How the project works

Working rules the author follows, useful if you contribute:

- **Spec first.** Each chunk has a `SPEC.md` next to its code. Write or update the spec in the same change as the code, and keep one source of truth per topic. The previous codebase (`archive/DP9`) died of
  documentation drift.
- **Every user-facing feature needs a fallback.** A failed normal boot goes to KittenOS, a failed KittenOS to the recovery loader, a failed loader to a halt with a message.
- **Hardware claims stay honest.** Anything not verified on the board is marked as such in the spec.
- **Do not commit unless asked**, and never commit private keys.
- Branch `PURR2.0` is the rewrite; `main` is the old line's history.

## Smoke test of the whole toolchain (no hardware)

```powershell
cd D:\Projects\PURR-OS-ESP32
python purrstrap/purrstrap.py list
python -m unittest discover -s purrstrap/tests
python PurrOS/components/coreos/test/host/run.py
```

You should see `Ran 72 tests ... OK` and `ALL PASSED` (see [20](20-testing.md)).

Next: [04 Building and flashing](04-building-and-flashing.md).
