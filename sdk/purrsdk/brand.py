"""Flavors: the names and look of the boot screens and the system come from a brand.toml.

    [brand]    product, recovery, codename, logo, logo_accent_from
    [colours]  accent, text, dim, ok, warn          (boot screens, #RRGGBB)
    [shell]    accent                                (a Windows Phone palette name)

`check()` says what is wrong in plain words; `header()` writes the C header the boot package, the loader,
KittenOS and the shell are built with (PURR_BRAND_HEADER, see PurrOS/components/kernel/include/purr_brand.h
and bootpkg/src/pkg_brand.h).
"""

import re

from . import minitoml

# The shell's palette, in the order the theme lists it (PurrOS/main/purr_w10m_theme.c).
ACCENTS = [("Lime", 0xA4C400), ("Green", 0x60A917), ("Emerald", 0x00BA00), ("Teal", 0x00ABA9),
           ("Cyan", 0x1BA1E2), ("Cobalt", 0x0050EF), ("Indigo", 0x6A00FF), ("Violet", 0xAA00FF),
           ("Pink", 0xF472D0), ("Magenta", 0xD80073), ("Crimson", 0xA20025), ("Red", 0xE51400),
           ("Orange", 0xFA6800), ("Amber", 0xF0A30A), ("Yellow", 0xE3C800), ("Brown", 0x825A2C),
           ("Olive", 0x6D8764), ("Steel", 0x647687), ("Mauve", 0x76608A), ("Taupe", 0x87794E)]

DEFAULT = {
    "brand": {"product": "PURR OS", "recovery": "KittenOS", "codename": "", "logo": "PURR OS!", "logo_accent_from": 7},
    "colours": {"accent": "#FFB400", "text": "#FFFFFF", "dim": "#949694", "ok": "#52CA7B", "warn": "#FFB400"},
    "shell": {"accent": "Cobalt"},
}

TEMPLATE = """# brand.toml: the names and the look of your flavor of PURR OS.
# Change anything, then run `purr brand build brand.toml` (or use the Brand tool in Studio to see it first).

[brand]
product   = "PURR OS"        # the full system's name: boot menu, About, messages
recovery  = "KittenOS"       # the recovery system's name
codename  = ""               # the board line under the logo (leave empty to keep the board's own)
logo      = "PURR OS!"       # the wordmark on the boot screens, drawn bold and italic
logo_accent_from = 7         # characters from this position on use the accent colour (-1 for none)

[colours]                    # boot screens, as #RRGGBB
accent = "#FFB400"           # the logo's accent, the selection border, the default tag
text   = "#FFFFFF"
dim    = "#949694"
ok     = "#52CA7B"
warn   = "#FFB400"

[shell]
accent = "Cobalt"            # the shell's first accent colour: one of the Windows Phone names
"""

LIMITS = {"product": 20, "recovery": 16, "codename": 16, "logo": 14}
HEX = re.compile(r"^#[0-9A-Fa-f]{6}$")


def load(path):
    return merge(minitoml.load(path))


def merge(user):
    out = {k: dict(v) for k, v in DEFAULT.items()}
    for section, vals in (user or {}).items():
        if isinstance(vals, dict):
            out.setdefault(section, {}).update(vals)
    return out


def _lum(rgb):
    def lin(c):
        c = c / 255.0
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    return 0.2126 * lin((rgb >> 16) & 255) + 0.7152 * lin((rgb >> 8) & 255) + 0.0722 * lin(rgb & 255)


def contrast_on_black(hexcolour):
    return (_lum(int(hexcolour[1:], 16)) + 0.05) / 0.05


