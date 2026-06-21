//! Build a typed IR control-flow graph from a recovered `analysis::Function`,
//! and pretty-print IR for inspection (`--mode ir`).
//!
//! Per-instruction compute is produced by `ir::lift`; this layer adds the block
//! terminators (`Branch`/`Jump`/`Return`) that need CFG block ids, deriving each
//! conditional branch's condition from the CPU flags (read as IR `Flag`
//! locations) according to the `jcc` condition code.
//!
//! Parallel to the working text pipeline; reachable via `--mode ir`.
#![allow(dead_code)]

use super::lift::{cc_to_cond, lift};
use super::types::*;
use crate::analysis::Function;
use crate::disasm::Flow;
use crate::loader::Program;
use std::collections::HashMap;
use std::fmt::Write;

/// Lift a recovered function into an SSA-ready IR CFG (pre-SSA: `Set`/`Read`).
pub fn build_ir(prog: &Program, func: &Function) -> IrFunction {
    let bits = prog.bitness.bits();
    let order: Vec<u64> = func.blocks.keys().copied().collect();
    let idx: HashMap<u64, u32> = order
        .iter()
        .enumerate()
        .map(|(i, &a)| (a, i as u32))
        .collect();

    // Static x87 FPU stack analysis: maps each modelled FPU instruction to its
    // (stack depth before, truncation-mode) so `lift_x87` can resolve `st(i)` to
    // a concrete slot. `None` if the function should not be x87-modelled (any
    // unmodelled FPU op, ambiguous depth, or a float→int store whose rounding we
    // cannot prove) — those instructions then degrade to a sound `Asm`.
    let (x87, fp_calls) = match x87_states(func) {
        Some((ops, calls)) => (Some(ops), calls),
        None => (None, HashMap::new()),
    };
    // Does this function itself return an fp value (st(0))? Then store st(0) into
    // the fp return channel at each `ret`, so callers can recover it.
    let self_returns_fp = FP_RETURNING.with(|c| c.borrow().contains(&func.entry));
    // Keep every stack access as raw `__frame` memory (no named scalars) when the
    // function spills 80-bit x87 values OR 128-bit XMM registers: those go through
    // byte-addressed memory, and a function may read the same bytes back at a
    // finer granularity (e.g. 4-byte packed-float lanes). Named scalars would
    // alias-split those accesses; raw `__frame` keeps them byte-consistent.
    // OR in any caller-forced setting (the transpiler's shared-stack mode forces
    // frames off so `[ebp+d]` incoming arguments stay raw loads from the shared
    // stack instead of becoming undefined named locals).
    let raw_frames = x87.is_some() || uses_xmm128_mem(func) || crate::ir::lift::frames_off();
    crate::ir::lift::set_frames_off(raw_frames);

    let mut blocks: Vec<Block> = Vec::with_capacity(order.len());
    for (i, &addr) in order.iter().enumerate() {
        let blk = &func.blocks[&addr];
        let mut stmts: Vec<Stmt> = Vec::new();

        let is_control = matches!(
            blk.terminator,
            Flow::CondJump | Flow::Jump | Flow::Return | Flow::Indirect | Flow::Interrupt
        );
        let body_len = if is_control {
            blk.insns.len().saturating_sub(1)
        } else {
            blk.insns.len()
        };
        for insn in &blk.insns[..body_len] {
            let mut s = match x87.as_ref().and_then(|m| m.get(&insn.address)) {
                Some(&(sp_in, trunc)) => crate::ir::lift::lift_x87(insn, sp_in, trunc),
                None => lift(insn, bits),
            };
            fold_ro_loads(&mut s, insn, prog);
            stmts.extend(s);
            // A call to a fp-returning function leaves its result in st(0): recover
            // it from the fp return channel into the slot the depth analysis says
            // it lands in, so subsequent x87 ops read the real value (not undef).
            if let Some(&depth) = fp_calls.get(&insn.address) {
                if let Some(slot) = crate::ir::lift::x87_slot(depth as i32) {
                    stmts.push(Stmt::Set { dst: slot, expr: crate::ir::lift::x87_ret_load() });
                }
            }
        }

        // Internal successors (block indices), in CFG order.
        let succ: Vec<u32> = blk
            .successors
            .iter()
            .filter_map(|t| idx.get(t).copied())
            .collect();

        // Terminator.
        match blk.terminator {
            Flow::CondJump => {
                let jcc = blk.insns.last().unwrap();
                let taken = blk.successors.first().and_then(|t| idx.get(t).copied());
                let fall = blk.successors.get(1).and_then(|t| idx.get(t).copied());
                match (taken, fall) {
                    (Some(t), Some(f)) => stmts.push(Stmt::Branch {
                        cond: cc_to_cond(jcc.raw.condition_code()),
                        taken: BlockId(t),
                        fallthrough: BlockId(f),
                    }),
                    _ => stmts.push(Stmt::Asm(jcc.text.clone())),
                }
            }
            Flow::Jump => {
                let last = blk.insns.last().unwrap();
                let t = blk.successors.first().and_then(|t| idx.get(t).copied());
                match t {
                    Some(t) => stmts.push(Stmt::Jump(BlockId(t))),
                    // No internal target: a direct jump to a function entry (or a
                    // resolved external symbol) is a tail call — `return f(args)`.
                    None => {
                        let ct = match (last.target, &last.call_name) {
                            (Some(target), _) if prog.is_executable(target) => {
                                Some(CallTarget::Direct(target))
                            }
                            (_, Some(name)) => Some(CallTarget::Named(name.clone())),
                            _ => None,
                        };
                        match ct {
                            Some(ct) => stmts.push(Stmt::Return(Some(tail_call(ct, bits)))),
                            None => stmts.push(Stmt::Asm(last.text.clone())),
                        }
                    }
                }
            }
            // Model the return value as the result register (rax/eax family),
            // so its computation stays live (analogous to the text pipeline's
            // `return eax`).
            Flow::Return => {
                // An fp-returning function leaves st(0) (slot 0, since the terminal
                // x87 depth is 1) in the fp return channel for its caller.
                if self_returns_fp {
                    if let Some(slot) = crate::ir::lift::x87_slot(0) {
                        stmts.push(crate::ir::lift::x87_ret_store(Expr::Read(slot)));
                    }
                }
                stmts.push(Stmt::Return(Some(Expr::Read(Location::Reg(RegId(0))))));
            }
            Flow::Indirect => {
                let last = blk.insns.last().unwrap();
                // A resolved jump table (the analysis attached case edges) with an
                // index register becomes a typed switch.
                if !succ.is_empty() {
                    // Index from the jump's own memory operand (`jmp [t+idx*8]`),
                    // or recovered from the PIE idiom (`jmp reg`).
                    let value = crate::ir::lift::switch_index(&last.raw)
                        .or_else(|| pie_switch_index(func, &last.raw))
                        .or_else(|| abs_switch_index(func, &last.raw));
                    match value {
                        Some(value) => {
                            let cases = succ
                                .iter()
                                .enumerate()
                                .map(|(i, &b)| (i as i128, BlockId(b)))
                                .collect();
                            stmts.push(Stmt::Switch { value, cases, default: BlockId(succ[0]) });
                        }
                        None => stmts.push(Stmt::Asm(last.text.clone())),
                    }
                } else if last.raw.is_ip_rel_memory_operand()
                    && prog.import_name(last.raw.ip_rel_memory_address()).is_some()
                {
                    // jmp qword [rip+GOT] to an import is a tail call to it.
                    let name = prog.import_name(last.raw.ip_rel_memory_address()).unwrap();
                    stmts.push(Stmt::Return(Some(tail_call(CallTarget::Named(name.to_string()), bits))));
                } else if last.raw.op_kind(0) == iced_x86::OpKind::Register
                    && crate::ir::lift::switch_index(&last.raw).is_none()
                    && pie_switch_index(func, &last.raw).is_none()
                {
                    // `jmp reg` with no jump-table idiom is an indirect tail call
                    // through a function pointer: `return (*reg)(args)`. (An
                    // *unresolved* table — index idiom present but no case edges —
                    // falls through to the sound `Asm` below instead.)
                    match crate::ir::lift::reg_value(last.raw.op_register(0)) {
                        Some(target) => stmts.push(Stmt::Return(Some(tail_call(
                            CallTarget::Indirect(Box::new(target)),
                            bits,
                        )))),
                        None => stmts.push(Stmt::Asm(last.text.clone())),
                    }
                } else {
                    stmts.push(Stmt::Asm(last.text.clone()));
                }
            }
            Flow::Interrupt => stmts.push(Stmt::Asm(blk.insns.last().unwrap().text.clone())),
            Flow::Fallthrough | Flow::Call => {
                if let Some(&t) = succ.first() {
                    stmts.push(Stmt::Jump(BlockId(t)));
                }
            }
        }

        blocks.push(Block {
            id: i as u32,
            addr,
            stmts,
            succ,
            pred: Vec::new(),
        });
    }

    // Predecessors by inverting successor edges.
    let preds_of: Vec<Vec<u32>> = {
        let mut p = vec![Vec::new(); blocks.len()];
        for b in &blocks {
            for &s in &b.succ {
                p[s as usize].push(b.id);
            }
        }
        p
    };
    for (b, preds) in blocks.iter_mut().zip(preds_of) {
        b.pred = preds;
    }

    // Name resolved import / PLT call targets (e.g. Direct(plt) -> "malloc"),
    // and intercept indirect import calls. Compilers often load an import's IAT
    // slot into a (callee-saved) register once and `call reg` repeatedly, so we
    // track which registers currently hold an import function pointer and resolve
    // calls through them too. The tracking is a simple forward scan invalidated
    // on reassignment / opaque `Asm`.
    {
        let mut held: std::collections::HashMap<Location, String> = std::collections::HashMap::new();
        for b in &mut blocks {
            for s in &mut b.stmts {
                name_calls_in_stmt(s, prog, bits, &held);
                update_import_regs(s, prog, &mut held);
            }
        }
    }

    // Reset the per-function frame-folding flag for the next function/thread.
    crate::ir::lift::set_frames_off(false);

    IrFunction {
        entry: func.entry,
        name: func.name.clone(),
        bits,
        reg_params: Vec::new(),
        frame_base_values: Vec::new(),
        fp80_values: Vec::new(),
        blocks,
        next_value: 0,
        next_temp: 0,
    }
}

