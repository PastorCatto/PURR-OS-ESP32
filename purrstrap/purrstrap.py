#!/usr/bin/env python3
"""
purrstrap — PURR OS final image builder

Reads source/devices/<device>/device.pcat, resolves all driver + module
dependencies, generates glue, calls IDF to compile the kernel spine, then
assembles the final flashable image in cattobaked/<device>/.

Usage:
  purrstrap build <device>              build firmware for device
  purrstrap flash <device> [-p]         build + flash to connected device
  purrstrap monitor <device> [-p] [-b]  open serial monitor (idf_monitor.py)
  purrstrap clean <device>              remove build artifacts for device
  purrstrap list                        list supported devices (reads source/devices/)
  purrstrap status                      show current .purrstrap workspace config
  purrstrap doctor                      check environment health (IDF, tools present)

Output: cattobaked/<device>/
  firmware.bin          complete merged flash image
  bootloader.bin
  partition-table.bin
  purr_kernel.bin       kernel spine only
  build.json            build metadata (device, versions, timestamp)
"""

import argparse
import datetime
import difflib
import glob
import json
import os
import re
import shutil
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

PURROS_VERSION = "1.0.0-dp7"
KITT_VERSION   = "1.0.0"

def info(msg):        print(f"{C_GRN}[purrstrap]{C_RST} {msg}")
def warn(msg):        print(f"{C_YLW}[warn]     {C_RST} {msg}")
def die(msg, code=1): print(f"{C_RED}[err]      {C_RST} {msg}", file=sys.stderr); sys.exit(code)
def div(label=""):
    if label:
        line = f"─ {label} " + "─" * max(0, 52 - len(label) - 2)
    else:
        line = "─" * 52
    print(f"{C_GRY}{line}{C_RST}")

REPO_DIR    = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_DIR  = os.path.join(REPO_DIR, "source")
DEVICES_DIR = os.path.join(SOURCE_DIR, "devices")
KERNEL_DIR  = os.path.join(SOURCE_DIR, "kernel")
OUTPUT_DIR  = os.path.join(REPO_DIR, "cattobaked")
CFG_FILE    = os.path.join(REPO_DIR, ".purrstrap")

# ── .pcat parser (minimal TOML-subset) ───────────────────────────────────────

def parse_pcat(path):
    """Parse a .pcat file into a flat dict. Handles [sections] and key=value."""
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

# ── Device listing ────────────────────────────────────────────────────────────

def list_devices():
    if not os.path.isdir(DEVICES_DIR):
        die(f"source/devices/ not found — run from repo root or check SOURCE_DIR")
    devices = []
    for entry in sorted(os.listdir(DEVICES_DIR)):
        pcat = os.path.join(DEVICES_DIR, entry, "device.pcat")
        if os.path.isfile(pcat):
            cfg = parse_pcat(pcat)
            chip = cfg.get("device.chip", "?")
            name = cfg.get("device.name", entry)
            devices.append((entry, name, chip))
    return devices

def cmd_list(args):
    div()
    print(f"{C_BOLD}Supported devices{C_RST}")
    div()
    for slug, name, chip in list_devices():
        print(f"  {C_CYN}{slug:<20}{C_RST}  {name}  ({chip})")
    div()

# ── Status ────────────────────────────────────────────────────────────────────

def cmd_status(args):
    if not os.path.isfile(CFG_FILE):
        warn("no .purrstrap workspace config found — run 'purrstrap build <device>' to create one")
        return
    with open(CFG_FILE) as f:
        cfg = json.load(f)
    div()
    print(f"{C_BOLD}purrstrap workspace{C_RST}")
    div()
    for k, v in cfg.items():
        print(f"  {C_CYN}{k:<20}{C_RST}  {v}")
    div()

# ── Doctor ────────────────────────────────────────────────────────────────────

def cmd_doctor(args):
    div()
    print(f"{C_BOLD}purrstrap doctor{C_RST}")
    div()
    checks = [
        ("idf.py",        "IDF build tool"),
        ("esptool.py",    "esptool (flash)"),
        ("python3",       "Python 3"),
        ("git",           "Git"),
    ]
    all_ok = True
    for tool, desc in checks:
        found = shutil.which(tool)
        if found:
            print(f"  {C_GRN}[OK]{C_RST}  {desc:<20}  {found}")
        else:
            print(f"  {C_RED}[XX]{C_RST}  {desc:<20}  not found")
            all_ok = False
    # Check IDF_PATH
    idf_path = os.environ.get("IDF_PATH", "")
    if idf_path and os.path.isdir(idf_path):
        print(f"  {C_GRN}[OK]{C_RST}  IDF_PATH              {idf_path}")
    else:
        print(f"  {C_RED}[XX]{C_RST}  IDF_PATH              not set or invalid")
        all_ok = False
    # Check spiffsgen
    idf = _idf_path()
    sg = _spiffsgen(idf) if idf else None
    if sg:
        print(f"  {C_GRN}[OK]{C_RST}  spiffsgen             {sg}")
    else:
        print(f"  {C_YLW}[--]{C_RST}  spiffsgen             not found (IDF_PATH required)")
        all_ok = False
    # Check source tree
    for rel in ["source/kernel/core/boot.c", "source/kernel/core/purr_kernel.h"]:
        p = os.path.join(REPO_DIR, rel)
        if os.path.isfile(p):
            print(f"  {C_GRN}[OK]{C_RST}  {rel}")
        else:
            print(f"  {C_RED}[XX]{C_RST}  {rel}  — missing")
            all_ok = False
    div()
    if all_ok:
        info("all checks passed")
    else:
        warn("some checks failed — fix above before building")

# ── Build helpers ─────────────────────────────────────────────────────────────

def resolve_device(device_slug):
    pcat_path = os.path.join(DEVICES_DIR, device_slug, "device.pcat")
    if not os.path.isfile(pcat_path):
        die(f"no device.pcat found for '{device_slug}' — check source/devices/")
    cfg = parse_pcat(pcat_path)
    apply_radio_companion_defaults(cfg)
    return cfg, pcat_path

# 2nd-stage bootloader flash offset — chip-specific, dictated by the ROM
# bootloader (not configurable in ESP-IDF itself). Confirmed against IDF's
# own default in components/bootloader/Kconfig.projbuild:
#   default 0x1000 if IDF_TARGET_ESP32 || IDF_TARGET_ESP32S2
#   default 0x2000 if IDF_TARGET_ESP32P4 || IDF_TARGET_ESP32C5
#   default 0x0    (everything else — esp32s3, esp32c3, esp32c6, esp32h2, ...)
# Single source of truth for both call sites (_merge_flash_image() and
# cmd_flash()) — they used to each carry their own copy of this mapping,
# and esp32s2 was wrong in both (mapped to 0x0 instead of 0x1000), just
# never exercised since no esp32s2 device.pcat exists in this tree yet.
def bootloader_offset(chip):
    if chip in ("esp32", "esp32s2"):
        return "0x1000"
    if chip in ("esp32p4", "esp32c5"):
        return "0x2000"
    return "0x0"

# ── Radio companion capability defaults ──────────────────────────────────────
# Every device with WiFi gets ESP-NOW proximity discovery + pairing "for
# free," rather than needing it hand-wired into each device.pcat one at a
# time: proximity_module.c/pairing_module.c already handle no-PSRAM and
# no-LoRa gracefully (see their own header comments), so WiFi is the only
# real hardware requirement. msn + nearby (the apps that let a user actually
# see/act on it) are added on top only when the UI backend implements
# purr_win (catcall_ui_t) — a raw-framebuffer UI like oled_ui needs its own
# custom screen instead (see heltec's hand-built Pair screen in
# oled_ui_module.c) rather than a generic purr_win one.
#
# [modules] radio_companion = true/false overrides this heuristic either
# direction — force it on for a WiFi device this doesn't detect correctly,
# or off for one that technically has WiFi but shouldn't carry this (e.g. a
# stripped-down diagnostic/test kernel).
PURR_WIN_UI_BACKENDS = {"miniwin", "cupcake", "kittenui", "cardstack", "pounce"}

def apply_radio_companion_defaults(cfg):
    """Mutates cfg in place, adding proximity/pairing (+ msn/nearby where the
    UI backend supports it) unless already present or explicitly opted out."""
    flag = cfg.get("modules.radio_companion", "").strip().lower()
    if flag in ("false", "0", "no"):
        return cfg
    forced_on = flag in ("true", "1", "yes")
    wifi = cfg.get("radio.wifi", "").strip().lower() in ("true", "1", "yes")
    if not (forced_on or wifi):
        return cfg

    for key, val in (("modules.proximity", "proximity"), ("modules.pairing", "pairing"),
                      ("modules.proximity_rpc", "proximity_rpc"),
                      ("modules.app_manager_remote", "app_manager_remote"),
                      ("modules.homebase", "homebase"),
                      ("modules.msn_relay", "msn_relay"),
                      ("flash.proximity", "2"), ("flash.pairing", "2"), ("flash.proximity_rpc", "2"),
                      ("flash.app_manager_remote", "2"), ("flash.homebase", "2"), ("flash.msn_relay", "2")):
        cfg.setdefault(key, val)

    if cfg.get("modules.ui", "") in PURR_WIN_UI_BACKENDS:
        for key, val in (("apps.msn", "true"), ("apps.nearby", "true"), ("apps.milkbar", "true"),
                          ("flash.apps/msn", "3"), ("flash.apps/nearby", "3"), ("flash.apps/milkbar", "3")):
            cfg.setdefault(key, val)
    return cfg

def _idf_path():
    p = os.environ.get("IDF_PATH", "")
    if p and os.path.isdir(p):
        return p
    for c in (os.path.expanduser("~/esp/idf"), os.path.expanduser("~/esp/esp-idf"), "/opt/esp-idf"):
        if os.path.isdir(c):
            os.environ["IDF_PATH"] = c
            return c
    return ""

def _idf_venv_python():
    """Locate the ESP-IDF managed Python venv interpreter, cross-platform.

    Prefers IDF_PYTHON_ENV_PATH (set by every export.ps1/export.sh/export.bat,
    including the official Windows installer's C:\\Espressif layout) over
    guessing a directory layout. Falls back to the ~/.espressif Unix installer
    layout, then None if nothing is found.
    """
    import glob as _glob
    env_path = os.environ.get("IDF_PYTHON_ENV_PATH", "")
    if env_path:
        for candidate in (os.path.join(env_path, "Scripts", "python.exe"),
                          os.path.join(env_path, "bin", "python3")):
            if os.path.isfile(candidate):
                return candidate
    for pattern in ("~/.espressif/python_env/idf*/bin/python3",
                    "~/.espressif/python_env/idf*/Scripts/python.exe"):
        matches = sorted(_glob.glob(os.path.expanduser(pattern)))
        if matches:
            return matches[-1]
    return None

def _idf_tools_path(idf_python):
    """Locate IDF_TOOLS_PATH (holds espidf.constraints.vX.Y.txt, toolchains,
    etc). idf.py hard-fails ('espidf.constraints... doesn't exist') without
    this set correctly on installations that don't use the ~/.espressif
    default — e.g. the Windows installer's C:\\Espressif\\tools layout, which
    is 3 levels up from the venv (tools/python/vX.Y.Z/venv)."""
    env_path = os.environ.get("IDF_TOOLS_PATH", "")
    if env_path and os.path.isdir(env_path):
        return env_path
    if idf_python:
        # .../tools/python/vX.Y.Z/venv/Scripts/python.exe -> .../tools (5 levels up)
        candidate = idf_python
        for _ in range(5):
            candidate = os.path.dirname(candidate)
        if os.path.isdir(candidate) and glob.glob(os.path.join(candidate, "espidf.constraints.*")):
            return candidate
    default = os.path.expanduser("~/.espressif")
    return default if os.path.isdir(default) else None

