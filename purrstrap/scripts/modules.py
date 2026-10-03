"""Build a PURR module: Modules/SPEC.md.

Compiles a module's C source with the ESP-IDF cross compiler, links it twice at two
different base addresses using a linker script that merges code, literals, rodata and
data into one segment, and compares the two results word by word to find every place
that needs the real load address added (the link-twice method, same idea
CatFormat/SPEC.md section 8 describes for `.cat` apps, extended to modules). Writes an
unsigned PURR_IMG_MODULE container; sign it with `keys sign` same as any other image.
"""

import glob
import os
import shutil
import struct

from lib import image as pimg
from lib.model import Action, Param, Script

# board -> (cross compiler prefix, chip id). Reuses coreos.py's mapping conventions.
BOARDS = {"tdeck_plus": ("xtensa-esp32s3-elf", pimg.CHIP_ESP32S3),
          "cyd_24c": ("xtensa-esp32-elf", pimg.CHIP_ESP32),
          "waveshare154": ("xtensa-esp32s3-elf", pimg.CHIP_ESP32S3)}

# name -> MOD_* subtype (purr_abi.h / purr_keybag.c's purr_role_may_sign).
# "coreos" added 2026-09-29 (PurrOS/SPEC.md section 6): the relocatable-module mechanism is
# the same one CoreOS-as-a-file will use, and PURR_MOD_COREOS already exists for it (it's
# PURR_ROLE_BOOT-only, same tier as kernel/bootpkg/loader -- purr_keybag.c), it just had no
# buildable kind here yet. kernel/bootpkg/loader stay out of this table on purpose: they are
# real ESP app images or the boot package binary, not relocatable module payloads, and go
# through coreos.py's packaging instead.
KINDS = {"driver": pimg.MOD_DRIVER, "appmanager": pimg.MOD_APPMANAGER,
         "runtime": pimg.MOD_RUNTIME, "devbundle": pimg.MOD_DEVBUNDLE,
         "coreos": pimg.MOD_COREOS}

# Far enough apart that a real relocated address can't be mistaken for an unrelated
# small integer constant that happens to differ between builds for some other reason.
BASE_A = 0x00000000
BASE_B = 0x00100000


def find_tool(prefix, name):
    exe = f"{prefix}-{name}"
    found = shutil.which(exe)
    if found:
        return found
    roots = [os.environ.get("IDF_TOOLS_PATH", ""), r"C:\Espressif",
             os.path.expanduser("~/.espressif")]
    for root in roots:
        if not root:
            continue
        pattern = os.path.join(root, "tools", "xtensa-esp-elf", "*", "xtensa-esp-elf",
                               "bin", exe + (".exe" if os.name == "nt" else ""))
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[-1]
    return None


def compile_args(repo_root):
    # kernel/include and littlefs are needed only for TYPES a coreos header transitively
    # pulls in (purr_appmgr.h's purr_fs_t parameters need purr_fs.h, which embeds an lfs_t) --
    # never for linkage, same as coreos's own CMakeLists reaching into them the same way.
    return ["-O2", "-std=gnu11", "-ffreestanding", "-fno-builtin", "-mlongcalls",
            "-mtext-section-literals", "-ffunction-sections", "-fdata-sections",
            "-Wall", "-Wextra", "-Werror",
            "-I", os.path.join(repo_root, "PurrOS", "components", "coreos", "include"),
            "-I", os.path.join(repo_root, "PurrOS", "components", "kernel", "include"),
            "-I", os.path.join(repo_root, "PurrOS", "components", "littlefs")]


def linker_script(base, entry):
    """Merges every input section that could hold code or relocatable data into one
    output section at `base`, so the whole module is a single flat, single-based blob --
    Modules/SPEC.md section 3's "one segment, not several". ENTRY() gives --gc-sections a
    root to keep live; without one everything looks unreferenced and gets discarded."""
    return f"""
ENTRY({entry})

SECTIONS
{{
    .module {base:#x} : {{
        *(.literal*)
        *(.text*)
        *(.rodata*)
        *(.data*)
        . = ALIGN(4);
    }}
    .bss (NOLOAD) : {{
        *(.bss*)
        *(COMMON)
    }}
    /DISCARD/ : {{ *(.comment) *(.xtensa.info) *(.xt.prop) *(.debug*) *(.note*) }}
}}
"""


