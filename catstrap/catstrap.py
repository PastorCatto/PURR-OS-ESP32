#!/usr/bin/env python3
"""
catstrap — PURR OS user app builder + SDK

Compiles and packages user apps for PURR OS:
  .paws  — compiled userland apps (win.*, sd.* API only)
  .claw  — compiled kernel-access apps (full kernel API: MagicMac, MagiDOS)
  .meow  — Lua scripts, sandboxed (validate + package, no compilation)
  .hiss  — Lua scripts, privileged (same as .meow + kitt./radio./gps.)

Also manages the catstrap SDK — the headers and stubs that app developers
build against. Apps never link against the kernel directly; they link against
the catstrap SDK which mirrors the catcall interface.

Usage:
  catstrap build <app_dir>          build an app (reads app.pcat in the dir)
  catstrap build all                build all apps under source/apps/
  catstrap build magicmac           build MagicMac (.claw) from magicmac/
  catstrap build magidos             build MagiDOS (.claw) from magidos/
  catstrap package <app_dir>        package a built app for distribution
  catstrap validate <file.meow|.hiss>  syntax-check a Lua script
  catstrap sdk install              install SDK headers to catstrap/sdk/include/
  catstrap sdk info                 show SDK version and API surface
  catstrap list                     list all buildable apps
  catstrap clean [app|all]          remove build artifacts

Output: cattobaked/apps/<name>.<tier>
        cattobaked/apps/magicmac.claw
        cattobaked/apps/magidos.claw
"""

import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys

os.system("")

# Force UTF-8 on our own stdout/stderr.
#
# These tools print box-drawing characters (the U+2500 divider) and en/em
# dashes. On Windows, a Python subprocess with no PYTHONIOENCODING gets the
# ANSI codepage — cp1252 here — and the FIRST divider raises
#
#   UnicodeEncodeError: 'charmap' codec can't encode characters ...
#
# which kills the tool. purrstrap invokes catstrap as a subprocess, so it died
# there mid-build and reported "catstrap exited with errors — some apps may be
# missing": the SPIFFS image was then built without them, and the failure was a
# traceback about a print statement rather than anything to do with apps.
#
# Setting PYTHONIOENCODING in the shell hides this, which is exactly why it went
# unnoticed — every invocation during development had it set. Fixing it here
# means the tools work in a plain terminal, from any shell, with no environment
# setup.
#
# errors="replace" rather than "strict": a decorative character is never worth
# aborting a build over.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass   # not a real stream (redirected/captured) — nothing to do


C_RST  = "\033[0m"
C_BOLD = "\033[1m"
C_GRY  = "\033[90m"
C_RED  = "\033[91m"
C_GRN  = "\033[92m"
C_YLW  = "\033[93m"
C_CYN  = "\033[96m"
C_MGN  = "\033[95m"
C_WHT  = "\033[97m"

def info(msg):        print(f"{C_GRN}[catstrap]{C_RST} {msg}")
def warn(msg):        print(f"{C_YLW}[warn]    {C_RST} {msg}")
def die(msg, code=1): print(f"{C_RED}[err]     {C_RST} {msg}", file=sys.stderr); sys.exit(code)
def div():            print(f"{C_GRY}" + "─" * 52 + C_RST)

REPO_DIR    = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_DIR  = os.path.join(REPO_DIR, "source")
APPS_DIR    = os.path.join(SOURCE_DIR, "apps")
OUTPUT_DIR  = os.path.join(REPO_DIR, "cattobaked")
OUT_APPS    = os.path.join(OUTPUT_DIR, "apps")
SDK_DIR     = os.path.join(REPO_DIR, "catstrap", "sdk")
MAGICMAC_DIR = os.path.join(SOURCE_DIR, "apps", "exclusive", "magicmac")
MAGIDOS_DIR  = os.path.join(SOURCE_DIR, "apps", "exclusive", "magidos")

TIER_COLORS = {
    "meow":   C_GRN,
    "hiss":   C_RED,
    "paws":   C_CYN,
    "claw":   C_MGN,
    "kitten": C_YLW,
}

