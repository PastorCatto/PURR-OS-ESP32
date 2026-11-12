"""The `purr` command. Friendly on purpose: it says what it found and what to do next."""

import argparse
import getpass
import json
import os
import shutil
import subprocess
import sys
import time

from . import __version__, boards, brand, configs, doctor, emulator, flash, font, formats, image, inspector, keys, minitoml, projects, release, repo, runner, source as sourcemod, system as systemmod, ui, uimaker, workspace

TTY = sys.stdout.isatty()
COL = {"ok": "92", "warn": "93", "bad": "91", "info": "96", "dim": "90"}
MARK = {"ok": "✓", "warn": "!", "bad": "✗", "info": "i"}


def paint(text, kind):
    return "\033[%sm%s\033[0m" % (COL[kind], text) if TTY else text


def line(kind, name, text):
    print("  %s %s%s" % (paint(MARK[kind], kind), (name + ": ") if name else "", text))


def password(args, prompt="Key password: "):
    pw = os.environ.get("PURRSTRAP_KEY_PASSWORD")
    if pw is not None:
        return pw
    if not sys.stdin.isatty():
        raise SystemExit("I need the key's password: set PURRSTRAP_KEY_PASSWORD, or run me in a terminal.")
    return getpass.getpass(prompt)


def ws_of(args):
    return workspace.Workspace(getattr(args, "workspace", None)).ensure()


def resolve_key(ws, key):
    if not key:
        return None
    if os.path.isabs(key) or os.path.exists(key):
        return os.path.abspath(key)
    return os.path.join(ws.keys, key if key.endswith(".key") else key + ".key")


# ---------------------------------------------------------------------------------------------------------------- commands
def cmd_studio(a):
    from importlib import import_module
    sys.path.insert(0, workspace.sdk_path("studio"))
    server = import_module("server")
    return server.serve(a.workspace, a.port, not a.no_open)


def cmd_doctor(a):
    worst = 0
    print("Let's make sure the kit is whole.\n")
    for c in doctor.run():
        line(c["status"], c["name"], c["text"])
        worst = max(worst, {"ok": 0, "info": 0, "warn": 0, "bad": 1}[c["status"]])
    print("\n" + ("Everything is in order." if not worst else "Something needs fixing: see the lines marked ✗."))
    return worst


def show_report(rep, depth=0):
    pad = "  " * depth
    print("%s%s" % (pad, paint(rep["title"], "info") if depth == 0 else rep["title"]))
    print("%s  %s" % (pad, rep["summary"]))
    for c in rep["checks"]:
        print(pad + "  " + paint(MARK[c["status"]], c["status"]) + " " + c["name"] + ": " + c["text"])
    if depth == 0 and rep["facts"]:
        for k, v in rep["facts"]:
            print("%s    %-20s %s" % (pad, k, v))
    for p in rep["parts"]:
        print()
        show_report(p, depth + 1)


def worst_of(rep):
    bad = any(c["status"] == "bad" for c in rep["checks"])
    return bad or any(worst_of(p) for p in rep["parts"])


def cmd_inspect(a):
    ws = ws_of(a)
    key_dirs = [ws.keys] + list(a.keys or [])
    rep = inspector.inspect_path(a.path, key_dirs)
    if a.json:
        print(json.dumps(rep, indent=1, default=lambda b: b.hex() if isinstance(b, bytes) else str(b)))
    else:
        show_report(rep)
        if rep["next"] and not worst_of(rep):
            print("\n  Next: " + ";  ".join(n["label"].lower() + " (" + n["why"].lower().rstrip(".") + ")" for n in rep["next"]) + ".")
    return 1 if worst_of(rep) else 0


def cmd_new(a):
    ws = ws_of(a)
    try:
        r = projects.new(ws, a.kind, a.name, **{"class": a.driver_class, "template": a.template})
    except projects.ProjectError as e:
        print(paint("Not this time: ", "bad") + str(e))
        return 1
    print(paint("Made it!", "ok") + " " + r["path"])
    for f in r["files"][:12]:
        print("   " + f)
    print("\n" + r["next"])
    return 0


def cmd_build(a):
    ws = ws_of(a)
    path = a.project if os.path.isdir(a.project) else os.path.join(ws.projects, a.project)
    key = resolve_key(ws, a.key)
    pw = password(a) if key else None
    try:
        r = projects.build(ws, path, board=a.board, key=key, key_id=a.key_id, password=pw, on_line=lambda l: print("  " + l))
    except projects.ProjectError as e:
        print(paint("Not this time: ", "bad") + str(e))
        return 1
    print()
    if r["ok"]:
        print(paint("Built.", "ok"))
        for p in r["artifacts"]:
            print("   " + p)
        for n in r["notes"]:
            print(paint("   note: ", "dim") + n)
        return 0
    print(paint("That did not build. The lines above say why.", "bad"))
    return 1


