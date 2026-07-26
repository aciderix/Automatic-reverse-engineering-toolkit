//! SSA construction over the typed IR (roadmap §3.4).
//!
//! Standard Cytron et al. algorithm, reusing the shared dominator/dominance-
//! frontier toolkit (`crate::cfg::dom`):
//!
//! 1. Place φ-nodes for each versioned [`Location`] at the iterated dominance
//!    frontier of its definition sites.
//! 2. Rename every definition to a fresh [`ValueId`] and every use to the
//!    reaching version, walking the dominator tree.
//!
//! Registers, flags, and lifter temporaries are versioned; memory (`Load`/
//! `Store`) is left untouched (handled later by alias analysis / stack
//! promotion). Built in parallel with the working pipeline; not yet wired in.
#![allow(dead_code)]

use crate::cfg::dom;
use crate::ir::types::*;
use std::collections::{HashMap, HashSet};

/// A φ-node: a versioned `var`, the value it defines, and one argument per CFG
/// predecessor (in predecessor order).
#[derive(Clone, Debug)]
struct Phi {
    var: Location,
    dst: ValueId,
    args: Vec<ValueId>,
}

fn is_versioned(l: &Location) -> bool {
    matches!(l, Location::Reg(_) | Location::Flag(_) | Location::Temp(_))
}

/// An x87 FPU stack slot (`RegId` 96..104) or swap scratch (104) — its SSA
/// values are `long double`.
fn is_fp80_loc(l: &Location) -> bool {
    matches!(l, Location::Reg(RegId(r)) if (96..112).contains(r))
}

fn fresh(counter: &mut u32) -> ValueId {
    let v = ValueId(*counter);
    *counter += 1;
    v
}

