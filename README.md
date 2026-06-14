# ARET — Automatic Reverse Engineering Toolkit

A reverse-engineering pipeline in Rust that takes a binary, disassembles it,
recovers its functions and control-flow graphs, lifts the machine code, and
emits readable **pseudo-C**.

```
binary  →  load (PE/ELF/Mach-O)  →  disassemble (x86/x64)  →
        →  function & CFG recovery  →  IR lifting  →  pseudo-C
```

## Honest scope (read this first)

Marketing claims of "perfectly turning any binary back into its original source
code" are not physically possible, and this project does not pretend otherwise:

- **Compilation is lossy.** Variable names, comments, types, class layouts and
  template structure are *discarded* by the compiler. No tool can recover them —
  the best any decompiler (including Ghidra/IDA Hex-Rays) can do is *invent*
  plausible replacements. The information is simply not in the bytes anymore.
- **Some sub-problems are undecidable** in the formal sense (perfectly
  separating code from data, recovering exact function boundaries on optimized
  or obfuscated code). Every tool relies on heuristics, not certainties.

So ARET aims to be a *genuinely functional* reverse-engineering pipeline with a
clean architecture you can extend — not a magic "press button, get source."
What it produces is real per-instruction semantics on a faithful CFG, which is
exactly what a young decompiler's output looks like before full structuring.

## What works today

- **Loader** (`src/loader`): parses PE / ELF / Mach-O via the `object` crate;
  extracts sections, entry point, and symbols; exposes a uniform memory view.
- **Disassembler** (`src/disasm`): decodes x86 / x86-64 with `iced-x86` and
  classifies each instruction's control flow.
- **Analysis** (`src/analysis`): recursive-descent **function discovery** from
  the entry point + symbols + call targets, plus **prologue scanning** to
  recover functions reached only indirectly (vtables/callbacks), then
  **basic-block / CFG** construction. Scales via a single global decode pass
  (27 MB game binary → 43k functions / 3M instructions in ~50 s; `--no-prologue-scan`
  for the ~5 s directly-called-only subset).
- **IR lifting** (`src/ir`): local semantic translation of instructions into C
  (`mov`→`=`, `add`→`+=`, `lea`→address, memory operands→typed dereferences),
  **branch-condition recovery** (`cmp`/`test` + `jcc` → `eax <= 1`, correct
  signed/unsigned), and **frame-variable recovery** (`[ebp+8]`→`arg_8`,
  `[ebp-4]`→`local_4`, with recovered argument lists and local declarations).
- **Structuring** (`src/structure`): dominator + post-dominator analysis and
  natural-loop detection drive a recursive emitter that produces `if`/`else`
  and `while` loops. Edges it cannot reduce degrade to explicit `goto`, so the
  output is always semantically faithful. (Cut gotos by ~80% on the game.)
- **Dataflow** (`src/dataflow`): global **constant propagation** (a meet-based
  forward analysis that is exact across branch joins), global register
  **liveness**, **dead-assignment elimination**, **single-use expression
  propagation** (also across straight-line block chains), and **call-result
  binding** (`f(args)` → `eax = f(args)` when the result is used). All gated on
  provable safety — constants track exact register names with family-aware
  aliasing and call-clobber invalidation, so a value is only substituted when
  every path agrees on it.
- **Decompiler** (`src/decompile`): the flat goto-based emitter (`--flat`),
  plus shared per-block lifting used by the structured emitter.

## Example

Source:

```c
int factorial(int n) {
    int result = 1;
    for (int i = 2; i <= n; i++) result *= i;
    return result;
}
```

`aret demo --function factorial` (from the *stripped of source* binary):

```c
int64_t factorial(void) {
    if ((int64_t)edi <= (int64_t)1) {
        edx = 1;
    } else {
        edi += 1;
        eax = 2;
        edx = 1;
        while (true) {
            edx *= eax;          // result *= i
            eax += 1;            // i++
            if (!(eax != edi)) break;
        }
    }
    eax = edx;
    return rax;
}
```

The loop, the `if`/`else`, and both conditions are recovered from raw machine
code. Use `--flat` for the lower-level goto-based form.


### Measured results (north-star metric)

Recompilability (level 1) and differential equivalence (level 2) on real code:

| Binary | Recompiled |
|---|---|
| gzip (ELF, stripped) | 131/131 (100%) |
| ls / cat / sha256sum / base64 | 100% |
| MightyQuest.exe (27 MB game) | 100% (sampled) |

Differential equivalence (recompiled vs original on random inputs): **16/16**
corpus functions, including pointer/array/loop/string code. Z3 is available in
this environment (`pip install z3-solver`) for the planned level-3 SMT proofs.

## Usage

```bash
cargo build --release

aret <binary>                       # structured pseudo-C (default)
aret <binary> --flat                # flat goto-based pseudo-C
aret <binary> --mode info           # format, arch, sections, symbols
aret <binary> --mode asm            # disassembly listing
aret <binary> --mode cfg            # control-flow graph + call edges
aret <binary> --function <name|hex> # restrict to one function
aret <binary> -o out.c              # write to a file
aret <binary> --split out_dir/      # one .c per function + index.csv
```

## Roadmap (the honest path to "real and powerful")

Done: control-flow structuring (`if`/`while`), frame-variable recovery
(args/locals), branch-condition recovery, cdecl call-site argument recovery,
prologue/epilogue cleanup, prologue-scan coverage, large-binary scaling,
global liveness + dead-code elimination + single-use expression propagation.

### Next: a typed SSA IR

See [`ROADMAP.md`](ROADMAP.md) for the full technical plan (typed SSA IR →
optimisation passes → type inference → high-level construct recovery →
compilable C → a verified recompile/equivalence loop → an LLM naming layer).

The central insight from that review: the current text-based IR is the
architectural ceiling — real constant folding, propagation across branch joins,
and type inference all need an expression-tree + SSA IR. That migration has
started, **in parallel** with the working text pipeline (so nothing regresses):

- `src/cfg/dom.rs` — shared dominators / post-dominators / **dominance
  frontiers** (Cytron), for φ-node placement; the structurer now uses it.
- `src/ir/types.rs` — the typed SSA IR (`Expr`/`Stmt`/`Ty`/`Location`/`ValueId`,
  explicit CPU flags), the foundation the rest of the roadmap builds on.

- `src/ir/lift.rs` — lifts instructions to typed IR via `iced-x86`'s structured
  operand API, with explicit flag definitions (`Stmt::Asm` fallback for the
  rest, so output is never silently wrong).
- `src/ssa/mod.rs` — SSA construction (Cytron φ-placement + renaming).
- `src/ir/build.rs` — builds the IR CFG from a recovered function and runs the
  whole chain; inspect it with `aret <binary> --mode ir --function <name>`
  (machine code → typed SSA IR with φ-nodes, verified on real binaries).

Immediate next steps: SCCP (sparse conditional constant propagation + dead
branch elimination) and DCE on the SSA, then recovering branch conditions from
the flag definitions, then an IR→C emitter at parity with the text pipeline.
4. **`switch`/jump-table recovery** and full indirect-call resolution via
   vtable analysis (names the indirect call sites, not just the targets).
5. **Library/CRT signature matching** (FLIRT-style) to name known functions and
   skip runtime boilerplate.

## License

MIT