def cmd_keys(a):
    ws = ws_of(a)
    if a.action == "list":
        for k in keys.list_keys(ws.keys):
            print("%-10s %-22s %s" % (k["kind"], k["name"], k.get("fingerprint", "")))
        print(paint("\nKeys every device knows:", "dim"))
        from . import keybag
        for k in keybag.load_dir(keybag.default_dir()):
            print("%-10s id %-3d %-10s %s" % ("built-in", k["id"], k["role"], k["fingerprint"]))
        return 0
    pw = password(a, "Choose a password for the new key: ")
    res = keys.generate(ws.keys, a.role, a.key_id, pw, a.force)
    print(res["output"])
    return res["rc"]


def cmd_sign(a):
    ws = ws_of(a)
    key = resolve_key(ws, a.key)
    pw = password(a)
    res = keys.sign(a.file, key, a.key_id, pw, a.out or "")
    print(res["output"])
    return res["rc"]


def cmd_brand(a):
    b = brand.load(a.file)
    issues = brand.check(b)
    for i in issues:
        line("bad" if i["level"] == "bad" else "warn", i["field"], i["text"])
    if any(i["level"] == "bad" for i in issues):
        return 1
    if a.action == "check":
        print(paint("Looks good.", "ok"))
        return 0
    out = a.out or os.path.join(os.path.dirname(os.path.abspath(a.file)), "purr_brand.h")
    with open(out, "w") as fh:
        fh.write(brand.header(b))
    print(paint("Wrote ", "ok") + out)
    print("Build the boot screens with it:  purrstrap bootpkg build --brand %s   (or make a flavor project and `purr build` it)" % out)
    return 0


def cmd_board(a):
    b = boards.load(a.file)
    issues = boards.check(b)
    for i in issues:
        line({"bad": "bad", "warn": "warn", "info": "info"}[i["level"]], i["where"], i["text"])
    if any(i["level"] == "bad" for i in issues):
        return 1
    cov = boards.coverage(b)
    if a.action == "check":
        for c in cov:
            line({"works": "ok", "built": "ok", "written": "warn", "template": "info", "missing": "bad"}[c["status"]], c["label"], c["status"] + (" (" + c["driver"] + ")" if c["driver"] else ""))
        return 0
    out = a.out or os.path.dirname(os.path.abspath(a.file))
    os.makedirs(out, exist_ok=True)
    cfile = os.path.join(out, b["board"]["id"] + ".c")
    with open(cfile, "w") as fh:
        fh.write(boards.to_c(b))
    conf, n = boards.to_profile(b)
    with open(os.path.join(out, "board.conf"), "w") as fh:
        fh.write(conf)
    print(paint("Wrote ", "ok") + cfile + " and board.conf (%d device(s))." % n)
    return 0


def cmd_release(a):
    ws = ws_of(a)
    parts = release.prebuilt(a.board)
    if not parts:
        print("I cannot find the prebuilt parts. Put them in the kit's images/ folder, or set PURR_IMAGES.")
        return 1
    key = resolve_key(ws, a.key)
    pw = password(a) if key else None
    out = a.out or os.path.join(ws.builds, "release-" + a.board)
    r = release.assemble(out, parts, a.board, key, a.key_id, pw, on_line=lambda l: print("  " + l))
    print(paint("Your release is ready: ", "ok") + out if r["ok"] else paint("That did not work. The lines above say why.", "bad"))
    return 0 if r["ok"] else 1


def cmd_image(a):
    """Make one whole flash image for a device from the signed parts (a release folder, or the kit's images/)."""
    ws = ws_of(a)
    folder = a.parts or os.environ.get("PURR_IMAGES") or workspace.kit_path("images")
    out = a.out or os.path.join(ws.builds, "%s-%s.img" % (a.board, time.strftime("%Y%m%d-%H%M%S")))
    try:
        if a.plan:
            p = image.plan(a.board, folder)
            for r in p["parts"]:
                line("ok" if r["found"] else "bad", r["part"], ("%s · %s bytes of %s" % (r["version"], r["size"], r["room"])) if r["found"] else "missing")
            for t in p["problems"]:
                print(paint("  " + t, "bad"))
            return 0 if p["ok"] else 1
        r = image.build(a.board, folder, out, trust_block=a.trust_block, on_line=print)
    except image.ImageError as e:
        print(paint("Not this time: ", "bad") + str(e))
        return 1
    for n in r["notes"]:
        line("info", "", n)
    print(paint("Your flash image is ready: ", "ok") + r["out"])
    print("  Flash it with: purr flash %s --port <your port>" % r["out"])
    return 0


