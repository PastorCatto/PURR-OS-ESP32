"""Build the system from the source instead of from ready-made parts: pull the newest PURR OS source, compile the firmware with ESP-IDF, then package and sign
each part with your own key (the "compile from source" road of Studio's build-a-system screens).

    pull      fetch the source (only the folders a firmware build needs) into <workspace>/source/tree, with git if there is git, otherwise as a GitHub tarball
    compiler  find ESP-IDF 5.3.5 (or install it once, with git)
    compile   purrstrap `coreos build` for the three profiles of the board: full = the kernel, minimal = the recovery loader, recovery = KittenOS
    package   wrap each binary in a PURR container (`coreos package`, `package-component`, `bootpkg build`) and sign it with your key

The result is a folder of parts named the way the image maker expects (`kernel-<board>.cat` ...), so the rest of the road (image, delivery) is the same as with
ready-made parts. Nothing here touches your own checkout of the repository: the source lives in the workspace.
"""

import hashlib
import io
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import time
import urllib.request

from . import formats, runner

DEFAULT_URL = "https://github.com/PastorCatto/PURR-OS-ESP32"
DEFAULT_REF = "main"
PATHS = ("PurrOS", "UI", "purrstrap", "bootloader", "bootpkg", "Modules", "Apps", "tools/release")      # what a build reads from the repository
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


def path_warning(ws, windows=None):
    """On Windows the pulled source and ESP-IDF's build folders sit under the workspace, and a long path runs into the 260-character limit: a sentence saying so, or ''."""
    windows = (os.name == "nt") if windows is None else windows
    if windows and len(ws.path) > 24:
        return ("The workspace folder %s is long. ESP-IDF's build folders go several levels deeper and Windows stops at 260 characters, so a compile can fail with a missing file. "
                "Use a short workspace such as C:\\p (set PURR_WORKSPACE, or pass --workspace) and turn on long paths (git config --system core.longpaths true)." % ws.path)
    return ""


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


def _why(output):
    """The line of a failed git command that says why: the first `fatal:` or `error:` line, or the last line when there is none."""
    lines = [l.strip() for l in output.splitlines() if l.strip()]
    for l in lines:
        if l.lower().startswith(("fatal:", "error:")):
            return l.split(":", 1)[1].strip()
    return lines[-1] if lines else "no reason given"


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
            raise SourceError("I could not reach %s: %s" % (url, _why(out)))
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


def _rmtree(path):
    """Remove a folder, also when git has made its files read-only (Windows refuses to delete those; shutil.rmtree(ignore_errors=True) then silently leaves them behind)."""
    def clear(func, p, _exc):
        try:
            os.chmod(os.path.dirname(p), stat.S_IRWXU)      # (the same on other systems: a folder one may not write to)
            os.chmod(p, stat.S_IWRITE | stat.S_IRWXU)
            func(p)
        except OSError:
            pass
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=clear)
    else:
        shutil.rmtree(path, onerror=clear)


def pull(ws, url=DEFAULT_URL, ref=DEFAULT_REF, on_line=print):
    """Fetch the newest source. Returns {"commit", "changed"}. The tree is replaced as a whole, so a half-finished pull never leaves a mixed one."""
    latest = remote_head(url, ref)
    st = state(ws)
    if st.get("commit") == latest and st.get("url") == url and st.get("ref") == ref and os.path.isdir(os.path.join(tree(ws), "PurrOS")):
        on_line("The source is already the newest (%s)." % latest[:10])
        return {"commit": latest, "changed": False}
    os.makedirs(folder(ws), exist_ok=True)
    tmp = os.path.join(folder(ws), "tree.new")
    _rmtree(tmp)
    on_line("Fetching %s (%s) %s..." % (url, ref, "with git" if have_git() else "as a tarball"))
    try:
        if have_git():
            _fetch_git(url, ref, tmp, on_line)
        else:
            _fetch_tarball(url, ref, tmp, on_line)
    except SourceError:
        _rmtree(tmp)
        raise
    if not os.path.isdir(os.path.join(tmp, "PurrOS")):
        _rmtree(tmp)
        raise SourceError("%s (%s) has no PurrOS folder: this is not the PURR OS source." % (url, ref))
    # keep the previous build folders: they are what makes the next compile a short one
    old = tree(ws)
    for sub in ("PurrOS/build",):
        if os.path.isdir(os.path.join(old, sub)):
            os.makedirs(os.path.dirname(os.path.join(tmp, sub)), exist_ok=True)
            _rmtree(os.path.join(tmp, sub))
            shutil.move(os.path.join(old, sub), os.path.join(tmp, sub))
    _rmtree(old)
    if os.path.isdir(old):
        raise SourceError("I could not remove the old source folder %s (something has files in it open). Close what uses it and try again; the new copy is waiting in %s." % (old, tmp))
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
        _rmtree(dest)
        rc, out = _git(["clone", "--depth", "1", "--branch", ref, url, dest], on_line=on_line)
        if rc != 0:
            raise SourceError("git could not fetch the source: %s" % _why(out))
    _rmtree(os.path.join(dest, ".git"))


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
            if len(parts) < 2 or not any(parts[1] == q or parts[1].startswith(q + "/") for q in PATHS):
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
            raise SourceError("git could not fetch ESP-IDF: %s" % _why(out))
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

