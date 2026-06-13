//! Control-flow analysis: function discovery (recursive descent) and
//! basic-block / CFG construction.
//!
//! Strategy for scaling to large binaries: decode every reachable address
//! exactly once into a global instruction map (Phase A), then partition that
//! map into functions with cheap map lookups (Phase B). This keeps the whole
//! pipeline ~O(code reached) instead of O(functions × size).

use crate::disasm::{Disassembler, Flow, Insn};
use crate::loader::Program;
use indexmap::IndexMap;
use std::collections::{BTreeMap, BTreeSet, VecDeque};

/// A maximal straight-line run of instructions with a single entry and exit.
#[derive(Debug, Clone)]
pub struct BasicBlock {
    pub start: u64,
    pub insns: Vec<Insn>,
    /// Start addresses of successor blocks (within this function).
    pub successors: Vec<u64>,
    pub terminator: Flow,
}

impl BasicBlock {
    pub fn end(&self) -> u64 {
        self.insns.last().map(|i| i.next_addr()).unwrap_or(self.start)
    }
}

/// A recovered function: an entry point and its basic blocks in address order.
#[derive(Debug, Clone)]
pub struct Function {
    pub entry: u64,
    pub name: String,
    /// Blocks keyed by start address, preserving address order.
    pub blocks: IndexMap<u64, BasicBlock>,
    /// Direct call targets observed in the body (callees).
    pub callees: BTreeSet<u64>,
}

/// Result of analysing a whole program.
pub struct AnalysisResult {
    pub functions: Vec<Function>,
    /// Total instructions decoded across the program.
    pub instruction_count: usize,
}

/// Entry point: global decode, then build each function's CFG. When
/// `prologue_scan` is set, also recover functions reached only indirectly by
/// scanning executable sections for function prologues.
pub fn analyze(prog: &Program, disasm: &Disassembler, prologue_scan: bool) -> AnalysisResult {
    let (global, entries) = global_decode(prog, disasm, prologue_scan);
    let instruction_count = global.len();

    let mut functions = Vec::new();
    for &entry in &entries {
        if let Some(f) = build_function(prog, &global, entry, &entries) {
            functions.push(f);
        }
    }
    functions.sort_by_key(|f| f.entry);
    AnalysisResult {
        functions,
        instruction_count,
    }
}

/// Phase A — decode every reachable instruction once, collecting the set of
/// function entry points (entry + direct/indirect-target call sites).
fn global_decode(
    prog: &Program,
    disasm: &Disassembler,
    prologue_scan: bool,
) -> (BTreeMap<u64, Insn>, BTreeSet<u64>) {
    let mut global: BTreeMap<u64, Insn> = BTreeMap::new();
    let mut entries: BTreeSet<u64> = prog.seed_functions().into_iter().collect();
    if entries.is_empty() && prog.is_executable(prog.entry) {
        entries.insert(prog.entry);
    }

    // Pass 1: recursive descent from direct-call seeds.
    let mut work: VecDeque<u64> = entries.iter().copied().collect();
    drain(prog, disasm, &mut global, &mut entries, &mut work);

    // Pass 2: prologue scanning recovers indirectly-reached functions. Only
    // seed prologues we haven't already decoded as part of a known function.
    if prologue_scan {
        let mut extra: VecDeque<u64> = VecDeque::new();
        for p in prog.prologue_seeds() {
            if !global.contains_key(&p) && prog.is_executable(p) && entries.insert(p) {
                extra.push_back(p);
            }
        }
        drain(prog, disasm, &mut global, &mut entries, &mut extra);
    }

    (global, entries)
}

/// Drain a worklist of run starts, decoding each reachable instruction once
/// into `global` and recording direct-call targets as new entries.
fn drain(
    prog: &Program,
    disasm: &Disassembler,
    global: &mut BTreeMap<u64, Insn>,
    entries: &mut BTreeSet<u64>,
    work: &mut VecDeque<u64>,
) {
    while let Some(start) = work.pop_front() {
        let mut cur = start;
        loop {
            if global.contains_key(&cur) || !prog.is_executable(cur) {
                break;
            }
            let insn = match disasm.decode_at(prog, cur) {
                Some(i) => i,
                None => break,
            };
            let next = insn.next_addr();
            let flow = insn.flow;
            let target = insn.target;
            global.insert(cur, insn);

            match flow {
                Flow::Fallthrough => cur = next,
                Flow::Call => {
                    if let Some(t) = target {
                        if prog.is_executable(t) && entries.insert(t) {
                            work.push_back(t);
                        }
                    }
                    cur = next; // call returns to fallthrough
                }
                Flow::CondJump => {
                    if let Some(t) = target {
                        work.push_back(t);
                    }
                    cur = next;
                }
                Flow::Jump => {
                    if let Some(t) = target {
                        work.push_back(t);
                    }
                    break;
                }
                Flow::Return | Flow::Indirect | Flow::Interrupt => break,
            }
        }
    }
}

