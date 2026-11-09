"""Game packages: emulators and games as PURR application packages, portable across boards.

A game is a normal PURR application package (a signed `.cat`, PURR_IMG_APP, installed and updated like any app, through the Store or the
dev server) whose manifest says `kind = "game"`. It names the systems it plays and the file extensions it opens, so a front end can list
games without knowing which core plays them, and it says what it needs (buttons, audio, screen size) so a device can tell whether it fits.

The payload comes in two kinds:
  payload = "image"   a complete ESP-IDF application image (Retro-Go's cores are built this way today), built for one board. Wrapped as
                      PGAM (below) and marked with the app flag APPF_GAME_IMAGE so nothing mistakes it for a relocatable module.
  payload = "module"  a relocatable module that uses the system's game interfaces: one package for every device (not built yet).

PGAM v1, the payload of an image package:
  4s  magic "PGAM"       <H  format = 1      <H  reserved = 0
  <I  meta_len           <I  image_len
  meta_len bytes of UTF-8 JSON (the manifest, flattened: id, version, systems, extensions, needs, board, chip, ...), padded to 4 bytes
  image_len bytes of application image

`target` writes a Retro-Go target for a board from the board file, so a new device is a board file, not a hand-written port.
"""

import hashlib
import json
import os
import re
import shutil
import struct

from lib import image as pimg
from lib.model import Action, Param, Script

PGAM_MAGIC = b"PGAM"
PGAM_FORMAT = 1
APPF_GAME_IMAGE = 0x10               # the app subtype flag (PURR_FLAGS_SUBTYPE) of an image package
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,30}$")
VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
EXT_RE = re.compile(r"^\.[a-z0-9]{1,6}$")
BUTTONS = ("up", "down", "left", "right", "a", "b", "x", "y", "l", "r", "start", "select", "option", "menu")
CHIPS = {"esp32s3": pimg.CHIP_ESP32S3, "esp32": pimg.CHIP_ESP32}


def _toml(path):
    try:
        import tomllib
    except ImportError:
        raise SystemExit("games: needs Python 3.11 or newer (tomllib). The developer kit's bundled Python has it.")
    with open(path, "rb") as fh:
        return tomllib.load(fh)


# ----------------------------------------------------------------------------- the manifest

def check_manifest(m):
    """Returns a list of problems with a game manifest (empty = fine)."""
    bad = []
    if not NAME_RE.match(str(m.get("name", ""))):
        bad.append("name: letters, digits, '-' and '_' only, 1 to 31 characters")
    if not VERSION_RE.match(str(m.get("version", ""))):
        bad.append("version: major.minor.patch, like 1.0.0")
    if m.get("kind") != "game":
        bad.append('kind must be "game"')
    if m.get("payload") not in ("image", "module"):
        bad.append('payload must be "image" or "module"')
    g = m.get("game")
    if not isinstance(g, dict):
        return bad + ["missing [game] section"]
    systems = g.get("systems")
    if not isinstance(systems, list) or not all(isinstance(s, str) and re.match(r"^[a-z0-9_-]{1,12}$", s) for s in systems):
        bad.append("[game] systems: a list of short lowercase names, like [\"nes\", \"gb\"] (a launcher plays none: use [])")
    for e in g.get("extensions", []):
        if not EXT_RE.match(str(e)):
            bad.append("[game] extensions: %r is not like \".nes\"" % (e,))
    needs = g.get("needs", {})
    for b in needs.get("buttons", []):
        if b not in BUTTONS:
            bad.append("[game.needs] buttons: %r is not one of %s" % (b, ", ".join(BUTTONS)))
    dm = needs.get("display_min")
    if dm is not None and not (isinstance(dm, list) and len(dm) == 2 and all(isinstance(x, int) and x > 0 for x in dm)):
        bad.append("[game.needs] display_min: [width, height]")
    if m.get("payload") == "image":
        if not g.get("image"):
            bad.append("[game] image: the application image file (relative to the manifest)")
        if m.get("board") is None and g.get("board") is None:
            bad.append("an image package is built for one board: set board")
    return bad


