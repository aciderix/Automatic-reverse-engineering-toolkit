//! Structured IR → C emission: recovers `if`/`else`/`while` over the IR CFG
//! instead of the flat goto form, using the same dominator/post-dominator +
//! natural-loop region-walking algorithm as the text `structure` module (here
//! adapted to IR statements). Unreducible edges degrade to `goto`, so the
//! output is always faithful. Runs after SSA destruction.

use super::{collect_callees, collect_values, destruct_ssa, expr_c, frame_decls, frame_name};
use crate::cfg::dom;
use crate::ir::types::*;
use std::collections::{BTreeSet, HashMap};
use std::fmt::Write;

const UNDEF: usize = usize::MAX;

fn label(id: u32) -> String {
    format!("L{}", id)
}

/// Render a non-control statement to a C line (control terminators return None).
fn body_line(s: &Stmt) -> Option<String> {
    match s {
        Stmt::Assign { dst, expr } => Some(format!("v{} = {};", dst.0, expr_c(expr))),
        Stmt::Store { addr, value, ty } => Some(format!(
            "{} = {};",
            super::lvalue_c(addr, super::int_bits(ty)),
            expr_c(value)
        )),
        // SEH-establish marker (see lift.rs): render the setjmp that lets
        // _except_handler3 transfer to this function's __except block. On the first pass
        // setjmp returns 0 and the __try body runs; a longjmp from _except_handler3 (after
        // it matched a filter and unwound) returns non-zero, carrying the matched scope
        // level + 1, so we run that __except handler funclet and return its value. The
        // frame comes from g_seh_frame (a C local holding it would be indeterminate after
        // longjmp); the level comes from the longjmp value (well-defined). Keyed by the
        // frame address (= esp here), matching _except_handler3's aret_longjmp_do(frame,…).
        Stmt::CallStmt(Expr::Call { target: CallTarget::Named(n), args, .. })
            if n == "__aret_seh_establish" && !args.is_empty() =>
        {
            // Runtime guard distinguishes an ESTABLISH from a RESTORE without dataflow:
            // an establish links the new frame's prev to the current head, so
            // `newframe->prev == fs:[0]` holds here (before the store); a restore pops to
            // an older frame, for which it does not. Only an establish arms a setjmp.
            // Exclude the chain terminator (0xffffffff) / null before dereferencing the
            // candidate frame: a RESTORE that pops fs:[0] back to the end-of-chain
            // sentinel would otherwise deref an invalid address.
            let f = expr_c(&args[0]);
            Some(format!(
                "if ((uint32_t)({f}) != 0xffffffffu && (uint32_t)({f}) != 0 && \
                 *(uint32_t*)(uintptr_t)({f}) == *(uint32_t*)(uintptr_t)__aret_fs()) \
                 {{ uint32_t _sj = aret_seh_setjmp({f}); if (_sj) return aret_seh_run(g_seh_frame, _sj - 1); }}"
            ))
        }
        Stmt::CallStmt(e) => Some(format!("(void)({});", expr_c(e))),
        // An instruction the lifter could not model. In the read-only decompile
        // it is a comment; in the *transpile* (shared-stack) path it is live code
        // that must not silently no-op — reaching it means we would guess at an
        // unknown effect, so fail loud with the instruction text instead.
        Stmt::Asm(t) => Some(if super::shared_stack() {
            format!("aret_unmodelled({:?});", t)
        } else {
            format!("/* asm: {} */", t)
        }),
        Stmt::Set { dst: Location::Frame(d), expr } => {
            Some(format!("{} = {};", frame_name(*d), expr_c(expr)))
        }
        Stmt::Set { .. } | Stmt::Nop => None,
        _ => None, // Branch/Jump/Return/Switch handled by the structurer
    }
}

fn is_control(s: &Stmt) -> bool {
    matches!(
        s,
        Stmt::Branch { .. } | Stmt::Jump(_) | Stmt::Return(_) | Stmt::Switch { .. }
    )
}

struct Structurer {
    nodes: Vec<u32>,
    idx: HashMap<u32, usize>,
    blocks: Vec<Block>,
    succ: Vec<Vec<usize>>,
    preds: Vec<Vec<usize>>,
    idom: Vec<usize>,
    ipdom: Vec<usize>,
    is_header: Vec<bool>,
    indeg: Vec<usize>,
    entry: usize,
    emitted: Vec<bool>,
    out: String,
}

