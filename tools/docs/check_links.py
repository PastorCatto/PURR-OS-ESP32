#!/usr/bin/env python3
"""Check every relative link and anchor in the Markdown documentation.

    python3 tools/docs/check_links.py [PATH ...]        (default: README.md, docs/, the plans, and the per-area SPEC files)

A link is broken when its file does not exist, or its #anchor is not a heading (or explicit id) in the target file. Web links (http, https,
mailto) are not fetched: this runs offline and says how many it skipped. Links inside code blocks and inline code are ignored. Exit status 1
when anything is broken, so it can gate a commit.
"""

import argparse
import os
import sys
from urllib.parse import unquote

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mdscan  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WEB = ("http://", "https://", "mailto:", "tel:", "ftp://")


def default_paths():
    out = []
    for p in ("README.md", "docs", "showtime1.md", "THIRD_PARTY.md"):
        if os.path.exists(os.path.join(ROOT, p)):
            out.append(os.path.join(ROOT, p))
    for f in sorted(os.listdir(ROOT)):
        if f.startswith("plan") and f.endswith(".md"):
            out.append(os.path.join(ROOT, f))
    return out


def check(paths, root=ROOT):
    """Returns (problems, files_checked, links_checked, web_skipped). A problem is (file, line, target, why)."""
    cache = {}

    def anchors_of(path):
        if path not in cache:
            with open(path, encoding="utf-8", errors="replace") as fh:
                cache[path] = mdscan.anchors(fh.read())
        return cache[path]

    problems, nfiles, nlinks, nweb = [], 0, 0, 0
    for f in mdscan.markdown_files(paths):
        nfiles += 1
        with open(f, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        for n, target in mdscan.links(text):
            if target.startswith(WEB):
                nweb += 1
                continue
            nlinks += 1
            path, _, frag = target.partition("#")
            path = unquote(path)
            if path == "":
                dest = f
            elif path.startswith("/"):
                dest = os.path.join(root, path.lstrip("/"))
            else:
                dest = os.path.normpath(os.path.join(os.path.dirname(f), path))
            if not os.path.exists(dest):
                problems.append((f, n, target, "no such file"))
                continue
            if frag and dest.lower().endswith(".md") and os.path.isfile(dest):
                if unquote(frag) not in anchors_of(dest):
                    problems.append((f, n, target, "no such heading in %s" % os.path.relpath(dest, root)))
    return problems, nfiles, nlinks, nweb


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("paths", nargs="*")
    a = ap.parse_args(argv)
    problems, nfiles, nlinks, nweb = check(a.paths or default_paths())
    for f, n, target, why in problems:
        print("%s:%d: %s  (%s)" % (os.path.relpath(f, ROOT), n, target, why))
    print("%d file(s), %d relative link(s) checked, %d web link(s) not fetched, %d broken." % (nfiles, nlinks, nweb, len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
