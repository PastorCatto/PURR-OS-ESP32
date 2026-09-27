#!/usr/bin/env python3
"""
uiconf.py — parser/validator/compiler for the `.pui` declarative UI-config
format (see /home/PastorCatto/.claude/plans/delightful-exploring-hearth.md
for the full design). Deliberately separate from the existing `.pcat`
parser (purrstrap.py/catstrap.py/modulestrap.py's own `parse_pcat()`,
identical in all three) — that grammar is flat `key = value` under
`[section]` headers, and this format genuinely needs nesting and
repetition (screens -> sections -> widgets -> items), which `.pcat`'s
grammar structurally cannot express. A real recursive-descent
tokenizer/parser, not a regex flattener.

Public entry point: compile_app_ui(app_dir) -> {screen_name: bytes}.
Called from catstrap.py's build_app() for claw-tier apps (see that file's
own new step) once per app, over its whole `ui/*.pui` directory as one
unit — screen-name references (`open_screen` targets) can cross files
within the same app, so validation has to see the whole set at once, not
one file at a time.
"""

import os
import re
import struct

# ── Fixed vocabularies ──────────────────────────────────────────────────
# Hand-maintained name tables, resolved to small integer IDs at compile
# time so the on-device interpreter never parses or hashes a symbol name
# at runtime — same "hand-maintained parallel table in Python and C"
# shape UI_BACKEND_MAP (purrstrap.py) already carries for device.pcat's
# `ui=` values. Each C-side table lives in purr_uiconf_core.c and MUST be
# kept in the same order/membership as these — see that file's own top
# comment for the reverse-direction pointer back here.

WIDGET_TYPES = ["text", "list", "button"]          # index 0/1/2 == WIDGET_TEXT/WIDGET_LIST/WIDGET_BUTTON in the binary format
BINDINGS = [
    "clock_hhmm", "battery_percent", "battery_voltage_mv",
    "free_ram_kb", "uptime_ms", "wifi_connected", "version", "current_user",
]
SOURCES = ["app_list", "user_list", "paired_devices", "mesh_backend_options", "module_list"]
CONDITIONS = ["wifi_available", "lora_available", "is_admin", "sd_available", "flash_available"]
# exit_app — added for the Phase 2 apps: a single-screen app (diagnostics'
# own first-pass scope) has nowhere to `open_screen` back TO, so its
# on-screen Back button needs a real way to signal "I'm done" up to
# app_manager instead. Takes no `target` (see check_handler_attrs below).
ACTIONS = ["open_screen", "launch_app", "run_command", "exit_app"]
EVENTS = ["select", "back", "activate", "change"]

# ── Tokenizer ────────────────────────────────────────────────────────────

TOKEN_RE = re.compile(r'''
      (?P<ws>\s+)
    | (?P<comment>\#[^\n]*)
    | (?P<string>"(?:[^"\\]|\\.)*")
    | (?P<number>-?\d+(?:\.\d+)?)
    | (?P<ident>[A-Za-z_][A-Za-z0-9_\.\$]*)
    | (?P<lbrace>\{)
    | (?P<rbrace>\})
    | (?P<eq>=)
    | (?P<semi>;)
''', re.VERBOSE)


class PuiError(Exception):
    def __init__(self, msg, line):
        super().__init__(f"line {line}: {msg}")
        self.line = line


class Token:
    __slots__ = ("kind", "value", "line")
    def __init__(self, kind, value, line):
        self.kind = kind
        self.value = value
        self.line = line
    def __repr__(self):
        return f"Token({self.kind!r}, {self.value!r})"


def tokenize(text):
    tokens = []
    line = 1
    pos = 0
    n = len(text)
    while pos < n:
        m = TOKEN_RE.match(text, pos)
        if not m:
            bad = text[pos]
            raise PuiError(f"unexpected character {bad!r}", line)
        pos = m.end()
        kind = m.lastgroup
        val = m.group()
        newlines = val.count("\n")
        if kind in ("ws", "comment"):
            line += newlines
            continue
        if kind == "string":
            val = val[1:-1].replace('\\"', '"').replace("\\\\", "\\")
        tokens.append(Token(kind, val, line))
        line += newlines
    tokens.append(Token("eof", None, line))
    return tokens