def _idf_toolchain_bin_dirs(idf_tools_path):
    """Resolve cmake/ninja/xtensa-esp-elf/riscv32-esp-elf/ccache bin dirs
    directly from IDF_TOOLS_PATH's on-disk layout (tool/<version>/<extracted>/...),
    same layout `idf_tools.py install` always produces regardless of platform.

    This is a supplement to (not a replacement for) `idf_tools.py export`:
    that command depends on an installed-tools registry file that some
    installer flavors (e.g. the Windows EIM installer's C:\\Espressif\\tools
    layout) don't populate the way idf_tools.py expects, silently reporting
    every tool as 'not installed' and contributing nothing to PATH even
    though the tools are genuinely sitting on disk. Purrstrap must be able to
    build without relying on the user's shell already having these on PATH."""
    if not idf_tools_path or not os.path.isdir(idf_tools_path):
        return []
    patterns = [
        "cmake/*/bin",
        "ninja/*",
        "xtensa-esp-elf/*/xtensa-esp-elf/bin",
        "riscv32-esp-elf/*/riscv32-esp-elf/bin",
        "xtensa-esp-elf-gdb/*/xtensa-esp-elf-gdb/bin",
        "riscv32-esp-elf-gdb/*/riscv32-esp-elf-gdb/bin",
        "esp32ulp-elf/*/esp32ulp-elf/bin",
        "ccache/*/*",
        "dfu-util/*",
        "openocd-esp32/*/openocd-esp32/bin",
        "idf-exe/*",
    ]
    found = []
    for pattern in patterns:
        for match in sorted(glob.glob(os.path.join(idf_tools_path, pattern))):
            if os.path.isdir(match) and match not in found:
                found.append(match)
    return found

def _spiffsgen(idf_path):
    candidate = os.path.join(idf_path, "components", "spiffs", "spiffsgen.py")
    if os.path.isfile(candidate):
        return candidate
    return None

def run_live(cmd, cwd=None, env=None):
    proc = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace")
    try:
        for line in proc.stdout:
            print(line, end="", flush=True)
        proc.wait()
    except KeyboardInterrupt:
        proc.terminate(); proc.wait()
        warn("Interrupted."); sys.exit(0)
    return proc.returncode

# ── Parse [flash] section from device.pcat ────────────────────────────────────
#
# Returns list of (module_slug, priority) tuples.
# module_slug is like "miniwin", "display/st7789", etc.
# These map to cattobaked/modules/<name>.purr or cattobaked/drivers/<type>/<name>.purr

def parse_flash_manifest(cfg):
    """
    Read all 'flash.*' keys from a parsed device.pcat and return a list of
    (slug, priority) tuples that should be baked into the SPIFFS image.
    """
    entries = []
    for key, val in cfg.items():
        if not key.startswith("flash."): continue
        slug = key[len("flash."):]        # e.g. "miniwin", "display/st7789"
        try:
            priority = int(val)
        except ValueError:
            priority = 3
        entries.append((slug, priority))
    return entries

def _find_purr_blob(slug):
    """
    Find the compiled blob for a given slug in cattobaked/.
    Slug can be:
      "miniwin"         → cattobaked/modules/miniwin.purr
      "app_manager"     → cattobaked/modules/app_manager.purr
      "display/st7789"  → cattobaked/drivers/display/st7789.purr
      "apps/terminal"   → cattobaked/apps/terminal.claw (or .paws, .meow, .hiss, .kitten)
    """
    parts = slug.split("/")
    if len(parts) == 2 and parts[0] == "apps":
        name = parts[1]
        app_dir = os.path.join(OUTPUT_DIR, "apps")
        for ext in ("claw", "paws", "meow", "hiss", "kitten"):
            p = os.path.join(app_dir, f"{name}.{ext}")
            if os.path.isfile(p):
                return p
        # Also accept the .meta.json registration as a stand-in (no binary yet)
        meta = os.path.join(app_dir, f"{name}.claw.meta.json")
        if not os.path.isfile(meta):
            meta = os.path.join(app_dir, f"{name}.paws.meta.json")
        if os.path.isfile(meta):
            return meta   # caller will handle .meta.json → stage as placeholder
        return None
    if len(parts) == 1:
        # module
        p = os.path.join(OUTPUT_DIR, "modules", f"{slug}.purr")
        if os.path.isfile(p):
            return p
    else:
        # driver: parts[0]=type, parts[1]=name
        p = os.path.join(OUTPUT_DIR, "drivers", parts[0], f"{parts[1]}.purr")
        if os.path.isfile(p):
            return p
    return None

# ── SPIFFS staging + image generation ────────────────────────────────────────

def build_flash_image(device, pcat_cfg, out_dir, spiffs_size_kb=512):
    """
    Stage userland app files into a SPIFFS image (flash.bin).

    Drivers, system modules, and UI modules are statically linked into the
    firmware via PURR_MODULE_REGISTER() — they do NOT need .purr blobs on
    SPIFFS. Only entries under apps/* in [flash] are staged here.

    Steps:
      1. Run catstrap to build any .meow / .hiss / .paws / .claw app files.
      2. Stage apps/* entries from [flash] into spiffs_staging/apps/.
      3. Run spiffsgen.py to produce flash.bin.

    Returns path to flash.bin or None on failure.
    """
    CATSTRAP_PY = os.path.join(REPO_DIR, "catstrap", "catstrap.py")
    staging_dir = os.path.join(out_dir, "spiffs_staging")
    flash_bin   = os.path.join(out_dir, "flash.bin")

    # ── Step 1: build userland apps ───────────────────────────────────────────
    if os.path.isfile(CATSTRAP_PY):
        info("invoking catstrap to build apps...")
        rc = run_live([sys.executable, CATSTRAP_PY, "build", "all"])
        if rc != 0:
            warn("catstrap exited with errors — some apps may be missing")

    # ── Step 2: read flash manifest and stage apps only ───────────────────────
    flash_entries = parse_flash_manifest(pcat_cfg)

    if os.path.isdir(staging_dir):
        shutil.rmtree(staging_dir)
    os.makedirs(os.path.join(staging_dir, "apps"), exist_ok=True)

    # Bundled wallpapers (source/assets/wallpapers/*.rgb565) — not part of
    # the [flash] manifest system above (they're assets, not a module/
    # driver/app), so staged unconditionally here instead. Mounted at
    # /flash/wallpapers/ at runtime; cupcake_ui.c's load_wallpaper_choice()
    # falls back to this for its "default" wallpaper before giving up and
    # drawing the plain gradient.
    wallpapers_src = os.path.join(REPO_DIR, "source", "assets", "wallpapers")
    wallpapers_dst = os.path.join(staging_dir, "wallpapers")
    staged_wallpapers = 0
    if os.path.isdir(wallpapers_src):
        for fname in sorted(os.listdir(wallpapers_src)):
            if not fname.endswith(".rgb565"):
                continue
            os.makedirs(wallpapers_dst, exist_ok=True)
            src_path = os.path.join(wallpapers_src, fname)
            shutil.copy2(src_path, os.path.join(wallpapers_dst, fname))
            size_kb = os.path.getsize(src_path) // 1024
            print(f"  {C_GRN}[OK]{C_RST}  wallpapers/{fname:<26}  {size_kb} KB")
            staged_wallpapers += 1
    if staged_wallpapers:
        info(f"staged {staged_wallpapers} wallpaper(s) into spiffs_staging/wallpapers/")

    staged = 0

    for slug, priority in sorted(flash_entries, key=lambda x: x[1]):
        parts = slug.split("/")
        prio_str = f"P{priority}"

        # Drivers, system modules, and UI are statically linked — skip them
        if len(parts) != 2 or parts[0] != "apps":
            print(f"  {C_GRN}[**]{C_RST}  {prio_str} {slug:<30}  static (in firmware)")
            continue

        name = parts[1]
        src  = _find_purr_blob(slug)

        if src is None:
            print(f"  {C_YLW}[--]{C_RST}  {prio_str} {slug} — no blob (pre-linked in firmware)")
            continue

        app_staging = os.path.join(staging_dir, "apps")
        if src.endswith(".meta.json"):
            dst = os.path.join(app_staging, f"{name}.meta.json")
        else:
            ext = os.path.splitext(src)[1]
            dst = os.path.join(app_staging, f"{name}{ext}")

        shutil.copy2(src, dst)
        size_kb = os.path.getsize(src) // 1024
        print(f"  {C_GRN}[OK]{C_RST}  {prio_str} {slug:<30}  {size_kb} KB")
        staged += 1

    info(f"staged {staged} app(s) into spiffs_staging/")

    # ── Step 4: run spiffsgen.py ───────────────────────────────────────────────
    idf = _idf_path()
    spiffsgen = _spiffsgen(idf) if idf else None

    if not spiffsgen:
        warn("spiffsgen.py not found — cannot produce flash.bin")
        warn("Set IDF_PATH and activate the IDF environment, then rebuild.")
        return None

    spiffs_size_bytes = spiffs_size_kb * 1024
    cmd = [sys.executable, spiffsgen, str(spiffs_size_bytes), staging_dir, flash_bin]
    info(f"running spiffsgen ({spiffs_size_kb} KB)...")
    rc = run_live(cmd)
    if rc != 0:
        warn(f"spiffsgen exited {rc} — flash.bin may be corrupt or missing")
        return None

    size_kb = os.path.getsize(flash_bin) // 1024
    info(f"flash.bin ready  ({size_kb} KB)  →  {os.path.relpath(flash_bin, REPO_DIR)}")
    return flash_bin

# ── Glue layer generation ─────────────────────────────────────────────────────
#
# Generates purr_device_glue.c for the target device — a thin C file that
# #includes the right driver headers and provides a purr_device_init() that
# configures pin numbers from device.pcat before calling purr_kernel_scan_modules().

def _has_specialized_kernel(device):
    """Return True if source/kernel/kernel_<device>/ exists."""
    spec_dir = os.path.join(KERNEL_DIR, f"kernel_{device}")
    return os.path.isdir(spec_dir)

