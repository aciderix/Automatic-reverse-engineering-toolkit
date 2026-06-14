//! Frame-region analysis (roadmap §4.1) — the offset-aware layer that lets
//! stack-slot promotion fire on real, heap-using functions.
//!
//! ## The problem it solves
//!
//! The coarse alias verdict (`opt::alias::frame_promotable`) disqualifies any
//! function with an unknown-pointer access — which is *every* function that does
//! heap work, i.e. essentially all of them. That is correct but useless: a heap
//! pointer cannot actually alias a stack slot whose address never escaped, and a
//! function's `rbp`-relative locals live in a different part of the frame than
//! its `rsp`-relative spills / outgoing arguments.
//!
//! This module reasons about *offsets within the frame* so a non-aliased local
//! can be promoted even when the function touches the heap and the stack:
//!
//! 1. **Taint** (flow-insensitive): which registers hold a value derived from
//!    `rsp`/`rbp`. A generic access through a *non*-tainted base is a heap access
//!    — provably disjoint from a non-escaped frame, so it is ignored.
//! 2. **`rsp` offset** (flow-sensitive): the byte offset of `rsp` relative to
//!    function entry, tracked through `push`/`pop`/`sub rsp`/`mov rsp,rbp`. This
//!    places `rbp` (`mov rbp, rsp`) and every `rsp`/`rbp`-relative access on one
//!    entry-relative axis.
//! 3. **Overlap**: a named local `[rbp-d]` is promotable iff its byte range
//!    overlaps no *generic* frame access (spill, outgoing arg, saved register).
//!
//! ## Soundness
//!
//! Conservative throughout: anything the model cannot follow (an `rsp` write it
//! does not recognise, a conflicting `rsp` at a CFG join, a variable-indexed
//! frame access, a frame pointer escaping via copy/store/call/`lea`) makes the
//! whole analysis yield *no* promotable slots. A reported slot is guaranteed
//! free of any aliasing frame access.
#![allow(dead_code)]

use crate::ir::types::*;
use std::collections::{BTreeSet, HashSet};

const RSP: u16 = 4;
const RBP: u16 = 5;

/// Outcome of the frame analysis.
struct FrameInfo {
    /// Whether the `rsp`/`rbp` model held everywhere (else: promote nothing).
    ok: bool,
    /// `rbp`'s byte offset relative to entry `rsp` (set at `mov rbp, rsp`).
    rbp_off: Option<i64>,
    /// Entry-relative `[start, end)` byte ranges of every *generic* frame access
    /// (rsp/rbp-relative loads/stores, push/pop saves, spills, outgoing args).
    generic_ranges: Vec<(i64, i64)>,
    /// Every constant-offset frame access as `(entry-relative offset, width
    /// bytes)`. Raw material for recovering `rsp`-relative locals as named slots.
    accesses: Vec<(i64, i64)>,
    /// Per-block entry `rsp` byte offset relative to function entry (`None` =
    /// block not reached). Lets the rewrite recompute each access's offset.
    entry_rsp: Vec<Option<i64>>,
    /// A frame pointer escaped (taken by `lea`, copied into a base, stored to or
    /// passed beyond the frame): no local can be assumed un-aliased.
    escaped: bool,
}

impl FrameInfo {
    fn bail() -> Self {
        FrameInfo {
            ok: false,
            rbp_off: None,
            generic_ranges: Vec::new(),
            accesses: Vec::new(),
            entry_rsp: Vec::new(),
            escaped: true,
        }
    }
}

/// A recovered stack slot's usage summary.
#[derive(Default, Clone, Debug)]
pub struct SlotInfo {
    /// Access widths in bits observed at this slot.
    pub widths: BTreeSet<u32>,
    /// Number of accesses (loads + stores).
    pub count: u32,
}

/// The set of local slot displacements (`Location::Frame(d)`, `d < 0`) that are
/// provably free of any aliasing frame access — safe to promote. Empty if the
/// analysis bailed or anything escaped.
pub fn promotable_slots(func: &IrFunction) -> BTreeSet<i64> {
    let info = analyze(func);
    let empty = BTreeSet::new();
    if !info.ok || info.escaped {
        return empty;
    }
    let Some(rbp_off) = info.rbp_off else { return empty };

    // Named local slots (negative displacement). Width is not carried on a Frame
    // location, so assume a full 8-byte slot — conservative for overlap.
    let mut slots = BTreeSet::new();
    collect_local_slots(func, &mut slots);

    slots
        .into_iter()
        .filter(|&disp| {
            let s = rbp_off + disp;
            let e = s + 8;
            !info.generic_ranges.iter().any(|&(gs, ge)| gs < e && s < ge)
        })
        .collect()
}