# ── Parser — produces a plain nested-dict AST ────────────────────────────
# Node shape: {"kind": str, "name": str|None, "attrs": {str: value},
#              "children": [node, ...], "line": int}
# An attr value is either a scalar (str/float/bool) or another such dict
# (for an inline `on_activate = { action = ...; target = ... }` value —
# see the grammar's own "attr value can be a block" note in the plan).

BLOCK_KEYWORDS = {"screen", "section", "widget", "item", "on"}


class Parser:
    def __init__(self, tokens, filename):
        self.tokens = tokens
        self.pos = 0
        self.filename = filename

    def peek(self):
        return self.tokens[self.pos]

    def advance(self):
        t = self.tokens[self.pos]
        self.pos += 1
        return t

    def expect(self, kind):
        t = self.advance()
        if t.kind != kind:
            raise PuiError(f"expected {kind}, got {t.kind} ({t.value!r})", t.line)
        return t

    def parse_file(self):
        screens = []
        while self.peek().kind != "eof":
            t = self.peek()
            if t.kind == "ident" and t.value == "screen":
                screens.append(self.parse_block())
            else:
                raise PuiError(f"expected top-level 'screen', got {t.value!r}", t.line)
        return screens

    def parse_block(self):
        kw_tok = self.advance()   # "screen" / "section" / "widget" / "item" / "on"
        kw = kw_tok.value
        name = None
        if kw != "item":
            # screen/section/widget take an IDENT name; "on" takes an
            # event-name IDENT (validated against EVENTS later, not here —
            # keep the parser itself vocabulary-agnostic).
            name_tok = self.expect("ident")
            name = name_tok.value
        self.expect("lbrace")
        attrs = {}
        children = []
        while self.peek().kind != "rbrace":
            t = self.peek()
            if t.kind == "eof":
                raise PuiError(f"unterminated '{kw}' block (opened line {kw_tok.line})", t.line)
            if t.kind == "ident" and t.value in BLOCK_KEYWORDS:
                children.append(self.parse_block())
                continue
            attr_name, attr_val = self.parse_attr()
            attrs[attr_name] = attr_val
        self.expect("rbrace")
        return {"kind": kw, "name": name, "attrs": attrs, "children": children, "line": kw_tok.line}

    def parse_attr(self):
        key_tok = self.expect("ident")
        self.expect("eq")
        val = self.parse_value()
        if self.peek().kind == "semi":
            self.advance()
        return key_tok.value, val

    def parse_value(self):
        t = self.peek()
        if t.kind == "lbrace":
            # Inline block value — e.g. `on_activate = { action = ...; target = ... }`.
            self.advance()
            attrs = {}
            while self.peek().kind != "rbrace":
                if self.peek().kind == "eof":
                    raise PuiError("unterminated inline block value", self.peek().line)
                k, v = self.parse_attr()
                attrs[k] = v
            self.expect("rbrace")
            return attrs
        if t.kind == "string":
            self.advance()
            return t.value
        if t.kind == "number":
            self.advance()
            return float(t.value) if "." in t.value else int(t.value)
        if t.kind == "ident":
            self.advance()
            if t.value in ("true", "false"):
                return t.value == "true"
            return t.value   # bare identifier (e.g. `align = center`, `type = text`)
        raise PuiError(f"expected a value, got {t.kind} ({t.value!r})", t.line)


def parse_pui(path):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    tokens = tokenize(text)
    return Parser(tokens, path).parse_file()


# ── Validation ───────────────────────────────────────────────────────────
# Every error here is a BUILD-TIME failure — the on-device interpreter
# only ever loads a well-formed .puib and never has to detect or degrade
# from a malformed one (see the plan's own "build-time errors" framing).

def _split_conditions(expr):
    # "lora_available, is_admin" -> [("lora_available", False), ("is_admin", False)]
    # "!wifi_available" -> [("wifi_available", True)]
    out = []
    for part in expr.split(","):
        part = part.strip()
        if not part:
            continue
        neg = part.startswith("!")
        if neg:
            part = part[1:].strip()
        out.append((part, neg))
    return out