#[derive(Clone, Copy)]
struct Ctx {
    stop: usize,
    brk: usize,
    cont: usize,
}
impl Ctx {
    fn root() -> Ctx {
        Ctx { stop: UNDEF, brk: UNDEF, cont: UNDEF }
    }
}

/// Emit a single function as structured C.
pub fn emit_function(func: &IrFunction, forward: &mut BTreeSet<u64>, with_params: bool) -> String {
    // Recover struct layouts from the SSA form (stable value ids) and make them
    // available to `lvalue_c` while this function's body is emitted.
    let struct_defs = {
        let si = crate::types::struct_info(func);
        let defs = si.defs();
        super::set_struct_info(Some(si));
        defs
    };

    let mut f = func.clone();
    destruct_ssa(&mut f);
    collect_callees(&f, forward);

    let mut values = BTreeSet::new();
    collect_values(&f, &mut values);
    if with_params {
        for p in &f.reg_params {
            values.remove(p); // declared as parameters, not locals
        }
    }

    let nodes: Vec<u32> = f.blocks.iter().map(|b| b.id).collect();
    let idx: HashMap<u32, usize> = nodes.iter().enumerate().map(|(i, &id)| (id, i)).collect();
    // Re-index blocks in `nodes` order.
    let blocks: Vec<Block> = nodes
        .iter()
        .map(|id| f.blocks.iter().find(|b| b.id == *id).unwrap().clone())
        .collect();
    let n = blocks.len();

    let mut succ = vec![Vec::new(); n];
    let mut preds = vec![Vec::new(); n];
    let mut indeg = vec![0usize; n];
    for (i, b) in blocks.iter().enumerate() {
        for &s in &b.succ {
            if let Some(&j) = idx.get(&s) {
                succ[i].push(j);
                preds[j].push(i);
                indeg[j] += 1;
            }
        }
    }
    let entry = blocks.iter().position(|b| b.addr == f.entry).unwrap_or(0);

    let mut s = Structurer {
        nodes,
        idx,
        blocks,
        succ,
        preds,
        idom: vec![UNDEF; n],
        ipdom: vec![UNDEF; n],
        is_header: vec![false; n],
        indeg,
        entry,
        emitted: vec![false; n],
        out: String::new(),
    };
    s.compute_doms();
    s.detect_loops();

    let mut out = String::new();
    // File-scope struct declarations (function-unique names), used by the body.
    out.push_str(&struct_defs);
    // Honesty marker: if any instruction could not be lifted (it survives as an
    // `asm` comment), the decompilation is incomplete and its result may be
    // wrong. Say so loudly rather than emitting silently-incorrect code.
    let unlifted = super::count_unlifted(&f);
    if unlifted > 0 {
        let _ = writeln!(
            out,
            "// WARNING: {} unmodelled instruction(s) — decompilation INCOMPLETE, result may be incorrect",
            unlifted
        );
    }
    let _ = writeln!(out, "{} {{", super::signature(&f, with_params));
    out.push_str(&super::value_decls(&values, &f.frame_base_values, &f.fp80_values));
    if let Some(fd) = frame_decls(&f, with_params) {
        let _ = writeln!(out, "{}", fd);
    }
    let entry = s.entry;
    s.emit_seq(entry, Ctx::root(), 1);
    // Emit any block not reached by structural traversal (e.g. switch cases,
    // which are reached only via the `goto`s the switch emits) so every label is
    // defined.
    for b in 0..s.blocks.len() {
        if !s.emitted[b] {
            s.emit_seq(b, Ctx::root(), 1);
        }
    }
    out.push_str(&s.out);
    let _ = writeln!(out, "    return 0;");
    let _ = writeln!(out, "}}");
    super::set_struct_info(None);
    out
}

impl Structurer {
    fn compute_doms(&mut self) {
        let n = self.blocks.len();
        self.idom = dom::dominators(n, self.entry, &self.succ, &self.preds);

        // Post-dominators: dominators on the reversed graph with a virtual exit.
        let virt = n;
        let total = n + 1;
        let mut rsucc = vec![Vec::new(); total];
        let mut rpred = vec![Vec::new(); total];
        for i in 0..n {
            for &p in &self.preds[i] {
                rsucc[i].push(p);
                rpred[p].push(i);
            }
        }
        for i in 0..n {
            let leaves = self.succ[i].is_empty()
                || matches!(self.blocks[i].stmts.last(), Some(Stmt::Return(_)) | Some(Stmt::Asm(_)));
            if leaves {
                rsucc[virt].push(i);
                rpred[i].push(virt);
            }
        }
        let ip = dom::dominators(total, virt, &rsucc, &rpred);
        self.ipdom = ip[..n].to_vec();
        for v in self.ipdom.iter_mut() {
            if *v == virt {
                *v = UNDEF;
            }
        }
    }

