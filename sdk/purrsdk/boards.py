"""A board is data: pins, buses and the devices on them, in a board.toml.

`check()` finds mistakes in plain words (a pin the chip cannot use, two parts on one pin), `to_c()` writes the board file the kernel is built
with (PurrOS/components/kernel/boards/<id>.c), `to_profile()` writes the /etc/board.conf stanzas a running device reads, and `coverage()` says
which parts of a "100 % supported" board have a driver.
"""

import os
import re

from . import minitoml

# ESP32-S3: pins the chip's flash and PSRAM, USB and strapping use (PurrOS/components/coreos/src/purr_hwscan.c, purr_hw_reserved_hard_s3()).
RESERVED_S3 = set([19, 20] + list(range(22, 38)) + list(range(49, 64)))
STRAPPING_S3 = {0: "boot mode", 3: "JTAG source", 45: "flash voltage", 46: "boot messages"}
PIN_KEYS = ("tx", "rx", "cs", "rst", "busy", "dio1", "dio2", "pwr", "pps", "int", "en", "irq", "mosi", "miso", "sclk", "bck", "ws", "din", "dout", "mclk",
            "sda", "scl", "pin", "backlight", "dc", "int_pin", "rst_pin", "up", "down", "left", "right", "click")
SHARED_OK = ("sda", "scl", "mosi", "miso", "sclk")           # bus lines several parts may share
STATUSES = ("works", "built", "written", "template")
CLASSES = ("display", "touch", "keyboard", "battery", "light", "gnss", "lora", "modem", "wifi", "bluetooth", "pointer", "storage", "audio", "sensor")

# What a 100 % supported T-Deck Plus has (the owner's end goal): class -> what it means.
TDECK_PLUS_PARTS = [
    ("display", "Display (ST7789)"), ("touch", "Touch (GT911)"), ("keyboard", "Keyboard"), ("light", "Keyboard backlight"),
    ("battery", "Battery gauge"), ("gnss", "GPS"), ("lora", "LoRa radio"), ("pointer", "Trackball"), ("storage", "SD card"), ("audio", "Speaker"),
]


def load(path):
    b = minitoml.load(path)
    b.setdefault("device", [])
    return b


def _pins(section, name):
    """[(pin, role)] for one part's settings."""
    out = []
    for k, v in section.items():
        if k in PIN_KEYS and isinstance(v, int) and not isinstance(v, bool) and v >= 0:
            out.append((v, k))
    return out


def all_parts(board):
    parts = []
    for key in ("display", "keyboard", "touch"):
        if key in board:
            parts.append((key, dict(board[key]), key))
    for d in board.get("device", []):
        parts.append((d.get("name", "?"), dict(d), d.get("class", "")))
    return parts


