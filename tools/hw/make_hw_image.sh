#!/bin/zsh
# A full 16 MB flash image for a real T-Deck Plus, from the release folder (tools/release/make_release.py) and the current bootloader.
#
#   PURRSTRAP_KEY_PASSWORD=... tools/hw/make_hw_image.sh [RELEASE_FOLDER] [OUT]
#
# A BLANK device that starts in the recovery loader once: bootloader, partition table, trust block (enforce, signed, developer mode
# OFF, FRESH on, "boot the loader once"), boot package, recovery loader, and the UI assets. The kernel and KittenOS slots, the
# filesystem and the saved network are left erased, so the loader asks to recover from the internet (the official release).
set -e
ROOT=${0:A:h:h:h}
cd $ROOT
REL=${1:-$HOME/Desktop/PURR-OS-release}
OUT=${2:-$ROOT/out/hw/tdeck_plus-blank-flash.bin}
PY=$HOME/.espressif/python_env/idf5.3_py3.9_env/bin/python
ps() { $PY purrstrap/purrstrap.py "$@"; }
BL=$ROOT/out/boot_tdeck_plus/bootloader/bootloader.bin
PT=$ROOT/out/stdfull/partition_table/partition-table.bin
UIA=${UI_ASSETS_FROM:-$HOME/Projects/ESP32-Emulator/Testrun/vm-secure/flash.bin.prev}     # the ui_assets partition of a known-good image
W=$ROOT/out/hw/work
mkdir -p $W ${OUT:h}
for f in $BL $PT $REL/JumpingJaguar1/loader_tdeck_plus.cat $REL/JumpingJaguar1/bootpkg_tdeck_plus.cat $UIA; do
  [[ -f $f ]] || { echo "missing $f"; exit 1; }
done
ps coreos slot-image --file $REL/JumpingJaguar1/loader_tdeck_plus.cat --board tdeck_plus --slot loader --out $W/loader_slot.bin >/dev/null
# DEV_RECOVERY=1 (the default while debugging) sets the bootloader's DEV_RECOVERY flag, so recovery offers the dev server (developer mode comes on with it). Release images: build with DEV_RECOVERY=0.
tb=(--mode enforce --force-loader)
[[ ${DEV_RECOVERY-1} == 0 ]] || tb+=(--dev-recovery --dev-mode)      # the bootloader honours DEV_RECOVERY only while developer mode is on, and a blank device has no KittenOS to switch it on
ps keys trust-block --key signing_keys/boot.key $tb --out $W/purrcfg.bin >/dev/null
$PY - "$OUT" "$W" "$BL" "$PT" "$REL" "$UIA" <<'PYEOF'
import sys
out, w, bl, pt, rel, uia = sys.argv[1:7]
img = bytearray(b"\xff" * (16 * 1024 * 1024))
def put(off, path, limit, skip=0):
    d = open(path, "rb").read()[skip:]
    assert len(d) <= limit, "%s is %d bytes, room for %d" % (path, len(d), limit)
    img[off:off + len(d)] = d
put(0x0, bl, 0xA000)
put(0xA000, pt, 0x1000)
put(0xF000, w + "/purrcfg.bin", 0x2000)
put(0x12000, rel + "/JumpingJaguar1/bootpkg_tdeck_plus.cat", 0xE000)
put(0x380000, w + "/loader_slot.bin", 0x180000)
ui = open(uia, "rb").read()[0x501000:0x501000 + 0x500000]
assert len(ui) == 0x500000
img[0x501000:0x501000 + 0x500000] = ui
open(out, "wb").write(img)
print("wrote", out, len(img), "bytes")
PYEOF
