//! SSA optimisation passes (roadmap §4): constant/copy propagation, constant
//! folding + algebraic simplification, and dead-code elimination.
//!
//! In SSA each value is defined exactly once, so propagation is trivial and
//! provably safe. Folding includes the rewrites that begin to recover branch
//! conditions from flags by dataflow: pushing logical negation through
//! relationals (`!(a==b)` → `a!=b`) and recognising `CF|ZF` → `a<=b` (unsigned).
//! Signed `SF!=OF` reconstruction is left for a dedicated pass.
//!
//! Memory is not value-numbered, so `Load`/`Call` expressions are never inlined
//! or dropped when they could carry a side effect.
//!
//! Parallel to the text pipeline; runs in `--mode ir`.
#![allow(dead_code)]

pub mod alias;
pub mod frame;

use crate::ir::types::*;
use std::collections::HashMap;

/// Run propagation + folding + DCE to a fixpoint, then tidy call sites.
pub fn optimize(func: &mut IrFunction) {
    for _ in 0..8 {
        let a = propagate(func);
        fold_function(func);
        let b = dce(func);
        if !a && !b {
            break;
        }
    }
    prune_call_args(func);
}

/// Trim trailing call arguments that carry no value.
///
/// The lifter over-approximates a call by reading all six SysV integer argument
/// registers (`rdi,rsi,rdx,rcx,r8,r9`). After propagation, a trailing argument
/// register that was clobbered by an earlier call (or otherwise never assigned a
/// real value) folds to `Expr::Undef`. Such an argument provably carries no
/// information, so dropping it is sound and turns `f(a, b, undef, undef)` into
/// `f(a, b)`. Arguments that hold a real value or a forwarded parameter are
/// `Expr::Use(_)` and are left untouched, so no live argument is ever lost.
fn prune_call_args(func: &mut IrFunction) {
    // Shared machine-stack mode (transpiler) passes a *fixed* argument list to
    // every call — the stack pointer plus the volatile registers — to match each
    // callee's fixed parameter list. A register that folds to Undef must stay, or
    // the call/callee arities disagree (a compile error once the callee's real
    // prototype is in scope). So never trim in that mode.
    if crate::emit::shared_stack() {
        return;
    }
    fn trim(args: &mut Vec<Expr>) {
        while matches!(args.last(), Some(Expr::Undef)) {
            args.pop();
        }
    }
    fn visit(e: &mut Expr) {
        if let Expr::Call { target, args, .. } = e {
            for a in args.iter_mut() {
                visit(a);
            }
            // Trailing-Undef trimming undoes the lifter's 6-register call-arg
            // over-approximation. The synthetic float/packed runtime helpers
            // (`__fp_*`/`__pi_*`) have exact, fixed arity — never trim them, even
            // when an operand folds to Undef (which would corrupt the call).
            let is_helper = matches!(target, CallTarget::Named(n) if n.starts_with("__"));
            if !is_helper {
                trim(args);
            }
            if let CallTarget::Indirect(x) = target {
                visit(x);
            }
            return;
        }
        match e {
            Expr::Load { addr, .. } => visit(addr),
            Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => visit(x),
            Expr::Binary(_, a, b) => {
                visit(a);
                visit(b);
            }
            Expr::Select { cond, then_, else_ } => {
                visit(cond);
                visit(then_);
                visit(else_);
            }
            _ => {}
        }
    }
    for b in &mut func.blocks {
        for s in &mut b.stmts {
            map_exprs(s, &mut visit);
        }
    }
}

// --- constant / copy / single-use propagation -----------------------------

/// Cannot be *hoisted/duplicated* (memory reads, calls): used to gate
/// propagation, so a load is never moved across a store.
fn contains_side_effect(e: &Expr) -> bool {
    match e {
        Expr::Load { .. } | Expr::Call { .. } | Expr::Read(_) => true,
        Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => contains_side_effect(x),
        Expr::Binary(_, a, b) => contains_side_effect(a) || contains_side_effect(b),
        Expr::Select { cond, then_, else_ } => {
            contains_side_effect(cond) || contains_side_effect(then_) || contains_side_effect(else_)
        }
        _ => false,
    }
}

