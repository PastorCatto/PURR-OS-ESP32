"""ECDSA P-256 signature checking in pure Python, so looking at an image needs no extra packages.

Only verification lives here (it is public maths on public data). Signing uses `cryptography`
(see keys.py), because a private key deserves a real, constant-time library.
"""

import hashlib

P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
A = P - 3
B = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
G = (0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296,
     0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5)


def _egcd_inv(a, m):
    lo, hi = a % m, m
    x0, x1 = 1, 0
    while hi:
        q = lo // hi
        lo, hi = hi, lo - q * hi
        x0, x1 = x1, x0 - q * x1
    return x0 % m


try:
    pow(3, -1, 7)

    def _inv(x, m):
        return pow(x, -1, m)
except ValueError:                    # Python before 3.8
    _inv = _egcd_inv


def _add(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    x1, y1 = p1
    x2, y2 = p2
    if x1 == x2:
        if (y1 + y2) % P == 0:
            return None
        lam = (3 * x1 * x1 + A) * _inv(2 * y1, P) % P
    else:
        lam = (y2 - y1) * _inv(x2 - x1, P) % P
    x3 = (lam * lam - x1 - x2) % P
    return x3, (lam * (x1 - x3) - y1) % P


def _mul(k, point):
    result = None
    while k:
        if k & 1:
            result = _add(result, point)
        point = _add(point, point)
        k >>= 1
    return result


def on_curve(x, y):
    return 0 <= x < P and 0 <= y < P and (y * y - (x * x * x + A * x + B)) % P == 0


def verify_hash(pub64, digest32, sig64):
    """True when `sig64` (raw r || s) is a valid signature of the 32-byte `digest32` by the
    public key `pub64` (raw X || Y)."""
    if len(pub64) != 64 or len(sig64) != 64 or len(digest32) != 32:
        return False
    x = int.from_bytes(pub64[:32], "big")
    y = int.from_bytes(pub64[32:], "big")
    r = int.from_bytes(sig64[:32], "big")
    s = int.from_bytes(sig64[32:], "big")
    if not on_curve(x, y) or not (1 <= r < N and 1 <= s < N):
        return False
    z = int.from_bytes(digest32, "big")
    w = _inv(s, N)
    point = _add(_mul(z * w % N, G), _mul(r * w % N, (x, y)))
    return point is not None and point[0] % N == r


def verify(pub64, message, sig64):
    return verify_hash(pub64, hashlib.sha256(message).digest(), sig64)