    fn dominates(&self, a: usize, b: usize) -> bool {
        dom::dominates(a, b, &self.idom)
    }

    fn detect_loops(&mut self) {
        let n = self.blocks.len();
        for u in 0..n {
            for &h in &self.succ[u].clone() {
                if self.dominates(h, u) {
                    self.is_header[h] = true;
                }
            }
        }
    }

    fn loop_info(&self, header: usize) -> (Vec<bool>, Option<usize>) {
        let n = self.blocks.len();
        let mut in_loop = vec![false; n];
        in_loop[header] = true;
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
        let mut follow = None;
        for i in 0..n {
            if !in_loop[i] {
                continue;
            }
            for &t in &self.succ[i] {
                if !in_loop[t] {
                    follow = Some(match follow {
                        Some(f) => std::cmp::min(f, t),
                        None => t,
                    });
                }
            }
        }
        (in_loop, follow)
    }

    fn line(&mut self, depth: usize, s: &str) {
        for _ in 0..depth {
            self.out.push_str("    ");
        }
        self.out.push_str(s);
        self.out.push('\n');
    }

    /// The control terminator of a block, and its (taken, fallthrough) successors.
    fn terminator(&self, i: usize) -> &Stmt {
        self.blocks[i].stmts.last().unwrap_or(&Stmt::Nop)
    }

    fn emit_body(&mut self, i: usize, depth: usize) {
        let stmts = self.blocks[i].stmts.clone();
        let body_len = if stmts.last().map(is_control).unwrap_or(false) {
            stmts.len() - 1
        } else {
            stmts.len()
        };
        for s in &stmts[..body_len] {
            if let Some(l) = body_line(s) {
                self.line(depth, &l);
            }
        }
    }

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
            // Label every block: any block may become a `goto` target in an
            // irreducible region, and an undefined label is a hard error.
            // (Unused labels are warnings, suppressed at compile time.)
            let lbl = label(self.nodes[cur]);
            self.line(depth.saturating_sub(1), &format!("{}:;", lbl));
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

    fn emit_block(&mut self, i: usize, ctx: Ctx, depth: usize) -> Option<usize> {
        self.emit_body(i, depth);
        match self.terminator(i).clone() {
            Stmt::Return(e) => {
                let v = e.map(|x| expr_c(&x)).unwrap_or_else(|| "0".into());
                self.line(depth, &format!("return {};", v));
                None
            }
            Stmt::Branch { cond, .. } => {
                self.emit_if(i, expr_c(&cond), ctx, depth);
                None
            }
            Stmt::Jump(_) => self.succ[i].first().copied(),
            Stmt::Switch { value, cases, .. } => {
                // Cases are this block's CFG successors in table order. The case
                // *key* comes from the IR: an index (0,1,2… for `jmp [tab+idx]`) or
                // a target VA (for an address-keyed computed goto, where we switch
                // on the loaded address). The out-of-range path was branched off
                // before the table, so the default is unreachable — route to case 0.
                self.line(depth, &format!("switch ({}) {{", expr_c(&value)));
                let succ = self.succ[i].clone();
                for (k, &t) in succ.iter().enumerate() {
                    let key = cases.get(k).map(|(v, _)| *v).unwrap_or(k as i128);
                    self.line(depth + 1, &format!("case {}: goto {};", key, label(self.nodes[t])));
                }
                if let Some(&t0) = succ.first() {
                    self.line(depth + 1, &format!("default: goto {};", label(self.nodes[t0])));
                }
                self.line(depth, "}");
                None
            }
            _ => None, // Asm leaf / no successor
        }
    }

    fn emit_if(&mut self, i: usize, cond: String, ctx: Ctx, depth: usize) {
        let taken = self.succ[i].first().copied();
        let fall = self.succ[i].get(1).copied();
        let join = {
            let j = self.ipdom[i];
            if j == UNDEF { ctx.stop } else { j }
        };
        let inner = Ctx { stop: join, ..ctx };

        self.line(depth, &format!("if ({}) {{", cond));
        if let Some(t) = taken {
            self.emit_seq(t, inner, depth + 1);
        }
        let has_else = match fall {
            Some(f) => f != join && f != ctx.stop,
            None => false,
        };
        if has_else {
            self.line(depth, "} else {");
            self.emit_seq(fall.unwrap(), inner, depth + 1);
        }
        self.line(depth, "}");
        if join != UNDEF && join != ctx.stop {
            self.emit_seq(join, ctx, depth);
        }
    }

