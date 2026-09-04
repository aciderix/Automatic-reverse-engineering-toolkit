//! Control-flow analysis: function discovery (recursive descent) and
//! basic-block / CFG construction.
//!
//! Strategy for scaling to large binaries: decode every reachable address
//! exactly once into a global instruction map (Phase A), then partition that
//! map into functions with cheap map lookups (Phase B). This keeps the whole
//! pipeline ~O(code reached) instead of O(functions × size).

use crate::disasm::{Disassembler, Flow, Insn};
use crate::loader::Program;

pub mod gnu_eh;
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
    let (global, entries, jump_tables, prologue_only, cxx_conts) =
        global_decode(prog, disasm, prologue_scan);
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
    // The function list: every entry except cold companions (which the parent absorbs).
    let func_entries: BTreeSet<u64> = entries.difference(&cold).copied().collect();

    // False-start guard (KN-0066). Two flags-based shapes prove an address is an
    // *interior* byte wrongly promoted to a function entry (a jump-table case, an
    // address-taken interior byte, a shared bare `ret`, a stray decode target).
    // Building a function there does double harm: at runtime it can abort on a
    // flags-undefined `Jcc` (`aret_unmodelled`), and — as a truncation boundary — it
    // orphans the real enclosing function at that point, so an interior branch whose
    // target lies past the split can no longer be resolved. Dropping it lets the real
    // function collect straight through.
    //
    //  (A) the entry's *own* first instruction is a conditional branch (`Jcc`): it
    //      reads EFLAGS, and no calling convention makes flags a function live-in, so
    //      its condition depends on a `cmp`/`test` before this address — impossible at
    //      a real entry.
    //  (B) the instruction ending *exactly* at the entry is a `Jcc`: the entry is that
    //      branch's fall-through. A function never *ends* on a conditional jump (it
    //      falls through), so the `Jcc` and this address belong to the same function —
    //      the address is interior. (This is the exact `g_once` fast-path shape
    //      `mov eax,[cache]; test eax,eax; je init; ret; init: …`, where the inline
    //      `ret` was seeded as a bogus entry and truncated the function before `init`.)
    //
    // Exempt the two authoritative sources, which are proofs not guesses: a
    // direct-call target (proven callable) and an EH continuation / landing pad
    // (reached by the unwinder, already kept out of the boundary). Sound and additive:
    // real code neither opens with a bare `Jcc` nor ends a function on one, so no
    // genuine entry is dropped and the behavioural hash is unchanged; the worst case
    // for a mistakenly-kept indirect-dispatch target is a loud abort, never a
    // miscompile.
    let call_targets: BTreeSet<u64> = global
        .values()
        .filter(|i| i.flow == crate::disasm::Flow::Call)
        .filter_map(|i| i.target)
        .collect();
    // The decoded instruction that ends exactly at `addr`, taken only from the
    // authoritative reachable stream `global` (never a fresh backward decode, which
    // x86 non-self-synchronisation makes unsound) — mirrors `boundary_at`'s scan.
    let ends_at = |addr: u64| -> Option<crate::disasm::Flow> {
        (1..=15u64).find_map(|k| {
            global
                .get(&(addr - k))
                .filter(|prev| prev.next_addr() == addr)
                .map(|prev| prev.flow)
        })
    };
    // Fresh decode at the candidate (not `global.get`): the real enclosing function
    // may have decoded this region with different instruction boundaries, so `global`
    // need not hold a key exactly at `e`; a fresh decode is what the function builder
    // sees starting here.
    let false_starts: BTreeSet<u64> = func_entries
        .iter()
        .copied()
        .filter(|e| {
            if call_targets.contains(e) || cxx_conts.contains(e) {
                return false;
            }
            let opens_jcc = disasm
                .decode_at(prog, *e)
                .is_some_and(|i| i.flow == crate::disasm::Flow::CondJump);
            let after_jcc = ends_at(*e) == Some(crate::disasm::Flow::CondJump);
            opens_jcc || after_jcc
        })
        .collect();
    let func_entries: BTreeSet<u64> =
        func_entries.difference(&false_starts).copied().collect();

    // The truncation boundary drives where `collect_function` stops. A C++ catch
    // **continuation** is a resume point *inside its establisher's body* (also reached by the
    // establisher's normal control flow), so it must NOT truncate the establisher — exclude it
    // from the boundary (like `.cold`) so the establisher absorbs the shared post-try tail as its
    // own blocks, letting its interior `je`/`jne` into that tail resolve. The continuation is
    // still built as its own function (it stays in `func_entries`) for the runtime EH-resume
    // `aret_call`; duplicating the shared tail across both is sound (identical code).
    let boundary: BTreeSet<u64> = func_entries.difference(&cxx_conts).copied().collect();

    // Functions are independent (everything they read — `global`, `entries`,
    // `jump_tables`, `prog` — is shared read-only), so build them in parallel.
    use rayon::prelude::*;
    let entry_vec: Vec<u64> = func_entries.iter().copied().collect();
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
    known_prologue_bytes(&code, allow_leaf) || is_x87_leaf_thunk(prog, addr)
}