DEFAULT_VERSION = "1.1.0"
_VERSION = re.compile(r"^\d{1,5}(\.\d{1,5}){0,2}$")


def version_problem(base):
    """Why `base` cannot be a version the device reads (major.minor.patch, numbers only), or ''."""
    if not _VERSION.match(base or ""):
        return "The version is numbers separated by dots, like 1.1.0: the device compares it to decide what is newer."
    if len(base) > 10:
        return "The version is too long (a container header holds 12 characters, and the commit's letters follow it)."
    return ""


def short_version(commit, base=DEFAULT_VERSION):
    """The version a part built from source carries: your version, which the device compares, then a plus and as many letters of the commit as fit in the 12
    characters of a container header (1.1.0+23e74). The device reads the numbers before the plus; the letters only say which source it was."""
    room = 12 - len(base) - 1
    return base + ("+" + commit[:room] if room > 0 and commit else "")


def build_dir(ws, board, profile, shell):
    return os.path.join(tree(ws), "PurrOS", "build", "%s-%s%s" % (board, profile, "" if shell == "w10m" else "-" + shell))


def compile_and_package(ws, board, shell="android", key=None, key_id=4, password=None, out=None, on_step=None, on_line=print, run=None, clean=False, base=DEFAULT_VERSION):
    """Compile the firmware of `board` from the pulled source and put parts in `out`, signed with `key` (the boot key: it signs everything below the OS). Returns {"ok", "out", "version", "parts": [names], "problem"}.

    `run(args, extra_env=None, password=None)` runs purrstrap and returns {"rc", "output"}; it is a parameter so tests can stand in for the compiler."""
    st = state(ws)
    if not st.get("commit"):
        return {"ok": False, "problem": "Pull the source first."}
    if board not in BOARDS_CHIP:
        return {"ok": False, "problem": "I cannot build firmware for the board %s yet." % board}
    if shell not in SHELLS:
        return {"ok": False, "problem": "The shell is one of %s." % ", ".join(SHELLS)}
    if not key:
        return {"ok": False, "problem": "Choose a boot key: the parts you compile have to be signed."}
    if version_problem(base):
        return {"ok": False, "problem": version_problem(base)}
    version = short_version(st["commit"], base)
    t = tree(ws)
    env = {"PURR_REPO_ROOT": t}
    ps = run or (lambda args, extra_env=None, password=None: runner.purrstrap(args, password=password, on_line=on_line, extra_env=extra_env))
    out = out or os.path.join(ws.builds, "firmware-%s" % board)
    _rmtree(out)
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
            return fail(sid, "Compiling %s failed: %s" % (what, ([l.strip() for l in r["output"].splitlines() if l.strip()] or ["the lines above say why"])[-1]))
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


# ---------------------------------------------------------------------------------------------------------------- trust: whose keys the image believes

OUR_IDS = (1, 2, 3, 4, 5)           # PURR's own keys, compiled into every build unless you take them out
KEYS_C = os.path.join("PurrOS", "components", "coreos", "keys", "purr_default_keys.c")
_ENTRY = re.compile(r"\.key_id\s*=\s*(\d+),\s*\.role\s*=\s*PURR_ROLE_(\w+),\s*\.pub\s*=\s*\{([^}]*)\}")


