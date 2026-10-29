"""Look inside a file or folder and say, in plain words, what it is, whether it is whole, who signed it,
and what you can do with it next. This is what Studio's "drop an image here" shows, and `purr inspect` prints.

Everything returns plain dicts and lists (JSON-friendly):

    {"kind": "purr-image", "title": "...", "summary": "...",
     "facts":  [["Version", "1.0.11"], ...],
     "checks": [{"status": "ok|warn|bad|info", "name": "...", "text": "..."}],
     "parts":  [ ...smaller reports of the pieces inside... ],
     "next":   [{"tool": "sign", "label": "...", "why": "..."}]}
"""

import hashlib
import os
import struct

from . import formats, keybag, p256

IMG_NAMES = {1: "operating system", 2: "recovery image", 3: "module", 4: "app"}
MOD_NAMES = {1: "kernel", 2: "CoreOS", 3: "AppManager", 4: "runtime", 5: "driver", 6: "boot package",
             7: "developer bundle", 8: "recovery loader", 9: "bootloader"}
CHIP_NAMES = {0: "ESP32", 9: "ESP32-S3", 0xFFFF: "any chip"}
SECURE_NAMES = {0: "off", 1: "warn", 2: "enforce"}
CFG_FLAGS = [(8, "development mode"), (9, "fresh (never reached a healthy boot)"), (10, "dev recovery (local server)"),
             (3, "boot KittenOS once"), (4, "boot the loader once"), (5, "unattended reinstall"),
             (11, "kernel marked bad"), (12, "KittenOS marked bad"), (13, "fast start"), (14, "show menu")]
ESP_MAGIC = 0xE9
TRAILER = 4096


def _check(status, name, text):
    return {"status": status, "name": name, "text": text}


def _report(kind, title, summary):
    return {"kind": kind, "title": title, "summary": summary, "facts": [], "checks": [], "parts": [], "next": []}


def _size(n):
    if n >= 1 << 20:
        return "%.2f MB" % (n / float(1 << 20))
    if n >= 1 << 10:
        return "%.1f KB" % (n / 1024.0)
    return "%d bytes" % n


def describe_header(f):
    img = formats.image()
    if f["image_type"] == img.IMG_MODULE:
        return MOD_NAMES.get(f["flags"] & 0xFF, "module %d" % (f["flags"] & 0xFF))
    return IMG_NAMES.get(f["image_type"], "type %d" % f["image_type"])


def check_signature(f, header_bytes, keys):
    """The signature verdict for one header: {"status", "text", "key"}; `keys` is a keybag list."""
    img = formats.image()
    if f["signature"] == b"\0" * 64 or f["signature"] == b"\xff" * 64:
        return {"status": "warn", "key": None,
                "text": "Not signed. A device in enforce mode will refuse it; warn mode boots it with a warning."}
    if f["key_id"] == 0:
        return {"status": "bad", "key": None, "text": "It says it was signed with key 0, which means no key. A device will refuse it."}
    digest = hashlib.sha256(header_bytes[:img.SIGNED_LEN]).digest()
    matching = [k for k in keys if k["id"] == f["key_id"]]
    if not matching:
        return {"status": "info", "key": None,
                "text": "Signed with key %d, which is not one I know. Add its public key to check it." % f["key_id"]}
    for k in matching:
        if p256.verify_hash(k["pub"], digest, f["signature"]):
            allowed = keybag.allowed_roles(f["image_type"], f["flags"] & 0xFF)
            if allowed and k["role"] not in allowed:
                return {"status": "bad", "key": k,
                        "text": "The signature is real (key %d, %s role), but the %s role may not sign a %s, so a device would refuse it. "
                                "It wants: %s." % (k["id"], k["role"], k["role"], describe_header(f), ", ".join(allowed))}
            return {"status": "ok", "key": k,
                    "text": "Signed by key %d (%s role, fingerprint %s). The signature checks out." % (k["id"], k["role"], k["fingerprint"])}
    return {"status": "bad", "key": None,
            "text": "Signed as key %d, but the signature does not match that key. The file was changed after signing, "
                    "or signed by a different key." % f["key_id"]}


