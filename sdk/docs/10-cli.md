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
| `purr serve [--dir D] [--port P]` | run the dev server |
| `purr publish FILE` | put a build in the server's folder |
| `purr flash IMAGE --port P` | write an image to a board (a whole image whose partition table is not the board's layout gets a note first) |
| `purr emulator [IMAGE]` | open the emulator window on the VM image (or one you name); it runs a copy, never the original |
| `purr run TARGET [--key K] [--window] [--shots DIR]` | build and sign an app (TARGET is a project, or a signed `.cat`), serve it, boot the VM, and put the app on it through the Developer Portal; pictures go to `--shots` (see [12-emulator.md](12-emulator.md)) |
| `purr config schema`, `purr config check system FILE` | the settings a device knows; check a file |
| `purr font check FILE.ttf [--max-size BYTES] [--json]` | check a TrueType font the way the OS means to before it uses it: size cap (default 1 MB), table directory, required tables, glyph count, code point coverage, colour or bitmap tables (which the renderer does not support), and the copyright and licence strings; exits 1 with a plain message if the file fails |
| `purr ui pack FOLDER [--out pack.bin]` | pack a folder of PNGs (file name = picture name) into a UI pack with purrstrap's packer: at most 128 pictures and 5 MB, transparency kept (RGB565 with an alpha plane), opaque pictures as plain RGB565 |
| `purr ui check PACK_OR_THEME [--shell w10m\|android] [--json]` | check a pack file (header, directory, offsets, CRC, size cap, picture sizes; with `--shell`, every required picture name of that shell) or a theme folder (`theme.conf` keys, its pack); exits 1 if it fails. See [14-ui-packs-and-themes.md](14-ui-packs-and-themes.md) |

`purr ui pack` and `purr ui check` use the kit's copy of purrstrap's packer, so a pack made here is the pack the release tools make. The device side that reads packs and themes is not finished ([14-ui-packs-and-themes.md](14-ui-packs-and-themes.md)).

`purr font check` reads the file itself, with no other tools, and warns (does not fail) for a colour font, a variable font, or a missing licence string. It checks the file's structure, not each glyph's outline, so a pass is not a proof that the file is safe to draw. A `pack` or `add` step for fonts is not written. It is a gate for the device side that is designed but not built: [docs/21-fonts.md](../../docs/21-fonts.md) in the repository.

Projects, keys and builds live in `~/PURR-Projects` (change it with `--workspace` or `PURR_WORKSPACE`). A key's password comes from a prompt, or from `PURRSTRAP_KEY_PASSWORD` for scripts.
