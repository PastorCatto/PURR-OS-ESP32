"""Build the system from the source instead of from ready-made parts: pull the newest PURR OS source, compile the firmware with ESP-IDF, then package and sign
each part with your own key (the "compile from source" road of Studio's build-a-system screens).

    pull      fetch the source (only the folders a firmware build needs) into <workspace>/source/tree, with git if there is git, otherwise as a GitHub tarball
    compiler  find ESP-IDF 5.3.5 (or install it once, with git)
    compile   purrstrap `coreos build` for the three profiles of the board: full = the kernel, minimal = the recovery loader, recovery = KittenOS
    package   wrap each binary in a PURR container (`coreos package`, `package-component`, `bootpkg build`) and sign it with your key

The result is a folder of parts named the way the image maker expects (`kernel-<board>.cat` ...), so the rest of the road (image, delivery) is the same as with
ready-made parts. Nothing here touches your own checkout of the repository: the source lives in the workspace.
"""

import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request

from . import formats, runner

DEFAULT_URL = "https://github.com/PastorCatto/PURR-OS-ESP32"
DEFAULT_REF = "main"
PATHS = ("PurrOS", "UI", "purrstrap", "bootloader", "bootpkg")      # what a firmware build reads from the repository
IDF_VERSION = "5.3.5"
PROFILES = (("full", "the kernel"), ("minimal", "the recovery loader"), ("recovery", "KittenOS"))
SHELLS = ("w10m", "android")
BOARDS_CHIP = {"tdeck_plus": "esp32s3", "tdeck": "esp32s3", "waveshare154": "esp32s3"}