def check(board):
    """[{"level": "bad|warn|info", "where": "...", "text": "..."}]"""
    issues = []

    def add(level, where, text):
        issues.append({"level": level, "where": where, "text": text})

    b = board.get("board", {})
    for key in ("id", "name", "chip"):
        if not b.get(key):
            add("bad", "board." + key, "This needs a value.")
    if b.get("id") and not re.match(r"^[a-z][a-z0-9_]*$", b["id"]):
        add("bad", "board.id", "Use lowercase letters, digits and underscores, starting with a letter: it becomes a C name and a file name.")
    if b.get("chip") and b["chip"] != "esp32s3":
        add("warn", "board.chip", "Only the ESP32-S3 is checked by the kit; other chips need their own reserved-pin list.")
    uses = {}
    for name, part, cls in all_parts(board):
        status = part.get("status", "works" if name in ("display", "keyboard", "touch") else "template")
        if status not in STATUSES:
            add("bad", name, "status must be one of: " + ", ".join(STATUSES) + ".")
        if name not in ("display", "keyboard", "touch"):
            if not part.get("compatible"):
                add("bad", name, "A device needs `compatible`: the driver that serves it.")
            if len([1 for k in part if k not in ("name", "compatible", "class", "bus", "priority", "status", "note")]) > 12:
                add("bad", name, "A device can carry at most 12 settings.")
            for k in part:
                if k not in ("name", "compatible", "class", "bus", "priority", "status", "note", "port", "addr", "hz", "baud", "mul", "div", "tcxo", "dio2sw") \
                        and k not in PIN_KEYS and len(k) > 7:
                    add("bad", name, "The setting %r is longer than 7 letters; the device config keeps names of at most 7." % k)
            if part.get("class") and part["class"] not in CLASSES:
                add("warn", name, "Unknown class %r. Known: %s." % (part["class"], ", ".join(CLASSES)))
        for pin, role in _pins(part, name):
            uses.setdefault(pin, []).append((name, role))
            if pin in RESERVED_S3:
                add("bad", name + "." + role, "GPIO %d is used by the chip's flash, PSRAM or USB; a device cannot use it." % pin)
            elif pin in STRAPPING_S3:
                add("info", name + "." + role, "GPIO %d is a strapping pin (%s): fine as an input once the chip has started, but keep it unpulled at reset." % (pin, STRAPPING_S3[pin]))
    spi = board.get("spi", {})
    for role in ("mosi", "miso", "sclk"):
        if isinstance(spi.get(role), int):
            uses.setdefault(spi[role], []).append(("spi", role))
    for pin, who in sorted(uses.items()):
        if len(who) < 2:
            continue
        if all(r in SHARED_OK for _, r in who):
            continue
        names = ", ".join("%s (%s)" % w for w in who)
        # the shared bus lines are fine; two different jobs on one pin are not, except a reset line that two parts share on purpose
        if all(r in ("rst", "rst_pin") for _, r in who):
            add("info", "gpio %d" % pin, "A reset line shared by %s." % names)
        else:
            add("bad", "gpio %d" % pin, "Used by more than one part: %s." % names)
    p = b.get("power_pin")
    if isinstance(p, int) and p >= 0 and p in uses and any(r != "power" for _, r in uses[p]):
        add("bad", "board.power_pin", "GPIO %d is also used by a part." % p)
    for pin in b.get("idle_high_pins", []):
        if pin in RESERVED_S3:
            add("bad", "board.idle_high_pins", "GPIO %d is reserved by the chip." % pin)
    issues += check_layout(board)
    return issues


# ESP-IDF partition types and the named subtypes the PURR tables use (the numeric ones, 0x40 and up, are written as numbers in the board file).
PART_TYPES = {"app": 0, "data": 1}
PART_SUBTYPES = {("app", "factory"): 0, ("app", "ota_0"): 0x10, ("app", "test"): 0x20, ("data", "nvs"): 2, ("data", "phy"): 1}
TABLE_AT = 0xA000                      # PURR keeps the table at 0xA000 (the bootloader gets 40 KB); the area up to 0x20000 is the system's own
FIRST_FREE = 0x20000


def _sub(part):
    """The numeric subtype of a board-file partition, or None when it is not understood."""
    sub = part.get("subtype", 0)
    if isinstance(sub, int):
        return sub
    if isinstance(sub, str):
        if (part.get("type"), sub) in PART_SUBTYPES:
            return PART_SUBTYPES[(part.get("type"), sub)]
        try:
            return int(sub, 0)
        except ValueError:
            return None
    return None


def flash_layout(board):
    """The board's flash layout as [{"name", "type", "subtype", "offset", "size"}] with numbers, sorted by offset; [] when the board file has none."""
    out = []
    for p in board.get("partition", []):
        out.append({"name": p.get("name", ""), "type": PART_TYPES.get(p.get("type"), -1), "subtype": _sub(p), "offset": p.get("offset", 0), "size": p.get("size", 0)})
    return sorted(out, key=lambda p: p["offset"])


