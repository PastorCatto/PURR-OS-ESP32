#!/usr/bin/env python3
"""Count the words of real prose in the documentation, per file and in total.

    python3 tools/docs/count_words.py [PATH ...]        (default: docs/)

Not counted: fenced code, table rows, headings' markup, link targets, image lines, blank lines. A link counts as its text. The brief for the
documentation is "at least 100 pages" taken as about 500 words a page: 50,000 words (showtime1.md, Part B).
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mdscan  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TABLE = re.compile(r"^\s*\|")
WORD = re.compile(r"[^\W_]+(?:['’\-][^\W_]+)*", re.UNICODE)
PAGE = 500


def words(text):
    n = 0
    for _, line in mdscan.lines_outside_code(text):
        if TABLE.match(line) or mdscan.IMAGE.search(line) and not mdscan.LINK.search(line):
            continue
        line = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", line)          # a link is its text
        line = re.sub(r"<[^>]+>", " ", line)                           # html tags
        line = re.sub(r"^\s*(#{1,6}|[-*+]|\d+\.|>)\s+", "", line)      # markers
        n += len(WORD.findall(line))
    return n


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("paths", nargs="*", default=[os.path.join(ROOT, "docs")])
    ap.add_argument("--target", type=int, default=50000, help="the word count the documentation must reach (default 50000)")
    a = ap.parse_args(argv)
    total = 0
    rows = []
    for f in mdscan.markdown_files(a.paths):
        with open(f, encoding="utf-8", errors="replace") as fh:
            n = words(fh.read())
        rows.append((os.path.relpath(f, ROOT), n))
        total += n
    for name, n in rows:
        print("%7d  %s" % (n, name))
    print("%7d  total, about %d pages at %d words a page (target %d)" % (total, round(total / PAGE), PAGE, a.target))
    return 0 if total >= a.target else 1


if __name__ == "__main__":
    sys.exit(main())
