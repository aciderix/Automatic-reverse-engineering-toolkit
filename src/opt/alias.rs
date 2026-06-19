//! Memory alias analysis (roadmap §4.1) — the sound foundation that stack-slot
//! promotion and typed emission must be built on.
//!
//! ## What it does
//!
//! Classifies the address of every memory access into one of three regions:
//!
//! * **`Frame`** — a stack location at a statically-known `rbp/rsp + disp`
//!   offset (or the address of a named [`Location::Frame`] slot), with a byte
//!   range `[off, off + width)`.
//! * **`Global`** — an absolute / ip-relative address, or `&global`.
//! * **`Unknown`** — anything else (a value loaded from memory, a heap pointer,
//!   a variable index): conservatively aliases everything.
//!
//! From this, [`may_alias`] answers whether two accesses can touch the same
//! byte. The rules are deliberately conservative (the project never emits
//! incorrect output): two frame accesses alias only when their byte ranges
//! overlap; a frame access and a global access never alias; anything involving
//! `Unknown` may alias.
//!
//! ## Why it runs pre-SSA
//!
//! Before SSA renaming, a register read is an explicit `Read(Reg(rbp))`, so the
//! frame/stack base is visible syntactically. After renaming it becomes an
//! anonymous `Use(v)` whose origin is lost, so classification would need a
//! side-table. Running here keeps the analysis simple and exact.
//!
//! ## Status
//!
//! Pure analysis: it does not mutate the IR, so it cannot affect emitted code.
//! It is consumed today only by a display-only diagnostic (the IR dump reports
//! each function's frame-promotability verdict). Promotion will consume
//! [`frame_promotable`] next.
#![allow(dead_code)]

use crate::ir::types::*;

/// RegId of the stack pointer family (rsp/esp) and frame pointer family
/// (rbp/ebp), per `ir::lift::reg_id`.
const RSP: u16 = 4;
const RBP: u16 = 5;

fn is_frame_reg(r: RegId) -> bool {
    r.0 == RSP || r.0 == RBP
}

/// A classified memory reference.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum MemRef {
    /// Stack slot at `[off, off + width_bytes)` relative to the frame base.
    Frame { off: i64, width: u32 },
    /// An absolute / ip-relative / `&global` address. We do not track the
    /// numeric address, so two globals conservatively may-alias.
    Global { width: u32 },
    /// Unconstrained: aliases everything.
    Unknown,
}

/// Classify an address expression (and its access width in *bits*) into a region.
pub fn classify(addr: &Expr, width: u32) -> MemRef {
    classify_off(addr, 0, width)
}

/// Classify `addr` carrying an accumulated constant `off` (bytes).
fn classify_off(e: &Expr, off: i64, width: u32) -> MemRef {
    match e {
        // An absolute / ip-relative address is global memory.
        Expr::Const(_, _) => MemRef::Global { width },
        // The address of a named frame slot is exactly that slot.
        Expr::Addr(Location::Frame(d)) => MemRef::Frame { off: d + off, width },
        // The address of any other named location is a global object.
        Expr::Addr(_) => MemRef::Global { width },
        // A direct read of the frame/stack pointer is the frame base.
        Expr::Read(Location::Reg(r)) if is_frame_reg(*r) => MemRef::Frame { off, width },
        // base + k / k + base: fold the constant into the offset and recurse.
        Expr::Binary(BinOp::Add, a, b) => {
            if let Expr::Const(k, _) = b.as_ref() {
                classify_off(a, off.wrapping_add(*k as i64), width)
            } else if let Expr::Const(k, _) = a.as_ref() {
                classify_off(b, off.wrapping_add(*k as i64), width)
            } else {
                // base + variable index: even a frame-based one spans an
                // unknown range, so it could alias many slots — be conservative.
                MemRef::Unknown
            }
        }
        Expr::Binary(BinOp::Sub, a, b) => {
            if let Expr::Const(k, _) = b.as_ref() {
                classify_off(a, off.wrapping_sub(*k as i64), width)
            } else {
                MemRef::Unknown
            }
        }
        // A value read from a register/memory/temp used as an address is an
        // arbitrary pointer.
        _ => MemRef::Unknown,
    }
}