/// Recover `rsp`/`rbp`-relative constant-offset accesses as named stack slots,
/// keyed by displacement in the function's frame coordinate (rbp-relative when a
/// frame pointer is established, else entry-`rsp`-relative). These are exactly
/// the accesses currently emitted as `*(T*)(reg + k)`; naming them turns them
/// into `local_…`. Returns `None` if the frame model bailed or anything escaped
/// (then no naming is sound). Display/measurement layer for now.
pub fn recover_stack_slots(func: &IrFunction) -> Option<std::collections::BTreeMap<i64, SlotInfo>> {
    use std::collections::BTreeMap;
    let info = analyze(func);
    if !info.ok || info.escaped {
        return None;
    }
    // Convert entry-relative offsets to the function's frame coordinate.
    let to_disp = |o: i64| match info.rbp_off {
        Some(r) => o - r,
        None => o,
    };
    let mut m: BTreeMap<i64, SlotInfo> = BTreeMap::new();
    for (o, w) in &info.accesses {
        let e = m.entry(to_disp(*o)).or_default();
        e.widths.insert((*w * 8) as u32);
        e.count += 1;
    }
    Some(m)
}

/// Rewrite the *safe* recovered `rsp`/`rbp`-relative accesses into named
/// `Location::Frame` slots, so they emit as `local_…` instead of
/// `*(T*)(reg + k)`. This is also a **correctness fix**: a stack access through
/// the (uninitialised, in a standalone recompile) frame register otherwise
/// dereferences a wild pointer; a real C local does not.
///
/// Runs on the pre-SSA IR. A slot is rewritten only when the whole-function
/// frame model held (`ok`, nothing escaped) and the slot is *unambiguous*:
///   * a single access width (the `Frame` model carries no width), and
///   * its byte range overlaps no other recovered slot.
/// Everything else is left as memory — sound by omission. Returns the number of
/// distinct slots promoted.
pub fn promote_stack_slots(func: &mut IrFunction) -> usize {
    use std::collections::{BTreeMap, HashMap};
    let info = analyze(func);
    if !info.ok || info.escaped {
        return 0;
    }
    let rbp_off = info.rbp_off;
    let to_disp = |o: i64| match rbp_off {
        Some(r) => o - r,
        None => o,
    };

    // Per displacement: the set of widths and the spanned entry-relative range.
    let mut widths: BTreeMap<i64, std::collections::BTreeSet<i64>> = BTreeMap::new();
    let mut range: BTreeMap<i64, (i64, i64)> = BTreeMap::new();
    for (o, w) in &info.accesses {
        let d = to_disp(*o);
        widths.entry(d).or_default().insert(*w);
        let r = range.entry(d).or_insert((*o, *o + *w));
        r.0 = r.0.min(*o);
        r.1 = r.1.max(*o + *w);
    }

    // Safe = single width AND range disjoint from every other slot's range.
    let ranges: Vec<(i64, i64, i64)> = range.iter().map(|(d, &(s, e))| (*d, s, e)).collect();
    let mut safe: HashMap<i64, i64> = HashMap::new(); // disp -> width bytes
    for (d, s, e) in &ranges {
        let ws = &widths[d];
        if ws.len() != 1 {
            continue;
        }
        let overlaps_other = ranges
            .iter()
            .any(|(d2, s2, e2)| d2 != d && *s2 < *e && *s < *e2);
        if !overlaps_other {
            safe.insert(*d, *ws.iter().next().unwrap());
        }
    }
    if safe.is_empty() {
        return 0;
    }

    // Second pass: re-walk in CFG order with rsp tracking, rewriting the safe
    // accesses in place. Block-entry rsp offsets come from the analysis.
    let n = func.blocks.len();
    let tainted = tainted_regs(func);
    for bi in 0..n {
        let Some(mut rsp) = info.entry_rsp.get(bi).copied().flatten() else { continue };
        let mut stmts = std::mem::take(&mut func.blocks[bi].stmts);
        for s in &mut stmts {
            rewrite_stmt(s, rsp, rbp_off, &safe, &tainted);
            // Update rsp *after* the statement's accesses (matches `push`: the
            // decrement precedes the store at the new rsp).
            if let Stmt::Set { dst: Location::Reg(r), expr } = s {
                if r.0 == RSP {
                    match rsp_delta(expr) {
                        Some(RspWrite::Delta(k)) => rsp += k,
                        Some(RspWrite::FromRbp) => {
                            if let Some(o) = rbp_off {
                                rsp = o;
                            }
                        }
                        None => {}
                    }
                }
            }
        }
        func.blocks[bi].stmts = stmts;
    }
    safe.len()
}

