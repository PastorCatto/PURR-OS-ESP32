"""UI packs and themes: make a pack from a folder of PNGs, check a pack or a theme folder the way the OS will.

A pack (`pack.bin`) is the file `purrstrap uiassets` writes: a 12-byte header, a directory of 40-byte entries, then raw RGB565 pixels
(or RGB565 plus an alpha plane), CRC32 over the directory and the pixels. The kit packs with purrstrap's own code (scripts/uiassets.py),
so a pack made here is the pack the release tools make. A theme is a folder with a `theme.conf` and a `pack.bin` (plan7.md, plan1.md
"Flash map v2"); it is assets and settings, never code.

The limits below are the ones the device code reads (PurrOS/main/purr_ui_assets.c): at most 5 MB a file, at most 128 pictures in a pack,
entry offsets even and inside the file, theme.conf under 512 bytes. The lists of picture names are UI/PACK-NAMES.md, kept in step by a test.
"""

import os
import re
import struct
import zlib

from . import formats

MAX_PACK_BYTES = 5 * 1024 * 1024
MAX_PICTURES = 128                      # what PurrOS/main/purr_ui_assets.c can index (MAX_ASSETS)
MAX_SIDE = 2048                         # no picture the shells draw is anywhere near this; a bigger one is a mistake
NAME_RE = re.compile(r"^[A-Za-z0-9_.-]{1,23}$")
THEME_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,39}$")      # the folder name /ui/themes/<id>: id_ok() on the device
SHELLS = ("w10m", "android")
THEME_SHELLS = SHELLS + ("any",)
THEME_KEYS = ("name", "author", "licence", "version", "shell", "accent", "dark")
THEME_REQUIRED = ("name", "author", "licence", "version", "shell")
THEME_LIMITS = {"name": 31, "author": 31, "version": 11}      # the sizes of purr_theme_info_t on the device (one less than the buffer)
THEME_CONF_MAX = 511                    # the device reads theme.conf into a 512 byte buffer
ACCENTS = 20                            # PurrOS/main/purr_w10m_theme.c has 20 accent colours; an index is 0 to 19
HEX_COLOUR = re.compile(r"^#[0-9A-Fa-f]{6}$")

_WALL = (320, 569)
WALLPAPERS = {"img%d" % i: _WALL for i in range(10)}

# Required (R): a complete pack for the shell carries it. name -> (width, height). See UI/PACK-NAMES.md for where each is drawn.
REQUIRED = {
    "w10m": dict(
        [(n, (32, 32)) for n in ("wp_settings", "wp_system", "wp_devices", "wp_network", "wp_personalization", "wp_apps", "wp_account",
                                 "wp_time", "wp_privacy", "wp_about")] +
        [("wifi_icon", (24, 24))] +
        [(n, (16, 16)) for n in ("wifi_ic", "brightness_ic", "settings_ic", "airplane_ic", "cellular_ic", "bluetooth_ic", "battery_ic",
                                 "hotspot_ic", "vpn_ic", "location_ic")]),
    "android": dict(
        [(n, (48, 48)) for n in ("fr_app", "fr_settings", "fr_folder", "fr_gallery", "fr_messaging")] +
        [("fr_s_app", (16, 16))] +
        [("st_b%d" % n, (19, 19)) for n in (0, 10, 20, 40, 60, 80, 100)] +
        [("st_bc%d" % n, (19, 19)) for n in range(6)] +
        [("st_bunk", (19, 19)), ("st_snull", (19, 19))] +
        [("st_s%d" % n, (19, 19)) for n in range(5)] +
        [("st_w%d" % n, (19, 19)) for n in range(5)]),
}
# Fallback (F): the shell draws something else on purpose when it is missing. Listed so a size can be checked and a misspelt name noticed.
FALLBACK = {
    "w10m": dict(WALLPAPERS),
    "android": dict(WALLPAPERS, fr_dock_apps=(40, 40), kb_menu=(20, 20), kb_home=(20, 20), kb_back=(20, 20),
                    **{"fr_" + n: (48, 48) for n in ("phone", "contacts", "browser", "camera", "music", "clock", "video", "recorder")}),
}
# Names that only exist in the files of the shell's own art today (not asked for by the code, or asked for through a pattern): not reported as unknown.
KNOWN_EXTRA = {"android": ("fr_dock_phone", "fr_dock_browser", "fr_dock_contacts", "fr_dock_messaging", "fr_s_messaging", "fr_s_settings")}


