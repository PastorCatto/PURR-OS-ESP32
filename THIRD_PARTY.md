# Third-party material

## windows-10-mobile-lvgl

The Windows 10 Mobile style shell (`PurrOS/main/purr_w10m.c`, plan7.md) is modelled on, and uses
the unchanged artwork of, **windows-10-mobile-lvgl** by Felix Biego,
<https://github.com/fbiego/windows-10-mobile-lvgl>, under the MIT licence:

> MIT License
>
> Copyright (c) 2024 Felix Biego
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
> associated documentation files (the "Software"), to deal in the Software without restriction,
> including without limitation the rights to use, copy, modify, merge, publish, distribute,
> sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or
> substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
> NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
> NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
> DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

The images in that project (icons, tile art, wallpapers, and the Windows and Lumia imagery among
them) are packed into the picture pack (a raw `ui_assets` partition up to 1.0.28, a file in LittleFS from 1.0.29) unchanged for now (`purrstrap uiassets
import-lvgl`). Some of that imagery is not Felix Biego's to license (the Windows logo and Lumia art
are Microsoft's), so **it is placeholder art only and must be replaced before anything is
shipped**. The packaging is built so the replacement is a drop-in: a new pack.

## Fonts (Roboto, Noto Emoji)

Two fonts are stored for the "fonts from files" work (showtime1.md Part D, `docs/21-fonts.md`). They are subsets (about 85 KB each for Roboto Regular and Bold, 192 KB for the emoji) and live in [`UI/fonts/`](UI/fonts/). They are **not built into the kernel**; the loader that would read them from files is designed, not built. Each folder holds the licence text and a `SOURCE.txt` with the upstream address, version, SHA-256 of the original and the exact subset command.

- **Roboto** 3.016 (Regular and Bold, Latin, Greek, Cyrillic, Vietnamese subset), copyright 2011 The Roboto Project Authors, <https://github.com/googlefonts/roboto-3-classic>, **SIL Open Font License 1.1**. The UI text font. Folder: [`UI/fonts/roboto/`](UI/fonts/roboto/SOURCE.txt).
- **Noto Emoji** 3.002 (monochrome outline emoji, core set of 414 code points), copyright 2013 Google LLC, from the Google Fonts repository, <https://github.com/google/fonts/tree/main/ofl/notoemoji>, **SIL Open Font License 1.1**. The emoji font, tinted by the text colour. Folder: [`UI/fonts/noto-emoji/`](UI/fonts/noto-emoji/SOURCE.txt).

Both are modified versions (subset, hinting removed; Noto Emoji fixed to its regular weight) under section 2 of the OFL; neither declares a Reserved Font Name, so the names are kept. A colour emoji set is only studied so far: [`UI/fonts/COLOUR-EMOJI.md`](UI/fonts/COLOUR-EMOJI.md).

## LVGL

LVGL (MIT) is used as a managed component, version 9.2.x, <https://lvgl.io>.

## Android Open Source Project (Android 2.2 "Froyo" icons)

The Android style shell (`PurrOS/main/purr_android.c`, plan7.md, "Android shell build-out") draws its
launcher, dock and status-bar icons from the **Android Open Source Project**, copyright (c) The
Android Open Source Project, licensed under the **Apache License, Version 2.0**
(<https://www.apache.org/licenses/LICENSE-2.0>); the licence text and notice are in
`UI/android-assets/NOTICE`. They come from the Froyo (Android 2.2) release: the launcher and dock art
from the Launcher2, Phone, Contacts, Browser, Settings, Camera, Messaging, DeskClock and Music
repositories, and the battery, signal and Wi-Fi status icons from `frameworks/base`
(`core/res/res/drawable-mdpi`, branch `froyo-release`).

**Changes made:** each icon was resampled once, offline, to the size the shell draws it at
(`tools/ui/froyo_pack.py`) and renamed for its asset. Nothing is scaled on the device. The
resampled PNGs are in `UI/android-assets/`; `purrstrap uiassets build --alpha` packs them.

**Licence of the shell itself.** The Android style shell's source (`PurrOS/main/purr_android*.c` and `.h`, each marked with an SPDX line) and the original artwork made for it (the `kb_` key legends and the wallpapers) are released under the Apache License, Version 2.0, the same licence as the Android resources above, in respect of the Android Open Source Project. The rest of PURR OS is under the GNU General Public License version 3 (`LICENSE`).

Not used, on purpose: Google application artwork (Gmail, Maps, Market and the like) and any phone
maker's skin or icons, which are not under that licence. The shell's wallpapers and layout are
original.


## The emulator in the Developer Kit (QEMU, its libraries, Rust crates)

The kit's `emulator/` folder (`sdk/build-kit.py`, Showtime Plan 1 part A) carries the standalone emulator. The full notice, with versions and sources, is [`sdk/NOTICES-emulator.md`](sdk/NOTICES-emulator.md); it is copied into the kit as `emulator/NOTICES-emulator.md` next to `emulator/licenses/` (the licence texts) and `emulator/SOURCE.txt` (the source offer).

- **QEMU**, Espressif's fork (tag `esp-develop-9.2.2-20260417`, <https://github.com/espressif/qemu>), **GPL-2.0**, with our device models added (the ESP32-S3 SPI, I2C, analog master, ADC, GPIO and a socket bridge to the emulator application). Corresponding source: the base tree at that tag, plus `emulator/source/qemu-patches/` in the kit (also the `qemu/` folder of <https://github.com/PastorCatto/ESP32-Emulator>). The kit includes a written offer for the source (`emulator/SOURCE.txt`, valid three years from the kit's date). QEMU runs as a separate program; nothing in PURR OS links with it.
- **Libraries bundled with QEMU** (macOS, from MacPorts, unchanged, as separate files in `emulator/vendor/qemu/lib/` so an LGPL library can be replaced): glib 2.88.3, libgcrypt 1.12.4, libgpg-error 1.61, libiconv 1.19 and libintl (gettext 1.0) under **LGPL-2.1-or-later**; pcre2 10.48 and libslirp 4.9.1 (BSD-3-Clause); libffi 3.4.8 and pixman 0.46.0 (MIT); zlib 1.3.2 (zlib licence); zstd 1.5.7 (BSD-3-Clause, used under BSD); bzip2 1.0.8 (bzip2 licence).
- **The emulator application** (`esp32-emulator`, `headless_tap`) is **MIT OR Apache-2.0**, built with about 190 Rust crates (all MIT, Apache-2.0, BSD, ISC, Zlib, Unlicense, CC0, 0BSD or Unicode-3.0; none copyleft) and the fonts of `epaint_default_fonts` (SIL OFL 1.1, Ubuntu Font Licence 1.0, MIT). The list and the licence texts are generated at kit build time: `emulator/licenses/RUST-CRATES.txt` and `RUST-LICENSES.txt`.
- **ROM images** (`emulator/vendor/qemu/share/qemu/*.bin`) are the chip ROM images that Espressif's QEMU repository carries in its `pc-bios` folder, loaded unchanged.
