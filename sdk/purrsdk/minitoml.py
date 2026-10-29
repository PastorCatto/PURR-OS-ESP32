"""A small TOML reader and writer for the files the kit uses (brand.toml, board.toml, project.toml).

Python 3.11+ has `tomllib`; the kit bundles 3.12 and uses it, but a checkout on an older Python still
works because this reads the subset we write: [tables], [[arrays of tables]], strings, integers
(decimal and 0x), booleans, and flat arrays of those. Comments (#) are kept out of values.
"""

import re

try:
    import tomllib as _tomllib
except ImportError:                               # Python before 3.11
    _tomllib = None


class TomlError(ValueError):
    pass


def loads(text):
    if _tomllib is not None:
        try:
            return _tomllib.loads(text)
        except _tomllib.TOMLDecodeError as e:
            raise TomlError(str(e))
    return _parse(text)


def load(path):
    with open(path, "r", encoding="utf-8") as fh:
        return loads(fh.read())


def _strip_comment(line):
    out, quote, esc = [], None, False
    for ch in line:
        if quote:
            out.append(ch)
            if esc:
                esc = False
            elif ch == "\\" and quote == '"':
                esc = True
            elif ch == quote:
                quote = None
        elif ch in "\"'":
            quote = ch
            out.append(ch)
        elif ch == "#":
            break
        else:
            out.append(ch)
    return "".join(out).strip()


def _value(tok, n):
    tok = tok.strip()
    if not tok:
        raise TomlError("line %d: a value is missing" % n)
    if tok[0] == '"':
        if len(tok) < 2 or tok[-1] != '"':
            raise TomlError("line %d: a string is not closed" % n)
        return bytes(tok[1:-1], "utf-8").decode("unicode_escape") if "\\" in tok else tok[1:-1]
    if tok[0] == "'":
        if len(tok) < 2 or tok[-1] != "'":
            raise TomlError("line %d: a string is not closed" % n)
        return tok[1:-1]
    if tok[0] == "[":
        if tok[-1] != "]":
            raise TomlError("line %d: an array is not closed" % n)
        inner, items, depth, quote, cur = tok[1:-1], [], 0, None, ""
        for ch in inner:
            if quote:
                cur += ch
                if ch == quote:
                    quote = None
            elif ch in "\"'":
                quote = ch
                cur += ch
            elif ch == "," and depth == 0:
                items.append(cur)
                cur = ""
            else:
                if ch == "[":
                    depth += 1
                elif ch == "]":
                    depth -= 1
                cur += ch
        if cur.strip():
            items.append(cur)
        return [_value(i, n) for i in items]
    if tok in ("true", "false"):
        return tok == "true"
    try:
        return int(tok.replace("_", ""), 0)
    except ValueError:
        pass
    try:
        return float(tok)
    except ValueError:
        raise TomlError("line %d: cannot read the value %r" % (n, tok))


def _parse(text):
    root, cur = {}, None
    cur = root
    for n, raw in enumerate(text.splitlines(), 1):
        line = _strip_comment(raw)
        if not line:
            continue
        m = re.match(r"^\[\[\s*([A-Za-z0-9_.-]+)\s*\]\]$", line)
        if m:
            parent = root
            *path, last = m.group(1).split(".")
            for p in path:
                parent = parent.setdefault(p, {})
                if isinstance(parent, list):
                    parent = parent[-1]
            parent.setdefault(last, []).append({})
            cur = parent[last][-1]
            continue
        m = re.match(r"^\[\s*([A-Za-z0-9_.-]+)\s*\]$", line)
        if m:
            cur = root
            for p in m.group(1).split("."):
                cur = cur.setdefault(p, {})
                if isinstance(cur, list):
                    cur = cur[-1]
            continue
        if "=" not in line:
            raise TomlError("line %d: expected key = value" % n)
        key, val = line.split("=", 1)
        key = key.strip().strip('"')
        cur[key] = _value(val, n)
    return root


def _fmt(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return repr(v)
    if isinstance(v, (list, tuple)):
        return "[" + ", ".join(_fmt(i) for i in v) + "]"
    return '"' + str(v).replace("\\", "\\\\").replace('"', '\\"') + '"'


def dumps(data, comments=None):
    """Write a dict of scalars/lists and one level of tables (and lists of tables). `comments` maps
    "table.key" (or "table") to a comment line written above it."""
    comments = comments or {}
    lines = []

    def emit(prefix, d):
        for k, v in d.items():
            if not isinstance(v, (dict, list)) or (isinstance(v, list) and not (v and isinstance(v[0], dict))):
                if (prefix + k) in comments:
                    lines.append("# " + comments[prefix + k])
                lines.append("%s = %s" % (k, _fmt(v)))

    emit("", {k: v for k, v in data.items() if not isinstance(v, dict) and not (isinstance(v, list) and v and isinstance(v[0], dict))})
    for k, v in data.items():
        if isinstance(v, dict):
            lines.append("")
            if k in comments:
                lines.append("# " + comments[k])
            lines.append("[%s]" % k)
            emit(k + ".", v)
        elif isinstance(v, list) and v and isinstance(v[0], dict):
            for item in v:
                lines.append("")
                lines.append("[[%s]]" % k)
                emit(k + ".", item)
    return "\n".join(lines).strip() + "\n"