/// Does the function access 128-bit (16-byte) memory via an XMM register? Such
/// spills are written as two 8-byte halves but may be read back at a finer
/// granularity, so the function must keep frame slots as raw `__frame` memory.
fn uses_xmm128_mem(func: &Function) -> bool {
    func.blocks.values().any(|b| {
        b.insns.iter().any(|i| {
            i.raw.memory_size().size() == 16
                && (0..i.raw.op_count()).any(|k| i.raw.op_register(k).is_xmm())
        })
    })
}

thread_local! {
    /// Entry addresses of functions that return a floating-point value (in
    /// `st(0)` per the x87 ABI). A `call` to one of these pushes onto the x87
    /// stack, which the depth analysis must count. Installed once per transpile by
    /// `set_fp_returning`; empty for the verify/decompile paths (calls then
    /// modelled as x87-neutral, i.e. the prior behavior — no differential change).
    static FP_RETURNING: std::cell::RefCell<std::collections::HashSet<u64>> =
        std::cell::RefCell::new(std::collections::HashSet::new());
}

/// Install the set of fp-returning function entry addresses for x87 depth
/// tracking on this thread (computed by `compute_fp_returning`).
pub fn set_fp_returning(set: std::collections::HashSet<u64>) {
    FP_RETURNING.with(|c| *c.borrow_mut() = set);
}

