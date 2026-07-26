#!/usr/bin/env python3
"""
modulestrap — PURR OS kernel module + driver compiler

Compiles .purr kernel module blobs (drivers, system modules, miniwin, etc.)
from source/modules/ and source/drivers/. Each blob is a self-contained
precompiled binary with a purr_module_header_t at its start.

Usage:
  modulestrap build <module>        compile one module by name
  modulestrap build all             compile all modules and drivers
  modulestrap build drivers         compile all drivers only
  modulestrap build modules         compile all system modules only
  modulestrap list                  list all buildable targets
  modulestrap clean [module|all]    remove compiled .purr blobs

Output: cattobaked/modules/<name>.purr
        cattobaked/drivers/<type>/<name>.purr
"""

import argparse
import datetime
import json
import os
import shutil
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

def info(msg):        print(f"{C_GRN}[modulestrap]{C_RST} {msg}")
def warn(msg):        print(f"{C_YLW}[warn]       {C_RST} {msg}")
def die(msg, code=1): print(f"{C_RED}[err]        {C_RST} {msg}", file=sys.stderr); sys.exit(code)
def div():            print(f"{C_GRY}" + "─" * 52 + C_RST)

REPO_DIR         = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_DIR       = os.path.join(REPO_DIR, "source")
MODULES_DIR      = os.path.join(SOURCE_DIR, "modules")
DRIVERS_DIR      = os.path.join(SOURCE_DIR, "drivers")
USER_DRIVERS_DIR = os.path.join(REPO_DIR, "user_drivers")   # community/custom drivers
OUTPUT_DIR       = os.path.join(REPO_DIR, "cattobaked")
OUT_MODULES      = os.path.join(OUTPUT_DIR, "modules")
OUT_DRIVERS      = os.path.join(OUTPUT_DIR, "drivers")

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

# ── Target discovery ─────────────────────────────────────────────────────────

def find_modules(extra_driver_dirs=None):
    """Return list of (slug, source_dir, pcat_path, kind) for all buildable targets."""
    targets = []

    # System modules: source/modules/<name>/module.pcat
    if os.path.isdir(MODULES_DIR):
        for name in sorted(os.listdir(MODULES_DIR)):
            pcat = os.path.join(MODULES_DIR, name, "module.pcat")
            if os.path.isfile(pcat):
                targets.append((name, os.path.join(MODULES_DIR, name), pcat, "module"))

    # Built-in drivers: source/drivers/<type>/<name>/driver.pcat
    driver_roots = [DRIVERS_DIR]
    # User/community drivers: user_drivers/<type>/<name>/driver.pcat (or flat user_drivers/<name>/driver.pcat)
    if os.path.isdir(USER_DRIVERS_DIR):
        driver_roots.append(USER_DRIVERS_DIR)
    # Extra paths passed via --drivers flag
    if extra_driver_dirs:
        driver_roots.extend(extra_driver_dirs)

    for root in driver_roots:
        if not os.path.isdir(root): continue
        tag = "(user)" if root != DRIVERS_DIR else ""
        for entry in sorted(os.listdir(root)):
            entry_path = os.path.join(root, entry)
            if not os.path.isdir(entry_path): continue
            # Two layouts: <type>/<name>/driver.pcat  OR  <name>/driver.pcat (flat)
            pcat_flat = os.path.join(entry_path, "driver.pcat")
            if os.path.isfile(pcat_flat):
                slug = f"{entry}{tag}"
                targets.append((slug, entry_path, pcat_flat, "driver"))
            else:
                # <type>/<name>/
                for name in sorted(os.listdir(entry_path)):
                    pcat = os.path.join(entry_path, name, "driver.pcat")
                    if os.path.isfile(pcat):
                        slug = f"{entry}/{name}{tag}"
                        targets.append((slug, os.path.join(entry_path, name), pcat, "driver"))

    return targets

def extra_drivers(args):
    return getattr(args, "drivers", None) or []

def cmd_list(args):
    targets = find_modules(extra_drivers(args))
    div()
    print(f"{C_BOLD}Buildable targets ({len(targets)}){C_RST}")
    if os.path.isdir(USER_DRIVERS_DIR):
        print(f"  {C_GRY}+ user_drivers/ included{C_RST}")
    if extra_drivers(args):
        for d in extra_drivers(args):
            print(f"  {C_GRY}+ {d}{C_RST}")
    div()
    for slug, src_dir, pcat, kind in targets:
        cfg = parse_pcat(pcat)
        version = cfg.get("version", "?")
        has_src = any(f.endswith(".c") or f.endswith(".cpp")
                      for f in os.listdir(src_dir) if os.path.isfile(os.path.join(src_dir, f)))
        status = f"{C_GRN}src{C_RST}" if has_src else f"{C_YLW}pcat-only{C_RST}"
        print(f"  {C_CYN}{slug:<34}{C_RST}  v{version:<8}  [{kind}]  {status}")
    div()

