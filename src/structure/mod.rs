//! Control-flow structuring: turn a function's CFG into structured C
//! (`if`/`else`, `while`) instead of a flat list of blocks and `goto`s.
//!
//! Approach: compute the dominator tree and post-dominator tree, detect natural
//! loops from back edges, then emit code with a recursive region walker. Any
//! edge the structurer cannot express cleanly degrades to an explicit `goto`,
//! so the output is always semantically faithful even where structure was not
//! recovered.

use crate::analysis::Function;
use crate::dataflow::{optimize_function, FunctionCode};
use crate::decompile::{label, return_reg, signature_and_locals};
use crate::disasm::Flow;
use crate::loader::Program;
use std::collections::HashMap;
use std::fmt::Write;

const UNDEF: usize = usize::MAX;

/// Per-function structuring state.
struct Structurer<'a> {
    prog: &'a Program,
    func: &'a Function,
    nodes: Vec<u64>,
    idx: HashMap<u64, usize>,
    /// Internal successor block indices, order-preserving (CondJump = [taken, fall]).
    succ: Vec<Vec<usize>>,
    /// Internal predecessor indices.
    preds: Vec<Vec<usize>>,
    idom: Vec<usize>,
    ipdom: Vec<usize>,
    /// True if node is the header of a natural loop.
    is_header: Vec<bool>,
    /// In-degree from internal edges (used to decide which nodes get a label).
    indeg: Vec<usize>,
    entry: usize,
    emitted: Vec<bool>,
    /// Dataflow-optimised statements + condition per block start address.
    code: FunctionCode,
    out: String,
}

/// Region context threaded through the recursive walk.
#[derive(Clone, Copy)]
struct Ctx {
    stop: usize,   // emission of this node ends the current region
    brk: usize,    // reaching this node emits `break`
    cont: usize,   // reaching this node emits `continue`
}

impl Ctx {
    fn root() -> Ctx {
        Ctx {
            stop: UNDEF,
            brk: UNDEF,
            cont: UNDEF,
        }
    }
}

/// Structure and render a single function as pseudo-C.
pub fn structure_function(prog: &Program, func: &Function) -> String {
    let s = Structurer::new(prog, func);
    drop_redundant_continues(s.run())
}

/// Remove a `continue;` that is followed only by the closing braces leading out
/// of its enclosing `while (true)` loop — falling through is then identical to
/// continuing, so the statement is pure noise. Provably semantics-preserving.
fn drop_redundant_continues(text: String) -> String {
    let lines: Vec<&str> = text.lines().collect();
    // Track the block-nesting stack: true = a `while (true)` loop.
    let mut stack: Vec<bool> = Vec::new();
    let mut drop: Vec<bool> = vec![false; lines.len()];

    for (i, raw) in lines.iter().enumerate() {
        let t = raw.trim();
        if t == "continue;" {
            // Closes needed to fall out past the innermost enclosing loop.
            if let Some(loop_depth) = stack.iter().rposition(|&is_loop| is_loop) {
                let closes_needed = stack.len() - loop_depth;
                // The next `closes_needed` non-empty lines must all be `}`.
                let mut seen = 0;
                let mut ok = true;
                for next in lines.iter().skip(i + 1) {
                    let nt = next.trim();
                    if nt.is_empty() {
                        continue;
                    }
                    if nt == "}" {
                        seen += 1;
                        if seen == closes_needed {
                            break;
                        }
                    } else {
                        ok = false;
                        break;
                    }
                }
                if ok && seen == closes_needed {
                    drop[i] = true;
                }
            }
            continue;
        }
        // Maintain the nesting stack.
        if t.ends_with('{') {
            if t == "} else {" {
                // closes an if, opens an if — net stack type unchanged
            } else {
                stack.push(t.starts_with("while (true)"));
            }
        } else if t == "}" {
            stack.pop();
        }
    }

    let mut out = String::with_capacity(text.len());
    for (i, raw) in lines.iter().enumerate() {
        if drop[i] {
            continue;
        }
        out.push_str(raw);
        out.push('\n');
    }
    out
}

