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
  the entry point + symbols + call targets, then **basic-block / CFG**
  construction. Scales via a single global decode pass (a 27 MB game binary →
  5781 functions / 795k instructions in ~5 s).
- **IR lifting** (`src/ir`): local semantic translation of instructions into C
  (`mov`→`=`, `add`→`+=`, `lea`→address, memory operands→typed dereferences),
  **branch-condition recovery** (`cmp`/`test` + `jcc` → `eax <= 1`, correct
  signed/unsigned), and **frame-variable recovery** (`[ebp+8]`→`arg_8`,
  `[ebp-4]`→`local_4`, with recovered argument lists and local declarations).
- **Structuring** (`src/structure`): dominator + post-dominator analysis and
  natural-loop detection drive a recursive emitter that produces `if`/`else`
  and `while` loops. Edges it cannot reduce degrade to explicit `goto`, so the
  output is always semantically faithful. (Cut gotos by ~80% on the game.)
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
(args/locals), branch-condition recovery, large-binary scaling.

Remaining, in priority order:

1. **SSA-based IR** with register/stack-slot tracking, so values flow across
   instructions instead of literal register names (enables the cleanups below).
2. **Dataflow**: dead-code elimination, expression propagation, constant
   folding — collapses the verbose per-instruction output (and prologue/epilogue
   `push`/`pop` noise) into real statements.
3. **Argument-count recovery at call sites** and conservative **type inference**
   beyond the current width-based types.
4. **`switch`/jump-table recovery** and indirect-call resolution via vtable
   analysis (reaches the functions only called indirectly — most of a C++ game).
5. **Library/CRT signature matching** (FLIRT-style) to name known functions and
   skip runtime boilerplate.

## License

MIT