    fn emit_loop(&mut self, header: usize, ctx: Ctx, depth: usize) {
        let (in_loop, follow) = self.loop_info(header);
        let follow_idx = follow.unwrap_or(UNDEF);
        let lctx = Ctx { stop: UNDEF, brk: follow_idx, cont: header };

        self.line(depth, "while (1) {");
        self.emit_body(header, depth + 1);
        match self.terminator(header).clone() {
            Stmt::Branch { cond, .. } => {
                let taken = self.succ[header].first().copied();
                let fall = self.succ[header].get(1).copied();
                let cs = expr_c(&cond);
                let taken_in = taken.map(|t| in_loop[t]).unwrap_or(false);
                let fall_in = fall.map(|f| in_loop[f]).unwrap_or(false);
                if taken_in && !fall_in {
                    // `fall` leaves the loop, `taken` stays in. Route the exit
                    // edge through emit_seq (in loop ctx): it emits `break` only
                    // when the target IS the follow block, otherwise it emits the
                    // real exit target (inline / `goto`). A bare `break` here would
                    // silently divert any non-follow exit to the follow block,
                    // dropping that path's effects (Bug #2: loop-carried result).
                    self.line(depth + 1, &format!("if (!({})) {{", cs));
                    match fall {
                        Some(fb) => self.emit_seq(fb, lctx, depth + 2),
                        None => self.line(depth + 2, "break;"),
                    }
                    self.line(depth + 1, "}");
                    if let Some(t) = taken {
                        self.emit_seq(t, lctx, depth + 1);
                    }
                } else if fall_in && !taken_in {
                    // `taken` leaves the loop, `fall` stays in (symmetric).
                    self.line(depth + 1, &format!("if ({}) {{", cs));
                    match taken {
                        Some(t) => self.emit_seq(t, lctx, depth + 2),
                        None => self.line(depth + 2, "break;"),
                    }
                    self.line(depth + 1, "}");
                    if let Some(fb) = fall {
                        self.emit_seq(fb, lctx, depth + 1);
                    }
                } else {
                    self.line(depth + 1, &format!("if ({}) {{", cs));
                    if let Some(t) = taken {
                        self.emit_seq(t, lctx, depth + 2);
                    }
                    self.line(depth + 1, "}");
                    if let Some(fb) = fall {
                        self.emit_seq(fb, lctx, depth + 1);
                    }
                }
            }
            Stmt::Jump(_) => {
                if let Some(next) = self.succ[header].first().copied() {
                    self.emit_seq(next, lctx, depth + 1);
                }
            }
            Stmt::Return(e) => {
                let v = e.map(|x| expr_c(&x)).unwrap_or_else(|| "0".into());
                self.line(depth + 1, &format!("return {};", v));
            }
            // A loop header that ends in a jump-table dispatch (the canonical
            // interpreter loop: `while(1){ fetch; switch(op){...} }`). Without this
            // the switch falls through `_ =>` and the loop body is empty — an
            // infinite `while(1){}`. Emit the dispatch, then the case-target blocks
            // *inside* the loop so their `goto header` edges fold to `continue`.
            Stmt::Switch { value, cases, .. } => {
                self.line(depth + 1, &format!("switch ({}) {{", expr_c(&value)));
                let succ = self.succ[header].clone();
                for (k, &t) in succ.iter().enumerate() {
                    let key = cases.get(k).map(|(v, _)| *v).unwrap_or(k as i128);
                    self.line(depth + 2, &format!("case {}: goto {};", key, label(self.nodes[t])));
                }
                if let Some(&t0) = succ.first() {
                    self.line(depth + 2, &format!("default: goto {};", label(self.nodes[t0])));
                }
                self.line(depth + 1, "}");
                let mut seen = std::collections::BTreeSet::new();
                for &t in &succ {
                    if seen.insert(t) {
                        self.emit_seq(t, lctx, depth + 1);
                    }
                }
            }
            _ => {}
        }
        self.line(depth, "}");
        if follow_idx != UNDEF && follow_idx != ctx.stop {
            self.emit_seq(follow_idx, ctx, depth);
        }
    }
}

