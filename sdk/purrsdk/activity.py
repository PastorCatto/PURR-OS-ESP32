"""What was done last, in any part of Studio: the main menu's big tile is the newest entry here.

A small JSON file in the workspace (`activity.json`), newest first, one entry per thing (the same kind and title moves to the top instead of repeating):

    {"kind": "ui", "title": "moonlit", "route": "builder", "arg": "moonlit", "note": "9 elements", "when": 1731100000}

`kind` picks the colour and wording of the big tile; `route` and `arg` say where Studio goes when it is pressed.
"""

import json
import os
import time

KINDS = ("ui", "image", "driver", "app", "sign", "flash", "server", "emulator", "release", "keys", "board", "other")
MAX = 30


def _path(ws):
    return os.path.join(ws.path, "activity.json")


def load(ws):
    try:
        with open(_path(ws), encoding="utf-8") as fh:
            items = json.load(fh)
    except (OSError, ValueError):
        return []
    return [i for i in items if isinstance(i, dict) and i.get("title")][:MAX]


def record(ws, kind, title, route, arg="", note="", when=None):
    """Put (kind, title) at the top of the list. Never raises: losing a line of history must not break what the person was doing."""
    try:
        if kind not in KINDS:
            kind = "other"
        items = [i for i in load(ws) if (i.get("kind"), i.get("title")) != (kind, title)]
        items.insert(0, {"kind": kind, "title": str(title)[:60], "route": str(route)[:40], "arg": str(arg)[:80], "note": str(note)[:100], "when": int(when or time.time())})
        os.makedirs(ws.path, exist_ok=True)
        tmp = _path(ws) + ".part"
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump(items[:MAX], fh, indent=1)
        os.replace(tmp, _path(ws))
    except OSError:
        pass


def ago(when, now=None):
    d = max(0, int((now or time.time()) - when))
    if d < 60:
        return "just now"
    if d < 3600:
        return "%d min ago" % (d // 60)
    if d < 86400:
        return "%d h ago" % (d // 3600)
    if d < 172800:
        return "yesterday"
    return "%d days ago" % (d // 86400)
