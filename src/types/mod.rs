//! Constraint-based type inference over the SSA IR (roadmap §5).
//!
//! Milestone 1+2: a type lattice with union-find unification, constraint
//! generation from value *usage*, and resolution to a per-value type map that
//! distinguishes pointers, code pointers, and signed vs unsigned integers from
//! plain 64-bit scalars.
//!
//! ## Why union-find
//!
//! Copies (`v = use(w)`) and φ-nodes link several SSA values into one logical
//! variable; they must share a type. Union-find makes that linkage cheap: each
//! set carries the *join* of every constraint applied to any of its members, so
//! a pointer fact discovered at one use flows to all the aliased versions.
//!
//! ## Why it is safe to wire in
//!
//! The result is **display-only** for now: it annotates the IR dump. The emitter
//! keeps every value stored as `uint64_t` with explicit masks/casts, so a
//! recovered type can never change program semantics — the worst an inference
//! bug can do is print a less-precise type. The regression gate (recompilation +
//! differential execution) is therefore unaffected.
#![allow(dead_code)]

use crate::ir::types::*;
use std::collections::{BTreeMap, HashMap};

/// Union-find over SSA values, each set carrying the join of its constraints.
pub struct TypeEnv {
    parent: Vec<u32>,
    rank: Vec<u8>,
    ty: Vec<Ty>,
}

impl TypeEnv {
    fn new(n: usize) -> Self {
        let n = n.max(1);
        TypeEnv {
            parent: (0..n as u32).collect(),
            rank: vec![0; n],
            ty: vec![Ty::Unknown; n],
        }
    }

    fn in_range(&self, v: u32) -> bool {
        (v as usize) < self.parent.len()
    }

    /// Representative of `x`'s set, with path halving.
    fn find(&mut self, mut x: u32) -> u32 {
        while self.parent[x as usize] != x {
            let gp = self.parent[self.parent[x as usize] as usize];
            self.parent[x as usize] = gp;
            x = gp;
        }
        x
    }

    /// Merge the sets of `a` and `b`, joining their accumulated types.
    fn union(&mut self, a: u32, b: u32) {
        if !self.in_range(a) || !self.in_range(b) {
            return;
        }
        let (ra, rb) = (self.find(a), self.find(b));
        if ra == rb {
            return;
        }
        let merged = join(&self.ty[ra as usize], &self.ty[rb as usize]);
        // Union by rank.
        let (lo, hi) = if self.rank[ra as usize] < self.rank[rb as usize] {
            (ra, rb)
        } else {
            (rb, ra)
        };
        self.parent[lo as usize] = hi;
        if self.rank[lo as usize] == self.rank[hi as usize] {
            self.rank[hi as usize] += 1;
        }
        self.ty[hi as usize] = merged;
    }

    /// Raise `v`'s type by joining it with `t`.
    fn constrain(&mut self, v: u32, t: Ty) {
        if !self.in_range(v) {
            return;
        }
        let r = self.find(v);
        self.ty[r as usize] = join(&self.ty[r as usize], &t);
    }

    /// The resolved type of `v`.
    pub fn get(&mut self, v: u32) -> Ty {
        if !self.in_range(v) {
            return Ty::Unknown;
        }
        let r = self.find(v);
        self.ty[r as usize].clone()
    }
}

