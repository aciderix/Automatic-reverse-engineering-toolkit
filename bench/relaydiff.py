#!/usr/bin/env python3
"""ARET<->Wine execution diff (doc 81 section 4).

Both sides log every call that crosses into the Win32/CRT surface, in a form that
lines up, and this walks the two sequences in lockstep to report the FIRST place they
disagree -- which is the first place our HLE and Wine diverge on a real binary. That
is the root cause of a "MFC gives up" wall: instead of chasing a null object back by
hand, the differ points straight at the API whose return we got wrong (or the branch
that made the program call a different API).

  ARET side : build with ARET_RELAY=1, run with ARET_RELAY=1. Each shim call emits
              NNNN:Call NAME(a0,a1,a2,a3) / NNNN:Ret NAME() retval=X   (stderr)
  Wine side : run under WINEDEBUG=+relay. Wine emits
              NNNN:Call MODULE.Name(args) ret=CALLER
              NNNN:Ret  MODULE.Name() retval=X ret=CALLER

The two are NOT 1:1: Wine additionally logs Win32->Win32 internal plumbing (e.g.
kernel32.GetVersion calling ntdll.RtlGetVersion), which ARET, implementing each API
monolithically, does not. Those internal calls have a CALLER inside a core system
DLL; program/middleware calls into Win32 have a caller outside it. So the Wine trace
is filtered to calls whose caller is NOT in the monolithic-core set, using a module
map (base->name) captured in the same run. What remains is the program's own Win32
call sequence, comparable to ARET's.

Usage:
  relaydiff.py --aret aret.log --wine wine.log --winemap wine.loaddll.log
"""
import argparse
import re
import sys

# Wine builtin DLLs that ARET reimplements as single monolithic shims: a call whose
# CALLER is one of these is Win32-internal plumbing ARET never makes, so it is
# dropped. Everything else (the exe, the shipped redistributables, and the lifted
# DLLs) is "program code" whose Win32 calls must match on both sides.
CORE_CALLERS = {
    "ntdll", "kernel32", "kernelbase", "user32", "gdi32", "win32u", "advapi32",
    "sechost", "ucrtbase", "combase", "rpcrt4", "sspicli", "shcore", "imm32",
    "gdi32full", "msvcp_win", "bcrypt", "win32k",
}

# APIs Wine's +relay excludes by default (RelayExclude); drop them on the ARET side
# too so the sequences match. SetLastError is the notable one MFC hammers.
RELAY_EXCLUDE = {
    "SetLastError", "TlsGetValue", "TlsSetValue", "FlsGetValue", "FlsSetValue",
    "RtlEnterCriticalSection", "RtlLeaveCriticalSection", "RtlTryEnterCriticalSection",
    "EnterCriticalSection", "LeaveCriticalSection", "TryEnterCriticalSection",
}

# Process/CRT/loader startup primitives Wine's msvcrt and loader call that ARET's HLE
# shortcuts (it uses the host heap/locale directly). They are pure asymmetry, carry no
# program meaning, and only add noise to the alignment, so both sides drop them. This
# is not hiding divergence: none of these ever returns a value the program branches on
# in a way our HLE could get wrong -- they are allocator and lock plumbing.
NOISE = {
    "HeapCreate", "HeapAlloc", "HeapFree", "HeapReAlloc", "HeapDestroy", "HeapSize",
    "RtlAllocateHeap", "RtlFreeHeap", "RtlReAllocateHeap", "RtlSizeHeap",
    "RtlInitializeCriticalSection", "RtlInitializeCriticalSectionEx",
    "RtlInitializeCriticalSectionAndSpinCount", "RtlDeleteCriticalSection",
    "InitializeCriticalSection", "InitializeCriticalSectionEx",
    "InitializeCriticalSectionAndSpinCount", "DeleteCriticalSection",
    "TlsAlloc", "TlsFree", "FlsAlloc", "FlsFree",
    "NtAllocateVirtualMemory", "NtFreeVirtualMemory", "NtProtectVirtualMemory",
    "VirtualAlloc", "VirtualFree", "VirtualProtect", "VirtualQuery",
    "RtlEnterCriticalSection", "RtlLeaveCriticalSection",
    "decode_pointer", "encode_pointer", "DecodePointer", "EncodePointer",
    "RtlDecodePointer", "RtlEncodePointer", "lock", "unlock", "_lock", "_unlock",
}


