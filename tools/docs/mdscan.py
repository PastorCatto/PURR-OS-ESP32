"""Shared Markdown reading for the documentation tools: headings, anchors and links, with code left alone."""

import os
import re

FENCE = re.compile(r"^\s{0,3}(```|~~~)")
INLINE_CODE = re.compile(r"`[^`\n]*`")
LINK = re.compile(r"(?<!\!)\[(?:[^\]\\]|\\.)*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
IMAGE = re.compile(r"\!\[(?:[^\]\\]|\\.)*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
REF_DEF = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*<?(\S+?)>?(?:\s+.*)?$")
HEADING = re.compile(r"^\s{0,3}(#{1,6})\s+(.*?)\s*#*\s*$")
HTML_ID = re.compile(r"""<a\s+(?:name|id)=["']([^"']+)["']""", re.I)


def slug(text):
    """The anchor GitHub makes for a heading: lower case, links and markup dropped, punctuation removed, spaces to hyphens."""
    t = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    t = re.sub(r"[`*_~]", "", t).strip().lower()
    t = re.sub(r"[^\w\- ]", "", t, flags=re.UNICODE)
    return t.replace(" ", "-")


def lines_outside_code(text):
    """Yield (line number, line) for every line that is not inside a fenced code block."""
    fence = None
    for n, line in enumerate(text.splitlines(), 1):
        m = FENCE.match(line)
        if m:
            if fence is None:
                fence = m.group(1)
            elif m.group(1) == fence:
                fence = None
            continue
        if fence is None:
            yield n, line


def anchors(text):
    """The set of anchors a file offers: every heading's slug (repeats get -1, -2 as on GitHub) and explicit HTML ids."""
    seen, out = {}, set()
    for _, line in lines_outside_code(text):
        m = HEADING.match(line)
        if m:
            base = slug(m.group(2))
            k = seen.get(base, 0)
            seen[base] = k + 1
            out.add(base if k == 0 else "%s-%d" % (base, k))
        for h in HTML_ID.findall(line):
            out.add(h)
    return out


def links(text):
    """Every link and image target in the file: (line number, target)."""
    found = []
    for n, line in lines_outside_code(text):
        bare = INLINE_CODE.sub("", line)
        for rx in (LINK, IMAGE):
            for m in rx.finditer(bare):
                found.append((n, m.group(1)))
        m = REF_DEF.match(bare)
        if m:
            found.append((n, m.group(1)))
    return found


def markdown_files(paths, skip=("archive", "out", "node_modules", ".git", "managed_components")):
    for p in paths:
        if os.path.isfile(p):
            yield p
            continue
        for d, dirs, files in os.walk(p):
            dirs[:] = sorted(x for x in dirs if x not in skip)
            for f in sorted(files):
                if f.lower().endswith(".md"):
                    yield os.path.join(d, f)