def _generate_glue(device, cfg, out_dir):
    """Write source/glue/purr_device_glue_<device>.c from device.pcat [pins] section."""
    glue_dir = os.path.join(out_dir, "glue")
    os.makedirs(glue_dir, exist_ok=True)
    glue_path = os.path.join(glue_dir, "purr_device_glue.c")

    chip        = cfg.get("device.chip", "esp32")
    display_drv = cfg.get("drivers.display", "")
    touch_drv   = cfg.get("drivers.touch", "")
    radio_drv   = cfg.get("drivers.radio", "")
    gps_drv     = cfg.get("drivers.gps", "")
    input_drv   = cfg.get("drivers.input", "")

    # Specialized kernels bake display/touch/input into the boot directly.
    # Skip those from the module loader to avoid double-init.
    specialized = _has_specialized_kernel(device)
    if specialized:
        info(f"  specialized kernel detected — display/touch/input are baked in")
        display_drv = ""
        touch_drv   = ""
        input_drv   = ""

    def pin(key, default=-1):
        val = cfg.get(f"pins.{key}", str(default))
        try:    return int(val)
        except: return default

    def fpin(key, default=None):
        val = cfg.get(f"pins.{key}", "")
        if not val:
            return default
        try:    return float(val)
        except: return default

    # Some boards (Heltec WiFi LoRa 32 V3 confirmed) gate onboard peripherals
    # (OLED, sometimes more) behind a separate power rail — "Vext" in
    # Heltec's own naming — that must be driven to its enable level before
    # those peripherals' drivers can talk to them at all. -1 (unset, the
    # default) means this device has no such pin and purr_device_init()
    # stays an empty no-op, as it always was before this existed.
    vext_pin = pin("vext_pin")

    # adc_battery.c's compile-time defaults are T-Deck-shaped (GPIO4/CH3,
    # x2.11, no enable pin) — a board with a different divider pin/ratio,
    # or (Heltec V3, confirmed) an enable pin that must be driven low
    # before the divider is even connected, overrides them via a runtime
    # adc_battery_configure() call instead (see that function's own
    # comment for why not more #ifndef config in the driver itself).
    # battery_adc_channel unset (None) means "this device doesn't need to
    # override adc_battery's defaults" — skips generating the call at all.
    battery_adc_channel    = pin("battery_adc_channel", None) if cfg.get("pins.battery_adc_channel", "") else None
    battery_adc_ctrl_pin   = pin("battery_adc_ctrl_pin", -1)
    battery_adc_multiplier = fpin("battery_adc_multiplier", None)
    # ADC attenuation — ESP32-S3's ADC under-reads badly (near-zero counts)
    # on a high source-impedance divider at the driver's DB_12 default;
    # Meshtastic's own heltec_v3 variant.h uses DB_2_5 for exactly this
    # reason ("lower dB for high resistance voltage divider"). Ordinals
    # match adc_atten_t: DB_0=0, DB_2_5=1, DB_6=2, DB_12=3.
    _ADC_ATTEN_ORDINAL = {"0": 0, "2_5": 1, "6": 2, "12": 3}
    battery_adc_atten = _ADC_ATTEN_ORDINAL.get(str(cfg.get("pins.battery_adc_atten", "12")), 3)

    lines = [
        f"// purr_device_glue.c — auto-generated by purrstrap for {device}",
        f"// Device: {cfg.get('device.name', device)}  Chip: {chip}",
        f"// Do not edit — regenerated on every purrstrap build.",
        "",
        '#include "purr_kernel.h"',
        '#include "purr_module.h"',
    ]
    if vext_pin >= 0:
        lines += [
            '#include "driver/gpio.h"',
            '#include "freertos/FreeRTOS.h"',
            '#include "freertos/task.h"',
        ]
    lines += [""]

    # Pin #defines from [pins] section
    pin_map = {
        "CONFIG_DRV_DISPLAY_CS_PIN":    pin("display_cs"),
        "CONFIG_DRV_DISPLAY_DC_PIN":    pin("display_dc"),
        "CONFIG_DRV_DISPLAY_MOSI_PIN":  pin("display_mosi"),
        "CONFIG_DRV_DISPLAY_SCLK_PIN":  pin("display_sclk"),
        "CONFIG_DRV_DISPLAY_RST_PIN":   pin("display_rst"),
        "CONFIG_DRV_DISPLAY_BL_PIN":    pin("display_bl"),
        "CONFIG_DRV_TOUCH_SDA_PIN":     pin("touch_sda"),
        "CONFIG_DRV_TOUCH_SCL_PIN":     pin("touch_scl"),
        "CONFIG_DRV_TOUCH_INT_PIN":     pin("touch_int", 0xFF),
        "CONFIG_DRV_TOUCH_RST_PIN":     pin("touch_rst", 0xFF),
        "CONFIG_GPS_TX_PIN":            pin("gps_tx", 43),
        "CONFIG_GPS_RX_PIN":            pin("gps_rx", 44),
        "CONFIG_LORA_MOSI_PIN":         pin("lora_mosi"),
        "CONFIG_LORA_MISO_PIN":         pin("lora_miso"),
        "CONFIG_LORA_SCLK_PIN":         pin("lora_sclk"),
        "CONFIG_LORA_CS_PIN":           pin("lora_cs"),
        "CONFIG_LORA_RST_PIN":          pin("lora_rst"),
        "CONFIG_LORA_IRQ_PIN":          pin("lora_irq"),
    }
    for macro, val in pin_map.items():
        lines.append(f"#define {macro} {val}")

    # Radio capability flags from [radio] section
    def radio_bool(key):
        return cfg.get(f"radio.{key}", "false").lower() in ("true", "1", "yes")

    lines += [""]
    lines.append(f"#define CONFIG_PURR_WIFI  {'1' if radio_bool('wifi') else '0'}")
    lines.append(f"#define CONFIG_PURR_BT    {'1' if radio_bool('bt')   else '0'}")
    lora_drv = cfg.get("radio.lora", "")
    lines.append(f"#define CONFIG_PURR_LORA  {'1' if lora_drv else '0'}")
    if lora_drv:
        lines.append(f'#define CONFIG_PURR_LORA_DRIVER "{lora_drv}"')

    # ── Static module registration from [drivers] + [modules] sections ───────
    # Map pcat driver/module values to their purr_module_<id> symbol names.
    # pcat value is the driver name (e.g. "st7789", "generic_nmea").
    # The C symbol is purr_module_<value> with hyphens/slashes replaced by _.
    def to_sym(name):
        return name.replace("-", "_").replace("/", "_")

    module_ids = []
    # Specialized kernels bake display/touch/input in directly; skip them here
    # to avoid double-init. Radio and GPS are still plug-and-play.
    baked_keys = ("drivers.display", "drivers.touch", "drivers.input") if specialized else ()
    for key in ("drivers.display", "drivers.touch", "drivers.input",
                "drivers.radio", "drivers.gps", "drivers.battery"):
        if key in baked_keys:
            continue
        val = cfg.get(key, "")
        if val:
            module_ids.append(to_sym(val))
    # [modules] section — ui, app_manager, etc. "radio_companion" is a
    # control flag (see apply_radio_companion_defaults()), not a module
    # name — excluded here or to_sym("true"/"false") would produce a bogus
    # "extern purr_module_true;" reference.
    for raw_key, raw_val in sorted(cfg.items()):
        if raw_key.startswith("modules.") and raw_val and raw_key != "modules.radio_companion":
            module_ids.append(to_sym(raw_val))
    # driver_manager is always included if present
    if to_sym("driver_manager") not in module_ids:
        module_ids.append("driver_manager")
    # [apps] section — apps compiled into firmware as static modules
    for raw_key, raw_val in sorted(cfg.items()):
        if raw_key.startswith("apps.") and raw_val.lower() in ("true", "1", "yes"):
            app_name = raw_key.split(".", 1)[1]
            app_sym = to_sym(app_name)
            if app_sym not in module_ids:
                module_ids.append(app_sym)

    lines += [""]
    for mid in module_ids:
        lines.append(f"extern purr_module_header_t purr_module_{mid};")

    lines += [
        "",
        "// Generated by purrstrap — registers exactly the modules this device needs.",
        "// Called from boot.c before purr_kernel_load_static_modules().",
        "void purr_register_static_modules(void) {",
    ]
    for mid in module_ids:
        lines.append(f"    purr_kernel_register_module_static(&purr_module_{mid});")
    lines.append("}")

    lines += [
        "",
        "// Called from boot.c before any module (including display/radio",
        "// drivers) is initialized — this is where a board power rail that",
        "// those drivers depend on has to already be live.",
        "void purr_device_init(void) {",
        f"    // Device: {device}",
    ]
    if vext_pin >= 0:
        lines += [
            f"    // Vext power rail — confirmed against Heltec's own arduino-esp32",
            f"    // board variant (pins_arduino.h): active LOW, GPIO{vext_pin} on this",
            f"    // device. Must be enabled before ssd1306/sx1262 init runs, or their",
            f"    // I2C/SPI transactions go out against an unpowered, floating bus.",
            f"    gpio_config_t vext_cfg = {{",
            f"        .pin_bit_mask = 1ULL << {vext_pin},",
            f"        .mode         = GPIO_MODE_OUTPUT,",
            f"        .pull_up_en   = GPIO_PULLUP_DISABLE,",
            f"        .pull_down_en = GPIO_PULLDOWN_DISABLE,",
            f"        .intr_type    = GPIO_INTR_DISABLE,",
            f"    }};",
            f"    gpio_config(&vext_cfg);",
            f"    gpio_set_level({vext_pin}, 0);",
            f"    vTaskDelay(pdMS_TO_TICKS(10));  // let the rail settle before I2C/SPI touch it",
        ]
    if battery_adc_channel is not None and battery_adc_multiplier is not None:
        lines += [
            f"    // Battery ADC — overrides adc_battery.c's T-Deck-shaped defaults",
            f"    // for this board's actual divider pin/ratio (confirmed against",
            f"    // Meshtastic's own variant.h for this device). Must run before",
            f"    // adc_battery's module_init() configures the ADC channel.",
            f"    extern void adc_battery_configure(int channel, int ctrl_pin, float multiplier, int atten);",
            f"    adc_battery_configure({battery_adc_channel}, {battery_adc_ctrl_pin}, {battery_adc_multiplier}f, {battery_adc_atten});",
        ]
    lines += [
        "}",
        "",
        "// Flash and SD module directories for this device.",
        'const char *purr_flash_module_dir  = "/flash/modules";',
        'const char *purr_flash_driver_dir  = "/flash/drivers";',
        'const char *purr_flash_app_dir     = "/flash/apps";',
        'const char *purr_sd_module_dir     = "/sdcard/modules";',
        'const char *purr_sd_driver_dir     = "/sdcard/drivers";',
        'const char *purr_sd_app_dir        = "/sdcard/apps";',
    ]

    with open(glue_path, "w") as f:
        f.write("\n".join(lines) + "\n")

    info(f"  glue layer → {os.path.relpath(glue_path, REPO_DIR)}")
    return glue_path

# ── sdkconfig generation from device.pcat ─────────────────────────────────────
#
# CoreOS/sdkconfig_<device> used to be a hand-maintained file; almost every
# line in it (flash size, PSRAM, UI backend) is already declared in
# device.pcat under a different vocabulary. This generates it instead, chained
# with an optional CoreOS/sdkconfig_<device>.overrides for the handful of
# genuine hardware quirks (panel mirroring, WinCE shell flag, ...) that have
# no equivalent pcat field.

# device.pcat's [modules] ui value -> the exact PURR_UI_BACKEND_* Kconfig
# choice member (CoreOS/main/Kconfig.projbuild). NOT a blind .upper() —
# e.g. "oled_ui" maps to OLED, not OLED_UI (heltec would silently fall back
# to the Kconfig default backend if this were ever "simplified" to .upper()).
UI_BACKEND_MAP = {
    "kittenui":  "KITTENUI",
    "miniwin":   "MINIWIN",
    "oled_ui":   "OLED",
    "blackpurr": "BLACKPURR",
    "cardstack": "CARDSTACK",
    "cupcake":   "CUPCAKE",
    "lvgldebug": "LVGLDEBUG",
    "pounce":    "POUNCE",
    "nougat":    "NOUGAT",
}

