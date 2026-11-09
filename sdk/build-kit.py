#!/usr/bin/env python3
"""Builds the PURR Developer Kit: one self-contained folder (the kit's root is laid out like the repository, trimmed to what the tools need).

    python3 sdk/build-kit.py [--out ~/Desktop/PURR-SDK] [--images DIR] [--no-python] [--no-toolchain] [--no-emulator] [--emulator-src DIR] [--keep-cache]

What goes in (see sdk/DESIGN.md):
    bin/purr, PURR-Studio.command        launchers (use the bundled Python)
    python/                              a standalone CPython 3.12 (python-build-standalone) with `cryptography` added
    toolchain/                           the xtensa-esp-elf compiler, trimmed to the ESP32-S3 and C
    sdk/                                 purrsdk (the library), studio (the app), purr.py, boards, data, docs, tests
    purrstrap/                           the build tool
    PurrOS/components/...                the ABI headers the builders compile against, and the device's built-in public keys
    PurrOS/partitions/                   each board's partition table (the image maker writes them into a whole flash image)
    AppSDK/, Modules/, Drivers/, bootpkg/  starters, reference driver sources, specs, and the boot package source
    images/                              the prebuilt parts of a system (kernel, KittenOS, loader, boot package, bootloader)
    emulator/                            the standalone emulator: esp32-emulator (window), headless_tap, the patched QEMU with its own libraries
                                         (re-pointed, so the folder can be moved), boards/, licences/, source/ (the QEMU source offer), run-emulator
    VERSION, SDK-MANIFEST.json, README.txt, LICENSE-TODO

Downloads (cached in out/sdk-cache): the Python build and the `cryptography` wheels. Needs network the first time.
The emulator is built from a checkout of the emulator project (read only; its build goes to out/sdk-cache/emulator-target) with the
compiler on THIS computer; the kit's user needs no compiler. The patched QEMU is taken from that checkout's vendor/qemu.
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

IS_WIN = sys.platform == "win32"                 # a Windows kit is built on a Windows computer (written 2026-11-08, NOT yet run on Windows)
HOST = "windows-x86_64" if IS_WIN else "macos-x86_64"
PBS_API = "https://api.github.com/repos/astral-sh/python-build-standalone/releases/latest"
PY_MINOR = "cpython-3.12"
PY_TAG = "x86_64-pc-windows-msvc-install_only.tar.gz" if IS_WIN else "x86_64-apple-darwin-install_only.tar.gz"
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
    if IS_WIN:
        platok = plat == "any" or any(p == "win_amd64" for p in plat.split("."))
    else:
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
    env = {"python_version": "3.12", "python_full_version": "3.12.0", "sys_platform": "win32" if IS_WIN else "darwin", "platform_system": "Windows" if IS_WIN else "Darwin",
           "os_name": "nt" if IS_WIN else "posix", "platform_machine": "AMD64" if IS_WIN else "x86_64", "implementation_name": "cpython", "platform_python_implementation": "CPython", "extra": ""}
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
    site = (os.path.join(KIT, "python", "Lib", "site-packages") if IS_WIN else glob.glob(os.path.join(KIT, "python", "lib", "python3.*", "site-packages"))[0])
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
                if os.path.isfile(os.path.join(root, d, "__init__.py")) and not d.startswith(("test", "docs", "ci", "flasher_stub", "espefuse")):   # espefuse burns eFuses: the kit never ships it
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
        rel = os.path.relpath(directory, src).replace(os.sep, "/")        # (on Windows the folders are joined with backslashes: without this none of the trimming below matched and the kit kept the whole compiler)
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

    shutil.copytree(src, dst, symlinks=not IS_WIN, ignore=ignore)      # (Windows: copying a link needs a privilege)
    for root, _d, files in os.walk(os.path.join(dst, "libexec")):
        for f in files:
            if f in ("cc1plus", "cc1plus.exe"):
                os.remove(os.path.join(root, f))
    size = sum(os.path.getsize(os.path.join(r, f)) for r, _d, fs in os.walk(dst) for f in fs if not os.path.islink(os.path.join(r, f)))
    say("  %d MB" % (size >> 20))


EMU_SRC_DEFAULT = os.path.expanduser("~/Projects/ESP32-Emulator")
EMU_TARGET = os.path.join(CACHE, "emulator-target")
# Only boards for hardware that is public and ours to show go in the kit; a board file the emulator project adds later stays out until it is listed here.
EMU_BOARDS = ("t-deck-plus", "t-deck-plus-vm", "generic-esp32s3", "cyd-esp32-2432s028r", "cyd-s024c", "cyd-s028r")
EMU_ROMS = ("esp32s3_rev0_rom.bin", "esp32-v3-rom.bin", "esp32-v3-rom-app.bin")       # the S3 (the kit's chip) and the plain ESP32 (the CYD boards)
EMU_SYSTEM_PREFIXES = ("/usr/lib/", "/System/")
# Where each bundled library's licence text comes from: (label, a file in the package manager's doc folder or a member of its source tarball).
EMU_LIB_LICENSES = (
    ("glib (glib, gobject, gio, gmodule), LGPL-2.1+", "doc", "glib/COPYING"),
    ("libgcrypt, LGPL-2.1+", "doc", "libgcrypt/COPYING.LIB"),
    ("libgpg-error, LGPL-2.1+", "doc", "libgpg-error-*/COPYING.LIB"),
    ("libiconv, LGPL-2.1+", "dist", ("libiconv/libiconv-*.tar.gz", r"^[^/]+/COPYING\.LIB$")),
    ("libintl (gettext-runtime), LGPL-2.1+", "dist", ("gettext/gettext-*.tar.xz", r"^[^/]+/gettext-runtime/intl/COPYING\.LIB$")),
    ("pcre2, BSD-3-Clause", "doc", "pcre2/LICENCE.md"),
    ("libffi, MIT", "dist", ("libffi/libffi-*.tar.gz", r"^[^/]+/LICENSE$")),
    ("pixman, MIT", "doc", "libpixman/COPYING"),
    ("libslirp, BSD-3-Clause", "dist", ("libslirp/libslirp-*.tar.bz2", r"^[^/]+/COPYRIGHT$")),
    ("zlib, zlib licence", "dist", ("zlib/zlib-*.tar.xz", r"^[^/]+/LICENSE$")),
    ("zstd, BSD-3-Clause (offered as BSD or GPL-2.0; used under BSD)", "dist", ("zstd/zstd-*.tar.gz", r"^[^/]+/LICENSE$")),
    ("bzip2, bzip2 licence (BSD-style)", "dist", ("bzip2/bzip2-*.tar.gz", r"^[^/]+/LICENSE$")),
)


def _run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        raise SystemExit("%s failed:\n%s%s" % (" ".join(cmd[:3]), r.stdout[-2000:], r.stderr[-2000:]))
    return r.stdout


def _macho_deps(path):
    out = _run(["otool", "-L", path]).splitlines()[1:]
    return [l.split()[0] for l in out if l.strip()]


def _macho_rpaths(path):
    lines = _run(["otool", "-l", path]).splitlines()
    return [lines[i + 2].split()[1] for i, l in enumerate(lines) if l.strip() == "cmd LC_RPATH"]


def _is_local(dep):
    """A library that is not part of macOS and not already relative: the kind that has to travel with the program."""
    return dep.startswith("/") and not dep.startswith(EMU_SYSTEM_PREFIXES)


def relocate_qemu(src_bin, dest_root):
    """Copies the patched QEMU and every non-system library it needs (found recursively with otool), then re-points them with
    install_name_tool: the program finds its libraries at @executable_path/../lib, the libraries find each other at @loader_path.
    Returns (prefix the libraries came from, [library file names]). The result runs from any folder and needs no package manager."""
    bindir, libdir = os.path.join(dest_root, "bin"), os.path.join(dest_root, "lib")
    os.makedirs(bindir, exist_ok=True)
    os.makedirs(libdir, exist_ok=True)
    binary = os.path.join(bindir, os.path.basename(src_bin))
    shutil.copy2(src_bin, binary)
    os.chmod(binary, 0o755)
    copied, queue, prefix = {}, [(binary, src_bin)], None         # install name -> file in the kit
    while queue:
        dest, original = queue.pop()
        for dep in _macho_deps(original):
            if not _is_local(dep) or dep in copied:
                continue
            real = os.path.realpath(dep)
            if not os.path.isfile(real):
                raise SystemExit("the emulator's QEMU needs %s, which is not on this computer" % dep)
            prefix = prefix or os.path.dirname(os.path.dirname(dep))
            target = os.path.join(libdir, os.path.basename(dep))
            shutil.copy2(real, target)
            os.chmod(target, 0o644)
            copied[dep] = target
            queue.append((target, real))
    files = [binary] + sorted(copied.values())
    for f in files:
        args = ["install_name_tool"]
        if f != binary:
            args += ["-id", "@loader_path/" + os.path.basename(f)]
        for dep in _macho_deps(f):
            if dep in copied:
                new = ("@executable_path/../lib/" if f == binary else "@loader_path/") + os.path.basename(copied[dep])
                args += ["-change", dep, new]
        for rp in _macho_rpaths(f):
            args += ["-delete_rpath", rp]
        if len(args) > 1:
            _run(args + [f])
        _run(["codesign", "--force", "--sign", "-", f])              # the edit invalidated any signature; ad hoc is enough for a local program
    return prefix, [os.path.basename(f) for f in files[1:]]


def check_relocated(dest_root):
    """Fails unless nothing in dest_root points at an absolute path outside macOS (no package manager, no home folder)."""
    bad = []
    for dp, _d, fns in os.walk(dest_root):
        for fn in fns:
            f = os.path.join(dp, fn)
            if not (fn.endswith(".dylib") or os.path.basename(dp) == "bin"):
                continue
            bad += [(f, d) for d in _macho_deps(f) if _is_local(d)]
            bad += [(f, "rpath " + r) for r in _macho_rpaths(f) if r.startswith("/")]
    if bad:
        raise SystemExit("the relocated QEMU still points outside the kit:\n" + "\n".join("  %s -> %s" % b for b in bad))


def _license_files(directory, depth=2):
    """Licence-like files in a crate or package folder (top level, and one folder down), as {relative path: bytes}."""
    import re
    pat = re.compile(r"^((LICEN[SC]E|COPYING|NOTICE|UNLICENSE|COPYRIGHT|OFL|UFL)([-_. ].*)?|.*licen[sc]e.*\.(txt|md))$", re.I)
    found = {}
    for dp, dns, fns in os.walk(directory):
        if os.path.relpath(dp, directory).count(os.sep) >= depth - 1:
            dns[:] = []
        for fn in fns:
            if pat.match(fn):
                with open(os.path.join(dp, fn), "rb") as fh:
                    found[os.path.relpath(os.path.join(dp, fn), directory)] = fh.read()
    return found


def rust_crates(src):
    """The crates the emulator application is built from (normal and build dependencies for this host), from `cargo metadata`:
    [(name, version, licence, repository, folder)]. The emulator's own workspace members are left out."""
    meta = json.loads(_run(["cargo", "metadata", "--offline", "--locked", "--format-version", "1", "--filter-platform", "x86_64-apple-darwin"], cwd=src))
    nodes = {n["id"]: n for n in meta["resolve"]["nodes"]}
    pk = {p["id"]: p for p in meta["packages"]}
    root = next(p["id"] for p in meta["packages"] if p["name"] == "shell")
    seen, todo = set(), [root]
    while todo:
        i = todo.pop()
        if i in seen:
            continue
        seen.add(i)
        todo += [d["pkg"] for d in nodes[i]["deps"] if any(k["kind"] in (None, "build") for k in d["dep_kinds"])]
    members = set(meta["workspace_members"])
    return sorted((pk[i]["name"], pk[i]["version"], pk[i].get("license") or "see its licence file", pk[i].get("repository") or "", os.path.dirname(pk[i]["manifest_path"]))
                  for i in seen if i not in members)


