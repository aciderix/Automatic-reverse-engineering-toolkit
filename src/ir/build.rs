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
            stmts.extend(lift(insn, bits));
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
                let t = blk.successors.first().and_then(|t| idx.get(t).copied());
                match t {
                    Some(t) => stmts.push(Stmt::Jump(BlockId(t))),
                    None => stmts.push(Stmt::Asm(blk.insns.last().unwrap().text.clone())),
                }
            }
            // Model the return value as the result register (rax/eax family),
            // so its computation stays live (analogous to the text pipeline's
            // `return eax`).
            Flow::Return => stmts.push(Stmt::Return(Some(Expr::Read(Location::Reg(RegId(0)))))),
            Flow::Indirect | Flow::Interrupt => {
                stmts.push(Stmt::Asm(blk.insns.last().unwrap().text.clone()))
            }
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

    IrFunction {
        entry: func.entry,
        name: func.name.clone(),
        bits,
        blocks,
        next_value: 0,
        next_temp: 0,
    }
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
    for b in &f.blocks {
        let succ: Vec<String> = b.succ.iter().map(|s| format!("B{}", s)).collect();
        let _ = writeln!(out, "B{} (0x{:x})  -> [{}]:", b.id, b.addr, succ.join(", "));
        for s in &b.stmts {
            let _ = writeln!(out, "    {}", stmt_str(s));
        }
    }
    out
}