# ── Build ─────────────────────────────────────────────────────────────────────

def run_live(cmd, cwd=None, env=None):
    proc = subprocess.Popen(cmd, cwd=cwd, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace")
    try:
        for line in proc.stdout:
            print(line, end="", flush=True)
        proc.wait()
    except KeyboardInterrupt:
        proc.terminate(); proc.wait()
        warn("Interrupted."); sys.exit(0)
    return proc.returncode

def _write_meta(out_path, slug, name, version, kind, c_files, status):
    with open(out_path + ".meta.json", "w") as f:
        json.dump({
            "slug": slug, "name": name, "version": version,
            "kind": kind, "sources": c_files,
            "built_at": datetime.datetime.now().isoformat(),
            "status": status,
        }, f, indent=2)

def build_target(slug, src_dir, pcat_path, kind):
    """
    Register one module or driver for inclusion in the CoreOS IDF build.

    ESP-IDF does not support building isolated components as standalone binaries —
    all modules are compiled as IDF components inside the single CoreOS project.
    modulestrap therefore:
      1. Writes a .meta.json describing the module (source list, version, kind).
      2. Generates a CMakeLists.txt IDF component fragment inside the module dir,
         so purrstrap can include it when running idf.py build on CoreOS.
      3. Updates cattobaked/components_manifest.cmake — CoreOS's CMakeLists.txt
         includes this file to pull in all active modules.

    The actual .purr blob (compiled binary) is produced by purrstrap as part of
    the full CoreOS firmware build, not by modulestrap individually.
    """
    cfg     = parse_pcat(pcat_path)
    name    = cfg.get("name", slug.split("/")[-1])
    version = cfg.get("version", "0.0.0")

    # Find C source files
    c_files = [f for f in os.listdir(src_dir)
               if (f.endswith(".c") or f.endswith(".cpp"))
               and os.path.isfile(os.path.join(src_dir, f))]

    if not c_files:
        warn(f"  {slug}: no C source — pcat-only, skipping")
        return False

    # Determine output metadata path
    if kind == "module":
        out_dir = OUT_MODULES
    else:
        parts   = slug.split("/")
        dtype   = parts[0] if len(parts) > 1 else "misc"
        out_dir = os.path.join(OUT_DRIVERS, dtype)
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, f"{name}.purr")

    info(f"  {C_CYN}{slug}{C_RST}  v{version}  →  {os.path.relpath(out_path, REPO_DIR)}")

    # Generate IDF component CMakeLists.txt inside the module source dir
    # (only if one doesn't already exist — don't clobber hand-written ones)
    cmake_path = os.path.join(src_dir, "CMakeLists.txt")
    # .replace(os.sep, "/"): CMake requires forward slashes even on Windows —
    # see generate_components_manifest()'s comment on the same issue.
    kernel_rel = os.path.relpath(os.path.join(REPO_DIR, "source", "kernel"), src_dir).replace(os.sep, "/")
    src_list   = "\n        ".join(c_files)
    req        = cfg.get("idf_requires", "esp_common driver freertos nvs_flash")
    cmake_txt  = (
        f"# Auto-generated by modulestrap — safe to customize.\n"
        f"idf_component_register(\n"
        f"    SRCS\n"
        f"        {src_list}\n"
        f"    INCLUDE_DIRS\n"
        f"        .\n"
        f"        {kernel_rel}/core\n"
        f"        {kernel_rel}/catcalls\n"
        f"    REQUIRES {req}\n"
        f")\n"
    )
    if not os.path.isfile(cmake_path):
        with open(cmake_path, "w") as f:
            f.write(cmake_txt)
        info(f"    wrote CMakeLists.txt")

    # Write metadata
    _write_meta(out_path, slug, name, version, kind, c_files, "registered")
    return True

