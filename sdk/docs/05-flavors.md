# Making your own flavor

*Current as of PURR OS 1.1 (2026-11-08).*

A flavor is PURR OS with your own name and look. Its names and colours are **data**, in a file called `brand.toml`, so changing them never means editing code.

```toml
[brand]
product   = "Moon OS"        # the full system's name: boot menu, About, messages
recovery  = "Lunar Rescue"   # the recovery system's name
codename  = "Moonbeam"       # the board line under the wordmark
logo      = "MOON OS!"       # the wordmark on the boot screens, drawn bold and italic
logo_accent_from = 7         # letters from this position on use the accent colour (-1: none)

[colours]                    # boot screens, as #RRGGBB
accent = "#00C8FF"
text   = "#FFFFFF"
dim    = "#949694"
ok     = "#52CA7B"
warn   = "#FFB400"

[shell]
accent = "Teal"              # the shell's first accent: a Windows Phone colour name
```

## See it first

Studio's **brand** tool draws the splash, the boot menu and the "something is wrong" screen as you change things, using the boot package's own 8x8 font and layout, so what you see is what the screen will show. It also checks for trouble: a name that is too long, a character the font does not have, an accent too dim to read on black.

## What changes, and where

- **The boot screens** (the boot package: splash, menu, problem screen) take the wordmark, the names, the board line and the colours. The kit rebuilds the boot package from source, so this works today: make a flavor project, press **build**, sign it with your key.
- **The bootloader itself** (the very first code on the flash) has no screen. It only writes log lines and reports a version, so there is no brand to change there. "Rebranding the bootloader" really means rebranding the boot package it loads.
- **The system's names** (the shell, the recovery loader, KittenOS: About, the setup wizard, messages) use `PURR_BRAND_PRODUCT` and `PURR_BRAND_RECOVERY` from a header the kit writes (`purr_brand.h`). They change when the system is built from source; the prebuilt parts in this kit keep the stock names. The first accent colour works the same way.

## From a terminal

```
bin/purr new flavor moon-os
# edit projects/moon-os/brand.toml
bin/purr brand check projects/moon-os/brand.toml
bin/purr build moon-os --key boot --key-id 1
```

`purr brand build brand.toml` writes just the header, for a source build:

```
purrstrap bootpkg build --brand purr_brand.h
```

## Will a device take it?

A device only boots parts signed by a key in its key bag. A flavor you sign with your own key is accepted by a device in **warn** mode (it boots with a warning), or by one whose key bag has your key, which needs a bootloader built from source with your key. Until then, use a development board in developer mode. See [Keys and signing](06-keys.md).
