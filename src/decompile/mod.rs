//! Pseudo-C emitter. Renders a recovered function as structured-ish C using
//! `goto` for control flow it cannot yet reduce, with recovered branch
//! conditions. This is the honest output level of a young decompiler: real
//! per-instruction semantics + a faithful CFG, before full loop/if structuring.

use crate::analysis::{BasicBlock, Function};
use crate::disasm::Flow;
use crate::ir;
use crate::ir::{branch_condition, lift_insn, operand_to_c};
use crate::loader::{Bitness, Program};
use std::collections::BTreeSet;
use std::fmt::Write;

pub fn label(addr: u64) -> String {
    format!("L_{:08x}", addr)
}

/// Callee-saved registers whose lone `push`/`pop` is frame save/restore noise.
fn is_callee_saved(reg: &str) -> bool {
    matches!(
        reg,
        "ebp" | "esi" | "edi" | "ebx"
            | "rbp" | "rsi" | "rdi" | "rbx"
            | "r12" | "r13" | "r14" | "r15"
    )
}

fn is_stack_ptr(op: &str) -> bool {
    matches!(op.trim(), "esp" | "rsp")
}

/// Parse a hex (`0x..`) or decimal immediate.
fn parse_uint(s: &str) -> Option<u64> {
    let s = s.trim();
    if let Some(h) = s.strip_prefix("0x") {
        u64::from_str_radix(h, 16).ok()
    } else if s.bytes().all(|c| c.is_ascii_digit()) && !s.is_empty() {
        s.parse().ok()
    } else {
        None
    }
}

/// If the instruction after index `i` is `add esp/rsp, K`, return the implied
/// cdecl argument count (`K / pointer_size`).
fn cdecl_arg_count(body: &[crate::disasm::Insn], i: usize, ptr: u64) -> Option<usize> {
    let nxt = body.get(i + 1)?;
    let (mn, ops) = ir::split_insn(&nxt.text);
    if mn != "add" || ops.len() != 2 || !is_stack_ptr(&ops[0]) {
        return None;
    }
    let k = parse_uint(&ops[1])?;
    if ptr == 0 {
        return None;
    }
    Some((k / ptr) as usize)
}

/// True for residual frame-management lines we suppress for readability.
fn is_frame_noise(s: &str) -> bool {
    matches!(
        s,
        "ebp = esp;" | "rbp = rsp;" | "esp = ebp;" | "rsp = rbp;" | "// leave (restore frame)"
    ) || s
        .strip_prefix("push(")
        .and_then(|r| r.strip_suffix(");"))
        .map(is_callee_saved)
        .unwrap_or(false)
        || s.strip_suffix(" = pop();")
            .map(is_callee_saved)
            .unwrap_or(false)
}

/// Render the per-instruction statements of a block plus, for a conditional
/// terminator, the recovered branch condition.
///
/// Beyond raw lifting this performs light dataflow cleanup used by the
/// structured emitter: cdecl call-site argument recovery (via the trailing
/// `add esp, N`), suppression of stack-pointer bookkeeping, and removal of
/// prologue/epilogue save/restore noise. The `--flat` emitter keeps the raw
/// per-instruction form instead.
pub fn block_statements(prog: &Program, blk: &BasicBlock) -> (Vec<String>, Option<String>) {
    let ptr = (prog.bitness.bits() / 8) as u64;
    let control = is_control_terminator(blk.terminator);
    let body_len = if control {
        blk.insns.len().saturating_sub(1)
    } else {
        blk.insns.len()
    };
    let body = &blk.insns[..body_len];

    // `out` holds emitted lines; `None` marks a line consumed as a call argument.
    let mut out: Vec<Option<String>> = Vec::new();
    // (index into `out`, pushed expression) for pushes since the last call.
    let mut pending: Vec<(usize, String)> = Vec::new();
    let mut last_cmp = None;

    for (i, insn) in body.iter().enumerate() {
        if let Some(c) = parse_cmp(&insn.text) {
            last_cmp = Some(c);
        }
        let (mn, ops) = ir::split_insn(&insn.text);
        match mn.as_str() {
            "push" if ops.len() == 1 => {
                let e = ir::operand_to_c(&ops[0]);
                out.push(Some(format!("push({});", e)));
                pending.push((out.len() - 1, e));
            }
            "call" => {
                let name = ir::call_name_of(insn, prog);
                let args = match cdecl_arg_count(body, i, ptr) {
                    Some(n) => {
                        let take = n.min(pending.len());
                        let consumed = pending.split_off(pending.len() - take);
                        let mut argv = Vec::new();
                        for (idx, expr) in &consumed {
                            out[*idx] = None; // remove the push line; it's an argument
                            argv.push(expr.clone());
                        }
                        argv.reverse(); // cdecl pushes right-to-left
                        argv
                    }
                    None => Vec::new(),
                };
                pending.clear();
                out.push(Some(format!("{}({});", name, args.join(", "))));
            }
            "add" | "sub" if ops.len() == 2 && is_stack_ptr(&ops[0]) && parse_uint(&ops[1]).is_some() => {
                // Stack-pointer bookkeeping (locals alloc / cdecl cleanup): drop it.
            }
            "cmp" | "test" => out.push(Some(format!("// flags = {}", insn.text))),
            _ => {
                for line in lift_insn(insn, prog) {
                    out.push(Some(line));
                }
            }
        }
    }

    let stmts: Vec<String> = out
        .into_iter()
        .flatten()
        .filter(|s| !is_frame_noise(s))
        .collect();

    let cond = if blk.terminator == Flow::CondJump {
        Some(branch_condition(blk.insns.last().unwrap(), last_cmp.as_ref()))
    } else {
        None
    };
    (stmts, cond)
}

/// Build a function's C signature (with recovered arguments) and the list of
/// local-variable declarations, shared by both emitters.
pub fn signature_and_locals(func: &Function) -> (String, Vec<String>) {
    let (args, locals) = crate::ir::scan_frame_vars(func);
    let sig = if args.is_empty() {
        format!("int64_t {}(void)", func.name)
    } else {
        let params: Vec<String> = args.iter().map(|(n, t)| format!("{} {}", t, n)).collect();
        format!("int64_t {}({})", func.name, params.join(", "))
    };
    let decls = locals.iter().map(|(n, t)| format!("{} {};", t, n)).collect();
    (sig, decls)
}

/// The register holding a function's return value, for this target width.
pub fn return_reg(prog: &Program) -> &'static str {
    match prog.bitness {
        Bitness::Bits64 => "rax",
        Bitness::Bits32 => "eax",
    }
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
            let ret_reg = match prog.bitness {
                Bitness::Bits64 => "rax",
                Bitness::Bits32 => "eax",
            };
            let _ = writeln!(out, "{}return {};", ind, ret_reg);
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
    let (sig, decls) = signature_and_locals(func);
    let _ = writeln!(out, "{} {{", sig);
    for d in &decls {
        let _ = writeln!(out, "    {}", d);
    }

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
