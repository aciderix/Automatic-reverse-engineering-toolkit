#!/usr/bin/env python3
"""Dump a frozen process's mapped memory into an ARET snapshot (ARETSNP1).

Usage:  dump_snapshot.py <pid> <out.snap> [lo_va] [hi_va]

Reads /proc/<pid>/maps + /proc/<pid>/mem and writes the readable regions in the
[lo_va, hi_va) window (default: the low 2 GiB, where 32-bit Windows images live)
as records  va:u64-LE  len:u64-LE  bytes.  This is the A-side of the A+B pipe:
freeze the game post-OEP (e.g. SIGSTOP after the /etc/hosts online-hang) and dump
its initialized state; ARET's `--snapshot` seeds it into the lifted code.
"""
import sys, struct

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    pid = int(sys.argv[1]); out = sys.argv[2]
    lo = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x10000
    hi = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x80000000
    regions = []
    with open(f"/proc/{pid}/maps") as f:
        for line in f:
            rng, perms = line.split()[:2]
            if 'r' not in perms:
                continue
            a, b = (int(x, 16) for x in rng.split('-'))
            a, b = max(a, lo), min(b, hi)
            if a < b:
                regions.append((a, b))
    with open(f"/proc/{pid}/mem", "rb") as mem, open(out, "wb") as o:
        o.write(b"ARETSNP1")
        total = 0
        for a, b in regions:
            try:
                mem.seek(a); data = mem.read(b - a)
            except (OSError, ValueError):
                continue
            if data:
                o.write(struct.pack("<QQ", a, len(data))); o.write(data)
                total += len(data)
    print(f"wrote {out}: {len(regions)} regions, {total} bytes")

if __name__ == "__main__":
    main()