/// Transform `func` into SSA form in place.
pub fn to_ssa(func: &mut IrFunction) {
    if func.blocks.is_empty() {
        return;
    }
    // Block ids are assumed to equal their index; entry is the block at the
    // function entry address (fall back to index 0).
    let entry0 = func
        .blocks
        .iter()
        .position(|b| b.addr == func.entry)
        .unwrap_or(0);

    // --- entry-block splitting -------------------------------------------
    // If the entry block has predecessors, it is a loop header (every pred of
    // the entry is a back-edge — the entry dominates the whole function). A
    // loop-header phi needs an argument for the *entry edge* carrying the
    // initial (parameter/undef) value; but the entry block has no predecessor
    // block for that edge, so the phi would be built with back-edge args only
    // and the initial value is lost (the induction variable — often a
    // register parameter, e.g. `eax` in a regparm function like sqlite3's
    // `sqlite3ExprAffinity` — never advances → wrong value / infinite loop).
    // Fix: insert an empty pre-header that takes over the entry VA and jumps to
    // the old header. The header now has a clean entry edge (from the
    // pre-header), so its phis get a proper initial-value argument. Standard
    // SSA construction on an irreducible-looking entry.
    if !func.blocks[entry0].pred.is_empty() {
        let ph_idx = func.blocks.len() as u32; // id == index
        // Give the old header a fresh synthetic address (guaranteed unique and
        // != func.entry) so `addr == func.entry` now resolves to the pre-header.
        let synth = func.blocks.iter().map(|b| b.addr).max().unwrap_or(0) + 1;
        func.blocks[entry0].addr = synth;
        func.blocks[entry0].pred.push(ph_idx);
        func.blocks.push(Block {
            id: ph_idx,
            addr: func.entry,
            stmts: vec![Stmt::Jump(BlockId(entry0 as u32))],
            succ: vec![entry0 as u32],
            pred: vec![],
        });
    }

    let n = func.blocks.len();
    let entry = func
        .blocks
        .iter()
        .position(|b| b.addr == func.entry)
        .unwrap_or(0);

    let succ: Vec<Vec<usize>> = func
        .blocks
        .iter()
        .map(|b| b.succ.iter().map(|&x| x as usize).collect())
        .collect();
    let preds: Vec<Vec<usize>> = func
        .blocks
        .iter()
        .map(|b| b.pred.iter().map(|&x| x as usize).collect())
        .collect();

    let idom = dom::dominators(n, entry, &succ, &preds);
    let df = dom::dominance_frontiers(n, &preds, &idom);

    // Dominator-tree children.
    let mut children = vec![Vec::new(); n];
    for i in 0..n {
        if i != entry && idom[i] != dom::UNDEF && idom[i] != i {
            children[idom[i]].push(i);
        }
    }

    // --- 1. φ placement ---------------------------------------------------
    // Definition sites per variable.
    let mut defsites: HashMap<Location, Vec<usize>> = HashMap::new();
    for (b, blk) in func.blocks.iter().enumerate() {
        for s in &blk.stmts {
            if let Stmt::Set { dst, .. } = s {
                if is_versioned(dst) {
                    defsites.entry(dst.clone()).or_default().push(b);
                }
            }
        }
    }

    let mut phis: Vec<Vec<Phi>> = vec![Vec::new(); n];
    for (var, sites) in &defsites {
        let mut has_phi: HashSet<usize> = HashSet::new();
        let mut worklist: Vec<usize> = sites.clone();
        while let Some(b) = worklist.pop() {
            for &d in &df[b] {
                if has_phi.insert(d) {
                    let arg_count = preds[d].len();
                    phis[d].push(Phi {
                        var: var.clone(),
                        dst: ValueId(0), // assigned during renaming
                        args: vec![ValueId(u32::MAX); arg_count],
                    });
                    if !sites.contains(&d) {
                        worklist.push(d);
                    }
                }
            }
        }
    }

    // --- 2. Renaming ------------------------------------------------------
    let mut counter: u32 = 0;
    let mut stacks: HashMap<Location, Vec<ValueId>> = HashMap::new();
    let mut undef: HashMap<Location, ValueId> = HashMap::new();
    // SSA values that version an x87 FPU slot → emitted as `long double`.
    let mut fp80: Vec<u32> = Vec::new();

    // Take the statements out so we can mutate blocks and read successors freely.
    let mut block_stmts: Vec<Vec<Stmt>> =
        func.blocks.iter_mut().map(|b| std::mem::take(&mut b.stmts)).collect();

    // Iterative dominator-tree traversal with explicit pop markers.
    enum Step {
        Enter(usize),
        Pop(Vec<Location>),
    }
    let mut work = vec![Step::Enter(entry)];

    while let Some(step) = work.pop() {
        match step {
            Step::Pop(vars) => {
                for v in vars {
                    if let Some(s) = stacks.get_mut(&v) {
                        s.pop();
                    }
                }
            }
            Step::Enter(b) => {
                let mut pushed: Vec<Location> = Vec::new();
                let top = |stacks: &HashMap<Location, Vec<ValueId>>,
                           undef: &mut HashMap<Location, ValueId>,
                           counter: &mut u32,
                           v: &Location|
                 -> ValueId {
                    if let Some(s) = stacks.get(v) {
                        if let Some(&x) = s.last() {
                            return x;
                        }
                    }
                    *undef.entry(v.clone()).or_insert_with(|| {
                        let id = ValueId(*counter);
                        *counter += 1;
                        id
                    })
                };

                // φ-nodes define new versions first.
                for phi in &mut phis[b] {
                    let nv = fresh(&mut counter);
                    phi.dst = nv;
                    if is_fp80_loc(&phi.var) {
                        fp80.push(nv.0);
                    }
                    stacks.entry(phi.var.clone()).or_default().push(nv);
                    pushed.push(phi.var.clone());
                }

                // Rewrite the block's statements.
                let mut new_stmts = Vec::with_capacity(block_stmts[b].len());
                for stmt in std::mem::take(&mut block_stmts[b]) {
                    match stmt {
                        Stmt::Set { dst, expr } if is_versioned(&dst) => {
                            let e = rewrite_reads(expr, &stacks, &mut undef, &mut counter);
                            let nv = fresh(&mut counter);
                            if is_fp80_loc(&dst) {
                                fp80.push(nv.0);
                            }
                            stacks.entry(dst.clone()).or_default().push(nv);
                            pushed.push(dst.clone());
                            new_stmts.push(Stmt::Assign { dst: nv, expr: e });
                        }
                        other => {
                            new_stmts.push(rewrite_stmt(other, &stacks, &mut undef, &mut counter));
                        }
                    }
                }
                block_stmts[b] = new_stmts;

                // Fill φ arguments in successors for this predecessor slot.
                for &s in &succ[b] {
                    let j = preds[s].iter().position(|&p| p == b).unwrap();
                    for phi in &mut phis[s] {
                        phi.args[j] = top(&stacks, &mut undef, &mut counter, &phi.var);
                    }
                }

                // Schedule pop after children, then enter children.
                work.push(Step::Pop(pushed));
                for &c in &children[b] {
                    work.push(Step::Enter(c));
                }
            }
        }
    }

    // Prepend φ-nodes (as Assign of an Expr::Phi) to each block, write back.
    for b in 0..n {
        let mut stmts = Vec::new();
        for phi in &phis[b] {
            stmts.push(Stmt::Assign {
                dst: phi.dst,
                expr: Expr::Phi(phi.args.clone()),
            });
        }
        stmts.append(&mut block_stmts[b]);
        func.blocks[b].stmts = stmts;
    }
    func.next_value = counter;

    // Register-passed parameters (64-bit ABIs): an argument register read
    // before being written (its entry/undef version is referenced) is a
    // parameter. System V order; Win64's rcx,rdx,r8,r9 is a prefix of it minus
    // rsi/rdi, so this is a reasonable superset for recovery.
    if func.bits == 64 {
        // RegId order matches ir::lift::reg_id: rcx=1, rdx=2, rsi=6, rdi=7, r8=8, r9=9.
        let sysv = [7u16, 6, 2, 1, 8, 9];
        for r in sysv {
            if let Some(v) = undef.get(&Location::Reg(RegId(r))) {
                func.reg_params.push(v.0);
            }
        }
    } else if crate::emit::shared_stack() {
        // 32-bit shared machine stack (UBT M3): besides the stack, pass the
        // volatile general registers eax/ecx/edx so register-passed arguments
        // (gcc `-O1` regparm, `__fastcall`) cross calls, AND the callee-saved
        // registers ebp/esi/edi/ebx. ebp: a *frameless* helper that uses the
        // caller's frame without a `push ebp; mov ebp,esp` prologue (the MSVC EH
        // funclets that share their parent's frame and read `[ebp+x]`) must inherit
        // the caller's ebp. esi/edi/ebx: standard code save/restores them (so their
        // incoming value is dead), but some optimised MSVC/MFC helpers receive an
        // argument in one — e.g. a constructor whose `this` arrives in ESI and is
        // used (`mov [esi],vtable`) without a save. Without threading them, such a
        // helper reads 0 (its SSA entry/undef) and dereferences NULL. Threading is
        // the correct ABI and strictly additive: a helper that only save/restores
        // esi is unaffected (its incoming value flows through the push/pop), and the
        // caller keeps its own esi across the call (callee-saved) since the call
        // does not reassign the caller's SSA value. The list is FIXED so every
        // function has the same parameter positions; a register not read before
        // being written gets an unused placeholder value to keep the position
        // stable. Order: eax,ecx,edx,ebp then esi,edi,ebx (RegId 6,7,3).
        for r in [0u16, 1, 2, 5, 6, 7, 3] {
            match undef.get(&Location::Reg(RegId(r))).copied() {
                Some(v) => func.reg_params.push(v.0),
                None => {
                    let d = func.fresh_value();
                    func.reg_params.push(d.0);
                }
            }
        }
    }

    // Frame-base values: the entry (undef) versions of rsp/rbp. Stack-variable
    // promotion points these at a real local array so generic stack accesses
    // (arrays, rsp-relative spills) hit real memory instead of dereferencing an
    // uninitialised frame register. In shared-stack mode ebp (RegId 5) is instead
    // a threaded reg parameter (above), so only rsp is a frame base there.
    let frame_regs: &[u16] = if crate::emit::shared_stack() { &[4] } else { &[4, 5] };
    for &r in frame_regs {
        if let Some(v) = undef.get(&Location::Reg(RegId(r))) {
            func.frame_base_values.push(v.0);
        }
    }

    // x87 FPU values are `long double`. Include any undef (entry) versions too,
    // though a well-formed function defines a slot (fld/fild) before reading it.
    for (loc, v) in &undef {
        if is_fp80_loc(loc) {
            fp80.push(v.0);
        }
    }
    fp80.sort_unstable();
    fp80.dedup();
    func.fp80_values = fp80;

    // Entry (undef) versions: the incoming value of each register/flag/temp read
    // before it is written. The differential SSA interpreter seeds these from the
    // initial CPU state so a `Use` of an entry version reads the right input.
    func.entry_values = undef.iter().map(|(l, v)| (l.clone(), v.0)).collect();
}

