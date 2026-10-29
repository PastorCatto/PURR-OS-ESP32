"""The `purr` command. Friendly on purpose: it says what it found and what to do next."""

import argparse
import getpass
import json
import os
import shutil
import subprocess
import sys

from . import __version__, boards, brand, configs, doctor, formats, inspector, keys, minitoml, projects, release, runner, workspace

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


def cmd_run(a):
    exe = os.environ.get("PURR_EMULATOR")
    if not exe:
        print("The emulator is not part of the kit yet. When it is installed, set PURR_EMULATOR to its launcher\nand `purr run IMAGE` will start it with your image.")
        return 1
    return subprocess.call([exe, a.image])


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

    s = sub.add_parser("run", help="run an image in the emulator (when one is installed)")
    s.add_argument("image")
    s.set_defaults(fn=cmd_run)

    s = sub.add_parser("config", help="the settings a device knows, or check a system.conf / apps.conf")
    s.add_argument("action", choices=("schema", "check"))
    s.add_argument("kind", nargs="?", choices=("system", "apps"), default="system")
    s.add_argument("file", nargs="?")
    s.set_defaults(fn=cmd_config)

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