def validate_screens(screens, app_name):
    errors = []
    screen_names = set()
    for s in screens:
        if s["name"] in screen_names:
            errors.append(f"{app_name}: duplicate screen name '{s['name']}' (line {s['line']})")
        screen_names.add(s["name"])

    def check_conditions(attrs, line):
        for key in ("visible_if", "enabled_if"):
            if key not in attrs:
                continue
            for cond, _neg in _split_conditions(str(attrs[key])):
                if cond not in CONDITIONS:
                    errors.append(f"{app_name}: unknown condition '{cond}' in {key} (line {line})")

    def check_handler_attrs(attrs, line):
        action = attrs.get("action")
        if action is None:
            errors.append(f"{app_name}: 'on' handler missing 'action' (line {line})")
            return
        if action not in ACTIONS:
            errors.append(f"{app_name}: unknown action '{action}' (line {line})")
            return
        target = attrs.get("target")
        if action == "open_screen":
            if not target:
                errors.append(f"{app_name}: open_screen missing 'target' (line {line})")
            elif target not in screen_names:
                errors.append(f"{app_name}: open_screen target '{target}' is not a known screen (line {line})")
        elif action == "run_command" and not target:
            errors.append(f"{app_name}: run_command missing 'target' (line {line})")
        # launch_app's target is "$item" (resolved at runtime against the
        # activated row) or a literal app name — either is a bare string,
        # nothing to check at compile time.

    def walk_widget(node):
        wtype = node["attrs"].get("type")
        if wtype is None:
            errors.append(f"{app_name}: widget '{node['name']}' missing 'type' (line {node['line']})")
        elif wtype not in WIDGET_TYPES:
            errors.append(f"{app_name}: widget '{node['name']}' has unknown type '{wtype}' (line {node['line']})")
        if "bind" in node["attrs"] and node["attrs"]["bind"] not in BINDINGS:
            errors.append(f"{app_name}: unknown bind '{node['attrs']['bind']}' (line {node['line']})")
        if "source" in node["attrs"] and node["attrs"]["source"] not in SOURCES:
            errors.append(f"{app_name}: unknown source '{node['attrs']['source']}' (line {node['line']})")
        check_conditions(node["attrs"], node["line"])
        # Inline block-valued attrs (on_activate = { ... }) are handlers too.
        for key, val in node["attrs"].items():
            if key.startswith("on_") and isinstance(val, dict):
                check_handler_attrs(val, node["line"])
        for child in node["children"]:
            if child["kind"] == "item":
                check_conditions(child["attrs"], child["line"])
                for key, val in child["attrs"].items():
                    if key.startswith("on_") and isinstance(val, dict):
                        check_handler_attrs(val, child["line"])
            elif child["kind"] == "on":
                if child["name"] not in EVENTS:
                    errors.append(f"{app_name}: unknown event '{child['name']}' (line {child['line']})")
                check_handler_attrs(child["attrs"], child["line"])
            else:
                errors.append(f"{app_name}: unexpected '{child['kind']}' inside widget (line {child['line']})")

    def walk_section(node):
        check_conditions(node["attrs"], node["line"])
        for child in node["children"]:
            if child["kind"] == "widget":
                walk_widget(child)
            else:
                errors.append(f"{app_name}: unexpected '{child['kind']}' inside section (line {child['line']})")

    for s in screens:
        for child in s["children"]:
            if child["kind"] == "section":
                walk_section(child)
            elif child["kind"] == "widget":
                walk_widget(child)
            elif child["kind"] == "on":
                if child["name"] not in EVENTS:
                    errors.append(f"{app_name}: unknown event '{child['name']}' (line {child['line']})")
                check_handler_attrs(child["attrs"], child["line"])
            else:
                errors.append(f"{app_name}: unexpected '{child['kind']}' directly in screen (line {child['line']})")

    return errors


# ── Binary compiler ──────────────────────────────────────────────────────
# A flat record table, not a serialization format needing real decode
# logic on-device — see the plan's own "Binary format" section for the
# exact shape. Kept deliberately boring: fixed-size node records + one
# deduped string pool, everything else pre-resolved to small integer IDs.

