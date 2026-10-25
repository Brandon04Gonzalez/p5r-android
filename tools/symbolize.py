#!/usr/bin/env python3
"""Map a hades.exe address (as logged: hades.exe+<va>) to the recompiled function and guest instruction.

Usage: python tools/symbolize.py <hades.map> <va hex> [generated dir]
"""
import bisect
import re
import sys


def main():
    va = int(sys.argv[2], 16)
    syms = []
    for line in open(sys.argv[1], errors="replace"):
        m = re.match(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s", line)
        if m:
            syms.append((int(m.group(2), 16), m.group(1)))
    syms.sort()
    k = bisect.bisect_right([s[0] for s in syms], va) - 1
    addr, name = syms[k]
    print(f"{name} + {va - addr:#x}")
    fn = re.search(r"(main|sdk)_([0-9A-F]{8})", name)
    if fn:
        print(f"guest function: {fn.group(1)}+{int(fn.group(2), 16):#x}")


if __name__ == "__main__":
    main()
