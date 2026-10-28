#!/usr/bin/env python3
"""Packs the OTA release folder: freshly built, signed images plus a recovery manifest that lists them, ready to upload as
GitHub release assets. Standard library only (purrstrap does the packaging and signing).

    PURRSTRAP_KEY_PASSWORD=... python3 tools/release/make_release.py --version 1.0.0 --out ~/Desktop/PURR-OS-release

What it makes (the firmware reads recovery.manifest from the JumpingJaguar1 release and modules.manifest from JumpingJaguar2;
a file name in a manifest is fetched from the same folder as the manifest, so each folder is one release's assets):

    JumpingJaguar1/   recovery.manifest, kernel_<board>.cat, kittenos_<board>.kitt, loader_<board>.cat, bootpkg_<board>.cat,
                      legal.txt, and (carried over unchanged from the current release) the other boards' images
    JumpingJaguar2/   modules.manifest and the module files, carried over unchanged
    SHA256SUMS.txt, README.txt

New images get a version ABOVE what is published now, because a device refuses to move below the version floors it has
recorded, and the loader would otherwise "recover" a device to an older system than it was running.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "purrstrap"))
sys.path.insert(0, os.path.join(ROOT, "purrstrap", "scripts"))
from lib import image as pimg  # noqa: E402

PY = os.path.expanduser("~/.espressif/python_env/idf5.3_py3.9_env/bin/python")
RELEASE1 = "https://github.com/PastorCatto/PURR-OS-ESP32/releases/download/JumpingJaguar1/"
RELEASE2 = "https://github.com/PastorCatto/PURR-OS-ESP32/releases/download/JumpingJaguar2/"
CHIPS = {pimg.CHIP_ANY: "any", pimg.CHIP_ESP32: "esp32", pimg.CHIP_ESP32S3: "esp32s3"}


def ps(*args):
    r = subprocess.run([PY, os.path.join(ROOT, "purrstrap", "purrstrap.py")] + list(args), capture_output=True, text=True,
                       cwd=ROOT)
    if r.returncode != 0:
        sys.exit("purrstrap %s failed:\n%s%s" % (" ".join(args[:3]), r.stdout, r.stderr))
    return r.stdout


def sha256(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def fetch(url, dest):
    with urllib.request.urlopen(url, timeout=60) as r, open(dest, "wb") as fh:
        shutil.copyfileobj(r, fh)


def parse_manifest(text):
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


def stanza_text(s, order=("component", "type", "version", "chip", "board", "file", "size", "sha256", "key", "min_bootloader",
                          "min_coreos", "payload_size", "payload_sha256")):
    keys = [k for k in order if k in s] + [k for k in s if k not in order]
    return "\n".join("%s=%s" % (k, s[k]) for k in keys) + "\n"


def component_entry(path, name, board, filename, version):
    with open(path, "rb") as fh:
        data = fh.read()
    f = pimg.header_fields(pimg.parse_header(data))
    if f["version"] != version:
        sys.exit("%s carries version %r, expected %r" % (path, f["version"], version))
    return {"component": name, "version": version, "chip": CHIPS.get(f["chip_id"], "any"), "board": board, "file": filename,
            "size": str(len(data)), "sha256": hashlib.sha256(data).hexdigest(), "key": "boot",
            "payload_size": str(f["payload_size"]), "payload_sha256": f["payload_sha256"].hex()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True, help="version for kernel, loader and KittenOS, e.g. 1.0.0")
    ap.add_argument("--board", default="tdeck_plus")
    ap.add_argument("--out", required=True)
    ap.add_argument("--key", default=os.path.join(ROOT, "signing_keys", "boot.key"))
    ap.add_argument("--pub", default=os.path.join(ROOT, "signing_keys", "boot.pub"))
    ap.add_argument("--full", default=os.path.join(ROOT, "out", "stdfull", "purros.bin"))
    ap.add_argument("--min", dest="minimal", default=os.path.join(ROOT, "out", "stdmin", "purros.bin"))
    ap.add_argument("--rec", default=os.path.join(ROOT, "out", "stdrec", "purros.bin"))
    ap.add_argument("--legal", default=os.path.join(ROOT, "OTA", "legal.txt"))
    ap.add_argument("--offline", action="store_true", help="do not fetch the current release (no carried-over files)")
    a = ap.parse_args()
    if not os.environ.get("PURRSTRAP_KEY_PASSWORD"):
        sys.exit("set PURRSTRAP_KEY_PASSWORD for this one command (the boot key's passphrase)")
    for p in (a.full, a.minimal, a.rec, a.legal, a.key):
        if not os.path.exists(p):
            sys.exit("missing %s" % p)

    out = os.path.abspath(os.path.expanduser(a.out))
    r1, r2 = os.path.join(out, "JumpingJaguar1"), os.path.join(out, "JumpingJaguar2")
    if os.path.exists(out):
        sys.exit("%s already exists: move it away first" % out)
    os.makedirs(r1)
    os.makedirs(r2)
    b = a.board

    # ---- new images, signed with the boot key
    kernel = os.path.join(r1, "kernel_%s.cat" % b)
    loader = os.path.join(r1, "loader_%s.cat" % b)
    kitten = os.path.join(r1, "kittenos_%s.kitt" % b)
    bootpkg = os.path.join(r1, "bootpkg_%s.cat" % b)
    ps("coreos", "package-component", "--component", "kernel", "--file", a.full, "--version", a.version, "--board", b, "--out", kernel)
    ps("coreos", "package-component", "--component", "loader", "--file", a.minimal, "--version", a.version, "--board", b, "--out", loader)
    from coreos import pimg as cpimg, CHIP_IDS, BOARDS, PACKAGE_KIND  # the same call mkimage_dbg.sh makes
    t, name = PACKAGE_KIND["recovery"]
    with open(a.rec, "rb") as fh, open(kitten, "wb") as out_fh:
        out_fh.write(cpimg.make_image(name, a.version, t, 0, fh.read(), CHIP_IDS[BOARDS[b]]))
    for p in (kernel, loader, kitten):
        ps("keys", "sign", "--key", a.key, "--image", p, "--key-id", "1")
    ps("bootpkg", "build", "--board", b)
    ps("keys", "sign", "--key", a.key, "--image", os.path.join(ROOT, "bootpkg", "build", b, "bootpkg.bin"), "--out", bootpkg,
       "--key-id", "1")
    for p in (kernel, loader, kitten, bootpkg):
        ps("keys", "verify", "--key", a.pub, "--image", p)

    bp = pimg.header_fields(pimg.parse_header(open(bootpkg, "rb").read()))["version"]
    new = [component_entry(kernel, "kernel", b, os.path.basename(kernel), a.version),
           component_entry(kitten, "kittenos", b, os.path.basename(kitten), a.version),
           component_entry(loader, "loader", b, os.path.basename(loader), a.version),
           component_entry(bootpkg, "bootpkg", b, os.path.basename(bootpkg), bp)]
    shutil.copyfile(a.legal, os.path.join(r1, "legal.txt"))

    # ---- carry over what is already published for the other boards and the modules, unchanged
    carried, notes = [], []
    if not a.offline:
        old_text = urllib.request.urlopen(RELEASE1 + "recovery.manifest", timeout=60).read().decode()
        _, old = parse_manifest(old_text)
        for s in old:
            if s.get("board") == b and s.get("component") in ("kernel", "kittenos", "loader", "bootpkg"):
                notes.append("replaced %s %s %s -> %s" % (s["board"], s["component"], s["version"],
                                                           next(n["version"] for n in new if n["component"] == s["component"])))
                continue
            dest = os.path.join(r1, s["file"])
            if not os.path.exists(dest):
                fetch(RELEASE1 + s["file"], dest)
            if sha256(dest) != s["sha256"] or os.path.getsize(dest) != int(s["size"]):
                sys.exit("%s does not match the published manifest" % s["file"])
            carried.append(s)
        mod_text = urllib.request.urlopen(RELEASE2 + "modules.manifest", timeout=60).read().decode()
        with open(os.path.join(r2, "modules.manifest"), "w", encoding="utf-8") as fh:
            fh.write(mod_text)
        _, mods = parse_manifest(mod_text)
        for s in mods:
            dest = os.path.join(r2, s["file"])
            if not os.path.exists(dest):
                fetch(RELEASE2 + s["file"], dest)
            if sha256(dest) != s["sha256"]:
                sys.exit("module %s (%s) does not match modules.manifest" % (s["file"], s["board"]))

    lines = ["release=%s+%s" % (a.version, b), "released=%s" % time.strftime("%Y-%m-%d"), ""]
    for s in new + carried:
        lines.append(stanza_text(s))
    with open(os.path.join(r1, "recovery.manifest"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))

    # ---- sums and a readme
    sums = []
    for d in (r1, r2):
        for fn in sorted(os.listdir(d)):
            sums.append("%s  %s/%s" % (sha256(os.path.join(d, fn)), os.path.basename(d), fn))
    with open(os.path.join(out, "SHA256SUMS.txt"), "w") as fh:
        fh.write("\n".join(sums) + "\n")
    with open(os.path.join(out, "README.txt"), "w") as fh:
        fh.write(README % {"v": a.version, "b": b, "bp": bp, "notes": "\n".join("  - " + n for n in notes) or "  (offline: nothing carried over)"})
    print("wrote", out)
    for n in new:
        print("  %-9s %s  %s" % (n["component"], n["version"], n["file"]))


README = """PURR OS release assets (built %(b)s images at version %(v)s)

