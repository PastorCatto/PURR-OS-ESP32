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
import io
import json
import os
import re
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


# ── Device-aware component selection ─────────────────────────────────────────
#
# Historically this file emitted EVERY discovered component into one global
# PURR_MODULE_DIRS list, identical for all 12 devices, and never looked at
# device.pcat at all. Selection happened in two other places instead:
#
#   * purrstrap's generated glue, which decides what gets REGISTERED
#   * a hand-maintained EXCLUDE_COMPONENTS denylist in CoreOS/CMakeLists.txt
#
# Three sources of truth, none authoritative, and they could disagree. Every
# multi-device build failure in the DP8 bake came from exactly that:
#
#   tab5        device.pcat said ui = "cupcake" while EXCLUDE_COMPONENTS removed
#               the component, so the glue referenced a module that was never
#               compiled — "undefined reference to purr_module_cupcake", then a
#               week of silently stale binaries.
#   meshtastic  compiled into all 12 devices, nanopb and the whole protobuf set
#               included, on boards with no LoRa.
#   m5tab5_bsp  compiled everywhere until hand-excluded ("esp_lcd_mipi_dsi.h:
#               No such file" on waveshare169).
#   heltec      built msn/meshtastic/proximity despite listing none of them.
#
# Now device.pcat is the single source of truth: a component is compiled if that
# device references it, or if something it references pulls it in. A
# contradiction like tab5's is no longer expressible.

def _component_index(targets):
    """slug -> source_dir for every discoverable component (modules, drivers, apps)."""
    index = {}
    for _slug, src_dir, _pcat, _kind in targets:
        # Key by DIRECTORY BASENAME, not find_modules()'s slug. Drivers get a
        # "<type>/<name>" slug there (e.g. "display/st7789"), while ESP-IDF names
        # a component after its directory and device.pcat writes the bare name —
        # so the slug form matches neither and every driver silently failed to
        # resolve when this was first written.
        index[os.path.basename(src_dir)] = src_dir
    for group in ("system", "exclusive"):
        group_dir = os.path.join(SOURCE_DIR, "apps", group)
        if not os.path.isdir(group_dir):
            continue
        for app_name in sorted(os.listdir(group_dir)):
            app_dir = os.path.join(group_dir, app_name)
            if (os.path.isdir(app_dir)
                    and os.path.isfile(os.path.join(app_dir, "app.pcat"))
                    and os.path.isfile(os.path.join(app_dir, "CMakeLists.txt"))):
                index[app_name] = app_dir
    return index


def _component_requires(src_dir):
    """Component slugs this component REQUIRES, ours only.

    Read from CMakeLists.txt rather than declared in the .pcat, deliberately:
    the CMake file is what the build actually obeys, so a .pcat that drifted
    from it would reintroduce exactly the two-sources-of-truth problem this
    whole change removes. ESP-IDF's own component names do not appear in our
    index and fall out on their own.

    Line-based rather than a regex over the whole file. REQUIRES lists here run
    across several lines and end at the closing paren or the next ALL_CAPS
    keyword, which a regex expresses badly and a small state machine expresses
    plainly.
    """
    cm = os.path.join(src_dir, "CMakeLists.txt")
    if not os.path.isfile(cm):
        return set()
    try:
        text = io.open(cm, encoding="utf-8", errors="replace").read()
    except OSError:
        return set()

    KEYWORDS = ("SRCS", "INCLUDE_DIRS", "PRIV_INCLUDE_DIRS", "REQUIRES",
                "PRIV_REQUIRES", "EMBED_FILES", "EMBED_TXTFILES",
                "LDFRAGMENTS", "KCONFIG", "KCONFIG_PROJBUILD", "WHOLE_ARCHIVE")

    out = set()
    collecting = False
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()      # strip comments
        if not line:
            continue
        toks = line.replace("(", " ").replace(")", " ").split()
        for tok in toks:
            if tok in ("REQUIRES", "PRIV_REQUIRES"):
                collecting = True
                continue
            if tok in KEYWORDS or tok.startswith("idf_component_register"):
                collecting = False
                continue
            if collecting:
                name = tok.strip('"${}')
                if name and name.replace("_", "").replace("-", "").isalnum():
                    out.add(name)
    return out


def _device_referenced(cfg):
    """Component slugs this device's device.pcat names directly."""
    want = set()
    for key, val in cfg.items():
        if not isinstance(val, str):
            continue
        v = val.strip().strip('"')
        if key.startswith("drivers.") and v:
            want.add(v)
        elif key.startswith("modules.") and v:
            # modules.radio_companion is a policy flag, not a component.
            if v.lower() not in ("true", "false", "0", "1", "yes", "no"):
                want.add(v)
        elif key.startswith("apps.") and v.lower() in ("true", "1", "yes"):
            want.add(key.split(".", 1)[1])
    return want


