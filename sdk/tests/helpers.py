"""Shared test helpers: a throwaway key, synthetic images, and where the headers live."""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.dirname(HERE)
ROOT = os.path.dirname(SDK)
sys.path.insert(0, SDK)

from purrsdk import formats  # noqa: E402

try:
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec, utils
    HAVE_CRYPTO = True
except ImportError:
    HAVE_CRYPTO = False

INCLUDES = [os.path.join(ROOT, "PurrOS", "components", "kernel", "include"), os.path.join(ROOT, "PurrOS", "components", "coreos", "include")]


def clang():
    return shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")


def tmpdir():
    return tempfile.mkdtemp(prefix="purrsdk-test-")


class Key:
    """A P-256 key that can sign a PURR image header."""

    def __init__(self):
        self.priv = ec.generate_private_key(ec.SECP256R1())
        n = self.priv.public_key().public_numbers()
        self.pub = n.x.to_bytes(32, "big") + n.y.to_bytes(32, "big")

    def sign_image(self, data, key_id):
        img = formats.image()
        fields = list(img.parse_header(bytes(data)))
        fields[img.IX_KEY_ID] = key_id
        fields[img.IX_SIGNATURE] = b"\0" * 64
        import struct
        hdr = struct.pack(img.HEADER_FORMAT, *fields)
        der = self.priv.sign(img.sha256(hdr[:img.SIGNED_LEN]), ec.ECDSA(utils.Prehashed(hashes.SHA256())))
        r, s = utils.decode_dss_signature(der)
        fields[img.IX_SIGNATURE] = r.to_bytes(32, "big") + s.to_bytes(32, "big")
        out = bytearray(data)
        out[:img.HEADER_SIZE] = struct.pack(img.HEADER_FORMAT, *fields)
        return bytes(out)


def make(name="demo", version="1.2.3", image_type=3, subtype=5, payload=b"hello payload"):
    img = formats.image()
    return img.make_image(name, version, image_type, subtype, payload, img.CHIP_ESP32S3)


def keybag_dir(role, key_id, key):
    d = tmpdir()
    with open(os.path.join(d, "%s_%d.pub.bin" % (role, key_id)), "wb") as fh:
        fh.write(key.pub)
    return d
