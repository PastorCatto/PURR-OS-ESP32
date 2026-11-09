"""Check a TrueType font file the way the OS means to before it hands the bytes to the renderer.

Standard library only: the sfnt tables are read here, not with fontTools. The renderer on the device (LVGL Tiny TTF,
which is stb_truetype) draws outline glyphs from `glyf` (TrueType) or `CFF` and ignores colour and bitmap tables, so this
check says so. It is a gate for files somebody else made: every offset and length is tested against the file size
before it is read, and a file that fails gets a plain message and a non-zero exit from `purr font check`.

Nothing here has been run on a device; the kernel side of "fonts from files" is designed, not built (docs/21-fonts.md).
"""

import os
import struct

DEFAULT_MAX_BYTES = 1 << 20          # 1 MB: the OS reads the whole file into PSRAM
MAX_TABLES = 64                      # real fonts have 10 to 25
SFNT_TRUETYPE = (0x00010000, 0x74727565)   # 1.0 and 'true'
SFNT_CFF = 0x4F54544F                # 'OTTO'
REQUIRED = ("head", "hhea", "maxp", "cmap", "hmtx")
COLOUR_TABLES = ("CBDT", "CBLC", "COLR", "CPAL", "sbix", "SVG ", "EBDT", "EBLC")
LATIN1 = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100))      # 191 code points
SHELL_SYMBOLS = [                                               # what the shells draw as text, as far as is known
    0x2190, 0x2191, 0x2192, 0x2193,     # arrows
    0x2713, 0x2714, 0x2717,             # check, heavy check, ballot x
    0x2022, 0x2026, 0x25CF, 0x25CB,     # bullet, ellipsis, circles
    0x25B6, 0x25C0, 0x25B2, 0x25BC,     # triangles
    0x2605, 0x2665, 0x266A,             # star, heart, note
    0x00D7, 0x2212, 0x00B0, 0x20AC,     # times, minus, degree, euro
]
NAME_IDS = {0: "copyright", 1: "family", 2: "style", 4: "full name", 5: "version", 13: "licence", 14: "licence URL"}


class FontError(ValueError):
    """The file is not a font the OS would load."""


def _u16(d, o):
    return struct.unpack_from(">H", d, o)[0]


def _u32(d, o):
    return struct.unpack_from(">I", d, o)[0]


def _need(data, off, length, what):
    if off < 0 or length < 0 or off + length > len(data):
        raise FontError("%s runs past the end of the file" % what)


def read_directory(data):
    """The sfnt table directory as {tag: (offset, length)}; raises FontError for anything out of bounds."""
    if len(data) < 12:
        raise FontError("the file is too short to be a font (%d bytes)" % len(data))
    magic = _u32(data, 0)
    if data[:4] == b"ttcf":
        raise FontError("this is a font collection (.ttc); the OS loads single fonts only")
    if data[:4] == b"wOFF" or data[:4] == b"wOF2":
        raise FontError("this is a web font (WOFF); the OS loads plain TrueType (.ttf) files")
    if magic not in SFNT_TRUETYPE and magic != SFNT_CFF:
        raise FontError("this does not start like a TrueType or OpenType font")
    count = _u16(data, 4)
    if count == 0 or count > MAX_TABLES:
        raise FontError("the table directory says %d tables (expected 1 to %d)" % (count, MAX_TABLES))
    _need(data, 12, 16 * count, "the table directory")
    table = {}
    first_data = 12 + 16 * count
    for i in range(count):
        tag_b, _sum, off, length = struct.unpack_from(">4sIII", data, 12 + 16 * i)
        tag = tag_b.decode("latin-1")
        if tag in table:
            raise FontError("the table '%s' is listed twice" % tag)
        if off < first_data:
            raise FontError("the table '%s' starts inside the directory" % tag)
        _need(data, off, length, "the table '%s'" % tag)
        table[tag] = (off, length)
    spans = sorted((o, o + l, t) for t, (o, l) in table.items() if l)
    for (a0, a1, at), (b0, b1, bt) in zip(spans, spans[1:]):
        if b0 < a1:
            raise FontError("the tables '%s' and '%s' overlap" % (at, bt))
    return table


def _table(data, tab, tag):
    off, length = tab[tag]
    return data[off:off + length]


