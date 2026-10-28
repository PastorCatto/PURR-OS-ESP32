# tools/vm - scripts for the emulator test loop

These live in the repo (not in a session scratchpad) so a wiped `/tmp` cannot lose them. Outputs go to `out/`
(ignored by git).

| Script | What it does |
|--------|--------------|
| `mkimage_dbg.sh` | Patches a base flash image with a new signed kernel slot and a developer-mode trust block: the debug image (autologin dev/dev). |
| `publish_apps.sh` | Builds the sample apps from `Apps/`, signs them with developer key 2, publishes them to `out/devrepo`. |

Both need `PURRSTRAP_KEY_PASSWORD` in the environment of the one command (never written to a file).

Kernel build for the debug image (once, then `ninja -C out/vmdbg`; takes over 10 minutes the first time):

    cd PurrOS && . ~/esp/esp-idf-v5.3.5/export.sh
    cmake -B ../out/vmdbg -DIDF_TARGET=esp32s3 -DCCACHE_ENABLE=0 -DSDKCONFIG=../out/vmdbg.sdkconfig \
      -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.board.tdeck_plus;sdkconfig.profile.full;sdkconfig.flavor.vm;sdkconfig.flavor.vmdebug" -G Ninja .

Run the VM: back up `ESP32-Emulator/Testrun/vm-secure/flash.bin` as `flash.bin.prev`, copy `out/vm-debug-flash.bin` over
it, run `./run.sh` there. Serve the apps: `python3 purrstrap/purrstrap.py dev serve --dir out/devrepo --port 47801`.

Bootloader (patched into the image at 0x0 when present; the VM image needs the same bootloader as the boot package):

    cd bootloader && . ~/esp/esp-idf-v5.3.5/export.sh && B=../out/boot_tdeck_plus
    idf.py -B $B -D SDKCONFIG=$B/sdkconfig -D IDF_TARGET=esp32s3 -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.board.tdeck_plus" build

(The old `bootloader/build_*` folders came from a Windows checkout and cannot be reused here.)

Scenarios (`SCENARIO=... tools/vm/mkimage_dbg.sh`): `normal` boots the OS; `recover` starts the recovery loader once with the system
untouched; `first` is a blank device (FRESH on, kernel and KittenOS erased) that starts in the loader, for the "Hi there, first
time?" page. The loader and KittenOS slots are rebuilt from `out/vmmin` and `out/vmrec` when those builds exist (same cmake recipe as
vmdbg, with `sdkconfig.profile.minimal` and `sdkconfig.profile.recovery` and no vmdebug flavor).
