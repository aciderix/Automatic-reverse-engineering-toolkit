//! Control-flow analysis: function discovery (recursive descent) and
//! basic-block / CFG construction.
//!
//! Strategy for scaling to large binaries: decode every reachable address
//! exactly once into a global instruction map (Phase A), then partition that
//! map into functions with cheap map lookups (Phase B). This keeps the whole
//! pipeline ~O(code reached) instead of O(functions × size).

use crate::disasm::{Disassembler, Flow, Insn};
use crate::loader::Program;
use indexmap::IndexMap;
use std::collections::{BTreeMap, BTreeSet, HashMap, VecDeque};

/// A maximal straight-line run of instructions with a single entry and exit.
#[derive(Debug, Clone)]
pub struct BasicBlock {
    pub start: u64,
    pub insns: Vec<Insn>,
    /// Start addresses of successor blocks (within this function).
    pub successors: Vec<u64>,
    pub terminator: Flow,
}

impl BasicBlock {
    pub fn end(&self) -> u64 {
        self.insns.last().map(|i| i.next_addr()).unwrap_or(self.start)
    }
}

/// A recovered function: an entry point and its basic blocks in address order.
#[derive(Debug, Clone)]
pub struct Function {
    pub entry: u64,
    pub name: String,
    /// Blocks keyed by start address, preserving address order.
    pub blocks: IndexMap<u64, BasicBlock>,
    /// Direct call targets observed in the body (callees).
    pub callees: BTreeSet<u64>,
}

/// Result of analysing a whole program.
pub struct AnalysisResult {
    pub functions: Vec<Function>,
    /// Total instructions decoded across the program.
    pub instruction_count: usize,
}

/// Transitive closure of direct-call callees reachable from `root` (inclusive),
/// over the recovered `functions` — the set of functions that must be transpiled
/// to run `root` standalone (Phase 2, targeted conversion / `--function`).
///
/// Only *direct* calls (the `callees` set) are followed. Code reached solely
/// through an indirect call / vtable / callback is deliberately not pulled in:
/// such a call is left to fail loud at runtime (`aret_call` → unmodelled) rather
/// than silently guessing a target — consistent with the sacred principle. The
/// returned set may contain callee addresses that are imports or unrecovered
/// (not present as a `Function`); the caller filters to real functions.
pub fn reachable_closure(functions: &[Function], root: u64) -> BTreeSet<u64> {
    let by_entry: HashMap<u64, &Function> = functions.iter().map(|f| (f.entry, f)).collect();
    let mut seen = BTreeSet::new();
    let mut stack = vec![root];
    while let Some(e) = stack.pop() {
        if !seen.insert(e) {
            continue;
        }
        if let Some(f) = by_entry.get(&e) {
            for &c in &f.callees {
                if !seen.contains(&c) {
                    stack.push(c);
                }
            }
        }
    }
    seen
}

/// Locate `main` in a stripped CRT binary by the call the startup makes to it.
///
/// The mingw/MSVC C-runtime startup (`__tmainCRTStartup`) sets up the cdecl
/// arguments `argc`/`argv`/`envp` and then `call main`, storing the result as the
/// process exit code. We look — only inside functions reachable from the entry
/// point, to avoid matching an ordinary 3-argument call elsewhere — for a direct
/// `call` whose target is a recovered, *non-library* function and which is
/// immediately preceded by stores of the cdecl arguments to `[esp]`, `[esp+4]`
/// (and usually `[esp+8]`). That target is `main`. Returns its address.
pub fn find_main(prog: &Program, result: &AnalysisResult) -> Option<u64> {
    use iced_x86::{Mnemonic, OpKind, Register};

    let by_entry: HashMap<u64, &Function> = result.functions.iter().map(|f| (f.entry, f)).collect();

    // Functions reachable from the program entry (the startup chain).
    let mut reachable: BTreeSet<u64> = BTreeSet::new();
    let mut queue: VecDeque<u64> = VecDeque::new();
    queue.push_back(prog.entry);
    while let Some(a) = queue.pop_front() {
        if !reachable.insert(a) {
            continue;
        }
        if let Some(f) = by_entry.get(&a) {
            for &c in &f.callees {
                if !reachable.contains(&c) {
                    queue.push_back(c);
                }
            }
        }
        if reachable.len() > 64 {
            break; // the startup chain is small; don't wander the whole program
        }
    }

    let is_user_fn = |t: u64| -> bool {
        by_entry.contains_key(&t)
            && prog.import_name(t).is_none()
            && prog.crt_symbol(t).is_none()
            && !prog.is_startup_glue(t)
    };
    // A store `mov [esp+disp], _` with disp in {0,4,8} → an arg slot was written.
    let arg_slot = |insn: &iced_x86::Instruction| -> Option<u64> {
        if insn.mnemonic() == Mnemonic::Mov
            && insn.op0_kind() == OpKind::Memory
            && insn.memory_base() == Register::ESP
            && insn.memory_index() == Register::None
        {
            let d = insn.memory_displacement64();
            if d == 0 || d == 4 || d == 8 {
                return Some(d);
            }
        }
        None
    };
    // `eax` (the call's result) is the process exit status: the startup saves it
    // (`mov [mem], eax`) before tearing down. This separates the real `call main`
    // from the CRT helper calls a startup makes first (whose results feed back
    // into setup, e.g. an argv-parsing loop). Scan the few instructions after the
    // call, stopping once `eax` is clobbered or another call intervenes.
    let writes_eax = |insn: &iced_x86::Instruction| -> bool {
        insn.mnemonic() == Mnemonic::Call
            || (insn.op0_kind() == OpKind::Register
                && matches!(
                    insn.op0_register(),
                    Register::EAX | Register::AX | Register::AL | Register::AH | Register::RAX
                ))
    };
    let saves_result = |insns: &[Insn], call_i: usize| -> bool {
        for insn in insns.iter().skip(call_i + 1).take(5) {
            if insn.raw.mnemonic() == Mnemonic::Mov
                && insn.raw.op0_kind() == OpKind::Memory
                && insn.raw.op1_kind() == OpKind::Register
                && matches!(insn.raw.op1_register(), Register::EAX | Register::RAX)
            {
                return true; // result stored → exit-code save
            }
            if writes_eax(&insn.raw) {
                return false; // result consumed/clobbered before any store
            }
        }
        false
    };

    // The real `call main(argc, argv, …)` is the first call that both sets the
    // argument slots *and* saves its result as the exit code. The CRT helper
    // calls a startup makes first do not save their result, so they are rejected
    // — returning the wrong `main` would silently mistranslate the program. We
    // walk the startup chain in entry order (the startup sits at a low address);
    // None ⇒ caller falls back to an explicit `--entry <addr>` rather than a guess.
    for f in &result.functions {
        if !reachable.contains(&f.entry) {
            continue;
        }
        for block in f.blocks.values() {
            let mut slots: BTreeSet<u64> = BTreeSet::new();
            for (i, insn) in block.insns.iter().enumerate() {
                if let Some(d) = arg_slot(&insn.raw) {
                    slots.insert(d);
                    continue;
                }
                if matches!(insn.flow, Flow::Call) {
                    if let Some(t) = insn.target {
                        if slots.contains(&0)
                            && slots.contains(&4)
                            && is_user_fn(t)
                            && saves_result(&block.insns, i)
                        {
                            return Some(t);
                        }
                    }
                    slots.clear();
                }
                // Other instructions between the arg setup and the call are fine
                // (the idiom interleaves the argc/argv loads); only a call resets.
            }
        }
    }
    None
}