/// Least-conflict join of two type facts. Total by construction: a genuine
/// conflict falls back to a deterministic 64-bit scalar rather than ⊥, so
/// inference never panics and the worst case is "less specific", never "wrong
/// storage" (emission ignores the result for semantics).
fn join(a: &Ty, b: &Ty) -> Ty {
    use Ty::*;
    match (a, b) {
        (Unknown, t) | (t, Unknown) => t.clone(),
        (
            Int { bits: b1, signed: s1 },
            Int { bits: b2, signed: s2 },
        ) => Int {
            bits: (*b1).max(*b2),
            signed: join_sign(*s1, *s2),
        },
        (Ptr(x), Ptr(y)) => Ptr(Box::new(join(x, y))),
        (Float { bits: b1 }, Float { bits: b2 }) => Float { bits: (*b1).max(*b2) },
        (Code, Code) => Code,
        (Bool, Bool) => Bool,
        // A pointer dominates a plain integer: pointer arithmetic keeps the value
        // in a 64-bit GPR, and the load/store that proved it a pointer is the
        // stronger evidence. A code pointer behaves likewise.
        (Ptr(x), Int { .. }) | (Int { .. }, Ptr(x)) => Ptr(x.clone()),
        (Code, Int { .. }) | (Int { .. }, Code) => Code,
        // Ptr vs Code — both are pointers; prefer the data pointer. `Code` is only
        // raised from (rare) indirect-call targets, so this rarely fires.
        (Ptr(x), Code) | (Code, Ptr(x)) => Ptr(x.clone()),
        // Any remaining mix (e.g. Float vs Int) is a real conflict; widen to a
        // 64-bit scalar of unknown signedness.
        _ => Int { bits: 64, signed: None },
    }
}

/// Merge two signedness facts; conflicting signs collapse to "unknown".
fn join_sign(a: Option<bool>, b: Option<bool>) -> Option<bool> {
    match (a, b) {
        (None, x) | (x, None) => x,
        (Some(x), Some(y)) if x == y => Some(x),
        _ => None,
    }
}

/// An `Int` carrying signedness but no asserted width (`bits == 0` means
/// "width unknown"; `join` takes the max, so it never lowers a real width).
fn sgn(signed: bool) -> Ty {
    Ty::Int { bits: 0, signed: Some(signed) }
}

/// If `e` is a bare value read, its id.
fn as_use(e: &Expr) -> Option<u32> {
    match e {
        Expr::Use(v) => Some(v.0),
        _ => None,
    }
}

/// The base-register value of an address expression: `v`, `v + k`, `k + v`,
/// `v - k`, or `v + i` (index form, base taken as the left operand, matching how
/// the lifter builds effective addresses).
fn ptr_base(e: &Expr) -> Option<u32> {
    match e {
        Expr::Use(v) => Some(v.0),
        Expr::Binary(BinOp::Add | BinOp::Sub, a, b) => {
            if matches!(b.as_ref(), Expr::Const(..)) {
                ptr_base(a)
            } else if matches!(a.as_ref(), Expr::Const(..)) {
                ptr_base(b)
            } else {
                ptr_base(a)
            }
        }
        _ => None,
    }
}

/// Signedness implied by a relational/division operator, if any. Shifts are
/// handled separately (only the shifted operand carries signedness).
fn binop_sign(op: BinOp) -> Option<bool> {
    use BinOp::*;
    match op {
        SDiv | SMod | Slt | Sle | Sgt | Sge => Some(true),
        UDiv | UMod | Ult | Ule | Ugt | Uge => Some(false),
        _ => None,
    }
}

/// Infer a type for every SSA value of `func`. Values that stay `Unknown` are
/// omitted from the map.
pub fn infer(func: &IrFunction) -> HashMap<u32, Ty> {
    let mut env = build_env(func);
    let n = func.next_value;
    let mut out = HashMap::new();
    for v in 0..n {
        let t = env.get(v);
        if !matches!(t, Ty::Unknown) {
            out.insert(v, t);
        }
    }
    out
}

// ---------------------------------------------------------------------------
// Aggregate (struct) reconstruction — roadmap §5.3, milestone 3.
//
// When one base pointer is accessed at several distinct offsets with coherent
// widths, the object behind it is a record: synthesise a `struct` from the
// observed `{offset -> type}` map. Versions of the same base (copies, φ) are
// unified by the inference union-find, so `p`, `p` after a copy, and a φ of both
// fold to one base and one struct. Display-only, like the type annotations.
// ---------------------------------------------------------------------------

/// A reconstructed struct: the base value it is reached through, and its fields
/// as `(byte offset, type)` sorted by offset.
#[derive(Clone, Debug, PartialEq)]
pub struct Aggregate {
    pub base: u32,
    pub fields: Vec<(i64, Ty)>,
}