def _image_report(data, keys, title, header_offset=0, payload_from=None):
    img = formats.image()
    header_bytes = data[header_offset:header_offset + img.HEADER_SIZE]
    f = img.header_fields(img.parse_header(header_bytes))
    what = describe_header(f)
    rep = _report("purr-image", title, "A %s, %s, for %s." % (what, f["version"] or "no version",
                                                             CHIP_NAMES.get(f["chip_id"], "chip %d" % f["chip_id"])))
    rep["facts"] = [["Name", f["name"]], ["Kind", what], ["Version", f["version"] or "(none)"],
                    ["Chip", CHIP_NAMES.get(f["chip_id"], str(f["chip_id"]))],
                    ["Needs boot version", f["min_boot_version"] or "(any)"],
                    ["Payload", "%s at offset %d" % (_size(f["payload_size"]), f["payload_offset"])],
                    ["Payload SHA-256", f["payload_sha256"].hex()], ["Signing key id", str(f["key_id"])]]
    start = header_offset + f["payload_offset"] if payload_from is None else payload_from
    end = start + f["payload_size"]
    if end > len(data):
        rep["checks"].append(_check("bad", "Whole file", "The header says the payload runs past the end of the file, so part of it is missing."))
    elif hashlib.sha256(data[start:end]).digest() == f["payload_sha256"]:
        rep["checks"].append(_check("ok", "Payload hash", "The contents match the hash in the header, so nothing is damaged or edited."))
    else:
        rep["checks"].append(_check("bad", "Payload hash", "The contents do not match the hash in the header: the file is damaged or was edited after signing."))
    sig = check_signature(f, header_bytes, keys)
    rep["checks"].append(_check(sig["status"], "Signature", sig["text"]))
    rep["header"] = {k: (v.hex() if isinstance(v, bytes) else v) for k, v in f.items()}
    if sig["status"] == "warn":
        rep["next"].append({"tool": "sign", "label": "Sign it", "why": "Give it your signature so a device in enforce mode will take it."})
    rep["next"].append({"tool": "release", "label": "Put it in a release", "why": "Gather it with the other parts into a folder a device can recover from."})
    rep["next"].append({"tool": "server", "label": "Send it to the dev server", "why": "Host it on your network so a device can fetch it."})
    return rep


def _parse_partitions(data, offset):
    parts = []
    for i in range(0, 0x1000, 32):
        e = data[offset + i:offset + i + 32]
        if len(e) < 32 or e[:2] != b"\xaa\x50":
            break
        ptype, sub, off, size = struct.unpack_from("<BBII", e, 2)
        parts.append({"name": e[12:28].split(b"\0")[0].decode("ascii", "replace"),
                      "type": ptype, "subtype": sub, "offset": off, "size": size})
    return parts


def _cfg_report(data):
    pc = formats.purrcfg()
    rep = _report("purrcfg", "Trust block", "")
    try:
        c = pc.parse_cfg(data)
    except ValueError as e:
        rep["summary"] = "This area should hold the device's trust settings, but it does not look right (%s)." % e
        rep["checks"].append(_check("warn", "Trust settings", str(e)))
        return rep
    mode = SECURE_NAMES.get(c["secure_mode"], str(c["secure_mode"]))
    flags = [label for bit, label in CFG_FLAGS if c["flags"] & (1 << bit)]
    signed = c["trust_sig"] not in (b"\0" * 64, b"\xff" * 64)
    rep["summary"] = "Secure mode: %s. %s." % (mode, "The trust block is signed" if signed else "The trust block is not signed")
    rep["facts"] = [["Secure mode", mode], ["Trust block signed", "yes" if signed else "no"],
                    ["Development locked", "yes" if c["trust_flags"] & pc.TRUSTF_DEV_LOCKED else "no"],
                    ["Flags", ", ".join(flags) or "(none)"],
                    ["Revoked keys", "%08x" % c["revoked"] if c["revoked"] else "(none)"],
                    ["Write counter", str(c["seq"])]]
    slots = []
    for i in range(pc.KEY_SLOTS):
        s = c["keys"][i * pc.KEY_SLOT_SIZE:(i + 1) * pc.KEY_SLOT_SIZE]
        if s[2] == 1:
            slots.append("id %d, %s, %s" % (s[0], keybag.ROLE_NAMES.get(s[1], "role %d" % s[1]), keybag.fingerprint(bytes(s[4:68]))))
    rep["facts"].append(["Extra keys", "; ".join(slots) or "(none)"])
    rep["trust"] = {"secure_mode": mode, "signed": signed, "dev_locked": bool(c["trust_flags"] & pc.TRUSTF_DEV_LOCKED)}
    return rep