/// Do two byte ranges `[o1, o1+b1)` and `[o2, o2+b2)` overlap?
fn ranges_overlap(o1: i64, b1: i64, o2: i64, b2: i64) -> bool {
    o1 < o2 + b2 && o2 < o1 + b1
}

/// Can two classified accesses touch the same byte? Conservative: `true` unless
/// provably disjoint.
pub fn may_alias(a: MemRef, b: MemRef) -> bool {
    use MemRef::*;
    match (a, b) {
        (Unknown, _) | (_, Unknown) => true,
        (Frame { off: o1, width: w1 }, Frame { off: o2, width: w2 }) => {
            ranges_overlap(o1, (w1 / 8).max(1) as i64, o2, (w2 / 8).max(1) as i64)
        }
        // Distinct address spaces never alias.
        (Frame { .. }, Global { .. }) | (Global { .. }, Frame { .. }) => false,
        // Two globals: addresses untracked, so be conservative.
        (Global { .. }, Global { .. }) => true,
    }
}

/// Walk every expression of a (pre-SSA) function, calling `f` on each node.
fn walk_exprs(func: &IrFunction, mut f: impl FnMut(&Expr)) {
    fn rec(e: &Expr, f: &mut impl FnMut(&Expr)) {
        f(e);
        match e {
            Expr::Load { addr, .. } => rec(addr, f),
            Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => rec(x, f),
            Expr::Binary(_, a, b) => {
                rec(a, f);
                rec(b, f);
            }
            Expr::Select { cond, then_, else_ } => {
                rec(cond, f);
                rec(then_, f);
                rec(else_, f);
            }
            Expr::Call { target, args, .. } => {
                if let CallTarget::Indirect(x) = target {
                    rec(x, f);
                }
                for a in args {
                    rec(a, f);
                }
            }
            _ => {}
        }
    }
    for b in &func.blocks {
        for s in &b.stmts {
            match s {
                Stmt::Set { expr, .. } | Stmt::Assign { expr, .. } | Stmt::CallStmt(expr) => {
                    rec(expr, &mut f)
                }
                Stmt::Store { addr, value, .. } => {
                    rec(addr, &mut f);
                    rec(value, &mut f);
                }
                Stmt::Branch { cond, .. } => rec(cond, &mut f),
                Stmt::Switch { value, .. } => rec(value, &mut f),
                Stmt::Return(Some(e)) => rec(e, &mut f),
                _ => {}
            }
        }
    }
}

/// Does the function materialise the address of any frame slot (`&local`)? Once
/// a slot's address is taken it can be stored or passed, so the slot can be
/// aliased by an arbitrary pointer — the classic blocker for SSA promotion.
pub fn frame_address_taken(func: &IrFunction) -> bool {
    let mut taken = false;
    walk_exprs(func, |e| {
        if matches!(e, Expr::Addr(Location::Frame(_))) {
            taken = true;
        }
    });
    taken
}

/// Does the function perform any generic memory access (`Load`/`Store`) whose
/// address classifies as `Unknown` — i.e. an arbitrary pointer that, by the
/// conservative alias rule, may overlap a frame slot?
pub fn has_unknown_access(func: &IrFunction) -> bool {
    let mut found = false;
    // Loads (anywhere in an expression) ...
    walk_exprs(func, |e| {
        if let Expr::Load { addr, ty } = e {
            if matches!(classify(addr, int_bits_u32(ty)), MemRef::Unknown) {
                found = true;
            }
        }
    });
    // ... and stores.
    for b in &func.blocks {
        for s in &b.stmts {
            if let Stmt::Store { addr, ty, .. } = s {
                if matches!(classify(addr, int_bits_u32(ty)), MemRef::Unknown) {
                    found = true;
                }
            }
        }
    }
    found
}

fn int_bits_u32(t: &Ty) -> u32 {
    match t {
        Ty::Int { bits, .. } => *bits as u32,
        _ => 64,
    }
}

