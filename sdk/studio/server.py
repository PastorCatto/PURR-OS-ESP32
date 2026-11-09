"""PURR Studio's back end: a small local web server (standard library only) and the API the window talks to.

It listens on 127.0.0.1 only, and every request must carry the random token it printed at start-up (as a cookie the first page sets), so another
page open in your browser cannot reach it. Passwords come in a request body, go to the build tool's environment and are never kept or logged.
"""

import json
import mimetypes
import os
import secrets
import shutil
import sys
import tempfile
import threading
import time
import traceback
import webbrowser
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from purrsdk import (__version__, activity, boards, brand, configs, devserver, doctor, emulator, flash, image as imagemaker, inspector, keybag, keys, minitoml, projects, repo as repo_mod,  # noqa: E402
                     release, uibuilder, uimaker, workspace)

WEB = os.path.join(HERE, "web")
MAX_UPLOAD = 64 * 1024 * 1024


class Jobs:
    """Long things (builds) run on a thread; the window polls for their lines."""

    def __init__(self):
        self.items = {}
        self.lock = threading.Lock()

    def start(self, title, fn):
        jid = secrets.token_hex(4)
        job = {"id": jid, "title": title, "lines": [], "done": False, "result": None, "started": time.time()}
        with self.lock:
            self.items[jid] = job

        def run():
            try:
                job["result"] = fn(lambda line: job["lines"].append(line))
            except Exception as e:                          # a bug in a tool must not hang the window
                job["lines"].append("Something went wrong inside the tool: %s" % e)
                job["lines"].append(traceback.format_exc())
                job["result"] = {"ok": False}
            job["done"] = True

        threading.Thread(target=run, daemon=True).start()
        return jid

    def get(self, jid, since=0):
        job = self.items.get(jid)
        if job is None:
            return None
        return {"id": jid, "title": job["title"], "lines": job["lines"][since:], "next": len(job["lines"]), "done": job["done"], "result": job["result"]}


class State:
    def __init__(self, ws):
        self.ws = ws
        self.jobs = Jobs()
        self.server = devserver.DevServer()
        self.token = secrets.token_urlsafe(24)
        self.uploads = tempfile.mkdtemp(prefix="purr-studio-")
        self.emu = {"busy": False, "proc": None, "shots_dir": None}          # the Run page: one run or window at a time, and the folder of its pictures

    def safe_project(self, name):
        if not name or "/" in name or "\\" in name or name.startswith("."):
            raise ValueError("That is not a project name.")
        return os.path.join(self.ws.projects, name)


def _json(handler, obj, status=200):
    body = json.dumps(obj).encode("utf-8")
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(body)))
    handler.send_header("Cache-Control", "no-store")
    handler.end_headers()
    handler.wfile.write(body)


def make_handler(state):
    class Handler(BaseHTTPRequestHandler):
        server_version = "PURRStudio/" + __version__

        def log_message(self, fmt, *args):
            pass

        # ---- guards
        def _host_ok(self):
            host = (self.headers.get("Host") or "").split(":")[0]
            return host in ("127.0.0.1", "localhost")

        def _authed(self):
            cookie = self.headers.get("Cookie", "")
            return ("purr_token=" + state.token) in [c.strip() for c in cookie.split(";")]

        def _deny(self, text="Open Studio from the link it printed in your terminal."):
            _json(self, {"error": text}, 403)

        # ---- routing
        def do_GET(self):
            if not self._host_ok():
                return self._deny()
            url = urlparse(self.path)
            q = {k: v[0] for k, v in parse_qs(url.query).items()}
            if url.path == "/" and q.get("t") == state.token:
                self.send_response(302)
                self.send_header("Set-Cookie", "purr_token=%s; Path=/; HttpOnly; SameSite=Strict" % state.token)
                self.send_header("Location", "/")
                self.end_headers()
                return
            if url.path.startswith("/api/"):
                if not self._authed():
                    return self._deny()
                return self._api("GET", url.path[5:], q, None)
            if not self._authed():
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                body = b"<meta charset=utf-8><body style='font:16px system-ui;background:#111;color:#eee;padding:40px'><h1 style='font-weight:300'>purr studio</h1><p>Open Studio from the link it printed in your terminal. It carries a one-time key so only you can use it.</p>"
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            self._static(url.path)

        def do_POST(self):
            if not self._host_ok() or not self._authed():
                return self._deny()
            url = urlparse(self.path)
            if not url.path.startswith("/api/"):
                return _json(self, {"error": "not found"}, 404)
            length = int(self.headers.get("Content-Length") or 0)
            if length > MAX_UPLOAD:
                return _json(self, {"error": "That is bigger than I can take in one go (%d MB)." % (MAX_UPLOAD >> 20)}, 413)
            raw = self.rfile.read(length) if length else b""
            q = {k: v[0] for k, v in parse_qs(url.query).items()}
            if url.path in ("/api/inspect/upload", "/api/ui/upload"):
                return self._api("POST", url.path[5:], q, raw)
            try:
                body = json.loads(raw.decode("utf-8")) if raw else {}
            except ValueError:
                return _json(self, {"error": "That did not look like JSON."}, 400)
            self._api("POST", url.path[5:], q, body)

        def _static(self, path):
            rel = path.lstrip("/") or "index.html"
            full = os.path.normpath(os.path.join(WEB, rel))
            if not full.startswith(WEB) or not os.path.isfile(full):
                self.send_response(404)
                self.end_headers()
                return
            ctype = mimetypes.guess_type(full)[0] or "application/octet-stream"
            with open(full, "rb") as fh:
                data = fh.read()
            self.send_response(200)
            self.send_header("Content-Type", ctype + ("; charset=utf-8" if ctype.startswith("text/") or ctype.endswith("javascript") else ""))
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(data)

        def _api(self, method, route, q, body):
            try:
                result = handle(state, method, route, q, body)
            except (ValueError, projects.ProjectError, minitoml.TomlError, OSError) as e:
                return _json(self, {"error": str(e)}, 400)
            except Exception as e:
                traceback.print_exc()
                return _json(self, {"error": "Something went wrong inside Studio: %s" % e}, 500)
            if result is None:
                return _json(self, {"error": "I do not know that one."}, 404)
            _json(self, result)

    return Handler