def flash_report(data, keys):
    img = formats.image()
    rep = _report("flash-image", "Flash image", "A whole-flash image of %s." % _size(len(data)))
    table_at = next((o for o in (0xA000, 0x8000) if len(data) > o + 32 and data[o:o + 2] == b"\xaa\x50"), None)
    if data[0] == ESP_MAGIC:
        rep["checks"].append(_check("ok", "Bootloader", "An ESP image starts at 0x0, before the partition table."))
    else:
        rep["checks"].append(_check("warn", "Bootloader", "Nothing that looks like a bootloader at the start. A device cannot boot this by itself."))
    if table_at is None:
        rep["checks"].append(_check("bad", "Partition table", "I cannot find a partition table at 0x8000 or 0xA000."))
        return rep
    parts = _parse_partitions(data, table_at)
    rep["facts"] = [["Size", _size(len(data))], ["Partition table at", "0x%X" % table_at], ["Partitions", str(len(parts))]]
    for p in parts:
        off, size, name = p["offset"], p["size"], p["name"]
        region = data[off:off + size]
        blank = len(region) > 0 and not region.strip(b"\xff")
        part = None
        if name == "purrcfg":
            part = _cfg_report(region)
            part["title"] = "Trust block (purrcfg)"
            rep["trust"] = part.get("trust")
        elif region[:4] == b"RRUP":
            part = _image_report(region, keys, name)
        elif p["type"] == 0 and region[:1] == bytes([ESP_MAGIC]):
            at = size - TRAILER
            if region[at:at + 4] == b"RRUP":
                part = _image_report(region, keys, name, header_offset=at, payload_from=0)
                part["title"] = name
            else:
                part = _report("esp-app", name, "An app image is in the slot, but it has no signature trailer.")
                part["checks"].append(_check("warn", "Signature", "No signature trailer in the slot's last sector."))
        if part is None:
            part = _report("partition", name, "Empty (erased)." if blank else "%s of data." % _size(size))
        part["partition"] = {"name": name, "offset": off, "size": size, "type": p["type"], "subtype": p["subtype"], "blank": blank}
        part["title"] = part.get("title") or name
        rep["parts"].append(part)
    filled = [p["partition"]["name"] for p in rep["parts"] if p["kind"] in ("purr-image", "esp-app")]
    missing = [n for n in ("kittenos", "kernel", "loader") if n not in filled]
    if missing:
        rep["checks"].append(_check("info", "Slots", "No system in: %s. A blank slot is normal on a fresh device: "
                                    "the recovery loader fills them from the internet." % ", ".join(missing)))
    bad = [p["title"] for p in rep["parts"] if any(c["status"] == "bad" for c in p["checks"])]
    rep["checks"].append(_check("bad" if bad else "ok", "Signatures and hashes",
                                ("Problems in: " + ", ".join(bad)) if bad else "Every part that is signed checks out."))
    rep["summary"] = "A full flash image with %d partitions, %d of them holding signed parts." % (len(parts), len(filled))
    rep["next"] = [{"tool": "flash", "label": "Flash it", "why": "Write it to a board with esptool."},
                   {"tool": "emulator", "label": "Run it", "why": "Try it in the emulator (coming soon)."}]
    return rep


def parse_manifest(text):
    """recovery.manifest, drivers.manifest and modules.manifest: header lines, then blank-line separated key=value blocks."""
    blocks, cur = [], {}
    for line in text.splitlines():
        line = line.strip()
        if not line:
            if cur:
                blocks.append(cur)
                cur = {}
            continue
        if "=" in line:
            k, v = line.split("=", 1)
            cur[k.strip()] = v.strip()
    if cur:
        blocks.append(cur)
    return blocks