class SourceError(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------------- the source

def folder(ws):
    return os.path.join(ws.path, "source")


def tree(ws):
    return os.path.join(folder(ws), "tree")


def _state_path(ws):
    return os.path.join(folder(ws), "source.json")


def have_git():
    return bool(shutil.which("git"))


def github(url):
    """(owner, repo) for a github.com address, or None."""
    u = url.strip().rstrip("/")
    if u.endswith(".git"):
        u = u[:-4]
    for pre in ("https://github.com/", "http://github.com/", "git@github.com:"):
        if u.startswith(pre):
            parts = u[len(pre):].split("/")
            if len(parts) >= 2 and parts[0] and parts[1]:
                return parts[0], parts[1]
    return None


def _git(args, cwd=None, on_line=None):
    """Run git. Returns (rc, output)."""
    try:
        p = subprocess.Popen(["git"] + list(args), cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, errors="replace", stdin=subprocess.DEVNULL,
                             env=dict(os.environ, GIT_TERMINAL_PROMPT="0"))
    except OSError as e:
        return 127, "cannot start git: %s" % e
    out = []
    for line in p.stdout:
        out.append(line)
        if on_line and line.strip():
            on_line(line.rstrip())
    return p.wait(), "".join(out)


def _http(url, accept=None, timeout=30):
    req = urllib.request.Request(url, headers={"User-Agent": "purr-sdk", **({"Accept": accept} if accept else {})})
    return urllib.request.urlopen(req, timeout=timeout)


def remote_head(url, ref):
    """The newest commit of `ref` at the source address. Raises SourceError when it cannot be reached."""
    if have_git():
        rc, out = _git(["ls-remote", url, ref])
        if rc == 0 and out.split():
            return out.split()[0]
        if rc != 0:
            raise SourceError("I could not reach %s: %s" % (url, (out.strip().splitlines() or ["no answer"])[-1]))
        raise SourceError("%s has no branch or tag called %s." % (url, ref))
    g = github(url)
    if not g:
        raise SourceError("Without git I can only fetch from github.com. Install git, or use a github.com address.")
    try:
        with _http("https://api.github.com/repos/%s/%s/commits/%s" % (g[0], g[1], ref), accept="application/vnd.github.sha") as r:
            return r.read().decode("ascii").strip()
    except Exception as e:
        raise SourceError("I could not reach github.com: %s" % e)


def state(ws):
    try:
        with open(_state_path(ws), "r", encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return {}


def _save_state(ws, d):
    os.makedirs(folder(ws), exist_ok=True)
    with open(_state_path(ws), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(d, fh)


def check(ws, url=DEFAULT_URL, ref=DEFAULT_REF):
    """What is pulled and whether something newer is waiting: {"pulled", "commit", "latest", "newer", "when", "error"}."""
    st = state(ws)
    pulled = bool(st.get("commit")) and os.path.isdir(os.path.join(tree(ws), "PurrOS")) and st.get("url") == url and st.get("ref") == ref
    out = {"pulled": pulled, "commit": st.get("commit", "") if pulled else "", "when": st.get("when", "") if pulled else "", "latest": "", "newer": False, "error": ""}
    try:
        out["latest"] = remote_head(url, ref)
        out["newer"] = (not pulled) or out["latest"] != out["commit"]
    except SourceError as e:
        out["error"] = str(e)
    return out


def pull(ws, url=DEFAULT_URL, ref=DEFAULT_REF, on_line=print):
    """Fetch the newest source. Returns {"commit", "changed"}. The tree is replaced as a whole, so a half-finished pull never leaves a mixed one."""
    latest = remote_head(url, ref)
    st = state(ws)
    if st.get("commit") == latest and st.get("url") == url and st.get("ref") == ref and os.path.isdir(os.path.join(tree(ws), "PurrOS")):
        on_line("The source is already the newest (%s)." % latest[:10])
        return {"commit": latest, "changed": False}
    os.makedirs(folder(ws), exist_ok=True)
    tmp = os.path.join(folder(ws), "tree.new")
    shutil.rmtree(tmp, ignore_errors=True)
    on_line("Fetching %s (%s) %s..." % (url, ref, "with git" if have_git() else "as a tarball"))
    try:
        if have_git():
            _fetch_git(url, ref, tmp, on_line)
        else:
            _fetch_tarball(url, ref, tmp, on_line)
    except SourceError:
        shutil.rmtree(tmp, ignore_errors=True)
        raise
    if not os.path.isdir(os.path.join(tmp, "PurrOS")):
        shutil.rmtree(tmp, ignore_errors=True)
        raise SourceError("%s (%s) has no PurrOS folder: this is not the PURR OS source." % (url, ref))
    # keep the previous build folders: they are what makes the next compile a short one
    old = tree(ws)
    for sub in ("PurrOS/build",):
        if os.path.isdir(os.path.join(old, sub)):
            os.makedirs(os.path.dirname(os.path.join(tmp, sub)), exist_ok=True)
            shutil.rmtree(os.path.join(tmp, sub), ignore_errors=True)
            shutil.move(os.path.join(old, sub), os.path.join(tmp, sub))
    shutil.rmtree(old, ignore_errors=True)
    os.replace(tmp, old)
    _save_state(ws, {"url": url, "ref": ref, "commit": latest, "when": time.strftime("%Y-%m-%d %H:%M")})
    on_line("Pulled %s." % latest[:10])
    return {"commit": latest, "changed": True}


def _fetch_git(url, ref, dest, on_line):
    # only the folders a firmware build needs: the repository holds far more than that
    rc, out = _git(["clone", "--depth", "1", "--filter=blob:none", "--sparse", "--branch", ref, url, dest], on_line=on_line)
    if rc == 0:
        rc, out = _git(["sparse-checkout", "set"] + list(PATHS), cwd=dest, on_line=on_line)
    if rc != 0:
        on_line("A partial fetch did not work here; fetching the whole branch instead (this is bigger).")
        shutil.rmtree(dest, ignore_errors=True)
        rc, out = _git(["clone", "--depth", "1", "--branch", ref, url, dest], on_line=on_line)
        if rc != 0:
            raise SourceError("git could not fetch the source: %s" % (out.strip().splitlines() or ["no reason given"])[-1])
    shutil.rmtree(os.path.join(dest, ".git"), ignore_errors=True)


def _fetch_tarball(url, ref, dest, on_line):
    g = github(url)
    if not g:
        raise SourceError("Without git I can only fetch from github.com. Install git, or use a github.com address.")
    on_line("Downloading the source from github.com...")
    try:
        with _http("https://codeload.github.com/%s/%s/tar.gz/%s" % (g[0], g[1], ref), timeout=120) as r:
            data = r.read()
    except Exception as e:
        raise SourceError("I could not download the source: %s" % e)
    _extract_tar(data, dest)


def _extract_tar(data, dest):
    """Unpack only the folders a build needs from a GitHub tarball (whose files all sit under one top folder)."""
    os.makedirs(dest, exist_ok=True)
    root = os.path.realpath(dest)
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tf:
        for m in tf:
            parts = m.name.split("/", 1)
            if len(parts) < 2 or not parts[1].split("/")[0] in PATHS:
                continue
            target = os.path.realpath(os.path.join(dest, parts[1]))
            if not (target == root or target.startswith(root + os.sep)):
                continue                                    # a path that climbs out of the folder
            if m.isdir():
                os.makedirs(target, exist_ok=True)
            elif m.isfile():
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, "wb") as fh:
                    fh.write(tf.extractfile(m).read())
                if m.mode & 0o111:
                    os.chmod(target, 0o755)


# ---------------------------------------------------------------------------------------------------------------- the compiler

def _coreos():
    """purrstrap's coreos module (it knows where ESP-IDF may be)."""
    d = formats.purrstrap_dir()
    for p in (d, os.path.join(d, "scripts"), os.path.join(d, "lib")):
        if p not in sys.path:
            sys.path.insert(0, p)
    import importlib
    return importlib.import_module("coreos")


def compiler(find=None):
    """{"found", "version", "path", "right"}: ESP-IDF as purrstrap finds it."""
    try:
        idf = (find or _coreos().find_idf)()
    except Exception:
        idf = None
    if not idf:
        return {"found": False, "version": "", "path": "", "right": False}
    return {"found": True, "version": idf.version, "path": idf.path, "right": (not idf.version) or idf.version == IDF_VERSION}


def idf_home():
    return os.path.join(os.path.expanduser("~"), "esp", "v" + IDF_VERSION, "esp-idf")


def install_compiler(on_line=print):
    """Install ESP-IDF once, where purrstrap looks for it (~/esp/v5.3.5/esp-idf). Needs git and a few GB of room. Windows: use the ESP-IDF installer."""
    if os.name == "nt":
        raise SourceError("On Windows install ESP-IDF with Espressif's installer (the EIM installer), version %s: this window finds it afterwards." % IDF_VERSION)
    if not have_git():
        raise SourceError("Installing the compiler needs git. Install git, then try again.")
    home = idf_home()
    if not os.path.isfile(os.path.join(home, "install.sh")):
        os.makedirs(os.path.dirname(home), exist_ok=True)
        on_line("Fetching ESP-IDF %s (about 1 GB)..." % IDF_VERSION)
        rc, out = _git(["clone", "--depth", "1", "--branch", "v" + IDF_VERSION, "--recursive", "--shallow-submodules", "https://github.com/espressif/esp-idf.git", home], on_line=on_line)
        if rc != 0:
            raise SourceError("git could not fetch ESP-IDF: %s" % (out.strip().splitlines() or ["no reason given"])[-1])
    on_line("Installing the ESP32-S3 tools (about 1.5 GB, a few minutes)...")
    r = runner_shell(["bash", os.path.join(home, "install.sh"), "esp32s3"], on_line)
    if r != 0:
        raise SourceError("The ESP-IDF installer failed; the lines above say why.")
    return compiler()


def runner_shell(argv, on_line):
    try:
        p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, errors="replace", stdin=subprocess.DEVNULL)
    except OSError as e:
        on_line("cannot start %s: %s" % (argv[0], e))
        return 127
    for line in p.stdout:
        on_line(line.rstrip())
    return p.wait()


# ---------------------------------------------------------------------------------------------------------------- compile, package, sign

def short_version(commit):
    """The version a part built from source carries (12 characters at most in a container header): src- and the first seven letters of the commit."""
    return "src-" + commit[:7]


def build_dir(ws, board, profile, shell):
    return os.path.join(tree(ws), "PurrOS", "build", "%s-%s%s" % (board, profile, "" if shell == "w10m" else "-" + shell))


def compile_and_package(ws, board, shell="android", key=None, key_id=4, password=None, out=None, on_step=None, on_line=print, run=None, clean=False):
    """Compile the firmware of `board` from the pulled source and put signed parts in `out`. Returns {"ok", "out", "version", "parts": [names], "problem"}.

    `run(args, extra_env=None, password=None)` runs purrstrap and returns {"rc", "output"}; it is a parameter so tests can stand in for the compiler."""
    st = state(ws)
    if not st.get("commit"):
        return {"ok": False, "problem": "Pull the source first."}
    if board not in BOARDS_CHIP:
        return {"ok": False, "problem": "I cannot build firmware for the board %s yet." % board}
    if shell not in SHELLS:
        return {"ok": False, "problem": "The shell is one of %s." % ", ".join(SHELLS)}
    if not key:
        return {"ok": False, "problem": "Choose a signing key: the parts you compile have to be signed."}
    version = short_version(st["commit"])
    t = tree(ws)
    env = {"PURR_REPO_ROOT": t}
    ps = run or (lambda args, extra_env=None, password=None: runner.purrstrap(args, password=password, on_line=on_line, extra_env=extra_env))
    out = out or os.path.join(ws.builds, "firmware-%s" % board)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)

    def step(i, state_, note=""):
        if on_step:
            on_step(i, state_, note)

    def fail(i, note):
        step(i, "fail", note)
        on_line(note)
        return {"ok": False, "problem": note, "out": out, "version": version}

    # 1 compile the three profiles
    bins = {}
    for profile, what in PROFILES:
        sid = "compile-" + profile
        step(sid, "run", "%s, from %s" % (what, st["commit"][:7]))
        args = ["coreos", "build", "--board", board, "--profile", profile, "--shell", shell]
        if clean:
            args.append("--clean")
        r = ps(args, extra_env=env)
        if r["rc"] != 0:
            return fail(sid, "The %s did not compile. The lines above say why." % what)
        b = os.path.join(build_dir(ws, board, profile, shell), "purros.bin")
        if not os.path.isfile(b):
            return fail(sid, "The compile finished but %s is not there." % b)
        bins[profile] = b
        step(sid, "done", "%s compiled" % what)

    # 2 package
    step("package", "run", version)
    full = bins["full"]
    parts = {"kernel": ("coreos", ["package-component", "--component", "kernel", "--file", full, "--version", version, "--board", board], "kernel-%s.cat" % board),
             "loader": ("coreos", ["package-component", "--component", "loader", "--file", bins["minimal"], "--version", version, "--board", board], "loader-%s.cat" % board),
             "bootloader": ("coreos", ["package-component", "--component", "bootloader", "--file", os.path.join(os.path.dirname(os.path.dirname(full)), "%s-%s%s" % (board, "full", "" if shell == "w10m" else "-" + shell), "bootloader", "bootloader.bin"),
                                       "--version", version, "--board", board], "bootloader-%s.cat" % board)}
    made = []
    for name, (_s, args, fn) in parts.items():
        dest = os.path.join(out, fn)
        r = ps(["coreos"] + args[:] + ["--out", dest])
        if r["rc"] != 0:
            return fail("package", "Could not package the %s: %s" % (name, (r["output"].strip().splitlines() or ["no reason given"])[-1]))
        made.append(dest)
    dest = os.path.join(out, "kittenos-%s.kitt" % board)
    r = ps(["coreos", "package", "--board", board, "--profile", "recovery", "--version", version, "--out", dest, "--shell", shell], extra_env=env)
    if r["rc"] != 0:
        return fail("package", "Could not package KittenOS: %s" % (r["output"].strip().splitlines() or ["no reason given"])[-1])
    made.append(dest)
    bdir = os.path.join(out, ".bootpkg")
    r = ps(["bootpkg", "build", "--board", board, "--out", bdir], extra_env=env)
    if r["rc"] != 0:
        return fail("package", "The boot package did not build. The lines above say why.")
    dest = os.path.join(out, "bootpkg-%s.cat" % board)
    shutil.copyfile(os.path.join(bdir, "bootpkg.bin"), dest)
    made.append(dest)
    step("package", "done", "%d parts" % len(made))

    # 3 sign
    step("sign", "run", "key id %s" % key_id)
    for p in made:
        r = ps(["keys", "sign", "--key", key, "--image", p, "--key-id", str(int(key_id))], password=password)
        if r["rc"] != 0:
            return fail("sign", "Signing %s failed: %s" % (os.path.basename(p), (r["output"].strip().splitlines() or ["no reason given"])[-1]))
    step("sign", "done", "%d parts signed" % len(made))
    return {"ok": True, "out": out, "version": version, "parts": [os.path.basename(p) for p in made]}