/// Decompose an address into `(base value, byte offset)` for the simple
/// `base`, `base + k`, `k + base` forms. Negative or non-constant offsets are
/// rejected: those are stack frames / indexing, handled elsewhere.
fn base_offset(e: &Expr) -> Option<(u32, i64)> {
    match e {
        Expr::Use(v) => Some((v.0, 0)),
        Expr::Binary(BinOp::Add, a, b) => match (a.as_ref(), b.as_ref()) {
            (Expr::Use(v), Expr::Const(k, _)) | (Expr::Const(k, _), Expr::Use(v)) => {
                let k = *k;
                if k >= 0 && k <= i64::MAX as i128 {
                    Some((v.0, k as i64))
                } else {
                    None
                }
            }
            _ => None,
        },
        _ => None,
    }
}

/// Walk every memory access (`Load`/`Store`) of `func`, yielding `(addr, elem ty)`.
fn collect_accesses(func: &IrFunction) -> Vec<(Expr, Ty)> {
    fn walk(e: &Expr, out: &mut Vec<(Expr, Ty)>) {
        match e {
            Expr::Load { addr, ty } => {
                out.push(((**addr).clone(), ty.clone()));
                walk(addr, out);
            }
            Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => walk(x, out),
            Expr::Binary(_, a, b) => {
                walk(a, out);
                walk(b, out);
            }
            Expr::Select { cond, then_, else_ } => {
                walk(cond, out);
                walk(then_, out);
                walk(else_, out);
            }
            Expr::Call { target, args, .. } => {
                if let CallTarget::Indirect(x) = target {
                    walk(x, out);
                }
                for a in args {
                    walk(a, out);
                }
            }
            _ => {}
        }
    }
    let mut out = Vec::new();
    for b in &func.blocks {
        for s in &b.stmts {
            match s {
                Stmt::Store { addr, value, ty } => {
                    out.push((addr.clone(), ty.clone()));
                    walk(addr, &mut out);
                    walk(value, &mut out);
                }
                Stmt::Set { expr, .. } | Stmt::Assign { expr, .. } | Stmt::CallStmt(expr) => {
                    walk(expr, &mut out)
                }
                Stmt::Branch { cond, .. } => walk(cond, &mut out),
                Stmt::Switch { value, .. } => walk(value, &mut out),
                Stmt::Return(Some(e)) => walk(e, &mut out),
                _ => {}
            }
        }
    }
    out
}

/// Reconstruct structs from multi-offset pointer accesses. A base is promoted to
/// a struct only when it is accessed at **two or more distinct offsets** (a
/// single `*p` is just a scalar dereference, not a record).
pub fn recover_aggregates(func: &IrFunction) -> Vec<Aggregate> {
    let mut env = build_env(func);
    let mut acc: HashMap<u32, BTreeMap<i64, Ty>> = HashMap::new();
    for (addr, ty) in collect_accesses(func) {
        if let Some((base, off)) = base_offset(&addr) {
            let r = env.find(base);
            let slot = acc.entry(r).or_default().entry(off).or_insert(Ty::Unknown);
            *slot = join(slot, &ty);
        }
    }
    let mut out: Vec<Aggregate> = acc
        .into_iter()
        .filter(|(_, m)| m.len() >= 2)
        .map(|(base, m)| Aggregate { base, fields: m.into_iter().collect() })
        .collect();
    out.sort_by_key(|a| a.base);
    out
}

/// Render a reconstructed struct as a one-line C-ish declaration for the dump.
pub fn render_struct(a: &Aggregate) -> String {
    let mut s = format!("struct s_v{} {{ ", a.base);
    for (off, ty) in &a.fields {
        s.push_str(&format!("/*+0x{:x}*/ {} field_{:x}; ", off, ty_name(ty), off));
    }
    s.push('}');
    s
}

/// Generate and resolve all constraints, returning the populated environment.
pub fn build_env(func: &IrFunction) -> TypeEnv {
    let mut env = TypeEnv::new(func.next_value as usize);
    for b in &func.blocks {
        for s in &b.stmts {
            gen_stmt(&mut env, s);
        }
    }
    env
}