/// Collect the instructions belonging to one function by walking intra-function
/// edges over the already-decoded global map, stopping at other entries.
fn collect_function(
    global: &BTreeMap<u64, Insn>,
    entry: u64,
    boundary: &BTreeSet<u64>,
) -> BTreeMap<u64, Insn> {
    let mut insns: BTreeMap<u64, Insn> = BTreeMap::new();
    let mut work: VecDeque<u64> = VecDeque::new();
    work.push_back(entry);

    while let Some(addr) = work.pop_front() {
        if insns.contains_key(&addr) {
            continue;
        }
        if addr != entry && boundary.contains(&addr) {
            continue; // belongs to another function
        }
        let insn = match global.get(&addr) {
            Some(i) => i.clone(),
            None => continue,
        };
        let next = insn.next_addr();
        match insn.flow {
            Flow::Fallthrough => work.push_back(next),
            Flow::Call => work.push_back(next),
            Flow::CondJump => {
                if let Some(t) = insn.target {
                    work.push_back(t);
                }
                work.push_back(next);
            }
            Flow::Jump => {
                if let Some(t) = insn.target {
                    work.push_back(t);
                }
            }
            Flow::Return | Flow::Indirect | Flow::Interrupt => {}
        }
        insns.insert(addr, insn);
    }
    insns
}

/// Phase B — build the CFG of a single function from the global map.
fn build_function(
    prog: &Program,
    global: &BTreeMap<u64, Insn>,
    entry: u64,
    all_entries: &BTreeSet<u64>,
) -> Option<Function> {
    let mut boundary = all_entries.clone();
    boundary.remove(&entry);

    let insns = collect_function(global, entry, &boundary);
    if insns.is_empty() {
        return None;
    }

    let mut callees = BTreeSet::new();
    for insn in insns.values() {
        if insn.flow == Flow::Call {
            if let Some(t) = insn.target {
                callees.insert(t);
            }
        }
    }

    // Determine basic-block leaders.
    let mut leaders: BTreeSet<u64> = BTreeSet::new();
    leaders.insert(entry);
    for insn in insns.values() {
        match insn.flow {
            Flow::CondJump | Flow::Jump => {
                if let Some(t) = insn.target {
                    if insns.contains_key(&t) {
                        leaders.insert(t);
                    }
                }
                if insns.contains_key(&insn.next_addr()) {
                    leaders.insert(insn.next_addr());
                }
            }
            Flow::Return | Flow::Indirect | Flow::Interrupt => {
                if insns.contains_key(&insn.next_addr()) {
                    leaders.insert(insn.next_addr());
                }
            }
            _ => {}
        }
    }

    let ordered: Vec<u64> = insns.keys().copied().collect();
    let mut blocks: IndexMap<u64, BasicBlock> = IndexMap::new();
    let mut cur_start: Option<u64> = None;
    let mut cur: Vec<Insn> = Vec::new();

    let flush = |blocks: &mut IndexMap<u64, BasicBlock>, start: u64, body: Vec<Insn>| {
        if body.is_empty() {
            return;
        }
        let last: &Insn = body.last().unwrap();
        let terminator = last.flow;
        let next = last.next_addr();
        let successors = match terminator {
            Flow::CondJump => {
                let mut s = Vec::new();
                if let Some(t) = last.target {
                    s.push(t);
                }
                s.push(next);
                s
            }
            Flow::Jump => last.target.into_iter().collect(),
            Flow::Return | Flow::Indirect | Flow::Interrupt => Vec::new(),
            Flow::Fallthrough | Flow::Call => vec![next],
        };
        blocks.insert(
            start,
            BasicBlock {
                start,
                insns: body,
                successors,
                terminator,
            },
        );
    };

    for (i, &addr) in ordered.iter().enumerate() {
        let insn = insns[&addr].clone();
        let is_leader = leaders.contains(&addr);
        if is_leader && cur_start.is_some() {
            flush(&mut blocks, cur_start.unwrap(), std::mem::take(&mut cur));
            cur_start = None;
        }
        if cur_start.is_none() {
            cur_start = Some(addr);
        }
        let terminates = matches!(
            insn.flow,
            Flow::CondJump | Flow::Jump | Flow::Return | Flow::Indirect | Flow::Interrupt
        );
        let next_is_leader = ordered
            .get(i + 1)
            .map(|n| leaders.contains(n))
            .unwrap_or(true);
        cur.push(insn);
        if terminates || next_is_leader {
            flush(&mut blocks, cur_start.unwrap(), std::mem::take(&mut cur));
            cur_start = None;
        }
    }
    if let Some(start) = cur_start {
        flush(&mut blocks, start, std::mem::take(&mut cur));
    }

    let name = prog
        .symbol_name(entry)
        .map(|s| s.to_string())
        .unwrap_or_else(|| format!("sub_{:x}", entry));

    Some(Function {
        entry,
        name,
        blocks,
        callees,
    })
}
