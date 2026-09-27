#!/usr/bin/env python3
"""
modulestrap interactive UI — wraps modulestrap.py with numbered menus.

Run with no arguments (or via purr.py) for a guided session.
All actions are translated into modulestrap.py CLI calls.
"""

import os
import subprocess
import sys

os.system("")

C_RST  = "\033[0m"
C_BOLD = "\033[1m"
C_GRY  = "\033[90m"
C_RED  = "\033[91m"
C_GRN  = "\033[92m"
C_YLW  = "\033[93m"
C_CYN  = "\033[96m"
C_WHT  = "\033[97m"

SCRIPT_DIR      = os.path.dirname(os.path.abspath(__file__))
REPO_DIR        = os.path.dirname(SCRIPT_DIR)
MODULESTRAP_PY  = os.path.join(SCRIPT_DIR, "modulestrap.py")
SOURCE_DIR      = os.path.join(REPO_DIR, "source")
MODULES_DIR     = os.path.join(SOURCE_DIR, "modules")
DRIVERS_DIR     = os.path.join(SOURCE_DIR, "drivers")
USER_DRIVERS    = os.path.join(REPO_DIR, "user_drivers")

def div(label=""):
    line = "─" * 52
    if label:
        line = f"─ {label} " + "─" * max(0, 52 - len(label) - 2)
    print(f"{C_GRY}{line}{C_RST}")

def header():
    print()
    div("modulestrap")
    print(f"  {C_BOLD}PURR OS kernel module + driver compiler{C_RST}")
    div()

def run(*args):
    cmd = [sys.executable, MODULESTRAP_PY] + list(args)
    print(f"{C_GRY}→ {' '.join(cmd[2:])}{C_RST}\n")
    proc = subprocess.run(cmd)
    return proc.returncode

# ── Target discovery (lightweight, for menus only) ────────────────────────────

def parse_pcat(path):
    result = {}
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or line.startswith("["): continue
                if "=" in line:
                    k, _, v = line.partition("=")
                    result[k.strip()] = v.strip().strip('"')
    except FileNotFoundError:
        pass
    return result

def _collect_targets():
    targets = []
    if os.path.isdir(MODULES_DIR):
        for name in sorted(os.listdir(MODULES_DIR)):
            pcat = os.path.join(MODULES_DIR, name, "module.pcat")
            if os.path.isfile(pcat):
                targets.append((name, "module"))
    for root, kind_tag in [(DRIVERS_DIR, "driver"), (USER_DRIVERS, "driver(user)")]:
        if not os.path.isdir(root): continue
        for entry in sorted(os.listdir(root)):
            ep = os.path.join(root, entry)
            if not os.path.isdir(ep): continue
            if os.path.isfile(os.path.join(ep, "driver.pcat")):
                targets.append((entry, kind_tag))
            else:
                for name in sorted(os.listdir(ep)):
                    pcat = os.path.join(ep, name, "driver.pcat")
                    if os.path.isfile(pcat):
                        targets.append((f"{entry}/{name}", kind_tag))
    return targets

# ── Interactive pickers ───────────────────────────────────────────────────────

ACTIONS = [
    ("build_all",     "Build all modules and drivers"),
    ("build_modules", "Build system modules only"),
    ("build_drivers", "Build drivers only"),
    ("build_one",     "Build a specific target"),
    ("list",          "List all buildable targets"),
    ("clean_all",     "Clean all compiled output"),
    ("clean_one",     "Clean a specific target"),
]

def pick_action():
    print(f"\n  {C_BOLD}Actions:{C_RST}")
    for i, (key, desc) in enumerate(ACTIONS, 1):
        print(f"  [{i}] {C_CYN}{key:<18}{C_RST}  {desc}")
    print(f"  [0] Exit")
    raw = input("\n  Choose action: ").strip()
    if not raw or raw == "0":
        return None
    if raw.isdigit():
        idx = int(raw) - 1
        if 0 <= idx < len(ACTIONS):
            return ACTIONS[idx][0]
    return None

def pick_target(prompt="Choose target"):
    targets = _collect_targets()
    if not targets:
        print(f"  {C_YLW}No targets found.{C_RST}")
        return None
    print(f"\n  {C_BOLD}Available targets:{C_RST}")
    for i, (slug, kind) in enumerate(targets, 1):
        print(f"  [{i}] {C_CYN}{slug:<30}{C_RST}  [{kind}]")
    raw = input(f"\n  {prompt} — number or name: ").strip()
    if raw.isdigit():
        idx = int(raw) - 1
        if 0 <= idx < len(targets):
            return targets[idx][0]
    return raw or None

def pick_extra_drivers():
    raw = input("  Extra --drivers path(s)? (comma-separated, blank to skip): ").strip()
    if not raw:
        return []
    return [p.strip() for p in raw.split(",") if p.strip()]

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    header()

    while True:
        action = pick_action()
        if action is None:
            print()
            break

        print()

        if action == "build_all":
            extra = pick_extra_drivers()
            args = ["build", "all"]
            for p in extra:
                args += ["--drivers", p]
            run(*args)

        elif action == "build_modules":
            run("build", "modules")

        elif action == "build_drivers":
            extra = pick_extra_drivers()
            args = ["build", "drivers"]
            for p in extra:
                args += ["--drivers", p]
            run(*args)

        elif action == "build_one":
            target = pick_target("Choose target to build")
            if target:
                print()
                run("build", target)

        elif action == "list":
            extra = pick_extra_drivers()
            args = ["list"]
            for p in extra:
                args += ["--drivers", p]
            run(*args)

        elif action == "clean_all":
            confirm = input("  Clean all compiled output? [y/N]: ").strip().lower()
            if confirm == "y":
                run("clean", "all")

        elif action == "clean_one":
            target = pick_target("Choose target to clean")
            if target:
                print()
                run("clean", target)

        print()
        again = input("  Back to menu? [Y/n]: ").strip().lower()
        if again == "n":
            break
        print()

if __name__ == "__main__":
    main()
