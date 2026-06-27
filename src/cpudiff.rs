//! Differential CPU test: lift an instruction, then check the lifted IR against
//! the Unicorn x86 emulator (the oracle) over many random register/flag states.
//!
//! This validates the *lifter* — the most bug-prone layer, where the carry/sign
//! flag bugs (`add_flags`, `sub_flags`), the x87 depth pass, the shift/cmov
//! handling all live. Instead of waiting for a real program (Lua, busybox) to
//! surface a miscompiled instruction via a crash, we evaluate the lifted IR
//! directly against Unicorn for thousands of states and report any divergence.
//! The IR interpreter only models the integer subset; anything it cannot
//! evaluate (memory, sign-extension width, calls, x87) makes it return `None`
//! and the case is skipped — so the harness never raises a false alarm.
//!
//! Gated on `feature = "unpack"` (it reuses the system libunicorn already linked
//! for the dynamic unpacker). Run: `cargo test --features unpack cpudiff`.
#![cfg(feature = "unpack")]

use crate::ir::types::{BinOp, Expr, FlagKind, Location, Stmt, Ty, UnOp};
use std::collections::HashMap;
use std::os::raw::{c_int, c_void};

// ---- Minimal FFI to the system libunicorn (x86) ---------------------------
#[allow(non_camel_case_types)]
type uc_engine = c_void;
const UC_ARCH_X86: c_int = 4;
const UC_MODE_32: c_int = 4;
const UC_PROT_ALL: u32 = 7;

// UC_X86_REG ids (from unicorn/x86.h).
const UC_X86_REG_EAX: c_int = 19;
const UC_X86_REG_EBP: c_int = 20;
const UC_X86_REG_EBX: c_int = 21;
const UC_X86_REG_ECX: c_int = 22;
const UC_X86_REG_EDI: c_int = 23;
const UC_X86_REG_EDX: c_int = 24;
const UC_X86_REG_EFLAGS: c_int = 25;
const UC_X86_REG_ESI: c_int = 29;
const UC_X86_REG_ESP: c_int = 30;

#[link(name = "unicorn")]
extern "C" {
    fn uc_open(arch: c_int, mode: c_int, uc: *mut *mut uc_engine) -> c_int;
    fn uc_close(uc: *mut uc_engine) -> c_int;
    fn uc_mem_map(uc: *mut uc_engine, address: u64, size: usize, perms: u32) -> c_int;
    fn uc_mem_write(uc: *mut uc_engine, address: u64, bytes: *const c_void, size: usize) -> c_int;
    fn uc_reg_write(uc: *mut uc_engine, regid: c_int, value: *const c_void) -> c_int;
    fn uc_reg_read(uc: *mut uc_engine, regid: c_int, value: *mut c_void) -> c_int;
    fn uc_emu_start(uc: *mut uc_engine, begin: u64, until: u64, timeout: u64, count: usize)
        -> c_int;
}

/// RegId index (0=rax..7=rdi) -> the 32-bit Unicorn register id.
const UC_GP: [c_int; 8] = [
    UC_X86_REG_EAX,
    UC_X86_REG_ECX,
    UC_X86_REG_EDX,
    UC_X86_REG_EBX,
    UC_X86_REG_ESP,
    UC_X86_REG_EBP,
    UC_X86_REG_ESI,
    UC_X86_REG_EDI,
];

const CODE_ADDR: u64 = 0x1000;
const STACK_ADDR: u64 = 0x20_0000; // a valid ESP so stack-touching ops don't fault

// EFLAGS bit positions for the flags we model.
fn flag_bit(f: FlagKind) -> u32 {
    match f {
        FlagKind::Cf => 0,
        FlagKind::Pf => 2,
        FlagKind::Af => 4,
        FlagKind::Zf => 6,
        FlagKind::Sf => 7,
        FlagKind::Of => 11,
    }
}

/// The CPU state both sides start from / are compared on.
#[derive(Clone)]
struct CpuState {
    regs: [u64; 8],
    flags: HashMap<FlagKind, u64>,
}

// ---- IR interpreter (integer subset) --------------------------------------