/// Conservatively choose `main` as the transpile entry when the program's own
/// entry is a CRT bootstrap (`*CRTStartup`) and a distinct `main`/`_main`
/// function symbol exists. Returns `None` for a freestanding binary (no such
/// symbol, or the entry already *is* `main`) so its entry is left untouched.
///
/// The native CRT startup (`__tmainCRTStartup` and friends) runs MSVC/mingw
/// runtime initialization we replace wholesale with the HLE; re-running it
/// reaches unmodelled internals. Starting at `main` (with a synthetic argc/argv
/// frame laid by the generated `main`) is the sound, documented default.
pub fn auto_main_entry(prog: &Program) -> Option<u64> {
    // The entry must look like a CRT bootstrap; otherwise leave it alone.
    let entry_is_bootstrap = prog
        .symbol_name(prog.entry)
        .is_some_and(|n| n.contains("CRTStartup"))
        || prog.is_startup_glue(prog.entry);
    if !entry_is_bootstrap {
        return None;
    }
    // A user `main` symbol distinct from the entry (mingw/MSVC decorate it
    // `main`/`_main`; never the `@`-suffixed stdcall freestanding form).
    prog.symbols
        .values()
        .find(|k| k.is_function && (k.name == "main" || k.name == "_main"))
        .map(|k| k.address)
        .filter(|&a| a != prog.entry)
}

/// Entry point: global decode, then build each function's CFG. When
/// `prologue_scan` is set, also recover functions reached only indirectly by
/// scanning executable sections for function prologues.
pub fn analyze(prog: &Program, disasm: &Disassembler, prologue_scan: bool) -> AnalysisResult {
    let (global, entries, jump_tables, prologue_only) = global_decode(prog, disasm, prologue_scan);
    let instruction_count = global.len();

    // Hot/cold splitting (gcc `-freorder-blocks-and-partition`) emits the cold
    // path as a companion symbol `foo.cold` in `.text.unlikely`. It is part of
    // `foo`, not a separate function: exclude such addresses from the function
    // list and from the boundary set so the parent collects them across its
    // `ja`/`jmp` into the cold region.
    let cold: BTreeSet<u64> = entries
        .iter()
        .copied()
        .filter(|&a| prog.symbol_name(a).is_some_and(|n| n.contains(".cold")))
        .collect();
    let boundary: BTreeSet<u64> = entries.difference(&cold).copied().collect();

    // Functions are independent (everything they read — `global`, `entries`,
    // `jump_tables`, `prog` — is shared read-only), so build them in parallel.
    use rayon::prelude::*;
    let entry_vec: Vec<u64> = boundary.iter().copied().collect();
    let mut functions: Vec<Function> = entry_vec
        .par_iter()
        .filter_map(|&entry| {
            build_function(prog, &global, entry, &boundary, &jump_tables, &prologue_only)
        })
        .collect();
    functions.sort_by_key(|f| f.entry);
    AnalysisResult {
        functions,
        instruction_count,
    }
}

/// Phase A — decode every reachable instruction once, collecting the set of
/// function entry points (entry + direct/indirect-target call sites).
/// A high-confidence guess that `addr` begins a function, by its opening bytes.
/// Used to filter address-taken candidates: the value already being a valid code
/// address is the primary signal, this is the secondary one. Kept deliberately
/// tight (common entry prologues only) to avoid seeding interior bytes as bogus
/// functions.
///
/// `allow_leaf` adds `mov reg,[esp+disp]` — a leaf function reading its first
/// stack argument. That shape is also how many switch *case* bodies begin, so it
/// is only safe for candidates sourced from a code *immediate* (a genuine
/// by-value callback like Lua's `_getS`), never from a data word, where a
/// jump-table's run of case-target pointers would otherwise be mistaken for
/// functions.
fn looks_like_func_start(prog: &Program, addr: u64, allow_leaf: bool) -> bool {
    // A function a library signature recognises (a statically-linked CRT routine
    // or startup glue) is unambiguously a real entry, whatever prologue it uses —
    // the strongest signal there is, and independent of the byte heuristics below.
    // Lets address-taken recovery seed e.g. `atexit(___do_global_dtors)` even when
    // the dtor runner starts with an unusual `mov eax,[abs];mov eax,[eax];test`.
    if prog.crt_symbol(addr).is_some() || prog.is_startup_glue(addr) {
        return true;
    }
    let Some(code) = prog.read_from(addr) else { return false };
    let b0 = code[0];
    let b1 = code.get(1).copied().unwrap_or(0);
    let b2 = code.get(2).copied().unwrap_or(0);
    matches!(b0, 0x55 | 0x53 | 0x56 | 0x57) // push ebp/ebx/esi/edi
        || b0 == 0xe9                         // jmp rel32 (tail-call thunk)
        || (b0 == 0x83 && b1 == 0xec)         // sub esp, imm8
        || (b0 == 0x81 && b1 == 0xec)         // sub esp, imm32
        || (b0 == 0x8b && b1 == 0xff)         // mov edi, edi (hot-patch pad)
        || (b0 == 0x89 && b1 == 0xff)
        || (b0 == 0xff && b1 == 0x25)         // jmp [mem] (import thunk)
        || (b0 == 0xff && b1 == 0x15)         // call [mem]; ret (import-call thunk /
                                              // address-taken callback wrapper)
        // Run-once init guard `mov eax,[moffs32]; test eax,eax` — the prologue of
        // a `_initterm`/local-static initializer (address-taken in a CRT
        // initializer table). The 7-byte signature is specific enough not to
        // seed interior bytes.
        || (b0 == 0xa1 && code.get(5) == Some(&0x85) && code.get(6) == Some(&0xc0))
        // Same guard with an extra pointer deref: `mov eax,[moffs32]; mov eax,[eax];
        // test eax,eax` — mingw's `__do_global_dtors_aux`/`__do_global_ctors_aux`
        // (registered via `atexit(&aux)`, reached only by indirect call at exit).
        // The 9-byte signature is version-independent (unlike a FLIRT match).
        || (b0 == 0xa1 && code.get(5) == Some(&0x8b) && code.get(6) == Some(&0x00)
            && code.get(7) == Some(&0x85) && code.get(8) == Some(&0xc0))
        // mov reg, [esp+disp] (modrm rm=100=SIB, mod≠11; SIB=24 → base=esp).
        || (allow_leaf && b0 == 0x8b && (b1 & 0x07) == 0x04 && (b1 & 0xc0) != 0xc0 && b2 == 0x24)
        // A tiny x87 math thunk (`fld [esp+d]; …; ret`) — the C `ceil`/`floor`/
        // `trunc`/`atan2`/`fmod` a real MSVC CRT ships as leaf helpers, reached
        // only through a data pointer (stored as a SQL function's user-data, an
        // isolated slot — not a >=3 run that the table heuristic trusts). Their
        // `fld m64,[esp+d]` prologue matches none of the shapes above, so without
        // this the indirect call through the pointer lands on unrecovered code and
        // aborts (a sound but needless `atan2`/`fmod`/`trunc` failure). The whole
        // body is verified x87+glue ending in `ret`, so this is safe even for a
        // bare data pointer (random data does not decode as a clean x87 leaf).
        || is_x87_leaf_thunk(prog, addr)
}

/// Decode forward from `addr` and decide whether it is a small, self-contained
/// x87 math leaf: it *starts* with an FPU load of a stack argument
/// (`fld`/`fild [esp+disp]`) and is composed solely of FPU ops plus the integer
/// glue such helpers use (stack adjust, control-word `mov`/`or`/`and`, `sahf`
/// for an `fprem` completion loop), terminating at a `ret` within a tight bound,
/// with no `call` and no branch leaving the body. This whole-body check is a far
/// stronger signal than a prologue byte pattern — strong enough to seed a
/// function from an isolated data pointer without risking interior-byte or
/// random-data false positives.
fn is_x87_leaf_thunk(prog: &Program, addr: u64) -> bool {
    use iced_x86::Mnemonic::*;
    let dis = Disassembler::new(prog.bitness);
    // First instruction: an x87 load of a stack slot (`fld`/`fild [esp+…]`).
    let Some(first) = dis.decode_at(prog, addr) else { return false };
    if !matches!(first.raw.mnemonic(), Fld | Fild)
        || first.raw.memory_base() != iced_x86::Register::ESP
    {
        return false;
    }
    let mut a = addr;
    for _ in 0..40 {
        let Some(ins) = dis.decode_at(prog, a) else { return false };
        let m = ins.raw.mnemonic();
        match ins.flow {
            Flow::Return => return true, // clean leaf terminus
            Flow::Call | Flow::Jump | Flow::Indirect | Flow::Interrupt => return false,
            Flow::CondJump => {
                // Only a backward branch that stays inside the body (the `fprem`
                // completion loop `jp`); anything leaving the body disqualifies it.
                // A validated local branch is allowed as-is (its mnemonic is a jcc,
                // neither x87 nor integer glue), so skip the body-shape check below.
                match ins.target {
                    Some(t) if t >= addr && t <= a => {
                        a = ins.next_addr();
                        continue;
                    }
                    _ => return false,
                }
            }
            Flow::Fallthrough => {}
        }
        let ok = crate::ir::lift::is_x87(&ins.raw)
            || matches!(
                m,
                // integer glue the CRT math thunks use around the FPU core
                Sub | Add | Mov | Movzx | Movsx | Or | And | Sahf | Lahf | Nop | Xchg | Lea | Test
            );
        if !ok {
            return false;
        }
        a = ins.next_addr();
    }
    false // ran past the bound without a `ret` — not a tidy leaf
}