fn gen_stmt(env: &mut TypeEnv, s: &Stmt) {
    match s {
        Stmt::Assign { dst, expr } => {
            gen_expr(env, expr);
            match expr {
                // Copies and φ-nodes link versions into one variable.
                Expr::Use(src) => env.union(dst.0, src.0),
                Expr::Phi(args) => {
                    for a in args {
                        env.union(dst.0, a.0);
                    }
                }
                // The value receives the loaded / returned / address type.
                Expr::Load { ty, .. } => env.constrain(dst.0, ty.clone()),
                Expr::Call { ret, .. } if !matches!(ret, Ty::Unknown) => {
                    env.constrain(dst.0, ret.clone())
                }
                Expr::Addr(_) => env.constrain(dst.0, Ty::Ptr(Box::new(Ty::Unknown))),
                _ => {}
            }
        }
        Stmt::Set { expr, .. } => gen_expr(env, expr),
        Stmt::Store { addr, value, ty } => {
            gen_expr(env, addr);
            gen_expr(env, value);
            if let Some(base) = ptr_base(addr) {
                env.constrain(base, Ty::Ptr(Box::new(ty.clone())));
            }
            if let Some(v) = as_use(value) {
                env.constrain(v, ty.clone());
            }
        }
        Stmt::Branch { cond, .. } => gen_expr(env, cond),
        Stmt::Switch { value, .. } => gen_expr(env, value),
        Stmt::Return(Some(e)) => gen_expr(env, e),
        Stmt::CallStmt(e) => gen_expr(env, e),
        _ => {}
    }
}

fn gen_expr(env: &mut TypeEnv, e: &Expr) {
    match e {
        Expr::Load { addr, ty } => {
            gen_expr(env, addr);
            if let Some(base) = ptr_base(addr) {
                env.constrain(base, Ty::Ptr(Box::new(ty.clone())));
            }
        }
        Expr::Binary(op, a, b) => {
            gen_expr(env, a);
            gen_expr(env, b);
            match op {
                // A shift's signedness applies only to the shifted value, not the
                // count (`a >> b`: `a` is signed for `sar`, unsigned for `shr`).
                BinOp::Sar => {
                    if let Some(v) = as_use(a) {
                        env.constrain(v, sgn(true));
                    }
                }
                BinOp::Shr => {
                    if let Some(v) = as_use(a) {
                        env.constrain(v, sgn(false));
                    }
                }
                _ => {
                    if let Some(sign) = binop_sign(*op) {
                        if let Some(v) = as_use(a) {
                            env.constrain(v, sgn(sign));
                        }
                        if let Some(v) = as_use(b) {
                            env.constrain(v, sgn(sign));
                        }
                    }
                }
            }
        }
        Expr::Unary(UnOp::SignExtend, x) => {
            gen_expr(env, x);
            if let Some(v) = as_use(x) {
                env.constrain(v, sgn(true));
            }
        }
        Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => gen_expr(env, x),
        Expr::Select { cond, then_, else_ } => {
            gen_expr(env, cond);
            gen_expr(env, then_);
            gen_expr(env, else_);
        }
        Expr::Call { target, args, .. } => {
            if let CallTarget::Indirect(x) = target {
                gen_expr(env, x);
                if let Some(v) = as_use(x) {
                    env.constrain(v, Ty::Code);
                }
            }
            for a in args {
                gen_expr(env, a);
            }
        }
        _ => {}
    }
}