SDK_VERSION = "0.1.0"

# Catstrap SDK API surface — what each tier gets
# .meow never had kitt.* — that was a long-standing doc bug (three separate
# places claimed it: this dict, README.md, and app_manager.h's tier comment)
# contradicted by docs/06_Apps.md and the real lua_runtime.c. .hiss is the
# tier that actually adds kitt./radio./gps. on top of the same win./sd. VM.
SDK_API = {
    "meow":   ["win.*", "sd.*", "purr.info()"],
    "hiss":   ["win.*", "sd.*", "kitt.*", "radio.*", "gps.*", "purr.info()"],
    "paws":   ["win.*", "sd.*"],
    "claw":   ["win.*", "sd.*", "kitt.*", "purr.*", "purr_kernel_*"],
    "kitten": ["win.*", "sd.*", "kitt.*", "radio.*", "gps.*", "purr.info()"],
}

# ── .pcat parser ─────────────────────────────────────────────────────────────

def parse_pcat(path):
    result = {}
    section = ""
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"): continue
                if line.startswith("["):
                    section = line.strip("[]").strip()
                    continue
                if "=" in line:
                    k, _, v = line.partition("=")
                    key = f"{section}.{k.strip()}" if section else k.strip()
                    result[key] = v.strip().strip('"')
    except FileNotFoundError:
        pass
    return result

# ── App discovery ─────────────────────────────────────────────────────────────

def find_apps():
    """Return list of (name, app_dir, pcat_path, tier) for all apps."""
    apps = []
    if not os.path.isdir(APPS_DIR):
        return apps
    for category in sorted(os.listdir(APPS_DIR)):
        cat_dir = os.path.join(APPS_DIR, category)
        if not os.path.isdir(cat_dir): continue
        for name in sorted(os.listdir(cat_dir)):
            app_dir = os.path.join(cat_dir, name)
            if not os.path.isdir(app_dir): continue
            pcat = os.path.join(app_dir, "app.pcat")
            if os.path.isfile(pcat):
                cfg  = parse_pcat(pcat)
                tier = cfg.get("tier", "paws")
                apps.append((name, app_dir, pcat, tier))
            # Also pick up bare .meow / .hiss / .kitten scripts
            for f in os.listdir(app_dir):
                if f.endswith(".meow"):
                    apps.append((f[:-5], app_dir, os.path.join(app_dir, f), "meow"))
                elif f.endswith(".hiss"):
                    apps.append((f[:-5], app_dir, os.path.join(app_dir, f), "hiss"))
                elif f.endswith(".kitten"):
                    apps.append((f[:-7], app_dir, os.path.join(app_dir, f), "kitten"))
    return apps

def cmd_list(args):
    apps = find_apps()
    div()
    print(f"{C_BOLD}Apps{C_RST}")
    div()
    print(f"  {C_BOLD}System apps:{C_RST}")
    for key, (app_dir, tier, desc) in SYSTEM_APPS.items():
        color   = TIER_COLORS.get(tier, C_WHT)
        present = f"{C_GRN}ready{C_RST}" if os.path.isdir(app_dir) else f"{C_YLW}not yet created{C_RST}"
        print(f"  {color}{key:<24}{C_RST}  [{tier}]  {present}  — {desc}")
    print()
    print(f"  {C_BOLD}User apps ({len(apps)}):{C_RST}")
    for name, app_dir, pcat, tier in apps:
        color = TIER_COLORS.get(tier, C_WHT)
        rel   = os.path.relpath(app_dir, REPO_DIR)
        print(f"  {color}{name:<24}{C_RST}  [{tier}]  {rel}")
    if not apps:
        warn("no user apps found in source/apps/ — create an app directory with app.pcat")
    div()

# ── SDK ───────────────────────────────────────────────────────────────────────

