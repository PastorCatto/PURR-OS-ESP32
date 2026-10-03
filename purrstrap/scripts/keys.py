"""The key tool: generate, sign and verify PURR image containers (Keys/SPEC.md section 3).

Core signing only for this pass: generate, export-public, sign, verify, inspect. Vendor
certificates and revocation (section 4) are a later chunk.

Uses the `cryptography` package, a requirement of this subscript alone.
"""

import getpass
import glob
import os
import struct
import sys

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils

from lib import image as pimg
from lib.model import Action, Param, Script

# The container layout lives in lib/image.py, shared with bootpkg and coreos. Aliased here
# under their original names, since other modules (test_keys.py) reference them this way.
IMAGE_MAGIC = pimg.MAGIC
HEADER_FORMAT = pimg.HEADER_FORMAT
HEADER_SIZE = pimg.HEADER_SIZE
SIGNED_LEN = pimg.SIGNED_LEN
sha256 = pimg.sha256
parse_header = pimg.parse_header
header_fields = pimg.header_fields
is_purr_image = pimg.is_purr_image

ROLES = {"boot": 1, "system": 2, "owner": 3, "developer": 4, "vendor": 5}


# -- small helpers -------------------------------------------------------
def get_password(ctx, prompt, confirm=False):
    env = os.environ.get("PURRSTRAP_KEY_PASSWORD")
    if env is not None:
        return env
    if not sys.stdin.isatty():
        ctx.error("no PURRSTRAP_KEY_PASSWORD and no terminal to ask on. "
                  "Set the environment variable, or run this interactively.")
        return None
    pw = getpass.getpass(prompt)
    if confirm and getpass.getpass("Confirm password: ") != pw:
        ctx.error("passwords did not match")
        return None
    return pw


def raw_pub(public_key):
    """The 64 raw bytes (X || Y) the device expects, from a P-256 public key."""
    nums = public_key.public_numbers()
    return nums.x.to_bytes(32, "big") + nums.y.to_bytes(32, "big")


def public_key_from_raw(raw64):
    x = int.from_bytes(raw64[:32], "big")
    y = int.from_bytes(raw64[32:], "big")
    return ec.EllipticCurvePublicNumbers(x, y, ec.SECP256R1()).public_key()


def load_public_key(path):
    """A PEM SubjectPublicKeyInfo file, or 64 raw bytes."""
    with open(path, "rb") as fh:
        data = fh.read()
    if data.startswith(b"-----BEGIN"):
        return serialization.load_pem_public_key(data)
    if len(data) == 64:
        return public_key_from_raw(data)
    raise ValueError(f"{path}: not a PEM public key or 64 raw bytes")


def raw_sig_to_der(raw_sig):
    r = int.from_bytes(raw_sig[:32], "big")
    s = int.from_bytes(raw_sig[32:], "big")
    return utils.encode_dss_signature(r, s)


def der_sig_to_raw(der_sig):
    r, s = utils.decode_dss_signature(der_sig)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


# -- generate --------------------------------------------------------------
def generate(ctx, role, out, key_id, force=False):
    if role not in ROLES:
        ctx.error(f"unknown role {role!r}. Known roles: {', '.join(ROLES)}")
        return 1
    key_path = os.path.join(out, f"{role}.key")
    pub_path = os.path.join(out, f"{role}.pub")
    # F-19: this used to overwrite an existing key pair with no warning -- the old private
    # key (and anything only it could sign for) is gone the moment the new file lands,
    # with no undo. --force is the explicit, typed-out way to mean it.
    if not force and (os.path.exists(key_path) or os.path.exists(pub_path)):
        ctx.error(f"{key_path} already exists. Pass --force to replace it (and its "
                 "matching .pub) -- anything only the old key could sign for is lost.")
        return 1
    pw = get_password(ctx, f"Password for the new {role} key: ", confirm=True)
    if pw is None:
        return 1
    if not pw:
        ctx.error("an empty password protects nothing. Refusing.")
        return 1

    private_key = ec.generate_private_key(ec.SECP256R1())
    os.makedirs(out, exist_ok=True)

    with open(key_path, "wb") as fh:
        fh.write(private_key.private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
            serialization.BestAvailableEncryption(pw.encode())))
    with open(pub_path, "wb") as fh:
        fh.write(private_key.public_key().public_bytes(
            serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))

    fp = sha256(raw_pub(private_key.public_key()))[:8].hex()
    ctx.ok(f"{role} key pair generated (id {key_id}, fingerprint {fp})")
    ctx.info(f"    private (password-protected): {key_path}")
    ctx.info(f"    public:                       {pub_path}")
    ctx.info("Keep the private key offline. It is never needed to verify anything.")
    return 0