/// Cannot be *removed* even if its result is unused (only calls have an
/// observable effect; a dead memory load is safe to drop, as every decompiler
/// does — we do not model faulting/volatile reads).
fn contains_call(e: &Expr) -> bool {
    match e {
        Expr::Call { .. } => true,
        Expr::Load { addr, .. } => contains_call(addr),
        Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => contains_call(x),
        Expr::Binary(_, a, b) => contains_call(a) || contains_call(b),
        Expr::Select { cond, then_, else_ } => {
            contains_call(cond) || contains_call(then_) || contains_call(else_)
        }
        _ => false,
    }
}

/// Synthetic runtime helpers with no observable effect beyond their return
/// value: no memory write, no fault/trap, no I/O. A dead call to one of these is
/// safe to delete (unlike a general call). This matters beyond tidiness: a
/// lingering dead flag helper adds an extra *use* of its operand, which blocks
/// copy-propagation from inlining that operand and so defeats downstream idiom
/// recovery — most visibly magic-division, where a dead parity `__ix_pf` left on
/// the shift result keeps `x*M` from folding into the shift and the `x / c`
/// rewrite never fires.
///
/// Deliberately a whitelist: helpers that can fault (`__ix_udiv32`/`idiv32`/… and
/// the 64-bit `__ix_udiv`/`idiv` trap on #DE) or write memory (`__rep_*` string
/// ops, the `aret_*` HLE shims) are excluded — dropping a dead one of those would
/// remove an observable effect.
fn is_pure_helper(name: &str) -> bool {
    matches!(name, "__ix_pf" | "__ix_cpuid" | "__ix_xgetbv")
        || name.starts_with("__fp_")
        || name.starts_with("__pi_")
        || name.starts_with("__ps_")
        || name.starts_with("__x87_")
}

/// Whether an expression contains a call that is *not* a pure helper — i.e. a
/// call whose result, if dead, must still be evaluated for its side effect.
/// Mirrors `contains_call` but lets DCE drop dead pure-helper computations.
fn has_impure_call(e: &Expr) -> bool {
    match e {
        Expr::Call { target, args, .. } => {
            let target_impure = match target {
                CallTarget::Named(n) => !is_pure_helper(n),
                // An indirect call has an unknown target, hence unknown side
                // effects — it must ALWAYS be treated as impure (like a Direct
                // call), never dropped when its result is dead. Recursing into the
                // target *expression* (the function pointer, e.g. a plain memory
                // load) wrongly classified such calls as pure, so DCE deleted them
                // and silently discarded their side effects — e.g. sqlite's
                // `pPage->xParseCell(pPage, pCell, &info)` whose result is
                // immediately overwritten: dropping it left `info` uninitialised
                // and corrupted every DELETE/UPDATE.
                CallTarget::Indirect(_) => true,
                _ => true,
            };
            target_impure || args.iter().any(has_impure_call)
        }
        Expr::Load { addr, .. } => has_impure_call(addr),
        Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => has_impure_call(x),
        Expr::Binary(_, a, b) => has_impure_call(a) || has_impure_call(b),
        Expr::Select { cond, then_, else_ } => {
            has_impure_call(cond) || has_impure_call(then_) || has_impure_call(else_)
        }
        _ => false,
    }
}

fn propagate(func: &mut IrFunction) -> bool {
    // Single definition per value (SSA); collect defs and use counts.
    let mut defs: HashMap<u32, Expr> = HashMap::new();
    for b in &func.blocks {
        for s in &b.stmts {
            if let Stmt::Assign { dst, expr } = s {
                defs.insert(dst.0, expr.clone());
            }
        }
    }
    let mut uses: HashMap<u32, u32> = HashMap::new();
    for b in &func.blocks {
        for s in &b.stmts {
            for_each_use(s, &mut |v| *uses.entry(v).or_insert(0) += 1);
        }
    }

    let mut changed = false;
    for b in &mut func.blocks {
        for s in &mut b.stmts {
            map_exprs(s, &mut |e| {
                let ne = subst(e, &defs, &uses);
                if ne != *e {
                    changed = true;
                }
                *e = ne;
            });
        }
    }
    changed
}