struct Interp<'a> {
    regs: [u64; 8],
    flags: HashMap<FlagKind, u64>,
    temps: HashMap<u32, u64>,
    _life: std::marker::PhantomData<&'a ()>,
}

impl<'a> Interp<'a> {
    fn new(s: &CpuState) -> Self {
        Interp {
            regs: s.regs,
            flags: s.flags.clone(),
            temps: HashMap::new(),
            _life: std::marker::PhantomData,
        }
    }

    /// Evaluate an expression to a 64-bit value, or `None` if it uses something
    /// the integer interpreter does not model (memory load, width-bearing
    /// sign/zero-extension or truncate, a call, an SSA node, …).
    fn eval(&self, e: &Expr) -> Option<u64> {
        Some(match e {
            Expr::Const(v, _) => *v as u64,
            Expr::Read(loc) => match loc {
                Location::Reg(r) if (r.0 as usize) < 8 => self.regs[r.0 as usize],
                Location::Flag(f) => self.flags.get(f).copied().unwrap_or(0),
                Location::Temp(t) => self.temps.get(t).copied().unwrap_or(0),
                _ => return None,
            },
            Expr::Unary(op, x) => {
                let a = self.eval(x)?;
                match op {
                    UnOp::Neg => 0u64.wrapping_sub(a),
                    UnOp::Not => !a,
                    // width-dependent: not modelled here
                    UnOp::SignExtend | UnOp::ZeroExtend | UnOp::Truncate => return None,
                }
            }
            Expr::Binary(op, a, b) => {
                let x = self.eval(a)?;
                let y = self.eval(b)?;
                bin(*op, x, y)?
            }
            Expr::Cast { to, expr } => {
                let v = self.eval(expr)?;
                let bits: u32 = match to {
                    Ty::Int { bits, .. } => *bits as u32,
                    Ty::Bool => 1,
                    _ => return None,
                };
                if bits >= 64 { v } else { v & ((1u64 << bits) - 1) }
            }
            Expr::Select { cond, then_, else_ } => {
                if self.eval(cond)? != 0 { self.eval(then_)? } else { self.eval(else_)? }
            }
            // Memory, addresses, calls, SSA forms: not modelled -> skip the case.
            _ => return None,
        })
    }

    /// Execute one lifted statement; `None` if it touches the unmodelled world.
    fn exec(&mut self, s: &Stmt) -> Option<()> {
        match s {
            Stmt::Set { dst, expr } => {
                let v = self.eval(expr)?;
                match dst {
                    Location::Reg(r) if (r.0 as usize) < 8 => self.regs[r.0 as usize] = v,
                    Location::Flag(f) => {
                        self.flags.insert(*f, v & 1);
                    }
                    Location::Temp(t) => {
                        self.temps.insert(*t, v);
                    }
                    _ => return None,
                }
                Some(())
            }
            Stmt::Nop => Some(()),
            // A store, branch, call, asm, … — not part of a single-instruction
            // arithmetic test; bail so the case is skipped rather than mis-scored.
            _ => None,
        }
    }
}

fn bin(op: BinOp, a: u64, b: u64) -> Option<u64> {
    use BinOp::*;
    let r = match op {
        Add => a.wrapping_add(b),
        Sub => a.wrapping_sub(b),
        Mul => a.wrapping_mul(b),
        UDiv => {
            if b == 0 { return None; }
            a / b
        }
        SDiv => {
            if b == 0 { return None; }
            ((a as i64).wrapping_div(b as i64)) as u64
        }
        UMod => {
            if b == 0 { return None; }
            a % b
        }
        SMod => {
            if b == 0 { return None; }
            ((a as i64).wrapping_rem(b as i64)) as u64
        }
        And => a & b,
        Or => a | b,
        Xor => a ^ b,
        Shl => if b >= 64 { 0 } else { a << b },
        Shr => if b >= 64 { 0 } else { a >> b },
        Sar => {
            let sh = if b >= 64 { 63 } else { b };
            ((a as i64) >> sh) as u64
        }
        Eq => (a == b) as u64,
        Ne => (a != b) as u64,
        Ult => (a < b) as u64,
        Ule => (a <= b) as u64,
        Ugt => (a > b) as u64,
        Uge => (a >= b) as u64,
        Slt => (((a as i64) < (b as i64)) as u64),
        Sle => (((a as i64) <= (b as i64)) as u64),
        Sgt => (((a as i64) > (b as i64)) as u64),
        Sge => (((a as i64) >= (b as i64)) as u64),
    };
    Some(r)
}