def known(shell):
    out = dict(REQUIRED[shell])
    out.update(FALLBACK[shell])
    return out


def _u():
    return formats.uiassets()


def _add(checks, status, name, text):
    checks.append({"status": status, "name": name, "text": text})


def _report(checks, facts=None):
    return {"ok": not any(c["status"] == "bad" for c in checks), "checks": checks, "facts": facts or {}}


# ------------------------------------------------------------------------------------------------------ making a pack

def _name_problem(name):
    if not NAME_RE.match(name):
        return "%r: a picture name is 1 to 23 letters, digits, underscores, dots or dashes" % name
    return None


def pack_folder(folder, out):
    """Packs every .png in `folder` (file name without .png = picture name) into `out`. A picture with real transparency is stored as
    RGB565A8, an opaque one (no alpha channel, or all alpha 255) as plain RGB565. Raises ValueError with a message for the person.
    Returns {"path", "count", "bytes", "names"}."""
    ua = _u()
    if not os.path.isdir(folder):
        raise ValueError("%s is not a folder" % folder)
    files = sorted(f for f in os.listdir(folder) if f.lower().endswith(".png"))
    if not files:
        raise ValueError("%s has no .png files" % folder)
    if len(files) > MAX_PICTURES:
        raise ValueError("%d pictures: a pack holds at most %d" % (len(files), MAX_PICTURES))
    images = []
    for f in files:
        name = f[:-4]
        bad = _name_problem(name)
        if bad:
            raise ValueError(bad)
        w, h, ch, px = ua.decode_png(os.path.join(folder, f))
        if not (1 <= w <= MAX_SIDE and 1 <= h <= MAX_SIDE):
            raise ValueError("%s: %d x %d is not a sensible picture size (1 to %d)" % (f, w, h, MAX_SIDE))
        transparent = ch == 4 and any(px[i] != 255 for i in range(3, len(px), 4))
        if transparent:
            images.append((name, w, h, ua.to_rgb565a8(w, h, ch, px), ua.CF_RGB565A8))
        else:
            images.append((name, w, h, ua.to_rgb565(w, h, ch, px), ua.CF_RGB565))
    blob = ua.pack(images)
    if len(blob) > MAX_PACK_BYTES:
        raise ValueError("the pack would be %s bytes, over the %s byte cap" % (format(len(blob), ","), format(MAX_PACK_BYTES, ",")))
    d = os.path.dirname(os.path.abspath(out))
    os.makedirs(d, exist_ok=True)
    with open(out, "wb") as fh:
        fh.write(blob)
    return {"path": out, "count": len(images), "bytes": len(blob), "names": [i[0] for i in images]}


# ------------------------------------------------------------------------------------------------------ checking a pack

