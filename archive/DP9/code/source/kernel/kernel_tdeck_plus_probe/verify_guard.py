#!/usr/bin/env python3
"""Verify the probe access gate's tables and interval rules. Run before flashing.

    python verify_guard.py

Parses the REAL allow/deny tables out of purr_probe_guard.c and the REAL base
addresses out of ESP-IDF's esp32s3 reg_base.h, then mirrors classify()'s rules
exactly. Because it reads both real sources rather than a copy, it cannot drift
from the code it checks: edit a window in the C file and this test sees the
edit.

Scope, stated honestly: this validates the TABLES and the RULES, which is where
a realistic mistake lives — a mistyped base address, a window widened without
noticing it swallowed eFuse, an off-by-one at a boundary. It does not execute
the compiled ARM/Xtensa object. The compiled code is validated separately on
the device itself by the `guardtest` command, which runs the real classifier
against real eFuse addresses and reports `all_refused 1`.

Exit status 0 means every check passed.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GUARD_C = os.path.join(HERE, "purr_probe_guard.c")

# Mirror of PROBE_MAX_READ_WORDS in purr_probe_guard.c.
MAX_WORDS = 256


def find_reg_base():
    """Locate esp32s3 reg_base.h. IDF_PATH if exported, else the usual spots."""
    candidates = []
    idf = os.environ.get("IDF_PATH")
    if idf:
        candidates.append(idf)
    candidates += [
        r"C:\esp\v5.3.5\esp-idf",
        os.path.expanduser("~/esp/esp-idf"),
        "/opt/esp-idf",
    ]
    for root in candidates:
        p = os.path.join(root, "components", "soc", "esp32s3", "include", "soc", "reg_base.h")
        if os.path.isfile(p):
            return p
    sys.exit("could not find esp32s3 reg_base.h — export IDF_PATH and retry")


def load_bases(path):
    bases = {}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            m = re.match(r"\s*#define\s+(DR_REG_\w+)\s+(0x[0-9A-Fa-f]+)", ln)
            if m:
                bases[m.group(1)] = int(m.group(2), 16)
    if not bases:
        sys.exit(f"parsed no base addresses from {path}")
    return bases


def load_tables(bases):
    src = open(GUARD_C, encoding="utf-8", errors="replace").read()
    out = {}
    for name in ("s_deny", "s_allow"):
        m = re.search(name + r"\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
        if not m:
            sys.exit(f"could not find {name}[] in purr_probe_guard.c")
        rows = []
        for em in re.finditer(
            r"\{\s*(DR_REG_\w+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*\"([^\"]*)\"", m.group(1)
        ):
            sym, length, label = em.group(1), em.group(2), em.group(3)
            if sym not in bases:
                sys.exit(f"{name}: unknown base symbol {sym}")
            rows.append(
                (bases[sym], int(length, 16 if length.startswith("0x") else 10), label, sym)
            )
        if not rows:
            sys.exit(f"{name}[] parsed as empty — regex out of date with the source?")
        out[name] = rows
    return out["s_allow"], out["s_deny"]


def classify(addr, words, allow, deny):
    """Exact mirror of classify() in purr_probe_guard.c."""
    if words == 0 or words > MAX_WORDS:
        return "bad-span"
    if addr & 3:
        return "unaligned"
    lo, hi = addr, addr + words * 4
    if hi > 0x100000000:
        return "bad-span"
    for base, ln, _l, _s in deny:          # deny first, on ANY overlap
        if lo < base + ln and base < hi:
            return "forbidden-region"
    for base, ln, _l, _s in allow:         # then full containment in ONE window
        if lo >= base and hi <= base + ln:
            return "ok"
    return "not-allowed"


def main():
    bases = load_bases(find_reg_base())
    allow, deny = load_tables(bases)
    fails = []

    print(f"parsed {len(allow)} allow windows, {len(deny)} deny windows\n")
    for b, ln, lbl, sym in deny:
        print(f"  DENY   {b:08x}-{b + ln - 1:08x}  {lbl:<10} ({sym})")
    print()
    for b, ln, lbl, sym in allow:
        print(f"  ALLOW  {b:08x}-{b + ln - 1:08x}  {lbl:<10} ({sym})")
    print()

    if "DR_REG_EFUSE_BASE" not in [s for _b, _l, _lb, s in deny]:
        fails.append("EFUSE is not in the deny table at all")

    # 1. eFuse unreachable at every single word.
    efuse = bases["DR_REG_EFUSE_BASE"]
    for a in range(efuse, efuse + 0x1000, 4):
        if classify(a, 1, allow, deny) != "forbidden-region":
            fails.append(f"eFuse word {a:08x} NOT refused")
    print(f"[1] eFuse: all {0x1000 // 4} words refused")

    # 2. Every word of every deny window.
    n = sum(ln // 4 for _b, ln, _l, _s in deny)
    for b, ln, lbl, _s in deny:
        for a in range(b, b + ln, 4):
            if classify(a, 1, allow, deny) != "forbidden-region":
                fails.append(f"{lbl} word {a:08x} NOT refused")
    print(f"[2] deny windows: all {n} words refused")

    # 3. No allow window may intersect a deny window. This is the property that
    #    stops a future careless bulk allow window re-exposing eFuse.
    over = 0
    for ab, al, albl, _ in allow:
        for db, dl, dlbl, _ in deny:
            if ab < db + dl and db < ab + al:
                fails.append(f"allow {albl} overlaps deny {dlbl}")
                over += 1
    print(f"[3] allow/deny overlap: {over} found (want 0)")

    # 4. A span starting in allowed space and running into a deny window must
    #    be refused whole, not truncated to the safe prefix.
    n = 0
    for b, ln, lbl, _s in deny:
        for back in (4, 16, 64, 256, 1020):
            start = (b - back) & ~3
            words = (back // 4) + 4
            if words <= MAX_WORDS:
                if classify(start, words, allow, deny) != "forbidden-region":
                    fails.append(f"span into {lbl} from {start:08x} NOT refused")
                n += 1
    print(f"[4] spans running into deny windows: {n} checked, all refused")

    # 5. Spans must not straddle two adjacent allow windows — adjacency is a
    #    fact about the chip's map, not an intention of the list. (SPI2/SPI3
    #    really are adjacent, so this case is live, not hypothetical.)
    straddle = 0
    for ab, al, albl, _ in allow:
        for ob, _ol, oblbl, _ in allow:
            if ob == ab + al:
                if classify(ob - 8, 4, allow, deny) == "ok":
                    fails.append(f"span straddles {albl} -> {oblbl} and was allowed")
                straddle += 1
    print(f"[5] adjacent-window straddles: {straddle} checked, none allowed")

    # 6. Boundaries, alignment, span limits, and a few addresses that must
    #    never be reachable.
    spi2 = bases["DR_REG_SPI2_BASE"]
    checks = [
        (classify(spi2 + 1, 1, allow, deny), "unaligned", "unaligned addr"),
        (classify(spi2, 0, allow, deny), "bad-span", "zero words"),
        (classify(spi2, MAX_WORDS + 1, allow, deny), "bad-span", "over word cap"),
        (classify(0xFFFFFFFC, 4, allow, deny), "bad-span", "wrap past 4GB"),
        (classify(spi2, 1, allow, deny), "ok", "SPI2 base allowed"),
        (classify(spi2 + 0xFFC, 1, allow, deny), "ok", "SPI2 last word"),
        (classify(0x60001000, 1, allow, deny), "not-allowed", "gap: UART0..SPI1"),
        (classify(0x6002F000, 1, allow, deny), "not-allowed", "gap: past UART2"),
        (classify(0x00000000, 1, allow, deny), "not-allowed", "null"),
        (classify(0x3FC80000, 1, allow, deny), "not-allowed", "internal SRAM"),
        (classify(0x42000000, 1, allow, deny), "not-allowed", "flash mmap"),

        # The RTC-span boundary, pinned explicitly. The RTC_CNTL deny window was
        # narrowed from 0x1000 to 0x800 so the SAR ADC could be reached; these
        # assert that the narrowing bought exactly SENS and nothing adjacent.
        # Without them, a future widening back to 0x1000 (or a slip in the other
        # direction) would pass silently.
        (classify(0x60008000, 1, allow, deny), "forbidden-region", "RTC_CNTL base"),
        (classify(0x600080FC, 1, allow, deny), "forbidden-region", "RTC_CNTL last reg"),
        (classify(0x600083FC, 1, allow, deny), "forbidden-region", "RTC_CNTL slot end"),
        (classify(0x60008400, 1, allow, deny), "forbidden-region", "RTCIO base"),
        (classify(0x600087FC, 1, allow, deny), "forbidden-region", "RTCIO slot end"),
        (classify(0x60008800, 1, allow, deny), "ok", "SENS base reachable"),
        (classify(0x600088FC, 1, allow, deny), "ok", "SENS last real reg"),
        (classify(0x60008BFC, 1, allow, deny), "ok", "SENS slot end"),
        (classify(0x60008C00, 1, allow, deny), "not-allowed", "past SENS slot"),
        (classify(0x60008FFC, 1, allow, deny), "not-allowed", "end of RTC 4K span"),
        # A span may not walk out of SENS back into denied RTC space.
        (classify(0x600087F0, 8, allow, deny), "forbidden-region", "RTCIO->SENS straddle"),
    ]
    for got, want, label in checks:
        if got != want:
            fails.append(f"{label}: got {got}, want {want}")
    print(f"[6] boundary/alignment: {len(checks)} checks")

    print()
    if fails:
        print(f"FAILED — {len(fails)} problem(s):")
        for f in fails[:20]:
            print("  " + f)
        if len(fails) > 20:
            print(f"  ... and {len(fails) - 20} more")
        return 1
    print("ALL CHECKS PASSED")
    print("eFuse, flash-encryption and secure-boot registers are unreachable.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
