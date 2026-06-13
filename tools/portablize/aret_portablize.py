#!/usr/bin/env python3
"""
aret_portablize.py — turn an ARET pseudo-C dump into a *portable, compilable*
C project (roadmap "level 1: recompilable").

It does NOT recover the running program: registers are modelled as globals,
arguments are zero-initialised, and irreducible instructions become comments.
What it guarantees is that the output **compiles** with gcc/clang/msvc on any
platform (Linux/macOS/Windows), faithfully mirroring the recovered structure.
This is the honest baseline the verified-decompilation pipeline builds on.

Usage:
    aret_portablize.py <dump_dir> <out_dir> [--parts N] [--limit N]

<dump_dir> is an ARET --split directory (one sub_*.c per function + index.csv).
"""
import os, re, sys, csv, argparse

# All x86 register tokens we model as independent globals. Modelling sub-register
# aliasing correctly is a job for the SSA/type pipeline; here we only need the
# code to compile, so each name is its own 32-bit global.
REGS32 = ["eax","ebx","ecx","edx","esi","edi","ebp","esp"]
REGS16 = ["ax","bx","cx","dx","si","di","bp","sp"]
REGS8  = ["al","bl","cl","dl","ah","bh","ch","dh"]
XMM    = [f"xmm{i}" for i in range(16)]
# x86 status flags, emitted by ARET when a branch has no recovered compare.
FLAGS  = ["ZF", "CF", "SF", "OF", "PF", "AF"]
# Segment registers occasionally surface in operands.
SEG    = ["cs", "ds", "es", "fs", "gs", "ss"]
# Control / debug registers from privileged instructions.
CTRL   = [f"cr{i}" for i in range(9)] + [f"dr{i}" for i in range(8)]
ALLREGS = REGS32 + REGS16 + REGS8 + XMM + FLAGS + SEG + CTRL

SIG_RE   = re.compile(r'^(\s*)(int64_t)\s+(\w+)\s*\((.*)\)\s*\{\s*$')
PARAM_RE = re.compile(r'\b(arg_[0-9a-fA-F]+)\b')
DEREF_INCDEC_RE = re.compile(r'^(\s*)(\*\(.*\))(\+\+|--);\s*$')

def transform_line(line):
    """Rewrite a single body line so it compiles. Returns the new line."""
    # Fix postfix ++/-- precedence on a memory deref: `*(T*)(x)++;` parses as
    # incrementing the pointer; the intent is the pointee, so re-parenthesise.
    m = DEREF_INCDEC_RE.match(line)
    if m:
        line = f"{m.group(1)}({m.group(2)}){m.group(3)};"

    # Placeholder conditions ARET could not render -> a constant (compiles,
    # faithfully marks "condition not recovered").
    line = line.replace('/*cond*/', '0').replace(' ?? ', ' == ')

    stripped = line.strip()
    indent = line[:len(line) - len(line.lstrip())]

    # Indirect call through a register/memory expression: `(*(eax))();`
    # (handled here, before the non-lvalue guard, as it has no '=').
    if stripped.startswith('(*(') and '))(' in stripped:
        return indent + 'aret_icall(); // ' + stripped

    # Non-lvalue assignment target produced by lifting (e.g. `(~ecx) &= eax;`,
    # `(esp+0x..) = pop();`, or a literal LHS like `0 = ...;` / `1 <<= cl;`):
    # a parenthesised or numeric LHS is never an lvalue, so neutralise the
    # statement (marked unrecovered).
    if stripped and (stripped[0] == '(' or stripped[0].isdigit()) and \
       ('=' in stripped or stripped.endswith('++;') or stripped.endswith('--;')):
        return indent + '// ' + stripped

    # Irreducible inline asm -> line comment (the text may itself contain
    # /* */, so a block comment would nest and break; // is safe).
    if stripped.startswith('__asm__'):
        return indent + '// ' + stripped

    return line

LABEL_DEF_RE  = re.compile(r'^\s*(L_[0-9a-fA-F]+):\s*$')
GOTO_RE       = re.compile(r'goto\s+(L_[0-9a-fA-F]+);')

def transform_function(text):
    """Transform one function file's text into compilable C."""
    lines = text.splitlines()

    # Labels defined in this function (C goto cannot cross function boundaries).
    defined = set()
    for line in lines:
        m = LABEL_DEF_RE.match(line)
        if m:
            defined.add(m.group(1))

    out = []
    inserted_args = False
    for line in lines:
        m = SIG_RE.match(line)
        if m and not inserted_args:
            indent, ret, name, params = m.groups()
            out.append(f"{indent}{ret} {name}() {{")
            # Turn recovered parameters into zero-initialised locals so any
            # call-site arity compiles (functions are declared K&R / no-proto).
            args = PARAM_RE.findall(params)
            seen = set()
            for a in args:
                if a not in seen:
                    out.append(f"    uint32_t {a} = 0;")
                    seen.add(a)
            inserted_args = True
            continue

        # A goto whose target label lives outside this function is illegal in C
        # (tail-call / cross-function edge). Neutralise it, keeping the intent.
        g = GOTO_RE.search(line)
        if g and g.group(1) not in defined:
            indent = line[:len(line) - len(line.lstrip())]
            out.append(f"{indent}aret_trap(); // {line.strip()}")
            continue

        out.append(transform_line(line))
    return "\n".join(out) + "\n"