def read_pack(data):
    """Parses a pack the way the device does. Returns (entries, problems, facts): entries are dicts (name, offset, length, w, h, cf),
    problems a list of strings that make the device refuse the pack."""
    ua = _u()
    problems, entries = [], []
    hsz, esz = struct.calcsize(ua.HEADER_FMT), ua.ENTRY_SIZE
    if len(data) < hsz:
        return entries, ["%d bytes is too short to be a pack (the header alone is %d)" % (len(data), hsz)], {}
    magic, count, crc = struct.unpack_from(ua.HEADER_FMT, data, 0)
    facts = {"count": count, "bytes": len(data)}
    if magic != ua.MAGIC:
        return entries, ["bad magic %08x: this is not a UI pack" % magic], facts
    if count > MAX_PICTURES:
        problems.append("%d pictures; the device indexes at most %d" % (count, MAX_PICTURES))
        return entries, problems, facts
    dir_end = hsz + count * esz
    if dir_end > len(data):
        problems.append("the directory (%d entries) runs past the end of the file" % count)
        return entries, problems, facts
    region = len(data) - dir_end
    used = 0
    for i in range(count):
        raw_name, off, length, w, h, cf = struct.unpack_from(ua.ENTRY_FMT, data, hsz + i * esz)
        end = raw_name.find(b"\0")
        if end < 0:
            problems.append("entry %d: the name has no end (it must be at most 23 characters)" % i)
            continue
        try:
            name = raw_name[:end].decode("ascii")
        except UnicodeDecodeError:
            problems.append("entry %d: the name is not ASCII" % i)
            continue
        e = {"name": name, "offset": off, "length": length, "w": w, "h": h, "cf": cf}
        entries.append(e)
        bad = _name_problem(name)
        if bad:
            problems.append("entry %d: %s" % (i, bad))
        if cf not in (ua.CF_RGB565, ua.CF_RGB565A8):
            problems.append("%s: colour format %d is not one the device knows (0 = RGB565, 1 = RGB565 with alpha)" % (name, cf))
        elif length != w * h * (3 if cf == ua.CF_RGB565A8 else 2):
            problems.append("%s: %d bytes is not what a %d x %d picture takes" % (name, length, w, h))
        if w == 0 or h == 0 or w > MAX_SIDE or h > MAX_SIDE:
            problems.append("%s: %d x %d is not a sensible picture size" % (name, w, h))
        if off + length > region:
            problems.append("%s: its pixels run past the end of the file" % name)
        else:
            used = max(used, off + length)
        if off % 2:
            problems.append("%s: its pixels start at an odd offset (the device refuses that)" % name)
    names = [e["name"] for e in entries]
    for n in sorted({n for n in names if names.count(n) > 1}):
        problems.append("%s appears more than once" % n)
    if not problems:
        want = zlib.crc32(data[hsz:dir_end + used]) & 0xFFFFFFFF
        facts["crc_ok"] = want == crc
        if want != crc:
            problems.append("CRC mismatch: the header says %08x, the directory and pixels give %08x (a damaged or partly written file)" % (crc, want))
        facts["trailing"] = region - used
    return entries, problems, facts


def check_pack_bytes(data, shell=None):
    ua = _u()
    checks = []
    if len(data) > MAX_PACK_BYTES:
        _add(checks, "bad", "size", "%s bytes is over the %s byte cap; the device refuses it" % (format(len(data), ","), format(MAX_PACK_BYTES, ",")))
    else:
        _add(checks, "ok", "size", "%s bytes (cap %s)" % (format(len(data), ","), format(MAX_PACK_BYTES, ",")))
    entries, problems, facts = read_pack(data)
    for p in problems:
        _add(checks, "bad", "format", p)
    if not problems:
        _add(checks, "ok", "format", "%d pictures; header, directory and offsets are inside the file; CRC matches" % len(entries))
        if facts.get("trailing"):
            _add(checks, "warn", "padding", "%d byte(s) after the last picture (the device ignores them; a packed release component leaves them out)" % facts["trailing"])
        total = sum(e["length"] for e in entries)
        _add(checks, "ok", "pictures", "%s bytes of pixels; %s" % (format(total, ","), ", ".join(
            "%s %dx%d%s" % (e["name"], e["w"], e["h"], "+a" if e["cf"] == ua.CF_RGB565A8 else "") for e in entries[:6]) + (", ..." if len(entries) > 6 else "")))
    if shell:
        _shell_checks(checks, entries, shell, complete=True)
    return _report(checks, {"pictures": [e["name"] for e in entries], "bytes": len(data)})


def _shell_checks(checks, entries, shell, complete):
    """Names against the shell's list. A default pack (`complete`) must have every required name; a theme pack may leave names out
    (the shell's own pack answers for them), so they are listed as a warning."""
    have = {e["name"]: e for e in entries}
    req = REQUIRED[shell]
    missing = [n for n in req if n not in have]
    if not missing:
        _add(checks, "ok", "%s names" % shell, "all %d required names are present" % len(req))
    elif complete:
        _add(checks, "bad", "%s names" % shell, "%d of %d required names are missing: %s" % (len(missing), len(req), ", ".join(missing)))
    else:
        _add(checks, "warn", "%s names" % shell, "%d of %d names are not in this theme (the shell's default pack answers for them): %s" % (
            len(missing), len(req), ", ".join(missing[:8]) + (", ..." if len(missing) > 8 else "")))
    kn = known(shell)
    wrong = ["%s is %dx%d, the shell draws %dx%d" % (n, e["w"], e["h"], kn[n][0], kn[n][1]) for n, e in have.items()
             if n in kn and (e["w"], e["h"]) != kn[n]]
    if wrong:
        _add(checks, "warn", "sizes", "; ".join(wrong[:6]) + ("; ..." if len(wrong) > 6 else "") + ". Nothing is scaled on the device")
    else:
        _add(checks, "ok", "sizes", "every picture the shell knows is the size it draws")
    odd = sorted(n for n in have if n not in kn and n not in KNOWN_EXTRA.get(shell, ()) and not (shell == "android" and n.startswith("fr_")))
    if odd:
        _add(checks, "info", "other names", "not asked for by the %s shell (a misspelling?): %s" % (shell, ", ".join(odd[:10]) + (", ..." if len(odd) > 10 else "")))
    if shell == "android" and any(n.startswith("wp_") for n in have):
        _add(checks, "warn", "wp_ names", "the Android build also draws wp_* pictures in Settings if they are present: leave them out unless you want them there")


