"""The files that configure a device, as data you can edit safely.

    system.conf   key=value lines, the keys the shell registers (data/settings.json lists them with what each accepts)
    apps.conf     Hyprland-style `keyword = value`: general:max_apps, rule, bind, exec-boot, exec-once
    board.conf    the discovered-hardware profile (see boards.py to write one from a board.toml)
"""

import json
import os
import re

from . import workspace

APPS_DEFAULTS = {"general:max_apps": 8, "general:reserve_kb": 64, "general:low_kb": 512, "general:critical_kb": 256}
APPS_HEADER = """# /etc/apps.conf - how the app runtime behaves. Hyprland-style: keyword = value.
"""


def settings_schema():
    path = workspace.sdk_path("data", "settings.json")
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def _range(values):
    m = re.match(r"^(\d+)-(\d+)$", values)
    return (int(m.group(1)), int(m.group(2))) if m else None


def validate_setting(key, value, schema=None):
    """None when the value is acceptable, else a sentence saying what is wrong."""
    schema = schema or settings_schema()
    entry = next((e for e in schema if e["key"] == key), None)
    if entry is None:
        return "I do not know a setting called %r." % key
    value = str(value).strip()
    values = entry["values"]
    if value == "":
        return "This cannot be empty."
    if values.startswith("a name from") or values.startswith("<user>"):
        return None
    options = [o for o in re.split(r"[|\s]", values) if o]
    ranges = [_range(o) for o in options if _range(o)]
    words = [o for o in options if not _range(o)]
    if "-" in value and False:
        pass
    if value.lstrip("-").isdigit() and ranges:
        n = int(value)
        if any(lo <= n <= hi for lo, hi in ranges):
            return None
        return "That needs to be %s." % " or ".join("%d to %d" % r for r in ranges)
    if value in words or (words and value.lower() in [w.lower() for w in words]):
        return None
    if ranges and not words:
        return "That needs to be a number from %d to %d." % ranges[0]
    return "Pick one of: %s." % ", ".join(options)


def parse_system_conf(text):
    """[(key, value)] in file order; comments and blank lines are dropped."""
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        out.append((k.strip(), v.strip()))
    return out


def dump_system_conf(pairs):
    return "".join("%s=%s\n" % (k, v) for k, v in pairs)


def parse_apps_conf(text):
    """{"general": {name: int}, "rules": [..], "binds": [..], "exec_boot": [..], "exec_once": [..], "errors": [..]}"""
    out = {"general": {}, "rules": [], "binds": [], "exec_boot": [], "exec_once": [], "errors": []}
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if "=" not in line:
            out["errors"].append("line %d: expected keyword = value" % n)
            continue
        k, v = [x.strip() for x in line.split("=", 1)]
        if k.startswith("general:"):
            if k not in APPS_DEFAULTS:
                out["errors"].append("line %d: I do not know %s." % (n, k))
            elif not v.isdigit():
                out["errors"].append("line %d: %s wants a whole number." % (n, k))
            else:
                out["general"][k.split(":", 1)[1]] = int(v)
        elif k == "rule":
            out["rules"].append(v)
        elif k == "bind":
            out["binds"].append(v)
        elif k == "exec-boot":
            out["exec_boot"].append(v)
        elif k == "exec-once":
            out["exec_once"].append(v)
        else:
            out["errors"].append("line %d: I do not know the keyword %r." % (n, k))
    return out


def dump_apps_conf(conf):
    lines = [APPS_HEADER.rstrip(), ""]
    for k, v in conf.get("general", {}).items():
        lines.append("general:%s = %d" % (k, v))
    for r in conf.get("rules", []):
        lines.append("rule = " + r)
    for b in conf.get("binds", []):
        lines.append("bind = " + b)
    for a in conf.get("exec_boot", []):
        lines.append("exec-boot = " + a)
    for a in conf.get("exec_once", []):
        lines.append("exec-once = " + a)
    return "\n".join(lines).rstrip() + "\n"
