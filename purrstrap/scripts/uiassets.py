"""Packs images into a UI pack file, `pack.bin`: the shell's pictures by name (flash map v2, plan1.md:
`/ui/<shell>/pack.bin` and `/ui/themes/<id>/pack.bin` in LittleFS; until then purr_ui_assets.c read the
same bytes from a raw `ui_assets` partition, which the new partition table no longer has).
PurrOS/main/purr_ui_config.h's "image" widgets reference the pictures by name.

Takes a directory of PNG files (filename without extension = the asset name
purr_ui_config.c's `asset = "name"` property looks up), decodes each with a small pure-Python
PNG reader (this project has never taken a Pillow/PIL dependency -- purrstrap is stdlib
only), converts to RGB565 (purr_display.h's PURR_RGB565 macro, byte for byte), and packs
them into one binary: a 12-byte header, a directory of 40-byte entries, then the raw pixel
data. That exact layout was verified against the real xtensa-esp32s3-elf cross-compiler
(_Static_assert on sizeof/offsetof) before this packer was written to match it, not the
other way around -- the netrec record_t struct earlier in this project got bitten by
exactly the mistake that check was for (an unaccounted-for compiler padding byte).

The file itself is not signed (OTA/SPEC.md's reasoning for every other plain-data file: not executable
code); `package` wraps it for the release, where the system key signs it -- just a CRC32 (zlib's, byte-for-byte what purr_crc32 computes) over the directory and
pixel data, checked once at init by purr_ui_assets_init() so a corrupt or partial write (an
interrupted netinstall, say) is read as "no assets" rather than garbage.

Only 8-bit-per-channel, non-interlaced, true-color RGB or RGBA PNGs are supported -- anything
else (palette, grayscale, 16-bit, interlaced) is refused with a clear error rather than
silently mishandled.
"""

import os
import re
import struct
import zlib

from lib import image as pimg
from lib.model import Action, Param, Script

MAGIC = 0x54534150        # 'PAST', little-endian: "Purr ASset Table"
HEADER_FMT = "<III"                       # magic, count, crc32
ENTRY_FMT = "<24sIIHHB3x"                 # name, offset, length, width, height, color_format
CF_RGB565 = 0                             # w*h*2 bytes
CF_RGB565A8 = 1                           # an RGB565 plane (w*h*2) then an 8-bit alpha plane (w*h)
ALIGN = 4                                 # every entry's data starts on a 4-byte boundary
ENTRY_SIZE = struct.calcsize(ENTRY_FMT)
BOARDS = ("tdeck", "tdeck_plus")          # the only boards that build LVGL in at all
MAX_PACK_BYTES = 5 * 1024 * 1024          # a pack is a file in LittleFS now: the cap the release tools and `purr ui check` apply
PACK_COMPONENTS = ("uiassets", "ui_w10m", "ui_android")   # the names a packed pack can carry; the last two are the per-shell packs, `uiassets` the old single one


# -- a small, pure-Python PNG decoder (8bpc RGB/RGBA, non-interlaced only) -------------

def _unfilter(raw, width, height, channels):
    stride = width * channels
    out = bytearray(height * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        filt = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if filt == 1:                     # Sub
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + a) & 0xFF
        elif filt == 2:                   # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filt == 3:                   # Average
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + (a + prev[i]) // 2) & 0xFF
        elif filt == 4:                   # Paeth
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif filt != 0:                   # 0 = None, nothing to undo
            raise ValueError(f"unknown PNG filter type {filt}")
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return bytes(out)