def _pcat_bool(cfg, key):
    return cfg.get(key, "false").strip().lower() in ("true", "1", "yes")

def _sdkconfig_lines(device, cfg):
    """Build the CONFIG_* lines for CoreOS/sdkconfig_<device> from a parsed
    device.pcat. Pure — no I/O — so --check can diff without writing."""
    lines = [
        f"# AUTO-GENERATED by purrstrap from source/devices/{device}/device.pcat — DO NOT EDIT BY HAND",
        f"# Regenerate with: python3 purrstrap/purrstrap.py generate {device}",
    ]

    flash_mb_raw = cfg.get("device.flash_mb", "")
    try:
        flash_mb = int(flash_mb_raw)
    except ValueError:
        die(f"{device}: device.pcat [device] flash_mb is missing or not an integer")
    if flash_mb not in (4, 8, 16):
        die(f"{device}: flash_mb={flash_mb} has no matching partitions_{flash_mb}mb.csv "
            f"(only 4/8/16 MB partition tables exist)")
    partitions_csv = os.path.join(REPO_DIR, "CoreOS", f"partitions_{flash_mb}mb.csv")
    if not os.path.isfile(partitions_csv):
        die(f"{device}: expected CoreOS/partitions_{flash_mb}mb.csv does not exist")

    lines.append("")
    lines.append(f"CONFIG_ESPTOOLPY_FLASHSIZE_{flash_mb}MB=y")
    lines.append(f'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_{flash_mb}mb.csv"')

    if _pcat_bool(cfg, "device.psram"):
        psram_mb_raw = cfg.get("device.psram_mb", "")
        try:
            psram_bytes = int(psram_mb_raw) * 1048576
        except ValueError:
            die(f"{device}: device.pcat has psram=true but psram_mb is missing or not an integer")
        lines.append("")
        if cfg.get("device.chip", "") == "esp32p4":
            # The P4's PSRAM is hex-mode with its own Kconfig symbol set (and
            # CONFIG_IDF_EXPERIMENTAL_FEATURES for 200 MHz) — the S3-shaped
            # OCT/80M symbols below don't exist there. Mode/speed live in the
            # device's hand-written sdkconfig_<device>.overrides instead.
            lines.append("CONFIG_SPIRAM=y")
            lines.append("CONFIG_SPIRAM_USE_MALLOC=y")
        else:
            lines.append("CONFIG_SPIRAM=y")
            lines.append("CONFIG_SPIRAM_MODE_OCT=y")
            lines.append("CONFIG_SPIRAM_SPEED_80M=y")
            lines.append(f"CONFIG_SPIRAM_SIZE={psram_bytes}")
            lines.append("CONFIG_SPIRAM_USE_MALLOC=y")

    if cfg.get("device.kernel_type", "native") == "arduino":
        # CONFIG_FREERTOS_HZ=1000 is deliberately NOT re-emitted here — it's
        # already project-wide in CoreOS/sdkconfig.defaults.
        lines.append("")
        lines.append("CONFIG_ARDUINO_RUNNING_CORE=1")
        lines.append("CONFIG_ARDUINO_LOOP_STACK_SIZE=8192")
        lines.append("CONFIG_ARDUINO_EVENT_RUNNING_CORE=1")
        lines.append("CONFIG_ARDUINO_SERIAL_EVENT_TASK_RUNNING_CORE=1")

    ui = cfg.get("modules.ui", "")
    if ui:
        mapped = UI_BACKEND_MAP.get(ui)
        if not mapped:
            die(f"{device}: device.pcat [modules] ui = \"{ui}\" has no entry in UI_BACKEND_MAP "
                f"(valid: {', '.join(sorted(UI_BACKEND_MAP))})")
        lines.append("")
        lines.append("# PURR OS UI Backend")
        lines.append(f"CONFIG_PURR_UI_BACKEND_{mapped}=y")

    # [modules] bt/mesh presence -> the Kconfig gates that actually compile
    # bt_mgr.c/meshtastic's mesh_router.c+mesh_radio.c in. Mirrors the ui
    # mapping above; previously these were only ever hand-set in a device's
    # .overrides file even though [modules] bt/mesh already existed in the
    # schema — device.pcat is now the single source of truth for both.
    if cfg.get("modules.bt", ""):
        lines.append("")
        lines.append("# Bluetooth (NimBLE)")
        lines.append("CONFIG_BT_ENABLED=y")
        lines.append("CONFIG_BT_NIMBLE_ENABLED=y")

    if cfg.get("modules.mesh", ""):
        lines.append("")
        lines.append("# Meshtastic")
        lines.append("CONFIG_PURR_FEATURE_MESHTASTIC=y")

    if cfg.get("modules.meshcore", ""):
        lines.append("")
        lines.append("# MeshCore")
        lines.append("CONFIG_PURR_FEATURE_MESHCORE=y")

    return lines

def _generate_sdkconfig(device, cfg, check=False):
    """Write CoreOS/sdkconfig_<device> from device.pcat. In check mode, diff
    in-memory output against the committed file and report drift without
    writing. Returns True if it matches/was written cleanly, False on drift
    (check mode only)."""
    out_path = os.path.join(REPO_DIR, "CoreOS", f"sdkconfig_{device}")
    content = "\n".join(_sdkconfig_lines(device, cfg)) + "\n"

    if check:
        existing = ""
        if os.path.isfile(out_path):
            with open(out_path, encoding="utf-8") as f:
                existing = f.read()
        if existing == content:
            info(f"{device}: OK — sdkconfig matches device.pcat")
            return True
        warn(f"{device}: DRIFT — sdkconfig_{device} does not match device.pcat")
        sys.stdout.writelines(difflib.unified_diff(
            existing.splitlines(keepends=True),
            content.splitlines(keepends=True),
            fromfile=f"sdkconfig_{device} (committed)",
            tofile=f"sdkconfig_{device} (generated)",
        ))
        return False

    with open(out_path, "w", encoding="utf-8") as f:
        f.write(content)
    info(f"  sdkconfig      → {os.path.relpath(out_path, REPO_DIR)}")
    return True

def cmd_generate(args):
    targets = [args.device] if args.device else [slug for slug, _, _ in list_devices()]
    if not targets:
        die("no devices found in source/devices/")

    drift = 0
    for device in targets:
        cfg, _ = resolve_device(device)
        ok = _generate_sdkconfig(device, cfg, check=args.check)
        if args.check and not ok:
            drift += 1

    if args.check:
        if drift:
            die(f"{drift} of {len(targets)} device(s) have sdkconfig drift — "
                f"run 'purrstrap generate' (no --check) to fix")
        info(f"all {len(targets)} device(s) match — no drift")

# ── IDF kernel spine build ────────────────────────────────────────────────────

def _build_kernel_spine(device, cfg, out_dir):
    """
    Run idf.py build on the CoreOS kernel spine for the target device.
    Returns path to firmware.bin on success, None on failure or unavailability.
    """
    idf = _idf_path()
    if not idf:
        warn("IDF_PATH not set — skipping kernel spine build")
        return None

    coreos_dir = os.path.join(REPO_DIR, "CoreOS")
    if not os.path.isdir(coreos_dir):
        warn(f"CoreOS/ not found at {coreos_dir} — skipping IDF build")
        return None

    chip = cfg.get("device.chip", "esp32")
    valid_chips = ("esp32", "esp32s3", "esp32c3", "esp32s2", "esp32h2", "esp32c6", "esp32p4")
    if chip not in valid_chips:
        chip = "esp32"

    idf_py = os.path.join(idf, "tools", "idf.py")
    if not os.path.isfile(idf_py):
        idf_py = "idf.py"

    # Find the IDF Python venv so idf.py's dependencies (click, etc.) are available.
    _idf_python = _idf_venv_python() or sys.executable

    env = os.environ.copy()
    env["IDF_PATH"] = idf
    env["PURR_DEVICE"] = device
    _tools_path = _idf_tools_path(_idf_python)
    if _tools_path:
        env["IDF_TOOLS_PATH"] = _tools_path
    env["PURR_KERNEL_TYPE"] = cfg.get("device.kernel_type", "native")

    # idf.py's own __main__ guard does `if 'MSYSTEM' in os.environ: print_warning(...)`
    # as an if/elif/else chain — when MSYSTEM is set (Git Bash, MSYS2, any
    # MinGW-derived shell always sets it) it prints a warning and returns
    # WITHOUT calling main() at all. Every subprocess invocation silently
    # no-ops (exit 0, no build, no error) if this parent process inherited
    # MSYSTEM from a Git-Bash-launched purrstrap. Strip it so idf.py actually runs.
    env.pop("MSYSTEM", None)

    # Inject IDF toolchain paths (cmake, ninja, xtensa-esp-elf-gcc, ccache, ...)
    # into PATH. Resolved directly from IDF_TOOLS_PATH's on-disk layout — see
    # _idf_toolchain_bin_dirs()'s docstring for why this doesn't just shell out
    # to `idf_tools.py export` (unreliable on some installer layouts).
    _tool_bin_dirs = _idf_toolchain_bin_dirs(_tools_path)
    if _tool_bin_dirs:
        env["PATH"] = os.pathsep.join(_tool_bin_dirs) + os.pathsep + env.get("PATH", "")
        info(f"  toolchain PATH: {len(_tool_bin_dirs)} tool dir(s) resolved from {_tools_path}")
    else:
        warn(f"  no toolchain bin dirs found under IDF_TOOLS_PATH={_tools_path} — "
             f"cmake/ninja/gcc may be missing from PATH")

    # esp_rom_elfs generates gdbinit debug helpers at build time; not fatal if
    # missing (only affects `idf.py gdb`), but silences a spurious warning.
    _rom_elf_dirs = sorted(glob.glob(os.path.join(_tools_path or "", "esp-rom-elfs", "*"))) if _tools_path else []
    if _rom_elf_dirs:
        env["ESP_ROM_ELF_DIR"] = _rom_elf_dirs[-1]

    # Chain sdkconfig: base defaults + generated device config + optional
    # hand-maintained overrides for quirks device.pcat can't express
    # (IDF v5 supports semicolon-chaining multiple files).
    sdkconfig_base      = os.path.join(coreos_dir, "sdkconfig.defaults")
    sdkconfig_device    = os.path.join(coreos_dir, f"sdkconfig_{device}")
    sdkconfig_overrides = os.path.join(coreos_dir, f"sdkconfig_{device}.overrides")
    chain = [p for p in (sdkconfig_base, sdkconfig_device, sdkconfig_overrides) if os.path.isfile(p)]
    if chain:
        env["SDKCONFIG_DEFAULTS"] = ";".join(chain)

    build_dir = os.path.join(coreos_dir, f"build_{device}")

    # Every device build passes the same -C coreos_dir, so IDF's default
    # merged sdkconfig output (${PROJECT_DIR}/sdkconfig, i.e. CoreOS/sdkconfig)
    # is a SINGLE FILE SHARED BY EVERY DEVICE. SDKCONFIG_DEFAULTS only seeds
    # it when it doesn't already exist — once any device's build creates it,
    # every other device's incremental build silently reuses that stale,
    # wrong-device Kconfig state instead of its own generated sdkconfig_<device>.
    # Redirect it per-device so each build_<device>/ owns its own sdkconfig.
    # NOTE: CMakeLists.txt's project.cmake checks the CMake variable SDKCONFIG
    # (not $ENV{SDKCONFIG} — unlike SDKCONFIG_DEFAULTS, which IS read from the
    # environment), so this must be passed as a -D cache define to idf.py, not
    # as an env var.
    os.makedirs(build_dir, exist_ok=True)
    sdkconfig_path = os.path.join(build_dir, "sdkconfig")
    sdkconfig_define = f"SDKCONFIG={sdkconfig_path}"
    firmware_out = os.path.join(out_dir, "firmware.bin")
    bootloader_out = os.path.join(out_dir, "bootloader.bin")
    partitions_out = os.path.join(out_dir, "partition-table.bin")

    div("kernel spine")

    # Only run set-target when there's no existing build dir — it triggers fullclean.
    cmake_cache = os.path.join(build_dir, "CMakeCache.txt")
    if not os.path.isfile(cmake_cache):
        info(f"  idf.py set-target {chip}")
        rc = run_live([_idf_python, idf_py, "-D", sdkconfig_define,
                       "-C", coreos_dir, "-B", build_dir,
                       "set-target", chip], env=env)
        if rc != 0:
            warn(f"  set-target failed (rc={rc})")
            return None
    else:
        info(f"  set-target skipped (build dir exists)")

    info(f"  idf.py build")
    rc = run_live([_idf_python, idf_py, "-D", sdkconfig_define,
                   "-C", coreos_dir, "-B", build_dir,
                   "build"], env=env)
    if rc != 0:
        warn(f"  idf.py build failed (rc={rc})")
        return None

    # Copy binaries to out_dir
    import shutil as _sh
    for src_name, dst in (
        (f"purr_os.bin",        firmware_out),
        ("bootloader/bootloader.bin", bootloader_out),
        ("partition_table/partition-table.bin", partitions_out),
    ):
        src = os.path.join(build_dir, src_name)
        if os.path.isfile(src):
            _sh.copy2(src, dst)
            info(f"  {C_GRN}OK{C_RST}  {os.path.basename(dst)}")
        else:
            warn(f"  not found: {src_name}")

    return firmware_out if os.path.isfile(firmware_out) else None