WHY: the published recovery files were old (kernel 0.6.1, KittenOS 0.2.8, loader 0.1.0), so a recovery downgraded a device to a
system without the new setup wizard, permissions, power management and boot screens. These images are higher than anything
published, so a device never moves backwards by recovering from them.

THE TWO FOLDERS ARE TWO GITHUB RELEASES (the firmware reads each manifest from its own release; a file named in a manifest is
fetched from the same release as that manifest):

  JumpingJaguar1/   upload as the assets of release tag JumpingJaguar1
      recovery.manifest        what the loader and KittenOS read first
      kernel_%(b)s.cat         NEW %(v)s   the full system
      kittenos_%(b)s.kitt      NEW %(v)s   KittenOS (recovery and first-run setup)
      loader_%(b)s.cat         NEW %(v)s   the internet recovery loader
      bootpkg_%(b)s.cat        NEW %(bp)s   the boot menu and boot screens
      legal.txt                NEW         "The legal stuff & cool people" (loaded at run time; edit it, re-upload, done)
      ... plus the other boards' files, carried over unchanged from the current release
  JumpingJaguar2/   upload as the assets of release tag JumpingJaguar2
      modules.manifest and the module files: unchanged copies of what is published (no module changed)

TO UPLOAD (GitHub release page -> Edit): delete the old assets with the same names, drag these in, save. An asset name must be
unique inside a release, so replace rather than add. Then check from any computer:
      curl -L https://github.com/PastorCatto/PURR-OS-ESP32/releases/download/JumpingJaguar1/recovery.manifest

WHAT CHANGED IN THE MANIFEST:
%(notes)s

SECURITY: every image is signed with the boot key (id 1) and verified against it before the loader writes anything.
SHA256SUMS.txt lists every file.

NOT INCLUDED: the bootloader and partition table (flashed over USB, not over the air). The new boot package works with the old
bootloader, but the "why we are recovering" page needs the new bootloader to show real reasons.
"""

if __name__ == "__main__":
    main()
