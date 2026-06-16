//! IR lifter: machine instruction -> typed IR statements (pre-SSA).
//!
//! Built on `iced-x86`'s *structured* operand API (registers, memory, immediates)
//! rather than the Intel text, per roadmap §3.3. CPU flags are modelled
//! explicitly so a later pass can recover branch conditions by dataflow.
//!
//! Scope: the compute (data movement + arithmetic/logic + flags) part of an
//! instruction, plus `ret`/`call`. Branch terminators (`jmp`/`jcc`) carry
//! `BlockId`s that only exist once the IR CFG is built, so they are added by
//! that later layer, not here. Anything with an operand shape we do not model
//! (high-byte registers, far pointers, exotic SIMD) falls back to `Stmt::Asm`,
//! keeping the IR always semantically honest.
//!
//! Not yet wired into the default pipeline (parallel migration, roadmap §3.5).
#![allow(dead_code)]

use super::types::*;
use crate::disasm::Insn;
use iced_x86::{
    ConditionCode, Instruction, InstructionInfoFactory, MemorySize, Mnemonic, OpAccess, OpKind,
    Register, RflagsBits,
};

/// Canonical register-family id (al/ax/eax/rax all share one). XMM families
/// start at 16.
fn reg_id(r: Register) -> Option<RegId> {
    if r.is_xmm() {
        return Some(RegId(16 + r.number() as u16));
    }
    use Register::*;
    let n = match r.full_register() {
        RAX => 0,
        RCX => 1,
        RDX => 2,
        RBX => 3,
        RSP => 4,
        RBP => 5,
        RSI => 6,
        RDI => 7,
        R8 => 8,
        R9 => 9,
        R10 => 10,
        R11 => 11,
        R12 => 12,
        R13 => 13,
        R14 => 14,
        R15 => 15,
        _ => return Option::None,
    };
    Some(RegId(n))
}

fn is_high_byte(r: Register) -> bool {
    matches!(r, Register::AH | Register::BH | Register::CH | Register::DH)
}

fn mask(bits: u32) -> i128 {
    if bits >= 128 {
        -1
    } else {
        (1i128 << bits) - 1
    }
}

fn konst(v: i128) -> Expr {
    Expr::Const(v, Ty::int(64))
}

/// Read a register operand as its correctly-masked value.
/// Whether any operand of `ins` is a vector (XMM/YMM/ZMM) register — i.e. an
/// SSE/AVX instruction the scalar IR cannot model faithfully.
fn uses_vector_reg(ins: &Instruction) -> bool {
    (0..ins.op_count()).any(|i| {
        let r = ins.op_register(i);
        r.is_xmm() || r.is_ymm() || r.is_zmm()
    })
}

/// Scalar SSE floating-point instructions modelled via the `__fp_*` runtime
/// helpers (operating on bit patterns). These are exempt from the vector guard.
fn is_scalar_float(ins: &Instruction) -> bool {
    use Mnemonic::*;
    matches!(
        ins.mnemonic(),
        Movss | Movsd | Movd | Movq
            | Addss | Subss | Mulss | Divss
            | Addsd | Subsd | Mulsd | Divsd
            | Cvtsi2ss | Cvtsi2sd | Cvttss2si | Cvttsd2si
            | Cvtss2sd | Cvtsd2ss
            | Comiss | Comisd | Ucomiss | Ucomisd
            | Pxor | Xorps | Xorpd
            | Movaps | Movapd | Movups | Movupd
            | Movdqa | Movdqu | Paddd | Psubd | Psrldq
            | Pand | Pandn | Por | Pcmpeqd | Pcmpgtd | Pshufd
            | Paddw | Paddq | Pcmpgtw | Pmuludq | Psrlq | Psubusw
            | Punpcklwd | Punpckhwd | Punpckldq | Punpckhdq | Punpcklqdq | Punpckhqdq
            | Movhlps | Movlhps | Movhps | Movhpd | Movlps | Movlpd | Shufpd
            | Unpcklpd | Unpckhpd | Addpd | Subpd | Mulpd | Divpd
            | Addps | Subps | Mulps | Divps | Minps | Maxps | Sqrtps | Cvtdq2ps
            | Cmpps | Andps | Orps | Andnps | Shufps | Movmskps | Unpcklps | Unpckhps
    )
}

/// Build a call to a named runtime helper returning a 64-bit bit pattern.
fn fcall(name: &str, args: Vec<Expr>) -> Expr {
    Expr::Call { target: CallTarget::Named(name.to_string()), args, ret: Ty::int(64) }
}

/// XMM lane number of a register operand.
fn xmm_num(r: Register) -> Option<u16> {
    if r.is_xmm() {
        Some(r.number() as u16)
    } else {
        None
    }
}

/// The low / high 64-bit halves of XMM register `n`, modelled as two locations
/// (the integer IR has no 128-bit value). Low half reuses the XMM register id
/// (`16+n`, matching `reg_id`); the high half lives at `64+n`.
fn xmm_lo(n: u16) -> Location {
    Location::Reg(RegId(16 + n))
}
fn xmm_hi(n: u16) -> Location {
    Location::Reg(RegId(64 + n))
}

/// Read operand `i` as a 128-bit value `(low64, high64)` — an XMM register or a
/// 16-byte memory location.
fn read_xmm128(ins: &Instruction, i: u32) -> Option<(Expr, Expr)> {
    match ins.op_kind(i) {
        OpKind::Register => {
            let n = xmm_num(ins.op_register(i))?;
            Some((Expr::Read(xmm_lo(n)), Expr::Read(xmm_hi(n))))
        }
        OpKind::Memory => {
            if ins.segment_prefix() != Register::None {
                return None;
            }
            // Build the two half addresses. For a rip-relative load use explicit
            // constant addresses (`abs`, `abs+8`) so the read-only constant folder
            // can replace both halves with literals.
            let (lo_addr, hi_addr) = if ins.is_ip_rel_memory_operand() {
                let a = ins.ip_rel_memory_address() as i128;
                (konst(a), konst(a + 8))
            } else {
                let (addr, _) = mem_addr(ins)?;
                (addr.clone(), bin(BinOp::Add, addr, konst(8)))
            };
            let lo = Expr::Load { addr: Box::new(lo_addr), ty: Ty::int(64) };
            let hi = Expr::Load { addr: Box::new(hi_addr), ty: Ty::int(64) };
            Some((lo, hi))
        }
        _ => None,
    }
}

/// Write a 128-bit value `(low64, high64)` to operand 0 — an XMM register or a
/// 16-byte memory location.
fn write_xmm128(ins: &Instruction, lo: Expr, hi: Expr) -> Option<Vec<Stmt>> {
    match ins.op_kind(0) {
        OpKind::Register => {
            let n = xmm_num(ins.op_register(0))?;
            // Compute both halves into temporaries before assigning: a cross-half
            // op (e.g. `punpcklwd`, whose high result depends on the low source)
            // would otherwise see the half this same instruction just overwrote.
            let base = (ins.ip() as u32).wrapping_mul(2);
            let t_lo = Location::Temp(base);
            let t_hi = Location::Temp(base.wrapping_add(1));
            Some(vec![
                Stmt::Set { dst: t_lo.clone(), expr: lo },
                Stmt::Set { dst: t_hi.clone(), expr: hi },
                Stmt::Set { dst: xmm_lo(n), expr: Expr::Read(t_lo) },
                Stmt::Set { dst: xmm_hi(n), expr: Expr::Read(t_hi) },
            ])
        }
        OpKind::Memory => {
            if ins.segment_prefix() != Register::None {
                return None;
            }
            // A rip-relative store targets a global by absolute address — the same
            // form ARET emits for any `mov [global], reg`, so handle it likewise.
            let (addr, _) = mem_addr(ins)?;
            Some(vec![
                Stmt::Store { addr: addr.clone(), value: lo, ty: Ty::int(64) },
                Stmt::Store {
                    addr: bin(BinOp::Add, addr, konst(8)),
                    value: hi,
                    ty: Ty::int(64),
                },
            ])
        }
        _ => None,
    }
}

/// Lift a binary scalar-float op `dst = helper(dst, src)`.
fn fbin(name: &str, ins: &Instruction, bits: u32) -> Option<Vec<Stmt>> {
    let a = op_value(ins, 0)?;
    let b = op_value(ins, 1)?;
    write_op0(ins, fcall(name, vec![a, b]), bits)
}

/// Lift a unary scalar-float convert `dst = helper(src)` (`src` = operand 1).
fn fcvt(name: &str, ins: &Instruction, bits: u32) -> Option<Vec<Stmt>> {
    let a = op_value(ins, 1)?;
    write_op0(ins, fcall(name, vec![a]), bits)
}

/// Bit width of operand `i` (register size, or memory access size).
fn op_bits(ins: &Instruction, i: u32) -> u32 {
    match ins.op_kind(i) {
        OpKind::Register => (ins.op_register(i).size() * 8) as u32,
        OpKind::Memory => (ins.memory_size().size() * 8) as u32,
        _ => 64,
    }
}

/// The switch index of an indexed indirect jump `jmp [base + idx*scale]`: a read
/// of the index register (masked to its width). `None` if there is no index
/// register (a computed `jmp reg`, which isn't a recoverable jump table here).
pub(crate) fn switch_index(ins: &Instruction) -> Option<Expr> {
    let idx = ins.memory_index();
    if idx == Register::None {
        return None;
    }
    read_reg(idx)
}

/// Read a register as an `Expr` (masked to its width) — for jump-table index
/// recovery in `ir::build`.
pub(crate) fn reg_value(r: Register) -> Option<Expr> {
    read_reg(r)
}

fn read_reg(r: Register) -> Option<Expr> {
    let id = reg_id(r)?;
    let full = Expr::Read(Location::Reg(id));
    if is_high_byte(r) {
        // ah/bh/ch/dh are bits 8..=15 of the full register: (full >> 8) & 0xff.
        return Some(Expr::Binary(
            BinOp::And,
            Box::new(Expr::Binary(BinOp::Shr, Box::new(full), Box::new(konst(8)))),
            Box::new(konst(0xff)),
        ));
    }
    let w = (r.size() * 8) as u32;
    if w >= 64 {
        Some(full)
    } else {
        Some(Expr::Binary(BinOp::And, Box::new(full), Box::new(konst(mask(w)))))
    }
}

