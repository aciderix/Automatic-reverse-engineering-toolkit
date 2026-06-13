//! Conservative dataflow cleanup over a function's basic blocks:
//! global register **liveness**, **dead-assignment elimination**, and
//! **single-use expression propagation**.
//!
//! Everything here is gated on correctness: a register def is only removed when
//! it is provably unused (not read later in the block and not live on any path
//! out of the block), and an expression is only inlined into a use when that
//! use is the *only* consumer and nothing between the def and the use can
//! change the result. Anything uncertain is left exactly as lifted.

use crate::analysis::Function;
use crate::decompile::block_statements;
use crate::loader::Program;
use std::collections::{HashMap, HashSet};

/// Canonical register family (so `al`/`ax`/`eax`/`rax` alias correctly).
fn reg_family(tok: &str) -> Option<&'static str> {
    Some(match tok {
        "al" | "ah" | "ax" | "eax" | "rax" => "rax",
        "bl" | "bh" | "bx" | "ebx" | "rbx" => "rbx",
        "cl" | "ch" | "cx" | "ecx" | "rcx" => "rcx",
        "dl" | "dh" | "dx" | "edx" | "rdx" => "rdx",
        "sil" | "si" | "esi" | "rsi" => "rsi",
        "dil" | "di" | "edi" | "rdi" => "rdi",
        "bpl" | "bp" | "ebp" | "rbp" => "rbp",
        "spl" | "sp" | "esp" | "rsp" => "rsp",
        "r8b" | "r8w" | "r8d" | "r8" => "r8",
        "r9b" | "r9w" | "r9d" | "r9" => "r9",
        "r10b" | "r10w" | "r10d" | "r10" => "r10",
        "r11b" | "r11w" | "r11d" | "r11" => "r11",
        "r12b" | "r12w" | "r12d" | "r12" => "r12",
        "r13b" | "r13w" | "r13d" | "r13" => "r13",
        "r14b" | "r14w" | "r14d" | "r14" => "r14",
        "r15b" | "r15w" | "r15d" | "r15" => "r15",
        _ => {
            if tok.starts_with("xmm") && tok[3..].bytes().all(|c| c.is_ascii_digit()) {
                // Leak a stable family string per xmm register.
                return XMM.iter().find(|x| **x == tok).copied();
            }
            return None;
        }
    })
}

const XMM: [&str; 16] = [
    "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10",
    "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
];

/// Registers a call clobbers that we treat as *killed* for liveness. Kept
/// minimal (just the return register) — under-killing is always safe, while
/// over-killing could wrongly mark a live value dead.
const CALL_KILLS: [&str; 1] = ["rax"];

/// Registers a call may *read* as arguments (System V + Win64 supersets). We
/// can't tell which are real arguments, so conservatively treat all as read so
/// their setup is never eliminated. (For 32-bit cdecl, stack `push`es already
/// cover this; these extra reads are harmless there.)
const ARG_REGS: [&str; 6] = ["rdi", "rsi", "rdx", "rcx", "r8", "r9"];

/// Every register family we track (used as the conservative "everything live").
fn all_families() -> HashSet<&'static str> {
    [
        "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r8", "r9", "r10", "r11", "r12",
        "r13", "r14", "r15",
    ]
    .into_iter()
    .chain(XMM)
    .collect()
}

/// Scan a string for register tokens, returning their families (deduped).
fn read_families(s: &str) -> Vec<&'static str> {
    let mut out = Vec::new();
    for ident in identifiers(s) {
        if let Some(f) = reg_family(ident) {
            if !out.contains(&f) {
                out.push(f);
            }
        }
    }
    out
}

/// Yield maximal `[a-z0-9_]` identifier substrings (our lifted text is lowercase).
fn identifiers(s: &str) -> Vec<&str> {
    let bytes = s.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i < bytes.len() {
        let c = bytes[i];
        let is_word = c.is_ascii_alphanumeric() || c == b'_';
        if is_word && (bytes[i].is_ascii_alphabetic() || bytes[i] == b'_') {
            let start = i;
            while i < bytes.len() && (bytes[i].is_ascii_alphanumeric() || bytes[i] == b'_') {
                i += 1;
            }
            out.push(&s[start..i]);
        } else if c.is_ascii_alphanumeric() || c == b'_' {
            // starts with a digit (a number) — skip the whole run
            while i < bytes.len() && (bytes[i].is_ascii_alphanumeric() || bytes[i] == b'_') {
                i += 1;
            }
        } else {
            i += 1;
        }
    }
    out
}

