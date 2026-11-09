"""The keys a device trusts, for checking signatures: files named <role>_<id>.pub.bin
(64 raw bytes, X || Y), as `purrstrap keys export-public` writes them."""

import hashlib
import os
import re

from . import formats

ROLE_NAMES = {1: "boot", 2: "system", 3: "owner", 4: "developer", 5: "vendor"}
ROLE_IDS = {v: k for k, v in ROLE_NAMES.items()}


def fingerprint(pub64):
    return hashlib.sha256(pub64).digest()[:8].hex()


def load_dir(path):
    keys = []
    if not path or not os.path.isdir(path):
        return keys
    for name in sorted(os.listdir(path)):
        m = re.match(r"^(boot|system|owner|developer|vendor)_(\d+)\.pub\.bin$", name)
        if not m:
            continue
        with open(os.path.join(path, name), "rb") as fh:
            raw = fh.read()
        if len(raw) == 64:
            keys.append({"id": int(m.group(2)), "role": m.group(1), "pub": raw,
                         "fingerprint": fingerprint(raw), "source": name})
    return keys


def default_dir():
    """The device's built-in key bag: the public keys in the coreos component (the kit carries the same folder)."""
    here = os.path.dirname(os.path.abspath(__file__))
    for rel in (("..", "..", "PurrOS", "components", "coreos", "keys"),):
        p = os.path.abspath(os.path.join(here, *rel))
        if os.path.isdir(p) and load_dir(p):
            return p
    return None


def load_all(extra_dirs=()):
    keys = load_dir(default_dir())
    for d in extra_dirs:
        keys += load_dir(d)
    return keys


# Which roles may sign which kind of file (Keys/SPEC.md section 2).
def allowed_roles(image_type, subtype):
    img = formats.image()
    if image_type in (img.IMG_OS, img.IMG_RECOVERY):
        return ["boot"]
    if image_type == img.IMG_MODULE:
        if subtype in (img.MOD_KERNEL, img.MOD_COREOS, img.MOD_BOOTPKG, img.MOD_LOADER, img.MOD_BOOTLOADER):
            return ["boot"]
        if getattr(img, "MOD_UIASSETS", None) == subtype:
            return ["boot", "system"]
        if subtype == img.MOD_DRIVER:
            return ["system", "developer", "vendor"]
        return ["system"]
    if image_type == img.IMG_APP:
        return ["system", "owner", "developer", "vendor"]
    return []
