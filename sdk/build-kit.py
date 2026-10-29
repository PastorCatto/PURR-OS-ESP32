#!/usr/bin/env python3
"""Builds the PURR Developer Kit: one self-contained folder (the kit's root is laid out like the repository, trimmed to what the tools need).

    python3 sdk/build-kit.py [--out ~/Desktop/PURR-SDK] [--images DIR] [--no-python] [--no-toolchain] [--keep-cache]

What goes in (see sdk/DESIGN.md):
    bin/purr, PURR-Studio.command        launchers (use the bundled Python)
    python/                              a standalone CPython 3.12 (python-build-standalone) with `cryptography` added
    toolchain/                           the xtensa-esp-elf compiler, trimmed to the ESP32-S3 and C
    sdk/                                 purrsdk (the library), studio (the app), purr.py, boards, data, docs, tests
    purrstrap/                           the build tool
    PurrOS/components/...                the ABI headers the builders compile against, and the device's built-in public keys
    AppSDK/, Modules/, Drivers/, bootpkg/  starters, reference driver sources, specs, and the boot package source
    images/                              the prebuilt parts of a system (kernel, KittenOS, loader, boot package, bootloader)
    VERSION, SDK-MANIFEST.json, README.txt, LICENSE-TODO

Downloads (cached in out/sdk-cache): the Python build and the `cryptography` wheels. Needs network the first time.
"""
import argparse
import glob
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from purrsdk import __version__  # noqa: E402

HOST = "macos-x86_64"
PBS_API = "https://api.github.com/repos/astral-sh/python-build-standalone/releases/latest"
PY_MINOR = "cpython-3.12"
PY_TAG = "x86_64-apple-darwin-install_only.tar.gz"
CACHE = os.path.join(ROOT, "out", "sdk-cache")

IGNORE = shutil.ignore_patterns("__pycache__", "*.pyc", ".DS_Store", "build", ".state.json", "*.orig")


def say(text):
    print(text, flush=True)


def copy(src, dst, ignore=IGNORE):
    s = os.path.join(ROOT, src)
    if not os.path.exists(s):
        raise SystemExit("missing %s" % s)
    d = os.path.join(KIT, dst)
    if os.path.isdir(s):
        shutil.copytree(s, d, ignore=ignore, dirs_exist_ok=True)
    else:
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copy2(s, d)


def copy_glob(pattern, dst_dir):
    hits = glob.glob(os.path.join(ROOT, pattern))
    if not hits:
        raise SystemExit("nothing matches %s" % pattern)
    os.makedirs(os.path.join(KIT, dst_dir), exist_ok=True)
    for h in hits:
        shutil.copy2(h, os.path.join(KIT, dst_dir, os.path.basename(h)))


def download(url, dest):
    if os.path.isfile(dest) and os.path.getsize(dest) > 0:
        return dest
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    say("  downloading %s" % url.rsplit("/", 1)[-1])
    req = urllib.request.Request(url, headers={"User-Agent": "purr-sdk-build"})
    with urllib.request.urlopen(req, timeout=120) as r, open(dest + ".part", "wb") as fh:
        shutil.copyfileobj(r, fh)
    os.rename(dest + ".part", dest)
    return dest


SDIST_OK = ("esptool",)


def _wheel_ok(filename):
    """Can this wheel run on CPython 3.12 on an Intel Mac?"""
    import re
    parts = filename[:-4].split("-")
    pytag, abitag, plat = parts[-3:]
    pyok = False
    for t in pytag.split("."):
        m = re.match(r"^cp3(\d+)$", t)
        if t in ("py3", "py312", "cp312"):
            pyok = True
        elif m and abitag.startswith("abi3") and not abitag.endswith("t") and int(m.group(1)) <= 12:
            pyok = True
    if abitag not in ("none", "abi3", "cp312") and not abitag.startswith("abi3"):
        pyok = False
    platok = plat == "any" or any(p.startswith("macosx_") and (p.endswith("x86_64") or p.endswith("universal2")) for p in plat.split("."))
    return pyok and platok