def generate_components_manifest(targets):
    """
    Write cattobaked/components_manifest.cmake — included by CoreOS to add
    all registered modules as IDF components. Purrstrap reads this during build.
    """
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    manifest_path = os.path.join(OUTPUT_DIR, "components_manifest.cmake")
    lines = [
        "# components_manifest.cmake — auto-generated by modulestrap",
        "# Include this from CoreOS/CMakeLists.txt:",
        "#   include(${CMAKE_SOURCE_DIR}/../cattobaked/components_manifest.cmake)",
        "",
        "set(PURR_MODULE_DIRS",
    ]
    for slug, src_dir, pcat, kind in targets:
        cfg = parse_pcat(pcat)
        c_files = [f for f in os.listdir(src_dir)
                   if (f.endswith(".c") or f.endswith(".cpp"))
                   and os.path.isfile(os.path.join(src_dir, f))]
        if c_files:
            # CMake requires forward slashes even on Windows — backslash is
            # a string escape character there, so os.path.relpath()'s native
            # separator breaks the generated file (e.g. "\m" isn't a valid
            # escape, causing a cryptic "Syntax error in cmake code").
            rel = os.path.relpath(src_dir, REPO_DIR).replace(os.sep, "/")
            lines.append(f"    ${{CMAKE_SOURCE_DIR}}/../{rel}")

    # Built-in apps: source/apps/<group>/<name>/app.pcat + CMakeLists.txt
    # Compiled into firmware as static modules, not loaded from SPIFFS.
    #
    # "exclusive" was previously missing from this list, which is why the
    # in-house exclusives could never build: device.pcat's [apps] would put them
    # in the generated glue, the glue would reference purr_module_<name>, and the
    # link would then fail with an undefined reference because the component
    # directory was never added to EXTRA_COMPONENT_DIRS in the first place.
    # Adding an app.pcat alone does not fix it — the directory has to be scanned.
    for group in ("system", "exclusive"):
        group_dir = os.path.join(SOURCE_DIR, "apps", group)
        if not os.path.isdir(group_dir):
            continue
        for app_name in sorted(os.listdir(group_dir)):
            app_dir = os.path.join(group_dir, app_name)
            if (os.path.isdir(app_dir)
                    and os.path.isfile(os.path.join(app_dir, "app.pcat"))
                    and os.path.isfile(os.path.join(app_dir, "CMakeLists.txt"))):
                rel = os.path.relpath(app_dir, REPO_DIR).replace(os.sep, "/")
                lines.append(f"    ${{CMAKE_SOURCE_DIR}}/../{rel}")

    lines += [")", ""]
    lines += [
        "foreach(comp_dir ${PURR_MODULE_DIRS})",
        "    list(APPEND EXTRA_COMPONENT_DIRS ${comp_dir})",
        "endforeach()",
    ]
    with open(manifest_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    info(f"components manifest → {os.path.relpath(manifest_path, REPO_DIR)}")

def cmd_build(args):
    targets = find_modules(extra_drivers(args))
    target_arg = args.target

    if target_arg == "all":
        selected = targets
    elif target_arg == "modules":
        selected = [(s, d, p, k) for s, d, p, k in targets if k == "module"]
    elif target_arg == "drivers":
        selected = [(s, d, p, k) for s, d, p, k in targets if k == "driver"]
    else:
        # Find by exact slug or name
        selected = [(s, d, p, k) for s, d, p, k in targets
                    if s == target_arg or s.endswith("/" + target_arg)]
        if not selected:
            die(f"unknown target '{target_arg}' — run 'modulestrap list' to see options")

    div()
    info(f"registering {len(selected)} target(s)")
    div()
    ok = err = skip = 0
    for slug, src_dir, pcat, kind in selected:
        result = build_target(slug, src_dir, pcat, kind)
        if result is True:  ok += 1
        elif result is False: skip += 1
        else: err += 1

    # Regenerate the full components manifest from ALL targets (not just selected)
    all_targets = find_modules(extra_drivers(args))
    generate_components_manifest(all_targets)

    div()
    info(f"done — {ok} registered, {skip} skipped (pcat-only), {err} errors")
    info(f"Run 'purrstrap build <device>' to compile via IDF.")

def cmd_clean(args):
    target = getattr(args, "target", "all")
    if target == "all":
        for d in [OUT_MODULES, OUT_DRIVERS]:
            if os.path.isdir(d):
                shutil.rmtree(d)
                info(f"removed {d}")
    else:
        warn(f"targeted clean not yet implemented — use 'clean all'")

# ── CLI ───────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(prog="modulestrap",
                                     description="PURR OS kernel module compiler")
    sub = parser.add_subparsers(dest="cmd")

    p_build = sub.add_parser("build", help="Compile module(s) into .purr blobs")
    p_build.add_argument("target", help="module name, driver slug, 'all', 'modules', or 'drivers'")
    p_build.add_argument("--drivers", nargs="+", metavar="DIR",
                         help="extra driver directories to include (e.g. ~/my_drivers)")

    p_clean = sub.add_parser("clean", help="Remove compiled .purr blobs")
    p_clean.add_argument("target", nargs="?", default="all")

    p_list = sub.add_parser("list", help="List all buildable targets")
    p_list.add_argument("--drivers", nargs="+", metavar="DIR",
                        help="extra driver directories to include")

    args = parser.parse_args()
    dispatch = {"build": cmd_build, "clean": cmd_clean, "list": cmd_list}
    if args.cmd not in dispatch:
        parser.print_help()
        sys.exit(0)
    dispatch[args.cmd](args)

if __name__ == "__main__":
    main()