def _system_overview(ws, name, s):
    line("info", name, "%s, parts %s%s" % (s["board"], "compiled from source" if s["parts_from"] == "source" else "ready-made",
                                              (" (%s, %s, shell %s, version %s)" % (s["source_url"], s["source_ref"], s["shell"], s["version"])) if s["parts_from"] == "source" else ""))
    if s["parts_from"] == "source":
        line("info", "keys", "boot %s (id %d), developer %s (id %d), PURR's keys %s" % (s["boot_key"] or "not chosen", s["boot_key_id"], s["key"] or "not chosen", s["key_id"], "kept" if s["keep_ours"] else "taken out"))
        for t in sourcemod.trust_problems(s["keep_ours"], s["boot_key"], s["boot_key_id"], s["key"], s["key_id"]):
            line("bad", "", t)
    line("info", "apps", ", ".join(s["apps"]) or "none")
    line("info", "drivers", ", ".join(s["drivers"]) if s["drivers_set"] else "all that fit the board")


def cmd_system(a):
    """A system is a device, its parts, drivers, apps and keys, saved by name: make one, change it, build it from the signed parts or from source, test the image."""
    ws = ws_of(a)
    try:
        if a.action == "list":
            for x in systemmod.list_systems(ws):
                line("info", x["name"], "%s, %d drivers, %d apps" % (x["board"], x["drivers"], x["apps"]))
            return 0
        if a.action == "new":
            systemmod.new_system(ws, a.name, a.board)
            line("ok", a.name, "made. Change it with `purr system set %s ...`, build it with `purr system build %s`." % (a.name, a.name))
            return 0
        s = systemmod.load(ws, a.name)
        if a.action == "show":
            _system_overview(ws, a.name, s)
            return 0
        if a.action == "set":
            for k, attr in (("board", "board"), ("parts_from", "parts_from"), ("source_url", "source_url"), ("source_ref", "source_ref"), ("shell", "shell"), ("version", "version"),
                            ("boot_key", "boot_key"), ("key", "key"), ("ui", "ui"), ("parts", "parts")):
                if getattr(a, attr, None) is not None:
                    s[k] = getattr(a, attr)
            for k in ("boot_key_id", "key_id"):
                if getattr(a, k, None) is not None:
                    s[k] = getattr(a, k)
            if a.keep_ours is not None:
                s["keep_ours"] = a.keep_ours == "on"
            if a.drivers is not None:
                s["drivers"], s["drivers_set"] = [x for x in a.drivers.split(",") if x], True
            if a.apps is not None:
                s["apps"] = [x for x in a.apps.split(",") if x]
            if s["board"] not in systemmod.BOARDS:
                raise systemmod.SystemError_("The board is one of %s." % ", ".join(systemmod.BOARDS))
            if s["shell"] not in sourcemod.SHELLS:
                raise systemmod.SystemError_("The shell is one of %s." % ", ".join(sourcemod.SHELLS))
            systemmod.save(ws, a.name, s)
            _system_overview(ws, a.name, systemmod.load(ws, a.name))
            return 0
        if a.action == "build":
            from_source = s["parts_from"] == "source"
            dev_pw = password(a, "Developer key password: ") if (from_source or s["apps"]) else None
            boot_pw = None
            if from_source:
                boot_pw = os.environ.get("PURRSTRAP_BOOT_KEY_PASSWORD")
                if boot_pw is None:
                    boot_pw = dev_pw if (not sys.stdin.isatty() or s["boot_key"] == s["key"]) else getpass.getpass("Boot key password (Enter for the same): ") or dev_pw
            marks = {"run": "info", "done": "ok", "skip": "dim", "fail": "bad"}

            def on_step(i, st, note):
                if st != "run":
                    print("  %s %s%s" % (paint(MARK.get(marks[st], "i") if st != "skip" else "-", marks[st]), i, (": " + note) if note else ""))
            r = systemmod.build(ws, a.name, dev_pw, on_step=on_step, on_line=lambda l: print("    " + l) if a.verbose else None, boot_password=boot_pw)
            if not r["ok"]:
                bad = [(k, v["note"]) for k, v in r["steps"].items() if v["state"] == "fail"]
                print(paint("Not this time: ", "bad") + (bad[0][0] + ": " + bad[0][1] if bad else "the build stopped; run again with --verbose."))
                return 1
            print(paint("Built: ", "ok") + r["image"])
            print("  Delivery folder: " + r["delivery"])
            return 0
        if a.action == "test":
            img = a.image
            if not img:
                found = [os.path.join(ws.builds, "system-" + a.name, f) for f in os.listdir(os.path.join(ws.builds, "system-" + a.name))] if os.path.isdir(os.path.join(ws.builds, "system-" + a.name)) else []
                found = [f for f in found if f.endswith(".img")]
                if not found:
                    raise systemmod.SystemError_("There is no built image for %s yet: run `purr system build %s` first." % (a.name, a.name))
                img = found[0]
            if not emulator.available(s["board"]):
                raise systemmod.SystemError_("The emulator is not available for %s here (it needs the kit's emulator, or PURR_EMULATOR_DIR)." % s["board"])
            t = emulator.test_image(img, s["board"], seconds=a.seconds, workdir=os.path.join(ws.builds, "system-" + a.name, "shots"), on_line=lambda l: None)
            for c in t["checks"]:
                line({"ok": "ok", "warn": "warn", "fail": "bad"}.get(c["status"], "info"), c["id"], c["text"])
            return 0 if t["ok"] else 1
    except (systemmod.SystemError_, sourcemod.SourceError) as e:
        print(paint("Not this time: ", "bad") + str(e))
        return 1
    return 1