def _manifest_report(path, text):
    blocks = parse_manifest(text)
    head = blocks[0] if blocks and "component" not in blocks[0] else {}
    items = [b for b in blocks if "component" in b]
    rep = _report("manifest", os.path.basename(path), "A manifest for release %s listing %d files." % (head.get("release", "?"), len(items)))
    rep["facts"] = [["Release", head.get("release", "?")], ["Released", head.get("released", "?")], ["Files listed", str(len(items))]]
    folder = os.path.dirname(os.path.abspath(path))
    problems = 0
    for b in items:
        fname = b.get("file", "")
        full = os.path.join(folder, fname)
        label = "%s %s (%s)" % (b["component"], b.get("version", ""), b.get("board", b.get("chip", "")))
        if not os.path.isfile(full):
            rep["checks"].append(_check("warn", label, "%s is not next to the manifest." % fname))
            problems += 1
            continue
        with open(full, "rb") as fh:
            blob = fh.read()
        ok_size = str(len(blob)) == b.get("size", str(len(blob)))
        ok_hash = hashlib.sha256(blob).hexdigest() == b.get("sha256", hashlib.sha256(blob).hexdigest())
        if ok_size and ok_hash:
            rep["checks"].append(_check("ok", label, "%s matches the size and hash listed." % fname))
        else:
            rep["checks"].append(_check("bad", label, "%s does not match what the manifest lists (%s)." % (fname, "size" if not ok_size else "hash")))
            problems += 1
    if items:
        rep["summary"] += " All of them match." if not problems else " %d need a look." % problems
    return rep


def _dir_report(path, keys):
    names = sorted(os.listdir(path))
    manifests = [n for n in names if n.endswith(".manifest")]
    rep = _report("folder", os.path.basename(os.path.abspath(path)) or path, "A folder with %d files." % len(names))
    for m in manifests:
        with open(os.path.join(path, m), "r", errors="replace") as fh:
            rep["parts"].append(_manifest_report(os.path.join(path, m), fh.read()))
    if manifests:
        rep["summary"] = "A release folder: %s." % ", ".join(manifests)
        rep["next"] = [{"tool": "server", "label": "Serve it", "why": "Host this folder for a device on your network."}]
    for n in names:
        full = os.path.join(path, n)
        if os.path.isfile(full) and n.endswith((".cat", ".kitt")):
            with open(full, "rb") as fh:
                d = fh.read()
            if d[:4] == b"RRUP":
                rep["parts"].append(_image_report(d, keys, n))
    return rep


def inspect_path(path, extra_key_dirs=()):
    """The report for a file or folder."""
    keys = keybag.load_all(extra_key_dirs)
    if os.path.isdir(path):
        return _dir_report(path, keys)
    with open(path, "rb") as fh:
        data = fh.read()
    name = os.path.basename(path)
    if data[:4] == b"RRUP" and len(data) >= formats.image().HEADER_SIZE:
        return _image_report(data, keys, name)
    if len(data) >= 0x10000 and (data[0] == ESP_MAGIC or data[0xA000:0xA002] == b"\xaa\x50" or data[0x8000:0x8002] == b"\xaa\x50"):
        return flash_report(data, keys)
    if data[:4] == struct.pack("<I", formats.purrcfg().CFG_MAGIC):
        r = _cfg_report(data)
        r["title"] = name
        return r
    if data.startswith(b"-----BEGIN"):
        return _report("key", name, "A key file (%s)." % data.split(b"\n", 1)[0].decode("ascii", "replace").strip("- "))
    if len(data) == 64:
        return _report("key", name, "64 raw bytes: a public key (X || Y), fingerprint %s." % keybag.fingerprint(data))
    if name.endswith(".manifest"):
        return _manifest_report(path, data.decode("utf-8", "replace"))
    if data[:1] == bytes([ESP_MAGIC]):
        r = _report("esp-app", name, "An ESP image of %s with no PURR wrapper (a raw bootloader or app)." % _size(len(data)))
        r["checks"].append(_check("info", "Wrapper", "Raw ESP images carry no PURR signature. A signed bootloader update is the .cat built with --bootloader."))
        return r
    return _report("unknown", name, "I do not recognise this file (%s). Drop a .cat, .kitt, flash image, manifest or release folder." % _size(len(data)))