/// Does the instruction call a function known to return an fp value in `st(0)`?
/// Only resolved for direct near calls; indirect calls are treated as x87-neutral.
fn call_returns_fp(ins: &iced_x86::Instruction, fp: &std::collections::HashSet<u64>) -> bool {
    use iced_x86::FlowControl;
    if ins.flow_control() != FlowControl::Call {
        return false;
    }
    fp.contains(&ins.near_branch_target())
}

/// A genuine bail of the x87 depth analysis: an unmodelled op, unprovable
/// rounding, a stack under/overflow, or an ambiguous join depth. The function's
/// FPU ops then fall back to a sound `Asm` (flagged INCOMPLETE).
struct X87Bail;

/// Result of the x87 depth pass: per-op `(depth_before, needs_trunc)`, the
/// per-fp-call `address -> depth the result lands at`, and the terminal depth at
/// each reached `ret`.
struct X87Info {
    ops: HashMap<u64, (u32, bool)>,
    fp_calls: HashMap<u64, u32>,
    ret_depths: Vec<i32>,
}

/// Static x87 stack-depth + rounding-mode analysis (the thread-local
/// `FP_RETURNING` set models fp-returning calls as a push). Returns the per-op
/// depth map and the fp-call sites, or `None` to disable x87 modelling for the
/// whole function.
fn x87_states(func: &Function) -> Option<(HashMap<u64, (u32, bool)>, HashMap<u64, u32>)> {
    FP_RETURNING.with(|c| {
        x87_depth_pass(func, &c.borrow())
            .ok()
            .map(|i| (i.ops, i.fp_calls))
    })
}

/// Does `func` return an fp value? True iff every reachable `ret` leaves exactly
/// one value on the x87 stack (the `st(0)` return). Used by `compute_fp_returning`.
fn func_returns_fp(func: &Function, fp: &std::collections::HashSet<u64>) -> bool {
    match x87_depth_pass(func, fp) {
        Ok(i) => !i.ret_depths.is_empty() && i.ret_depths.iter().all(|&d| d == 1),
        Err(_) => false,
    }
}