def flatten(m):
    """The metadata carried inside the package (and read by a front end without unpacking the image)."""
    g = m.get("game", {})
    return {
        "id": m["name"], "title": m.get("display", {}).get("name", m["name"]), "version": m["version"],
        "description": m.get("description", ""), "payload": m["payload"],
        "board": m.get("board") or g.get("board", ""), "chip": m.get("chip", "esp32s3"),
        "systems": g.get("systems", []), "extensions": g.get("extensions", []), "needs": g.get("needs", {}),
        "entry": g.get("entry", m["name"]), "licence": m.get("licence", ""), "source": m.get("source", ""),
    }


# ----------------------------------------------------------------------------- the package

def pack_payload(meta, image_bytes):
    blob = json.dumps(meta, sort_keys=True, separators=(",", ":")).encode()
    pad = (-len(blob)) % 4
    head = struct.pack("<4sHHII", PGAM_MAGIC, PGAM_FORMAT, 0, len(blob) + pad, len(image_bytes))
    return head + blob + b"\0" * pad + image_bytes


def unpack_payload(payload):
    """(meta dict, image bytes), or raises ValueError."""
    if len(payload) < 16 or payload[:4] != PGAM_MAGIC:
        raise ValueError("not a PGAM payload")
    magic, fmt, _res, meta_len, image_len = struct.unpack_from("<4sHHII", payload, 0)
    if fmt != PGAM_FORMAT or 16 + meta_len + image_len != len(payload):
        raise ValueError("bad PGAM sizes (format %d)" % fmt)
    meta = json.loads(payload[16:16 + meta_len].decode().rstrip("\0"))
    return meta, payload[16 + meta_len:]


def package(ctx, manifest, out):
    mpath = os.path.abspath(manifest)
    m = _toml(mpath)
    problems = check_manifest(m)
    if problems:
        for p in problems:
            ctx.error("%s: %s" % (manifest, p))
        return 1
    if m["payload"] != "image":
        ctx.error("payload \"module\" packages are built by the module pipeline, not here (not available yet)")
        return 1
    meta = flatten(m)
    ipath = os.path.join(os.path.dirname(mpath), m["game"]["image"])
    if not os.path.isfile(ipath):
        ctx.error("%s: no such image file" % ipath)
        return 1
    with open(ipath, "rb") as fh:
        image = fh.read()
    if image[:1] != b"\xE9":
        ctx.error("%s does not look like an ESP-IDF application image (first byte 0x%02X, expected 0xE9)" % (ipath, image[:1][0] if image else 0))
        return 1
    chip = CHIPS.get(meta["chip"])
    if chip is None:
        ctx.error("chip %r is not one of %s" % (meta["chip"], ", ".join(CHIPS)))
        return 1
    meta["image_sha256"] = hashlib.sha256(image).hexdigest()
    meta["image_size"] = len(image)
    data = pimg.make_image(m["name"], m["version"], pimg.IMG_APP, APPF_GAME_IMAGE, pack_payload(meta, image), chip)
    out = out or os.path.join(os.path.dirname(mpath), "%s-%s.cat" % (m["name"], meta["board"]))
    with open(out, "wb") as fh:
        fh.write(data)
    ctx.ok("%s: %d bytes (%s %s for %s, image %d bytes, unsigned)" % (out, len(data), m["name"], m["version"], meta["board"], len(image)))
    ctx.info("sign it: purrstrap keys sign --key <role>.key --image %s --key-id <id>" % out)
    return 0