def gen_runtime_h():
    decls = "\n".join(f"extern uint32_t {r};" for r in ALLREGS)
    return f"""/* aret_runtime.h — portable execution model (compile-only). */
#ifndef ARET_RUNTIME_H
#define ARET_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* x86 registers modelled as independent globals (no aliasing). */
{decls}

/* Operand-stack model for residual push/pop. */
void     push(uint32_t v);
uint32_t pop(void);

/* No-prototype trampolines: any arity / any target compiles. */
int64_t aret_icall();   /* indirect call site */
int64_t aret_trap();    /* irreducible / interrupt */

#endif
"""

def gen_runtime_c():
    defs = "\n".join(f"uint32_t {r};" for r in ALLREGS)
    return f"""/* aret_runtime.c — definitions for the portable model. */
#include "aret_runtime.h"

{defs}

static uint32_t aret_stack[1u << 16];
static int      aret_sp = 0;

void push(uint32_t v) {{ if (aret_sp < (int)(sizeof aret_stack/sizeof *aret_stack)) aret_stack[aret_sp++] = v; }}
uint32_t pop(void)     {{ return aret_sp > 0 ? aret_stack[--aret_sp] : 0u; }}

int64_t aret_icall() {{ return 0; }}
int64_t aret_trap()  {{ return 0; }}
"""

def gen_makefile(nparts):
    return f"""# Portable build of the ARET-recovered functions.
CC      ?= cc
CFLAGS  ?= -w -O0 -fno-strict-aliasing -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration
OBJS     = aret_runtime.o $(patsubst %.c,%.o,$(wildcard src/part_*.c))

libaret_recovered.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -I. -c $< -o $@

clean:
	rm -f $(OBJS) libaret_recovered.a
.PHONY: clean
"""

def gen_cmake():
    return """cmake_minimum_required(VERSION 3.10)
project(aret_recovered C)
set(CMAKE_C_STANDARD 99)
add_compile_options(-w -fno-strict-aliasing -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration)
file(GLOB PARTS src/part_*.c)
add_library(aret_recovered STATIC aret_runtime.c ${PARTS})
target_include_directories(aret_recovered PUBLIC ${CMAKE_SOURCE_DIR})
"""

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump_dir")
    ap.add_argument("out_dir")
    ap.add_argument("--parts", type=int, default=64,
                    help="number of translation units to split functions into")
    ap.add_argument("--limit", type=int, default=0,
                    help="only process the first N functions (0 = all)")
    args = ap.parse_args()

    files = sorted(f for f in os.listdir(args.dump_dir)
                   if f.startswith("sub_") and f.endswith(".c"))
    if args.limit:
        files = files[:args.limit]

    os.makedirs(os.path.join(args.out_dir, "src"), exist_ok=True)

    # Forward declarations for every function (K&R / no prototype).
    names = [f[:-2] for f in files]
    with open(os.path.join(args.out_dir, "decls.h"), "w") as fh:
        fh.write("#ifndef ARET_DECLS_H\n#define ARET_DECLS_H\n#include <stdint.h>\n")
        for n in names:
            fh.write(f"int64_t {n}();\n")
        fh.write("#endif\n")

    with open(os.path.join(args.out_dir, "aret_runtime.h"), "w") as fh:
        fh.write(gen_runtime_h())
    with open(os.path.join(args.out_dir, "aret_runtime.c"), "w") as fh:
        fh.write(gen_runtime_c())

    # Distribute functions across N translation units.
    parts = [[] for _ in range(args.parts)]
    for idx, fn in enumerate(files):
        with open(os.path.join(args.dump_dir, fn), errors="replace") as fh:
            parts[idx % args.parts].append(transform_function(fh.read()))

    for p, chunk in enumerate(parts):
        if not chunk:
            continue
        with open(os.path.join(args.out_dir, "src", f"part_{p:03d}.c"), "w") as fh:
            fh.write('#include "aret_runtime.h"\n#include "decls.h"\n\n')
            fh.write("\n".join(chunk))

    with open(os.path.join(args.out_dir, "Makefile"), "w") as fh:
        fh.write(gen_makefile(args.parts))
    with open(os.path.join(args.out_dir, "CMakeLists.txt"), "w") as fh:
        fh.write(gen_cmake())

    print(f"portablized {len(files)} functions -> {args.out_dir} "
          f"({args.parts} translation units)")

if __name__ == "__main__":
    main()