def check(brand):
    """A list of {"level": "bad|warn", "field": "brand.logo", "text": "..."}; empty means all good."""
    issues = []

    def add(level, field, text):
        issues.append({"level": level, "field": field, "text": text})

    b, c, s = brand.get("brand", {}), brand.get("colours", {}), brand.get("shell", {})
    for key in ("product", "recovery", "logo", "codename"):
        v = b.get(key, "")
        if not isinstance(v, str):
            add("bad", "brand." + key, "This needs to be text in quotes.")
            continue
        if key != "codename" and not v:
            add("bad", "brand." + key, "This cannot be empty.")
        if any(ord(ch) < 32 or ord(ch) > 126 or ch in '"\\' for ch in v):
            add("bad", "brand." + key, "Use plain letters, digits and punctuation, without quotes or backslashes: the boot screens' font has nothing else.")
        if len(v) > LIMITS[key]:
            add("bad", "brand." + key, "That is %d characters; %d is the most that fits." % (len(v), LIMITS[key]))
    logo = b.get("logo", "")
    if isinstance(logo, str) and len(logo) > 9:
        add("warn", "brand.logo", "Wordmarks longer than 9 characters shrink to fit the 320-pixel line, so they print smaller.")
    af = b.get("logo_accent_from", 7)
    if not isinstance(af, int) or isinstance(af, bool):
        add("bad", "brand.logo_accent_from", "This needs to be a whole number (use -1 for no accent).")
    for key in ("accent", "text", "dim", "ok", "warn"):
        v = c.get(key, "")
        if not isinstance(v, str) or not HEX.match(v):
            add("bad", "colours." + key, "Write the colour as #RRGGBB, for example #FFB400.")
    if not [i for i in issues if i["field"] == "colours.accent"] and contrast_on_black(c["accent"]) < 3.0:
        add("warn", "colours.accent", "That accent is hard to see on the black boot screens. Try a lighter one.")
    if not [i for i in issues if i["field"] == "colours.dim"] and contrast_on_black(c["dim"]) < 3.0:
        add("warn", "colours.dim", "That dim colour is hard to read on black.")
    if s.get("accent") not in [a[0] for a in ACCENTS]:
        add("bad", "shell.accent", "Pick one of: " + ", ".join(a[0] for a in ACCENTS) + ".")
    return issues


def rgb565(hexcolour):
    v = int(hexcolour[1:], 16)
    r, g, b = (v >> 16) & 255, (v >> 8) & 255, v & 255
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def header(brand):
    """The C header text, or raises ValueError listing the problems if the brand has any 'bad' issue."""
    bad = [i for i in check(brand) if i["level"] == "bad"]
    if bad:
        raise ValueError("; ".join("%s: %s" % (i["field"], i["text"]) for i in bad))
    b, c, s = brand["brand"], brand["colours"], brand["shell"]
    accent_index = [a[0] for a in ACCENTS].index(s["accent"])
    af = b["logo_accent_from"]
    lines = [
        "/* Generated by `purr brand build`. Edit brand.toml, not this file. */",
        "#ifndef PURR_BRAND_GENERATED_H",
        "#define PURR_BRAND_GENERATED_H",
        "",
        '#define PURR_BRAND_PRODUCT "%s"' % b["product"],
        '#define PURR_BRAND_RECOVERY "%s"' % b["recovery"],
        "#define PURR_BRAND_ACCENT_INDEX %d" % accent_index,
        "",
        '#define PKG_BRAND_PRODUCT "%s"' % b["product"],
        '#define PKG_BRAND_RECOVERY "%s"' % b["recovery"],
        '#define PKG_BRAND_LOGO "%s"' % b["logo"],
        "#define PKG_BRAND_LOGO_ACCENT_FROM %d" % (af if af >= 0 else 1000),
    ]
    if b.get("codename"):
        lines.append('#define PKG_BRAND_CODENAME "%s"' % b["codename"])
    lines += ["#define PKG_BRAND_COLOUR_ACCENT 0x%04X" % rgb565(c["accent"]),
              "#define PKG_BRAND_COLOUR_TEXT 0x%04X" % rgb565(c["text"]),
              "#define PKG_BRAND_COLOUR_DIM 0x%04X" % rgb565(c["dim"]),
              "#define PKG_BRAND_COLOUR_OK 0x%04X" % rgb565(c["ok"]),
              "", "#endif", ""]
    return "\n".join(lines)
