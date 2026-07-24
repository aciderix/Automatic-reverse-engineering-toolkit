# ARET — Automatic Reverse Engineering Toolkit

**ARET turns a 32-bit Windows PE executable into a *native* standalone Linux ELF
or WebAssembly module — no emulator, no Wine at runtime, no CPU emulation.** The
foreign binary's x86 machine code is lifted to C, its Windows API calls are
re-implemented in a native high-level-emulation layer, and the whole thing is
recompiled to a real native binary that runs directly on the host CPU.

```
Windows PE  →  lift x86 → typed SSA IR → optimise → C/LLVM  →  recompile
             +  Win32/CRT re-implemented natively (HLE)      →  native ELF / WASM
```

It began as a decompiler (machine code → readable pseudo-C, still supported) and
grew into a **Universal Binary Transpiler**: the goal is to take software built
for one system and make it run, fully functional and fully native, on another.

---

## The one rule that defines the project

> **Never present a wrong result as correct. Be right, or stop loudly.**

Every mechanism is either **proven correct against an independent oracle**, or it
**aborts with a named message** (`aret_unmodelled("…")`) at the exact point it
would otherwise guess. There are no silent no-ops, no per-binary hacks, and
nothing is emitted on a hunch:

- **Correct or loud abort.** Anything not provably modelled stays an explicit
  `Asm`/abort — the program halts and tells you where, instead of drifting.
- **Fix the general cause, never the single binary.** A bug is a class of
  binaries, and the fix is verified across that class.
- **Nothing proven ⇒ nothing guessed.** x87 rounding mode, stack depth,
  `noreturn`, indirect-call targets: if it isn't proven, ARET takes the sound
  fallback (a runtime FPU net, an abort), never the optimistic assumption.

This makes the honest guarantee *"functional, **or** an abort that says where —
never wrong in silence."* Genuinely compiled software (compilers, databases,
interpreters, games) is fully reachable; the theoretically-impossible residue
(hand-crafted / obfuscated / VM-packed code) **flags itself** rather than lying.

---

## Proven results (bit-identical to Wine, natively recompiled)

Every demonstrator below is a real third-party binary, transpiled to a native
Linux ELF and checked **output-for-output against the same PE running under
Wine** (Wine is used only as ground truth — it is itself native i386, not an
emulator):

| Binary | Toolchain | Result |
|---|---|---|
| **Lua 5.4.7** (650 KB, symboled **and** stripped) | mingw | **35/35** subsystems: closures, metatables/OOP, coroutines, patterns, `table.sort`, `pcall`, 64-bit, varargs, `goto`, GC stress |
| **sqlite3.exe** (stripped, 2958 fns) | MSVC | Full SQL engine, sweep **30/30** (`:memory:` + on-disk): CRUD, JOIN, GROUP BY, window functions, CTE, index, JSON, triggers |
| **NASM 2.16.01** (1.5 MB, stripped) | MSVC | `-f elf` / `-f win32` / `-f bin` / `-f obj` objects **bit-identical to Wine** |
| **busybox-w32** (stripped) | mingw | sweep **60/60**: `cksum`/`md5sum`/`sha1sum`/`sort`/`grep`/`sed`/`awk`/`tr`/… |
| **strings.exe** (Sysinternals, static-CRT C++) | MSVC | **100% bit-identical to Wine**, version banner included |

Plus a committed **gauntlet** of 21 varied PEs (`bench/gauntlet/`) at 21/21
functional, and a **WebAssembly** target (PE → WASM, 7/7 fixtures) proving the
same lift retargets to a genuinely different backend.

---

## How correctness is enforced — the differential oracle suite

ARET's credibility is its verification layer. Nothing ships without an oracle,
and an unmodelled path makes the oracle *skip* (never a false pass):

| Oracle | Proves | 
|---|---|
| **cpudiff** (Unicorn) | each lifted instruction matches a real CPU (registers + flags + memory) over thousands of states; plus generative 2–3-instruction sequences |
| **funcdiff** (Unicorn) | a whole recovered function's lift, and that the SSA + optimisation passes preserve semantics — **0 divergence** across ~20.6k scored functions |
| **difftest** | the decompile pipeline, O0→O3 (**272/272**) |
| **difftest_transpile** | the transpile pipeline is a byte-stable product (a behavioural hash) |
| **winediff** | OS-API behaviour bit-for-bit vs Wine (**169/169**) |
| **DIB-hash / Xvfb** | GDI primitives are pixel-exact vs Wine; the window→GDI→SDL pipeline composes on a real screen |
| **Z3 / SMT** | rewrite-rule equivalence |