def fetch_wheels(names, dest):
    """Downloads `names` and everything they need (from PyPI's JSON API, no pip involved) for CPython 3.12 on an Intel Mac.
    Newest releases first; a release with no wheel that fits is skipped (a package may drop an old platform)."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.__file__), "site-packages"))
    try:
        from pip._vendor.packaging.requirements import Requirement
        from pip._vendor.packaging.version import Version
    except ImportError:
        raise SystemExit("this host Python has no pip to borrow a requirement parser from")
    env = {"python_version": "3.12", "python_full_version": "3.12.0", "sys_platform": "darwin", "platform_system": "Darwin", "os_name": "posix",
           "platform_machine": "x86_64", "implementation_name": "cpython", "platform_python_implementation": "CPython", "extra": ""}
    done, queue, got = set(), list(names), []

    def get_json(url):
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "purr-sdk-build"}), timeout=60) as r:
            return json.load(r)

    while queue:
        req = queue.pop(0)
        r = Requirement(req) if not isinstance(req, str) or any(c in req for c in "<>=!;[") else Requirement(req)
        key = r.name.lower().replace("_", "-")
        if key in done:
            continue
        done.add(key)
        data = get_json("https://pypi.org/pypi/%s/json" % r.name)
        versions = sorted((v for v in data["releases"] if data["releases"][v]), key=lambda v: Version(v), reverse=True)
        chosen = None
        for v in versions:
            ver = Version(v)
            if ver.is_prerelease or not r.specifier.contains(ver, prereleases=False):
                continue
            files = [f for f in data["releases"][v] if f["filename"].endswith(".whl") and not f.get("yanked") and _wheel_ok(f["filename"])]
            if files:
                files.sort(key=lambda f: (f["filename"].endswith("none-any.whl"), "universal2" in f["filename"]), reverse=True)
                chosen = (v, files[0])
                break
        if chosen is None and key in SDIST_OK:                      # pure Python published only as source (esptool): use the newest source release
            for v in versions:
                src = [f for f in data["releases"][v] if f["filename"].endswith(".tar.gz") and not f.get("yanked")]
                if src and not Version(v).is_prerelease:
                    chosen = (v, src[0])
                    break
        if chosen is None:
            raise SystemExit("no wheel of %s fits CPython 3.12 on an Intel Mac" % r.name)
        v, f = chosen
        path = download(f["url"], os.path.join(dest, f["filename"]))
        got.append(path)
        info = get_json("https://pypi.org/pypi/%s/%s/json" % (r.name, v))["info"]
        for dep in info.get("requires_dist") or []:
            d = Requirement(dep)
            if d.marker is None or d.marker.evaluate(env):
                queue.append(str(d.name) + (str(d.specifier) if d.specifier else ""))
    return got


def bundle_python():
    say("Python...")
    with urllib.request.urlopen(urllib.request.Request(PBS_API, headers={"User-Agent": "purr-sdk-build"}), timeout=60) as r:
        rel = json.load(r)
    asset = next((a for a in rel["assets"] if PY_MINOR in a["name"] and a["name"].endswith(PY_TAG) and "stripped" not in a["name"]), None)
    if asset is None:
        raise SystemExit("cannot find a %s build for this host in the python-build-standalone release %s" % (PY_MINOR, rel.get("tag_name")))
    tgz = download(asset["browser_download_url"], os.path.join(CACHE, asset["name"].replace("+", "_")))
    with tarfile.open(tgz) as t:
        t.extractall(KIT)                                       # unpacks to <kit>/python
    # the packages the tools need, as wheels (a wheel is a zip; unzipping it into site-packages is installing it)
    wheels = os.path.join(CACHE, "wheels")
    os.makedirs(wheels, exist_ok=True)
    site = glob.glob(os.path.join(KIT, "python", "lib", "python3.*", "site-packages"))[0]
    for w in fetch_wheels(["cryptography", "esptool"], wheels):
        if w.endswith(".whl"):
            with zipfile.ZipFile(w) as z:
                z.extractall(site)
        else:                                                     # a source release of pure Python: its importable folders are the package
            with tarfile.open(w) as t:
                top = t.getnames()[0].split("/")[0]
                members = [m for m in t.getmembers() if m.name.split("/")[0] == top and len(m.name.split("/")) > 1]
                tmp = os.path.join(CACHE, "sdist-" + top)
                shutil.rmtree(tmp, ignore_errors=True)
                t.extractall(tmp)
            root = os.path.join(tmp, top)
            for d in sorted(os.listdir(root)):
                if os.path.isfile(os.path.join(root, d, "__init__.py")) and not d.startswith(("test", "docs", "ci", "flasher_stub")):
                    shutil.copytree(os.path.join(root, d), os.path.join(site, d), dirs_exist_ok=True, ignore=IGNORE)
        say("  added %s" % os.path.basename(w))
    return asset["name"]


TC_DROP_BIN = ("gdb", "g++", "c++", "gcov", "gprof", "lto-dump", "cc1plus")
TC_KEEP_PREFIXES = ("xtensa-esp-elf-", "xtensa-esp32s3-elf-")


def bundle_toolchain():
    say("Toolchain...")
    found = sorted(glob.glob(os.path.expanduser("~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf")))
    if not found:
        raise SystemExit("no xtensa-esp-elf toolchain in ~/.espressif/tools (install ESP-IDF first), or pass --no-toolchain")
    src = found[-1]
    dst = os.path.join(KIT, "toolchain")

    def ignore(directory, names):
        rel = os.path.relpath(directory, src)
        drop = set()
        for n in names:
            full = os.path.join(rel, n)
            if rel == "bin" and not n.startswith(TC_KEEP_PREFIXES):
                drop.add(n)
            elif rel == "bin" and any(w in n for w in TC_DROP_BIN):
                drop.add(n)
            elif rel == "share":
                drop.add(n)
            elif rel.startswith("lib/gcc/xtensa-esp-elf/") and n in ("esp32", "esp32s2", "plugin"):
                drop.add(n)
            elif rel == "xtensa-esp-elf/lib" and n in ("esp32", "esp32s2"):
                drop.add(n)
            elif rel.startswith("xtensa-esp-elf/lib") and n in ("libstdc++.a", "libsupc++.a", "libstdc++_nano.a", "libsupc++_nano.a"):
                drop.add(n)
            elif rel == "xtensa-esp-elf/include" and n == "c++":
                drop.add(n)
        return drop

    shutil.copytree(src, dst, symlinks=True, ignore=ignore)
    for root, _d, files in os.walk(os.path.join(dst, "libexec")):
        for f in files:
            if f == "cc1plus":
                os.remove(os.path.join(root, f))
    size = sum(os.path.getsize(os.path.join(r, f)) for r, _d, fs in os.walk(dst) for f in fs if not os.path.islink(os.path.join(r, f)))
    say("  %d MB" % (size >> 20))


LAUNCHER = """#!/bin/sh
# The PURR Developer Kit's command. Uses the kit's own Python and compiler; nothing else on this computer is touched.
HERE="$(cd "$(dirname "$0")/.." && pwd)"
PY="$HERE/python/bin/python3"
if [ -n "$PURR_SYSTEM_PYTHON" ] || [ ! -x "$PY" ]; then
    PY="$(command -v python3)"
