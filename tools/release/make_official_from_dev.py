#!/usr/bin/env python3
"""Builds the two official release folders from a dev-server recovery folder that has been tested on a device, ready to upload as GitHub
release assets. Nothing is signed here: every file keeps the signature it already carries, and only the manifest headers change.

    python3 tools/release/make_official_from_dev.py --release 1.0.27 --out ~/Desktop/PURR-OS-release-1.0.27

What it makes (the firmware reads recovery.manifest from the JumpingJaguar1 release and modules.manifest and drivers.manifest from
JumpingJaguar2; a file named in a manifest is fetched from the same folder):

    JumpingJaguar1/   recovery.manifest and the files it names (bootloader, boot package, kernel, KittenOS, loader, picture pack), legal.txt,
                      and, carried over unchanged from the current official release, the stanzas and files of the other boards
    JumpingJaguar2/   modules.manifest, drivers.manifest and the module and driver files they name
    SHA256SUMS.txt, README.txt

The manifest headers keep the `withdrawn_versions` and `withdrawn_reason` lines, so a withdrawn release stays withdrawn on the official
server. Every file is checked against its manifest (size and sha256) before the tool finishes; it stops at the first mismatch.
"""
import argparse
import hashlib
import os
import shutil
import sys
import time
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
RELEASE1 = "https://github.com/PastorCatto/PURR-OS-ESP32/releases/download/JumpingJaguar1/"


def sha256(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def parse_manifest(text):
    """-> (header lines as (key, value), stanzas as dicts); a stanza starts at a `component=` line."""
    head, stanzas, cur = [], [], None
    for line in text.splitlines():
        if not line.strip():
            if cur is not None:
                stanzas.append(cur)
                cur = None
            continue
        if line.startswith("#"):
            continue
        k, _, v = line.partition("=")
        if k == "component":
            if cur is not None:
                stanzas.append(cur)
            cur = {}
        if cur is None:
            head.append((k, v))
        else:
            cur[k] = v
    if cur is not None:
        stanzas.append(cur)
    return head, stanzas


def stanza_text(s):
    order = ("component", "type", "version", "chip", "board", "file", "size", "sha256", "key", "min_bootloader", "min_coreos",
             "payload_size", "payload_sha256")
    keys = [k for k in order if k in s] + [k for k in s if k not in order]
    return "\n".join("%s=%s" % (k, s[k]) for k in keys) + "\n"


def write_manifest(path, release, head, stanzas):
    keep = [(k, v) for k, v in head if k in ("withdrawn_versions", "withdrawn_reason")]
    lines = ["release=%s" % release, "released=%s" % time.strftime("%Y-%m-%d")] + ["%s=%s" % kv for kv in keep] + [""]
    lines += [stanza_text(s) for s in stanzas]
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


def copy_checked(src_dir, dest_dir, stanza):
    src = os.path.join(src_dir, stanza["file"])
    if not os.path.exists(src):
        sys.exit("%s is named in the manifest but is not in %s" % (stanza["file"], src_dir))
    if os.path.getsize(src) != int(stanza["size"]) or sha256(src) != stanza["sha256"]:
        sys.exit("%s does not match its manifest entry (size or sha256)" % src)
    shutil.copyfile(src, os.path.join(dest_dir, stanza["file"]))


def carried_over(board, r1, fetch_text, fetch_file):
    """The other boards' stanzas from the published recovery.manifest, with their files, unchanged."""
    _, old = parse_manifest(fetch_text("recovery.manifest"))
    out = []
    for s in old:
        if s.get("board") == board:
            continue
        dest = os.path.join(r1, s["file"])
        fetch_file(s["file"], dest)
        if os.path.getsize(dest) != int(s["size"]) or sha256(dest) != s["sha256"]:
            sys.exit("%s does not match the published manifest" % s["file"])
        out.append(s)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--release", required=True, help="the release number, like 1.0.27")
    ap.add_argument("--out", required=True, help="a folder that does not exist yet")
    ap.add_argument("--src", default=os.path.join(ROOT, "out", "devrepo", "recovery"), help="the dev server's recovery folder")
    ap.add_argument("--board", default="tdeck_plus")
    ap.add_argument("--offline", action="store_true", help="do not fetch the other boards from the published release")
    a = ap.parse_args()

    out = os.path.abspath(os.path.expanduser(a.out))
    if os.path.exists(out):
        sys.exit("%s already exists: move it away first" % out)
    rec_path, mod_path, drv_path = (os.path.join(a.src, n) for n in ("recovery.manifest", "modules.manifest", "drivers.manifest"))
    for p in (rec_path, mod_path, drv_path):
        if not os.path.exists(p):
            sys.exit("missing %s" % p)
    r1, r2 = os.path.join(out, "JumpingJaguar1"), os.path.join(out, "JumpingJaguar2")
    os.makedirs(r1)
    os.makedirs(r2)

    head, rec = parse_manifest(open(rec_path, encoding="utf-8").read())
    for s in rec:
        copy_checked(a.src, r1, s)
    legal = os.path.join(a.src, "legal.txt")
    if not os.path.exists(legal):
        sys.exit("missing %s" % legal)
    shutil.copyfile(legal, os.path.join(r1, "legal.txt"))
    if not a.offline:
        def fetch_text(name):
            return urllib.request.urlopen(RELEASE1 + name, timeout=60).read().decode()

        def fetch_file(name, dest):
            with urllib.request.urlopen(RELEASE1 + name, timeout=60) as r, open(dest, "wb") as fh:
                shutil.copyfileobj(r, fh)
        rec += carried_over(a.board, r1, fetch_text, fetch_file)
    write_manifest(os.path.join(r1, "recovery.manifest"), "%s+%s" % (a.release, a.board), head, rec)

    for src_manifest, name in ((mod_path, "modules.manifest"), (drv_path, "drivers.manifest")):
        h, stanzas = parse_manifest(open(src_manifest, encoding="utf-8").read())
        for s in stanzas:
            copy_checked(a.src, r2, s)
        keep_release = dict(h).get("release", "%s" % a.release)
        write_manifest(os.path.join(r2, name), keep_release if name == "drivers.manifest" else "%s+%s" % (a.release, a.board), h, stanzas)

    sums = []
    for d in (r1, r2):
        for fn in sorted(os.listdir(d)):
            sums.append("%s  %s/%s" % (sha256(os.path.join(d, fn)), os.path.basename(d), fn))
    with open(os.path.join(out, "SHA256SUMS.txt"), "w") as fh:
        fh.write("\n".join(sums) + "\n")
    with open(os.path.join(out, "README.txt"), "w") as fh:
        fh.write("PURR OS %s: upload the files in JumpingJaguar1/ as the assets of the release tag JumpingJaguar1, and those in JumpingJaguar2/ as "
                 "the assets of JumpingJaguar2. Replace the assets of the same names. After uploading, fetch every file each manifest names "
                 "from its release and compare size and sha256 (SHA256SUMS.txt lists them).\n" % a.release)
    print("%d recovery files, %d module and driver files, written to %s" % (len(os.listdir(r1)), len(os.listdir(r2)), out))


if __name__ == "__main__":
    main()
