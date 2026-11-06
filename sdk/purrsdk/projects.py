"""Projects: start one from a template, build it, sign it. Five kinds:

    app      a PURR app (AppSDK): unprivileged starter
    driver   a freestanding driver for a device class (templates.py)
    board    a board.toml: checked, then written out as the kernel's board file and a device profile
    flavor   a brand.toml: checked, then built into the boot package with your names and colours
    theme    a UI theme: a theme.conf and a folder of PNGs, built into the pack.bin the device reads (ui.py)
"""

import os
import re
import shutil

from . import boards as boards_mod
from . import brand as brand_mod
from . import minitoml, runner, templates, workspace
from . import ui as ui_mod

KINDS = ("app", "driver", "board", "flavor", "theme")
APP_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


class ProjectError(Exception):
    pass


def _write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def new(ws, kind, name, **opts):
    """Make a project in the workspace. Returns {"path", "files", "next"}."""
    if kind not in KINDS:
        raise ProjectError("A project is one of: %s." % ", ".join(KINDS))
    dest = os.path.join(ws.projects, name)
    if os.path.exists(dest):
        raise ProjectError("There is already a project called %s." % name)
    files = []
    if kind == "app":
        if not APP_NAME.match(name):
            raise ProjectError("Name an app with letters, digits and underscores, not starting with a digit: it becomes a C name.")
        src = workspace.kit_path("AppSDK", "templates", opts.get("template", "unprivileged"))
        if not os.path.isdir(src):
            raise ProjectError("The app template is missing from this kit.")
        shutil.copytree(src, dest)
        for root, _d, fs in os.walk(dest):
            for f in fs:
                p = os.path.join(root, f)
                with open(p, "r", encoding="utf-8") as fh:
                    text = fh.read()
                with open(p, "w", encoding="utf-8", newline="\n") as fh:
                    fh.write(text.replace("myapp", name))
                files.append(os.path.relpath(p, dest))
        _write(os.path.join(dest, "purr-project.toml"), minitoml.dumps({"project": {"kind": "app", "name": name, "version": "0.1.0"}}))
        nxt = "Open src/ and make it yours, then Build."
    elif kind == "driver":
        cls = opts.get("class", "sensor-i2c")
        try:
            src, entry, compat, bus = templates.driver_source(name, cls, opts.get("compatible"))
        except ValueError as e:
            raise ProjectError(str(e))
        _write(os.path.join(dest, "src", name.replace("-", "_") + "_module.c"), src)
        _write(os.path.join(dest, "purr-project.toml"), minitoml.dumps(
            {"project": {"kind": "driver", "name": name, "version": "0.1.0"},
             "driver": {"class": cls, "compatible": compat, "entry": entry, "bus": bus, "source": "src/%s_module.c" % name.replace("-", "_")}}))
        files = ["purr-project.toml", "src/%s_module.c" % name.replace("-", "_")]
        nxt = "Fill in the TODOs in src/, then Build. Add a device with compatible %r to your board.toml to bind it." % compat
    elif kind == "board":
        base = workspace.sdk_path("boards", "tdeck_plus.toml")
        with open(base, "r", encoding="utf-8") as fh:
            text = fh.read()
        ident = re.sub(r"[^a-z0-9_]", "_", name.lower())
        text = text.replace('id = "tdeck_plus"', 'id = "%s"' % ident, 1).replace('name = "LilyGO T-Deck Plus"', 'name = "%s"' % name, 1)
        _write(os.path.join(dest, "board.toml"), text)
        _write(os.path.join(dest, "purr-project.toml"), minitoml.dumps({"project": {"kind": "board", "name": name, "version": "0.1.0"}}))
        files = ["board.toml", "purr-project.toml"]
        nxt = "This starts as a copy of the T-Deck Plus. Change the pins and devices to match your board, then Build."
    elif kind == "theme":
        if not ui_mod.THEME_ID_RE.match(name):
            raise ProjectError("Name a theme with 1 to 39 letters, digits, underscores or dashes: it becomes the folder /ui/themes/<name> on the device.")
        _write(os.path.join(dest, "theme.conf"), ui_mod.THEME_CONF_TEMPLATE % {"name": name})
        _write(os.path.join(dest, "README.txt"), ui_mod.THEME_README)
        os.makedirs(os.path.join(dest, "images"))
        _write(os.path.join(dest, "purr-project.toml"), minitoml.dumps({"project": {"kind": "theme", "name": name, "version": "0.1.0"}}))
        files = ["theme.conf", "README.txt", "images/", "purr-project.toml"]
        nxt = "Fill in theme.conf, put PNG pictures in images/ (the names are in UI/PACK-NAMES.md), then Build, or `purr ui pack` and `purr ui check`."
    else:
        _write(os.path.join(dest, "brand.toml"), brand_mod.TEMPLATE)
        _write(os.path.join(dest, "purr-project.toml"), minitoml.dumps({"project": {"kind": "flavor", "name": name, "version": "0.1.0"}, "flavor": {"board": opts.get("board", "tdeck_plus")}}))
        files = ["brand.toml", "purr-project.toml"]
        nxt = "Change the names and colours in brand.toml (or use the Brand tool to see them first), then Build."
    return {"path": dest, "files": files, "next": nxt}


