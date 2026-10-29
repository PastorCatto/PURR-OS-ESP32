"""Repositories: make one, add packages, sign it, check it. The format is Repositories/SPEC.md.

A maintainer's working folder:

    myrepo/
      repo.toml                       name, maintainer, url, description, min_os
      keys/repo.key, repo.pub         the repository's key (the private half is password-protected: keep it out of git)
      endorsement.json                optional: the project's endorsement of this key (see `endorse`)
      packages/<file>                 the packages (a .cat app, or any asset or extra file)
      packages/<file>.toml            what the Store shows about each: title, desc, category, source, perms, type, target, dest ...

`build` writes the files to upload, flat (a GitHub release's assets) or as a tree (a web host).
"""

import hashlib
import json
import os
import shutil
import time

from . import formats, keybag, minitoml, p256

FORMAT = 2
ENDORSE_PREFIX = b"PURRREPO\0"
TYPES = ("app", "asset", "extra")
TARGETS = ("flash", "sd", "any")


class RepoError(Exception):
    pass


def fingerprint(pub):
    """The repository id: the first 16 hex digits of SHA-256 of the 64 public-key bytes."""
    return hashlib.sha256(pub).hexdigest()[:16]


def pretty_fingerprint(rid):
    return " ".join(rid[i:i + 4] for i in range(0, len(rid), 4))


def _crypto():
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec, utils
    except ImportError:
        raise RepoError("Making and signing needs the cryptography package. The kit's own Python has it.")
    return hashes, serialization, ec, utils


def _raw_pub(public_key):
    n = public_key.public_numbers()
    return n.x.to_bytes(32, "big") + n.y.to_bytes(32, "big")


def sign_bytes(private_key, data):
    """Raw 64-byte r||s over SHA-256 of `data`."""
    hashes, _s, ec, utils = _crypto()
    der = private_key.sign(hashlib.sha256(data).digest(), ec.ECDSA(utils.Prehashed(hashes.SHA256())))
    r, s = utils.decode_dss_signature(der)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def load_private(path, password):
    _h, serialization, _e, _u = _crypto()
    with open(path, "rb") as fh:
        try:
            return serialization.load_pem_private_key(fh.read(), password.encode() if password else None)
        except (ValueError, TypeError) as e:
            raise RepoError("Could not open the key %s (wrong password?): %s" % (path, e))


# ------------------------------------------------------------------------------------------------------------------ working folder

