#!/usr/bin/env python3
"""Build and run the parser fuzzer (plan3.md item 1). Standard library only.

    python fuzz.py                 every target, 100000 iterations each, seed 1
    python fuzz.py verify 500000 7 one target, more iterations, another seed

Compiles fuzz_parsers.c with AddressSanitizer and UndefinedBehaviorSanitizer (any C compiler
that supports them: Apple clang, clang, gcc). A crashing input is saved as crash-<target>.bin
in this folder. See fuzz_parsers.c for how inputs are made.
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
BUILD = os.path.join(HERE, "build")
SOURCES = ["purr_cfg.c", "purr_util.c", "purr_keybag.c", "purr_verify.c", "purr_trailer.c",
           "purr_module.c", "purr_relocate.c", "purr_manifest.c", "purr_wifi.c", "purr_trust.c"]


def main(argv):
    cc = os.environ.get("PURR_GCC") or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        print("error: no C compiler found. Set PURR_GCC.")
        return 2
    os.makedirs(BUILD, exist_ok=True)
    exe = os.path.join(BUILD, "fuzz_parsers")
    cmd = [cc, "-std=c11", "-O1", "-g", "-fno-omit-frame-pointer",
           "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
           "-Wall", "-Wextra", "-Wno-unused-parameter",
           "-I", os.path.join(CORE, "include"), "-I", os.path.join(HERE, ".."),
           os.path.join(HERE, "fuzz_parsers.c")] + \
          [os.path.join(CORE, "src", s) for s in SOURCES] + ["-o", exe]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(res.stdout + res.stderr)
        return 2
    run = subprocess.run([exe] + argv, cwd=HERE)
    return run.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
