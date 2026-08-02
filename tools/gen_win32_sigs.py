#!/usr/bin/env python3
"""Extract TYPED Win32 prototypes from the mingw headers via clang's JSON AST.

This is layer 2 of the shim-industrialisation pipeline (doc 82): the SIGNATURE layer.
Layer 1 (gen_stdcall_pops.py) derives the `@N` stack-pop of ~10k APIs from the import
libraries; that is the ABI decoration only. Here we recover the actual PROTOTYPE — return
type, per-argument types, calling convention — which the import libs do not carry. Two
sound uses, neither of which ever ships a guessed value (principe sacre, doc 70 §0):

  --check     Cross-validate: recompute each stdcall's `@N` from the SUM OF ITS ARGUMENT
              SIZES (i686 ABI) and compare to src/ir/stdcall_pops.rs. The two numbers come
              from INDEPENDENT toolchain paths (header prototype vs import-lib mangling), so
              agreement is mutual proof of the ABI layer, and any disagreement is a real
              finding. We only assert where every argument is PROVABLY sized; a by-value
              struct / unknown typedef makes us ABSTAIN (report, never fail) rather than guess.

  --skeleton NAME [NAME...]
              Emit a compile-ready ARET HLE shim scaffold for each NAME, with each argument
              unpacked into a typed local via the right accessor, and a SOUND `aret_unimpl`
              body (it ABORTS until a human fills in real logic). This kills the ABI-plumbing
              boilerplate of a body port without ever inventing behaviour.

The clang AST is parsed from a fixed probe TU including the core system headers. No libclang
bindings needed (we shell out to `clang -Xclang -ast-dump=json`). Autonomous at runtime is
unaffected: this only helps FABRICATE shims; nothing here is linked into the binary.

Run: python3 tools/gen_win32_sigs.py --check
     python3 tools/gen_win32_sigs.py --skeleton StrFromTimeIntervalW GetCPInfoExW
"""
import json, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
POPS = os.path.join(ROOT, "src/ir/stdcall_pops.rs")
MINGW_INC = os.environ.get("MINGW_INC", "/usr/i686-w64-mingw32/include")
CLANG = os.environ.get("CLANG", "clang-18")

# Core system headers whose prototypes matter for the app surface we transpile.
PROBE_HEADERS = [
    "windows.h", "shlwapi.h", "shlobj.h", "mlang.h", "wininet.h", "commctrl.h",
    "commdlg.h", "shellapi.h", "wingdi.h", "winuser.h", "winbase.h",
]

# i686 scalar sizes (bytes). Anything <=4 occupies one 4-byte stack slot; 8-byte scalars
# occupy two. Pointers are 4. wchar_t is 2 on mingw. long/unsigned long are 4 on WIN32.
SCALAR = {
    "char": 1, "signed char": 1, "unsigned char": 1, "_Bool": 1, "bool": 1,
    "short": 2, "short int": 2, "unsigned short": 2, "unsigned short int": 2,
    "wchar_t": 2, "char16_t": 2,
    "int": 4, "unsigned int": 4, "long": 4, "unsigned long": 4, "long int": 4,
    "unsigned long int": 4, "float": 4, "char32_t": 4,
    "long long": 8, "unsigned long long": 8, "long long int": 8,
    "unsigned long long int": 8, "double": 8,
    "void": 0,
}


def slot_size(desugar, qual):
    """Return the i686 stdcall stack bytes for one argument, or None if unprovable."""
    t = (desugar or qual or "").strip()
    # strip const/volatile qualifiers that don't affect size
    t = re.sub(r"\b(const|volatile|restrict|__restrict|__unaligned)\b", "", t).strip()
    t = re.sub(r"\s+", " ", t)
    if "*" in t or "[" in t:
        return 4                     # any pointer / decayed array
    if t.endswith(")") and "(" in t:  # function type param (decays to pointer)
        return 4
    if t in SCALAR:
        s = SCALAR[t]
        return 8 if s == 8 else 4     # <=4 rounds to one slot; 8 stays two
    if t.startswith("enum "):
        return 4
    return None                       # struct/union by value, or unknown typedef -> abstain


