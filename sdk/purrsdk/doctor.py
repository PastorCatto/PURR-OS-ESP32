"""Is the kit whole? Each check says what it found and, when something is wrong, what to do."""

import glob
import hashlib
import json
import os
import shutil
import sys

from . import __version__, formats, keybag, runner, workspace


def _c(status, name, text):
    return {"status": status, "name": name, "text": text}


def find_toolchain():
    found = shutil.which("xtensa-esp32s3-elf-gcc")
    if found:
        return found
    for root in (os.environ.get("IDF_TOOLS_PATH", ""), os.path.expanduser("~/.espressif")):
        hits = sorted(glob.glob(os.path.join(root, "tools", "xtensa-esp-elf", "*", "xtensa-esp-elf", "bin", "xtensa-esp32s3-elf-gcc"))) if root else []
        if hits:
            return hits[-1]
    kit = workspace.kit_path("toolchain", "bin", "xtensa-esp32s3-elf-gcc")
    return kit if os.path.isfile(kit) else None


def run():
    checks = []
    checks.append(_c("ok", "Kit", "PURR Developer Kit %s, Python %s." % (__version__, sys.version.split()[0])))
    if sys.version_info < (3, 11):
        checks.append(_c("warn", "Python", "This Python is older than 3.11. Most tools work, but the app builder needs 3.11; the kit's own Python is 3.12."))
    try:
        formats.purrstrap_dir()
        checks.append(_c("ok", "Build tool", "purrstrap is in %s." % formats.purrstrap_dir()))
    except RuntimeError as e:
        checks.append(_c("bad", "Build tool", str(e)))
    try:
        import cryptography  # noqa: F401
        checks.append(_c("ok", "Signing", "The cryptography package is here, so keys can be made and images signed."))
    except ImportError:
        checks.append(_c("warn", "Signing", "The cryptography package is missing: you can look at images, but not make keys or sign. The kit's Python includes it."))
    tc = find_toolchain()
    checks.append(_c("ok", "Compiler", "Found %s." % tc) if tc else _c("bad", "Compiler", "No xtensa-esp32s3-elf-gcc: drivers, apps and the boot screens cannot be built."))
    keys = keybag.load_dir(keybag.default_dir())
    checks.append(_c("ok", "Key bag", "The device's built-in keys: " + ", ".join("%s (id %d, %s)" % (k["role"], k["id"], k["fingerprint"]) for k in keys)) if keys
                  else _c("warn", "Key bag", "No built-in public keys found, so signatures cannot be checked."))
    for rel, label in (("PurrOS/components/kernel/include/purr_driver.h", "driver headers"), ("PurrOS/components/coreos/include/purr_abi.h", "image headers"),
                       ("AppSDK/templates/unprivileged", "app template")):
        ok = os.path.exists(workspace.kit_path(*rel.split("/")))
        checks.append(_c("ok" if ok else "bad", label.capitalize(), "Present." if ok else "Missing from the kit: %s." % rel))
    manifest = workspace.kit_path("SDK-MANIFEST.json")
    if os.path.isfile(manifest):
        with open(manifest) as fh:
            m = json.load(fh)
        bad = []
        for rel, digest in m.get("files", {}).items():
            p = workspace.kit_path(*rel.split("/"))
            if not os.path.isfile(p):
                bad.append(rel + " (missing)")
            else:
                h = hashlib.sha256()
                with open(p, "rb") as fh:
                    for chunk in iter(lambda: fh.read(1 << 20), b""):
                        h.update(chunk)
                if h.hexdigest() != digest:
                    bad.append(rel + " (changed)")
        checks.append(_c("ok", "Kit files", "All %d files match SDK-MANIFEST." % len(m.get("files", {}))) if not bad else
                      _c("warn", "Kit files", "%d file(s) differ from the manifest, for example %s. Did you edit the kit?" % (len(bad), bad[0])))
    parts = None
    try:
        from . import release
        parts = release.prebuilt()
    except Exception:
        pass
    checks.append(_c("ok", "Prebuilt parts", "Found: " + ", ".join(sorted(parts))) if parts else _c("info", "Prebuilt parts", "No prebuilt system parts in images/: releases need them (the kit download includes them)."))
    return checks
