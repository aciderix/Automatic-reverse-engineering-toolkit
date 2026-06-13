//! Control-flow analysis: function discovery (recursive descent) and
//! basic-block / CFG construction.

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
}

/// Entry point: discover functions then build each one's CFG.
pub fn analyze(prog: &Program, disasm: &Disassembler) -> AnalysisResult {
    let entries = discover_functions(prog, disasm);
    let mut functions = Vec::new();
    for &entry in &entries {
        if let Some(f) = build_function(prog, disasm, entry, &entries) {
            functions.push(f);
        }
    }
    functions.sort_by_key(|f| f.entry);
    AnalysisResult { functions }
}

/// Phase 1 — find every plausible function entry by following direct calls
/// transitively from the seed set (entry point + symbols).
fn discover_functions(prog: &Program, disasm: &Disassembler) -> BTreeSet<u64> {
    let mut entries: BTreeSet<u64> = prog.seed_functions().into_iter().collect();
    let mut work: VecDeque<u64> = entries.iter().copied().collect();

    while let Some(entry) = work.pop_front() {
        let (_, callees) = sweep(prog, disasm, entry, &BTreeSet::new());
        for callee in callees {
            if prog.is_executable(callee) && entries.insert(callee) {
                work.push_back(callee);
            }
        }
    }
    entries
}

/// Linearly explore a function body via recursive descent, returning every
/// decoded instruction (keyed by address) and the set of direct call targets.
///
/// `boundary` lists addresses owned by *other* functions; traversal never
/// decodes across them (prevents two functions from being merged).
fn sweep(
    prog: &Program,
    disasm: &Disassembler,
    entry: u64,
    boundary: &BTreeSet<u64>,
) -> (BTreeMap<u64, Insn>, BTreeSet<u64>) {
    let mut insns: BTreeMap<u64, Insn> = BTreeMap::new();
    let mut callees = BTreeSet::new();
    let mut work: VecDeque<u64> = VecDeque::new();
    work.push_back(entry);

    while let Some(addr) = work.pop_front() {
        if insns.contains_key(&addr) {
            continue;
        }
        if addr != entry && boundary.contains(&addr) {
            continue;
        }
        if !prog.is_executable(addr) {
            continue;
        }
        let insn = match disasm.decode_at(prog, addr) {
            Some(i) => i,
            None => continue,
        };

        let next = insn.next_addr();
        match insn.flow {
            Flow::Fallthrough => work.push_back(next),
            Flow::Call => {
                if let Some(t) = insn.target {
                    callees.insert(t);
                }
                work.push_back(next); // call returns
            }
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

    (insns, callees)
}

/// Phase 2 — build the CFG of a single function.
fn build_function(
    prog: &Program,
    disasm: &Disassembler,
    entry: u64,
    all_entries: &BTreeSet<u64>,
) -> Option<Function> {
    // Boundary = every other function entry.
    let mut boundary = all_entries.clone();
    boundary.remove(&entry);

    let (insns, callees) = sweep(prog, disasm, entry, &boundary);
    if insns.is_empty() {
        return None;
    }

    // Determine basic-block leaders.
    let mut leaders: BTreeSet<u64> = BTreeSet::new();
    leaders.insert(entry);
    for insn in insns.values() {
        match insn.flow {
            Flow::CondJump => {
                if let Some(t) = insn.target {
                    if insns.contains_key(&t) {
                        leaders.insert(t);
                    }
                }
                if insns.contains_key(&insn.next_addr()) {
                    leaders.insert(insn.next_addr());
                }
            }
            Flow::Jump => {
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

    // Walk instructions in address order, cutting at leaders / terminators.
    let ordered: Vec<u64> = insns.keys().copied().collect();
    let mut blocks: IndexMap<u64, BasicBlock> = IndexMap::new();
    let mut cur_start: Option<u64> = None;
    let mut cur: Vec<Insn> = Vec::new();

    let flush = |blocks: &mut IndexMap<u64, BasicBlock>, start: u64, body: Vec<Insn>| {
        if body.is_empty() {
            return;
        }
        let last = body.last().unwrap();
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
            // Block was cut because the next instruction is a leader: fall through.
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
        }
        if cur_start.is_none() || is_leader {
            cur_start = Some(addr);
        }
        let terminates = matches!(
            insn.flow,
            Flow::CondJump | Flow::Jump | Flow::Return | Flow::Indirect | Flow::Interrupt
        );
        // Detect a boundary where the *next* ordered instruction is a leader.
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