def check_pack_file(path, shell=None):
    with open(path, "rb") as fh:
        data = fh.read(MAX_PACK_BYTES + 1 + 1024)
    rep = check_pack_bytes(data, shell)
    if len(data) > MAX_PACK_BYTES:
        rep["checks"][0]["text"] = "over the %s byte cap (only the start was read); the device refuses it" % format(MAX_PACK_BYTES, ",")
    return rep


# ------------------------------------------------------------------------------------------------------ theme.conf

def parse_conf(text):
    """key=value lines, the way the device reads them: a key is matched at the start of a line, with `=` right after it. Returns (values, extra),
    where extra lists lines that are neither blank, a # comment nor key=value."""
    values, extra = {}, []
    for raw in text.splitlines():
        line = raw.rstrip("\r")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if "=" not in line:
            extra.append(line)
            continue
        k, v = line.split("=", 1)
        if k != k.strip() or not re.match(r"^[a-z_]+$", k):
            extra.append(line)
            continue
        values.setdefault(k, v)
    return values, extra


def check_conf_text(text, size=None):
    checks = []
    size = len(text.encode("utf-8")) if size is None else size
    if size > THEME_CONF_MAX:
        _add(checks, "bad", "size", "%d bytes: the device reads at most %d bytes of theme.conf" % (size, THEME_CONF_MAX))
    if any(ord(c) < 32 and c not in "\r\n\t" for c in text):
        _add(checks, "bad", "text", "control characters in theme.conf")
    values, extra = parse_conf(text)
    for line in extra:
        _add(checks, "warn", "line", "%r is not key=value (no spaces around =); the device skips it" % line[:40])
    for k in values:
        if k not in THEME_KEYS:
            _add(checks, "warn", k, "not a theme.conf key (the keys are %s)" % ", ".join(THEME_KEYS))
    for k in THEME_REQUIRED:
        v = values.get(k, "")
        if not v.strip():
            _add(checks, "bad", k, "is missing or empty")
            continue
        if k in THEME_LIMITS and len(v) > THEME_LIMITS[k]:
            _add(checks, "bad", k, "%d characters; the device keeps %d" % (len(v), THEME_LIMITS[k]))
        elif k == "shell":
            if v in THEME_SHELLS:
                _add(checks, "ok", k, v)
            else:
                _add(checks, "bad", k, "%r: one of %s" % (v, ", ".join(THEME_SHELLS)))
        elif k == "version":
            if re.match(r"^[0-9][A-Za-z0-9.+-]*$", v):
                _add(checks, "ok", k, v)
            else:
                _add(checks, "bad", k, "%r: start with a digit, like 1.0.0" % v)
        else:
            _add(checks, "ok", k, v)
    if "accent" in values:
        v = values["accent"]
        if HEX_COLOUR.match(v) or (v.isdigit() and int(v) < ACCENTS):
            _add(checks, "ok", "accent", v)
        else:
            _add(checks, "bad", "accent", "%r: an index 0 to %d or a colour like #RRGGBB" % (v, ACCENTS - 1))
    if "dark" in values:
        v = values["dark"]
        if v in ("0", "1"):
            _add(checks, "ok", "dark", v)
        else:
            _add(checks, "bad", "dark", "%r: 1 (dark) or 0 (light)" % v)
    return checks, values


# ------------------------------------------------------------------------------------------------------ checking a theme folder