/// Are this function's named frame slots safe to promote to SSA values? They are
/// when no slot address escapes *and* no unknown-pointer access could alias
/// them. Sound (over-approximate): a `false` may be overly cautious, but a
/// `true` guarantees promotion cannot change behaviour.
pub fn frame_promotable(func: &IrFunction) -> bool {
    !frame_address_taken(func) && !has_unknown_access(func)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame_reg() -> Expr {
        Expr::Read(Location::Reg(RegId(RBP)))
    }

    #[test]
    fn classifies_frame_and_global() {
        // [rbp - 8], 64-bit
        let a = Expr::Binary(BinOp::Sub, Box::new(frame_reg()), Box::new(Expr::konst(8, 64)));
        assert_eq!(classify(&a, 64), MemRef::Frame { off: -8, width: 64 });
        // absolute address
        assert_eq!(classify(&Expr::konst(0x404040, 64), 32), MemRef::Global { width: 32 });
        // &local
        assert_eq!(
            classify(&Expr::Addr(Location::Frame(-16)), 64),
            MemRef::Frame { off: -16, width: 64 }
        );
        // a loaded pointer
        assert_eq!(classify(&Expr::Use(ValueId(3)), 64), MemRef::Unknown);
    }

    #[test]
    fn disjoint_frame_slots_do_not_alias() {
        let s1 = MemRef::Frame { off: -8, width: 64 }; // bytes [-8,0)
        let s2 = MemRef::Frame { off: 0, width: 64 }; // bytes [0,8)
        assert!(!may_alias(s1, s2));
        // Overlapping (a 64-bit and an 8-bit write into it) do alias.
        let s3 = MemRef::Frame { off: -8, width: 8 }; // [-8,-7)
        assert!(may_alias(s1, s3));
    }

    #[test]
    fn frame_and_global_never_alias() {
        assert!(!may_alias(
            MemRef::Frame { off: -8, width: 64 },
            MemRef::Global { width: 64 }
        ));
    }

    #[test]
    fn unknown_aliases_everything() {
        assert!(may_alias(MemRef::Unknown, MemRef::Frame { off: 0, width: 64 }));
        assert!(may_alias(MemRef::Global { width: 8 }, MemRef::Unknown));
    }

    #[test]
    fn variable_index_is_unknown() {
        // [rbp + rax] — a variable index spans an unknown range.
        let a = Expr::Binary(
            BinOp::Add,
            Box::new(frame_reg()),
            Box::new(Expr::Use(ValueId(0))),
        );
        assert_eq!(classify(&a, 64), MemRef::Unknown);
    }

    fn func(stmts: Vec<Stmt>) -> IrFunction {
        IrFunction {
            entry: 0,
            name: "t".into(),
            bits: 64,
            reg_params: vec![],
            frame_base_values: vec![],
            fp80_values: vec![],
            frame_widths: std::collections::HashMap::new(),
            blocks: vec![Block { id: 0, addr: 0, stmts, succ: vec![], pred: vec![] }],
            next_value: 0,
            next_temp: 0,
        }
    }

    #[test]
    fn promotable_when_clean() {
        // Only a named frame read/write, no escape, no unknown access.
        let f = func(vec![
            Stmt::Set { dst: Location::Frame(-8), expr: Expr::konst(1, 64) },
            Stmt::Return(Some(Expr::Read(Location::Frame(-8)))),
        ]);
        assert!(frame_promotable(&f));
    }

    #[test]
    fn not_promotable_when_address_taken() {
        let f = func(vec![Stmt::CallStmt(Expr::Call {
            target: CallTarget::Named("g".into()),
            args: vec![Expr::Addr(Location::Frame(-8))],
            ret: Ty::Unknown,
        })]);
        assert!(frame_address_taken(&f));
        assert!(!frame_promotable(&f));
    }

    #[test]
    fn not_promotable_with_unknown_pointer_store() {
        // *(uint64_t*)(v0) = 1  — v0 is an arbitrary pointer.
        let f = func(vec![Stmt::Store {
            addr: Expr::Use(ValueId(0)),
            value: Expr::konst(1, 64),
            ty: Ty::int(64),
        }]);
        assert!(has_unknown_access(&f));
        assert!(!frame_promotable(&f));
    }
}
