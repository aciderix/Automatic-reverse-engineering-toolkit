//! Pseudo-C emitter. Renders a recovered function as structured-ish C using
//! `goto` for control flow it cannot yet reduce, with recovered branch
//! conditions. This is the honest output level of a young decompiler: real
//! per-instruction semantics + a faithful CFG, before full loop/if structuring.

use crate::analysis::{BasicBlock, Function};
use crate::disasm::Flow;
use crate::ir::{branch_condition, lift_insn, operand_to_c};
use crate::loader::Program;
use std::collections::BTreeSet;
use std::fmt::Write;

fn label(addr: u64) -> String {
    format!("L_{:08x}", addr)
}

/// Parse a `cmp`/`test` instruction text into `(lhs, rhs, is_test)`.
fn parse_cmp(text: &str) -> Option<(String, String, bool)> {
    let lower = text.trim().to_lowercase();
    let (mn, rest) = lower.split_once(' ')?;
    let is_test = match mn {
        "cmp" => false,
        "test" => true,
        _ => return None,
    };
    let mut ops = rest.split(',').map(|s| s.trim());
    let a = ops.next()?;
    let b = ops.next()?;
    Some((operand_to_c(a), operand_to_c(b), is_test))
}

/// Does this terminator consume the block's last instruction as control flow
/// (rather than a normal statement)?
fn is_control_terminator(flow: Flow) -> bool {
    matches!(
        flow,
        Flow::CondJump | Flow::Jump | Flow::Return | Flow::Indirect | Flow::Interrupt
    )
}

/// Compute the set of block addresses that need an emitted label because some
/// `goto` will reference them.
fn referenced_labels(func: &Function, order: &[u64]) -> BTreeSet<u64> {
    let mut refs = BTreeSet::new();
    for (i, &start) in order.iter().enumerate() {
        let blk = &func.blocks[&start];
        let next = order.get(i + 1).copied();
        match blk.terminator {
            Flow::CondJump => {
                if let Some(&taken) = blk.successors.first() {
                    refs.insert(taken);
                }
                if let Some(&fall) = blk.successors.get(1) {
                    if Some(fall) != next {
                        refs.insert(fall);
                    }
                }
            }
            Flow::Jump | Flow::Fallthrough | Flow::Call => {
                if let Some(&t) = blk.successors.first() {
                    if Some(t) != next {
                        refs.insert(t);
                    }
                }
            }
            Flow::Return | Flow::Indirect | Flow::Interrupt => {}
        }
    }
    // Only keep references that actually resolve to a block in this function.
    refs.retain(|a| func.blocks.contains_key(a));
    refs
}

/// Emit a `goto` to `target`, or a comment if the target is outside the
/// function (e.g. a tail call).
fn goto_or_note(out: &mut String, indent: &str, func: &Function, target: u64) {
    if func.blocks.contains_key(&target) {
        let _ = writeln!(out, "{}goto {};", indent, label(target));
    } else {
        let _ = writeln!(
            out,
            "{}/* tail transfer to 0x{:x} (outside function) */",
            indent, target
        );
    }
}

/// Render a single basic block's body and terminator.
fn emit_block(
    out: &mut String,
    prog: &Program,
    func: &Function,
    blk: &BasicBlock,
    next: Option<u64>,
) {
    let ind = "    ";
    let control = is_control_terminator(blk.terminator);
    let body_len = if control {
        blk.insns.len().saturating_sub(1)
    } else {
        blk.insns.len()
    };

    let mut last_cmp = None;
    for insn in &blk.insns[..body_len] {
        if let Some(c) = parse_cmp(&insn.text) {
            last_cmp = Some(c);
        }
        for line in lift_insn(insn, prog) {
            let _ = writeln!(out, "{}{}", ind, line);
        }
    }

    match blk.terminator {
        Flow::CondJump => {
            let jcc = blk.insns.last().unwrap();
            let cond = branch_condition(jcc, last_cmp.as_ref());
            let taken = blk.successors.first().copied();
            let fall = blk.successors.get(1).copied();
            if let Some(t) = taken {
                if func.blocks.contains_key(&t) {
                    let _ = writeln!(out, "{}if ({}) goto {};", ind, cond, label(t));
                } else {
                    let _ = writeln!(
                        out,
                        "{}if ({}) {{ /* -> 0x{:x} (outside) */ }}",
                        ind, cond, t
                    );
                }
            }
            if let Some(f) = fall {
                if Some(f) != next {
                    goto_or_note(out, ind, func, f);
                }
            }
        }
        Flow::Jump => {
            if let Some(&t) = blk.successors.first() {
                if Some(t) != next {
                    goto_or_note(out, ind, func, t);
                }
            }
        }
        Flow::Return => {
            let _ = writeln!(out, "{}return rax;", ind);
        }
        Flow::Indirect => {
            let insn = blk.insns.last().unwrap();
            let _ = writeln!(out, "{}/* indirect: {} */", ind, insn.text);
        }
        Flow::Interrupt => {
            let insn = blk.insns.last().unwrap();
            let _ = writeln!(out, "{}__asm__(\"{}\"); /* trap */", ind, insn.text);
        }
        Flow::Fallthrough | Flow::Call => {
            if let Some(&t) = blk.successors.first() {
                if Some(t) != next {
                    goto_or_note(out, ind, func, t);
                }
            }
        }
    }
}

/// Render a whole function as pseudo-C.
pub fn decompile_function(prog: &Program, func: &Function) -> String {
    let mut out = String::new();
    let order: Vec<u64> = func.blocks.keys().copied().collect();
    let refs = referenced_labels(func, &order);

    let _ = writeln!(
        out,
        "// {}  @ 0x{:x}  ({} basic blocks)",
        func.name,
        func.entry,
        func.blocks.len()
    );
    let _ = writeln!(out, "int64_t {}(void) {{", func.name);

    for (i, &start) in order.iter().enumerate() {
        let blk = &func.blocks[&start];
        let next = order.get(i + 1).copied();
        if refs.contains(&start) {
            let _ = writeln!(out, "{}:", label(start));
        }
        emit_block(&mut out, prog, func, blk, next);
    }

    let _ = writeln!(out, "}}");
    out
}
