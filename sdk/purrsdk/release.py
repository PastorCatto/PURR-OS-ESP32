"""Releases: gather the parts of a system into a folder a device can recover from.

The kit ships the prebuilt parts (images/). A release re-signs them with *your* key (so you decide who may sign what), optionally swaps in your
flavor's boot package, and lays them out with their manifests the way `purr serve` and the recovery client expect (<folder>/recovery/...).
"""

import os
import shutil

from . import formats, inspector, runner, workspace

COMPONENTS = ("kernel", "kittenos", "loader", "bootpkg", "bootloader")
NAMES = {"kernel": "kernel_{b}.cat", "kittenos": "kittenos_{b}.kitt", "loader": "loader_{b}.cat",
         "bootpkg": "bootpkg_{b}.cat", "bootloader": "bootloader_{b}.cat"}
FLAGS = {"kernel": "--kernel", "kittenos": "--kittenos", "loader": "--loader", "bootpkg": "--bootpkg", "bootloader": "--bootloader"}


def prebuilt(board="tdeck_plus"):
    """{component: path} for the parts found in the kit's images/ folder, or a release folder given by PURR_IMAGES."""
    roots = [os.environ.get("PURR_IMAGES"), workspace.kit_path("images"), workspace.kit_path("images", "JumpingJaguar1")]
    found = {}
    for root in roots:
        if not root or not os.path.isdir(root):
            continue
        for comp, pattern in NAMES.items():
            p = os.path.join(root, pattern.format(b=board))
            if comp not in found and os.path.isfile(p):
                found[comp] = p
    return found


def describe(parts):
    """[{component, path, name, version, signer}] using the inspector, for showing what a release would contain."""
    out = []
    for comp, path in parts.items():
        rep = inspector.inspect_path(path)
        facts = dict(rep.get("facts", []))
        sig = next((c for c in rep["checks"] if c["name"] == "Signature"), {"status": "info", "text": ""})
        out.append({"component": comp, "path": path, "name": facts.get("Name", ""), "version": facts.get("Version", ""),
                    "signature": sig["status"], "signature_text": sig["text"]})
    return out


def assemble(dest, parts, board="tdeck_plus", key=None, key_id=0, password=None, legal=None, on_line=None):
    """Build a hosting folder at `dest` from {component: path}. With a key, every part is re-signed with it first.
    Returns {"ok", "log", "folder"}."""
    log = []

    def say(line):
        log.append(line)
        if on_line:
            on_line(line)

    if key:
        role = os.path.basename(key).split(".")[0].split("_")[0]
        if role != "boot":
            say("Note: you are signing boot-level parts with a %s key. A device in enforce mode only takes them if your key is in its key bag; "
                "in warn mode it boots them with a warning." % role)
    unknown = [c for c in parts if c not in COMPONENTS]
    if unknown:
        return {"ok": False, "log": "I do not know the component(s): %s." % ", ".join(unknown), "folder": dest}
    work = os.path.join(dest, ".work")
    os.makedirs(work, exist_ok=True)
    args = ["dev", "recovery", "--dir", dest, "--board", board]
    for comp, path in parts.items():
        target = os.path.join(work, os.path.basename(path))
        shutil.copyfile(path, target)
        if key:
            r = runner.purrstrap(["keys", "sign", "--key", key, "--image", target, "--key-id", int(key_id)], password=password, on_line=say)
            if r["rc"] != 0:
                return {"ok": False, "log": "\n".join(log), "folder": dest}
        args += [FLAGS[comp], target]
    if legal:
        args += ["--legal", legal]
    r = runner.purrstrap(args, on_line=say)
    shutil.rmtree(work, ignore_errors=True)
    return {"ok": r["rc"] == 0, "log": "\n".join(log), "folder": dest}