/// Frame displacement of an address if it is a safe slot at the given `rsp`.
fn safe_disp(
    addr: &Expr,
    rsp: i64,
    rbp_off: Option<i64>,
    safe: &std::collections::HashMap<i64, i64>,
    tainted: &HashSet<u16>,
) -> Option<i64> {
    if let AddrClass::Frame(o) = classify(addr, rsp, rbp_off, tainted) {
        let d = match rbp_off {
            Some(r) => o - r,
            None => o,
        };
        if safe.contains_key(&d) {
            return Some(d);
        }
    }
    None
}

/// Rewrite safe frame `Load`s (anywhere) and a safe frame `Store` (statement
/// level) into `Frame` reads/sets.
fn rewrite_stmt(
    s: &mut Stmt,
    rsp: i64,
    rbp_off: Option<i64>,
    safe: &std::collections::HashMap<i64, i64>,
    tainted: &HashSet<u16>,
) {
    // A store to a safe slot becomes `Set { Frame(d) }`.
    if let Stmt::Store { addr, value, .. } = s {
        if let Some(d) = safe_disp(addr, rsp, rbp_off, safe, tainted) {
            let mut value = std::mem::replace(value, Expr::Undef);
            rewrite_loads(&mut value, rsp, rbp_off, safe, tainted);
            *s = Stmt::Set { dst: Location::Frame(d), expr: value };
            return;
        }
    }
    each_expr_mut(s, &mut |e| rewrite_loads(e, rsp, rbp_off, safe, tainted));
}

/// Replace `Load` of a safe frame address with `Read(Frame(d))`, recursively.
fn rewrite_loads(
    e: &mut Expr,
    rsp: i64,
    rbp_off: Option<i64>,
    safe: &std::collections::HashMap<i64, i64>,
    tainted: &HashSet<u16>,
) {
    if let Expr::Load { addr, .. } = e {
        if let Some(d) = safe_disp(addr, rsp, rbp_off, safe, tainted) {
            *e = Expr::Read(Location::Frame(d));
            return;
        }
        rewrite_loads(addr, rsp, rbp_off, safe, tainted);
        return;
    }
    match e {
        Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => rewrite_loads(x, rsp, rbp_off, safe, tainted),
        Expr::Binary(_, a, b) => {
            rewrite_loads(a, rsp, rbp_off, safe, tainted);
            rewrite_loads(b, rsp, rbp_off, safe, tainted);
        }
        Expr::Select { cond, then_, else_ } => {
            rewrite_loads(cond, rsp, rbp_off, safe, tainted);
            rewrite_loads(then_, rsp, rbp_off, safe, tainted);
            rewrite_loads(else_, rsp, rbp_off, safe, tainted);
        }
        Expr::Call { target, args, .. } => {
            if let CallTarget::Indirect(x) = target {
                rewrite_loads(x, rsp, rbp_off, safe, tainted);
            }
            for a in args {
                rewrite_loads(a, rsp, rbp_off, safe, tainted);
            }
        }
        _ => {}
    }
}

/// Apply `f` to each top-level expression of a statement (mutable).
fn each_expr_mut(s: &mut Stmt, f: &mut impl FnMut(&mut Expr)) {
    match s {
        Stmt::Set { expr, .. } | Stmt::Assign { expr, .. } | Stmt::CallStmt(expr) => f(expr),
        Stmt::Store { addr, value, .. } => {
            f(addr);
            f(value);
        }
        Stmt::Branch { cond, .. } => f(cond),
        Stmt::Switch { value, .. } => f(value),
        Stmt::Return(Some(e)) => f(e),
        _ => {}
    }
}

/// Negative-displacement frame slots (the locals).
fn collect_local_slots(func: &IrFunction, out: &mut BTreeSet<i64>) {
    let mut note = |l: &Location| {
        if let Location::Frame(d) = l {
            if *d < 0 {
                out.insert(*d);
            }
        }
    };
    for b in &func.blocks {
        for s in &b.stmts {
            if let Stmt::Set { dst, .. } = s {
                note(dst);
            }
            each_expr(s, &mut |e| {
                if let Expr::Read(l) | Expr::Addr(l) = e {
                    note(l);
                }
            });
        }
    }
}

