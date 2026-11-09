# Making a device image

*Current as of PURR OS 1.1 (2026-11-08).*

**Short version.** The image maker puts the signed parts of a system into one file the size of the board's flash. Flash that file by cable and the device starts straight into the system. In Studio it is the **device image** tile; from a terminal it is `bin/purr image`. It needs no compiler, no ESP-IDF and no shell script.

## What goes in

| Where | What | Where it comes from |
|---|---|---|
| 0x0 | the bootloader | the raw image inside the signed bootloader container |
| 0xA000 | the partition table | written from the board's table (`PurrOS/partitions/<board>.csv`), byte for byte what ESP-IDF's tool writes |
| `purrcfg` | the trust block | only if you give one (`--trust-block`); without it the area stays erased and the device uses its defaults |
| `bootpkg` | the boot package | the signed container, as it is |
| `kittenos`, `kernel`, `loader` | the three slots | each signed container cut into a whole-partition image with its signature trailer (`purrstrap coreos slot-image`) |
| `netrec`, `root` | saved network, file system | left erased: the setup wizard or a recovery fills them |

Nothing about *where* is fixed in the code. The offsets and sizes come from the board's table, so a board with its own table works the same way. Today that is the T-Deck Plus, the T-Deck and the Waveshare e-paper board (flash map v2 shape).

## The parts have to be signed already

The image maker does not sign. The kit's prebuilt parts are signed by the project. A release made with `bin/purr release` is signed with your key. Point the image maker at a folder with the five parts: it looks for `kernel-<board>.cat` or `kernel_<board>.cat` (and the same for `kittenos` as `.kitt`, `loader`, `bootpkg` and `bootloader`), in the folder itself or its `recovery/` or `JumpingJaguar1/` subfolder. Each file is checked first: it must be a PURR container whose payload matches its hash, and it must fit its slot.

## In Studio

Open **device image**. Pick the device, pick the folder with the parts (Studio lists the kit's `images/` and your releases, and says how many of the five parts each has). The page shows each part with its version, the key that signed it and how full its slot is. **make the image** writes the file into your builds folder, then offers **flash it to a board** and **look inside**.

## From a terminal

```
bin/purr image --board tdeck_plus --parts <folder>            # make it
bin/purr image --board tdeck_plus --parts <folder> --plan     # only say what is there and what is missing
bin/purr flash <the image> --port /dev/cu.usbmodem1101
```

## Good to know

- The result is checked before it is written: the partition table inside it is read back and compared with the board's layout. `purr inspect` on the image agrees (layout, hashes, signatures).
- A device only boots parts signed by a key in its key bag. A stock device knows the project's keys; see [06-keys.md](06-keys.md) for what your own key means.
- Not run on a device yet. The image was built and inspected on the computer; flashing it is the next step to try.

## Compile everything yourself

The image maker packs signed parts and does not care where they came from. **Ready-made** parts come from a release. In Studio's *build a system* (device step) you can choose **compile from the newest source** instead, for a person who trusts nobody's binaries and wants an image they vetted and built themselves. Studio pulls the PURR OS source into `<workspace>/source/tree` (git, only the `PurrOS`, `UI`, `purrstrap`, `bootloader`, `bootpkg`, `Modules` and `Apps` folders; without git, a GitHub tarball) and compiles, with ESP-IDF 5.3.5 and the kit's compiler:

- **the firmware:** the kernel (`full` profile), the recovery loader (`minimal`), KittenOS (`recovery`), the boot package and the bootloader, signed with your **boot key**;
- **the eight system modules** (about, appmgr, apps, fs, login, netinstall, sysinfo, wifi), **the drivers** you switched on (those with source in `Modules/drivers`; one without is left out and named, never replaced by a ready-made one) and **the apps** from the source's `Apps/` folder that you switched on, plus your own app projects, all signed with your **developer key**.

**Also the binaries baked into the kernel.** The kernel carries signed binaries inside its own source (`PurrOS/main/commands.c`): the display, touch and keyboard drivers the recovery loader starts from, the battery and backlight drivers, and the system modules it plants on a fresh device; `purr_builtin_apps.c` holds the Store and its companion. They are signed by PURR's key, so an image that does not trust PURR's key could not even start its display. A build from source therefore compiles each of these 14 binaries and the two Store apps again from `Modules/` and `Apps/`, signs them with your developer key, and writes them into the tree before compiling the kernel and the loader. (Two test blobs used only by test commands are left as they are; nothing trusts them.)

Versions read `src-<seven letters of the commit>`. The image maker and the delivery folder (with a `drivers.manifest` and the modules) then use what was compiled; nothing ready-made is mixed in.

### Whose keys the image trusts

The bootloader, the kernel and the boot package all check signatures against one compiled-in list of public keys (`PurrOS/components/coreos/keys/purr_default_keys.c`). A build from source writes that list before compiling:

- **Keep PURR's keys** (on by default): the list holds PURR's five keys and yours. The image runs what you compiled and still accepts PURR's updates, so you can build your own image and keep getting ours. Give your keys ids from 6 to 31.
- **Off:** the list holds only your boot key and developer key. The bootloader and the kernel then trust nothing signed by PURR, including its updates, until you switch it back on and build again. This is the image for someone who has vetted the source and wants nobody else's key in it.

The page checks the choice (both keys chosen, different ids, ids 0 to 31, and no id that is PURR's while PURR's are kept). The public keys are the `.pub` files next to your private ones (the keys page writes both). The passwords are asked once, in memory, and never saved.

- **The Android Froyo screen is part of the kernel.** The build packs the pictures from `UI/android-assets` into the kernel; with the shell set to Android (`--shell android`, the `sdkconfig.shell.android` overlay) that comes from the pulled source, so pulling updates it too. The default source is the project's `main` branch: if it does not hold the shell yet, the build stops and says which file is missing.
- **Sizes:** a compiled Froyo kernel is close to its slot (about 98% of 2.13 MB); the build stops if a part does not fit.
- **Not done:** the trust block (`purrcfg`) is still left blank by the image maker; a device whose purrcfg carries a signed block checks it against the compiled-in boot keys, so a block signed by your boot key works only once your key is in the list.
- From a terminal: `purr system new`, `purr system set ... --parts-from source ...`, `purr system build`, `purr system test` (see [10-cli.md](10-cli.md)), and the steps underneath are `purrstrap coreos build --board tdeck_plus --profile full --shell android` (and `minimal`, `recovery`), then `coreos package-component`, `coreos package`, `bootpkg build` and `keys sign`; modules, drivers and apps are `purrstrap modules build --kind driver|app`. `PURR_REPO_ROOT=<folder>` makes purrstrap build another checkout than its own, and `keys export-public` writes the key list.