def inspect(ctx, package_file):
    with open(package_file, "rb") as fh:
        data = fh.read()
    if not pimg.is_purr_image(data):
        ctx.error("not a PURR image")
        return 1
    f = pimg.header_fields(pimg.parse_header(data))
    if f["image_type"] != pimg.IMG_APP or (f["flags"] & 0xFF) != APPF_GAME_IMAGE:
        ctx.error("a PURR image, but not a game image package (type %s, flags 0x%X)" % (f["image_type"], f["flags"]))
        return 1
    try:
        meta, image = unpack_payload(data[pimg.HEADER_SIZE:pimg.HEADER_SIZE + f["payload_size"]])
    except ValueError as exc:
        ctx.error(str(exc))
        return 1
    ctx.info("%s %s (%s), %d-byte image for %s, key id %s" % (meta["id"], meta["version"], meta["title"], len(image), meta["board"], f["key_id"]))
    ctx.info("plays: %s   opens: %s" % (", ".join(meta["systems"]) or "-", " ".join(meta["extensions"]) or "-"))
    ctx.info("needs: %s" % json.dumps(meta["needs"], sort_keys=True))
    if hashlib.sha256(image).hexdigest() != meta.get("image_sha256"):
        ctx.error("the image inside does not match its recorded hash")
        return 1
    ctx.ok("package is consistent")
    return 0


# ----------------------------------------------------------------------------- the target generator

ILI9341_INIT = """\
#define RG_SCREEN_INIT()                                                                                         \\
    ILI9341_CMD(0xCF, 0x00, 0xc3, 0x30);                                                                         \\
    ILI9341_CMD(0xED, 0x64, 0x03, 0x12, 0x81);                                                                   \\
    ILI9341_CMD(0xE8, 0x85, 0x00, 0x78);                                                                         \\
    ILI9341_CMD(0xCB, 0x39, 0x2c, 0x00, 0x34, 0x02);                                                             \\
    ILI9341_CMD(0xF7, 0x20);                                                                                     \\
    ILI9341_CMD(0xEA, 0x00, 0x00);                                                                               \\
    ILI9341_CMD(0xC0, 0x1B);                 /* Power control   //VRH[5:0] */                                    \\
    ILI9341_CMD(0xC1, 0x12);                 /* Power control   //SAP[2:0];BT[3:0] */                            \\
    ILI9341_CMD(0xC5, 0x32, 0x3C);           /* VCM control */                                                   \\
    ILI9341_CMD(0xC7, 0x91);                 /* VCM control2 */                                                  \\
    ILI9341_CMD(0x36, 0x%02X);                 /* Memory Access Control */                                         \\
    ILI9341_CMD(0xB1, 0x00, 0x10);           /* Frame Rate Control (1B=70, 1F=61, 10=119) */                     \\
    ILI9341_CMD(0xB6, 0x0A, 0xA2);           /* Display Function Control */                                      \\
    ILI9341_CMD(0xF6, 0x01, 0x30);                                                                               \\
    ILI9341_CMD(0xF2, 0x00);                 /* 3Gamma Function Disable */                                       \\
    ILI9341_CMD(0x26, 0x01);                 /* Gamma curve selected */                                          \\
    ILI9341_CMD(0xE0, 0xD0, 0x00, 0x02, 0x07, 0x0a, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0e, 0x12, 0x14, 0x17);       \\
    ILI9341_CMD(0xE1, 0xD0, 0x00, 0x02, 0x07, 0x0a, 0x28, 0x31, 0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1b, 0x1e);
"""

# the S3's ADC1 channel for a GPIO: channel n is GPIO n+1
def _adc1_channel(pin):
    return pin - 1


def _device(board, compat):
    return next((d for d in board.get("device", []) if d.get("compatible") == compat), None)