// --- taint ----------------------------------------------------------------

/// Registers that may hold an `rsp`/`rbp`-derived value (flow-insensitive,
/// over-approximate — safe for escape detection).
fn tainted_regs(func: &IrFunction) -> HashSet<u16> {
    let mut t: HashSet<u16> = HashSet::new();
    t.insert(RSP);
    t.insert(RBP);
    loop {
        let mut changed = false;
        for b in &func.blocks {
            for s in &b.stmts {
                if let Stmt::Set { dst: Location::Reg(r), expr } = s {
                    if !t.contains(&r.0) && mentions_tainted(expr, &t) {
                        t.insert(r.0);
                        changed = true;
                    }
                }
            }
        }
        if !changed {
            break;
        }
    }
    t
}

fn mentions_tainted(e: &Expr, t: &HashSet<u16>) -> bool {
    let mut hit = false;
    walk(e, &mut |x| {
        if let Expr::Read(Location::Reg(r)) = x {
            if t.contains(&r.0) {
                hit = true;
            }
        }
    });
    hit
}

// --- the model ------------------------------------------------------------

fn analyze(func: &IrFunction) -> FrameInfo {
    let n = func.blocks.len();
    if n == 0 {
        return FrameInfo { ok: true, rbp_off: None, generic_ranges: Vec::new(), accesses: Vec::new(), entry_rsp: Vec::new(), escaped: false };
    }
    let tainted = tainted_regs(func);

    // Block-entry rsp offsets via a worklist; None = not yet reached.
    let entry = func.blocks.iter().position(|b| b.addr == func.entry).unwrap_or(0);
    let mut rsp_in: Vec<Option<i64>> = vec![None; n];
    rsp_in[entry] = Some(0);

    let mut info =
        FrameInfo { ok: true, rbp_off: None, generic_ranges: Vec::new(), accesses: Vec::new(), entry_rsp: Vec::new(), escaped: false };

    let mut work = vec![entry];
    while let Some(bi) = work.pop() {
        let mut rsp = rsp_in[bi].unwrap();
        for s in &func.blocks[bi].stmts {
            if !step(s, &mut rsp, &mut info, &tainted) {
                return FrameInfo::bail();
            }
        }
        // Propagate rsp to successors; a conflict means a non-static frame.
        for &su in &func.blocks[bi].succ {
            let su = su as usize;
            if su >= n {
                continue;
            }
            match rsp_in[su] {
                None => {
                    rsp_in[su] = Some(rsp);
                    work.push(su);
                }
                Some(prev) if prev != rsp => return FrameInfo::bail(),
                Some(_) => {}
            }
        }
    }
    info.entry_rsp = rsp_in;
    info
}

/// Interpret one statement, updating `rsp` and recording accesses/escapes.
/// Returns `false` to bail (model broken).
fn step(s: &Stmt, rsp: &mut i64, info: &mut FrameInfo, tainted: &HashSet<u16>) -> bool {
    match s {
        Stmt::Set { dst: Location::Reg(r), expr } if r.0 == RSP => {
            // rsp must move by a constant, or be reloaded from rbp (leave).
            match rsp_delta(expr) {
                Some(RspWrite::Delta(k)) => *rsp += k,
                Some(RspWrite::FromRbp) => {
                    if let Some(off) = info.rbp_off {
                        *rsp = off;
                    } else {
                        return false;
                    }
                }
                None => return false,
            }
            // The rhs may still embed a frame load (rare) — scan it.
            scan_expr(expr, *rsp, info, tainted)
        }
        Stmt::Set { dst: Location::Reg(r), expr } if r.0 == RBP => {
            // `mov rbp, rsp` establishes the frame base. A `pop rbp` (Load) is an
            // epilogue restore — ignore. Anything else makes rbp non-static.
            match expr {
                Expr::Read(Location::Reg(b)) if b.0 == RSP => {
                    match info.rbp_off {
                        None => info.rbp_off = Some(*rsp),
                        Some(prev) if prev != *rsp => return false,
                        Some(_) => {}
                    }
                    true
                }
                Expr::Load { addr, ty } => {
                    // restoring rbp from the stack: range the load, don't bail.
                    range_access(addr, int_bytes(ty), *rsp, info, tainted)
                }
                _ => false,
            }
        }
        Stmt::Set { expr, .. } => scan_expr(expr, *rsp, info, tainted),
        Stmt::Assign { expr, .. } | Stmt::CallStmt(expr) => scan_expr(expr, *rsp, info, tainted),
        Stmt::Store { addr, value, ty } => {
            // A tainted value stored to a non-frame address leaks a frame pointer.
            let cls = classify(addr, *rsp, info.rbp_off, tainted);
            if mentions_tainted(value, tainted) || matches!(value, Expr::Addr(Location::Frame(_)))
            {
                if !matches!(cls, AddrClass::Frame(_)) {
                    info.escaped = true;
                }
            }
            if !apply_class(cls, int_bytes(ty), info) {
                return false;
            }
            scan_expr(addr, *rsp, info, tainted) && scan_expr(value, *rsp, info, tainted)
        }
        Stmt::Branch { cond, .. } => scan_expr(cond, *rsp, info, tainted),
        Stmt::Switch { value, .. } => scan_expr(value, *rsp, info, tainted),
        Stmt::Return(Some(e)) => scan_expr(e, *rsp, info, tainted),
        _ => true,
    }
}