elif [ ! -f "$HERE/.python-ok" ]; then
    # The first time, make sure the kit's Python really starts (a computer that is busy checking a new program can stall it for a while).
    "$PY" -c "" >/dev/null 2>&1 &
    pid=$!
    n=0
    while kill -0 "$pid" 2>/dev/null && [ "$n" -lt 80 ]; do sleep 0.1; n=$((n + 1)); done
    if kill -0 "$pid" 2>/dev/null; then
        echo "(the kit's own Python is not answering yet, so I will use this computer's Python for now; restarting the computer usually clears it)" >&2
        PY="$(command -v python3)"
    else
        touch "$HERE/.python-ok" 2>/dev/null
    fi
fi
export PURR_PYTHON="$PY"
export PATH="$HERE/toolchain/bin:$PATH"
exec "$PY" "$HERE/sdk/purr.py" "$@"
"""

STUDIO_COMMAND = """#!/bin/sh
cd "$(dirname "$0")" && exec ./bin/purr studio
"""

README = """PURR Developer Kit %(version)s  (%(host)s)

Hello! This folder is everything you need to make things for PURR OS: apps, drivers (GPS, LoRa, LTE, Wi-Fi, Bluetooth and more), boards,
and your own flavor of the system with its own names and colours. Nothing else has to be installed, and nothing outside this folder is touched.