/// Substitute a use by its definition when safe: constants and copies always;
/// other pure expressions only when single-use.
fn subst(e: &Expr, defs: &HashMap<u32, Expr>, uses: &HashMap<u32, u32>) -> Expr {
    match e {
        Expr::Use(v) => match defs.get(&v.0) {
            Some(Expr::Const(c, t)) => Expr::Const(*c, t.clone()),
            Some(Expr::Use(w)) => subst(&Expr::Use(*w), defs, uses),
            Some(Expr::Phi(_)) | None => e.clone(),
            Some(d) => {
                if !contains_side_effect(d) && uses.get(&v.0).copied().unwrap_or(0) == 1 {
                    subst(d, defs, uses)
                } else {
                    e.clone()
                }
            }
        },
        Expr::Load { addr, ty } => Expr::Load {
            addr: Box::new(subst(addr, defs, uses)),
            ty: ty.clone(),
        },
        Expr::Unary(op, x) => Expr::Unary(*op, Box::new(subst(x, defs, uses))),
        Expr::Binary(op, a, b) => Expr::Binary(
            *op,
            Box::new(subst(a, defs, uses)),
            Box::new(subst(b, defs, uses)),
        ),
        Expr::Cast { to, expr } => Expr::Cast {
            to: to.clone(),
            expr: Box::new(subst(expr, defs, uses)),
        },
        Expr::Call { target, args, ret } => {
            let target = match target {
                CallTarget::Indirect(x) => CallTarget::Indirect(Box::new(subst(x, defs, uses))),
                t => t.clone(),
            };
            Expr::Call {
                target,
                args: args.iter().map(|a| subst(a, defs, uses)).collect(),
                ret: ret.clone(),
            }
        }
        Expr::Select { cond, then_, else_ } => Expr::Select {
            cond: Box::new(subst(cond, defs, uses)),
            then_: Box::new(subst(then_, defs, uses)),
            else_: Box::new(subst(else_, defs, uses)),
        },
        other => other.clone(),
    }
}

// --- folding / algebraic simplification -----------------------------------

fn fold_function(func: &mut IrFunction) {
    for b in &mut func.blocks {
        for s in &mut b.stmts {
            map_exprs(s, &mut |e| *e = fold(e));
        }
    }
}

fn is_const(e: &Expr) -> Option<i128> {
    if let Expr::Const(c, _) = e {
        Some(*c)
    } else {
        None
    }
}

/// Negate a relational operator (for pushing logical-not inward).
fn negate_rel(op: BinOp) -> Option<BinOp> {
    use BinOp::*;
    Some(match op {
        Eq => Ne,
        Ne => Eq,
        Ult => Uge,
        Uge => Ult,
        Ugt => Ule,
        Ule => Ugt,
        Slt => Sge,
        Sge => Slt,
        Sgt => Sle,
        Sle => Sgt,
        _ => return None,
    })
}

fn fold(e: &Expr) -> Expr {
    match e {
        Expr::Unary(op, x) => {
            let x = fold(x);
            Expr::Unary(*op, Box::new(x))
        }
        Expr::Cast { to, expr } => Expr::Cast {
            to: to.clone(),
            expr: Box::new(fold(expr)),
        },
        Expr::Load { addr, ty } => Expr::Load {
            addr: Box::new(fold(addr)),
            ty: ty.clone(),
        },
        Expr::Binary(op, a, b) => {
            let a = fold(a);
            let b = fold(b);
            fold_binary(*op, a, b)
        }
        Expr::Call { target, args, ret } => Expr::Call {
            target: target.clone(),
            args: args.iter().map(fold).collect(),
            ret: ret.clone(),
        },
        Expr::Select { cond, then_, else_ } => {
            let c = fold(cond);
            // Fold a constant condition away.
            match is_const(&c) {
                Some(0) => fold(else_),
                Some(_) => fold(then_),
                None => Expr::Select {
                    cond: Box::new(c),
                    then_: Box::new(fold(then_)),
                    else_: Box::new(fold(else_)),
                },
            }
        }
        other => other.clone(),
    }
}