def parse_aret(path):
    """-> ordered list of (name, arg0, retval)."""
    calls = {}
    out = []
    for line in open(path, errors="replace"):
        m = re.match(r"([0-9a-f]+):Call (\w+)\(([0-9a-f]+)", line)
        if m:
            calls[m.group(1)] = (m.group(2), m.group(3))
            continue
        m = re.match(r"([0-9a-f]+):Ret\s+(\w+)\(\) retval=([0-9a-f]+)", line)
        if m and m.group(2) not in RELAY_EXCLUDE and m.group(2) not in NOISE:
            arg0 = calls.get(m.group(1), (m.group(2), "?"))[1]
            out.append((m.group(2), arg0, m.group(3)))
    return out


def parse_winemap(path):
    """Wine +loaddll lines -> list of (base, end, module) sorted by base."""
    mods = []
    for line in open(path, errors="replace"):
        # e.g. trace:loaddll:... Loaded L"C:\\windows\\system32\\kernel32.dll" at 0x7b620000: builtin
        m = re.search(r'Loaded L?"([^"]+)" at (?:0x)?([0-9a-fA-F]+)', line)
        if not m:
            continue
        name = re.split(r"[\\/]", m.group(1))[-1].lower().replace(".dll", "").replace(".exe", "")
        base = int(m.group(2), 16)
        mods.append((base, name))
    mods.sort()
    return mods


def module_of(addr, mods):
    lo, hi = 0, len(mods)
    while lo < hi:
        mid = (lo + hi) // 2
        if mods[mid][0] <= addr:
            lo = mid + 1
        else:
            hi = mid
    return mods[lo - 1][1] if lo > 0 else "?"


def parse_wine(path, mods):
    """-> ordered list of (name, arg0, retval), only calls made by non-core code."""
    pend = {}
    out = []
    for line in open(path, errors="replace"):
        m = re.match(r"([0-9a-f]+):Call\s+([\w.]+)\(([0-9a-fA-F]*).*?\)\s+ret=([0-9a-f]+)", line)
        if m:
            seq, full, arg0, ret = m.group(1), m.group(2), m.group(3), int(m.group(4), 16)
            name = full.split(".")[-1]
            caller = module_of(ret, mods) if mods else "?"
            pend[seq] = (name, arg0.lower().zfill(8) if arg0 else "?", caller)
            continue
        m = re.match(r"([0-9a-f]+):Ret\s+([\w.]+)\(\) retval=([0-9a-f]+)", line)
        if m:
            seq = m.group(1)
            if seq not in pend:
                continue
            name, arg0, caller = pend.pop(seq)
            if name in RELAY_EXCLUDE or name in NOISE:
                continue
            if mods and caller in CORE_CALLERS:
                continue          # Win32-internal plumbing ARET does not replicate
            out.append((name, arg0, m.group(3)))
    return out


