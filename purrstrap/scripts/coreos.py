"""Build the PURR OS base system (PurrOS/) with ESP-IDF. See SPEC.md section 5."""

import glob
import json
import os
import shlex
import shutil
from dataclasses import dataclass

from lib import image as pimg
from lib.model import Action, Param, Script

IDF_VERSION = "5.3.5"
# A board decides the chip. Adding a board means adding a row here and a folder of
# settings files in PurrOS/.
BOARDS = {"tdeck_plus": "esp32s3", "cyd_24c": "esp32"}
PROFILES = ("minimal", "recovery", "full")
PROJECT_DIR = "PurrOS"

# Chip name (as BOARDS gives it) -> the numeric chip id purr_abi.h uses.
CHIP_IDS = {"esp32s3": pimg.CHIP_ESP32S3, "esp32": pimg.CHIP_ESP32}
# profile -> (image type, the name embedded in the header). Only profiles meant to be
# fetched and installed as a PURR image are here; "minimal" (the recovery loader itself)
# is flashed directly, never downloaded, so it has no packaged form.
PACKAGE_KIND = {"recovery": (pimg.IMG_RECOVERY, "kittenos"), "full": (pimg.IMG_OS, "purros")}


# -- finding ESP-IDF ---------------------------------------------------
@dataclass
class Idf:
    kind: str          # "active", "manifest" or "export"
    path: str = ""
    activation: str = ""
    version: str = ""


def _read_version(idf_path):
    try:
        with open(os.path.join(idf_path, "version.txt"), encoding="utf-8") as fh:
            return fh.read().strip().lstrip("v")
    except OSError:
        return ""


def _from_manifest(path):
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return None
    installs = data.get("idfInstalled") or []
    if not installs:
        return None
    chosen = next((i for i in installs if i.get("id") == data.get("idfSelectedId")),
                  installs[0])
    activation = chosen.get("activationScript", "")
    if not activation or not os.path.exists(activation):
        return None
    return Idf("manifest", chosen.get("path", ""), activation,
               str(chosen.get("name", "")).lstrip("v"))


def find_idf(env=None, home=None, windows=None, which=None):
    """Locate ESP-IDF in the order given in the spec, or return None."""
    env = os.environ if env is None else env
    home = home or os.path.expanduser("~")
    windows = (os.name == "nt") if windows is None else windows
    which = which or shutil.which

    idf_path = env.get("IDF_PATH")
    if idf_path and which("idf.py"):
        return Idf("active", idf_path, "", _read_version(idf_path))

    if windows:
        candidates = []
        if env.get("IDF_TOOLS_PATH"):
            candidates.append(os.path.join(env["IDF_TOOLS_PATH"], "eim_idf.json"))
        candidates.append(r"C:\Espressif\tools\eim_idf.json")
    else:
        candidates = [os.path.join(home, ".espressif", "eim_idf.json")]
    for cand in candidates:
        found = _from_manifest(cand)
        if found:
            return found

    if not windows:
        for script in sorted(glob.glob(os.path.join(home, "esp", "*", "esp-idf",
                                                    "export.sh"))):
            root = os.path.dirname(script)
            name = os.path.basename(os.path.dirname(root)).lstrip("v")
            return Idf("export", root, script, _read_version(root) or name)
    return None


# -- running commands --------------------------------------------------
def _psq(text):
    return "'" + text.replace("'", "''") + "'"


def wrap(idf, argv, windows=None):
    """Wrap a command so it runs with ESP-IDF activated."""
    windows = (os.name == "nt") if windows is None else windows
    if idf.kind == "active":
        return list(argv)
    if windows:
        script = (f". {_psq(idf.activation)} *> $null; {argv[0]} "
                  + " ".join(_psq(a) for a in argv[1:])
                  + "; exit $LASTEXITCODE")
        return ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-Command", script]
    return ["bash", "-c",
            f". {shlex.quote(idf.activation)} >/dev/null 2>&1; "
            f"exec {shlex.join(argv)}"]


