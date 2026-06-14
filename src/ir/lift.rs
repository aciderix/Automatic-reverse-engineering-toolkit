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
    ConditionCode, Instruction, InstructionInfoFactory, Mnemonic, OpAccess, OpKind, Register,
    RflagsBits,
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
fn read_reg(r: Register) -> Option<Expr> {
    if is_high_byte(r) {
        return None; // model partial high-byte reads later
    }
    let id = reg_id(r)?;
    let w = (r.size() * 8) as u32;
    let full = Expr::Read(Location::Reg(id));
    if w >= 64 {
        Some(full)
    } else {
        Some(Expr::Binary(BinOp::And, Box::new(full), Box::new(konst(mask(w)))))
    }
}

/// If operand `i` is a pure frame slot `[ebp/rbp ± disp]` (base = frame pointer,
/// no index), return its displacement. These become named `Frame` locations.
fn frame_disp(ins: &Instruction, i: u32) -> Option<i64> {
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
            if is_high_byte(r) {
                return None;
            }
            let id = reg_id(r)?;
            let w = (r.size() * 8) as u32;
            let dst = Location::Reg(id);
            let expr = combine_write(&dst, w, value, bits);
            Some(vec![Stmt::Set { dst, expr }])
        }
        OpKind::Memory => {
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
        Mnemonic::Mul | Mnemonic::Div | Mnemonic::Idiv if ins.op_count() == 1 => {
            if op0_width(ins, bits) != 32 {
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
            let target = match insn.target {
                Some(t) => CallTarget::Direct(t),
                None => match op_value(ins, 0) {
                    Some(e) => CallTarget::Indirect(Box::new(e)),
                    None => return asm(),
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