def parse_ast():
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "probe.c")
        with open(src, "w") as f:
            for h in PROBE_HEADERS:
                f.write(f"#include <{h}>\n")
        out = subprocess.run(
            [CLANG, "-Xclang", "-ast-dump=json", "-fsyntax-only",
             "-target", "i686-w64-mingw32", "-isystem", MINGW_INC, src],
            capture_output=True, text=True)
        if not out.stdout:
            sys.exit(f"clang produced no AST:\n{out.stderr[:2000]}")
        return json.loads(out.stdout)


def collect(ast):
    """name -> dict(cc, ret, params[list of (desugar,qual)], variadic)."""
    sigs = {}

    def walk(n):
        if n.get("kind") == "FunctionDecl" and n.get("name"):
            t = n.get("type", {}).get("qualType", "")
            cc = ("stdcall" if "stdcall" in t else
                  "cdecl" if "cdecl" in t else "none")
            params = []
            variadic = "..." in t
            for c in n.get("inner", []) or []:
                if c.get("kind") == "ParmVarDecl":
                    ct = c.get("type", {})
                    params.append((ct.get("desugaredQualType"), ct.get("qualType")))
            ret = t.split("(", 1)[0].strip()
            # keep the first declaration seen (headers may redeclare)
            sigs.setdefault(n["name"], dict(cc=cc, ret=ret, params=params, variadic=variadic))
        for c in n.get("inner", []) or []:
            walk(c)

    walk(ast)
    return sigs


def load_pops():
    tbl = {}
    for m in re.finditer(r'\("([^"]+)",\s*(\d+)\)', open(POPS).read()):
        tbl[m.group(1)] = int(m.group(2))
    return tbl


def computed_pop(sig):
    """Sum of argument slot sizes for a stdcall function, or None if any arg is unprovable
    (or the function is not stdcall / is variadic)."""
    if sig["cc"] != "stdcall" or sig["variadic"]:
        return None
    total = 0
    for desugar, qual in sig["params"]:
        s = slot_size(desugar, qual)
        if s is None:
            return None
        total += s
    return total


# The mingw HEADER and the mingw IMPORT LIB disagree for these two symbols: the header
# carries a later-era prototype with an extra argument, while the import lib (which is what
# a linked binary actually references, hence AUTHORITATIVE for the runtime stack cleanup)
# mangles the earlier arity. Verified by nm on the .a files: _I_RpcGetAssociationContext@4
# (mincore/rpcrt4) and _mmDrvInstall@12 (winmm). stdcall_pops.rs correctly follows the libs;
# this is a documented header/lib skew, not a table bug. Both are obscure internals off any
# app path. Listed so the cross-check stays green while still SURFACING the skew.
KNOWN_SKEW = {"I_RpcGetAssociationContext", "mmDrvInstall"}


def cmd_check(sigs):
    pops = load_pops()
    proven = conflict = abstain = skew = 0
    conflicts = []
    for name, sig in sorted(sigs.items()):
        c = computed_pop(sig)
        if c is None:
            abstain += 1
            continue
        if name in pops:
            if pops[name] == c:
                proven += 1
            elif name in KNOWN_SKEW:
                skew += 1
            else:
                conflict += 1
                conflicts.append((name, pops[name], c,
                                  "int (" + ", ".join(q for _, q in sig["params"]) + ")"))
    print(f"stdcall prototypes parsed        : {sum(1 for s in sigs.values() if s['cc']=='stdcall')}")
    print(f"@N provable from types           : {proven + conflict}")
    print(f"  agree with stdcall_pops.rs     : {proven}")
    print(f"  CONFLICT with stdcall_pops.rs  : {conflict}")
    print(f"  known header/lib skew (docd)   : {skew}")
    print(f"  abstained (by-val/unknown arg) : {abstain}")
    for name, p, c, proto in conflicts[:40]:
        print(f"    CONFLICT {name}: pops={p} types={c}  [{proto}]")
    print("-" * 42)
    if conflict:
        print("signature cross-check: CONFLICTS FOUND")
        return 1
    print(f"signature cross-check: PASS ({proven} functions mutually proven, "
          f"{skew} documented skew)")
    return 0


