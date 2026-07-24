# ARET — Automatic Reverse Engineering Toolkit

ARET is a reverse-engineering and binary-translation toolkit written in Rust. It
takes a 32-bit Windows PE executable and produces a **native, standalone Linux
ELF or WebAssembly module** that runs directly on the host CPU — with no
emulator, no CPU virtualization, and no Wine at runtime. It also works as a
classic decompiler, emitting readable pseudo-C.

```
Windows PE ──▶ lift x86 to typed SSA IR ──▶ optimise ──▶ C / LLVM backend ──┐
            +  Win32 & CRT re-implemented natively (HLE, statically linked) ─┴─▶ native ELF / WASM
```

The project started as a decompiler and grew into a **Universal Binary
Transpiler (UBT)**: the aim is to take software compiled for one platform and
make it run, fully functional and fully native, on another.

## Table of contents

- [Overview](#overview)
- [Features](#features)
- [How it works](#how-it-works)
- [Verification](#verification)
- [Proven results](#proven-results)
- [Building](#building)
- [Usage](#usage)
- [Project status](#project-status)
- [Roadmap](#roadmap)
- [Documentation](#documentation)
- [License](#license)

## Overview

Most tools that "run a Windows program on Linux" either emulate the CPU
(qemu-style) or keep a compatibility runtime in the loop (Wine). ARET does
neither: it **translates the machine code to C**, **re-implements the operating
system calls in a native shim layer**, and **recompiles** the result into an
ordinary native binary. The output has no dependency on ARET, on Wine, or on any
DLL — it is a normal ELF (or `.wasm`).

The core design constraint is soundness. ARET never emits a result it cannot
justify: every mechanism is either verified against an independent oracle, or it
**aborts with a named message** at the point where it would otherwise have to
guess. See [Design principles](#design-principles).

## Features

- **PE → native ELF** transpilation (32-bit x86), and **PE → WebAssembly** from
  the same lift.
- **Decompiler mode**: machine code → structured pseudo-C (`if`/`while`, with
  `goto` fallback) or flat form.
- **Function & CFG recovery** on stripped binaries (prologue scanning,
  address-taken analysis, jump/pointer tables, FLIRT signatures), scaling to
  large binaries (a 27 MB game → ~43k functions).
- **Native HLE runtime** re-implementing kernel32 / msvcrt / user32 / gdi32 /
  comctl32 in C, statically linked into the output.
- **Cooperative threads** (real fibers), **structured exception handling**
  (SEH), and a **GUI stack** (SDL2 windows + FreeType text rendering).
- **System-DLL lifting**: a real DLL (e.g. `comctl32.dll`) can be lifted through
  the same pipeline as the app.
- A **differential verification suite** (Unicorn, Wine, Z3) that gates every
  change.

## How it works

### Pipeline

| Stage | Crate | What it does |
|---|---|---|
| Load | `src/loader` | Parse PE/ELF/Mach-O; multi-module loader for DLL lifting; expose a uniform memory view. |
| Analyse | `src/analysis` | Recover functions and control-flow graphs; jump/pointer tables; FLIRT matching. |
| Lift | `src/ir/lift` | Translate each x86 instruction to a typed SSA IR with explicit CPU flags. |
| Model | `src/ir/build` | The shared-stack call model (see below), callee-pop (`ret N`) handling, frame recovery. |
| Optimise | `src/ssa`, `src/opt` | SSA construction, constant propagation, DCE, expression propagation. |
| Emit | `src/emit` | C (`structured.rs`) or LLVM (`llvm.rs`) backend; WASM is produced via the C backend. |
| Recompile | `src/builder` | Compile the generated C + the HLE runtime into a native ELF / WASM. |

### The shared-stack model

Lifted functions receive the machine stack pointer `esp` **by value**; the
machine stack is a single shared region, and `ebp` is threaded as an extra
callee-saved parameter. This lets arguments cross function calls whether they
were passed on the stack (cdecl/stdcall) or in registers (regparm/fastcall),
without reconstructing high-level signatures. It also keeps the model
thread-safe, which is what makes the cooperative-fiber threading sound.

### The HLE runtime

Instead of reimplementing Windows from scratch or shipping Wine, ARET provides a
native high-level-emulation layer (`runtime/aret_hle/`) that implements the
Windows/CRT surface a program actually uses:

- **kernel32 / msvcrt**: files and paths (Windows→POSIX translation),
  `printf`/`scanf` (including `%I64`), heap, locale & codepage, an in-memory
  registry, time, `setjmp`/`longjmp`.
- **Threads**: `CreateThread` and friends implemented as cooperative fibers
  (`ucontext`), with critical sections, events, mutexes, semaphores and
  per-fiber TLS. Scheduling is deterministic round-robin, which keeps the
  differential oracle reproducible.
- **SEH**: `RaiseException`, `RtlUnwind`, and hardware faults (SIGSEGV/SIGFPE)
  dispatched through the real `fs:[0]` exception chain.
- **GUI**: USER32/GDI with visible SDL2 windows, a `WM_PAINT` model, and text
  rendered with FreeType (the same rasteriser Wine uses, so glyphs are
  pixel-identical). Native dialogs and standard controls paint on screen.
- **DLL lifting** (`--with-dll`): a system DLL is lifted through the same
  pipeline as the app; its exported logic becomes verified lifted code running
  on top of the HLE. Real `comctl32` controls (progress bar, image list) run
  this way.

### Design principles

- **Correct or loud abort.** Anything not provably modelled stays an explicit
  `Asm`/abort — the program halts with a named message (`aret_unmodelled(...)`)
  instead of producing a wrong result. There are no silent no-ops.
- **General fixes, not per-binary hacks.** A bug is treated as a class of
  binaries and fixed at the general cause, verified across that class.
- **Nothing proven ⇒ nothing guessed.** Where a property (x87 rounding, stack
  depth, `noreturn`, an indirect-call target) cannot be proven, ARET takes a
  sound fallback rather than an optimistic assumption.

The practical guarantee is: *functional, or an abort that tells you where —
never wrong in silence.*

## Verification

Correctness is enforced by a suite of differential oracles. An unmodelled path
makes an oracle **skip** rather than pass, so a match is a proven correction and
a divergence is a proven bug. `bench/regression.sh` runs the gate on every
change.

| Oracle | Command | Proves |
|---|---|---|
| cpudiff (Unicorn) | `cargo test --features unpack cpudiff` | each lifted instruction matches a real CPU (registers, flags, memory) |
| funcdiff (Unicorn) | `bench/funcdiff.sh` | whole-function lift + that SSA/opt passes preserve semantics (0 divergence, ~20.6k functions scored) |
| difftest | `bench/difftest.sh` | the decompile pipeline, O0→O3 (272/272) |
| difftest_transpile | `bench/difftest_transpile.sh` | the transpile pipeline is byte-stable (behavioural hash) |
| winediff | `bench/winediff.sh` | OS-API behaviour bit-for-bit vs Wine (169/169) |
| DIB-hash / Xvfb | in `winediff` | GDI primitives are pixel-exact; the window→GDI→SDL pipeline composes on screen |
| SMT | `bench/smt_rewrites.sh` | rewrite-rule equivalence (Z3) |

Wine is used strictly as **ground truth**, never as a runtime dependency — a PE
under Wine executes real i386 on the CPU, so its output is a faithful oracle for
what the native ELF must reproduce.

## Proven results

Each of these is a real third-party binary transpiled to a native Linux ELF and
checked output-for-output against the same PE under Wine:

| Binary | Toolchain | Result |
|---|---|---|
| **Lua 5.4.7** (650 KB, symboled and stripped) | mingw | 35/35 subsystems (closures, coroutines, metatables, patterns, `table.sort`, `pcall`, GC stress, …) |
| **sqlite3.exe** (stripped, 2958 fns) | MSVC | full SQL engine, sweep 30/30 (CRUD, JOIN, GROUP BY, window functions, CTE, JSON, triggers) |
| **NASM 2.16.01** (1.5 MB, stripped) | MSVC | `-f elf`/`win32`/`bin`/`obj` output bit-identical to Wine |
| **busybox-w32** (stripped) | mingw | sweep 60/60 (`cksum`, `md5sum`, `grep`, `sed`, `awk`, `sort`, …) |
| **strings.exe** (Sysinternals) | MSVC static-CRT C++ | 100% bit-identical to Wine, version banner included |

A committed gauntlet of 21 varied PEs (`bench/gauntlet/`) is at 21/21 functional,
and the WebAssembly target passes 7/7 fixtures, proving the same lift retargets
to a different backend.

## Building

```bash
# Requires a Rust toolchain and a 32-bit native toolchain (for the stack model):
sudo apt-get install -y gcc-multilib g++-multilib
cargo build --release
```

Optional, for the full test matrix:

- `gcc-mingw-w64-i686` — build the Windows PE fixtures.
- a 32-bit Wine (`wine`, `wine32:i386`) — run `winediff` against ground truth.
- `libsdl2-dev:i386`, `libfreetype`, `fontconfig`, `Xvfb` — the GUI fixtures.
- `z3` — the SMT oracle.

## Usage

```bash
# Transpile a Windows PE to a native ELF and run it:
aret program.exe --mode transpile --out-dir out/ --run

# Retarget the same lift to WebAssembly:
aret program.exe --mode transpile --target wasm --out-dir out/ --run

# Lift a system DLL alongside the application:
aret app.exe --with-dll comctl32.dll=/path/to/comctl32.dll --mode transpile --run

# Static coverage map (unmodelled instructions, missing imports) before running:
aret program.exe --mode walls

# Decompiler mode: machine code → pseudo-C
aret program.exe                 # structured (if/while); add --flat for goto form
aret program.exe --mode asm      # disassembly
aret program.exe --mode cfg      # control-flow graph + call edges
aret program.exe --function <name|hex>   # restrict to one function
```

## Project status

**Working today:** the transpile pipeline (PE → native ELF, and → WASM); the
decompiler; function/CFG recovery on stripped binaries; the HLE surface for
console programs (kernel32/CRT/files); cooperative threads; SEH; and the
demonstrators above, all verified bit-identical to Wine.

**In progress:** the GUI stack. Message-only USER32, GDI (vector + raster +
FreeType text) and dialogs are bit-exact vs Wine; visible SDL2 windows and native
controls paint on screen; system-DLL lifting runs real `comctl32` controls. The
current work is making a lifted `comctl32` control paint end-to-end on screen —
which surfaced a real lifter bug (indirect calls through an IAT-loaded register
mis-resolving their target/pop) now being fixed. Because the project only ships
verified mechanisms, unfinished areas abort loudly rather than misbehave.

The single source of truth for detailed state is
[`docs/vision/70-reference-etat-methode-reste.md`](docs/vision/70-reference-etat-methode-reste.md).

## Roadmap

- **GUI / graphics (M7):** finish native-control painting; then games via DXVK /
  vkd3d (D3D → Vulkan) with the game's own code lifted natively.
- **Type inference:** width/sign/pointer and aggregate recovery, for readability
  (never at the cost of semantics).
- **64-bit lift:** extend the lifter/model to x86-64 (REX, 64-bit ABIs), which
  also unlocks ARM ELF output via the existing LLVM backend.
- **Windows re-targeting:** a PE backend so an old 32-bit binary becomes a
  self-contained modern PE, and (longer term) a 16-bit NE frontend so legacy
  16-bit software runs natively on 64-bit Windows.
- **macOS:** Mach-O loading/emitting and a minimal macOS HLE.

Undecidability sets a hard limit: *"any binary + 100% functional + 100% pure
native"* is impossible in general. What ARET targets is genuinely compiled
software → fully functional native; the impossible residue (obfuscated /
hand-crafted / VM-packed) signals itself with an abort instead of lying.

## Documentation

- [`docs/vision/70-reference-etat-methode-reste.md`](docs/vision/70-reference-etat-methode-reste.md)
  — reference: state, method, rules, roadmap.
- [`docs/vision/80-orientations-architecturales.md`](docs/vision/80-orientations-architecturales.md)
  — design of the large in-flight work (fibers, DLL lifting, SEH).
- [`docs/vision/71-journal-de-bord.md`](docs/vision/71-journal-de-bord.md)
  — searchable engineering journal.
- [`ROADMAP.md`](ROADMAP.md) — the typed-SSA/decompiler technical plan.

## License

MIT