def cmd_source(a):
    """The PURR OS source a system is compiled from: is something newer, pull it, is the compiler there, install it."""
    ws = ws_of(a)
    try:
        if a.action == "compiler":
            c = sourcemod.compiler()
            line("ok" if c["found"] and c["right"] else "bad", "ESP-IDF", ("%s at %s" % (c["version"] or "found", c["path"])) if c["found"] else "not installed (`purr source install-compiler` installs 5.3.5, about 2.5 GB)")
            return 0 if c["found"] and c["right"] else 1
        if a.action == "install-compiler":
            c = sourcemod.install_compiler(on_line=print)
            line("ok" if c["found"] else "bad", "ESP-IDF", c["version"] or "not found after the install")
            return 0 if c["found"] else 1
        url, ref = a.url or sourcemod.DEFAULT_URL, a.ref or sourcemod.DEFAULT_REF
        if a.action == "check":
            c = sourcemod.check(ws, url, ref)
            if c["error"]:
                line("bad", "", c["error"])
                return 1
            line("info", "pulled", c["commit"][:10] if c["pulled"] else "nothing yet")
            line("ok" if not c["newer"] else "warn", "latest", c["latest"][:10] + ("  (newer than what you have)" if c["newer"] else "  (what you have)"))
            return 0
        if a.action == "pull":
            r = sourcemod.pull(ws, url, ref, on_line=print)
            line("ok", "pulled", r["commit"][:10] + ("" if r["changed"] else " (already the newest)"))
            return 0
    except sourcemod.SourceError as e:
        print(paint("Not this time: ", "bad") + str(e))
        return 1
    return 1


def cmd_serve(a):
    ws = ws_of(a)
    args = ["dev", "serve", "--dir", a.dir or ws.repo, "--port", str(a.port)]
    if a.name:
        args += ["--name", a.name]
    return runner.purrstrap(args, on_line=print)["rc"]


def cmd_publish(a):
    ws = ws_of(a)
    return runner.purrstrap(["dev", "publish", "--file", a.file, "--dir", a.dir or ws.repo], on_line=print)["rc"]


def cmd_flash(a):
    port = a.port
    if not port:
        ports = flash.list_ports()
        if len(ports) != 1:
            print("Tell me which port: --port PORT. I can see: " + (", ".join(ports) or "no boards (is it in download mode?)"))
            return 1
        port = ports[0]
    if not a.yes:
        print("This writes %s to the board on %s at %s, replacing what is there." % (os.path.basename(a.image), port, a.offset))
        if not sys.stdin.isatty() or input("Type yes to go on: ").strip().lower() != "yes":
            print("Not flashing.")
            return 1
    r = flash.write(port, a.image, a.offset, on_line=print)
    print(paint("Flashed.", "ok") if r["ok"] else paint("That did not work. The lines above say why.", "bad"))
    return 0 if r["ok"] else 1


def cmd_repo(a):
    try:
        if a.action == "init":
            pw = password(a, "Choose a password for the repository's key: ")
            r = repo.init(a.folder, a.name or os.path.basename(os.path.abspath(a.folder)), a.maintainer or "", a.url or "", pw, a.description or "")
            print(paint("Made the repository.", "ok") + " id " + repo.pretty_fingerprint(r["id"]))
            print("Add packages with `purr repo add %s FILE`, then `purr repo build %s`." % (a.folder, a.folder))
            print("Keep keys/repo.key private (it is already in .gitignore).")
        elif a.action == "add":
            m = repo.add(a.folder, a.file, a.title or "", a.desc or "", a.category or "", a.source or "", (a.perms or "").split(",") if a.perms else [], a.type, a.target, a.dest or "",
                         a.min_os or "", "", None, a.version, a.needs_sd)
            print(paint("Added ", "ok") + "%s %s (%s)" % (m["name"], m["version"], m["type"]))
        elif a.action == "build":
            pw = password(a, "Repository key password: ")
            out = a.out or os.path.join(a.folder, "out")
            r = repo.build(a.folder, out, pw, a.layout)
            print(paint("Built ", "ok") + "%d item(s), repository %s, into %s" % (r["apps"], repo.pretty_fingerprint(r["id"]), out))
            for f in r["files"]:
                print("   " + f)
            print("Upload them as the assets of a release (flat), or to your web host (tree).")
        elif a.action == "release":
            pw = password(a, "Repository key password: ")
            out = a.out or os.path.join(a.folder, "out", "JumpingJaguar3")
            r = repo.build(a.folder, out, pw, "flat")
            print(paint("Built the release folder ", "ok") + out + " (%d item(s)). Upload every file in it as an asset of the JumpingJaguar3 release." % r["apps"])
        elif a.action == "endorse":
            pw = password(a, "System key password: ")
            e = repo.endorse(a.folder, resolve_key(ws_of(a), a.key), a.key_id, pw)
            print(paint("Endorsed ", "ok") + "repository %s with key %d. Rebuild to include it." % (repo.pretty_fingerprint(e["repo_id"]), e["key_id"]))
        elif a.action == "move":
            if not (a.from_url and a.recovery and a.modules and a.seq and a.key):
                print(paint("Not this time: ", "bad") + "`move` needs --from-url, --recovery, --modules, --seq and --key (see sdk/docs/11-repositories.md).")
                return 1
            pw = password(a, "System key password: ")
            repo.move(a.folder, a.from_url, a.recovery, a.modules, resolve_key(ws_of(a), a.key), a.key_id, pw, a.seq, a.message or "")
            print(paint("Wrote ", "ok") + "moved.txt and moved.txt.sig into %s. Put both next to %s." % (a.folder, a.from_url))
        elif a.action == "verify":
            r = repo.verify(a.folder, [ws_of(a).keys])
            print("%s: %s, repository %s, %d item(s), %s layout" % (r["name"] or "?", paint(r["tier"], "ok" if r["ok"] else "bad"), repo.pretty_fingerprint(r["id"]) if r["id"] else "?", r["apps"], r.get("layout", "?")))
            for n in r["notes"]:
                line("info", "", n)
            for pr in r["problems"]:
                line("bad", "", pr)
            return 0 if r["ok"] else 1
    except repo.RepoError as e:
        print(paint("Not this time: ", "bad") + str(e))
        return 1
    return 0