# Always built regardless of device.pcat, because purrstrap's generated glue
# registers them for every device — selection has to match the glue exactly or
# the link fails with "undefined reference to purr_module_<name>".
#
#   boot_splash    — main REQUIREs it, and speed_demon draws its restore screen
#   app_manager    — launches anything at all
#   driver_manager — _generate_glue() appends it unconditionally ("driver_manager
#                    is always included if present"), whether or not any
#                    device.pcat mentions it. Missing this was caught by heltec.
CORE_COMPONENTS = {"boot_splash", "app_manager", "driver_manager"}


def select_components(cfg, targets):
    """Transitive closure of what this device actually needs.

    Returns (ordered slugs, ordered dirs).
    """
    index = _component_index(targets)
    frontier = (_device_referenced(cfg) | CORE_COMPONENTS) & set(index)
    seen = set()
    while frontier:
        slug = frontier.pop()
        if slug in seen:
            continue
        seen.add(slug)
        for dep in _component_requires(index[slug]):
            if dep in index and dep not in seen:
                frontier.add(dep)
    ordered = sorted(seen)
    return ordered, [index[s] for s in ordered]


def generate_components_manifest(targets, cfg=None, device=None):
    """
    Write cattobaked/components_manifest.cmake — included by CoreOS to add
    this device's components as IDF components.

    cfg is a parsed device.pcat. Given one, only the components that device
    needs are emitted (see select_components). Without one — the bare
    `modulestrap build all` CLI, which has no device — every discoverable
    component is emitted, preserving the original behaviour for that path.
    """
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    manifest_path = os.path.join(OUTPUT_DIR, "components_manifest.cmake")

    if cfg is not None:
        slugs, dirs = select_components(cfg, targets)
        header_note = (f"# Selected for device: {device or cfg.get('device.name', '?')}\n"
                       f"# {len(slugs)} component(s), chosen from device.pcat and the\n"
                       "# transitive REQUIRES of what it names. Rerun purrstrap to refresh.")
    else:
        # No device in hand (the bare `modulestrap build all` CLI). Emit
        # everything, which is what this file always used to do.
        index = _component_index(targets)
        slugs = sorted(index)
        dirs  = [index[x] for x in slugs]
        header_note = ("# No device supplied — emitting every discoverable component.\n"
                       "# purrstrap always passes a device, so a real firmware build\n"
                       "# gets the selected subset instead.")

    lines = [
        "# components_manifest.cmake — auto-generated by modulestrap",
        "# Include this from CoreOS/CMakeLists.txt:",
        "#   include(${CMAKE_SOURCE_DIR}/../cattobaked/components_manifest.cmake)",
        "#",
        header_note,
        "",
        "set(PURR_MODULE_DIRS",
    ]
    for src_dir in dirs:
        c_files = [f for f in os.listdir(src_dir)
                   if (f.endswith(".c") or f.endswith(".cpp"))
                   and os.path.isfile(os.path.join(src_dir, f))]
        # A component with no sources of its own would register an empty IDF
        # component; harmless, but nothing to build, so it is skipped exactly as
        # before.
        if not c_files:
            continue
        # CMake requires forward slashes even on Windows — backslash is a string
        # escape there, so a native separator breaks the generated file with a
        # cryptic "Syntax error in cmake code".
        rel = os.path.relpath(src_dir, REPO_DIR).replace(os.sep, "/")
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


# ── Per-device module toggling ───────────────────────────────────────────────
#
# device.pcat is the single source of truth for what a device contains (see
# select_components), so toggling a module means editing that file — not a
# parallel list somewhere else, which is the mistake that let tab5's UI backend
# and the component exclusion list disagree for a week.
#
# Edits are line-based rather than parse-and-rewrite. A .pcat carries a lot of
# hard-won commentary — why a device is on one radio driver and not another,
# what broke last time someone changed it — and a naive round-trip through
# parse_pcat() would silently delete every word of it.

DEVICES_DIR = os.path.join(SOURCE_DIR, "devices")

# Refused: purrstrap's generated glue registers these for every device whether
# or not device.pcat mentions them, so "disabling" one only breaks the link with
# an undefined reference to purr_module_<name>.
UNTOGGLEABLE = {"app_manager", "driver_manager"}

# Auto-added by purrstrap's apply_radio_componion_defaults() to any device with
# [radio] wifi = true, via cfg.setdefault(). Commenting the line out therefore
# does NOT disable them — setdefault sees the key as absent and puts it straight
# back, so the command appeared to succeed and changed nothing.
#
# setdefault does respect a key that IS present, and purrstrap's glue skips
# empty values (`if raw_key.startswith("modules.") and raw_val`), so an explicit
# empty assignment is what actually suppresses one.
RADIO_COMPANIONS = {"proximity", "pairing", "proximity_rpc",
                    "app_manager_remote", "homebase", "msn_relay"}


