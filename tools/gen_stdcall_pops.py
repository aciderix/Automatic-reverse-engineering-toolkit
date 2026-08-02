#!/usr/bin/env python3
"""Generate the __stdcall @N pop table (src/ir/stdcall_pops.rs) from the mingw-w64
import libraries — the ground truth for the callee-pops-args byte count.

Why this exists: the table was hand-maintained, and a missing/wrong @N silently
drifts esp for every caller of that import (invisible to difftest/cpudiff — only a
real binary reveals it). mingw's import libs decorate every __stdcall export as
`_Name@N`, so N is derivable mechanically for ~12,700 core Win32 functions instead
of the ~950 curated by hand.

Soundness rules (measured, not assumed — see doc 81 I12):
  * Restricted to CORE system DLLs. The full lib set has version-skewed DirectX
    duplicates (d3dx9_24..43) with contradictory N; the core set has only 17
    contradictions, 15 of them Script* (gdi32 ships @0 stubs; usp10 is the real
    owner) — resolved by preferring the owner DLL. The 2 remaining are internal
    RPC/NDR symbols no normal binary calls.
  * ADDITIVE MERGE: every existing hand entry is kept verbatim. The generator only
    ADDS names not already present. So the behavioural transpile hash cannot change
    and no caller can regress; it purely widens coverage. Any case where the ground
    truth disagrees with an existing hand value is REPORTED (for review) but the hand
    value is kept — never silently overwritten.
  * @0 entries are omitted (nothing to pop; matches stdcall_pop_bytes returning 0 for
    an absent name).

Run: python3 tools/gen_stdcall_pops.py            # rewrites src/ir/stdcall_pops.rs
     python3 tools/gen_stdcall_pops.py --check    # report only, do not write
"""
import os, re, subprocess, sys, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RS = os.path.join(ROOT, "src/ir/stdcall_pops.rs")
LIBDIR = os.environ.get("MINGW_LIBDIR", "/usr/i686-w64-mingw32/lib")
NM = os.environ.get("NM", "i686-w64-mingw32-nm")

# Core system DLLs whose @N we trust. Ordered so that, on a contradiction, the
# EARLIER lib wins — usp10 before gdi32 makes the Script* family resolve to the real
# owner (gdi32's are @0 forwarding stubs).
CORE = [
    "usp10", "kernel32", "user32", "gdi32", "shell32", "shlwapi", "advapi32",
    "ole32", "oleaut32", "comctl32", "comdlg32", "ws2_32", "winmm", "version",
    "msimg32", "winspool", "gdiplus", "imm32", "secur32", "crypt32", "wininet",
    "urlmon", "uxtheme", "dwmapi", "bcrypt", "userenv", "psapi", "iphlpapi",
    "setupapi", "powrprof", "mpr", "netapi32", "rpcrt4",
]

def ground_truth():
    """name -> pop, from the core libs; first lib in CORE order wins a contradiction."""
    gt = {}
    for lib in CORE:
        path = os.path.join(LIBDIR, f"lib{lib}.a")
        if not os.path.isfile(path):
            continue
        out = subprocess.run([NM, "--defined-only", path], capture_output=True,
                             text=True).stdout
        for m in re.finditer(r' T _([A-Za-z_0-9]+)@([0-9]+)', out):
            name, pop = m.group(1), int(m.group(2))
            if pop == 0:
                continue                      # @0: nothing to pop, omit
            if name not in gt:                # first (owner) lib wins
                gt[name] = pop
    return gt

def parse_current(text):
    """Return (prefix, entries dict, suffix) around the `static TABLE` array body."""
    m = re.search(r'(static TABLE:[^=]*=\s*&\[\n)(.*?)(\n\];)', text, re.S)
    if not m:
        sys.exit("could not locate `static TABLE` array in stdcall_pops.rs")
    body = m.group(2)
    entries = {}
    for em in re.finditer(r'\("([A-Za-z_0-9]+)",\s*(\d+)\)', body):
        entries[em.group(1)] = int(em.group(2))
    prefix = text[:m.start()] + m.group(1)
    suffix = m.group(3) + text[m.end():]
    return prefix, entries, suffix

def main():
    check = "--check" in sys.argv
    text = open(RS).read()
    prefix, cur, suffix = parse_current(text)
    gt = ground_truth()
    if not gt:
        sys.exit(f"no import libs under {LIBDIR}")

    conflicts = {n: (cur[n], gt[n]) for n in cur if n in gt and cur[n] != gt[n]}
    added = {n: gt[n] for n in gt if n not in cur}
    merged = dict(cur)                        # additive: keep every hand entry
    merged.update(added)

    print(f"ground-truth core @N     : {len(gt)}")
    print(f"current hand entries     : {len(cur)}")
    print(f"  agree with GT          : {sum(1 for n in cur if gt.get(n) == cur[n])}")
    print(f"  CONFLICT (kept hand)   : {len(conflicts)}")
    for n, (c, g) in sorted(conflicts.items()):
        print(f"      {n}: hand={c} gt={g}")
    print(f"ADDED from GT            : {len(added)}")
    print(f"merged table             : {len(merged)}")

    if check:
        return
    lines = "".join(f'    ("{n}", {merged[n]}),\n' for n in sorted(merged))
    open(RS, "w").write(prefix + lines + suffix)
    print(f"wrote {RS}")

if __name__ == "__main__":
    main()