def child_env(env=None):
    """Environment for ESP-IDF commands, without the MSYS/MinGW variables.

    Git Bash sets MSYSTEM, and then idf.py prints a warning and exits 0
    without doing anything, which would look like a successful build.
    """
    env = os.environ if env is None else env
    return {k: v for k, v in env.items()
            if not k.upper().startswith(("MSYS", "MINGW"))}


def build_dir_name(board, profile):
    return f"build/{board}-{profile}"


def idf_args(board, profile):
    d = build_dir_name(board, profile)
    defaults = ";".join(("sdkconfig.defaults", f"sdkconfig.board.{board}",
                         f"sdkconfig.profile.{profile}"))
    return ["idf.py", "-B", d, "-D", f"IDF_TARGET={BOARDS[board]}",
            "-D", f"SDKCONFIG={d}/sdkconfig",
            "-D", f"SDKCONFIG_DEFAULTS={defaults}"]


# -- project checks ----------------------------------------------------
def project_problems(project, board, profile):
    """Files the build needs that are missing, as readable strings."""
    needed = ["CMakeLists.txt", "sdkconfig.defaults", f"sdkconfig.board.{board}",
              f"sdkconfig.profile.{profile}", f"partitions/{board}.csv"]
    return [f"{PROJECT_DIR}/{n} is missing" for n in needed
            if not os.path.isfile(os.path.join(project, n))]


def _project(ctx):
    return os.path.join(ctx.repo_root, PROJECT_DIR)


def _require_idf(ctx):
    idf = find_idf()
    if idf is None:
        ctx.error("ESP-IDF not found. Install it, or open an ESP-IDF shell. "
                  "Run 'coreos check' for details.")
        return None
    if idf.version and idf.version != IDF_VERSION:
        ctx.warn(f"ESP-IDF {idf.version} found, {IDF_VERSION} expected")
    return idf


# -- actions -----------------------------------------------------------
def check(ctx):
    bad = 0
    idf = find_idf()
    if idf is None:
        ctx.error("ESP-IDF: not found")
        bad += 1
    else:
        where = {"active": "active shell", "manifest": "installer manifest",
                 "export": "export.sh"}[idf.kind]
        ctx.ok(f"ESP-IDF: {idf.path or '(from PATH)'} via {where}")
        if not idf.version:
            ctx.warn(f"version unknown, {IDF_VERSION} expected")
        elif idf.version != IDF_VERSION:
            ctx.warn(f"version {idf.version}, {IDF_VERSION} expected")
        else:
            ctx.ok(f"version {idf.version}")
    project = _project(ctx)
    if not os.path.isdir(project):
        ctx.error(f"{PROJECT_DIR}/ does not exist")
        return 1
    for board in BOARDS:
        for profile in PROFILES:
            problems = project_problems(project, board, profile)
            if problems:
                bad += 1
                ctx.error(f"{board}/{profile}:")
                for line in problems:
                    ctx.info(f"    {line}")
            else:
                ctx.ok(f"{board}/{profile}: ready to build")
    return 1 if bad else 0


def build(ctx, board, profile, clean):
    project = _project(ctx)
    problems = project_problems(project, board, profile)
    if problems:
        ctx.error(f"cannot build {board}/{profile}:")
        for line in problems:
            ctx.info(f"    {line}")
        return 1
    idf = _require_idf(ctx)
    if idf is None:
        return 1
    bdir = os.path.join(project, build_dir_name(board, profile))
    if clean and os.path.isdir(bdir):
        ctx.info(f"removing {bdir}")
        shutil.rmtree(bdir)
    ctx.info(f"building {board}/{profile} into {bdir}")
    code = ctx.run(wrap(idf, idf_args(board, profile) + ["build"]), cwd=project,
                   env=child_env(),
                   log_path=os.path.join(bdir, "purrstrap-build.log"))
    if code != 0:
        ctx.error(f"build failed (exit code {code})")
        return code
    ctx.ok("build finished")
    try:
        with open(os.path.join(bdir, "project_description.json"),
                  encoding="utf-8") as fh:
            name = json.load(fh).get("project_name", "")
    except (OSError, ValueError):
        name = ""
    for rel in (f"{name}.bin" if name else "", "bootloader/bootloader.bin",
                "partition_table/partition-table.bin", "flash_args"):
        if rel and os.path.isfile(os.path.join(bdir, rel)):
            ctx.info(f"    {os.path.join(bdir, rel)}")
    return 0


