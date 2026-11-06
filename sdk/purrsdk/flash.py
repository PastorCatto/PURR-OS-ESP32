"""Flashing: write an image to a board over USB with esptool (bundled in the kit's Python)."""

import glob
import os
import subprocess
import sys

from . import inspector, runner


def list_ports():
    pats = ["/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/ttyACM*", "/dev/ttyUSB*"]
    found = []
    for p in pats:
        found += glob.glob(p)
    try:
        from serial.tools import list_ports as lp
        found += [p.device for p in lp.comports()]
    except Exception:
        pass
    return sorted(set(found))


def have_esptool():
    exe = os.environ.get("PURR_PYTHON") or sys.executable
    try:
        return subprocess.call([exe, "-c", "import esptool"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0
    except OSError:
        return False


def layout_note(image, offset="0x0"):
    """For a whole flash image written at 0x0: a sentence when its partition table is not the board's layout (flash map v2, release 1.0.29), else None.
    Moving a device from one layout to another takes a whole image written over USB (the partition table is never replaced by an update)."""
    try:
        if int(str(offset), 0) != 0:
            return None
        with open(image, "rb") as fh:
            data = fh.read()
    except (OSError, ValueError):
        return None
    table_at = next((o for o in (0xA000, 0x8000) if len(data) > o + 32 and data[o:o + 2] == b"\xaa\x50"), None)
    if table_at is None:
        return None
    lay = inspector._layout_check(inspector._parse_partitions(data, table_at), len(data))
    return lay["text"] if lay and lay["status"] != "ok" else None


def write(port, image, offset="0x0", baud=460800, on_line=None, chip="esp32s3"):
    """Returns {"ok", "log"}. The caller has already checked that the person means it."""
    if not os.path.isfile(image):
        return {"ok": False, "log": "I cannot find %s." % image}
    if not have_esptool():
        return {"ok": False, "log": "esptool is not installed in this Python. The kit's own Python has it; otherwise: pip install esptool."}
    exe = os.environ.get("PURR_PYTHON") or sys.executable
    cmd = [exe, "-m", "esptool", "--chip", chip, "--port", port, "--baud", str(baud), "write_flash", str(offset), image]
    log = []
    note = layout_note(image, offset)
    if note:
        log.append("Note: " + note)
        if on_line:
            on_line("Note: " + note)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, errors="replace", stdin=subprocess.DEVNULL)
    for line in proc.stdout:
        line = line.rstrip("\n")
        # esptool redraws its progress line with carriage returns; keep the last piece of each
        for piece in line.split("\r"):
            if piece.strip():
                log.append(piece)
                if on_line:
                    on_line(piece)
    return {"ok": proc.wait() == 0, "log": "\n".join(log)}
