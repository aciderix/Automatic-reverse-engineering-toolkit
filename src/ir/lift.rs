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
use iced_x86::{Instruction, Mnemonic, OpKind, Register};

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

/// Build the address expression and access size (bits) of a memory operand.
fn mem_addr(ins: &Instruction) -> Option<(Expr, u32)> {
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
    let asm = || vec![Stmt::Asm(insn.text.clone())];

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
            let (addr, _) = some_or_asm!(mem_addr(ins));
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
            vec![Stmt::CallStmt(Expr::Call {
                target,
                args: Vec::new(),
                ret: Ty::int(bits as u8),
            })]
        }
        Mnemonic::Ret => vec![Stmt::Return(None)],

        _ => asm(),
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
