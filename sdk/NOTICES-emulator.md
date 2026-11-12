# Notices for the emulator in the PURR Developer Kit

The kit's `emulator/` folder holds third-party programs and libraries. This page says what they are, under which licence the kit passes them on, and where their source is. The licence texts are in `emulator/licenses/`. The rest of the kit is covered by `LICENSE-TODO` and the repository's `THIRD_PARTY.md`.

Short version: the emulator application is ours (MIT OR Apache-2.0) and uses open-source Rust crates. QEMU is GPL-2.0 and ships with its source offer (`emulator/SOURCE.txt`). The libraries QEMU needs are separate files you can replace.

## 1. QEMU (GPL-2.0)

| | |
|---|---|
| What | `emulator/vendor/qemu/bin/qemu-system-xtensa`, the Xtensa (ESP32, ESP32-S3) system emulator |
| Base | Espressif's QEMU fork, tag `esp-develop-9.2.2-20260417` (QEMU 9.2.2), <https://github.com/espressif/qemu> |
| Licence | GNU General Public License version 2 (`licenses/QEMU-GPL-2.0.txt`); parts of QEMU are under other compatible licences, stated in the source files (`licenses/QEMU-LGPL.txt`) |
| Our changes | New device models and edits to the board model: the ESP32-S3 general-purpose SPI, I2C, analog master, ADC (SENS), USB serial/JTAG, a GPIO output/bit-bang model, a socket bridge to the emulator application, and the app CPU held in reset until released. They are GPL-2.0 too, as part of the same program. |
| Our changes, source | `emulator/source/qemu-patches/` in the kit (the device sources under `devices/`, `apply.sh`, `build.sh`); the same files are in the emulator project, <https://github.com/PastorCatto/ESP32-Emulator>, folder `qemu/` |
| Rebuilding | `source/fetch-qemu-src.sh` fetches the base tree at the tag above; `qemu/apply.sh` applies our changes; `qemu/build.sh` configures and builds. The exact commits and the sha256 of the shipped program are in `emulator/SOURCE.txt` |
| ROM images | `vendor/qemu/share/qemu/*.bin` are the chip ROM images that Espressif's QEMU repository carries in its `pc-bios` folder; they are loaded by QEMU at start and are not changed. No separate licence text for these files came with the Espressif repository, and none has been asked for yet; they are carried unchanged as part of the QEMU tree they come in |

**Written offer (GPL-2.0, section 3).** For at least three years from the date of the kit, we will give anyone who asks a complete machine-readable copy of the corresponding source of the QEMU build in this kit, for a charge no more than the cost of physically doing the distribution. Ask in the issue tracker of the project repository named above. The kit already carries our changes in source form (`source/qemu-patches/`), and the base tree is public at the address and tag above.

QEMU is started as a separate program by the emulator application, and as a command by the launcher. The kit's other parts do not link with it.

## 2. Libraries that travel with QEMU

QEMU was built with MacPorts on macOS (x86_64). It needs these shared libraries; they are copied unchanged into `emulator/vendor/qemu/lib/` and the program is re-pointed at them (`install_name_tool`), so nothing is read from the package manager. They are loaded as separate files, so an LGPL library can be replaced by your own build with the same file name (the program looks for exactly these names next to it).

| Library | Version | Licence | Source |
|---|---|---|---|
| glib (libglib, libgobject, libgio, libgmodule) | 2.88.3 | LGPL-2.1-or-later | <https://download.gnome.org/sources/glib/> |
| libgcrypt | 1.12.4 | LGPL-2.1-or-later | <https://gnupg.org/ftp/gcrypt/libgcrypt/> |
| libgpg-error | 1.61 | LGPL-2.1-or-later | <https://gnupg.org/ftp/gcrypt/libgpg-error/> |
| libiconv | 1.19 | LGPL-2.1-or-later | <https://ftp.gnu.org/gnu/libiconv/> |
| libintl (from gettext-runtime) | 1.0 | LGPL-2.1-or-later | <https://ftp.gnu.org/gnu/gettext/> |
| pcre2 | 10.48 | BSD-3-Clause (with the PCRE2 exemption) | <https://github.com/PCRE2Project/pcre2/releases> |
| libffi | 3.4.8 | MIT | <https://github.com/libffi/libffi/releases> |
| pixman | 0.46.0 | MIT | <https://cairographics.org/releases/> |
| libslirp | 4.9.1 | BSD-3-Clause | <https://gitlab.freedesktop.org/slirp/libslirp> |
| zlib | 1.3.2 | zlib | <https://zlib.net/> |
| zstd | 1.5.7 | BSD-3-Clause (offered as BSD or GPL-2.0; used under BSD) | <https://github.com/facebook/zstd> |
| bzip2 | 1.0.8 | bzip2 (BSD-style) | <https://sourceware.org/bzip2/> |

The libraries are MacPorts builds of those releases; the patches MacPorts applies are in its ports tree, <https://github.com/macports/macports-ports>. The same written offer as in section 1 covers the source of the LGPL libraries: ask and we will give you the release tarball and the port files we built from. The licence texts are in `licenses/LIBRARIES.txt`.

## 3. The emulator application (Rust)

