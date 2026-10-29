#!/usr/bin/env python3
"""Reads the settings tables out of the shell's source and writes data/settings.json, the schema Studio's Configure tool is built from.

    python3 sdk/tools/extract_settings.py [repo_root]

Every `purr_setting_t` entry looks like {"key", "help", "values", admin, get, set}; the strings may be split across lines.
"""
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.abspath(os.path.join(HERE, "..", ".."))
STR = r'((?:"(?:[^"\\]|\\.)*"\s*)+)'
ENTRY = re.compile(r'\{\s*"([a-z0-9_]+\.[a-z0-9_]+)"\s*,\s*' + STR + r',\s*' + STR + r',\s*(\d)\s*,\s*\w+\s*,\s*\w+\s*\}')


def join(literals):
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', literals)).replace('\\"', '"').replace("\\n", "\n")


out = []
for path in sorted(glob.glob(os.path.join(ROOT, "PurrOS", "main", "*.c"))):
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    for m in ENTRY.finditer(text):
        out.append({"key": m.group(1), "help": join(m.group(2)), "values": join(m.group(3)), "admin": m.group(4) == "1",
                    "file": os.path.relpath(path, ROOT)})
# time.zone and time.clock24 use the {key, help, values, admin, get, set} form without a surrounding table; the regex covers them too.
seen, unique = set(), []
for e in out:
    if e["key"] not in seen:
        seen.add(e["key"])
        unique.append(e)
dest = os.path.join(HERE, "..", "data", "settings.json")
with open(dest, "w", encoding="utf-8") as fh:
    json.dump(unique, fh, indent=1)
    fh.write("\n")
print("wrote %s: %d settings" % (os.path.normpath(dest), len(unique)))
for e in unique:
    print("  %-24s %s" % (e["key"], e["values"][:40]))