// ---- The differential driver ----------------------------------------------

/// A divergence between the lifted IR and Unicorn for one (instruction, state).
#[derive(Debug)]
pub struct Mismatch {
    pub asm: String,
    pub bytes: Vec<u8>,
    pub field: String, // "eax", "CF", …
    pub lifted: u64,
    pub oracle: u64,
}

fn decode(bytes: &[u8]) -> crate::disasm::Insn {
    use iced_x86::{Decoder, DecoderOptions, Instruction};
    let mut dec = Decoder::with_ip(32, bytes, CODE_ADDR, DecoderOptions::NONE);
    let mut raw = Instruction::default();
    dec.decode_out(&mut raw);
    crate::disasm::Insn {
        address: CODE_ADDR,
        len: raw.len(),
        text: format!("{}", raw),
        flow: crate::disasm::Flow::Fallthrough,
        target: None,
        call_name: None,
        raw,
    }
}

/// Does any statement contain a `Sar` (arithmetic right shift)? Such results are
/// width-dependent and the bare-u64 interpreter cannot reproduce them faithfully,
/// so their register value is not scored (flags still are).
fn stmts_contain_sar(stmts: &[Stmt]) -> bool {
    fn ex(e: &Expr) -> bool {
        match e {
            Expr::Binary(BinOp::Sar, _, _) => true,
            Expr::Binary(_, a, b) => ex(a) || ex(b),
            Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => ex(x),
            Expr::Select { cond, then_, else_ } => ex(cond) || ex(then_) || ex(else_),
            Expr::Load { addr, .. } => ex(addr),
            _ => false,
        }
    }
    stmts.iter().any(|s| match s {
        Stmt::Set { expr, .. } => ex(expr),
        Stmt::Store { addr, value, .. } => ex(addr) || ex(value),
        Stmt::CallStmt(e) => ex(e),
        _ => false,
    })
}

/// Which of CF/ZF/SF/OF the lifted statements assign (only those are compared).
fn flags_written(stmts: &[Stmt]) -> Vec<FlagKind> {
    let mut v = Vec::new();
    for s in stmts {
        if let Stmt::Set { dst: Location::Flag(f), .. } = s {
            if matches!(f, FlagKind::Cf | FlagKind::Zf | FlagKind::Sf | FlagKind::Of) && !v.contains(f) {
                v.push(*f);
            }
        }
    }
    v
}