START
  Double-click  PURR-Studio.command   (or run  bin/purr  in a terminal).
  PURR Studio opens. Drop any image on it (a .cat, a .kitt, a release folder, a whole flash image) and it tells you what is inside.

IN A TERMINAL
  bin/purr doctor                  is the kit whole?
  bin/purr inspect FILE            look inside an image
  bin/purr new driver gnss-mine --class gnss
  bin/purr build gnss-mine --key developer
  bin/purr keys new --role developer
  bin/purr serve                   hand builds to a device on your network
  bin/purr --help

READ
  sdk/docs/           the guides (they are also in Studio, under "docs")
  Modules/drivers/    real drivers to learn from: display, touch, keyboard, GPS, LoRa, modem, Wi-Fi, Bluetooth, trackball
  reference-drivers/  those drivers already built for the T-Deck Plus (unsigned: sign them with your key)
  sdk/boards/         the T-Deck Plus, described as data

Built %(date)s from PURR OS sources. See LICENSE-TODO.
"""

LICENSE_TODO = """The licence for the PURR Developer Kit has not been chosen yet.
This file is a placeholder: until it is replaced, the kit is for the owner's own use.
Third-party parts keep their own licences: Python (PSF), cryptography (Apache-2.0 / BSD), the Espressif toolchain (GPL with the GCC runtime exception).
"""


def write(path, text, mode=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as fh:
        fh.write(text)
    if mode:
        os.chmod(path, mode)


def manifest():
    files = {}
    for dp, dns, fns in os.walk(KIT):
        rel_dir = os.path.relpath(dp, KIT)
        if rel_dir.split(os.sep)[0] in ("python", "toolchain"):
            dns[:] = []
            continue
        for f in fns:
            if f in ("SDK-MANIFEST.json", ".DS_Store"):
                continue
            full = os.path.join(dp, f)
            h = hashlib.sha256()
            with open(full, "rb") as fh:
                for chunk in iter(lambda: fh.read(1 << 20), b""):
                    h.update(chunk)
            files[os.path.relpath(full, KIT).replace(os.sep, "/")] = h.hexdigest()
    return files


def main():
    global KIT
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.expanduser("~/Desktop/PURR-SDK"))
    ap.add_argument("--images", help="the release folder with the prebuilt parts (default: the newest PURR-OS-release-* on the Desktop, its JumpingJaguar1)")
    ap.add_argument("--no-python", action="store_true")
    ap.add_argument("--no-toolchain", action="store_true")
    a = ap.parse_args()

    name = "purr-sdk-%s-%s" % (__version__, HOST)
    out = os.path.abspath(os.path.expanduser(a.out))
    KIT = os.path.join(out, name)
    if os.path.exists(KIT):
        say("Replacing %s" % KIT)
        shutil.rmtree(KIT)
    os.makedirs(KIT)
    os.makedirs(CACHE, exist_ok=True)

    say("Copying the sources...")
    for s, d in (("sdk/purrsdk", "sdk/purrsdk"), ("sdk/studio", "sdk/studio"), ("sdk/boards", "sdk/boards"), ("sdk/data", "sdk/data"), ("sdk/docs", "sdk/docs"),
                 ("sdk/tests", "sdk/tests"), ("sdk/purr.py", "sdk/purr.py"), ("sdk/DESIGN.md", "sdk/DESIGN.md"),
                 ("purrstrap/lib", "purrstrap/lib"), ("purrstrap/scripts", "purrstrap/scripts"), ("purrstrap/purrstrap.py", "purrstrap/purrstrap.py"),
                 ("purrstrap/freestanding", "purrstrap/freestanding"), ("purrstrap/SPEC.md", "purrstrap/SPEC.md"),
                 ("PurrOS/components/coreos/include", "PurrOS/components/coreos/include"), ("PurrOS/components/kernel/include", "PurrOS/components/kernel/include"),
                 ("PurrOS/components/coreos/keys", "PurrOS/components/coreos/keys"), ("PurrOS/components/kernel/boards", "PurrOS/components/kernel/boards"),
                 ("PurrOS/components/coreos/src/purr_menu.c", "PurrOS/components/coreos/src/purr_menu.c"),
                 ("PurrOS/components/kernel/src/purr_font_data.c", "PurrOS/components/kernel/src/purr_font_data.c"),
                 ("AppSDK", "AppSDK"), ("Modules/drivers", "Modules/drivers"), ("Modules/SPEC.md", "Modules/SPEC.md"), ("Drivers/SPEC.md", "Drivers/SPEC.md"),
                 ("Keys/SPEC.md", "Keys/SPEC.md"), ("bootloader/SPEC.md", "bootloader/SPEC.md"), ("bootpkg/src", "bootpkg/src"), ("bootpkg/boards", "bootpkg/boards"),
                 ("bootpkg/link", "bootpkg/link")):
        copy(s, d)
    for h in ("lfs.h", "lfs_util.h"):
        copy("PurrOS/components/littlefs/" + h, "PurrOS/components/littlefs/" + h)
    shutil.rmtree(os.path.join(KIT, "sdk", "purrsdk", "__pycache__"), ignore_errors=True)

    say("The prebuilt parts...")
    images = a.images
    if not images:
        cands = sorted(glob.glob(os.path.expanduser("~/Desktop/PURR-OS-release-[0-9]*")), key=lambda p: [int(x) if x.isdigit() else 0 for x in os.path.basename(p).split("-")[-1].split(".")])
        cands = [c for c in cands if os.path.isdir(os.path.join(c, "JumpingJaguar1")) and "prev" not in c and "old" not in c]
        images = os.path.join(cands[-1], "JumpingJaguar1") if cands else None
    if images and os.path.isdir(images):
        os.makedirs(os.path.join(KIT, "images"), exist_ok=True)
        n = 0
        for f in sorted(os.listdir(images)):
            if f.endswith((".cat", ".kitt", ".manifest")) or f == "legal.txt":
                shutil.copy2(os.path.join(images, f), os.path.join(KIT, "images", f))
                n += 1
        say("  %d files from %s" % (n, images))
    else:
        say("  (no release folder found: the kit will have no prebuilt parts; pass --images)")

    drivers = [f for f in glob.glob(os.path.join(ROOT, "out", "drivers", "*.cat"))]
    if drivers:
        os.makedirs(os.path.join(KIT, "reference-drivers"), exist_ok=True)
        for f in drivers:
            shutil.copy2(f, os.path.join(KIT, "reference-drivers", os.path.basename(f)))
        write(os.path.join(KIT, "reference-drivers", "README.txt"), "The reference drivers, built for the T-Deck Plus and NOT signed.\nSign them with your key (bin/purr sign FILE --key developer) before a device will take them,\nor build them yourself from Modules/drivers/ (bin/purr new driver ...).\n")
        say("  %d reference drivers (unsigned)" % len(drivers))

    py_name = None
    if not a.no_python:
        py_name = bundle_python()
    if not a.no_toolchain:
        bundle_toolchain()

    write(os.path.join(KIT, "bin", "purr"), LAUNCHER, 0o755)
    write(os.path.join(KIT, "PURR-Studio.command"), STUDIO_COMMAND, 0o755)
    write(os.path.join(KIT, "VERSION"), __version__ + "\n")
    write(os.path.join(KIT, "README.txt"), README % {"version": __version__, "host": HOST, "date": time.strftime("%Y-%m-%d")})
    write(os.path.join(KIT, "LICENSE-TODO"), LICENSE_TODO)
    say("Hashing the kit's own files...")
    m = {"kit": __version__, "host": HOST, "built": time.strftime("%Y-%m-%d %H:%M:%S"), "python": py_name, "files": manifest()}
    with open(os.path.join(KIT, "SDK-MANIFEST.json"), "w") as fh:
        json.dump(m, fh, indent=1)
        fh.write("\n")
    total = sum(os.path.getsize(os.path.join(r, f)) for r, _d, fs in os.walk(KIT) for f in fs if not os.path.islink(os.path.join(r, f)))
    say("\nDone: %s (%d MB, %d hashed files)" % (KIT, total >> 20, len(m["files"])))


KIT = None
if __name__ == "__main__":
    main()