def _bootloader_offset(chip):
    # Second-stage bootloader offset differs per chip: 0x1000 on the original
    # ESP32, 0x2000 on the ESP32-P4, 0x0 on every other target shipped so far.
    if chip == "esp32p4":
        return "0x2000"
    if chip in ("esp32s3", "esp32s2", "esp32c3", "esp32c6", "esp32h2"):
        return "0x0"
    return "0x1000"

def _merge_flash_image(device, cfg, out_dir, firmware_bin, flash_bin):
    """
    Use esptool merge_bin to combine firmware + SPIFFS into one flashable image.
    Output: out_dir/PURR_OS_<device>.bin
    """
    spiffs_offset = cfg.get("device.spiffs_offset", "0x290000")
    merged_out    = os.path.join(out_dir, f"PURR_OS_{device}.bin")
    bootloader    = os.path.join(out_dir, "bootloader.bin")
    partitions    = os.path.join(out_dir, "partition-table.bin")

    chip = cfg.get("device.chip", "esp32")
    bl_offset = _bootloader_offset(chip)

    parts = []
    if os.path.isfile(bootloader):   parts += [bl_offset,        bootloader]
    if os.path.isfile(partitions):   parts += ["0x8000",         partitions]
    parts += ["0x10000", firmware_bin]
    parts += [spiffs_offset, flash_bin]

    # Find esptool: prefer IDF venv python -m esptool, fall back to esptool.py in PATH
    _venv_py = _idf_venv_python()
    _esptool_runner = [_venv_py, "-m", "esptool"] if _venv_py else ["esptool.py"]

    cmd = _esptool_runner + ["--chip", cfg.get("device.chip", "esp32"),
           "merge_bin", "-o", merged_out] + parts

    div("merge")
    info(f"  merging final image → {os.path.relpath(merged_out, REPO_DIR)}")
    rc = run_live(cmd)
    if rc == 0:
        size_kb = os.path.getsize(merged_out) // 1024
        info(f"  {C_GRN}PURR_OS_{device}.bin{C_RST}  {size_kb} KB")
    else:
        warn(f"  merge_bin failed (rc={rc})")
    return merged_out if os.path.isfile(merged_out) else None

# ── Build ─────────────────────────────────────────────────────────────────────

def _regenerate_components_manifest():
    """
    Regenerate cattobaked/components_manifest.cmake from whatever module/app
    directories currently exist on disk, before the IDF build reads it.

    Root cause of a real bug: components_manifest.cmake is gitignored (it's
    "auto-generated by modulestrap" per its own header) and ONLY ever gets
    regenerated by modulestrap.py's own cmd_build() — which nothing here
    called. A brand-new source/modules/<name>/ or source/apps/system/<name>/
    directory would sit there correctly discoverable, but never actually
    make it into EXTRA_COMPONENT_DIRS until someone thought to separately
    run `python modulestrap/modulestrap.py build all` (undocumented as part
    of the normal `purrstrap build <device>` flow) — surfaced repeatedly
    while adding new modules this session as an "undefined reference to
    purr_module_<name>" linker error, and independently reported by a user
    hitting the same thing. Cheap to call on every build (it's just a
    directory scan + one file write), so no reason to make this opt-in.
    """
    modulestrap_dir = os.path.join(REPO_DIR, "modulestrap")
    if modulestrap_dir not in sys.path:
        sys.path.insert(0, modulestrap_dir)
    import modulestrap
    targets = modulestrap.find_modules()
    modulestrap.generate_components_manifest(targets)

def cmd_build(args):
    device = args.device
    cfg, pcat_path = resolve_device(device)

    chip    = cfg.get("device.chip", "esp32")
    name    = cfg.get("device.name", device)
    out_dir = os.path.join(OUTPUT_DIR, device)

    div()
    info(f"building {name} ({chip})")
    info(f"output → cattobaked/{device}/")
    div()

    os.makedirs(out_dir, exist_ok=True)

    # Save workspace config
    workspace = {
        "device":         device,
        "chip":           chip,
        "purros_version": PURROS_VERSION,
        "kitt_version":   KITT_VERSION,
        "last_build":     datetime.datetime.now().isoformat(),
    }
    with open(CFG_FILE, "w") as f:
        json.dump(workspace, f, indent=2)

    # ── Generate device glue layer ─────────────────────────────────────────────
    _generate_glue(device, cfg, out_dir)
    _generate_sdkconfig(device, cfg)

    # Remove stale merged image so a failed build never leaves a flashable artifact
    stale = os.path.join(out_dir, f"PURR_OS_{device}.bin")
    if os.path.isfile(stale):
        os.remove(stale)

    # ── SPIFFS flash image (modules + drivers baked in) ────────────────────────
    # Determine SPIFFS partition size from device.pcat (default 512 KB)
    spiffs_kb = int(cfg.get("device.spiffs_kb", "512"))
    flash_bin = build_flash_image(device, cfg, out_dir, spiffs_size_kb=spiffs_kb)

    div()

    # Must run before _build_kernel_spine() — CoreOS/CMakeLists.txt includes
    # this file to populate EXTRA_COMPONENT_DIRS, so a stale manifest here
    # means a just-added module/app silently never gets built at all (see
    # _regenerate_components_manifest()'s own comment for the bug this fixes).
    _regenerate_components_manifest()

    # ── Kernel spine (IDF build) ───────────────────────────────────────────────
    firmware_bin = _build_kernel_spine(device, cfg, out_dir)

    # ── Merge final image ──────────────────────────────────────────────────────
    if firmware_bin and flash_bin:
        _merge_flash_image(device, cfg, out_dir, firmware_bin, flash_bin)
    elif not firmware_bin:
        warn("kernel spine not built — IDF unavailable or build failed")
        warn("SPIFFS flash.bin is ready; run IDF manually then merge with esptool merge_bin")

    # Write build metadata
    meta = {
        **workspace,
        "pcat":         pcat_path,
        "flash_bin":    flash_bin or "not built",
        "firmware_bin": firmware_bin or "not built",
        "spiffs_kb":    spiffs_kb,
    }
    with open(os.path.join(out_dir, "build.json"), "w") as f:
        json.dump(meta, f, indent=2)

    info(f"build.json written")
    div()

def cmd_flash(args):
    cmd_build(args)

    # Abort if the build didn't produce a merged image
    out_dir    = os.path.join(OUTPUT_DIR, args.device)
    merged_bin = os.path.join(out_dir, f"PURR_OS_{args.device}.bin")
    if not os.path.isfile(merged_bin):
        warn("build did not produce a flashable image — aborting flash")
        return

    cfg, _ = resolve_device(args.device)
    port = getattr(args, "port", None) or cfg.get("device.port", "auto")
    flash_bin = os.path.join(out_dir, "flash.bin")
    div()
    info(f"flash target: {args.device}  port: {port}")

    # Determine flash offset for SPIFFS partition (device-specific, from partition table)
    # Default offset matches the common 4MB layout: 0x290000
    spiffs_offset = cfg.get("device.spiffs_offset", "0x290000")

    idf = _idf_path()
    if not idf:
        warn("IDF_PATH not set — cannot flash")
        return

    esptool_port = port if port != "auto" else "/dev/ttyUSB0"

    # Find esptool: prefer IDF venv python -m esptool
    _venv_py = _idf_venv_python()
    _esptool = [_venv_py, "-m", "esptool"] if _venv_py else ["esptool.py"]

    # Flash bootloader/partition-table/firmware/SPIFFS as separate parts of
    # one write_flash call, each at its own real offset — NOT the merged
    # single-file image. merge_bin fills any gap between listed parts with
    # 0xFF (its default), and the NVS partition (WiFi calibration, and now
    # this project's own persisted mesh node/channel tables) sits exactly in
    # the gap between partition-table (0x8000) and firmware (0x10000) — so
    # writing that merged image at 0x0 silently erases NVS on every single
    # dev flash, even without --erase-all. Confirmed live: a freshly-learned
    # Meshtastic node's public key vanished after an otherwise-unrelated
    # reflash, and separately "falling back to full calibration" showing up
    # in the boot log after routine flashes. A multi-offset write_flash (the
    # same shape as the "or from ..." alternative command already printed
    # after every build) only ever touches the exact byte ranges of the
    # files given — it never touches the untouched gap in between, so NVS
    # survives a normal flash. PURR_OS_<device>.bin (built by
    # _merge_flash_image above) is still produced and used as-is by `bake`
    # for shipping to a brand-new device, where there's no prior NVS content
    # to preserve anyway.
    bootloader_bin = os.path.join(out_dir, "bootloader.bin")
    partition_bin  = os.path.join(out_dir, "partition-table.bin")
    firmware_bin   = os.path.join(out_dir, "firmware.bin")
    if all(os.path.isfile(p) for p in (bootloader_bin, partition_bin, firmware_bin, flash_bin)):
        cfg_flash, _ = resolve_device(args.device)
        chip     = cfg_flash.get("device.chip", "esp32s3")
        flash_mb = cfg_flash.get("device.flash_mb", "4")
        bl_offset = _bootloader_offset(chip)
        info(f"flashing bootloader+partition-table+firmware+SPIFFS (NVS untouched) ...")
        erase_flag = ["--erase-all"] if getattr(args, "erase", False) else []
        cmd = _esptool + [
            "--chip", chip,
            "--port", esptool_port,
            "--baud", "460800",
            "--before", "default_reset",
            "--after", "hard_reset",
            "write_flash",
        ] + erase_flag + [
            "--flash_mode", "dio",
            "--flash_size", f"{flash_mb}MB",
            "--flash_freq", "80m",
            bl_offset, bootloader_bin,
            "0x8000", partition_bin,
            "0x10000", firmware_bin,
            spiffs_offset, flash_bin,
        ]
        run_live(cmd)
    elif os.path.isfile(flash_bin):
        # Fallback: SPIFFS only (firmware must have been flashed separately)
        cfg_flash, _ = resolve_device(args.device)
        spiffs_offset = cfg_flash.get("device.spiffs_offset", "0x290000")
        info(f"flashing SPIFFS only to {spiffs_offset} (no merged image found)...")
        cmd = _esptool + [
            "--port", esptool_port,
            "--baud", "460800",
            "write_flash",
            spiffs_offset, flash_bin,
        ]
        run_live(cmd)
    else:
        warn("no flashable image found — run purrstrap build first")