def trust_problems(keep_ours, boot_key, boot_id, dev_key, dev_id):
    """What is wrong with the choice of keys, as readable sentences (empty when it is fine)."""
    out = []
    if not boot_key:
        out.append("Choose a boot key: it signs the kernel, the loader, KittenOS, the boot package and the bootloader.")
    if not dev_key:
        out.append("Choose a developer key: it signs the modules, drivers and apps.")
    if boot_key and dev_key and boot_id == dev_id:
        out.append("The boot key and the developer key need different ids.")
    if keep_ours:
        for what, i in (("boot", boot_id), ("developer", dev_id)):
            if i in OUR_IDS:
                out.append("The %s key's id %d is one of PURR's (1 to 5). While you keep PURR's keys, give yours an id from 6 to 31." % (what, i))
    for what, i in (("boot", boot_id), ("developer", dev_id)):
        if not 0 <= i <= 31:
            out.append("The %s key's id must be from 0 to 31." % what)
    return out


def pub_path(key):
    return os.path.splitext(key)[0] + ".pub"


def read_keybag(text):
    """[(id, role, 64 bytes)] from a generated purr_default_keys.c."""
    out = []
    for m in _ENTRY.finditer(text):
        raw = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(3)))
        if len(raw) == 64:
            out.append((int(m.group(1)), m.group(2).lower(), raw))
    return out


def apply_trust(ws, keep_ours, own, run=None, on_line=print):
    """Write the key list the build compiles in: PURR's keys (when kept) and yours. `own` is [(public key file, role, id)]. The bootloader, the kernel and
    the boot package all read this one list, so what is trusted is decided here. Returns the ids in the list."""
    c = os.path.join(tree(ws), KEYS_C)
    ours = c + ".ours"
    if not os.path.isfile(c):
        raise SourceError("The source has no key list (%s): it is not the PURR OS source." % KEYS_C)
    if not os.path.isfile(ours):
        shutil.copyfile(c, ours)                                # the list as pulled; kept so the choice can change later
    ps = run or (lambda args, extra_env=None, password=None: runner.purrstrap(args, on_line=on_line, extra_env=extra_env))
    tmp = os.path.join(folder(ws), "keys.work")
    _rmtree(tmp)
    os.makedirs(tmp)
    if keep_ours:
        with open(ours, "r", encoding="utf-8") as fh:
            for kid, role, raw in read_keybag(fh.read()):
                with open(os.path.join(tmp, "%s_%d.pub.bin" % (role, kid)), "wb") as out:
                    out.write(raw)
    for pub, role, kid in own:
        if not os.path.isfile(pub):
            raise SourceError("I cannot find the public key %s (make the key in Studio's keys page, which writes it next to the private one)." % pub)
        r = ps(["keys", "export-public", "--pub", pub, "--role", role, "--key-id", str(kid), "--out", tmp])
        if r["rc"] != 0:
            raise SourceError("Could not add your %s key to the build: %s" % (role, (r["output"].strip().splitlines() or ["no reason given"])[-1]))
    gen = os.path.join(tmp, "purr_default_keys.c")
    if not os.path.isfile(gen):
        raise SourceError("The key list was not written.")
    shutil.copyfile(gen, c)
    _rmtree(tmp)
    with open(c, "r", encoding="utf-8") as fh:
        return [k[0] for k in read_keybag(fh.read())]


# ---------------------------------------------------------------------------------------------------------------- the modules, drivers and apps

# the system modules: name, source in the repository, entry symbol (all are built as driver-kind modules, as the published ones are)
SYSTEM_MODULES = (("about", "Modules/test/about_module.c", "about_module_entry"), ("appmgr", "Modules/coreos/appmgr_module.c", "appmgr_module_entry"),
                  ("apps", "Modules/appmanager/apps_module.c", "apps_module_entry"), ("fs", "Modules/coreos/fs_module.c", "fs_module_entry"),
                  ("login", "Modules/coreos/login_module.c", "login_module_entry"), ("netinstall", "Modules/netinstall/netinstall_module.c", "netinstall_module_entry"),
                  ("sysinfo", "Modules/coreos/sysinfo_module.c", "sysinfo_module_entry"), ("wifi", "Modules/network/wifi_module.c", "wifi_module_entry"))