/// Rewrite all `Read(loc)` in an expression to `Use(version)`.
fn rewrite_reads(
    e: Expr,
    stacks: &HashMap<Location, Vec<ValueId>>,
    undef: &mut HashMap<Location, ValueId>,
    counter: &mut u32,
) -> Expr {
    let top = |v: &Location,
               undef: &mut HashMap<Location, ValueId>,
               counter: &mut u32|
     -> ValueId {
        if let Some(s) = stacks.get(v) {
            if let Some(&x) = s.last() {
                return x;
            }
        }
        *undef.entry(v.clone()).or_insert_with(|| {
            let id = ValueId(*counter);
            *counter += 1;
            id
        })
    };
    match e {
        Expr::Read(loc) => {
            if is_versioned(&loc) {
                Expr::Use(top(&loc, undef, counter))
            } else {
                Expr::Read(loc)
            }
        }
        Expr::Load { addr, ty } => Expr::Load {
            addr: Box::new(rewrite_reads(*addr, stacks, undef, counter)),
            ty,
        },
        Expr::Unary(op, x) => {
            Expr::Unary(op, Box::new(rewrite_reads(*x, stacks, undef, counter)))
        }
        Expr::Binary(op, a, b) => Expr::Binary(
            op,
            Box::new(rewrite_reads(*a, stacks, undef, counter)),
            Box::new(rewrite_reads(*b, stacks, undef, counter)),
        ),
        Expr::Cast { to, expr } => Expr::Cast {
            to,
            expr: Box::new(rewrite_reads(*expr, stacks, undef, counter)),
        },
        Expr::Select { cond, then_, else_ } => Expr::Select {
            cond: Box::new(rewrite_reads(*cond, stacks, undef, counter)),
            then_: Box::new(rewrite_reads(*then_, stacks, undef, counter)),
            else_: Box::new(rewrite_reads(*else_, stacks, undef, counter)),
        },
        Expr::Call { target, args, ret } => {
            let target = match target {
                CallTarget::Indirect(x) => {
                    CallTarget::Indirect(Box::new(rewrite_reads(*x, stacks, undef, counter)))
                }
                t => t,
            };
            Expr::Call {
                target,
                args: args
                    .into_iter()
                    .map(|a| rewrite_reads(a, stacks, undef, counter))
                    .collect(),
                ret,
            }
        }
        other => other, // Const, Use, Addr, Phi, Undef
    }
}

