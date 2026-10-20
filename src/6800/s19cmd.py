#!/usr/bin/env python3
"""Convert a Motorola S19 file to a FLEX binary (.CMD) file.

FLEX binary records are $02 addr_hi addr_lo count data...; the transfer
address record is $16 addr_hi addr_lo. Contiguous S1 data is merged and
split into records of at most 255 bytes.
"""
import sys


def read_s19(path):
    mem = {}
    entry = None
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith("S"):
                continue
            kind = line[1]
            raw = bytes.fromhex(line[2:])
            if sum(raw) & 0xFF != 0xFF:
                sys.exit(f"{path}: bad checksum: {line}")
            if kind == "1":
                addr = raw[1] << 8 | raw[2]
                for i, b in enumerate(raw[3:-1]):
                    mem[addr + i] = b
            elif kind == "9":
                entry = raw[1] << 8 | raw[2]
    return mem, entry


def runs(mem):
    addrs = sorted(mem)
    start = prev = None
    for a in addrs:
        if start is None:
            start = prev = a
        elif a == prev + 1 and a - start < 255:
            prev = a
        else:
            yield start, prev
            start = prev = a
    if start is not None:
        yield start, prev


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: s19cmd.py in.s19 out.cmd")
    mem, entry = read_s19(sys.argv[1])
    out = bytearray()
    for lo, hi in runs(mem):
        out += bytes([0x02, lo >> 8, lo & 0xFF, hi - lo + 1])
        out += bytes(mem[a] for a in range(lo, hi + 1))
    if entry is not None:
        out += bytes([0x16, entry >> 8, entry & 0xFF])
    with open(sys.argv[2], "wb") as f:
        f.write(out)


if __name__ == "__main__":
    main()
