# PURR kernel

The standalone kernel binary: hardware bring-up and the root filesystem, nothing else
(`PurrOS/components/kernel/SPEC.md`). A separate ESP-IDF project, same shape as
`bootloader/`, reusing `PurrOS/components/kernel` directly -- no `PurrOS/main` (no shell,
login, modules, apps), so this is a genuinely smaller binary, not just another PurrOS
profile. ~267 KB vs. the monolith's ~1.08 MB.

Idles once hardware and the filesystem are confirmed up -- there is nothing to hand off to
yet. Loading CoreOS as a relocatable file from here (`PurrOS/SPEC.md` section 6) is the next
real step, not built here.

## Build

```
idf.py -B build_tdeck_plus -D SDKCONFIG=build_tdeck_plus/sdkconfig -D IDF_TARGET=esp32s3 \
    -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.board.tdeck_plus" build
```

The app binary is `build_tdeck_plus/purr_kernel.bin`. Flash it alone to the `kernel`
partition (`0x1a0000` on the T-Deck Plus) -- never the bootloader/partition-table portion of
the suggested `flash_args`, which belong to `bootloader/`'s own project, not this one.