fn rewrite_stmt(
    s: Stmt,
    stacks: &HashMap<Location, Vec<ValueId>>,
    undef: &mut HashMap<Location, ValueId>,
    counter: &mut u32,
) -> Stmt {
    match s {
        Stmt::Store { addr, value, ty } => Stmt::Store {
            addr: rewrite_reads(addr, stacks, undef, counter),
            value: rewrite_reads(value, stacks, undef, counter),
            ty,
        },
        Stmt::Branch {
            cond,
            taken,
            fallthrough,
        } => Stmt::Branch {
            cond: rewrite_reads(cond, stacks, undef, counter),
            taken,
            fallthrough,
        },
        Stmt::Switch {
            value,
            cases,
            default,
        } => Stmt::Switch {
            value: rewrite_reads(value, stacks, undef, counter),
            cases,
            default,
        },
        Stmt::Return(Some(e)) => Stmt::Return(Some(rewrite_reads(e, stacks, undef, counter))),
        Stmt::CallStmt(e) => Stmt::CallStmt(rewrite_reads(e, stacks, undef, counter)),
        // A non-versioned Set (a Frame slot) keeps its location but its read
        // operands are still renamed.
        Stmt::Set { dst, expr } => Stmt::Set {
            dst,
            expr: rewrite_reads(expr, stacks, undef, counter),
        },
        other => other,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn blk(id: u32, stmts: Vec<Stmt>, succ: Vec<u32>, pred: Vec<u32>) -> Block {
        Block {
            id,
            addr: id as u64,
            stmts,
            succ,
            pred,
        }
    }

    /// Diamond CFG: rax defined on both branches' dominators, used at the join.
    ///   b0: rax = 1; -> b1, b2
    ///   b1: rax = 2; -> b3
    ///   b2:          -> b3
    ///   b3: rbx = rax  (join: needs a φ for rax)
    #[test]
    fn phi_inserted_at_join() {
        let rax = Location::Reg(RegId(0));
        let rbx = Location::Reg(RegId(3));
        let mut f = IrFunction {
            entry: 0,
            name: "t".into(),
            bits: 64,
            reg_params: vec![],
            frame_base_values: vec![],
            fp80_values: vec![],
            entry_values: vec![],
            blocks: vec![
                blk(
                    0,
                    vec![Stmt::Set {
                        dst: rax.clone(),
                        expr: Expr::konst(1, 32),
                    }],
                    vec![1, 2],
                    vec![],
                ),
                blk(
                    1,
                    vec![Stmt::Set {
                        dst: rax.clone(),
                        expr: Expr::konst(2, 32),
                    }],
                    vec![3],
                    vec![0],
                ),
                blk(2, vec![], vec![3], vec![0]),
                blk(
                    3,
                    vec![Stmt::Set {
                        dst: rbx.clone(),
                        expr: Expr::Read(rax.clone()),
                    }],
                    vec![],
                    vec![1, 2],
                ),
            ],
            next_value: 0,
            next_temp: 0,
        };

        to_ssa(&mut f);

        // Exactly one φ, and it is in block 3.
        let mut phi_dst = None;
        for (b, blk) in f.blocks.iter().enumerate() {
            for s in &blk.stmts {
                if let Stmt::Assign { dst, expr: Expr::Phi(args) } = s {
                    assert_eq!(b, 3, "phi should be at the join");
                    assert_eq!(args.len(), 2, "one arg per predecessor");
                    assert!(phi_dst.is_none(), "expected a single phi");
                    phi_dst = Some(*dst);
                }
            }
        }
        let phi_dst = phi_dst.expect("a phi must be inserted at the join");

        // The use of rax in block 3 must reference the φ's value.
        let used = f.blocks[3].stmts.iter().find_map(|s| match s {
            Stmt::Assign { expr: Expr::Use(v), .. } => Some(*v),
            _ => None,
        });
        assert_eq!(used, Some(phi_dst), "join use must read the phi");
    }

    /// Every definition gets a unique ValueId (SSA invariant).
    #[test]
    fn defs_are_unique() {
        let rax = Location::Reg(RegId(0));
        let mut f = IrFunction {
            entry: 0,
            name: "t".into(),
            bits: 64,
            reg_params: vec![],
            frame_base_values: vec![],
            fp80_values: vec![],
            entry_values: vec![],
            blocks: vec![blk(
                0,
                vec![
                    Stmt::Set { dst: rax.clone(), expr: Expr::konst(1, 32) },
                    Stmt::Set { dst: rax.clone(), expr: Expr::konst(2, 32) },
                ],
                vec![],
                vec![],
            )],
            next_value: 0,
            next_temp: 0,
        };
        to_ssa(&mut f);
        let mut seen = HashSet::new();
        for s in &f.blocks[0].stmts {
            if let Stmt::Assign { dst, .. } = s {
                assert!(seen.insert(dst.0), "duplicate SSA def {:?}", dst);
            }
        }
        assert_eq!(seen.len(), 2);
    }
}