impl<'a> Structurer<'a> {
    fn new(prog: &'a Program, func: &'a Function) -> Self {
        let nodes: Vec<u64> = func.blocks.keys().copied().collect();
        let idx: HashMap<u64, usize> =
            nodes.iter().enumerate().map(|(i, &a)| (a, i)).collect();
        let n = nodes.len();

        let mut succ = vec![Vec::new(); n];
        let mut preds = vec![Vec::new(); n];
        let mut indeg = vec![0usize; n];
        for (i, &a) in nodes.iter().enumerate() {
            let blk = &func.blocks[&a];
            for &t in &blk.successors {
                if let Some(&j) = idx.get(&t) {
                    succ[i].push(j);
                    preds[j].push(i);
                    indeg[j] += 1;
                }
            }
        }

        let entry = idx.get(&func.entry).copied().unwrap_or(0);

        let mut me = Structurer {
            prog,
            func,
            nodes,
            idx,
            succ,
            preds,
            idom: vec![UNDEF; n],
            ipdom: vec![UNDEF; n],
            is_header: vec![false; n],
            indeg,
            entry,
            emitted: vec![false; n],
            code: optimize_function(prog, func),
            out: String::new(),
        };
        me.compute_dominators();
        me.compute_postdominators();
        me.detect_loops();
        me
    }

    fn blk(&self, i: usize) -> &crate::analysis::BasicBlock {
        &self.func.blocks[&self.nodes[i]]
    }

    // ---- dominator computation (shared crate::cfg::dom) ------------------

    fn compute_dominators(&mut self) {
        self.idom =
            crate::cfg::dom::dominators(self.nodes.len(), self.entry, &self.succ, &self.preds);
    }

    /// Post-dominators via dominators on the reversed graph with a virtual exit.
    fn compute_postdominators(&mut self) {
        let n = self.nodes.len();
        let virt = n; // virtual exit index
        let total = n + 1;

        // Reversed adjacency: rev_succ[u] = original preds; virtual -> exits.
        let mut rev_succ = vec![Vec::new(); total];
        let mut rev_pred = vec![Vec::new(); total];
        for i in 0..n {
            for &p in &self.preds[i] {
                rev_succ[i].push(p);
                rev_pred[p].push(i);
            }
        }
        // A node is an exit if control can leave the function there.
        for i in 0..n {
            let blk = self.blk(i);
            let leaves = matches!(
                blk.terminator,
                Flow::Return | Flow::Indirect | Flow::Interrupt
            ) || self.succ[i].is_empty()
                || blk.successors.iter().any(|t| !self.idx.contains_key(t));
            if leaves {
                rev_succ[virt].push(i);
                rev_pred[i].push(virt);
            }
        }

        let ipdom_full = crate::cfg::dom::dominators(total, virt, &rev_succ, &rev_pred);
        self.ipdom = ipdom_full[..n].to_vec();
        // Map the virtual exit (and unreachable) to UNDEF so callers treat it
        // as "no real post-dominator".
        for v in self.ipdom.iter_mut() {
            if *v == virt {
                *v = UNDEF;
            }
        }
    }

    fn dominates(&self, a: usize, b: usize) -> bool {
        crate::cfg::dom::dominates(a, b, &self.idom)
    }

    /// Mark loop headers: a back edge u->h exists when h dominates u.
    fn detect_loops(&mut self) {
        let n = self.nodes.len();
        for u in 0..n {
            for &h in &self.succ[u].clone() {
                if self.dominates(h, u) {
                    self.is_header[h] = true;
                }
            }
        }
    }