def write_rust_licenses(src, dest):
    crates = rust_crates(src)
    os.makedirs(dest, exist_ok=True)
    index = ["Rust crates compiled into esp32-emulator and headless_tap (%d), with the licence each declares.\n" % len(crates),
             "Each crate is used under the terms named here; where it offers a choice (\"A OR B\") the choice is the user's.\n",
             "The licence texts are in RUST-LICENSES.txt.\n\n"]
    texts = {}                                                    # sha256 -> (text, [crate names])
    for name, ver, lic, repo, folder in crates:
        index.append("%-40s %-12s %s%s\n" % (name, ver, lic, ("   " + repo) if repo else ""))
        for rel, data in _license_files(folder).items():
            h = hashlib.sha256(data).hexdigest()
            texts.setdefault(h, (data, []))[1].append("%s %s" % (name, ver))
    write(os.path.join(dest, "RUST-CRATES.txt"), "".join(index))
    with open(os.path.join(dest, "RUST-LICENSES.txt"), "wb") as fh:
        fh.write(b"Licence and notice files shipped by the Rust crates listed in RUST-CRATES.txt. Identical texts are given once, followed by the crates that carry them.\n")
        for h, (data, who) in sorted(texts.items(), key=lambda kv: kv[1][1][0]):
            fh.write(b"\n" + b"=" * 78 + b"\nUsed by: " + ", ".join(sorted(set(who))).encode() + b"\n" + b"=" * 78 + b"\n")
            fh.write(data if data.endswith(b"\n") else data + b"\n")
    return len(crates), len(texts)


