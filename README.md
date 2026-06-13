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
  construction with proper leader detection.
- **IR lifting** (`src/ir`): local semantic translation of instructions into C
  (`mov`→`=`, `add`→`+=`, `lea`→address, memory operands→typed dereferences),
  plus **branch-condition recovery** (a `cmp`/`test` + `jcc` pair becomes a real
  relational expression like `eax <= 1`, with correct signed/unsigned choice).
- **Decompiler** (`src/decompile`): emits pseudo-C per function, using recovered
  conditions and `goto` for control flow it cannot yet reduce.

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
    // flags = cmp edi, 1
    if ((int64_t)edi <= (int64_t)1) goto L_00401159;
    edi += 1;
    eax = 2;
    edx = 1;
L_0040114c:
    edx *= eax;
    eax += 1;
    // flags = cmp eax, edi
    if (eax != edi) goto L_0040114c;
L_00401156:
    eax = edx;
    return rax;
L_00401159:
    edx = 1;
    goto L_00401156;
}
```

The loop and both conditions are recovered correctly from raw machine code.

## Usage

```bash
cargo build --release

aret <binary>                       # pseudo-C (default)
aret <binary> --mode info           # format, arch, sections, symbols
aret <binary> --mode asm            # disassembly listing
aret <binary> --mode cfg            # control-flow graph + call edges
aret <binary> --function <name|hex> # restrict to one function
aret <binary> -o out.c              # write to a file
```

## Roadmap (the honest path to "real and powerful")

These are the layers that turn the goto-based output into structured code, in
priority order:

1. **SSA-based IR** with register/stack-slot tracking, so values flow across
   instructions instead of literal register names.
2. **Control-flow structuring** (dominator/interval analysis) to recover
   `if/else`, `while`, `for`, and `switch` and eliminate the `goto`s.
3. **Dataflow**: dead-code elimination, expression propagation, constant
   folding — collapses the verbose per-instruction output into real statements.
4. **Type & variable recovery**: stack-frame reconstruction, calling-convention
   analysis to recover function arguments, and conservative type inference.
5. **Library/CRT signature matching** (FLIRT-style) to name known functions and
   skip runtime boilerplate.

## License

MIT
