//! Disassembly layer built on `iced-x86`. Decodes bytes into a uniform
//! `Insn` carrying the control-flow facts the analysis passes need.

use crate::loader::{Bitness, Program};
use iced_x86::{Decoder, DecoderOptions, Formatter, Instruction, IntelFormatter, OpKind};

/// High-level classification of how control leaves an instruction.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Flow {
    /// Falls through to the next instruction.
    Fallthrough,
    /// Unconditional jump to a known address.
    Jump,
    /// Conditional jump (two successors: target and fallthrough).
    CondJump,
    /// Direct call to a known address (returns to fallthrough).
    Call,
    /// Call/jump through a register or memory (target unknown statically).
    Indirect,
    /// Returns from the function.
    Return,
    /// Software interrupt / syscall / int3 — treated as a terminator.
    Interrupt,
}

/// A decoded instruction plus the facts we care about for analysis.
#[derive(Debug, Clone)]
pub struct Insn {
    pub address: u64,
    pub len: usize,
    pub text: String,
    pub flow: Flow,
    /// For Jump/CondJump/Call: the resolved target address.
    pub target: Option<u64>,
    /// The raw iced instruction, kept for IR lifting.
    pub raw: Instruction,
}

impl Insn {
    pub fn next_addr(&self) -> u64 {
        self.address + self.len as u64
    }
}

pub struct Disassembler {
    bitness: u32,
}

impl Disassembler {
    pub fn new(bitness: Bitness) -> Self {
        Disassembler {
            bitness: bitness.bits(),
        }
    }

    /// Decode a single instruction at virtual address `addr` from program memory.
    /// Returns `None` if the address is not mapped or decoding fails.
    pub fn decode_at(&self, prog: &Program, addr: u64) -> Option<Insn> {
        let bytes = prog.read_from(addr)?;
        // Limit window to the max instruction length to avoid over-reading.
        let window = &bytes[..bytes.len().min(16)];
        let mut decoder = Decoder::with_ip(self.bitness, window, addr, DecoderOptions::NONE);
        if !decoder.can_decode() {
            return None;
        }
        let mut raw = Instruction::default();
        decoder.decode_out(&mut raw);
        if raw.is_invalid() || raw.len() == 0 {
            return None;
        }

        let (flow, mut target) = classify(&raw);
        // In object files, a direct call/jump's displacement is a placeholder
        // fixed up by a relocation; prefer the relocation-resolved target so
        // recursive/cross-function calls decode to the real address.
        if matches!(flow, Flow::Call | Flow::Jump | Flow::CondJump) {
            if let Some(t) = prog.reloc_branch_target(addr, raw.len()) {
                target = Some(t);
            }
        }

        let mut formatter = IntelFormatter::new();
        // Render numbers C-style (0x..., lowercase, no leading zeros) so the
        // listing doubles as a basis for pseudo-C translation.
        let opts = formatter.options_mut();
        opts.set_hex_prefix("0x");
        opts.set_hex_suffix("");
        opts.set_uppercase_hex(false);
        opts.set_leading_zeros(false);
        opts.set_space_after_operand_separator(true);
        let mut text = String::new();
        formatter.format(&raw, &mut text);

        Some(Insn {
            address: addr,
            len: raw.len(),
            text,
            flow,
            target,
            raw,
        })
    }
}

/// Derive flow class and resolved target from an iced instruction.
fn classify(insn: &Instruction) -> (Flow, Option<u64>) {
    use iced_x86::FlowControl as FC;
    match insn.flow_control() {
        FC::Next => (Flow::Fallthrough, None),
        FC::Return => (Flow::Return, None),
        FC::UnconditionalBranch => (Flow::Jump, near_target(insn)),
        FC::ConditionalBranch => (Flow::CondJump, near_target(insn)),
        FC::Call => (Flow::Call, near_target(insn)),
        // Indirect call still returns to the fallthrough, so model it as a Call
        // with an unknown target rather than a block terminator.
        FC::IndirectCall => (Flow::Call, None),
        FC::IndirectBranch => (Flow::Indirect, None),
        FC::Interrupt | FC::Exception | FC::XbeginXabortXend => (Flow::Interrupt, None),
    }
}

/// Resolve a direct near/relative branch/call target.
fn near_target(insn: &Instruction) -> Option<u64> {
    match insn.op0_kind() {
        OpKind::NearBranch16 | OpKind::NearBranch32 | OpKind::NearBranch64 => {
            Some(insn.near_branch_target())
        }
        _ => None,
    }
}