`emulator/esp32-emulator` (the window) and `emulator/headless_tap` (the same without a window, driven by a command file) come from the emulator project, <https://github.com/PastorCatto/ESP32-Emulator>, under **MIT OR Apache-2.0**. They are statically linked with these Rust crates, built from the project's `Cargo.lock`:

- `licenses/RUST-CRATES.txt` lists every crate, its version and the licence it declares; `licenses/RUST-LICENSES.txt` has the licence and notice texts that the crates carry.
- About 190 crates. The licences are MIT, Apache-2.0, BSD-2/3-Clause, ISC, Zlib, Unlicense, CC0-1.0, 0BSD and Unicode-3.0, mostly as "MIT OR Apache-2.0" choices. None is copyleft: the one crate with a GPL option (`Apache-2.0 OR GPL-2.0-only`) is used under Apache-2.0.
- The application embeds the fonts of the `epaint_default_fonts` crate (Hack, Ubuntu Light, Noto Emoji, emoji-icon-font), under the SIL Open Font Licence 1.1, the Ubuntu Font Licence 1.0, Apache-2.0 and MIT as their licence files in `RUST-LICENSES.txt` say.
- The Rust compiler and standard library are MIT OR Apache-2.0.

macOS system libraries and frameworks (libSystem, AppKit, CoreFoundation, OpenGL, vmnet and the like) are not part of the kit; they come with the computer.

## 4. Platform notes

- The emulator folder of the **Windows** kit is for 64-bit Windows 10 or 11; see section 5. The rest of this section is about the macOS kit.
- The emulator folder is for macOS on Intel (x86_64); QEMU and its libraries were built for macOS 26 or newer.
- Programs from an archive that was downloaded carry macOS's quarantine mark. If macOS refuses to start one, remove the mark from the kit folder once: `xattr -dr com.apple.quarantine <kit folder>`.

## 5. The Windows build

The Windows kit carries the same QEMU source (section 1) built with MSYS2's UCRT64 toolchain (gcc 16.2.0) on Windows 11 (build log and every mistake: the emulator project's `docs/NOTES-windows-qemu-build.md`), and the same Rust application (section 3) built with Rust 1.99.1 for the MSVC target. `emulator/SOURCE.txt` is the written offer for this build; the licence texts are in `emulator/licenses/` (`libraries/` holds the ones MSYS2 ships with each package).

The libraries are unmodified MSYS2 UCRT64 packages, in `emulator/vendor/qemu/bin/` next to `qemu-system-xtensa.exe` (Windows loads a DLL from the folder of the program). The licence column is what MSYS2 reports for the package, not a legal reading:

| DLL | MSYS2 package | Version | Licence |
|---|---|---|---|
| libbz2-1.dll | mingw-w64-ucrt-x86_64-bzip2 | 1.0.8-4 | bzip2 licence (BSD-style) |
| libffi-8.dll | mingw-w64-ucrt-x86_64-libffi | 3.8.0-1 | MIT |
| libgcrypt-20.dll | mingw-w64-ucrt-x86_64-libgcrypt | 1.12.4-2 | LGPL-2.1-or-later and GPL-2.0-or-later |
| libgio-2.0-0.dll, libglib-2.0-0.dll, libgmodule-2.0-0.dll, libgobject-2.0-0.dll | mingw-w64-ucrt-x86_64-glib2 | 2.90.1-1 | LGPL-2.1-or-later |
| libgpg-error-0.dll | mingw-w64-ucrt-x86_64-libgpg-error | 1.61-1 | LGPL-2.1-or-later |
| libiconv-2.dll | mingw-w64-ucrt-x86_64-libiconv | 1.19.1-1 | LGPL-2.1-or-later |
| libintl-8.dll | mingw-w64-ucrt-x86_64-gettext-runtime | 1.0-1 | GPL-3.0-or-later and LGPL-2.1-or-later (to be checked before a public release) |
| libpcre2-8-0.dll | mingw-w64-ucrt-x86_64-pcre2 | 10.49-2 | BSD-3-Clause |
| libpixman-1-0.dll | mingw-w64-ucrt-x86_64-pixman | 0.46.4-3 | MIT |
| libslirp-0.dll | mingw-w64-ucrt-x86_64-libslirp | 4.9.7-1 | BSD-3-Clause |
| libwinpthread-1.dll | mingw-w64-ucrt-x86_64-libwinpthread | 14.0.0.r598.ge3d18511f-1 | MIT and BSD-3-Clause |
| libz-1.dll, zlib1.dll | mingw-w64-ucrt-x86_64-zlib | 1.4-2 | zlib |
| libzstd.dll | mingw-w64-ucrt-x86_64-zstd | 1.5.7-2 | BSD-3-Clause or GPL-2.0-or-later (used under BSD) |

User-mode networking (libslirp) is on in this build; libslirp-0.dll is the shared library from MSYS2 (the MINGW64 environment has no libslirp package, which is why QEMU is built in UCRT64). The Windows programs are **not signed**: Windows's Smart App Control blocks them while it is on (turn it off in Windows Security), and SmartScreen may warn on first start. The Rust programs link the Microsoft C runtime dynamically, so a PC without the Visual C++ Redistributable may refuse to start them.