MAGIC = b"PUIB"
FORMAT_VERSION = 1

NODE_KIND_IDS = {
    "SCREEN": 0, "SECTION": 1,
    "WIDGET_TEXT": 2, "WIDGET_LIST": 3, "WIDGET_BUTTON": 4,
    "ITEM": 5, "ON_HANDLER": 6,
}
WIDGET_KIND_BY_TYPE = {"text": "WIDGET_TEXT", "list": "WIDGET_LIST", "button": "WIDGET_BUTTON"}

# One node record: kind(u8) parent(i16) first_child(i16) next_sibling(i16)
# attr_off(u32) attr_len(u16) — attrs themselves are a small serialized
# key/value run in the string pool (see _encode_attrs below), not a
# separate table; this stays a flat, bounds-checked memcpy shape on the
# C side rather than anything needing a real deserializer.
NODE_STRUCT = struct.Struct("<BhhhIH")


class StringPool:
    def __init__(self):
        self.buf = bytearray()
        self.offsets = {}

    def intern(self, s):
        if s in self.offsets:
            return self.offsets[s]
        off = len(self.buf)
        enc = s.encode("utf-8") + b"\x00"
        self.buf.extend(enc)
        self.offsets[s] = off
        return off


def _resolve_id(table, name, what, app_name, line):
    if name not in table:
        raise PuiError(f"{app_name}: unknown {what} '{name}'", line)
    return table.index(name)


def _encode_attrs(attrs, pool, app_name, line):
    # Small run of (key_off:u32, kind:u8, value) tuples, kind 0=string
    # (value is a pool offset u32), 1=number (f32), 2=bool (u8),
    # 3=condition-list (see below). Compact, fixed-shape, no nested
    # decode beyond "read N of these."
    out = bytearray()
    count = 0
    for key, val in attrs.items():
        if isinstance(val, dict):
            continue   # inline handler blocks are compiled as ON_HANDLER children, not attrs — see compile_node()
        key_off = pool.intern(key)
        if isinstance(val, bool):
            out += struct.pack("<IB", key_off, 2)
            out += struct.pack("<B", 1 if val else 0)
        elif isinstance(val, (int, float)):
            out += struct.pack("<IB", key_off, 1)
            out += struct.pack("<f", float(val))
        elif key in ("visible_if", "enabled_if"):
            conds = _split_conditions(str(val))
            out += struct.pack("<IB", key_off, 3)
            out += struct.pack("<B", len(conds))
            for cond, neg in conds:
                cid = _resolve_id(CONDITIONS, cond, "condition", app_name, line)
                out += struct.pack("<BB", cid, 1 if neg else 0)
        else:
            s = str(val)
            val_off = pool.intern(s)
            out += struct.pack("<IB", key_off, 0)
            out += struct.pack("<I", val_off)
        count += 1
    return bytes(out), count