#[derive(Debug, Clone, Copy, PartialEq)]
enum Kind {
    /// `reg = rhs;` — pure full-register write (candidate for DCE/propagation).
    Assign,
    /// `reg OP= rhs;`, `reg++`/`--` — reads and writes the same register.
    Compound,
    /// Anything with side effects we keep: stores, calls, asm, pushes, comments.
    Keep,
    /// `__asm__` — opaque; conservatively reads everything.
    Opaque,
}

#[derive(Debug, Clone)]
struct Stmt {
    text: String,
    kind: Kind,
    /// Exact write register name and family, for Assign/Compound.
    write: Option<(String, &'static str)>,
    /// Register families read.
    reads: Vec<&'static str>,
    /// For Assign: the RHS expression (for propagation).
    rhs: Option<String>,
    /// RHS dereferences memory (load) — limits propagation across stores/calls.
    rhs_loads_mem: bool,
    /// This statement defines the caller-saved set (a call).
    is_call: bool,
}

fn parse_stmt(text: &str) -> Stmt {
    let t = text.trim();
    let keep = |reads: Vec<&'static str>, is_call: bool, opaque: bool| Stmt {
        text: text.to_string(),
        kind: if opaque { Kind::Opaque } else { Kind::Keep },
        write: None,
        reads,
        rhs: None,
        rhs_loads_mem: false,
        is_call,
    };

    if t.starts_with("//") {
        return keep(Vec::new(), false, false);
    }
    if t.starts_with("__asm__") {
        return keep(read_families(t), false, true);
    }
    // Store: `*(...) = val;` — memory write, keep.
    if t.starts_with("*(") {
        return keep(read_families(t), false, false);
    }
    // Strip trailing ';'
    let body = t.strip_suffix(';').unwrap_or(t);

    // inc / dec
    if let Some(name) = body.strip_suffix("++").or_else(|| body.strip_suffix("--")) {
        if let Some(fam) = reg_family(name.trim()) {
            return Stmt {
                text: text.to_string(),
                kind: Kind::Compound,
                write: Some((name.trim().to_string(), fam)),
                reads: vec![fam],
                rhs: None,
                rhs_loads_mem: false,
                is_call: false,
            };
        }
    }

    // Compound assigns
    for op in ["+=", "-=", "*=", "&=", "|=", "^=", "<<=", ">>="] {
        if let Some(pos) = body.find(op) {
            let lhs = body[..pos].trim();
            let rhs = body[pos + op.len()..].trim();
            if let Some(fam) = reg_family(lhs) {
                let mut reads = vec![fam];
                for r in read_families(rhs) {
                    if !reads.contains(&r) {
                        reads.push(r);
                    }
                }
                return Stmt {
                    text: text.to_string(),
                    kind: Kind::Compound,
                    write: Some((lhs.to_string(), fam)),
                    reads,
                    rhs: None,
                    rhs_loads_mem: false,
                    is_call: false,
                };
            }
            return keep(read_families(body), false, false);
        }
    }

    // Plain assign `lhs = rhs`
    if let Some(pos) = body.find(" = ") {
        let lhs = body[..pos].trim();
        let rhs = body[pos + 3..].trim();
        // A call/pop on the RHS has side effects — keep it.
        let impure = rhs.contains("pop(") || is_call_expr(rhs);
        if let Some(fam) = reg_family(lhs) {
            if impure {
                // e.g. `eax = pop();` — write known, but don't DCE/propagate.
                return Stmt {
                    text: text.to_string(),
                    kind: Kind::Keep,
                    write: Some((lhs.to_string(), fam)),
                    reads: read_families(rhs),
                    rhs: None,
                    rhs_loads_mem: false,
                    is_call: false,
                };
            }
            return Stmt {
                text: text.to_string(),
                kind: Kind::Assign,
                write: Some((lhs.to_string(), fam)),
                reads: read_families(rhs),
                rhs: Some(rhs.to_string()),
                rhs_loads_mem: rhs.contains("*("),
                is_call: false,
            };
        }
        // lhs is memory/local/arg — keep as a side-effecting write.
        return keep(read_families(body), false, false);
    }

