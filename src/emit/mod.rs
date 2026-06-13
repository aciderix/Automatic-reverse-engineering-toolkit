//! IR → compilable C emitter (roadmap §7), first milestone: goto-based C that
//! recompiles. Structured emission (reusing `structure`) and typed variables
//! come later; this closes the loop enough to start the verification harness
//! (§8, "recompiles" level).
//!
//! Pipeline: optimized SSA IR → SSA destruction (φ lowering) → C text.
//! Every value becomes a `uint64_t`; unmodelled instructions are emitted as
//! comments (their clobbers were already `Undef`), so the output compiles.
#![allow(dead_code)]

pub mod structured;

use crate::ir::types::*;
use std::collections::BTreeSet;
use std::fmt::Write;

/// Lower φ-nodes out of SSA by inserting, on every incoming edge, a small block
/// that copies each φ argument into the φ's value, then jumps to the join.
/// Inserting on *every* edge (not only critical ones) keeps it simple and
/// correct: copies always live in a dedicated single-pred/single-succ block.
pub(crate) fn destruct_ssa(func: &mut IrFunction) {
    let n = func.blocks.len();
    let mut next_id = func.blocks.iter().map(|b| b.id).max().unwrap_or(0) + 1;
    let mut new_blocks: Vec<Block> = Vec::new();

    for s in 0..n {
        let phis: Vec<(ValueId, Vec<ValueId>)> = func.blocks[s]
            .stmts
            .iter()
            .filter_map(|st| match st {
                Stmt::Assign { dst, expr: Expr::Phi(args) } => Some((*dst, args.clone())),
                _ => None,
            })
            .collect();
        if phis.is_empty() {
            continue;
        }
        let preds = func.blocks[s].pred.clone();
        let s_id = func.blocks[s].id;
        for (i, &p) in preds.iter().enumerate() {
            let mid = next_id;
            next_id += 1;
            let mut stmts: Vec<Stmt> = phis
                .iter()
                .map(|(dst, args)| Stmt::Assign {
                    dst: *dst,
                    expr: Expr::Use(args[i]),
                })
                .collect();
            stmts.push(Stmt::Jump(BlockId(s_id)));
            new_blocks.push(Block {
                id: mid,
                addr: 0,
                stmts,
                succ: vec![s_id],
                pred: vec![p],
            });
            // Redirect predecessor p's terminator edge s_id -> mid.
            let pb = func.blocks.iter_mut().find(|b| b.id == p).unwrap();
            for su in pb.succ.iter_mut() {
                if *su == s_id {
                    *su = mid;
                }
            }
            redirect_terminator(pb.stmts.last_mut(), s_id, mid);
        }
        // The join's predecessors are now the new copy blocks; drop its φs.
        func.blocks[s].pred = (0..preds.len() as u32).map(|k| next_id - preds.len() as u32 + k).collect();
        func.blocks[s]
            .stmts
            .retain(|st| !matches!(st, Stmt::Assign { expr: Expr::Phi(_), .. }));
    }
    func.blocks.extend(new_blocks);
}

fn redirect_terminator(last: Option<&mut Stmt>, from: u32, to: u32) {
    if let Some(st) = last {
        match st {
            Stmt::Jump(b) => {
                if b.0 == from {
                    b.0 = to;
                }
            }
            Stmt::Branch { taken, fallthrough, .. } => {
                if taken.0 == from {
                    taken.0 = to;
                }
                if fallthrough.0 == from {
                    fallthrough.0 = to;
                }
            }
            Stmt::Switch { cases, default, .. } => {
                for (_, b) in cases.iter_mut() {
                    if b.0 == from {
                        b.0 = to;
                    }
                }
                if default.0 == from {
                    default.0 = to;
                }
            }
            _ => {}
        }
    }
}

pub(crate) fn ctype(bits: u8) -> &'static str {
    match bits {
        8 => "uint8_t",
        16 => "uint16_t",
        32 => "uint32_t",
        _ => "uint64_t",
    }
}

fn ty_ctype(t: &Ty) -> &'static str {
    match t {
        Ty::Int { bits, .. } => ctype(*bits),
        _ => "uint64_t",
    }
}

fn const_c(v: i128) -> String {
    if v < 0 {
        format!("(uint64_t)({})", v)
    } else if v > 9 {
        format!("0x{:x}ULL", v as u128)
    } else {
        format!("{}", v)
    }
}

/// Name a recovered stack-frame slot.
pub(crate) fn frame_name(d: i64) -> String {
    if d == 0 {
        "saved_bp".into()
    } else if d > 0 {
        format!("arg_{:x}", d)
    } else {
        format!("local_{:x}", -d)
    }
}