# --- skeleton emitter -------------------------------------------------------------------
def accessor(desugar, qual):
    """Pick the ARET arg accessor + a C type for a typed local."""
    t = (desugar or qual or "").strip()
    tq = (qual or desugar or "").strip()
    if "*" in t or "[" in t:
        # string pointer heuristics only affect the comment, not soundness
        if re.search(r"\bWCHAR\b|\bwchar_t\b|LPWSTR|LPCWSTR|PWSTR|PCWSTR", tq):
            return "WP", "uint16_t *", tq
        if re.search(r"\bCHAR\b|\bchar\b|LPSTR|LPCSTR|PSTR|PCSTR", tq):
            return "WS", "char *", tq
        return "WP", "void *", tq
    st = re.sub(r"\b(const|volatile)\b", "", t).strip()
    if st in ("int", "long", "long int", "short", "signed char", "char"):
        return "WI", "int32_t", tq
    return "WU", "uint32_t", tq


def cmd_skeleton(sigs, names):
    for name in names:
        sig = sigs.get(name)
        if not sig:
            print(f"/* {name}: not found in parsed headers */")
            continue
        proto = f"{sig['ret']} ({', '.join(q for _, q in sig['params']) or 'void'})"
        pop = computed_pop(sig)
        print(f"/* {name} :: {proto}"
              + (f"  [stdcall @{pop}]" if pop is not None else f"  [{sig['cc']}]") + " */")
        print(f"uint32_t aret_{name}(uint32_t esp) {{")
        for i, (desugar, qual) in enumerate(sig["params"]):
            acc, cty, orig = accessor(desugar, qual)
            print(f"    {cty:<11} a{i} = ({cty}){acc}({i});   /* {orig} */")
        for i in range(len(sig["params"])):
            print(f"    (void)a{i};")
        print(f'    aret_unimpl("{name}");   /* TODO: port real behaviour (sound: aborts until then) */')
        print("    return 0;")
        print("}")
        print()


# --- A/W marshalling emitter --------------------------------------------------------------
# Classify a parameter's string-ness from its (sugared) type. Narrow vs wide vs not-a-string,
# and input (const) vs output (writable). Marshalling A->W is sound ONLY for INPUT strings.
NARROW_IN  = re.compile(r'\b(LPCSTR|PCSTR|PCTSTR|LPCTSTR)\b|const\s+(unsigned\s+)?char\s*\*|const\s+CHAR\s*\*')
NARROW_OUT = re.compile(r'\b(LPSTR|PSTR|PTSTR|LPTSTR)\b|(?<!const )\bCHAR\s*\*|(?<!const )\bchar\s*\*')
WIDE_IN    = re.compile(r'\b(LPCWSTR|PCWSTR)\b|const\s+(WCHAR|wchar_t)\s*\*')
WIDE_OUT   = re.compile(r'\b(LPWSTR|PWSTR)\b|(?<!const )\b(WCHAR|wchar_t)\s*\*')


def _strclass(q):
    if NARROW_IN.search(q):  return "narrow_in"
    if WIDE_IN.search(q):    return "wide_in"
    if NARROW_OUT.search(q): return "narrow_out"
    if WIDE_OUT.search(q):   return "wide_out"
    return None


