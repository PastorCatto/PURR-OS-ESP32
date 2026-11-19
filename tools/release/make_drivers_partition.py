#!/usr/bin/env python3
"""Build the `drivers` partition image of a board from the source tree: each essential driver compiled, signed with the developer key, and packed into ten 8 KB slots.

    PURRSTRAP_KEY_PASSWORD=... python3 tools/release/make_drivers_partition.py --board tdeck_plus --key signing_keys/developer.key --key-id 4 --out out/drivers-tdeck_plus.bin

The image goes at the `drivers` partition's offset (0xFEC000 in flash map v3); tools/hw/make_hw_image.sh and tools/vm/mkimage_dbg.sh take it as DRIVERS_FROM, the SDK's image maker finds it as
drivers-<board>.bin. It is the same code the SDK runs (sdk/purrsdk/drivers.py); the passphrase comes from the environment of this one command and is never written anywhere.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "sdk"))

from purrsdk import drivers, minitoml, runner  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--board", required=True)
    ap.add_argument("--key", required=True, help="a developer-role private key (a driver is a module: the boot key may not sign one)")
    ap.add_argument("--key-id", type=int, default=4)
    ap.add_argument("--out", required=True)
    ap.add_argument("--version", default="1.0.0")
    a = ap.parse_args()
    toml_path = os.path.join(ROOT, "sdk", "boards", a.board + ".toml")
    toml = minitoml.load(toml_path) if os.path.isfile(toml_path) else None
    try:
        files = drivers.board_files(a.board, toml)
    except drivers.DriverError as e:
        sys.exit(str(e))
    password = os.environ.get("PURRSTRAP_KEY_PASSWORD")
    r = drivers.build(a.board, ROOT, os.path.abspath(a.key), a.key_id, password, os.path.abspath(a.out), a.version, files=files,
                      run=lambda args, extra_env=None, password=None: runner.purrstrap(args, password=password, on_line=print, extra_env=extra_env))
    if not r["ok"]:
        sys.exit(r["problem"])
    print("wrote %s: %s" % (a.out, ", ".join(r["files"])))


if __name__ == "__main__":
    main()