def clean(ctx, board, profile):
    bdir = os.path.join(_project(ctx), build_dir_name(board, profile))
    if not os.path.isdir(bdir):
        ctx.info(f"nothing to clean: {bdir} does not exist")
        return 0
    shutil.rmtree(bdir)
    ctx.ok(f"removed {bdir}")
    return 0


def size(ctx, board, profile):
    project = _project(ctx)
    if not os.path.isdir(os.path.join(project, build_dir_name(board, profile))):
        ctx.error(f"no build for {board}/{profile}. Run 'coreos build' first.")
        return 1
    idf = _require_idf(ctx)
    if idf is None:
        return 1
    return ctx.run(wrap(idf, idf_args(board, profile) + ["size"]), cwd=project,
                   env=child_env())


def package(ctx, board, profile, version, out):
    """Wrap an already-built profile's binary into an unsigned PURR image container, ready
    for `keys sign`, so it can be published as a release asset (OTA/SPEC.md section 5)."""
    if profile not in PACKAGE_KIND:
        ctx.error(f"{profile}: nothing to package (it is flashed directly, never downloaded)")
        return 1
    project = _project(ctx)
    bdir = os.path.join(project, build_dir_name(board, profile))
    binpath = os.path.join(bdir, "purros.bin")
    if not os.path.isfile(binpath):
        ctx.error(f"no build for {board}/{profile}. Run 'coreos build' first.")
        return 1
    with open(binpath, "rb") as fh:
        payload = fh.read()

    image_type, name = PACKAGE_KIND[profile]
    chip_id = CHIP_IDS[BOARDS[board]]
    image = pimg.make_image(name, version, image_type, 0, payload, chip_id)

    dest = out or os.path.join(bdir, f"{name}.kitt")
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    with open(dest, "wb") as fh:
        fh.write(image)
    ctx.ok(f"{dest}: {len(image)} bytes ({name} {version}, unsigned)")
    ctx.info(f"sign it: purrstrap keys sign --key <role>.key --image {dest} --key-id <id>")
    return 0


_BOARD = Param("board", "choice", default="tdeck_plus", choices=tuple(BOARDS),
               help="the board to build for (it decides the chip)")
_PROFILE = Param("profile", "choice", default="full", choices=PROFILES,
                help="minimal = recovery loader, recovery = KittenOS, full = normal system")

SCRIPT = Script(
    name="coreos",
    title="Core OS",
    description="Build the PURR OS base system (PurrOS/) with ESP-IDF.",
    actions=(
        Action("check", "Check", check,
               help="report ESP-IDF and project status without building"),
        Action("build", "Build", build,
               help="build one board and profile",
               params=(_BOARD, _PROFILE,
                       Param("clean", "bool", default=False,
                             help="delete the build directory first"))),
        Action("clean", "Clean", clean,
               help="delete one build directory", params=(_BOARD, _PROFILE)),
        Action("size", "Size", size,
               help="show memory use of an existing build",
               params=(_BOARD, _PROFILE)),
        Action("package", "Package", package,
               help="wrap a built recovery/full binary into an unsigned PURR image, ready to sign",
               params=(_BOARD, _PROFILE,
                       Param("version", "str", required=True, help='e.g. "1.2.0"'),
                       Param("out", "path", default="",
                             help="output file (default: <board>-<profile> build dir)"))),
    ),
)