def check_theme_folder(folder, shell=None):
    checks = []
    base = os.path.basename(os.path.abspath(folder))
    if THEME_ID_RE.match(base):
        _add(checks, "ok", "id", "%s (the folder name becomes /ui/themes/%s)" % (base, base))
    else:
        _add(checks, "warn", "id", "%r: the device only lists a theme whose folder name is 1 to 39 letters, digits, underscores or dashes; install it under such a name" % base)
    conf = os.path.join(folder, "theme.conf")
    values = {}
    if not os.path.isfile(conf):
        _add(checks, "bad", "theme.conf", "is missing")
    else:
        size = os.path.getsize(conf)
        with open(conf, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read(4096)
        more, values = check_conf_text(text, size)
        checks += more
    if shell and values.get("shell") in SHELLS and values["shell"] != shell:
        _add(checks, "bad", "shell", "theme.conf says %s, but you are checking for %s" % (values["shell"], shell))
    target = shell or (values.get("shell") if values.get("shell") in SHELLS else None)
    pack = os.path.join(folder, "pack.bin")
    images = os.path.join(folder, "images")
    if os.path.isfile(pack):
        rep = check_pack_file(pack)
        for c in rep["checks"]:
            _add(checks, c["status"], "pack.bin " + c["name"], c["text"])
        if rep["ok"] and target:
            with open(pack, "rb") as fh:
                entries, _p, _f = read_pack(fh.read())
            _shell_checks(checks, entries, target, complete=False)
        elif rep["ok"]:
            _add(checks, "info", "names", "the theme is for any shell: pass --shell w10m or android to check its names")
    elif os.path.isdir(images):
        _images_checks(checks, images, target)
        _add(checks, "info", "pack.bin", "not built yet: `purr ui pack %s --out %s`" % (images, pack))
    else:
        _add(checks, "bad", "pictures", "neither pack.bin nor an images folder is here")
    return _report(checks)


def _images_checks(checks, images, target):
    ua = _u()
    entries = []
    pngs = sorted(f for f in os.listdir(images) if f.lower().endswith(".png"))
    if not pngs:
        _add(checks, "bad", "images", "the folder has no .png files")
        return
    total = 0
    for f in pngs:
        bad = _name_problem(f[:-4])
        if bad:
            _add(checks, "bad", f, bad)
            continue
        try:
            w, h, ch, _px = ua.decode_png(os.path.join(images, f))
        except (ValueError, OSError, zlib.error) as e:
            _add(checks, "bad", f, str(e))
            continue
        if not (1 <= w <= MAX_SIDE and 1 <= h <= MAX_SIDE):
            _add(checks, "bad", f, "%d x %d is not a sensible picture size" % (w, h))
            continue
        total += w * h * (3 if ch == 4 else 2)
        entries.append({"name": f[:-4], "w": w, "h": h})
    _add(checks, "ok", "images", "%d picture(s), about %s bytes in a pack" % (len(entries), format(total, ",")))
    if len(pngs) > MAX_PICTURES:
        _add(checks, "bad", "count", "%d pictures; a pack holds at most %d" % (len(pngs), MAX_PICTURES))
    if total > MAX_PACK_BYTES:
        _add(checks, "bad", "size", "about %s bytes: over the %s byte cap" % (format(total, ","), format(MAX_PACK_BYTES, ",")))
    if target:
        _shell_checks(checks, entries, target, complete=False)


def check(path, shell=None):
    """A pack file or a theme folder. Returns {"ok", "checks", "facts"}."""
    if shell is not None and shell not in SHELLS:
        raise ValueError("--shell is w10m or android")
    if os.path.isdir(path):
        return check_theme_folder(path, shell)
    if not os.path.isfile(path):
        raise ValueError("%s is not a file or a folder" % path)
    return check_pack_file(path, shell)


# ------------------------------------------------------------------------------------------------------ a starter theme

THEME_CONF_TEMPLATE = """# The theme's settings (key=value, no spaces around =). The device reads this file at start.
name=%(name)s
author=
# the licence of your pictures, for example CC-BY-4.0 or CC0-1.0
licence=
version=0.1.0
# w10m, android or any
shell=any
# optional: an accent colour (an index 0 to 19, or #RRGGBB) and the base (1 dark, 0 light)
#accent=13
#dark=1
"""

THEME_README = """Put your pictures in images/ as PNG files named for what they replace (the names are in UI/PACK-NAMES.md of the repository).
Then `purr ui pack images --out pack.bin` and `purr ui check .` (add `--shell w10m` or `--shell android`); the theme is theme.conf plus pack.bin.
"""
