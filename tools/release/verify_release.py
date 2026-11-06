#!/usr/bin/env python3
"""Checks an uploaded release the way a device will see it: fetches each manifest from the release, then every file the manifest names, and compares size and
sha256 with the manifest (and, with --folder, with the local copy that was uploaded). Standard library only. Exits 1 on the first kind of failure it finds,
after listing all of them.

    python3 tools/release/verify_release.py --folder ~/Desktop/PURR-OS-release-1.0.28

By default it reads the official GitHub releases (JumpingJaguar1 for recovery.manifest, JumpingJaguar2 for modules.manifest and drivers.manifest).
--base1 and --base2 name other folders (for a test against the dev server, both can be http://127.0.0.1:47801/recovery/).
"""
import argparse
import hashlib
import os
import sys
import urllib.request

BASE1 = "https://github.com/PastorCatto/PURR-OS-ESP32/releases/download/JumpingJaguar1/"
BASE2 = "https://github.com/PastorCatto/PURR-OS-ESP32/releases/download/JumpingJaguar2/"


def get(url):
    with urllib.request.urlopen(url, timeout=60) as r:
        return r.read()


def stanzas(text):
    out = []
    for block in text.split("\n\n"):
        kv = dict(l.split("=", 1) for l in block.splitlines() if "=" in l and not l.startswith("#"))
        if "file" in kv and "sha256" in kv and "size" in kv:
            out.append(kv)
    return out


def check(base, manifest, local, problems):
    try:
        text = get(base + manifest).decode()
    except Exception as e:
        problems.append("%s%s: could not be fetched (%s)" % (base, manifest, e))
        return 0
    head = text.splitlines()[0] if text else ""
    print("%s: %s" % (manifest, head))
    n = 0
    for s in stanzas(text):
        n += 1
        try:
            data = get(base + s["file"])
        except Exception as e:
            problems.append("%s: could not be fetched (%s)" % (s["file"], e))
            continue
        if len(data) != int(s["size"]) or hashlib.sha256(data).hexdigest() != s["sha256"]:
            problems.append("%s: size or sha256 differs from the manifest" % s["file"])
        if local:
            path = os.path.join(local, s["file"])
            if not os.path.exists(path):
                problems.append("%s: not in the local folder %s" % (s["file"], local))
            elif open(path, "rb").read() != data:
                problems.append("%s: differs from the local copy" % s["file"])
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--folder", help="the local release folder that was uploaded (with JumpingJaguar1 and JumpingJaguar2 inside)")
    ap.add_argument("--base1", default=BASE1)
    ap.add_argument("--base2", default=BASE2)
    a = ap.parse_args()
    f1 = os.path.join(os.path.expanduser(a.folder), "JumpingJaguar1") if a.folder else None
    f2 = os.path.join(os.path.expanduser(a.folder), "JumpingJaguar2") if a.folder else None
    problems = []
    n = check(a.base1, "recovery.manifest", f1, problems)
    n += check(a.base2, "modules.manifest", f2, problems)
    n += check(a.base2, "drivers.manifest", f2, problems)
    print("%d files checked, %d problem(s)" % (n, len(problems)))
    for p in problems:
        print("  PROBLEM:", p)
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