def generate_config(board):
    """The text of a Retro-Go target `config.h` for a board file (a dict from the TOML). Raises ValueError with what is missing."""
    g = board.get("game")
    if g is None:
        raise ValueError("the board file has no [game] section")
    spi, disp = board.get("spi"), board.get("display")
    sd, spk, bat = _device(board, "sd-spi"), _device(board, "i2s-out"), _device(board, "battery-adc")
    for what, v in (("[spi]", spi), ("[display]", disp), ("a sd-spi device", sd)):
        if v is None:
            raise ValueError("the board file has no " + what)
    b = board.get("board", {})
    kb = board.get("keyboard", {})
    host = "SPI%d_HOST" % (spi.get("host", 1) + 1)
    L = ["// Generated by `purrstrap games target` from the board file (%s). Do not edit: change the board file and generate again." % b.get("id", "?"),
         "// Target definition", '#define RG_TARGET_NAME             "%s"' % g.get("target_name", b.get("id", "BOARD").upper().replace("_", "-")),
         "// Storage", '#define RG_STORAGE_ROOT             "/sd"', "#define RG_STORAGE_SDSPI_HOST       %s" % host,
         "#define RG_STORAGE_SDSPI_SPEED      SDMMC_FREQ_DEFAULT", "// GPIO Extender"]
    kind = g.get("input", "gpio")
    if kind == "i2c-keyboard-raw":
        L += ["// A key matrix read over I2C in raw mode: Retro-Go's keyboard code (patched, see register_target) is enabled by PURR_KBD_RAW",
              "#define PURR_KBD_RAW                1",
              "#define T_DECK_KBD_ADDRESS          0x%02X" % g.get("kbd_addr", kb.get("addr", 0x55)),
              "#define T_DECK_KBD_MODE_RAW_CMD     0x%02X" % g.get("kbd_raw_cmd", 3),
              "#define RG_I2C_GPIO_ADDR            T_DECK_KBD_ADDRESS"]
    elif kind == "i2c-expander":
        L += ["#define RG_I2C_GPIO_DRIVER          %d   // 1 = AW9523, 2 = PCF9539, 3 = MCP23017" % g.get("expander_driver", 3),
              "#define RG_I2C_GPIO_ADDR            0x%02X" % g.get("expander_addr", 0x20)]
    L += ["// Audio", "#define RG_AUDIO_USE_INT_DAC        0", "#define RG_AUDIO_USE_EXT_DAC        %d" % (1 if spk else 0)]
    pins = []
    if b.get("power_pin", -1) >= 0:
        pins.append(b["power_pin"])
    idle = [p for p in b.get("idle_high_pins", [])] + [disp["cs"]]
    L += ["// Board bring-up: the power rail first, then every chip select on the shared bus held high", "#define RG_CUSTOM_PLATFORM_INIT()  \\"]
    if pins:
        L += ["    gpio_set_direction(GPIO_NUM_%d, GPIO_MODE_OUTPUT); \\" % pins[0], "    gpio_set_level(GPIO_NUM_%d, 1); \\" % pins[0]]
    for p in dict.fromkeys(idle):
        L += ["    gpio_set_direction(GPIO_NUM_%d, GPIO_MODE_OUTPUT); \\" % p, "    gpio_set_level(GPIO_NUM_%d, 1); \\" % p]
    L += ["    gpio_set_pull_mode(GPIO_NUM_%d, GPIO_PULLUP_ONLY); \\" % spi["miso"], "    rg_task_delay(%d);" % b.get("power_settle_ms", 50)]
    L += ["// Video", "#define RG_SCREEN_DRIVER            0   // 0 = ILI9341/ST7789", "#define RG_SCREEN_HOST              %s" % host,
          "#define RG_SCREEN_SPEED             SPI_MASTER_FREQ_80M", "#define RG_SCREEN_BACKLIGHT         %d" % (1 if disp.get("backlight", -1) >= 0 else 0),
          "#define RG_SCREEN_WIDTH             %d" % disp["width"], "#define RG_SCREEN_HEIGHT            %d" % disp["height"],
          "#define RG_SCREEN_ROTATE            0", "#define RG_SCREEN_VISIBLE_AREA      {0, 0, 0, 0}", "#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0}"]
    L.append(ILI9341_INIT % g.get("screen_madctl", 0xC8))
    keys = g.get("keys", {})
    unknown = [k for k in keys if k not in BUTTONS]
    if unknown:
        raise ValueError("[game.keys]: unknown button(s): " + ", ".join(unknown))
    L.append("// Input")
    if kind in ("i2c-keyboard-raw", "i2c-expander"):
        L.append("#define RG_GAMEPAD_I2C_MAP { \\")
        for name in BUTTONS:
            if name in keys and name != "menu":
                L.append("    {RG_KEY_%s, .num = %d, .level = %d}, \\" % (name.upper(), keys[name], 1 if kind == "i2c-keyboard-raw" else 0))
        L.append("}")
    if g.get("menu_gpio") is not None:
        L += ["#define RG_GAMEPAD_GPIO_MAP { \\", "    {RG_KEY_MENU, .num = GPIO_NUM_%d, .pullup = 1, .level = 0}, \\" % g["menu_gpio"], "}",
              "#define RG_RECOVERY_BTN             RG_KEY_MENU"]
    if bat:
        lo, hi = g.get("battery_min_mv", 3300), g.get("battery_max_mv", 4200)
        L += ["// Battery", "#define RG_BATTERY_DRIVER           1", "#define RG_BATTERY_ADC_UNIT         ADC_UNIT_1",
              "#define RG_BATTERY_ADC_CHANNEL      ADC1_CHANNEL_%d" % _adc1_channel(bat["pin"]),
              "#define RG_BATTERY_CALC_PERCENT(raw) (((raw) * 2.f - %d.f) / (%d.f - %d.f) * 100.f)" % (lo, hi, lo),
              "#define RG_BATTERY_CALC_VOLTAGE(raw) ((raw) * 2.f * 0.001f)"]
    L += ["// I2C bus", "#define RG_GPIO_I2C_SDA             GPIO_NUM_%d" % kb.get("sda", 18), "#define RG_GPIO_I2C_SCL             GPIO_NUM_%d" % kb.get("scl", 8),
          "// SPI display", "#define RG_GPIO_LCD_MISO            GPIO_NUM_%d" % spi["miso"], "#define RG_GPIO_LCD_MOSI            GPIO_NUM_%d" % spi["mosi"],
          "#define RG_GPIO_LCD_CLK             GPIO_NUM_%d" % spi["sclk"], "#define RG_GPIO_LCD_CS              GPIO_NUM_%d" % disp["cs"],
          "#define RG_GPIO_LCD_DC              GPIO_NUM_%d" % disp["dc"]]
    if disp.get("backlight", -1) >= 0:
        L.append("#define RG_GPIO_LCD_BCKL            GPIO_NUM_%d" % disp["backlight"])
    if disp.get("rst", -1) >= 0:
        L.append("#define RG_GPIO_LCD_RST             GPIO_NUM_%d" % disp["rst"])
    L += ["// SPI SD card", "#define RG_GPIO_SDSPI_MISO          GPIO_NUM_%d" % spi["miso"], "#define RG_GPIO_SDSPI_MOSI          GPIO_NUM_%d" % spi["mosi"],
          "#define RG_GPIO_SDSPI_CLK           GPIO_NUM_%d" % spi["sclk"], "#define RG_GPIO_SDSPI_CS            GPIO_NUM_%d" % sd["cs"]]
    if spk:
        L += ["// External I2S DAC", "#define RG_GPIO_SND_I2S_BCK         GPIO_NUM_%d" % spk["bck"], "#define RG_GPIO_SND_I2S_WS          GPIO_NUM_%d" % spk["ws"],
              "#define RG_GPIO_SND_I2S_DATA        GPIO_NUM_%d" % spk["dout"]]
    return "\n".join(L) + "\n"