def cmd_emulator(a):
    try:
        proc = emulator.open_window(a.image, a.board)
    except emulator.EmulatorError as e:
        print(paint("Not this time: ", "bad") + str(e))
        return 1
    print(paint("The emulator is open.", "ok") + " Close its window to finish. (It runs a copy of the image; the original is not changed.)")
    return proc.wait()


def cmd_vm_image(a):
    """Put a built image where Run and the emulator look for it (the kit's images/vm-flash.bin)."""
    src = a.image or emulator.latest_built_image(ws_of(a))
    if not src:
        print(paint("Not this time: ", "bad") + "there is no built image in your workspace yet (a system build leaves one in builds/system-<name>/). Name an image to use instead.")
        return 1
    res = emulator.adopt_image(src, force=True)
    if res["status"] != "placed":
        print(paint("Not this time: ", "bad") + res["reason"])
        return 1
    print(paint("Done.", "ok") + " %s is now the emulator's image (a copy at %s). Run and `purr emulator` use it." % (os.path.basename(src), res["path"]))
    return 0


def cmd_run(a):
    """Build and sign an app, serve it, boot the VM, and put the app on it through the Developer Portal."""
    ws = ws_of(a)
    target = a.target
    cat = None
    if os.path.isfile(target) and target.endswith(".cat"):
        cat = os.path.abspath(target)
    else:
        path = target if os.path.isdir(target) else os.path.join(ws.projects, target)
        key = resolve_key(ws, a.key)
        pw = password(a) if key else None
        try:
            r = projects.build(ws, path, board=a.board, key=key, key_id=a.key_id, password=pw, on_line=lambda l: print("  " + l))
        except projects.ProjectError as e:
            print(paint("Not this time: ", "bad") + str(e))
            return 1
        if not r["ok"] or not r["artifacts"]:
            print(paint("The build did not work, so there is nothing to run. The lines above say why.", "bad"))
            return 1
        cat = r["artifacts"][-1]
        for n in r["notes"]:
            line("info", "", n)
    try:
        res = emulator.run_app(cat, a.image, a.emulator_board, a.shots, a.port, headless=not a.window, on_line=print)
    except emulator.EmulatorError as e:
        print(paint("Not this time: ", "bad") + str(e))
        return 1
    for p in res["shots"]:
        print("   picture: " + p)
    return 0 if res["ok"] else 1


def cmd_font(a):
    rep = font.check_file(a.file, a.max_size)
    if a.json:
        print(json.dumps(rep, indent=1))
    else:
        print("%s\n" % a.file)
        for c in rep["checks"]:
            line(c["status"], c["name"], c["text"])
        print("\n" + ("This font passes the checks." if rep["ok"] else "This font fails the checks: see the lines marked ✗."))
    return 0 if rep["ok"] else 1