def driver_source(ws, name):
    """(source file, entry symbol) of a driver in the pulled source, or None. The file is Modules/drivers/<stem>_module.c and the driver is called <stem> with dashes, or that and a suffix."""
    d = os.path.join(tree(ws), "Modules", "drivers")
    if not os.path.isdir(d):
        return None
    best = None
    for f in sorted(os.listdir(d)):
        if not f.endswith("_module.c"):
            continue
        stem = f[:-len("_module.c")]
        dashed = stem.replace("_", "-")
        if name == dashed or name.startswith(dashed + "-"):
            if best is None or len(stem) > len(best[0]):
                best = (stem, os.path.join(d, f))
    return (best[1], best[0] + "_entry") if best else None


def tree_apps(ws):
    """[(name, version)] of the apps in the pulled source (Apps/<name>/<name>.c with a manifest.toml)."""
    d = os.path.join(tree(ws), "Apps")
    out = []
    if os.path.isdir(d):
        for n in sorted(os.listdir(d)):
            m = os.path.join(d, n, "manifest.toml")
            if os.path.isfile(os.path.join(d, n, n + ".c")) and os.path.isfile(m):
                mv = re.search(r'^version\s*=\s*"([^"]+)"', open(m, encoding="utf-8").read(), re.M)
                out.append((n, mv.group(1) if mv else "0.1.0"))
    return out


