"""Keys: make them, list them, sign with them. Signing and making keys use purrstrap's key tool (the `cryptography` package);
everything that only looks (listing, fingerprints) works without it."""

import os

from . import keybag, runner


def list_keys(directory):
    """Key files in a folder: private (<role>.key, password-protected), public (<role>.pub), and exported (<role>_<id>.pub.bin)."""
    out = []
    if not os.path.isdir(directory):
        return out
    for name in sorted(os.listdir(directory)):
        full = os.path.join(directory, name)
        if name.endswith(".key"):
            out.append({"name": name, "kind": "private", "role": name[:-4], "path": full,
                        "note": "Password-protected. Keep it somewhere private."})
        elif name.endswith(".pub"):
            out.append({"name": name, "kind": "public", "role": name[:-4], "path": full, "note": "Safe to share."})
        elif name.endswith(".pub.bin"):
            with open(full, "rb") as fh:
                raw = fh.read()
            out.append({"name": name, "kind": "exported", "role": name.split("_")[0], "path": full,
                        "fingerprint": keybag.fingerprint(raw) if len(raw) == 64 else "", "note": "The raw form a device's key bag embeds."})
    return out


def generate(directory, role, key_id, password, force=False):
    os.makedirs(directory, exist_ok=True)
    args = ["keys", "generate", "--role", role, "--out", directory, "--key-id", int(key_id)]
    if force:
        args.append("--force")
    res = runner.purrstrap(args, password=password)
    if res["rc"] == 0:
        pub = os.path.join(directory, role + ".pub")
        res2 = runner.purrstrap(["keys", "export-public", "--pub", pub, "--role", role, "--key-id", int(key_id), "--out", directory])
        res["output"] += res2["output"]
        res["rc"] = res2["rc"]
    return res


def sign(image, key, key_id, password, out=""):
    args = ["keys", "sign", "--key", key, "--image", image, "--key-id", int(key_id)]
    if out:
        args += ["--out", out]
    return runner.purrstrap(args, password=password)
