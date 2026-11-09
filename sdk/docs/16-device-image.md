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