fn zero64() -> Expr {
    Expr::Const(0, Ty::int(64))
}

fn as_binop<'a>(e: &'a Expr, op: BinOp) -> Option<(&'a Expr, &'a Expr)> {
    match e {
        Expr::Binary(o, x, y) if *o == op => Some((x, y)),
        _ => None,
    }
}

fn is_zero(e: &Expr) -> bool {
    matches!(e, Expr::Const(0, _))
}

/// Recognise the lifter's `SF != OF` idiom for `a - b` and recover `a <s b`.
/// `sf` must be `Slt(a-b, 0)` and `of` the signed-overflow expression of `a-b`.
fn match_signed_lt(sf: &Expr, of: &Expr) -> Option<(Expr, Expr)> {
    let (r, z) = as_binop(sf, BinOp::Slt)?;
    if !is_zero(z) {
        return None;
    }
    let (a, b) = as_binop(r, BinOp::Sub)?;
    let (x, y) = as_binop(of, BinOp::And)?;
    let slt0 = |e: &Expr| Expr::Binary(BinOp::Slt, Box::new(e.clone()), Box::new(zero64()));
    let exp_x = Expr::Binary(BinOp::Ne, Box::new(slt0(a)), Box::new(slt0(b)));
    let exp_y = Expr::Binary(BinOp::Ne, Box::new(slt0(r)), Box::new(slt0(a)));
    if *x == exp_x && *y == exp_y {
        Some((a.clone(), b.clone()))
    } else {
        None
    }
}

/// Hacker's Delight `magicu` (Fig. 10-1) for unsigned 32-bit division by `d`.
/// Returns `(M, s, add)` such that `floor(x/d)` is `mulhi(x,M) >> s` when
/// `add` is false, or `((mulhi(x,M)+x) >> s')` when an extra add is required.
/// All arithmetic is modulo 2^32 (matching C `unsigned`), so use wrapping ops.
fn magicu32(d: u32) -> Option<(u32, u32, bool)> {
    if d < 2 {
        return None;
    }
    let two31: u32 = 0x8000_0000;
    let nc = u32::MAX - (0u32.wrapping_sub(d)) % d;
    let mut p: u32 = 31;
    let mut q1: u32 = two31 / nc;
    let mut r1: u32 = two31 - q1.wrapping_mul(nc);
    let mut q2: u32 = 0x7FFF_FFFF / d;
    let mut r2: u32 = 0x7FFF_FFFF - q2.wrapping_mul(d);
    let mut add = false;
    loop {
        p += 1;
        if r1 >= nc - r1 {
            q1 = q1.wrapping_mul(2).wrapping_add(1);
            r1 = r1.wrapping_mul(2).wrapping_sub(nc);
        } else {
            q1 = q1.wrapping_mul(2);
            r1 = r1.wrapping_mul(2);
        }
        if r2 + 1 >= d - r2 {
            if q2 >= 0x7FFF_FFFF {
                add = true;
            }
            q2 = q2.wrapping_mul(2).wrapping_add(1);
            r2 = r2.wrapping_mul(2).wrapping_add(1).wrapping_sub(d);
        } else {
            if q2 >= two31 {
                add = true;
            }
            q2 = q2.wrapping_mul(2);
            r2 = r2.wrapping_mul(2).wrapping_add(1);
        }
        let delta = d - 1 - r2;
        if p >= 64 || !(q1 < delta || (q1 == delta && r1 == 0)) {
            break;
        }
    }
    Some((q2.wrapping_add(1), p - 32, add))
}

fn peel_cast(e: &Expr) -> &Expr {
    match e {
        Expr::Cast { expr, .. } => peel_cast(expr),
        _ => e,
    }
}

