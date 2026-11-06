"""The developer side of the app repository (plan8.md, Repo/SPEC.md): serve a folder of built apps to a device
on the local network, so an app goes from the build to the device with one command and a tap.

    dev publish --file clock.cat --dir ./repo     put a built app in the repository folder
    dev index   --dir ./repo                      (re)write purr/v1/index.json
    dev list    --dir ./repo                      show what the index would offer
    dev serve   --dir ./repo                      serve it (and answer the device's discovery broadcast)
    dev recovery --dir ./repo --kernel k.cat ...  put recovery images in the folder and write the recovery manifests
    dev find                                      listen for servers on this network (what a device on the same AP sees)

The same server can host the RECOVERY images (kernel, boot package, loader, KittenOS, the module index and legal.txt) under
/recovery/. A device whose bootloader allowed it (the DEV_RECOVERY flag, development mode only) offers this server on an extra
setup page, checks it, and pulls the install from it instead of the internet; the images are still verified against the boot
key before anything is written.

The repository folder holds   purr/v1/apps/<name>/<version>/package.cat   (and an optional meta.json with a
title and description). The index is rebuilt from the folder on every request, so a publish shows up on the
device's next refresh with no restart. The format is the one the device's repository client reads
(PurrOS/components/coreos/include/purr_repo.h); `kind` is "sdk", which the device only contacts on the local
network and only installs from while developer mode is active.

Standard library only.
"""

import hashlib
import http.server
import json
import os
import re
import shutil
import socket
import socketserver
import subprocess
import sys
import threading
import time

from lib import image as pimg
from lib.model import Action, Param, Script

FORMAT = 1
DISCOVERY_PORT = 47800
DEFAULT_PORT = 47801
NAME_RE = re.compile(r"^[A-Za-z0-9_-]{1,31}$")
VERSION_RE = re.compile(r"^[0-9][A-Za-z0-9.+-]{0,14}$")
CHIPS = {pimg.CHIP_ANY: "any", pimg.CHIP_ESP32: "esp32", pimg.CHIP_ESP32S3: "esp32s3"}
MAX_PACKAGE = 512 * 1024
RECOVERY_DIR = "recovery"
RECOVERY_VM_DIR = "recovery-vm"                  # the same files built for the emulator (the VM flavor's recovery URL points here)
RECOVERY_COMPONENTS = ("kernel", "kittenos", "loader", "bootpkg", "bootloader", "uiassets")      # the files the loader and KittenOS fetch by name
RECOVERY_FILE_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,79}$")    # a plain file name: no slash, so no way out of the folder


def _apps_dir(repo):
    return os.path.join(repo, "purr", "v1", "apps")


def _version_key(v):
    parts = re.match(r"v?([0-9]+(?:\.[0-9]+)*)", v)
    nums = tuple(int(x) for x in parts.group(1).split(".")) if parts else (0,)
    return nums + (0,) * (6 - len(nums)), 0 if "-" in v else 1


def _read_app(path):
    """The index fields from a .cat file, or (None, reason)."""
    with open(path, "rb") as fh:
        data = fh.read()
    if not pimg.is_purr_image(data) or len(data) < pimg.HEADER_SIZE:
        return None, "not a PURR image"
    f = pimg.header_fields(pimg.parse_header(data))
    if f["image_type"] != pimg.IMG_APP:
        return None, "not an app (image type %d)" % f["image_type"]
    if not NAME_RE.match(f["name"]):
        return None, "bad app name %r" % f["name"]
    if not VERSION_RE.match(f["version"]):
        return None, "bad version %r" % f["version"]
    if len(data) > MAX_PACKAGE:
        return None, "larger than the device's %d byte limit" % MAX_PACKAGE
    signed = f["signature"] != b"\0" * 64
    payload = data[f["payload_offset"]:f["payload_offset"] + f["payload_size"]]
    corrupt = len(payload) != f["payload_size"] or hashlib.sha256(payload).digest() != f["payload_sha256"]
    return {
        "corrupt": corrupt,
        "name": f["name"], "version": f["version"], "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "chip": CHIPS.get(f["chip_id"], "any"),
        "signed": signed, "key_id": f["key_id"] if signed else 0,
    }, ""