    // Call statement `name(args);`
    if is_call_expr(body) {
        // Reads = families in the textual args, plus all potential argument
        // registers (we can't tell which are real args, so keep their setup).
        let mut reads = read_families(body);
        for r in ARG_REGS {
            if !reads.contains(&r) {
                reads.push(r);
            }
        }
        return keep(reads, true, false);
    }

    // Anything else (push, leave-comment already handled): keep, read all named regs.
    keep(read_families(body), false, false)
}

/// Heuristic: does this expression look like a function call (`name(...)`)
/// rather than a cast/deref like `*(uint32_t*)(...)`?
fn is_call_expr(s: &str) -> bool {
    let s = s.trim();
    // find first '(' preceded by an identifier char that isn't a cast
    if let Some(pos) = s.find('(') {
        if pos == 0 {
            return false;
        }
        let prev = s.as_bytes()[pos - 1];
        return prev.is_ascii_alphanumeric() || prev == b'_';
    }
    false
}

/// Replace whole-word occurrences of `name` with `repl` in `s`.
fn replace_word(s: &str, name: &str, repl: &str) -> String {
    let bytes = s.as_bytes();
    let mut out = String::with_capacity(s.len());
    let mut i = 0;
    while i < bytes.len() {
        if s[i..].starts_with(name) {
            let before_ok = i == 0 || !is_word_byte(bytes[i - 1]);
            let after = i + name.len();
            let after_ok = after >= bytes.len() || !is_word_byte(bytes[after]);
            if before_ok && after_ok {
                out.push_str(repl);
                i = after;
                continue;
            }
        }
        out.push(bytes[i] as char);
        i += 1;
    }
    out
}

fn is_word_byte(b: u8) -> bool {
    b.is_ascii_alphanumeric() || b == b'_'
}

fn count_word(s: &str, name: &str) -> usize {
    let mut n = 0;
    let bytes = s.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if s[i..].starts_with(name) {
            let before_ok = i == 0 || !is_word_byte(bytes[i - 1]);
            let after = i + name.len();
            let after_ok = after >= bytes.len() || !is_word_byte(bytes[after]);
            if before_ok && after_ok {
                n += 1;
                i = after;
                continue;
            }
        }
        i += 1;
    }
    n
}

/// Optimised code for the whole function: block-start -> (statements, condition).
pub type FunctionCode = HashMap<u64, (Vec<String>, Option<String>)>;