    /// Collect the body nodes of the natural loop with the given header, and
    /// the chosen follow (exit) node, if any.
    fn loop_info(&self, header: usize) -> (Vec<bool>, Option<usize>) {
        let n = self.nodes.len();
        let mut in_loop = vec![false; n];
        in_loop[header] = true;
        // Latches: predecessors of header dominated by header (back edges).
        let mut stack = Vec::new();
        for &p in &self.preds[header] {
            if self.dominates(header, p) && !in_loop[p] {
                in_loop[p] = true;
                stack.push(p);
            }
        }
        while let Some(node) = stack.pop() {
            for &p in &self.preds[node] {
                if !in_loop[p] {
                    in_loop[p] = true;
                    stack.push(p);
                }
            }
        }
        // Follow = a successor of a loop node that is outside the loop.
        // Prefer the exit of the header's conditional; else the smallest-index
        // external target.
        let mut follow: Option<usize> = None;
        for i in 0..n {
            if !in_loop[i] {
                continue;
            }
            for &t in &self.succ[i] {
                if !in_loop[t] {
                    follow = Some(match follow {
                        Some(f) => f.min(t),
                        None => t,
                    });
                }
            }
        }
        (in_loop, follow)
    }

    // ---- emission --------------------------------------------------------

    fn run(mut self) -> String {
        let _ = writeln!(
            self.out,
            "// {}  @ 0x{:x}  ({} basic blocks)",
            self.func.name,
            self.func.entry,
            self.func.blocks.len()
        );
        let (sig, decls) = signature_and_locals(self.func);
        let _ = writeln!(self.out, "{} {{", sig);
        for d in &decls {
            self.line(1, d);
        }
        if !decls.is_empty() {
            self.out.push('\n');
        }
        let entry = self.entry;
        self.emit_seq(entry, Ctx::root(), 1);
        let _ = writeln!(self.out, "}}");
        self.out
    }

    fn line(&mut self, depth: usize, s: &str) {
        for _ in 0..depth {
            self.out.push_str("    ");
        }
        self.out.push_str(s);
        self.out.push('\n');
    }

    /// Emit a node and the region it begins, following the chain of control.
    fn emit_seq(&mut self, start: usize, ctx: Ctx, depth: usize) {
        let mut cur = start;
        loop {
            if cur == ctx.stop {
                return;
            }
            if cur == ctx.cont {
                self.line(depth, "continue;");
                return;
            }
            if cur == ctx.brk {
                self.line(depth, "break;");
                return;
            }
            if self.emitted[cur] {
                self.line(depth, &format!("goto {};", label(self.nodes[cur])));
                return;
            }
            self.emitted[cur] = true;

            // Label join points / loop headers so gotos can target them.
            if self.indeg[cur] > 1 || self.is_header[cur] {
                let lbl = label(self.nodes[cur]);
                self.line(depth.saturating_sub(1).max(0), &format!("{}:", lbl));
            }

            if self.is_header[cur] {
                self.emit_loop(cur, ctx, depth);
                return;
            }

            match self.emit_block(cur, ctx, depth) {
                Some(next) => cur = next,
                None => return,
            }
        }
    }

    /// Emit a non-loop block; returns the next node to continue the sequence
    /// with, or None if this region is finished.
    fn emit_block(&mut self, i: usize, ctx: Ctx, depth: usize) -> Option<usize> {
        let term = self.blk(i).terminator;
        let (stmts, cond) = self.code[&self.nodes[i]].clone();
        for s in stmts {
            self.line(depth, &s);
        }

        match term {
            Flow::Return => {
                let r = return_reg(self.prog);
                self.line(depth, &format!("return {};", r));
                None
            }
            Flow::Indirect => {
                let t = self.blk(i).insns.last().unwrap().text.clone();
                self.line(depth, &format!("/* indirect: {} */", t));
                None
            }
            Flow::Interrupt => {
                let t = self.blk(i).insns.last().unwrap().text.clone();
                self.line(depth, &format!("__asm__(\"{}\"); /* trap */", t));
                None
            }
            Flow::Jump | Flow::Fallthrough | Flow::Call => {
                self.single_succ(i)
            }
            Flow::CondJump => {
                self.emit_if(i, cond.unwrap_or_else(|| "/*cond*/".into()), ctx, depth);
                None
            }
        }
    }

    /// Resolve the (single) successor of a non-branching block.
    fn single_succ(&self, i: usize) -> Option<usize> {
        let blk = self.blk(i);
        match blk.successors.first() {
            Some(t) => self.idx.get(t).copied(),
            None => None,
        }
    }