/// A lone address-taken code pointer whose target is a **frame-pointer-omitted**
/// (FPO) function opens with a non-standard prologue (`push imm`, `cmp [mem],imm`,
/// …) that `looks_like_func_start` rejects — yet the stored pointer already proves
/// the address is *taken*. The only remaining question is whether it is a genuine
/// function *start* (not an interior byte), and a clean terminator immediately
/// before it settles that: the previous function ended exactly there. Two sound
/// witnesses, both hard boundary proof:
///  - (A) an already-decoded instruction ends exactly at `addr` and is a control
///    terminator (`ret`/`ret N`/`jmp`) — the preceding function's last instruction;
///  - (B) the byte before `addr` is `int3` (0xCC) — MSVC inter-function padding,
///    which never appears as interior fall-through code.
///
/// Both prove `addr` is a real entry, so no recognised prologue is required. Sound
/// by construction: recovery from a proven boundary cannot truncate a function
/// (nothing spans a terminator), and a data word coincidentally equal to such an
/// address still lands on a true function start (worst case: a dead function, lifted
/// correctly or a sound abort — never a miscompile). Only trusted for a candidate
/// already known to be address-taken (a data-section code pointer / a stored or
/// pushed code immediate), never for a linear-scan seed.
fn preceded_by_terminator(prog: &Program, global: &BTreeMap<u64, Insn>, addr: u64) -> bool {
    if addr == 0 {
        return false;
    }
    if boundary_at(prog, global, addr) {
        return true;
    }
    // (C) NOP-padding boundary: GCC/mingw aligns the next function by filling the gap
    // after the preceding one's terminator with single-byte `nop` (0x90) — an FPO callee
    // (a `qsort`/`GCompareFunc` comparator opening `mov eax,[esp+4]`) sits right after
    // such a run, so neither (A) (the terminator is not *adjacent*) nor (B) (the adjacent
    // byte is a nop, not int3/ret) fires. Skip a *bounded* nop run (at most an alignment
    // pad, so <= 15 bytes) and require a **proven terminator** (a recovered `ret`/`jmp`,
    // or `int3`/`ret` padding) just before it. NOP is a single-byte instruction, so
    // skipping it cannot desync. Only proven terminators are accepted here — a `call`
    // (might return) or bare alignment (might be an intra-function loop-head pad) are
    // NOT boundaries: an earlier version accepted those and force-split a real libstdc++
    // function at a loop head → an infinite loop (a miscompile). Terminator-proven only.
    let mut n = 1u64;
    while n <= 15 && prog.read_from(addr - n).and_then(|b| b.first().copied()) == Some(0x90) {
        if boundary_at(prog, global, addr - n) {
            return true;
        }
        n += 1;
    }
    false
}

/// True when `addr` is a proven function boundary: a decoded terminator ends exactly
/// there (A), or the byte before it is one-byte `int3`/`ret` padding (B). See
/// `preceded_by_terminator` for the soundness argument.
fn boundary_at(prog: &Program, global: &BTreeMap<u64, Insn>, addr: u64) -> bool {
    if addr == 0 {
        return false;
    }
    // (A) a decoded terminator instruction (in the reachable stream) ends exactly at
    // `addr` — the authoritative linear decode of the preceding function. Only trust an
    // instruction already in `global`: a *fresh* decode at `addr-k` is unsound here
    // because x86 is not self-synchronising (decoding from a mid-instruction byte can
    // yield a spurious short instruction that ends at `addr` yet is not the real
    // predecessor), which both misses real terminators and could match false ones.
    for k in 1..=15u64 {
        if let Some(prev) = global.get(&(addr - k)) {
            if prev.next_addr() == addr {
                return matches!(prev.flow, Flow::Return | Flow::Jump);
            }
        }
    }
    // (B) the byte immediately before `addr` is a one-byte function terminator: `int3`
    // (0xCC) inter-function padding, or a plain `ret` (0xC3). This catches an FPO
    // callee whose *preceding* function was never recovered (so its `ret` is absent
    // from `global`, and (A) cannot see it). Both bytes are single-byte instructions,
    // so `addr` is a real boundary; combined with the address-taken pointer that
    // sourced this candidate, a coincidental interior match is vanishingly unlikely
    // (and would abort soundly, never miscompile).
    prog.read_from(addr - 1)
        .is_some_and(|b| matches!(b.first(), Some(0xCC) | Some(0xC3)))
}

