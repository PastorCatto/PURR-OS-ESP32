#!/bin/zsh
# Make the VM image from a vmdbg kernel build. Developer mode is off unless DEV_MODE=1.
#
# It patches a base flash image rather than rebuilding every slot: the bootloader, partition table, boot package,
# loader and KittenOS slots are taken from BASE, and only the bootloader, the boot package, the kernel slot and the trust block are replaced. So it
# needs just the kernel build (cmake -B $BUILD ... sdkconfig.flavor.vmdebug, see the notes in this folder).
#
#   PURRSTRAP_KEY_PASSWORD=... tools/vm/mkimage_dbg.sh [BUILD_DIR] [BASE_IMAGE] [OUT_IMAGE]
#
# The layout is flash map v2 (release 1.0.29, plan1.md): no picture partition, `root` (LittleFS) at 0x5C1000 for 0xA3F000 bytes, to the end of the 16 MB flash.
# The script writes the build's partition table ($BUILD/partition_table/partition-table.bin, or PT_FROM) to 0xA000 in EVERY scenario. If the BASE image carried
# a different table (an older layout: a 5 MB picture partition and a root at 0xA01000, or the first flash map v2 draft), everything from 0x20000 to the end is erased (the slots,
# netrec, root and the old picture area; the slots are then written again below), and it says so: old LittleFS or picture data must never be read through the new table. With the table already the new one, `root` is left alone unless the
# scenario is first or auto. Written for the new layout and not yet run.
#
# Defaults: BUILD_DIR=out/vmdbg  BASE_IMAGE=~/Projects/ESP32-Emulator/Testrun/vm-secure/flash.bin.prev-or-flash.bin
#           OUT_IMAGE=out/vm-debug-flash.bin. The passphrase is read from the environment of this one command only.
set -e
ROOT=${0:A:h:h:h}
cd $ROOT
BUILD=${1:-$ROOT/out/vmdbg}
BASE=${2:-$HOME/Projects/ESP32-Emulator/Testrun/vm-secure/flash.bin}
OUT=${3:-$ROOT/out/vm-debug-flash.bin}
W=$ROOT/out/secdbg
PY=$HOME/.espressif/python_env/idf5.3_py3.9_env/bin/python
ps() { $PY purrstrap/purrstrap.py "$@"; }
[[ -f $BUILD/purros.bin ]] || { echo "no $BUILD/purros.bin: build the vmdbg kernel first"; exit 1; }
[[ -f $BASE ]] || { echo "no base image $BASE"; exit 1; }
PT=${PT_FROM:-$BUILD/partition_table/partition-table.bin}
[[ -f $PT ]] || { echo "no partition table $PT: build the vmdbg kernel (it writes the flash map v2 table), or give PT_FROM"; exit 1; }
mkdir -p $W ${OUT:h}
ps bootpkg build --board tdeck_plus >/dev/null
ps keys sign --key signing_keys/boot.key --image bootpkg/build/tdeck_plus/bootpkg.bin --out $W/bootpkg.signed --key-id 1 >/dev/null
ps coreos package-component --component kernel --file $BUILD/purros.bin --version 1.0.0 --board tdeck_plus --out $W/kernel.cat >/dev/null
ps keys sign --key signing_keys/boot.key --image $W/kernel.cat --key-id 1 >/dev/null
ps coreos slot-image --file $W/kernel.cat --board tdeck_plus --slot kernel --out $W/kernel_slot.bin >/dev/null
# SCENARIO=normal (default): boots the OS. recover: the recovery loader starts once (as if asked), the system untouched.
# first: a blank device (FRESH on, kernel and KittenOS erased) that starts in the loader: the "first time?" path.
SCENARIO=${SCENARIO:-normal}
# Developer mode is OFF unless DEV_MODE=1 (the debug kernel's automatic sign-in and the dev-server recovery page need it).
tb=(--key signing_keys/boot.key --mode enforce)
[[ -z $DEV_MODE ]] || tb+=(--dev-mode)
[[ $SCENARIO == first || $SCENARIO == auto ]] || tb+=(--installed)
[[ $SCENARIO == auto ]] && tb+=(--auto-reinstall)                    # auto: the loader installs unattended (a blank device)
[[ $SCENARIO == kitten ]] && tb+=(--force-recovery)                  # kitten: KittenOS starts once
[[ $SCENARIO == normal || $SCENARIO == kitten ]] || tb+=(--force-loader)
[[ -z $DEV_RECOVERY ]] || tb+=(--dev-recovery)          # DEV_RECOVERY=1: recovery may pull from a dev server (the extra setup page)
ps keys trust-block $tb --out $W/purrcfg.bin >/dev/null
# The recovery loader and KittenOS slots are rebuilt too when their builds exist (out/vmmin, out/vmrec).
LOADER=$ROOT/out/vmmin/purros.bin; KITTEN=$ROOT/out/vmrec/purros.bin
if [[ -f $LOADER && -f $KITTEN ]]; then
  ps coreos package-component --component loader --file $LOADER --version 1.0.0 --board tdeck_plus --out $W/loader.cat >/dev/null
  ps keys sign --key signing_keys/boot.key --image $W/loader.cat --key-id 1 >/dev/null
  ps coreos slot-image --file $W/loader.cat --board tdeck_plus --slot loader --out $W/loader_slot.bin >/dev/null
  $PY - "$W" "$KITTEN" <<'PYEOF'