/// Emit a *modular* translation unit: a shared declarations header plus one body
/// string per chunk of `chunk_size` functions. A real program is millions of
/// lines of C — passing it to the compiler as one translation unit chokes it, so
/// the builder writes the header to `aret_decls.h`, each returned chunk to its own
/// `.c` (which `#include`s the header), and compiles the chunks in parallel.
///
/// The header carries `<stdint.h>`, the HLE declarations, the (static-inline)
/// float helpers, and an empty-parameter forward declaration for every function,
/// so any chunk can call any other function. (Empty `sub_x();` is compatible both
/// with calls and with the parameterised definitions emitted in the chunks.)
/// Returns `(shared header, chunk bodies, referenced-but-undefined function
/// addresses)`. The last are call targets ARET did not recover (e.g. inside a
/// packer section, or indirect targets); the builder emits weak stubs for them so
/// the program still links.
pub fn emit_split(funcs: &[IrFunction], chunk_size: usize) -> (String, Vec<String>, Vec<u64>) {
    // The cdecl arity fixup (non-shared only) needs an owned, mutable copy. In
    // shared-stack mode we skip it, so borrow directly — cloning tens of
    // thousands of IR functions for a real binary is a needless memory spike.
    let mut owned: Vec<IrFunction>;
    let funcs: &[IrFunction] = if super::shared_stack() {
        funcs
    } else {
        owned = funcs.to_vec();
        super::fixup_call_arity(&mut owned);
        &owned
    };
    let with_params = true;
    let defined: BTreeSet<u64> = funcs.iter().map(|f| f.entry).collect();
    let mut forward: BTreeSet<u64> = BTreeSet::new();

    let mut chunks: Vec<String> = Vec::new();
    let mut cur = String::new();
    let mut n = 0usize;
    let mut needs_float = false;
    for f in funcs {
        let body = emit_function(f, &mut forward, with_params);
        // The float helpers use `__int128`, unavailable under -m32, so only pull
        // them in when a function actually uses floating point (as `emit_unit`).
        if !super::float_preamble(&body).is_empty() {
            needs_float = true;
        }
        cur.push_str(&body);
        cur.push('\n');
        n += 1;
        if n >= chunk_size.max(1) {
            chunks.push(std::mem::take(&mut cur));
            n = 0;
        }
    }
    if !cur.is_empty() {
        chunks.push(cur);
    }

    let mut header = String::new();
    header.push_str("#include <stdint.h>\n");
    header.push_str("#include \"aret_hle.h\"\n");
    if needs_float {
        // Static-inline definitions: each chunk gets its own copy, no link clash.
        header.push_str(super::FLOAT_HELPERS);
    }
    header.push('\n');
    // Forward-declare every function. In shared-stack mode (the transpile path)
    // each `sub_` has the uniform 5-arg signature (`__esp`, eax, ecx, edx, ebp),
    // so emit it in full: an empty `()` is K&R "unspecified args" which x86
    // tolerates but WASM's strictly typed `call_indirect` does not (a mismatch
    // traps via a bitcast trampoline).
    let sig = if super::shared_stack() {
        "(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t)"
    } else {
        "()"
    };
    for a in forward.union(&defined) {
        let _ = writeln!(header, "uint64_t sub_{:x}{};", a, sig);
    }
    let undefined: Vec<u64> = forward.difference(&defined).copied().collect();
    (header, chunks, undefined)
}

/// Emit a complete structured C translation unit.
pub fn emit_unit(funcs: &[IrFunction]) -> String {
    // Always emit real parameter signatures; the interprocedural fixup makes
    // every call to a function defined here match its callee's arity so the unit
    // recompiles (call-site arguments — roadmap §15.4 #2).
    let mut funcs = funcs.to_vec();
    // In shared-stack mode every function takes exactly one parameter (`__esp`)
    // and every internal call passes exactly one argument (the stack pointer), so
    // the cdecl arity fixup must not run (it would truncate that argument).
    if !super::shared_stack() {
        super::fixup_call_arity(&mut funcs);
    }
    let with_params = true;
    let mut body = String::new();
    let mut forward: BTreeSet<u64> = BTreeSet::new();
    let defined: BTreeSet<u64> = funcs.iter().map(|f| f.entry).collect();
    for f in &funcs {
        body.push_str(&emit_function(f, &mut forward, with_params));
        body.push('\n');
    }
    let mut out = String::new();
    out.push_str("#include <stdint.h>\n\n");
    out.push_str(super::float_preamble(&body));
    // Forward-declare every function (defined or external) with an empty
    // parameter list so calls that precede a definition still see a compatible
    // prototype.
    for a in forward.union(&defined) {
        let _ = writeln!(out, "uint64_t sub_{:x}();", a);
    }
    out.push('\n');
    out.push_str(&body);
    out
}
