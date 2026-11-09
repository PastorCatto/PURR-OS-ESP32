# The tour of PURR Studio

*Current as of PURR OS 1.1 (2026-11-08).*

Studio looks like the start screen of a Windows Phone on purpose. Every tile is a tool. Click one, and the back button (the circle with the arrow) takes you home.

## open an image

Drag any file onto the window. Studio looks inside it:

- A `.cat` or `.kitt` file: its name, version, chip, and whether the contents are whole (the hash) and signed (the signature). If it was signed with a key I know, I say whose, and whether that role is allowed to sign this kind of file.
- A **release folder**: every file in its manifests is checked against its size and hash.
- A **flash image**: a coloured map of the partitions. Click one to see what is in it. Green means signed and checked, red needs a look, grey is erased. The trust block (how strict the device is) is shown in plain words. It also says whether the partition table is the board's layout (flash map v2, see [09-flashing.md](09-flashing.md)).
- A manifest, or a key.

Under the result are the next steps that make sense for that file.

## new project, my projects

Four kinds of project:

- **app**: a program people run. Starts from the app template.
- **driver**: teaches the OS to talk to a chip. Pick the class (GPS, LoRa, modem, Wi-Fi, Bluetooth, or a plain I2C sensor) and you get a complete module with the right interface and "TODO" notes where your code goes.
- **board**: the pins, buses and parts of a piece of hardware.
- **flavor**: your own names and colours.

In **my projects** you edit the files right there, then press **build**. Pick a key to sign with, or leave it unsigned for a first try.

## boards

Shows a board file and checks it as you type: a pin the chip cannot use, two parts on one pin, a setting name that is too long. The right side shows which parts of a complete T-Deck Plus have a driver, a map of the pins, and what the board file makes: the C file the kernel is built with, and the device profile a running device reads.

## brand

Names and colours for your flavor. See [Making your own flavor](05-flavors.md).

## keys

Make a key, see the keys every device already knows, and a plain table of who may sign what.

## release, server

Gather the parts of a system into a folder a device can recover from, then hand it to a device on your network. See [Releases and the dev server](07-release-and-server.md).

## configure

Edit the files that configure a device: `system.conf` (the settings), `apps.conf` (how apps run) and the boot trust block.

## doctor, run

**doctor** checks the kit. **run** puts an app on the emulated device: pick an app project, the board to build for and a key, and press **run in the emulator** (the same button sits on a project's build tab). Studio builds and signs the app, serves it on port 47801, boots the VM and installs the app through the Developer Portal. Without the window the emulator presses the screen itself and the pictures appear on the page; with the window you drive it. **open the emulator** opens the VM on its own, and **stop it** closes a window Studio opened. The page says plainly when it cannot run (no emulator in the kit, no VM image, Windows) and why. Port 47801 must be free: stop a dev server that is using it first. The same thing from a terminal is `purr run`.