class Compiler:
    def __init__(self, app_name):
        self.app_name = app_name
        self.pool = StringPool()
        self.nodes = []   # list of dicts pre-serialization: {kind, parent, attrs_raw, line}

    def add_node(self, kind, parent, attrs, line):
        idx = len(self.nodes)
        self.nodes.append({"kind": kind, "parent": parent, "attrs": attrs, "line": line, "children": []})
        if parent is not None:
            self.nodes[parent]["children"].append(idx)
        return idx

    def compile_widget(self, node, parent):
        wtype = node["attrs"]["type"]
        kind = WIDGET_KIND_BY_TYPE[wtype]
        idx = self.add_node(kind, parent, node["attrs"], node["line"])
        for child in node["children"]:
            if child["kind"] == "item":
                self.add_node("ITEM", idx, child["attrs"], child["line"])
                for key, val in child["attrs"].items():
                    if key.startswith("on_") and isinstance(val, dict):
                        self.add_node("ON_HANDLER", self.nodes[idx]["children"][-1], val, child["line"])
            elif child["kind"] == "on":
                h = self.add_node("ON_HANDLER", idx, dict(child["attrs"], _event=child["name"]), child["line"])
        for key, val in node["attrs"].items():
            if key.startswith("on_") and isinstance(val, dict):
                self.add_node("ON_HANDLER", idx, val, node["line"])
        return idx

    def compile_section(self, node, parent):
        idx = self.add_node("SECTION", parent, node["attrs"], node["line"])
        for child in node["children"]:
            if child["kind"] == "widget":
                self.compile_widget(child, idx)
        return idx

    def compile_screen(self, node):
        idx = self.add_node("SCREEN", None, node["attrs"], node["line"])
        for child in node["children"]:
            if child["kind"] == "section":
                self.compile_section(child, idx)
            elif child["kind"] == "widget":
                self.compile_widget(child, idx)
            elif child["kind"] == "on":
                self.add_node("ON_HANDLER", idx, dict(child["attrs"], _event=child["name"]), child["line"])
        return idx

    def serialize(self):
        # Pass 1: encode every node's attrs into the pool, recording
        # (offset, count) per node — done before laying out the fixed
        # node table so attr blobs land in the pool ahead of it.
        attr_blobs = []
        for n in self.nodes:
            attrs = dict(n["attrs"])
            event_name = attrs.pop("_event", None)
            blob, count = _encode_attrs(attrs, self.pool, self.app_name, n["line"])
            if event_name is not None:
                # Event name is stored as a synthetic first attr, key "_event",
                # value = the event's integer ID (as a number attr) — cheap,
                # keeps the attr-run shape uniform rather than a special field.
                eid = _resolve_id(EVENTS, event_name, "event", self.app_name, n["line"])
                key_off = self.pool.intern("_event")
                extra = struct.pack("<IBf", key_off, 1, float(eid))
                blob = extra + blob
                count += 1
            off = len(self.pool.buf)
            self.pool.buf.extend(blob)
            attr_blobs.append((off, count))

        # Pass 2: fixed node table. first_child/next_sibling computed
        # from each node's own `children` list built during compile_*().
        node_bytes = bytearray()
        for i, n in enumerate(self.nodes):
            kind_id = NODE_KIND_IDS[n["kind"]]
            parent = n["parent"] if n["parent"] is not None else -1
            children = n["children"]
            first_child = children[0] if children else -1
            # next_sibling: found by looking at the parent's own child list.
            next_sibling = -1
            if n["parent"] is not None:
                siblings = self.nodes[n["parent"]]["children"]
                pos = siblings.index(i)
                if pos + 1 < len(siblings):
                    next_sibling = siblings[pos + 1]
            attr_off, attr_count = attr_blobs[i]
            node_bytes += NODE_STRUCT.pack(kind_id, parent, first_child, next_sibling, attr_off, attr_count)

        header = MAGIC + struct.pack("<BHI", FORMAT_VERSION, len(self.nodes), len(self.pool.buf))
        # header: magic(4s) version(u8) node_count(u16) pool_size(u32) —
        # node table and string pool both follow immediately, node table
        # first (fixed-size, no offset field needed for it specifically).
        return header + bytes(node_bytes) + bytes(self.pool.buf)


def compile_screen(screen_node, app_name):
    c = Compiler(app_name)
    c.compile_screen(screen_node)
    return c.serialize()


def compile_app_ui(app_dir):
    """Parses every ui/*.pui in app_dir, validates the WHOLE set together
    (screen-name references can cross files within one app), and returns
    {screen_name: compiled_bytes}. Raises PuiError on the first validation
    failure — callers (catstrap.py) should catch and report it as a real
    build failure, same as any other malformed source input."""
    ui_dir = os.path.join(app_dir, "ui")
    if not os.path.isdir(ui_dir):
        return {}

    app_name = os.path.basename(app_dir.rstrip("/"))
    all_screens = []
    for fname in sorted(os.listdir(ui_dir)):
        if not fname.endswith(".pui"):
            continue
        all_screens.extend(parse_pui(os.path.join(ui_dir, fname)))

    if not all_screens:
        return {}

    errors = validate_screens(all_screens, app_name)
    if errors:
        raise PuiError("\n".join(errors), all_screens[0]["line"])

    return {s["name"]: compile_screen(s, app_name) for s in all_screens}