/// The immediate of a `push imm32` or `mov [esp+d], imm32` — a value being placed
/// in an outgoing stack-argument slot. When it points into executable code it is
/// almost always a function pointer passed by value (a callback being registered
/// or handed to a helper: `atexit(cleanup)`, `qsort(…, cmp)`), which is a stronger
/// signal than any prologue heuristic — so such a target is seeded whatever
/// prologue the callee uses (many CRT cleanups start with `mov eax,imm; xchg`, not
/// `push ebp`). Restricted to the stack-arg forms so a `mov reg, imm` loading a
/// scalar constant is not mistaken for a callback.
fn stack_arg_code_imm(insn: &iced_x86::Instruction) -> Option<u64> {
    use iced_x86::{Mnemonic, OpKind, Register};
    match insn.mnemonic() {
        Mnemonic::Push
            if matches!(insn.op0_kind(), OpKind::Immediate32 | OpKind::Immediate32to64) =>
        {
            Some(insn.immediate(0))
        }
        Mnemonic::Mov
            if insn.op0_kind() == OpKind::Memory
                && insn.memory_base() == Register::ESP
                && insn.memory_index() == Register::None
                && matches!(insn.op1_kind(), OpKind::Immediate32 | OpKind::Immediate32to64) =>
        {
            Some(insn.immediate(1))
        }
        _ => None,
    }
}

/// Code-address immediates of an instruction: a `push imm32`/`mov reg,imm32`/
/// `mov [mem],imm32` whose immediate, or an absolute `[imm32]` memory operand,
/// could be a taken function address. Branch displacements are `NearBranch`
/// (not immediates) and so are excluded.
fn imm_code_ptrs(insn: &iced_x86::Instruction) -> Vec<u64> {
    use iced_x86::{OpKind, Register};
    let mut out = Vec::new();
    for i in 0..insn.op_count() {
        match insn.op_kind(i) {
            OpKind::Immediate32 | OpKind::Immediate32to64 => out.push(insn.immediate(i)),
            OpKind::Memory
                if insn.memory_base() == Register::None
                    && insn.memory_index() == Register::None =>
            {
                out.push(insn.memory_displacement64())
            }
            _ => {}
        }
    }
    out
}

/// Slot VA of an absolute-indirect `call [disp32]` / `jmp [disp32]` — a call
/// through a fixed pointer slot in the image (no base/index register). The
/// *contents* of that slot are a function address: this is definitive proof of a
/// function entry, stronger than any prologue heuristic. The classic case is the
/// Control-Flow-Guard check (`call [__guard_check_icall_fptr]`) whose default
/// target is a bare `ret` thunk — not a recognisable prologue, reached by no
/// direct call, so otherwise invisible to recovery. Indexed forms (`[base+idx*s]`,
/// jump tables) are excluded by requiring no index and a memory base of None.
fn abs_indirect_slot(insn: &iced_x86::Instruction) -> Option<u64> {
    use iced_x86::{FlowControl, Register};
    if !matches!(insn.flow_control(), FlowControl::IndirectCall | FlowControl::IndirectBranch) {
        return None;
    }
    // Operand 0 of an indirect call/jmp is the target. Require a pure absolute
    // memory operand `[disp32]`.
    if insn.op0_kind() == iced_x86::OpKind::Memory
        && insn.memory_base() == Register::None
        && insn.memory_index() == Register::None
    {
        Some(insn.memory_displacement64())
    } else {
        None
    }
}

/// Table-base VA of an indexed indirect call `call [idx*4 + disp32]` — a dispatch
/// through an image-fixed **function-pointer table** (a statically-linked CRT
/// init/atexit array walked by `call [ebx*4 + base]`, e.g. NASM's initializer
/// thunks). Its code-pointer entries are functions reached by no direct call, so
/// otherwise invisible to recovery. Requires no base register (a base register
/// means a stack/heap array, not an image table) and a dword (scale-4) index.
fn indexed_call_table_base(insn: &iced_x86::Instruction) -> Option<u64> {
    use iced_x86::{FlowControl, Register};
    // Only a `call` — a `jmp [idx*4+base]` is a switch jump table whose entries are
    // case bodies (interior code), not function entries (handled by resolve_jump_table).
    if insn.flow_control() != FlowControl::IndirectCall {
        return None;
    }
    if insn.op0_kind() == iced_x86::OpKind::Memory
        && insn.memory_base() == Register::None
        && insn.memory_index() != Register::None
        && insn.memory_index_scale() == 4
    {
        Some(insn.memory_displacement64())
    } else {
        None
    }
}

/// The `(slot, imm)` of a `mov dword [disp32], imm32` — an immediate stored into a
/// fixed absolute address. When `slot` is a proven indirect-call slot (used by a
/// `call [slot]` elsewhere), the stored code `imm` is the *runtime* function
/// pointer that the static `abs_indirect_slot` read misses (a `.data` slot reads
/// 0 statically). Busybox `od` installs its default no-op handler (a bare `ret`)
/// this way: `movl $handler, g ; … ; call [g]`.
fn abs_store_imm(insn: &iced_x86::Instruction) -> Option<(u64, u64)> {
    use iced_x86::{Mnemonic, OpKind, Register};
    if insn.mnemonic() == Mnemonic::Mov
        && insn.op0_kind() == OpKind::Memory
        && insn.memory_base() == Register::None
        && insn.memory_index() == Register::None
        && matches!(insn.op1_kind(), OpKind::Immediate32 | OpKind::Immediate32to64)
    {
        Some((insn.memory_displacement64(), insn.immediate(1)))
    } else {
        None
    }
}

/// The code immediate of a `mov [base+…], imm32` that writes a **code pointer**
/// into a struct/object field through a base register (`mov [ebx], method`, `mov
/// [obj+8], handler`). Storing a `.text` address into a pointed-to object is an
/// address-taken function pointer — the same strength of proof as a by-value
/// stack-arg callback — so `imm` names a function whatever prologue the callee
/// uses. NASM's OMF backend installs a bare-`ret` no-op method into its `struct
/// ofmt` this way and later dispatches it through `call [obj+disp]`; without
/// recovering it the indirect call aborts on unrecovered code (the isolated
/// method is reached by no direct call and sits behind a computed address, so no
/// other heuristic sees it). A base register is required, so a `mov [disp32],
/// imm` to a fixed slot (handled as an indirect-call-slot store) is not
/// double-counted, and so a scalar constant stored to a fixed scalar global is
/// not mistaken for a method table.
fn mem_store_code_imm(insn: &iced_x86::Instruction) -> Option<u64> {
    use iced_x86::{Mnemonic, OpKind, Register};
    if insn.mnemonic() == Mnemonic::Mov
        && insn.op0_kind() == OpKind::Memory
        && insn.memory_base() != Register::None
        && matches!(insn.op1_kind(), OpKind::Immediate32 | OpKind::Immediate32to64)
    {
        Some(insn.immediate(1))
    } else {
        None
    }
}