def add_target_include(text, tid):
    """Retro-Go's config.h picks a target with one #if / #elif chain ending in a single #else (the fallback). Returns the text with a branch for
    `tid` added just before that #else, unchanged if it is already there."""
    macro = "RG_TARGET_" + re.sub(r"[^A-Z0-9]", "_", tid.upper())
    include = '#include "targets/%s/config.h"' % tid
    if include in text:
        return text
    if "\n#else\n" not in text:
        raise ValueError("config.h has no #else to add a target before")
    return text.replace("\n#else\n", "\n#elif defined(%s)\n%s\n#else\n" % (macro, include), 1)


def register_target(retro_go, tid):
    """Retro-Go finds a target by name in its top-level config.h, and its keyboard code knows only the T-Deck by name. Makes both aware of a
    generated target. Idempotent; returns the list of files changed."""
    rg = os.path.join(retro_go, "components", "retro-go")
    changed = []
    cfg = os.path.join(rg, "config.h")
    with open(cfg, encoding="utf-8") as fh:
        text = fh.read()
    new = add_target_include(text, tid)
    if new != text:
        with open(cfg, "w", encoding="utf-8") as fh:
            fh.write(new)
        changed.append(cfg)
    inp = os.path.join(rg, "rg_input.c")
    with open(inp, encoding="utf-8") as fh:
        text = fh.read()
    if "defined(PURR_KBD_RAW)" not in text:
        text = text.replace("#elif defined(RG_TARGET_T_DECK_PLUS)", "#elif defined(RG_TARGET_T_DECK_PLUS) || defined(PURR_KBD_RAW)")
        with open(inp, "w", encoding="utf-8") as fh:
            fh.write(text)
        changed.append(inp)
    return changed


