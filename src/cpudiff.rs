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

use crate::ir::types::{BinOp, CallTarget, Expr, FlagKind, IrFunction, Location, Stmt, Ty, UnOp};
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
    /// Whole-function mode (funcdiff): the mapped address space mirrored into
    /// Unicorn (PE image + a stack). When non-empty, `mem_read`/`mem_write` use
    /// these regions and the scratch page above is unused. A read/write outside
    /// every region returns `None` → the state is skipped, never a false positive.
    regions: Vec<Region>,
    /// Stack addresses at which a recursed call pushed a return-address sentinel
    /// (closure mode). Those 4-byte slots hold different bytes than Unicorn (which
    /// pushes the real return address) and are excluded from the memory diff — they
    /// are ABI plumbing, not a lift-correctness signal.
    ret_slots: Vec<u64>,
    /// SSA value store (post-opt IR mode): `ValueId.0 → value`. A `Use(v)` reads
    /// it; an `Assign` writes it. Empty in pre-SSA mode (no `Use` nodes exist).
    vids: HashMap<u32, u64>,
    /// The block we arrived from (post-opt IR mode), to resolve φ arguments by
    /// predecessor position.
    prev_block: Option<u32>,
}

/// A mapped memory region for whole-function differencing, mirrored byte-for-byte
/// into Unicorn so both engines start identical.
#[derive(Clone)]
struct Region {
    base: u64,
    data: Vec<u8>,
    writable: bool,
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
            regions: Vec::new(),
            ret_slots: Vec::new(),
            vids: HashMap::new(),
            prev_block: None,
        }
    }

    /// Index of the region containing `[addr, addr+n)` wholly, if any.
    fn region_of(&self, addr: u64, n: usize) -> Option<usize> {
        self.regions.iter().position(|r| {
            addr >= r.base && (addr - r.base) as usize + n <= r.data.len()
        })
    }

    /// Read `n` little-endian bytes. In whole-function mode reads the mapped
    /// regions; otherwise the single-instruction scratch page. `None` (→ skip)
    /// when the access falls outside every mapped span.
    fn mem_read(&self, addr: u64, n: usize) -> Option<u64> {
        let (buf, off) = if !self.regions.is_empty() {
            let ri = self.region_of(addr, n)?;
            (&self.regions[ri].data, (addr - self.regions[ri].base) as usize)
        } else {
            let off = addr.checked_sub(DATA_ADDR)? as usize;
            if off + n > self.mem.len() {
                return None;
            }
            (&self.mem, off)
        };
        let mut v = 0u64;
        for i in 0..n {
            v |= (buf[off + i] as u64) << (8 * i);
        }
        Some(v)
    }

    fn mem_write(&mut self, addr: u64, n: usize, val: u64) -> Option<()> {
        if !self.regions.is_empty() {
            let ri = self.region_of(addr, n)?;
            if !self.regions[ri].writable {
                return None; // a store into read-only image memory: not modelled here
            }
            let off = (addr - self.regions[ri].base) as usize;
            for i in 0..n {
                self.regions[ri].data[off + i] = (val >> (8 * i)) as u8;
            }
            return Some(());
        }
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
            // Post-SSA read of a versioned value. Unbound (an entry version we did
            // not seed, or a value the SSA interpreter never assigned) → skip.
            Expr::Use(v) => match self.vids.get(&v.0) {
                Some(&x) => x,
                None => return None,
            },
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

    /// Write a 64-bit value to a modelled location; `None` (→ skip) for a
    /// `Frame`/`Mem` location the integer interpreter does not track.
    fn write_loc(&mut self, dst: &Location, v: u64) -> Option<()> {
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

    /// Execute one lifted statement; `None` if it touches the unmodelled world.
    fn exec(&mut self, s: &Stmt) -> Option<()> {
        match s {
            Stmt::Set { dst, expr } => {
                let v = self.eval(expr)?;
                self.write_loc(dst, v)
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

    /// Interpret a whole lifted function's blocks, following control flow to a
    /// `Return` and *into* directly-called recovered callees (the closure). The
    /// returned `Option<u64>` is the callee's combined return value on a clean
    /// return, or `None` (→ skip the state, never a false positive) on any
    /// construct not modelled faithfully: an indirect/unmodelled call, a switch,
    /// an `Asm` safety valve, an out-of-region access, a missing block, an over-
    /// budget run (suspected model-side infinite loop), or exceeded recursion.
    ///
    /// `depth` counts recursion (0 = the top-level function under test). `budget`
    /// is shared across the whole recursion so total work stays bounded.
    fn run_closure(
        &mut self,
        irf: &IrFunction,
        ctx: &ClosureCtx,
        depth: u32,
        budget: &mut u32,
    ) -> Option<u64> {
        // Entry block: the one at the function entry address, else the first.
        let mut cur = irf
            .blocks
            .iter()
            .find(|b| b.addr == irf.entry)
            .or_else(|| irf.blocks.first())?
            .id;
        loop {
            *budget = budget.checked_sub(1)?;
            let blk = irf.blocks.iter().find(|b| b.id == cur)?;
            let mut next: Option<u32> = None;
            for s in &blk.stmts {
                match s {
                    Stmt::Branch { cond, taken, fallthrough } => {
                        let c = self.eval(cond)?;
                        next = Some(if c != 0 { taken.0 } else { fallthrough.0 });
                    }
                    Stmt::Jump(t) => next = Some(t.0),
                    // A tail call (`jmp target`, lifted `Return(Some(Call))`)
                    // pushes no return address; the callee's own `ret` returns for
                    // us. We only follow it at the top level (depth 0): nested, the
                    // pop that the *entering* call must apply becomes the callee's
                    // `ret N`, not this function's — ambiguous, so skip (sound).
                    Stmt::Return(Some(Expr::Call { target: CallTarget::Direct(t), .. })) => {
                        if depth != 0 {
                            return None;
                        }
                        let callee = *ctx.funcs.get(t)?;
                        return self.run_closure(callee, ctx, depth + 1, budget);
                    }
                    Stmt::Return(Some(e)) => return self.eval_or_call(e, ctx, depth, budget),
                    Stmt::Return(None) => return None, // no modelled return value
                    // A statement-position call: perform it for its side effects
                    // (recursing into a recovered callee), ignoring the result.
                    Stmt::CallStmt(e) => {
                        self.eval_or_call(e, ctx, depth, budget)?;
                    }
                    Stmt::Set { dst, expr } => {
                        // A post-call clobber `Set{ecx, Undef}` (and the SysV set)
                        // is a conservative ABI model: correct code never reads a
                        // caller-saved register across a call. Recursion already
                        // left the callee's *real* value there (= Unicorn's), so
                        // keep it — dropping the assignment is both sound and what
                        // makes call-bearing functions scorable at all.
                        if matches!(expr, Expr::Undef) {
                            continue;
                        }
                        let v = self.eval_or_call(expr, ctx, depth, budget)?;
                        self.write_loc(dst, v)?;
                    }
                    // Unmodelled control/effects → skip the whole function.
                    Stmt::Switch { .. } | Stmt::Asm(_) => return None,
                    other => self.exec(other)?,
                }
            }
            cur = next?; // a block with no follow-able terminator → skip
        }
    }

    /// Evaluate `e`, performing a *direct* call to a recovered callee if `e` is
    /// one (returning its combined return value). Any other call form (indirect,
    /// an unmodelled named import, or a call nested inside a larger expression)
    /// falls through to `eval`, which returns `None` → skip.
    fn eval_or_call(
        &mut self,
        e: &Expr,
        ctx: &ClosureCtx,
        depth: u32,
        budget: &mut u32,
    ) -> Option<u64> {
        if let Expr::Call { target: CallTarget::Direct(t), .. } = e {
            return self.call_direct(*t, ctx, depth, budget);
        }
        self.eval(e)
    }

    /// Recurse into a directly-called recovered callee, modelling the hardware
    /// call/return stack discipline exactly so the shared memory + registers stay
    /// byte-identical to Unicorn.
    ///
    /// At the call site `esp = S` (arguments already pushed by preceding stores).
    /// The hardware `call` pushes the return address (`esp = S-4`); the callee
    /// runs and restores `esp` to its entry value before `ret N`, which pops the
    /// return address and `N` argument bytes (`esp = S+N`). We reproduce exactly
    /// that net effect and record the sentinel slot for the memory diff to skip.
    fn call_direct(
        &mut self,
        t: u64,
        ctx: &ClosureCtx,
        depth: u32,
        budget: &mut u32,
    ) -> Option<u64> {
        if depth >= CLOSURE_DEPTH {
            return None;
        }
        FUNCDIFF_CALLS.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let callee = *ctx.funcs.get(&t)?; // must be a recovered function
        let pop = *ctx.ret_pops.get(&t)?; // must have a known, unambiguous `ret N`
        let s = self.regs[4];
        let slot = s.wrapping_sub(4);
        self.mem_write(slot, 4, FN_RET_SENTINEL)?; // must land in the stack region
        self.ret_slots.push(slot);
        self.regs[4] = slot; // callee entry esp (return address at [esp])
        let rv = self.run_closure(callee, ctx, depth + 1, budget)?;
        self.regs[4] = s.wrapping_add(pop as u64); // net: -4 (push) +4+N (ret N)
        Some(rv)
    }

    // ---- post-opt SSA interpreter (optimizer differential) ---------------

    /// Seed the SSA value store with the entry (undef) versions of registers and
    /// flags from the initial CPU state, so a `Use` of an entry version reads the
    /// same input the pre-SSA interpreter read from `regs`/`flags`. Entry versions
    /// of anything else (xmm/fp80/temp) are left unseeded — a `Use` of them then
    /// returns `None` (skip), never a wrong value.
    fn seed_entry_values(&mut self, irf: &IrFunction) {
        for (loc, vid) in &irf.entry_values {
            let v = match loc {
                Location::Reg(r) if (r.0 as usize) < 8 => self.regs[r.0 as usize],
                Location::Flag(f) => self.flags.get(f).copied().unwrap_or(0),
                _ => continue, // xmm/fp80/temp/frame: unseeded → Use → skip
            };
            self.vids.insert(*vid, v);
        }
    }

    /// Interpret a whole post-opt SSA function, following control flow to a
    /// `Return` and returning its value. φ-nodes are resolved by the predecessor
    /// we arrived from (`prev_block`). `None` (→ skip, never a false verdict) on
    /// any unmodelled construct, an unbound value, an out-of-region access, an
    /// over-budget run, or a φ reached with no known predecessor (entry-block φ).
    ///
    /// This is a leaf interpreter (calls are rejected by the caller's `is_leaf`
    /// gate); the closure across calls is a later increment.
    fn run_ssa(&mut self, irf: &IrFunction, budget: &mut u32) -> Option<u64> {
        let mut cur = irf
            .blocks
            .iter()
            .find(|b| b.addr == irf.entry)
            .or_else(|| irf.blocks.first())?
            .id;
        self.prev_block = None;
        loop {
            *budget = budget.checked_sub(1)?;
            let blk = irf.blocks.iter().find(|b| b.id == cur)?;
            let mut next: Option<u32> = None;
            for s in &blk.stmts {
                match s {
                    // φ: pick the argument for the predecessor we came from. Its
                    // value is defined in that predecessor → already in `vids`.
                    Stmt::Assign { dst, expr: Expr::Phi(args) } => {
                        let prev = self.prev_block?;
                        let idx = blk.pred.iter().position(|&p| p == prev)?;
                        let arg = args.get(idx)?;
                        let v = self.vids.get(&arg.0).copied()?;
                        self.vids.insert(dst.0, v);
                    }
                    Stmt::Assign { dst, expr } => {
                        let v = self.eval(expr)?;
                        self.vids.insert(dst.0, v);
                    }
                    Stmt::Store { addr, value, ty } => {
                        let a = self.eval(addr)?;
                        let v = self.eval(value)?;
                        self.mem_write(a, ty_bytes(ty)?, v)?;
                    }
                    Stmt::Branch { cond, taken, fallthrough } => {
                        let c = self.eval(cond)?;
                        next = Some(if c != 0 { taken.0 } else { fallthrough.0 });
                    }
                    Stmt::Jump(t) => next = Some(t.0),
                    Stmt::Return(Some(e)) => return self.eval(e),
                    Stmt::Return(None) => return None,
                    Stmt::Nop => {}
                    // A call/switch/asm: rejected by `is_leaf`; guard anyway.
                    Stmt::Switch { .. } | Stmt::CallStmt(_) | Stmt::Asm(_) => return None,
                    // Pre-SSA `Set` should not appear post-SSA; skip if it does.
                    Stmt::Set { .. } => return None,
                }
            }
            self.prev_block = Some(cur);
            cur = next?;
        }
    }
}

/// Context for closure-mode interpretation: the recovered functions keyed by
/// entry (to follow direct calls into) and their `ret N` pop counts.
pub struct ClosureCtx<'a> {
    funcs: &'a HashMap<u64, &'a IrFunction>,
    ret_pops: &'a HashMap<u64, i64>,
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
/// Apply `f` to each of the two f32 lanes packed in a 64-bit half, returning the
/// repacked bit pattern (host f32 ops match Unicorn's softfloat bit-for-bit).
fn ps_map2(a: u64, b: u64, f: impl Fn(f32, f32) -> f32) -> u64 {
    let lo = f(f32::from_bits(a as u32), f32::from_bits(b as u32)).to_bits();
    let hi = f(f32::from_bits((a >> 32) as u32), f32::from_bits((b >> 32) as u32)).to_bits();
    lo as u64 | (hi as u64) << 32
}

/// One `cmpps` lane: predicate `p & 7` (eq/lt/le/unord and their negations) →
/// all-ones or all-zeros, mirroring the C `__ps_cmp1` helper.
fn ps_cmp1(a: u32, b: u32, p: u64) -> u32 {
    let (x, y) = (f32::from_bits(a), f32::from_bits(b));
    let r = match p & 7 {
        0 => x == y,
        1 => x < y,
        2 => x <= y,
        3 => x.is_nan() || y.is_nan(),
        4 => x != y,
        5 => !(x < y),
        6 => !(x <= y),
        _ => !(x.is_nan() || y.is_nan()),
    };
    if r { 0xffff_ffff } else { 0 }
}

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

        // ---- packed-integer SIMD (lanes within the two 64-bit halves) ----
        "__pi_add32" => {
            let l = (a[0] as u32).wrapping_add(a[1] as u32);
            let h = ((a[0] >> 32) as u32).wrapping_add((a[1] >> 32) as u32);
            return Some(l as u64 | (h as u64) << 32);
        }
        "__pi_sub32" => {
            let l = (a[0] as u32).wrapping_sub(a[1] as u32);
            let h = ((a[0] >> 32) as u32).wrapping_sub((a[1] >> 32) as u32);
            return Some(l as u64 | (h as u64) << 32);
        }
        "__pi_eq32" => {
            let l = if a[0] as u32 == a[1] as u32 { !0u32 } else { 0 };
            let h = if (a[0] >> 32) as u32 == (a[1] >> 32) as u32 { !0u32 } else { 0 };
            return Some(l as u64 | (h as u64) << 32);
        }
        "__pi_gt32" => {
            let l = if (a[0] as i32) > (a[1] as i32) { !0u32 } else { 0 };
            let h = if ((a[0] >> 32) as i32) > ((a[1] >> 32) as i32) { !0u32 } else { 0 };
            return Some(l as u64 | (h as u64) << 32);
        }
        "__pi_muludq" => return Some((a[0] & 0xffff_ffff) * (a[1] & 0xffff_ffff)),
        "__pi_unpckldq_lo" => return Some((a[0] & 0xffff_ffff) | ((a[1] & 0xffff_ffff) << 32)),
        "__pi_unpckldq_hi" => return Some((a[0] >> 32) | ((a[1] >> 32) << 32)),
        "__pi_shuf_lo" | "__pi_shuf_hi" => {
            let (lo, hi, m) = (a[0], a[1], a[2]);
            let lane = |k: u64| (if k < 2 { lo } else { hi }) >> ((k & 1) * 32) & 0xffff_ffff;
            let (s0, s1) = if name == "__pi_shuf_lo" {
                (m & 3, (m >> 2) & 3)
            } else {
                ((m >> 4) & 3, (m >> 6) & 3)
            };
            return Some(lane(s0) | (lane(s1) << 32));
        }
        "__pi_eq8" => {
            let mut r = 0u64;
            for i in (0..64).step_by(8) {
                if (a[0] >> i) as u8 == (a[1] >> i) as u8 {
                    r |= 0xffu64 << i;
                }
            }
            return Some(r);
        }
        "__pi_eq16" => {
            let mut r = 0u64;
            for i in (0..64).step_by(16) {
                if (a[0] >> i) as u16 == (a[1] >> i) as u16 {
                    r |= 0xffffu64 << i;
                }
            }
            return Some(r);
        }
        "__pi_gt8" => {
            let mut r = 0u64;
            for i in (0..64).step_by(8) {
                if ((a[0] >> i) as i8) > ((a[1] >> i) as i8) {
                    r |= 0xffu64 << i;
                }
            }
            return Some(r);
        }
        "__pi_shufw" => {
            let (x, imm) = (a[0], a[1]);
            let mut r = 0u64;
            for i in 0..4 {
                let sel = (imm >> (i * 2)) & 3;
                r |= ((x >> (sel * 16)) & 0xffff) << (i * 16);
            }
            return Some(r);
        }
        "__pi_mskb" => {
            let (lo, hi) = (a[0], a[1]);
            let mut m = 0u64;
            for i in 0..8 {
                m |= ((lo >> (i * 8 + 7)) & 1) << i;
                m |= ((hi >> (i * 8 + 7)) & 1) << (i + 8);
            }
            return Some(m);
        }
        "__pi_add16" => {
            let mut r = 0u64;
            for i in (0..64).step_by(16) {
                let s = ((a[0] >> i) as u16).wrapping_add((a[1] >> i) as u16);
                r |= (s as u64) << i;
            }
            return Some(r);
        }
        "__pi_gt16" => {
            let mut r = 0u64;
            for i in (0..64).step_by(16) {
                if ((a[0] >> i) as i16) > ((a[1] >> i) as i16) {
                    r |= 0xffffu64 << i;
                }
            }
            return Some(r);
        }
        "__pi_subus16" => {
            let mut r = 0u64;
            for i in (0..64).step_by(16) {
                let (x, y) = ((a[0] >> i) as u16, (a[1] >> i) as u16);
                r |= (x.saturating_sub(y) as u64) << i;
            }
            return Some(r);
        }

        // ---- packed single-precision float (two f32 lanes per 64-bit half) ----
        "__ps_add" => return Some(ps_map2(a[0], a[1], |x, y| x + y)),
        "__ps_sub" => return Some(ps_map2(a[0], a[1], |x, y| x - y)),
        "__ps_mul" => return Some(ps_map2(a[0], a[1], |x, y| x * y)),
        "__ps_div" => return Some(ps_map2(a[0], a[1], |x, y| x / y)),
        "__ps_min" | "__ps_max" => {
            // x86 min/max return the *source* lane's original bits on NaN/equal:
            // min = a<b ? a : b, max = a>b ? a : b.
            let pick = |la: u32, lb: u32| -> u32 {
                let (fa, fb) = (f32::from_bits(la), f32::from_bits(lb));
                let take_a = if name == "__ps_min" { fa < fb } else { fa > fb };
                if take_a { la } else { lb }
            };
            let l = pick(a[0] as u32, a[1] as u32);
            let h = pick((a[0] >> 32) as u32, (a[1] >> 32) as u32);
            return Some(l as u64 | (h as u64) << 32);
        }
        "__ps_sqrt" => {
            let l = f32::from_bits(a[0] as u32).sqrt().to_bits();
            let h = f32::from_bits((a[0] >> 32) as u32).sqrt().to_bits();
            return Some(l as u64 | (h as u64) << 32);
        }
        "__ps_cvtdq" => {
            let l = ((a[0] as i32) as f32).to_bits();
            let h = (((a[0] >> 32) as i32) as f32).to_bits();
            return Some(l as u64 | (h as u64) << 32);
        }
        "__ps_cmp" => {
            let l = ps_cmp1(a[0] as u32, a[1] as u32, a[2]);
            let h = ps_cmp1((a[0] >> 32) as u32, (a[1] >> 32) as u32, a[2]);
            return Some(l as u64 | (h as u64) << 32);
        }
        "__ps_movmsk" => {
            return Some(
                ((a[0] >> 31) & 1)
                    | (((a[0] >> 63) & 1) << 1)
                    | (((a[1] >> 31) & 1) << 2)
                    | (((a[1] >> 63) & 1) << 3),
            );
        }
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
        vec![0x86, 0xe0], // xchg al, ah (high-byte swap, used by hex()/endian helpers)
        vec![0x87, 0xc8], // xchg eax, ecx
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
        // ---- packed SIMD (xmm0, xmm1 — both 128-bit lanes compared) ----
        // packed-integer (66 0F): lane-wise add/sub/compare/mul, bitwise, unpack
        vec![0x66, 0x0f, 0xfe, 0xc1], // paddd     xmm0, xmm1
        vec![0x66, 0x0f, 0xfa, 0xc1], // psubd     xmm0, xmm1
        vec![0x66, 0x0f, 0xfd, 0xc1], // paddw     xmm0, xmm1
        vec![0x66, 0x0f, 0x76, 0xc1], // pcmpeqd   xmm0, xmm1
        vec![0x66, 0x0f, 0x66, 0xc1], // pcmpgtd   xmm0, xmm1
        vec![0x66, 0x0f, 0x65, 0xc1], // pcmpgtw   xmm0, xmm1
        vec![0x66, 0x0f, 0xd9, 0xc1], // psubusw   xmm0, xmm1
        vec![0x66, 0x0f, 0xf4, 0xc1], // pmuludq   xmm0, xmm1
        vec![0x66, 0x0f, 0xdb, 0xc1], // pand      xmm0, xmm1
        vec![0x66, 0x0f, 0xdf, 0xc1], // pandn     xmm0, xmm1
        vec![0x66, 0x0f, 0xeb, 0xc1], // por       xmm0, xmm1
        vec![0x66, 0x0f, 0xef, 0xc1], // pxor      xmm0, xmm1
        vec![0x66, 0x0f, 0x62, 0xc1], // punpckldq xmm0, xmm1
        vec![0x66, 0x0f, 0x6a, 0xc1], // punpckhdq xmm0, xmm1
        vec![0x66, 0x0f, 0x6c, 0xc1], // punpcklqdq xmm0, xmm1
        vec![0x66, 0x0f, 0x6d, 0xc1], // punpckhqdq xmm0, xmm1
        vec![0x66, 0x0f, 0x70, 0xc1, 0x1b], // pshufd xmm0, xmm1, 0x1b
        // SSE2 string-scan ops (byte/word compare, byte mask, word shuffle)
        vec![0x66, 0x0f, 0x74, 0xc1],       // pcmpeqb  xmm0, xmm1
        vec![0x66, 0x0f, 0x75, 0xc1],       // pcmpeqw  xmm0, xmm1
        vec![0x66, 0x0f, 0x64, 0xc1],       // pcmpgtb  xmm0, xmm1
        vec![0x66, 0x0f, 0xd7, 0xc1],       // pmovmskb eax, xmm1
        vec![0xf2, 0x0f, 0x70, 0xc1, 0x1b], // pshuflw  xmm0, xmm1, 0x1b
        vec![0xf3, 0x0f, 0x70, 0xc1, 0x1b], // pshufhw  xmm0, xmm1, 0x1b
        // packed single-precision float (0F): arith, min/max, sqrt, cvt, bitwise,
        // compare, unpack, movmask (GP result)
        vec![0x0f, 0x58, 0xc1],       // addps     xmm0, xmm1
        vec![0x0f, 0x5c, 0xc1],       // subps     xmm0, xmm1
        vec![0x0f, 0x59, 0xc1],       // mulps     xmm0, xmm1
        vec![0x0f, 0x5e, 0xc1],       // divps     xmm0, xmm1
        vec![0x0f, 0x5d, 0xc1],       // minps     xmm0, xmm1
        vec![0x0f, 0x5f, 0xc1],       // maxps     xmm0, xmm1
        vec![0x0f, 0x51, 0xc1],       // sqrtps    xmm0, xmm1
        vec![0x0f, 0x5b, 0xc1],       // cvtdq2ps  xmm0, xmm1
        vec![0x0f, 0x54, 0xc1],       // andps     xmm0, xmm1
        vec![0x0f, 0x55, 0xc1],       // andnps    xmm0, xmm1
        vec![0x0f, 0x56, 0xc1],       // orps      xmm0, xmm1
        vec![0x0f, 0x57, 0xc1],       // xorps     xmm0, xmm1
        vec![0x0f, 0xc2, 0xc1, 0x02], // cmpleps   xmm0, xmm1
        vec![0x0f, 0x14, 0xc1],       // unpcklps  xmm0, xmm1
        vec![0x0f, 0x15, 0xc1],       // unpckhps  xmm0, xmm1
        vec![0x0f, 0x50, 0xc1],       // movmskps  eax, xmm1
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

// ==== whole-function differential (funcdiff) ===============================
//
// Extends the per-instruction oracle to whole recovered FUNCTIONS of a real
// binary: run the function's bytes in Unicorn and its lifted IR in the
// interpreter from an identical register + memory state, and compare the final
// registers and memory. A divergence is a real *lift* bug — e.g. a store dropped
// at lift: Unicorn executes it, the IR lacks it, so the memory differs. Sound by
// construction: a call / switch / unmodelled instruction / out-of-image access /
// Unicorn fault / non-return all make the state skipped — never a false verdict.
// Leaf functions only for now (a call → skip); the call-closure extension (to
// reach functions like busybox's regex engine) is the documented next step.

/// Count of fully-scored (interp+Unicorn both cleanly returned) iterations —
/// diagnostic, so a run can confirm it exercised functions rather than skipping.
pub static FUNCDIFF_SCORED: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
/// Count of direct calls the closure interpreter actually followed into a
/// recovered callee — the signal that the *closure* path (not just leaf scoring)
/// was exercised. A guard asserts this is non-zero across the fixture corpus.
pub static FUNCDIFF_CALLS: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
/// Count of iterations scored by the *optimizer* differential (pre-opt vs post-opt
/// SSA both cleanly returned) — non-vacuity signal for that path.
pub static FUNCDIFF_OPT_SCORED: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

const FN_STACK_BASE: u64 = 0x1000_0000;
const FN_STACK_SIZE: usize = 0x1_0000;
const FN_SENTINEL: u64 = 0xdead_0000; // unmapped return address: Unicorn stops here
/// The return address the interpreter pushes for a *recursed* (closure) call.
/// Deliberately unmapped (well above any image or the stack): a callee that
/// derefs it (a get-pc thunk reading `[esp]`) faults `mem_read → None → skip`
/// rather than diverging silently. Its stack slot is excluded from the memory
/// comparison (Unicorn writes the real return address there); a residual leak
/// into a compared register is caught by an explicit guard in `diff_function`.
const FN_RET_SENTINEL: u64 = 0xdead_1000;
/// Cap on interpreter call-recursion depth (guards the *harness's* Rust stack;
/// exceeding it → skip, never a false verdict).
const CLOSURE_DEPTH: u32 = 200;

/// A per-function divergence between the lifted IR and Unicorn.
pub struct FnMismatch {
    pub func: u64,
    pub what: String,
    pub lifted: u64,
    pub unicorn: u64,
}

/// The runtime helpers the interpreter models by value (see `helper_call`): the
/// integer-division, packed-integer, packed-single and scalar-float families. A
/// named call to anything else (an import shim, an x87 runtime helper, the `asm:`
/// safety valve) has effects the interpreter cannot reproduce → its function is
/// not closure-modelable. A prefix hit that `helper_call` does not actually model
/// still returns `None` at runtime (skip), so this over-approximation is sound.
fn is_modeled_helper(name: &str) -> bool {
    name.starts_with("__ix_")
        || name.starts_with("__pi_")
        || name.starts_with("__ps_")
        || name.starts_with("__fp_")
}

/// Check every call inside `e` is closure-modelable, collecting the recovered
/// direct-call targets. `None` if any call is indirect, an unmodelled named
/// import, or a direct call to a function we cannot recover / whose `ret N` we
/// do not know.
fn check_expr_calls(
    e: &Expr,
    funcs: &HashMap<u64, &IrFunction>,
    ret_pops: &HashMap<u64, i64>,
    targets: &mut Vec<u64>,
) -> Option<()> {
    match e {
        Expr::Call { target, args, .. } => {
            match target {
                CallTarget::Direct(t) => {
                    if !funcs.contains_key(t) || !ret_pops.contains_key(t) {
                        return None;
                    }
                    targets.push(*t);
                }
                CallTarget::Named(n) => {
                    if !is_modeled_helper(n) {
                        return None;
                    }
                }
                CallTarget::Indirect(_) => return None,
            }
            for a in args {
                check_expr_calls(a, funcs, ret_pops, targets)?;
            }
        }
        Expr::Unary(_, a) | Expr::Cast { expr: a, .. } => check_expr_calls(a, funcs, ret_pops, targets)?,
        Expr::Binary(_, a, b) => {
            check_expr_calls(a, funcs, ret_pops, targets)?;
            check_expr_calls(b, funcs, ret_pops, targets)?;
        }
        Expr::Load { addr, .. } => check_expr_calls(addr, funcs, ret_pops, targets)?,
        Expr::Select { cond, then_, else_ } => {
            check_expr_calls(cond, funcs, ret_pops, targets)?;
            check_expr_calls(then_, funcs, ret_pops, targets)?;
            check_expr_calls(else_, funcs, ret_pops, targets)?;
        }
        _ => {}
    }
    Some(())
}

/// The recovered direct callees of `irf`, or `None` if `irf` contains a construct
/// the closure interpreter cannot model (switch, `asm`, an unfollowable call).
fn fn_local_targets(
    irf: &IrFunction,
    funcs: &HashMap<u64, &IrFunction>,
    ret_pops: &HashMap<u64, i64>,
) -> Option<Vec<u64>> {
    if irf.blocks.is_empty() {
        return None;
    }
    let mut targets = Vec::new();
    for b in &irf.blocks {
        for s in &b.stmts {
            match s {
                Stmt::Switch { .. } | Stmt::Asm(_) => return None,
                // A tail call needs only that its target is recovered (no pop is
                // applied — the callee returns for us); followed at depth 0 only.
                Stmt::Return(Some(Expr::Call { target: CallTarget::Direct(t), .. })) => {
                    if !funcs.contains_key(t) {
                        return None;
                    }
                    targets.push(*t);
                }
                Stmt::Return(Some(e)) | Stmt::CallStmt(e) | Stmt::Set { expr: e, .. } => {
                    check_expr_calls(e, funcs, ret_pops, &mut targets)?;
                }
                Stmt::Store { addr, value, .. } => {
                    check_expr_calls(addr, funcs, ret_pops, &mut targets)?;
                    check_expr_calls(value, funcs, ret_pops, &mut targets)?;
                }
                Stmt::Branch { cond, .. } => check_expr_calls(cond, funcs, ret_pops, &mut targets)?,
                _ => {}
            }
        }
    }
    Some(targets)
}

/// A function whose whole direct-call closure the interpreter can attempt: every
/// function reached is locally modelable (`fn_local_targets`). Others are skipped
/// (sound). This is the static gate that keeps Unicorn setup off hopeless
/// functions; the per-statement `None` in `run_closure` is the real safety net.
fn is_closure_modelable(
    entry: u64,
    funcs: &HashMap<u64, &IrFunction>,
    ret_pops: &HashMap<u64, i64>,
) -> bool {
    let mut seen = std::collections::HashSet::new();
    let mut stack = vec![entry];
    while let Some(a) = stack.pop() {
        if !seen.insert(a) {
            continue;
        }
        let irf = match funcs.get(&a) {
            Some(f) => f,
            None => return false,
        };
        match fn_local_targets(irf, funcs, ret_pops) {
            Some(ts) => {
                for t in ts {
                    if !seen.contains(&t) {
                        stack.push(t);
                    }
                }
            }
            None => return false,
        }
    }
    true
}

/// Per-function `ret N` pop count (0 for a plain `ret`), for the functions whose
/// return is a single, unambiguous near return. A function with no such return
/// (tail-jump / no-return only) or with conflicting `ret N` counts is absent —
/// calls to it are then not followed (skip), never modelled with a wrong esp.
fn compute_ret_pops(functions: &[crate::analysis::Function]) -> HashMap<u64, i64> {
    use crate::disasm::Flow;
    let mut m = HashMap::new();
    for f in functions {
        let mut pop: Option<i64> = None;
        let mut ok = true;
        let mut saw = false;
        for b in f.blocks.values() {
            if !matches!(b.terminator, Flow::Return) {
                continue;
            }
            let last = match b.insns.last() {
                Some(i) => i,
                None => {
                    ok = false;
                    break;
                }
            };
            // `ret N` carries the pop as its sole immediate operand; a bare `ret`
            // has none (pop 0).
            let n = if last.raw.op_count() > 0 {
                last.raw.immediate(0) as i64
            } else {
                0
            };
            saw = true;
            match pop {
                None => pop = Some(n),
                Some(p) if p == n => {}
                Some(_) => {
                    ok = false;
                    break;
                }
            }
        }
        if ok && saw {
            if let Some(n) = pop {
                m.insert(f.entry, n);
            }
        }
    }
    m
}

/// Does `e` contain a call anywhere (used to keep call-bearing functions out of
/// the leaf-only optimizer differential)?
fn expr_contains_call(e: &Expr) -> bool {
    match e {
        Expr::Call { .. } => true,
        Expr::Unary(_, a) | Expr::Cast { expr: a, .. } => expr_contains_call(a),
        Expr::Binary(_, a, b) => expr_contains_call(a) || expr_contains_call(b),
        Expr::Load { addr, .. } => expr_contains_call(addr),
        Expr::Select { cond, then_, else_ } => {
            expr_contains_call(cond) || expr_contains_call(then_) || expr_contains_call(else_)
        }
        _ => false,
    }
}

/// A function the SSA interpreter can attempt end-to-end: no call/switch/asm.
/// (The optimizer differential is leaf-only for now — the SSA closure across
/// calls, with its esp-through-values threading, is a later increment.)
fn is_leaf(irf: &IrFunction) -> bool {
    irf.blocks.iter().all(|b| {
        b.stmts.iter().all(|s| match s {
            Stmt::Switch { .. } | Stmt::Asm(_) | Stmt::CallStmt(_) => false,
            Stmt::Set { expr, .. } | Stmt::Assign { expr, .. } => !expr_contains_call(expr),
            Stmt::Store { addr, value, .. } => {
                !expr_contains_call(addr) && !expr_contains_call(value)
            }
            Stmt::Branch { cond, .. } => !expr_contains_call(cond),
            Stmt::Return(Some(e)) => !expr_contains_call(e),
            _ => true,
        })
    })
}

/// Optimizer differential for one leaf function: run its **pre-opt** IR (the
/// oracle — already validated bit-for-bit against Unicorn by `diff_function`) and
/// its **post-opt SSA** IR (`to_ssa` + `optimize`) from an identical state, and
/// compare the return value + all mapped memory. A divergence is a real bug in
/// SSA construction or an optimizer pass.
///
/// Sound by construction: `optimize` never removes a `Store` (no alias analysis →
/// all stores kept) and never mutates the CFG (it only folds expressions inside
/// statements), so a correct optimizer yields byte-identical memory and an equal
/// return value. Either interpreter returning `None` (unmodelled / out-of-region /
/// over-budget) skips the state — never a false verdict.
pub fn diff_function_opt(
    prog: &crate::loader::Program,
    pre_opt: &IrFunction,
    iters: u32,
    seed: &mut u64,
) -> Vec<FnMismatch> {
    let mut out = Vec::new();
    if pre_opt.blocks.is_empty() || !is_leaf(pre_opt) {
        return out;
    }
    // Post-opt form: SSA-construct and optimize a clone.
    let mut post = pre_opt.clone();
    crate::ssa::to_ssa(&mut post);
    crate::opt::optimize(&mut post);

    // One mirrored region over the whole PE image (same bounds as diff_function).
    let lo = match prog.sections.iter().map(|s| s.address).min() {
        Some(x) => x & !0xfff,
        None => return out,
    };
    let hi = match prog.sections.iter().map(|s| s.address + s.data.len() as u64).max() {
        Some(x) => (x + 0xfff) & !0xfff,
        None => return out,
    };
    if hi <= lo || (hi - lo) > 0x400_0000 {
        return out;
    }
    let mut img0 = vec![0u8; (hi - lo) as usize];
    for s in &prog.sections {
        let off = (s.address - lo) as usize;
        img0[off..off + s.data.len()].copy_from_slice(&s.data);
    }

    let empty_funcs: HashMap<u64, &IrFunction> = HashMap::new();
    let empty_pops: HashMap<u64, i64> = HashMap::new();
    let ctx = ClosureCtx { funcs: &empty_funcs, ret_pops: &empty_pops };

    for _ in 0..iters {
        let mut next = || {
            let mut x = *seed;
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            *seed = x;
            x
        };
        let mut regs = [0u64; 8];
        for r in 0..8 {
            regs[r] = (next() as u32) as u64;
        }
        regs[4] = FN_STACK_BASE + (FN_STACK_SIZE as u64) / 2; // esp
        let mut stack = vec![0u8; FN_STACK_SIZE];
        for b in stack.iter_mut() {
            *b = next() as u8;
        }
        let mut flags = HashMap::new();
        for f in [FlagKind::Cf, FlagKind::Zf, FlagKind::Sf, FlagKind::Of, FlagKind::Pf, FlagKind::Af] {
            flags.insert(f, next() & 1);
        }
        let state = CpuState { regs, flags: flags.clone(), xmm: [[0; 2]; 8] };
        let regions = || {
            vec![
                Region { base: lo, data: img0.clone(), writable: true },
                Region { base: FN_STACK_BASE, data: stack.clone(), writable: true },
            ]
        };

        // ---- pre-opt IR (the oracle) ----
        let mut a = Interp::new(&state, Vec::new());
        a.regions = regions();
        let mut budget_a = 500_000u32;
        let a_ret = match a.run_closure(pre_opt, &ctx, 0, &mut budget_a) {
            Some(v) => v,
            None => continue,
        };

        // ---- post-opt SSA IR (under test) ----
        let mut b = Interp::new(&state, Vec::new());
        b.regions = regions();
        b.seed_entry_values(&post);
        let mut budget_b = 500_000u32;
        let b_ret = match b.run_ssa(&post, &mut budget_b) {
            Some(v) => v,
            None => continue,
        };

        FUNCDIFF_OPT_SCORED.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        if a_ret != b_ret {
            out.push(FnMismatch {
                func: pre_opt.entry,
                what: "ret".to_string(),
                lifted: b_ret,
                unicorn: a_ret,
            });
        }
        for (ri, base, label) in [(0usize, lo, "mem"), (1usize, FN_STACK_BASE, "stack")] {
            let (da, db) = (&a.regions[ri].data, &b.regions[ri].data);
            for i in 0..da.len() {
                if da[i] != db[i] {
                    out.push(FnMismatch {
                        func: pre_opt.entry,
                        what: format!("{label} {:#x}", base + i as u64),
                        lifted: db[i] as u64,
                        unicorn: da[i] as u64,
                    });
                    break;
                }
            }
        }
        if !out.is_empty() {
            break; // one witness per function is enough
        }
    }
    out
}

/// Run `iters` random states through one whole recovered function, comparing the
/// lifted IR (interpreter) against Unicorn. Empty result = matched or wholly
/// skipped. ESP is not compared (the IR `Return` does not model the `ret` pop);
/// everything else — the 7 other GP registers and all mapped memory — is.
pub fn diff_function(
    prog: &crate::loader::Program,
    irf: &IrFunction,
    ctx: &ClosureCtx,
    iters: u32,
    seed: &mut u64,
) -> Vec<FnMismatch> {
    let mut out = Vec::new();
    if !is_closure_modelable(irf.entry, ctx.funcs, ctx.ret_pops) {
        return out;
    }
    // One mirrored region over the whole PE image.
    let lo = match prog.sections.iter().map(|s| s.address).min() {
        Some(x) => x & !0xfff,
        None => return out,
    };
    let hi = match prog.sections.iter().map(|s| s.address + s.data.len() as u64).max() {
        Some(x) => (x + 0xfff) & !0xfff,
        None => return out,
    };
    if hi <= lo || (hi - lo) > 0x400_0000 {
        return out;
    }
    let mut img0 = vec![0u8; (hi - lo) as usize];
    for s in &prog.sections {
        let off = (s.address - lo) as usize;
        img0[off..off + s.data.len()].copy_from_slice(&s.data);
    }

    let mut uc: *mut uc_engine = std::ptr::null_mut();
    unsafe {
        if uc_open(UC_ARCH_X86, UC_MODE_32, &mut uc) != 0 {
            return out;
        }
        if uc_mem_map(uc, lo, img0.len(), UC_PROT_ALL) != 0
            || uc_mem_map(uc, FN_STACK_BASE, FN_STACK_SIZE, UC_PROT_ALL) != 0
        {
            uc_close(uc);
            return out;
        }
    }

    for _ in 0..iters {
        let mut next = || {
            let mut x = *seed;
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            *seed = x;
            x
        };
        let mut regs = [0u64; 8];
        for r in 0..8 {
            regs[r] = (next() as u32) as u64;
        }
        let esp = FN_STACK_BASE + (FN_STACK_SIZE as u64) / 2;
        regs[4] = esp;
        let mut stack = vec![0u8; FN_STACK_SIZE];
        for b in stack.iter_mut() {
            *b = next() as u8;
        }
        let soff = (esp - FN_STACK_BASE) as usize;
        stack[soff..soff + 4].copy_from_slice(&(FN_SENTINEL as u32).to_le_bytes());
        let mut flags = HashMap::new();
        for f in [FlagKind::Cf, FlagKind::Zf, FlagKind::Sf, FlagKind::Of, FlagKind::Pf, FlagKind::Af] {
            flags.insert(f, next() & 1);
        }
        let state = CpuState { regs, flags: flags.clone(), xmm: [[0; 2]; 8] };

        // ---- interpret the lifted IR ----
        let mut interp = Interp::new(&state, Vec::new());
        interp.regions = vec![
            Region { base: lo, data: img0.clone(), writable: true },
            Region { base: FN_STACK_BASE, data: stack.clone(), writable: true },
        ];
        let mut budget = 500_000u32;
        if interp.run_closure(irf, ctx, 0, &mut budget).is_none() {
            continue;
        }
        // Soundness guard for closure mode: if the unmapped return-address
        // sentinel leaked into a compared register (a get-pc thunk returning
        // `[esp]`), we cannot trust the comparison — Unicorn holds the real return
        // address there. Skip rather than risk a false positive.
        if (0..8).any(|r| r != 4 && (interp.regs[r] as u32) == FN_RET_SENTINEL as u32) {
            continue;
        }

        // ---- run Unicorn from the same state ----
        unsafe {
            uc_mem_write(uc, lo, img0.as_ptr() as *const c_void, img0.len());
            uc_mem_write(uc, FN_STACK_BASE, stack.as_ptr() as *const c_void, FN_STACK_SIZE);
            for r in 0..8 {
                let v = regs[r] as u32;
                uc_reg_write(uc, UC_GP[r], &v as *const u32 as *const c_void);
            }
            let mut eflags: u32 = 0x2;
            for (f, bp) in [
                (FlagKind::Cf, 0u32), (FlagKind::Pf, 2), (FlagKind::Af, 4),
                (FlagKind::Zf, 6), (FlagKind::Sf, 7), (FlagKind::Of, 11),
            ] {
                if flags.get(&f).copied().unwrap_or(0) != 0 {
                    eflags |= 1 << bp;
                }
            }
            uc_reg_write(uc, UC_X86_REG_EFLAGS, &eflags as *const u32 as *const c_void);
            // Bounded run; stop when the function returns into the sentinel.
            let err = uc_emu_start(uc, irf.entry, FN_SENTINEL, 200_000, 100_000);
            if err != 0 {
                continue; // fault / unmapped access — not scored
            }
            let mut eip: u32 = 0;
            uc_reg_read(uc, UC_X86_REG_EIP, &mut eip as *mut u32 as *mut c_void);
            if eip as u64 != FN_SENTINEL {
                continue; // hit the instruction cap mid-run — not a clean return
            }
            FUNCDIFF_SCORED.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            // Compare the 7 GP registers other than ESP.
            for r in 0..8 {
                if r == 4 {
                    continue;
                }
                let mut uv: u32 = 0;
                uc_reg_read(uc, UC_GP[r], &mut uv as *mut u32 as *mut c_void);
                let lv = (interp.regs[r] & 0xffff_ffff) as u32;
                if uv != lv {
                    out.push(FnMismatch { func: irf.entry, what: format!("reg r{r}"), lifted: lv as u64, unicorn: uv as u64 });
                }
            }
            // Compare all mapped memory (image + stack).
            let mut uimg = vec![0u8; img0.len()];
            uc_mem_read(uc, lo, uimg.as_mut_ptr() as *mut c_void, img0.len());
            for i in 0..img0.len() {
                if uimg[i] != interp.regions[0].data[i] {
                    out.push(FnMismatch { func: irf.entry, what: format!("mem {:#x}", lo + i as u64), lifted: interp.regions[0].data[i] as u64, unicorn: uimg[i] as u64 });
                    break;
                }
            }
            let mut ustk = vec![0u8; FN_STACK_SIZE];
            uc_mem_read(uc, FN_STACK_BASE, ustk.as_mut_ptr() as *mut c_void, FN_STACK_SIZE);
            // Exclude the return-address slots the interpreter pushed for recursed
            // calls: both engines wrote *something* there (Unicorn the real return
            // address, the interpreter a sentinel) — ABI plumbing, not a lift
            // signal. Comparing them would be a guaranteed false positive.
            let is_ret_slot = |addr: u64| interp.ret_slots.iter().any(|&s| addr >= s && addr < s + 4);
            for i in 0..FN_STACK_SIZE {
                if ustk[i] != interp.regions[1].data[i] && !is_ret_slot(FN_STACK_BASE + i as u64) {
                    out.push(FnMismatch { func: irf.entry, what: format!("stack +{i:#x}"), lifted: interp.regions[1].data[i] as u64, unicorn: ustk[i] as u64 });
                    break;
                }
            }
        }
        if !out.is_empty() {
            break; // one witness per function is enough
        }
    }
    unsafe {
        uc_close(uc);
    }
    out
}

/// Load a 32-bit PE, recover its functions, and run the whole-function
/// differential over each. Returns all divergences found.
pub fn run_functions(path: &str, iters: u32) -> Result<Vec<FnMismatch>, String> {
    let data = std::fs::read(path).map_err(|e| e.to_string())?;
    let prog = crate::loader::Program::load(&data).map_err(|e| e.to_string())?;
    if prog.bitness.bits() != 32 {
        return Ok(Vec::new());
    }
    let disasm = crate::disasm::Disassembler::new(prog.bitness);
    let result = crate::analysis::analyze(&prog, &disasm, true);
    let refs: Vec<&crate::analysis::Function> = result.functions.iter().collect();
    crate::ir::build::set_noreturn(crate::ir::build::compute_noreturn(&refs));
    crate::ir::build::set_fp_returning(crate::ir::build::compute_fp_returning(&prog, &refs));
    crate::ir::build::set_call_clobbers(crate::ir::build::compute_call_clobbers(&refs));

    // Build every function's IR up front so the closure interpreter can follow a
    // direct call into its callee. Force frame-pointer-omission (raw `[esp±d]` /
    // `[ebp±d]` loads instead of named `Frame` slots): this is exactly what the
    // transpiler — the shipped product — lowers, and it makes stack accesses
    // interpretable against the mirrored stack region (a `Frame` slot is opaque
    // to the interpreter → skip). Both engines read the same stack bytes, so it
    // is the *more* faithful mode, not a shortcut.
    crate::ir::lift::set_frames_off(true);
    let irfs: Vec<IrFunction> = result
        .functions
        .iter()
        .map(|f| {
            crate::ir::lift::set_frames_off(true); // build_ir resets it per call
            crate::ir::build::build_ir(&prog, f)
        })
        .collect();
    let funcs: HashMap<u64, &IrFunction> = irfs.iter().map(|f| (f.entry, f)).collect();
    let ret_pops = compute_ret_pops(&result.functions);
    let ctx = ClosureCtx { funcs: &funcs, ret_pops: &ret_pops };

    let mut seed: u64 = 0x1234_5678_9abc_def1;
    let mut out = Vec::new();
    for irf in &irfs {
        out.extend(diff_function(&prog, irf, &ctx, iters, &mut seed));
    }
    Ok(out)
}

/// Load a 32-bit PE, recover its functions, and run the **optimizer** differential
/// (pre-opt vs post-opt SSA) over each leaf function. Returns all divergences —
/// each a real bug in SSA construction or an optimizer pass.
pub fn run_functions_opt(path: &str, iters: u32) -> Result<Vec<FnMismatch>, String> {
    let data = std::fs::read(path).map_err(|e| e.to_string())?;
    let prog = crate::loader::Program::load(&data).map_err(|e| e.to_string())?;
    if prog.bitness.bits() != 32 {
        return Ok(Vec::new());
    }
    let disasm = crate::disasm::Disassembler::new(prog.bitness);
    let result = crate::analysis::analyze(&prog, &disasm, true);
    let refs: Vec<&crate::analysis::Function> = result.functions.iter().collect();
    crate::ir::build::set_noreturn(crate::ir::build::compute_noreturn(&refs));
    crate::ir::build::set_fp_returning(crate::ir::build::compute_fp_returning(&prog, &refs));
    crate::ir::build::set_call_clobbers(crate::ir::build::compute_call_clobbers(&refs));
    let mut seed: u64 = 0x1234_5678_9abc_def1;
    let mut out = Vec::new();
    for f in &result.functions {
        crate::ir::lift::set_frames_off(true); // build_ir resets it per call
        let pre_opt = crate::ir::build::build_ir(&prog, f);
        out.extend(diff_function_opt(&prog, &pre_opt, iters, &mut seed));
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Corpus gate (run via `bench/funcdiff.sh`): both differentials — the lifter
    /// closure (vs Unicorn) and the optimizer diff (pre-opt vs post-opt SSA) — must
    /// find **no** divergence on the committed real binaries (mingw busybox + MSVC
    /// sqlite), and must be non-vacuous (score functions, follow calls). #[ignore]
    /// so it stays out of the default suite; the bench invokes it explicitly.
    #[test]
    #[ignore] // gate: `bash bench/funcdiff.sh`  /  `cargo test --features unpack funcdiff_corpus -- --ignored --nocapture`
    fn funcdiff_corpus() {
        let corpus = [
            "bench/.cache/busybox-w32-FRP-5579-g5749feb35.exe",
            "bench/.cache/sqlite3-3400100.exe",
        ];
        let mut any = false;
        let (mut tot_scored, mut tot_calls, mut tot_opt, mut tot_div) = (0usize, 0usize, 0usize, 0usize);
        for path in corpus {
            if !std::path::Path::new(path).exists() {
                eprintln!("SKIP (absent): {path}");
                continue;
            }
            any = true;
            let name = path.rsplit('/').next().unwrap();

            FUNCDIFF_SCORED.store(0, std::sync::atomic::Ordering::Relaxed);
            FUNCDIFF_CALLS.store(0, std::sync::atomic::Ordering::Relaxed);
            let lift = run_functions(path, 100).expect("lift-closure");
            let s = FUNCDIFF_SCORED.load(std::sync::atomic::Ordering::Relaxed);
            let c = FUNCDIFF_CALLS.load(std::sync::atomic::Ordering::Relaxed);
            eprintln!("{name} LIFT-closure: {s} scored, {c} calls, {} divergence(s)", lift.len());
            for m in lift.iter().take(8) {
                eprintln!("  fn {:#x} {}: lifted={:#x} unicorn={:#x}", m.func, m.what, m.lifted, m.unicorn);
            }

            FUNCDIFF_OPT_SCORED.store(0, std::sync::atomic::Ordering::Relaxed);
            let opt = run_functions_opt(path, 100).expect("opt-diff");
            let os = FUNCDIFF_OPT_SCORED.load(std::sync::atomic::Ordering::Relaxed);
            eprintln!("{name} OPT-diff:     {os} scored, {} divergence(s)", opt.len());
            for m in opt.iter().take(8) {
                eprintln!("  fn {:#x} {}: post-opt={:#x} pre-opt={:#x}", m.func, m.what, m.lifted, m.unicorn);
            }

            tot_scored += s; tot_calls += c; tot_opt += os;
            tot_div += lift.len() + opt.len();
        }
        if !any {
            eprintln!("SKIP: no corpus binary present");
            return;
        }
        eprintln!(
            "FUNCDIFF-CORPUS: lift {tot_scored} scored / {tot_calls} calls, opt {tot_opt} scored, {tot_div} divergence(s)"
        );
        assert_eq!(tot_div, 0, "funcdiff found divergence(s) on the committed corpus");
        // Non-vacuous: the corpus must actually exercise both paths.
        assert!(tot_calls > 0, "lifter closure followed no calls on the corpus");
        assert!(tot_opt > 0, "optimizer differential scored nothing on the corpus");
    }

    /// Whole-function differential must find no divergence on committed,
    /// known-good PEs (soundness of the funcdiff harness itself) — and must
    /// actually exercise some functions (non-vacuous), *including* the closure
    /// path (a call followed into a callee — `recursion.exe`'s `fib` recurses
    /// through the interpreter and must match Unicorn). A real lift bug here
    /// would be a genuine finding; a harness false-positive must be fixed.
    #[test]
    fn functions_match_unicorn_on_fixtures() {
        let dir = std::path::Path::new("tests/m1/fixtures");
        if !dir.exists() {
            return;
        }
        FUNCDIFF_SCORED.store(0, std::sync::atomic::Ordering::Relaxed);
        FUNCDIFF_CALLS.store(0, std::sync::atomic::Ordering::Relaxed);
        let mut all = Vec::new();
        let mut entries: Vec<_> = std::fs::read_dir(dir)
            .unwrap()
            .filter_map(|e| e.ok().map(|e| e.path()))
            .filter(|p| p.extension().map(|x| x == "exe").unwrap_or(false))
            .collect();
        entries.sort();
        for p in &entries {
            all.extend(run_functions(p.to_str().unwrap(), 100).expect("funcdiff harness"));
        }
        let scored = FUNCDIFF_SCORED.load(std::sync::atomic::Ordering::Relaxed);
        if !all.is_empty() {
            let mut msg = format!("{} funcdiff divergence(s):\n", all.len());
            for m in all.iter().take(12) {
                msg.push_str(&format!(
                    "  fn {:#x} {}: lifted={:#x} unicorn={:#x}\n",
                    m.func, m.what, m.lifted, m.unicorn
                ));
            }
            panic!("{msg}");
        }
        // Non-vacuous: at least one fixture must have scored a function, or the
        // "no divergence" verdict proves nothing.
        assert!(scored > 0, "funcdiff scored 0 functions — vacuous, harness not exercised");
        // And the *closure* specifically must have followed at least one call
        // into a callee — otherwise this only re-tests leaf functions.
        let calls = FUNCDIFF_CALLS.load(std::sync::atomic::Ordering::Relaxed);
        assert!(calls > 0, "funcdiff closure followed 0 calls — the closure path is vacuous");
    }
}
