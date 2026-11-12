"""Shared test helpers: a throwaway key, synthetic images, and where the headers live."""
import os
import shutil
import stat
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


# The name the SDK looks for when it wants the emulator's headless runner. On Windows that is headless_tap.exe, which a test cannot fake with a script,
# so a .cmd that starts a Python file stands in (the tests point emulator._HEADLESS_NAMES at it).
RUNNER_NAME = "headless_tap.cmd" if os.name == "nt" else "headless_tap"


def write_fake_runner(root, text):
    """Write a fake headless runner (the text is a Python program) into root; returns the file the SDK should start."""
    exe = os.path.join(root, RUNNER_NAME)
    if os.name == "nt":
        with open(os.path.join(root, "headless_tap.py"), "w") as fh:
            fh.write(text)
        with open(exe, "w") as fh:
            fh.write('@"%s" "%%~dp0headless_tap.py" %%*\r\n' % sys.executable)
        return exe
    with open(exe, "w") as fh:
        fh.write(text)
    os.chmod(exe, os.stat(exe).st_mode | stat.S_IXUSR)
    return exe


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