def _device_pcat(device):
    p = os.path.join(DEVICES_DIR, device, "device.pcat")
    if not os.path.isfile(p):
        die(f"no such device: {device} (expected {os.path.relpath(p, REPO_DIR)})")
    return p


def _read_lines(path):
    return io.open(path, encoding="utf-8").read().splitlines()


def _write_lines(path, lines):
    io.open(path, "w", encoding="utf-8", newline="").write("\n".join(lines) + "\n")


def _section_bounds(lines, name):
    """(start, end) line indices of [name]'s body, or (None, None)."""
    start = None
    for i, line in enumerate(lines):
        st = line.strip()
        if st.startswith("[") and st.endswith("]"):
            if st[1:-1].strip() == name:
                start = i + 1
            elif start is not None:
                return start, i
    return (start, len(lines)) if start is not None else (None, None)


def _resolve_device_cfg(device):
    """device.pcat as purrstrap will actually see it.

    Goes through purrstrap.resolve_device() rather than parse_pcat() directly,
    because purrstrap applies apply_radio_companion_defaults() — silently adding
    proximity/pairing/msn_relay/homebase/app_manager_remote to any device with
    [radio] wifi = true. Reading the raw file under-reports what really builds
    (heltec: 13 vs the true 17), and a count that disagrees with the build is
    worse than no count at all.

    Falls back to the raw parse if purrstrap is not importable, so this command
    still works in a bare checkout.
    """
    path = _device_pcat(device)
    try:
        purrstrap_dir = os.path.join(REPO_DIR, "purrstrap")
        if purrstrap_dir not in sys.path:
            sys.path.insert(0, purrstrap_dir)
        import purrstrap
        cfg, _ = purrstrap.resolve_device(device)
        return cfg, path
    except Exception:
        return parse_pcat(path), path


def cmd_modules(args):
    """Show every module/app this device has on, and what is available but off."""
    device = args.device
    cfg, _path = _resolve_device_cfg(device)
    targets = find_modules()
    index = _component_index(targets)

    on_modules, on_apps = {}, []
    for k, v in sorted(cfg.items()):
        if k.startswith("modules.") and k != "modules.radio_companion" and v:
            on_modules[k.split(".", 1)[1]] = v
        elif k.startswith("apps.") and v.lower() in ("true", "1", "yes"):
            on_apps.append(k.split(".", 1)[1])

    selected, _dirs = select_components(cfg, targets)

    print(f"\n{C_BOLD}{device}{C_RST}  —  {len(selected)} component(s) compiled "
          f"of {len(index)} discoverable\n")

    print(f"{C_BOLD}[modules] on{C_RST}")
    for role, mod in on_modules.items():
        mark = "" if mod in selected else f"  {C_YLW}(not compiled!){C_RST}"
        print(f"  {role:<16} = {mod}{mark}")

    if on_apps:
        print(f"\n{C_BOLD}[apps] on{C_RST}")
        print("  " + "  ".join(sorted(on_apps)))

    off = sorted(set(index) - set(selected))
    if off:
        print(f"\n{C_BOLD}available but off{C_RST}")
        for i in range(0, len(off), 5):
            print("  " + "  ".join(f"{n:<20}" for n in off[i:i + 5]).rstrip())
    print()


def _set_app(lines, name, on):
    """Flip an [apps] entry, adding it if absent. Returns (lines, what_happened)."""
    start, end = _section_bounds(lines, "apps")
    val = "true" if on else "false"
    if start is None:
        lines = lines + ["", "[apps]", f"{name:<12}= {val}"]
        return lines, "added [apps] section"
    for i in range(start, end):
        st = lines[i].strip()
        if st.startswith("#") or "=" not in st:
            continue
        if st.split("=", 1)[0].strip() == name:
            lines[i] = f"{name:<12}= {val}"
            return lines, f"set apps.{name} = {val}"
    lines.insert(end, f"{name:<12}= {val}")
    return lines, f"added apps.{name} = {val}"