A divergence is a *proven* bug; a match is a *proven* correction. This is what
lets the project move fast without regressing — `bench/regression.sh` gates every
change.

---

## What the native runtime covers

The high-level-emulation layer (`runtime/aret_hle/`) re-implements the Windows
surface natively, in C, statically linked into the output (so the result needs
no Wine, no DLLs):

- **CPU**: full x86-32 integer + flags (width-aware), x87 FPU (static depth pass
  **and** a sound runtime FPU net), SSE/SSE2 scalar + packed, string ops.
- **kernel32 / msvcrt**: files, `printf`/`scanf` (incl. `%I64`), heap, locale &
  codepage, an in-memory registry, time, `setjmp`/`longjmp`.
- **Threads** — real **cooperative fibers** (`ucontext`): `CreateThread`,
  critical sections, events, mutexes, semaphores, per-fiber TLS, deterministic
  round-robin scheduling (so the differential oracle stays valid). No data races
  by construction; WASM aborts soundly where `ucontext` is absent.
- **SEH**: `RaiseException`, `RtlUnwind`, and hardware faults (SIGSEGV→dispatch)
  routed through the real `fs:[0]` chain.
- **GUI**: USER32/GDI with **visible SDL2 windows**, a `WM_PAINT` model, and
  **FreeType text rendering that is pixel-identical to Wine** (Wine rasterises
  with FreeType too). Native dialogs and controls paint on screen.
- **DLL lifting** (`--with-dll`): a real system DLL (e.g. Wine's `comctl32.dll`)
  is lifted through the *same* pipeline as the app — its controls are proven
  code (cpudiff/funcdiff) on top of the HLE gdi32, the purest form of the
  doctrine. Real comctl32 controls (progress bar, image list) run bit-identical
  to Wine.

---

## Usage

```bash
cargo build --release        # needs gcc-multilib (32-bit) to build the stack model

# Transpile a Windows PE to a native ELF and run it:
aret program.exe --mode transpile --out-dir out/ --run

# Retarget the same lift to WebAssembly:
aret program.exe --mode transpile --target wasm --out-dir out/ --run

# Lift a system DLL alongside the app:
aret app.exe --with-dll comctl32.dll=/path/to/comctl32.dll --mode transpile --run

# Static coverage map before running (unmodelled instructions / missing imports):
aret program.exe --mode walls

# Decompiler mode (machine code → readable pseudo-C):
aret program.exe            # structured (if/while); --flat for goto form
aret program.exe --mode asm | --mode cfg | --function <name|hex>
```

Verification harnesses live in `bench/` (`regression.sh` is the unified gate;
`winediff.sh`, `funcdiff.sh`, `difftest*.sh`, `sqlite_sweep.sh`, … the
per-axis ones). Building the PE fixtures and running `winediff` needs
`gcc-mingw-w64-i686` and a 32-bit Wine.

---

## Architecture

```
src/loader      PE/ELF/Mach-O parsing, multi-module loader (DLL lifting)
src/analysis    function & CFG recovery, jump/pointer tables, FLIRT
src/ir/lift     per-instruction x86 → typed SSA IR (explicit CPU flags)
src/ir/build    shared-stack call model (esp by value, ebp threaded), callee-pops
src/ssa,opt     SSA construction + optimisation passes
src/emit        C (structured.rs) and LLVM (llvm.rs) backends; WASM via C
src/cpudiff     Unicorn-backed differential oracles
runtime/aret_hle  the native HLE: aret_hle.c / aret_crt.c / aret_win32.c
```

The design, methodology, and full state live in
[`docs/vision/`](docs/vision/) — start with
[`70-reference-etat-methode-reste.md`](docs/vision/70-reference-etat-methode-reste.md)
(the single reference document) and
[`80-orientations-architecturales.md`](docs/vision/80-orientations-architecturales.md)
(design of the large in-flight work: fibers, DLL lifting, SEH).

---

## The honest hard limit

The triple *"**any** binary + 100% functional + 100% pure native"* is provably
impossible in general (undecidability — the halting family). What ARET actually
achieves is: **genuinely compiled software → fully functional native**, and the
theoretically-impossible residue **signals itself** with a named abort. The
finality and the guarantee are unchanged whether a mechanism is hand-written or a
reused, verified brick (Wine as oracle, Unicorn, LLVM) — those only change how
fast we get there, never the soundness contract.

## License

MIT