pub(crate) fn expr_c(e: &Expr) -> String {
    match e {
        Expr::Const(v, _) => const_c(*v),
        Expr::Use(v) => format!("v{}", v.0),
        Expr::Undef => "0 /*undef*/".into(),
        Expr::Read(Location::Frame(d)) => frame_name(*d),
        Expr::Read(_) => "0 /*reg*/".into(), // other reads don't remain post-SSA
        Expr::Load { addr, ty } => format!("(*({}*)({}))", ctype(int_bits(ty)), expr_c(addr)),
        Expr::Unary(op, x) => {
            let xs = expr_c(x);
            match op {
                UnOp::Neg => format!("(-({}))", xs),
                UnOp::Not => format!("(~({}))", xs),
                // width lost; emit the value (approximate extension/truncation)
                _ => format!("({})", xs),
            }
        }
        Expr::Binary(op, a, b) => binary_c(*op, &expr_c(a), &expr_c(b)),
        Expr::Cast { to, expr } => format!("(({})({}))", ty_ctype(to), expr_c(expr)),
        Expr::Addr(Location::Frame(d)) => format!("(uint64_t)(&{})", frame_name(*d)),
        Expr::Addr(_) => "0 /*addr*/".into(),
        Expr::Call { target, args, .. } => {
            let a: Vec<String> = args.iter().map(expr_c).collect();
            match target {
                CallTarget::Direct(addr) => format!("sub_{:x}({})", addr, a.join(", ")),
                CallTarget::Named(n) if n.starts_with("asm:") => {
                    format!("0 /*{}*/", n)
                }
                CallTarget::Named(n) => format!("{}({})", n, a.join(", ")),
                CallTarget::Indirect(_) => "0 /*indirect call*/".into(),
            }
        }
        Expr::Select { cond, then_, else_ } => {
            format!("({} ? {} : {})", expr_c(cond), expr_c(then_), expr_c(else_))
        }
        Expr::Phi(_) => "0 /*phi*/".into(),
    }
}

pub(crate) fn int_bits(t: &Ty) -> u8 {
    match t {
        Ty::Int { bits, .. } => *bits,
        _ => 64,
    }
}

fn binary_c(op: BinOp, a: &str, b: &str) -> String {
    use BinOp::*;
    let u = |x: &str| format!("(uint64_t)({})", x);
    let i = |x: &str| format!("(int64_t)({})", x);
    let sym = |o: &str, x: &str, y: &str| format!("({} {} {})", x, o, y);
    match op {
        Add => sym("+", a, b),
        Sub => sym("-", a, b),
        Mul => sym("*", a, b),
        And => sym("&", a, b),
        Or => sym("|", a, b),
        Xor => sym("^", a, b),
        Shl => sym("<<", a, b),
        Shr => sym(">>", &u(a), b),
        Sar => sym(">>", &i(a), b),
        UDiv => sym("/", &u(a), &u(b)),
        SDiv => sym("/", &i(a), &i(b)),
        UMod => sym("%", &u(a), &u(b)),
        SMod => sym("%", &i(a), &i(b)),
        Eq => sym("==", a, b),
        Ne => sym("!=", a, b),
        Ult => sym("<", &u(a), &u(b)),
        Ule => sym("<=", &u(a), &u(b)),
        Ugt => sym(">", &u(a), &u(b)),
        Uge => sym(">=", &u(a), &u(b)),
        Slt => sym("<", &i(a), &i(b)),
        Sle => sym("<=", &i(a), &i(b)),
        Sgt => sym(">", &i(a), &i(b)),
        Sge => sym(">=", &i(a), &i(b)),
    }
}

/// Collect every `ValueId` referenced anywhere (defs, uses, φ args) so they can
/// all be declared (including undef versions that are read but never assigned).
pub(crate) fn collect_values(func: &IrFunction, out: &mut BTreeSet<u32>) {
    fn walk(e: &Expr, out: &mut BTreeSet<u32>) {
        match e {
            Expr::Use(v) => {
                out.insert(v.0);
            }
            Expr::Phi(args) => {
                for v in args {
                    out.insert(v.0);
                }
            }
            Expr::Load { addr, .. } => walk(addr, out),
            Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => walk(x, out),
            Expr::Binary(_, a, b) => {
                walk(a, out);
                walk(b, out);
            }
            Expr::Call { target, args, .. } => {
                if let CallTarget::Indirect(x) = target {
                    walk(x, out);
                }
                for a in args {
                    walk(a, out);
                }
            }
            Expr::Select { cond, then_, else_ } => {
                walk(cond, out);
                walk(then_, out);
                walk(else_, out);
            }
            _ => {}
        }
    }
    for b in &func.blocks {
        for s in &b.stmts {
            if let Stmt::Assign { dst, .. } = s {
                out.insert(dst.0);
            }
            match s {
                Stmt::Set { expr, .. }
                | Stmt::Assign { expr, .. }
                | Stmt::CallStmt(expr) => walk(expr, out),
                Stmt::Store { addr, value, .. } => {
                    walk(addr, out);
                    walk(value, out);
                }
                Stmt::Branch { cond, .. } => walk(cond, out),
                Stmt::Switch { value, .. } => walk(value, out),
                Stmt::Return(Some(e)) => walk(e, out),
                _ => {}
            }
        }
    }
}