def _cmap(data, tab, num_glyphs):
    """The code points the font maps to a real glyph, from the best cmap subtable the renderer can read."""
    cm = _table(data, tab, "cmap")
    if len(cm) < 4:
        raise FontError("the cmap table is too short")
    n = _u16(cm, 2)
    if n == 0 or 4 + 8 * n > len(cm):
        raise FontError("the cmap table lists %d subtables that do not fit in it" % n)
    subs = []
    for i in range(n):
        pid, eid, off = struct.unpack_from(">HHI", cm, 4 + 8 * i)
        if off + 4 > len(cm):
            raise FontError("a cmap subtable starts past the end of the table")
        subs.append((pid, eid, off, _u16(cm, off)))
    # the renderer reads formats 0, 4, 6 and 12; this check reads 4 and 12 (nearly every font has one of them)
    pref = [(3, 10), (0, 4), (0, 6), (0, 3), (3, 1), (0, 2), (0, 1), (0, 0)]
    usable = [s for s in subs if s[3] in (4, 12)]
    if not usable:
        raise FontError("no Unicode cmap subtable of format 4 or 12 (found formats %s)" % sorted({s[3] for s in subs}))
    usable.sort(key=lambda s: (pref.index((s[0], s[1])) if (s[0], s[1]) in pref else len(pref), -s[3]))
    pid, eid, off, fmt = usable[0]
    codes = set()
    if fmt == 4:
        sub = cm[off:]
        if len(sub) < 14:
            raise FontError("the cmap format 4 subtable is truncated")
        length = _u16(sub, 2)
        sub = sub[:length] if 14 <= length <= len(sub) else sub
        segx2 = _u16(sub, 6)
        seg = segx2 // 2
        if seg == 0 or 16 + 4 * segx2 > len(sub):
            raise FontError("the cmap format 4 subtable has %d segments that do not fit in it" % seg)
        ends, starts = 14, 16 + segx2
        deltas, ranges = starts + segx2, starts + 2 * segx2
        budget = 0x110000
        for i in range(seg):
            end, start = _u16(sub, ends + 2 * i), _u16(sub, starts + 2 * i)
            delta, ro = struct.unpack_from(">hH", sub, deltas + 2 * i)[0], _u16(sub, ranges + 2 * i)
            if start > end:
                raise FontError("a cmap segment ends before it starts")
            budget -= end - start + 1
            if budget < 0:
                raise FontError("the cmap segments overlap (they cover more code points than exist)")
            for c in range(start, end + 1):
                if c == 0xFFFF:
                    continue
                if ro == 0:
                    gid = (c + delta) & 0xFFFF
                else:
                    p = ranges + 2 * i + ro + 2 * (c - start)
                    if p + 2 > len(sub):
                        raise FontError("a cmap segment points outside its subtable")
                    gid = _u16(sub, p)
                    if gid:
                        gid = (gid + delta) & 0xFFFF
                if 0 < gid < num_glyphs:
                    codes.add(c)
    else:
        sub = cm[off:]
        if len(sub) < 16:
            raise FontError("the cmap format 12 subtable is truncated")
        ngroups = _u32(sub, 12)
        if ngroups > (len(sub) - 16) // 12:
            raise FontError("the cmap format 12 subtable has %d groups that do not fit in it" % ngroups)
        prev_end = -1
        for i in range(ngroups):
            start, end, gid = struct.unpack_from(">III", sub, 16 + 12 * i)
            if start > end or end > 0x10FFFF or start <= prev_end:
                raise FontError("a cmap group has a bad range, or the groups are out of order")
            prev_end = end
            if gid >= num_glyphs:
                continue
            end = min(end, start + (num_glyphs - 1 - gid))
            if gid == 0:                 # glyph 0 is .notdef, not a character
                start += 1
            codes.update(range(start, end + 1))
    return codes, (pid, eid, fmt)


def ranges(codes):
    """Sorted code points as [(first, last), ...]."""
    out = []
    for c in sorted(codes):
        if out and c == out[-1][1] + 1:
            out[-1][1] = c
        else:
            out.append([c, c])
    return [tuple(r) for r in out]