/// Recognise the compiler's unsigned division-by-constant idiom
/// `(x * M) >> t` (the high-multiply form, `t >= 32`, no correction add) and
/// recover `x / d`. The recovered `d` is *self-validated*: we recompute the
/// canonical magic for `d` and require it to match `(M, t-32)` exactly, so a
/// sequence that is not in fact a division by `d` is never rewritten.
fn try_magic_udiv(shifted: &Expr, t: i128) -> Option<Expr> {
    if !(32..=63).contains(&t) {
        return None;
    }
    let inner = peel_cast(shifted);
    let (x, m) = match inner {
        Expr::Binary(BinOp::Mul, p, q) => {
            if let Some(mc) = is_const(q) {
                (p.as_ref(), mc)
            } else if let Some(mc) = is_const(p) {
                (q.as_ref(), mc)
            } else {
                return None;
            }
        }
        _ => return None,
    };
    if m <= 0 || m > u32::MAX as i128 {
        return None; // no-add form: M fits in 32 bits
    }
    let m = m as u32;
    let s_obs = (t - 32) as u32;
    let d0 = (1u128 << t) / m as u128;
    for cand in [d0.wrapping_sub(1), d0, d0 + 1] {
        if !(2..=u32::MAX as u128).contains(&cand) {
            continue;
        }
        let d = cand as u32;
        if let Some((mm, ss, add)) = magicu32(d) {
            if !add && mm == m && ss == s_obs {
                return Some(Expr::Binary(
                    BinOp::UDiv,
                    Box::new(x.clone()),
                    Box::new(Expr::Const(d as i128, Ty::int(32))),
                ));
            }
        }
    }
    None
}

fn fold_binary(op: BinOp, a: Expr, b: Expr) -> Expr {
    use BinOp::*;
    let i32ty = Ty::int(32);

    // Unsigned division-by-constant: `(x * M) >> t` -> `x / d`.
    if op == Shr {
        if let Some(t) = is_const(&b) {
            if let Some(rw) = try_magic_udiv(&a, t) {
                return rw;
            }
        }
    }

    // Const op Const — only width-independent ops (bitwise, equality).
    if let (Some(x), Some(y)) = (is_const(&a), is_const(&b)) {
        let r = match op {
            And => Some(x & y),
            Or => Some(x | y),
            Xor => Some(x ^ y),
            Eq => Some((x == y) as i128),
            Ne => Some((x != y) as i128),
            _ => None,
        };
        if let Some(v) = r {
            return Expr::Const(v, i32ty);
        }
    }

    // Algebraic identities.
    match op {
        Add | Sub | Or | Xor => {
            if is_const(&b) == Some(0) {
                return a;
            }
            if op == Add || op == Or || op == Xor {
                if is_const(&a) == Some(0) {
                    return b;
                }
            }
        }
        Mul => {
            if is_const(&b) == Some(1) {
                return a;
            }
            if is_const(&a) == Some(1) {
                return b;
            }
            if is_const(&a) == Some(0) || is_const(&b) == Some(0) {
                return Expr::Const(0, i32ty);
            }
        }
        And => {
            if is_const(&b) == Some(0) || is_const(&a) == Some(0) {
                return Expr::Const(0, i32ty);
            }
            // mask-of-mask: (x & c1) & c2  ->  x & (c1 & c2)
            if let (Expr::Binary(And, x, c1), Some(c2)) = (&a, is_const(&b)) {
                if let Some(c1v) = is_const(c1) {
                    return Expr::Binary(And, x.clone(), Box::new(Expr::Const(c1v & c2, i32ty)));
                }
            }
        }
        _ => {}
    }

    // Push logical-not through a relational: (rel == 0) -> negated rel.
    if op == Eq && is_const(&b) == Some(0) {
        if let Expr::Binary(inner, x, y) = &a {
            if let Some(neg) = negate_rel(*inner) {
                return Expr::Binary(neg, x.clone(), y.clone());
            }
        }
    }

    // Recover the signed `a <s b` idiom: `SF != OF`.
    if op == Ne {
        if let Some((x, y)) = match_signed_lt(&a, &b).or_else(|| match_signed_lt(&b, &a)) {
            return Expr::Binary(Slt, Box::new(x), Box::new(y));
        }
    }

    // Recognise combinations of compare flags into a single relational.
    //   Ult|Eq -> Ule (jbe, unsigned) ;  Slt|Eq -> Sle (jle, signed)
    if op == Or {
        for (lo, hi) in [(Ult, Ule), (Slt, Sle)] {
            if let (Expr::Binary(o, x1, y1), Expr::Binary(Eq, x2, y2)) = (&a, &b) {
                if *o == lo && x1 == x2 && y1 == y2 {
                    return Expr::Binary(hi, x1.clone(), y1.clone());
                }
            }
            if let (Expr::Binary(Eq, x1, y1), Expr::Binary(o, x2, y2)) = (&a, &b) {
                if *o == lo && x1 == x2 && y1 == y2 {
                    return Expr::Binary(hi, x1.clone(), y1.clone());
                }
            }
        }
    }

    // Idempotent / reflexive identities on equal operands.
    if a == b {
        match op {
            And | Or => return a, // x & x -> x ; x | x -> x  (e.g. `test r,r`)
            Eq | Ule | Uge | Sle | Sge => return Expr::Const(1, i32ty),
            Ne | Ult | Ugt | Slt | Sgt => return Expr::Const(0, i32ty),
            Sub | Xor => return Expr::Const(0, i32ty),
            _ => {}
        }
    }

    Expr::Binary(op, Box::new(a), Box::new(b))
}