/// Whether `addr` begins a **bare-`ret` stub** — a `ret`/`ret imm16` as the very
/// first instruction (optionally after a `mov edi,edi` hot-patch pad). Such a
/// no-op is a legitimate function body that `looks_like_func_start` rejects (no
/// prologue). Only trusted for an *address-taken* code pointer (a stored/pushed
/// function pointer), never for a linear-scan seed, so alignment/padding `ret`
/// bytes are not turned into spurious functions. NASM's OMF `struct ofmt` uses
/// one as a do-nothing method (`cleanup`/`filename`), dispatched indirectly.
fn is_bare_ret_stub(prog: &Program, addr: u64) -> bool {
    let dis = Disassembler::new(prog.bitness);
    let Some(first) = dis.decode_at(prog, addr) else { return false };
    matches!(first.raw.mnemonic(), iced_x86::Mnemonic::Ret | iced_x86::Mnemonic::Retf)
}

/// A code immediate loaded into a register that is then used — before the register
/// is overwritten — as an indirect `call`/`jmp` target: `mov ebp,imm ; … ; call
/// *ebp`. The register-indirect call through the just-loaded pointer is definitive
/// proof `imm` is a function entry, the same strength as a stack-arg callback or a
/// `call [slot]`, so it bypasses the prologue heuristic (the callee may be a bare
/// `ret` or an FPO body). Busybox `cksum` selects its CRC variant this way.
///
/// The forward scan stays on the *straight-line* block from the `mov` (it stops at
/// the first address gap, a return, any reassignment of the register, or — for a
/// caller-saved register — an intervening call that would clobber it), so it can
/// never attribute an unrelated later value to the call.
fn reg_imm_reaches_indirect_call(
    global: &BTreeMap<u64, Insn>,
    mov: &Insn,
    in_exec: &dyn Fn(u64) -> bool,
) -> Option<u64> {
    use iced_x86::{FlowControl, InstructionInfoFactory, Mnemonic, OpAccess, OpKind, Register};
    let r = &mov.raw;
    if r.mnemonic() != Mnemonic::Mov
        || r.op0_kind() != OpKind::Register
        || !matches!(r.op1_kind(), OpKind::Immediate32 | OpKind::Immediate32to64)
    {
        return None;
    }
    let reg = r.op0_register().full_register();
    let imm = r.immediate(1);
    if !in_exec(imm) {
        return None;
    }
    let caller_saved = matches!(reg, Register::RAX | Register::RCX | Register::RDX);
    let mut factory = InstructionInfoFactory::new();
    let mut expected = mov.next_addr();
    let mut budget = 48u32;
    for (&addr, si) in global.range(expected..) {
        if addr != expected || budget == 0 {
            break; // left the straight-line block, or scanned far enough
        }
        budget -= 1;
        let s = &si.raw;
        // Indirect call/jmp through the same register — proven function entry.
        if matches!(s.flow_control(), FlowControl::IndirectCall | FlowControl::IndirectBranch)
            && s.op0_kind() == OpKind::Register
            && s.op0_register().full_register() == reg
        {
            return Some(imm);
        }
        // A call clobbers caller-saved registers → the value can no longer be proven.
        if caller_saved && matches!(s.flow_control(), FlowControl::IndirectCall | FlowControl::Call) {
            return None;
        }
        // Any write to the register → it was reassigned before any indirect call.
        for ur in factory.info(s).used_registers() {
            if ur.register().full_register() == reg
                && matches!(
                    ur.access(),
                    OpAccess::Write | OpAccess::CondWrite | OpAccess::ReadWrite | OpAccess::ReadCondWrite
                )
            {
                return None;
            }
        }
        if matches!(s.flow_control(), FlowControl::Return) {
            return None;
        }
        expected = si.next_addr();
    }
    None
}

