"""The layout side of the UI builder: the elements a person drags onto the screen, and the compile step that turns them into the screen file the device reads.

A layout is a list of elements in `ui.toml`, one `[[element]]` block each:

    [[element]]
    screen = "home"        # which screen of the UI it is on
    kind = "glyph"         # wallpaper, status, grid, dock, glyph, box, text
    glyph = "home"         # for a glyph: which picture (home, back, menu, drawer, ...)
    name = "home"
    x = 136                # position and size in the device's own pixels
    y = 216
    w = 48
    h = 24
    label = ""
    action = "home"        # what a tap should do (see ACTIONS)
    color = "#0050ef"

`to_screen_conf()` writes the `screen "name" { ... }` text PurrOS/main/purr_ui_config.c parses (labels, buttons, images, an app grid), so the device draws the layout
with no code of yours. What that file can say today is small, and the compile step is plain about it:

  * at most 16 widgets on a screen (the device's PURR_UI_MAX_WIDGETS);
  * a tap does nothing yet on the shells that read this file (the screen config has no actions: `purr_ui_config.c` says the buttons are "visual only"), so each
    action is kept in the layout and listed in the report, ready for the firmware that wires them;
  * lists, sliders, toggles, the notification shade and the lock slider have no widget of their own: they are drawn as plain buttons with their label.

`parse_screen_conf()` is a Python copy of the device's parser (same rules, same limits), used to check that what is written is what the device will read.
"""

import re

MAX_WIDGETS = 16
NAME_LEN = 31
TEXT_LEN = 63

KINDS = ("wallpaper", "status", "grid", "dock", "glyph", "box", "text")
ACTIONS = ("nothing", "drawer", "home", "back", "menu", "recent", "notifications", "lock", "app")
GLYPHS = ("home", "back", "menu", "recent", "drawer", "search", "power", "wifi", "battery", "signal", "clock", "date", "text", "button", "image", "tile", "list",
          "toggle", "slider", "dock", "shade", "lock", "statusbar", "wallpaper", "grid")

# a glyph element is drawn with this picture from the pack when the pack has it (the names of UI/PACK-NAMES.md)
GLYPH_ASSET = {"home": "kb_home", "back": "kb_back", "menu": "kb_menu", "drawer": "fr_dock_apps", "wifi": "st_w4", "battery": "st_b100", "signal": "st_s4"}
NO_WIDGET = ("list", "toggle", "slider", "shade", "lock")


def _hex(c, default="#000000"):
    return c if isinstance(c, str) and re.match(r"^#[0-9A-Fa-f]{6}$", c) else default


def element(d, i=0):
    """One element from a ui.toml block, with defaults; returns a clean dict. Raises ValueError for something that cannot be placed."""
    e = {"screen": str(d.get("screen", "home")), "kind": str(d.get("kind", "box")), "glyph": str(d.get("glyph", "tile")), "name": str(d.get("name", "element%d" % i)),
         "x": int(d.get("x", 0)), "y": int(d.get("y", 0)), "w": int(d.get("w", 0)), "h": int(d.get("h", 0)), "label": str(d.get("label", "")),
         "action": str(d.get("action", "nothing")), "color": _hex(d.get("color", "#2b2b2b"), "#2b2b2b")}
    if e["kind"] not in KINDS:
        raise ValueError("element %r: kind %r is not one of %s" % (e["name"], e["kind"], ", ".join(KINDS)))
    if e["action"] not in ACTIONS:
        raise ValueError("element %r: action %r is not one of %s" % (e["name"], e["action"], ", ".join(ACTIONS)))
    if e["w"] < 0 or e["h"] < 0:
        raise ValueError("element %r: width and height cannot be negative" % e["name"])
    return e


def elements_from(doc):
    """The elements in a parsed ui.toml (a dict), in drawing order."""
    return [element(d, i) for i, d in enumerate(doc.get("element", []) or [])]