def fmt_ranges(rs, limit=12):
    parts = ["U+%04X" % a if a == b else "U+%04X-%04X" % (a, b) for a, b in rs[:limit]]
    if len(rs) > limit:
        parts.append("... (%d more ranges)" % (len(rs) - limit))
    return ", ".join(parts)


def _names(data, tab):
    """Selected strings of the `name` table (first match per id, English or Windows first)."""
    if "name" not in tab:
        return {}
    nm = _table(data, tab, "name")
    if len(nm) < 6:
        return {}
    count, base = _u16(nm, 2), _u16(nm, 4)
    if 6 + 12 * count > len(nm) or base > len(nm):
        return {}
    got = {}
    for i in range(count):
        pid, eid, lang, nid, length, off = struct.unpack_from(">HHHHHH", nm, 6 + 12 * i)
        if nid not in NAME_IDS or base + off + length > len(nm):
            continue
        raw = nm[base + off:base + off + length]
        try:
            text = raw.decode("utf-16-be") if pid in (0, 3) else raw.decode("mac-roman") if pid == 1 else None
        except UnicodeDecodeError:
            text = None
        if not text:
            continue
        rank = (0 if (pid == 3 and lang == 0x409) else 1 if pid in (0, 3) else 2)
        if nid not in got or rank < got[nid][0]:
            got[nid] = (rank, " ".join(text.split()))
    return {NAME_IDS[k]: v[1] for k, v in got.items()}


