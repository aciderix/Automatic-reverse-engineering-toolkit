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

use crate::ir::types::*;
use std::collections::HashMap;

/// Run propagation + folding + DCE to a fixpoint.
pub fn optimize(func: &mut IrFunction) {
    for _ in 0..8 {
        let a = propagate(func);
        fold_function(func);
        let b = dce(func);
        if !a && !b {
            break;
        }
    }
}

// --- constant / copy / single-use propagation -----------------------------

fn contains_side_effect(e: &Expr) -> bool {
    match e {
        Expr::Load { .. } | Expr::Call { .. } => true,
        Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => contains_side_effect(x),
        Expr::Binary(_, a, b) => contains_side_effect(a) || contains_side_effect(b),
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

fn fold_binary(op: BinOp, a: Expr, b: Expr) -> Expr {
    use BinOp::*;
    let i32ty = Ty::int(32);

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
                uses.get(&dst.0).copied().unwrap_or(0) > 0 || contains_side_effect(expr)
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
