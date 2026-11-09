"""A system project: one device, the drivers and apps that go on it, a look, a key, and the build that makes it all (the "build a system" screens of Studio).

It remembers a person's choices in `<workspace>/systems/<name>/system.toml` and runs the whole road in one go, reporting each step as it happens:

    1 apps      build and sign the app projects you included          (projects.build, with your key)
    2 ui        draw and pack the UI you chose                         (uimaker.compose)
    3 drivers   gather the signed drivers you switched on              (a drivers.manifest and the files, from the drivers folder)
    4 parts     check the signed system parts exist and fit            (image.plan)
    5 image     make the whole flash image                             (image.build)
    6 delivery  lay out a folder a dev server can hand to a device     (purrstrap dev recovery, dev publish)

Signing happens where it can: the apps are signed with the key you give; the system parts and the official drivers arrive signed (by the project, or by the key
of a release you made with `purr release`). The password is only ever passed to the build tool for the one command, never stored.

A step is reported to the caller as `on_step(id, state, note)` with state one of `run`, `done`, `skip`, `fail`.
"""

import hashlib
import os
import shutil
import socket
import time

from . import image, minitoml, projects, runner, source as sourcemod, uimaker, workspace

STEPS = (("apps", "Build your apps"), ("ui", "Draw your UI"), ("drivers", "Gather the drivers"), ("parts", "Check the system parts"), ("image", "Make the flash image"),
         ("delivery", "Lay out the delivery folder"))
MODULES = ("about", "appmgr", "apps", "fs", "login", "netinstall", "sysinfo", "wifi")
BOARDS = ("tdeck_plus", "tdeck", "waveshare154")


class SystemError_(Exception):
    pass


def _dir(ws):
    return os.path.join(ws.path, "systems")


def _path(ws, name):
    if not name or "/" in name or "\\" in name or name.startswith("."):
        raise SystemError_("That is not a system name.")
    return os.path.join(_dir(ws), name, "system.toml")


def list_systems(ws):
    out = []
    d = _dir(ws)
    if os.path.isdir(d):
        for n in sorted(os.listdir(d)):
            if os.path.isfile(os.path.join(d, n, "system.toml")):
                try:
                    s = load(ws, n)
                    out.append({"name": n, "board": s["board"], "drivers": len(s["drivers"]), "apps": len(s["apps"])})
                except (OSError, SystemError_):
                    pass
    return out


def load(ws, name):
    p = _path(ws, name)
    if not os.path.isfile(p):
        raise SystemError_("I do not know a system called %s." % name)
    d = minitoml.load(p)
    s = d.get("system", {})
    return {"name": name, "board": str(s.get("board", "tdeck_plus")), "parts": str(s.get("parts", "")), "key": str(s.get("key", "")), "key_id": int(s.get("key_id", 4)),
            "ui": str(s.get("ui", "")), "drivers": list(d.get("drivers", {}).get("enabled", [])), "apps": list(d.get("apps", {}).get("include", [])),
            "drivers_set": bool(d.get("drivers", {}).get("chosen", False)),
            "parts_from": "source" if s.get("parts_from") == "source" else "ready", "source_url": str(s.get("source_url", "") or sourcemod.DEFAULT_URL),
            "source_ref": str(s.get("source_ref", "") or sourcemod.DEFAULT_REF), "shell": str(s.get("shell", "android")) if str(s.get("shell", "android")) in sourcemod.SHELLS else "android"}