def _symbols(nm_output):
    table = {}
    for line in nm_output.splitlines():
        parts = line.split()
        if len(parts) == 3:
            table[parts[2]] = int(parts[0], 16)
    return table


def _function_addresses(readelf_output):
    """The set of addresses that are genuinely functions (ELF symbol type FUNC), from
    `readelf -s`. Deliberately not nm's section-letter classification: with everything
    merged into one section (linker_script(), for the "one segment" design), nm's T/t just
    means "lives in .module" and includes read-only DATA (a const struct, a string) placed
    there by -mtext-section-literals -- not "this is callable". readelf's type column is the
    compiler's real, section-independent answer to "is this a function or an object"."""
    addrs = set()
    for line in readelf_output.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[3] == "FUNC":
            try:
                addrs.add(int(parts[1], 16))
            except ValueError:
                pass
    return addrs


def build_one(ctx, gcc, objcopy, nm, readelf, sources, out_dir, tag, base, entry):
    """Compiles and links `sources` (one or more files -- CoreOS itself will eventually
    span many, unlike every single-file module built so far) at `base`. Returns
    (flat_bytes, entry_symbol_table, function_addresses) or None on failure (ctx.error
    already called)."""
    objs = []
    for i, source in enumerate(sources):
        obj = os.path.join(out_dir, f"module_{tag}_{i}.o")
        code = ctx.run([gcc] + compile_args(ctx.repo_root) + ["-c", source, "-o", obj])
        if code != 0:
            ctx.error(f"compile failed ({tag}): {source}")
            return None
        objs.append(obj)

    ld = os.path.join(out_dir, f"module_{tag}.ld")
    with open(ld, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(linker_script(base, entry))

    elf = os.path.join(out_dir, f"module_{tag}.elf")
    # -lgcc: compiler-support helpers (64-bit division/shift on a 32-bit target, e.g.
    # __udivdi3) that gcc emits calls to regardless of -ffreestanding -- found building the
    # first real kernel-table module (`uptime`'s divide). Not libc: still fine under -nostdlib.
    code = ctx.run([gcc, "-nostdlib", "-Wl,--gc-sections", "-T", ld,
                    f"-Wl,-Map={os.path.join(out_dir, f'module_{tag}.map')}",
                    *objs, "-lgcc", "-o", elf])
    if code != 0:
        ctx.error(f"link failed ({tag})")
        return None

    flat = os.path.join(out_dir, f"module_{tag}.bin")
    code = ctx.run([objcopy, "-O", "binary", "-j", ".module", elf, flat])
    if code != 0:
        ctx.error(f"objcopy failed ({tag})")
        return None
    with open(flat, "rb") as fh:
        data = fh.read()

    listing = os.path.join(out_dir, f"module_{tag}_symbols.txt")
    code = ctx.run([nm, elf], log_path=listing)
    if code != 0:
        return None
    with open(listing, encoding="utf-8", errors="replace") as fh:
        syms = _symbols(fh.read())

    elf_syms_path = os.path.join(out_dir, f"module_{tag}_elfsyms.txt")
    code = ctx.run([readelf, "-s", elf], log_path=elf_syms_path)
    if code != 0:
        return None
    with open(elf_syms_path, encoding="utf-8", errors="replace") as fh:
        func_addrs = _function_addresses(fh.read())

    return data, syms, func_addrs


def find_relocations(ctx, code_a, code_b, delta, func_addrs):
    """Word-by-word diff between the two builds, split into (data_relocs, code_relocs), or
    None (ctx.error already called) if anything doesn't fit the "differs by exactly delta, or
    doesn't differ at all" rule -- that means something in the module isn't relocatable this
    way (Modules/SPEC.md section 4).

    The split matters and can't be skipped: a word will be relocated against wherever the
    module is mapped EXECUTABLE if the address it holds is a function's (it will be called
    through), or against the plain WRITABLE copy otherwise (an object's address, read or
    written) -- the writable mapping has no exec permission and the executable one is
    typically exec+read only, not writable, so a global relocated to the wrong one either
    can't be written to, or can't be run. Found exactly this on real hardware: a module with
    one writable global crashed with a "cache disabled" panic from writing through the
    executable alias, which a single-base scheme had put its address on."""
    if len(code_a) != len(code_b):
        ctx.error(f"the two builds came out different sizes ({len(code_a)} vs {len(code_b)}) "
                 "-- something about the module isn't deterministic across base addresses")
        return None
    if len(code_a) % 4 != 0:
        ctx.error(f"module size ({len(code_a)}) isn't a multiple of 4")
        return None

    data_relocs, code_relocs = [], []
    mask = 0xFFFFFFFF
    for off in range(0, len(code_a), 4):
        wa = int.from_bytes(code_a[off:off + 4], "little")
        wb = int.from_bytes(code_b[off:off + 4], "little")
        if wa == wb:
            continue
        if (wb - wa) & mask != delta & mask:
            ctx.error(f"offset {off:#x}: not relocatable (base-A word {wa:#010x}, "
                     f"base-B word {wb:#010x} -- doesn't differ by exactly the base delta). "
                     "This usually means a hardcoded address or something non-relocatable.")
            return None
        (code_relocs if wa in func_addrs else data_relocs).append(off)
    return data_relocs, code_relocs


def pack_payload(entry_offset, data_relocs, code_relocs, code):
    body = struct.pack("<II", entry_offset, len(data_relocs))
    body += b"".join(struct.pack("<I", r) for r in data_relocs)
    body += struct.pack("<I", len(code_relocs))
    body += b"".join(struct.pack("<I", r) for r in code_relocs)
    body += code
    return body


def build(ctx, board, source, entry, kind, name, version, out):
    if board not in BOARDS:
        ctx.error(f"unknown board {board}")
        return 1
    if kind not in KINDS:
        ctx.error(f"unknown module kind {kind!r}. Known: {', '.join(KINDS)}")
        return 1
    # --source takes one path, same as always, or a comma-separated list -- CoreOS will
    # need many files linked into one relocatable blob, unlike every module built so far.
    sources = [s.strip() for s in source.split(",") if s.strip()]
    if not sources:
        ctx.error("--source: no files given")
        return 1
    for s in sources:
        if not os.path.isfile(s):
            ctx.error(f"{s}: no such file")
            return 1

    prefix, chip_id = BOARDS[board]
    gcc, objcopy, nm, readelf = (find_tool(prefix, t) for t in ("gcc", "objcopy", "nm", "readelf"))
    if not (gcc and objcopy and nm and readelf):
        ctx.error(f"{prefix} tools not found. Install ESP-IDF, or put them on PATH.")
        return 1

    out_dir = os.path.join(ctx.repo_root, "Modules", "build", name)
    os.makedirs(out_dir, exist_ok=True)

    a = build_one(ctx, gcc, objcopy, nm, readelf, sources, out_dir, "a", BASE_A, entry)
    if a is None:
        return 1
    b = build_one(ctx, gcc, objcopy, nm, readelf, sources, out_dir, "b", BASE_B, entry)
    if b is None:
        return 1
    code_a, syms_a, func_addrs_a = a
    code_b, syms_b, _func_addrs_b = b

    if entry not in syms_a:
        ctx.error(f"entry symbol {entry!r} not found. Known symbols: "
                 f"{', '.join(sorted(syms_a)) or '(none)'}")
        return 1
    entry_offset = syms_a[entry] - BASE_A

    split = find_relocations(ctx, code_a, code_b, BASE_B - BASE_A, func_addrs_a)
    if split is None:
        return 1
    data_relocs, code_relocs = split

    payload = pack_payload(entry_offset, data_relocs, code_relocs, code_a)
    image = pimg.make_image(name, version, pimg.IMG_MODULE, KINDS[kind], payload, chip_id)

    dest = out or os.path.join(out_dir, f"{name}.cat")
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    with open(dest, "wb") as fh:
        fh.write(image)
    ctx.ok(f"{dest}: {len(image)} bytes ({name} {version}, unsigned, "
           f"{len(data_relocs)} data + {len(code_relocs)} code relocation(s), "
           f"entry at code offset {entry_offset:#x})")
    ctx.info(f"sign it: purrstrap keys sign --key <role>.key --image {dest} --key-id <id>")
    return 0


def index(ctx, board, modules_dir, kernelmods_dir, coreos_version, key, out):
    """Builds the module index (OTA/SPEC.md section 6.1): one stanza per already-built,
    already-signed .cat file found in `modules_dir`/`kernelmods_dir`, using the same flat
    key=value format and parser (`purr_manifest.c`) the recovery manifest uses. `type=module`
    for files from `modules_dir` (-> /system), `type=kernelmod` for `kernelmods_dir`
    (-> /kernelmods). Each file's own header supplies its name and version; this tool supplies
    what the header doesn't carry (board, the signing role, and the CoreOS version it targets)."""
    if board not in BOARDS:
        ctx.error(f"unknown board {board}")
        return 1

    entries = []
    for kind_name, d in (("module", modules_dir), ("kernelmod", kernelmods_dir)):
        if not d:
            continue
        if not os.path.isdir(d):
            ctx.error(f"{d}: no such directory")
            return 1
        for fname in sorted(os.listdir(d)):
            if not fname.endswith(".cat"):
                continue
            path = os.path.join(d, fname)
            with open(path, "rb") as fh:
                data = fh.read()
            if not pimg.is_purr_image(data):
                ctx.error(f"{path}: not a PURR image, skipping")
                continue
            f = pimg.header_fields(pimg.parse_header(data))
            entries.append((kind_name, fname, f, len(data), pimg.sha256(data)))

    if not entries:
        ctx.error("no .cat files found in the given directories")
        return 1

    lines = []
    for kind_name, fname, f, size, digest in entries:
        lines.append(f"component={f['name']}")
        lines.append(f"type={kind_name}")
        lines.append(f"version={f['version']}")
        lines.append(f"board={board}")
        lines.append(f"file={fname}")
        lines.append(f"size={size}")
        lines.append(f"sha256={digest.hex()}")
        lines.append(f"key={key}")
        if coreos_version:
            lines.append(f"min_coreos={coreos_version}")
        lines.append("")

    dest = out or "modules.manifest"
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    with open(dest, "w", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    ctx.ok(f"{dest}: {len(entries)} entries ({sum(1 for e in entries if e[0] == 'module')} "
           f"module(s), {sum(1 for e in entries if e[0] == 'kernelmod')} kernelmod(s))")
    return 0


SCRIPT = Script(
    name="modules",
    title="Modules",
    description="Build a PURR module (Modules/SPEC.md): compile, relocate, and pack.",
    actions=(
        Action("build", "Build", build,
               help="compile a module's C source, find its relocations, and pack it",
               params=(
                   Param("board", "choice", default="tdeck_plus", choices=tuple(BOARDS),
                         help="the board to build for (it decides the chip and compiler)"),
                   Param("source", "path", required=True,
                         help="the module's C source file, or several comma-separated"),
                   Param("entry", "str", required=True,
                         help="the C symbol the core calls to enter the module"),
                   Param("kind", "choice", default="driver", choices=tuple(KINDS),
                         help="the module subtype (decides which signing role may sign it)"),
                   Param("name", "str", required=True, help='the module\'s name, e.g. "hello"'),
                   Param("version", "str", required=True, help='e.g. "0.1.0"'),
                   Param("out", "path", default="",
                         help="output file (default: Modules/build/<name>/<name>.cat)"),
               )),
        Action("index", "Build module index", index,
               help="build the module index (OTA/SPEC.md section 6.1) from already-built, "
                    "already-signed .cat files",
               params=(
                   Param("board", "choice", default="tdeck_plus", choices=tuple(BOARDS),
                         help="the board these modules were built for"),
                   Param("modules_dir", "path", default="",
                         help="folder of .cat files for /system (type=module)"),
                   Param("kernelmods_dir", "path", default="",
                         help="folder of .cat files for /kernelmods (type=kernelmod)"),
                   Param("coreos_version", "str", default="",
                         help="the CoreOS version these modules target (min_coreos)"),
                   Param("key", "str", default="developer",
                         help="the role that signed these files, e.g. \"developer\""),
                   Param("out", "path", default="",
                         help="output file (default: modules.manifest)"),
               )),
    ),
)
