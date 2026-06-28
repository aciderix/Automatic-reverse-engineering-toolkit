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

use crate::ir::types::{BinOp, CallTarget, Expr, FlagKind, Location, Stmt, Ty, UnOp};
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
const UC_X86_REG_EIP: c_int = 26;
const UC_X86_REG_ESI: c_int = 29;
const UC_X86_REG_ESP: c_int = 30;
const UC_X86_REG_XMM0: c_int = 122; // XMMn = 122 + n
const UC_X86_REG_MXCSR: c_int = 249;

#[link(name = "unicorn")]
extern "C" {
    fn uc_open(arch: c_int, mode: c_int, uc: *mut *mut uc_engine) -> c_int;
    fn uc_close(uc: *mut uc_engine) -> c_int;
    fn uc_mem_map(uc: *mut uc_engine, address: u64, size: usize, perms: u32) -> c_int;
    fn uc_mem_write(uc: *mut uc_engine, address: u64, bytes: *const c_void, size: usize) -> c_int;
    fn uc_mem_read(uc: *mut uc_engine, address: u64, bytes: *mut c_void, size: usize) -> c_int;
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
const DATA_ADDR: u64 = 0x30_0000; // a scratch data page for memory operands
const DATA_SIZE: usize = 0x1000;
const DATA_PTR: u64 = DATA_ADDR + 0x80; // memory-operand base (room for small ±disp)

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
    xmm: [[u64; 2]; 8], // [n] = [low64, high64] of XMMn
}

// ---- IR interpreter (integer subset + SSE scalar) -------------------------

struct Interp {
    regs: [u64; 8],
    flags: HashMap<FlagKind, u64>,
    temps: HashMap<u32, u64>,
    mem: Vec<u8>,       // mirror of the scratch data page [DATA_ADDR, DATA_ADDR+DATA_SIZE)
    xmm: [[u64; 2]; 8], // XMM lanes, mirroring CpuState
}

/// Byte width of an integer Ty (1/2/4/8), or `None` for non-integer.
fn ty_bytes(t: &Ty) -> Option<usize> {
    match t {
        Ty::Int { bits, .. } => Some((*bits as usize).div_ceil(8)),
        Ty::Bool => Some(1),
        _ => None,
    }
}

impl Interp {
    fn new(s: &CpuState, mem: Vec<u8>) -> Self {
        Interp {
            regs: s.regs,
            flags: s.flags.clone(),
            temps: HashMap::new(),
            mem,
            xmm: s.xmm,
        }
    }

    /// Read `n` little-endian bytes from the scratch page, or `None` if out of it.
    fn mem_read(&self, addr: u64, n: usize) -> Option<u64> {
        let off = addr.checked_sub(DATA_ADDR)? as usize;
        if off + n > self.mem.len() {
            return None;
        }
        let mut v = 0u64;
        for i in 0..n {
            v |= (self.mem[off + i] as u64) << (8 * i);
        }
        Some(v)
    }

    fn mem_write(&mut self, addr: u64, n: usize, val: u64) -> Option<()> {
        let off = addr.checked_sub(DATA_ADDR)? as usize;
        if off + n > self.mem.len() {
            return None;
        }
        for i in 0..n {
            self.mem[off + i] = (val >> (8 * i)) as u8;
        }
        Some(())
    }

