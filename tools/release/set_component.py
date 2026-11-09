#!/usr/bin/env python3
"""Puts a signed container into a release folder in place of the file the manifest names for one component and board, and updates that manifest line (version, file name with
its new 10-letter hash, size, sha256). Nothing is signed here: the file keeps the signature it carries.

    python3 tools/release/set_component.py --folder out/release-1.1/signed-dev --component bootloader --board tdeck_plus \\
        --file bootloader-0.5.0-signed.cat --version 0.5.0

The folder must hold the manifest that names the component (recovery.manifest for the parts). The old file is removed from the folder; every other file is left as it is."""
import argparse
import hashlib
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from make_official_from_dev import parse_manifest, write_manifest  # noqa: E402

HASHED = re.compile(r"^(?P<stem>.+)\.(?P<h>[0-9a-f]{10})(?P<ext>\.[A-Za-z]+)$")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--folder", required=True)
    ap.add_argument("--component", required=True)
    ap.add_argument("--board", required=True)
    ap.add_argument("--file", required=True, help="the signed container to put in")
    ap.add_argument("--version", required=True)
    ap.add_argument("--manifest", default="recovery.manifest")
    a = ap.parse_args(argv)
    mpath = os.path.join(a.folder, a.manifest)
    if not os.path.isfile(mpath) or not os.path.isfile(a.file):
        sys.exit("I need %s and %s." % (mpath, a.file))
    with open(mpath, encoding="utf-8") as fh:
        head, stanzas = parse_manifest(fh.read())
    hit = [s for s in stanzas if s.get("component") == a.component and s.get("board") == a.board]
    if len(hit) != 1:
        sys.exit("%s has %d lines for %s on %s: I need exactly one." % (a.manifest, len(hit), a.component, a.board))
    s = hit[0]
    data = open(a.file, "rb").read()
    digest = hashlib.sha256(data).hexdigest()
    old = s["file"]
    m = HASHED.match(old)
    name = "%s.%s%s" % (m.group("stem"), digest[:10], m.group("ext")) if m else old
    shutil.copyfile(a.file, os.path.join(a.folder, name))
    if name != old and not any(o is not s and o["file"] == old for o in stanzas):
        os.remove(os.path.join(a.folder, old))
    s.update(file=name, size=str(len(data)), sha256=digest, version=a.version)
    write_manifest(mpath, dict(head).get("release", "release"), head, stanzas)
    print("%s on %s: %s -> %s (%s, %d bytes)" % (a.component, a.board, old, name, a.version, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