fn global_decode(
    prog: &Program,
    disasm: &Disassembler,
    prologue_scan: bool,
) -> (BTreeMap<u64, Insn>, BTreeSet<u64>, HashMap<u64, Vec<u64>>, BTreeSet<u64>) {
    let mut global: BTreeMap<u64, Insn> = BTreeMap::new();
    let mut entries: BTreeSet<u64> = prog.seed_functions().into_iter().collect();
    let mut jump_tables: HashMap<u64, Vec<u64>> = HashMap::new();
    if entries.is_empty() && prog.is_executable(prog.entry) {
        entries.insert(prog.entry);
    }

    // Pass 1: recursive descent from direct-call seeds.
    let mut work: VecDeque<u64> = entries.iter().copied().collect();
    drain(prog, disasm, &mut global, &mut entries, &mut jump_tables, &mut work);

    // Pass 2: prologue scanning recovers indirectly-reached functions. Only
    // seed prologues we haven't already decoded as part of a known function.
    // These seeds are heuristic (a `push rbp; mov rbp,rsp` mid-function looks
    // like an entry) — record them so `collect_function` can absorb the ones
    // that turn out to be reachable by fall-through from a real function.
    let mut prologue_only: BTreeSet<u64> = BTreeSet::new();
    if prologue_scan {
        let mut extra: VecDeque<u64> = VecDeque::new();
        for p in prog.prologue_seeds() {
            if !global.contains_key(&p) && prog.is_executable(p) && entries.insert(p) {
                prologue_only.insert(p);
                extra.push_back(p);
            }
        }
        drain(prog, disasm, &mut global, &mut entries, &mut jump_tables, &mut extra);

        // Pass 2b: address-taken function discovery. A frame-pointer-omitted
        // function (gcc/MSVC `-O2`) reached only through a pointer — a callback,
        // a vtable slot, a registration/dispatch table (Lua's `luaL_Reg` arrays)
        // or a by-value `push imm32` — is found by neither recursive descent (no
        // direct call) nor the `push ebp` prologue scan. Candidates come from two
        // places: pointer-aligned section *data* words, and immediate operands in
        // the decoded *code* stream. Both must point into an executable section,
        // at a not-yet-decoded plausible function start.
        let ptr = (prog.bitness.bits() / 8) as usize;
        let exec: Vec<(u64, u64)> = prog
            .sections
            .iter()
            .filter(|s| s.executable)
            .map(|s| (s.address, s.address + s.data.len() as u64))
            .collect();
        let in_exec = |a: u64| a != 0 && exec.iter().any(|&(lo, hi)| a >= lo && a < hi);
        // Re-scan to a fixpoint (each newly decoded function reveals more
        // pointers), draining candidates in *ascending* address order one at a
        // time: a parent function decoded first then covers its own interior, so
        // an intra-function block address (a jump-table case target stored in
        // .rdata, a label pushed as an argument) is rejected by the
        // `!global.contains_key` gate instead of splitting the function. We also
        // skip resolved jump-table targets outright.
        loop {
            // Resolve any jump tables now visible *before* seeding function
            // candidates this round, so a switch's case targets are excluded from
            // the data scan (they must not be mistaken for function-pointer-table
            // entries). Newly seeded functions (drained below) can reveal further
            // tables, caught on the next iteration.
            let jt_progress =
                resolve_jump_tables_fixpoint(prog, disasm, &mut global, &mut entries, &mut jump_tables);
            let jt_targets: BTreeSet<u64> =
                jump_tables.values().flat_map(|v| v.iter().copied()).collect();
            let mut cands: BTreeSet<u64> = BTreeSet::new();
            // Entries forced from a confirmed function-pointer table even though the
            // linear sweep already decoded them (it absorbed them into a preceding
            // function by falling through a *noreturn* call). See the `forced`
            // commit below.
            let mut forced: BTreeSet<u64> = BTreeSet::new();
            for sec in &prog.sections {
                let d = &sec.data;
                let nwords = d.len() / ptr;
                let word = |w: usize| -> u64 {
                    let o = w * ptr;
                    match ptr {
                        8 => u64::from_le_bytes(d[o..o + 8].try_into().unwrap()),
                        _ => u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]]) as u64,
                    }
                };
                // A window holding >= 3 code pointers is a function-pointer table
                // (vtable, applet/callback array, _initterm/TLS-callback list):
                // random data almost never has three valid code addresses in a
                // tight window, so accept *every* code entry — including tiny
                // callbacks (`xor eax,eax; ret`, or a CRT initializer beginning
                // `mov [mem],imm32`) whose prologue `looks_like_func_start` would
                // reject. A lone code pointer still needs the prologue gate (a jump
                // table's case-target run is excluded later via `jt_targets`).
                //
                // The window tolerates NULL gaps: an `_initterm`/TLS-callback list
                // legitimately holds NULL slots (padding, a NULL terminator)
                // between live pointers, so a strict "consecutive" run would miss
                // the isolated entries. A NULL is unambiguous (never a code
                // address), so allowing it does not loosen the random-data guard;
                // we cap the gap at a few consecutive NULLs so unrelated zero
                // regions are not merged into one bogus table.
                const MAX_NULL_GAP: usize = 4;
                let mut w = 0usize;
                while w < nwords {
                    if in_exec(word(w)) {
                        let start = w;
                        let mut ncode = 0usize;
                        loop {
                            if in_exec(word(w)) {
                                ncode += 1;
                                w += 1;
                            } else if word(w) == 0 {
                                // Consume a bounded NULL gap only if a code pointer
                                // continues the table after it.
                                let mut z = w;
                                while z < nwords && z - w < MAX_NULL_GAP && word(z) == 0 {
                                    z += 1;
                                }
                                if z < nwords && z - w < MAX_NULL_GAP && in_exec(word(z)) {
                                    w = z; // skip the gap, stay in the table
                                } else {
                                    break;
                                }
                            } else {
                                break;
                            }
                            if w >= nwords {
                                break;
                            }
                        }
                        let in_table = ncode >= 3;
                        // A *jump table* (dense `switch`) is also a run of consecutive
                        // code pointers, but a given target repeats — many indices map
                        // to the same case, especially the `default`. A genuine
                        // function-pointer table (vtable, applet/callback array) holds
                        // distinct entries. So a value repeating >= 3x within the run is
                        // a switch case, not a function: its target is an interior case
                        // body (not a prologue), so require the per-entry prologue gate
                        // for it instead of accepting it just for being in the run.
                        let mut counts: HashMap<u64, u32> = HashMap::new();
                        for k in start..w {
                            if word(k) != 0 {
                                *counts.entry(word(k)).or_insert(0) += 1;
                            }
                        }
                        for k in start..w {
                            let v = word(k);
                            if v == 0 {
                                continue; // NULL gap slot — not an entry
                            }
                            let trusted = in_table && counts[&v] < 3;
                            if !global.contains_key(&v)
                                && (trusted || looks_like_func_start(prog, v, false))
                            {
                                cands.insert(v);
                            } else if trusted
                                && global.contains_key(&v)
                                && looks_like_func_start(prog, v, false)
                            {
                                // Already decoded, but a confirmed function-pointer
                                // table (>= 3 consecutive code pointers) says `v` is a
                                // genuine function start. The linear sweep absorbed it
                                // into the preceding function by falling through a
                                // *noreturn* call (e.g. `*_and_die`/exit), so `v` is
                                // not yet an entry and the indirect call to it aborts.
                                // Force it as an entry to split at the true boundary;
                                // `looks_like_func_start` excludes interior jump-table
                                // case bodies, and jump-table targets are filtered on
                                // commit below.
                                forced.insert(v);
                            }
                        }
                    } else {
                        w += 1;
                    }
                }
            }
            // Absolute slots used as an indirect call/jmp target (`call [g]`) — a
            // proven function-pointer variable. A code immediate stored into one
            // (`mov [g], imm`) is the runtime function pointer the static
            // `abs_indirect_slot` read misses when the slot lives in writable data.
            let icall_slots: BTreeSet<u64> =
                global.values().filter_map(|i| abs_indirect_slot(&i.raw)).collect();
            for insn in global.values() {
                for v in imm_code_ptrs(&insn.raw) {
                    // Code immediates: a by-value callback — allow the leaf shape.
                    if in_exec(v) && !global.contains_key(&v) && looks_like_func_start(prog, v, true) {
                        cands.insert(v);
                    }
                }
                // A code pointer placed on the stack as an argument (`push imm32` /
                // `mov [esp+d], imm32`) is a callback passed by value — e.g.
                // `atexit(_dtoa_lock_cleanup)`, `qsort(…, cmp)`. The argument
                // position proves it is a function, whatever prologue the callee
                // has, so accept it without the prologue heuristic. (A resolved
                // jump-table target caught this way is pruned after the fixpoint.)
                if let Some(v) = stack_arg_code_imm(&insn.raw) {
                    if in_exec(v) {
                        if !global.contains_key(&v) {
                            cands.insert(v);
                        } else if looks_like_func_start(prog, v, true) {
                            // Absorbed into the preceding function by falling through
                            // a *noreturn* call (e.g. `atexit(sayAbnormalExit)` sits
                            // right after a `call _shell_out_of_memory` that never
                            // returns). The by-value callback position proves `v` is a
                            // function; force the boundary re-split (guarded by
                            // `looks_like_func_start`, which excludes jump-table case
                            // bodies, since this truncates an existing function).
                            forced.insert(v);
                        }
                    }
                }
                // A code pointer written into a struct/object field through a base
                // register (`mov [ebx], method`) — a method installed into an
                // object, address-taken and later dispatched via `call [obj+disp]`.
                // Like the stack-arg callback, the store proves `v` is a function;
                // accept the bare-`ret` no-op stub NASM's OMF backend installs
                // (which `looks_like_func_start` rejects) in addition to any normal
                // prologue. Only through this address-taken store, never a linear
                // seed, so padding `ret`s are not turned into functions.
                if let Some(v) = mem_store_code_imm(&insn.raw) {
                    if in_exec(v) {
                        if !global.contains_key(&v) {
                            // Fresh, unclaimed target: accept a normal prologue or the
                            // bare-`ret` no-op stub (seeding unclaimed space is safe).
                            if looks_like_func_start(prog, v, true) || is_bare_ret_stub(prog, v) {
                                cands.insert(v);
                            }
                        } else if looks_like_func_start(prog, v, true) {
                            // Absorbed into a preceding function; only a real prologue
                            // forces the re-split — a bare `ret` interior to a function
                            // must never truncate it.
                            forced.insert(v);
                        }
                    }
                }
                // Absolute-indirect `call/jmp [slot]`: the pointer stored at the
                // slot is the call target — definitive proof of a function entry,
                // so it bypasses the prologue heuristic (the CFG-guard default is a
                // bare `ret`). `read_u32`/`read_u64` returns the static slot value;
                // an IAT slot holds an import RVA into non-exec data and is filtered
                // by `in_exec`, so internal pointer slots only.
                if let Some(slot) = abs_indirect_slot(&insn.raw) {
                    let target = if ptr == 8 { prog.read_u64(slot) } else { prog.read_u32(slot).map(u64::from) };
                    if let Some(v) = target {
                        if in_exec(v) {
                            if !global.contains_key(&v) {
                                cands.insert(v);
                            } else if looks_like_func_start(prog, v, true) {
                                // Same noreturn-absorption case as above; the slot's
                                // pointer proves `v` is a function → force re-split.
                                forced.insert(v);
                            }
                        }
                    }
                }
                // Indexed indirect call `call [idx*4 + base]`: `base` is a
                // function-pointer table (a CRT init/atexit/callback array). Harvest
                // its code-pointer entries — reached by no direct call, so invisible
                // otherwise (NASM's initializer thunk aborted here). Scan dwords from
                // `base`: `0` ends the table, `0xffffffff` is a count/sentinel (skip),
                // an in-image function-start pointer is harvested, and any other value
                // ends the table — bounded to avoid runaway scans of unrelated data.
                if let Some(base) = indexed_call_table_base(&insn.raw) {
                    let mut a = base;
                    for _ in 0..256 {
                        let Some(v) = prog.read_u32(a).map(u64::from) else { break };
                        if v == 0 {
                            break; // null terminator ends the table
                        } else if v == 0xffff_ffff {
                            // count marker / sentinel — keep scanning
                        } else if in_exec(v) && looks_like_func_start(prog, v, true) {
                            // A `call [idx*4+base]` is definitive proof `v` is a
                            // function. If the linear sweep already absorbed it into a
                            // preceding function, force the boundary (re-split) as with
                            // the >=3-pointer table case; else seed it fresh.
                            if global.contains_key(&v) {
                                forced.insert(v);
                            } else {
                                cands.insert(v);
                            }
                        } else {
                            break; // a plain data value ends the table
                        }
                        a += 4;
                    }
                }
                // A code immediate stored into a proven indirect-call slot
                // (`mov [g], imm` where `call [g]` occurs) — the runtime function
                // pointer (busybox `od`'s default no-op `ret` handler).
                if let Some((slot, imm)) = abs_store_imm(&insn.raw) {
                    if icall_slots.contains(&slot) && in_exec(imm) && !global.contains_key(&imm) {
                        cands.insert(imm);
                    }
                }
                // A code immediate loaded into a register then indirect-called
                // (`mov reg, imm ; … ; call *reg`) — a function pointer selected in
                // code (busybox `cksum`'s CRC variant). Proven, bypasses prologue.
                if let Some(v) = reg_imm_reaches_indirect_call(&global, insn, &in_exec) {
                    if !global.contains_key(&v) {
                        cands.insert(v);
                    }
                }
            }
            let mut progressed = jt_progress;
            for c in cands {
                // A function drained earlier this pass (lower address) may now
                // cover `c`, or it may be a jump-table case target — either way
                // it is interior, not an entry.
                if global.contains_key(&c) || jt_targets.contains(&c) {
                    continue;
                }
                if entries.insert(c) {
                    prologue_only.insert(c);
                    progressed = true;
                    let mut q: VecDeque<u64> = VecDeque::from([c]);
                    drain(prog, disasm, &mut global, &mut entries, &mut jump_tables, &mut q);
                }
            }
            // Forced table entries are already decoded (no drain needed); adding them
            // to `entries` makes them boundaries so the over-absorbing predecessor is
            // re-split there. They are confirmed function starts, not prologue-scan
            // guesses, so they are *not* added to `prologue_only` (never dropped on a
            // fall-through edge).
            for c in forced {
                if jt_targets.contains(&c) {
                    continue;
                }
                if entries.insert(c) {
                    progressed = true;
                }
            }
            if !progressed {
                break;
            }
        }
    }

    // Jump-table resolution is order-sensitive: it scans the instructions
    // *preceding* an indirect `jmp` for the table idiom, so if a `jmp` is first
    // decoded as another path's target (before its own `lea/movsxd/add` setup
    // exists), the inline attempt in `drain` fails and is never retried. Re-run
    // resolution to a fixpoint now that all reachable code is decoded, decoding
    // any newly discovered case targets.
    resolve_jump_tables_fixpoint(prog, disasm, &mut global, &mut entries, &mut jump_tables);

    // A switch stores its case-target addresses in a `.rdata` word array. The data
    // scan above sees that dense run of code pointers and, if the switch's function
    // was not yet decoded when it ran (its `jmp [table+idx*4]` unresolved), mistakes
    // the array for a function-pointer table and seeds each interior case body as a
    // bogus function — which then truncates the real function at that boundary. The
    // race is unavoidable (the data scan is global; the function is reached late),
    // so correct it *after* the fixpoint, when every jump table is resolved: a
    // resolved case target is an interior block, never a function entry. Removing it
    // lets the real function collect through it. (A prologue-only seed only; a real
    // symbol/call target is never a case target.)
    let jt_targets: BTreeSet<u64> =
        jump_tables.values().flat_map(|v| v.iter().copied()).collect();
    entries.retain(|e| !jt_targets.contains(e));
    prologue_only.retain(|e| !jt_targets.contains(e));

    (global, entries, jump_tables, prologue_only)
}