def write_library_licenses(prefix, dest):
    """The licence text of each library that travels with QEMU, taken from the package manager that built them (its doc folder or the source it unpacked)."""
    import re
    os.makedirs(dest, exist_ok=True)
    out, missing = [], []
    for label, kind, where in EMU_LIB_LICENSES:
        text = None
        if kind == "doc":
            hits = sorted(glob.glob(os.path.join(prefix, "share", "doc", where)))
            if hits:
                with open(hits[-1], "rb") as fh:
                    text = fh.read()
        else:
            pattern, member = where
            for tgz in sorted(glob.glob(os.path.join(prefix, "var", "macports", "distfiles", pattern))):
                with tarfile.open(tgz) as t:
                    m = next((m for m in t.getmembers() if re.match(member, m.name)), None)
                    if m:
                        text = t.extractfile(m).read()
                        break
        if text is None:
            missing.append(label)
            continue
        out.append(("=" * 78 + "\n%s\n" + "=" * 78 + "\n") % label)
        out.append(text.decode("utf-8", "replace").rstrip() + "\n\n")
    if missing:
        say("  WARNING: no licence text found for: " + "; ".join(missing))
    write(os.path.join(dest, "LIBRARIES.txt"), "Licences of the libraries that travel with QEMU (emulator/vendor/qemu/lib). Versions and sources: NOTICES-emulator.md.\n\n" + "".join(out))
    return len(out) // 2, missing