def check_layout(board):
    """Mistakes in the flash layout, in the same shape as check(). No layout is not a mistake (a board may leave it to the build)."""
    issues = []

    def add(level, where, text):
        issues.append({"level": level, "where": where, "text": text})

    parts = board.get("partition", [])
    if not parts:
        return issues
    flash = board.get("board", {}).get("flash_mb")
    end = flash * 1024 * 1024 if isinstance(flash, int) else None
    seen = set()
    last = None
    for p in sorted(parts, key=lambda q: q.get("offset", 0)):
        name = p.get("name", "?")
        where = "partition." + str(name)
        off, size = p.get("offset"), p.get("size")
        if not isinstance(off, int) or not isinstance(size, int) or size <= 0:
            add("bad", where, "A partition needs a numeric `offset` and a `size` above zero.")
            continue
        if name in seen or not name or len(name) > 15:
            add("bad", where, "Names must be unique and at most 15 letters (the table keeps 16 bytes).")
        seen.add(name)
        if p.get("type") not in PART_TYPES or _sub(p) is None:
            add("bad", where, "`type` is app or data, and `subtype` a known name or a number.")
        if off % 0x1000 or size % 0x1000:
            add("bad", where, "Offset and size must be multiples of 4 KB (flash sectors).")
        if p.get("type") == "app" and off % 0x10000:
            add("bad", where, "An app partition must start on a 64 KB boundary (the flash is mapped in 64 KB pages).")
        if off < FIRST_FREE and name not in ("nvs", "purrcfg", "phy_init", "bootpkg"):
            add("bad", where, "Below 0x%X only the system's own areas live (nvs, purrcfg, phy_init, bootpkg)." % FIRST_FREE)
        if off < TABLE_AT + 0x1000:
            add("bad", where, "Overlaps the bootloader or the partition table (0x0 to 0x%X)." % (TABLE_AT + 0x1000))
        if last is not None and off < last["offset"] + last["size"]:
            add("bad", where, "Overlaps %s." % last["name"])
        last = {"name": name, "offset": off, "size": size}
        if end is not None and off + size > end:
            add("bad", where, "Runs past the end of the %d MB flash." % flash)
    if end is not None and last is not None:
        names = [p.get("name") for p in parts]
        if "root" not in names:
            add("bad", "partition", "A board needs a `root` partition: the LittleFS file system.")
        elif last["name"] != "root" or last["offset"] + last["size"] != end:
            add("warn", "partition.root", "`root` is meant to be the last partition and to run to the end of the flash (flash map v2): everything that is not a slot is a file in it.")
    if "ui_assets" in [p.get("name") for p in parts]:
        add("bad", "partition.ui_assets", "The raw picture partition is gone in flash map v2 (1.0.29): the picture packs are files in `root`.")
    return issues