def build_index(repo, name, repo_id):
    """The index for a repository folder: the newest version of each app."""
    best = {}
    root = _apps_dir(repo)
    if os.path.isdir(root):
        for app in sorted(os.listdir(root)):
            for ver in sorted(os.listdir(os.path.join(root, app))):
                pkg = os.path.join(root, app, ver, "package.cat")
                if not os.path.isfile(pkg):
                    continue
                entry, why = _read_app(pkg)
                if entry is None or entry["name"] != app or entry["version"] != ver:
                    continue
                meta = {}
                try:
                    with open(os.path.join(root, app, ver, "meta.json"), encoding="utf-8") as fh:
                        meta = json.load(fh)
                except (OSError, ValueError):
                    pass
                entry.pop("corrupt", None)
                entry["title"] = str(meta.get("title") or app)[:31]
                entry["desc"] = str(meta.get("desc") or "")[:79]
                entry["url"] = "apps/%s/%s/package.cat" % (app, ver)
                entry["updated"] = int(os.path.getmtime(pkg))
                if app not in best or _version_key(ver) > _version_key(best[app]["version"]):
                    best[app] = entry
    return {"format": FORMAT, "repo": {"id": repo_id, "name": name, "kind": "sdk"},
            "apps": [best[k] for k in sorted(best)]}


def _repo_id(repo):
    seed = "%s|%s" % (socket.gethostname(), os.path.abspath(repo))
    return hashlib.sha256(seed.encode()).hexdigest()[:12]


def _name(name):
    return (name or socket.gethostname().split(".")[0] or "purr-sdk")[:31]


# -- actions ------------------------------------------------------------------
def publish(ctx, file, dir, title, desc, force):
    entry, why = _read_app(file)
    if entry is None:
        ctx.error("%s: %s" % (file, why))
        return 1
    if entry["corrupt"] and not force:
        ctx.error("%s: the payload does not match its hash (a damaged or edited package); "
                  "use --force to publish it anyway, for testing the device's refusal" % file)
        return 1
    if entry["corrupt"]:
        ctx.warn("publishing a CORRUPT package (--force)")
    dest = os.path.join(_apps_dir(dir), entry["name"], entry["version"])
    os.makedirs(dest, exist_ok=True)
    shutil.copyfile(file, os.path.join(dest, "package.cat"))
    meta = {}
    if title:
        meta["title"] = title
    if desc:
        meta["desc"] = desc
    if meta:
        with open(os.path.join(dest, "meta.json"), "w", encoding="utf-8") as fh:
            json.dump(meta, fh)
    state = "signed (key %d)" % entry["key_id"] if entry["signed"] else "UNSIGNED"
    ctx.ok("published %s %s, %d bytes, %s" % (entry["name"], entry["version"], entry["size"], state))
    if not entry["signed"]:
        ctx.info("    the device only installs unsigned apps with developer mode on and the switch enabled")
    return index(ctx, dir, "", "")