def decode_png(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")

    width = height = bit_depth = color_type = interlace = None
    idat = b""
    pos = 8
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 8 + length + 4          # +4 skips the chunk's trailing CRC
        if ctype == b"IHDR":
            width, height, bit_depth, color_type, _, _, interlace = \
                struct.unpack(">IIBBBBB", chunk)
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
    if width is None:
        raise ValueError(f"{path}: no IHDR chunk")
    if interlace != 0:
        raise ValueError(f"{path}: interlaced PNGs aren't supported")
    if bit_depth != 8:
        raise ValueError(f"{path}: only 8-bit-per-channel PNGs are supported (got {bit_depth})")
    if color_type == 2:
        channels = 3
    elif color_type == 6:
        channels = 4
    else:
        raise ValueError(f"{path}: only true-color RGB/RGBA PNGs are supported "
                         f"(color type {color_type}, not palette/grayscale)")

    pixels = _unfilter(zlib.decompress(idat), width, height, channels)
    return width, height, channels, pixels


def to_rgb565(width, height, channels, pixels):
    """purr_display.h's PURR_RGB565(r,g,b) macro, byte for byte, as a little-endian
    (host-native on ESP32) uint16 per pixel -- what purr_display_v2_t::blit() and this
    project's own RGB565 convention everywhere else already expect."""
    out = bytearray(width * height * 2)
    for i in range(width * height):
        r = pixels[i * channels]
        g = pixels[i * channels + 1]
        b = pixels[i * channels + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3)
        out[i * 2] = v & 0xFF
        out[i * 2 + 1] = (v >> 8) & 0xFF
    return bytes(out)


def to_rgb565a8(width, height, channels, pixels):
    """An RGB565 plane followed by an 8-bit alpha plane (LVGL's RGB565A8). A picture without
    alpha (3 channels) gets a fully opaque plane, so every image in a pack can be the same format."""
    plane = to_rgb565(width, height, channels, pixels)
    if channels == 4:
        alpha = bytes(pixels[i * 4 + 3] for i in range(width * height))
    else:
        alpha = b"\xff" * (width * height)
    return plane + alpha


# -- packing -------------------------------------------------------------------------

def pack(images):
    """images: a list of (name, width, height, data) or (name, width, height, data, color_format).
    The data is RGB565 (CF_RGB565, the default) or an RGB565 plane followed by an alpha plane
    (CF_RGB565A8). Returns the packed bytes. Each entry's data starts on a 4-byte boundary, so
    the reader can hand a pointer straight to LVGL."""
    dir_rows = []
    pixel_blob = bytearray()
    for item in images:
        name, w, h, data = item[:4]
        cf = item[4] if len(item) > 4 else CF_RGB565
        name_b = name.encode("utf-8")
        if len(name_b) >= 24:
            raise ValueError(f"asset name too long for 24 bytes: {name!r}")
        want = w * h * (3 if cf == CF_RGB565A8 else 2)
        if cf not in (CF_RGB565, CF_RGB565A8) or len(data) != want:
            raise ValueError(f"{name}: {len(data)} bytes does not match {w}x{h} at color format {cf}")
        while len(pixel_blob) % ALIGN:
            pixel_blob.append(0xFF)
        dir_rows.append(struct.pack(ENTRY_FMT, name_b, len(pixel_blob), len(data), w, h, cf))
        pixel_blob += data

    body = b"".join(dir_rows) + bytes(pixel_blob)
    crc = zlib.crc32(body) & 0xFFFFFFFF
    header = struct.pack(HEADER_FMT, MAGIC, len(images), crc)
    return header + body


# -- importing SquareLine / LVGL image arrays (e.g. fbiego/windows-10-mobile-lvgl) ------------

_ARRAY_RE = re.compile(r"ui_img_(\w+?)_png_data\[\]\s*=\s*\{(.*?)\};", re.S)
_DSC_RE = re.compile(r"ui_img_(\w+?)_png\s*=\s*\{(.*?)\};", re.S)


def parse_lvgl_array(path):
    """Reads one `ui_img_<name>_png.c` exported by SquareLine Studio for LVGL 9 and returns
    (name, width, height, data, color_format). The data is used exactly as exported (no
    resampling, no recolouring): LVGL's native RGB565, or RGB565 with a separate alpha plane."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    arr = _ARRAY_RE.search(text)
    dsc = None
    for m in _DSC_RE.finditer(text):
        if "header.w" in m.group(2):
            dsc = m
    if arr is None or dsc is None:
        raise ValueError(f"{path}: not a SquareLine image array")
    name = arr.group(1)
    body = dsc.group(2)
    w = int(re.search(r"\.header\.w\s*=\s*(\d+)", body).group(1))
    h = int(re.search(r"\.header\.h\s*=\s*(\d+)", body).group(1))
    cf_name = re.search(r"\.header\.cf\s*=\s*(\w+)", body).group(1)
    if cf_name == "LV_COLOR_FORMAT_NATIVE":
        cf = CF_RGB565
    elif cf_name == "LV_COLOR_FORMAT_NATIVE_WITH_ALPHA":
        cf = CF_RGB565A8
    else:
        raise ValueError(f"{path}: color format {cf_name} is not supported "
                         "(only 16-bit native, with or without alpha)")
    data = bytes(int(b, 16) for b in re.findall(r"0[xX][0-9A-Fa-f]{1,2}", arr.group(2)))
    return name, w, h, data, cf


def _write_pack(ctx, images, out, what):
    """Packs, applies the 5 MB cap, writes the file (no padding: it is a file in LittleFS, not a partition)."""
    image = pack(images)
    if len(image) > MAX_PACK_BYTES:
        ctx.error(f"packed {what} are {len(image)} bytes, more than the {MAX_PACK_BYTES}-byte cap "
                  f"({len(image) - MAX_PACK_BYTES} over)")
        return 1
    dest = out or os.path.join(ctx.repo_root, "rel", "ui", "pack.bin")
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    with open(dest, "wb") as fh:
        fh.write(image)
    ctx.ok(f"{dest}: {len(images)} asset(s), {len(image)} bytes")
    return 0


def import_lvgl(ctx, board, src, out, skip):
    if board not in BOARDS:
        ctx.error(f"unknown board {board!r}")
        return 1
    if not os.path.isdir(src):
        ctx.error(f"{src}: no such directory")
        return 1
    skipped = {s.strip() for s in skip.split(",") if s.strip()}
    images = []
    for fname in sorted(os.listdir(src)):
        if not (fname.startswith("ui_img_") and fname.endswith("_png.c")):
            continue
        try:
            item = parse_lvgl_array(os.path.join(src, fname))
        except (ValueError, AttributeError) as e:
            ctx.error(f"{fname}: {e}")
            return 1
        if item[0] in skipped:
            continue
        images.append(item)
    if not images:
        ctx.error(f"{src}: no ui_img_*_png.c files")
        return 1
    return _write_pack(ctx, images, out, "assets")


def build(ctx, board, assets_dir, out, alpha=False):
    """A folder of PNGs to a pack file. `alpha` decides for the whole folder: RGB565A8 for every picture (an RGB one gets an opaque
    plane), or plain RGB565 for every picture."""
    if board not in BOARDS:
        ctx.error(f"unknown board {board!r} (LVGL only builds for {', '.join(BOARDS)})")
        return 1
    if not os.path.isdir(assets_dir):
        ctx.error(f"{assets_dir}: no such directory")
        return 1

    pngs = sorted(f for f in os.listdir(assets_dir) if f.lower().endswith(".png"))
    if not pngs:
        ctx.error(f"{assets_dir}: no .png files")
        return 1

    images = []
    for fname in pngs:
        name = os.path.splitext(fname)[0]
        path = os.path.join(assets_dir, fname)
        try:
            w, h, channels, pixels = decode_png(path)
        except ValueError as e:
            ctx.error(str(e))
            return 1
        if alpha:
            images.append((name, w, h, to_rgb565a8(w, h, channels, pixels), CF_RGB565A8))
            ctx.info(f"{fname}: {w}x{h}, {channels} channel(s) -> {w * h * 3} bytes RGB565A8")
        else:
            images.append((name, w, h, to_rgb565(w, h, channels, pixels)))
            ctx.info(f"{fname}: {w}x{h}, {channels} channel(s) -> {w * h * 2} bytes RGB565")
    return _write_pack(ctx, images, out, "assets")


def used_length(data):
    """How many bytes of a packed pack are real: the header, the directory and the furthest pixel range, rounded up to ALIGN.
    A built .bin is padded with erased-flash 0xFF to the whole partition; only this much is signed and sent."""
    if len(data) < struct.calcsize(HEADER_FMT):
        raise ValueError("too short to be an asset pack")
    magic, count, _crc = struct.unpack_from(HEADER_FMT, data, 0)
    if magic != MAGIC:
        raise ValueError("not an asset pack (bad magic)")
    head = struct.calcsize(HEADER_FMT) + count * ENTRY_SIZE
    if len(data) < head:
        raise ValueError("the directory runs past the end of the file")
    end = 0
    for i in range(count):
        _name, off, length, _w, _h, _cf = struct.unpack_from(ENTRY_FMT, data, struct.calcsize(HEADER_FMT) + i * ENTRY_SIZE)
        end = max(end, off + length)
    total = head + end
    if total > len(data):
        raise ValueError("an entry's pixels run past the end of the file")
    return (total + ALIGN - 1) // ALIGN * ALIGN


def package(ctx, board, file, version, out, name="uiassets"):
    """Wraps a built pack into the unsigned PURR_IMG_MODULE container (subtype PURR_MOD_UIASSETS) that stage 2 installs; `name`
    is the component: `ui_w10m` or `ui_android` for the per-shell packs (flash map v2), `uiassets` for the old single pack.
    Sign it with `keys sign` (the release's system key) before publishing."""
    if board not in BOARDS:
        ctx.error(f"unknown board {board!r} (LVGL only builds for {', '.join(BOARDS)})")
        return 1
    if name not in PACK_COMPONENTS:
        ctx.error(f"unknown component {name!r}: one of {', '.join(PACK_COMPONENTS)}")
        return 1
    if not os.path.isfile(file):
        ctx.error(f"{file}: no such file")
        return 1
    with open(file, "rb") as fh:
        data = fh.read()
    try:
        n = used_length(data)
    except ValueError as e:
        ctx.error(f"{file}: {e}")
        return 1
    if n > MAX_PACK_BYTES:
        ctx.error(f"{file}: the pack is {n} bytes, more than the {MAX_PACK_BYTES}-byte cap")
        return 1
    image = pimg.make_image(name, version, pimg.IMG_MODULE, pimg.MOD_UIASSETS, data[:n], pimg.CHIP_ESP32S3)
    dest = out or f"{name}-{board}.cat"
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    with open(dest, "wb") as fh:
        fh.write(image)
    ctx.ok(f"{dest}: {len(image)} bytes ({name} {version}, {n} of {len(data)} bytes of the pack, unsigned)")
    ctx.info(f"sign it: purrstrap keys sign --key <system key> --image {dest} --key-id <id>")
    return 0


SCRIPT = Script(
    name="uiassets",
    title="UI asset pack",
    description="Pack PNG images into a UI pack file (pack.bin) the LVGL UI's pictures are read from by name, "
                "and wrap a pack as a release component (ui_w10m, ui_android).",
    actions=(
        Action("import-lvgl", "Import LVGL arrays", import_lvgl,
               help="pack SquareLine/LVGL image arrays (ui_img_*_png.c) as they are, alpha included",
               params=(
                   Param("board", "choice", default="tdeck_plus", choices=BOARDS,
                         help="the board"),
                   Param("src", "path", required=True,
                         help="directory holding the ui_img_<name>_png.c files"),
                   Param("out", "path", default="",
                         help="output file (default: rel/ui/pack.bin)"),
                   Param("skip", "str", default="",
                         help="asset names to leave out, comma separated"),
               )),
        Action("build", "Build", build,
               help="decode a directory of PNGs and pack them into a pack file (at most 5 MB)",
               params=(
                   Param("board", "choice", default="tdeck_plus", choices=BOARDS,
                         help="the board"),
                   Param("assets_dir", "path", required=True,
                         help="directory of .png files; each file name (without .png) "
                              "becomes the asset name"),
                   Param("out", "path", default="",
                         help="output file (default: rel/ui/pack.bin)"),
                   Param("alpha", "bool", default=False,
                         help="keep each PNG's alpha channel (RGB565A8, 3 bytes a pixel) instead "
                              "of flattening to opaque RGB565; the Android shell's icons need it"),
               )),
        Action("package", "Package for the recovery server", package,
               help="wrap a built pack in a PURR image (subtype uiassets) that stage 2 installs; sign it afterwards",
               params=(
                   Param("board", "choice", default="tdeck_plus", choices=BOARDS,
                         help="the board the pack is for"),
                   Param("file", "path", required=True,
                         help="the pack built by `uiassets build`"),
                   Param("version", "str", required=True,
                         help="the pack's version, for example 1.0.29"),
                   Param("out", "path", default="",
                         help="output file (default: <name>-<board>.cat)"),
                   Param("name", "choice", default="uiassets", choices=PACK_COMPONENTS,
                         help="the component: ui_w10m or ui_android (a shell's own pack), or uiassets (the old single pack)"),
               )),
    ),
)