def cmd_ui(a):
    if a.action in ("new", "import", "make"):
        ws = ws_of(a)
        if a.action == "make":
            path = a.target if os.path.isdir(a.target) else os.path.join(ws.projects, a.target)
            r = uimaker.compose(path, on_line=lambda l: print("  " + l))
            for t in r.get("problems", []):
                print(paint("  " + t, "bad"))
            for n in r.get("notes", []):
                line("info", "", n)
            if r["ok"]:
                print(paint("Your UI is ready: ", "ok") + r["out"])
                print("   %d picture(s): %d drawn, %d yours, %d resized. Copy the folder to /ui/themes/<name> on the device." % (r["count"], len(r["generated"]), len(r["yours"]), len(r["resized"])))
            return 0 if r["ok"] else 1
        try:
            r = uimaker.new_project(ws, a.target, a.shell or "android", a.board, squareline=a.squareline if a.action == "import" else None, on_line=lambda l: print("  " + l))
        except (projects.ProjectError, ValueError) as e:
            print(paint("Not this time: ", "bad") + str(e))
            return 1
        print(paint("Started the UI project: ", "ok") + r["path"])
        print("   Edit ui.toml, then `purr ui make %s`." % a.target)
        if r["import"]:
            for p_ in r["import"]["pictures"]:
                line("ok" if p_["slot"] else "info", p_["id"], "%dx%d -> %s" % (p_["w"], p_["h"], p_["slot"] or "no slot matched: point a slot at imported/%s.png in ui.toml" % p_["id"]))
        return 0
    if a.action == "pack":
        out = a.out or "pack.bin"
        try:
            r = ui.pack_folder(a.target, out)
        except ValueError as e:
            print(paint("Not this time: ", "bad") + str(e))
            return 1
        print(paint("Packed.", "ok") + " %s: %d picture(s), %s bytes (cap %s)" % (r["path"], r["count"], format(r["bytes"], ","), format(ui.MAX_PACK_BYTES, ",")))
        print("   " + ", ".join(r["names"][:12]) + (", ..." if len(r["names"]) > 12 else ""))
        print(paint("Check it:", "dim") + " purr ui check %s --shell w10m|android" % out)
        return 0
    rep = ui.check(a.target, a.shell)
    if a.json:
        print(json.dumps(rep, indent=1))
    else:
        print("%s\n" % a.target)
        for c in rep["checks"]:
            line(c["status"], c["name"], c["text"])
        print("\n" + ("This passes the checks." if rep["ok"] else "This fails the checks: see the lines marked ✗."))
    return 0 if rep["ok"] else 1


def cmd_config(a):
    if a.action == "schema":
        for s in configs.settings_schema():
            print("%-24s %-30s %s" % (s["key"], s["values"][:30], s["help"]))
        return 0
    with open(a.file, "r") as fh:
        text = fh.read()
    if a.kind == "system":
        bad = 0
        for k, v in configs.parse_system_conf(text):
            err = configs.validate_setting(k, v)
            line("bad" if err else "ok", k, err or v)
            bad += bool(err)
        return 1 if bad else 0
    c = configs.parse_apps_conf(text)
    for e in c["errors"]:
        line("bad", "", e)
    if not c["errors"]:
        print(paint("Looks good.", "ok"))
    return 1 if c["errors"] else 0