/// Run `iters` random states through one instruction, returning any mismatches.
fn diff_one(bytes: &[u8], iters: u32, seed: &mut u64) -> Result<Vec<Mismatch>, String> {
    let insn = decode(bytes);
    if insn.len != bytes.len() {
        return Err(format!("decode consumed {}/{} bytes", insn.len, bytes.len()));
    }
    let asm = insn.text.clone();
    let stmts = crate::ir::lift::lift(&insn, 32);
    // Safety valve / unmodelled: skip (it is correctly *not* claimed correct).
    if stmts.iter().any(|s| matches!(s, Stmt::Asm(_))) {
        return Ok(Vec::new());
    }
    let cmp_flags = flags_written(&stmts);
    // The interpreter works in 64-bit and cannot faithfully reproduce an
    // *arithmetic* right shift at the operand width (a 32-bit value sign-extends
    // from bit 31, not bit 63 — the C backend gets this right via Ty tracking,
    // but the bare-u64 interpreter would not). So when the lift uses `Sar`, do
    // not score the register *value* (flags are width-explicit and still scored).
    let has_sar = stmts_contain_sar(&stmts);

    let mut uc: *mut uc_engine = std::ptr::null_mut();
    unsafe {
        if uc_open(UC_ARCH_X86, UC_MODE_32, &mut uc) != 0 {
            return Err("uc_open failed".into());
        }
        uc_mem_map(uc, CODE_ADDR, 0x1000, UC_PROT_ALL);
        uc_mem_map(uc, STACK_ADDR & !0xfff, 0x4000, UC_PROT_ALL);
        uc_mem_write(uc, CODE_ADDR, bytes.as_ptr() as *const c_void, bytes.len());
    }

    let mut out = Vec::new();
    for _ in 0..iters {
        // xorshift RNG for reproducibility.
        let mut next = || {
            let mut x = *seed;
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            *seed = x;
            x
        };
        let mut st = CpuState { regs: [0; 8], flags: HashMap::new() };
        for r in 0..8 {
            st.regs[r] = (next() as u32) as u64;
        }
        st.regs[4] = STACK_ADDR; // ESP must stay valid
        for f in [FlagKind::Cf, FlagKind::Zf, FlagKind::Sf, FlagKind::Of, FlagKind::Pf, FlagKind::Af] {
            st.flags.insert(f, next() & 1);
        }

        // ---- interpret the lifted IR ----
        let mut interp = Interp::new(&st);
        let mut modelled = true;
        for s in &stmts {
            if interp.exec(s).is_none() {
                modelled = false;
                break;
            }
        }
        if !modelled {
            // The interpreter can't model this instruction; do not score it.
            return Ok(out);
        }

        // ---- run Unicorn from the same state ----
        let mut eflags: u32 = 0x2; // reserved bit
        for (f, bitpos) in [
            (FlagKind::Cf, 0u32), (FlagKind::Pf, 2), (FlagKind::Af, 4),
            (FlagKind::Zf, 6), (FlagKind::Sf, 7), (FlagKind::Of, 11),
        ] {
            if st.flags.get(&f).copied().unwrap_or(0) != 0 {
                eflags |= 1 << bitpos;
            }
        }
        unsafe {
            for r in 0..8 {
                let v = st.regs[r] as u32;
                uc_reg_write(uc, UC_GP[r], &v as *const u32 as *const c_void);
            }
            uc_reg_write(uc, UC_X86_REG_EFLAGS, &eflags as *const u32 as *const c_void);
            let rc = uc_emu_start(uc, CODE_ADDR, CODE_ADDR + bytes.len() as u64, 0, 1);
            if rc != 0 {
                continue; // emulation fault (e.g. div by zero) — skip this state
            }
        }
        let mut uc_regs = [0u32; 8];
        let mut uc_eflags: u32 = 0;
        unsafe {
            for r in 0..8 {
                uc_reg_read(uc, UC_GP[r], &mut uc_regs[r] as *mut u32 as *mut c_void);
            }
            uc_reg_read(uc, UC_X86_REG_EFLAGS, &mut uc_eflags as *mut u32 as *mut c_void);
        }

        // ---- compare GP regs (32-bit) ----
        const NAMES: [&str; 8] = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"];
        for r in 0..8 {
            if has_sar {
                break;
            }
            let lifted = interp.regs[r] as u32;
            if lifted != uc_regs[r] {
                out.push(Mismatch {
                    asm: asm.clone(),
                    bytes: bytes.to_vec(),
                    field: NAMES[r].into(),
                    lifted: lifted as u64,
                    oracle: uc_regs[r] as u64,
                });
            }
        }
        // ---- compare the flags the lift claims to set ----
        for f in &cmp_flags {
            let lifted = interp.flags.get(f).copied().unwrap_or(0);
            let oracle = ((uc_eflags >> flag_bit(*f)) & 1) as u64;
            if lifted != oracle {
                out.push(Mismatch {
                    asm: asm.clone(),
                    bytes: bytes.to_vec(),
                    field: format!("{:?}", f),
                    lifted,
                    oracle,
                });
            }
        }
        if out.len() > 20 {
            break; // enough evidence
        }
    }
    unsafe { uc_close(uc); }
    Ok(out)
}