/// Resolve every static jump table reachable in `global` to a fixpoint, decoding
/// each newly discovered case target. Returns whether any *new* table was found.
///
/// Jump-table resolution is order-sensitive: it scans the instructions preceding
/// an indirect `jmp` for the table idiom, so if a `jmp` was first decoded as
/// another path's target (before its own `lea/movsxd/add` setup existed), the
/// inline attempt in `drain` failed and is never retried — hence this re-run once
/// all reachable code is decoded. Running it *before* function-candidate seeding
/// is what keeps a switch's case targets (interior addresses a compiler stores in
/// a `.rdata` table) out of the seed set: otherwise the data scan sees a dense run
/// of code pointers and mistakes the case bodies for a function-pointer table,
/// seeding each as a bogus function that then truncates the real one.
fn resolve_jump_tables_fixpoint(
    prog: &Program,
    disasm: &Disassembler,
    global: &mut BTreeMap<u64, Insn>,
    entries: &mut BTreeSet<u64>,
    jump_tables: &mut HashMap<u64, Vec<u64>>,
) -> bool {
    let mut any = false;
    loop {
        let found: Vec<(u64, Vec<u64>)> = global
            .iter()
            .filter(|(addr, insn)| insn.flow == Flow::Indirect && !jump_tables.contains_key(addr))
            .filter_map(|(addr, insn)| {
                resolve_jump_table(prog, global, insn)
                    .or_else(|| resolve_pie_jump_table(prog, global, insn))
                    .or_else(|| resolve_abs_jump_table(prog, global, insn))
                    .map(|t| (*addr, t))
            })
            .collect();
        if found.is_empty() {
            break;
        }
        any = true;
        let mut newwork: VecDeque<u64> = VecDeque::new();
        for (addr, targets) in found {
            for &t in &targets {
                if !global.contains_key(&t) {
                    newwork.push_back(t);
                }
            }
            jump_tables.insert(addr, targets);
        }
        if !newwork.is_empty() {
            drain(prog, disasm, global, entries, jump_tables, &mut newwork);
        }
    }
    any
}

/// Recognise a jump-table dispatch `jmp [table + idx*ptr]` and read its target
/// list from the binary. Conservative: pointer-sized entries, an absolute (or
/// rip-relative) table base, entries kept while they point into executable code.
fn resolve_jump_table(prog: &Program, global: &BTreeMap<u64, Insn>, insn: &Insn) -> Option<Vec<u64>> {
    use iced_x86::{OpKind, Register};
    let ins = &insn.raw;
    if ins.op_kind(0) != OpKind::Memory || ins.memory_index() == Register::None {
        return None;
    }
    let ptr = prog.bitness.bits() / 8;
    if ins.memory_index_scale() != ptr {
        return None; // entries must be pointer-sized
    }
    // The table base must be a static address (no base register), unless it is
    // rip-relative (then iced gives the absolute address).
    let table = if ins.is_ip_rel_memory_operand() {
        ins.ip_rel_memory_address()
    } else if ins.memory_base() == Register::None {
        ins.memory_displacement64()
    } else {
        return None;
    };

    // Cap the table at the `cmp idx, N; ja default` range check that guards the
    // jump (valid indices 0..=N ⇒ N+1 entries). Without it, a table immediately
    // followed by another (a second switch, sharing executable case targets)
    // over-reads into it — merging two functions and corrupting both CFGs.
    let bound = jump_index_bound(global, insn, ins.memory_index().full_register());
    read_jump_table(prog, table, ptr as u64, bound)
}

/// The `cmp idx, N; ja/jae default` bound on a switch index, scanned just before
/// the indirect jump: returns `N+1` (the entry count) if found. Matches the index
/// register family (`cmp edx,N` guards `jmp [edx*4+t]`); a `ja` (unsigned above)
/// means indices `> N` are out of range, so the table has `N+1` slots.
fn jump_index_bound(global: &BTreeMap<u64, Insn>, jmp: &Insn, idx: iced_x86::Register) -> Option<u64> {
    use iced_x86::{Mnemonic, OpKind};
    for (_, ins) in global.range(..jmp.address).rev().take(8) {
        let r = &ins.raw;
        if r.mnemonic() == Mnemonic::Cmp
            && r.op0_kind() == OpKind::Register
            && r.op0_register().full_register() == idx
            && matches!(r.op1_kind(), OpKind::Immediate8 | OpKind::Immediate8to16
                | OpKind::Immediate8to32 | OpKind::Immediate16 | OpKind::Immediate32)
        {
            return Some(r.immediate(1).wrapping_add(1));
        }
    }
    None
}