def compile_modules(ws, board, key, key_id, password, drivers, apps, out, on_step=None, on_line=print, run=None, base=DEFAULT_VERSION):
    """Compile the system modules, the chosen drivers and the chosen apps of the pulled source, and sign each with `key` (the developer key).

    `drivers` is [{"name", "scope"}], `apps` is [name of an app in the pulled source]. Returns {"ok", "modules": [paths], "drivers": [{name, scope, version, path, file}],
    "apps": [paths], "missing": [driver names with no source], "problem"}."""
    st = state(ws)
    version = short_version(st.get("commit", "0000000"), base)
    env = {"PURR_REPO_ROOT": tree(ws)}
    ps = run or (lambda args, extra_env=None, password=None: runner.purrstrap(args, password=password, on_line=on_line, extra_env=extra_env))
    res = {"ok": False, "modules": [], "drivers": [], "apps": [], "missing": [], "problem": ""}
    os.makedirs(out, exist_ok=True)

    def step(i, state_, note=""):
        if on_step:
            on_step(i, state_, note)

    def build(name, src, entry, kind, dest, ver):
        r = ps(["modules", "build", "--board", board, "--source", src, "--entry", entry, "--kind", kind, "--name", name, "--version", ver, "--out", dest], extra_env=env)
        if r["rc"] != 0:
            return "%s did not compile: %s" % (name, (r["output"].strip().splitlines() or ["no reason given"])[-1])
        r = ps(["keys", "sign", "--key", key, "--image", dest, "--key-id", str(int(key_id))], password=password)
        if r["rc"] != 0:
            return "Signing %s failed: %s" % (name, (r["output"].strip().splitlines() or ["no reason given"])[-1])
        return ""

    t = tree(ws)
    step("modules", "run", "%d system modules" % len(SYSTEM_MODULES))
    for name, rel, entry in SYSTEM_MODULES:
        src = os.path.join(t, *rel.split("/"))
        if not os.path.isfile(src):
            res["problem"] = "The source of the %s module (%s) is not in the pulled source." % (name, rel)
            step("modules", "fail", res["problem"])
            return res
        dest = os.path.join(out, "%s-%s.cat" % (name, board))
        bad = build(name, src, entry, "driver", dest, version)
        if bad:
            res["problem"] = bad
            step("modules", "fail", bad)
            return res
        res["modules"].append(dest)
    step("modules", "done", "%d compiled and signed" % len(res["modules"]))

    step("drivers", "run", "%d driver(s)" % len(drivers))
    for d in drivers:
        found = driver_source(ws, d["name"])
        if not found:
            res["missing"].append(d["name"])
            on_line("There is no source for the driver %s in this source: it is left out (nothing ready-made is used in a build from source)." % d["name"])
            continue
        dest = os.path.join(out, "%s-%s.cat" % (d["name"], d["scope"]))
        bad = build(d["name"], found[0], found[1], "driver", dest, version)
        if bad:
            res["problem"] = bad
            step("drivers", "fail", bad)
            return res
        data = open(dest, "rb").read()
        res["drivers"].append({"name": d["name"], "scope": d["scope"], "version": version, "path": dest, "file": os.path.basename(dest), "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    step("drivers", "done" if res["drivers"] else "skip", "%d compiled and signed%s" % (len(res["drivers"]), (", %d without source" % len(res["missing"])) if res["missing"] else ""))

    avail = dict(tree_apps(ws))
    step("srcapps", "run" if apps else "skip", "%d app(s) from the source" % len(apps) if apps else "none chosen")
    for a in apps:
        if a not in avail:
            res["problem"] = "The app %s is not in the pulled source." % a
            step("srcapps", "fail", res["problem"])
            return res
        dest = os.path.join(out, "%s.cat" % a)
        bad = build(a, os.path.join(t, "Apps", a, a + ".c"), a + "_entry", "app", dest, avail[a])
        if bad:
            res["problem"] = bad
            step("srcapps", "fail", bad)
            return res
        res["apps"].append(dest)
    if apps:
        step("srcapps", "done", "%d compiled and signed" % len(res["apps"]))
    res["ok"] = True
    return res


# ---------------------------------------------------------------------------------------------------------------- binaries baked into the kernel

# PurrOS/main/commands.c holds signed binaries as byte arrays (signed by PURR's developer key): the loader's display, keyboard and touch drivers come from them, and the
# kernel plants the system modules from them. A build that trusts nobody's binaries compiles each one again from source and writes its own signed copy into the array.
# array in commands.c -> (source, entry symbol)
BAKED = (("s_about_module", "Modules/test/about_module.c", "about_module_entry"), ("s_apps_module", "Modules/appmanager/apps_module.c", "apps_module_entry"),
         ("s_wifi_module", "Modules/network/wifi_module.c", "wifi_module_entry"), ("s_netinstall_module", "Modules/netinstall/netinstall_module.c", "netinstall_module_entry"),
         ("s_sysinfo_module", "Modules/coreos/sysinfo_module.c", "sysinfo_module_entry"), ("s_fs_module", "Modules/coreos/fs_module.c", "fs_module_entry"),
         ("s_login_module", "Modules/coreos/login_module.c", "login_module_entry"), ("s_appmgr_module", "Modules/coreos/appmgr_module.c", "appmgr_module_entry"),
         ("s_debug_module", "Modules/test/debug_module.c", "debug_module_entry"),
         ("s_kbd_i2c_driver_module", "Modules/drivers/kbd_i2c_module.c", "kbd_i2c_entry"), ("s_gt911_driver_module", "Modules/drivers/gt911_module.c", "gt911_entry"),
         ("s_st7789_driver_module", "Modules/drivers/st7789_module.c", "st7789_entry"), ("s_battery_driver_module", "Modules/drivers/battery_module.c", "battery_entry"),
         ("s_kbd_backlight_driver_module", "Modules/drivers/kbd_backlight_module.c", "kbd_backlight_entry"))
BUILTIN_APPS = ("store", "storesvc")                    # purr_builtin_apps.c: the Store and its companion (unsigned: the kernel's hash vouches for them)
KIND_OF_FLAGS = {2: "coreos", 3: "appmanager", 4: "runtime", 5: "driver", 7: "devbundle"}
COMMANDS_C = os.path.join("PurrOS", "main", "commands.c")


def _array_pattern(name):
    return re.compile(r"(static const uint8_t %s\[\] = \{)(.*?)(\n\};)" % re.escape(name), re.S)


def _array_bytes(text, name):
    m = _array_pattern(name).search(text)
    if not m:
        return None
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(2)))


def _format_array(data):
    return "\n" + "\n".join("    " + " ".join("0x%02x," % b for b in data[i:i + 12]) for i in range(0, len(data), 12))