enum RspWrite {
    Delta(i64),
    FromRbp,
}

/// Recognise an `rsp` write: `rsp ± const`, or `rsp` reloaded from `rbp`.
fn rsp_delta(e: &Expr) -> Option<RspWrite> {
    match e {
        Expr::Read(Location::Reg(r)) if r.0 == RBP => Some(RspWrite::FromRbp),
        Expr::Read(Location::Reg(r)) if r.0 == RSP => Some(RspWrite::Delta(0)),
        Expr::Binary(BinOp::Add, a, b) => match (a.as_ref(), b.as_ref()) {
            (Expr::Read(Location::Reg(r)), Expr::Const(k, _)) if r.0 == RSP => {
                Some(RspWrite::Delta(*k as i64))
            }
            _ => None,
        },
        Expr::Binary(BinOp::Sub, a, b) => match (a.as_ref(), b.as_ref()) {
            (Expr::Read(Location::Reg(r)), Expr::Const(k, _)) if r.0 == RSP => {
                Some(RspWrite::Delta(-(*k as i64)))
            }
            _ => None,
        },
        _ => None,
    }
}

/// What a memory address refers to.
enum AddrClass {
    /// An rsp/rbp-relative frame access at this entry-relative byte offset.
    Frame(i64),
    /// A heap / global / constant address (disjoint from a non-escaped frame).
    Heap,
    /// A frame value reached through a copy register — offset unknown: escape.
    CopyFrame,
    /// A frame base plus a variable index — spans an unknown range: bail.
    VarFrame,
}

fn classify(e: &Expr, rsp: i64, rbp_off: Option<i64>, tainted: &HashSet<u16>) -> AddrClass {
    classify_off(e, 0, rsp, rbp_off, tainted)
}

fn classify_off(
    e: &Expr,
    off: i64,
    rsp: i64,
    rbp_off: Option<i64>,
    tainted: &HashSet<u16>,
) -> AddrClass {
    match e {
        Expr::Read(Location::Reg(r)) if r.0 == RSP => AddrClass::Frame(rsp + off),
        Expr::Read(Location::Reg(r)) if r.0 == RBP => match rbp_off {
            Some(b) => AddrClass::Frame(b + off),
            None => AddrClass::CopyFrame, // rbp used before established
        },
        Expr::Read(Location::Reg(r)) if tainted.contains(&r.0) => AddrClass::CopyFrame,
        Expr::Read(Location::Reg(_)) => AddrClass::Heap,
        Expr::Addr(Location::Frame(d)) => AddrClass::Frame(rbp_off.unwrap_or(0) + d + off),
        Expr::Const(_, _) => AddrClass::Heap,
        Expr::Binary(BinOp::Add, a, b) => match (a.as_ref(), b.as_ref()) {
            (_, Expr::Const(k, _)) => classify_off(a, off + *k as i64, rsp, rbp_off, tainted),
            (Expr::Const(k, _), _) => classify_off(b, off + *k as i64, rsp, rbp_off, tainted),
            _ => merge_var(
                classify_off(a, off, rsp, rbp_off, tainted),
                classify_off(b, 0, rsp, rbp_off, tainted),
            ),
        },
        Expr::Binary(BinOp::Sub, a, b) => match b.as_ref() {
            Expr::Const(k, _) => classify_off(a, off - *k as i64, rsp, rbp_off, tainted),
            _ => merge_var(classify_off(a, off, rsp, rbp_off, tainted), AddrClass::Heap),
        },
        _ => AddrClass::Heap,
    }
}

