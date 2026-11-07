"""Build a loadable shell: a PURR module of subtype MOD_SHELL (plan7.md, "Shells as installable items").

A shell is ordinary C, the same source the kernel compiles when the shell is built in, plus one small file that exports purr_shell_module_main() (see
PurrOS/main/shell_modules/purr_w10m_module.c). It is compiled with the kernel's own include paths and defines (read from the kernel build's
compile_commands.json, so the headers and sdkconfig.h are the kernel's), with -mlongcalls, and linked twice at two load addresses, as a plain module is
(modules.py: the words that differ by exactly the base are the relocations). What is different from a module:

  - the OS is not reached through a table of function pointers. The linker script gives every name of PurrOS/main/purr_shell_exports.def a placeholder address
    (lib/shellabi.py), the module's literals hold the placeholders, and the build records where they are; the loader writes the real addresses over them.
    A name the shell uses that is not in the list is a link error here (this script prints the missing names), never a crash on a device.
  - the payload says how much bss the shell needs and how many exports it was linked against (lib/shellabi.py: pack_payload).

Sign the result like any image (`keys sign`, the key the device trusts for shells).
"""

import importlib.util
import json
import os
import re
import shlex
import subprocess

from lib import image as pimg
from lib import shellabi
from lib.model import Action, Param, Script

HERE = os.path.dirname(os.path.abspath(__file__))
ENTRY = "purr_shell_module_main"
DEFAULT_DEF = os.path.join("PurrOS", "main", "purr_shell_exports.def")


