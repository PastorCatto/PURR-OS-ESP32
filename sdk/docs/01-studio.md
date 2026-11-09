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

## The main menu

The big tile at the top is always **the last thing you did**, in any part of Studio: a UI you were building, a device image, a driver, a flash that stopped halfway. Press it and you are back where you were. To its right, **recent activity** lists the last things, newest first. Under the big tile every part of Studio is one press away, in groups: **make** (build a UI, device image, new app, new driver, describe a board, brand a system, icon packs), **deliver** (flash a board, serve to a device, try in the emulator, gather a release, run a repository), **trust** (keys, sign a file), **look and check** (open an image, kit check, configure a device) and **learn** (guides, the element library, import from SquareLine). The chip under the title says whether a board is plugged in.

## build a system

One road for an image, drivers, apps and signing: **device**, **parts**, **build**, **deliver**. A *system* is saved by name (in `<workspace>/systems/`) so you can come back to it.

- **device:** choose the board (T-Deck Plus, T-Deck, Waveshare e-paper), the folder with the five signed system parts (the kit's `images/` by default, or a release you made), and your signing key. The page says which versions it found.
- **parts:** switch drivers on and off (each says whether it works on any ESP32-S3 board or only on the T-Deck boards), include your app projects, and choose a UI you built. Everything is saved as you click.
- **build:** one button runs the road and shows each step as it happens: build and sign your apps (it asks for the key's password here, once), draw your UI, gather the signed drivers and write their manifest, check the system parts and that they fit, make the whole flash image, and lay out a delivery folder (manifests, parts, drivers, apps, your UI) for a dev server. A failed step stops the road and says why.
- **deliver:** by cable (pick the port, tick that you mean it, a progress bar while it writes), over the air (Studio serves the delivery folder and tells the device where to look), or in the emulator.

The drivers come from the first drivers folder it finds: `PURR_DRIVERS`, the kit's `drivers/` folder, a checkout's dev server folder, or the hub's `repos/drivers`. The parts are already signed; only your apps are signed with your key at build time.

## build a UI

Choose the device (T-Deck Plus, T-Deck, Waveshare e-paper) and a starting point (blank, Android style, Windows style, a SquareLine export), name it, and you are in the builder. The centre is the device's real screen at 200%. Drag Home, Back, Menu, an app drawer, a dock, an app grid, a clock and the rest from the list on the left (or click one to add it); move things on the screen (arrow keys nudge, Shift for bigger steps, Delete removes); the right side edits the selected element: label, position, size, what a tap should do, colour, layers. Screens (home, app drawer, lock, notifications) are tabs. Everything saves as you go. **make my ui** turns the layout into the picture pack, colours and the screen file the device reads, checks it, and opens the **it is ready** page: what the device will show, and how to put it on a device, try it in the emulator, export it as a zip or publish it. **icons** in the toolbar switches between Studio's set and your own icon pack; **icon packs…** opens the page where you drop a zip of PNGs, a `pack.bin` or a SquareLine export, see which slots it fills, which Studio fills, and which are resized. See [16-device-image.md](16-device-image.md) and [17-making-a-ui.md](17-making-a-ui.md) for what is made.

What the device draws today is limited (at most 16 widgets a screen, and a tap does nothing yet on the shells that read the screen file): the builder says so as you work, and keeps every action in your layout for when the firmware wires it.

## make a UI

Describe a theme in one small file, `ui.toml`, and Studio draws every picture a shell asks for; your own pictures, or a SquareLine Studio export dropped in as a zip, win. See [17-making-a-ui.md](17-making-a-ui.md).

## device image

One whole flash image for a device, from the signed parts, with no terminal: pick the device and the folder with the parts, check how each part fits its slot, press **make the image**, then flash it. See [16-device-image.md](16-device-image.md).

## doctor, run

**doctor** checks the kit. **run** puts an app on the emulated device: pick an app project, the board to build for and a key, and press **run in the emulator** (the same button sits on a project's build tab). Studio builds and signs the app, serves it on port 47801, boots the VM and installs the app through the Developer Portal. Without the window the emulator presses the screen itself and the pictures appear on the page; with the window you drive it. **open the emulator** opens the VM on its own, and **stop it** closes a window Studio opened. The page says plainly when it cannot run (no emulator in the kit, no VM image, Windows) and why. Port 47801 must be free: stop a dev server that is using it first. The same thing from a terminal is `purr run`.
