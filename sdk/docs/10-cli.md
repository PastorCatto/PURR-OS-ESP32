# The `purr` command

*Current as of PURR OS 1.1 (2026-11-08).*

`purr` with no arguments opens Studio. Everything Studio does works here too.

| Command | What it does |
|---|---|
| `purr studio [--no-open]` | open PURR Studio (or print the link) |
| `purr doctor` | check the kit |
| `purr inspect PATH [--json]` | look inside an image, flash image (its partition table is compared with the board's layout), manifest or release folder; exits 1 if something is wrong |
| `purr new KIND NAME [--class C]` | start a project: `app`, `driver`, `board`, `flavor`, `theme` (a `theme.conf` and an `images/` folder; `purr build` packs it) |
| `purr build PROJECT [--board B] [--key K --key-id N]` | build and sign |
| `purr keys list`, `purr keys new --role R --key-id N` | your keys |
| `purr sign FILE --key K --key-id N` | sign an image |
| `purr brand check\|build brand.toml` | check a flavor, or write its header |
| `purr board check\|build board.toml` | check a board, or write its board file and profile |
| `purr release --board B --key K` | gather a signed release |
| `purr image --board B [--parts DIR] [--out FILE] [--trust-block F] [--plan]` | make one whole flash image from the signed parts (see [16-device-image.md](16-device-image.md)) |
| `purr system list\|new\|show\|set\|build\|test NAME ...` | a *system*: a device with its parts, drivers, apps and keys, saved by name. `new NAME --board B`; `set NAME --parts-from ready\|source --source-ref PURR2.0 --shell android --version 1.1.0 --boot-key K --boot-key-id 7 --key D --key-id 6 --keep-ours on\|off --restore-url http://10.0.2.2:47801/recovery --drivers a,b --apps mine,src:notes`; `show` lists the choices and what is wrong with the keys; `build` runs the whole road (below); `test` boots the built image in the emulator and checks it (`--seconds N`, `--image F`) |
| `purr source check\|pull\|compiler\|install-compiler [--url U] [--ref R]` | the PURR OS source a system is compiled from: is something newer, pull it into the workspace, is ESP-IDF 5.3.5 there, install it (about 2.5 GB; not on Windows) |
| `purr serve [--dir D] [--port P]` | run the dev server |
| `purr publish FILE` | put a build in the server's folder |
| `purr flash IMAGE --port P` | write an image to a board (a whole image whose partition table is not the board's layout gets a note first) |
| `purr emulator [IMAGE]` | open the emulator window on the VM image (or one you name); it runs a copy, never the original |
| `purr run TARGET [--key K] [--window] [--shots DIR]` | build and sign an app (TARGET is a project, or a signed `.cat`), serve it, boot the VM, and put the app on it through the Developer Portal; pictures go to `--shots` (see [12-emulator.md](12-emulator.md)) |
| `purr config schema`, `purr config check system FILE` | the settings a device knows; check a file |
| `purr font check FILE.ttf [--max-size BYTES] [--json]` | check a TrueType font the way the OS means to before it uses it: size cap (default 1 MB), table directory, required tables, glyph count, code point coverage, colour or bitmap tables (which the renderer does not support), and the copyright and licence strings; exits 1 with a plain message if the file fails |
| `purr ui new NAME [--shell S --board B]`, `purr ui import NAME --squareline EXPORT`, `purr ui make NAME` | start a UI project (a `ui.toml`), start one from a SquareLine Studio export, or turn `ui.toml` into `theme.conf` and `pack.bin` (see [17-making-a-ui.md](17-making-a-ui.md)) |
| `purr ui pack FOLDER [--out pack.bin]` | pack a folder of PNGs (file name = picture name) into a UI pack with purrstrap's packer: at most 128 pictures and 5 MB, transparency kept (RGB565 with an alpha plane), opaque pictures as plain RGB565 |
| `purr ui check PACK_OR_THEME [--shell w10m\|android] [--json]` | check a pack file (header, directory, offsets, CRC, size cap, picture sizes; with `--shell`, every required picture name of that shell) or a theme folder (`theme.conf` keys, its pack); exits 1 if it fails. See [14-ui-packs-and-themes.md](14-ui-packs-and-themes.md) |

`purr ui pack` and `purr ui check` use the kit's copy of purrstrap's packer, so a pack made here is the pack the release tools make. The device side that reads packs and themes is not finished ([14-ui-packs-and-themes.md](14-ui-packs-and-themes.md)).

`purr font check` reads the file itself, with no other tools, and warns (does not fail) for a colour font, a variable font, or a missing licence string. It checks the file's structure, not each glyph's outline, so a pass is not a proof that the file is safe to draw. A `pack` or `add` step for fonts is not written. It is a gate for the device side that is designed but not built: [docs/21-fonts.md](../../docs/21-fonts.md) in the repository.

Projects, keys and builds live in `~/PURR-Projects` (change it with `--workspace` or `PURR_WORKSPACE`). A key's password comes from a prompt, or from `PURRSTRAP_KEY_PASSWORD` for scripts.

## Building a system from a terminal
```
purr system new froyo --board tdeck_plus
purr system set froyo --parts-from source --source-ref PURR2.0 --boot-key my-boot.key --boot-key-id 7 --key my-dev.key --key-id 6 --keep-ours on
purr system show froyo                 # says what is wrong with the keys, if anything
PURRSTRAP_KEY_PASSWORD=...  PURRSTRAP_BOOT_KEY_PASSWORD=...  purr system build froyo
purr system test froyo                 # boots the image in the emulator and checks it
```
The passwords come from the environment of that one command, or are asked for in a terminal; they are never saved. `PURRSTRAP_KEY_PASSWORD` is the developer key's, `PURRSTRAP_BOOT_KEY_PASSWORD` the boot key's (the same one is used when it is unset). The same settings are in Studio's *build a system*; see [01-studio.md](01-studio.md) and [16-device-image.md](16-device-image.md). Exit status is 0 when the build (or the test) passed.