# ------------------------------------------------------------------------------------------------------------------ the API

def _read_text(path):
    with open(path, "r", encoding="utf-8") as fh:
        return fh.read()


# ------------------------------------------------------------------------------------------------------------------ the emulator

def _emulator_status(state):
    """What the Run page needs to know: is there an emulator, can it show a window, can it run headless, is there an image to boot, and why not."""
    em = emulator.find()
    image = emulator.vm_image()
    why = ""
    if os.name == "nt":
        why = "The emulator is not available on Windows yet (the patched QEMU has no Windows build)."
    elif em is None:
        why = "There is no emulator in this kit. A kit built with the emulator has an emulator/ folder; set PURR_EMULATOR_DIR to use another."
    elif not (em["gui"] or em["headless"]):
        why = "The emulator folder has no program to run."
    elif not image:
        why = "There is no VM flash image to boot. Put one at images/vm-flash.bin in the kit, or set PURR_VM_IMAGE."
    boards_dir = em["boards"] if em else None
    names = sorted(f[:-5] for f in os.listdir(boards_dir) if f.endswith(".toml")) if boards_dir and os.path.isdir(boards_dir) else []
    proc = state.emu["proc"]
    return {"available": not why, "window": bool(em and em["gui"]), "headless": bool(em and em["headless"]), "image": image, "root": em["root"] if em else None,
            "boards": names, "default_board": emulator.DEFAULT_BOARD, "why": why, "busy": state.emu["busy"], "open": proc is not None and proc.poll() is None}


def _emulator_run(state, body):
    """Build an app (or take a built .cat), put it on the emulated device and show it. A job: the page watches its lines."""
    ws = state.ws
    st = _emulator_status(state)
    window = bool(body.get("window"))
    if not st["available"]:
        raise ValueError(st["why"])
    if window and not st["window"]:
        raise ValueError("This kit's emulator has no window program. Run it without the window.")
    if not window and not st["headless"]:
        raise ValueError("This kit's emulator has no headless program. Use the window.")
    if state.emu["busy"] or (state.emu["proc"] is not None and state.emu["proc"].poll() is None):
        raise ValueError("The emulator is already busy. Close its window, or stop it, first.")
    cat = body.get("file") or None
    root = None
    if not cat:
        root = state.safe_project(body.get("project"))
    elif not (os.path.isfile(cat) and cat.endswith(".cat")):
        raise ValueError("That is not a built app (.cat).")
    key, kid, pw = body.get("key") or None, int(body.get("key_id") or 0), body.get("password") or None
    if key and not os.path.isabs(key):
        key = os.path.join(ws.keys, key)
    board = body.get("board") or "tdeck_plus"
    image = body.get("image") or None
    ebd = body.get("emulator_board") or emulator.DEFAULT_BOARD
    activity.record(ws, "emulator", (body.get("project") or os.path.basename(cat or "an app")), "run", "", "in the emulator")
    shots = tempfile.mkdtemp(prefix="purr-shots-", dir=state.uploads)
    state.emu["busy"], state.emu["shots_dir"] = True, shots

    def work(say):
        try:
            path = cat
            if root:
                r = projects.build(ws, root, board=board, key=key, key_id=kid, password=pw, on_line=say)
                if not r["ok"] or not r["artifacts"]:
                    say("The build did not work, so there is nothing to run. The lines above say why.")
                    return {"ok": False}
                path = r["artifacts"][-1]
            try:
                res = emulator.run_app(path, image, ebd, shots, int(body.get("port") or devserver.DEFAULT_PORT), headless=not window, on_line=say)
            except emulator.EmulatorError as e:
                say("Not this time: %s" % e)
                return {"ok": False}
            return {"ok": res["ok"], "shots": [os.path.basename(x) for x in res["shots"]]}
        finally:
            state.emu["busy"] = False
    return {"job": state.jobs.start("Running in the emulator", work)}


