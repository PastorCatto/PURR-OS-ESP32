#!/usr/bin/env python3
"""
driverstrap interactive UI — guided driver template wizard.

Run with no arguments for a fully prompted session.
All actions delegate to driverstrap.py.
"""

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

SCRIPT_DIR      = os.path.dirname(os.path.abspath(__file__))
REPO_DIR        = os.path.dirname(os.path.dirname(SCRIPT_DIR))
DRIVERSTRAP_PY  = os.path.join(SCRIPT_DIR, "driverstrap.py")

DRIVER_TYPES = {
    "display": {
        "desc":       "Pixel display (SPI, I2C, QSPI, parallel)",
        "interfaces": ["spi", "i2c", "qspi", "parallel"],
    },
    "touch": {
        "desc":       "Touch controller (capacitive or resistive)",
        "interfaces": ["i2c", "spi"],
    },
    "input": {
        "desc":       "HID input — keyboard, trackball, buttons",
        "interfaces": ["gpio", "i2c", "spi"],
    },
    "radio": {
        "desc":       "LoRa / sub-GHz radio transceiver",
        "interfaces": ["spi", "uart"],
    },
    "gps": {
        "desc":       "NMEA GPS module",
        "interfaces": ["uart", "i2c"],
    },
    "wifi": {
        "desc":       "WiFi system module (no catcall)",
        "interfaces": ["builtin"],
    },
}

INTERFACE_LABELS = {
    "spi":      "SPI (MOSI / MISO / SCLK / CS)",
    "i2c":      "I2C (SDA / SCL)",
    "qspi":     "QSPI 4-bit parallel (AXS15231B-style)",
    "parallel": "Parallel 8/16-bit (esp_lcd panel IO)",
    "gpio":     "GPIO interrupt / polling",
    "uart":     "UART (TX / RX)",
    "builtin":  "Built-in silicon (ESP32 WiFi stack)",
}

CHIP_OPTIONS = {
    "1": ("both",    "ESP32 + ESP32-S3"),
    "2": ("esp32s3", "ESP32-S3 only"),
    "3": ("esp32",   "ESP32 only"),
}

TARGET_OPTIONS = {
    "1": ("user",   "user_drivers/   — community / personal drivers  (recommended)"),
    "2": ("core",   "source/drivers/ — PURR OS built-in drivers (core contribution)"),
    "3": ("custom", "Custom path      — specify a directory"),
}


def div(label=""):
    line = f"─ {label} " + "─" * max(0, 50 - len(label) - 2) if label else "─" * 50
    print(f"{C_GRY}{line}{C_RST}")


def header():
    print()
    div("driverstrap")
    print(f"  {C_BOLD}PURR OS driver template generator{C_RST}")
    print(f"  {C_GRY}Generates a ready-to-modify driver skeleton in seconds.{C_RST}")
    div()
    print()


def menu(prompt, options, allow_back=False):
    """options: list of (label, description) or dict with str keys."""
    if isinstance(options, dict):
        items = list(options.items())
    else:
        items = [(str(i+1), o) for i, o in enumerate(options)]

    for key, val in items:
        if isinstance(val, tuple):
            label, desc = val
            print(f"  {C_CYN}{key}.{C_RST} {C_WHT}{label}{C_RST}  {C_GRY}{desc}{C_RST}")
        else:
            print(f"  {C_CYN}{key}.{C_RST} {val}")

    keys = [k for k, _ in items]
    if allow_back:
        print(f"  {C_YLW}b.{C_RST} Back")
        keys.append("b")

    while True:
        choice = input(f"\n  {prompt} [{'/'.join(keys)}]: ").strip().lower()
        if choice in keys:
            return choice
        print(f"  {C_RED}Invalid choice.{C_RST}")


def ask(prompt, default="", validator=None):
    default_hint = f" [{C_GRY}{default}{C_RST}]" if default else ""
    while True:
        val = input(f"  {prompt}{default_hint}: ").strip()
        if not val and default:
            val = default
        if validator:
            err = validator(val)
            if err:
                print(f"  {C_RED}{err}{C_RST}")
                continue
        return val


def run_driverstrap(*args):
    cmd = [sys.executable, DRIVERSTRAP_PY] + list(args)
    print(f"\n{C_GRY}→ {' '.join(cmd[1:])}{C_RST}\n")
    proc = subprocess.run(cmd)
    return proc.returncode


def validate_name(name):
    import re
    if not name:
        return "Name cannot be empty."
    if not re.match(r"^[a-z][a-z0-9_]*$", name):
        return "Name must be lowercase letters, digits, and underscores only. Example: my_oled"
    if len(name) > 32:
        return "Name is too long (max 32 characters)."
    return None


