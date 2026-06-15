//! Typed SSA intermediate representation — the foundation the roadmap calls for.
//!
//! The current pipeline lifts machine code straight to C *text* and re-parses
//! it for dataflow, which caps what analyses are possible (no real constant
//! folding, no propagation across branch joins, no type inference). This module
//! defines an expression-tree + SSA IR that those analyses can target instead.
//!
//! It is being built **in parallel** with the working text pipeline (roadmap
//! §3.5): the data model lands first, then the lifter, SSA construction, and
//! optimisation passes are added behind a flag until they reach parity, at
//! which point the text manipulation in `dataflow` is retired.
//!
//! Nothing here is wired into the default pipeline yet.
#![allow(dead_code)]

use std::fmt;

/// An SSA value: a versioned definition of some [`Location`].
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct ValueId(pub u32);

/// Canonical register family identifier (mirrors `dataflow::reg_family`, but as
/// a small integer so the IR is cheap to compare/hash). Sub-register width is
/// carried separately on the access, not the family.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct RegId(pub u16);

/// The CPU condition flags, modelled explicitly so `cmp`/`test` define them and
/// `jcc`/`setcc`/`cmov` read them. This is what lets branch conditions be
/// recovered by dataflow rather than by re-parsing the nearest `cmp`.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub enum FlagKind {
    Zf,
    Sf,
    Of,
    Cf,
    Pf,
    Af,
}

/// An abstract storage location, before SSA renaming.
#[derive(Clone, PartialEq, Eq, Hash, Debug)]
pub enum Location {
    /// A general-purpose register family.
    Reg(RegId),
    /// A CPU flag.
    Flag(FlagKind),
    /// A stack-frame slot at a normalised `rbp/rsp + disp` offset.
    Frame(i64),
    /// Generic memory (refined by alias analysis later).
    Mem,
    /// A temporary introduced by the lifter.
    Temp(u32),
}

/// A recovered type. A small lattice: `Unknown` is ⊤, a conflict collapses to a
/// concrete fallback. Filled in by the constraint-based type-inference pass.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Ty {
    Unknown,
    /// Integer of `bits` width; `signed` stays `None` until determined.
    Int { bits: u8, signed: Option<bool> },
    Ptr(Box<Ty>),
    Float { bits: u8 },
    /// A reconstructed struct/array, referenced by id.
    Aggregate(u32),
    /// A function/code pointer.
    Code,
    Bool,
}

impl Ty {
    pub const fn int(bits: u8) -> Ty {
        Ty::Int { bits, signed: None }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum UnOp {
    Neg,
    Not,
    /// Sign/zero extension to a wider width.
    SignExtend,
    ZeroExtend,
    /// Truncate to a narrower width.
    Truncate,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum BinOp {
    Add,
    Sub,
    Mul,
    UDiv,
    SDiv,
    UMod,
    SMod,
    And,
    Or,
    Xor,
    Shl,
    Shr,  // logical
    Sar,  // arithmetic
    // Relational (produce a Bool); signedness encoded in the variant.
    Eq,
    Ne,
    Ult,
    Ule,
    Ugt,
    Uge,
    Slt,
    Sle,
    Sgt,
    Sge,
}

/// Where a call transfers to.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum CallTarget {
    /// A resolved function address.
    Direct(u64),
    /// An indirect target computed at runtime.
    Indirect(Box<Expr>),
    /// A named import (PLT/IAT) or library function.
    Named(String),
}

/// An expression tree. The core of the IR: every value is an expression, not a
/// register name in a string.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Expr {
    Const(i128, Ty),
    /// Pre-SSA read of a location (before SSA renaming turns it into `Use`).
    Read(Location),
    /// Post-SSA read of a versioned value.
    Use(ValueId),
    Load {
        addr: Box<Expr>,
        ty: Ty,
    },
    Unary(UnOp, Box<Expr>),
    Binary(BinOp, Box<Expr>, Box<Expr>),
    Cast {
        to: Ty,
        expr: Box<Expr>,
    },
    /// Address of a location (`&local`, `&global`).
    Addr(Location),
    Call {
        target: CallTarget,
        args: Vec<Expr>,
        ret: Ty,
    },
    /// Conditional select (`cmov`, `? :`): `cond ? then_ : else_`.
    Select {
        cond: Box<Expr>,
        then_: Box<Expr>,
        else_: Box<Expr>,
    },
    /// SSA φ-node merging values from predecessor blocks.
    Phi(Vec<ValueId>),
    /// Unknown / uninitialised.
    Undef,
}

impl Expr {
    pub fn konst(v: i128, bits: u8) -> Expr {
        Expr::Const(v, Ty::int(bits))
    }
}

/// A block identifier in a function's CFG.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct BlockId(pub u32);

/// An IR statement.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Stmt {
    /// Pre-SSA assignment to a location (produced by the lifter; SSA
    /// construction rewrites it into `Assign`).
    Set { dst: Location, expr: Expr },
    /// SSA definition: `dst` is defined here as `expr`.
    Assign { dst: ValueId, expr: Expr },
    Store { addr: Expr, value: Expr, ty: Ty },
    Branch {
        cond: Expr,
        taken: BlockId,
        fallthrough: BlockId,
    },
    Jump(BlockId),
    Switch {
        value: Expr,
        cases: Vec<(i128, BlockId)>,
        default: BlockId,
    },
    Return(Option<Expr>),
    /// A call whose return value is ignored.
    CallStmt(Expr),
    /// An instruction we cannot (yet) lift — the safety valve. Keeping the raw
    /// assembly text guarantees the emitted code is never silently wrong.
    Asm(String),
    Nop,
}