import sys
sys.path[:0]=["purrstrap","purrstrap/scripts","purrstrap/lib"]
from coreos import pimg, CHIP_IDS, BOARDS, PACKAGE_KIND
t,name=PACKAGE_KIND["recovery"]
open(sys.argv[1]+"/kittenos.kitt","wb").write(pimg.make_image(name,"1.0.0",t,0,open(sys.argv[2],"rb").read(),CHIP_IDS[BOARDS["tdeck_plus"]]))
PYEOF
  ps keys sign --key signing_keys/boot.key --image $W/kittenos.kitt --key-id 1 >/dev/null
  ps coreos slot-image --file $W/kittenos.kitt --board tdeck_plus --slot kittenos --out $W/kittenos_slot.bin >/dev/null
fi
ps keys verify --key signing_keys/boot.pub --image $W/kernel.cat | tail -1 | cut -c1-90
BOOTLOADER=${BOOTLOADER:-$ROOT/out/boot_tdeck_plus/bootloader/bootloader.bin}      # built from bootloader/, see README
$PY - "$BASE" "$OUT" "$W" "$BOOTLOADER" "$SCENARIO" "$PT" <<'PYEOF'
import os, struct, sys
base, out, w, bl, scenario, pt = sys.argv[1:7]
img = bytearray(open(base, "rb").read())
newtab = open(pt, "rb").read()
assert len(img) == 16 * 1024 * 1024, "%s is not a 16 MB image" % base
assert len(newtab) <= 0x1000, "%s is not a partition table" % pt
newtab = newtab.ljust(0x1000, b"\xff")
rows = {}
for i in range(0, len(newtab) - 31, 32):                  # the new table must be flash map v2: no picture partition, root to the end of the flash
    if newtab[i:i + 2] != b"\xaa\x50":
        break
    _t, _s, o, z = struct.unpack_from("<BBII", newtab, i + 2)
    rows[newtab[i + 12:i + 28].split(b"\0")[0].decode()] = (o, z)
if "ui_assets" in rows or rows.get("root") != (0x5C1000, 0xA3F000):
    sys.exit("%s is not the flash map v2 table (root must be 0x5C1000 for 0xA3F000 bytes and there is no ui_assets)" % pt)
relayout = bytes(img[0xA000:0xB000]) != newtab                  # the base image has another (old) table: its root and picture data cannot be kept
img[0xA000:0xB000] = newtab
if relayout:
    print("the base image has a different partition table (an older layout): erasing everything from the kittenos slot (0x20000) to the end, so no slot, netrec, root or old picture data is read through the new table (the loader and KittenOS slots are written again only when out/vmmin and out/vmrec exist)")
    img[0x20000:] = b"\xff" * (len(img) - 0x20000)
def put(off, path, limit):
    d = open(path, "rb").read()
    assert len(d) <= limit, "%s is %d bytes, slot holds %d" % (path, len(d), limit)
    img[off:off + limit] = b"\xff" * limit
    img[off:off + len(d)] = d
if os.path.exists(bl):
    put(0x0, bl, 0xA000)                                  # the bootloader, up to the partition table at 0xA000
put(0xF000, w + "/purrcfg.bin", 0x2000)     # up to the boot package at 0x12000
put(0x12000, w + "/bootpkg.signed", 0xE000)     # the boot package (menu and boot screens)
put(0x1E0000, w + "/kernel_slot.bin", 0x220000)
if os.path.exists(w + "/loader_slot.bin"):
    put(0x400000, w + "/loader_slot.bin", 0x1C0000)
    put(0x20000, w + "/kittenos_slot.bin", 0x1C0000)
if scenario in ("first", "auto"):                                   # a blank device: nothing installed, and a blank root filesystem
    img[0x5C1000:0x5C1000 + 0xA3F000] = b"\xff" * 0xA3F000           # root, from 0x5C1000 to the end of the flash
    img[0x5C0000:0x5C0000 + 0x1000] = b"\xff" * 0x1000     # no remembered network either
    img[0x1E0000:0x1E0000 + 0x220000] = b"\xff" * 0x220000
    img[0x20000:0x20000 + 0x1C0000] = b"\xff" * 0x1C0000
open(out, "wb").write(img)
print("wrote", out, len(img), "bytes")
PYEOF