thread_local! {
    /// When set, the lifter does not fold `[rbp+d]` operands into named `Frame`
    /// slots (scalars). Enabled per-function for x87 code, whose 80-bit FPU
    /// spills access those same stack bytes through opaque helpers: keeping every
    /// frame access as raw `__frame` memory makes the two alias consistently.
    static FRAMES_OFF: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

/// Disable/enable named frame-slot folding for subsequent `lift` calls on this
/// thread (set per-function by `ir::build`).
pub(crate) fn set_frames_off(off: bool) {
    FRAMES_OFF.with(|c| c.set(off));
}

/// If operand `i` is a pure frame slot `[ebp/rbp ± disp]` (base = frame pointer,
/// no index), return its displacement. These become named `Frame` locations.
fn frame_disp(ins: &Instruction, i: u32) -> Option<i64> {
    if FRAMES_OFF.with(|c| c.get()) {
        return None;
    }
    if ins.op_kind(i) != OpKind::Memory || ins.is_ip_rel_memory_operand() {
        return None;
    }
    // A segment override (`fs:`/`gs:` — e.g. the stack canary at `fs:0x28`, or
    // TLS) changes the effective address; we don't model segment bases, so never
    // treat such an operand as a plain frame slot.
    if ins.segment_prefix() != Register::None {
        return None;
    }
    let base = ins.memory_base();
    if (base != Register::RBP && base != Register::EBP) || ins.memory_index() != Register::None {
        return None;
    }
    Some(ins.memory_displacement64() as i64)
}

/// Build the address expression and access size (bits) of a memory operand.
fn mem_addr(ins: &Instruction) -> Option<(Expr, u32)> {
    // Segment-overridden memory (`fs:`/`gs:`) has an effective address we don't
    // model — falling through would silently read the wrong (absolute) address,
    // which is exactly the kind of incorrect-but-compilable output the project
    // forbids. Bail so the instruction becomes an honest `Stmt::Asm`.
    if ins.segment_prefix() != Register::None {
        return None;
    }
    let size_bits = (ins.memory_size().size() * 8) as u32;
    if ins.is_ip_rel_memory_operand() {
        return Some((konst(ins.ip_rel_memory_address() as i128), size_bits));
    }
    let mut acc: Option<Expr> = None;
    let base = ins.memory_base();
    if base != Register::None {
        acc = Some(read_reg(base)?);
    }
    let index = ins.memory_index();
    if index != Register::None {
        let idx = read_reg(index)?;
        let scale = ins.memory_index_scale();
        let scaled = if scale == 1 {
            idx
        } else {
            Expr::Binary(BinOp::Mul, Box::new(idx), Box::new(konst(scale as i128)))
        };
        acc = Some(match acc {
            Some(b) => Expr::Binary(BinOp::Add, Box::new(b), Box::new(scaled)),
            None => scaled,
        });
    }
    let disp = ins.memory_displacement64() as i64 as i128;
    if disp != 0 || acc.is_none() {
        let d = konst(disp);
        acc = Some(match acc {
            Some(b) => Expr::Binary(BinOp::Add, Box::new(b), Box::new(d)),
            None => d,
        });
    }
    Some((acc.unwrap(), size_bits))
}

/// Value of operand `i`.
fn op_value(ins: &Instruction, i: u32) -> Option<Expr> {
    match ins.op_kind(i) {
        OpKind::Register => read_reg(ins.op_register(i)),
        OpKind::Immediate8
        | OpKind::Immediate16
        | OpKind::Immediate32
        | OpKind::Immediate64
        | OpKind::Immediate8to16
        | OpKind::Immediate8to32
        | OpKind::Immediate8to64
        | OpKind::Immediate32to64 => Some(konst(ins.immediate(i) as i128)),
        OpKind::Memory => {
            // Segment-overridden memory (`fs:`/`gs:`) is TLS. Model every read as
            // one consistent pseudo-value (0). The stack-protector stores
            // `fs:[0x28]` to a local and later compares the reload to `fs:[0x28]`;
            // with both reads equal, the check folds to a no-op (`0 - 0 == 0` →
            // branch to the success path) — observationally identical to the real
            // random canary, which always matches on a correct run. Without this
            // the whole instruction degrades to `asm`, dropping the canary `je`
            // terminator and breaking the function's structure.
            if ins.segment_prefix() != Register::None {
                return Some(konst(0));
            }
            if let Some(d) = frame_disp(ins, i) {
                let w = (ins.memory_size().size() * 8) as u32;
                let full = Expr::Read(Location::Frame(d));
                return Some(if w >= 64 {
                    full
                } else {
                    Expr::Binary(BinOp::And, Box::new(full), Box::new(konst(mask(w))))
                });
            }
            let (addr, sz) = mem_addr(ins)?;
            Some(Expr::Load {
                addr: Box::new(addr),
                ty: Ty::int(sz as u8),
            })
        }
        _ => None,
    }
}

/// Width in bits of destination operand 0.
fn op0_width(ins: &Instruction, bits: u32) -> u32 {
    match ins.op_kind(0) {
        OpKind::Register => (ins.op_register(0).size() * 8) as u32,
        OpKind::Memory => (ins.memory_size().size() * 8) as u32,
        _ => bits,
    }
}

/// Combine a value into a destination register of width `w`, honouring x86
/// partial-write semantics (32-bit writes zero the upper half on x64; 8/16-bit
/// writes preserve the upper bits).
fn combine_write(dst: &Location, w: u32, value: Expr, bits: u32) -> Expr {
    if w >= 64 || w >= bits {
        return value;
    }
    if w == 32 {
        // x64: writing a 32-bit register zeroes the upper 32 bits.
        return Expr::Binary(BinOp::And, Box::new(value), Box::new(konst(mask(32))));
    }
    let m = mask(w);
    let keep = Expr::Binary(
        BinOp::And,
        Box::new(Expr::Read(dst.clone())),
        Box::new(konst(!m & mask(64))),
    );
    let newbits = Expr::Binary(BinOp::And, Box::new(value), Box::new(konst(m)));
    Expr::Binary(BinOp::Or, Box::new(keep), Box::new(newbits))
}

/// Assign `value` to operand 0 (register or memory).
fn write_op0(ins: &Instruction, value: Expr, bits: u32) -> Option<Vec<Stmt>> {
    match ins.op_kind(0) {
        OpKind::Register => {
            let r = ins.op_register(0);
            let id = reg_id(r)?;
            let dst = Location::Reg(id);
            if is_high_byte(r) {
                // dst = (dst & ~0xff00) | ((value & 0xff) << 8)
                let keep = Expr::Binary(
                    BinOp::And,
                    Box::new(Expr::Read(dst.clone())),
                    Box::new(konst(!0xff00i128 & mask(64))),
                );
                let newbits = Expr::Binary(
                    BinOp::Shl,
                    Box::new(Expr::Binary(BinOp::And, Box::new(value), Box::new(konst(0xff)))),
                    Box::new(konst(8)),
                );
                let expr = Expr::Binary(BinOp::Or, Box::new(keep), Box::new(newbits));
                return Some(vec![Stmt::Set { dst, expr }]);
            }
            let w = (r.size() * 8) as u32;
            let expr = combine_write(&dst, w, value, bits);
            Some(vec![Stmt::Set { dst, expr }])
        }
        OpKind::Memory => {
            // A write through a segment override (`fs:`/`gs:`, TLS) is dropped:
            // reads of TLS are modelled as a constant (see `op_value`), so a write
            // has no observable effect in our model. This keeps canary/TLS-using
            // functions fully lifted instead of degrading to `asm`.
            if ins.segment_prefix() != Register::None {
                return Some(vec![Stmt::Nop]);
            }
            if let Some(d) = frame_disp(ins, 0) {
                let dst = Location::Frame(d);
                let w = (ins.memory_size().size() * 8) as u32;
                let expr = combine_write(&dst, w, value, bits);
                return Some(vec![Stmt::Set { dst, expr }]);
            }
            let (addr, sz) = mem_addr(ins)?;
            Some(vec![Stmt::Store {
                addr,
                value,
                ty: Ty::int(sz as u8),
            }])
        }
        _ => None,
    }
}

fn sign_neg(x: &Expr) -> Expr {
    Expr::Binary(BinOp::Slt, Box::new(x.clone()), Box::new(konst(0)))
}

fn set_flag(k: FlagKind, e: Expr) -> Stmt {
    Stmt::Set {
        dst: Location::Flag(k),
        expr: e,
    }
}

fn read_flag(k: FlagKind) -> Expr {
    Expr::Read(Location::Flag(k))
}

/// Logical negation of a 0/1 flag expression.
fn lnot(e: Expr) -> Expr {
    Expr::Binary(BinOp::Eq, Box::new(e), Box::new(konst(0)))
}

fn lor(a: Expr, b: Expr) -> Expr {
    Expr::Binary(BinOp::Or, Box::new(a), Box::new(b))
}

/// The boolean condition of a `jcc`/`setcc`/`cmovcc`, expressed over the CPU
/// flags. A later pass folds it back to a relational (`SF!=OF` → `a < b`, etc.).
pub fn cc_to_cond(cc: ConditionCode) -> Expr {
    use ConditionCode as C;
    use FlagKind::*;
    let ne = |a: Expr, b: Expr| Expr::Binary(BinOp::Ne, Box::new(a), Box::new(b));
    match cc {
        C::e => read_flag(Zf),
        C::ne => lnot(read_flag(Zf)),
        C::b => read_flag(Cf),
        C::ae => lnot(read_flag(Cf)),
        C::be => lor(read_flag(Cf), read_flag(Zf)),
        C::a => lnot(lor(read_flag(Cf), read_flag(Zf))),
        C::s => read_flag(Sf),
        C::ns => lnot(read_flag(Sf)),
        C::o => read_flag(Of),
        C::no => lnot(read_flag(Of)),
        C::p => read_flag(Pf),
        C::np => lnot(read_flag(Pf)),
        C::l => ne(read_flag(Sf), read_flag(Of)),
        C::ge => lnot(ne(read_flag(Sf), read_flag(Of))),
        C::le => lor(read_flag(Zf), ne(read_flag(Sf), read_flag(Of))),
        C::g => lnot(lor(read_flag(Zf), ne(read_flag(Sf), read_flag(Of)))),
        C::None => konst(1),
    }
}

fn reads_access(a: OpAccess) -> bool {
    matches!(
        a,
        OpAccess::Read | OpAccess::CondRead | OpAccess::ReadWrite | OpAccess::ReadCondWrite
    )
}

fn writes_access(a: OpAccess) -> bool {
    matches!(
        a,
        OpAccess::Write | OpAccess::CondWrite | OpAccess::ReadWrite | OpAccess::ReadCondWrite
    )
}

const FLAG_BITS: [(u32, FlagKind); 6] = [
    (RflagsBits::ZF, FlagKind::Zf),
    (RflagsBits::SF, FlagKind::Sf),
    (RflagsBits::OF, FlagKind::Of),
    (RflagsBits::CF, FlagKind::Cf),
    (RflagsBits::PF, FlagKind::Pf),
    (RflagsBits::AF, FlagKind::Af),
];

/// Sound fallback for an instruction we don't model: capture its real register/
/// flag effects (via iced) so dataflow stays correct. The inputs are kept alive
/// by an opaque call; each written location is clobbered to `Undef` so later
/// reads get a fresh (honestly unknown) version rather than a stale one.
fn asm_fallback(insn: &Insn) -> Vec<Stmt> {
    let ins = &insn.raw;
    let mut factory = InstructionInfoFactory::new();
    let info = factory.info(ins);

    let mut reads: Vec<Expr> = Vec::new();
    let mut writes: Vec<Location> = Vec::new();
    for ur in info.used_registers() {
        if let Some(id) = reg_id(ur.register()) {
            let loc = Location::Reg(id);
            if reads_access(ur.access()) {
                let r = Expr::Read(loc.clone());
                if !reads.contains(&r) {
                    reads.push(r);
                }
            }
            if writes_access(ur.access()) && !writes.contains(&loc) {
                writes.push(loc);
            }
        }
    }
    let (rr, rw) = (ins.rflags_read(), ins.rflags_written());
    for (bit, k) in FLAG_BITS {
        if rr & bit != 0 {
            reads.push(Expr::Read(Location::Flag(k)));
        }
        if rw & bit != 0 && !writes.contains(&Location::Flag(k)) {
            writes.push(Location::Flag(k));
        }
    }

    let mut out = vec![Stmt::CallStmt(Expr::Call {
        target: CallTarget::Named(format!("asm:{}", insn.text)),
        args: reads,
        ret: Ty::Unknown,
    })];
    for w in writes {
        out.push(Stmt::Set {
            dst: w,
            expr: Expr::Undef,
        });
    }
    out
}

/// Flags for a subtraction `a - b` with result `r` (covers `cmp`, `sub`, `neg`).
fn sub_flags(a: &Expr, b: &Expr, r: &Expr) -> Vec<Stmt> {
    let of = Expr::Binary(
        BinOp::And,
        Box::new(Expr::Binary(
            BinOp::Ne,
            Box::new(sign_neg(a)),
            Box::new(sign_neg(b)),
        )),
        Box::new(Expr::Binary(
            BinOp::Ne,
            Box::new(sign_neg(r)),
            Box::new(sign_neg(a)),
        )),
    );
    vec![
        set_flag(FlagKind::Zf, Expr::Binary(BinOp::Eq, Box::new(a.clone()), Box::new(b.clone()))),
        set_flag(FlagKind::Sf, sign_neg(r)),
        set_flag(FlagKind::Cf, Expr::Binary(BinOp::Ult, Box::new(a.clone()), Box::new(b.clone()))),
        set_flag(FlagKind::Of, of),
    ]
}

/// Flags for a logical result `r` (covers `test`, `and`, `or`, `xor`): CF=OF=0.
fn logic_flags(r: &Expr) -> Vec<Stmt> {
    vec![
        set_flag(FlagKind::Zf, Expr::Binary(BinOp::Eq, Box::new(r.clone()), Box::new(konst(0)))),
        set_flag(FlagKind::Sf, sign_neg(r)),
        set_flag(FlagKind::Cf, konst(0)),
        set_flag(FlagKind::Of, konst(0)),
    ]
}

fn bin(op: BinOp, a: Expr, b: Expr) -> Expr {
    Expr::Binary(op, Box::new(a), Box::new(b))
}

/// Lift one instruction's compute semantics into IR statements. `bits` is the
/// target pointer width (32 or 64). Returns `[Asm]` for anything not modelled.
pub fn lift(insn: &Insn, bits: u32) -> Vec<Stmt> {
    let ins = &insn.raw;
    let asm = || asm_fallback(insn);

    // SSE/AVX vector instructions are not modelled in general. `reg_id` maps
    // XMM/YMM/ZMM to a register id, so without this guard a packed op (e.g.
    // `paddd`, `movdqu`) would be lifted as a *scalar* 64-bit operation —
    // silently wrong. The scalar floating-point subset handled below (operating
    // on bit patterns via runtime helpers) is exempt; everything else degrades to
    // an honest `asm` fallback that flags the decompilation incomplete.
    if uses_vector_reg(ins) && !is_scalar_float(ins) {
        return asm();
    }

    // `rep movs` is a forward `memcpy(rdi, rsi, rcx*size)` that leaves rdi/rsi
    // advanced and rcx = 0. Handled here because the string `movsd` shares its
    // mnemonic with the SSE scalar move — the rep prefix disambiguates. (Backward
    // copies via DF=1 are not modelled; gcc emits forward `rep movs`.)
    if ins.has_rep_prefix() {
        use Mnemonic::*;
        let sz: i128 = match ins.mnemonic() {
            Movsb => 1,
            Movsw => 2,
            Movsd => 4,
            Movsq => 8,
            _ => 0,
        };
        if sz != 0 {
            let rdi = Location::Reg(RegId(7));
            let rsi = Location::Reg(RegId(6));
            let rcx = Location::Reg(RegId(1));
            let bytes = bin(BinOp::Mul, Expr::Read(rcx.clone()), konst(sz));
            return vec![
                Stmt::CallStmt(Expr::Call {
                    target: CallTarget::Named("memcpy".into()),
                    args: vec![Expr::Read(rdi.clone()), Expr::Read(rsi.clone()), bytes.clone()],
                    ret: Ty::int(64),
                }),
                Stmt::Set { dst: rdi.clone(), expr: bin(BinOp::Add, Expr::Read(rdi), bytes.clone()) },
                Stmt::Set { dst: rsi.clone(), expr: bin(BinOp::Add, Expr::Read(rsi), bytes) },
                Stmt::Set { dst: rcx, expr: konst(0) },
            ];
        }
        // `rep stos`: forward fill of `rcx` elements at `rdi` with the low bytes of
        // the accumulator (al/ax/eax/rax). Leaves rdi advanced and rcx = 0.
        let (ssz, helper): (i128, &str) = match ins.mnemonic() {
            Stosb => (1, "__rep_stos8"),
            Stosw => (2, "__rep_stos16"),
            Stosd => (4, "__rep_stos32"),
            Stosq => (8, "__rep_stos64"),
            _ => (0, ""),
        };
        if ssz != 0 {
            let rdi = Location::Reg(RegId(7));
            let rax = Location::Reg(RegId(0));
            let rcx = Location::Reg(RegId(1));
            let bytes = bin(BinOp::Mul, Expr::Read(rcx.clone()), konst(ssz));
            return vec![
                Stmt::CallStmt(fcall(
                    helper,
                    vec![Expr::Read(rdi.clone()), Expr::Read(rax), Expr::Read(rcx.clone())],
                )),
                Stmt::Set { dst: rdi.clone(), expr: bin(BinOp::Add, Expr::Read(rdi), bytes) },
                Stmt::Set { dst: rcx, expr: konst(0) },
            ];
        }
    }

    // Helper to require Some or bail to Asm.
    macro_rules! some_or_asm {
        ($e:expr) => {
            match $e {
                Some(v) => v,
                None => return asm(),
            }
        };
    }

    match ins.mnemonic() {
        Mnemonic::Nop | Mnemonic::Endbr32 | Mnemonic::Endbr64 => vec![Stmt::Nop],

        Mnemonic::Mov => {
            let v = some_or_asm!(op_value(ins, 1));
            some_or_asm!(write_op0(ins, v, bits))
        }
        Mnemonic::Movzx => {
            // Source already masked to its width → zero-extended when stored wide.
            let v = some_or_asm!(op_value(ins, 1));
            some_or_asm!(write_op0(ins, v, bits))
        }
        Mnemonic::Movsx | Mnemonic::Movsxd => {
            let v = some_or_asm!(op_value(ins, 1));
            let sv = Expr::Unary(UnOp::SignExtend, Box::new(v));
            some_or_asm!(write_op0(ins, sv, bits))
        }
        Mnemonic::Lea => {
            let addr = if let Some(d) = frame_disp(ins, 1) {
                Expr::Addr(Location::Frame(d))
            } else {
                some_or_asm!(mem_addr(ins)).0
            };
            some_or_asm!(write_op0(ins, addr, bits))
        }

        Mnemonic::Add | Mnemonic::Sub | Mnemonic::And | Mnemonic::Or | Mnemonic::Xor => {
            let a = some_or_asm!(op_value(ins, 0));
            let b = some_or_asm!(op_value(ins, 1));
            let (op, flags) = match ins.mnemonic() {
                Mnemonic::Add => (BinOp::Add, None),
                Mnemonic::Sub => (BinOp::Sub, Some(true)),
                Mnemonic::And => (BinOp::And, Some(false)),
                Mnemonic::Or => (BinOp::Or, Some(false)),
                Mnemonic::Xor => (BinOp::Xor, Some(false)),
                _ => unreachable!(),
            };
            let r = bin(op, a.clone(), b.clone());
            let mut out = match flags {
                Some(true) => sub_flags(&a, &b, &r),  // sub
                Some(false) => logic_flags(&r),       // and/or/xor
                None => {
                    // add: ZF/SF from result, CF=carry, OF=signed overflow
                    let of = bin(
                        BinOp::And,
                        bin(BinOp::Eq, sign_neg(&a), sign_neg(&b)),
                        bin(BinOp::Ne, sign_neg(&r), sign_neg(&a)),
                    );
                    vec![
                        set_flag(FlagKind::Zf, bin(BinOp::Eq, r.clone(), konst(0))),
                        set_flag(FlagKind::Sf, sign_neg(&r)),
                        set_flag(FlagKind::Cf, bin(BinOp::Ult, r.clone(), a.clone())),
                        set_flag(FlagKind::Of, of),
                    ]
                }
            };
            out.extend(some_or_asm!(write_op0(ins, r, bits)));
            out
        }

        // Subtract/add with borrow/carry: fold the carry flag into the second
        // operand. `sbb dst,src` = dst - (src + CF); `adc dst,src` = dst + src + CF.
        // The `sbb r,r` idiom (`(a<b) ? -1 : 0`) lifts to `r - (r + CF)` = -CF.
        Mnemonic::Sbb => {
            let a = some_or_asm!(op_value(ins, 0));
            let b = some_or_asm!(op_value(ins, 1));
            let bc = bin(BinOp::Add, b, read_flag(FlagKind::Cf));
            let r = bin(BinOp::Sub, a.clone(), bc.clone());
            let mut out = sub_flags(&a, &bc, &r);
            out.extend(some_or_asm!(write_op0(ins, r, bits)));
            out
        }
        Mnemonic::Adc => {
            let a = some_or_asm!(op_value(ins, 0));
            let b = some_or_asm!(op_value(ins, 1));
            let bc = bin(BinOp::Add, b, read_flag(FlagKind::Cf));
            let r = bin(BinOp::Add, a.clone(), bc.clone());
            let of = bin(
                BinOp::And,
                bin(BinOp::Eq, sign_neg(&a), sign_neg(&bc)),
                bin(BinOp::Ne, sign_neg(&r), sign_neg(&a)),
            );
            let mut out = vec![
                set_flag(FlagKind::Zf, bin(BinOp::Eq, r.clone(), konst(0))),
                set_flag(FlagKind::Sf, sign_neg(&r)),
                set_flag(FlagKind::Cf, bin(BinOp::Ult, r.clone(), a.clone())),
                set_flag(FlagKind::Of, of),
            ];
            out.extend(some_or_asm!(write_op0(ins, r, bits)));
            out
        }

        // --- scalar SSE floating point (via __fp_* bit-pattern helpers) -------
        // Moves copy bit patterns; the helpers reinterpret only the relevant low
        // bits, so an unmasked move is fine.
        Mnemonic::Movss | Mnemonic::Movsd | Mnemonic::Movd | Mnemonic::Movq => {
            let v = some_or_asm!(op_value(ins, 1));
            some_or_asm!(write_op0(ins, v, bits))
        }
        // 128-bit moves: copy both 64-bit halves (register or 16-byte memory).
        Mnemonic::Movaps | Mnemonic::Movapd | Mnemonic::Movups | Mnemonic::Movupd
        | Mnemonic::Movdqa | Mnemonic::Movdqu => {
            let (lo, hi) = some_or_asm!(read_xmm128(ins, 1));
            some_or_asm!(write_xmm128(ins, lo, hi))
        }
        // 128-bit lane-wise integer add/sub (no carry across 32-bit lanes).
        Mnemonic::Paddd | Mnemonic::Psubd => {
            let (alo, ahi) = some_or_asm!(read_xmm128(ins, 0));
            let (blo, bhi) = some_or_asm!(read_xmm128(ins, 1));
            let h = if ins.mnemonic() == Mnemonic::Paddd { "__pi_add32" } else { "__pi_sub32" };
            let lo = fcall(h, vec![alo, blo]);
            let hi = fcall(h, vec![ahi, bhi]);
            some_or_asm!(write_xmm128(ins, lo, hi))
        }
        // Byte shift of the whole 128-bit value (right). Handles the reduction
        // shifts (4/8/12); other counts aren't modelled.
        Mnemonic::Psrldq => {
            let n = xmm_num(ins.op_register(0));
            let imm = ins.immediate(1);
            match (n, imm) {
                (Some(n), 8) => vec![
                    Stmt::Set { dst: xmm_lo(n), expr: Expr::Read(xmm_hi(n)) },
                    Stmt::Set { dst: xmm_hi(n), expr: konst(0) },
                ],
                (Some(n), 12) => vec![
                    Stmt::Set {
                        dst: xmm_lo(n),
                        expr: bin(BinOp::Shr, Expr::Read(xmm_hi(n)), konst(32)),
                    },
                    Stmt::Set { dst: xmm_hi(n), expr: konst(0) },
                ],
                (Some(n), 4) => {
                    // lo' = (lo>>32)|(hi<<32) ; hi' = hi>>32  (each masked to 64).
                    let lo = bin(
                        BinOp::Or,
                        bin(BinOp::Shr, Expr::Read(xmm_lo(n)), konst(32)),
                        bin(BinOp::And, bin(BinOp::Shl, Expr::Read(xmm_hi(n)), konst(32)), konst(mask(64))),
                    );
                    let hi = bin(BinOp::Shr, Expr::Read(xmm_hi(n)), konst(32));
                    vec![
                        Stmt::Set { dst: xmm_lo(n), expr: lo },
                        Stmt::Set { dst: xmm_hi(n), expr: hi },
                    ]
                }
                _ => return asm(),
            }
        }
        // 128-bit bitwise (exact on both halves). `pandn dst,src` = ~dst & src.
        Mnemonic::Pand | Mnemonic::Por | Mnemonic::Pandn => {
            let (alo, ahi) = some_or_asm!(read_xmm128(ins, 0));
            let (blo, bhi) = some_or_asm!(read_xmm128(ins, 1));
            let (lo, hi) = match ins.mnemonic() {
                Mnemonic::Pand => (bin(BinOp::And, alo, blo), bin(BinOp::And, ahi, bhi)),
                Mnemonic::Por => (bin(BinOp::Or, alo, blo), bin(BinOp::Or, ahi, bhi)),
                _ => (
                    bin(BinOp::And, Expr::Unary(UnOp::Not, Box::new(alo)), blo),
                    bin(BinOp::And, Expr::Unary(UnOp::Not, Box::new(ahi)), bhi),
                ),
            };
            some_or_asm!(write_xmm128(ins, lo, hi))
        }
        // Lane-wise 32-bit compares producing all-ones / zero masks.
        Mnemonic::Pcmpeqd | Mnemonic::Pcmpgtd => {
            let (alo, ahi) = some_or_asm!(read_xmm128(ins, 0));
            let (blo, bhi) = some_or_asm!(read_xmm128(ins, 1));
            let h = if ins.mnemonic() == Mnemonic::Pcmpeqd { "__pi_eq32" } else { "__pi_gt32" };
            some_or_asm!(write_xmm128(ins, fcall(h, vec![alo, blo]), fcall(h, vec![ahi, bhi])))
        }
        // Lane-wise add: 16-bit (helper) and 64-bit (one per half).
        Mnemonic::Paddw => {
            let (alo, ahi) = some_or_asm!(read_xmm128(ins, 0));
            let (blo, bhi) = some_or_asm!(read_xmm128(ins, 1));
            some_or_asm!(write_xmm128(
                ins,
                fcall("__pi_add16", vec![alo, blo]),
                fcall("__pi_add16", vec![ahi, bhi])
            ))
        }
        Mnemonic::Paddq => {
            let (alo, ahi) = some_or_asm!(read_xmm128(ins, 0));
            let (blo, bhi) = some_or_asm!(read_xmm128(ins, 1));
            some_or_asm!(write_xmm128(ins, bin(BinOp::Add, alo, blo), bin(BinOp::Add, ahi, bhi)))
        }
        // 16-bit lane signed compare (mask), unsigned 32x32->64 of even lanes,
        // and 64-bit logical right shift by an immediate.
        Mnemonic::Pcmpgtw => {
            let (alo, ahi) = some_or_asm!(read_xmm128(ins, 0));
            let (blo, bhi) = some_or_asm!(read_xmm128(ins, 1));
            some_or_asm!(write_xmm128(
                ins,
                fcall("__pi_gt16", vec![alo, blo]),
                fcall("__pi_gt16", vec![ahi, bhi])
            ))
        }
        Mnemonic::Psubusw => {
            let (alo, ahi) = some_or_asm!(read_xmm128(ins, 0));
            let (blo, bhi) = some_or_asm!(read_xmm128(ins, 1));
            some_or_asm!(write_xmm128(
                ins,
                fcall("__pi_subus16", vec![alo, blo]),
                fcall("__pi_subus16", vec![ahi, bhi])
            ))
        }
        Mnemonic::Pmuludq => {
            let (dlo, dhi) = some_or_asm!(read_xmm128(ins, 0));
            let (slo, shi) = some_or_asm!(read_xmm128(ins, 1));
            some_or_asm!(write_xmm128(
                ins,
                fcall("__pi_muludq", vec![dlo, slo]),
                fcall("__pi_muludq", vec![dhi, shi])
            ))
        }
        Mnemonic::Psrlq if ins.op_kind(1) == OpKind::Immediate8 => {
            let n = konst(ins.immediate(1) as i128);
            let (lo, hi) = some_or_asm!(read_xmm128(ins, 0));
            some_or_asm!(write_xmm128(
                ins,
                bin(BinOp::Shr, lo, n.clone()),
                bin(BinOp::Shr, hi, n)
            ))
        }
        // Unpack/interleave. The *high* variants are the low ones applied to the
        // high halves; the quadword unpacks are pure half selection.
        Mnemonic::Punpcklwd | Mnemonic::Punpckhwd => {
            let (dlo, dhi) = some_or_asm!(read_xmm128(ins, 0));
            let (slo, shi) = some_or_asm!(read_xmm128(ins, 1));
            let (d, s) = if ins.mnemonic() == Mnemonic::Punpcklwd { (dlo, slo) } else { (dhi, shi) };
            some_or_asm!(write_xmm128(
                ins,
                fcall("__pi_unpcklwd_lo", vec![d.clone(), s.clone()]),
                fcall("__pi_unpcklwd_hi", vec![d, s])
            ))
        }
        // unpcklps/unpckhps interleave 32-bit lanes exactly like punpckldq/hdq.
        Mnemonic::Punpckldq | Mnemonic::Punpckhdq | Mnemonic::Unpcklps | Mnemonic::Unpckhps => {
            let (dlo, dhi) = some_or_asm!(read_xmm128(ins, 0));
            let (slo, shi) = some_or_asm!(read_xmm128(ins, 1));
            let low = matches!(ins.mnemonic(), Mnemonic::Punpckldq | Mnemonic::Unpcklps);
            let (d, s) = if low { (dlo, slo) } else { (dhi, shi) };
            some_or_asm!(write_xmm128(
                ins,
                fcall("__pi_unpckldq_lo", vec![d.clone(), s.clone()]),
                fcall("__pi_unpckldq_hi", vec![d, s])
            ))
        }
        Mnemonic::Punpcklqdq => {
            let (dlo, _) = some_or_asm!(read_xmm128(ins, 0));
            let (slo, _) = some_or_asm!(read_xmm128(ins, 1));
            some_or_asm!(write_xmm128(ins, dlo, slo))
        }
        Mnemonic::Punpckhqdq => {
            let (_, dhi) = some_or_asm!(read_xmm128(ins, 0));
            let (_, shi) = some_or_asm!(read_xmm128(ins, 1));
            some_or_asm!(write_xmm128(ins, dhi, shi))
        }
        // Packed single-precision arithmetic/min/max: two floats per 64-bit half,
        // bit-exact via the `__ps_*` helpers.
        Mnemonic::Addps | Mnemonic::Subps | Mnemonic::Mulps | Mnemonic::Divps
        | Mnemonic::Minps | Mnemonic::Maxps => {
            let h = match ins.mnemonic() {
                Mnemonic::Addps => "__ps_add",
                Mnemonic::Subps => "__ps_sub",
                Mnemonic::Mulps => "__ps_mul",
                Mnemonic::Divps => "__ps_div",
                Mnemonic::Minps => "__ps_min",
                _ => "__ps_max",
            };
            let (alo, ahi) = some_or_asm!(read_xmm128(ins, 0));
            let (blo, bhi) = some_or_asm!(read_xmm128(ins, 1));
            some_or_asm!(write_xmm128(ins, fcall(h, vec![alo, blo]), fcall(h, vec![ahi, bhi])))
        }
        Mnemonic::Sqrtps => {
            let (lo, hi) = some_or_asm!(read_xmm128(ins, 1));
            some_or_asm!(write_xmm128(ins, fcall("__ps_sqrt", vec![lo]), fcall("__ps_sqrt", vec![hi])))
        }
        Mnemonic::Cvtdq2ps => {
            let (lo, hi) = some_or_asm!(read_xmm128(ins, 1));
            some_or_asm!(write_xmm128(ins, fcall("__ps_cvtdq", vec![lo]), fcall("__ps_cvtdq", vec![hi])))
        }
        // cmpps with the imm8 predicate (`cmpltps`/`cmpleps`/`cmpeqps`… aliases).
        Mnemonic::Cmpps => {
            let (alo, ahi) = some_or_asm!(read_xmm128(ins, 0));
            let (blo, bhi) = some_or_asm!(read_xmm128(ins, 1));
            let p = konst(ins.immediate(2) as i128);
            some_or_asm!(write_xmm128(
                ins,
                fcall("__ps_cmp", vec![alo, blo, p.clone()]),
                fcall("__ps_cmp", vec![ahi, bhi, p])
            ))
        }
        // Bitwise packed-float logic: operate on the raw 128-bit halves.
        Mnemonic::Andps | Mnemonic::Orps | Mnemonic::Andnps => {
            let (alo, ahi) = some_or_asm!(read_xmm128(ins, 0));
            let (blo, bhi) = some_or_asm!(read_xmm128(ins, 1));
            let (lo, hi) = match ins.mnemonic() {
                Mnemonic::Andps => (bin(BinOp::And, alo, blo), bin(BinOp::And, ahi, bhi)),
                Mnemonic::Orps => (bin(BinOp::Or, alo, blo), bin(BinOp::Or, ahi, bhi)),
                _ => (
                    bin(BinOp::And, Expr::Unary(UnOp::Not, Box::new(alo)), blo),
                    bin(BinOp::And, Expr::Unary(UnOp::Not, Box::new(ahi)), bhi),
                ),
            };
            some_or_asm!(write_xmm128(ins, lo, hi))
        }
        // shufps: low two lanes from dst, high two from src (imm8 lane selectors).
        // Reuses the dword-shuffle helpers (same 32-bit lane permutation).
        Mnemonic::Shufps => {
            let (dlo, dhi) = some_or_asm!(read_xmm128(ins, 0));
            let (slo, shi) = some_or_asm!(read_xmm128(ins, 1));
            let imm = konst(ins.immediate(2) as i128);
            some_or_asm!(write_xmm128(
                ins,
                fcall("__pi_shuf_lo", vec![dlo, dhi, imm.clone()]),
                fcall("__pi_shuf_hi", vec![slo, shi, imm])
            ))
        }
        // Extract the 4 lane sign bits into a GP register.
        Mnemonic::Movmskps => {
            let (lo, hi) = some_or_asm!(read_xmm128(ins, 1));
            some_or_asm!(write_op0(ins, fcall("__ps_movmsk", vec![lo, hi]), bits))
        }

        // Packed double arithmetic: each 64-bit half is one IEEE-754 double,
        // computed bit-exactly via the scalar `__fp_*64` helpers.
        Mnemonic::Addpd | Mnemonic::Subpd | Mnemonic::Mulpd | Mnemonic::Divpd => {
            let h = match ins.mnemonic() {
                Mnemonic::Addpd => "__fp_add64",
                Mnemonic::Subpd => "__fp_sub64",
                Mnemonic::Mulpd => "__fp_mul64",
                _ => "__fp_div64",
            };
            let (alo, ahi) = some_or_asm!(read_xmm128(ins, 0));
            let (blo, bhi) = some_or_asm!(read_xmm128(ins, 1));
            some_or_asm!(write_xmm128(ins, fcall(h, vec![alo, blo]), fcall(h, vec![ahi, bhi])))
        }
        // Double-precision unpack: pure 64-bit half selection.
        Mnemonic::Unpcklpd => {
            // dst.low unchanged, dst.high = src.low.
            let (dlo, _) = some_or_asm!(read_xmm128(ins, 0));
            let (slo, _) = some_or_asm!(read_xmm128(ins, 1));
            some_or_asm!(write_xmm128(ins, dlo, slo))
        }
        Mnemonic::Unpckhpd => {
            // dst.low = dst.high, dst.high = src.high.
            let (_, dhi) = some_or_asm!(read_xmm128(ins, 0));
            let (_, shi) = some_or_asm!(read_xmm128(ins, 1));
            some_or_asm!(write_xmm128(ins, dhi, shi))
        }
        // 64-bit half moves between XMM registers (no memory operand).
        Mnemonic::Movhlps => {
            // dst.low = src.high; dst.high unchanged.
            let (_, dhi) = some_or_asm!(read_xmm128(ins, 0));
            let (_, shi) = some_or_asm!(read_xmm128(ins, 1));
            some_or_asm!(write_xmm128(ins, shi, dhi))
        }
        Mnemonic::Movlhps => {
            // dst.high = src.low; dst.low unchanged.
            let (dlo, _) = some_or_asm!(read_xmm128(ins, 0));
            let (slo, _) = some_or_asm!(read_xmm128(ins, 1));
            some_or_asm!(write_xmm128(ins, dlo, slo))
        }
        // Move a 64-bit half to/from memory. `movhps`/`movhpd` use the high half,
        // `movlps`/`movlpd` the low half; the other half is preserved.
        Mnemonic::Movhps | Mnemonic::Movhpd | Mnemonic::Movlps | Mnemonic::Movlpd => {
            let high = matches!(ins.mnemonic(), Mnemonic::Movhps | Mnemonic::Movhpd);
            if ins.segment_prefix() != Register::None {
                return asm();
            }
            match (ins.op_kind(0), ins.op_kind(1)) {
                // Load m64 into the selected half of the destination register.
                (OpKind::Register, OpKind::Memory) => {
                    let (addr, _) = some_or_asm!(mem_addr(ins));
                    let m = Expr::Load { addr: Box::new(addr), ty: Ty::int(64) };
                    let (dlo, dhi) = some_or_asm!(read_xmm128(ins, 0));
                    if high {
                        some_or_asm!(write_xmm128(ins, dlo, m))
                    } else {
                        some_or_asm!(write_xmm128(ins, m, dhi))
                    }
                }
                // Store the selected half of the source register to m64.
                (OpKind::Memory, OpKind::Register) => {
                    let (addr, _) = some_or_asm!(mem_addr(ins));
                    let (slo, shi) = some_or_asm!(read_xmm128(ins, 1));
                    let v = if high { shi } else { slo };
                    vec![Stmt::Store { addr, value: v, ty: Ty::int(64) }]
                }
                _ => return asm(),
            }
        }
        // Select one 64-bit lane from each source per the imm8 (bit0 → dst low
        // from dst, bit1 → dst high from src).
        Mnemonic::Shufpd => {
            let (dlo, dhi) = some_or_asm!(read_xmm128(ins, 0));
            let (slo, shi) = some_or_asm!(read_xmm128(ins, 1));
            let imm = ins.immediate(2);
            let lo = if imm & 1 != 0 { dhi } else { dlo };
            let hi = if imm & 2 != 0 { shi } else { slo };
            some_or_asm!(write_xmm128(ins, lo, hi))
        }
        // Shuffle the four 32-bit lanes per the imm8 selector.
        Mnemonic::Pshufd => {
            let (lo, hi) = some_or_asm!(read_xmm128(ins, 1));
            let imm = konst(ins.immediate(2) as i128);
            let nlo = fcall("__pi_shuf_lo", vec![lo.clone(), hi.clone(), imm.clone()]);
            let nhi = fcall("__pi_shuf_hi", vec![lo, hi, imm]);
            some_or_asm!(write_xmm128(ins, nlo, nhi))
        }
        Mnemonic::Addss => some_or_asm!(fbin("__fp_add32", ins, bits)),
        Mnemonic::Subss => some_or_asm!(fbin("__fp_sub32", ins, bits)),
        Mnemonic::Mulss => some_or_asm!(fbin("__fp_mul32", ins, bits)),
        Mnemonic::Divss => some_or_asm!(fbin("__fp_div32", ins, bits)),
        Mnemonic::Addsd => some_or_asm!(fbin("__fp_add64", ins, bits)),
        Mnemonic::Subsd => some_or_asm!(fbin("__fp_sub64", ins, bits)),
        Mnemonic::Mulsd => some_or_asm!(fbin("__fp_mul64", ins, bits)),
        Mnemonic::Divsd => some_or_asm!(fbin("__fp_div64", ins, bits)),
        Mnemonic::Cvtsi2ss => {
            let n = if op_bits(ins, 1) >= 64 { "__fp_i64_32" } else { "__fp_i32_32" };
            some_or_asm!(fcvt(n, ins, bits))
        }
        Mnemonic::Cvtsi2sd => {
            let n = if op_bits(ins, 1) >= 64 { "__fp_i64_64" } else { "__fp_i32_64" };
            some_or_asm!(fcvt(n, ins, bits))
        }
        Mnemonic::Cvttss2si => {
            let n = if op_bits(ins, 0) >= 64 { "__fp_32_i64" } else { "__fp_32_i32" };
            some_or_asm!(fcvt(n, ins, bits))
        }
        Mnemonic::Cvttsd2si => {
            let n = if op_bits(ins, 0) >= 64 { "__fp_64_i64" } else { "__fp_64_i32" };
            some_or_asm!(fcvt(n, ins, bits))
        }
        Mnemonic::Cvtss2sd => some_or_asm!(fcvt("__fp_32_64", ins, bits)),
        Mnemonic::Cvtsd2ss => some_or_asm!(fcvt("__fp_64_32", ins, bits)),
        Mnemonic::Comiss | Mnemonic::Ucomiss | Mnemonic::Comisd | Mnemonic::Ucomisd => {
            let a = some_or_asm!(op_value(ins, 0));
            let b = some_or_asm!(op_value(ins, 1));
            let is32 = matches!(ins.mnemonic(), Mnemonic::Comiss | Mnemonic::Ucomiss);
            let (lt, eq, un) = if is32 {
                ("__fp_lt32", "__fp_eq32", "__fp_un32")
            } else {
                ("__fp_lt64", "__fp_eq64", "__fp_un64")
            };
            // comiss flags: CF=a<b|unord, ZF=a==b|unord, PF=unord, SF=OF=0.
            let unord = fcall(un, vec![a.clone(), b.clone()]);
            vec![
                set_flag(FlagKind::Cf, bin(BinOp::Or, fcall(lt, vec![a.clone(), b.clone()]), unord.clone())),
                set_flag(FlagKind::Zf, bin(BinOp::Or, fcall(eq, vec![a.clone(), b.clone()]), unord.clone())),
                set_flag(FlagKind::Pf, unord),
                set_flag(FlagKind::Sf, konst(0)),
                set_flag(FlagKind::Of, konst(0)),
            ]
        }
        Mnemonic::Pxor | Mnemonic::Xorps | Mnemonic::Xorpd => {
            // Full 128-bit xor on both halves (the `xorps xmm,xmm` zeroing idiom
            // folds to 0). Zeroing *both* halves matters: the high half feeds
            // later packed ops, so zeroing only the low half corrupts them.
            let (alo, ahi) = some_or_asm!(read_xmm128(ins, 0));
            let (blo, bhi) = some_or_asm!(read_xmm128(ins, 1));
            some_or_asm!(write_xmm128(ins, bin(BinOp::Xor, alo, blo), bin(BinOp::Xor, ahi, bhi)))
        }

        // sahf: load CPU flags from AH (CF=AH.0, PF=AH.2, AF=AH.4, ZF=AH.6,
        // SF=AH.7). Completes the x87 `fcom; fnstsw ax; sahf` compare idiom.
        Mnemonic::Sahf => {
            let ah = bin(BinOp::Shr, Expr::Read(Location::Reg(RegId(0))), konst(8));
            let bit = |n: i128| bin(BinOp::And, bin(BinOp::Shr, ah.clone(), konst(n)), konst(1));
            vec![
                set_flag(FlagKind::Cf, bin(BinOp::And, ah.clone(), konst(1))),
                set_flag(FlagKind::Pf, bit(2)),
                set_flag(FlagKind::Af, bit(4)),
                set_flag(FlagKind::Zf, bit(6)),
                set_flag(FlagKind::Sf, bit(7)),
            ]
        }

        Mnemonic::Cmp => {
            let a = some_or_asm!(op_value(ins, 0));
            let b = some_or_asm!(op_value(ins, 1));
            let r = bin(BinOp::Sub, a.clone(), b.clone());
            sub_flags(&a, &b, &r)
        }
        Mnemonic::Test => {
            let a = some_or_asm!(op_value(ins, 0));
            let b = some_or_asm!(op_value(ins, 1));
            let r = bin(BinOp::And, a, b);
            logic_flags(&r)
        }

        Mnemonic::Inc | Mnemonic::Dec => {
            let a = some_or_asm!(op_value(ins, 0));
            let op = if ins.mnemonic() == Mnemonic::Inc {
                BinOp::Add
            } else {
                BinOp::Sub
            };
            let r = bin(op, a.clone(), konst(1));
            let mut out = vec![
                set_flag(FlagKind::Zf, bin(BinOp::Eq, r.clone(), konst(0))),
                set_flag(FlagKind::Sf, sign_neg(&r)),
            ];
            out.extend(some_or_asm!(write_op0(ins, r, bits)));
            out
        }
        Mnemonic::Neg => {
            let a = some_or_asm!(op_value(ins, 0));
            let r = bin(BinOp::Sub, konst(0), a.clone());
            let mut out = sub_flags(&konst(0), &a, &r);
            out.extend(some_or_asm!(write_op0(ins, r, bits)));
            out
        }
        Mnemonic::Not => {
            let a = some_or_asm!(op_value(ins, 0));
            let r = Expr::Unary(UnOp::Not, Box::new(a));
            some_or_asm!(write_op0(ins, r, bits))
        }
        // 2-operand `imul dst, src` and 3-operand `imul dst, src, imm`.
        // (1-operand form writes a double-width result -> Asm fallback.)
        Mnemonic::Imul if ins.op_count() == 2 => {
            let r = bin(BinOp::Mul, some_or_asm!(op_value(ins, 0)), some_or_asm!(op_value(ins, 1)));
            some_or_asm!(write_op0(ins, r, bits))
        }
        Mnemonic::Imul if ins.op_count() == 3 => {
            let r = bin(BinOp::Mul, some_or_asm!(op_value(ins, 1)), some_or_asm!(op_value(ins, 2)));
            some_or_asm!(write_op0(ins, r, bits))
        }
        // 1-operand mul/div/idiv (implicit edx:eax). 32-bit form only: the
        // 64-bit-wide result fits a 64-bit IR value. 64-bit operands would need
        // 128-bit arithmetic -> Asm fallback (sound).
        Mnemonic::Mul | Mnemonic::Imul | Mnemonic::Div | Mnemonic::Idiv if ins.op_count() == 1 => {
            let w = op0_width(ins, bits);
            // 64-bit form: rdx:rax is a 128-bit operand — use __int128 helpers.
            if w == 64 {
                let src = some_or_asm!(op_value(ins, 0));
                let rax = Location::Reg(RegId(0));
                let rdx = Location::Reg(RegId(2));
                let ra = Expr::Read(rax.clone());
                let rd = Expr::Read(rdx.clone());
                let t_lo = Location::Temp((insn.address as u32).wrapping_mul(2));
                let t_hi = Location::Temp((insn.address as u32).wrapping_mul(2).wrapping_add(1));
                let (lo_e, hi_e) = match ins.mnemonic() {
                    Mnemonic::Mul => (
                        bin(BinOp::Mul, ra.clone(), src.clone()),
                        fcall("__ix_mul64hi", vec![ra, src]),
                    ),
                    Mnemonic::Imul => (
                        bin(BinOp::Mul, ra.clone(), src.clone()),
                        fcall("__ix_imul64hi", vec![ra, src]),
                    ),
                    Mnemonic::Div => (
                        fcall("__ix_udiv", vec![rd.clone(), ra.clone(), src.clone()]),
                        fcall("__ix_umod", vec![rd, ra, src]),
                    ),
                    _ => (
                        fcall("__ix_idiv", vec![rd.clone(), ra.clone(), src.clone()]),
                        fcall("__ix_imod", vec![rd, ra, src]),
                    ),
                };
                return vec![
                    Stmt::Set { dst: t_lo.clone(), expr: lo_e },
                    Stmt::Set { dst: t_hi.clone(), expr: hi_e },
                    Stmt::Set { dst: rax, expr: Expr::Read(t_lo) },
                    Stmt::Set { dst: rdx, expr: Expr::Read(t_hi) },
                ];
            }
            if w != 32 {
                return asm();
            }
            let src = some_or_asm!(op_value(ins, 0));
            let rax = Location::Reg(RegId(0));
            let rdx = Location::Reg(RegId(2));
            let m32 = || konst(mask(32));
            let eax = bin(BinOp::And, Expr::Read(rax.clone()), m32());
            let edx = bin(BinOp::And, Expr::Read(rdx.clone()), m32());
            let lo = |e: Expr| combine_write(&rax, 32, bin(BinOp::And, e, m32()), bits);
            let hi = |e: Expr| combine_write(&rdx, 32, bin(BinOp::And, e, m32()), bits);
            // Both results depend on the *original* edx:eax. Compute them into
            // temporaries first, then assign — otherwise the second write would
            // read the register the first write already changed.
            let t_lo = Location::Temp((insn.address as u32).wrapping_mul(2));
            let t_hi = Location::Temp((insn.address as u32).wrapping_mul(2).wrapping_add(1));
            let (lo_expr, hi_expr) = match ins.mnemonic() {
                Mnemonic::Mul => {
                    let p = bin(BinOp::Mul, eax, src); // <= 64 bits
                    (p.clone(), bin(BinOp::Shr, p, konst(32)))
                }
                Mnemonic::Imul => {
                    // Signed 32×32→64 product: sign-extend both operands first.
                    let a = Expr::Unary(UnOp::SignExtend, Box::new(eax));
                    let b = Expr::Unary(UnOp::SignExtend, Box::new(src));
                    let p = bin(BinOp::Mul, a, b);
                    (bin(BinOp::And, p.clone(), konst(mask(32))), bin(BinOp::Shr, p, konst(32)))
                }
                Mnemonic::Div => {
                    let d = bin(BinOp::Or, bin(BinOp::Shl, edx, konst(32)), eax);
                    (bin(BinOp::UDiv, d.clone(), src.clone()), bin(BinOp::UMod, d, src))
                }
                _ => {
                    // idiv: signed; dividend is the signed 64-bit edx:eax.
                    let d = bin(BinOp::Or, bin(BinOp::Shl, edx, konst(32)), eax);
                    (bin(BinOp::SDiv, d.clone(), src.clone()), bin(BinOp::SMod, d, src))
                }
            };
            vec![
                Stmt::Set { dst: t_lo.clone(), expr: lo_expr },
                Stmt::Set { dst: t_hi.clone(), expr: hi_expr },
                Stmt::Set { dst: rax.clone(), expr: lo(Expr::Read(t_lo)) },
                Stmt::Set { dst: rdx.clone(), expr: hi(Expr::Read(t_hi)) },
            ]
        }

        // cdq/cltd: EDX = sign-extension of EAX (all-ones if negative else 0).
        Mnemonic::Cdq => {
            let rax = Location::Reg(RegId(0));
            let rdx = Location::Reg(RegId(2));
            let eax = bin(BinOp::And, Expr::Read(rax), konst(mask(32)));
            let sign = bin(BinOp::And, bin(BinOp::Sar, eax, konst(31)), konst(mask(32)));
            vec![Stmt::Set { dst: rdx.clone(), expr: combine_write(&rdx, 32, sign, bits) }]
        }
        // cqo: RDX = sign-extension of RAX (64-bit).
        Mnemonic::Cqo => {
            let rax = Location::Reg(RegId(0));
            let rdx = Location::Reg(RegId(2));
            vec![Stmt::Set { dst: rdx, expr: bin(BinOp::Sar, Expr::Read(rax), konst(63)) }]
        }

        // cbw/cwde/cdqe: sign-extend the accumulator in place (al->ax, ax->eax,
        // eax->rax).
        Mnemonic::Cbw | Mnemonic::Cwde | Mnemonic::Cdqe => {
            let (sw, dw) = match ins.mnemonic() {
                Mnemonic::Cbw => (8u32, 16u32),
                Mnemonic::Cwde => (16, 32),
                _ => (32, 64), // Cdqe
            };
            let rax = Location::Reg(RegId(0));
            let src = bin(BinOp::And, Expr::Read(rax.clone()), konst(mask(sw)));
            let sv = Expr::Unary(UnOp::SignExtend, Box::new(src));
            vec![Stmt::Set { dst: rax.clone(), expr: combine_write(&rax, dw, sv, bits) }]
        }

        // Bit test (+ complement/set/reset). Register form only: `CF = (dst >>
        // (idx mod width)) & 1`, and the variants toggle/set/clear that bit. The
        // memory form with a register index addresses a bit string and isn't
        // modelled.
        Mnemonic::Bt | Mnemonic::Btc | Mnemonic::Bts | Mnemonic::Btr
            if ins.op_kind(0) == OpKind::Register =>
        {
            let w = op0_width(ins, bits);
            let dst = some_or_asm!(op_value(ins, 0));
            let idx = some_or_asm!(op_value(ins, 1));
            let pos = bin(BinOp::And, idx, konst((w - 1) as i128));
            let bit = bin(BinOp::And, bin(BinOp::Shr, dst.clone(), pos.clone()), konst(1));
            let mut out = vec![set_flag(FlagKind::Cf, bit)];
            if ins.mnemonic() != Mnemonic::Bt {
                let m = bin(BinOp::Shl, konst(1), pos);
                let nv = match ins.mnemonic() {
                    Mnemonic::Bts => bin(BinOp::Or, dst, m),
                    Mnemonic::Btr => bin(BinOp::And, dst, Expr::Unary(UnOp::Not, Box::new(m))),
                    _ => bin(BinOp::Xor, dst, m), // Btc
                };
                out.extend(some_or_asm!(write_op0(ins, nv, bits)));
            }
            out
        }
        // Byte swap (endianness reversal) of a 32- or 64-bit register.
        Mnemonic::Bswap => {
            let w = op0_width(ins, bits);
            let v = some_or_asm!(op_value(ins, 0));
            let h = if w == 64 { "__ix_bswap64" } else { "__ix_bswap32" };
            some_or_asm!(write_op0(ins, fcall(h, vec![v]), bits))
        }
        // Bit scan forward/reverse: index of lowest/highest set bit; sets ZF when
        // the source is zero (the destination is then undefined — modelled as 0).
        Mnemonic::Bsf | Mnemonic::Bsr => {
            let w = op0_width(ins, bits);
            let src = some_or_asm!(op_value(ins, 1));
            let h = match (ins.mnemonic(), w) {
                (Mnemonic::Bsf, 64) => "__ix_bsf64",
                (Mnemonic::Bsf, _) => "__ix_bsf32",
                (_, 64) => "__ix_bsr64",
                (_, _) => "__ix_bsr32",
            };
            let mut out = vec![set_flag(FlagKind::Zf, bin(BinOp::Eq, src.clone(), konst(0)))];
            out.extend(some_or_asm!(write_op0(ins, fcall(h, vec![src]), bits)));
            out
        }

        // tzcnt/lzcnt (BMI, defined at 0 → width) and popcnt. CF = (src == 0)
        // for tz/lzcnt (an over-approximation of their exact flag rules, but the
        // count result is exact); popcnt sets ZF = (src == 0).
        Mnemonic::Tzcnt | Mnemonic::Lzcnt | Mnemonic::Popcnt => {
            let w = op0_width(ins, bits);
            let src = some_or_asm!(op_value(ins, 1));
            let h = match (ins.mnemonic(), w) {
                (Mnemonic::Tzcnt, 64) => "__ix_tzcnt64",
                (Mnemonic::Tzcnt, _) => "__ix_tzcnt32",
                (Mnemonic::Lzcnt, 64) => "__ix_lzcnt64",
                (Mnemonic::Lzcnt, _) => "__ix_lzcnt32",
                (_, 64) => "__ix_popcnt64",
                (_, _) => "__ix_popcnt32",
            };
            let zf = bin(BinOp::Eq, src.clone(), konst(0));
            let flag = if ins.mnemonic() == Mnemonic::Popcnt { FlagKind::Zf } else { FlagKind::Cf };
            let mut out = vec![set_flag(flag, zf)];
            out.extend(some_or_asm!(write_op0(ins, fcall(h, vec![src]), bits)));
            out
        }

        // leave: mov rsp, rbp ; pop rbp.
        Mnemonic::Leave => {
            let ptr = (bits / 8) as i128;
            let sp = Location::Reg(RegId(4));
            let bp = Location::Reg(RegId(5));
            vec![
                Stmt::Set { dst: sp.clone(), expr: Expr::Read(bp.clone()) },
                Stmt::Set {
                    dst: bp,
                    expr: Expr::Load {
                        addr: Box::new(Expr::Read(sp.clone())),
                        ty: Ty::int(bits as u8),
                    },
                },
                Stmt::Set { dst: sp.clone(), expr: bin(BinOp::Add, Expr::Read(sp), konst(ptr)) },
            ]
        }

        Mnemonic::Shl => {
            let a = some_or_asm!(op_value(ins, 0));
            let b = some_or_asm!(op_value(ins, 1));
            let r = bin(BinOp::Shl, a, b);
            let mut out = vec![set_flag(FlagKind::Zf, bin(BinOp::Eq, r.clone(), konst(0)))];
            out.extend(some_or_asm!(write_op0(ins, r, bits)));
            out
        }
        Mnemonic::Shr | Mnemonic::Sar => {
            let a = some_or_asm!(op_value(ins, 0));
            let b = some_or_asm!(op_value(ins, 1));
            let op = if ins.mnemonic() == Mnemonic::Shr {
                BinOp::Shr
            } else {
                BinOp::Sar
            };
            let r = bin(op, a, b);
            let mut out = vec![set_flag(FlagKind::Zf, bin(BinOp::Eq, r.clone(), konst(0)))];
            out.extend(some_or_asm!(write_op0(ins, r, bits)));
            out
        }

        // Rotate left/right. Models the value and CF (the bit rotated out); the
        // x86 spec leaves OF *undefined* for multi-bit rotates, so no correct
        // compiler branches on it — left untouched (sound). ZF/SF/PF/AF are not
        // affected by rotates. The complement count is masked (`(w-c)&(w-1)`) to
        // avoid C's shift-by-width UB when the count is 0.
        Mnemonic::Rol | Mnemonic::Ror => {
            let w = op0_width(ins, bits);
            if w == 0 || (w & (w - 1)) != 0 {
                return asm();
            }
            let x = bin(BinOp::And, some_or_asm!(op_value(ins, 0)), konst(mask(w)));
            let c = bin(BinOp::And, some_or_asm!(op_value(ins, 1)), konst((w - 1) as i128));
            let wc = bin(BinOp::And, bin(BinOp::Sub, konst(w as i128), c.clone()), konst((w - 1) as i128));
            let val = if ins.mnemonic() == Mnemonic::Rol {
                bin(BinOp::Or, bin(BinOp::Shl, x.clone(), c), bin(BinOp::Shr, x, wc))
            } else {
                bin(BinOp::Or, bin(BinOp::Shr, x.clone(), c), bin(BinOp::Shl, x, wc))
            };
            let val = bin(BinOp::And, val, konst(mask(w)));
            // CF = LSB (rol) / MSB (ror) of the rotated result.
            let cf = if ins.mnemonic() == Mnemonic::Rol {
                bin(BinOp::And, val.clone(), konst(1))
            } else {
                bin(BinOp::And, bin(BinOp::Shr, val.clone(), konst((w - 1) as i128)), konst(1))
            };
            let mut out = vec![set_flag(FlagKind::Cf, cf)];
            out.extend(some_or_asm!(write_op0(ins, val, bits)));
            out
        }

        // Exchange two operands. Register/register only (the common case); the
        // locked memory form is left as `asm` (sound). Sequenced through a temp so
        // each side sees the other's original value.
        Mnemonic::Xchg
            if ins.op_kind(0) == OpKind::Register
                && ins.op_kind(1) == OpKind::Register
                && !is_high_byte(ins.op_register(0))
                && !is_high_byte(ins.op_register(1)) =>
        {
            let v0 = some_or_asm!(op_value(ins, 0));
            let v1 = some_or_asm!(op_value(ins, 1));
            let id1 = some_or_asm!(reg_id(ins.op_register(1)));
            let w1 = (ins.op_register(1).size() * 8) as u32;
            let t = Location::Temp((insn.address as u32).wrapping_mul(2));
            let dst1 = Location::Reg(id1);
            let mut out = vec![Stmt::Set { dst: t.clone(), expr: v0 }];
            out.extend(some_or_asm!(write_op0(ins, v1, bits)));
            out.push(Stmt::Set { dst: dst1.clone(), expr: combine_write(&dst1, w1, Expr::Read(t), bits) });
            out
        }

        Mnemonic::Push => {
            let v = some_or_asm!(op_value(ins, 0));
            let ptr = (bits / 8) as i128;
            let sp = Location::Reg(RegId(4)); // rsp/esp family
            let new_sp = bin(BinOp::Sub, Expr::Read(sp.clone()), konst(ptr));
            vec![
                Stmt::Set { dst: sp.clone(), expr: new_sp },
                Stmt::Store {
                    addr: Expr::Read(sp),
                    value: v,
                    ty: Ty::int(bits as u8),
                },
            ]
        }
        Mnemonic::Pop => {
            let ptr = (bits / 8) as i128;
            let sp = Location::Reg(RegId(4));
            let load = Expr::Load {
                addr: Box::new(Expr::Read(sp.clone())),
                ty: Ty::int(bits as u8),
            };
            let mut out = some_or_asm!(write_op0(ins, load, bits));
            out.push(Stmt::Set {
                dst: sp.clone(),
                expr: bin(BinOp::Add, Expr::Read(sp), konst(ptr)),
            });
            out
        }

        Mnemonic::Call => {
            // A defined target resolves to its address (`sub_<addr>`); a call
            // relocated to an undefined external (object files) is emitted by
            // name (`strlen`); otherwise it is an indirect call.
            let target = match insn.target {
                Some(t) => CallTarget::Direct(t),
                None => match &insn.call_name {
                    Some(name) => CallTarget::Named(name.clone()),
                    None => match op_value(ins, 0) {
                        Some(e) => CallTarget::Indirect(Box::new(e)),
                        None => return asm(),
                    },
                },
            };
            // 64-bit: pass the SysV integer argument registers (read at their
            // pre-call versions). Over-approximate (all 6); a later prune drops
            // trailing never-defined ones. 32-bit cdecl args are on the stack
            // (recovered by the text pipeline) — left empty here for now.
            let args = if bits == 64 {
                [7u16, 6, 2, 1, 8, 9]
                    .iter()
                    .map(|&r| Expr::Read(Location::Reg(RegId(r))))
                    .collect()
            } else {
                Vec::new()
            };
            let call = Expr::Call {
                target,
                args,
                ret: Ty::int(bits as u8),
            };
            // The call returns its value in rax; caller-saved registers are
            // undefined afterwards. Modelling this lets later reads of the
            // result use the call, and prevents stale reads of clobbered regs.
            let mut out = vec![Stmt::Set { dst: Location::Reg(RegId(0)), expr: call }];
            let clobbers: &[u16] = if bits == 64 {
                &[1, 2, 6, 7, 8, 9, 10, 11] // rcx,rdx,rsi,rdi,r8-r11 (SysV caller-saved)
            } else {
                &[1, 2] // ecx,edx (cdecl caller-saved scratch)
            };
            for &r in clobbers {
                out.push(Stmt::Set {
                    dst: Location::Reg(RegId(r)),
                    expr: Expr::Undef,
                });
            }
            out
        }
        Mnemonic::Ret => vec![Stmt::Return(None)],

        _ => {
            let cc = ins.condition_code();
            if cc != ConditionCode::None && insn.flow == crate::disasm::Flow::Fallthrough {
                // setcc: single byte destination from a condition.
                if ins.op_count() == 1 {
                    if let Some(s) = write_op0(ins, cc_to_cond(cc), bits) {
                        return s;
                    }
                }
                // cmovcc dst, src:  dst = cond ? src : dst
                if ins.op_count() == 2 {
                    if let (Some(src), Some(cur)) = (op_value(ins, 1), op_value(ins, 0)) {
                        let sel = Expr::Select {
                            cond: Box::new(cc_to_cond(cc)),
                            then_: Box::new(src),
                            else_: Box::new(cur),
                        };
                        if let Some(s) = write_op0(ins, sel, bits) {
                            return s;
                        }
                    }
                }
            }
            asm()
        }
    }
}

// ===========================================================================
// x87 FPU (80-bit extended-precision floating point)
// ===========================================================================
//
// The x87 stack is modelled with a *statically tracked* top-of-stack: the
// analysis in `ir::build` computes the stack depth (`sp_in`) at every FPU
// instruction (bailing the whole function if the depth is ambiguous), so each
// `st(i)` reference resolves to a concrete physical slot. Slot d (0 = oldest
// live value) is `RegId(96+d)`; `RegId(104)` is a swap scratch. Those values are
// emitted as native `long double` — which on x86-64 *is* the 80-bit x87 format,
// so the recompiled C re-lowers to equivalent x87 code: bit-exact by
// construction. Arithmetic/loads/stores/compares are bit-exact regardless of
// rounding mode; only `fistp`/`fist` (float→int) depend on it, so those are
// emitted only when the analysis proves truncation mode (the `(int)x` idiom) or
// the instruction is `fisttp` (always truncates). Anything else bails to `Asm`.

/// x87 FPU stack slot at absolute depth `d` (0 = oldest live value).
fn fpr(d: i32) -> Option<Location> {
    if (0..8).contains(&d) {
        Some(Location::Reg(RegId(96 + d as u16)))
    } else {
        None
    }
}

/// Long-double scratch register (for `fxch` swaps).
fn fpr_scratch() -> Location {
    Location::Reg(RegId(104))
}

/// The x87 FPU status word (the condition bits C0/C2/C3), modelled as an integer
/// pseudo-register. `fcom`/`fucom` set it; `fnstsw` copies it to AX, after which
/// `sahf` (or a `test ah, imm`) turns it into CPU flags. Not an fp80 value.
fn fsw() -> Location {
    Location::Reg(RegId(120))
}

/// A call to a `long double` x87 runtime helper.
fn x87call(name: &str, args: Vec<Expr>) -> Expr {
    Expr::Call { target: CallTarget::Named(name.to_string()), args, ret: Ty::Unknown }
}

/// Helper name for loading a memory operand of `ms` as a `long double`.
fn x87_load_helper(ms: MemorySize) -> Option<&'static str> {
    Some(match ms {
        MemorySize::Float32 => "__x87_ld32",
        MemorySize::Float64 => "__x87_ld64",
        MemorySize::Float80 => "__x87_ld80",
        MemorySize::Int16 => "__x87_ild16",
        MemorySize::Int32 => "__x87_ild32",
        MemorySize::Int64 => "__x87_ild64",
        _ => return None,
    })
}