/// Read a pointer-sized table of absolute code addresses at `table`, in index
/// order *with duplicates preserved* (the structured emitter maps `case k ->
/// successors[k]`, so collapsing duplicate targets — common when several switch
/// labels share a body — would shift every later case onto the wrong block).
/// Stops at the first non-code word (table end); needs >= 2 distinct targets to
/// count as a real dispatch table.
fn read_jump_table(prog: &Program, table: u64, ptr: u64, bound: Option<u64>) -> Option<Vec<u64>> {
    let mut targets = Vec::new();
    let mut distinct = BTreeSet::new();
    let limit = bound.unwrap_or(1024).min(1024);
    for i in 0..limit {
        let ea = table + i * ptr;
        let entry = match if ptr == 8 { prog.read_u64(ea) } else { prog.read_u32(ea).map(|v| v as u64) } {
            Some(e) => e,
            None => break,
        };
        if !prog.is_executable(entry) {
            break;
        }
        distinct.insert(entry);
        targets.push(entry);
    }
    if distinct.len() >= 2 {
        Some(targets)
    } else {
        None
    }
}

/// Resolve a computed-goto / register-indirect jump table:
///
/// ```text
///   mov  tgt, [table + idx*ptr]   (table = absolute code-address array)
///   jmp  tgt
/// ```
///
/// The compiler emits this for GCC `&&label` computed gotos (e.g. an interpreter
/// dispatch loop) and dense switches: a load of an absolute target from a static
/// table, then an indirect jump through the register. Returns the table's targets.
fn resolve_abs_jump_table(prog: &Program, global: &BTreeMap<u64, Insn>, jmp: &Insn) -> Option<Vec<u64>> {
    use iced_x86::{Mnemonic, OpKind, Register};
    let j = &jmp.raw;
    if j.op0_kind() != OpKind::Register {
        return None;
    }
    let tgt = j.op0_register().full_register();
    let ptr = (prog.bitness.bits() / 8) as u32;
    // Reaching definition of the jump register, just before the jump: it must be a
    // `mov tgt, [table + idx*ptr]` from a static (absolute or rip-relative) table.
    for (_, ins) in global.range(..jmp.address).rev().take(8) {
        let r = &ins.raw;
        if r.op0_kind() != OpKind::Register || r.op0_register().full_register() != tgt {
            continue;
        }
        if r.mnemonic() == Mnemonic::Mov
            && r.op1_kind() == OpKind::Memory
            && r.memory_index() != Register::None
            && r.memory_index_scale() == ptr
        {
            let table = if r.is_ip_rel_memory_operand() {
                r.ip_rel_memory_address()
            } else if r.memory_base() == Register::None {
                r.memory_displacement64()
            } else {
                return None; // based + indexed: not a static table
            };
            let bound = jump_index_bound(global, jmp, r.memory_index().full_register());
            return read_jump_table(prog, table, ptr as u64, bound);
        }
        // The -O0 computed-address idiom: the table address is built in steps
        // (`shl idx,2; add idx,table; mov tgt,[idx]; jmp tgt`) instead of one
        // base+index load. The reaching def is `mov tgt, [base]` (a plain deref);
        // trace `base` back to the `add base, table` that set the table address.
        if r.mnemonic() == Mnemonic::Mov
            && r.op1_kind() == OpKind::Memory
            && r.memory_index() == Register::None
            && r.memory_base() != Register::None
            && r.memory_displacement64() == 0
        {
            let base = r.memory_base().full_register();
            for (_, ins2) in global.range(..ins.address).rev().take(6) {
                let a = &ins2.raw;
                if a.op0_kind() != OpKind::Register || a.op0_register().full_register() != base {
                    continue;
                }
                if a.mnemonic() == Mnemonic::Add
                    && matches!(a.op1_kind(), OpKind::Immediate8 | OpKind::Immediate8to32
                        | OpKind::Immediate16 | OpKind::Immediate32)
                {
                    return read_jump_table(prog, a.immediate(1), ptr as u64, None);
                }
                break; // base last set by something other than `add base, table`
            }
        }
        return None; // tgt last written by something other than a table load
    }
    None
}

/// Resolve the PIE relative jump-table idiom ending in `jmp reg`:
///
/// ```text
///   cmp idx, N ; ja default          (bound, optional)
///   lea base, [rip+table]
///   movsxd tgt, [base + idx*4]        (signed 4-byte offset)
///   add tgt, base                     (target = table + offset)
///   (notrack) jmp tgt
/// ```
///
/// The three setup instructions precede `jmp` in `global`. Reads the relative
/// offset table from the binary and returns the absolute case targets (so the
/// caller decodes them and attaches the CFG edges).
fn resolve_pie_jump_table(prog: &Program, global: &BTreeMap<u64, Insn>, jmp: &Insn) -> Option<Vec<u64>> {
    use iced_x86::{Mnemonic, OpKind, Register};
    let j = &jmp.raw;
    if j.op0_kind() != OpKind::Register {
        return None;
    }
    let tgt = j.op0_register().full_register();
    // Scan the instructions just before the jump (the idiom may interleave an
    // index zero-extension between the lea and the movsxd, so match by pattern
    // rather than fixed position).
    let pre: Vec<iced_x86::Instruction> =
        global.range(..jmp.address).rev().take(10).map(|(_, i)| i.raw).collect();
    // add tgt, base
    let add = pre.iter().find(|i| {
        i.mnemonic() == Mnemonic::Add
            && i.op0_kind() == OpKind::Register
            && i.op1_kind() == OpKind::Register
            && i.op0_register().full_register() == tgt
    })?;
    let base = add.op1_register().full_register();
    // movsxd tgt, [base + idx*4]
    if !pre.iter().any(|i| {
        i.mnemonic() == Mnemonic::Movsxd
            && i.op0_register().full_register() == tgt
            && i.memory_base().full_register() == base
            && i.memory_index() != Register::None
            && i.memory_index_scale() == 4
    }) {
        return None;
    }
    // Find the reaching definition of `base`: scan backwards for the first
    // instruction that writes it. It must be `lea base, [rip+table]` (the table
    // address); any other writer (or a return — function boundary) means we
    // cannot prove the table, so bail. The base register is often set in an
    // earlier (dominating) block, so this looks past the jump's own block.
    let mut table = None;
    for (_, ins) in global.range(..jmp.address).rev().take(2000) {
        let r = &ins.raw;
        if r.flow_control() == iced_x86::FlowControl::Return {
            break;
        }
        if r.op0_kind() == OpKind::Register && r.op0_register().full_register() == base {
            if r.mnemonic() == Mnemonic::Lea && r.is_ip_rel_memory_operand() {
                table = Some(r.ip_rel_memory_address());
            }
            break; // first writer of `base` decides it
        }
    }
    let table = table?;

    // Exact case count from the nearest preceding `cmp idx, N` (the bound check).
    // All immediate encodings must be recognised — notably `Immediate8to64`,
    // used by `cmp r64, imm8` (e.g. `cmp rbx, 9`); missing it skips the real
    // bound and picks a wrong earlier `cmp`, over-reading the table. If no bound
    // is found, fall back to reading until a target leaves executable code.
    let mut count = 256u64;
    for (_, ins) in global.range(..jmp.address).rev() {
        if ins.raw.mnemonic() == Mnemonic::Cmp
            && matches!(
                ins.raw.op1_kind(),
                OpKind::Immediate8
                    | OpKind::Immediate16
                    | OpKind::Immediate32
                    | OpKind::Immediate8to16
                    | OpKind::Immediate8to32
                    | OpKind::Immediate8to64
                    | OpKind::Immediate32to64
            )
        {
            count = (ins.raw.immediate(1) as u64).saturating_add(1).min(256);
            break;
        }
    }

    // Table order (with duplicates) so case index i maps to target[i].
    let mut ordered = Vec::new();
    for i in 0..count {
        let off = match prog.read_u32(table + i * 4) {
            Some(v) => v as i32 as i64,
            None => break,
        };
        let t = (table as i64).wrapping_add(off) as u64;
        if !prog.is_executable(t) {
            break;
        }
        ordered.push(t);
    }
    if ordered.len() >= 2 {
        Some(ordered)
    } else {
        None
    }
}

