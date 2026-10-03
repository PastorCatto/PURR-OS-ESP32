#!/usr/bin/env python3
"""Build and run the CoreOS host tests. Standard library only.

    python run.py            build everything and run every test
    python run.py verify     only tests whose name contains "verify"

Needs a host C compiler (gcc) and the micro-ecc source that ships with ESP-IDF
(used only as the P-256 backend for the tests). Both are looked for in the usual
places, and can be set with PURR_GCC and PURR_UECC_DIR.
"""

import glob
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.abspath(os.path.join(HERE, "..", ".."))
BUILD = os.path.join(HERE, "build")
COMPONENTS = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
LFS = os.path.join(COMPONENTS, "littlefs")
KERNEL = os.path.join(COMPONENTS, "kernel")


def find_gcc():
    if os.environ.get("PURR_GCC"):
        return os.environ["PURR_GCC"]
    found = shutil.which("gcc")
    if found:
        return found
    local = os.environ.get("LOCALAPPDATA", "")
    pattern = os.path.join(local, "Microsoft", "WinGet", "Packages",
                           "BrechtSanders.WinLibs*", "mingw64", "bin", "gcc.exe")
    hits = sorted(glob.glob(pattern))
    return hits[-1] if hits else None


def find_idf():
    if os.environ.get("IDF_PATH"):
        return os.environ["IDF_PATH"]
    for manifest in (r"C:\Espressif\tools\eim_idf.json",
                     os.path.expanduser("~/.espressif/eim_idf.json")):
        try:
            with open(manifest, encoding="utf-8") as fh:
                data = json.load(fh)
            installs = data.get("idfInstalled") or []
            chosen = next((i for i in installs if i.get("id") == data.get("idfSelectedId")),
                          installs[0] if installs else None)
            if chosen and chosen.get("path"):
                return chosen["path"]
        except (OSError, ValueError):
            pass
    return None


def find_uecc():
    if os.environ.get("PURR_UECC_DIR"):
        return os.environ["PURR_UECC_DIR"]
    idf = find_idf()
    if idf:
        path = os.path.join(idf, "components", "bootloader", "subproject",
                            "components", "micro-ecc", "micro-ecc")
        if os.path.isfile(os.path.join(path, "uECC.c")):
            return path
    return None


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def main(argv):
    gcc = find_gcc()
    if not gcc:
        print("error: no gcc found. Install one, or set PURR_GCC.")
        return 2
    uecc = find_uecc()
    if not uecc:
        print("error: micro-ecc source not found. Set PURR_UECC_DIR or IDF_PATH.")
        return 2
    print(f"gcc:      {gcc}")
    print(f"micro-ecc: {uecc}")

    os.makedirs(BUILD, exist_ok=True)
    src = sorted(glob.glob(os.path.join(CORE, "src", "*.c")))
    # The filesystem layer is kernel code, but it is plain C over an abstract block device,
    # so it is tested here against a RAM disk. LittleFS itself is third-party.
    src.append(os.path.join(KERNEL, "src", "purr_fs.c"))
    # The driver registry (F-02, kernel/SPEC.md sections 2/5/6) is plain C with no ESP-IDF
    # dependency -- matching/pin-ownership logic tested here with fake drivers and a fake
    # board, the real drivers' own ESP-IDF-coupled internals untouched by any of it.
    src.append(os.path.join(KERNEL, "src", "purr_driver.c"))
    common = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
              "-I", os.path.join(CORE, "include"), "-I", os.path.join(KERNEL, "include"),
              "-I", LFS, "-I", HERE, "-I", uecc]

    # Third-party code is compiled once, without our warning flags.
    uecc_obj = os.path.join(BUILD, "uECC.o")
    res = run([gcc, "-std=c11", "-O1", "-w", "-I", uecc, "-c",
               os.path.join(uecc, "uECC.c"), "-o", uecc_obj])
    if res.returncode != 0:
        print(res.stdout + res.stderr)
        return 2

    lfs_objs = []
    for name in ("lfs.c", "lfs_util.c"):
        obj = os.path.join(BUILD, name[:-2] + ".o")
        res = run([gcc, "-std=c11", "-O1", "-w", "-DLFS_NO_DEBUG", "-DLFS_NO_WARN",
                   "-DLFS_NO_ERROR", "-I", LFS, "-c", os.path.join(LFS, name), "-o", obj])
        if res.returncode != 0:
            print(res.stdout + res.stderr)
            return 2
        lfs_objs.append(obj)

    tests = sorted(glob.glob(os.path.join(HERE, "test_*.c")))
    if argv:
        tests = [t for t in tests if any(a in os.path.basename(t) for a in argv)]
    failed = 0
    for test in tests:
        name = os.path.splitext(os.path.basename(test))[0]
        exe = os.path.join(BUILD, name + (".exe" if os.name == "nt" else ""))
        cmd = [gcc] + common + [test, os.path.join(HERE, "crypto_uecc.c")] + src + \
              [uecc_obj] + lfs_objs + ["-o", exe]
        if os.name == "nt":
            cmd.append("-ladvapi32")
        res = run(cmd)
        if res.returncode != 0:
            print(f"{name}: BUILD FAILED")
            print(res.stdout + res.stderr)
            failed += 1
            continue
        res = run([exe])
        print(res.stdout, end="")
        if res.returncode != 0:
            print(res.stderr, end="")
            failed += 1
    print("\nALL PASSED" if not failed else f"\n{failed} test file(s) FAILED")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