def list_projects(ws):
    out = []
    if not os.path.isdir(ws.projects):
        return out
    for name in sorted(os.listdir(ws.projects)):
        meta = os.path.join(ws.projects, name, "purr-project.toml")
        if os.path.isfile(meta):
            try:
                d = minitoml.load(meta)
            except minitoml.TomlError:
                d = {}
            p = d.get("project", {})
            out.append({"name": name, "kind": p.get("kind", "?"), "version": p.get("version", ""), "path": os.path.join(ws.projects, name),
                        "driver": d.get("driver", {})})
    return out


def _load(path):
    meta = os.path.join(path, "purr-project.toml")
    if not os.path.isfile(meta):
        raise ProjectError("%s is not a PURR project (there is no purr-project.toml)." % path)
    return minitoml.load(meta)


def build(ws, path, board="tdeck_plus", key=None, key_id=0, password=None, on_line=None):
    """Build a project. Returns {"ok", "artifacts": [paths], "log": text, "notes": [...]}."""
    meta = _load(path)
    p = meta["project"]
    kind, name, version = p["kind"], p["name"], p.get("version", "0.1.0")
    out_dir = os.path.join(path, "build")
    os.makedirs(out_dir, exist_ok=True)
    log, artifacts, notes = [], [], []

    def say(line):
        log.append(line)
        if on_line:
            on_line(line)

    def finish(ok):
        return {"ok": ok, "artifacts": artifacts, "log": "\n".join(log), "notes": notes}

    if kind == "board":
        b = boards_mod.load(os.path.join(path, "board.toml"))
        issues = boards_mod.check(b)
        for i in issues:
            say("[%s] %s: %s" % (i["level"], i["where"], i["text"]))
        if any(i["level"] == "bad" for i in issues):
            say("Fix the problems above, then build again.")
            return finish(False)
        cfile = os.path.join(out_dir, b["board"]["id"] + ".c")
        _write(cfile, boards_mod.to_c(b))
        conf, n = boards_mod.to_profile(b)
        pfile = os.path.join(out_dir, "board.conf")
        _write(pfile, conf)
        artifacts += [cfile, pfile]
        say("Wrote the board file (%s) and a device profile with %d device(s)." % (os.path.basename(cfile), n))
        notes.append("The board file goes into PurrOS/components/kernel/boards/ for a source build. The profile can be copied to /etc/board.conf on a device.")
        return finish(True)

    if kind == "flavor":
        bt = brand_mod.merge(minitoml.load(os.path.join(path, "brand.toml")))
        for i in brand_mod.check(bt):
            say("[%s] %s: %s" % (i["level"], i["field"], i["text"]))
        try:
            header = brand_mod.header(bt)
        except ValueError as e:
            say("Fix the problems above: %s" % e)
            return finish(False)
        hfile = os.path.join(out_dir, "purr_brand.h")
        _write(hfile, header)
        artifacts.append(hfile)
        res = runner.purrstrap(["bootpkg", "build", "--board", board, "--brand", hfile, "--out", os.path.join(out_dir, "bootpkg")], on_line=say)
        if res["rc"] != 0:
            say("The boot package did not build. The lines above say why.")
            return finish(False)
        binfile = os.path.join(out_dir, "bootpkg", "bootpkg.bin")
        artifacts.append(binfile)
        if key:
            signed = os.path.join(out_dir, "bootpkg_%s.cat" % board)
            shutil.copyfile(binfile, signed)
            r = runner.purrstrap(["keys", "sign", "--key", key, "--image", signed, "--key-id", int(key_id)], password=password, on_line=say)
            if r["rc"] != 0:
                return finish(False)
            artifacts.append(signed)
        else:
            notes.append("Not signed: choose a key to sign it. An unsigned boot package only runs on a device in warn or off mode.")
        notes.append("The system names (the shell, the loader, KittenOS) change with a source build; the kit rebuilds the boot screens today.")
        return finish(True)

    if kind == "driver":
        d = meta["driver"]
        out = os.path.join(out_dir, "%s-%s.cat" % (name, board))
        res = runner.purrstrap(["modules", "build", "--board", board, "--source", os.path.join(path, d["source"]), "--entry", d["entry"],
                                "--kind", "driver", "--name", name, "--version", version, "--out", out], on_line=say)
        if res["rc"] != 0:
            say("The driver did not build. The lines above say why.")
            return finish(False)
        artifacts.append(out)
        if key:
            r = runner.purrstrap(["keys", "sign", "--key", key, "--image", out, "--key-id", int(key_id)], password=password, on_line=say)
            if r["rc"] != 0:
                return finish(False)
        else:
            notes.append("Not signed yet: choose a key to sign it before a device will take it.")
        return finish(True)

    if kind == "app":
        out = os.path.join(out_dir, "%s.cat" % name)
        res = runner.purrstrap(["apps", "build", "--project", path, "--board", board, "--out", out], on_line=say)
        if res["rc"] != 0:
            say("The app did not build. The lines above say why.")
            return finish(False)
        artifacts.append(out)
        if key:
            r = runner.purrstrap(["keys", "sign", "--key", key, "--image", out, "--key-id", int(key_id)], password=password, on_line=say)
            if r["rc"] != 0:
                return finish(False)
        else:
            notes.append("Not signed yet: choose a key to sign it. A device in developer mode runs unsigned apps from your own folder.")
        return finish(True)

    if kind == "theme":
        conf_path = os.path.join(path, "theme.conf")
        if not os.path.isfile(conf_path):
            say("There is no theme.conf in the project.")
            return finish(False)
        with open(conf_path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        items, _values = ui_mod.check_conf_text(text, os.path.getsize(conf_path))
        for item in items:
            if item["status"] in ("bad", "warn"):
                say("[%s] %s: %s" % (item["status"], item["name"], item["text"]))
        if any(item["status"] == "bad" for item in items):
            say("Fix the problems above, then build again.")
            return finish(False)
        out = os.path.join(out_dir, name)
        os.makedirs(out, exist_ok=True)
        try:
            r = ui_mod.pack_folder(os.path.join(path, "images"), os.path.join(out, "pack.bin"))
        except ValueError as e:
            say("The pack did not build: %s" % e)
            return finish(False)
        shutil.copyfile(conf_path, os.path.join(out, "theme.conf"))
        artifacts += [os.path.join(out, "theme.conf"), r["path"]]
        say("Packed %d picture(s), %d bytes." % (r["count"], r["bytes"]))
        notes.append("Copy the folder %s to /ui/themes/%s on the device (or the SD card's ui/themes folder). Check it first: purr ui check %s --shell <w10m|android>." % (out, name, out))
        return finish(True)

    raise ProjectError("Cannot build a %r project." % kind)