def layout_differences(found, reference):
    """Compare a flash image's partition table with a board's layout. `found` is [{"name","type","subtype","offset","size"}] (as the inspector reads them),
    `reference` is flash_layout(). Returns a list of plain sentences; empty when they agree."""
    ref = {p["name"]: p for p in reference}
    got = {p["name"]: p for p in found}
    out = []
    for name, p in sorted(got.items(), key=lambda kv: kv[1]["offset"]):
        r = ref.get(name)
        if r is None:
            out.append("%s (0x%X, %d KB) is not in the board's layout" % (name, p["offset"], p["size"] // 1024))
        elif (p["offset"], p["size"]) != (r["offset"], r["size"]):
            out.append("%s is at 0x%X for 0x%X bytes, the board's layout says 0x%X for 0x%X" % (name, p["offset"], p["size"], r["offset"], r["size"]))
    for name, r in sorted(ref.items(), key=lambda kv: kv[1]["offset"]):
        if name not in got:
            out.append("%s (0x%X) is missing" % (name, r["offset"]))
    return out


def pin_map(board):
    """Which part uses which GPIO: {"pins": {pin: [[part, role], ...]}, "reserved": [...], "strapping": {pin: why}}"""
    pins = {}
    for name, part, cls in all_parts(board):
        for pin, role in _pins(part, name):
            pins.setdefault(pin, []).append([name, role])
    spi = board.get("spi", {})
    for role in ("mosi", "miso", "sclk"):
        if isinstance(spi.get(role), int) and spi[role] >= 0:
            pins.setdefault(spi[role], []).append(["spi", role])
    p = board.get("board", {}).get("power_pin")
    if isinstance(p, int) and p >= 0:
        pins.setdefault(p, []).append(["board", "power"])
    return {"pins": pins, "reserved": sorted(RESERVED_S3), "strapping": STRAPPING_S3}


def coverage(board):
    """For each part of a fully supported T-Deck Plus: whether the board has it and how far it has got."""
    have = {}
    for name, part, cls in all_parts(board):
        status = part.get("status", "works" if name in ("display", "keyboard", "touch") else "template")
        have[cls or name] = (name, status, part.get("compatible", ""))
    out = []
    for cls, label in TDECK_PLUS_PARTS:
        if cls in have:
            out.append({"class": cls, "label": label, "device": have[cls][0], "driver": have[cls][2], "status": have[cls][1]})
        else:
            out.append({"class": cls, "label": label, "device": "", "driver": "", "status": "missing"})
    return out


def _c_str(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def _pin(v):
    return "PURR_PIN_NONE" if v is None or v < 0 else str(v)


def _hex_or_int(v):
    return "0x%02X" % v if isinstance(v, int) and v > 9 and v < 256 and False else str(v)


def to_c(board, include_templates=False):
    """The board file. Devices still at status "template" (no driver yet) are left out unless asked for."""
    b, spi = board["board"], board.get("spi", {})
    d, kb, tc, btn = board.get("display", {}), board.get("keyboard", {}), board.get("touch", {}), board.get("buttons", {})
    idle = list(b.get("idle_high_pins", []))[:4]
    idle += ["PURR_PIN_NONE"] * (4 - len(idle))
    o = []
    w = o.append
    w("/* Generated by `purr board build` from %s.toml. Edit the .toml, not this file. */" % b["id"])
    w("#include <stddef.h>\n")
    w('#include "purr_board.h"')
    w('#include "purr_driver.h"\n')
    w("static purr_board_t s_board = {")
    w("    .name = %s," % _c_str(b["id"]))
    w("    .power_pin = %s," % _pin(b.get("power_pin", -1)))
    w("    .power_settle_ms = %d," % b.get("power_settle_ms", 0))
    w("    .idle_high_pins = {%s}," % ", ".join(str(i) if isinstance(i, int) else i for i in idle))
    w("    .spi = {.host = %d, .mosi = %s, .miso = %s, .sclk = %s}," % (spi.get("host", 1), _pin(spi.get("mosi", -1)), _pin(spi.get("miso", -1)), _pin(spi.get("sclk", -1))))
    if d:
        w("    .display = {")
        w("        .name = %s," % _c_str(d.get("name", "")))
        w("        .compatible = %s," % _c_str(d["compatible"]))
        w("        .cs = %s, .dc = %s, .rst = %s, .backlight = %s," % (_pin(d.get("cs", -1)), _pin(d.get("dc", -1)), _pin(d.get("rst", -1)), _pin(d.get("backlight", -1))))
        w("        .width = %d, .height = %d," % (d.get("width", 0), d.get("height", 0)))
        w("        .col_off = %d, .row_off = %d," % (d.get("col_off", 0), d.get("row_off", 0)))
        w("        .madctl = 0x%02X, .bgr = %d, .invert = %d," % (d.get("madctl", 0), d.get("bgr", 0), d.get("invert", 0)))
        w("        .spi_hz = %d," % d.get("spi_hz", 40000000))
        w("    },")
    if kb:
        w("    .keyboard = {.port = %d, .sda = %s, .scl = %s, .addr = 0x%02X, .hz = %d}," % (kb.get("port", 0), _pin(kb.get("sda", -1)), _pin(kb.get("scl", -1)), kb.get("addr", 0), kb.get("hz", 100000)))
    if tc:
        w("    .touch = {.port = %d, .sda = %s, .scl = %s, .int_pin = %s, .rst_pin = %s," % (tc.get("port", 0), _pin(tc.get("sda", -1)), _pin(tc.get("scl", -1)), _pin(tc.get("int_pin", -1)), _pin(tc.get("rst_pin", -1))))
        w("              .hz = %d, .width = %d, .height = %d," % (tc.get("hz", 400000), tc.get("width", 0), tc.get("height", 0)))
        w("              .native_x_max = %d, .native_y_max = %d}," % (tc.get("native_x_max", 0), tc.get("native_y_max", 0)))
    w("    .btn_boot_pin = %s," % _pin(btn.get("boot", -1)))
    w("    .btn_pwr_pin = %s," % _pin(btn.get("pwr", -1)))
    w("    .btn_hold_ms = %d," % btn.get("hold_ms", 0))
    w("};\n")

    devs = []
    for i, dev in enumerate(board.get("device", [])):
        if dev.get("status", "template") == "template" and not include_templates:
            continue
        bus = dev.get("bus", "")
        sym = "s_cfg_%d" % i
        extra = []
        if bus == "adc":
            w("static const purr_adc_dev_cfg_t %s = {.pin = %s, .mul = %d, .div = %d};" % (sym, _pin(dev.get("pin", -1)), dev.get("mul", 1), dev.get("div", 1)))
            extra = [dev.get("pin", -1)]
        elif bus == "i2c":
            w("static const purr_i2c_dev_cfg_t %s = {.port = %d, .sda = %s, .scl = %s, .addr = 0x%02X, .hz = %d};" % (sym, dev.get("port", 0), _pin(dev.get("sda", -1)), _pin(dev.get("scl", -1)), dev.get("addr", 0), dev.get("hz", 100000)))
        else:
            skip = ("name", "compatible", "class", "bus", "priority", "status", "note")
            kv = [(k, v) for k, v in dev.items() if k not in skip and isinstance(v, int) and not isinstance(v, bool)]
            kv += [(k, 1 if v else 0) for k, v in dev.items() if k not in skip and isinstance(v, bool)]
            w("static const purr_kv_cfg_t %s = {.count = %d, .kv = {" % (sym, len(kv)))
            for k, v in kv:
                w("    {%s, %d}," % (_c_str(k), v))
            w("}};")
            extra = [v for k, v in kv if k in PIN_KEYS and k not in SHARED_OK and v >= 0 and k not in ("baud",)]
        extra = (extra + [-1] * PURR_MAX_EXTRA)[:PURR_MAX_EXTRA]
        pri = {"required": "PURR_PRI_REQUIRED", "important": "PURR_PRI_IMPORTANT"}.get(dev.get("priority", "optional"), "PURR_PRI_OPTIONAL")
        devs.append((dev, sym, pri, extra))
    w("")
    w("static const purr_device_t s_devices[] = {")
    if d:
        w('    {.name = "display", .compatible = %s, .priority = PURR_PRI_REQUIRED, .extra_pins = {%s}, .cfg = &s_board.display},' % (
            _c_str(d["compatible"]), ", ".join(_pin(x) for x in ([d.get("cs", -1), d.get("dc", -1), d.get("rst", -1), d.get("backlight", -1)] + [-1] * 2))))
    if kb:
        w('    {.name = "keyboard", .compatible = %s, .priority = PURR_PRI_OPTIONAL, .extra_pins = {%s}, .cfg = &s_board.keyboard},' % (_c_str(kb.get("compatible", "kbd-i2c")), ", ".join(["PURR_PIN_NONE"] * 6)))
    if tc:
        w('    {.name = "touch", .compatible = %s, .priority = PURR_PRI_OPTIONAL, .extra_pins = {%s}, .cfg = &s_board.touch},' % (
            _c_str(tc.get("compatible", "gt911")), ", ".join(_pin(x) for x in ([tc.get("rst_pin", -1)] + [-1] * 5))))
    for dev, sym, pri, extra in devs:
        w('    {.name = %s, .compatible = %s, .priority = %s, .extra_pins = {%s}, .cfg = &%s},' % (_c_str(dev["name"]), _c_str(dev["compatible"]), pri, ", ".join(_pin(x) for x in extra), sym))
    w("};\n")
    w("const purr_board_t *purr_board(void)")
    w("{")
    w("    if (s_board.devices == NULL) {")
    w("        s_board.devices = s_devices;")
    w("        s_board.device_count = sizeof(s_devices) / sizeof(s_devices[0]);")
    w("    }")
    w("    return &s_board;")
    w("}")
    return "\n".join(o) + "\n"


PURR_MAX_EXTRA = 6


def to_profile(board, include_status=("works", "built", "written")):
    """The /etc/board.conf text for the extra devices (not the display, touch or keyboard): the stanzas a running device reads."""
    out = ["# Written by `purr board build` from %s.toml." % board["board"]["id"], ""]
    n = 0
    for dev in board.get("device", []):
        if dev.get("status", "template") not in include_status:
            continue
        bus = dev.get("bus", "")
        if bus == "adc":
            continue                      # the battery is part of the board file, not a discovered device
        out.append("device=%s" % dev["name"])
        out.append("compatible=%s" % dev["compatible"])
        out.append("label=%s" % str(dev.get("label", dev["name"]))[:39])
        out.append("bus=%s" % (bus or "gpio"))
        for k, v in dev.items():
            if k in ("name", "compatible", "class", "bus", "priority", "status", "note"):
                continue
            if isinstance(v, bool):
                v = int(v)
            if isinstance(v, int):
                out.append("%s=%s" % (k, ("0x%02x" % v) if k == "addr" else v))
        out.append("")
        n += 1
    return "\n".join(out).rstrip() + "\n", n