def main():
    import difflib
    ap = argparse.ArgumentParser()
    ap.add_argument("--aret", required=True)
    ap.add_argument("--wine", required=True)
    ap.add_argument("--winemap", default=None,
                    help="Wine +loaddll capture; without it, no caller filtering")
    ap.add_argument("--context", type=int, default=10)
    args = ap.parse_args()

    mods = parse_winemap(args.winemap) if args.winemap else []
    a = parse_aret(args.aret)
    w = parse_wine(args.wine, mods)
    print(f"ARET: {len(a)} program Win32 calls   Wine: {len(w)} program Win32 calls")

    # The two traces are NOT the same length: Wine runs a full process (loader, CRT
    # init, every dependency's DllMain) that ARET's runtime does not replicate, so it
    # has hundreds of extra calls. Align the NAME sequences with difflib -- the extra
    # Wine plumbing becomes insertions it skips -- then the divergence is the first
    # aligned pair whose RETURN VALUE differs (our HLE answered differently), or the
    # first large structural break that is not just skippable plumbing (control flow
    # diverged). This is what lets the comparison reach deep into MFC init.
    # Align on the call NAME sequence only. Keying on arg0 too would be wrong: handles
    # and pointers legitimately differ between the two runs (different allocators), so a
    # GetDeviceCaps(hdc) has a different hdc on each side and would never align. Name-
    # only alignment is robust; the arg0 mismatch is then handled at COMPARISON time --
    # a retval is only judged when the inputs (arg0) actually match.
    an = [x[0] for x in a]
    wn = [x[0] for x in w]
    sm = difflib.SequenceMatcher(a=an, b=wn, autojunk=False)
    # Establish sync first. The two traces begin differently (Wine's loader + CRT +
    # per-DLL init vs ARET's bare entry), so the leading opcodes are asymmetry, not
    # divergence. Skip until a solid common run (>= MIN_SYNC identical calls in a row)
    # proves the two executions are in step, and only report divergences after it.
    MIN_SYNC = 4
    synced = False
    benign = []
    handle_diffs = []
    synced_replace_reported = False
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if not synced:
            if tag == "equal" and (i2 - i1) >= MIN_SYNC:
                synced = True
                print(f"synced at ARET#{i1} / Wine#{j1} on "
                      f"{[x[0] for x in a[i1:i1+MIN_SYNC]]}")
            else:
                continue
        if tag == "equal":
            for k in range(i2 - i1):
                ra, rw = a[i1 + k][2], w[j1 + k][2]
                # Only judge a return when the INPUTS match: differing arg0 means a
                # different handle/pointer/index, so a differing return says nothing.
                if a[i1 + k][1] != w[j1 + k][1] or ra == rw:
                    continue
                za, zw = int(ra, 16) == 0, int(rw, 16) == 0
                name = a[i1 + k][0]
                if za != zw and _is_creator(name):
                    # A creator/getter that yields a handle on one side and 0 on the
                    # other: exactly the "get-or-create failed our side" shape.
                    handle_diffs.append((name, a[i1 + k][1], ra, rw, i1 + k, j1 + k))
                else:
                    benign.append((name, a[i1 + k][1], ra, rw))
        elif tag == "replace" and not synced_replace_reported:
            # Name-only alignment, so a 'replace' is a GENUINE control-flow split: the
            # two runs call differently-named APIs at the same point. Strongest signal.
            print(f"\n*** CONTROL-FLOW DIVERGENCE at ARET#{i1} / Wine#{j1} ***")
            print(f"    ARET calls: {[x[0] for x in a[i1:i1+6]]}")
            print(f"    Wine calls: {[x[0] for x in w[j1:j1+6]]}")
            _ctx(a, w, i1, j1, args.context)
            synced_replace_reported = True

    if handle_diffs:
        print(f"\n*** {len(handle_diffs)} CREATOR NULL/non-null divergence(s) "
              f"(handle made on one side, not the other) ***")
        for name, a0, ra, rw, ia, iw in handle_diffs[:12]:
            side = "ARET=NULL Wine=ok" if int(ra, 16) == 0 else "ARET=ok Wine=NULL"
            print(f"    {name}(arg0={a0})  ARET={ra} Wine={rw}   [{side}]  @ARET#{ia}")
        print("\n    context around the FIRST creator divergence:")
        _ctx(a, w, handle_diffs[0][4], handle_diffs[0][5], args.context)
    print(f"\nvalue-only retval diffs (colors/metrics/counts): {len(benign)}")
    _benign(benign)
    return 1 if handle_diffs else 0


def _is_creator(name):
    """Names whose return is a HANDLE/pointer, where 0 means failure (not a valid
    value like a black COLORREF or a zero metric)."""
    if name in ("GetSysColor", "GetSystemMetrics", "GetDeviceCaps", "GetTickCount",
                "GetCurrentThreadId", "GetCurrentProcessId", "GetLastError",
                "GetSysColorBrush"):  # GetSysColorBrush is a handle but stable; skip noise
        return False
    for p in ("Create", "Load", "Open", "Alloc", "Find", "GetModuleHandle", "GetDC",
              "GetWindowDC", "BeginPaint", "RegisterClass", "GetStockObject",
              "SelectObject", "GetMenu", "GetParent", "GetWindow", "SysAllocString",
              "GetProcAddress", "GetActiveWindow", "GetFocus", "SetFocus"):
        if name.startswith(p):
            return True
    return False


def _benign(b):
    from collections import Counter
    c = Counter(x[0] for x in b)
    for name, n in c.most_common(15):
        ex = next(x for x in b if x[0] == name)
        print(f"    {name:<28} x{n:<4} e.g. arg0={ex[1]} ARET={ex[2]} Wine={ex[3]}")


def _ctx(a, w, i, j, c):
    print("    --- aligned context (name(arg0) : ARET_ret | Wine_ret) ---")
    for k in range(1, c + 1):
        ii, jj = i - c + k - 1, j - c + k - 1
        ae = a[ii] if 0 <= ii < len(a) else ("-", "-", "-")
        we = w[jj] if 0 <= jj < len(w) else ("-", "-", "-")
        flag = "" if (ae[0], ae[2]) == (we[0], we[2]) else "   <-- differs"
        an = f"{ae[0]}({ae[1]})"; wnm = f"{we[0]}({we[1]})"
        print(f"    A#{ii:<5} {an:<30} {ae[2]:>8} | W#{jj:<5} {wnm:<30} {we[2]:>8}{flag}")


if __name__ == "__main__":
    sys.exit(main())