// --- dead-code elimination -------------------------------------------------

fn dce(func: &mut IrFunction) -> bool {
    let mut uses: HashMap<u32, u32> = HashMap::new();
    for b in &func.blocks {
        for s in &b.stmts {
            for_each_use(s, &mut |v| *uses.entry(v).or_insert(0) += 1);
        }
    }
    let mut changed = false;
    for b in &mut func.blocks {
        let before = b.stmts.len();
        b.stmts.retain(|s| match s {
            Stmt::Assign { dst, expr } => {
                uses.get(&dst.0).copied().unwrap_or(0) > 0 || has_impure_call(expr)
            }
            _ => true,
        });
        if b.stmts.len() != before {
            changed = true;
        }
    }
    changed
}

// --- traversal helpers -----------------------------------------------------

/// Apply `f` to each top-level expression of a statement.
fn map_exprs(s: &mut Stmt, f: &mut impl FnMut(&mut Expr)) {
    match s {
        Stmt::Set { expr, .. } | Stmt::Assign { expr, .. } | Stmt::CallStmt(expr) => f(expr),
        Stmt::Store { addr, value, .. } => {
            f(addr);
            f(value);
        }
        Stmt::Branch { cond, .. } => f(cond),
        Stmt::Switch { value, .. } => f(value),
        Stmt::Return(Some(e)) => f(e),
        _ => {}
    }
}