/// Collect recovered stack-frame slot displacements (for declaration).
pub(crate) fn collect_frame_vars(func: &IrFunction, out: &mut BTreeSet<i64>) {
    fn walk(e: &Expr, out: &mut BTreeSet<i64>) {
        match e {
            Expr::Read(Location::Frame(d)) | Expr::Addr(Location::Frame(d)) => {
                out.insert(*d);
            }
            Expr::Load { addr, .. } => walk(addr, out),
            Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => walk(x, out),
            Expr::Binary(_, a, b) => {
                walk(a, out);
                walk(b, out);
            }
            Expr::Call { args, .. } => {
                for a in args {
                    walk(a, out);
                }
            }
            Expr::Select { cond, then_, else_ } => {
                walk(cond, out);
                walk(then_, out);
                walk(else_, out);
            }
            _ => {}
        }
    }
    for b in &func.blocks {
        for s in &b.stmts {
            if let Stmt::Set { dst: Location::Frame(d), .. } = s {
                out.insert(*d);
            }
            match s {
                Stmt::Set { expr, .. }
                | Stmt::Assign { expr, .. }
                | Stmt::CallStmt(expr) => walk(expr, out),
                Stmt::Store { addr, value, .. } => {
                    walk(addr, out);
                    walk(value, out);
                }
                Stmt::Branch { cond, .. } => walk(cond, out),
                Stmt::Return(Some(e)) => walk(e, out),
                _ => {}
            }
        }
    }
}

/// Render the local declaration line for frame variables, if any.
pub(crate) fn frame_decls(func: &IrFunction) -> Option<String> {
    let mut fv = BTreeSet::new();
    collect_frame_vars(func, &mut fv);
    if fv.is_empty() {
        return None;
    }
    let decls: Vec<String> = fv.iter().map(|d| format!("{} = 0", frame_name(*d))).collect();
    Some(format!("    uint64_t {};", decls.join(", ")))
}

/// Collect direct call targets, to forward-declare them.
pub(crate) fn collect_callees(func: &IrFunction, out: &mut BTreeSet<u64>) {
    fn walk(e: &Expr, out: &mut BTreeSet<u64>) {
        match e {
            Expr::Call { target: CallTarget::Direct(a), args, .. } => {
                out.insert(*a);
                for x in args {
                    walk(x, out);
                }
            }
            Expr::Call { args, .. } => {
                for x in args {
                    walk(x, out);
                }
            }
            Expr::Load { addr, .. } => walk(addr, out),
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
            _ => {}
        }
    }
    for b in &func.blocks {
        for s in &b.stmts {
            match s {
                Stmt::Set { expr, .. } | Stmt::Assign { expr, .. } | Stmt::CallStmt(expr) => {
                    walk(expr, out)
                }
                Stmt::Store { addr, value, .. } => {
                    walk(addr, out);
                    walk(value, out);
                }
                Stmt::Return(Some(e)) => walk(e, out),
                _ => {}
            }
        }
    }
}

fn stmt_c(s: &Stmt, out: &mut String) {
    match s {
        Stmt::Assign { dst, expr } => {
            let _ = writeln!(out, "    v{} = {};", dst.0, expr_c(expr));
        }
        Stmt::Set { dst: Location::Frame(d), expr } => {
            let _ = writeln!(out, "    {} = {};", frame_name(*d), expr_c(expr));
        }
        Stmt::Set { .. } => {}
        Stmt::Store { addr, value, ty } => {
            let _ = writeln!(
                out,
                "    *({}*)({}) = {};",
                ctype(int_bits(ty)),
                expr_c(addr),
                expr_c(value)
            );
        }
        Stmt::Branch { cond, taken, fallthrough } => {
            let _ = writeln!(out, "    if ({}) goto B{};", expr_c(cond), taken.0);
            let _ = writeln!(out, "    goto B{};", fallthrough.0);
        }
        Stmt::Jump(b) => {
            let _ = writeln!(out, "    goto B{};", b.0);
        }
        Stmt::Switch { value, default, .. } => {
            let _ = writeln!(out, "    (void)({}); goto B{};", expr_c(value), default.0);
        }
        Stmt::Return(Some(e)) => {
            let _ = writeln!(out, "    return {};", expr_c(e));
        }
        Stmt::Return(None) => {
            let _ = writeln!(out, "    return 0;");
        }
        Stmt::CallStmt(e) => {
            let _ = writeln!(out, "    (void)({});", expr_c(e));
        }
        Stmt::Asm(t) => {
            let _ = writeln!(out, "    /* asm: {} */", t);
        }
        Stmt::Nop => {}
    }
}