def _git(src, *args):
    r = subprocess.run(["git", "-C", src] + list(args), capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ""


EMU_LAUNCHER = """#!/bin/sh
# Starts the emulator from the PURR Developer Kit.
#   run-emulator FLASH-IMAGE BOARD [SD-IMAGE]                      open the window and boot
#   run-emulator --headless FLASH-IMAGE BOARD [SECONDS] [OUT.png]  no window: a script presses the screen and takes pictures
# BOARD is a name from boards/ (t-deck-plus-vm, t-deck-plus, ...) or the path of a board .toml file. Nothing outside this folder is used.
HERE="$(cd "$(dirname "$0")" && pwd)"
usage() { sed -n '2,5p' "$0" | sed 's/^# \\{0,1\\}//' >&2; echo "boards: $(ls "$HERE/boards" | sed 's/\\.toml$//' | tr '\\n' ' ')" >&2; exit 2; }
HEADLESS=
if [ "$1" = "--headless" ]; then HEADLESS=1; shift; fi
[ $# -ge 2 ] || usage
FLASH="$1"; BOARD="$2"; shift 2
[ -f "$FLASH" ] || { echo "run-emulator: no such flash image: $FLASH" >&2; exit 1; }
if [ -f "$BOARD" ]; then :; elif [ -f "$HERE/boards/$BOARD.toml" ]; then BOARD="$HERE/boards/$BOARD.toml"; else echo "run-emulator: no board '$BOARD'" >&2; usage; fi
CALLER="$(pwd)"
abs() { case "$1" in /*) printf '%s' "$1";; *) printf '%s' "$CALLER/$1";; esac; }       # the emulator runs in its own folder: paths you give are made absolute first
FLASH="$(abs "$FLASH")"; BOARD="$(abs "$BOARD")"
if [ -z "$HEADLESS" ]; then                                       # an SD card image, or other files, after the board
    n=$#; while [ "$n" -gt 0 ]; do a="$1"; shift; [ -f "$a" ] && a="$(abs "$a")"; set -- "$@" "$a"; n=$((n - 1)); done
fi
export ESP32_EMULATOR_QEMU="$HERE/vendor/qemu/bin/qemu-system-xtensa"
mkdir -p "$HERE/run" && cd "$HERE/run" || exit 1                 # the emulator keeps its working copy of the image here, so the image you pass is never changed
if [ -n "$HEADLESS" ]; then
    export BOARD_TOML="$BOARD"
    [ -z "$HEADLESS_CMD" ] || export HEADLESS_CMD="$(abs "$HEADLESS_CMD")"
    OUT="$(abs "${2:-$HERE/run/shot.png}")"
    mkdir -p "$(dirname "$OUT")"
    exec "$HERE/headless_tap" "$FLASH" "${1:-120}" "$OUT"
fi
exec "$HERE/esp32-emulator" "$FLASH" "$BOARD" "$@" --run
"""


def bundle_emulator(src):
    say("Emulator...")
    src = os.path.abspath(os.path.expanduser(src))
    qemu = os.path.join(src, "vendor", "qemu")
    if not (os.path.isfile(os.path.join(src, "Cargo.toml")) and os.path.isfile(os.path.join(qemu, "bin", "qemu-system-xtensa"))):
        raise SystemExit("no emulator checkout with a built vendor/qemu at %s (pass --emulator-src, or --no-emulator)" % src)
    dst = os.path.join(KIT, "emulator")
    os.makedirs(dst)
    # the program: built on this computer, into the cache (the emulator checkout is not touched)
    home = os.path.expanduser("~")
    env = dict(os.environ, CARGO_TARGET_DIR=EMU_TARGET, RUSTFLAGS=(os.environ.get("RUSTFLAGS", "") + " --remap-path-prefix=%s=/build" % home).strip())    # no home folder name in the programs' panic messages
    build = ["cargo", "build", "--release", "--locked", "-p", "shell", "--bin", "esp32-emulator", "--example", "headless_tap"]
    say("  cargo build (release)...")
    r = subprocess.run(build + ["--offline"], cwd=src, env=env, capture_output=True, text=True)
    if r.returncode:
        say("  (offline build failed; trying with the network)")
        r = subprocess.run(build, cwd=src, env=env, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("cargo build failed:\n" + r.stderr[-3000:])
    rel = os.path.join(EMU_TARGET, "release")
    for sp, dp in ((os.path.join(rel, "esp32-emulator"), "esp32-emulator"), (os.path.join(rel, "examples", "headless_tap"), "headless_tap")):
        shutil.copy2(sp, os.path.join(dst, dp))
        os.chmod(os.path.join(dst, dp), 0o755)
        _run(["codesign", "--force", "--sign", "-", os.path.join(dst, dp)])
    # QEMU, relocated, and what it loads at run time (the ROM images, found through -L)
    prefix, libs = relocate_qemu(os.path.join(qemu, "bin", "qemu-system-xtensa"), os.path.join(dst, "vendor", "qemu"))
    check_relocated(os.path.join(dst, "vendor", "qemu"))
    rom_dir = os.path.join(dst, "vendor", "qemu", "share", "qemu")
    os.makedirs(rom_dir)
    for n in EMU_ROMS:
        shutil.copy2(os.path.join(qemu, "share", "qemu", n), os.path.join(rom_dir, n))
    say("  QEMU with %d libraries from %s, ROMs: %s" % (len(libs), os.path.basename(prefix or "?"), ", ".join(EMU_ROMS)))
    # boards
    os.makedirs(os.path.join(dst, "boards"))
    for b in EMU_BOARDS:
        shutil.copy2(os.path.join(src, "boards", b + ".toml"), os.path.join(dst, "boards", b + ".toml"))
    # licences and the source offer
    lic = os.path.join(dst, "licenses")
    os.makedirs(lic)
    qsrc = os.path.join(src, "vendor", "src")
    for n, d in (("COPYING", "QEMU-GPL-2.0.txt"), ("COPYING.LIB", "QEMU-LGPL.txt")):
        if os.path.isfile(os.path.join(qsrc, n)):
            shutil.copy2(os.path.join(qsrc, n), os.path.join(lic, d))
    nlib, missing = write_library_licenses(prefix, lic) if prefix else (0, [])
    ncrate, ntext = write_rust_licenses(src, lic)
    say("  licences: %d libraries, %d Rust crates (%d distinct texts)" % (nlib, ncrate, ntext))
    shutil.copytree(os.path.join(src, "qemu"), os.path.join(dst, "source", "qemu-patches"), ignore=IGNORE)      # the device models and apply.sh: what is ours in the QEMU build
    shutil.copy2(os.path.join(src, "scripts", "fetch-qemu-src.sh"), os.path.join(dst, "source", "fetch-qemu-src.sh"))
    qemu_base = _git(qsrc, "rev-parse", "HEAD")
    qemu_tag = _git(qsrc, "describe", "--tags", "--exact-match")
    emu_commit = _git(src, "rev-parse", "HEAD")
    dirty = bool(_git(src, "status", "--porcelain", "--", "qemu"))                    # the QEMU side is what the source offer covers
    app_dirty = bool(_git(src, "status", "--porcelain", "--", "crates", "boards"))
    sha = hashlib.sha256(open(os.path.join(dst, "vendor", "qemu", "bin", "qemu-system-xtensa"), "rb").read()).hexdigest()
    write(os.path.join(dst, "SOURCE.txt"), """The corresponding source of the QEMU in this folder (GPL-2.0), as built here.

Base:     Espressif's QEMU fork, %(tag)s (commit %(base)s)
          https://github.com/espressif/qemu   (tag %(tag)s)
Changes:  source/qemu-patches/   our device models (devices/) and apply.sh, the script that grafts them into the base tree and
          edits its files by anchored insertion; build.sh configures and builds. source/fetch-qemu-src.sh fetches the base tree.
          The same files are in the emulator project: https://github.com/PastorCatto/ESP32-Emulator (folder qemu/), commit %(emu)s%(dirty)s.
To rebuild: fetch the base tree into vendor/src, run qemu/build.sh from the emulator project (needs glib, pixman and libslirp).
The libraries beside the program (vendor/qemu/lib) are used unchanged; their licences are in licenses/LIBRARIES.txt and their sources are named in
NOTICES-emulator.md.

Written offer: for at least three years from the date of this kit, on request we will give you a complete machine-readable copy of the
corresponding source of this QEMU build and of the libraries above, for a charge no more than the cost of physically doing so. Ask in the
issue tracker of the project repository named above.

Program built into this folder: qemu-system-xtensa sha256 %(sha)s
""" % {"tag": qemu_tag or "see source/fetch-qemu-src.sh", "base": qemu_base or "unknown", "emu": emu_commit or "unknown", "dirty": " plus uncommitted changes" if dirty else "", "sha": sha})
    shutil.copy2(os.path.join(ROOT, "sdk", "NOTICES-emulator.md"), os.path.join(dst, "NOTICES-emulator.md"))
    write(os.path.join(dst, "run-emulator"), EMU_LAUNCHER, 0o755)
    size = sum(os.path.getsize(os.path.join(r, f)) for r, _d, fs in os.walk(dst) for f in fs if not os.path.islink(os.path.join(r, f)))
    say("  emulator/ %.1f MB" % (size / 1048576.0))
    return {"qemu_sha256": sha, "emulator_commit": emu_commit, "emulator_uncommitted_changes": app_dirty, "qemu_base": qemu_base}


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

WIN_LAUNCHER = """@echo off
rem The PURR Developer Kit launcher for Windows. Written 2026-11-08, not yet run on Windows.
set "HERE=%~dp0.."
set "PURR_PYTHON=%HERE%\\python\\python.exe"
set "PATH=%HERE%\\toolchain\\bin;%PATH%"
"%PURR_PYTHON%" "%HERE%\\sdk\\purr.py" %*
"""

WIN_STUDIO = """@echo off
cd /d "%~dp0" && bin\\purr.cmd studio
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
  emulator/run-emulator IMAGE t-deck-plus-vm   open a flash image in the emulator (no device needed; see sdk/docs/12-emulator.md)

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
The emulator/ folder holds QEMU (GPL-2.0, with its source offer in emulator/SOURCE.txt), the libraries it needs and the Rust crates the emulator
application is built from: see emulator/NOTICES-emulator.md and emulator/licenses/.
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
    ap.add_argument("--no-emulator", action="store_true", help="leave out the emulator/ folder")
    ap.add_argument("--emulator-src", default=os.environ.get("PURR_EMULATOR_SRC", EMU_SRC_DEFAULT), help="a checkout of the emulator project with vendor/qemu built (default %s)" % EMU_SRC_DEFAULT)
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
                 ("sdk/tests", "sdk/tests"), ("sdk/purr.py", "sdk/purr.py"), ("sdk/DESIGN.md", "sdk/DESIGN.md"), ("sdk/NOTICES-emulator.md", "sdk/NOTICES-emulator.md"),
                 ("purrstrap/lib", "purrstrap/lib"), ("purrstrap/scripts", "purrstrap/scripts"), ("purrstrap/purrstrap.py", "purrstrap/purrstrap.py"),
                 ("purrstrap/freestanding", "purrstrap/freestanding"), ("purrstrap/SPEC.md", "purrstrap/SPEC.md"),
                 ("PurrOS/components/coreos/include", "PurrOS/components/coreos/include"), ("PurrOS/components/kernel/include", "PurrOS/components/kernel/include"),
                 ("PurrOS/components/coreos/keys", "PurrOS/components/coreos/keys"), ("PurrOS/components/kernel/boards", "PurrOS/components/kernel/boards"), ("PurrOS/partitions", "PurrOS/partitions"),
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
    if IS_WIN and not a.no_emulator:
        say("The emulator is not part of the Windows kit yet (it needs a Windows build of the patched QEMU).")
    emu = None if (a.no_emulator or IS_WIN) else bundle_emulator(a.emulator_src)

    if IS_WIN:
        write(os.path.join(KIT, "bin", "purr.cmd"), WIN_LAUNCHER)
        write(os.path.join(KIT, "PURR-Studio.cmd"), WIN_STUDIO)
    else:
        write(os.path.join(KIT, "bin", "purr"), LAUNCHER, 0o755)
        write(os.path.join(KIT, "PURR-Studio.command"), STUDIO_COMMAND, 0o755)
    write(os.path.join(KIT, "VERSION"), __version__ + "\n")
    readme = README % {"version": __version__, "host": HOST, "date": time.strftime("%Y-%m-%d")}
    if IS_WIN:
        readme = readme.replace("PURR-Studio.command", "PURR-Studio.cmd").replace("bin/purr", "bin\\purr")
    write(os.path.join(KIT, "README.txt"), readme)
    write(os.path.join(KIT, "LICENSE-TODO"), LICENSE_TODO)
    say("Hashing the kit's own files...")
    m = {"kit": __version__, "host": HOST, "built": time.strftime("%Y-%m-%d %H:%M:%S"), "python": py_name, "emulator": emu, "files": manifest()}
    with open(os.path.join(KIT, "SDK-MANIFEST.json"), "w") as fh:
        json.dump(m, fh, indent=1)
        fh.write("\n")
    total = sum(os.path.getsize(os.path.join(r, f)) for r, _d, fs in os.walk(KIT) for f in fs if not os.path.islink(os.path.join(r, f)))
    say("\nDone: %s (%d MB, %d hashed files)" % (KIT, total >> 20, len(m["files"])))


KIT = None
if __name__ == "__main__":
    main()