/// Compute the fixpoint of fp-returning functions: a function returns fp if its
/// terminal x87 depth is 1, which can depend on fp-returning callees, so iterate
/// until stable. Cheap (the set only grows, bounded by the function count).
pub fn compute_fp_returning(funcs: &[&Function]) -> std::collections::HashSet<u64> {
    let mut set = std::collections::HashSet::new();
    loop {
        let mut changed = false;
        for f in funcs {
            if !set.contains(&f.entry) && func_returns_fp(f, &set) {
                set.insert(f.entry);
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }
    set
}

/// One forward propagation of the x87 stack depth over the CFG. Returns the
/// per-op `(depth_before, needs_trunc)` map plus the terminal depth at each
/// reached `ret` block (for fp-return detection).
fn x87_depth_pass(
    func: &Function,
    fp: &std::collections::HashSet<u64>,
) -> Result<X87Info, X87Bail> {
    use crate::ir::lift::{is_x87, x87_delta};
    use iced_x86::Mnemonic;

    if !func.blocks.values().any(|b| b.insns.iter().any(|i| is_x87(&i.raw))) {
        return Err(X87Bail);
    }

    let order: Vec<u64> = func.blocks.keys().copied().collect();
    let bidx: HashMap<u64, usize> = order.iter().enumerate().map(|(i, &a)| (a, i)).collect();
    let n = order.len();

    // Forward propagation of the entry stack depth over the CFG (-1 = unvisited).
    let mut entry_sp: Vec<i32> = vec![-1; n];
    let entry_i = *bidx.get(&func.entry).unwrap_or(&0);
    entry_sp[entry_i] = 0;
    let mut work = vec![entry_i];
    let mut out: HashMap<u64, (u32, bool)> = HashMap::new();
    let mut fp_calls: HashMap<u64, u32> = HashMap::new();
    let mut ret_depths: Vec<i32> = Vec::new();

    while let Some(bi) = work.pop() {
        let blk = &func.blocks[&order[bi]];
        let mut sp = entry_sp[bi];
        for (k, insn) in blk.insns.iter().enumerate() {
            let ins = &insn.raw;
            // A call to an fp-returning function leaves its result in st(0); the
            // x87 ABI keeps the stack otherwise empty across calls, so this single
            // push is the only adjustment a call needs. Record where the result
            // lands (`sp` before the push) so the lifter can recover it from the
            // fp return channel.
            if call_returns_fp(ins, fp) {
                if sp < 0 {
                    return Err(X87Bail);
                }
                fp_calls.insert(insn.address, sp as u32);
                sp += 1;
                if !(0..=8).contains(&sp) {
                    return Err(X87Bail);
                }
                continue;
            }
            if !is_x87(ins) {
                continue;
            }
            // unmodelled FPU op → bail whole function
            let delta = x87_delta(ins).ok_or(X87Bail)?;
            // float→int store (`fist`/`fistp`, but not the always-truncating
            // `fisttp`) is only sound when the rounding mode is truncation.
            let needs_trunc = matches!(ins.mnemonic(), Mnemonic::Fist | Mnemonic::Fistp);
            if needs_trunc && !truncate_active(&blk.insns, k) {
                return Err(X87Bail);
            }
            if !(0..=8).contains(&sp) {
                return Err(X87Bail);
            }
            out.insert(insn.address, (sp as u32, needs_trunc));
            sp += delta;
            if !(0..=8).contains(&sp) {
                return Err(X87Bail);
            }
        }
        // Terminal depth at a `ret`: an fp-returning function leaves st(0) here.
        if matches!(blk.terminator, crate::disasm::Flow::Return) {
            ret_depths.push(sp);
        }
        for &s in &blk.successors {
            if let Some(&si) = bidx.get(&s) {
                if entry_sp[si] == -1 {
                    entry_sp[si] = sp;
                    work.push(si);
                } else if entry_sp[si] != sp {
                    return Err(X87Bail); // ambiguous stack depth at a join
                }
            }
        }
    }
    Ok(X87Info { ops: out, fp_calls, ret_depths })
}

/// Is the x87 rounding mode proven to be truncation at instruction `idx` of
/// `insns`? Recognises the canonical `(int)x` idiom: an `or` that sets the
/// control word's RC=11 (truncate) bits, followed by `fldcw`, before this store.
fn truncate_active(insns: &[crate::disasm::Insn], idx: usize) -> bool {
    use iced_x86::{Mnemonic, OpKind};
    let lo = idx.saturating_sub(24);
    let mut saw_fldcw = false;
    for j in (lo..idx).rev() {
        let ins = &insns[j].raw;
        match ins.mnemonic() {
            Mnemonic::Fldcw => saw_fldcw = true,
            Mnemonic::Or if saw_fldcw => {
                // Immediate operand setting RC=truncate (control-word bits 0xc00).
                if matches!(ins.op_kind(1), OpKind::Immediate8 | OpKind::Immediate8to16
                    | OpKind::Immediate8to32 | OpKind::Immediate16 | OpKind::Immediate32)
                {
                    let imm = ins.immediate(1);
                    let hi_byte = matches!(
                        ins.op_register(0),
                        iced_x86::Register::AH | iced_x86::Register::BH
                            | iced_x86::Register::CH | iced_x86::Register::DH
                    );
                    if (imm & 0xc00) == 0xc00 || (hi_byte && (imm & 0x0c) == 0x0c) {
                        return true;
                    }
                }
            }
            _ => {}
        }
    }
    false
}

/// Fold a rip-relative load of read-only data into a literal. Without this,
/// `movss xmm,[rip+c]` (a float/integer constant in `.rodata`) emits a
/// dereference of an address that is a placeholder in an object file and unmapped
/// in a standalone recompile — silently crashing. The constant comes from the
/// relocation's captured data (object files) or from reading the absolute
/// read-only address directly (linked executables).
fn fold_ro_loads(stmts: &mut [Stmt], insn: &crate::disasm::Insn, prog: &Program) {
    let ins = &insn.raw;
    if !ins.is_ip_rel_memory_operand() {
        return;
    }
    let abs = ins.ip_rel_memory_address();
    let size = ins.memory_size().size();
    let reloc = prog.reloc_in(insn.address, insn.len);
    // A relocation to writable/unresolved data must stay a load.
    if reloc.is_some() && reloc.and_then(|r| r.data).is_none() {
        return;
    }
    // Low 8 bytes at `abs`, and (for a 128-bit load) the next 8 at `abs+8`. The
    // value comes from the relocation's captured read-only bytes (object files)
    // or from reading the absolute read-only address (linked executables).
    let mut fold_half = |stmts: &mut [Stmt], byte_off: u64, reloc_val: Option<u64>| {
        if size as u64 <= byte_off {
            return;
        }
        let value = match reloc {
            Some(_) => match reloc_val {
                Some(v) => v,
                None => return,
            },
            None => {
                if prog.section_at(abs + byte_off).map(|s| !s.writable).unwrap_or(false) {
                    match prog.read_u64(abs + byte_off) {
                        Some(v) => v,
                        None => return,
                    }
                } else {
                    return;
                }
            }
        };
        let bits = (((size as u64 - byte_off).min(8) * 8).min(64)) as u8;
        let masked = if bits >= 64 { value as i128 } else { (value & ((1u64 << bits) - 1)) as i128 };
        for st in stmts.iter_mut() {
            fold_const_load(st, (abs + byte_off) as i128, masked, bits);
        }
    };
    fold_half(stmts, 0, reloc.and_then(|r| r.data));
    fold_half(stmts, 8, reloc.and_then(|r| r.datahi));
}

/// Replace `Load { addr: Const(`bogus`) }` with `Const(val)` throughout `s`.
fn fold_const_load(s: &mut Stmt, bogus: i128, val: i128, bits: u8) {
    fn go(e: &mut Expr, bogus: i128, val: i128, bits: u8) {
        if let Expr::Load { addr, .. } = e {
            if matches!(addr.as_ref(), Expr::Const(c, _) if *c == bogus) {
                *e = Expr::Const(val, Ty::int(bits));
                return;
            }
            go(addr, bogus, val, bits);
            return;
        }
        match e {
            Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => go(x, bogus, val, bits),
            Expr::Binary(_, a, b) => {
                go(a, bogus, val, bits);
                go(b, bogus, val, bits);
            }
            Expr::Select { cond, then_, else_ } => {
                go(cond, bogus, val, bits);
                go(then_, bogus, val, bits);
                go(else_, bogus, val, bits);
            }
            Expr::Call { target, args, .. } => {
                if let CallTarget::Indirect(x) = target {
                    go(x, bogus, val, bits);
                }
                for a in args {
                    go(a, bogus, val, bits);
                }
            }
            _ => {}
        }
    }
    match s {
        Stmt::Set { expr, .. } | Stmt::Assign { expr, .. } | Stmt::CallStmt(expr) => {
            go(expr, bogus, val, bits)
        }
        Stmt::Store { addr, value, .. } => {
            go(addr, bogus, val, bits);
            go(value, bogus, val, bits);
        }
        Stmt::Branch { cond, .. } => go(cond, bogus, val, bits),
        Stmt::Switch { value, .. } => go(value, bogus, val, bits),
        Stmt::Return(Some(e)) => go(e, bogus, val, bits),
        _ => {}
    }
}

/// A tail call to `target`: `Call(target, SysV-arg-registers)`. The over-
/// approximated argument registers are pruned/fixed up later; direct import
/// targets are renamed by `name_calls_in_stmt`. Returned as the function value.
fn tail_call(target: CallTarget, bits: u32) -> Expr {
    let args = if bits == 64 {
        [7u16, 6, 2, 1, 8, 9]
            .iter()
            .map(|&r| Expr::Read(Location::Reg(RegId(r))))
            .collect()
    } else {
        Vec::new()
    };
    Expr::Call { target, args, ret: Ty::int(bits as u8) }
}

/// Recover the switch index of a PIE relative jump table (`movsxd tgt,[base+
/// idx*4]; add tgt,base; jmp tgt`): a read of the `movsxd`'s index register.
fn pie_switch_index(func: &Function, jmp: &iced_x86::Instruction) -> Option<Expr> {
    use iced_x86::{Mnemonic, OpKind, Register};
    // PIE idiom: `movsxd tgt, [base + idx*4]; add tgt, base; jmp tgt`. The index
    // is the memory index of the `movsxd` whose destination is the jump's own
    // target register. That `movsxd` may sit in a *predecessor* block (when the
    // `jmp` is itself a jump-table case target, hence a block leader), so scan the
    // function's instructions in a small window before the jump — not just the
    // jump's own block. Constraining the destination to the jump register keeps it
    // precise (no unrelated `movsxd` can be mistaken for the index source).
    if jmp.op0_kind() != OpKind::Register {
        return None;
    }
    let tgt = jmp.op0_register().full_register();
    let jmp_addr = jmp.ip();
    let mut window: Vec<&crate::disasm::Insn> = func
        .blocks
        .values()
        .flat_map(|b| b.insns.iter())
        .filter(|i| i.address < jmp_addr)
        .collect();
    window.sort_by_key(|i| i.address);
    let mov = window.iter().rev().take(10).find(|i| {
        i.raw.mnemonic() == Mnemonic::Movsxd
            && i.raw.memory_index() != Register::None
            && i.raw.memory_index_scale() == 4
            && i.raw.op0_register().full_register() == tgt
    })?;
    crate::ir::lift::reg_value(mov.raw.memory_index())
}

/// Recover the switch index of an absolute-table computed goto (`mov tgt,
/// [table + idx*ptr]; jmp tgt`): a read of the load's index register. Mirrors
/// `pie_switch_index` but for the load-then-jump form (GCC `&&label` dispatch).
fn abs_switch_index(func: &Function, jmp: &iced_x86::Instruction) -> Option<Expr> {
    use iced_x86::{Mnemonic, OpKind, Register};
    if jmp.op0_kind() != OpKind::Register {
        return None;
    }
    let tgt = jmp.op0_register().full_register();
    let jmp_addr = jmp.ip();
    let mut window: Vec<&crate::disasm::Insn> = func
        .blocks
        .values()
        .flat_map(|b| b.insns.iter())
        .filter(|i| i.address < jmp_addr)
        .collect();
    window.sort_by_key(|i| i.address);
    let mov = window.iter().rev().take(10).find(|i| {
        let r = &i.raw;
        r.mnemonic() == Mnemonic::Mov
            && r.op0_kind() == OpKind::Register
            && r.op0_register().full_register() == tgt
            && r.op1_kind() == OpKind::Memory
            && r.memory_index() != Register::None
            && r.memory_base() == Register::None
    })?;
    crate::ir::lift::reg_value(mov.raw.memory_index())
}

// --- pretty-printing ------------------------------------------------------

fn loc_str(l: &Location) -> String {
    match l {
        Location::Reg(r) => format!("r{}", r.0),
        Location::Flag(k) => format!("{:?}", k),
        Location::Frame(d) => format!("frame[{}]", d),
        Location::Mem => "mem".into(),
        Location::Temp(t) => format!("t{}", t),
    }
}

/// Rewrite `Call { target: Direct(addr) }` to `Named(import)` when `addr` is a
/// resolved PLT/IAT import. Internal `sub_*` targets are left as Direct so they
/// keep getting forward-declared (recompilable).
type HeldImports = std::collections::HashMap<Location, String>;

fn name_calls_in_expr(e: &mut Expr, prog: &Program, bits: u32, held: &HeldImports) {
    match e {
        Expr::Call { target, args, .. } => {
            if let CallTarget::Direct(a) = target {
                if let Some(name) = prog.import_name(*a) {
                    *target = CallTarget::Named(sanitize_import(name));
                } else if let Some(name) = prog.import_thunk(*a).map(|s| s.to_string()) {
                    // `call <thunk>` where the thunk tail-jumps through an IAT slot:
                    // bind to the import shim here, at the genuine call site. In
                    // 32-bit the shim reads its cdecl args off the machine stack.
                    *target = CallTarget::Named(sanitize_import(&name));
                    if bits == 32 && args.is_empty() {
                        *args = vec![Expr::Read(Location::Reg(RegId(4)))];
                    }
                } else if crate::emit::shared_stack() {
                    // Statically-linked CRT recognized by symbol → bind to the
                    // native shim (only when transpiling; keeps decompile output
                    // structurally faithful). The shim reads its cdecl arguments
                    // off the shared machine stack via esp, like an intercepted
                    // import, so hand it the stack pointer.
                    if let Some(name) = prog.crt_symbol(*a) {
                        *target = CallTarget::Named(sanitize_import(name));
                        if bits == 32 && args.is_empty() {
                            *args = vec![Expr::Read(Location::Reg(RegId(4)))];
                        }
                    } else if prog.is_startup_glue(*a) {
                        // mingw/MSVC startup glue (ctor/dtor runners, EH-frame,
                        // pseudo-reloc) → native no-op, so `main` can run.
                        *target = CallTarget::Named("aret_noop".to_string());
                        if bits == 32 && args.is_empty() {
                            *args = vec![Expr::Read(Location::Reg(RegId(4)))];
                        }
                    }
                }
            }
            // Indirect import call (UBT Phase 3 / API interception). Two shapes:
            //   * `call [abs]` — a direct dereference of a fixed IAT/GOT slot;
            //   * `call reg`   — through a register previously loaded from an IAT
            //     slot (`mov reg, [abs]`), tracked in `held`.
            // For 32-bit stdcall/cdecl the arguments live on the modelled stack,
            // so hand the shim the current stack pointer; it reads its arguments
            // at esp+0, esp+4, … (ABI-accurate).
            let import: Option<String> = if let CallTarget::Indirect(inner) = &*target {
                if let Some(a) = const_load_addr(inner) {
                    prog.import_name(a).map(sanitize_import)
                } else if let Some(loc) = peeled_reg(inner) {
                    held.get(loc).cloned()
                } else {
                    None
                }
            } else {
                None
            };
            if let Some(name) = import {
                *target = CallTarget::Named(name);
                if bits == 32 && args.is_empty() {
                    *args = vec![Expr::Read(Location::Reg(RegId(4)))];
                }
            }
            if let CallTarget::Indirect(x) = target {
                name_calls_in_expr(x, prog, bits, held);
            }
            for a in args.iter_mut() {
                name_calls_in_expr(a, prog, bits, held);
            }
        }
        Expr::Load { addr, .. } => name_calls_in_expr(addr, prog, bits, held),
        Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => name_calls_in_expr(x, prog, bits, held),
        Expr::Binary(_, a, b) => {
            name_calls_in_expr(a, prog, bits, held);
            name_calls_in_expr(b, prog, bits, held);
        }
        Expr::Select { cond, then_, else_ } => {
            name_calls_in_expr(cond, prog, bits, held);
            name_calls_in_expr(then_, prog, bits, held);
            name_calls_in_expr(else_, prog, bits, held);
        }
        _ => {}
    }
}

/// Update the register→import tracking after statement `s`: record `reg = [iat]`
/// loads of import slots, and invalidate a register when it is reassigned (or on
/// an opaque `Asm`, which may clobber anything).
fn update_import_regs(s: &Stmt, prog: &Program, held: &mut HeldImports) {
    match s {
        Stmt::Set { dst, expr } => {
            if let Some(a) = const_load_addr(expr) {
                if let Some(name) = prog.import_name(a) {
                    held.insert(dst.clone(), sanitize_import(name));
                    return;
                }
            }
            held.remove(dst);
        }
        Stmt::Asm(_) => held.clear(),
        _ => {}
    }
}

/// Address of a `call [abs]` slot: matches `Load { addr: Const(a) }` (an indirect
/// call through a fixed memory location, i.e. a PE IAT or ELF GOT slot).
fn const_load_addr(e: &Expr) -> Option<u64> {
    if let Expr::Load { addr, .. } = e {
        if let Expr::Const(a, _) = addr.as_ref() {
            return Some(*a as u64);
        }
    }
    None
}

/// The register a (possibly width-masked / cast) read resolves to. A 32-bit
/// register read in the 64-bit-wide IR is `(reg & 0xffffffff)`, so peel that and
/// any cast to recover the underlying `Reg` for import-pointer tracking.
fn peeled_reg(e: &Expr) -> Option<&Location> {
    match e {
        Expr::Read(loc) => Some(loc),
        Expr::Cast { expr, .. } => peeled_reg(expr),
        Expr::Binary(BinOp::And, a, b) => {
            // `reg & all-ones-mask` — a width truncation, not a real mask.
            if let Expr::Const(m, _) = b.as_ref() {
                let m = *m as u64;
                if m == 0xff || m == 0xffff || m == 0xffff_ffff || m == u64::MAX {
                    return peeled_reg(a);
                }
            }
            None
        }
        _ => None,
    }
}

/// Turn a raw import symbol into the HLE shim's C identifier: drop `@N` stdcall
/// decoration / non-identifier characters and prefix `aret_`. The prefix keeps
/// shim names (e.g. `aret_printf`, `aret_malloc`) from colliding with the real
/// libc functions the shims themselves call.
pub(crate) fn sanitize_import(name: &str) -> String {
    let stem = name
        .trim_start_matches('_')
        .split(['@', '+'])
        .next()
        .unwrap_or(name);
    let s: String = stem
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() || c == '_' { c } else { '_' })
        .collect();
    format!("aret_{}", s)
}

/// UBT M3 (shared machine stack): give every internal `call` the caller's stack
/// pointer minus one slot, modelling the return address the real `call` pushes.
/// The callee (compiled assuming that pushed slot) then reads its stack arguments
/// at `[__esp + 4]`, `[__esp + 8]`, … from the single shared stack — which is
/// exactly where the caller wrote them. Imports (now `Named`) are untouched: HLE
/// shims read their arguments at `[esp + 0]`. 32-bit only (64-bit passes
/// arguments in registers).
pub fn thread_internal_call_esp(irf: &mut IrFunction) {
    if irf.bits != 32 {
        return;
    }
    for b in &mut irf.blocks {
        for s in &mut b.stmts {
            thread_calls_in_stmt(s);
        }
    }
}

fn esp_minus_slot() -> Expr {
    Expr::Binary(
        BinOp::Sub,
        Box::new(Expr::Read(Location::Reg(RegId(4)))),
        Box::new(Expr::Const(4, Ty::int(32))),
    )
}

/// Arguments handed to an internal `call` in shared-stack mode: the pushed-return
/// stack pointer, then the volatile general registers eax/ecx/edx (matching the
/// callee's fixed parameter list, so both stack- and register-passed arguments
/// are conveyed).
fn internal_call_args() -> Vec<Expr> {
    vec![
        esp_minus_slot(),
        Expr::Read(Location::Reg(RegId(0))), // eax
        Expr::Read(Location::Reg(RegId(1))), // ecx
        Expr::Read(Location::Reg(RegId(2))), // edx
    ]
}

/// Arguments for an internal *tail* call (a `jmp` to a function entry). Unlike a
/// `call`, a `jmp` pushes no return address: the callee reuses the current frame,
/// so it must receive the stack pointer **as-is** (not `esp - 4`). Passing
/// `esp - 4` would shift every stack argument the callee reads by one slot — the
/// bug that made a `push ebp;mov ebp,esp;pop ebp;jmp f` thunk hand `f` the wrong
/// frame.
fn internal_tailcall_args() -> Vec<Expr> {
    vec![
        Expr::Read(Location::Reg(RegId(4))), // esp unchanged (no pushed return addr)
        Expr::Read(Location::Reg(RegId(0))), // eax
        Expr::Read(Location::Reg(RegId(1))), // ecx
        Expr::Read(Location::Reg(RegId(2))), // edx
    ]
}

fn thread_calls_in_expr(e: &mut Expr) {
    match e {
        Expr::Call { target, args, .. } => {
            // Direct internal calls and indirect (function-pointer) calls both take
            // the fixed machine context. Imports are already `Named` (handled in
            // name_calls with just the stack pointer), so they are not touched.
            if matches!(target, CallTarget::Direct(_) | CallTarget::Indirect(_)) {
                if let CallTarget::Indirect(x) = target {
                    thread_calls_in_expr(x);
                }
                *args = internal_call_args();
            }
            for a in args.iter_mut() {
                thread_calls_in_expr(a);
            }
        }
        Expr::Load { addr, .. } => thread_calls_in_expr(addr),
        Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => thread_calls_in_expr(x),
        Expr::Binary(_, a, b) => {
            thread_calls_in_expr(a);
            thread_calls_in_expr(b);
        }
        Expr::Select { cond, then_, else_ } => {
            thread_calls_in_expr(cond);
            thread_calls_in_expr(then_);
            thread_calls_in_expr(else_);
        }
        _ => {}
    }
}

fn thread_calls_in_stmt(s: &mut Stmt) {
    match s {
        Stmt::Set { expr, .. } | Stmt::Assign { expr, .. } | Stmt::CallStmt(expr) => {
            thread_calls_in_expr(expr)
        }
        Stmt::Store { addr, value, .. } => {
            thread_calls_in_expr(addr);
            thread_calls_in_expr(value);
        }
        Stmt::Branch { cond, .. } => thread_calls_in_expr(cond),
        // A `Return(Call)` is a tail call (`jmp f`): the outermost call must get
        // the stack pointer as-is (no pushed return slot). Nested calls inside its
        // arguments are ordinary calls and keep the `esp - 4` model.
        Stmt::Return(Some(e)) => {
            if let Expr::Call { target, args, .. } = e {
                if matches!(target, CallTarget::Direct(_) | CallTarget::Indirect(_)) {
                    if let CallTarget::Indirect(x) = target {
                        thread_calls_in_expr(x);
                    }
                    *args = internal_tailcall_args();
                    for a in args.iter_mut() {
                        thread_calls_in_expr(a);
                    }
                    return;
                }
            }
            thread_calls_in_expr(e)
        }
        _ => {}
    }
}

fn name_calls_in_stmt(s: &mut Stmt, prog: &Program, bits: u32, held: &HeldImports) {
    match s {
        Stmt::Set { expr, .. } | Stmt::Assign { expr, .. } | Stmt::CallStmt(expr) => {
            name_calls_in_expr(expr, prog, bits, held)
        }
        Stmt::Store { addr, value, .. } => {
            name_calls_in_expr(addr, prog, bits, held);
            name_calls_in_expr(value, prog, bits, held);
        }
        Stmt::Branch { cond, .. } => name_calls_in_expr(cond, prog, bits, held),
        Stmt::Return(Some(e)) => name_calls_in_expr(e, prog, bits, held),
        _ => {}
    }
}

fn ty_str(t: &Ty) -> String {
    match t {
        Ty::Int { bits, .. } => format!("i{}", bits),
        Ty::Ptr(_) => "ptr".into(),
        Ty::Float { bits } => format!("f{}", bits),
        Ty::Bool => "bool".into(),
        Ty::Code => "code".into(),
        Ty::Aggregate(_) => "agg".into(),
        Ty::Unknown => "?".into(),
    }
}

fn target_str(t: &CallTarget) -> String {
    match t {
        CallTarget::Direct(a) => format!("sub_{:x}", a),
        CallTarget::Named(n) => n.clone(),
        CallTarget::Indirect(e) => format!("(*{})", expr_str(e)),
    }
}

fn expr_str(e: &Expr) -> String {
    match e {
        Expr::Const(v, _) => {
            if *v < 0 || *v > 9 {
                format!("0x{:x}", *v as i64 as u64)
            } else {
                format!("{}", v)
            }
        }
        Expr::Read(l) => loc_str(l),
        Expr::Use(v) => format!("v{}", v.0),
        Expr::Load { addr, ty } => format!("{}*[{}]", ty_str(ty), expr_str(addr)),
        Expr::Unary(op, x) => format!("{:?}({})", op, expr_str(x)),
        Expr::Binary(op, a, b) => format!("({} {} {})", expr_str(a), op, expr_str(b)),
        Expr::Cast { to, expr } => format!("({}){}", ty_str(to), expr_str(expr)),
        Expr::Addr(l) => format!("&{}", loc_str(l)),
        Expr::Call { target, args, .. } => {
            let a: Vec<String> = args.iter().map(expr_str).collect();
            format!("{}({})", target_str(target), a.join(", "))
        }
        Expr::Select { cond, then_, else_ } => {
            format!("({} ? {} : {})", expr_str(cond), expr_str(then_), expr_str(else_))
        }
        Expr::Phi(args) => {
            let a: Vec<String> = args.iter().map(|v| format!("v{}", v.0)).collect();
            format!("phi({})", a.join(", "))
        }
        Expr::Undef => "undef".into(),
    }
}

fn stmt_str(s: &Stmt) -> String {
    match s {
        Stmt::Set { dst, expr } => format!("{} = {}", loc_str(dst), expr_str(expr)),
        Stmt::Assign { dst, expr } => format!("v{} = {}", dst.0, expr_str(expr)),
        Stmt::Store { addr, value, ty } => {
            format!("{}*[{}] = {}", ty_str(ty), expr_str(addr), expr_str(value))
        }
        Stmt::Branch {
            cond,
            taken,
            fallthrough,
        } => format!(
            "if ({}) goto B{} else B{}",
            expr_str(cond),
            taken.0,
            fallthrough.0
        ),
        Stmt::Jump(b) => format!("goto B{}", b.0),
        Stmt::Switch { value, default, .. } => {
            format!("switch ({}) default B{}", expr_str(value), default.0)
        }
        Stmt::Return(Some(e)) => format!("return {}", expr_str(e)),
        Stmt::Return(None) => "return".into(),
        Stmt::CallStmt(e) => format!("{};", expr_str(e)),
        Stmt::Asm(t) => format!("asm({:?})", t),
        Stmt::Nop => "nop".into(),
    }
}

/// Render an IR function as text for inspection.
pub fn dump(f: &IrFunction) -> String {
    let mut out = String::new();
    let _ = writeln!(out, "// {}  @ 0x{:x}  ({} blocks, {} values)", f.name, f.entry, f.blocks.len(), f.next_value);
    // Constraint-based type inference (roadmap §5): annotate values whose type
    // could be narrowed past a plain 64-bit scalar (pointers, code pointers,
    // signed/unsigned ints). Display-only — emission is unaffected.
    let types = crate::types::infer(f);
    if !types.is_empty() {
        let mut items: Vec<(&u32, &crate::ir::types::Ty)> = types.iter().collect();
        items.sort_by_key(|(k, _)| **k);
        let shown: Vec<String> = items
            .iter()
            .map(|(k, t)| format!("v{}: {}", k, crate::types::ty_name(t)))
            .collect();
        let _ = writeln!(out, "// inferred types: {}", shown.join(", "));
    }
    // Aggregate reconstruction (roadmap §5.3): bases accessed at multiple
    // offsets are synthesised as structs. Display-only.
    for agg in crate::types::recover_aggregates(f) {
        let _ = writeln!(out, "// {}", crate::types::render_struct(&agg));
    }
    // Constant-stride indexed accesses are arrays (roadmap §5.4 m.4). Display-only.
    for arr in crate::types::recover_arrays(f) {
        let _ = writeln!(out, "// {}", crate::types::render_array(&arr));
    }
    for b in &f.blocks {
        let succ: Vec<String> = b.succ.iter().map(|s| format!("B{}", s)).collect();
        let _ = writeln!(out, "B{} (0x{:x})  -> [{}]:", b.id, b.addr, succ.join(", "));
        for s in &b.stmts {
            let _ = writeln!(out, "    {}", stmt_str(s));
        }
    }
    out
}