def _set_module(lines, name, on):
    """Enable/disable a [modules] entry. Returns (lines, what_happened)."""
    start, end = _section_bounds(lines, "modules")
    if start is None:
        if not on:
            return lines, "nothing to do (no [modules] section)"
        return lines + ["", "[modules]", f'{name:<12}= "{name}"'], "added [modules] section"

    for i in range(start, end):
        st = lines[i].strip()
        if "=" not in st:
            continue
        commented = st.startswith("#")
        body = st.lstrip("#").strip()
        key, _, val = body.partition("=")
        val = val.split("#", 1)[0]          # inline comment is not the value
        key, val = key.strip(), val.strip().strip('"')
        if val != name and key != name:
            continue
        if on and commented:
            lines[i] = body                      # uncomment, keep the role key
            return lines, f"re-enabled {key} = {val}"
        if on and not commented:
            if not val:
                # An explicitly-emptied companion (see RADIO_COMPANIONS). The key
                # is present, so the old check called this "already on" and did
                # nothing — the one state disable() actually produces was the one
                # enable() could not undo.
                lines[i] = f'{key:<12}= "{name}"'
                return lines, f're-enabled {key} = "{name}"'
            return lines, f"already on ({key} = {val})"
        if not on and not commented:
            if name in RADIO_COMPANIONS:
                # No trailing comment: parse_pcat() does not strip inline
                # comments, so anything after the value becomes PART of it and
                # would reach purrstrap's glue as a module name to link.
                lines[i] = f'{key:<12}= ""'
                return lines, f'disabled {key} (explicit empty — see RADIO_COMPANIONS)'
            # Comment out rather than delete: the line records WHICH ROLE this
            # module filled, which is not recoverable from the module name alone.
            lines[i] = "# " + lines[i].lstrip()
            return lines, f"disabled {key} = {val}"
        return lines, f"already off ({key} = {val})"

    if on:
        lines.insert(end, f'{name:<12}= "{name}"')
        return lines, f'added {name} = "{name}"'
    if name in RADIO_COMPANIONS:
        # Not written in the file, but purrstrap will add it. An explicit empty
        # value is the only thing that stops that.
        lines.insert(end, f'{name:<12}= ""')
        return lines, f"disabled {name} (was auto-added by radio companions)"
    return lines, "not present, nothing to disable"


def _toggle(args, on):
    device, name = args.device, args.name
    if name in UNTOGGLEABLE:
        die(f"'{name}' is registered for every device by purrstrap's generated "
            f"glue — turning it off only breaks the link. Refusing.")

    path  = _device_pcat(device)
    lines = _read_lines(path)

    targets = find_modules()
    index   = _component_index(targets)
    before, _ = select_components(_resolve_device_cfg(device)[0], targets)
    is_app  = os.path.isdir(os.path.join(SOURCE_DIR, "apps", "system", name)) or \
              os.path.isdir(os.path.join(SOURCE_DIR, "apps", "exclusive", name))

    if on and name not in index:
        die(f"unknown component '{name}' — run `modulestrap list` to see what exists")

    lines, what = (_set_app(lines, name, on) if is_app
                   else _set_module(lines, name, on))
    _write_lines(path, lines)
    info(f"{device}: {what}")

    # Report the DELTA, not just a total. Turning a module off does not always
    # shrink the build: another component may REQUIRE it, in which case it still
    # compiles and only stops being REGISTERED. Saying "17 components" both
    # before and after, with no explanation, reads like the command did nothing.
    cfg, _ = _resolve_device_cfg(device)
    after, _d = select_components(cfg, targets)

    if len(after) == len(before):
        info(f"{len(after)} component(s) compile for {device} (unchanged)")
    else:
        arrow = "->" if len(after) < len(before) else "->"
        info(f"components for {device}: {len(before)} {arrow} {len(after)}")

    if not on and name in after:
        holders = [c for c in after
                   if c != name and name in _component_requires(index.get(c, ""))]
        if holders:
            who = ", ".join(sorted(holders)[:3])
            warn(f"'{name}' still COMPILES — {who} REQUIRES it. It is no longer "
                 f"registered at boot, which is what disabling controls; removing "
                 f"the code too means dropping whatever still needs it.")
        else:
            warn(f"'{name}' is still selected and nothing REQUIRES it — most "
                 f"likely purrstrap re-added it. Check [radio] wifi and "
                 f"modules.radio_companion in this device.pcat.")

    info("run `purrstrap build " + device + "` to apply")


def cmd_enable(args):
    _toggle(args, True)


def cmd_disable(args):
    _toggle(args, False)


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

    p_mods = sub.add_parser("modules",
                            help="Show which modules/apps a device has on, and what is available")
    p_mods.add_argument("device", help="device name (source/devices/<name>/)")

    p_en = sub.add_parser("enable", help="Turn a module or app on for a device")
    p_en.add_argument("name", help="module or app name")
    p_en.add_argument("device", help="device name")

    p_dis = sub.add_parser("disable", help="Turn a module or app off for a device")
    p_dis.add_argument("name", help="module or app name")
    p_dis.add_argument("device", help="device name")

    args = parser.parse_args()
    dispatch = {"build": cmd_build, "clean": cmd_clean, "list": cmd_list,
                "modules": cmd_modules, "enable": cmd_enable, "disable": cmd_disable}
    if args.cmd not in dispatch:
        parser.print_help()
        sys.exit(0)
    dispatch[args.cmd](args)

if __name__ == "__main__":
    main()