def cmd_monitor(args):
    device = args.device
    cfg_mon, _ = resolve_device(device)
    port = getattr(args, "port", None) or cfg_mon.get("device.port", "auto")
    baud = getattr(args, "baud", None) or "115200"

    idf = _idf_path()
    if not idf:
        warn("IDF_PATH not set — cannot run monitor")
        return

    build_dir = os.path.join(REPO_DIR, "CoreOS", f"build_{device}")
    if not os.path.isdir(build_dir):
        warn(f"no build dir for {device} — run purrstrap build {device} first")
        return

    monitor_py = os.path.join(idf, "tools", "idf_monitor.py")
    elf        = os.path.join(build_dir, "purr_os.elf")

    if not os.path.isfile(elf):
        warn(f"purr_os.elf not found in {build_dir}")
        return

    div()
    info(f"monitor: {device}  port: {port}  baud: {baud}")
    info("press Ctrl+] to exit")
    div()

    _idf_python = _idf_venv_python() or sys.executable
    _mon_env = os.environ.copy()
    _mon_env.pop("MSYSTEM", None)
    cmd = [
        _idf_python, monitor_py,
        "--port", port if port != "auto" else "/dev/ttyUSB0",
        "--baud", baud,
        elf,
    ]
    try:
        subprocess.run(cmd, cwd=build_dir, env=_mon_env)
    except KeyboardInterrupt:
        pass

def cmd_clean(args):
    device = args.device
    build_dir = os.path.join(REPO_DIR, "source", f"build_{device}")
    out_dir   = os.path.join(OUTPUT_DIR, device)
    for d in [build_dir, out_dir]:
        if os.path.isdir(d):
            shutil.rmtree(d)
            info(f"removed {d}")
        else:
            warn(f"nothing to clean: {d}")

# ── Bake ──────────────────────────────────────────────────────────────────────

RELEASES_DIR = os.path.join(REPO_DIR, "releases")

def cmd_bake(args):
    """
    Build all devices and copy outputs to releases/v<version>/.
    Creates a manifest.json with per-device build results.
    """
    devices = list_devices()
    if not devices:
        die("no devices found in source/devices/")

    release_dir = os.path.join(RELEASES_DIR, f"v{PURROS_VERSION}")
    os.makedirs(release_dir, exist_ok=True)

    div()
    info(f"baking PURR OS v{PURROS_VERSION} — {len(devices)} device(s)")
    info(f"output → releases/v{PURROS_VERSION}/")
    div()

    manifest = {
        "purros_version": PURROS_VERSION,
        "kitt_version":   KITT_VERSION,
        "baked_at":       datetime.datetime.now().isoformat(),
        "devices":        {},
    }

    ok_count = 0
    for slug, name, chip in devices:
        div(slug)

        # Reuse cmd_build logic
        class _Args: device = slug
        try:
            cmd_build(_Args())
        except SystemExit as e:
            manifest["devices"][slug] = {"status": "error", "error": str(e)}
            continue

        out_dir  = os.path.join(OUTPUT_DIR, slug)
        dest_dir = os.path.join(release_dir, slug)
        os.makedirs(dest_dir, exist_ok=True)

        # Copy artifacts
        copied = []
        for fname in ("flash.bin", "build.json", "firmware.bin"):
            src = os.path.join(out_dir, fname)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(dest_dir, fname))
                copied.append(fname)

        # Copy spiffs_staging manifest if present
        staging_meta = os.path.join(out_dir, "spiffs_staging")
        if os.path.isdir(staging_meta):
            # Just record what was staged, don't copy the whole tree
            staged = []
            for root, _, files in os.walk(staging_meta):
                for f in files:
                    if f.endswith(".purr"):
                        rel = os.path.relpath(os.path.join(root, f), staging_meta)
                        staged.append(rel)
            manifest["devices"][slug] = {
                "status":  "ok",
                "chip":    chip,
                "name":    name,
                "copied":  copied,
                "staged":  staged,
            }
        else:
            manifest["devices"][slug] = {
                "status": "partial",
                "chip":   chip,
                "name":   name,
                "copied": copied,
            }

        status = f"{C_GRN}OK{C_RST}" if "flash.bin" in copied else f"{C_YLW}partial{C_RST}"
        info(f"  {name} ({chip})  [{status}]  files: {', '.join(copied) or 'none'}")
        ok_count += 1

    # Write manifest
    manifest_path = os.path.join(release_dir, "manifest.json")
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    div()
    info(f"bake complete: {ok_count}/{len(devices)} devices")
    info(f"manifest: releases/v{PURROS_VERSION}/manifest.json")
    div()

    # --dp: additionally package a full CatReleases/DP<N>/-style developer
    # preview archive — opt-in, not the default bake output. releases/
    # above stays exactly what it always was (a lightweight per-version
    # record: flash.bin/build.json/firmware.bin, no README, no zip); this
    # is the separate, heavier "hand this to someone to actually flash"
    # package that used to only get built manually. Meant to be invokable
    # from a GitHub Action on a release tag, not part of every routine
    # local bake.
    if getattr(args, "dp", False):
        _bake_dp_release(devices, manifest)