/// Run liveness, DCE and propagation; return cleaned statements per block.
pub fn optimize_function(prog: &Program, func: &Function) -> FunctionCode {
    let nodes: Vec<u64> = func.blocks.keys().copied().collect();
    let idx: HashMap<u64, usize> = nodes.iter().enumerate().map(|(i, &a)| (a, i)).collect();
    let n = nodes.len();

    // Raw lifted code + parsed statements + per-block extra end-uses.
    let mut conds: Vec<Option<String>> = Vec::with_capacity(n);
    let mut stmts: Vec<Vec<Stmt>> = Vec::with_capacity(n);
    let mut end_uses: Vec<HashSet<&'static str>> = Vec::with_capacity(n);
    let mut keep_all: Vec<bool> = vec![false; n];
    let mut succ: Vec<Vec<usize>> = vec![Vec::new(); n];

    for (i, &a) in nodes.iter().enumerate() {
        let blk = &func.blocks[&a];
        let (raw, cond) = block_statements(prog, blk);
        let parsed: Vec<Stmt> = raw.iter().map(|s| parse_stmt(s)).collect();

        // End-of-block register uses.
        let mut eu: HashSet<&'static str> = HashSet::new();
        match blk.terminator {
            crate::disasm::Flow::CondJump => {
                if let Some(c) = &cond {
                    eu.extend(read_families(c));
                }
            }
            crate::disasm::Flow::Return => {
                eu.insert("rax");
            }
            crate::disasm::Flow::Indirect | crate::disasm::Flow::Interrupt => {
                keep_all[i] = true; // unknown reads — keep everything live
            }
            _ => {}
        }
        // Opaque statements also force keep-all for the block.
        if parsed.iter().any(|s| s.kind == Kind::Opaque) {
            keep_all[i] = true;
        }
        end_uses.push(eu);
        conds.push(cond);
        stmts.push(parsed);

        for &t in &blk.successors {
            if let Some(&j) = idx.get(&t) {
                succ[i].push(j);
            }
        }
    }

    // Per-block use/def (gen/kill) for liveness.
    let all = all_families();
    let mut use_b: Vec<HashSet<&'static str>> = vec![HashSet::new(); n];
    let mut def_b: Vec<HashSet<&'static str>> = vec![HashSet::new(); n];
    for i in 0..n {
        if keep_all[i] {
            use_b[i] = all.clone();
            continue;
        }
        let mut used = HashSet::new();
        let mut defined: HashSet<&'static str> = HashSet::new();
        let mut consider = |reads: &[&'static str], defs: &[&'static str]| {
            for &r in reads {
                if !defined.contains(r) {
                    used.insert(r);
                }
            }
            for &d in defs {
                defined.insert(d);
            }
        };
        for s in &stmts[i] {
            let defs: Vec<&'static str> = if s.is_call {
                CALL_KILLS.to_vec()
            } else {
                s.write.iter().map(|(_, f)| *f).collect()
            };
            consider(&s.reads, &defs);
        }
        // End uses happen after the statements.
        let eu: Vec<&'static str> = end_uses[i].iter().copied().collect();
        consider(&eu, &[]);
        use_b[i] = used;
        def_b[i] = defined;
    }

    // Backward liveness fixpoint.
    let mut live_in: Vec<HashSet<&'static str>> = vec![HashSet::new(); n];
    let mut live_out: Vec<HashSet<&'static str>> = vec![HashSet::new(); n];
    let mut changed = true;
    while changed {
        changed = false;
        for i in (0..n).rev() {
            let mut out: HashSet<&'static str> = HashSet::new();
            for &s in &succ[i] {
                out.extend(live_in[s].iter().copied());
            }
            // live_in = use ∪ (out − def)
            let mut lin = use_b[i].clone();
            for &r in &out {
                if !def_b[i].contains(r) {
                    lin.insert(r);
                }
            }
            if out != live_out[i] || lin != live_in[i] {
                live_out[i] = out;
                live_in[i] = lin;
                changed = true;
            }
        }
    }

    // Per-block DCE then propagation.
    let retreg = match prog.bitness {
        crate::loader::Bitness::Bits64 => "rax",
        crate::loader::Bitness::Bits32 => "eax",
    };
    let mut code: FunctionCode = HashMap::new();
    for i in 0..n {
        let mut block = stmts[i].clone();
        let lout = {
            let mut s = live_out[i].clone();
            s.extend(end_uses[i].iter().copied());
            s
        };
        if !keep_all[i] {
            // Iterate DCE + propagation to a fixpoint (capped).
            for _ in 0..4 {
                let a = dce(&mut block, &lout);
                let b = propagate(&mut block, &lout);
                if !a && !b {
                    break;
                }
            }
        }
        let mut lines: Vec<String> = block.into_iter().map(|s| s.text).collect();
        bind_call_results(&mut lines, &lout, retreg);
        code.insert(nodes[i], (lines, conds[i].clone()));
    }
    code
}

/// Bind a call's return value to the result register when it is used: rewrite
/// `f(args);` to `eax = f(args);` if `rax`/`eax` is live after the call. A call
/// always sets the result register, so this is semantically exact; the liveness
/// check only avoids noise on calls whose result is discarded.
fn bind_call_results(lines: &mut [String], live_out: &HashSet<&'static str>, retreg: &str) {
    let mut live = live_out.contains("rax");
    for line in lines.iter_mut().rev() {
        let reads_rax = read_families(line).contains(&"rax");
        if is_void_call_line(line) {
            if live {
                let indent: String = line.chars().take_while(|c| *c == ' ').collect();
                *line = format!("{}{} = {}", indent, retreg, line.trim_start());
            }
            // The call defines rax; upstream liveness is just its argument reads.
            live = reads_rax;
        } else {
            let writes_rax = line_writes_rax(line);
            live = reads_rax || (live && !writes_rax);
        }
    }
}

/// A bare `name(args);` call statement (not a cast, store, control keyword,
/// assignment, push, or asm).
fn is_void_call_line(line: &str) -> bool {
    let t = line.trim();
    if !t.ends_with(");") {
        return false;
    }
    let p = match t.find('(') {
        Some(p) if p > 0 => p,
        _ => return false,
    };
    let name = &t[..p];
    if !name.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_') {
        return false; // contains space/'='/'*' etc. → assignment, store, indirect
    }
    !matches!(
        name,
        "if" | "while" | "for" | "switch" | "return" | "sizeof" | "push" | "__asm__"
    )
}