/// Combine a base with a variable second term: if the base is frame-related, the
/// access spans an unknown range.
fn merge_var(base: AddrClass, _other: AddrClass) -> AddrClass {
    match base {
        AddrClass::Frame(_) | AddrClass::CopyFrame | AddrClass::VarFrame => AddrClass::VarFrame,
        AddrClass::Heap => AddrClass::Heap,
    }
}

/// Record/observe an address classification. Returns `false` to bail.
fn apply_class(cls: AddrClass, width_bytes: i64, info: &mut FrameInfo) -> bool {
    match cls {
        AddrClass::Frame(o) => {
            let w = width_bytes.max(1);
            info.generic_ranges.push((o, o + w));
            info.accesses.push((o, w));
            true
        }
        AddrClass::CopyFrame => {
            info.escaped = true;
            true
        }
        AddrClass::VarFrame => false,
        AddrClass::Heap => true,
    }
}

/// Classify a single load address and record it.
fn range_access(
    addr: &Expr,
    width_bytes: i64,
    rsp: i64,
    info: &mut FrameInfo,
    tainted: &HashSet<u16>,
) -> bool {
    let cls = classify(addr, rsp, info.rbp_off, tainted);
    apply_class(cls, width_bytes, info)
}

/// Scan an expression for nested frame loads, escaping `lea`s, and frame
/// pointers passed to calls. Returns `false` to bail.
fn scan_expr(e: &Expr, rsp: i64, info: &mut FrameInfo, tainted: &HashSet<u16>) -> bool {
    let mut ok = true;
    match e {
        Expr::Addr(Location::Frame(_)) => info.escaped = true,
        Expr::Load { addr, ty } => {
            if !range_access(addr, int_bytes(ty), rsp, info, tainted) {
                ok = false;
            }
            if !scan_expr(addr, rsp, info, tainted) {
                ok = false;
            }
        }
        Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => ok = scan_expr(x, rsp, info, tainted),
        Expr::Binary(_, a, b) => {
            ok = scan_expr(a, rsp, info, tainted) && scan_expr(b, rsp, info, tainted)
        }
        Expr::Select { cond, then_, else_ } => {
            ok = scan_expr(cond, rsp, info, tainted)
                && scan_expr(then_, rsp, info, tainted)
                && scan_expr(else_, rsp, info, tainted)
        }
        Expr::Call { target, args, .. } => {
            if let CallTarget::Indirect(x) = target {
                if !scan_expr(x, rsp, info, tainted) {
                    ok = false;
                }
            }
            for a in args {
                // A frame pointer passed to a call escapes.
                if matches!(a, Expr::Addr(Location::Frame(_))) || mentions_tainted(a, tainted) {
                    info.escaped = true;
                }
                if !scan_expr(a, rsp, info, tainted) {
                    ok = false;
                }
            }
        }
        _ => {}
    }
    ok
}

fn int_bytes(t: &Ty) -> i64 {
    (match t {
        Ty::Int { bits, .. } => (*bits / 8).max(1),
        _ => 8,
    }) as i64
}

// --- small expression walkers --------------------------------------------

fn walk(e: &Expr, f: &mut impl FnMut(&Expr)) {
    f(e);
    match e {
        Expr::Load { addr, .. } => walk(addr, f),
        Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => walk(x, f),
        Expr::Binary(_, a, b) => {
            walk(a, f);
            walk(b, f);
        }
        Expr::Select { cond, then_, else_ } => {
            walk(cond, f);
            walk(then_, f);
            walk(else_, f);
        }
        Expr::Call { target, args, .. } => {
            if let CallTarget::Indirect(x) = target {
                walk(x, f);
            }
            for a in args {
                walk(a, f);
            }
        }
        _ => {}
    }
}