def rebake(ws, board, key, key_id, password, on_step=None, on_line=print, run=None, gen=None, base=DEFAULT_VERSION):
    """Compile again from source every binary baked into the kernel and sign it with `key` (the developer key), writing the result into the pulled tree: the arrays of
    commands.c, and purr_builtin_apps.c (the Store). Returns {"ok", "count", "problem"}. `gen(args)` runs the generator script (a parameter for tests)."""
    t = tree(ws)
    st = state(ws)
    version = short_version(st.get("commit", "0000000"), base)
    env = {"PURR_REPO_ROOT": t}
    ps = run or (lambda args, extra_env=None, password=None: runner.purrstrap(args, password=password, on_line=on_line, extra_env=extra_env))
    img = formats.image()
    work = os.path.join(folder(ws), "baked.work")
    _rmtree(work)
    os.makedirs(work)

    def step(i, state_, note=""):
        if on_step:
            on_step(i, state_, note)

    def fail(i, note):
        step(i, "fail", note)
        on_line(note)
        return {"ok": False, "count": 0, "problem": note}

    cpath = os.path.join(t, COMMANDS_C)
    orig = cpath + ".orig"
    if not os.path.isfile(cpath):
        return fail("baked", "The source has no %s: it is not the PURR OS source." % COMMANDS_C)
    if not os.path.isfile(orig):
        shutil.copyfile(cpath, orig)                            # as pulled, so the arrays can always be read from the originals
    with open(orig, "r", encoding="utf-8") as fh:
        text = fh.read()
    step("baked", "run", "%d modules and drivers built into the kernel" % len(BAKED))
    n = 0
    for array, rel, entry in BAKED:
        old = _array_bytes(text, array)
        src = os.path.join(t, *rel.split("/"))
        if old is None or not os.path.isfile(src):
            return fail("baked", "I cannot rebuild %s: %s." % (array, "it is not in commands.c" if old is None else "its source %s is missing" % rel))
        try:
            f = img.header_fields(img.parse_header(old))
        except Exception:
            return fail("baked", "%s in commands.c is not a PURR container." % array)
        name = f["name"]
        kind = KIND_OF_FLAGS.get(f["flags"], "driver")
        dest = os.path.join(work, name + ".cat")
        r = ps(["modules", "build", "--board", board, "--source", src, "--entry", entry, "--kind", kind, "--name", name, "--version", version, "--out", dest], extra_env=env)
        if r["rc"] != 0:
            return fail("baked", "%s did not compile: %s" % (name, (r["output"].strip().splitlines() or ["no reason given"])[-1]))
        r = ps(["keys", "sign", "--key", key, "--image", dest, "--key-id", str(int(key_id))], password=password)
        if r["rc"] != 0:
            return fail("baked", "Signing %s failed: %s" % (name, (r["output"].strip().splitlines() or ["no reason given"])[-1]))
        with open(dest, "rb") as fh:
            new = fh.read()
        text = _array_pattern(array).sub(lambda m: m.group(1) + _format_array(new) + m.group(3), text, count=1)
        n += 1
    with open(cpath, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    step("baked", "done", "%d compiled and signed" % n)

    step("builtin", "run", "the Store and its companion")
    cats = []
    avail = dict(tree_apps(ws))
    for a in BUILTIN_APPS:
        if a not in avail:
            return fail("builtin", "The app %s is not in the pulled source." % a)
        dest = os.path.join(work, a + ".cat")
        r = ps(["modules", "build", "--board", board, "--source", os.path.join(t, "Apps", a, a + ".c"), "--entry", a + "_entry", "--kind", "app", "--name", a,
                "--version", avail[a], "--out", dest], extra_env=env)
        if r["rc"] != 0:
            return fail("builtin", "%s did not compile: %s" % (a, (r["output"].strip().splitlines() or ["no reason given"])[-1]))
        cats.append(dest)
    script = os.path.join(t, "tools", "release", "gen_builtin_apps.py")
    if not os.path.isfile(script):
        return fail("builtin", "The source has no tools/release/gen_builtin_apps.py.")
    g = gen or (lambda args: _run_python(args, t, on_line))
    r = g([script] + cats)
    if r["rc"] != 0:
        return fail("builtin", "Writing the built-in apps failed: %s" % (r["output"].strip().splitlines() or ["no reason given"])[-1])
    step("builtin", "done", "%d apps compiled" % len(cats))
    _rmtree(work)
    return {"ok": True, "count": n + len(cats), "problem": ""}


def _run_python(args, cwd, on_line):
    try:
        p = subprocess.run([sys.executable] + list(args), cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, errors="replace", stdin=subprocess.DEVNULL)
    except OSError as e:
        return {"rc": 127, "output": str(e)}
    for line in p.stdout.splitlines():
        on_line(line)
    return {"rc": p.returncode, "output": p.stdout}