def _repo_folder(ws, name):
    if not name or "/" in name or name.startswith("."):
        raise ValueError("That is not a repository name.")
    folder = os.path.join(ws.path, "repos", name)
    if not os.path.isfile(os.path.join(folder, "repo.toml")):
        raise ValueError("I do not know a repository called %s." % name)
    return folder


def _board_report(b):
    return {"board": b, "issues": boards.check(b), "coverage": boards.coverage(b), "pinmap": boards.pin_map(b)}


def handle(state, method, route, q, body):
    ws = state.ws
    if route == "state":
        return {"version": __version__, "workspace": ws.path, "accents": [{"name": n, "hex": "#%06X" % c} for n, c in brand.ACCENTS],
                "classes": list(__import__("purrsdk.templates", fromlist=["x"]).CLASSES.keys()), "kinds": list(projects.KINDS)}
    if route == "doctor":
        return {"checks": doctor.run()}
    if route == "inspect/upload":
        name = os.path.basename(q.get("name", "upload.bin")) or "upload.bin"
        path = os.path.join(state.uploads, name)
        with open(path, "wb") as fh:
            fh.write(body)
        rep = inspector.inspect_path(path, [ws.keys])
        rep["file"] = {"name": name, "size": len(body), "stored": path}
        return rep
    if route == "inspect":
        path = os.path.expanduser(body.get("path", ""))
        if not os.path.exists(path):
            raise ValueError("I cannot find %s." % path)
        rep = inspector.inspect_path(path, [ws.keys])
        rep["file"] = {"name": os.path.basename(path.rstrip("/")), "stored": path}
        return rep
    if route == "fs":                                              # a small folder browser, for choosing files without typing paths
        path = os.path.abspath(os.path.expanduser(q.get("path") or ws.path))
        if not os.path.isdir(path):
            path = os.path.dirname(path)
        entries = []
        for n in sorted(os.listdir(path), key=lambda s: s.lower()):
            if n.startswith("."):
                continue
            full = os.path.join(path, n)
            entries.append({"name": n, "dir": os.path.isdir(full), "size": os.path.getsize(full) if os.path.isfile(full) else 0})
        return {"path": path, "parent": os.path.dirname(path), "entries": entries[:500]}

    # ---- projects
    if route == "projects":
        ws.ensure()
        return {"projects": projects.list_projects(ws), "workspace": ws.path}
    if route == "projects/new":
        ws.ensure()
        opts = {k: v for k, v in body.items() if k not in ("kind", "name")}
        return projects.new(ws, body.get("kind", ""), (body.get("name") or "").strip(), **opts)
    if route == "projects/files":
        root = state.safe_project(q.get("project"))
        out = []
        for dp, dns, fns in os.walk(root):
            dns[:] = [d for d in dns if d not in ("build",) and not d.startswith(".")]
            for f in fns:
                out.append(os.path.relpath(os.path.join(dp, f), root))
        return {"files": sorted(out)}
    if route == "projects/file":
        root = state.safe_project(q.get("project") if method == "GET" else body.get("project"))
        rel = q.get("path") if method == "GET" else body.get("path")
        full = os.path.normpath(os.path.join(root, rel or ""))
        if not full.startswith(root + os.sep):
            raise ValueError("That file is outside the project.")
        if method == "GET":
            return {"path": rel, "text": _read_text(full)}
        with open(full, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(body.get("text", ""))
        return {"saved": rel}
    if route == "projects/build":
        root = state.safe_project(body.get("project"))
        key, kid, pw = body.get("key") or None, int(body.get("key_id") or 0), body.get("password") or None
        if key and not os.path.isabs(key):
            key = os.path.join(ws.keys, key)
        board = body.get("board") or "tdeck_plus"
        jid = state.jobs.start("Building %s" % body.get("project"), lambda say: projects.build(ws, root, board=board, key=key, key_id=kid, password=pw, on_line=say))
        try:
            kind = minitoml.load(os.path.join(root, "purr-project.toml")).get("project", {}).get("kind", "app")
        except (OSError, minitoml.TomlError):
            kind = "app"
        activity.record(ws, {"app": "app", "driver": "driver", "board": "board", "theme": "ui", "flavor": "other"}.get(kind, "other"), body.get("project"), "projects", body.get("project"), "built" + (", signed" if key else ", not signed"))
        return {"job": jid}
    if route == "job":
        out = state.jobs.get(q.get("id"), int(q.get("since") or 0))
        if out is None:
            raise ValueError("I do not know that job.")
        return out

    # ---- keys
    if route == "keys":
        ws.ensure()
        return {"dir": ws.keys, "keys": keys.list_keys(ws.keys), "builtin": [{"id": k["id"], "role": k["role"], "fingerprint": k["fingerprint"]}
                                                                       for k in keybag.load_dir(keybag.default_dir())]}
    if route == "keys/generate":
        ws.ensure()
        pw = body.get("password") or ""
        if len(pw) < 6:
            raise ValueError("Pick a password of at least 6 characters: it protects the private key.")
        role = body.get("role") or "developer"
        res = keys.generate(ws.keys, role, int(body.get("key_id") or 0) or {"boot": 1, "system": 2, "owner": 3, "developer": 4, "vendor": 5}[role] + 8, pw, bool(body.get("force")))
        return {"ok": res["rc"] == 0, "log": res["output"]}

    # ---- boards
    if route == "boards":
        ref = workspace.sdk_path("boards")
        out = [{"name": f[:-5], "path": os.path.join(ref, f), "where": "kit"} for f in sorted(os.listdir(ref)) if f.endswith(".toml")] if os.path.isdir(ref) else []
        for p in projects.list_projects(ws):
            if p["kind"] == "board":
                out.append({"name": p["name"], "path": os.path.join(p["path"], "board.toml"), "where": "mine"})
        return {"boards": out}
    if route == "board":
        path = q.get("path") if method == "GET" else body.get("path")
        if method == "GET" and (not path or not path.endswith(".toml")):
            raise ValueError("That is not a board file.")
        text = _read_text(path) if method == "GET" else body.get("text", "")
        b = minitoml.loads(text)
        b.setdefault("device", [])
        rep = _board_report(b)
        rep["text"] = text
        rep["path"] = path
        if "board" in b and b["board"].get("id"):
            bad = [i for i in rep["issues"] if i["level"] == "bad"]
            if not bad:
                rep["c"] = boards.to_c(b)
                rep["profile"] = boards.to_profile(b)[0]
        return rep
    if route == "board/save":
        text = body.get("text", "")
        minitoml.loads(text)
        path = body.get("path")
        if not path or not os.path.abspath(path).startswith(ws.projects):
            raise ValueError("Boards in the kit are read-only. Make a board project and edit that one.")
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        return {"saved": path}

    # ---- brand
    if route == "brand/default":
        return {"text": brand.TEMPLATE, "brand": brand.DEFAULT, "accents": [{"name": n, "hex": "#%06X" % c} for n, c in brand.ACCENTS]}
    if route == "brand/check":
        b = brand.merge(minitoml.loads(body.get("text", "")))
        issues = brand.check(brand.merge(b))
        out = {"brand": b, "issues": issues}
        if not [i for i in issues if i["level"] == "bad"]:
            out["header"] = brand.header(b)
        return out
    if route == "brand/font":
        with open(workspace.sdk_path("data", "font8x8.json")) as fh:
            return {"font": json.load(fh)}

    # ---- configure
    if route == "settings/schema":
        return {"settings": configs.settings_schema()}
    if route == "config/validate":
        kind, text = body.get("kind"), body.get("text", "")
        if kind == "system":
            problems = []
            for k, v in configs.parse_system_conf(text):
                err = configs.validate_setting(k, v)
                if err:
                    problems.append({"key": k, "text": err})
            return {"problems": problems, "pairs": configs.parse_system_conf(text)}
        if kind == "apps":
            c = configs.parse_apps_conf(text)
            return {"problems": [{"key": "", "text": e} for e in c["errors"]], "conf": c, "text": configs.dump_apps_conf(c)}
        raise ValueError("I can check system and apps files.")
    if route == "config/dump":
        if body.get("kind") == "system":
            return {"text": configs.dump_system_conf([(k, v) for k, v in body.get("pairs", [])])}
        if body.get("kind") == "apps":
            return {"text": configs.dump_apps_conf(body.get("conf", {}))}
        raise ValueError("I can write system and apps files.")

    # ---- repositories (Repositories/SPEC.md)
    if route == "repos":
        base = os.path.join(ws.path, "repos")
        out = []
        if os.path.isdir(base):
            for n in sorted(os.listdir(base)):
                if os.path.isfile(os.path.join(base, n, "repo.toml")):
                    out.append({"name": n, "path": os.path.join(base, n)})
        return {"repos": out, "base": base}
    if route == "repo/state":
        folder = _repo_folder(ws, q.get("name"))
        meta = minitoml.load(os.path.join(folder, "repo.toml")).get("repo", {})
        items = []
        pk = os.path.join(folder, "packages")
        if os.path.isdir(pk):
            for f in sorted(os.listdir(pk)):
                if f.endswith(".toml") or f.startswith("."):
                    continue
                m = minitoml.load(os.path.join(pk, f + ".toml")).get("item", {}) if os.path.isfile(os.path.join(pk, f + ".toml")) else {}
                items.append({"file": f, "size": os.path.getsize(os.path.join(pk, f)), "item": m})
        pub = repo_mod._public(folder)
        out_dir = os.path.join(folder, "out")
        built = sorted(os.listdir(out_dir)) if os.path.isdir(out_dir) else []
        return {"meta": meta, "id": repo_mod.fingerprint(pub), "fingerprint": repo_mod.pretty_fingerprint(repo_mod.fingerprint(pub)), "items": items, "built": built,
                "endorsed": os.path.isfile(os.path.join(folder, "endorsement.json")), "folder": folder, "out": out_dir}
    if route == "repo/init":
        name = (body.get("name") or "").strip()
        if not name or "/" in name or name.startswith("."):
            raise ValueError("Give the repository a simple name, like my-apps.")
        folder = os.path.join(ws.path, "repos", name)
        try:
            r = repo_mod.init(folder, body.get("title") or name, body.get("maintainer") or "", body.get("url") or "", body.get("password") or "", body.get("description") or "")
        except repo_mod.RepoError as e:
            raise ValueError(str(e))
        return {"id": r["id"], "name": name}
    if route == "repo/add":
        folder = _repo_folder(ws, body.get("name"))
        try:
            m = repo_mod.add(folder, body.get("file", ""), body.get("title", ""), body.get("desc", ""), body.get("category", ""), body.get("source", ""),
                             [p.strip() for p in (body.get("perms") or "").split(",") if p.strip()], body.get("type") or None, body.get("target") or "flash",
                             body.get("dest", ""), "", "", None, body.get("version") or None, bool(body.get("needs_sd")))
        except repo_mod.RepoError as e:
            raise ValueError(str(e))
        return {"added": m}
    if route == "repo/build":
        folder = _repo_folder(ws, body.get("name"))
        try:
            r = repo_mod.build(folder, os.path.join(folder, "out"), body.get("password") or "", body.get("layout") or "flat")
        except repo_mod.RepoError as e:
            raise ValueError(str(e))
        v = repo_mod.verify(os.path.join(folder, "out"), [ws.keys])
        return {"build": r, "verify": v}
    if route == "repo/verify":
        target = q.get("address") if method == "GET" and q.get("address") else os.path.join(_repo_folder(ws, q.get("name")), "out")
        return repo_mod.verify(target, [ws.keys])

    # ---- release
    if route == "release/parts":
        board = q.get("board") or "tdeck_plus"
        return {"parts": release.describe(release.prebuilt(board)), "board": board}
    if route == "release/build":
        ws.ensure()
        board = body.get("board") or "tdeck_plus"
        parts = release.prebuilt(board)
        parts.update({k: v for k, v in (body.get("parts") or {}).items() if v})
        chosen = {k: v for k, v in parts.items() if k in (body.get("include") or list(parts))}
        key, kid, pw = body.get("key") or None, int(body.get("key_id") or 0), body.get("password") or None
        if key and not os.path.isabs(key):
            key = os.path.join(ws.keys, key)
        dest = os.path.join(ws.builds, "release-" + (body.get("name") or time.strftime("%Y%m%d-%H%M%S")))
        if os.path.exists(dest):
            raise ValueError("%s is already there. Pick another name." % dest)
        jid = state.jobs.start("Gathering the release", lambda say: release.assemble(dest, chosen, board, key, kid, pw, on_line=say))
        return {"job": jid, "folder": dest}

    if route == "flash/ports":
        return {"ports": flash.list_ports(), "esptool": flash.have_esptool()}
    if route == "flash/write":
        if body.get("confirm") is not True:
            raise ValueError("Flashing replaces what is on the board. Tick the box to say you mean it.")
        port, image = body.get("port"), body.get("image")
        if not port or port not in flash.list_ports():
            raise ValueError("I cannot see that port. Is the board in download mode?")
        offset = str(body.get("offset") or "0x0")
        if not offset.lower().startswith("0x") or not all(c in "0123456789abcdefx" for c in offset.lower()):
            raise ValueError("The offset is a hex number like 0x0.")
        activity.record(ws, "flash", "flash %s" % os.path.basename(image or "an image"), "flash", image or "", "to %s" % port)
        return {"job": state.jobs.start("Flashing", lambda say: flash.write(port, image, offset, on_line=say))}
    if route == "trust/build":
        ws.ensure()
        from purrsdk import runner
        key, pw = body.get("key"), body.get("password") or None
        if not key:
            raise ValueError("Choose the boot key.")
        if not os.path.isabs(key):
            key = os.path.join(ws.keys, key)
        mode = body.get("mode", "warn")
        if mode not in ("off", "warn", "enforce"):
            raise ValueError("Mode is off, warn or enforce.")
        out = os.path.join(ws.builds, "purrcfg-%s.bin" % time.strftime("%Y%m%d-%H%M%S"))
        args = ["keys", "trust-block", "--key", key, "--mode", mode, "--out", out]
        for flag, opt in (("dev_locked", "--dev-locked"), ("dev_mode", "--dev-mode"), ("dev_recovery", "--dev-recovery"), ("installed", "--installed"),
                          ("auto_reinstall", "--auto-reinstall"), ("force_loader", "--force-loader"), ("force_recovery", "--force-recovery")):
            if body.get(flag):
                args.append(opt)

        def work(say):
            r = runner.purrstrap(args, password=pw, on_line=say)
            return {"ok": r["rc"] == 0, "artifacts": [out] if r["rc"] == 0 else []}
        return {"job": state.jobs.start("Trust block", work)}

    # ---- emulator (the Run page and the run button on a project)
    if route == "emulator":
        return _emulator_status(state)
    if route == "emulator/run":
        return _emulator_run(state, body)
    if route == "emulator/open":
        em = state.emu
        if em["busy"] or (em["proc"] is not None and em["proc"].poll() is None):
            raise ValueError("The emulator is already busy. Close its window, or stop it, first.")
        try:
            em["proc"] = emulator.open_window(body.get("image") or None, body.get("board") or emulator.DEFAULT_BOARD)
        except emulator.EmulatorError as e:
            raise ValueError(str(e))
        return {"ok": True}
    if route == "emulator/stop":
        proc = state.emu["proc"]
        if proc is not None and proc.poll() is None:
            proc.terminate()
        state.emu["proc"] = None
        return {"ok": True}
    if route == "emulator/shot":
        name = os.path.basename(q.get("name") or "")
        folder = state.emu["shots_dir"]
        path = os.path.join(folder, name) if folder and name.endswith(".png") else None
        if not path or not os.path.isfile(path):
            raise ValueError("I do not have that picture.")
        import base64
        with open(path, "rb") as fh:
            return {"name": name, "data": "data:image/png;base64," + base64.b64encode(fh.read()).decode("ascii")}

    # ---- image maker (a whole flash image for a device)
    if route == "image/boards":
        return {"boards": [{"id": b, "name": n} for b, n in (("tdeck_plus", "LilyGO T-Deck Plus"), ("tdeck", "LilyGO T-Deck"), ("waveshare154", "Waveshare e-paper 1.54"))]}
    if route == "image/folders":
        board = q.get("board") or "tdeck_plus"
        cands = [os.environ.get("PURR_IMAGES"), workspace.kit_path("images")]
        if os.path.isdir(ws.builds):
            cands += [os.path.join(ws.builds, d) for d in sorted(os.listdir(ws.builds)) if d.startswith("release")]
        out = []
        for c in cands:
            if c and os.path.isdir(c) and c not in [x["path"] for x in out]:
                found = imagemaker.find_parts(c, board)
                out.append({"path": c, "found": sorted(found), "complete": len(found) == len(imagemaker.PARTS)})
        return {"folders": out}
    if route == "image/plan":
        board, folder = q.get("board") or "tdeck_plus", q.get("folder") or ""
        try:
            return imagemaker.plan(board, folder)
        except imagemaker.ImageError as e:
            raise ValueError(str(e))
    if route == "image/build":
        ws.ensure()
        board, folder = body.get("board") or "tdeck_plus", body.get("folder") or ""
        out = os.path.join(ws.builds, "%s-%s.img" % (board, time.strftime("%Y%m%d-%H%M%S")))

        def work(say):
            try:
                r = imagemaker.build(board, folder, out, trust_block=body.get("trust_block") or None, on_line=say)
            except imagemaker.ImageError as e:
                say("Not this time: %s" % e)
                return {"ok": False}
            say("[ok] Your flash image is ready.")
            activity.record(ws, "image", "%s image" % board, "image", "", "made, %d MB" % (r["bytes"] >> 20))
            return {"ok": True, "out": r["out"], "bytes": r["bytes"], "sha256": r["sha256"], "notes": r["notes"], "fit": r["fit"], "artifacts": [r["out"]]}
        return {"job": state.jobs.start("Making the flash image", work)}

    # ---- UI maker (a theme from ui.toml; a SquareLine Studio export can be dropped in)
    if route == "ui/starter":
        return {"text": uimaker.starter(q.get("name") or "my-ui", q.get("shell") or "android", q.get("board") or "tdeck_plus")}
    if route == "ui/upload":
        os.makedirs(state.uploads, exist_ok=True)
        ext = ".zip" if q.get("kind") == "zip" else ".bin"
        path = os.path.join(state.uploads, "squareline-%s%s" % (secrets.token_hex(4), ext))
        with open(path, "wb") as fh:
            fh.write(body or b"")
        return {"path": path, "bytes": len(body or b"")}
    if route == "ui/projects":
        ws.ensure()
        return {"projects": [p for p in projects.list_projects(ws) if p["kind"] == "theme" and os.path.isfile(os.path.join(p["path"], "ui.toml"))]}
    if route == "ui/new":
        ws.ensure()
        name = (body.get("name") or "").strip()
        try:
            r = uimaker.new_project(ws, name, body.get("shell") or "android", body.get("board") or "tdeck_plus", squareline=body.get("squareline") or None, on_line=lambda l: None, start=body.get("start") or "android")
            activity.record(ws, "ui", name, "builder", name, "started")
        except projects.ProjectError as e:
            raise ValueError(str(e))
        return {"name": name, "path": r["path"], "import": r["import"]}
    if route == "ui/project":
        root = state.safe_project(q.get("name"))
        toml = os.path.join(root, "ui.toml")
        if not os.path.isfile(toml):
            raise ValueError("That project has no ui.toml.")
        cfg, problems = uimaker.load_template(toml)
        pics = []
        imp = os.path.join(root, "imported")
        if os.path.isdir(imp) and cfg:
            for f in sorted(os.listdir(imp)):
                if f.lower().endswith(".png"):
                    pics.append({"id": f[:-4], "file": "imported/" + f, "slot": next((k for k, v in cfg["pictures"].items() if v == "imported/" + f), None), "guess": uimaker.guess_slot(f[:-4], cfg["shell"])})
        return {"name": q.get("name"), "text": _read_text(toml), "problems": problems, "pictures": pics,
                "slots": sorted(uimaker.wanted(cfg)) if cfg else [], "shell": cfg["shell"] if cfg else None}
    if route == "ui/save":
        root = state.safe_project(body.get("name"))
        text = body.get("text") or ""
        cfg, problems = uimaker.load_template(text)
        with open(os.path.join(root, "ui.toml"), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        return {"saved": True, "problems": problems}
    if route == "ui/map":
        root = state.safe_project(body.get("name"))
        toml = os.path.join(root, "ui.toml")
        text = uimaker.set_mapping(_read_text(toml), body.get("slot") or "", body.get("file") or None, body.get("old_slot") or None)
        with open(toml, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        return {"text": text}
    if route == "ui/preview":
        cfg, problems = uimaker.load_template(body.get("text") or "")
        if cfg is None or problems and not cfg.get("look"):
            raise ValueError(" ".join(problems))
        sample = {"android": ["fr_app", "fr_settings", "fr_folder", "fr_gallery", "fr_messaging", "st_b60", "st_bc3", "st_s3", "st_w4", "kb_menu", "kb_home", "kb_back"],
                  "w10m": ["wp_settings", "wp_system", "wp_devices", "wp_network", "wp_personalization", "wp_apps", "wp_account", "wp_time", "wp_privacy", "wp_about", "wifi_icon", "brightness_ic"]}.get(cfg["shell"], [])
        import base64
        import io
        out = []
        want = uimaker.wanted(cfg)
        for n in sample:
            if n not in want:
                continue
            w, h = want[n]
            rgba = uimaker.draw(n, w, h, cfg["look"])
            tmp = os.path.join(state.uploads, "prev-%s.png" % secrets.token_hex(3))
            uimaker.write_png(tmp, w, h, rgba)
            with open(tmp, "rb") as fh:
                out.append({"name": n, "w": w, "h": h, "data": "data:image/png;base64," + base64.b64encode(fh.read()).decode("ascii")})
            os.remove(tmp)
        look = cfg["look"]
        return {"pictures": out, "problems": problems, "colours": {"accent": "#%02x%02x%02x" % look.accent, "background": "#%02x%02x%02x" % look.bg, "foreground": "#%02x%02x%02x" % look.fg}}
    if route == "ui/make":
        root = state.safe_project(body.get("name"))

        def work(say):
            r = uimaker.compose(root, on_line=say)
            for t in r.get("problems", []) + r.get("notes", []):
                say(t)
            if not r["ok"]:
                return {"ok": False}
            say("[ok] Your UI is ready.")
            activity.record(ws, "ui", body.get("name"), "ready", body.get("name"), "made, %d pictures" % r["count"])
            return {"ok": True, "out": r["out"], "bytes": r["bytes"], "count": r["count"], "generated": len(r["generated"]), "yours": r["yours"], "resized": r["resized"], "checks": r["checks"], "artifacts": [r["pack"]]}
        return {"job": state.jobs.start("Making the UI", work)}

    # ---- what was done last (the main menu's big tile)
    if route == "activity":
        ws.ensure()
        now = time.time()
        return {"items": [dict(i, ago=activity.ago(i["when"], now)) for i in activity.load(ws)[:12]]}
    if route == "activity/record":
        ws.ensure()
        activity.record(ws, body.get("kind") or "other", (body.get("title") or "").strip() or "something", body.get("route") or "home", body.get("arg") or "", body.get("note") or "")
        return {"ok": True}

    # ---- the UI builder (layouts, icon packs)
    if route == "ui/builder":
        root = state.safe_project(q.get("name"))
        toml = os.path.join(root, "ui.toml")
        if not os.path.isfile(toml):
            raise ValueError("That project has no ui.toml.")
        cfg, problems = uimaker.load_template(toml)
        if cfg is None:
            raise ValueError(" ".join(problems))
        look = cfg["look"]
        return {"name": q.get("name"), "shell": cfg["shell"], "board": cfg["board"], "size": {"w": cfg["size"][0], "h": cfg["size"][1]}, "elements": cfg["elements"],
                "screens": uibuilder.screens(cfg["elements"]), "icons": cfg["icons"], "problems": problems,
                "look": {"accent": cfg["accent_text"], "background": "#%02x%02x%02x" % look.bg, "foreground": "#%02x%02x%02x" % look.fg}}
    if route == "ui/builder/save":
        root = state.safe_project(body.get("name"))
        toml = os.path.join(root, "ui.toml")
        text = uimaker.save_elements(_read_text(toml), body.get("elements") or [])
        cfg, problems = uimaker.load_template(text)
        if cfg is None:
            raise ValueError(" ".join(problems))
        with open(toml, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        notes, probs = [], []
        names = set(uimaker.wanted(cfg))
        for scr in uibuilder.screens(cfg["elements"]):
            _t, n, p = uibuilder.to_screen_conf(cfg["elements"], scr, cfg["size"], names)
            notes += ["[%s] %s" % (scr, x) for x in n]
            probs += p
        activity.record(ws, "ui", body.get("name"), "builder", body.get("name"), "%d elements" % len(cfg["elements"]))
        return {"saved": True, "count": len(cfg["elements"]), "notes": notes, "problems": problems + probs}
    if route == "ui/icons":
        root = state.safe_project(q.get("name"))
        return uimaker.icon_report(root)
    if route == "ui/icons/import":
        root = state.safe_project(body.get("name"))
        src = body.get("src") or ""
        if not src or not os.path.exists(src):
            raise ValueError("I cannot find that icon pack. Drop a zip or folder of PNGs, a pack.bin, or a SquareLine export.")
        toml = os.path.join(root, "ui.toml")
        cfg, problems = uimaker.load_template(toml)
        if cfg is None:
            raise ValueError(" ".join(problems))
        try:
            info = uimaker.import_icon_pack(src, root, cfg["shell"], on_line=lambda l: None)
        except ValueError as e:
            raise ValueError(str(e))
        text = uimaker.apply_import(_read_text(toml), info)
        icons = dict(cfg["icons"], use=True, pack=os.path.basename(src))
        text = uimaker.set_icons(text, **icons)
        with open(toml, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        activity.record(ws, "ui", body.get("name"), "builder", body.get("name"), "icon pack: %s" % os.path.basename(src))
        rep = uimaker.icon_report(root)
        rep["imported"] = info["pictures"]
        rep["skipped"] = info["skipped"]
        return rep
    if route == "ui/icons/options":
        root = state.safe_project(body.get("name"))
        toml = os.path.join(root, "ui.toml")
        cfg, problems = uimaker.load_template(toml)
        if cfg is None:
            raise ValueError(" ".join(problems))
        icons = dict(cfg["icons"])
        for k in ("use", "fill_missing", "resize", "recolour"):
            if k in body:
                icons[k] = bool(body[k])
        text = uimaker.set_icons(_read_text(toml), **icons)
        with open(toml, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        return uimaker.icon_report(root)

    # ---- docs
    if route == "docs":
        d = workspace.sdk_path("docs")
        out = []
        if os.path.isdir(d):
            for f in sorted(os.listdir(d)):
                if f.endswith(".md"):
                    first = _read_text(os.path.join(d, f)).split("\n", 1)[0].lstrip("# ").strip()
                    out.append({"name": f[:-3], "title": first or f[:-3]})
        return {"docs": out}
    if route == "docs/page":
        name = os.path.basename(q.get("name", ""))
        path = os.path.join(workspace.sdk_path("docs"), name + ".md")
        if not os.path.isfile(path):
            raise ValueError("I do not have a guide called %s." % name)
        return {"text": _read_text(path)}

    # ---- server
    if route == "server":
        s = state.server.status()
        s["default_folder"] = ws.repo
        return s
    if route == "server/start":
        ws.ensure()
        folder = body.get("folder") or ws.repo
        return state.server.start(folder, int(body.get("port") or devserver.DEFAULT_PORT), body.get("name") or "")
    if route == "server/stop":
        return state.server.stop()
    if route == "server/publish":
        ws.ensure()
        src = body.get("file")
        if not src or not os.path.isfile(src):
            raise ValueError("I cannot find that file.")
        from purrsdk import runner
        res = runner.purrstrap(["dev", "publish", "--file", src, "--dir", body.get("folder") or ws.repo])
        return {"ok": res["rc"] == 0, "log": res["output"]}
    if route == "server/contents":
        folder = q.get("folder") or ws.repo
        out = []
        if os.path.isdir(folder):
            for dp, dns, fns in os.walk(folder):
                for f in sorted(fns):
                    full = os.path.join(dp, f)
                    out.append({"path": os.path.relpath(full, folder), "size": os.path.getsize(full)})
        return {"folder": folder, "files": out[:400]}
    return None


def serve(workspace_dir=None, port=0, open_window=True, quiet=False):
    ws = workspace.Workspace(workspace_dir).ensure()
    state = State(ws)
    httpd = ThreadingHTTPServer(("127.0.0.1", port), make_handler(state))
    url = "http://127.0.0.1:%d/?t=%s" % (httpd.server_address[1], state.token)
    if not quiet:
        print("PURR Studio is ready.\n  %s\nPress Ctrl+C here to close it." % url, flush=True)
    if open_window:
        threading.Timer(0.4, lambda: open_app_window(url)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        state.server.stop()
        shutil.rmtree(state.uploads, ignore_errors=True)
    return 0


def open_app_window(url):
    """A chromeless window if a Chromium-family browser is installed (it feels like an app), else the default browser."""
    candidates = []
    if sys.platform == "darwin":
        for app in ("Google Chrome", "Microsoft Edge", "Brave Browser", "Chromium"):
            if os.path.isdir("/Applications/%s.app" % app):
                candidates.append(["open", "-na", app, "--args", "--app=" + url, "--window-size=1180,820"])
    else:
        for exe in ("google-chrome", "chromium", "chromium-browser", "microsoft-edge", "msedge", "chrome"):
            found = shutil.which(exe)
            if found:
                candidates.append([found, "--app=" + url, "--window-size=1180,820"])
    for cmd in candidates:
        try:
            import subprocess
            subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return
        except OSError:
            continue
    webbrowser.open(url)
