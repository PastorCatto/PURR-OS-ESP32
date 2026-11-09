#!/usr/bin/env python3
"""Signs a release folder again with the NEW keys, so that nothing in it depends on the old ones. The old keys stay in the devices' key list (nothing is revoked here); they are
only deprecated: from this release on nothing is signed with them.

    PURRSTRAP_BOOT_KEY_PASSWORD=...  PURRSTRAP_KEY_PASSWORD=...  python3 tools/release/resign_release.py \\
        --src out/devrepo/recovery --out out/release-1.1/signed \\
        --boot-key signing_keys/new/boot.key --boot-id 3 --dev-key signing_keys/new/developer.key --dev-id 4

It reads recovery.manifest, modules.manifest and drivers.manifest in --src (whichever exist; a folder of one release has one or two of them). For each file a manifest names:
  - signed already by one of the new key ids: copied unchanged (the kernel, KittenOS and loader of 1.1 are);
  - otherwise signed again: with the BOOT key when the stanza says key=boot (bootloader, boot package), with the DEVELOPER key when it says key=developer
    (modules, drivers). A picture pack (key=system in the manifest) is signed with the BOOT key too: the device allows the boot or the system role for pictures (purr_role_may_sign,
    PURR_MOD_UIASSETS), so no third key is needed, and its manifest line then says key=boot, as the signer's role does. The payload is not touched, only the key id and the signature, so the file gets a new hash: its name (when it carries a 10-letter hash) and its
    size/sha256 line in the manifest are updated.
Everything is written to --out (a folder that does not exist yet); --src is never changed. Every file in --out is then verified with the public key that should have signed it
and checked against its manifest line (size and sha256); the tool stops at the first mismatch.

The passwords are read from the environment of this one command (PURRSTRAP_BOOT_KEY_PASSWORD for the boot key, PURRSTRAP_KEY_PASSWORD for the developer key; the boot key
falls back to the second), or asked for in a terminal. They are never put on a command line and never written anywhere. Run it with a Python that has the `cryptography`
module (the kit's, or the ESP-IDF Python).
"""
import argparse
import getpass
import hashlib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "purrstrap"))
from lib import image as pimg  # noqa: E402
from make_official_from_dev import parse_manifest, write_manifest  # noqa: E402

MANIFESTS = ("recovery.manifest", "modules.manifest", "drivers.manifest")
HASHED = re.compile(r"^(?P<stem>.+)\.(?P<h>[0-9a-f]{10})(?P<ext>\.[A-Za-z]+)$")