def _modules():
    spec = importlib.util.spec_from_file_location("purrstrap_modules_for_shells", os.path.join(HERE, "modules.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def kernel_flags(compile_commands, anchor="purr_w10m.c"):
    """The include paths, defines and sdkconfig of the kernel's own compile of `anchor`, from a compile_commands.json: -I, -isystem, -iquote, -D, -U, -include,
    -imacros (and the codegen flags a struct layout depends on). Returns a list of arguments, or raises ShellAbiError."""
    with open(compile_commands, encoding="utf-8") as fh:
        entries = json.load(fh)
    pick = None
    for e in entries:
        if os.path.basename(e["file"]) == anchor:
            pick = e
            break
    if pick is None:
        raise shellabi.ShellAbiError("%s has no compile of %s: give the build directory of a kernel that has the shell built in" % (compile_commands, anchor))
    args = shlex.split(pick["command"])
    keep, i = [], 1
    with_value = ("-I", "-isystem", "-iquote", "-include", "-imacros")
    glued = ("-I", "-D", "-U")
    same_codegen = ("-mdisable-hardware-atomics", "-fstrict-volatile-bitfields")
    while i < len(args):
        a = args[i]
        if a in with_value and i + 1 < len(args):
            keep += [a, args[i + 1]]
            i += 2
            continue
        if a.startswith(glued) and a not in glued:
            keep.append(a)
        elif a in same_codegen:
            keep.append(a)
        i += 1
    return keep


def module_flags(kflags):
    return ["-O2", "-std=gnu11", "-ffreestanding", "-fno-builtin", "-mlongcalls", "-mtext-section-literals", "-ffunction-sections", "-fdata-sections",
            "-fno-tree-loop-distribute-patterns", "-Wall", "-Wno-unused-parameter", "-Wno-sign-compare", "-Wno-unused-function", "-Wno-unused-variable"] + kflags


def linker_script(base, entries):
    """The module's layout (one flat segment, then bss) and the placeholder address of every export."""
    return f"""
ENTRY({ENTRY})

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
        __shell_bss_start = .;
        *(.bss*)
        *(.ext_ram.bss*)
        *(COMMON)
        . = ALIGN(4);
        __shell_bss_end = .;
    }}
    /DISCARD/ : {{ *(.comment) *(.xtensa.info) *(.xt.prop) *(.debug*) *(.note*) }}
}}

""" + shellabi.linker_script(entries)


def _missing(stderr):
    return sorted(set(re.findall(r"undefined reference to [`']([^'`]+)'", stderr)))


def build_one(ctx, tools, flags, sources, out_dir, tag, base, entries):
    gcc, objcopy, nm, readelf = tools
    mods = _modules()
    objs = []
    for i, source in enumerate(sources):
        obj = os.path.join(out_dir, f"shell_{tag}_{i}.o")
        code = ctx.run([gcc] + flags + ["-c", source, "-o", obj])
        if code != 0:
            ctx.error(f"compile failed ({tag}): {source}")
            return None
        objs.append(obj)
    ld = os.path.join(out_dir, f"shell_{tag}.ld")
    with open(ld, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(linker_script(base, entries))
    elf = os.path.join(out_dir, f"shell_{tag}.elf")
    r = subprocess.run([gcc, "-nostdlib", "-Wl,--gc-sections", "-T", ld, f"-Wl,-Map={os.path.join(out_dir, f'shell_{tag}.map')}", *objs, "-lgcc", "-o", elf],
                       capture_output=True, text=True)
    if r.returncode != 0:
        missing = _missing(r.stderr)
        if missing:
            ctx.error("the shell uses %d name(s) that the OS does not export (add each to the END of purr_shell_exports.def and to purr_shell_abi.c's headers if needed): %s"
                      % (len(missing), ", ".join(missing)))
        else:
            ctx.error("link failed (%s):\n%s" % (tag, r.stderr[-1500:]))
        return None
    flat = os.path.join(out_dir, f"shell_{tag}.bin")
    if ctx.run([objcopy, "-O", "binary", "-j", ".module", elf, flat]) != 0:
        ctx.error(f"objcopy failed ({tag})")
        return None
    with open(flat, "rb") as fh:
        data = fh.read()
    listing = os.path.join(out_dir, f"shell_{tag}_symbols.txt")
    if ctx.run([nm, elf], log_path=listing) != 0:
        return None
    with open(listing, encoding="utf-8", errors="replace") as fh:
        syms = mods._symbols(fh.read())
    elf_syms = os.path.join(out_dir, f"shell_{tag}_elfsyms.txt")
    if ctx.run([readelf, "-s", elf], log_path=elf_syms) != 0:
        return None
    with open(elf_syms, encoding="utf-8", errors="replace") as fh:
        funcs = mods._function_addresses(fh.read())
    return data, syms, funcs


def build(ctx, board, source, kernel_build, name, version, out, exports=""):
    mods = _modules()
    if board not in mods.BOARDS:
        ctx.error(f"unknown board {board}")
        return 1
    sources = [s.strip() for s in source.split(",") if s.strip()]
    for s in sources:
        if not os.path.isfile(s):
            ctx.error(f"{s}: no such file")
            return 1
    cc = os.path.join(kernel_build, "compile_commands.json")
    if not os.path.isfile(cc):
        ctx.error(f"{cc}: no such file (--kernel-build is the build directory of a kernel with the shell built in)")
        return 1
    def_path = exports or os.path.join(ctx.repo_root, DEF_DEFAULT_REL)
    try:
        with open(def_path, encoding="utf-8") as fh:
            entries = shellabi.parse_def(fh.read())
        flags = module_flags(kernel_flags(cc)) + ["-I", os.path.join(ctx.repo_root, "PurrOS", "main")]      # the shell's own headers (the kernel finds them next to its sources)
    except (shellabi.ShellAbiError, OSError) as e:
        ctx.error(str(e))
        return 1
    prefix, chip_id = mods.BOARDS[board]
    tools = tuple(mods.find_tool(prefix, t) for t in ("gcc", "objcopy", "nm", "readelf"))
    if not all(tools):
        ctx.error(f"{prefix} tools not found. Install ESP-IDF, or put them on PATH.")
        return 1
    out_dir = os.path.join(ctx.repo_root, "out", "shells", name)
    os.makedirs(out_dir, exist_ok=True)

    a = build_one(ctx, tools, flags, sources, out_dir, "a", mods.BASE_A, entries)
    if a is None:
        return 1
    b = build_one(ctx, tools, flags, sources, out_dir, "b", mods.BASE_B, entries)
    if b is None:
        return 1
    code_a, syms_a, funcs_a = a
    code_b, syms_b, _ = b
    if ENTRY not in syms_a:
        ctx.error(f"the shell has no {ENTRY}(): add the module entry file (PurrOS/main/shell_modules)")
        return 1
    try:
        imports = shellabi.find_imports(code_a, code_b, len(entries))
    except shellabi.ShellAbiError as e:
        ctx.error(str(e))
        return 1
    split = mods.find_relocations(ctx, code_a, code_b, mods.BASE_B - mods.BASE_A, funcs_a)
    if split is None:
        return 1
    data_relocs, code_relocs = split
    bss = syms_a.get("__shell_bss_end", 0) - syms_a.get("__shell_bss_start", 0)
    payload = shellabi.pack_payload(shellabi.ABI_VERSION, len(entries), syms_a[ENTRY] - mods.BASE_A, bss, data_relocs, code_relocs, imports, code_a)
    image = pimg.make_image(name, version, pimg.IMG_MODULE, pimg.MOD_SHELL, payload, chip_id)
    dest = out or os.path.join(out_dir, "shell.cat")
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    with open(dest, "wb") as fh:
        fh.write(image)
    used = len({i for _o, i in imports})
    ctx.ok(f"{dest}: {len(image)} bytes ({name} {version}, unsigned, {len(code_a)} bytes of code and data, {bss} of bss, "
           f"{len(data_relocs)} data + {len(code_relocs)} code relocation(s), {len(imports)} import site(s) of {used} different export(s) out of {len(entries)})")
    ctx.info(f"sign it: purrstrap keys sign --key <key>.key --image {dest} --key-id <id>")
    return 0


DEF_DEFAULT_REL = DEFAULT_DEF

SCRIPT = Script(
    name="shells",
    title="Shells",
    description="Build a loadable shell (plan7.md): compile against the kernel's headers, link against the shell export list, relocate and pack.",
    actions=(
        Action("build", "Build", build,
               help="compile a shell's C sources, find its relocations and import sites, and pack it",
               params=(
                   Param("board", "choice", default="tdeck_plus", choices=("tdeck", "tdeck_plus", "cyd_24c", "waveshare154"),
                         help="the board to build for (it decides the chip and compiler)"),
                   Param("source", "path", required=True, help="the shell's C source files, comma-separated, with the file that defines purr_shell_module_main"),
                   Param("kernel_build", "path", required=True, help="the build directory of a kernel that has this shell built in (its compile_commands.json gives the include paths)"),
                   Param("name", "str", required=True, help='the shell\'s id, e.g. "w10m"'),
                   Param("version", "str", required=True, help='e.g. "1.0.30"'),
                   Param("out", "path", default="", help="output file (default: out/shells/<name>/shell.cat)"),
                   Param("exports", "path", default="", help="the export list (default: PurrOS/main/purr_shell_exports.def)"),
               )),
    ),
)