def build_parser():
    p = argparse.ArgumentParser(prog="purr", description="PURR Developer Kit %s. With no command, opens PURR Studio." % __version__)
    p.add_argument("--workspace", help="where projects, keys and builds live (default ~/PURR-Projects)")
    sub = p.add_subparsers(dest="cmd")

    s = sub.add_parser("studio", help="open PURR Studio, the app")
    s.add_argument("--port", type=int, default=0)
    s.add_argument("--no-open", action="store_true", help="print the link instead of opening a window")
    s.set_defaults(fn=cmd_studio)

    sub.add_parser("doctor", help="check that the kit is whole").set_defaults(fn=cmd_doctor)

    s = sub.add_parser("inspect", aliases=["verify"], help="look inside an image, flash image, manifest or release folder")
    s.add_argument("path")
    s.add_argument("--json", action="store_true")
    s.add_argument("--keys", action="append", help="a folder of extra public keys (<role>_<id>.pub.bin)")
    s.set_defaults(fn=cmd_inspect)

    s = sub.add_parser("new", help="start a project: app, driver, board, flavor or theme")
    s.add_argument("kind", choices=projects.KINDS)
    s.add_argument("name")
    s.add_argument("--class", dest="driver_class", default="sensor-i2c", help="for a driver: gnss, lora, modem, wifi, bluetooth or sensor-i2c")
    s.add_argument("--template", default="unprivileged", help="for an app")
    s.set_defaults(fn=cmd_new)

    s = sub.add_parser("build", help="build (and optionally sign) a project")
    s.add_argument("project")
    s.add_argument("--board", default="tdeck_plus")
    s.add_argument("--key", help="a private key file, or a name in your keys folder")
    s.add_argument("--key-id", type=int, default=4)
    s.set_defaults(fn=cmd_build)

    s = sub.add_parser("keys", help="list your keys, or make one")
    s.add_argument("action", choices=("list", "new"))
    s.add_argument("--role", default="developer", choices=("boot", "system", "owner", "developer", "vendor"))
    s.add_argument("--key-id", type=int, default=4)
    s.add_argument("--force", action="store_true")
    s.set_defaults(fn=cmd_keys)

    s = sub.add_parser("sign", help="sign an image with a key")
    s.add_argument("file")
    s.add_argument("--key", required=True)
    s.add_argument("--key-id", type=int, default=4)
    s.add_argument("--out")
    s.set_defaults(fn=cmd_sign)

    s = sub.add_parser("brand", help="check a brand.toml, or write the header the system is built with")
    s.add_argument("action", choices=("check", "build"))
    s.add_argument("file")
    s.add_argument("--out")
    s.set_defaults(fn=cmd_brand)

    s = sub.add_parser("board", help="check a board.toml, or write the board file and device profile from it")
    s.add_argument("action", choices=("check", "build"))
    s.add_argument("file")
    s.add_argument("--out")
    s.set_defaults(fn=cmd_board)

    s = sub.add_parser("release", help="gather a signed release from the kit's prebuilt parts")
    s.add_argument("--board", default="tdeck_plus")
    s.add_argument("--key")
    s.add_argument("--key-id", type=int, default=4)
    s.add_argument("--out")
    s.set_defaults(fn=cmd_release)

    s = sub.add_parser("image", help="make one whole flash image for a device from the signed parts")
    s.add_argument("--board", default="tdeck_plus")
    s.add_argument("--parts", help="the folder with the signed parts (default: PURR_IMAGES, or the kit's images/)")
    s.add_argument("--out")
    s.add_argument("--trust-block", help="a trust block (purrcfg) to put in the image; without it the device uses its defaults")
    s.add_argument("--plan", action="store_true", help="only say what is there and what is missing")
    s.set_defaults(fn=cmd_image)

    s = sub.add_parser("system", help="a device with its parts, drivers, apps and keys: new, set, show, build (from signed parts or from source), test, list")
    s.add_argument("action", choices=("list", "new", "show", "set", "build", "test"))
    s.add_argument("name", nargs="?")
    s.add_argument("--board", choices=systemmod.BOARDS)
    s.add_argument("--parts-from", dest="parts_from", choices=("ready", "source"), help="ready-made signed parts, or compile everything from the newest source")
    s.add_argument("--source-url", dest="source_url")
    s.add_argument("--source-ref", dest="source_ref", help="the branch (default main)")
    s.add_argument("--shell", choices=sourcemod.SHELLS)
    s.add_argument("--version", help="the version the device compares, like 1.1.0")
    s.add_argument("--boot-key", dest="boot_key", help="name of your boot key in the keys folder (signs the firmware)")
    s.add_argument("--boot-key-id", dest="boot_key_id", type=int)
    s.add_argument("--key", help="name of your developer key (signs modules, drivers, apps)")
    s.add_argument("--key-id", dest="key_id", type=int)
    s.add_argument("--keep-ours", dest="keep_ours", choices=("on", "off"), help="keep PURR's keys in the image (so it takes PURR's updates) or take them out")
    s.add_argument("--drivers", help="comma-separated driver names (default: all that fit the board)")
    s.add_argument("--apps", help="comma-separated app projects; src:<name> is an app of the pulled source")
    s.add_argument("--ui")
    s.add_argument("--parts", help="the folder with ready-made signed parts")
    s.add_argument("--image", help="test: the image (default: the one the last build made)")
    s.add_argument("--seconds", type=int, default=100, help="test: how long to watch it boot")
    s.add_argument("--verbose", action="store_true", help="build: print every line the tools print")
    s.set_defaults(fn=cmd_system)

    s = sub.add_parser("source", help="the PURR OS source a system is compiled from: check, pull, compiler, install-compiler")
    s.add_argument("action", choices=("check", "pull", "compiler", "install-compiler"))
    s.add_argument("--url")
    s.add_argument("--ref")
    s.set_defaults(fn=cmd_source)

    s = sub.add_parser("serve", help="run the dev server for a device on your network")
    s.add_argument("--dir")
    s.add_argument("--port", type=int, default=47801)
    s.add_argument("--name", default="")
    s.set_defaults(fn=cmd_serve)

    s = sub.add_parser("publish", help="put a built .cat in the dev server's folder")
    s.add_argument("file")
    s.add_argument("--dir")
    s.set_defaults(fn=cmd_publish)

    s = sub.add_parser("flash", help="write an image to a board over USB (esptool)")
    s.add_argument("image")
    s.add_argument("--port")
    s.add_argument("--offset", default="0x0", help="0x0 for a full flash image")
    s.add_argument("--yes", action="store_true", help="do not ask")
    s.set_defaults(fn=cmd_flash)

    s = sub.add_parser("repo", help="make and run a repository of apps (init, add, build, release, endorse, move, verify)")
    s.add_argument("action", choices=("init", "add", "build", "release", "endorse", "move", "verify"))
    s.add_argument("folder", help="the repository folder (for `add`, the folder; for `verify`, a folder or https address)")
    s.add_argument("file", nargs="?", help="for `add`: the package")
    s.add_argument("--name"); s.add_argument("--maintainer"); s.add_argument("--url"); s.add_argument("--description")
    s.add_argument("--title"); s.add_argument("--desc"); s.add_argument("--category"); s.add_argument("--source"); s.add_argument("--perms", help="comma separated")
    s.add_argument("--type", choices=repo.TYPES); s.add_argument("--target", default="flash", choices=repo.TARGETS); s.add_argument("--dest"); s.add_argument("--min-os"); s.add_argument("--version"); s.add_argument("--needs-sd", action="store_true")
    s.add_argument("--layout", default="flat", choices=("flat", "tree")); s.add_argument("--out")
    s.add_argument("--key", help="for `endorse` and `move`: the project key a device trusts (today the boot key)"); s.add_argument("--key-id", type=int, default=1)
    s.add_argument("--from-url", help="for `move`: the recovery.manifest address the devices ask now"); s.add_argument("--recovery", help="for `move`: the new recovery.manifest address")
    s.add_argument("--modules", help="for `move`: the new modules.manifest address"); s.add_argument("--seq", type=int, help="for `move`: 1 for the first move, then higher each time")
    s.add_argument("--message", help="for `move`: what the user is told (printable ASCII, 79 characters)")
    s.set_defaults(fn=cmd_repo)

    s = sub.add_parser("emulator", help="open the emulator on the VM image (or one you name)")
    s.add_argument("image", nargs="?", help="a flash image (default: the kit's VM image)")
    s.add_argument("--board", default=emulator.DEFAULT_BOARD, help="the emulator's board file name (default %s)" % emulator.DEFAULT_BOARD)
    s.set_defaults(fn=cmd_emulator)

    s = sub.add_parser("vm-image", help="use a built image in the emulator: copy it to the kit's images/vm-flash.bin (default: your newest system build)")
    s.add_argument("image", nargs="?", help="a flash image (default: the newest .img under your workspace's builds/system-*)")
    s.set_defaults(fn=cmd_vm_image)

    s = sub.add_parser("run", help="build an app, put it on the emulated device and show it")
    s.add_argument("target", help="a project folder or name, or a signed .cat file")
    s.add_argument("--key", help="the key to sign with (a name in your workspace's keys, or a path)")
    s.add_argument("--key-id", type=int, default=2, help="the key's id on the device (2 = developer)")
    s.add_argument("--board", default="tdeck_plus", help="the PURR board the app is built for")
    s.add_argument("--emulator-board", default=emulator.DEFAULT_BOARD, help="the emulator's board file (default %s)" % emulator.DEFAULT_BOARD)
    s.add_argument("--image", help="a VM flash image (default: the kit's)")
    s.add_argument("--shots", help="where the pictures go (default: a temporary folder)")
    s.add_argument("--port", type=int, default=47801)
    s.add_argument("--window", action="store_true", help="open the emulator window and let you drive it, instead of the headless run")
    s.set_defaults(fn=cmd_run)

    s = sub.add_parser("config", help="the settings a device knows, or check a system.conf / apps.conf")
    s.add_argument("action", choices=("schema", "check"))
    s.add_argument("kind", nargs="?", choices=("system", "apps"), default="system")
    s.add_argument("file", nargs="?")
    s.set_defaults(fn=cmd_config)

    s = sub.add_parser("font", help="check a TrueType font file the way the OS will before it uses it")
    s.add_argument("action", choices=("check",))
    s.add_argument("file")
    s.add_argument("--max-size", type=int, default=font.DEFAULT_MAX_BYTES, help="the size cap in bytes (default %d, 1 MB)" % font.DEFAULT_MAX_BYTES)
    s.add_argument("--json", action="store_true")
    s.set_defaults(fn=cmd_font)

    s = sub.add_parser("ui", help="make a UI pack from a folder of PNGs, or check a pack or a theme folder the way the OS will")
    s.add_argument("action", choices=("pack", "check", "new", "import", "make"))
    s.add_argument("target", help="for `pack`: the folder of PNGs; for `check`: a pack.bin or a theme folder; for `new`, `import` and `make`: the UI project name")
    s.add_argument("--squareline", help="for `import`: a SquareLine Studio export folder, project folder or .zip")
    s.add_argument("--board", default="tdeck_plus", help="for `new` and `import`: the device the UI is for")
    s.add_argument("--out", help="for `pack`: the pack file (default pack.bin)")
    s.add_argument("--shell", choices=ui.SHELLS, help="for `new`/`import`: the shell the UI is for (default android). For `check`: this shell's picture names (UI/PACK-NAMES.md); a pack must have every required name, a theme folder may leave some out")
    s.add_argument("--json", action="store_true")
    s.set_defaults(fn=cmd_ui)

    sub.add_parser("version", help="print the kit's version").set_defaults(fn=lambda a: print("PURR Developer Kit", __version__) or 0)
    return p


def main(argv):
    p = build_parser()
    a = p.parse_args(argv)
    if not a.cmd:
        a = p.parse_args(["studio"] + argv)
    try:
        return a.fn(a) or 0
    except KeyboardInterrupt:
        return 130
    except (OSError, ValueError, minitoml.TomlError) as e:
        print(paint("Not this time: ", "bad") + str(e))
        return 1
