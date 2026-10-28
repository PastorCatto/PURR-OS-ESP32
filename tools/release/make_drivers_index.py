#!/usr/bin/env python3
"""Builds drivers.manifest (the driver index "Find hardware" and the update page read) from signed driver files. Standard library only.

    python3 tools/release/make_drivers_index.py --out DIR --driver out/drivers/bme280.cat:bme280:i2c:sensor [--driver ...] [--board tdeck_plus]
                                                [--abi-min 4] [--abi-max 0] [--key developer]

Copies each file into DIR as <name>-<board>.cat and writes DIR/drivers.manifest next to them (upload both to the same release as modules.manifest, JumpingJaguar2).
Each --driver is FILE:COMPATIBLE:BUSES:CATCALL. The name and version come from the file's own header. abi_min/abi_max are the driver bus
ABIs the driver works with (purr_driver_bus.h: the kernel serves PURR_DRIVER_BUS_ABI_MIN up to its current version; abi_max 0 = no upper
bound). A version found broken on a newer OS can be listed again with `blocked=yes` (edit the manifest by hand): it is never offered.
"""
import argparse
import hashlib
import os
import shutil
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "purrstrap"))
from lib import image as pimg  # noqa: E402

CHIPS = {pimg.CHIP_ANY: "any", pimg.CHIP_ESP32: "esp32", pimg.CHIP_ESP32S3: "esp32s3"}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--driver", action="append", default=[], help="FILE:COMPATIBLE:BUSES:CATCALL")
    ap.add_argument("--board", default="tdeck_plus")
    ap.add_argument("--abi-min", type=int, default=4)
    ap.add_argument("--abi-max", type=int, default=0)
    ap.add_argument("--key", default="developer", help="the role that signed them")
    a = ap.parse_args()
    if not a.driver:
        ap.error("give at least one --driver")
    os.makedirs(a.out, exist_ok=True)
    lines = ["release=drivers-%s" % time.strftime("%Y%m%d"), "released=%s" % time.strftime("%Y-%m-%d"), ""]
    for spec in a.driver:
        try:
            path, compatible, buses, catcall = spec.split(":")
        except ValueError:
            sys.exit("bad --driver %r: want FILE:COMPATIBLE:BUSES:CATCALL" % spec)
        data = open(path, "rb").read()
        if not pimg.is_purr_image(data) or len(data) < pimg.HEADER_SIZE:
            sys.exit("%s is not a PURR image" % path)
        f = pimg.header_fields(pimg.parse_header(data))
        if f["image_type"] != pimg.IMG_MODULE or (f["flags"] & 0xFF) != pimg.MOD_DRIVER:
            sys.exit("%s is not a driver module" % path)
        if f["key_id"] == 0:
            print("note: %s is unsigned: only a device in developer mode will load it" % path)
        name = f["name"]
        fname = "%s-%s.cat" % (name, a.board)
        shutil.copyfile(path, os.path.join(a.out, fname))
        lines += ["component=%s" % name, "type=driver", "version=%s" % f["version"], "chip=%s" % CHIPS.get(f["chip_id"], "any"),
                  "board=%s" % a.board, "file=%s" % fname, "size=%d" % len(data), "sha256=%s" % hashlib.sha256(data).hexdigest(),
                  "key=%s" % a.key, "compatible=%s" % compatible, "buses=%s" % buses, "catcall=%s" % catcall,
                  "abi_min=%d" % a.abi_min] + (["abi_max=%d" % a.abi_max] if a.abi_max else []) + [""]
        print("added %s %s (%d bytes)" % (name, f["version"], len(data)))
    with open(os.path.join(a.out, "drivers.manifest"), "w") as fh:
        fh.write("\n".join(lines))
    print("wrote %s" % os.path.join(a.out, "drivers.manifest"))


if __name__ == "__main__":
    main()
