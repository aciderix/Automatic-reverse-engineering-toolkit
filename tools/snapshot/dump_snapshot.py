#!/usr/bin/env python3
"""Dump a process's mapped memory into an ARET snapshot (ARETSNP1).

Usage:  dump_snapshot.py <pid> <out.snap> [lo_va] [hi_va]

The A-side of the A+B pipe: freeze the target (e.g. a game self-unpacked under
Wine, hung past the OEP via the /etc/hosts online-block) and dump its
initialized memory; ARET's `--snapshot` then seeds it into the lifted code.

Reads /proc/<pid>/maps + /proc/<pid>/mem, writing the readable regions in the
[lo_va, hi_va) window (default: the low 2 GiB, where 32-bit Windows images live)
as records  va:u64-LE  len:u64-LE  bytes.  The process is SIGSTOP'd while
dumping (a running process gives EIO on /proc/mem) and SIGCONT'd afterwards.
"""
import sys, struct, os, signal, time

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    pid = int(sys.argv[1]); out = sys.argv[2]
    lo = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x10000
    hi = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x80000000

    # Freeze: /proc/<pid>/mem reads fail with EIO on a *running* process.
    froze = False
    try:
        os.kill(pid, signal.SIGSTOP); froze = True; time.sleep(0.2)
    except ProcessLookupError:
        sys.exit(f"no such pid {pid}")

    try:
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
        kept = 0; total = 0
        with open(f"/proc/{pid}/mem", "rb", 0) as mem, open(out, "wb") as o:
            o.write(b"ARETSNP1")
            for a, b in regions:
                try:
                    mem.seek(a); data = mem.read(b - a)
                except (OSError, ValueError):
                    continue
                if data:
                    o.write(struct.pack("<QQ", a, len(data))); o.write(data)
                    kept += 1; total += len(data)
        print(f"wrote {out}: {kept}/{len(regions)} regions, {total} bytes")
    finally:
        if froze:
            try:
                os.kill(pid, signal.SIGCONT)
            except ProcessLookupError:
                pass

if __name__ == "__main__":
    main()