def screens(elems):
    """The screen names in the order they first appear."""
    out = []
    for e in elems:
        if e["screen"] not in out:
            out.append(e["screen"])
    return out or ["home"]


def _q(s):
    return '"' + str(s).replace('"', "'").replace("\n", " ").replace("\r", " ") + '"'


def _name(s, taken):
    base = re.sub(r"[^A-Za-z0-9_-]+", "_", s).strip("_")[:NAME_LEN - 3] or "w"
    n, k = base, 2
    while n in taken:
        n = "%s_%d" % (base, k)
        k += 1
    taken.add(n)
    return n


def to_screen_conf(elems, screen, size, assets=()):
    """The `screen` block for one screen of the layout. `size` is (w, h) of the device; `assets` the picture names the theme will carry (an image widget is only
    written for a picture that exists, otherwise a labelled button stands in). Returns (text, notes, problems)."""
    sw, sh = size
    notes, problems, widgets, taken = [], [], [], set()
    mine = [e for e in elems if e["screen"] == screen]
    bg = "#000000"
    for e in mine:
        if e["kind"] == "wallpaper":
            bg = _hex(e["color"], "#000000")

    def clamp(e):
        x = max(0, min(e["x"], max(0, sw - 1)))
        y = max(0, min(e["y"], max(0, sh - 1)))
        w = e["w"] or 32
        h = e["h"] or 24
        if x + w > sw or y + h > sh:
            notes.append("%s runs past the %d x %d screen and will be cut off." % (e["name"], sw, sh))
        return x, y, w, h

    def add(kind, name, props):
        widgets.append((kind, _name(name, taken), props))

    for e in mine:
        if e["kind"] == "wallpaper":
            continue
        x, y, w, h = clamp(e)
        if e["action"] != "nothing":
            notes.append("%s: a tap should %s. Taps are not wired on the device yet; the action is kept in the layout." % (e["name"], e["action"]))
        if e["kind"] == "status":
            add("label", e["name"] + "_time", ["position = %d, %d" % (x + 6, y + 1), "text = " + _q(e["label"] or "9:41"), "color = #ffffff"])
            for k, a in enumerate(("st_w4", "st_b100")):
                if a in assets:
                    add("image", e["name"] + "_" + a, ["position = %d, %d" % (x + w - 22 - k * 22, y), "asset = " + a])
        elif e["kind"] == "grid":
            cols = 4
            rows = 2
            add("app_grid", e["name"], ["position = %d, %d" % (x, y), "size = %d, %d" % (max(16, w // cols), max(16, h // rows)), "columns = %d" % cols])
        elif e["kind"] == "dock":
            add("button", e["name"], ["position = %d, %d" % (x, y), "size = %d, %d" % (w, h), "text = " + _q(e["label"]), "color = " + _hex(e["color"], "#1f1f1f")])
        elif e["kind"] == "text":
            scale = max(1, min(4, round((h or 16) / 16.0)))
            add("label", e["name"], ["position = %d, %d" % (x, y), "text = " + _q(e["label"] or e["name"]), "color = #ffffff", "scale = %d" % scale])
        elif e["kind"] == "glyph":
            asset = GLYPH_ASSET.get(e["glyph"])
            if e["glyph"] in NO_WIDGET:
                notes.append("%s (%s) has no widget of its own on the device: it is drawn as a plain button." % (e["name"], e["glyph"]))
            if asset and asset in assets and e["glyph"] not in NO_WIDGET:
                add("image", e["name"], ["position = %d, %d" % (x + max(0, (w - 20) // 2), y + max(0, (h - 20) // 2)), "asset = " + asset])
            else:
                add("button", e["name"], ["position = %d, %d" % (x, y), "size = %d, %d" % (w, h), "text = " + _q(e["label"] or e["glyph"]), "color = " + _hex(e["color"])])
        else:
            if e["glyph"] in NO_WIDGET or e["kind"] == "box" and e["glyph"] in ("shade", "lock"):
                notes.append("%s (%s) has no widget of its own on the device: it is drawn as a plain button." % (e["name"], e["glyph"]))
            add("button", e["name"], ["position = %d, %d" % (x, y), "size = %d, %d" % (w, h), "text = " + _q(e["label"] or e["name"]), "color = " + _hex(e["color"])])
    if len(widgets) > MAX_WIDGETS:
        problems.append("The screen %r needs %d widgets and the device draws at most %d. Remove or merge %d (the status bar and a dock use several)." % (screen, len(widgets), MAX_WIDGETS, len(widgets) - MAX_WIDGETS))
    lines = ["screen %s {" % _q(screen), "    background = " + bg]
    for kind, name, props in widgets:
        lines.append("    %s %s {" % (kind, _q(name)))
        lines += ["        " + p for p in props]
        lines.append("    }")
    lines.append("}")
    return "\n".join(lines) + "\n", notes, problems


# ------------------------------------------------------------------------------------------------ a copy of the device's parser

def parse_screen_conf(text):
    """What PurrOS/main/purr_ui_config.c would read from `text`: {"name", "background", "widgets": [{type, name, x, y, w, h, text, asset, color, scale, columns}]}.
    Raises ValueError where the device would refuse the file (a syntax error), and returns extras it would skip in "ignored"."""
    out = {"name": "", "background": None, "widgets": [], "ignored": []}
    state, cur = "top", None
    for no, raw in enumerate(text.split("\n"), 1):
        t = raw.strip()
        if not t or t.startswith("#"):
            continue
        if t == "}":
            if state == "widget":
                state, cur = "screen", None
            elif state == "screen":
                state = "top"
            else:
                raise ValueError("line %d: unexpected '}'" % no)
            continue
        if t.endswith("{"):
            m = re.match(r'^(\w+)\s+"([^"]*)"\s*\{$', t)
            if not m:
                raise ValueError("line %d: malformed block header" % no)
            word, name = m.group(1), m.group(2)
            if state == "top":
                if word != "screen":
                    raise ValueError("line %d: expected 'screen', got %r" % (no, word))
                out["name"] = name
                state = "screen"
            elif state == "screen":
                if len(out["widgets"]) >= MAX_WIDGETS:
                    out["ignored"].append("line %d: too many widgets, ignoring %r" % (no, name))
                    cur = None
                elif word in ("label", "button", "image", "app_grid", "navbar", "panel"):
                    cur = {"type": word, "name": name[:NAME_LEN], "x": 0, "y": 0, "w": 0, "h": 0, "text": "", "asset": "", "color": None, "scale": 0, "columns": 0}
                    out["widgets"].append(cur)
                else:
                    out["ignored"].append("line %d: unknown widget type %r" % (no, word))
                    cur = None
                state = "widget"
            else:
                raise ValueError("line %d: blocks don't nest that deep" % no)
            continue
        if "=" not in t:
            raise ValueError("line %d: expected 'key = value', got %r" % (no, t))
        key, _, val = t.partition("=")
        key, vals = key.strip(), [v.strip().strip('"') for v in val.split(",")]
        if state == "screen" and key == "background":
            if re.match(r"^#[0-9A-Fa-f]{6}$", vals[0]):
                out["background"] = vals[0].lower()
        elif state == "widget":
            if cur is None:
                continue
            if key == "position" and len(vals) >= 2:
                cur["x"], cur["y"] = int(vals[0]), int(vals[1])
            elif key == "size" and len(vals) >= 2:
                cur["w"], cur["h"] = int(vals[0]), int(vals[1])
            elif key in ("text", "label"):
                cur["text"] = vals[0][:TEXT_LEN]
            elif key == "asset":
                cur["asset"] = vals[0][:NAME_LEN]
            elif key == "color" and re.match(r"^#[0-9A-Fa-f]{6}$", vals[0]):
                cur["color"] = vals[0].lower()
            elif key == "scale":
                cur["scale"] = int(vals[0])
            elif key == "columns":
                cur["columns"] = int(vals[0])
            else:
                out["ignored"].append("line %d: unknown or malformed property %r" % (no, key))
        else:
            raise ValueError("line %d: property outside any block" % no)
    if state != "top":
        raise ValueError("unclosed block at end of file")
    if not out["name"]:
        raise ValueError("no screen block found")
    return out


# ------------------------------------------------------------------------------------------------ starting layouts

def starter_layout(start, size):
    """The elements a new UI starts with, for a screen of `size`: 'blank', 'android' (home pages, dock, drawer button, key strip) or 'metro' (a big clock and tiles)."""
    sw, sh = size
    bar = max(12, sh // 13)
    if start == "blank":
        return [{"screen": "home", "kind": "wallpaper", "glyph": "wallpaper", "name": "wallpaper", "x": 0, "y": 0, "w": sw, "h": sh, "label": "", "action": "nothing", "color": "#101214"}]
    if start == "metro":
        return [
            {"screen": "home", "kind": "wallpaper", "glyph": "wallpaper", "name": "wallpaper", "x": 0, "y": 0, "w": sw, "h": sh, "label": "", "action": "nothing", "color": "#000000"},
            {"screen": "home", "kind": "text", "glyph": "clock", "name": "clock", "x": 16, "y": 12, "w": 140, "h": 40, "label": "09:41", "action": "nothing", "color": "#ffffff"},
            {"screen": "home", "kind": "box", "glyph": "tile", "name": "messages", "x": 16, "y": 64, "w": 92, "h": 92, "label": "Messages", "action": "app", "color": "#0050ef"},
            {"screen": "home", "kind": "box", "glyph": "tile", "name": "notes", "x": 116, "y": 64, "w": 92, "h": 92, "label": "Notes", "action": "app", "color": "#60a917"},
            {"screen": "home", "kind": "box", "glyph": "tile", "name": "settings", "x": 216, "y": 64, "w": 88, "h": 92, "label": "Settings", "action": "app", "color": "#647687"},
        ]
    e = [
        {"screen": "home", "kind": "wallpaper", "glyph": "wallpaper", "name": "wallpaper", "x": 0, "y": 0, "w": sw, "h": sh, "label": "", "action": "nothing", "color": "#0b1c10"},
        {"screen": "home", "kind": "status", "glyph": "statusbar", "name": "status bar", "x": 0, "y": 0, "w": sw, "h": 18, "label": "", "action": "nothing", "color": "#000000"},
        {"screen": "home", "kind": "grid", "glyph": "grid", "name": "app grid", "x": 16, "y": 30, "w": max(16, sw - 32), "h": max(16, sh - 112), "label": "", "action": "nothing", "color": "#000000"},
        {"screen": "home", "kind": "dock", "glyph": "dock", "name": "dock", "x": 0, "y": sh - bar - 48, "w": sw, "h": 44, "label": "", "action": "nothing", "color": "#101214"},
        {"screen": "home", "kind": "glyph", "glyph": "drawer", "name": "app drawer", "x": (sw - 40) // 2, "y": sh - bar - 44, "w": 40, "h": 36, "label": "", "action": "drawer", "color": "#60a917"},
        {"screen": "home", "kind": "box", "glyph": "tile", "name": "key strip", "x": 0, "y": sh - bar, "w": sw, "h": bar, "label": "", "action": "nothing", "color": "#000000"},
    ]
    for i, (g, a) in enumerate((("back", "back"), ("home", "home"), ("menu", "menu"))):
        e.append({"screen": "home", "kind": "glyph", "glyph": g, "name": g, "x": (sw - 48) // 2 + (i - 1) * 72, "y": sh - bar, "w": 48, "h": bar, "label": "", "action": a, "color": "#0050ef"})
    return e