/// A basic block of IR: a label, its statements, and CFG successors.
#[derive(Clone, Debug, Default)]
pub struct Block {
    pub id: u32,
    pub addr: u64,
    pub stmts: Vec<Stmt>,
    pub succ: Vec<u32>,
    pub pred: Vec<u32>,
}

/// A function lifted to IR.
#[derive(Clone, Debug, Default)]
pub struct IrFunction {
    pub entry: u64,
    pub name: String,
    /// Target pointer width in bits (32 or 64), for frame-slot classification.
    pub bits: u32,
    /// SSA value ids that are register-passed parameters (64-bit ABIs), in
    /// calling-convention order. Populated by SSA construction.
    pub reg_params: Vec<u32>,
    /// SSA values that are the entry (undef) versions of rsp/rbp — pointed at a
    /// real local frame array by stack-variable promotion. Populated by SSA.
    pub frame_base_values: Vec<u32>,
    /// SSA values that live in an x87 FPU stack slot — emitted as `long double`
    /// (80-bit extended precision) rather than `uint64_t`. Populated by SSA.
    pub fp80_values: Vec<u32>,
    pub blocks: Vec<Block>,
    /// Next fresh SSA value id.
    pub next_value: u32,
    /// Next fresh temporary id.
    pub next_temp: u32,
}

impl IrFunction {
    pub fn fresh_value(&mut self) -> ValueId {
        let v = ValueId(self.next_value);
        self.next_value += 1;
        v
    }

    pub fn fresh_temp(&mut self) -> Location {
        let t = Location::Temp(self.next_temp);
        self.next_temp += 1;
        t
    }
}

impl fmt::Display for BinOp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        use BinOp::*;
        let s = match self {
            Add => "+",
            Sub => "-",
            Mul => "*",
            UDiv | SDiv => "/",
            UMod | SMod => "%",
            And => "&",
            Or => "|",
            Xor => "^",
            Shl => "<<",
            Shr | Sar => ">>",
            Eq => "==",
            Ne => "!=",
            Ult | Slt => "<",
            Ule | Sle => "<=",
            Ugt | Sgt => ">",
            Uge | Sge => ">=",
        };
        f.write_str(s)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fresh_ids_advance() {
        let mut f = IrFunction::default();
        assert_eq!(f.fresh_value(), ValueId(0));
        assert_eq!(f.fresh_value(), ValueId(1));
        assert!(matches!(f.fresh_temp(), Location::Temp(0)));
    }

    #[test]
    fn expr_tree_builds() {
        // (use(0) + 8) as a typed expression tree, no strings involved.
        let e = Expr::Binary(
            BinOp::Add,
            Box::new(Expr::Use(ValueId(0))),
            Box::new(Expr::konst(8, 32)),
        );
        match e {
            Expr::Binary(BinOp::Add, _, _) => {}
            _ => panic!("wrong shape"),
        }
    }
}