def save(ws, name, s):
    p = _path(ws, name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    doc = {"system": {"name": name, "board": s["board"], "parts": s.get("parts", ""), "key": s.get("key", ""), "key_id": int(s.get("key_id", 4)), "ui": s.get("ui", ""),
                      "parts_from": s.get("parts_from", "ready"), "source_url": s.get("source_url", "") or sourcemod.DEFAULT_URL, "source_ref": s.get("source_ref", "") or sourcemod.DEFAULT_REF,
                      "shell": s.get("shell", "android")},
           "drivers": {"enabled": list(s.get("drivers", [])), "chosen": bool(s.get("drivers_set", True))}, "apps": {"include": list(s.get("apps", []))}}
    tmp = p + ".part"
    with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(minitoml.dumps(doc))
    os.replace(tmp, p)


def new_system(ws, name, board):
    if board not in BOARDS:
        raise SystemError_("The board is one of %s." % ", ".join(BOARDS))
    if os.path.isfile(_path(ws, name)):
        raise SystemError_("There is already a system called %s." % name)
    save(ws, name, {"board": board, "drivers": [], "apps": [], "drivers_set": False})
    return load(ws, name)


# ---------------------------------------------------------------------------------------------------------------- drivers

def driver_folders():
    """Where a drivers.manifest may be: $PURR_DRIVERS, the kit's drivers/ folder, a checkout's dev server folder, the hub checkout next to the repository."""
    repo = os.path.dirname(workspace.sdk_path())
    cands = [os.environ.get("PURR_DRIVERS"), workspace.kit_path("drivers"), os.path.join(repo, "out", "devrepo", "recovery"), os.path.join(os.path.dirname(repo), "PURR-OS-HUB", "repos", "drivers")]
    out = []
    for c in cands:
        if c and os.path.isfile(os.path.join(c, "drivers.manifest")) and c not in out:
            out.append(c)
    return out


def parse_manifest(text):
    """The stanzas of a manifest as [{"raw": the stanza's text, key: value...}], skipping comments; the header lines (release, released) are returned as the first stanza without a component."""
    out, cur, raw = [], {}, []
    for line in text.split("\n") + [""]:
        if line.startswith("#"):
            continue
        if "=" in line:
            k, v = line.split("=", 1)
            cur[k.strip()] = v.strip()
            raw.append(line)
        elif cur:
            cur["raw"] = "\n".join(raw)
            out.append(cur)
            cur, raw = {}, []
    return out


def driver_catalog(board, folders=None):
    """The drivers this board can take, one per name: [{"name","version","scope","file","path","size","sha256","key","raw"}]. An entry for the board itself wins over `any`."""
    found = {}
    for folder in (folders if folders is not None else driver_folders()[:1]):         # the first folder that has a manifest: mixing two sets would be a surprise
        with open(os.path.join(folder, "drivers.manifest"), encoding="utf-8") as fh:
            for e in parse_manifest(fh.read()):
                if e.get("type") != "driver" or not e.get("component") or not e.get("file"):
                    continue
                b = e.get("board", "any")
                if b not in ("any", board):
                    continue
                path = os.path.join(folder, e["file"])
                if not os.path.isfile(path):
                    continue
                cur = found.get(e["component"])
                if cur and (cur["scope"] == board or b == "any"):
                    continue
                found[e["component"]] = {"name": e["component"], "version": e.get("version", ""), "scope": b, "file": e["file"], "path": path, "size": int(e.get("size", 0) or 0),
                                         "sha256": e.get("sha256", ""), "key": e.get("key", ""), "raw": e["raw"], "buses": e.get("buses", "")}
    return sorted(found.values(), key=lambda d: d["name"])


# ---------------------------------------------------------------------------------------------------------------- the whole picture for the page

def parts_folders(board):
    cands = [os.environ.get("PURR_IMAGES"), workspace.kit_path("images")]
    d = os.path.join(os.path.dirname(workspace.sdk_path()), "out", "devrepo", "recovery")
    cands.append(d)
    out = []
    for c in cands:
        if c and os.path.isdir(c) and c not in [x["path"] for x in out]:
            f = image.find_parts(c, board)
            out.append({"path": c, "found": sorted(f), "complete": len(f) == len(image.PARTS)})
    return out


def overview(ws, name):
    s = load(ws, name)
    drivers = driver_catalog(s["board"])
    chosen = set(s["drivers"]) if s["drivers_set"] else {d["name"] for d in drivers if d["scope"] != "xx"}
    apps = [p for p in projects.list_projects(ws) if p["kind"] == "app"]
    uis = [p for p in projects.list_projects(ws) if p["kind"] == "theme" and os.path.isfile(os.path.join(p["path"], "ui.toml"))]
    folders = parts_folders(s["board"])
    folder = s["parts"] or next((f["path"] for f in folders if f["complete"]), "")
    plan = image.plan(s["board"], folder) if folder else None
    src = {"git": sourcemod.have_git(), "compiler": sourcemod.compiler(), "state": sourcemod.state(ws)}
    return {"source": src, "system": s, "drivers": [dict(d, on=d["name"] in chosen, raw=None, path=None) for d in drivers], "apps": [{"name": a["name"], "version": a["version"], "on": a["name"] in s["apps"]} for a in apps],
            "uis": [{"name": u["name"], "on": u["name"] == s["ui"]} for u in uis], "folders": folders, "folder": folder,
            "plan": ({"ok": plan["ok"], "problems": plan["problems"], "parts": plan["parts"]} if plan else None)}


# ---------------------------------------------------------------------------------------------------------------- the build

def lan_address():
    """The address of this computer on the local network, for the device to reach; 127.0.0.1 if there is none."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def _find_modules(folder, board):
    out = []
    for m in MODULES:
        for sep in ("-", "_"):
            p = os.path.join(folder, "%s%s%s.cat" % (m, sep, board))
            if os.path.isfile(p):
                out.append(p)
                break
    return out if len(out) == len(MODULES) else []


def build(ws, name, password=None, on_step=None, on_line=print):
    """Run the whole road for a system. Returns {"ok", "image", "delivery", "steps": {id: {"state", "note"}}}. A failed step stops the road, and its note says why."""
    s = load(ws, name)
    steps = {}

    def step(i, state, note=""):
        steps[i] = {"state": state, "note": note}
        if on_step:
            on_step(i, state, note)

    def fail(i, note):
        step(i, "fail", note)
        on_line(note)
        return {"ok": False, "steps": steps, "image": "", "delivery": ""}

    out = os.path.join(ws.builds, "system-" + name)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    built_apps = []
    key = s["key"] or None
    if key and not os.path.isabs(key):
        key = os.path.join(ws.keys, key)

    # 0 source: pull the newest source and compile the firmware, when the parts are to be made rather than taken ready-made
    built_parts = None
    if s["parts_from"] == "source":
        if not key:
            return fail("source", "Choose a signing key on the device step: the parts you compile have to be signed.")
        step("source", "run", "%s (%s)" % (s["source_url"], s["source_ref"]))
        try:
            pr = sourcemod.pull(ws, s["source_url"], s["source_ref"], on_line=on_line)
        except sourcemod.SourceError as e:
            return fail("source", str(e))
        step("source", "done", "%s%s" % (pr["commit"][:7], "" if pr["changed"] else ", already the newest"))
        step("compiler", "run", "")
        c = sourcemod.compiler()
        if not c["found"]:
            return fail("compiler", "ESP-IDF %s, the compiler for the firmware, is not installed. Use \"install the compiler\" on the device step (about 2.5 GB, once)." % sourcemod.IDF_VERSION)
        step("compiler", "done", "ESP-IDF %s" % (c["version"] or "found"))
        res = sourcemod.compile_and_package(ws, s["board"], s["shell"], key, s["key_id"], password, out=os.path.join(out, "parts"), on_step=step, on_line=on_line)
        if not res["ok"]:
            return {"ok": False, "steps": steps, "image": "", "delivery": ""}
        built_parts = res["out"]

    # 1 apps
    if s["apps"]:
        step("apps", "run", "%d app(s)" % len(s["apps"]))
        if not key:
            return fail("apps", "Choose a signing key on the device step: your apps have to be signed before a device will run them.")
        for a in s["apps"]:
            root = os.path.join(ws.projects, a)
            try:
                r = projects.build(ws, root, board=s["board"], key=key, key_id=s["key_id"], password=password or None, on_line=on_line)
            except projects.ProjectError as e:
                return fail("apps", "%s: %s" % (a, e))
            if not r["ok"] or not r["artifacts"]:
                return fail("apps", "%s did not build; the lines above say why." % a)
            built_apps.append(r["artifacts"][-1])
        step("apps", "done", "%d built and signed" % len(built_apps))
    else:
        step("apps", "skip", "no apps chosen")

    # 2 ui
    if s["ui"]:
        step("ui", "run", s["ui"])
        r = uimaker.compose(os.path.join(ws.projects, s["ui"]), on_line=on_line)
        if not r["ok"]:
            return fail("ui", "The UI %s did not make: %s" % (s["ui"], " ".join(r.get("problems", []))))
        ui_out = r["out"]
        step("ui", "done", "%d pictures" % r["count"])
    else:
        ui_out = None
        step("ui", "skip", "no UI chosen")

    # 3 drivers
    cat = driver_catalog(s["board"])
    chosen = [d for d in cat if (d["name"] in s["drivers"] if s["drivers_set"] else True)]
    step("drivers", "run", "%d driver(s)" % len(chosen))
    rec = os.path.join(out, "recovery")
    os.makedirs(rec, exist_ok=True)
    stanzas = ["release=system-%s-%s" % (name, time.strftime("%Y%m%d")), "released=%s" % time.strftime("%Y-%m-%d"), ""]
    for d in chosen:
        data = open(d["path"], "rb").read()
        if d["sha256"] and hashlib.sha256(data).hexdigest() != d["sha256"]:
            return fail("drivers", "The driver %s does not match its manifest (a damaged or edited file)." % d["name"])
        shutil.copyfile(d["path"], os.path.join(rec, d["file"]))
        stanzas += [d["raw"], ""]
    if chosen:
        with open(os.path.join(rec, "drivers.manifest"), "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(stanzas))
    step("drivers", "done" if chosen else "skip", "%d signed driver(s)" % len(chosen) if chosen else "none switched on")

    # 4 parts
    folder = built_parts or s["parts"] or next((f["path"] for f in parts_folders(s["board"]) if f["complete"]), "")
    step("parts", "run", folder or "looking")
    if not folder:
        return fail("parts", "I cannot find the five signed system parts (kernel, recovery, loader, boot package, bootloader). Put them in the kit's images folder, or choose the folder on the device step.")
    plan = image.plan(s["board"], folder)
    if not plan["ok"]:
        return fail("parts", " ".join(plan["problems"]))
    step("parts", "done", "versions " + ", ".join(sorted({r["version"] for r in plan["parts"] if r["version"]})))

    # 5 image
    step("image", "run", s["board"])
    img = os.path.join(out, "%s-%s.img" % (name, s["board"]))
    try:
        res = image.build(s["board"], folder, img, on_line=on_line, work=os.path.join(out, ".work"))
    except image.ImageError as e:
        return fail("image", str(e))
    step("image", "done", "%d MB" % (res["bytes"] >> 20))

    # 6 delivery
    step("delivery", "run", "")
    parts = image.find_parts(folder, s["board"])
    args = ["dev", "recovery", "--dir", out, "--board", s["board"], "--kernel", parts["kernel"], "--kittenos", parts["kittenos"], "--loader", parts["loader"], "--bootpkg", parts["bootpkg"],
            "--bootloader", parts["bootloader"]]
    mods = _find_modules(folder, s["board"])
    if not mods and built_parts:
        ready = next((f["path"] for f in parts_folders(s["board"]) if f["complete"]), "")
        mods = _find_modules(ready, s["board"]) if ready else []        # the system modules are not part of the firmware compile: they come ready-made
    if mods:
        args += ["--modules", ",".join(mods)]
    r = runner.purrstrap(args, on_line=on_line)
    if r["rc"] != 0:
        return fail("delivery", "Could not lay out the delivery folder: " + (r["output"].strip().splitlines() or ["no reason given"])[-1])
    for cat_file in built_apps:
        r = runner.purrstrap(["dev", "publish", "--file", cat_file, "--dir", out], on_line=on_line)
        if r["rc"] != 0:
            return fail("delivery", "Could not publish %s: %s" % (os.path.basename(cat_file), (r["output"].strip().splitlines() or ["no reason given"])[-1]))
    if ui_out:
        shutil.copytree(ui_out, os.path.join(out, "ui", "themes", os.path.basename(ui_out)), dirs_exist_ok=True)
    step("delivery", "done", out)
    return {"ok": True, "image": img, "delivery": out, "bytes": res["bytes"], "steps": steps, "versions": res.get("versions", {}), "fit": res.get("fit", []), "apps": built_apps}