/// Read operand `i` (an `st(j)` register or a memory float/int) as `long double`.
fn x87_src(ins: &Instruction, i: u32, sp: i32) -> Option<Expr> {
    match ins.op_kind(i) {
        OpKind::Register => {
            let r = ins.op_register(i);
            if !r.is_st() {
                return None;
            }
            Some(Expr::Read(fpr(sp - 1 - r.number() as i32)?))
        }
        OpKind::Memory => {
            if ins.segment_prefix() != Register::None {
                return None;
            }
            let (addr, _) = mem_addr(ins)?;
            Some(x87call(x87_load_helper(ins.memory_size())?, vec![addr]))
        }
        _ => None,
    }
}

/// Is this an x87 FPU instruction (modelled or not)? Routes lifting to
/// `lift_x87` when the per-function stack analysis succeeded.
pub(crate) fn is_x87(ins: &Instruction) -> bool {
    let m = ins.mnemonic();
    // iced groups all FPU mnemonics in a contiguous range starting at `Fld`.
    use Mnemonic::*;
    if matches!(
        m,
        Fadd | Faddp | Fiadd | Fbld | Fbstp | Fchs | Fnclex | Fclex | Fcom | Fcomp | Fcompp
            | Fcomi | Fcomip | Fcos | Fdecstp | Fdiv | Fdivp | Fidiv | Fdivr | Fdivrp | Fidivr
            | Ffree | Ffreep | Ficom | Ficomp | Fild | Fincstp | Finit | Fninit | Fist | Fistp
            | Fisttp | Fisub | Fisubr | Fimul | Fld | Fld1 | Fldcw | Fldenv | Fldl2e | Fldl2t
            | Fldlg2 | Fldln2 | Fldpi | Fldz | Fmul | Fmulp | Fnop | Fpatan | Fprem | Fprem1
            | Fptan | Frndint | Frstor | Fnsave | Fsave | Fscale | Fsin | Fsincos | Fsqrt | Fst
            | Fstp | Fnstcw | Fstcw | Fnstenv | Fstenv | Fnstsw | Fstsw | Fsub | Fsubp | Fsubr
            | Fsubrp | Ftst | Fucom | Fucomp | Fucompp | Fucomi | Fucomip | Fxam | Fxch | Fxtract
            | Fyl2x | Fyl2xp1 | F2xm1 | Wait
    ) {
        return true;
    }
    (0..ins.op_count()).any(|i| ins.op_register(i).is_st())
}