/// Pure byte test for a recognised function-entry prologue (frame setup, stack
/// realignment, import thunks, CRT init guards). Split out from
/// `looks_like_func_start` so the signatures can be unit-tested directly. `code`
/// is the bytes at the candidate address; `allow_leaf` also accepts the bare
/// `mov reg,[esp+d]` leaf shape (used for stronger address-taken evidence).
fn known_prologue_bytes(code: &[u8], allow_leaf: bool) -> bool {
    if code.is_empty() {
        return false;
    }
    let b0 = code[0];
    let b1 = code.get(1).copied().unwrap_or(0);
    let b2 = code.get(2).copied().unwrap_or(0);
    matches!(b0, 0x55 | 0x53 | 0x56 | 0x57) // push ebp/ebx/esi/edi
        || b0 == 0xe9                         // jmp rel32 (tail-call thunk)
        || (b0 == 0x83 && b1 == 0xec)         // sub esp, imm8
        || (b0 == 0x81 && b1 == 0xec)         // sub esp, imm32
        || (b0 == 0x8b && b1 == 0xff)         // mov edi, edi (hot-patch pad)
        || (b0 == 0x89 && b1 == 0xff)
        // GCC/mingw frame-pointer-omitted stack-realignment prologue:
        // `lea ecx,[esp+4]; and esp,imm8` (8d 4c 24 04 83 e4 xx). A function needing
        // 16-byte stack alignment while omitting ebp opens with this (it keeps the
        // original stack in ecx for arg access) instead of `push ebp`. The 6-byte
        // signature is specific enough not to seed interior bytes; without it such a
        // function reached only through a data pointer (a `{name,func}` dispatch
        // table, as in winetest/busybox/interpreters) is unrecovered and the
        // indirect call to it aborts.
        || (b0 == 0x8d && b1 == 0x4c && b2 == 0x24 && code.get(3) == Some(&0x04)
            && code.get(4) == Some(&0x83) && code.get(5) == Some(&0xe4))
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

/// The immediate of a `mov reg, imm32` — a `.text` code address **materialised into a
/// register as a value**. That is address-taking a function pointer, even when the
/// value then reaches its call site *indirectly*: as a return value, through a runtime
/// global (a `.bss`/`.data` function-pointer slot), or a struct field — any of which
/// decouples the immediate from the eventual `call *reg`, so `reg_imm_reaches_indirect_call`
/// (needs the call in the same straight-line block) and `abs_store_imm`/`mem_store_code_imm`
/// (need a direct `mov [mem], imm`) all miss it. Materialising a function entry as a value
/// is the same strength of proof as a by-value callback push, so the caller gates `imm` on
/// a function-start witness — a recognised prologue **or** a proven boundary
/// (`preceded_by_terminator`) — so a scalar constant that merely lands in `.text` is not
/// mistaken for a function. spirv-cross's self-registering handler (`mov eax,&fn; ret`,
/// whose caller stores it in a `.bss` slot then `mov r,[slot]; call *r`) is the first wall
/// hit when driving it end-to-end; the target is reached by no direct call and sits behind a
/// runtime-installed pointer, so no other heuristic sees it.
fn reg_imm_code_value(insn: &iced_x86::Instruction) -> Option<u64> {
    use iced_x86::{Mnemonic, OpKind};
    if insn.mnemonic() == Mnemonic::Mov
        && insn.op0_kind() == OpKind::Register
        && matches!(insn.op1_kind(), OpKind::Immediate32 | OpKind::Immediate32to64)
    {
        Some(insn.immediate(1))
    } else {
        None
    }
}

/// C++ exception-handling entry recovery (MSVC/clang `__CxxFrameHandler[123]` model).
///
/// A function with try/catch installs an SEH frame whose handler is a small thunk
/// `mov eax, &FuncInfo; jmp __CxxFrameHandler[3]`. The FuncInfo's TryBlockMap points at the
/// **catch funclets**; each catch funclet, after running the catch body, returns *in eax* the
/// **continuation address** — where execution resumes in the establisher after the try/catch.
/// Neither the funclets (reached only through the EH dispatch) nor the continuations
/// (materialised only as a `mov eax,imm32` inside a funclet) are found by recursive descent,
/// the prologue scan, or the data-pointer scan — yet the program provably transfers to both.
///
/// This parses the binary's *own* EH tables to recover them as function entries: sound (nothing
/// guessed — every entry is proven by the metadata / a `mov eax,codeaddr;…;ret` the program
/// executes) and general (any MSVC-ABI C++ binary). At runtime the HLE `__CxxFrameHandler3`
/// dispatch finds the catch, runs the funclet, and resumes the returned continuation
/// (aret_seh_run), so both must exist as callable functions.
///
/// Returns `(entries, continuations)`. **Both** must be built as functions (the runtime
/// `aret_call`s the continuation), but a **continuation** is a resume point *inside the
/// establisher's own body* — the code after the try/catch, frequently also reached by the
/// establisher's normal control flow (a forward `je`/`jne` to the shared post-try tail). If it
/// acted as a function *boundary* it would truncate the establisher there, orphaning those
/// interior branch targets (their `Jcc` cannot resolve → abort). So continuations are returned
/// separately: `analyze` keeps them in the function list but **excludes them from the truncation
/// boundary**, letting the establisher absorb the shared tail as its own blocks (a sound
/// duplication — the standalone continuation function still exists for the EH-resume call). The
/// funclets, reached only via EH dispatch, are real boundaries and stay in `entries`.
fn cxx_eh_entries(prog: &Program, disasm: &Disassembler) -> (Vec<u64>, Vec<u64>) {
    let mut out = Vec::new();
    let mut conts = Vec::new();
    // A C++ frame-handler thunk is `mov eax, &FuncInfo; jmp <__CxxFrameHandler[3]>`. Its jmp
    // target is an import (IAT, `FF25`) when the CRT is dynamically linked, or an internal
    // function (`E9 rel32`) when it is statically linked (the real 1990s binaries). We therefore
    // identify the thunk *structurally* — by its FuncInfo operand (first dword = an EH magic
    // 0x19930520/21/22) plus the trailing jmp — so both cases are recovered without depending on
    // the handler being imported. (The magic + jmp shape is specific; a stray `mov eax,imm`
    // whose imm coincidentally addresses those bytes is vanishingly unlikely, and parse_cxx_func_info
    // re-checks the magic before touching the tables.)
    let is_func_info = |imm: u64| -> bool {
        matches!(prog.read_u32(imm), Some(m) if m & 0xffff_ff00 == 0x1993_0500)
    };
    let is_jmp = |addr: u64| -> bool {
        matches!(prog.read_from(addr),
                 Some(b) if b.len() >= 2 && (b[0] == 0xE9 || (b[0] == 0xFF && b[1] == 0x25)))
    };
    for sec in prog.sections.iter().filter(|s| s.executable) {
        let data = &sec.data;
        let mut i = 0usize;
        while i + 10 <= data.len() {
            if data[i] == 0xB8 {
                let func_info =
                    u32::from_le_bytes([data[i + 1], data[i + 2], data[i + 3], data[i + 4]]) as u64;
                if is_func_info(func_info) && is_jmp(sec.address + (i + 5) as u64) {
                    parse_cxx_func_info(prog, disasm, func_info, &mut out, &mut conts);
                }
            }
            i += 1;
        }
    }
    out.sort_unstable();
    out.dedup();
    conts.sort_unstable();
    conts.dedup();
    (out, conts)
}

/// Parse one MSVC C++ `FuncInfo` (`{magic, maxState, pUnwindMap, nTryBlocks, pTryBlockMap, …}`,
/// dwords) → its catch funclets + their continuation targets, appended to `out`.
fn parse_cxx_func_info(
    prog: &Program,
    disasm: &Disassembler,
    fi: u64,
    out: &mut Vec<u64>,
    conts: &mut Vec<u64>,
) {
    let magic = match prog.read_u32(fi) {
        Some(m) => m,
        None => return,
    };
    if magic & 0xffff_ff00 != 0x1993_0500 {
        return; // not an EH magic (0x19930520 / 21 / 22)
    }
    // UnwindMap {maxState, pUnwindMap}: an array of {int toState; void *action} indexed by
    // state; each non-null `action` is a destructor funclet the unwind runs. Recover them (a
    // frame can have only an UnwindMap and no try block — a function with a local that has a
    // destructor but no catch).
    let max_state = prog.read_u32(fi + 0x04).unwrap_or(0) as u64;
    let p_unwind = prog.read_u32(fi + 0x08).unwrap_or(0) as u64;
    if p_unwind != 0 && max_state <= 0x1000 {
        for s in 0..max_state {
            let action = prog.read_u32(p_unwind + s * 8 + 0x04).unwrap_or(0) as u64;
            if action != 0 && prog.is_executable(action) {
                out.push(action);
            }
        }
    }
    let n_try = prog.read_u32(fi + 0x0c).unwrap_or(0) as u64; // nTryBlocks
    let p_try = prog.read_u32(fi + 0x10).unwrap_or(0) as u64; // pTryBlockMap
    if n_try == 0 || n_try > 0x1000 {
        return;
    }
    for t in 0..n_try {
        // TryBlockMapEntry {tryLow, tryHigh, catchHigh, nCatches, pHandlerArray} (20 bytes).
        let e = p_try + t * 20;
        let n_catch = prog.read_u32(e + 0x0c).unwrap_or(0) as u64;
        let p_h = prog.read_u32(e + 0x10).unwrap_or(0) as u64;
        if n_catch > 0x1000 {
            continue;
        }
        for h in 0..n_catch {
            // HandlerType {adjectives, pType, dispCatchObj, addressOfHandler} (16 bytes).
            let handler = prog.read_u32(p_h + h * 16 + 0x0c).unwrap_or(0) as u64;
            if handler != 0 && prog.is_executable(handler) {
                out.push(handler);
                if let Some(cont) = cxx_funclet_continuation(prog, disasm, handler) {
                    conts.push(cont);
                }
            }
        }
    }
}

/// The continuation address a catch funclet resumes into: the funclet ends `mov eax, imm32;
/// …; ret`, where `imm32` is a code address in the establisher. Decode the funclet
/// instruction-aware (a raw `B8` byte can occur inside another instruction's operand) and take
/// the last `mov eax, <executable imm32>` before the terminating `ret`.
fn cxx_funclet_continuation(prog: &Program, disasm: &Disassembler, funclet: u64) -> Option<u64> {
    use iced_x86::{Mnemonic, OpKind, Register};
    let mut addr = funclet;
    let mut cont = None;
    for _ in 0..64 {
        let insn = disasm.decode_at(prog, addr)?;
        let r = &insn.raw;
        if r.mnemonic() == Mnemonic::Mov
            && r.op0_kind() == OpKind::Register
            && r.op0_register() == Register::EAX
            && matches!(r.op1_kind(), OpKind::Immediate32 | OpKind::Immediate32to64)
        {
            let v = r.immediate(1);
            if prog.is_executable(v) {
                cont = Some(v);
            }
        }
        if matches!(r.mnemonic(), Mnemonic::Ret | Mnemonic::Retf) {
            break;
        }
        addr = insn.next_addr();
    }
    cont
}

fn global_decode(
    prog: &Program,
    disasm: &Disassembler,
    prologue_scan: bool,
) -> (
    BTreeMap<u64, Insn>,
    BTreeSet<u64>,
    HashMap<u64, Vec<u64>>,
    BTreeSet<u64>,
    BTreeSet<u64>,
) {
    let mut global: BTreeMap<u64, Insn> = BTreeMap::new();
    let mut entries: BTreeSet<u64> = prog.seed_functions().into_iter().collect();
    // C++ exception handling: recover catch funclets + their catch-continuation targets from
    // the binary's own EH metadata (sound, general — see cxx_eh_entries). Reached by no direct
    // call and by no prologue/data-pointer scan, but the program provably transfers to them.
    // Continuations are tracked separately: they must be built as functions (the runtime resumes
    // them via aret_call) but must NOT act as truncation boundaries (they are resume points
    // inside the establisher's own body) — analyze() applies that distinction.
    let (cxx_funclets, cxx_conts_vec) = cxx_eh_entries(prog, disasm);
    for va in cxx_funclets.iter().chain(cxx_conts_vec.iter()) {
        entries.insert(*va);
    }
    let mut cxx_conts: BTreeSet<u64> = cxx_conts_vec.into_iter().collect();
    // GNU/Itanium C++ EH (mingw): the LSDA landing pads recovered by `gnu_eh_entries`
    // (brick 1a) are resume points *inside their establisher's body* — reached only when
    // the ARET EH dispatcher (a later brick) transfers there on a throw, by no direct
    // call or data pointer. Seed them exactly like the MSVC catch continuations: build
    // them as functions but keep them OUT of the truncation boundary, so a landing pad
    // mid-`main` does not orphan `main`'s interior branch targets. Empty on any binary
    // without an `.eh_frame` LSDA (no try/catch) -> no effect, hash unchanged.
    for f in gnu_eh::gnu_eh_entries(prog) {
        for cs in &f.call_sites {
            if cs.landing_pad != 0 && prog.is_executable(cs.landing_pad) {
                entries.insert(cs.landing_pad);
                cxx_conts.insert(cs.landing_pad);
            }
        }
    }
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
        // Compiler-certified function starts from `.eh_frame` FDE initial_locations
        // (static for this program; injected into the candidates each round below).
        // A PROOF of function start — stronger than any prologue/terminator witness,
        // and structurally free of landing pads (which never have their own FDE).
        let eh_starts = gnu_eh::eh_frame_function_starts(prog);
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
                            // A value repeating >= 3x in the run is normally a
                            // switch-case default (interior code) → needs the
                            // prologue gate. But a *bare-`ret` stub* is never a
                            // case body (a case does work then breaks): it is a
                            // no-op default method installed into many vtable
                            // slots (PuTTY's null handlers), a genuine trivial
                            // function. Accept it inside a confirmed table (>= 3
                            // code pointers) whatever its repeat count.
                            let bare_stub_in_table = in_table && is_bare_ret_stub(prog, v);
                            if !global.contains_key(&v)
                                && (trusted
                                    || bare_stub_in_table
                                    || looks_like_func_start(prog, v, false)
                                    // A lone FPO function pointer (no recognised
                                    // prologue) is still a genuine entry when a proven
                                    // terminator / int3 padding sits right before it.
                                    || preceded_by_terminator(prog, &global, v))
                            {
                                cands.insert(v);
                            } else if global.contains_key(&v)
                                && ((trusted && looks_like_func_start(prog, v, false))
                                    || bare_stub_in_table)
                            {
                                // Already decoded, but a confirmed function-pointer
                                // table (>= 3 consecutive code pointers) says `v` is a
                                // genuine function start. The linear sweep absorbed it
                                // into the preceding function by falling through a
                                // *noreturn* call (e.g. `*_and_die`/exit), so `v` is
                                // not yet an entry and the indirect call to it aborts.
                                // Force it as an entry to split at the true boundary;
                                // `looks_like_func_start` (or the bare-`ret` stub check
                                // — a table entry that is a bare `ret` is unambiguously a
                                // callable no-op method, never an interior return of a
                                // larger function) excludes interior jump-table case
                                // bodies, and jump-table targets are filtered on commit.
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
                // A code address materialised into a register as a value (`mov reg, imm`)
                // whose target is a proven function start — an address-taken function
                // pointer that reaches its call site indirectly (return value / `.bss`
                // slot / field), decoupled from the `call *reg` so the same-block call
                // rule and the direct-store rules miss it. Gated on a function-start
                // witness so a scalar constant landing in `.text` is not taken for code.
                if let Some(v) = reg_imm_code_value(&insn.raw) {
                    if in_exec(v) {
                        if !global.contains_key(&v) {
                            // Not yet decoded → seed it fresh (no split risk), gated on a
                            // function-start witness (prologue or a proven boundary).
                            if looks_like_func_start(prog, v, true)
                                || preceded_by_terminator(prog, &global, v)
                            {
                                cands.insert(v);
                            }
                        } else if preceded_by_terminator(prog, &global, v) {
                            // Already absorbed as interior code by an over-reaching
                            // predecessor (the self-referencing handler is decoded inside
                            // its own over-long neighbour). Force a re-split — but ONLY at a
                            // PROVEN boundary (`preceded_by_terminator`), never a prologue
                            // guess, so a real function is never truncated mid-body.
                            forced.insert(v);
                        }
                    }
                }
            }
            let mut progressed = jt_progress;
            // Compiler-certified starts from `.eh_frame` FDEs — a PROOF of function
            // start (a landing pad, interior to its establisher, never has its own
            // FDE, so the universal EH counterexample is structurally excluded).
            // Not yet decoded → seed fresh (no split). Decoded but interior (the
            // linear sweep over-absorbed it, e.g. by falling through a noreturn
            // `call` — the `0x7475c0` libgcc FPO case) → re-split at this proven
            // boundary via the existing `forced` path, which is exactly "confirmed
            // function starts, not prologue guesses". Gated on `in_exec`, and the
            // existing `jt_targets` guard covers the only interior address a start
            // could otherwise collide with. `eh_starts` is empty without `.eh_frame`,
            // so binaries without GCC unwind tables are unaffected (sound degradation).
            for &v in &eh_starts {
                if !in_exec(v) || jt_targets.contains(&v) {
                    continue;
                }
                if !global.contains_key(&v) {
                    cands.insert(v);
                } else if !entries.contains(&v) {
                    forced.insert(v);
                }
            }
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
    // A *directly-called* address is unambiguously a function, even when the same
    // address also appears as a jump-table default case — a bare `ret` shared
    // between a switch's default and a callable no-op stub (PuTTY installs one
    // null handler as both). Exempt direct-call targets from the jump-table-target
    // pruning, else such a function is dropped and its direct call (and any
    // indirect dispatch through a vtable slot holding it) aborts on unrecovered
    // code. A genuine bogus case-body seed is interior code, never directly
    // called, so it is still pruned.
    let call_targets: BTreeSet<u64> = global
        .values()
        .filter(|i| i.flow == Flow::Call)
        .filter_map(|i| i.target)
        .collect();
    entries.retain(|e| !jt_targets.contains(e) || call_targets.contains(e) || cxx_conts.contains(e));
    prologue_only.retain(|e| !jt_targets.contains(e));

    (global, entries, jump_tables, prologue_only, cxx_conts)
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
///
/// Also handles the MSVC memory-index idiom, where the guard compares the index
/// *in memory* and the jump register is just a reload of it:
///   `cmp [ecx+0x40], N ; ja default ; mov eax, [ecx+0x40] ; jmp [eax*4+table]`.
/// Here `cmp eax, N` never appears — the register `eax` is only a copy of the
/// bounded memory slot `[ecx+0x40]`, so we must accept the `cmp` on that same
/// memory operand. Missing this let the table over-read past its real end into a
/// neighbouring switch's entries (both executable), merging hundreds of unrelated
/// functions into one giant CFG.
fn jump_index_bound(global: &BTreeMap<u64, Insn>, jmp: &Insn, idx: iced_x86::Register) -> Option<u64> {
    use iced_x86::{Mnemonic, OpKind};
    let is_imm = |k: OpKind| matches!(k, OpKind::Immediate8 | OpKind::Immediate8to16
        | OpKind::Immediate8to32 | OpKind::Immediate16 | OpKind::Immediate32);
    // If the index register's reaching definition is a load `mov idx, [M]`, `M` is
    // the true index location a `cmp [M], N` may bound. Captured on the first write
    // to `idx` we meet scanning back (its reaching def); a non-load def leaves it None.
    let mut mem_src: Option<iced_x86::Instruction> = None;
    let mut idx_def_seen = false;
    for (_, ins) in global.range(..jmp.address).rev().take(8) {
        let r = &ins.raw;
        // Direct form: cmp idx, N.
        if r.mnemonic() == Mnemonic::Cmp
            && r.op0_kind() == OpKind::Register
            && r.op0_register().full_register() == idx
            && is_imm(r.op1_kind())
        {
            return Some(r.immediate(1).wrapping_add(1));
        }
        // Reaching def of idx: capture [M] if it is a load, then stop capturing.
        if !idx_def_seen
            && r.op0_kind() == OpKind::Register
            && r.op0_register().full_register() == idx
        {
            idx_def_seen = true;
            if r.mnemonic() == Mnemonic::Mov && r.op1_kind() == OpKind::Memory {
                mem_src = Some(*r);
            }
        }
        // Memory form: cmp [M], N on the same operand idx was loaded from.
        if let Some(m) = &mem_src {
            if r.mnemonic() == Mnemonic::Cmp
                && r.op0_kind() == OpKind::Memory
                && is_imm(r.op1_kind())
                && same_mem_operand(r, m)
            {
                return Some(r.immediate(1).wrapping_add(1));
            }
        }
    }
    None
}

/// Whether two instructions address the identical memory operand (same base,
/// index, scale, displacement and segment) — used to tie a `cmp [M], N` bound to
/// the `mov idx, [M]` reload that feeds a jump table.
fn same_mem_operand(a: &iced_x86::Instruction, b: &iced_x86::Instruction) -> bool {
    if a.is_ip_rel_memory_operand() || b.is_ip_rel_memory_operand() {
        return a.is_ip_rel_memory_operand()
            && b.is_ip_rel_memory_operand()
            && a.ip_rel_memory_address() == b.ip_rel_memory_address();
    }
    a.memory_base() == b.memory_base()
        && a.memory_index() == b.memory_index()
        && a.memory_index_scale() == b.memory_index_scale()
        && a.memory_displacement64() == b.memory_displacement64()
        && a.memory_segment() == b.memory_segment()
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

/// The callee-pop (`ret N`'s `N`) of the function at `target`, or `None` if it
/// cannot be pinned down (indirect/interrupt terminator, inconsistent `ret N`, or
/// it runs off the recovered map). Memoised. Used by `find_ret_jumps` to keep the
/// abstract esp exact across a call inside a `push imm; …; ret` body.
fn callee_ret_pop(
    global: &BTreeMap<u64, Insn>,
    target: u64,
    memo: &mut HashMap<u64, Option<u32>>,
) -> Option<u32> {
    if let Some(v) = memo.get(&target) {
        return *v;
    }
    memo.insert(target, None); // break recursion: unknown while computing
    let mut pops: BTreeSet<u32> = BTreeSet::new();
    let mut seen: BTreeSet<u64> = BTreeSet::new();
    let mut work = vec![target];
    let mut budget = 600u32;
    let mut ok = true;
    while let Some(a) = work.pop() {
        if budget == 0 {
            ok = false;
            break;
        }
        budget -= 1;
        if !seen.insert(a) {
            continue;
        }
        let Some(insn) = global.get(&a) else {
            ok = false;
            break;
        };
        match insn.flow {
            Flow::Return => {
                let inc = insn.raw.stack_pointer_increment();
                if inc < 4 {
                    ok = false;
                    break;
                }
                pops.insert((inc - 4) as u32);
            }
            Flow::Jump => match insn.target {
                Some(t) => work.push(t),
                None => {
                    ok = false;
                    break;
                }
            },
            Flow::CondJump => {
                if let Some(t) = insn.target {
                    work.push(t);
                }
                work.push(insn.next_addr());
            }
            Flow::Indirect | Flow::Interrupt => {
                ok = false;
                break;
            }
            Flow::Fallthrough | Flow::Call => work.push(insn.next_addr()),
        }
    }
    // A plain `ret` (N=0) inside a function that also has `ret N` (N>0) is a
    // push-ret JUMP, not a return — one function has a single calling convention, so
    // its real returns all pop the same N. Ignore the N=0 rets when any N>0 exists.
    let nonzero: Vec<u32> = pops.iter().copied().filter(|&n| n > 0).collect();
    let r = if !ok {
        None
    } else if nonzero.is_empty() {
        Some(0) // all plain `ret` → cdecl (caller cleans) or a pure push-ret helper
    } else if nonzero.len() == 1 {
        Some(nonzero[0])
    } else {
        None // conflicting `ret N` → can't pin the pop
    };
    memo.insert(target, r);
    r
}

/// Recognise the `push <code>; … ; ret` idiom — a `ret` used as a computed JUMP to
/// an in-function code address pushed earlier (the MSVC `__finally` continuation /
/// local-unwind tail). Returns `{ret_addr -> jump_target}`.
///
/// **Sound by construction.** A forward abstract interpretation tracks the stack
/// symbolically (each slot is either a specific pushed in-function code constant or
/// opaque). A `ret` is converted only when, on EVERY path reaching it, `[esp]` is
/// proven to be the *same* pushed code constant — which is exactly the address the
/// hardware `ret` pops and jumps to. A genuine return (whose `[esp]` is a caller
/// address, opaque here) is never converted. Any unmodelled esp effect (an indirect
/// call, `mov esp,…`, a misaligned stack write) merely makes slots opaque — never a
/// wrong constant — so the pass can only *miss* a jump, never invent one.
fn find_ret_jumps(
    insns: &BTreeMap<u64, Insn>,
    global: &BTreeMap<u64, Insn>,
    entry: u64,
    func_end: u64,
) -> HashMap<u64, u64> {
    use iced_x86::{Mnemonic, OpKind, Register};
    const MAXD: usize = 48;
    type Stk = Vec<Option<u64>>;

    let mut memo: HashMap<u64, Option<u32>> = HashMap::new();
    // A valid continuation is a real instruction start at or ahead of `entry`,
    // within a generous single-function span. It need NOT be in `insns`: the pushed
    // address is frequently mis-promoted to its own "function" entry (address-taken),
    // so the true continuation sits exactly at `func_end`. Looseness here is safe —
    // the abstract interpretation is the real gate (it converts a `ret` only when
    // `[esp]` is PROVABLY this pushed constant on every path; a genuine return, whose
    // `[esp]` is an opaque caller address, is never marked). `func_end` bounds the
    // search but is not trusted as the span limit.
    let _ = func_end;
    let is_cont = |imm: u64| global.contains_key(&imm) && imm >= entry && imm - entry < 0x8000;

    // Transfer: the abstract stack after `insn` executes (index 0 = top = [esp]).
    let transfer = |stk: &Stk, insn: &Insn, memo: &mut HashMap<u64, Option<u32>>| -> Stk {
        // `push <in-function code imm>` establishes a known top even if esp was opaque.
        if insn.raw.mnemonic() == Mnemonic::Push && insn.raw.op0_kind() == OpKind::Immediate32 {
            let imm = insn.raw.immediate32() as u64;
            let mut n = stk.clone();
            n.insert(0, if is_cont(imm) { Some(imm) } else { None });
            n.truncate(MAXD);
            return n;
        }
        // esp effect in stack slots: >0 pops from the top, <0 pushes opaque slots,
        // `None` = unknown => clear the tracked prefix (top becomes opaque).
        let delta: Option<i64> = if insn.flow == Flow::Call {
            insn.target.and_then(|t| callee_ret_pop(global, t, memo)).map(|m| m as i64 / 4)
        } else {
            let inc = insn.raw.stack_pointer_increment() as i64;
            let writes_esp_nonstack = insn.raw.op0_kind() == OpKind::Register
                && matches!(insn.raw.op0_register(), Register::ESP | Register::SP)
                && inc == 0;
            if writes_esp_nonstack || inc % 4 != 0 {
                None
            } else {
                Some(inc / 4)
            }
        };
        let mut n = match delta {
            None => Vec::new(),
            Some(d) if d >= 0 => {
                let mut n = stk.clone();
                for _ in 0..(d as usize).min(n.len()) {
                    n.remove(0);
                }
                n
            }
            Some(d) => {
                let mut n = stk.clone();
                for _ in 0..(-d) as usize {
                    n.insert(0, None);
                }
                n.truncate(MAXD);
                n
            }
        };
        // A store through `[esp+disp]` overwrites a tracked slot: opaque it (or, if
        // misaligned, clear — the write may span slots).
        if insn.raw.op0_kind() == OpKind::Memory
            && matches!(insn.raw.memory_base(), Register::ESP)
            && insn.raw.is_ip_rel_memory_operand() == false
        {
            let disp = insn.raw.memory_displacement64() as i64;
            if disp < 0 || disp % 4 != 0 {
                n.clear();
            } else {
                let k = (disp / 4) as usize;
                if k < n.len() {
                    n[k] = None;
                }
            }
        }
        n
    };

    // Join two abstract stacks (align at the top): equal constants survive, all
    // else becomes opaque; the result keeps the shorter length. Returns whether
    // `into` changed. `None` slot beyond a stack's length is the opaque floor.
    fn join(into: &mut Option<Stk>, incoming: &Stk) -> bool {
        match into {
            None => {
                *into = Some(incoming.clone());
                true
            }
            Some(cur) => {
                let newlen = cur.len().min(incoming.len());
                let mut changed = cur.len() != newlen;
                cur.truncate(newlen);
                for i in 0..newlen {
                    let m = if cur[i] == incoming[i] { cur[i] } else { None };
                    if m != cur[i] {
                        cur[i] = m;
                        changed = true;
                    }
                }
                changed
            }
        }
    }

    let mut state: HashMap<u64, Option<Stk>> = HashMap::new();
    state.insert(entry, Some(Vec::new()));
    let mut work = vec![entry];
    let mut budget = 20_000u32;
    while let Some(a) = work.pop() {
        if budget == 0 {
            return HashMap::new(); // pathological: bail, convert nothing (sound)
        }
        budget -= 1;
        let Some(insn) = insns.get(&a) else { continue };
        let Some(Some(s_in)) = state.get(&a).cloned() else { continue };
        let s_out = transfer(&s_in, insn, &mut memo);
        let succs: Vec<u64> = match insn.flow {
            Flow::CondJump => insn.target.into_iter().chain(std::iter::once(insn.next_addr())).collect(),
            Flow::Jump => insn.target.into_iter().collect(),
            Flow::Return | Flow::Indirect | Flow::Interrupt => Vec::new(),
            Flow::Fallthrough | Flow::Call => vec![insn.next_addr()],
        };
        for succ in succs {
            if !insns.contains_key(&succ) {
                continue;
            }
            if join(state.entry(succ).or_insert(None), &s_out) {
                work.push(succ);
            }
        }
    }

    let mut out = HashMap::new();
    for (&addr, insn) in insns {
        if insn.flow == Flow::Return
            && insn.raw.mnemonic() == Mnemonic::Ret
            && insn.raw.op_count() == 0
        {
            if let Some(Some(st)) = state.get(&addr) {
                if let Some(Some(imm)) = st.first() {
                    if is_cont(*imm) {
                        out.insert(addr, *imm);
                    }
                }
            }
        }
    }
    out
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

    let mut insns = collect_function(global, entry, &boundary, jump_tables, prologue_only);
    if insns.is_empty() {
        return None;
    }

    // `push <code>; … ; ret` (ret-as-jump, MSVC `__finally` continuation): rewrite
    // each proven such `ret` into a `jmp` to the pushed address, so block leaders,
    // successors and the lift all treat it as the jump it really is (build.rs adds
    // the `esp += 4` the `ret` still pops). Sound: see `find_ret_jumps`.
    let func_end = boundary.range((entry + 1)..).next().copied().unwrap_or(u64::MAX);
    let ret_jumps = find_ret_jumps(&insns, global, entry, func_end);
    for (&ra, &tgt) in &ret_jumps {
        if let Some(insn) = insns.get_mut(&ra) {
            insn.flow = Flow::Jump;
            insn.target = Some(tgt);
        }
        // The continuation is reachable only through this newly-recognised edge, so
        // collect its instructions (the epilogue tail) into the function now.
        if !insns.contains_key(&tgt) {
            for (a, i) in collect_function(global, tgt, &boundary, jump_tables, prologue_only) {
                insns.entry(a).or_insert(i);
            }
        }
    }

    // A tail `call <helper>; ret` whose lone `ret` was independently recovered as a
    // bare-`ret` stub (its address is taken elsewhere — e.g. an EH/vtable table)
    // turns that `ret`'s address into a truncating boundary: `collect_function`
    // stops *before* this function's own epilogue. The `call` block is then left
    // with a fall-through that is absent from the function, `build_ir` finds no
    // `Return` terminator, and `emit` synthesises a fallback `return 0` — silently
    // dropping a value the function had already placed in eax (an MSVC constructor's
    // `return this`, set right before `call _EH_epilog3`). Pull such a lone epilogue
    // `ret`/`ret N` back in: for a `call` whose fall-through is a boundary that
    // decodes to a single `Return`, absorb that one instruction. Sound — it is
    // exactly what the hardware executes after the call returns; the bare-`ret` stub
    // keeps its own (harmless, one-byte) recovery for its address-taken use.
    let stolen_epilogues: Vec<u64> = insns
        .values()
        .filter(|i| i.flow == Flow::Call)
        .map(|i| i.next_addr())
        .filter(|ft| !insns.contains_key(ft) && boundary.contains(ft))
        .filter(|ft| matches!(global.get(ft), Some(ri) if ri.flow == Flow::Return))
        .collect();
    for ft in stolen_epilogues {
        insns.insert(ft, global[&ft].clone());
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

#[cfg(test)]
mod prologue_tests {
    use super::known_prologue_bytes;

    #[test]
    fn recognises_gcc_stack_realign_prologue() {
        // `lea ecx,[esp+4]; and esp,-8` — GCC/mingw frame-pointer-omitted
        // realignment (kernel32_test.exe's test functions, reached only via a
        // {name,func} dispatch table). Must be recognised or the indirect call aborts.
        assert!(known_prologue_bytes(&[0x8d, 0x4c, 0x24, 0x04, 0x83, 0xe4, 0xf8], false));
        assert!(known_prologue_bytes(&[0x8d, 0x4c, 0x24, 0x04, 0x83, 0xe4, 0xf0], false));
    }

    #[test]
    fn still_recognises_classic_prologues() {
        assert!(known_prologue_bytes(&[0x55], false)); // push ebp
        assert!(known_prologue_bytes(&[0x83, 0xec, 0x20], false)); // sub esp,imm8
        assert!(known_prologue_bytes(&[0xff, 0x25, 0, 0, 0, 0], false)); // jmp [mem]
    }

    #[test]
    fn rejects_non_prologues() {
        assert!(known_prologue_bytes(&[], false).eq(&false));
        // `lea ecx,[esp+8]` (wrong disp) is not the realignment form.
        assert!(!known_prologue_bytes(&[0x8d, 0x4c, 0x24, 0x08, 0x83, 0xe4, 0xf8], false));
        // `lea ecx,[esp+4]` NOT followed by `and esp` is not the realignment form.
        assert!(!known_prologue_bytes(&[0x8d, 0x4c, 0x24, 0x04, 0x8b, 0x01], false));
        // random data
        assert!(!known_prologue_bytes(&[0x00, 0x11, 0x22, 0x33], false));
        // the leaf `mov reg,[esp+d]` shape is gated by allow_leaf
        assert!(!known_prologue_bytes(&[0x8b, 0x44, 0x24], false));
        assert!(known_prologue_bytes(&[0x8b, 0x44, 0x24], true));
    }
}