/// Call `f` with every `ValueId` used (read) by a statement, including φ args.
fn for_each_use(s: &Stmt, f: &mut impl FnMut(u32)) {
    fn walk(e: &Expr, f: &mut impl FnMut(u32)) {
        match e {
            Expr::Use(v) => f(v.0),
            Expr::Phi(args) => {
                for v in args {
                    f(v.0);
                }
            }
            Expr::Load { addr, .. } => walk(addr, f),
            Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => walk(x, f),
            Expr::Binary(_, a, b) => {
                walk(a, f);
                walk(b, f);
            }
            Expr::Call { target, args, .. } => {
                if let CallTarget::Indirect(x) = target {
                    walk(x, f);
                }
                for a in args {
                    walk(a, f);
                }
            }
            Expr::Select { cond, then_, else_ } => {
                walk(cond, f);
                walk(then_, f);
                walk(else_, f);
            }
            _ => {}
        }
    }
    match s {
        Stmt::Set { expr, .. } | Stmt::Assign { expr, .. } | Stmt::CallStmt(expr) => walk(expr, f),
        Stmt::Store { addr, value, .. } => {
            walk(addr, f);
            walk(value, f);
        }
        Stmt::Branch { cond, .. } => walk(cond, f),
        Stmt::Switch { value, .. } => walk(value, f),
        Stmt::Return(Some(e)) => walk(e, f),
        _ => {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn one_block(stmts: Vec<Stmt>) -> IrFunction {
        IrFunction {
            entry: 0,
            name: "t".into(),
            bits: 64,
            reg_params: vec![],
            frame_base_values: vec![],
            fp80_values: vec![],
            entry_values: vec![],
            blocks: vec![Block {
                id: 0,
                addr: 0,
                stmts,
                succ: vec![],
                pred: vec![],
            }],
            next_value: 100,
            next_temp: 0,
        }
    }

    #[test]
    fn folds_and_propagates_constants() {
        // v0 = 0 ; store [100] = (v0 | 7) ; return
        let mut f = one_block(vec![
            Stmt::Assign { dst: ValueId(0), expr: Expr::konst(0, 32) },
            Stmt::Store {
                addr: Expr::konst(100, 32),
                value: Expr::Binary(BinOp::Or, Box::new(Expr::Use(ValueId(0))), Box::new(Expr::konst(7, 32))),
                ty: Ty::int(32),
            },
            Stmt::Return(None),
        ]);
        optimize(&mut f);
        // v0 inlined+folded to 7, dead assign removed.
        let stmts = &f.blocks[0].stmts;
        assert!(!stmts.iter().any(|s| matches!(s, Stmt::Assign { .. })), "dead def should be gone");
        let stored = stmts.iter().find_map(|s| match s {
            Stmt::Store { value, .. } => Some(value.clone()),
            _ => None,
        });
        assert_eq!(stored, Some(Expr::Const(7, Ty::int(32))));
    }

    #[test]
    fn indirect_call_is_impure_and_survives_dce() {
        // An indirect call (through a function pointer, e.g. `call [esi+0x50]` for
        // a C++/vtable or struct callback like sqlite's `pPage->xParseCell`) has an
        // unknown target and therefore unknown side effects. It must be classified
        // impure and NEVER dropped by DCE, even when its result is dead — otherwise
        // its side effects (memory writes through pointer args) are silently lost.
        let fp = Expr::Load { addr: Box::new(Expr::konst(0x1000, 32)), ty: Ty::int(32) };
        let call = Expr::Call {
            target: CallTarget::Indirect(Box::new(fp)),
            args: vec![],
            ret: Ty::int(32),
        };
        assert!(has_impure_call(&call), "an indirect call must be classified impure");

        // End-to-end: its result is dead (a register immediately overwritten), yet
        // the call must survive optimization.
        let mut f = one_block(vec![
            Stmt::Assign { dst: ValueId(0), expr: call },
            // v0 is never used → dead result, but the call is not removable.
            Stmt::Store { addr: Expr::konst(0x2000, 32), value: Expr::konst(1, 32), ty: Ty::int(32) },
            Stmt::Return(None),
        ]);
        optimize(&mut f);
        let has_call = f.blocks[0].stmts.iter().any(|s| matches!(
            s,
            Stmt::Assign { expr: Expr::Call { target: CallTarget::Indirect(_), .. }, .. }
        ));
        assert!(has_call, "DCE must not drop a dead-result indirect call");
    }

    #[test]
    fn reconstructs_unsigned_condition() {
        // cond = (Ult(a,b) | Eq(a,b))  ->  Ule(a,b)
        let a = Expr::Read(Location::Reg(RegId(0)));
        let b = Expr::Read(Location::Reg(RegId(1)));
        let cond = Expr::Binary(
            BinOp::Or,
            Box::new(Expr::Binary(BinOp::Ult, Box::new(a.clone()), Box::new(b.clone()))),
            Box::new(Expr::Binary(BinOp::Eq, Box::new(a.clone()), Box::new(b.clone()))),
        );
        assert_eq!(
            fold(&cond),
            Expr::Binary(BinOp::Ule, Box::new(a), Box::new(b))
        );
    }

    /// Build the lifter's flag expressions for `a - b` and check signed branch
    /// reconstruction: `SF != OF` -> `a <s b`, and `ZF | (SF!=OF)` -> `a <=s b`.
    #[test]
    fn reconstructs_signed_conditions() {
        let a = Expr::Read(Location::Reg(RegId(0)));
        let b = Expr::Read(Location::Reg(RegId(1)));
        let z = || Expr::Const(0, Ty::int(64));
        let slt0 = |e: &Expr| Expr::Binary(BinOp::Slt, Box::new(e.clone()), Box::new(z()));
        let r = Expr::Binary(BinOp::Sub, Box::new(a.clone()), Box::new(b.clone()));
        let sf = slt0(&r);
        let of = Expr::Binary(
            BinOp::And,
            Box::new(Expr::Binary(BinOp::Ne, Box::new(slt0(&a)), Box::new(slt0(&b)))),
            Box::new(Expr::Binary(BinOp::Ne, Box::new(slt0(&r)), Box::new(slt0(&a)))),
        );
        // jl: SF != OF
        let jl = Expr::Binary(BinOp::Ne, Box::new(sf.clone()), Box::new(of.clone()));
        assert_eq!(
            fold(&jl),
            Expr::Binary(BinOp::Slt, Box::new(a.clone()), Box::new(b.clone()))
        );
        // jle: ZF | (SF != OF)
        let zf = Expr::Binary(BinOp::Eq, Box::new(a.clone()), Box::new(b.clone()));
        let jle = Expr::Binary(BinOp::Or, Box::new(zf), Box::new(jl));
        assert_eq!(
            fold(&jle),
            Expr::Binary(BinOp::Sle, Box::new(a), Box::new(b))
        );
    }

    #[test]
    fn recovers_unsigned_magic_division() {
        // gcc's `x / 10` for unsigned 32-bit: (uint64)((x & 0xffffffff) * 0xCCCCCCCD) >> 35.
        let x = Expr::Binary(
            BinOp::And,
            Box::new(Expr::Read(Location::Reg(RegId(7)))),
            Box::new(Expr::konst(0xffff_ffff, 64)),
        );
        let mul = Expr::Binary(BinOp::Mul, Box::new(x.clone()), Box::new(Expr::konst(0xCCCC_CCCD, 64)));
        let cast = Expr::Cast { to: Ty::int(64), expr: Box::new(mul) };
        let shr = Expr::Binary(BinOp::Shr, Box::new(cast), Box::new(Expr::konst(35, 64)));
        match fold(&shr) {
            Expr::Binary(BinOp::UDiv, lhs, rhs) => {
                assert_eq!(*lhs, x);
                assert_eq!(*rhs, Expr::Const(10, Ty::int(32)));
            }
            other => panic!("expected x / 10, got {:?}", other),
        }
    }

    #[test]
    fn magic_recovery_is_exact_only() {
        // magicu32 must reproduce the well-known constants, and a non-matching
        // shift must NOT be misrecognised as a division.
        assert_eq!(magicu32(10), Some((0xCCCC_CCCD, 3, false)));
        assert_eq!(magicu32(3), Some((0xAAAA_AAAB, 1, false)));
        // Low-multiply form (shift < 32) is not a high-multiply division idiom.
        let mul = Expr::Binary(
            BinOp::Mul,
            Box::new(Expr::Read(Location::Reg(RegId(7)))),
            Box::new(Expr::konst(0xCCCC_CCCD, 64)),
        );
        let shr = Expr::Binary(BinOp::Shr, Box::new(mul), Box::new(Expr::konst(31, 64)));
        assert!(matches!(fold(&shr), Expr::Binary(BinOp::Shr, ..)));
        // A plain shift with no multiply is never a division.
        let plain = Expr::Binary(
            BinOp::Shr,
            Box::new(Expr::Read(Location::Reg(RegId(7)))),
            Box::new(Expr::konst(35, 64)),
        );
        assert!(matches!(fold(&plain), Expr::Binary(BinOp::Shr, ..)));
    }

    #[test]
    fn pushes_not_through_relational() {
        // (Eq(a,b) == 0)  ->  Ne(a,b)
        let a = Expr::Read(Location::Reg(RegId(0)));
        let b = Expr::Read(Location::Reg(RegId(1)));
        let e = Expr::Binary(
            BinOp::Eq,
            Box::new(Expr::Binary(BinOp::Eq, Box::new(a.clone()), Box::new(b.clone()))),
            Box::new(Expr::konst(0, 8)),
        );
        assert_eq!(fold(&e), Expr::Binary(BinOp::Ne, Box::new(a), Box::new(b)));
    }
}