/// Emit one function as compilable C (goto form). `forward` receives the
/// forward declarations this function needs (callees).
pub fn emit_function(func: &IrFunction, forward: &mut BTreeSet<u64>) -> String {
    let mut f = func.clone();
    destruct_ssa(&mut f);

    let mut values = BTreeSet::new();
    collect_values(&f, &mut values);
    collect_callees(&f, forward);

    let mut out = String::new();
    let _ = writeln!(out, "uint64_t sub_{:x}(void) {{", f.entry);
    if !values.is_empty() {
        let decls: Vec<String> = values.iter().map(|v| format!("v{} = 0", v)).collect();
        let _ = writeln!(out, "    uint64_t {};", decls.join(", "));
    }
    if let Some(fd) = frame_decls(&f) {
        let _ = writeln!(out, "{}", fd);
    }
    // Enter at the function's entry block.
    let entry_id = f
        .blocks
        .iter()
        .find(|b| b.addr == f.entry)
        .map(|b| b.id)
        .unwrap_or(0);
    let _ = writeln!(out, "    goto B{};", entry_id);
    for b in &f.blocks {
        let _ = writeln!(out, "B{}:;", b.id);
        for s in &b.stmts {
            stmt_c(s, &mut out);
        }
        // A block with no terminator falls through; guard with a return.
        if !ends_in_terminator(b) {
            let _ = writeln!(out, "    return 0;");
        }
    }
    let _ = writeln!(out, "}}");
    out
}

fn ends_in_terminator(b: &Block) -> bool {
    matches!(
        b.stmts.last(),
        Some(Stmt::Jump(_) | Stmt::Branch { .. } | Stmt::Return(_) | Stmt::Switch { .. })
    )
}

/// Emit a complete compilable C translation unit for the given functions.
pub fn emit_unit(funcs: &[IrFunction]) -> String {
    let mut body = String::new();
    let mut forward: BTreeSet<u64> = BTreeSet::new();
    let defined: BTreeSet<u64> = funcs.iter().map(|f| f.entry).collect();
    for f in funcs {
        body.push_str(&emit_function(f, &mut forward));
        body.push('\n');
    }
    let mut out = String::new();
    out.push_str("#include <stdint.h>\n\n");
    // Forward-declare callees that aren't defined in this unit.
    for a in forward.difference(&defined) {
        let _ = writeln!(out, "uint64_t sub_{:x}(void);", a);
    }
    out.push('\n');
    out.push_str(&body);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn emits_and_lowers_phi() {
        // Diamond with a phi -> after destruction, no Phi remains and copy
        // blocks are inserted.
        let rax = Location::Reg(RegId(0));
        let mut f = IrFunction {
            entry: 0,
            name: "t".into(),
            blocks: vec![
                Block { id: 0, addr: 0, stmts: vec![Stmt::Set { dst: rax.clone(), expr: Expr::konst(1, 32) }, Stmt::Branch { cond: Expr::konst(1, 8), taken: BlockId(1), fallthrough: BlockId(2) }], succ: vec![1, 2], pred: vec![] },
                Block { id: 1, addr: 1, stmts: vec![Stmt::Set { dst: rax.clone(), expr: Expr::konst(2, 32) }, Stmt::Jump(BlockId(3)) ], succ: vec![3], pred: vec![0] },
                Block { id: 2, addr: 2, stmts: vec![Stmt::Jump(BlockId(3))], succ: vec![3], pred: vec![0] },
                Block { id: 3, addr: 3, stmts: vec![Stmt::Return(Some(Expr::Read(rax.clone())))], succ: vec![], pred: vec![1, 2] },
            ],
            next_value: 0,
            next_temp: 0,
        };
        crate::ssa::to_ssa(&mut f);
        crate::opt::optimize(&mut f);
        let mut fwd = BTreeSet::new();
        let c = emit_function(&f, &mut fwd);
        assert!(c.contains("uint64_t sub_0(void)"));
        assert!(!c.contains("phi"), "phi must be lowered, got:\n{}", c);
        assert!(c.contains("return"));
    }
}