fn each_expr(s: &Stmt, f: &mut impl FnMut(&Expr)) {
    match s {
        Stmt::Set { expr, .. } | Stmt::Assign { expr, .. } | Stmt::CallStmt(expr) => walk(expr, f),
        Stmt::Store { addr, value, .. } => {
            walk(addr, f);
            walk(value, f);
        }
        Stmt::Branch { cond, .. } => walk(cond, f),
        Stmt::Switch { value, .. } => walk(value, f),
        Stmt::Return(Some(e)) => walk(e, f),
        _ => {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rsp() -> Expr {
        Expr::Read(Location::Reg(RegId(RSP)))
    }
    fn rbp() -> Expr {
        Expr::Read(Location::Reg(RegId(RBP)))
    }

    fn func(stmts: Vec<Stmt>) -> IrFunction {
        IrFunction {
            entry: 0,
            name: "t".into(),
            bits: 64,
            reg_params: vec![],
            blocks: vec![Block { id: 0, addr: 0, stmts, succ: vec![], pred: vec![] }],
            next_value: 0,
            next_temp: 0,
        }
    }

    /// Standard prologue + a local + a heap store: the local is promotable even
    /// though the function dereferences a heap pointer.
    #[test]
    fn local_promotable_despite_heap_access() {
        let f = func(vec![
            // push rbp ; mov rbp, rsp ; sub rsp, 0x20
            Stmt::Set { dst: Location::Reg(RegId(RSP)), expr: Expr::Binary(BinOp::Sub, Box::new(rsp()), Box::new(Expr::konst(8, 64))) },
            Stmt::Store { addr: rsp(), value: rbp(), ty: Ty::int(64) }, // save rbp on stack (benign)
            Stmt::Set { dst: Location::Reg(RegId(RBP)), expr: rsp() },
            Stmt::Set { dst: Location::Reg(RegId(RSP)), expr: Expr::Binary(BinOp::Sub, Box::new(rsp()), Box::new(Expr::konst(0x20, 64))) },
            // local_8 = 1
            Stmt::Set { dst: Location::Frame(-8), expr: Expr::konst(1, 64) },
            // *(uint64*)(rdi) = 2   (heap)
            Stmt::Store { addr: Expr::Read(Location::Reg(RegId(7))), value: Expr::konst(2, 64), ty: Ty::int(64) },
        ]);
        let slots = promotable_slots(&f);
        assert!(slots.contains(&-8), "expected -8 promotable, got {:?}", slots);
    }

    /// Taking a local's address blocks promotion (a frame pointer escapes).
    #[test]
    fn address_taken_blocks() {
        let f = func(vec![
            Stmt::Store { addr: rsp(), value: rbp(), ty: Ty::int(64) },
            Stmt::Set { dst: Location::Reg(RegId(RBP)), expr: rsp() },
            Stmt::Set { dst: Location::Frame(-8), expr: Expr::konst(1, 64) },
            // g(&local_8)
            Stmt::CallStmt(Expr::Call {
                target: CallTarget::Named("g".into()),
                args: vec![Expr::Addr(Location::Frame(-8))],
                ret: Ty::Unknown,
            }),
        ]);
        assert!(promotable_slots(&f).is_empty());
    }

    /// An rsp-relative spill that overlaps the local blocks it; a disjoint one
    /// does not. Here the spill is at rbp-8 (overlaps local_8) → not promotable.
    #[test]
    fn overlapping_spill_blocks_only_that_slot() {
        let f = func(vec![
            Stmt::Store { addr: rsp(), value: rbp(), ty: Ty::int(64) },
            Stmt::Set { dst: Location::Reg(RegId(RBP)), expr: rsp() }, // rbp_off = -8
            Stmt::Set { dst: Location::Reg(RegId(RSP)), expr: Expr::Binary(BinOp::Sub, Box::new(rsp()), Box::new(Expr::konst(0x10, 64))) }, // rsp = -24
            Stmt::Set { dst: Location::Frame(-8), expr: Expr::konst(1, 64) },   // entry off -16
            Stmt::Set { dst: Location::Frame(-16), expr: Expr::konst(2, 64) },  // entry off -24
            // generic store at [rsp + 8] = entry off -16  -> overlaps local_8
            Stmt::Store { addr: Expr::Binary(BinOp::Add, Box::new(rsp()), Box::new(Expr::konst(8, 64))), value: Expr::konst(3, 64), ty: Ty::int(64) },
        ]);
        let slots = promotable_slots(&f);
        assert!(!slots.contains(&-8), "local_8 overlaps the spill: {:?}", slots);
        assert!(slots.contains(&-16), "local_16 is disjoint: {:?}", slots);
    }

    /// An `rsp`-relative spill accessed twice is recovered as one stable slot
    /// in rbp-relative coordinates.
    #[test]
    fn recovers_rsp_spill_as_slot() {
        let f = func(vec![
            Stmt::Set { dst: Location::Reg(RegId(RSP)), expr: Expr::Binary(BinOp::Sub, Box::new(rsp()), Box::new(Expr::konst(8, 64))) },
            Stmt::Store { addr: rsp(), value: rbp(), ty: Ty::int(64) }, // save rbp
            Stmt::Set { dst: Location::Reg(RegId(RBP)), expr: rsp() },   // rbp_off = -8
            Stmt::Set { dst: Location::Reg(RegId(RSP)), expr: Expr::Binary(BinOp::Sub, Box::new(rsp()), Box::new(Expr::konst(0x10, 64))) }, // rsp=-24
            // *(uint64*)(rsp+8) = rdi   -> entry off -16 -> disp -8
            Stmt::Store { addr: Expr::Binary(BinOp::Add, Box::new(rsp()), Box::new(Expr::konst(8, 64))), value: Expr::Read(Location::Reg(RegId(7))), ty: Ty::int(64) },
            // rax = *(uint64*)(rsp+8)
            Stmt::Assign { dst: ValueId(1), expr: Expr::Load { addr: Box::new(Expr::Binary(BinOp::Add, Box::new(rsp()), Box::new(Expr::konst(8, 64)))), ty: Ty::int(64) } },
        ]);
        let slots = recover_stack_slots(&f).expect("frame model should hold");
        let s = slots.get(&-8).expect("spill at disp -8 recovered");
        assert_eq!(s.count, 2, "store + load");
        assert!(s.widths.contains(&64));
    }

    /// Promotion rewrites a safe rsp spill's store/load into Frame set/read.
    #[test]
    fn promote_rewrites_spill_to_frame() {
        let mut f = func(vec![
            Stmt::Set { dst: Location::Reg(RegId(RSP)), expr: Expr::Binary(BinOp::Sub, Box::new(rsp()), Box::new(Expr::konst(8, 64))) },
            Stmt::Store { addr: rsp(), value: rbp(), ty: Ty::int(64) },
            Stmt::Set { dst: Location::Reg(RegId(RBP)), expr: rsp() }, // rbp_off=-8
            Stmt::Set { dst: Location::Reg(RegId(RSP)), expr: Expr::Binary(BinOp::Sub, Box::new(rsp()), Box::new(Expr::konst(0x10, 64))) },
            // *(uint64*)(rsp+8) = rdi   (disp -8)
            Stmt::Store { addr: Expr::Binary(BinOp::Add, Box::new(rsp()), Box::new(Expr::konst(8, 64))), value: Expr::Read(Location::Reg(RegId(7))), ty: Ty::int(64) },
            // rax = *(uint64*)(rsp+8)
            Stmt::Assign { dst: ValueId(1), expr: Expr::Load { addr: Box::new(Expr::Binary(BinOp::Add, Box::new(rsp()), Box::new(Expr::konst(8, 64)))), ty: Ty::int(64) } },
        ]);
        let n = promote_stack_slots(&mut f);
        assert!(n >= 1, "expected a slot promoted");
        let st = &f.blocks[0].stmts;
        // the spill store became Set { Frame(-8) }
        assert!(st.iter().any(|s| matches!(s, Stmt::Set { dst: Location::Frame(-8), .. })),
            "spill store should be Set Frame(-8): {:?}", st);
        // the reload became Read(Frame(-8))
        assert!(st.iter().any(|s| matches!(s, Stmt::Assign { expr: Expr::Read(Location::Frame(-8)), .. })),
            "reload should read Frame(-8): {:?}", st);
    }

    /// A variable-indexed frame access makes the analysis bail (sound).
    #[test]
    fn variable_index_bails() {
        let f = func(vec![
            Stmt::Store { addr: rsp(), value: rbp(), ty: Ty::int(64) },
            Stmt::Set { dst: Location::Reg(RegId(RBP)), expr: rsp() },
            Stmt::Set { dst: Location::Frame(-8), expr: Expr::konst(1, 64) },
            // store at [rbp + rax]
            Stmt::Store { addr: Expr::Binary(BinOp::Add, Box::new(rbp()), Box::new(Expr::Read(Location::Reg(RegId(0))))), value: Expr::konst(0, 64), ty: Ty::int(64) },
        ]);
        assert!(promotable_slots(&f).is_empty());
    }
}
