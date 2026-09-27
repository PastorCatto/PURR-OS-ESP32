#!/usr/bin/env python3
"""
catstrap interactive UI — wraps catstrap.py with numbered menus.

Run with no arguments (or via purr.py) for a guided session.
All actions are translated into catstrap.py CLI calls.
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
C_MGN  = "\033[95m"
C_WHT  = "\033[97m"

SCRIPT_DIR   = os.path.dirname(os.path.abspath(__file__))
REPO_DIR     = os.path.dirname(SCRIPT_DIR)
CATSTRAP_PY  = os.path.join(SCRIPT_DIR, "catstrap.py")
SOURCE_DIR   = os.path.join(REPO_DIR, "source")
APPS_DIR     = os.path.join(SOURCE_DIR, "apps")

SYSTEM_APPS = {
    "magicmac": ("catt", "Mac Plus emulator — in-house exclusive"),
    "magidos":  ("catt", "DOS 6.22 emulator — in-house exclusive"),
}

TIER_COLORS = {"meow": C_GRN, "hiss": C_RED, "paws": C_CYN, "claw": C_MGN, "catt": C_YLW}

def div(label=""):
    line = "─" * 52
    if label:
        line = f"─ {label} " + "─" * max(0, 52 - len(label) - 2)
    print(f"{C_GRY}{line}{C_RST}")

def header():
    print()
    div("catstrap")
    print(f"  {C_BOLD}PURR OS user app builder + SDK{C_RST}")
    div()

def run(*args):
    cmd = [sys.executable, CATSTRAP_PY] + list(args)
    print(f"{C_GRY}→ {' '.join(cmd[2:])}{C_RST}\n")
    proc = subprocess.run(cmd)
    return proc.returncode

# ── App discovery (lightweight) ───────────────────────────────────────────────

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

def _collect_user_apps():
    apps = []
    if not os.path.isdir(APPS_DIR):
        return apps
    for cat in sorted(os.listdir(APPS_DIR)):
        cat_dir = os.path.join(APPS_DIR, cat)
        if not os.path.isdir(cat_dir) or cat == "exclusive":
            continue
        for name in sorted(os.listdir(cat_dir)):
            app_dir = os.path.join(cat_dir, name)
            pcat    = os.path.join(app_dir, "app.pcat")
            if os.path.isfile(pcat):
                cfg  = parse_pcat(pcat)
                apps.append((name, cfg.get("tier", "paws")))
            for f in os.listdir(app_dir) if os.path.isdir(app_dir) else []:
                if f.endswith(".meow"):
                    apps.append((f[:-5], "meow"))
                elif f.endswith(".hiss"):
                    apps.append((f[:-5], "hiss"))
    return apps

# ── Actions ───────────────────────────────────────────────────────────────────

ACTIONS = [
    ("build_all",    "Build all user apps"),
    ("build_one",    "Build a specific app"),
    ("build_system", "Build a system exclusive (magicmac / magidos)"),
    ("validate",     "Validate a .meow/.hiss Lua script"),
    ("sdk_info",     "Show SDK version + API surface"),
    ("sdk_install",  "Install SDK headers to catstrap/sdk/include/"),
    ("list",         "List all apps"),
    ("clean_all",    "Clean all app build output"),
    ("clean_one",    "Clean a specific app"),
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

def pick_user_app(prompt="Choose app"):
    apps = _collect_user_apps()
    if not apps:
        print(f"  {C_YLW}No user apps found in source/apps/{C_RST}")
        return None
    print(f"\n  {C_BOLD}User apps:{C_RST}")
    for i, (name, tier) in enumerate(apps, 1):
        color = TIER_COLORS.get(tier, C_WHT)
        print(f"  [{i}] {color}{name:<24}{C_RST}  [{tier}]")
    raw = input(f"\n  {prompt} — number or name: ").strip()
    if raw.isdigit():
        idx = int(raw) - 1
        if 0 <= idx < len(apps):
            return apps[idx][0]
    return raw or None

def pick_system_app():
    keys = list(SYSTEM_APPS.keys())
    print(f"\n  {C_BOLD}System exclusives:{C_RST}")
    for i, key in enumerate(keys, 1):
        tier, desc = SYSTEM_APPS[key]
        color = TIER_COLORS.get(tier, C_WHT)
        print(f"  [{i}] {color}{key:<20}{C_RST}  [{tier}]  {desc}")
    raw = input("\n  Choose — number or name: ").strip()
    if raw.isdigit():
        idx = int(raw) - 1
        if 0 <= idx < len(keys):
            return keys[idx]
    return raw if raw in keys else None

def pick_meow_file():
    path = input("  Path to .meow/.hiss file: ").strip()
    return path or None

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
            run("build", "all")

        elif action == "build_one":
            app = pick_user_app("Choose app to build")
            if app:
                print()
                run("build", app)

        elif action == "build_system":
            app = pick_system_app()
            if app:
                print()
                run("build", app)

        elif action == "validate":
            path = pick_meow_file()
            if path:
                print()
                run("validate", path)

        elif action == "sdk_info":
            run("sdk", "info")

        elif action == "sdk_install":
            run("sdk", "install")

        elif action == "list":
            run("list")

        elif action == "clean_all":
            confirm = input("  Clean all app build output? [y/N]: ").strip().lower()
            if confirm == "y":
                run("clean", "all")

        elif action == "clean_one":
            app = pick_user_app("Choose app to clean")
            if app:
                print()
                run("clean", app)

        print()
        again = input("  Back to menu? [Y/n]: ").strip().lower()
        if again == "n":
            break
        print()

if __name__ == "__main__":
    main()