def index(ctx, dir, name, out):
    idx = build_index(dir, _name(name), _repo_id(dir))
    path = out or os.path.join(dir, "purr", "v1", "index.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(idx, fh, indent=1)
    ctx.ok("%s: %d app(s)" % (path, len(idx["apps"])))
    return 0


def list_apps(ctx, dir):
    idx = build_index(dir, _name(""), _repo_id(dir))
    if not idx["apps"]:
        ctx.info("no apps in %s (publish one with: dev publish --file app.cat --dir %s)" % (dir, dir))
        return 0
    for a in idx["apps"]:
        ctx.info("%-16s %-10s %7d bytes  %s" % (a["name"], a["version"], a["size"],
                                               "signed key %d" % a["key_id"] if a["signed"] else "UNSIGNED"))
    return 0


def _recovery_dir(repo):
    return os.path.join(repo, RECOVERY_DIR)


def _safe_recovery_name(name):
    return bool(RECOVERY_FILE_RE.match(name)) and ".." not in name


def _atomic_write(path, data):
    """Writes a file so a reader sees the old one or the new one, never half of either (a temporary name, then a rename)."""
    tmp = "%s.tmp-%d" % (path, os.getpid())
    with open(tmp, "wb") as fh:
        fh.write(data)
    os.replace(tmp, path)


def _atomic_copy(src, dst):
    with open(src, "rb") as fh:
        _atomic_write(dst, fh.read())


KEEP_GENERATIONS = 3                       # immutable copies of each image kept (the newest ones), so a device in the middle of an OTA can finish


def _immutable_name(component, board, sha, ext):
    """The name an image is published under: it holds the hash, so the same name is always the same bytes. A manifest that names it can never be
    contradicted by a later publish (2026-11-06: files replaced in place under fixed names made an OTA that overlapped a publish fail its size and hash check)."""
    return "%s-%s.%s.%s" % (component, board, sha[:10], ext)


def _prune_immutable(rdir, keep):
    """Removes immutable copies beyond the newest KEEP_GENERATIONS of each image; `keep` (names the current manifests use) are never removed."""
    groups = {}
    for fn in os.listdir(rdir):
        m = re.match(r"^([A-Za-z0-9_]+-[A-Za-z0-9_]+)\.[0-9a-f]{10}\.(cat|kitt)$", fn)
        if m:
            groups.setdefault(m.group(1), []).append(fn)
    for names in groups.values():
        names.sort(key=lambda n: os.path.getmtime(os.path.join(rdir, n)), reverse=True)
        for fn in names[KEEP_GENERATIONS:]:
            if fn not in keep:
                os.remove(os.path.join(rdir, fn))


def _sha256_file(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


WITHDRAWN_LIST = "withdrawn.list"          # recovery/withdrawn.list: "reason=<one line>", then "<component>@<version>" per line ("*" is every component)


def read_withdrawn(rdir):
    """(reason, [token, ...]) from recovery/withdrawn.list; ("", []) if there is none."""
    path = os.path.join(rdir, WITHDRAWN_LIST)
    reason, tokens = "", []
    if os.path.exists(path):
        for line in open(path, encoding="utf-8").read().splitlines():
            line = line.strip()
            if line.startswith("reason="):
                reason = line[len("reason="):].strip()
            elif "@" in line and not line.startswith("#"):
                tokens.append(line)
    return reason, tokens


def _write_withdrawn(rdir, reason, tokens):
    path = os.path.join(rdir, WITHDRAWN_LIST)
    if not tokens:
        if os.path.exists(path):
            os.remove(path)
        return
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("reason=%s\n%s\n" % (reason, "\n".join(tokens)))


def _is_withdrawn(tokens, component, version):
    return ("%s@%s" % (component, version) in tokens) or ("*@%s" % version in tokens)


def _withdrawn_lines(rdir):
    """The release-wide lines for a manifest header (purr_manifest.h: withdrawn_versions, withdrawn_reason): [] when nothing is withdrawn."""
    reason, tokens = read_withdrawn(rdir)
    if not tokens:
        return []
    return ["withdrawn_versions=%s" % ",".join(tokens), "withdrawn_reason=%s" % reason]


def build_recovery_manifests(repo):
    """Writes recovery/recovery.manifest (the components) and recovery/modules.manifest (everything else that is a PURR image)
    from the images in the folder. Files are named <component>-<board>.<ext>; the format is purr_manifest.h's. Returns
    (components, modules) as lists of the entries written."""
    rdir = _recovery_dir(repo)
    comps, mods = [], []
    if not os.path.isdir(rdir):
        return comps, mods
    for fn in sorted(os.listdir(rdir)):
        m = re.match(r"^([A-Za-z0-9_]+)-([A-Za-z0-9_]+)\.(cat|kitt)$", fn)
        if not m or not _safe_recovery_name(fn):
            continue
        path = os.path.join(rdir, fn)
        with open(path, "rb") as fh:
            data = fh.read()
        if not pimg.is_purr_image(data) or len(data) < pimg.HEADER_SIZE:
            continue
        f = pimg.header_fields(pimg.parse_header(data))
        sha = hashlib.sha256(data).hexdigest()
        immutable = _immutable_name(m.group(1), m.group(2), sha, m.group(3))
        if not os.path.exists(os.path.join(rdir, immutable)):
            _atomic_write(os.path.join(rdir, immutable), data)             # before the manifest that names it
        entry = {"component": m.group(1), "board": m.group(2), "file": immutable, "size": len(data),
                 "sha256": sha, "version": f["version"] or "0.0.0",
                 "chip": CHIPS.get(f["chip_id"], "any"), "payload_size": f["payload_size"],
                 "payload_sha256": f["payload_sha256"].hex()}
        (comps if entry["component"] in RECOVERY_COMPONENTS else mods).append(entry)
    stamp = time.strftime("%Y-%m-%d")
    _reason, tokens = read_withdrawn(rdir)
    lines = ["release=dev-%s" % time.strftime("%Y%m%d%H%M%S"), "released=%s" % stamp] + _withdrawn_lines(rdir) + [""]
    for e in comps:
        lines += ["component=%s" % e["component"], "version=%s" % e["version"], "chip=%s" % e["chip"],
                  "board=%s" % e["board"], "file=%s" % e["file"], "size=%d" % e["size"], "sha256=%s" % e["sha256"],
                  "key=%s" % ("system" if e["component"] == "uiassets" else "boot"), "payload_size=%d" % e["payload_size"], "payload_sha256=%s" % e["payload_sha256"]]
        if _is_withdrawn(tokens, e["component"], e["version"]):
            lines.append("withdrawn=yes")                    # the entry itself says so too, for a reader that only looks at entries
        lines.append("")
    _atomic_write(os.path.join(rdir, "recovery.manifest"), "\n".join(lines).encode("utf-8"))
    lines = ["release=dev-%s" % time.strftime("%Y%m%d%H%M%S"), "released=%s" % stamp] + _withdrawn_lines(rdir) + [""]
    for e in mods:
        lines += ["component=%s" % e["component"], "type=module", "version=%s" % e["version"], "chip=%s" % e["chip"],
                  "board=%s" % e["board"], "file=%s" % e["file"], "size=%d" % e["size"], "sha256=%s" % e["sha256"],
                  "key=developer", "min_coreos=0.2.0"]
        if _is_withdrawn(tokens, e["component"], e["version"]):
            lines.append("withdrawn=yes")
        lines.append("")
    _atomic_write(os.path.join(rdir, "modules.manifest"), "\n".join(lines).encode("utf-8"))
    _prune_immutable(rdir, {e["file"] for e in comps + mods})
    return comps, mods


# The partitions these images are written into (PurrOS/partitions.csv): an image whose payload is bigger cannot be installed, and the loader's only message is "something went
# wrong while installing" (2026-11-05, a KittenOS 11 KB over its 1.5 MB slot). Refused here instead.
SLOT_BYTES = {"kernel": 0x1E0000, "kittenos": 0x180000, "loader": 0x180000, "bootpkg": 0xE000, "bootloader": 0xA000, "uiassets": 0x500000}


def recovery(ctx, dir, board, kernel, kittenos, loader, bootpkg, legal, modules, bootloader="", uiassets=""):
    rdir = _recovery_dir(dir)
    os.makedirs(rdir, exist_ok=True)
    given = {"kernel": (kernel, ".cat"), "kittenos": (kittenos, ".kitt"), "loader": (loader, ".cat"),
             "bootpkg": (bootpkg, ".cat"), "bootloader": (bootloader, ".cat"), "uiassets": (uiassets, ".cat")}
    for comp, (src, ext) in given.items():
        if not src:
            continue
        with open(src, "rb") as fh:
            head = fh.read(pimg.HEADER_SIZE)
        if not pimg.is_purr_image(head):
            ctx.error("%s: not a PURR image" % src)
            return 1
        room = SLOT_BYTES.get(comp)
        size = pimg.header_fields(pimg.parse_header(head))["payload_size"]
        if room is not None and size > room:
            ctx.error("%s: its payload is %d bytes and the %s partition holds %d: %d too many (the loader could not install it)" % (src, size, comp, room, size - room))
            return 1
        _atomic_copy(src, os.path.join(rdir, "%s-%s%s" % (comp, board, ext)))
        ctx.ok("%s -> recovery/%s-%s%s" % (comp, comp, board, ext))
    for src in [x for x in (modules or "").split(",") if x.strip()]:
        src = src.strip()
        with open(src, "rb") as fh:
            head = fh.read(pimg.HEADER_SIZE)
        if not pimg.is_purr_image(head):
            ctx.error("%s: not a PURR image" % src)
            return 1
        name = pimg.header_fields(pimg.parse_header(head))["name"] or os.path.splitext(os.path.basename(src))[0]
        if name in RECOVERY_COMPONENTS or not re.match(r"^[A-Za-z0-9_]+$", name):
            ctx.error("%s: a module cannot be called %r" % (src, name))
            return 1
        _atomic_copy(src, os.path.join(rdir, "%s-%s.cat" % (name, board)))
        ctx.ok("module %s -> recovery/%s-%s.cat" % (name, name, board))
    if legal:
        _atomic_copy(legal, os.path.join(rdir, "legal.txt"))
        ctx.ok("legal text -> recovery/legal.txt")
    comps, mods = build_recovery_manifests(dir)
    ctx.ok("recovery.manifest: %s; modules.manifest: %d module(s)" %
           (", ".join("%s %s" % (e["component"], e["version"]) for e in comps) or "no components", len(mods)))
    return 0


VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")


def _tokens_for(version, components):
    names = [c.strip() for c in (components or "*").split(",") if c.strip()] or ["*"]
    return ["%s@%s" % (n, version) for n in names]


def withdraw(ctx, dir, version, components="*", reason=""):
    """Marks a version as withdrawn (bad) in the recovery manifests: devices stop offering and installing it, and one already running it is told."""
    if not VERSION_RE.match(version or ""):
        ctx.error("the version is like 1.0.20")
        return 1
    reason = " ".join((reason or "").split())[:60]
    if not reason:
        ctx.error("say why in a few words (--reason): the person sees it")
        return 1
    rdir = _recovery_dir(dir)
    if not os.path.isdir(rdir):
        ctx.error("%s has no recovery folder" % dir)
        return 1
    old_reason, tokens = read_withdrawn(rdir)
    names = [c.strip() for c in (components or "*").split(",") if c.strip()]
    if any(not re.match(r"^(\*|[A-Za-z0-9_]+)$", n) for n in names):
        ctx.error("components: a comma list of names, or * for all")
        return 1
    for t in _tokens_for(version, components):
        if t not in tokens:
            tokens.append(t)
    if len(",".join(tokens)) > 150:
        ctx.error("too many withdrawn versions in one list (the device keeps 160 characters): unwithdraw the old ones first")
        return 1
    _write_withdrawn(rdir, reason, tokens)
    comps, mods = build_recovery_manifests(dir)
    still = ["%s %s" % (e["component"], e["version"]) for e in comps + mods if _is_withdrawn(tokens, e["component"], e["version"])]
    ctx.ok("withdrawn: %s (%s)" % (", ".join(tokens), reason))
    if still:
        ctx.warn("the server still hosts %s, now marked withdrawn: devices are offered nothing for them until a good version is published with `dev recovery`" % ", ".join(still))
    return 0


def unwithdraw(ctx, dir, version, components="*"):
    """Takes a version off the withdrawn list again."""
    rdir = _recovery_dir(dir)
    reason, tokens = read_withdrawn(rdir)
    drop = set(_tokens_for(version, components))
    keep = [t for t in tokens if t not in drop]
    if len(keep) == len(tokens):
        ctx.error("nothing withdrawn matches %s" % ", ".join(sorted(drop)))
        return 1
    _write_withdrawn(rdir, reason, keep)
    build_recovery_manifests(dir)
    ctx.ok("no longer withdrawn: %s" % ", ".join(sorted(drop)))
    return 0


class _Handler(http.server.BaseHTTPRequestHandler):
    server_version = "purr-sdk"
    repo = "."
    name = "purr-sdk"
    repo_id = ""
    quiet = False

    def log_message(self, fmt, *args):
        if not self.quiet:
            sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    def _send(self, code, body=b"", ctype="text/plain"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)


    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/purr/v1/index.json":
            body = json.dumps(build_index(self.repo, self.name, self.repo_id)).encode()
            return self._send(200, body, "application/json")
        for sub in (RECOVERY_DIR, RECOVERY_VM_DIR):
            if not path.startswith("/" + sub + "/"):
                continue
            name = path[len(sub) + 2:]
            if _safe_recovery_name(name):
                try:
                    with open(os.path.join(self.repo, sub, name), "rb") as fh:
                        ctype = "text/plain" if name.endswith((".manifest", ".txt")) else "application/octet-stream"
                        return self._send(200, fh.read(), ctype)
                except OSError:
                    pass
            return self._send(404, b"not found\n")
        m = re.match(r"^/purr/v1/apps/([A-Za-z0-9_-]{1,31})/([0-9][A-Za-z0-9.+-]{0,14})/package\.cat$", path)
        if m:
            pkg = os.path.join(_apps_dir(self.repo), m.group(1), m.group(2), "package.cat")
            try:
                with open(pkg, "rb") as fh:
                    return self._send(200, fh.read(), "application/octet-stream")
            except OSError:
                pass
        self._send(404, b"not found\n")


class _Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def _announce(name, port, repo_id, stop):
    """Answers the device's broadcast: PURR-REPO?  ->  PURR-REPO! <name> <port> <id>"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):                       # macOS shares a UDP port only with this: `dev find` on the same machine
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    try:
        sock.bind(("", DISCOVERY_PORT))
    except OSError:
        return
    sock.settimeout(0.5)
    reply = ("PURR-REPO! %s %d %s" % (name.replace(" ", "_"), port, repo_id)).encode()
    while not stop.is_set():
        try:
            data, addr = sock.recvfrom(64)
        except socket.timeout:
            continue
        except OSError:
            break
        if data.strip() == b"PURR-REPO?":
            sock.sendto(reply, addr)
    sock.close()


def _broadcast_addresses():
    """Where to announce: the limited broadcast, plus each interface's own broadcast address (a laptop with Wi-Fi and a cable, a
    VPN, a VM bridge, would otherwise send the limited broadcast out of one of them only). Standard library only, so the
    interface list comes from the system tool, and a /24 is assumed for an address that cannot be asked about."""
    found = ["255.255.255.255"]
    try:
        text = subprocess.run(["ifconfig"], capture_output=True, text=True, timeout=3).stdout
        for m in re.finditer(r"broadcast\s+(\d+\.\d+\.\d+\.\d+)", text):                # macOS and BSD
            found.append(m.group(1))
        for m in re.finditer(r"inet\s+\d+\.\d+\.\d+\.\d+.*?Bcast:(\d+\.\d+\.\d+\.\d+)", text):    # older Linux
            found.append(m.group(1))
    except (OSError, subprocess.SubprocessError):
        pass
    if len(found) == 1:
        try:
            for ip in socket.gethostbyname_ex(socket.gethostname())[2]:
                if not ip.startswith("127."):
                    found.append(ip.rsplit(".", 1)[0] + ".255")
        except OSError:
            pass
    seen, out = set(), []
    for a in found:
        if a not in seen:
            seen.add(a)
            out.append(a)
    return out


def _announce_loop(name, port, repo_id, stop, interval, targets=None, discovery_port=DISCOVERY_PORT):
    """Tells the whole network the server is here, every `interval` seconds: PURR-REPO! <name> <port> <id>, the same line it
    answers a probe with. A device listening on the discovery port (or another computer running `dev find`) hears it without
    asking, and without being on the same subnet broadcast path as the probe it would have sent."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    msg = ("PURR-REPO! %s %d %s" % (name.replace(" ", "_"), port, repo_id)).encode()
    while not stop.is_set():
        for addr in (targets if targets is not None else _broadcast_addresses()):
            try:
                sock.sendto(msg, (addr, discovery_port))
            except OSError:
                pass                                           # an interface that is down: the others still go out
        stop.wait(interval)
    sock.close()


def find(ctx, wait, port):
    """Listens like a device does and prints the servers announcing themselves (and answering a probe)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        sock.bind(("", port))
    except OSError as exc:
        ctx.error("cannot listen on UDP %d: %s" % (port, exc))
        return 1
    sock.settimeout(0.5)
    for addr in _broadcast_addresses():                           # ask, too, in case nothing is announcing
        try:
            sock.sendto(b"PURR-REPO?", (addr, port))
        except OSError:
            pass
    seen = {}
    end = time.time() + wait
    while time.time() < end:
        try:
            data, addr = sock.recvfrom(128)
        except socket.timeout:
            continue
        m = re.match(r"^PURR-REPO! (\S+) (\d+) (\S+)\s*$", data.decode("ascii", "replace"))
        if m and addr[0] not in seen:
            seen[addr[0]] = m.groups()
            ctx.ok("%s  \"%s\"  http://%s:%s  id %s" % (addr[0], m.group(1), addr[0], m.group(2), m.group(3)))
    sock.close()
    if not seen:
        ctx.info("no PURR servers heard in %d s (is `dev serve` running, and are you on the same network?)" % wait)
    return 0 if seen else 1


def serve(ctx, dir, port, name, bind, quiet, announce=3):
    if not os.path.isdir(dir):
        os.makedirs(dir, exist_ok=True)
    name = _name(name)
    rid = _repo_id(dir)
    _Handler.repo, _Handler.name, _Handler.repo_id, _Handler.quiet = os.path.abspath(dir), name, rid, quiet
    try:
        srv = _Server((bind, port), _Handler)
    except OSError as exc:
        ctx.error("cannot listen on %s:%d: %s" % (bind, port, exc))
        return 1
    stop = threading.Event()
    threading.Thread(target=_announce, args=(name, port, rid, stop), daemon=True).start()
    if announce > 0:
        threading.Thread(target=_announce_loop, args=(name, port, rid, stop, announce), daemon=True).start()
    idx = build_index(dir, name, rid)
    ctx.ok("serving %s as \"%s\" (id %s) on port %d, %d app(s)" % (os.path.abspath(dir), name, rid, port,
                                                                 len(idx["apps"])))
    ctx.info("    a device in the VM reaches this at http://10.0.2.2:%d" % port)
    ctx.info("    local network only: the device refuses a public address. Ctrl-C to stop.")
    if announce > 0:
        ctx.info("    announcing every %d s to %s: devices on this network see it without asking "
                 "(check from another machine with: dev find)" % (announce, ", ".join(_broadcast_addresses())))
    if bind == "0.0.0.0":
        ctx.warn("listening on every interface; anyone on this network can read what is in the folder")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    stop.set()
    srv.server_close()
    return 0


SCRIPT = Script(
    name="dev",
    title="Developer repository",
    description="Serve a folder of built apps to a device on the local network, like a small app store "
                "for testing. The device's Developer Portal installs from it.",
    actions=(
        Action("recovery", "Host recovery images", recovery,
               help="copy recovery images into the repository folder and write recovery.manifest / modules.manifest",
               params=(Param("dir", "path", default="rel/devrepo", help="the repository folder"),
                       Param("board", "str", default="tdeck_plus", help="the board these images are for"),
                       Param("kernel", "path", default="", help="the kernel image (.cat)"),
                       Param("kittenos", "path", default="", help="the KittenOS image (.kitt)"),
                       Param("loader", "path", default="", help="the recovery loader image (.cat)"),
                       Param("bootpkg", "path", default="", help="the boot package image (.cat)"),
                       Param("bootloader", "path", default="", help="the signed bootloader image (.cat), installed by KittenOS with a backup"),
                       Param("uiassets", "path", default="", help="the signed UI icon pack (.cat from `uiassets package`), installed by stage 2"),
                       Param("legal", "path", default="", help="the legal and credits text file"),
                       Param("modules", "str", default="", help="module images to host, comma separated"))),
        Action("withdraw", "Withdraw a version", withdraw,
               help="mark a recovery version as bad: devices stop offering and installing it, and one running it is told",
               params=(Param("dir", "path", default="rel/devrepo", help="the repository folder"),
                       Param("version", "str", required=True, help="the version to withdraw, like 1.0.20"),
                       Param("components", "str", default="*", help="which components, comma separated (default all)"),
                       Param("reason", "str", required=True, help="a few words the person sees"))),
        Action("unwithdraw", "Take back a withdrawal", unwithdraw,
               help="remove a version from the withdrawn list",
               params=(Param("dir", "path", default="rel/devrepo", help="the repository folder"),
                       Param("version", "str", required=True, help="the version, like 1.0.20"),
                       Param("components", "str", default="*", help="which components, comma separated (default all)"))),
        Action("publish", "Publish an app", publish,
               help="copy a built .cat into the repository folder and refresh the index",
               params=(Param("file", "path", required=True, help="the built app (.cat)"),
                       Param("dir", "path", default="rel/devrepo", help="the repository folder"),
                       Param("title", "str", default="", help="a display title (default: the app name)"),
                       Param("desc", "str", default="", help="a one-line description"),
                       Param("force", "bool", default=False,
                             help="publish even if the payload hash is wrong (to test the device's refusal)"))),
        Action("index", "Write the index", index,
               help="(re)write purr/v1/index.json from the folder",
               params=(Param("dir", "path", default="rel/devrepo", help="the repository folder"),
                       Param("name", "str", default="", help="the repository's display name"),
                       Param("out", "path", default="", help="write the index here instead"))),
        Action("list", "List", list_apps, help="show what the repository offers",
               params=(Param("dir", "path", default="rel/devrepo", help="the repository folder"),)),
        Action("serve", "Serve", serve,
               help="serve the folder and answer the device's discovery broadcast",
               params=(Param("dir", "path", default="rel/devrepo", help="the repository folder"),
                       Param("port", "int", default=DEFAULT_PORT, help="TCP port"),
                       Param("name", "str", default="", help="the name the device shows (default: this computer's)"),
                       Param("bind", "str", default="0.0.0.0", help="address to listen on"),
                       Param("quiet", "bool", default=False, help="do not log each request"),
                       Param("announce", "int", default=3,
                             help="seconds between network-wide announcements, 0 for none (devices then need to probe)"))),
        Action("find", "Find servers", find,
               help="listen for dev servers on this network, as a device would (try it from another computer)",
               params=(Param("wait", "int", default=6, help="how many seconds to listen"),
                       Param("port", "int", default=DISCOVERY_PORT, help="the discovery UDP port"))),
    ),
)
