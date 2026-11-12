"""Build the boot package (bootpkg/): the boot menu the bootloader loads into RAM.

It is compiled with the ESP-IDF cross compiler on its own (no ESP-IDF build system),
linked for the bootloader's RAM window, and wrapped in a PURR image container.
See bootloader/SPEC.md section 9.
"""

import glob
import os
import shutil
import struct

from lib import image as pimg
from lib.model import Action, Param, Script

PKG_DIR = "bootpkg"
# board -> (compiler prefix, chip id, linker script). Chip ids are esp_chip_id_t values.
BOARDS = {
    "tdeck": ("xtensa-esp32s3-elf", 9, "esp32s3.ld"),
    "tdeck_plus": ("xtensa-esp32s3-elf", 9, "esp32s3.ld"),
    "waveshare154": ("xtensa-esp32s3-elf", 9, "esp32s3.ld"),
    "playtime": ("xtensa-esp32s3-elf", 9, "esp32s3.ld"),
}
# Which pkg_hw_*.c backs a board's display/input -- the one real per-board source file
# (the rest of sources() below is shared). "tdeck" reuses tdeck_plus's pkg_hw.c outright:
# identical pins, nothing here talks to the one hardware difference (GPS) either. "BlackDeck"/
# "BlackDeck+"/"InkyTuxedo" are this project's own codenames for these builds (pkg_main.c's
# draw_header()), not upstream terms -- picked so a device on the bench is identifiable by
# sight/log line, same reason the recovery releases this repo publishes are named
# (JumpingJaguar1, ...).
HW_SOURCE = {"tdeck": "pkg_hw.c", "tdeck_plus": "pkg_hw.c", "waveshare154": "pkg_hw_epd.c", "playtime": "pkg_hw.c"}
VERSION = "0.3.1"

# Re-exported under their original names: purrstrap/tests/test_bootpkg.py reads these.
HEADER_FORMAT = pimg.HEADER_FORMAT
HEADER_SIZE = pimg.HEADER_SIZE
PREAMBLE_FORMAT = "<IIII"          # purr_pkg_preamble_t
TEXT_MAX = DATA_MAX = 0x8000


def pack_preamble(text_size, data_size, bss_size, entry):
    return struct.pack(PREAMBLE_FORMAT, text_size, data_size, bss_size, entry)


def make_image(text, data, bss_size, entry, chip_id, version=VERSION):
    """A boot package image: header (unsigned), preamble, code, data."""
    # The bootloader's hardware SHA only takes whole words, so the payload is a multiple of 4.
    text = text + b"\0" * (-len(text) % 4)
    data = data + b"\0" * (-len(data) % 4)
    if len(text) > TEXT_MAX or len(data) + bss_size > DATA_MAX:
        raise ValueError(f"package too big for its window: text {len(text)} of "
                         f"{TEXT_MAX}, data+bss {len(data) + bss_size} of {DATA_MAX}")
    payload = pack_preamble(len(text), len(data), bss_size, entry) + text + data
    return pimg.make_image("bootpkg", version, pimg.IMG_MODULE, pimg.MOD_BOOTPKG, payload, chip_id)


# -- tools -------------------------------------------------------------
def _lower_exe(path):
    """shutil.which returns .EXE on Windows; ESP-IDF's compiler drivers pick their chip configuration from their own file name and
    fall back to the default (big-endian) one when it is spelled .EXE. Same helper as in modules.py."""
    if os.name == "nt" and path.lower().endswith(".exe") and not path.endswith(".exe"):
        return path[:-4] + ".exe"
    return path


def find_tool(prefix, name, env=None):
    """Find prefix-name in PATH or in the usual ESP-IDF tools folders."""
    exe = f"{prefix}-{name}"
    found = shutil.which(exe)
    if found:
        return _lower_exe(found)
    roots = [os.environ.get("IDF_TOOLS_PATH", ""), r"C:\Espressif",
             os.path.expanduser("~/.espressif")]
    for root in roots:
        if not root:
            continue
        pattern = os.path.join(root, "tools", "xtensa-esp-elf", "*", "xtensa-esp-elf",
                               "bin", exe + (".exe" if os.name == "nt" else ""))
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[-1]
    return None


def compile_args(repo_root, board, brand=""):
    brand_flag = [f'-DPURR_BRAND_HEADER="{brand}"'] if brand else []
    return brand_flag + ["-Os", "-std=gnu11", f'-DPKG_VERSION="{VERSION}"', "-ffreestanding", "-fno-builtin", "-mlongcalls",
            "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror",
            "-I", os.path.join(repo_root, PKG_DIR, "src"),
            "-I", os.path.join(repo_root, PKG_DIR, "boards"),
            "-I", os.path.join(repo_root, "PurrOS", "components", "coreos", "include"),
            "-I", os.path.join(repo_root, "PurrOS", "components", "kernel", "include")]


