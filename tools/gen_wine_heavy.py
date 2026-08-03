#!/usr/bin/env python3
"""Measure — and prepare — the HEAVY form of Wine reuse (doc 82): compile a WHOLE Wine
ntdll/user-mode source file UNCHANGED into an object, instead of transcribing bodies one at
a time (the medium form). This is the milestone mechanic: one file → dozens of real functions
at once, standing on a small, finite "floor" of primitives ported once.

What it does, for a Wine `.c` (path or a bare ntdll name fetched from the wine-9.0 mirror):
  1. splice FORWARD DECLARATIONS extracted from the file's own `RET WINAPI Name(...)` definitions
     (mingw's winternl.h declares only a subset of Rtl*, so use-before-def would otherwise error);
  2. compile it with `i686-w64-mingw32-gcc` against mingw's NT headers + the checked-in Wine-compat
     shim in tools/wine_heavy/ (wine/debug.h -> no-op TRACE/FIXME; ddk/ntddk.h -> empty; ARRAY_SIZE);
  3. report the functions DEFINED and the FLOOR (undefined symbols), classified libc / heap / NLS.

The floor is the point: it is what must already exist (libc/heap — ARET has them) or be ported
ONCE (the NLS codepage-conversion primitives). Nothing here is guessed or executed — it is a
build-time feasibility MEASUREMENT (§5.0 "measure before coding"), reproducible and checked in so
it survives the ephemeral container. Wiring the object into the HLE build + routing imports +
winediff is the next increment.

Run: python3 tools/gen_wine_heavy.py rtlstr.c        # fetch ntdll/rtlstr.c and measure
     python3 tools/gen_wine_heavy.py /path/to/x.c    # measure a local file
"""
import os, re, subprocess, sys, tempfile, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHIM = os.path.join(ROOT, "tools/wine_heavy")
MINGW_INC = os.environ.get("MINGW_INC", "/usr/i686-w64-mingw32/include")
GCC = os.environ.get("MINGW_GCC", "i686-w64-mingw32-gcc")
NM = os.environ.get("MINGW_NM", "i686-w64-mingw32-nm")
WINE_TAG = os.environ.get("WINE_TAG", "wine-9.0")
MIRROR = f"https://raw.githubusercontent.com/wine-mirror/wine/{WINE_TAG}/dlls/ntdll/"

LIBC = {"memcpy", "memmove", "memset", "memchr", "strlen", "strcmp", "wcschr", "wcslen",
        "wcscmp", "swprintf", "_snwprintf_s", "_snprintf", "toupper", "tolower"}
HEAP = {"GetProcessHeap", "RtlAllocateHeap", "RtlFreeHeap", "RtlReAllocateHeap"}


def fetch(src):
    """Return (path, cleanup) for a local file or a bare ntdll name fetched from the mirror."""
    if os.path.isfile(src):
        return src, None
    name = src if src.endswith(".c") else src + ".c"
    url = MIRROR + name
    tmp = tempfile.NamedTemporaryFile(suffix="_" + os.path.basename(name), delete=False)
    with urllib.request.urlopen(url, timeout=60) as r:
        tmp.write(r.read())
    tmp.close()
    print(f"fetched {url}")
    return tmp.name, tmp.name


def splice_forward_decls(src_text):
    """Prepend forward decls (extracted from this file's own definitions) after the last #include —
    resolves use-before-definition without guessing signatures (they come from the file)."""
    protos = []
    for m in re.finditer(r'(?m)^([A-Za-z_][\w \t]*?\**)\s*WINAPI\s+(\w+)\s*\(', src_text):
        ret, name = m.group(1).strip(), m.group(2)
        i = src_text.index('(', m.end() - 1)
        depth, j = 0, i
        while j < len(src_text):
            if src_text[j] == '(':
                depth += 1
            elif src_text[j] == ')':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        args = ' '.join(src_text[i + 1:j].split())
        protos.append(f"{ret} WINAPI {name}({args});")
    protos = list(dict.fromkeys(protos))
    lines = src_text.splitlines(keepends=True)
    inc = [k for k, l in enumerate(lines) if l.startswith('#include')]
    if not inc:
        return src_text, protos
    at = max(inc) + 1
    block = ("\n/* --- heavy-form: forward decls from this file (mingw winternl.h omits some) --- */\n"
             + "\n".join(protos) + "\n\n")
    return "".join(lines[:at]) + block + "".join(lines[at:]), protos


def main():
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} <ntdll-name|path.c>")
    path, cleanup = fetch(sys.argv[1])
    spliced, protos = splice_forward_decls(open(path).read())
    with tempfile.TemporaryDirectory() as d:
        cpath = os.path.join(d, "unit.c")
        opath = os.path.join(d, "unit.o")
        open(cpath, "w").write(spliced)
        r = subprocess.run(
            [GCC, "-m32", "-c", "-O1", "-w", "-std=gnu11", "-I", SHIM,
             "-isystem", MINGW_INC, "-D__WINESRC__", cpath, "-o", opath],
            capture_output=True, text=True)
        name = os.path.basename(sys.argv[1])
        if r.returncode != 0:
            print(f"[{name}] does NOT compile as-is ({len(protos)} decls spliced):\n{r.stderr[:3000]}")
            if cleanup:
                os.unlink(cleanup)
            sys.exit(1)
        nm = subprocess.run([NM, opath], capture_output=True, text=True).stdout
        defined = sorted(l.split()[-1] for l in nm.splitlines() if " T " in l)
        undef = sorted({l.split()[-1].split("@")[0].lstrip("_")
                        for l in nm.splitlines() if " U " in l})
    if cleanup:
        os.unlink(cleanup)
    nls = [s for s in undef if s not in LIBC and s not in HEAP]
    print(f"[{name}] compiles UNCHANGED -> {len(defined)} functions defined")
    print(f"  floor: {len(undef)} undefined symbols")
    print(f"    libc  ({sum(s in LIBC for s in undef)}): {', '.join(s for s in undef if s in LIBC)}")
    print(f"    heap  ({sum(s in HEAP for s in undef)}): {', '.join(s for s in undef if s in HEAP)}")
    print(f"    other ({len(nls)}) — the port-once floor: {', '.join(nls)}")
    print(f"  => one file, {len(defined)} real Wine functions, standing on {len(nls)} to-port primitives.")


if __name__ == "__main__":
    main()
