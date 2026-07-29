#!/usr/bin/env python3
"""
purr.py — PURR OS master build launcher

Provides a top-level interactive menu that routes to whichever build tool
you need: purrstrap (firmware), modulestrap (modules/drivers), or catstrap
(apps + SDK). Each tool opens its own interactive UI.

Usage:
  python purr.py               # interactive menu
  python purr.py purrstrap     # jump straight to purrstrap UI
  python purr.py modulestrap   # jump straight to modulestrap UI
  python purr.py catstrap      # jump straight to catstrap UI

  # Pass-through to the underlying CLI (skips UI):
  python purr.py purrstrap build tdeck_plus
  python purr.py modulestrap list
  python purr.py catstrap sdk info
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
C_MGN  = "\033[95m"
C_WHT  = "\033[97m"

REPO_DIR = os.path.dirname(os.path.abspath(__file__))

TOOLS = {
    "purrstrap": {
        "ui":   os.path.join(REPO_DIR, "purrstrap",   "purrstrap_ui.py"),
        "cli":  os.path.join(REPO_DIR, "purrstrap",   "purrstrap.py"),
        "desc": "Firmware image builder — compiles kernel + assembles flash image",
        "color": C_GRN,
    },
    "modulestrap": {
        "ui":   os.path.join(REPO_DIR, "modulestrap",  "modulestrap_ui.py"),
        "cli":  os.path.join(REPO_DIR, "modulestrap",  "modulestrap.py"),
        "desc": "Module + driver compiler — builds .purr kernel blobs",
        "color": C_CYN,
    },
    "catstrap": {
        "ui":   os.path.join(REPO_DIR, "catstrap",     "catstrap_ui.py"),
        "cli":  os.path.join(REPO_DIR, "catstrap",     "catstrap.py"),
        "desc": "App builder + SDK — compiles .paws/.claw/.catt apps, packages .meow/.hiss scripts",
        "color": C_MGN,
    },
}

PURROS_VERSION = "1.0.0-DP9"
KITT_VERSION   = "1.0.0"

# ── Helpers ───────────────────────────────────────────────────────────────────

def div(label=""):
    line = "─" * 56
    if label:
        line = f"─ {label} " + "─" * max(0, 56 - len(label) - 2)
    print(f"{C_GRY}{line}{C_RST}")

def banner():
    print()
    div()
    print(f"  {C_BOLD}PURR OS{C_RST}  v{PURROS_VERSION}   {C_GRY}KITT v{KITT_VERSION}{C_RST}")
    print(f"  {C_GRY}Build System — master launcher{C_RST}")
    div()
    print()

def launch_ui(name):
    path = TOOLS[name]["ui"]
    if not os.path.isfile(path):
        print(f"{C_RED}[err]{C_RST} {name}_ui.py not found at {path}", file=sys.stderr)
        return 1
    proc = subprocess.run([sys.executable, path])
    return proc.returncode

def launch_cli(name, args):
    path = TOOLS[name]["cli"]
    if not os.path.isfile(path):
        print(f"{C_RED}[err]{C_RST} {name}.py not found at {path}", file=sys.stderr)
        return 1
    proc = subprocess.run([sys.executable, path] + list(args))
    return proc.returncode

# ── Interactive menu ──────────────────────────────────────────────────────────

def interactive():
    banner()

    tool_keys = list(TOOLS.keys())

    print(f"  {C_BOLD}Build tools:{C_RST}\n")
    for i, key in enumerate(tool_keys, 1):
        t     = TOOLS[key]
        color = t["color"]
        print(f"  [{i}] {color}{key:<14}{C_RST}  {t['desc']}")
    print(f"\n  [0] Exit")
    print()

    raw = input("  Choose a tool: ").strip()
    if not raw or raw == "0":
        print()
        return 0

    if raw.isdigit():
        idx = int(raw) - 1
        if 0 <= idx < len(tool_keys):
            return launch_ui(tool_keys[idx])
        print(f"{C_YLW}[warn]{C_RST} invalid choice")
        return 1

    if raw in TOOLS:
        return launch_ui(raw)

    print(f"{C_YLW}[warn]{C_RST} unknown tool '{raw}'")
    return 1

# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    args = sys.argv[1:]

    if not args:
        sys.exit(interactive())

    tool = args[0]
    if tool not in TOOLS:
        print(f"{C_RED}[err]{C_RST} unknown tool '{tool}'")
        print(f"  Available: {', '.join(TOOLS.keys())}")
        print(f"  Usage: python purr.py [purrstrap|modulestrap|catstrap] [args...]")
        sys.exit(1)

    rest = args[1:]

    if not rest:
        # No sub-args — open the interactive UI for that tool
        sys.exit(launch_ui(tool))
    else:
        # Sub-args present — pass directly to the underlying CLI script
        sys.exit(launch_cli(tool, rest))

if __name__ == "__main__":
    main()
