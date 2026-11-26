#!/usr/bin/env python3
"""How much of the new link software is in a module or driver, and how much in the kernel? (owner's rule, 2026-11-25: kernel changes are fine where unavoidable, but at least 70% of the software must be in the module.)

    python3 tools/prox/split_report.py            prints the table and exits 1 if the module-side share is under 70%

Counts source lines that are not blank and not comments. The lists below say which file is which; add a file here when it is written. Kernel side = compiled into the kernel image (which the Store cannot update);
module side = a signed module or driver (the Store can), plus the plain-C cores that go into them and into the Mini."""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TARGET = 0.70

KERNEL = [
    "PurrOS/main/purr_rd_host.c",            # TEMPORARY: moves into the virtual display driver (plan1.md); until then it is kernel-resident
    "PurrOS/main/purr_rd_host.h",
    "PurrOS/main/purr_link.c",               # the pump, the pairing question and the call wrappers: kernel-side by nature
    "PurrOS/main/purr_link.h",
    "PurrOS/components/kernel/src/purr_link_bus.c",     # bus ABI 10: the thin primitives
    "PurrOS/components/kernel/src/purr_link_bus.h",
    # the link services table (the kernel's thin primitives) goes here when it is written: PurrOS/main/purr_link_services.c
]
MODULE = [
    "Modules/proximity/purr_prox.c",
    "Modules/proximity/purr_prox.h",
    "Modules/proximity/purr_px_crypto.c",
    "Modules/proximity/purr_px_crypto.h",
    "Modules/proximity/purr_px_pair.c",
    "Modules/proximity/purr_px_pair.h",
    "Modules/proximity/purr_px_link.c",
    "Modules/proximity/purr_px_link.h",
    "Modules/proximity/esp/purr_px_esp.c",
    "Modules/proximity/esp/purr_px_esp.h",
    "Modules/drivers/proximity_module.c",
    "Modules/vdisplay/purr_rdisp.c",
    "Modules/vdisplay/purr_rdisp.h",
]


def sloc(path):
    text = open(os.path.join(ROOT, path), encoding="utf-8", errors="replace").read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return sum(1 for line in text.splitlines() if line.strip())


def main():
    rows = []
    for side, files in (("kernel", KERNEL), ("module", MODULE)):
        for f in files:
            if os.path.isfile(os.path.join(ROOT, f)):
                rows.append((side, f, sloc(f)))
    k = sum(n for s, _f, n in rows if s == "kernel")
    m = sum(n for s, _f, n in rows if s == "module")
    for s, f, n in rows:
        print("%-7s %5d  %s" % (s, n, f))
    total = k + m
    share = m / total if total else 1.0
    print("\nkernel %d lines, module %d lines: the module side is %.0f%% (target %.0f%%)" % (k, m, share * 100, TARGET * 100))
    return 0 if share >= TARGET else 1


if __name__ == "__main__":
    sys.exit(main())