def _bake_dp_release(devices, manifest):
    """
    Package a full developer-preview release into CatReleases/DP<N>/,
    matching the existing CatReleases/DP1..DP3 shape exactly: every
    device's complete flashable file set (split images + merged image),
    a manifest.json, a README.md with flashing instructions, and a
    top-level DP<N>.zip. N comes from PURROS_VERSION's own "-dpN" suffix,
    so the two numbering schemes can't drift apart.
    """
    m = re.search(r"-dp(\d+)", PURROS_VERSION)
    if not m:
        warn(f"PURROS_VERSION '{PURROS_VERSION}' has no -dpN suffix — skipping --dp package")
        return
    dp_name = f"DP{m.group(1)}"
    dp_dir  = os.path.join(REPO_DIR, "CatReleases", dp_name)
    os.makedirs(dp_dir, exist_ok=True)

    div(dp_name)
    info(f"packaging developer preview → CatReleases/{dp_name}/")

    for slug, name, chip in devices:
        dev = manifest["devices"].get(slug, {})
        if dev.get("status") not in ("ok", "partial"):
            continue
        out_dir  = os.path.join(OUTPUT_DIR, slug)
        dest_dir = os.path.join(dp_dir, slug)
        os.makedirs(dest_dir, exist_ok=True)
        for fname in ("bootloader.bin", "partition-table.bin", "firmware.bin",
                      "flash.bin", f"PURR_OS_{slug}.bin", "build.json"):
            src = os.path.join(out_dir, fname)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(dest_dir, fname))

    dp_manifest_path = os.path.join(dp_dir, "manifest.json")
    with open(dp_manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    readme_lines = [
        f"# PURR OS — {dp_name} (Developer Preview {m.group(1)})",
        "",
        f"Full-stack build of all {len(devices)} supported devices. Every device folder",
        "has both split images and one pre-merged image, plus a `manifest.json`",
        "recording chip/name/copied-files for all of them in one place.",
        "",
        "## Flashing the merged image (recommended)",
        "",
        "```bash",
        "esptool.py -p <PORT> write_flash 0x0 PURR_OS_<device>.bin",
        "```",
        "",
        "## Flashing split images",
        "",
        "```bash",
        "esptool.py -p <PORT> write_flash \\",
        "  0x0     bootloader.bin \\",
        "  0x8000  partition-table.bin \\",
        "  0x10000 firmware.bin \\",
        "  <spiffs_offset from device.pcat>  flash.bin",
        "```",
        "",
        "## Devices in this release",
        "",
        "| Device | Chip | Status |",
        "|---|---|---|",
    ]
    for slug, name, chip in devices:
        dev_status = manifest["devices"].get(slug, {}).get("status", "error")
        readme_lines.append(f"| {name} | {chip} | {dev_status} |")
    with open(os.path.join(dp_dir, "README.md"), "w") as f:
        f.write("\n".join(readme_lines) + "\n")

    zip_base = os.path.join(REPO_DIR, "CatReleases", dp_name)
    shutil.make_archive(zip_base, "zip", os.path.join(REPO_DIR, "CatReleases"), dp_name)

    div()
    info(f"{dp_name} package complete")
    info(f"readme:  CatReleases/{dp_name}/README.md")
    info(f"archive: CatReleases/{dp_name}.zip")
    div()

# ── Local package manager (v1, no network) ───────────────────────────────────
#
# Two genuinely different mechanisms exist in PURR OS today, and this CLI
# keeps them as two separate command groups instead of papering over the
# difference with one model:
#
#   "static" packages — drivers, system/UI modules, and apps selected via
#   device.pcat's [drivers]/[modules]/[apps] sections — are compiled directly
#   into the firmware binary via PURR_MODULE_REGISTER() (see _generate_glue
#   above). There is no installable file for these: "installing" one means
#   editing device.pcat and rebuilding. `purrstrap pkg <list|add|remove|
#   upgrade|verify>` operates on this model.
#
#   "app" packages — `.meow` and `.hiss` Lua scripts today — are real files
#   that app_manager's boot-time scan picks up from /flash/apps or
#   /sdcard/apps without any firmware rebuild. `.claw`/`.paws` apps are NOT
#   real files; like drivers/modules they are statically linked, so
#   `purrstrap pkg app` refuses to "install" one and points at the static
#   path instead.
#   `purrstrap pkg app <list|install|remove|upgrade>` operates on this model.

APPS_BASES = ("system", "exclusive", "user")

def _depends_of(cfg):
    """Extract a {package_name: constraint} dict from a parsed .pcat's
    [depends] section, if present. Empty dict if the section is absent."""
    return {k[len("depends."):]: v for k, v in cfg.items() if k.startswith("depends.")}

def _find_package(name):
    """
    Locate a PURR package by name across modules/, drivers/*/, apps/*/.
    Returns (kind, pcat_path, slug, cfg) or (None, None, None, None).
    kind is one of "module", "driver", "app". slug is the device.pcat-style
    identifier ("name" for modules/apps, "type/name" for drivers).
    """
    # Driver given as "type/name" explicitly
    if "/" in name and not name.startswith("apps/"):
        p = os.path.join(SOURCE_DIR, "drivers", name, "driver.pcat")
        if os.path.isfile(p):
            return "driver", p, name, parse_pcat(p)

    # Module: source/modules/<name>/module.pcat
    p = os.path.join(SOURCE_DIR, "modules", name, "module.pcat")
    if os.path.isfile(p):
        return "module", p, name, parse_pcat(p)

    # Driver by bare name: search every type/ subdir
    drivers_dir = os.path.join(SOURCE_DIR, "drivers")
    if os.path.isdir(drivers_dir):
        for dtype in sorted(os.listdir(drivers_dir)):
            p = os.path.join(drivers_dir, dtype, name, "driver.pcat")
            if os.path.isfile(p):
                return "driver", p, f"{dtype}/{name}", parse_pcat(p)

    # App: source/apps/{system,exclusive,user}/<name>/app.pcat
    for base in APPS_BASES:
        p = os.path.join(SOURCE_DIR, "apps", base, name, "app.pcat")
        if os.path.isfile(p):
            return "app", p, name, parse_pcat(p)

    return None, None, None, None

def _resolve_static_deps(name, seen=None):
    """
    Recursively walk [depends] for a static package. Returns a flat list of
    (dep_name, constraint, found) tuples for every dependency in the closure
    (excluding `name` itself). `found` is False if no package with that name
    exists anywhere in source/ at all — `pkg add` refuses in that case.
    Does NOT check version constraints against what's currently selected for
    a device — that's a separate, cheaper check the caller does against the
    device's own device.pcat + installed.json.
    """
    seen = seen if seen is not None else set()
    out = []
    if name in seen:
        return out
    seen.add(name)
    kind, pcat, slug, cfg = _find_package(name)
    if not cfg:
        return out
    for dep_name, constraint in _depends_of(cfg).items():
        dk, dp, dslug, dcfg = _find_package(dep_name)
        out.append((dep_name, constraint, dcfg is not None))
        if dcfg is not None:
            out.extend(_resolve_static_deps(dep_name, seen))
    return out

# ── device.pcat in-place editing (preserves comments/formatting elsewhere) ───

def _pcat_lines_set(lines, section, key, value):
    """Return a new line list with `key = "value"` set inside [section],
    creating the section and/or key if either is missing."""
    header = f"[{section}]"
    out = []
    i = 0
    section_found = False
    key_written = False
    while i < len(lines):
        line = lines[i]
        if line.strip() == header:
            section_found = True
            out.append(line)
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("["):
                inner = lines[i]
                stripped = inner.strip()
                if stripped and not stripped.startswith("#") and "=" in stripped:
                    k = stripped.split("=", 1)[0].strip()
                    if k == key:
                        out.append(f'{key:<12} = "{value}"\n')
                        key_written = True
                        i += 1
                        continue
                out.append(inner)
                i += 1
            if not key_written:
                out.append(f'{key:<12} = "{value}"\n')
                key_written = True
            continue
        out.append(line)
        i += 1
    if not section_found:
        if out and out[-1].strip() != "":
            out.append("\n")
        out.append(f"{header}\n")
        out.append(f'{key:<12} = "{value}"\n')
    return out

def _pcat_lines_remove(lines, section, key):
    """Return a new line list with `key`'s line removed from [section], if
    present. No-op if the section or key doesn't exist."""
    header = f"[{section}]"
    out = []
    i = 0
    in_section = False
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        if stripped == header:
            in_section = True
            out.append(line)
            i += 1
            continue
        if in_section and stripped.startswith("["):
            in_section = False
        if in_section and stripped and not stripped.startswith("#") and "=" in stripped:
            k = stripped.split("=", 1)[0].strip()
            if k == key:
                i += 1
                continue
        out.append(line)
        i += 1
    return out

def _pcat_set(pcat_path, section, key, value):
    with open(pcat_path) as f:
        lines = f.readlines()
    lines = _pcat_lines_set(lines, section, key, value)
    with open(pcat_path, "w") as f:
        f.writelines(lines)

def _pcat_remove(pcat_path, section, key):
    with open(pcat_path) as f:
        lines = f.readlines()
    lines = _pcat_lines_remove(lines, section, key)
    with open(pcat_path, "w") as f:
        f.writelines(lines)

# ── installed.json — per-device record of static selections + app files ─────

INSTALLED_SCHEMA = 1

def _pkg_version(cfg):
    """module.pcat/driver.pcat/app.pcat are inconsistent about using a
    [section] header — driver_manager/app_manager/hwtest are flat ("version"
    at top level) while blackpurr/cardstack use [module] ("module.version").
    Check both rather than assuming one convention."""
    return cfg.get("version") or cfg.get("module.version") or "0.0.0"

def _installed_path(device):
    return os.path.join(OUTPUT_DIR, device, "installed.json")

def _load_installed(device):
    path = _installed_path(device)
    if os.path.isfile(path):
        with open(path) as f:
            try:
                return json.load(f)
            except json.JSONDecodeError:
                warn(f"{path} is corrupt — starting a fresh record")
    return {"schema": INSTALLED_SCHEMA, "device": device, "static": [], "apps": []}

def _save_installed(device, data):
    path = _installed_path(device)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(data, f, indent=2)

# ── pkg: static package selection (drivers / modules / apps-as-static) ──────

# Maps a static package's kind+type to the device.pcat [section] + key it's
# selected under. Drivers are single-slot per category under [drivers];
# everything under [modules] is a free-form key (purrstrap's glue generator
# includes any modules.* value regardless of key name — see _generate_glue).
def _static_pcat_target(kind, cfg, name):
    if kind == "driver":
        dtype = cfg.get("type", "")
        if not dtype:
            return None
        return ("drivers", dtype)
    if kind == "module":
        mtype = cfg.get("module.type") or cfg.get("type") or cfg.get("module_type", "")
        mtype = mtype.replace("PURR_MOD_", "").lower()
        if mtype == "ui":
            return ("modules", "ui")
        if name == "app_manager":
            return ("modules", "app_manager")
        # Generic system module with no named device.pcat slot — purrstrap's
        # glue generator only picks up [modules].* keys, so give it one
        # named after the module itself rather than refusing outright.
        return ("modules", name)
    if kind == "app":
        return ("apps", name)
    return None

def cmd_pkg_list(args):
    device = args.device
    cfg, pcat_path = resolve_device(device)
    installed = _load_installed(device)
    div(f"pkg list — {device}")
    print(f"{C_BOLD}{'slot':<22}{'package':<20}{'source':<10}{C_RST}")
    for key in ("display", "touch", "input", "radio", "gps"):
        val = cfg.get(f"drivers.{key}", "")
        if val:
            print(f"  {'drivers.' + key:<22}{val:<20}{'device.pcat'}")
    for raw_key, raw_val in sorted(cfg.items()):
        if raw_key.startswith("modules.") and raw_val:
            print(f"  {raw_key:<22}{raw_val:<20}{'device.pcat'}")
    for raw_key, raw_val in sorted(cfg.items()):
        if raw_key.startswith("apps.") and raw_val.lower() in ("true", "1", "yes"):
            name = raw_key.split(".", 1)[1]
            print(f"  {raw_key:<22}{name:<20}{'device.pcat'}")
    div()
    if installed.get("static"):
        print(f"{C_GRY}installed.json record (version pins):{C_RST}")
        for entry in installed["static"]:
            print(f"  {entry['name']:<20} v{entry.get('version', '?')}")
        div()

def cmd_pkg_add(args):
    device, name = args.device, args.name
    cfg, pcat_path = resolve_device(device)
    kind, src_pcat, slug, pkg_cfg = _find_package(name)
    if kind is None:
        die(f"no package named '{name}' found under source/modules, source/drivers, or source/apps")

    target = _static_pcat_target(kind, pkg_cfg, name)
    if target is None:
        die(f"'{name}' ({kind}) has no [type]/[module_type] set — can't determine its device.pcat slot")
    section, key = target

    # Dependency resolution — recursively, against what actually exists as a
    # buildable package. Does not silently overwrite an occupied driver slot;
    # for module deps with a free-form slot, adds them too.
    deps = _resolve_static_deps(name)
    hard_missing = [d for d, c, found in deps if not found]
    if hard_missing:
        die(f"'{name}' depends on package(s) not found anywhere: {', '.join(sorted(set(hard_missing)))}")

    for dep_name, constraint, found in deps:
        dk, dp, dslug, dcfg = _find_package(dep_name)
        dtarget = _static_pcat_target(dk, dcfg, dep_name)
        if dtarget is None:
            continue
        dsection, dkey = dtarget
        current = cfg.get(f"{dsection}.{dkey}", "")
        if current == dep_name:
            continue
        if current and dsection == "drivers":
            warn(f"  dependency '{dep_name}' ({constraint}) not auto-applied — "
                 f"[{dsection}] {dkey} is already '{current}'; resolve manually if needed")
            continue
        info(f"  resolving dependency: {dep_name} ({constraint}) → [{dsection}] {dkey}")
        _pcat_set(pcat_path, dsection, dkey, dep_name)
        cfg[f"{dsection}.{dkey}"] = dep_name

    current = cfg.get(f"{section}.{key}", "")
    if current == name:
        info(f"'{name}' is already selected for {device} ([{section}] {key} = \"{name}\")")
    else:
        if current and section == "drivers":
            info(f"replacing [{section}] {key} = \"{current}\" with \"{name}\"")
        _pcat_set(pcat_path, section, key, name)
        info(f"[{section}] {key} = \"{name}\" written to {os.path.relpath(pcat_path, REPO_DIR)}")

    if section == "apps":
        _pcat_set(pcat_path, "apps", name, "true")

    installed = _load_installed(device)
    installed["static"] = [e for e in installed["static"] if e["name"] != name]
    installed["static"].append({
        "name": name, "kind": kind, "version": _pkg_version(pkg_cfg),
        "slot": f"{section}.{key}",
        "depends": _depends_of(pkg_cfg),
    })
    _save_installed(device, installed)

    info(f"done — run 'purrstrap build {device}' to apply.")

def cmd_pkg_remove(args):
    device, name = args.device, args.name
    cfg, pcat_path = resolve_device(device)
    kind, src_pcat, slug, pkg_cfg = _find_package(name)
    if kind is None:
        die(f"no package named '{name}' found — nothing to remove from source")

    # Refuse if something else currently selected for this device depends on it
    if not args.force:
        installed = _load_installed(device)
        dependents = [e["name"] for e in installed.get("static", [])
                      if name in e.get("depends", {}) and e["name"] != name]
        if dependents:
            die(f"'{name}' is depended on by: {', '.join(dependents)} — use --force to remove anyway")

    target = _static_pcat_target(kind, pkg_cfg, name)
    if target is None:
        die(f"'{name}' ({kind}) has no known device.pcat slot")
    section, key = target
    current = cfg.get(f"{section}.{key}", "")
    if current != name:
        warn(f"'{name}' is not currently selected for {device} ([{section}] {key} = \"{current or '(unset)'}\")")
    else:
        _pcat_remove(pcat_path, section, key)
        info(f"removed [{section}] {key} from {os.path.relpath(pcat_path, REPO_DIR)}")
    if section == "apps":
        _pcat_remove(pcat_path, "apps", name)

    installed = _load_installed(device)
    installed["static"] = [e for e in installed["static"] if e["name"] != name]
    _save_installed(device, installed)
    info(f"done — run 'purrstrap build {device}' to apply.")

def cmd_pkg_upgrade(args):
    device = args.device
    names = [args.name] if args.name else [e["name"] for e in _load_installed(device).get("static", [])]
    if not names:
        info(f"nothing installed to upgrade for {device}")
        return
    for name in names:
        kind, pcat, slug, pkg_cfg = _find_package(name)
        if kind is None:
            warn(f"  {name}: no longer found in source — skipping (use 'pkg remove' to drop its record)")
            continue
        installed = _load_installed(device)
        entry = next((e for e in installed["static"] if e["name"] == name), None)
        new_version = _pkg_version(pkg_cfg)
        old_version = entry.get("version", "0.0.0") if entry else None
        if entry is None:
            info(f"  {name}: not tracked yet — run 'pkg add {device} {name}' first")
            continue
        if new_version == old_version:
            info(f"  {name}: already at v{new_version}")
            continue
        entry["version"] = new_version
        entry["depends"] = _depends_of(pkg_cfg)
        _save_installed(device, installed)
        info(f"  {name}: v{old_version} → v{new_version} (record updated — rebuild to apply)")

def cmd_pkg_verify(args):
    device = args.device
    cfg, pcat_path = resolve_device(device)
    installed = _load_installed(device)
    problems = 0
    for entry in installed.get("static", []):
        section, key = entry["slot"].split(".", 1)
        current = cfg.get(f"{section}.{key}", "")
        if current != entry["name"]:
            warn(f"  drift: installed.json says [{section}] {key} = \"{entry['name']}\", "
                 f"device.pcat actually has \"{current or '(unset)'}\"")
            problems += 1
        kind, pcat, slug, pkg_cfg = _find_package(entry["name"])
        if kind is None:
            warn(f"  drift: '{entry['name']}' is tracked as installed but no longer exists in source/")
            problems += 1
    if problems == 0:
        info(f"{device}: installed.json matches device.pcat and source/ — no drift")
    else:
        warn(f"{device}: {problems} issue(s) found")

# ── pkg app: runtime file hot-load (.meow/.hiss — see module docstring) ─────

def _find_meow_or_hiss(device, name):
    """Look for a .meow, .hiss, or .kitten script the user already has staged
    for this device's SPIFFS image, or under any apps/ source dir, by bare
    name. Returns (path, tier) — tier is "meow"/"hiss"/"kitten" matching
    whichever extension was actually found — or (None, None)."""
    for ext in ("meow", "hiss", "kitten"):
        candidates = [
            os.path.join(OUTPUT_DIR, device, "spiffs_staging", "apps", f"{name}.{ext}"),
        ]
        for base in APPS_BASES:
            candidates.append(os.path.join(SOURCE_DIR, "apps", base, name, f"{name}.{ext}"))
            candidates.append(os.path.join(SOURCE_DIR, "apps", base, f"{name}.{ext}"))
        for c in candidates:
            if os.path.isfile(c):
                return c, ext
    return None, None

def _app_target_dir(device, to):
    if to == "sd":
        die("--to sd requires a mounted SD card path — pass --sd-path /path/to/sdcard/apps")
    return os.path.join(OUTPUT_DIR, device, "spiffs_staging", "apps")

def cmd_pkg_app_list(args):
    device = args.device
    installed = _load_installed(device)
    div(f"pkg app list — {device}")
    if not installed.get("apps"):
        print(f"  {C_GRY}(none installed via pkg app){C_RST}")
    for entry in installed.get("apps", []):
        print(f"  {entry['name']:<20} v{entry.get('version','?')}  [{entry.get('tier','meow')}]  {entry.get('location','flash')}")
    div()

def cmd_pkg_app_install(args):
    device, name = args.device, args.name
    kind, pcat, slug, pkg_cfg = _find_package(name)
    if kind == "app" and pkg_cfg.get("tier") in ("claw", "paws"):
        die(f"'{name}' is a .{pkg_cfg.get('tier')} app — those are statically compiled into "
            f"firmware, not installable as a file. Use 'purrstrap pkg add {device} {name}' instead.")

    src, tier = _find_meow_or_hiss(device, name)
    if not src:
        die(f"no .meow or .hiss script found for '{name}' — only Lua (.meow/.hiss) apps "
            f"are real, installable files today; place one at "
            f"source/apps/user/{name}/{name}.meow (or .hiss) "
            f"or build it onto {device}'s staging area first")

    dst_dir = _app_target_dir(device, args.to)
    os.makedirs(dst_dir, exist_ok=True)
    dst = os.path.join(dst_dir, f"{name}.{tier}")
    if os.path.exists(dst) and os.path.samefile(src, dst):
        info(f"{name}.{tier} is already at {os.path.relpath(dst, REPO_DIR)} — registering as-is")
    else:
        shutil.copy2(src, dst)
        info(f"installed {name}.{tier} → {os.path.relpath(dst, REPO_DIR)}")

    installed = _load_installed(device)
    installed["apps"] = [e for e in installed.get("apps", []) if e["name"] != name]
    installed["apps"].append({
        "name": name, "tier": tier, "version": _pkg_version(pkg_cfg) if pkg_cfg else "0.0.0",
        "location": args.to, "path": os.path.relpath(dst, OUTPUT_DIR),
    })
    _save_installed(device, installed)
    info("staged and will be picked up by app_manager's boot-time scan — "
         "the Lua VM (lua_runtime module) runs it on launch.")

def cmd_pkg_app_remove(args):
    device, name = args.device, args.name
    installed = _load_installed(device)
    entry = next((e for e in installed.get("apps", []) if e["name"] == name), None)
    if not entry:
        die(f"'{name}' is not tracked as an installed app for {device}")
    full_path = os.path.join(OUTPUT_DIR, entry["path"])
    if os.path.isfile(full_path):
        os.remove(full_path)
        info(f"removed {entry['path']}")
    installed["apps"] = [e for e in installed["apps"] if e["name"] != name]
    _save_installed(device, installed)

def cmd_pkg_app_upgrade(args):
    device = args.device
    names = [args.name] if args.name else [e["name"] for e in _load_installed(device).get("apps", [])]
    for name in names:
        src, _tier = _find_meow_or_hiss(device, name)
        if not src:
            warn(f"  {name}: no source .meow/.hiss found — skipping")
            continue
        cmd_pkg_app_install(argparse.Namespace(device=device, name=name, to="flash"))

def cmd_pkg(args):
    dispatch = {
        "list": cmd_pkg_list, "add": cmd_pkg_add, "remove": cmd_pkg_remove,
        "upgrade": cmd_pkg_upgrade, "verify": cmd_pkg_verify,
    }
    if args.pkg_cmd == "app":
        app_dispatch = {
            "list": cmd_pkg_app_list, "install": cmd_pkg_app_install,
            "remove": cmd_pkg_app_remove, "upgrade": cmd_pkg_app_upgrade,
        }
        if args.app_cmd not in app_dispatch:
            die("usage: purrstrap pkg app <list|install|remove|upgrade> <device> [name]")
        return app_dispatch[args.app_cmd](args)
    if args.pkg_cmd not in dispatch:
        die("usage: purrstrap pkg <list|add|remove|upgrade|verify> <device> [name]")
    return dispatch[args.pkg_cmd](args)

# ── CLI ───────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(prog="purrstrap", description="PURR OS final image builder")
    sub = parser.add_subparsers(dest="cmd")

    p_build = sub.add_parser("build",  help="Build firmware for a device")
    p_build.add_argument("device")

    p_flash = sub.add_parser("flash",  help="Build + flash to device")
    p_flash.add_argument("device")
    p_flash.add_argument("-p", "--port", default=None)
    p_flash.add_argument("--erase", action="store_true", help="Erase flash before writing")

    p_monitor = sub.add_parser("monitor", help="Open serial monitor for a device")
    p_monitor.add_argument("device")
    p_monitor.add_argument("-p", "--port", default=None)
    p_monitor.add_argument("-b", "--baud", default=None)

    p_clean = sub.add_parser("clean",  help="Remove build artifacts")
    p_clean.add_argument("device")

    p_generate = sub.add_parser("generate", help="Regenerate CoreOS/sdkconfig_<device> from device.pcat")
    p_generate.add_argument("device", nargs="?", default=None, help="omit to regenerate all devices")
    p_generate.add_argument("--check", action="store_true",
                             help="diff in-memory output against committed file; exit nonzero on drift, don't write")

    p_bake = sub.add_parser("bake", help=f"Build all devices → releases/v{PURROS_VERSION}/")
    p_bake.add_argument("--dp", action="store_true",
                         help="Also package a full developer-preview archive into "
                              "CatReleases/DP<N>/ (split+merged images, README, zip) — "
                              "opt-in, meant for a tagged-release GitHub Action, not "
                              "routine local bakes.")
    sub.add_parser("list",   help="List supported devices")
    sub.add_parser("status", help="Show workspace config")
    sub.add_parser("doctor", help="Check environment health")

    # ── pkg — local package manager (v1, no network) ─────────────────────────
    p_pkg = sub.add_parser("pkg", help="Local package manager (static selection + .meow/.hiss hot-load)")
    pkg_sub = p_pkg.add_subparsers(dest="pkg_cmd")

    p_pkg_list = pkg_sub.add_parser("list", help="Show what's selected for a device")
    p_pkg_list.add_argument("device")

    p_pkg_add = pkg_sub.add_parser("add", help="Select a driver/module/app for a device (static — edits device.pcat)")
    p_pkg_add.add_argument("device")
    p_pkg_add.add_argument("name")

    p_pkg_remove = pkg_sub.add_parser("remove", help="Unselect a static package")
    p_pkg_remove.add_argument("device")
    p_pkg_remove.add_argument("name")
    p_pkg_remove.add_argument("--force", action="store_true", help="Remove even if something depends on it")

    p_pkg_upgrade = pkg_sub.add_parser("upgrade", help="Re-check version of one (or all) selected static packages")
    p_pkg_upgrade.add_argument("device")
    p_pkg_upgrade.add_argument("name", nargs="?", default=None)

    p_pkg_verify = pkg_sub.add_parser("verify", help="Check installed.json against device.pcat + source/ for drift")
    p_pkg_verify.add_argument("device")

    p_pkg_app = pkg_sub.add_parser("app", help="Runtime .meow/.hiss hot-load (the one real file-drop path)")
    app_sub = p_pkg_app.add_subparsers(dest="app_cmd")

    p_app_list = app_sub.add_parser("list", help="List apps installed via pkg app")
    p_app_list.add_argument("device")

    p_app_install = app_sub.add_parser("install", help="Stage a .meow/.hiss script onto a device's flash image")
    p_app_install.add_argument("device")
    p_app_install.add_argument("name")
    p_app_install.add_argument("--to", choices=["flash", "sd"], default="flash")

    p_app_remove = app_sub.add_parser("remove", help="Remove a staged .meow/.hiss script")
    p_app_remove.add_argument("device")
    p_app_remove.add_argument("name")

    p_app_upgrade = app_sub.add_parser("upgrade", help="Re-stage one (or all) installed .meow/.hiss scripts")
    p_app_upgrade.add_argument("device")
    p_app_upgrade.add_argument("name", nargs="?", default=None)

    args = parser.parse_args()
    dispatch = {
        "build":   cmd_build,
        "flash":   cmd_flash,
        "monitor": cmd_monitor,
        "clean":   cmd_clean,
        "generate": cmd_generate,
        "bake":    cmd_bake,
        "list":    cmd_list,
        "status":  cmd_status,
        "doctor":  cmd_doctor,
        "pkg":     cmd_pkg,
    }
    if args.cmd not in dispatch:
        parser.print_help()
        sys.exit(0)
    dispatch[args.cmd](args)

if __name__ == "__main__":
    main()