def sha256(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


def key_id_of(path):
    with open(path, "rb") as fh:
        head = fh.read(pimg.HEADER_SIZE)
    return pimg.header_fields(pimg.parse_header(head))["key_id"]


def is_picture_pack(path):
    with open(path, "rb") as fh:
        f = pimg.header_fields(pimg.parse_header(fh.read(pimg.HEADER_SIZE)))
    return f["image_type"] == pimg.IMG_MODULE and f["flags"] == pimg.MOD_UIASSETS


def purrstrap(args, password=None):
    env = dict(os.environ)
    env.pop("PURRSTRAP_KEY_PASSWORD", None)
    if password is not None:
        env["PURRSTRAP_KEY_PASSWORD"] = password
    p = subprocess.run([sys.executable, os.path.join(ROOT, "purrstrap", "purrstrap.py")] + [str(a) for a in args], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       universal_newlines=True, stdin=subprocess.DEVNULL)
    return p.returncode, p.stdout


def new_name(old, digest):
    m = HASHED.match(old)
    return "%s.%s%s" % (m.group("stem"), digest[:10], m.group("ext")) if m else old


def password_for(name, env_names):
    for n in env_names:
        if os.environ.get(n) is not None:
            return os.environ[n]
    if not sys.stdin.isatty():
        sys.exit("I need the %s key's password: set %s for this command, or run me in a terminal." % (name, env_names[0]))
    return getpass.getpass("%s key password: " % name.capitalize())


def pub_of(key):
    return os.path.splitext(key)[0] + ".pub"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--src", required=True, help="a folder with the manifests and the files they name")
    ap.add_argument("--out", required=True, help="a folder that does not exist yet")
    ap.add_argument("--boot-key", required=True)
    ap.add_argument("--boot-id", type=int, required=True)
    ap.add_argument("--dev-key", required=True)
    ap.add_argument("--dev-id", type=int, required=True)
    ap.add_argument("--dry-run", action="store_true", help="only say what would be signed again")
    a = ap.parse_args(argv)

    src, out = os.path.abspath(os.path.expanduser(a.src)), os.path.abspath(os.path.expanduser(a.out))
    new_ids = {a.boot_id, a.dev_id}
    if a.boot_id == a.dev_id:
        sys.exit("The boot key and the developer key need different ids.")
    for k in (a.boot_key, a.dev_key):
        if not os.path.isfile(k) or not os.path.isfile(pub_of(k)):
            sys.exit("I need the private key %s and its public half %s." % (k, pub_of(k)))
    present = [m for m in MANIFESTS if os.path.isfile(os.path.join(src, m))]
    if not present:
        sys.exit("There is no recovery.manifest, modules.manifest or drivers.manifest in %s." % src)
    if os.path.exists(out) and not a.dry_run:
        sys.exit("%s already exists: move it away first." % out)

    plan = []        # (manifest, head, stanzas)
    for m in present:
        with open(os.path.join(src, m), encoding="utf-8") as fh:
            head, stanzas = parse_manifest(fh.read())
        plan.append((m, head, stanzas))
    todo = []
    for m, _h, stanzas in plan:
        for s in stanzas:
            f = os.path.join(src, s["file"])
            if not os.path.isfile(f):
                sys.exit("%s names %s, which is not in %s." % (m, s["file"], src))
            if os.path.getsize(f) != int(s["size"]) or sha256(f) != s["sha256"]:
                sys.exit("%s does not match its line in %s (size or sha256): fix the source first." % (s["file"], m))
            kid = key_id_of(f)
            role = s.get("key", "")
            if kid in new_ids:
                continue
            if role == "system" and is_picture_pack(f):
                role = "boot"
            if role not in ("boot", "developer"):
                sys.exit("%s (%s) says key=%s: I only sign boot and developer files, and picture packs (a system key would be needed for the rest)." % (s["file"], m, role))
            todo.append((m, s, kid, role))
    print("%d file(s) are signed with an old key and will be signed again; the rest already carry a new key." % len(todo))
    for m, s, kid, role in todo:
        print("  %-14s %-44s key %d -> %d (%s)" % (m, s["file"], kid, a.boot_id if role == "boot" else a.dev_id, role))
    if a.dry_run:
        return 0

    boot_pw = password_for("boot", ("PURRSTRAP_BOOT_KEY_PASSWORD", "PURRSTRAP_KEY_PASSWORD")) if any(r == "boot" for *_x, r in todo) else None
    dev_pw = password_for("developer", ("PURRSTRAP_KEY_PASSWORD",)) if any(r == "developer" for *_x, r in todo) else None

    os.makedirs(out)
    renamed = {}
    for m, head, stanzas in plan:
        for s in stanzas:
            old = s["file"]
            if old in renamed:
                s["file"], s["sha256"], s["size"] = renamed[old]
                continue
            srcf = os.path.join(src, old)
            kid = key_id_of(srcf)
            if kid in new_ids:
                shutil.copyfile(srcf, os.path.join(out, old))
                continue
            role = s["key"]
            if role == "system" and is_picture_pack(srcf):
                role = s["key"] = "boot"                      # signed with the boot key, so the line says so
            tmp = os.path.join(out, ".signing-" + old)
            shutil.copyfile(srcf, tmp)
            key, kid_new, pw = (a.boot_key, a.boot_id, boot_pw) if role == "boot" else (a.dev_key, a.dev_id, dev_pw)
            rc, text = purrstrap(["keys", "sign", "--key", key, "--image", tmp, "--key-id", kid_new], password=pw)
            if rc != 0:
                os.remove(tmp)
                sys.exit("Signing %s failed: %s" % (old, (text.strip().splitlines() or ["no reason given"])[-1]))
            digest = sha256(tmp)
            name = new_name(old, digest)
            os.replace(tmp, os.path.join(out, name))
            s["file"], s["sha256"], s["size"] = name, digest, str(os.path.getsize(os.path.join(out, name)))
            renamed[old] = (s["file"], s["sha256"], s["size"])
    for m, head, stanzas in plan:
        write_manifest(os.path.join(out, m), dict(head).get("release", "resigned"), head, stanzas)
    legal = os.path.join(src, "legal.txt")
    if os.path.isfile(legal):
        shutil.copyfile(legal, os.path.join(out, "legal.txt"))

    # verify what was written
    problems = []
    for m, _h, stanzas in plan:
        for s in stanzas:
            f = os.path.join(out, s["file"])
            if sha256(f) != s["sha256"] or os.path.getsize(f) != int(s["size"]):
                problems.append("%s does not match its line in %s" % (s["file"], m))
                continue
            kid = key_id_of(f)
            key = a.boot_key if kid == a.boot_id else a.dev_key if kid == a.dev_id else None
            if key is None:
                problems.append("%s carries key id %d, which is neither new key" % (s["file"], kid))
                continue
            rc, text = purrstrap(["keys", "verify", "--key", pub_of(key), "--image", f])
            if rc != 0:
                problems.append("%s: %s" % (s["file"], (text.strip().splitlines() or ["does not verify"])[-1]))
    if problems:
        for p in problems:
            print("PROBLEM: " + p)
        return 1
    n = sum(len(st) for _m, _h, st in plan)
    print("Done: %d file(s) in %s, every one verified with the new key that signed it." % (n, out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