def check_bytes(data, max_bytes=DEFAULT_MAX_BYTES, size=None):
    """Validate font bytes. Returns {"ok", "checks": [{status, name, text}], "facts": {...}}; never raises for bad input.
    `size` is the real size when `data` was cut at the cap."""
    checks = []
    size = len(data) if size is None else size
    facts = {"bytes": size}

    def add(status, name, text):
        checks.append({"status": status, "name": name, "text": text})

    def done():
        return {"ok": not any(c["status"] == "bad" for c in checks), "checks": checks, "facts": facts}

    if size > max_bytes:
        add("bad", "size", "%s bytes is over the cap of %s bytes; the OS refuses it before reading it" % (format(size, ","), format(max_bytes, ",")))
        return done()
    add("ok", "size", "%s bytes (cap %s)" % (format(size, ","), format(max_bytes, ",")))
    try:
        tab = read_directory(data)
    except FontError as e:
        add("bad", "format", str(e))
        return done()
    facts["tables"] = sorted(tab)
    add("ok", "table directory", "%d tables, all inside the file" % len(tab))

    missing = [t for t in REQUIRED if t not in tab]
    if "CFF2" in tab:
        add("bad", "outlines", "CFF2 (variable PostScript) outlines are not supported by the renderer")
        return done()
    elif "glyf" in tab:
        if "loca" not in tab:
            missing.append("loca")
        facts["outlines"] = "TrueType (glyf)"
    elif "CFF " in tab:
        facts["outlines"] = "CFF"
    else:
        missing.append("glyf or CFF")
    if missing:
        add("bad", "required tables", "missing: " + ", ".join(missing))
        return done()
    add("ok", "required tables", "head, hhea, maxp, cmap, hmtx and %s are present" % ("glyf and loca" if facts["outlines"].startswith("True") else "CFF"))

    try:
        head, hhea, maxp = _table(data, tab, "head"), _table(data, tab, "hhea"), _table(data, tab, "maxp")
        if len(head) < 54 or _u32(head, 12) != 0x5F0F3CF5:
            raise FontError("the head table is damaged (bad magic number or too short)")
        upem, loc_fmt = _u16(head, 18), struct.unpack_from(">h", head, 50)[0]
        if not 16 <= upem <= 16384:
            raise FontError("units per em is %d (expected 16 to 16384)" % upem)
        if loc_fmt not in (0, 1):
            raise FontError("the head table's loca format is %d (expected 0 or 1)" % loc_fmt)
        if len(maxp) < 6:
            raise FontError("the maxp table is too short")
        num_glyphs = _u16(maxp, 4)
        if num_glyphs == 0:
            raise FontError("the font has no glyphs")
        if len(hhea) < 36:
            raise FontError("the hhea table is too short")
        nmetrics = _u16(hhea, 34)
        if nmetrics == 0 or nmetrics > num_glyphs:
            raise FontError("hhea says %d horizontal metrics for %d glyphs" % (nmetrics, num_glyphs))
        if len(_table(data, tab, "hmtx")) < 4 * nmetrics:
            raise FontError("the hmtx table is shorter than the %d metrics hhea promises" % nmetrics)
        if facts["outlines"].startswith("True"):
            loca = _table(data, tab, "loca")
            esize = 4 if loc_fmt else 2
            if len(loca) < esize * (num_glyphs + 1):
                raise FontError("the loca table is shorter than %d glyphs need" % num_glyphs)
            vals = struct.unpack_from(">%d%s" % (num_glyphs + 1, "I" if loc_fmt else "H"), loca, 0)
            if not loc_fmt:
                vals = [v * 2 for v in vals]
            if any(b < a for a, b in zip(vals, vals[1:])):
                raise FontError("the loca table's offsets go backwards")
            if vals[-1] > tab["glyf"][1]:
                raise FontError("the loca table points past the end of the glyf table")
    except FontError as e:
        add("bad", "tables", str(e))
        return done()
    facts.update(glyphs=num_glyphs, units_per_em=upem)
    add("ok", "tables", "%s glyphs, %d units per em, glyph offsets inside the glyph data" % (format(num_glyphs, ","), upem))

    colour = [t.strip() for t in COLOUR_TABLES if t in tab]
    facts["colour_tables"] = colour
    if colour:
        add("warn", "colour", "has %s: Tiny TTF does not support colour or bitmap tables and draws outlines only; colour emoji must be a picture pack instead" % ", ".join(colour))
    else:
        add("ok", "colour", "outline glyphs only (no colour or bitmap tables)")
    if "fvar" in tab:
        add("warn", "variable font", "has variation tables: only the default instance is drawn; make a static instance first (fontTools varLib.instancer)")

    try:
        codes, (pid, eid, fmt) = _cmap(data, tab, num_glyphs)
    except FontError as e:
        add("bad", "cmap", str(e))
        return done()
    if not codes:
        add("bad", "cmap", "no code point maps to a glyph")
        return done()
    rs = ranges(codes)
    facts.update(codepoints=len(codes), ranges=rs)
    latin = sum(1 for c in LATIN1 if c in codes)
    shell = [c for c in SHELL_SYMBOLS if c in codes]
    facts.update(latin1=latin, latin1_of=len(LATIN1), shell_symbols=len(shell), shell_symbols_of=len(SHELL_SYMBOLS),
                 shell_missing=[c for c in SHELL_SYMBOLS if c not in codes])
    add("ok", "coverage", "%s code points in %d ranges (cmap platform %d, encoding %d, format %d): %s" % (format(len(codes), ","), len(rs), pid, eid, fmt, fmt_ranges(rs)))
    add("ok" if latin == len(LATIN1) else "warn" if latin else "info", "Latin-1", "%d of %d (%d%%)" % (latin, len(LATIN1), round(100.0 * latin / len(LATIN1))))
    add("info", "shell symbols", "%d of %d (arrows, ticks, bullets, triangles, star, heart, note, signs)" % (len(shell), len(SHELL_SYMBOLS))
        + ("; missing: " + " ".join("U+%04X" % c for c in facts["shell_missing"]) if facts["shell_missing"] else ""))

    names = _names(data, tab)
    facts["names"] = names
    if names.get("family"):
        add("info", "name", names["family"] + (" " + names["style"] if names.get("style") else "") + (", " + names["version"] if names.get("version") else ""))
    add("ok" if names.get("copyright") else "warn", "copyright", names.get("copyright") or "none in the name table")
    lic = names.get("licence")
    add("ok" if lic else "warn", "licence", (lic[:200] + ("..." if len(lic) > 200 else "")) if lic else "none in the name table: the repository entry must carry the licence name")
    if names.get("licence URL"):
        add("info", "licence URL", names["licence URL"])
    return done()


def check_file(path, max_bytes=DEFAULT_MAX_BYTES):
    """Check a file on disk; a file over the cap is refused from its size alone, without being read."""
    size = os.path.getsize(path)
    with open(path, "rb") as fh:
        data = fh.read(0 if size > max_bytes else size)
    return check_bytes(data, max_bytes, size)