/// Human-readable C-ish name for a recovered type (for the IR dump annotation).
pub fn ty_name(t: &Ty) -> String {
    match t {
        Ty::Unknown => "?".into(),
        Ty::Bool => "bool".into(),
        Ty::Code => "code*".into(),
        Ty::Float { bits } => format!("float{}", bits),
        Ty::Ptr(inner) => match inner.as_ref() {
            Ty::Unknown => "void*".into(),
            _ => format!("{}*", ty_name(inner)),
        },
        Ty::Aggregate(id) => format!("agg{}", id),
        Ty::Int { bits, signed } => {
            let w = if *bits == 0 { 64 } else { *bits };
            match signed {
                Some(true) => format!("int{}_t", w),
                Some(false) => format!("uint{}_t", w),
                None => format!("int{}", w),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn func(next_value: u32, stmts: Vec<Stmt>) -> IrFunction {
        IrFunction {
            entry: 0,
            name: "t".into(),
            bits: 64,
            reg_params: vec![],
            blocks: vec![Block {
                id: 0,
                addr: 0,
                stmts,
                succ: vec![],
                pred: vec![],
            }],
            next_value,
            next_temp: 0,
        }
    }

    fn use_(v: u32) -> Expr {
        Expr::Use(ValueId(v))
    }

    /// A load through a value marks it a pointer; the loaded value gets the
    /// load's element type.
    #[test]
    fn load_makes_base_a_pointer() {
        // v1 = *(uint32_t*)(v0)
        let f = func(
            2,
            vec![Stmt::Assign {
                dst: ValueId(1),
                expr: Expr::Load {
                    addr: Box::new(use_(0)),
                    ty: Ty::int(32),
                },
            }],
        );
        let types = infer(&f);
        assert_eq!(ty_name(types.get(&0).unwrap()), "int32*"); // pointee width known, sign unknown
        assert_eq!(ty_name(types.get(&1).unwrap()), "int32");
    }

    /// Load through `base + k` still types `base` as a pointer.
    #[test]
    fn load_through_offset() {
        let f = func(
            2,
            vec![Stmt::Assign {
                dst: ValueId(1),
                expr: Expr::Load {
                    addr: Box::new(Expr::Binary(
                        BinOp::Add,
                        Box::new(use_(0)),
                        Box::new(Expr::konst(8, 64)),
                    )),
                    ty: Ty::int(64),
                },
            }],
        );
        let types = infer(&f);
        assert_eq!(ty_name(types.get(&0).unwrap()), "int64*");
    }

    /// Signed and unsigned relationals are distinguished.
    #[test]
    fn relational_signedness() {
        let f = func(
            4,
            vec![
                Stmt::Assign {
                    dst: ValueId(2),
                    expr: Expr::Binary(BinOp::Slt, Box::new(use_(0)), Box::new(use_(1))),
                },
                Stmt::Assign {
                    dst: ValueId(3),
                    expr: Expr::Binary(BinOp::Ult, Box::new(use_(0)), Box::new(use_(1))),
                },
            ],
        );
        // v0 is compared both signed and unsigned -> signedness conflict -> unknown.
        let types = infer(&f);
        assert_eq!(ty_name(types.get(&0).unwrap()), "int64");
    }

    /// A purely-signed comparison yields a signed operand.
    #[test]
    fn signed_only() {
        let f = func(
            3,
            vec![Stmt::Assign {
                dst: ValueId(2),
                expr: Expr::Binary(BinOp::Sgt, Box::new(use_(0)), Box::new(use_(1))),
            }],
        );
        let types = infer(&f);
        assert_eq!(ty_name(types.get(&0).unwrap()), "int64_t");
        assert_eq!(ty_name(types.get(&1).unwrap()), "int64_t");
    }

    /// A copy propagates a type across versions; a φ-node merges them.
    #[test]
    fn copy_and_phi_propagate() {
        // v1 = v0 (copy); v2 = phi(v1, v3); *(v2) loaded -> all are pointers.
        let f = func(
            4,
            vec![
                Stmt::Assign { dst: ValueId(1), expr: use_(0) },
                Stmt::Assign { dst: ValueId(2), expr: Expr::Phi(vec![ValueId(1), ValueId(3)]) },
                Stmt::Assign {
                    dst: ValueId(9),
                    expr: Expr::Load { addr: Box::new(use_(2)), ty: Ty::int(8) },
                },
            ],
        );
        let mut f = f;
        f.next_value = 10;
        let types = infer(&f);
        assert_eq!(ty_name(types.get(&0).unwrap()), "int8*");
        assert_eq!(ty_name(types.get(&1).unwrap()), "int8*");
        assert_eq!(ty_name(types.get(&3).unwrap()), "int8*");
    }

    /// An indirect call target is a code pointer.
    #[test]
    fn indirect_call_is_code() {
        let f = func(
            2,
            vec![Stmt::CallStmt(Expr::Call {
                target: CallTarget::Indirect(Box::new(use_(0))),
                args: vec![],
                ret: Ty::Unknown,
            })],
        );
        let types = infer(&f);
        assert_eq!(ty_name(types.get(&0).unwrap()), "code*");
    }

    /// Sign extension marks its source signed.
    #[test]
    fn sign_extend_is_signed() {
        let f = func(
            2,
            vec![Stmt::Assign {
                dst: ValueId(1),
                expr: Expr::Unary(UnOp::SignExtend, Box::new(use_(0))),
            }],
        );
        let types = infer(&f);
        assert_eq!(ty_name(types.get(&0).unwrap()), "int64_t");
    }

    /// Two accesses at distinct offsets off one base synthesise a struct.
    #[test]
    fn struct_from_multi_offset() {
        // v1 = *(int64*)(v0 + 0); v2 = *(int32*)(v0 + 8)
        let f = func(
            3,
            vec![
                Stmt::Assign {
                    dst: ValueId(1),
                    expr: Expr::Load { addr: Box::new(use_(0)), ty: Ty::int(64) },
                },
                Stmt::Assign {
                    dst: ValueId(2),
                    expr: Expr::Load {
                        addr: Box::new(Expr::Binary(
                            BinOp::Add,
                            Box::new(use_(0)),
                            Box::new(Expr::konst(8, 64)),
                        )),
                        ty: Ty::int(32),
                    },
                },
            ],
        );
        let aggs = recover_aggregates(&f);
        assert_eq!(aggs.len(), 1);
        assert_eq!(aggs[0].base, 0);
        assert_eq!(aggs[0].fields.len(), 2);
        assert_eq!(aggs[0].fields[0].0, 0);
        assert_eq!(aggs[0].fields[1].0, 8);
        let s = render_struct(&aggs[0]);
        assert!(s.contains("field_0"), "{}", s);
        assert!(s.contains("field_8"), "{}", s);
    }

    /// A single deref is not promoted to a struct.
    #[test]
    fn single_offset_is_not_a_struct() {
        let f = func(
            2,
            vec![Stmt::Assign {
                dst: ValueId(1),
                expr: Expr::Load { addr: Box::new(use_(0)), ty: Ty::int(64) },
            }],
        );
        assert!(recover_aggregates(&f).is_empty());
    }

    /// A copy of the base folds into one struct (union-find merges versions).
    #[test]
    fn struct_merges_aliased_base() {
        // v1 = v0; *(v0 + 0); *(v1 + 8) -> one struct on the shared base.
        let mut f = func(
            5,
            vec![
                Stmt::Assign { dst: ValueId(1), expr: use_(0) },
                Stmt::Assign {
                    dst: ValueId(2),
                    expr: Expr::Load { addr: Box::new(use_(0)), ty: Ty::int(64) },
                },
                Stmt::Assign {
                    dst: ValueId(3),
                    expr: Expr::Load {
                        addr: Box::new(Expr::Binary(
                            BinOp::Add,
                            Box::new(use_(1)),
                            Box::new(Expr::konst(8, 64)),
                        )),
                        ty: Ty::int(64),
                    },
                },
            ],
        );
        f.next_value = 5;
        let aggs = recover_aggregates(&f);
        assert_eq!(aggs.len(), 1, "aliased base must yield one struct: {:?}", aggs);
        assert_eq!(aggs[0].fields.len(), 2);
    }

    /// Pointer evidence dominates plain integer arithmetic on the same value.
    #[test]
    fn pointer_dominates_scalar() {
        // v1 = v0 + 1 (scalar use), v2 = *(v0) (pointer use) -> v0 is a pointer.
        let f = func(
            3,
            vec![
                Stmt::Assign {
                    dst: ValueId(1),
                    expr: Expr::Binary(BinOp::Add, Box::new(use_(0)), Box::new(Expr::konst(1, 64))),
                },
                Stmt::Assign {
                    dst: ValueId(2),
                    expr: Expr::Load { addr: Box::new(use_(0)), ty: Ty::int(64) },
                },
            ],
        );
        let types = infer(&f);
        assert_eq!(ty_name(types.get(&0).unwrap()), "int64*");
    }
}