def cmd_sdk(args):
    sub = args.sub
    if sub == "info":
        div()
        print(f"{C_BOLD}catstrap SDK v{SDK_VERSION}{C_RST}")
        div()
        for tier, api in SDK_API.items():
            color = TIER_COLORS.get(tier, C_WHT)
            print(f"  {color}.{tier:<6}{C_RST}  {', '.join(api)}")
        div()
    elif sub == "install":
        _sdk_install()
    else:
        die(f"unknown sdk subcommand '{sub}' — use 'info' or 'install'")

def _sdk_install():
    """
    Generate SDK include headers in catstrap/sdk/include/.

    Copies the real catcall headers from source/kernel/catcalls/ and generates
    tier-gated wrapper headers so apps include only what their tier allows.
    """
    inc_dir     = os.path.join(SDK_DIR, "include")
    catcall_src = os.path.join(REPO_DIR, "source", "kernel", "catcalls")
    kernel_src  = os.path.join(REPO_DIR, "source", "kernel", "core")
    os.makedirs(inc_dir, exist_ok=True)

    # Copy catcall headers verbatim — apps #include them directly
    catcall_headers = [
        "catcall_display.h", "catcall_touch.h", "catcall_input.h",
        "catcall_radio.h", "catcall_gps.h", "catcalls.h",
    ]
    for hdr in catcall_headers:
        src = os.path.join(catcall_src, hdr)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(inc_dir, hdr))
            info(f"  copied {hdr}")
        else:
            warn(f"  missing catcall header: {hdr}")

    # Copy kernel module ABI header (needed by .claw apps for purr_module_header_t)
    for hdr in ("purr_module.h", "purr_kernel.h"):
        src = os.path.join(kernel_src, hdr)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(inc_dir, hdr))
            info(f"  copied {hdr}")

    # purr_sdk.h — master tier-gated include
    with open(os.path.join(inc_dir, "purr_sdk.h"), "w") as f:
        f.write(f"// purr_sdk.h — catstrap SDK v{SDK_VERSION}\n")
        f.write("// Generated by: catstrap sdk install\n")
        f.write("// Compile with -DPURR_TIER_CLAW or -DPURR_TIER_PAWS.\n\n")
        f.write("#pragma once\n\n")
        f.write("#include <stdint.h>\n")
        f.write("#include <stdbool.h>\n\n")
        f.write("#if defined(PURR_TIER_CLAW)\n")
        f.write('#  include "purr_sdk_claw.h"\n')
        f.write("#elif defined(PURR_TIER_PAWS)\n")
        f.write('#  include "purr_sdk_paws.h"\n')
        f.write("#else\n")
        f.write("#  error \"Define PURR_TIER_CLAW or PURR_TIER_PAWS before including purr_sdk.h\"\n")
        f.write("#endif\n")

    # purr_sdk_paws.h — .paws tier: display + touch + input (read-only observation)
    # No kernel registration, no radio, no raw catcall init — use via purr_win_* wrappers.
    with open(os.path.join(inc_dir, "purr_sdk_paws.h"), "w") as f:
        f.write(f"// purr_sdk_paws.h — catstrap SDK v{SDK_VERSION} — .paws tier\n")
        f.write("// Userland apps: display output + touch/input polling via win.* API.\n")
        f.write("// No direct kernel registration. No radio or GPS access.\n\n")
        f.write("#pragma once\n\n")
        f.write("#include <stdint.h>\n")
        f.write("#include <stdbool.h>\n")
        f.write("#include <string.h>\n\n")
        f.write("// Catcall types — for reading display info and touch/input events.\n")
        f.write("// .paws apps use these types but do NOT call init/deinit.\n")
        f.write('#include "catcall_display.h"\n')
        f.write('#include "catcall_touch.h"\n')
        f.write('#include "catcall_input.h"\n\n')
        f.write("// Window + filesystem access — provided by the active UI module.\n")
        f.write("// Implemented at link time against the kernel's win_api.\n")
        f.write("// purr_win_create(), purr_win_draw_text(), purr_win_close(), etc.\n")
        f.write("// purr_sd_open(), purr_sd_read(), purr_sd_write(), purr_sd_close()\n")
        f.write("#ifdef PURR_PAWS_IMPL\n")
        f.write("// These are resolved by the kernel at pre-link. Leave this block empty\n")
        f.write("// unless you are implementing the .paws ABI bridge (kernel internal).\n")
        f.write("#endif\n\n")
        f.write("// Version info available to all .paws apps:\n")
        f.write(f'#define PURR_SDK_VERSION   "{SDK_VERSION}"\n')
        f.write('#define PURR_TIER_NAME     "paws"\n')

    # purr_sdk_claw.h — .claw tier: full kernel access
    with open(os.path.join(inc_dir, "purr_sdk_claw.h"), "w") as f:
        f.write(f"// purr_sdk_claw.h — catstrap SDK v{SDK_VERSION} — .claw tier\n")
        f.write("// Kernel-access apps: full catcall API, module registration, radio, GPS.\n")
        f.write("// Apps compiled at this tier are pre-linked into the firmware image.\n\n")
        f.write("#pragma once\n\n")
        f.write("#include <stdint.h>\n")
        f.write("#include <stdbool.h>\n")
        f.write("#include <string.h>\n\n")
        f.write("// Full catcall interface — all subsystems\n")
        f.write('#include "catcalls.h"\n\n')
        f.write("// Kernel module ABI — required for purr_module_header_t export\n")
        f.write('#include "purr_module.h"\n\n')
        f.write("// Kernel runtime API — catcall getters, module registry, panic\n")
        f.write('#include "purr_kernel.h"\n\n')
        f.write("// Version info:\n")
        f.write(f'#define PURR_SDK_VERSION   "{SDK_VERSION}"\n')
        f.write('#define PURR_TIER_NAME     "claw"\n')

    info(f"SDK headers written to catstrap/sdk/include/")
    info(f"  purr_sdk.h  purr_sdk_paws.h  purr_sdk_claw.h")
    info(f"  + catcall headers + purr_module.h + purr_kernel.h")
    info(f"SDK version: {SDK_VERSION}")