def wizard():
    header()

    # ── Step 1: Driver type ───────────────────────────────────────────────────
    print(f"{C_BOLD}Step 1 — Driver type{C_RST}")
    print()
    type_items = {str(i+1): (t, meta["desc"]) for i, (t, meta) in enumerate(DRIVER_TYPES.items())}
    choice = menu("Type", type_items)
    drv_type, _ = type_items[choice]
    print()

    # ── Step 2: Driver name ───────────────────────────────────────────────────
    div()
    print(f"\n{C_BOLD}Step 2 — Driver name{C_RST}")
    print(f"  {C_GRY}Use a short, descriptive slug. This becomes the file and folder name.{C_RST}")
    print(f"  {C_GRY}Examples:  my_oled  |  ws2812  |  ili9488  |  lsm6dso{C_RST}")
    print()
    name = ask("Driver name", validator=validate_name)
    print()

    # ── Step 3: Hardware interface ────────────────────────────────────────────
    meta = DRIVER_TYPES[drv_type]
    ifaces = meta["interfaces"]

    if len(ifaces) == 1:
        interface = ifaces[0]
        print(f"  Interface: {C_CYN}{interface}{C_RST} {C_GRY}(only option for {drv_type}){C_RST}\n")
    else:
        div()
        print(f"\n{C_BOLD}Step 3 — Hardware interface{C_RST}")
        print()
        iface_items = {str(i+1): (ifc, INTERFACE_LABELS.get(ifc, ifc)) for i, ifc in enumerate(ifaces)}
        choice = menu("Interface", iface_items)
        interface = ifaces[int(choice) - 1]
        print()

    # ── Step 4: Chip target ───────────────────────────────────────────────────
    div()
    print(f"\n{C_BOLD}Step 4 — Target chip{C_RST}")
    print()
    chip_choice = menu("Chip", CHIP_OPTIONS)
    chip = CHIP_OPTIONS[chip_choice][0]
    print()

    # ── Step 5: Output location ───────────────────────────────────────────────
    div()
    print(f"\n{C_BOLD}Step 5 — Output location{C_RST}")
    print()
    loc_choice = menu("Location", TARGET_OPTIONS)
    loc_val = TARGET_OPTIONS[loc_choice][0]
    custom_out = None

    if loc_val == "custom":
        custom_out = ask("Output directory (absolute or relative to repo root)").strip()
        if not os.path.isabs(custom_out):
            custom_out = os.path.join(REPO_DIR, custom_out)
    print()

    # ── Step 6: Options ───────────────────────────────────────────────────────
    div()
    print(f"\n{C_BOLD}Step 6 — Options{C_RST}")
    print()

    gen_hdr = True
    if DRIVER_TYPES[drv_type].get("interfaces") != ["builtin"]:
        hdr_ans = ask("Generate .h public header? [Y/n]", default="y")
        gen_hdr = hdr_ans.lower() not in ("n", "no")
    print()

    # ── Summary ───────────────────────────────────────────────────────────────
    div("summary")
    print(f"  Type:       {C_CYN}{drv_type}{C_RST}")
    print(f"  Name:       {C_WHT}{name}{C_RST}")
    print(f"  Interface:  {interface}")
    print(f"  Chip:       {chip}")
    if loc_val == "user":
        out_display = f"user_drivers/{drv_type}/{name}/"
    elif loc_val == "core":
        out_display = f"source/drivers/{drv_type}/{name}/"
    else:
        out_display = custom_out
    print(f"  Output:     {C_GRY}{out_display}{C_RST}")
    print(f"  Header:     {'yes' if gen_hdr else 'no'}")
    div()
    print()

    confirm = ask("Generate? [Y/n]", default="y")
    if confirm.lower() in ("n", "no"):
        print(f"\n  {C_YLW}Cancelled.{C_RST}\n")
        return

    # ── Run driverstrap.py ────────────────────────────────────────────────────
    print()
    cmd_args = ["new", drv_type, name,
                "--interface", interface,
                "--chip", chip]
    if loc_val == "core":
        cmd_args.append("--core")
    elif loc_val == "custom":
        cmd_args += ["--output", custom_out]
    if not gen_hdr:
        cmd_args.append("--no-header")

    rc = run_driverstrap(*cmd_args)

    if rc == 0:
        print(f"\n{C_GRN}Done!{C_RST} Open the generated files and fill in the TODO sections.\n")
    else:
        print(f"\n{C_RED}driverstrap exited with code {rc}{C_RST}\n")


def main_menu():
    header()

    while True:
        print(f"{C_BOLD}What do you want to do?{C_RST}\n")
        print(f"  {C_CYN}1.{C_RST} Generate a new driver template")
        print(f"  {C_CYN}2.{C_RST} List all driver types")
        print(f"  {C_CYN}3.{C_RST} List interfaces for a type")
        print(f"  {C_CYN}q.{C_RST} Quit")
        print()

        choice = input("  Choice [1/2/3/q]: ").strip().lower()
        print()

        if choice == "1":
            wizard()
            print()
        elif choice == "2":
            run_driverstrap("list-types")
            print()
        elif choice == "3":
            types = list(DRIVER_TYPES.keys())
            for i, t in enumerate(types, 1):
                print(f"  {C_CYN}{i}.{C_RST} {t}")
            print()
            t_choice = input("  Type [1-6]: ").strip()
            try:
                t = types[int(t_choice) - 1]
                run_driverstrap("list-interfaces", t)
            except (ValueError, IndexError):
                print(f"  {C_RED}Invalid choice.{C_RST}")
            print()
        elif choice in ("q", "quit", "exit"):
            print("  Bye.\n")
            break
        else:
            print(f"  {C_RED}Invalid choice.{C_RST}\n")


if __name__ == "__main__":
    if len(sys.argv) > 1:
        # If called with args, run the wizard non-interactively via driverstrap.py
        os.execv(sys.executable, [sys.executable, DRIVERSTRAP_PY] + sys.argv[1:])
    else:
        main_menu()