/// The curated instruction corpus: integer arithmetic / logic / shift / cmov /
/// setcc with register and sign-extended-immediate operands across 8/16/32-bit
/// widths — exactly where the flag/width bugs live.
fn corpus() -> Vec<Vec<u8>> {
    vec![
        // r/m32, r32 arithmetic & logic
        vec![0x01, 0xc8], // add eax, ecx
        vec![0x29, 0xc8], // sub eax, ecx
        vec![0x11, 0xc8], // adc eax, ecx
        vec![0x19, 0xc8], // sbb eax, ecx
        vec![0x39, 0xc8], // cmp eax, ecx
        vec![0x21, 0xc8], // and eax, ecx
        vec![0x09, 0xc8], // or  eax, ecx
        vec![0x31, 0xc8], // xor eax, ecx
        vec![0x85, 0xc8], // test eax, ecx
        // r/m32, sign-extended imm8 (the sub_flags / add_flags case)
        vec![0x83, 0xc0, 0xc1], // add eax, -63
        vec![0x83, 0xe8, 0xc1], // sub eax, -63
        vec![0x83, 0xf8, 0xc1], // cmp eax, -63
        vec![0x83, 0xd0, 0xc1], // adc eax, -63
        vec![0x83, 0xd8, 0xc1], // sbb eax, -63
        vec![0x83, 0xe0, 0x0f], // and eax, 15
        // shifts
        vec![0xd3, 0xe0], // shl eax, cl
        vec![0xd3, 0xe8], // shr eax, cl
        vec![0xd3, 0xf8], // sar eax, cl
        vec![0xd1, 0xe0], // shl eax, 1
        vec![0xc1, 0xe8, 0x05], // shr eax, 5
        vec![0xc1, 0xf8, 0x05], // sar eax, 5
        // double-precision shifts (shld/shrd)
        vec![0x0f, 0xa5, 0xc8], // shld eax, ecx, cl
        vec![0x0f, 0xad, 0xc8], // shrd eax, ecx, cl
        // unary
        vec![0xff, 0xc0], // inc eax
        vec![0xff, 0xc8], // dec eax
        vec![0xf7, 0xd8], // neg eax
        vec![0xf7, 0xd0], // not eax
        // cmovcc / setcc (flag-condition consumers)
        vec![0x0f, 0x4c, 0xc1], // cmovl eax, ecx
        vec![0x0f, 0x46, 0xc1], // cmovbe eax, ecx
        vec![0x0f, 0x44, 0xc1], // cmove eax, ecx
        vec![0x0f, 0x47, 0xc1], // cmova eax, ecx
        vec![0x0f, 0x9c, 0xc0], // setl al
        vec![0x0f, 0x96, 0xc0], // setbe al
        vec![0x0f, 0x94, 0xc0], // sete al
        // 16-bit (66 prefix) and 8-bit widths
        vec![0x66, 0x01, 0xc8], // add ax, cx
        vec![0x66, 0x29, 0xc8], // sub ax, cx
        vec![0x00, 0xc8], // add al, cl
        vec![0x28, 0xc8], // sub al, cl
        vec![0x38, 0xc8], // cmp al, cl
    ]
}

/// Run the whole corpus; returns every mismatch found.
pub fn run(iters_per_insn: u32) -> Result<Vec<Mismatch>, String> {
    let mut seed: u64 = 0x9e3779b97f4a7c15;
    let mut all = Vec::new();
    for bytes in corpus() {
        all.extend(diff_one(&bytes, iters_per_insn, &mut seed)?);
    }
    Ok(all)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lifter_matches_unicorn_over_random_states() {
        let mismatches = run(2000).expect("harness setup");
        if !mismatches.is_empty() {
            // Dedup by (asm, field) so distinct bugs are visible.
            let mut seen: std::collections::BTreeMap<(String, String), (u64, u64)> =
                std::collections::BTreeMap::new();
            for m in &mismatches {
                seen.entry((m.asm.clone(), m.field.clone())).or_insert((m.lifted, m.oracle));
            }
            let mut msg = format!(
                "{} mismatches, {} distinct (asm,field):\n",
                mismatches.len(),
                seen.len()
            );
            for ((asm, field), (l, o)) in &seen {
                msg.push_str(&format!("  [{asm}] {field}: lifted={l:#x} oracle={o:#x}\n"));
            }
            panic!("{msg}");
        }
    }
}