    /// Emit a structured `if`/`else` for a conditional block.
    fn emit_if(&mut self, i: usize, cond: String, ctx: Ctx, depth: usize) {
        let succ = self.blk(i).successors.clone();
        let taken = succ.first().and_then(|t| self.idx.get(t).copied());
        let fall = succ.get(1).and_then(|t| self.idx.get(t).copied());

        // Join = immediate post-dominator, if it is a real node.
        let join = {
            let j = self.ipdom[i];
            if j == UNDEF {
                ctx.stop
            } else {
                j
            }
        };
        let inner = Ctx {
            stop: join,
            ..ctx
        };

        self.line(depth, &format!("if ({}) {{", cond));
        match taken {
            Some(t) => self.emit_seq(t, inner, depth + 1),
            None => {
                let addr = succ.first().copied().unwrap_or(0);
                self.line(depth + 1, &format!("/* -> 0x{:x} (outside) */", addr));
            }
        }

        let has_else = match fall {
            Some(f) => f != join && f != ctx.stop,
            None => true,
        };
        if has_else {
            self.line(depth, "} else {");
            match fall {
                Some(f) => self.emit_seq(f, inner, depth + 1),
                None => {
                    let addr = succ.get(1).copied().unwrap_or(0);
                    self.line(depth + 1, &format!("/* -> 0x{:x} (outside) */", addr));
                }
            }
        }
        self.line(depth, "}");

        // Continue after the join.
        if join != UNDEF && join != ctx.stop {
            self.emit_seq(join, ctx, depth);
        }
    }

    /// Emit a natural loop as `while (true) { ... }` with break/continue.
    fn emit_loop(&mut self, header: usize, ctx: Ctx, depth: usize) {
        let (in_loop, follow) = self.loop_info(header);
        let follow_idx = follow.unwrap_or(UNDEF);
        let lctx = Ctx {
            stop: UNDEF,
            brk: follow_idx,
            cont: header,
        };

        self.line(depth, "while (true) {");

        // Emit the header body itself (already marked emitted by the caller).
        let term = self.blk(header).terminator;
        let (stmts, cond) = self.code[&self.nodes[header]].clone();
        let succ = self.blk(header).successors.clone();
        for s in stmts {
            self.line(depth + 1, &s);
        }

        match term {
            Flow::CondJump => {
                let taken = succ.first().and_then(|t| self.idx.get(t).copied());
                let fall = succ.get(1).and_then(|t| self.idx.get(t).copied());
                let cond = cond.unwrap_or_else(|| "/*cond*/".into());
                let taken_in = taken.map(|t| in_loop[t]).unwrap_or(false);
                let fall_in = fall.map(|f| in_loop[f]).unwrap_or(false);

                if taken_in && !fall_in {
                    // Stay in loop when cond true; exit otherwise.
                    self.line(depth + 1, &format!("if (!({})) break;", cond));
                    if let Some(t) = taken {
                        self.emit_seq(t, lctx, depth + 1);
                    }
                } else if fall_in && !taken_in {
                    self.line(depth + 1, &format!("if ({}) break;", cond));
                    if let Some(f) = fall {
                        self.emit_seq(f, lctx, depth + 1);
                    }
                } else {
                    // Both targets inside (or both outside): emit a plain if.
                    self.line(depth + 1, &format!("if ({}) {{", cond));
                    if let Some(t) = taken {
                        self.emit_seq(t, lctx, depth + 2);
                    }
                    self.line(depth + 1, "}");
                    if let Some(f) = fall {
                        self.emit_seq(f, lctx, depth + 1);
                    }
                }
            }
            _ => {
                // Header falls through into the body.
                if let Some(next) = self.single_succ(header) {
                    self.emit_seq(next, lctx, depth + 1);
                }
            }
        }

        self.line(depth, "}");

        // Continue after the loop.
        if follow_idx != UNDEF && follow_idx != ctx.stop {
            self.emit_seq(follow_idx, ctx, depth);
        }
    }
}