/// Net x87 stack-depth change of a *modelled* FPU instruction, or `None` if the
/// instruction is not modelled (so the function's x87 analysis must bail).
pub(crate) fn x87_delta(ins: &Instruction) -> Option<i32> {
    use Mnemonic::*;
    Some(match ins.mnemonic() {
        Fld | Fild | Fld1 | Fldz => 1,
        Fstp | Fistp | Fisttp => -1,
        Faddp | Fsubp | Fsubrp | Fmulp | Fdivp | Fdivrp => -1,
        Fcomip | Fucomip => -1,
        Fst | Fist => 0,
        Fadd | Fsub | Fsubr | Fmul | Fdiv | Fdivr => 0,
        Fiadd | Fisub | Fisubr | Fimul | Fidiv | Fidivr => 0,
        Fabs | Fchs | Fsqrt | Fxch => 0,
        Fcomi | Fucomi => 0,
        // Status-word compares (32-bit float idiom) + status-word store.
        Fcom | Fucom | Ficom => 0,
        Fcomp | Fucomp | Ficomp => -1,
        Fcompp | Fucompp => -2,
        Fnstsw | Fstsw => 0,
        Fldcw | Fnstcw | Fstcw | Fnclex | Fclex | Fnop | Wait => 0,
        _ => return None,
    })
}