    /// Evaluate an expression to a 64-bit value, or `None` if it uses something
    /// the integer interpreter does not model (width-bearing sign/zero-extension
    /// or truncate, a call, an SSA node, an out-of-page load, …).
    fn eval(&self, e: &Expr) -> Option<u64> {
        Some(match e {
            Expr::Const(v, _) => *v as u64,
            Expr::Read(loc) => match loc {
                Location::Reg(r) if (r.0 as usize) < 8 => self.regs[r.0 as usize],
                // XMM lanes: low half is RegId(16+n), high half RegId(64+n)
                // (matching the lifter's `xmm_lo`/`xmm_hi`).
                Location::Reg(r) if (16..24).contains(&r.0) => self.xmm[(r.0 - 16) as usize][0],
                Location::Reg(r) if (64..72).contains(&r.0) => self.xmm[(r.0 - 64) as usize][1],
                Location::Flag(f) => self.flags.get(f).copied().unwrap_or(0),
                Location::Temp(t) => self.temps.get(t).copied().unwrap_or(0),
                _ => return None,
            },
            Expr::Load { addr, ty } => {
                let a = self.eval(addr)?;
                self.mem_read(a, ty_bytes(ty)?)?
            }
            Expr::Unary(op, x) => {
                let a = self.eval(x)?;
                match op {
                    UnOp::Neg => 0u64.wrapping_sub(a),
                    UnOp::Not => !a,
                    // width-dependent: not modelled here
                    UnOp::SignExtend | UnOp::ZeroExtend | UnOp::Truncate => return None,
                }
            }
            // Arithmetic right shift is width-dependent: sign-extend from the
            // operand width before shifting. Mirror the C backend's `signed_cast`,
            // which infers the width from an `And(x, mask)` wrapper (the lifter
            // masks register operands to their width), defaulting to 64.
            Expr::Binary(BinOp::Sar, a, b) => {
                let av = sext(self.eval(a)?, signed_width(a));
                let cv = self.eval(b)?;
                let sh = if cv >= 64 { 63 } else { cv };
                ((av as i64) >> sh) as u64
            }
            // Signed division/remainder are width-dependent the same way: the
            // lifter masks each operand to its width, and the C backend's
            // `signed_cast` sign-extends from that width before dividing. Mirror
            // it so the interpreter matches the emitted C (and Unicorn). A zero
            // divisor is a #DE trap on the hardware, so `None` drops that state.
            Expr::Binary(op @ (BinOp::SDiv | BinOp::SMod), a, b) => {
                let av = sext(self.eval(a)?, signed_width(a)) as i64;
                let bv = sext(self.eval(b)?, signed_width(b)) as i64;
                if bv == 0 {
                    return None;
                }
                (if *op == BinOp::SDiv { av.wrapping_div(bv) } else { av.wrapping_rem(bv) }) as u64
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
            // SSE scalar float helpers (`__fp_*`): evaluate the bit-pattern
            // operands with host f64/f32 arithmetic, which on an x86-64 host is
            // the same IEEE-754 hardware Unicorn's softfloat mirrors; plus the
            // 32-bit integer-division helpers. Any other call (`__ps_*`,
            // `__pi_*`, `__x87_*`, …) is unmodelled -> skip.
            Expr::Call { target: CallTarget::Named(name), args, .. } => {
                let mut vals = Vec::with_capacity(args.len());
                for a in args {
                    vals.push(self.eval(a)?);
                }
                helper_call(name, &vals)?
            }
            // Memory, addresses, other calls, SSA forms: not modelled -> skip.
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
                    Location::Reg(r) if (16..24).contains(&r.0) => self.xmm[(r.0 - 16) as usize][0] = v,
                    Location::Reg(r) if (64..72).contains(&r.0) => self.xmm[(r.0 - 64) as usize][1] = v,
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
            Stmt::Store { addr, value, ty } => {
                let a = self.eval(addr)?;
                let v = self.eval(value)?;
                self.mem_write(a, ty_bytes(ty)?, v)
            }
            Stmt::Nop => Some(()),
            // A branch, call, asm, … — not part of a single-instruction
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

/// Evaluate one of the C backend's runtime helpers on bit-pattern arguments,
/// returning the bit pattern of the result (`None` for a helper this interpreter
/// does not model — e.g. the packed `__ps_*` / integer-SIMD forms — or for a
/// state where the helper *traps*, like an integer-division #DE).
///
/// The `__fp_*` arithmetic mirrors `runtime` C exactly (see `src/emit/mod.rs`): host
/// f64/f32 operations on an x86-64 host are the same IEEE-754 SSE instructions
/// Unicorn emulates, so results match bit-for-bit including NaN payloads and
/// rounding. Float→int conversions reproduce x86 `cvtt` truncation toward zero
/// with the "integer indefinite" (0x80000000 / 0x8000000000000000) the hardware
/// yields on overflow/NaN — *not* Rust's saturating `as`.
fn helper_call(name: &str, a: &[u64]) -> Option<u64> {
    // 32-bit integer-division helpers (the lifter routes div/idiv through these
    // so the C reproduces #DE). Return None on any fault state — zero divisor,
    // quotient overflow, INT64_MIN/-1 — because the helper *traps* there (no
    // value), exactly the states where Unicorn faults and the harness skips.
    match name {
        "__ix_udiv32" => {
            let d = a[1] & 0xffff_ffff;
            if d == 0 {
                return None;
            }
            let q = a[0] / d;
            return if q > 0xffff_ffff { None } else { Some(q) };
        }
        "__ix_umod32" => {
            let d = a[1] & 0xffff_ffff;
            if d == 0 || a[0] / d > 0xffff_ffff {
                return None;
            }
            return Some(a[0] % d);
        }
        "__ix_idiv32" | "__ix_imod32" => {
            let dd = a[1] as u32 as i32 as i64;
            if dd == 0 || (dd == -1 && a[0] == 0x8000_0000_0000_0000) {
                return None;
            }
            let n = a[0] as i64;
            let q = n / dd;
            if q > i32::MAX as i64 || q < i32::MIN as i64 {
                return None;
            }
            return Some((if name == "__ix_idiv32" { q } else { n % dd }) as u64);
        }
        // Parity flag: PF = 1 iff the low byte has an even number of set bits.
        "__ix_pf" => return Some(((a[0] & 0xff).count_ones() % 2 == 0) as u64),
        _ => {}
    }
    let g64 = f64::from_bits;
    let g32 = |b: u64| f32::from_bits(b as u32);
    let p64 = |d: f64| d.to_bits();
    let p32 = |f: f32| f.to_bits() as u64;
    Some(match name {
        // double / float arithmetic
        "__fp_add64" => p64(g64(a[0]) + g64(a[1])),
        "__fp_sub64" => p64(g64(a[0]) - g64(a[1])),
        "__fp_mul64" => p64(g64(a[0]) * g64(a[1])),
        "__fp_div64" => p64(g64(a[0]) / g64(a[1])),
        "__fp_add32" => p32(g32(a[0]) + g32(a[1])),
        "__fp_sub32" => p32(g32(a[0]) - g32(a[1])),
        "__fp_mul32" => p32(g32(a[0]) * g32(a[1])),
        "__fp_div32" => p32(g32(a[0]) / g32(a[1])),
        // int -> float (exact for i32 into f64; rounds otherwise, round-to-even)
        "__fp_i32_64" => p64((a[0] as u32 as i32) as f64),
        "__fp_i64_64" => p64((a[0] as i64) as f64),
        "__fp_i32_32" => p32((a[0] as u32 as i32) as f32),
        "__fp_i64_32" => p32((a[0] as i64) as f32),
        // float <-> float
        "__fp_32_64" => p64(g32(a[0]) as f64),
        "__fp_64_32" => p32(g64(a[0]) as f32),
        // float -> int (truncating; x86 indefinite on overflow/NaN)
        "__fp_64_i32" => cvtt_i32(g64(a[0])),
        "__fp_32_i32" => cvtt_i32(g32(a[0]) as f64),
        "__fp_64_i64" => cvtt_i64(g64(a[0])),
        "__fp_32_i64" => cvtt_i64(g32(a[0]) as f64),
        // ordered comparisons (comiss/comisd predicates)
        "__fp_lt64" => (g64(a[0]) < g64(a[1])) as u64,
        "__fp_eq64" => (g64(a[0]) == g64(a[1])) as u64,
        "__fp_un64" => (g64(a[0]).is_nan() || g64(a[1]).is_nan()) as u64,
        "__fp_lt32" => (g32(a[0]) < g32(a[1])) as u64,
        "__fp_eq32" => (g32(a[0]) == g32(a[1])) as u64,
        "__fp_un32" => (g32(a[0]).is_nan() || g32(a[1]).is_nan()) as u64,
        _ => return None,
    })
}

/// x86 `cvttsd2si`/`cvttss2si` to 32-bit: truncate toward zero, and on a value
/// out of the int32 range (or NaN) return the "integer indefinite" 0x80000000,
/// sign-extended to 64 bits exactly as the C helper `(uint64_t)(int64_t)(int32_t)`.
fn cvtt_i32(d: f64) -> u64 {
    let t = d.trunc();
    if d.is_nan() || t > i32::MAX as f64 || t < i32::MIN as f64 {
        (i32::MIN as i64) as u64
    } else {
        (t as i32 as i64) as u64
    }
}

/// x86 `cvttsd2si`/`cvttss2si` to 64-bit: as `cvtt_i32` but for the int64 range
/// (indefinite is 0x8000000000000000). The positive bound is 2^63 (i64::MAX is
/// not representable in f64 and rounds up to it).
fn cvtt_i64(d: f64) -> u64 {
    let t = d.trunc();
    if d.is_nan() || t >= 9223372036854775808.0 || t < -9223372036854775808.0 {
        i64::MIN as u64
    } else {
        (t as i64) as u64
    }
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

/// The width (in bits) to sign-extend from for an arithmetic shift / signed
/// compare, inferred from an `And(x, mask)` wrapper exactly as the C backend's
/// `signed_cast` does (0xff→8, 0xffff→16, 0xffffffff→32), else the full 64.
fn signed_width(e: &Expr) -> u32 {
    if let Expr::Binary(BinOp::And, _, m) = e {
        if let Expr::Const(c, _) = m.as_ref() {
            return match *c {
                0xff => 8,
                0xffff => 16,
                0xffffffff => 32,
                _ => 64,
            };
        }
    }
    64
}

/// Sign-extend the low `w` bits of `v` to 64 bits (no-op for `w >= 64`).
fn sext(v: u64, w: u32) -> u64 {
    if w >= 64 {
        v
    } else if (v >> (w - 1)) & 1 == 1 {
        v | !((1u64 << w) - 1)
    } else {
        v & ((1u64 << w) - 1)
    }
}

/// Whether the lifted statements touch the SSE/float world — an XMM lane or an
/// `__fp_*` helper. Only then does the harness seed and compare XMM registers,
/// so the integer corpus pays no extra Unicorn traffic.
fn fp_used(stmts: &[Stmt]) -> bool {
    fn loc_fp(l: &Location) -> bool {
        matches!(l, Location::Reg(r) if (16..24).contains(&r.0) || (64..72).contains(&r.0))
    }
    fn expr_fp(e: &Expr) -> bool {
        match e {
            Expr::Read(l) => loc_fp(l),
            Expr::Load { addr, .. } => expr_fp(addr),
            Expr::Unary(_, x) => expr_fp(x),
            Expr::Binary(_, a, b) => expr_fp(a) || expr_fp(b),
            Expr::Cast { expr, .. } => expr_fp(expr),
            Expr::Select { cond, then_, else_ } => {
                expr_fp(cond) || expr_fp(then_) || expr_fp(else_)
            }
            Expr::Call { target: CallTarget::Named(n), args, .. } => {
                n.starts_with("__fp_") || args.iter().any(expr_fp)
            }
            Expr::Call { args, .. } => args.iter().any(expr_fp),
            _ => false,
        }
    }
    stmts.iter().any(|s| match s {
        Stmt::Set { dst, expr } => loc_fp(dst) || expr_fp(expr),
        Stmt::Store { addr, value, .. } => expr_fp(addr) || expr_fp(value),
        _ => false,
    })
}

/// Which of CF/ZF/SF/OF the lifted statements assign (only those are compared).
fn flags_written(stmts: &[Stmt]) -> Vec<FlagKind> {
    let mut v = Vec::new();
    for s in stmts {
        if let Stmt::Set { dst: Location::Flag(f), .. } = s {
            if matches!(f, FlagKind::Cf | FlagKind::Zf | FlagKind::Sf | FlagKind::Of | FlagKind::Pf | FlagKind::Af) && !v.contains(f) {
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
    let fp = fp_used(&stmts);
    // If the instruction has a memory operand, point its base register at the
    // scratch page so loads/stores land in mapped memory. (Index-register forms
    // can wander out of the page; those loads return None and the case is
    // skipped — never a false positive.)
    let mem_base = gp_index(insn.raw.memory_base());

    let mut uc: *mut uc_engine = std::ptr::null_mut();
    unsafe {
        if uc_open(UC_ARCH_X86, UC_MODE_32, &mut uc) != 0 {
            return Err("uc_open failed".into());
        }
        uc_mem_map(uc, CODE_ADDR, 0x1000, UC_PROT_ALL);
        uc_mem_map(uc, STACK_ADDR & !0xfff, 0x4000, UC_PROT_ALL);
        uc_mem_map(uc, DATA_ADDR, DATA_SIZE, UC_PROT_ALL);
        uc_mem_write(uc, CODE_ADDR, bytes.as_ptr() as *const c_void, bytes.len());
        // Default MXCSR (round-to-nearest, no FTZ/DAZ) so SSE rounding matches
        // the host f64/f32 ops the interpreter uses.
        let mxcsr: u32 = 0x1f80;
        uc_reg_write(uc, UC_X86_REG_MXCSR, &mxcsr as *const u32 as *const c_void);
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
        let mut st = CpuState { regs: [0; 8], flags: HashMap::new(), xmm: [[0; 2]; 8] };
        for r in 0..8 {
            st.regs[r] = (next() as u32) as u64;
        }
        st.regs[4] = STACK_ADDR; // ESP must stay valid
        if let Some(b) = mem_base {
            st.regs[b] = DATA_PTR; // memory-operand base lands in the scratch page
        }
        for f in [FlagKind::Cf, FlagKind::Zf, FlagKind::Sf, FlagKind::Of, FlagKind::Pf, FlagKind::Af] {
            st.flags.insert(f, next() & 1);
        }
        // Seed XMM lanes with a mix of float classes: raw bit patterns (NaN/inf/
        // huge), exact integer-valued doubles, and fractions — so arithmetic,
        // conversions, and comparisons all get in-range and edge inputs.
        if fp {
            for n in 0..8 {
                st.xmm[n][0] = match next() % 4 {
                    0 => next(),
                    1 => ((next() as i32) as f64).to_bits(),
                    2 => (((next() as i32) as f64) / 64.0).to_bits(),
                    _ => next(),
                };
                st.xmm[n][1] = next();
            }
        }
        // Fresh random contents for the scratch page, mirrored into both engines.
        let mut page = vec![0u8; DATA_SIZE];
        for b in page.iter_mut() {
            *b = next() as u8;
        }
        unsafe {
            uc_mem_write(uc, DATA_ADDR, page.as_ptr() as *const c_void, DATA_SIZE);
        }

        // ---- interpret the lifted IR ----
        let mut interp = Interp::new(&st, page.clone());
        let mut modelled = true;
        for s in &stmts {
            if interp.exec(s).is_none() {
                modelled = false;
                break;
            }
        }
        if !modelled {
            // This state touched something the interpreter does not model — an
            // unmodelled construct (then every state of the instruction is
            // skipped, e.g. x87) or a per-state trap (a zero divisor, an
            // out-of-page load). Either way, do not score it. Never a false
            // positive; at worst the case is silently not exercised.
            continue;
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
            if fp {
                for n in 0..8 {
                    uc_reg_write(uc, UC_X86_REG_XMM0 + n as c_int, st.xmm[n].as_ptr() as *const c_void);
                }
            }
            let rc = uc_emu_start(uc, CODE_ADDR, CODE_ADDR + bytes.len() as u64, 0, 1);
            if rc != 0 {
                continue; // emulation fault (e.g. div by zero) — skip this state
            }
            // The instruction pointer is the reliable retirement signal: on a
            // CPU fault (a div #DE from a zero divisor or a quotient that
            // overflows the destination) Unicorn rolls the instruction back and
            // leaves EIP at the start — and does not always surface that as a
            // non-zero `rc`. If EIP did not advance past the instruction, the
            // state faulted on the hardware, so drop it rather than score the
            // interpreter's wrapped result against Unicorn's rolled-back regs.
            let mut eip: u32 = 0;
            uc_reg_read(uc, UC_X86_REG_EIP, &mut eip as *mut u32 as *mut c_void);
            if eip != (CODE_ADDR + bytes.len() as u64) as u32 {
                continue;
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
        // ---- compare XMM registers (SSE scalar/float instructions) ----
        if fp {
            for n in 0..8 {
                let mut uc_xmm = [0u64; 2];
                unsafe {
                    uc_reg_read(uc, UC_X86_REG_XMM0 + n as c_int, uc_xmm.as_mut_ptr() as *mut c_void);
                }
                for (half, tag) in [(0usize, "lo"), (1, "hi")] {
                    if interp.xmm[n][half] != uc_xmm[half] {
                        out.push(Mismatch {
                            asm: asm.clone(),
                            bytes: bytes.to_vec(),
                            field: format!("xmm{n}.{tag}"),
                            lifted: interp.xmm[n][half],
                            oracle: uc_xmm[half],
                        });
                    }
                }
            }
        }
        // ---- compare the scratch page (for memory stores) ----
        if mem_base.is_some() {
            let mut uc_page = vec![0u8; DATA_SIZE];
            unsafe {
                uc_mem_read(uc, DATA_ADDR, uc_page.as_mut_ptr() as *mut c_void, DATA_SIZE);
            }
            if uc_page != interp.mem {
                let i = (0..DATA_SIZE).find(|&i| uc_page[i] != interp.mem[i]).unwrap();
                out.push(Mismatch {
                    asm: asm.clone(),
                    bytes: bytes.to_vec(),
                    field: format!("mem[{i}]"),
                    lifted: interp.mem[i] as u64,
                    oracle: uc_page[i] as u64,
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

/// The RegId index (0=rax..7=rdi) of an iced GP register, else None.
fn gp_index(r: iced_x86::Register) -> Option<usize> {
    use iced_x86::Register::*;
    Some(match r.full_register() {
        RAX => 0,
        RCX => 1,
        RDX => 2,
        RBX => 3,
        RSP => 4,
        RBP => 5,
        RSI => 6,
        RDI => 7,
        _ => return Option::None,
    })
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
        vec![0x66, 0x39, 0xc8], // cmp ax, cx
        vec![0x66, 0x83, 0xe8, 0xc1], // sub ax, -63
        vec![0x66, 0xc1, 0xf8, 0x03], // sar ax, 3
        vec![0x66, 0xd3, 0xe0], // shl ax, cl
        vec![0x00, 0xc8], // add al, cl
        vec![0x28, 0xc8], // sub al, cl
        vec![0x38, 0xc8], // cmp al, cl
        vec![0x10, 0xc8], // adc al, cl
        vec![0x18, 0xc8], // sbb al, cl
        vec![0xc0, 0xf8, 0x02], // sar al, 2
        vec![0xfe, 0xc0], // inc al
        vec![0xfe, 0xc8], // dec al
        vec![0xf6, 0xd8], // neg al
        vec![0x66, 0x11, 0xc8], // adc cx, cx (16-bit carry-in)
        vec![0x66, 0x19, 0xc8], // sbb cx, cx
        // add with imm32
        vec![0x05, 0xc1, 0xff, 0xff, 0xff], // add eax, 0xffffffc1
        vec![0x2d, 0x39, 0x30, 0x00, 0x00], // sub eax, 0x3039
        // imul (2-operand and 3-operand), 1-operand mul/imul (edx:eax)
        vec![0x0f, 0xaf, 0xc1], // imul eax, ecx
        vec![0x6b, 0xc1, 0x07], // imul eax, ecx, 7
        vec![0xf7, 0xe1], // mul ecx   (edx:eax = eax*ecx)
        vec![0xf7, 0xe9], // imul ecx
        // rotates (set CF/OF)
        vec![0xd3, 0xc0], // rol eax, cl
        vec![0xd3, 0xc8], // ror eax, cl
        vec![0xc1, 0xc0, 0x05], // rol eax, 5
        vec![0xc1, 0xc8, 0x05], // ror eax, 5
        // bit test / set / reset / complement (CF = old bit)
        vec![0x0f, 0xa3, 0xc8], // bt  eax, ecx
        vec![0x0f, 0xab, 0xc8], // bts eax, ecx
        vec![0x0f, 0xb3, 0xc8], // btr eax, ecx
        vec![0x0f, 0xbb, 0xc8], // btc eax, ecx
        vec![0x0f, 0xba, 0xe0, 0x05], // bt eax, 5
        // moves with extension
        vec![0x0f, 0xb6, 0xc1], // movzx eax, cl
        vec![0x0f, 0xb7, 0xc1], // movzx eax, cx
        vec![0x0f, 0xbe, 0xc1], // movsx eax, cl
        vec![0x0f, 0xbf, 0xc1], // movsx eax, cx
        // the rest of the cmovcc family (flag-condition consumers)
        vec![0x0f, 0x40, 0xc1], // cmovo
        vec![0x0f, 0x41, 0xc1], // cmovno
        vec![0x0f, 0x42, 0xc1], // cmovb
        vec![0x0f, 0x43, 0xc1], // cmovae
        vec![0x0f, 0x45, 0xc1], // cmovne
        vec![0x0f, 0x48, 0xc1], // cmovs
        vec![0x0f, 0x49, 0xc1], // cmovns
        vec![0x0f, 0x4a, 0xc1], // cmovp
        vec![0x0f, 0x4b, 0xc1], // cmovnp
        vec![0x0f, 0x4d, 0xc1], // cmovge
        vec![0x0f, 0x4e, 0xc1], // cmovle
        vec![0x0f, 0x4f, 0xc1], // cmovg
        // the rest of the setcc family
        vec![0x0f, 0x92, 0xc0], // setb al
        vec![0x0f, 0x95, 0xc0], // setne al
        vec![0x0f, 0x98, 0xc0], // sets al
        vec![0x0f, 0x9d, 0xc0], // setge al
        vec![0x0f, 0x9f, 0xc0], // setg al
        // ---- memory operands ([edi] base, read / read-modify-write / store) ----
        vec![0x03, 0x0f], // add ecx, [edi]
        vec![0x01, 0x0f], // add [edi], ecx   (read-modify-write + store)
        vec![0x2b, 0x0f], // sub ecx, [edi]
        vec![0x29, 0x0f], // sub [edi], ecx
        vec![0x13, 0x0f], // adc ecx, [edi]
        vec![0x1b, 0x0f], // sbb ecx, [edi]
        vec![0x3b, 0x0f], // cmp ecx, [edi]
        vec![0x23, 0x0f], // and ecx, [edi]
        vec![0x0b, 0x0f], // or  ecx, [edi]
        vec![0x33, 0x0f], // xor ecx, [edi]
        vec![0x85, 0x0f], // test [edi], ecx
        vec![0x8b, 0x0f], // mov ecx, [edi]
        vec![0x89, 0x0f], // mov [edi], ecx
        vec![0x03, 0x4f, 0x04], // add ecx, [edi+4]
        vec![0x01, 0x4f, 0xfc], // add [edi-4], ecx
        vec![0xff, 0x07], // inc dword [edi]
        vec![0xff, 0x0f], // dec dword [edi]
        vec![0xf7, 0x1f], // neg dword [edi]
        vec![0xd3, 0x27], // shl dword [edi], cl
        vec![0xd3, 0x2f], // shr dword [edi], cl
        vec![0x66, 0x03, 0x0f], // add cx, [edi]   (16-bit load)
        vec![0x02, 0x0f], // add cl, [edi]   (8-bit load)
        vec![0x00, 0x0f], // add [edi], cl   (8-bit store)
        vec![0x0f, 0xbe, 0x0f], // movsx ecx, byte [edi]
        vec![0x0f, 0xb6, 0x0f], // movzx ecx, byte [edi]
        // ---- misc: lea, xchg, bswap, bit-scan, div/idiv ----
        vec![0x8d, 0x4f, 0x10], // lea ecx, [edi+0x10]
        vec![0x8d, 0x0c, 0x9f], // lea ecx, [edi+ebx*4]
        vec![0x91], // xchg eax, ecx
        vec![0x0f, 0xc8], // bswap eax
        vec![0x0f, 0xbc, 0xc8], // bsf ecx, eax
        vec![0x0f, 0xbd, 0xc8], // bsr ecx, eax
        vec![0x99], // cdq
        vec![0x98], // cwde
        // div/idiv (implicit edx:eax dividend, 32-bit form). Zero-divisor states
        // are dropped (interpreter returns None), and quotient-overflow states
        // (#DE on the hardware) are dropped because Unicorn faults and the state
        // is skipped before comparison. The signed forms exercise the divisor
        // sign-extension the C backend emits via `signed_cast`.
        vec![0xf7, 0xf1], // div  ecx   (edx:eax / ecx)
        vec![0xf7, 0xf9], // idiv ecx
        vec![0xf7, 0xf3], // div  ebx
        vec![0xf7, 0xfb], // idiv ebx
        // ---- SSE scalar float (xmm0/xmm1, the __fp_* helper path) ----
        // double arithmetic (low 64 result, high 64 preserved)
        vec![0xf2, 0x0f, 0x58, 0xc1], // addsd xmm0, xmm1
        vec![0xf2, 0x0f, 0x5c, 0xc1], // subsd xmm0, xmm1
        vec![0xf2, 0x0f, 0x59, 0xc1], // mulsd xmm0, xmm1
        vec![0xf2, 0x0f, 0x5e, 0xc1], // divsd xmm0, xmm1
        // single arithmetic (low 32 result)
        vec![0xf3, 0x0f, 0x58, 0xc1], // addss xmm0, xmm1
        vec![0xf3, 0x0f, 0x5c, 0xc1], // subss xmm0, xmm1
        vec![0xf3, 0x0f, 0x59, 0xc1], // mulss xmm0, xmm1
        vec![0xf3, 0x0f, 0x5e, 0xc1], // divss xmm0, xmm1
        // int <-> float conversions (exercise rounding and #IA indefinite)
        vec![0xf2, 0x0f, 0x2a, 0xc1], // cvtsi2sd xmm0, ecx
        vec![0xf3, 0x0f, 0x2a, 0xc1], // cvtsi2ss xmm0, ecx
        vec![0xf2, 0x0f, 0x2c, 0xc1], // cvttsd2si eax, xmm1
        vec![0xf3, 0x0f, 0x2c, 0xc1], // cvttss2si eax, xmm1
        vec![0xf2, 0x0f, 0x5a, 0xc1], // cvtsd2ss xmm0, xmm1
        vec![0xf3, 0x0f, 0x5a, 0xc1], // cvtss2sd xmm0, xmm1
        // ordered/unordered compares -> EFLAGS (CF/ZF/PF, SF=OF=0)
        vec![0x66, 0x0f, 0x2f, 0xc1], // comisd  xmm0, xmm1
        vec![0x66, 0x0f, 0x2e, 0xc1], // ucomisd xmm0, xmm1
        vec![0x0f, 0x2f, 0xc1],       // comiss  xmm0, xmm1
        vec![0x0f, 0x2e, 0xc1],       // ucomiss xmm0, xmm1
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