def target(ctx, board, retro_go, target_id, base_target):
    b = _toml(board)
    try:
        text = generate_config(b)
    except ValueError as exc:
        ctx.error("%s: %s" % (board, exc))
        return 1
    tdir = os.path.join(retro_go, "components", "retro-go", "targets")
    base = os.path.join(tdir, base_target)
    if not os.path.isdir(base):
        ctx.error("%s: no such base target (it supplies sdkconfig and env.py for the chip)" % base)
        return 1
    tid = target_id or "purr-" + b["board"]["id"].replace("_", "-")
    out = os.path.join(tdir, tid)
    os.makedirs(out, exist_ok=True)
    for f in ("sdkconfig", "env.py"):
        shutil.copyfile(os.path.join(base, f), os.path.join(out, f))
    with open(os.path.join(out, "config.h"), "w", encoding="utf-8") as fh:
        fh.write(text)
    for f in register_target(retro_go, tid):
        ctx.info("patched %s" % f)
    ctx.ok("target %s written to %s (config.h from %s; sdkconfig and env.py copied from %s)" % (tid, out, os.path.basename(board), base_target))
    return 0


SCRIPT = Script(
    name="games",
    title="Games",
    description="Emulators and games as PURR application packages, and Retro-Go targets generated from board files.",
    actions=(
        Action("package", "Package", package,
               help="wrap a game's application image in a PURR package from its manifest.toml (sign it afterwards)",
               params=(Param("manifest", "path", required=True, help="the game's manifest.toml"),
                       Param("out", "path", default="", help="output .cat (default: next to the manifest, <name>-<board>.cat)"))),
        Action("inspect", "Inspect", inspect,
               help="show what a game package says and check it is consistent",
               params=(Param("package_file", "path", required=True, help="the .cat to read"),)),
        Action("target", "Generate a target", target,
               help="write a Retro-Go target (config.h, sdkconfig, env.py) from a board file's [game] section",
               params=(Param("board", "path", required=True, help="the board file (sdk/boards/<id>.toml)"),
                       Param("retro_go", "path", required=True, help="a Retro-Go checkout"),
                       Param("target_id", "str", default="", help="the target's folder name (default: purr-<board id>)"),
                       Param("base_target", "str", default="t-deck-plus", help="an existing target of the same chip to take sdkconfig and env.py from"))),
    ),
)