# -- export-public -----------------------------------------------------
def export_public(ctx, pub, role, key_id, out):
    """Writes one key's raw bytes into `out`, then regenerates the default key bag C
    source from every *_<id>.pub.bin file already there, so keys can be added one at a
    time (generate one role, export it, repeat) without the tool remembering state."""
    if role not in ROLES:
        ctx.error(f"unknown role {role!r}. Known roles: {', '.join(ROLES)}")
        return 1
    if not (0 <= key_id <= 31):
        ctx.error("key id must be 0..31 (the revocation mask is 32 bits)")
        return 1
    try:
        pubkey = load_public_key(pub)
    except (ValueError, OSError) as e:
        ctx.error(f"{pub}: {e}")
        return 1

    os.makedirs(out, exist_ok=True)
    raw = raw_pub(pubkey)
    bin_path = os.path.join(out, f"{role}_{key_id}.pub.bin")
    with open(bin_path, "wb") as fh:
        fh.write(raw)
    ctx.ok(f"{role} (id {key_id}) exported: {bin_path}")

    entries = []
    for path in sorted(glob.glob(os.path.join(out, "*_*.pub.bin"))):
        base = os.path.basename(path)[:-len(".pub.bin")]
        found_role, _, id_s = base.rpartition("_")
        if found_role not in ROLES or not id_s.isdigit():
            continue                                # something else living in this directory
        with open(path, "rb") as fh:
            data = fh.read()
        if len(data) != 64:
            ctx.warn(f"{path}: not 64 bytes, skipped")
            continue
        entries.append((found_role, int(id_s), data))
    entries.sort(key=lambda e: e[1])

    c_path = os.path.join(out, "purr_default_keys.c")
    with open(c_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("/* Generated by purrstrap keys export-public. Do not edit by hand. */\n")
        fh.write('#include "purr_keybag.h"\n\n')
        fh.write("const purr_key_t purr_default_keys[] = {\n")
        for erole, eid, edata in entries:
            hexbytes = ", ".join(f"0x{b:02x}" for b in edata)
            fh.write(f"    {{.key_id = {eid}, .role = PURR_ROLE_{erole.upper()},\n"
                     f"     .pub = {{{hexbytes}}}}},\n")
        fh.write("};\n\n")
        fh.write("const size_t purr_default_keys_count = "
                 "sizeof(purr_default_keys) / sizeof(purr_default_keys[0]);\n")
    ctx.info(f"default key bag regenerated from {len(entries)} key(s): {c_path}")
    return 0


# -- sign ----------------------------------------------------------------
def sign(ctx, key, key_id, image_in, image_out):
    if not os.path.isfile(image_in):
        ctx.error(f"{image_in}: no such file")
        return 1
    pw = get_password(ctx, f"Password for {key}: ")
    if pw is None:
        return 1
    try:
        with open(key, "rb") as fh:
            key_bytes = fh.read()
        private_key = serialization.load_pem_private_key(key_bytes, pw.encode())
    except (ValueError, TypeError) as e:
        ctx.error(f"could not load {key}: {e}")
        return 1

    with open(image_in, "rb") as fh:
        data = bytearray(fh.read())
    if not is_purr_image(data):
        ctx.error(f"{image_in}: not a PURR image (bad magic)")
        return 1
    if len(data) < HEADER_SIZE:
        ctx.error(f"{image_in}: shorter than a header")
        return 1
    # Fields, in HEADER_FORMAT order: magic, header_version, header_size, chip_id,
    # image_type, key_id, flags, name, version, min_boot_version, payload_offset,
    # payload_size, payload_sha256, signature. Indices below rely on that order, not on
    # hand-computed byte offsets, so a layout change cannot silently corrupt the wrong bytes.
    KEY_ID_IX, PAYLOAD_SHA_IX, SIG_IX = 5, 12, 13
    fields = list(parse_header(bytes(data)))
    f = header_fields(tuple(fields))
    end = f["payload_offset"] + f["payload_size"]
    if f["payload_offset"] > len(data) or end > len(data) or end < f["payload_offset"]:
        ctx.error(f"{image_in}: payload offset/size out of range")
        return 1

    payload = bytes(data[f["payload_offset"]:end])
    fields[PAYLOAD_SHA_IX] = sha256(payload)
    if key_id is not None:
        fields[KEY_ID_IX] = key_id & 0xFF

    # F-19: id 0 means "no key" to the device (purr_keybag.c) -- it refuses any image whose
    # header carries it. Signing one anyway used to succeed silently and produce a container
    # the device would reject with no clue why; this catches it at the only point that knows
    # a real id was never supplied, not just whatever --key-id happened to default to.
    if fields[KEY_ID_IX] == 0:
        ctx.error("key id 0 means \"no key\" to the device -- it would reject this image. "
                 "Pass --key-id matching the id this key was exported under "
                 "(keys export-public).")
        return 1

    fields[SIG_IX] = b"\0" * 64                  # the signature covers everything before it
    unsigned_header = struct.pack(HEADER_FORMAT, *fields)
    header_hash = sha256(unsigned_header[:SIGNED_LEN])
    der_sig = private_key.sign(header_hash, ec.ECDSA(utils.Prehashed(hashes.SHA256())))
    fields[SIG_IX] = der_sig_to_raw(der_sig)

    data[:HEADER_SIZE] = struct.pack(HEADER_FORMAT, *fields)
    dest = image_out or image_in
    with open(dest, "wb") as fh:
        fh.write(data)
    f = header_fields(parse_header(bytes(data)))
    ctx.ok(f"signed {f['name']} {f['version']} with key id {f['key_id']}")
    ctx.info(f"    {dest}")
    return 0


# -- verify ----------------------------------------------------------------
def verify(ctx, key, image):
    image_in = image
    if not os.path.isfile(image_in):
        ctx.error(f"{image_in}: no such file")
        return 1
    try:
        pub = load_public_key(key)
    except (ValueError, OSError) as e:
        ctx.error(f"{key}: {e}")
        return 1

    with open(image_in, "rb") as fh:
        data = fh.read()
    if not is_purr_image(data):
        ctx.error(f"{image_in}: not a PURR image (bad magic)")
        return 1
    f = header_fields(parse_header(data))
    end = f["payload_offset"] + f["payload_size"]
    if f["payload_offset"] > len(data) or end > len(data):
        ctx.error(f"{image_in}: payload offset/size out of range")
        return 1

    payload = data[f["payload_offset"]:end]
    if sha256(payload) != f["payload_sha256"]:
        ctx.error("payload hash does not match: the file is corrupt or was edited")
        return 1

    header_hash = sha256(data[:SIGNED_LEN])
    try:
        pub.verify(raw_sig_to_der(f["signature"]), header_hash,
                  ec.ECDSA(utils.Prehashed(hashes.SHA256())))
    except Exception:
        ctx.error(f"signature does not verify against {key}")
        return 1

    ctx.ok(f"{f['name']} {f['version']}: payload hash and signature both check out")
    return 0


# -- inspect ---------------------------------------------------------------
def inspect(ctx, path):
    if not os.path.isfile(path):
        ctx.error(f"{path}: no such file")
        return 1
    with open(path, "rb") as fh:
        data = fh.read()

    if is_purr_image(data):
        if len(data) < HEADER_SIZE:
            ctx.error("truncated: shorter than a header")
            return 1
        f = header_fields(parse_header(data))
        unsigned = f["signature"] == b"\0" * 64
        ctx.info(f"PURR image: {f['name']} {f['version']}")
        ctx.info(f"    header version {f['header_version']}, chip id {f['chip_id']}, "
                 f"image type {f['image_type']}, flags 0x{f['flags']:04x}")
        ctx.info(f"    min boot version: {f['min_boot_version'] or '(none)'}")
        ctx.info(f"    payload: offset {f['payload_offset']}, size {f['payload_size']}")
        ctx.info(f"    payload sha256: {f['payload_sha256'].hex()}")
        ctx.info(f"    key id: {f['key_id']}")
        ctx.info(f"    signature: {'none (unsigned)' if unsigned else f['signature'].hex()}")
        return 0

    if data.startswith(b"-----BEGIN PUBLIC KEY"):
        pub = serialization.load_pem_public_key(data)
        raw = raw_pub(pub)
        ctx.info("P-256 public key")
        ctx.info(f"    fingerprint: {sha256(raw)[:8].hex()}")
        ctx.info(f"    raw (64 bytes): {raw.hex()}")
        return 0

    if data.startswith(b"-----BEGIN ENCRYPTED PRIVATE KEY") or data.startswith(b"-----BEGIN PRIVATE KEY"):
        pw = get_password(ctx, f"Password for {path}: ")
        if pw is None:
            return 1
        try:
            priv = serialization.load_pem_private_key(data, pw.encode() if pw else None)
        except (ValueError, TypeError) as e:
            ctx.error(f"could not load: {e}")
            return 1
        raw = raw_pub(priv.public_key())
        ctx.info("P-256 private key")
        ctx.info(f"    public fingerprint: {sha256(raw)[:8].hex()}")
        return 0

    if len(data) == 64:
        ctx.info("64 raw bytes: treating as a public key (X || Y)")
        ctx.info(f"    fingerprint: {sha256(data)[:8].hex()}")
        ctx.info(f"    raw: {data.hex()}")
        return 0

    ctx.error(f"{path}: not a PURR image, a PEM key, or 64 raw bytes "
             "(a certificate is not supported yet)")
    return 1


# -- wiring ------------------------------------------------------------
def _sign(ctx, key, image, out, key_id):
    return sign(ctx, key, key_id if key_id else None, image, out or None)


SCRIPT = Script(
    name="keys",
    title="Keys",
    description="Generate, sign and verify PURR image containers.",
    requires=("cryptography",),
    actions=(
        Action("generate", "Generate", generate,
               help="create a P-256 key pair for a role",
               params=(Param("role", "choice", required=True, choices=tuple(ROLES),
                             help="who the key speaks for"),
                       Param("out", "path", required=True, help="directory to write the key files into"),
                       Param("key_id", "int", default=0, help="the key id to remember it by (0-31)"),
                       Param("force", "bool", default=False,
                             help="replace an existing key pair of this role -- anything only "
                                  "the old key could sign for is lost"))),
        Action("export-public", "Export a public key", export_public,
               help="write one key's raw bytes and regenerate the default key bag from "
                    "everything already exported to --out",
               params=(Param("pub", "path", required=True, help="the public key file to export"),
                       Param("role", "choice", required=True, choices=tuple(ROLES),
                             help="who the key speaks for"),
                       Param("key_id", "int", required=True, help="the key id (0-31), matching generate"),
                       Param("out", "path", required=True, help="directory to write into"))),
        Action("sign", "Sign", _sign,
               help="sign a PURR image container with a private key",
               params=(Param("key", "path", required=True, help="the private key file"),
                       Param("image", "path", required=True, help="the image to sign"),
                       Param("out", "path", default="", help="output path (default: overwrite --image)"),
                       Param("key_id", "int", default=0, help="set the header's key id (0 = leave as is)"))),
        Action("verify", "Verify", verify,
               help="check a PURR image's payload hash and signature",
               params=(Param("key", "path", required=True, help="a public key file, or 64 raw bytes"),
                       Param("image", "path", required=True, help="the image to check"))),
        Action("inspect", "Inspect", inspect,
               help="show a key or a PURR image's header",
               params=(Param("path", "path", required=True, help="the file to look at"),)),
    ),
)