# ── Validate .meow / .hiss ───────────────────────────────────────────────────

# Self-declared "purr-sig" tag — .hiss and .kitten (both privileged tiers).
# Honor-system, not cryptographic: anyone editing the file can change it,
# same trust level as the extension-only decision for these tiers itself.
# Doesn't gate kitt./radio./gps. *availability* — that's decided by the
# extension alone — it gates whether an *unsigned* privileged script is
# allowed to run at all without Developer Mode on (app_manager.c's
# launch_meow()). See docs/06_Apps.md's .hiss section.
PURR_SIG_VALUES = ("unsigned", "dev-signed", "trusted-signed", "dev-approved")

def _read_purr_sig(path):
    """Scan a .hiss/.kitten file for a '-- purr-sig: <value>' comment line.
    Returns the tagged value, or "unsigned" if no tag (or an unrecognized value) is found."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if line.startswith("--") and "purr-sig:" in line:
                    value = line.split("purr-sig:", 1)[1].strip()
                    if value in PURR_SIG_VALUES:
                        return value
                    warn(f"    unrecognized purr-sig value '{value}' — treated as unsigned")
                    return "unsigned"
    except OSError:
        pass
    return "unsigned"

def cmd_validate(args):
    path = args.file
    if not os.path.isfile(path):
        die(f"file not found: {path}")
    is_privileged = path.endswith(".hiss") or path.endswith(".kitten")
    if not (path.endswith(".meow") or is_privileged):
        warn(f"expected a .meow/.hiss/.kitten file, got: {path}")

    lua = shutil.which("lua") or shutil.which("lua5.4") or shutil.which("lua5.3")
    if not lua:
        warn("lua interpreter not found — skipping syntax check")
    else:
        result = subprocess.run([lua, "-", path], input=f'loadfile("{path}")()',
                                capture_output=True, text=True)
        if result.returncode == 0:
            info(f"{C_GRN}[OK]{C_RST}  {path}")
        else:
            print(f"{C_RED}[FAIL]{C_RST} {path}")
            print(result.stderr)
            sys.exit(1)

    if is_privileged:
        sig = _read_purr_sig(path)
        if sig == "unsigned":
            warn(f"    no purr-sig tag found — treated as unsigned")
        else:
            info(f"    signature: {sig}")

# ── Build ─────────────────────────────────────────────────────────────────────

def build_app(name, app_dir, pcat_path, tier):
    is_script = pcat_path.endswith(".meow") or pcat_path.endswith(".hiss") or pcat_path.endswith(".kitten")
    cfg = parse_pcat(pcat_path) if not is_script else {}
    version = cfg.get("version", "0.1.0")
    color   = TIER_COLORS.get(tier, C_WHT)

    os.makedirs(OUT_APPS, exist_ok=True)
    out_name = f"{name}.{tier}"
    out_path = os.path.join(OUT_APPS, out_name)

    info(f"  {color}{name}{C_RST}  [{tier}]  v{version}  →  {os.path.relpath(out_path, REPO_DIR)}")

    if tier in ("meow", "hiss", "kitten"):
        # Package the Lua script directly — no compilation. .hiss and
        # .kitten both gate unsigned scripts behind Developer Mode at
        # runtime (app_manager.c's launch_meow()) — .kitten needs it even
        # more than .hiss since it autoruns unconditionally at every boot.
        src = pcat_path if is_script else os.path.join(app_dir, f"{name}.{tier}")
        if os.path.isfile(src):
            shutil.copy2(src, out_path)
            info(f"    packaged Lua script → {out_name}")
            if tier in ("hiss", "kitten"):
                sig = _read_purr_sig(src)
                info(f"    signature: {sig}" if sig != "unsigned"
                     else "    no purr-sig tag found — treated as unsigned")
        else:
            warn(f"    no .{tier} script found at {src}")
        return True

    # .paws / .claw — compiled apps are IDF components, same as modules.
    # catstrap registers them (CMakeLists.txt + metadata) so purrstrap's
    # idf.py build picks them up via components_manifest.cmake.
    c_files = [f for f in os.listdir(app_dir)
               if (f.endswith(".c") or f.endswith(".cpp"))
               and os.path.isfile(os.path.join(app_dir, f))]
    if not c_files:
        warn(f"    no C source found — skipping")
        return False

    info(f"    sources: {', '.join(c_files)}")

    # Generate IDF component CMakeLists.txt if missing
    cmake_path = os.path.join(app_dir, "CMakeLists.txt")
    if not os.path.isfile(cmake_path):
        kernel_rel = os.path.relpath(
            os.path.join(REPO_DIR, "source", "kernel"), app_dir)
        src_list   = "\n        ".join(c_files)
        req = cfg.get("idf_requires", "esp_common driver freertos nvs_flash")
        # .claw apps also get kernel headers
        extra_inc  = f"\n        {kernel_rel}/core\n        {kernel_rel}/catcalls" \
                     if tier == "claw" else ""
        cmake_txt  = (
            f"# Auto-generated by catstrap — safe to customize.\n"
            f"idf_component_register(\n"
            f"    SRCS\n"
            f"        {src_list}\n"
            f"    INCLUDE_DIRS\n"
            f"        .{extra_inc}\n"
            f"    REQUIRES {req}\n"
            f")\n"
        )
        with open(cmake_path, "w") as f:
            f.write(cmake_txt)
        info(f"    wrote CMakeLists.txt")

    with open(out_path + ".meta.json", "w") as f:
        json.dump({
            "name": name, "tier": tier, "version": version,
            "sources": c_files, "built_at": datetime.datetime.now().isoformat(),
            "status": "registered",
            # placement — "remote" (default, unset) | "local" | "hybrid".
            # See app_manager.h's app_placement_t. Recorded here for now as
            # documentation/a future codegen source, NOT yet consumed at
            # runtime — a compiled app has no path back to its own
            # app.pcat, and purr_module_header_t has no spare room left to
            # carry this without an ABI change (see app_manager.c's own
            # s_placement_table comment). Until that codegen step exists,
            # actually taking effect means adding a matching entry to
            # app_manager.c's s_placement_table by hand.
            "placement": cfg.get("placement", "remote"),
        }, f, indent=2)
    info(f"    registered — included in next purrstrap build")
    return True

SYSTEM_APPS = {
    "magicmac": (MAGICMAC_DIR, "claw",
                 "MagicMac — Mac OS inspired shell, full kernel API"),
    "magidos":  (MAGIDOS_DIR,  "claw",
                 "MagiDOS — DOS inspired shell, full kernel API"),
}

def _build_system_app(key):
    app_dir, tier, desc = SYSTEM_APPS[key]
    div()
    print(f"{C_BOLD}{key}{C_RST}  —  {desc}")
    div()
    if not os.path.isdir(app_dir):
        warn(f"{app_dir}/ not found — '{key}' has not been created yet")
        return
    pcat = os.path.join(app_dir, "app.pcat")
    build_app(key, app_dir, pcat if os.path.isfile(pcat) else pcat, tier)
    div()

def cmd_build(args):
    target = args.target

    # Special targets: magicmac, magidos
    if target in SYSTEM_APPS:
        _build_system_app(target)
        return

    if target == "all":
        # Build all user apps + system apps
        apps = find_apps()
        div()
        info(f"building {len(apps)} user app(s) + {len(SYSTEM_APPS)} system app(s)")
        div()
        for name, app_dir, pcat, tier in apps:
            build_app(name, app_dir, pcat, tier)
        for key in SYSTEM_APPS:
            _build_system_app(key)
    else:
        # Find by name in source/apps/
        apps = [a for a in find_apps() if a[0] == target]
        if not apps:
            # Try as direct path
            if os.path.isdir(target):
                pcat = os.path.join(target, "app.pcat")
                if os.path.isfile(pcat):
                    cfg  = parse_pcat(pcat)
                    tier = cfg.get("tier", "paws")
                    name = os.path.basename(target)
                    build_app(name, target, pcat, tier)
                    return
            die(f"app '{target}' not found — run 'catstrap list' to see options")
        name, app_dir, pcat, tier = apps[0]
        div()
        build_app(name, app_dir, pcat, tier)
    div()

def cmd_clean(args):
    target = getattr(args, "target", "all")
    if target == "all":
        if os.path.isdir(OUT_APPS):
            shutil.rmtree(OUT_APPS)
            info(f"removed {OUT_APPS}")
    else:
        # Remove specific app output
        for ext in ["meow", "hiss", "paws", "claw", "kitten"]:
            p = os.path.join(OUT_APPS, f"{target}.{ext}")
            if os.path.isfile(p):
                os.remove(p)
                info(f"removed {p}")

def cmd_package(args):
    info("package: not yet implemented — build output is the distributable artifact")

# ── CLI ───────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(prog="catstrap", description="PURR OS app builder + SDK")
    sub = parser.add_subparsers(dest="cmd")

    p_build = sub.add_parser("build", help="Build an app or all apps")
    p_build.add_argument("target", help="app name or 'all'")

    p_pkg = sub.add_parser("package", help="Package a built app for distribution")
    p_pkg.add_argument("app_dir")

    p_val = sub.add_parser("validate", help="Syntax-check a .meow/.hiss Lua script")
    p_val.add_argument("file")

    p_sdk = sub.add_parser("sdk", help="SDK management")
    p_sdk.add_argument("sub", choices=["info", "install"])

    p_clean = sub.add_parser("clean", help="Remove build artifacts")
    p_clean.add_argument("target", nargs="?", default="all")

    sub.add_parser("list", help="List all apps")

    args = parser.parse_args()
    dispatch = {
        "build":    cmd_build,
        "package":  cmd_package,
        "validate": cmd_validate,
        "sdk":      cmd_sdk,
        "clean":    cmd_clean,
        "list":     cmd_list,
    }
    if args.cmd not in dispatch:
        parser.print_help()
        sys.exit(0)
    dispatch[args.cmd](args)

if __name__ == "__main__":
    main()