def init(folder, name, maintainer, url, password, description="", min_os="1.0.0"):
    """Start a repository: a key and a repo.toml. Returns {"id", "folder"}."""
    _h, serialization, ec, _u = _crypto()
    if os.path.exists(os.path.join(folder, "repo.toml")):
        raise RepoError("%s is already a repository." % folder)
    if len(password) < 6:
        raise RepoError("Pick a password of at least 6 characters: it protects the repository's private key.")
    os.makedirs(os.path.join(folder, "keys"), exist_ok=True)
    os.makedirs(os.path.join(folder, "packages"), exist_ok=True)
    priv = ec.generate_private_key(ec.SECP256R1())
    with open(os.path.join(folder, "keys", "repo.key"), "wb") as fh:
        fh.write(priv.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.BestAvailableEncryption(password.encode())))
    with open(os.path.join(folder, "keys", "repo.pub"), "wb") as fh:
        fh.write(priv.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
    with open(os.path.join(folder, ".gitignore"), "w") as fh:
        fh.write("keys/repo.key\nout/\n")
    meta = {"repo": {"name": name, "maintainer": maintainer, "url": url, "description": description, "min_os": min_os}}
    with open(os.path.join(folder, "repo.toml"), "w") as fh:
        fh.write(minitoml.dumps(meta))
    return {"id": fingerprint(_raw_pub(priv.public_key())), "folder": folder}


def _public(folder):
    _h, serialization, _e, _u = _crypto()
    with open(os.path.join(folder, "keys", "repo.pub"), "rb") as fh:
        return _raw_pub(serialization.load_pem_public_key(fh.read()))


def package_info(path):
    """What can be read from a package file itself: {"type", "name", "version", "chip", "key_id", "signed"}."""
    with open(path, "rb") as fh:
        data = fh.read(formats.image().HEADER_SIZE + 4)
    img = formats.image()
    if data[:4] == b"RRUP" and len(data) >= img.HEADER_SIZE:
        f = img.header_fields(img.parse_header(data))
        chip = {9: "esp32s3", 0: "esp32", 0xFFFF: "any"}.get(f["chip_id"], "any")
        return {"type": "app" if f["image_type"] == img.IMG_APP else "module", "name": f["name"], "version": f["version"], "chip": chip,
                "key_id": f["key_id"], "signed": f["signature"] not in (b"\0" * 64, b"\xff" * 64)}
    return {}


def add(folder, file, title="", desc="", category="", source="", perms=None, type_=None, target="flash", dest="", min_os="", changelog="", name=None, version=None):
    """Copy a package into packages/ and write its metadata. Returns the metadata dict."""
    if not os.path.isfile(file):
        raise RepoError("I cannot find %s." % file)
    info = package_info(file)
    kind = type_ or ("app" if info.get("type") == "app" else "asset")
    if kind not in TYPES:
        raise RepoError("type is one of: %s." % ", ".join(TYPES))
    if info.get("type") == "module":
        raise RepoError("%s is a module or a driver. Repositories carry apps, assets and extras; drivers have their own path." % os.path.basename(file))
    if target not in TARGETS:
        raise RepoError("target is one of: %s." % ", ".join(TARGETS))
    if kind != "app" and not dest:
        raise RepoError("An asset or an extra needs a destination (dest), like fonts/mono.bin.")
    if kind != "app":
        _check_dest(dest)
    nm = name or info.get("name") or os.path.splitext(os.path.basename(file))[0]
    ver = version or info.get("version")
    if not ver:
        raise RepoError("I need a version for %s (--version)." % os.path.basename(file))
    fname = "%s-%s%s" % (nm, ver, os.path.splitext(file)[1])
    os.makedirs(os.path.join(folder, "packages"), exist_ok=True)
    shutil.copyfile(file, os.path.join(folder, "packages", fname))
    meta = {"item": {"name": nm, "version": ver, "type": kind, "target": target, "title": title or nm, "desc": desc, "category": category, "source": source,
                     "perms": list(perms or []), "dest": dest, "min_os": min_os, "changelog": changelog, "chip": info.get("chip", "any")}}
    with open(os.path.join(folder, "packages", fname + ".toml"), "w") as fh:
        fh.write(minitoml.dumps(meta))
    return meta["item"]


def _check_dest(dest):
    parts = dest.split("/")
    if dest.startswith("/") or "\\" in dest or any(p in ("", ".", "..") for p in parts) or len(dest) > 80:
        raise RepoError("dest must be a plain relative path like fonts/mono.bin (no .., no leading slash).")


# ------------------------------------------------------------------------------------------------------------------ building

def _items(folder):
    out = []
    pk = os.path.join(folder, "packages")
    if not os.path.isdir(pk):
        return out
    for fname in sorted(os.listdir(pk)):
        if fname.endswith(".toml") or fname.startswith("."):
            continue
        path = os.path.join(pk, fname)
        meta_path = path + ".toml"
        item = minitoml.load(meta_path)["item"] if os.path.isfile(meta_path) else {}
        with open(path, "rb") as fh:
            data = fh.read()
        info = package_info(path)
        out.append((fname, data, item, info))
    return out


def build(folder, out, password, layout="flat", now=None):
    """Write the files to upload into `out`. Returns {"id", "files": [...], "apps": n}."""
    if layout not in ("flat", "tree"):
        raise RepoError("layout is flat or tree.")
    meta = minitoml.load(os.path.join(folder, "repo.toml"))["repo"]
    priv = load_private(os.path.join(folder, "keys", "repo.key"), password)
    pub = _raw_pub(priv.public_key())
    rid = fingerprint(pub)
    now = int(now if now is not None else time.time())
    repo_json = {"format": FORMAT, "id": rid, "name": meta.get("name", ""), "description": meta.get("description", ""), "maintainer": meta.get("maintainer", ""),
                 "url": meta.get("url", ""), "key": {"alg": "p256", "pub": pub.hex()}, "min_os": meta.get("min_os", "1.0.0"), "layout": layout, "updated": now}
    end_path = os.path.join(folder, "endorsement.json")
    if os.path.isfile(end_path):
        with open(end_path) as fh:
            end = json.load(fh)
        if end.get("repo_id") == rid:
            repo_json["endorsement"] = {"key_id": end["key_id"], "sig": end["sig"]}
    apps = []
    files = {}
    for fname, data, item, info in _items(folder):
        name = item.get("name") or info.get("name") or fname
        version = item.get("version") or info.get("version") or "0"
        kind = item.get("type") or ("app" if info.get("type") == "app" else "asset")
        rel = fname if layout == "flat" else "%s/%s/%s/%s" % ({"app": "apps", "asset": "assets", "extra": "extras"}[kind], name, version, "package.cat" if kind == "app" else os.path.basename(item.get("dest") or fname))
        entry = {"name": name, "title": item.get("title") or name, "version": version, "type": kind, "target": item.get("target", "flash"), "size": len(data),
                 "sha256": hashlib.sha256(data).hexdigest(), "chip": item.get("chip") or info.get("chip", "any"), "signed": bool(info.get("signed")), "key_id": info.get("key_id", 0),
                 "desc": item.get("desc", ""), "url": rel, "updated": now}
        for k in ("category", "source", "min_os", "changelog", "dest"):
            if item.get(k):
                entry[k] = item[k]
        if item.get("perms"):
            entry["perms"] = item["perms"]
        if kind != "app":
            _check_dest(entry.get("dest", ""))
        apps.append(entry)
        files[rel] = data
    index = {"format": FORMAT, "repo": {"id": rid, "name": repo_json["name"], "kind": "store"}, "apps": apps}
    repo_bytes = json.dumps(repo_json, separators=(",", ":")).encode()
    index_bytes = json.dumps(index, separators=(",", ":")).encode()
    base = out if layout == "flat" else os.path.join(out, "purr", "v1")
    os.makedirs(base, exist_ok=True)
    written = []

    def put(rel, data):
        p = os.path.join(base, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as fh:
            fh.write(data)
        written.append(os.path.relpath(p, out))

    put("repo.json", repo_bytes)
    put("repo.json.sig", sign_bytes(priv, repo_bytes).hex().encode())
    put("index.json", index_bytes)
    put("index.json.sig", sign_bytes(priv, index_bytes).hex().encode())
    for rel, data in files.items():
        put(rel, data)
    return {"id": rid, "files": written, "apps": len(apps), "layout": layout}


def endorse(repo_pub_or_folder, system_key, key_id, password, out=None):
    """The project's endorsement of a repository's key: a signature over "PURRREPO\\0" + SHA-256(pub) by a key the devices already have.
    Writes endorsement.json into the repository folder (if given one) and returns it."""
    pub = _public(repo_pub_or_folder) if os.path.isdir(repo_pub_or_folder) else open(repo_pub_or_folder, "rb").read()
    if len(pub) != 64:
        raise RepoError("I need the repository's folder, or its public key as 64 raw bytes.")
    priv = load_private(system_key, password)
    sig = sign_bytes(priv, ENDORSE_PREFIX + hashlib.sha256(pub).digest())
    end = {"repo_id": fingerprint(pub), "key_id": int(key_id), "sig": sig.hex()}
    if os.path.isdir(repo_pub_or_folder):
        with open(os.path.join(repo_pub_or_folder, "endorsement.json"), "w") as fh:
            json.dump(end, fh, indent=1)
            fh.write("\n")
    return end


# ------------------------------------------------------------------------------------------------------------------ checking (no extra packages)

def _read(base, rel):
    if base.startswith(("http://", "https://")):
        import urllib.request
        with urllib.request.urlopen(base.rstrip("/") + "/" + rel, timeout=30) as r:
            return r.read()
    with open(os.path.join(base, rel), "rb") as fh:
        return fh.read()


def verify(base, key_dirs=()):
    """Check a repository (a folder or an https address) the way a device does. Returns a report:
    {"ok", "tier", "id", "name", "problems": [...], "notes": [...], "apps": n}"""
    problems, notes = [], []
    prefix = ""
    try:
        _read(base, "repo.json")
    except Exception:
        prefix = "purr/v1/"
    try:
        repo_bytes = _read(base, prefix + "repo.json")
        repo_sig = bytes.fromhex(_read(base, prefix + "repo.json.sig").decode().strip())
        repo = json.loads(repo_bytes)
    except Exception as e:
        return {"ok": False, "tier": "untrusted", "id": "", "name": "", "problems": ["I could not read repo.json and its signature: %s" % e], "notes": [], "apps": 0}
    pub = bytes.fromhex(repo.get("key", {}).get("pub", ""))
    rid = fingerprint(pub) if len(pub) == 64 else ""
    if repo.get("id") != rid:
        problems.append("The id in repo.json (%s) is not the fingerprint of its key (%s)." % (repo.get("id"), rid))
    if not p256.verify(pub, repo_bytes, repo_sig):
        problems.append("repo.json is not signed by its own key.")
    tier = "community"
    end = repo.get("endorsement")
    if end:
        keys = keybag.load_all(key_dirs)
        msg = ENDORSE_PREFIX + hashlib.sha256(pub).digest()
        try:
            esig = bytes.fromhex(end.get("sig", ""))
        except ValueError:
            esig = b""
        good = [k for k in keys if k["id"] == end.get("key_id") and p256.verify(k["pub"], msg, esig)]
        if good:
            tier = "official"
            notes.append("Endorsed by key %d (%s role)." % (good[0]["id"], good[0]["role"]))
        else:
            problems.append("The endorsement does not verify against any key a device knows.")
    try:
        index_bytes = _read(base, prefix + "index.json")
        index_sig = bytes.fromhex(_read(base, prefix + "index.json.sig").decode().strip())
        index = json.loads(index_bytes)
    except Exception as e:
        problems.append("I could not read index.json and its signature: %s" % e)
        index = {"apps": []}
        index_bytes = b""
    else:
        if not p256.verify(pub, index_bytes, index_sig):
            problems.append("index.json is not signed by the repository's key.")
        if len(index_bytes) > 24 * 1024:
            problems.append("index.json is %d bytes; a device refuses an index over 24 KB." % len(index_bytes))
        if index.get("repo", {}).get("id") != rid:
            problems.append("index.json names a different repository id.")
    for a in index.get("apps", []):
        rel = a.get("url", "")
        if rel.startswith("/") or ".." in rel or "://" in rel:
            problems.append("%s: its address is not a plain relative path." % a.get("name"))
            continue
        try:
            data = _read(base, prefix + rel)
        except Exception as e:
            problems.append("%s %s: cannot read %s (%s)." % (a.get("name"), a.get("version"), rel, e))
            continue
        if len(data) != a.get("size") or hashlib.sha256(data).hexdigest() != a.get("sha256"):
            problems.append("%s %s: %s does not match the size and hash in the index." % (a.get("name"), a.get("version"), rel))
        if a.get("type", "app") != "app" and not a.get("dest"):
            problems.append("%s: an asset or extra with no dest." % a.get("name"))
    return {"ok": not problems, "tier": "untrusted" if problems else tier, "id": rid, "name": repo.get("name", ""), "problems": problems, "notes": notes,
            "apps": len(index.get("apps", [])), "layout": "tree" if prefix else "flat"}