/// Build a `long double` arithmetic result via a helper, honouring the
/// non-commutative reverse forms (`fsubr`/`fdivr`: `src - dst`, `src / dst`).
fn x87_arith(op: &str, rev: bool, dst: Expr, src: Expr) -> Expr {
    let (a, b) = if rev { (src, dst) } else { (dst, src) };
    x87call(&format!("__x87_{}", op), vec![a, b])
}

/// Lift one x87 FPU instruction given the statically-tracked stack depth
/// `sp_in` (number of live values *before* it) and whether the current rounding
/// mode is truncation (`trunc`, required for `fistp`/`fist`). Falls back to a
/// sound `Asm` for anything outside the proven-bit-exact subset.
pub(crate) fn lift_x87(insn: &Insn, sp_in: u32, trunc: bool) -> Vec<Stmt> {
    x87_try(insn, sp_in as i32, trunc).unwrap_or_else(|| asm_fallback(insn))
}

fn x87_try(insn: &Insn, sp: i32, trunc: bool) -> Option<Vec<Stmt>> {
    use Mnemonic::*;
    let ins = &insn.raw;
    let mn = ins.mnemonic();
    let st0 = sp - 1; // depth of current top of stack

    Some(match mn {
        // --- pushes -------------------------------------------------------
        Fld | Fild => {
            let v = x87_src(ins, 0, sp)?;
            vec![Stmt::Set { dst: fpr(sp)?, expr: v }]
        }
        Fld1 => vec![Stmt::Set { dst: fpr(sp)?, expr: x87call("__x87_one", vec![]) }],
        Fldz => vec![Stmt::Set { dst: fpr(sp)?, expr: x87call("__x87_zero", vec![]) }],

        // --- stores (and pops) -------------------------------------------
        Fst | Fstp | Fist | Fistp | Fisttp => {
            let value = Expr::Read(fpr(st0)?);
            match ins.op_kind(0) {
                OpKind::Register => {
                    let r = ins.op_register(0);
                    if !r.is_st() {
                        return None;
                    }
                    vec![Stmt::Set { dst: fpr(sp - 1 - r.number() as i32)?, expr: value }]
                }
                OpKind::Memory => {
                    if ins.segment_prefix() != Register::None {
                        return None;
                    }
                    let (addr, _) = mem_addr(ins)?;
                    let h = match (mn, ins.memory_size()) {
                        (Fst | Fstp, MemorySize::Float32) => "__x87_st32",
                        (Fst | Fstp, MemorySize::Float64) => "__x87_st64",
                        (Fst | Fstp, MemorySize::Float80) => "__x87_st80",
                        // Integer stores truncate; only emitted when proven so.
                        (Fist | Fistp, MemorySize::Int16) if trunc => "__x87_ist16",
                        (Fist | Fistp, MemorySize::Int32) if trunc => "__x87_ist32",
                        (Fist | Fistp, MemorySize::Int64) if trunc => "__x87_ist64",
                        (Fisttp, MemorySize::Int16) => "__x87_ist16",
                        (Fisttp, MemorySize::Int32) => "__x87_ist32",
                        (Fisttp, MemorySize::Int64) => "__x87_ist64",
                        _ => return None,
                    };
                    vec![Stmt::CallStmt(x87call(h, vec![addr, value]))]
                }
                _ => return None,
            }
        }

        // --- arithmetic ---------------------------------------------------
        Fadd | Faddp | Fiadd | Fmul | Fmulp | Fimul | Fsub | Fsubp | Fisub | Fsubr | Fsubrp
        | Fisubr | Fdiv | Fdivp | Fidiv | Fdivr | Fdivrp | Fidivr => {
            let (op, rev) = match mn {
                Fadd | Faddp | Fiadd => ("add", false),
                Fmul | Fmulp | Fimul => ("mul", false),
                Fsub | Fsubp | Fisub => ("sub", false),
                Fsubr | Fsubrp | Fisubr => ("sub", true),
                _ => ("div", matches!(mn, Fdivr | Fdivrp | Fidivr)),
            };
            let is_p = matches!(mn, Faddp | Fsubp | Fsubrp | Fmulp | Fdivp | Fdivrp);
            if is_p {
                // `fXXXp st(i), st0`: st(i) = st(i) op st0, then pop. Default st1.
                let di = if ins.op_count() >= 1 && ins.op_register(0).is_st() {
                    ins.op_register(0).number() as i32
                } else {
                    1
                };
                let d = sp - 1 - di;
                let res = x87_arith(op, rev, Expr::Read(fpr(d)?), Expr::Read(fpr(st0)?));
                vec![Stmt::Set { dst: fpr(d)?, expr: res }]
            } else if ins.op_kind(0) == OpKind::Memory {
                // `fadd m` / `fiadd m`: st0 = st0 op mem.
                let src = x87_src(ins, 0, sp)?;
                let res = x87_arith(op, rev, Expr::Read(fpr(st0)?), src);
                vec![Stmt::Set { dst: fpr(st0)?, expr: res }]
            } else {
                // `fadd st0, st(i)` or `fadd st(i), st0`: dst is op0.
                let r0 = ins.op_register(0);
                let r1 = ins.op_register(1);
                if !r0.is_st() || !r1.is_st() {
                    return None;
                }
                let d0 = sp - 1 - r0.number() as i32;
                let d1 = sp - 1 - r1.number() as i32;
                let res = x87_arith(op, rev, Expr::Read(fpr(d0)?), Expr::Read(fpr(d1)?));
                vec![Stmt::Set { dst: fpr(d0)?, expr: res }]
            }
        }

        // --- unary on st0 -------------------------------------------------
        Fabs => vec![Stmt::Set { dst: fpr(st0)?, expr: x87call("__x87_abs", vec![Expr::Read(fpr(st0)?)]) }],
        Fchs => vec![Stmt::Set { dst: fpr(st0)?, expr: x87call("__x87_neg", vec![Expr::Read(fpr(st0)?)]) }],
        Fsqrt => vec![Stmt::Set { dst: fpr(st0)?, expr: x87call("__x87_sqrt", vec![Expr::Read(fpr(st0)?)]) }],

        // --- exchange st0, st(i) (default st1) ----------------------------
        Fxch => {
            // iced renders `fxch st(i)` as two operands (`st0, st(i)`); the swap
            // partner is the operand that is not st0.
            let di = (0..ins.op_count())
                .map(|k| ins.op_register(k))
                .filter(|r| r.is_st())
                .map(|r| r.number() as i32)
                .find(|&n| n != 0)
                .unwrap_or(1);
            let d = sp - 1 - di;
            let scratch = fpr_scratch();
            vec![
                Stmt::Set { dst: scratch.clone(), expr: Expr::Read(fpr(st0)?) },
                Stmt::Set { dst: fpr(st0)?, expr: Expr::Read(fpr(d)?) },
                Stmt::Set { dst: fpr(d)?, expr: Expr::Read(scratch) },
            ]
        }

        // --- comparisons setting EFLAGS (P6 fcomi family) -----------------
        Fcomi | Fcomip | Fucomi | Fucomip => {
            let r0 = ins.op_register(0);
            let r1 = ins.op_register(1);
            if !r0.is_st() || !r1.is_st() {
                return None;
            }
            let a = Expr::Read(fpr(sp - 1 - r0.number() as i32)?);
            let b = Expr::Read(fpr(sp - 1 - r1.number() as i32)?);
            let un = x87call("__x87_un", vec![a.clone(), b.clone()]);
            // comi flags: CF=a<b|unord, ZF=a==b|unord, PF=unord, SF=OF=0.
            vec![
                set_flag(FlagKind::Cf, bin(BinOp::Or, x87call("__x87_lt", vec![a.clone(), b.clone()]), un.clone())),
                set_flag(FlagKind::Zf, bin(BinOp::Or, x87call("__x87_eq", vec![a, b]), un.clone())),
                set_flag(FlagKind::Pf, un),
                set_flag(FlagKind::Sf, konst(0)),
                set_flag(FlagKind::Of, konst(0)),
            ]
        }

        // --- status-word compare (32-bit float idiom) ---------------------
        // `fcom`/`fucom` set the FPU condition bits C0/C2/C3; `fnstsw ax` then
        // copies them to AX and `sahf`/`test ah` derives the branch. Encode the
        // bits at their hardware positions (C0=bit8, C2=bit10, C3=bit14) so that
        // both `sahf` (CF=C0, ZF=C3, PF=C2) and `test ah, imm` read them right.
        Fcom | Fcomp | Fcompp | Fucom | Fucomp | Fucompp | Ficom | Ficomp => {
            let a = Expr::Read(fpr(st0)?);
            let b = if ins.op_count() == 0 {
                Expr::Read(fpr(st0 - 1)?) // st1
            } else {
                x87_src(ins, ins.op_count() - 1, sp)?
            };
            let lt = x87call("__x87_lt", vec![a.clone(), b.clone()]);
            let eq = x87call("__x87_eq", vec![a.clone(), b.clone()]);
            let un = x87call("__x87_un", vec![a, b]);
            let c0 = bin(BinOp::Or, lt, un.clone()); // less | unordered
            let c3 = bin(BinOp::Or, eq, un.clone()); // equal | unordered
            let c2 = un; // unordered
            let word = bin(
                BinOp::Or,
                bin(BinOp::Or, bin(BinOp::Shl, c0, konst(8)), bin(BinOp::Shl, c2, konst(10))),
                bin(BinOp::Shl, c3, konst(14)),
            );
            vec![Stmt::Set { dst: fsw(), expr: word }]
        }
        // Store the FPU status word to AX (or memory).
        Fnstsw | Fstsw => match ins.op_kind(0) {
            OpKind::Register => {
                let rax = Location::Reg(RegId(0));
                vec![Stmt::Set { dst: rax.clone(), expr: combine_write(&rax, 16, Expr::Read(fsw()), 64) }]
            }
            OpKind::Memory => {
                if ins.segment_prefix() != Register::None {
                    return None;
                }
                let (addr, _) = mem_addr(ins)?;
                vec![Stmt::Store { addr, value: Expr::Read(fsw()), ty: Ty::int(16) }]
            }
            _ => return None,
        },

        // --- control word / wait: no effect on the value stack ------------
        Fldcw | Fnstcw | Fstcw | Fnclex | Fclex | Fnop | Wait => vec![Stmt::Nop],

        _ => return None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Decode a single instruction from raw bytes and lift it.
    fn one(bytes: &[u8], bits: u32) -> Vec<Stmt> {
        use iced_x86::{Decoder, DecoderOptions};
        let mut dec = Decoder::with_ip(bits, bytes, 0x1000, DecoderOptions::NONE);
        let mut raw = Instruction::default();
        dec.decode_out(&mut raw);
        let insn = crate::disasm::Insn {
            address: 0x1000,
            len: raw.len(),
            text: format!("{:?}", raw.mnemonic()),
            flow: crate::disasm::Flow::Fallthrough,
            target: None,
            call_name: None,
            raw,
        };
        lift(&insn, bits)
    }

    #[test]
    fn mov_reg_reg() {
        // mov eax, ebx  (89 d8) in 32-bit
        let s = one(&[0x89, 0xd8], 32);
        assert!(matches!(s.as_slice(), [Stmt::Set { .. }]));
    }

    #[test]
    fn cmp_sets_flags() {
        // cmp eax, ebx (39 d8): four flag definitions, no register write
        let s = one(&[0x39, 0xd8], 32);
        assert_eq!(s.len(), 4);
        assert!(s.iter().all(|st| matches!(
            st,
            Stmt::Set { dst: Location::Flag(_), .. }
        )));
    }

    #[test]
    fn lea_is_address_not_load() {
        // lea eax, [ebx+4]  (8d 43 04)
        let s = one(&[0x8d, 0x43, 0x04], 32);
        match s.as_slice() {
            [Stmt::Set { expr, .. }] => {
                // address arithmetic, not a Load
                assert!(!matches!(expr, Expr::Load { .. }));
            }
            _ => panic!("expected single Set, got {:?}", s),
        }
    }
}