/// Drain a worklist of run starts, decoding each reachable instruction once
/// into `global` and recording direct-call targets as new entries.
fn drain(
    prog: &Program,
    disasm: &Disassembler,
    global: &mut BTreeMap<u64, Insn>,
    entries: &mut BTreeSet<u64>,
    jump_tables: &mut HashMap<u64, Vec<u64>>,
    work: &mut VecDeque<u64>,
) {
    while let Some(start) = work.pop_front() {
        let mut cur = start;
        loop {
            if global.contains_key(&cur) || !prog.is_executable(cur) {
                break;
            }
            let insn = match disasm.decode_at(prog, cur) {
                Some(i) => i,
                None => break,
            };
            let next = insn.next_addr();
            let flow = insn.flow;
            let target = insn.target;
            let jt = if flow == Flow::Indirect {
                resolve_jump_table(prog, global, &insn)
                    .or_else(|| resolve_pie_jump_table(prog, global, &insn))
                    .or_else(|| resolve_abs_jump_table(prog, global, &insn))
            } else {
                None
            };
            global.insert(cur, insn);

            match flow {
                Flow::Fallthrough => cur = next,
                Flow::Call => {
                    if let Some(t) = target {
                        if prog.is_executable(t) && entries.insert(t) {
                            work.push_back(t);
                        }
                    }
                    cur = next; // call returns to fallthrough
                }
                Flow::CondJump => {
                    if let Some(t) = target {
                        work.push_back(t);
                    }
                    cur = next;
                }
                Flow::Jump => {
                    if let Some(t) = target {
                        work.push_back(t);
                    }
                    break;
                }
                Flow::Indirect => {
                    if let Some(targets) = jt {
                        for &t in &targets {
                            work.push_back(t); // switch cases (intra-function)
                        }
                        jump_tables.insert(cur, targets);
                    }
                    break;
                }
                Flow::Return | Flow::Interrupt => break,
            }
        }
    }
}

/// Collect the instructions belonging to one function by walking intra-function
/// edges over the already-decoded global map, stopping at other entries.
fn collect_function(
    global: &BTreeMap<u64, Insn>,
    entry: u64,
    boundary: &BTreeSet<u64>,
    jump_tables: &HashMap<u64, Vec<u64>>,
    prologue_only: &BTreeSet<u64>,
) -> BTreeMap<u64, Insn> {
    let mut insns: BTreeMap<u64, Insn> = BTreeMap::new();
    // Each work item carries whether it was reached by a *fallthrough* edge.
    // Execution cannot fall through from one function into another, so a
    // fallthrough continuation into a *prologue-scan* entry means that entry is a
    // false positive mid-function (`push rbp; mov rbp,rsp` after an early-exit
    // branch) — absorb it. We restrict this to prologue-scanned entries; real
    // entries (symbols, call targets) always stop collection, even on fallthrough.
    let mut work: VecDeque<(u64, bool)> = VecDeque::new();
    work.push_back((entry, false));

    while let Some((addr, fallthrough)) = work.pop_front() {
        if insns.contains_key(&addr) {
            continue;
        }
        if addr != entry && boundary.contains(&addr)
            && !(fallthrough && prologue_only.contains(&addr))
        {
            continue; // belongs to another function
        }
        let insn = match global.get(&addr) {
            Some(i) => i.clone(),
            None => continue,
        };
        let next = insn.next_addr();
        match insn.flow {
            // The not-taken / sequential successor is an intra-function edge.
            Flow::Fallthrough => work.push_back((next, true)),
            Flow::CondJump => {
                if let Some(t) = insn.target {
                    work.push_back((t, false));
                }
                work.push_back((next, true));
            }
            // A call returns to `next`, but if the callee is no-return the bytes at
            // `next` may be padding or the following function — keep the boundary.
            Flow::Call => work.push_back((next, false)),
            Flow::Jump => {
                if let Some(t) = insn.target {
                    work.push_back((t, false));
                }
            }
            Flow::Indirect => {
                if let Some(targets) = jump_tables.get(&addr) {
                    for &t in targets {
                        work.push_back((t, false)); // switch cases
                    }
                }
            }
            Flow::Return | Flow::Interrupt => {}
        }
        insns.insert(addr, insn);
    }
    insns
}

/// Phase B — build the CFG of a single function from the global map.
fn build_function(
    prog: &Program,
    global: &BTreeMap<u64, Insn>,
    entry: u64,
    all_entries: &BTreeSet<u64>,
    jump_tables: &HashMap<u64, Vec<u64>>,
    prologue_only: &BTreeSet<u64>,
) -> Option<Function> {
    let mut boundary = all_entries.clone();
    boundary.remove(&entry);

    let insns = collect_function(global, entry, &boundary, jump_tables, prologue_only);
    if insns.is_empty() {
        return None;
    }

    let mut callees = BTreeSet::new();
    for insn in insns.values() {
        if insn.flow == Flow::Call {
            if let Some(t) = insn.target {
                callees.insert(t);
            }
        }
    }

    // Determine basic-block leaders.
    let mut leaders: BTreeSet<u64> = BTreeSet::new();
    leaders.insert(entry);
    for insn in insns.values() {
        match insn.flow {
            Flow::CondJump | Flow::Jump => {
                if let Some(t) = insn.target {
                    if insns.contains_key(&t) {
                        leaders.insert(t);
                    }
                }
                if insns.contains_key(&insn.next_addr()) {
                    leaders.insert(insn.next_addr());
                }
            }
            Flow::Return | Flow::Indirect | Flow::Interrupt => {
                if insns.contains_key(&insn.next_addr()) {
                    leaders.insert(insn.next_addr());
                }
                // Jump-table targets begin blocks.
                if let Some(targets) = jump_tables.get(&insn.address) {
                    for &t in targets {
                        if insns.contains_key(&t) {
                            leaders.insert(t);
                        }
                    }
                }
            }
            _ => {}
        }
    }

    let ordered: Vec<u64> = insns.keys().copied().collect();
    let mut blocks: IndexMap<u64, BasicBlock> = IndexMap::new();
    let mut cur_start: Option<u64> = None;
    let mut cur: Vec<Insn> = Vec::new();

    let flush = |blocks: &mut IndexMap<u64, BasicBlock>, start: u64, body: Vec<Insn>| {
        if body.is_empty() {
            return;
        }
        let last: &Insn = body.last().unwrap();
        let terminator = last.flow;
        let next = last.next_addr();
        let successors = match terminator {
            Flow::CondJump => {
                let mut s = Vec::new();
                if let Some(t) = last.target {
                    s.push(t);
                }
                s.push(next);
                s
            }
            Flow::Jump => last.target.into_iter().collect(),
            Flow::Return | Flow::Indirect | Flow::Interrupt => Vec::new(),
            Flow::Fallthrough | Flow::Call => vec![next],
        };
        blocks.insert(
            start,
            BasicBlock {
                start,
                insns: body,
                successors,
                terminator,
            },
        );
    };

    for (i, &addr) in ordered.iter().enumerate() {
        let insn = insns[&addr].clone();
        let is_leader = leaders.contains(&addr);
        if is_leader && cur_start.is_some() {
            flush(&mut blocks, cur_start.unwrap(), std::mem::take(&mut cur));
            cur_start = None;
        }
        if cur_start.is_none() {
            cur_start = Some(addr);
        }
        let terminates = matches!(
            insn.flow,
            Flow::CondJump | Flow::Jump | Flow::Return | Flow::Indirect | Flow::Interrupt
        );
        let next_is_leader = ordered
            .get(i + 1)
            .map(|n| leaders.contains(n))
            .unwrap_or(true);
        cur.push(insn);
        if terminates || next_is_leader {
            flush(&mut blocks, cur_start.unwrap(), std::mem::take(&mut cur));
            cur_start = None;
        }
    }
    if let Some(start) = cur_start {
        flush(&mut blocks, start, std::mem::take(&mut cur));
    }

    // Attach resolved jump-table targets as successors of their indirect block.
    for blk in blocks.values_mut() {
        if blk.terminator == Flow::Indirect {
            if let Some(targets) = jump_tables.get(&blk.insns.last().unwrap().address) {
                blk.successors = targets
                    .iter()
                    .copied()
                    .filter(|t| insns.contains_key(t))
                    .collect();
            }
        }
    }

    let name = prog
        .symbol_name(entry)
        .map(|s| s.to_string())
        .unwrap_or_else(|| format!("sub_{:x}", entry));

    Some(Function {
        entry,
        name,
        blocks,
        callees,
    })
}