def classify_pair(sigA, sigW):
    """Return ('accept', actions) where actions[i] is 'widen' | 'pass', or ('refuse', reason).
    ACCEPT only when A and W differ solely in DIRECT INPUT (const) string args."""
    if len(sigA["params"]) != len(sigW["params"]):
        return ("refuse", "A/W arities differ")
    actions = []
    for i, ((_, qa), (_, qw)) in enumerate(zip(sigA["params"], sigW["params"])):
        ca, cw = _strclass(qa), _strclass(qw)
        if qa == qw:
            actions.append("pass"); continue
        if ca == "narrow_in" and cw == "wide_in":
            actions.append("widen"); continue
        # A narrow OUT buffer marshals to W by a size-tracked round-trip we do NOT model.
        if ca in ("narrow_out",) or cw in ("wide_out",):
            return ("refuse", f"arg{i} is an OUTPUT string buffer ({qa}) -> needs size marshalling")
        # Anything else that differs (e.g. LPLOGFONTA vs LPLOGFONTW: distinct struct layouts,
        # the documented A!=marshal(W) trap, doc 70) is NOT a pure-string marshal.
        return ("refuse", f"arg{i} differs beyond strings ({qa} vs {qw}) -> A is not marshal(W)")
    if "widen" not in actions:
        return ("refuse", "no string arg differs (A and W identical) -> no marshalling needed")
    return ("accept", actions)


def cmd_marshal(sigs, names):
    for name in names:
        if not name.endswith("A"):
            print(f"/* {name}: pass the ANSI (…A) entry point */"); continue
        base = name[:-1]; wname = base + "W"
        sigA, sigW = sigs.get(name), sigs.get(wname)
        if not sigA or not sigW:
            print(f"/* {name}: A or W not found in parsed headers */"); continue
        verdict, info = classify_pair(sigA, sigW)
        protoA = f"{sigA['ret']} ({', '.join(q for _, q in sigA['params']) or 'void'})"
        if verdict == "refuse":
            # SOUND refusal: no thunk. A hand implementation is required; the abort keeps it honest.
            print(f"/* {name} :: {protoA}\n"
                  f" * REFUSED (not a pure-string marshal): {info}.\n"
                  f" * Emit a hand shim; do NOT auto-derive from {wname}. */")
            print(f"uint32_t aret_{name}(uint32_t esp) {{ aret_unimpl(\"{name}\"); return 0; }}\n")
            continue
        actions = info
        print(f"/* {name} :: {protoA}\n"
              f" * Auto-marshalled A->{wname}: widen input string args (u32_a2w), pass the rest,\n"
              f" * call the W core. NULL passes through as NULL. ASCII-exact (u32_a2w). */")
        print(f"uint32_t aret_{name}(uint32_t esp) {{")
        for i, act in enumerate(actions):
            if act == "widen":
                print(f"    const char *s{i} = (const char *)WP({i});")
                print(f"    int n{i} = 0; if (s{i}) while (s{i}[n{i}]) n{i}++;")
                print(f"    uint16_t w{i}[n{i} + 1]; u32_a2w(s{i}, w{i}, n{i} + 1);")
        n = len(actions)
        print(f"    uint32_t fr[{n}];")
        for i, act in enumerate(actions):
            if act == "widen":
                print(f"    fr[{i}] = s{i} ? (uint32_t)(uintptr_t)w{i} : 0;")
            else:
                print(f"    fr[{i}] = WU({i});")
        print(f"    return aret_{wname}((uint32_t)(uintptr_t)fr);")
        print("}")
        print()


def main():
    args = sys.argv[1:]
    ast = parse_ast()
    sigs = collect(ast)
    if not args or args[0] == "--check":
        sys.exit(cmd_check(sigs))
    if args[0] == "--skeleton":
        cmd_skeleton(sigs, args[1:])
        return
    if args[0] == "--marshal":
        cmd_marshal(sigs, args[1:])
        return
    sys.exit(f"usage: {sys.argv[0]} [--check | --skeleton NAME... | --marshal NAMEA...]")


if __name__ == "__main__":
    main()