def sources(repo_root, board):
    return [os.path.join(repo_root, PKG_DIR, "src", "pkg_main.c"),
            os.path.join(repo_root, PKG_DIR, "src", "pkg_ui.c"),
            os.path.join(repo_root, PKG_DIR, "src", HW_SOURCE[board]),
            os.path.join(repo_root, "PurrOS", "components", "coreos", "src", "purr_menu.c"),
            os.path.join(repo_root, "PurrOS", "components", "kernel", "src",
                         "purr_font_data.c")]


def board_header(repo_root, board):
    """The compiler sees the board's header as board.h."""
    return os.path.join(repo_root, PKG_DIR, "boards", f"{board}.h")


def _symbols(nm_output):
    table = {}
    for line in nm_output.splitlines():
        parts = line.split()
        if len(parts) == 3:
            table[parts[2]] = int(parts[0], 16)
    return table


def build(ctx, board, brand="", out=""):
    if board not in BOARDS:
        ctx.error(f"unknown board {board}")
        return 1
    prefix, chip_id, ldname = BOARDS[board]
    gcc, objcopy, nm = (find_tool(prefix, t) for t in ("gcc", "objcopy", "nm"))
    if not (gcc and objcopy and nm):
        ctx.error(f"{prefix} tools not found. Install ESP-IDF, or put them on PATH.")
        return 1
    root = ctx.repo_root
    brand = os.path.abspath(brand).replace(os.sep, "/") if brand else ""
    if brand and not os.path.isfile(brand):
        ctx.error(f"{brand}: no such brand header. Make one with `purr brand build brand.toml`.")
        return 1
    out = os.path.abspath(out) if out else os.path.join(root, PKG_DIR, "build", board)
    os.makedirs(out, exist_ok=True)

    # The board header is included as "board.h".
    shim = os.path.join(out, "board.h")
    with open(shim, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(f'#include "{board_header(root, board).replace(os.sep, "/")}"\n')

    objs = []
    for src in sources(root, board):
        obj = os.path.join(out, os.path.basename(src)[:-2] + ".o")
        code = ctx.run([gcc] + compile_args(root, board, brand) + ["-I", out, "-c", src, "-o", obj])
        if code != 0:
            ctx.error(f"compile failed: {os.path.basename(src)}")
            return code
        objs.append(obj)

    elf = os.path.join(out, "bootpkg.elf")
    code = ctx.run([gcc, "-nostdlib", "-Wl,--gc-sections",
                    "-T", os.path.join(root, PKG_DIR, "link", ldname),
                    f"-Wl,-Map={os.path.join(out, 'bootpkg.map')}"] + objs + ["-o", elf])
    if code != 0:
        ctx.error("link failed")
        return code

    text_bin, data_bin = os.path.join(out, "text.bin"), os.path.join(out, "data.bin")
    for sect, dest in ((".text", text_bin), (".data", data_bin)):
        code = ctx.run([objcopy, "-O", "binary", "-j", sect, elf, dest])
        if code != 0:
            ctx.error(f"objcopy failed for {sect}")
            return code

    listing = os.path.join(out, "symbols.txt")
    code = ctx.run([nm, elf], log_path=listing)
    if code != 0:
        return code
    with open(listing, encoding="utf-8", errors="replace") as fh:
        syms = _symbols(fh.read())
    if not {"purr_pkg_entry", "__bss_start", "__bss_end"} <= set(syms):
        ctx.error("the package has no purr_pkg_entry or bss markers")
        return 1

    with open(text_bin, "rb") as fh:
        text = fh.read()
    with open(data_bin, "rb") as fh:
        data = fh.read()
    bss = syms["__bss_end"] - syms["__bss_start"]
    try:
        image = make_image(text, data, bss, syms["purr_pkg_entry"], chip_id)
    except ValueError as e:
        ctx.error(str(e))
        return 1
    dest = os.path.join(out, "bootpkg.bin")
    with open(dest, "wb") as fh:
        fh.write(image)
    ctx.ok(f"{dest}: {len(image)} bytes (code {len(text)}, data {len(data)}, bss {bss})")
    ctx.info("unsigned: only accepted while secure mode is off")
    return 0


SCRIPT = Script(
    name="bootpkg",
    title="Boot package",
    description="Build the boot package (the boot menu the bootloader loads into RAM).",
    actions=(
        Action("build", "Build", build, help="compile, link and pack one board's package",
               params=(Param("board", "choice", default="tdeck_plus", choices=tuple(BOARDS),
                             help="the board to build for"),
                      Param("brand", "path", default="", help="a purr_brand.h from `purr brand build`: the flavor's names and colours"),
                      Param("out", "path", default="", help="build in this folder instead of bootpkg/build/<board>"))),
    ),
)
