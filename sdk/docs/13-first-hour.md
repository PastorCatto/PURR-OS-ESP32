# Your first hour

*Current as of PURR OS 1.1 (2026-11-08).*

**Short version.** In about an hour you can check the kit, make an app, build and sign it, watch it install on an emulated device, and know where to go to put it on a real T-Deck Plus. Every command here is run from the kit folder. If a step fails, `bin/purr doctor` says what is missing.

What has been checked: the commands below exist and are covered by the kit's tests; the emulator steps were run end to end on a development image (unlock, Developer Portal, find the server, trust it, install). **Not yet in the kit:** the VM flash image that `purr run` and `purr emulator` boot (see "What is missing"), and launching the installed app by script (open it from the app drawer by hand).

## Minutes 0 to 10: is the kit whole?

1. Unpack the kit and open a terminal in its folder.
2. Run `bin/purr version`. It prints the kit version (2.0.0 or later).
3. Run `bin/purr doctor`. Each line says what it checked: Python, the toolchain, the signing library, the emulator, the VM image. A line that says "missing" has the fix written next to it.
4. If macOS refuses to open a program because the kit was downloaded: `xattr -dr com.apple.quarantine <the kit folder>` once.

## Minutes 10 to 25: make and build an app

1. `bin/purr new app hello`. This makes a project folder with a working starter.
2. Open the project's source file and change the text the app shows.
3. `bin/purr build hello`. This compiles for the ESP32-S3, relocates the code, and writes a `.cat` file; the command prints the path of each file it made. Without a key it is built but unsigned.
4. Make yourself a developer key: `bin/purr keys --role developer --key-id 2 new`. It asks you to choose a password at the terminal prompt. **Keep that password to yourself and never put it in a file or a script.** `bin/purr keys list` shows the keys you have.
5. Build again with the key: `bin/purr build hello --key <name from keys list> --key-id 2`. It asks for the password at the prompt (or reads `PURRSTRAP_KEY_PASSWORD` when it is set for that one command, which is how scripts do it).
6. Check the result: `bin/purr inspect <the .cat path>` shows the header, the signer and the payload hash.

A device only runs an app that is signed by a key it trusts. A device in developer mode can be told to trust your developer key; see [06-keys.md](06-keys.md).

## Minutes 25 to 40: see it run without a device

1. Make sure nothing else uses port 47801 (another dev server would answer instead of yours; `purr run` stops with a message if so).
2. `bin/purr run hello`. It builds and signs the app, serves it from your computer, boots the emulated device, unlocks the screen, opens the Developer Portal, finds your computer as an SDK server, trusts it, and installs the app. It writes pictures of each step into the folder you give with `--shots`.
3. A scripted run needs steps close together: the emulated screen locks after about a minute without input. The built-in steps are timed for this.
4. To drive the device yourself, `bin/purr emulator` opens the window on the same image. The window works on a copy; the original is never changed.
5. When the app has installed, press Home and open the app drawer to start it.

The emulator is slower than the real chip. Booting takes about a minute. More about it, boards and the command file for scripted runs: [12-emulator.md](12-emulator.md).

## Minutes 40 to 55: put it on a real T-Deck Plus

1. Start the dev server on your computer: `bin/purr serve --dir <folder> --port 47801`. Publish the signed app to it with `bin/purr publish`.
2. On the device, turn on developer mode in Settings (Privacy and security, Developer options), open the Developer Portal, and let it find your computer.
3. The device asks whether to trust the server. Read the name it shows, then trust it.
4. Install your app from the list.

More about servers, releases and recovery images: [07-release-and-server.md](07-release-and-server.md). Flashing a whole device: [09-flashing.md](09-flashing.md).

## Minutes 55 to 60: where next

| You want to | Read |
|-------------|------|
| write a driver for a chip | [02-drivers.md](02-drivers.md) |
| describe a board | [03-boards.md](03-boards.md) |
| make your own flavor of the system | [05-flavors.md](05-flavors.md) |
| publish apps to other people | [11-repositories.md](11-repositories.md) |
| see every command | [10-cli.md](10-cli.md) |

## What is missing

- **The VM image.** `purr run` and `purr emulator` boot `images/vm-flash.bin` (or the file named in `PURR_VM_IMAGE`). The kit does not carry one yet. The plan: when `purr run` finds no image it asks whether to build one, and assembles it from the parts the kit carries; and a blank image that installs itself from the official server. Until then, bring your own image: see [showtime1.md](../../showtime1.md).
- **Launching the app by script.** The run installs the app; opening it from the drawer is by hand.
- **Not verified on a device:** the real-device steps above were written from the commands and the plans, not from a fresh install on a new unit.
