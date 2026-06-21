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
/// Used to filter address-taken candidates (a data word that points into code):
/// the value already being a valid code address is the primary signal, this is
/// the secondary one. Kept deliberately tight (common entry prologues only, no
/// `push imm`/`mov eax,imm` which are far more frequent mid-body) to avoid
/// seeding misaligned data as bogus functions.
fn looks_like_func_start(prog: &Program, addr: u64) -> bool {
    let Some(code) = prog.read_from(addr) else { return false };
    let b0 = code[0];
    let b1 = code.get(1).copied().unwrap_or(0);
    matches!(b0, 0x55 | 0x53 | 0x56 | 0x57) // push ebp/ebx/esi/edi
        || b0 == 0xe9                         // jmp rel32 (tail-call thunk)
        || (b0 == 0x83 && b1 == 0xec)         // sub esp, imm8
        || (b0 == 0x81 && b1 == 0xec)         // sub esp, imm32
        || (b0 == 0x8b && b1 == 0xff)         // mov edi, edi (hot-patch pad)
        || (b0 == 0x89 && b1 == 0xff)
        || (b0 == 0xff && b1 == 0x25) // jmp [mem] (import thunk)
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
        // a vtable slot, or a registration/dispatch table (Lua's `luaL_Reg`
        // arrays, etc.) — is found by neither recursive descent (no direct call)
        // nor the `push ebp` prologue scan. Scan pointer-aligned section data for
        // values that point at an as-yet-undecoded, plausible function start.
        // The `!global.contains_key` gate keeps us from splitting a function we
        // already decoded, so reachable code (the regression corpus) is untouched.
        let ptr = (prog.bitness.bits() / 8) as usize;
        let exec: Vec<(u64, u64)> = prog
            .sections
            .iter()
            .filter(|s| s.executable)
            .map(|s| (s.address, s.address + s.data.len() as u64))
            .collect();
        let in_exec = |a: u64| a != 0 && exec.iter().any(|&(lo, hi)| a >= lo && a < hi);
        let mut taken: VecDeque<u64> = VecDeque::new();
        for sec in &prog.sections {
            let d = &sec.data;
            let mut off = 0usize;
            while off + ptr <= d.len() {
                let v = match ptr {
                    8 => u64::from_le_bytes(d[off..off + 8].try_into().unwrap()),
                    _ => u32::from_le_bytes([d[off], d[off + 1], d[off + 2], d[off + 3]]) as u64,
                };
                if in_exec(v)
                    && !global.contains_key(&v)
                    && looks_like_func_start(prog, v)
                    && entries.insert(v)
                {
                    prologue_only.insert(v);
                    taken.push_back(v);
                }
                off += ptr;
            }
        }
        drain(prog, disasm, &mut global, &mut entries, &mut jump_tables, &mut taken);
    }

    // Jump-table resolution is order-sensitive: it scans the instructions
    // *preceding* an indirect `jmp` for the table idiom, so if a `jmp` is first
    // decoded as another path's target (before its own `lea/movsxd/add` setup
    // exists), the inline attempt in `drain` fails and is never retried. Re-run
    // resolution to a fixpoint now that all reachable code is decoded, decoding
    // any newly discovered case targets.
    loop {
        let found: Vec<(u64, Vec<u64>)> = global
            .iter()
            .filter(|(addr, insn)| insn.flow == Flow::Indirect && !jump_tables.contains_key(addr))
            .filter_map(|(addr, insn)| {
                resolve_jump_table(prog, insn)
                    .or_else(|| resolve_pie_jump_table(prog, &global, insn))
                    .or_else(|| resolve_abs_jump_table(prog, &global, insn))
                    .map(|t| (*addr, t))
            })
            .collect();
        if found.is_empty() {
            break;
        }
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
            drain(prog, disasm, &mut global, &mut entries, &mut jump_tables, &mut newwork);
        }
    }

    (global, entries, jump_tables, prologue_only)
}

/// Recognise a jump-table dispatch `jmp [table + idx*ptr]` and read its target
/// list from the binary. Conservative: pointer-sized entries, an absolute (or
/// rip-relative) table base, entries kept while they point into executable code.
fn resolve_jump_table(prog: &Program, insn: &Insn) -> Option<Vec<u64>> {
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

    read_jump_table(prog, table, ptr as u64)
}

/// Read a pointer-sized table of absolute code addresses at `table`, in index
/// order *with duplicates preserved* (the structured emitter maps `case k ->
/// successors[k]`, so collapsing duplicate targets — common when several switch
/// labels share a body — would shift every later case onto the wrong block).
/// Stops at the first non-code word (table end); needs >= 2 distinct targets to
/// count as a real dispatch table.
fn read_jump_table(prog: &Program, table: u64, ptr: u64) -> Option<Vec<u64>> {
    let mut targets = Vec::new();
    let mut distinct = BTreeSet::new();
    for i in 0..1024u64 {
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
            return read_jump_table(prog, table, ptr as u64);
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
                resolve_jump_table(prog, &insn)
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