/// Does a line assign to the `rax` family (`eax = ...` / `rax = ...`)?
fn line_writes_rax(line: &str) -> bool {
    let t = line.trim();
    if let Some(pos) = t.find(" = ") {
        let lhs = &t[..pos];
        return reg_family(lhs.trim()) == Some("rax");
    }
    false
}

/// Remove dead pure register assignments. Returns true if anything changed.
fn dce(block: &mut Vec<Stmt>, live_out: &HashSet<&'static str>) -> bool {
    let mut live = live_out.clone();
    let mut remove = vec![false; block.len()];
    for i in (0..block.len()).rev() {
        let s = &block[i];
        match s.kind {
            Kind::Assign | Kind::Compound => {
                let fam = s.write.as_ref().map(|(_, f)| *f).unwrap_or("");
                if fam.is_empty() {
                    for &r in &s.reads {
                        live.insert(r);
                    }
                    continue;
                }
                if !live.contains(fam) {
                    remove[i] = true; // dead: result never used
                } else {
                    if s.kind == Kind::Assign {
                        live.remove(fam); // fully overwritten here
                    }
                    for &r in &s.reads {
                        live.insert(r);
                    }
                }
            }
            Kind::Keep => {
                if s.is_call {
                    for f in CALL_KILLS {
                        live.remove(f);
                    }
                }
                for &r in &s.reads {
                    live.insert(r);
                }
            }
            Kind::Opaque => {
                for &r in &s.reads {
                    live.insert(r);
                }
            }
        }
    }
    let changed = remove.iter().any(|&r| r);
    if changed {
        let mut idx = 0;
        block.retain(|_| {
            let keep = !remove[idx];
            idx += 1;
            keep
        });
    }
    changed
}

/// Inline a single-use, locally-dead register definition into its one use.
///
/// Safe when: the defined register is not live out of the block, it has exactly
/// one use before its value ends (redefinition / clobber), and nothing between
/// the def and that use can change the inlined expression (no write to a source
/// register, and — for a memory-loading expression — no intervening store/call).
fn propagate(block: &mut Vec<Stmt>, live_out: &HashSet<&'static str>) -> bool {
    for i in 0..block.len() {
        let (name, fam, rhs, loads_mem) = match &block[i] {
            Stmt {
                kind: Kind::Assign,
                write: Some((name, fam)),
                rhs: Some(rhs),
                rhs_loads_mem,
                ..
            } => (name.clone(), *fam, rhs.clone(), *rhs_loads_mem),
            _ => continue,
        };
        if live_out.contains(fam) {
            continue; // value escapes the block
        }
        let sources = block[i].reads.clone();

        // Scan forward until the register's value ends, locating uses.
        let mut use_at: Option<usize> = None;
        let mut total_uses = 0;
        for j in (i + 1)..block.len() {
            let s = &block[j];
            let here = count_word(&s.text, &name);
            if here > 0 {
                total_uses += here;
                if use_at.is_none() {
                    use_at = Some(j);
                }
            }
            // Value ends when its family is rewritten or a call clobbers it.
            let redefines_fam = s.write.as_ref().map(|(_, wf)| *wf == fam).unwrap_or(false);
            if redefines_fam || (s.is_call && CALL_KILLS.contains(&fam)) {
                break;
            }
        }

        if total_uses != 1 {
            continue;
        }
        let p = use_at.unwrap();

        // Check for hazards strictly between the def and the use.
        let mut hazard = false;
        for s in &block[i + 1..p] {
            if s.is_call || s.kind == Kind::Opaque {
                hazard = true;
                break;
            }
            if let Some((_, wf)) = &s.write {
                if sources.contains(wf) {
                    hazard = true;
                    break;
                }
            }
            if loads_mem && s.text.trim_start().starts_with("*(") {
                hazard = true; // store may alias the loaded address
                break;
            }
        }
        if hazard {
            continue;
        }

        let repl = if needs_parens(&rhs) {
            format!("({})", rhs)
        } else {
            rhs.clone()
        };
        let new_text = replace_word(&block[p].text, &name, &repl);
        block[p] = parse_stmt(&new_text);
        block.remove(i);
        return true;
    }
    false
}

/// Whether an expression needs wrapping in parens when inlined.
fn needs_parens(rhs: &str) -> bool {
    let t = rhs.trim();
    // Single token (identifier, number, or a single deref) needs no parens.
    if t.starts_with("*(") && t.ends_with(')') {
        return false;
    }
    !t.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_')
}
