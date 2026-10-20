#!/usr/bin/env python3
"""Check the FNETDRV relocation table.

The package is assembled twice, at ORG and ORG+$100. Every byte that
differs must be the high byte of an operand listed in reltbl, and every
reltbl entry must differ. A missing entry means an absolute reference
written without the rel macro; it would point into the load image after
the installer moves the package.

usage: relocchk.py a.s19 a.lst b.s19
  a = built at the default PKGORG, b = built at PKGORG+$100
"""
import re
import sys

from s19cmd import read_s19


def symbols(lst):
    sym = {}
    for m in re.finditer(r"\b([A-Z0-9_]+) :\s+([0-9A-F]+) C", open(lst).read()):
        sym[m.group(1)] = int(m.group(2), 16)
    return sym


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    a, _ = read_s19(sys.argv[1])
    b, _ = read_s19(sys.argv[3])
    sym = symbols(sys.argv[2])
    org, end = sym["PKGBEG"], sym["PKGEND"]
    rt, re_ = sym["RELTBL"], sym["RELEND"]

    table = {a[i] << 8 | a[i + 1] for i in range(rt, re_, 2)}
    found, errors = set(), []
    for off in range(end - org):
        x, y = a.get(org + off), b.get(org + 0x100 + off)
        if x == y:
            continue
        if x is None or y is None or (y - x) & 0xFF != 1:
            errors.append(f"offset ${off:04X}: ${x} vs ${y}")
        else:
            found.add(off)
    for off in sorted(found - table):
        errors.append(f"offset ${off:04X} (${org + off:04X}): address not in reltbl")
    for off in sorted(table - found):
        errors.append(f"reltbl entry ${off:04X} does not hold a package address")
    if errors:
        print("\n".join(errors))
        sys.exit(1)
    print(f"relocchk: {len(table)} relocations, package ${end - org:04X} bytes: OK")


if __name__ == "__main__":
    main()
