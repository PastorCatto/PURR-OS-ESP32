#!/usr/bin/env python3
"""
purrstrap interactive UI — wraps purrstrap.py with numbered menus.

Run with no arguments (or via purr.py) for a guided session.
All actions are translated into purrstrap.py CLI calls.
"""

import glob
import json
import os
import subprocess
import sys

os.system("")  # enable ANSI on Windows

C_RST  = "\033[0m"
C_BOLD = "\033[1m"
C_GRY  = "\033[90m"
C_RED  = "\033[91m"
C_GRN  = "\033[92m"
C_YLW  = "\033[93m"
C_CYN  = "\033[96m"
C_WHT  = "\033[97m"

SCRIPT_DIR   = os.path.dirname(os.path.abspath(__file__))
REPO_DIR     = os.path.dirname(SCRIPT_DIR)
PURRSTRAP_PY = os.path.join(SCRIPT_DIR, "purrstrap.py")
SOURCE_DIR   = os.path.join(REPO_DIR, "source")
DEVICES_DIR  = os.path.join(SOURCE_DIR, "devices")
CFG_FILE     = os.path.join(REPO_DIR, ".purrstrap")

def div(label=""):
    line = "─" * 52
    if label:
        pad = max(0, 52 - len(label) - 2)
        line = f"─ {label} " + "─" * pad
    print(f"{C_GRY}{line}{C_RST}")

def header():
    print()
    div("purrstrap")
    print(f"  {C_BOLD}PURR OS firmware image builder{C_RST}")
    div()

def run(*args):
    cmd = [sys.executable, PURRSTRAP_PY] + list(args)
    print(f"{C_GRY}→ {' '.join(cmd[2:])}{C_RST}\n")
    proc = subprocess.run(cmd)
    return proc.returncode

# ── Serial port scanner ───────────────────────────────────────────────────────

_ESP32_VIDS = {0x1A86, 0x10C4, 0x0403, 0x303A, 0x239A}

def _scan_ports():
    results = []
    try:
        from serial.tools import list_ports
        for p in list_ports.comports():
            vid = getattr(p, "vid", None)
            desc = p.description or ""
            if vid in _ESP32_VIDS or any(s in desc.upper() for s in ("USB","CH340","CP210","FTDI")):
                results.append((p.device, desc))
    except ImportError:
        pass
    if not results and sys.platform.startswith("linux"):
        for pat in ("/dev/ttyUSB*", "/dev/ttyACM*"):
            for dev in sorted(glob.glob(pat)):
                if dev not in [r[0] for r in results]:
                    results.append((dev, ""))
    return results

def pick_port(saved=""):
    ports = _scan_ports()
    if not ports:
        example = "/dev/ttyUSB0" if sys.platform.startswith("linux") else "COM8"
        val = input(f"  Serial port (e.g. {example}, blank to skip) [{saved}]: ").strip()
        return val or saved or None
    print(f"\n  {C_CYN}Detected serial ports:{C_RST}")
    for i, (dev, desc) in enumerate(ports, 1):
        tag = f"  {C_GRY}{desc}{C_RST}" if desc else ""
        print(f"  [{i}] {dev}{tag}")
    default = saved if saved in [p[0] for p in ports] else ports[0][0]
    raw = input(f"\n  Choose port — number or path (Enter = {default}): ").strip()
    if not raw:
        return default
    if raw.isdigit():
        idx = int(raw) - 1
        return ports[idx][0] if 0 <= idx < len(ports) else default
    return raw

# ── Device picker ─────────────────────────────────────────────────────────────

def parse_pcat(path):
    result = {}
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"): continue
                if line.startswith("["): continue
                if "=" in line:
                    k, _, v = line.partition("=")
                    result[k.strip()] = v.strip().strip('"')
    except FileNotFoundError:
        pass
    return result

def list_devices():
    devices = []
    if not os.path.isdir(DEVICES_DIR):
        return devices
    for entry in sorted(os.listdir(DEVICES_DIR)):
        pcat = os.path.join(DEVICES_DIR, entry, "device.pcat")
        if os.path.isfile(pcat):
            cfg = parse_pcat(pcat)
            devices.append((entry, cfg.get("name", entry), cfg.get("chip", "?")))
    return devices

def pick_device():
    devices = list_devices()
    if not devices:
        print(f"  {C_YLW}No devices found in source/devices/{C_RST}")
        return None

    saved = ""
    if os.path.isfile(CFG_FILE):
        try:
            saved = json.load(open(CFG_FILE)).get("device", "")
        except Exception:
            pass

    print(f"\n  {C_BOLD}Available devices:{C_RST}")
    for i, (slug, name, chip) in enumerate(devices, 1):
        marker = f"  {C_GRY}← last used{C_RST}" if slug == saved else ""
        print(f"  [{i}] {C_CYN}{slug:<20}{C_RST}  {name}  ({chip}){marker}")

    raw = input(f"\n  Choose device — number or slug (Enter = {saved or devices[0][0]}): ").strip()
    default = saved or devices[0][0]
    if not raw:
        return default
    if raw.isdigit():
        idx = int(raw) - 1
        return devices[idx][0] if 0 <= idx < len(devices) else default
    return raw

# ── Action menus ──────────────────────────────────────────────────────────────

ACTIONS = [
    ("build",   "Build firmware for a device"),
    ("flash",   "Build + flash to connected device"),
    ("clean",   "Remove build artifacts"),
    ("list",    "List all supported devices"),
    ("status",  "Show current workspace config"),
    ("doctor",  "Check environment health"),
]

def pick_action():
    print(f"\n  {C_BOLD}Actions:{C_RST}")
    for i, (cmd, desc) in enumerate(ACTIONS, 1):
        print(f"  [{i}] {C_CYN}{cmd:<10}{C_RST}  {desc}")
    print(f"  [0] Exit")
    raw = input("\n  Choose action: ").strip()
    if not raw or raw == "0":
        return None
    if raw.isdigit():
        idx = int(raw) - 1
        if 0 <= idx < len(ACTIONS):
            return ACTIONS[idx][0]
    # allow typing the name directly
    names = [a[0] for a in ACTIONS]
    if raw in names:
        return raw
    return None

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    header()

    while True:
        action = pick_action()
        if action is None:
            print()
            break

        print()

        if action in ("build", "flash", "clean"):
            device = pick_device()
            if not device:
                continue
            print()
            if action == "flash":
                port = pick_port()
                print()
                if port:
                    run("flash", device, "-p", port)
                else:
                    run("flash", device)
            else:
                run(action, device)

        elif action == "list":
            run("list")

        elif action == "status":
            run("status")

        elif action == "doctor":
            run("doctor")

        print()
        again = input("  Back to menu? [Y/n]: ").strip().lower()
        if again == "n":
            break
        print()

if __name__ == "__main__":
    main()
