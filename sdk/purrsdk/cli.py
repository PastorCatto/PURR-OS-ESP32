"""The `purr` command. Friendly on purpose: it says what it found and what to do next."""

import argparse
import getpass
import json
import os
import shutil
import subprocess
import sys

from . import __version__, boards, brand, configs, doctor, emulator, flash, font, formats, inspector, keys, minitoml, projects, release, repo, runner, workspace

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

    s = sub.add_parser("new", help="start a project: app, driver, board or flavor")
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

    s = sub.add_parser("repo", help="make and run a repository of apps (init, add, build, release, endorse, verify)")
    s.add_argument("action", choices=("init", "add", "build", "release", "endorse", "verify"))
    s.add_argument("folder", help="the repository folder (for `add`, the folder; for `verify`, a folder or https address)")
    s.add_argument("file", nargs="?", help="for `add`: the package")
    s.add_argument("--name"); s.add_argument("--maintainer"); s.add_argument("--url"); s.add_argument("--description")
    s.add_argument("--title"); s.add_argument("--desc"); s.add_argument("--category"); s.add_argument("--source"); s.add_argument("--perms", help="comma separated")
    s.add_argument("--type", choices=repo.TYPES); s.add_argument("--target", default="flash", choices=repo.TARGETS); s.add_argument("--dest"); s.add_argument("--min-os"); s.add_argument("--version"); s.add_argument("--needs-sd", action="store_true")
    s.add_argument("--layout", default="flat", choices=("flat", "tree")); s.add_argument("--out")
    s.add_argument("--key", help="for `endorse`: the project key a device trusts (today the boot key)"); s.add_argument("--key-id", type=int, default=1)
    s.set_defaults(fn=cmd_repo)

    s = sub.add_parser("emulator", help="open the emulator on the VM image (or one you name)")
    s.add_argument("image", nargs="?", help="a flash image (default: the kit's VM image)")
    s.add_argument("--board", default=emulator.DEFAULT_BOARD, help="the emulator's board file name (default %s)" % emulator.DEFAULT_BOARD)
    s.set_defaults(fn=cmd_emulator)

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
