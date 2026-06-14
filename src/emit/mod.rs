//! IR → compilable C emitter (roadmap §7), first milestone: goto-based C that
//! recompiles. Structured emission (reusing `structure`) and typed variables
//! come later; this closes the loop enough to start the verification harness
//! (§8, "recompiles" level).
//!
//! Pipeline: optimized SSA IR → SSA destruction (φ lowering) → C text.
//! Every value becomes a `uint64_t`; unmodelled instructions are emitted as
//! comments (their clobbers were already `Undef`), so the output compiles.
#![allow(dead_code)]

pub mod structured;

use crate::ir::types::*;
use std::collections::BTreeSet;
use std::fmt::Write;

/// Lower φ-nodes out of SSA by inserting, on every incoming edge, a small block
/// that copies each φ argument into the φ's value, then jumps to the join.
/// Inserting on *every* edge (not only critical ones) keeps it simple and
/// correct: copies always live in a dedicated single-pred/single-succ block.
pub(crate) fn destruct_ssa(func: &mut IrFunction) {
    let n = func.blocks.len();
    let mut next_id = func.blocks.iter().map(|b| b.id).max().unwrap_or(0) + 1;
    let mut new_blocks: Vec<Block> = Vec::new();

    for s in 0..n {
        let phis: Vec<(ValueId, Vec<ValueId>)> = func.blocks[s]
            .stmts
            .iter()
            .filter_map(|st| match st {
                Stmt::Assign { dst, expr: Expr::Phi(args) } => Some((*dst, args.clone())),
                _ => None,
            })
            .collect();
        if phis.is_empty() {
            continue;
        }
        let preds = func.blocks[s].pred.clone();
        let s_id = func.blocks[s].id;
        for (i, &p) in preds.iter().enumerate() {
            let mid = next_id;
            next_id += 1;
            let mut stmts: Vec<Stmt> = phis
                .iter()
                .map(|(dst, args)| Stmt::Assign {
                    dst: *dst,
                    expr: Expr::Use(args[i]),
                })
                .collect();
            stmts.push(Stmt::Jump(BlockId(s_id)));
            new_blocks.push(Block {
                id: mid,
                addr: 0,
                stmts,
                succ: vec![s_id],
                pred: vec![p],
            });
            // Redirect predecessor p's terminator edge s_id -> mid.
            let pb = func.blocks.iter_mut().find(|b| b.id == p).unwrap();
            for su in pb.succ.iter_mut() {
                if *su == s_id {
                    *su = mid;
                }
            }
            redirect_terminator(pb.stmts.last_mut(), s_id, mid);
        }
        // The join's predecessors are now the new copy blocks; drop its φs.
        func.blocks[s].pred = (0..preds.len() as u32).map(|k| next_id - preds.len() as u32 + k).collect();
        func.blocks[s]
            .stmts
            .retain(|st| !matches!(st, Stmt::Assign { expr: Expr::Phi(_), .. }));
    }
    func.blocks.extend(new_blocks);
}

fn redirect_terminator(last: Option<&mut Stmt>, from: u32, to: u32) {
    if let Some(st) = last {
        match st {
            Stmt::Jump(b) => {
                if b.0 == from {
                    b.0 = to;
                }
            }
            Stmt::Branch { taken, fallthrough, .. } => {
                if taken.0 == from {
                    taken.0 = to;
                }
                if fallthrough.0 == from {
                    fallthrough.0 = to;
                }
            }
            Stmt::Switch { cases, default, .. } => {
                for (_, b) in cases.iter_mut() {
                    if b.0 == from {
                        b.0 = to;
                    }
                }
                if default.0 == from {
                    default.0 = to;
                }
            }
            _ => {}
        }
    }
}

pub(crate) fn ctype(bits: u8) -> &'static str {
    match bits {
        8 => "uint8_t",
        16 => "uint16_t",
        32 => "uint32_t",
        _ => "uint64_t",
    }
}

fn ty_ctype(t: &Ty) -> &'static str {
    match t {
        Ty::Int { bits, .. } => ctype(*bits),
        _ => "uint64_t",
    }
}

fn const_c(v: i128) -> String {
    if v < 0 {
        format!("(uint64_t)({})", v)
    } else if v > 9 {
        format!("0x{:x}ULL", v as u128)
    } else {
        format!("{}", v)
    }
}

/// Name a recovered stack-frame slot.
pub(crate) fn frame_name(d: i64) -> String {
    if d == 0 {
        "saved_bp".into()
    } else if d > 0 {
        format!("arg_{:x}", d)
    } else {
        format!("local_{:x}", -d)
    }
}

pub(crate) fn expr_c(e: &Expr) -> String {
    match e {
        Expr::Const(v, _) => const_c(*v),
        Expr::Use(v) => format!("v{}", v.0),
        Expr::Undef => "0 /*undef*/".into(),
        Expr::Read(Location::Frame(d)) => frame_name(*d),
        Expr::Read(_) => "0 /*reg*/".into(), // other reads don't remain post-SSA
        Expr::Load { addr, ty } => format!("(*({}*)({}))", ctype(int_bits(ty)), expr_c(addr)),
        Expr::Unary(op, x) => match op {
            UnOp::Neg => format!("(-({}))", expr_c(x)),
            UnOp::Not => format!("(~({}))", expr_c(x)),
            // Sign-extension must actually sign-extend: emit a width-aware signed
            // cast (the source width is carried by the masked value or typed
            // load it wraps). Rendering it as identity zero-extends, which is
            // wrong for negative narrow values (e.g. `movsx`).
            UnOp::SignExtend => format!("({})", signed_cast(x)),
            // Zero-extension / truncation: the operand is already masked to its
            // width, so the value is correct as-is.
            _ => format!("({})", expr_c(x)),
        },
        Expr::Binary(op, a, b) => binary_c(*op, a, b),
        Expr::Cast { to, expr } => format!("(({})({}))", ty_ctype(to), expr_c(expr)),
        Expr::Addr(Location::Frame(d)) => format!("(uint64_t)(&{})", frame_name(*d)),
        Expr::Addr(_) => "0 /*addr*/".into(),
        Expr::Call { target, args, .. } => {
            let a: Vec<String> = args.iter().map(expr_c).collect();
            match target {
                CallTarget::Direct(addr) => format!("sub_{:x}({})", addr, a.join(", ")),
                CallTarget::Named(n) if n.starts_with("asm:") => {
                    format!("0 /*{}*/", n)
                }
                CallTarget::Named(n) => format!("{}({})", n, a.join(", ")),
                CallTarget::Indirect(_) => "0 /*indirect call*/".into(),
            }
        }
        Expr::Select { cond, then_, else_ } => {
            format!("({} ? {} : {})", expr_c(cond), expr_c(then_), expr_c(else_))
        }
        Expr::Phi(_) => "0 /*phi*/".into(),
    }
}

pub(crate) fn int_bits(t: &Ty) -> u8 {
    match t {
        Ty::Int { bits, .. } => *bits,
        _ => 64,
    }
}

/// Signed-cast an operand, sign-extending from its apparent width. A value
/// masked to a sub-word width (`x & 0xff/0xffff/0xffffffff`) is sign-extended
/// from that width (`(int64_t)(int32_t)x`), not treated as a positive 64-bit
/// number — otherwise signed comparisons of negative 32-bit values are wrong.
fn signed_cast(e: &Expr) -> String {
    match e {
        // A value masked to a sub-word width is sign-extended from that width.
        Expr::Binary(BinOp::And, x, m) => {
            if let Expr::Const(c, _) = m.as_ref() {
                let st = match *c {
                    0xff => Some("int8_t"),
                    0xffff => Some("int16_t"),
                    0xffffffff => Some("int32_t"),
                    _ => None,
                };
                if let Some(t) = st {
                    return format!("(int64_t)({})({})", t, expr_c(x));
                }
            }
        }
        // A sub-word memory load is read through a signed pointer so its sign bit
        // extends correctly (the default `(*(uintN_t*)…)` would zero-extend).
        Expr::Load { addr, ty } => {
            let b = int_bits(ty);
            if b < 64 {
                return format!("(int64_t)(*({}*)({}))", sctype(b), expr_c(addr));
            }
        }
        _ => {}
    }
    format!("(int64_t)({})", expr_c(e))
}

/// Signed C integer type name for a width in bits.
fn sctype(bits: u8) -> &'static str {
    match bits {
        1..=8 => "int8_t",
        9..=16 => "int16_t",
        17..=32 => "int32_t",
        _ => "int64_t",
    }
}

fn binary_c(op: BinOp, a: &Expr, b: &Expr) -> String {
    use BinOp::*;
    let u = |x: &Expr| format!("(uint64_t)({})", expr_c(x));
    let s = |o: &str| format!("({} {} {})", signed_cast(a), o, signed_cast(b));
    let un = |o: &str| format!("({} {} {})", u(a), o, u(b));
    let plain = |o: &str| format!("({} {} {})", expr_c(a), o, expr_c(b));
    match op {
        Add => plain("+"),
        Sub => plain("-"),
        Mul => plain("*"),
        And => plain("&"),
        Or => plain("|"),
        Xor => plain("^"),
        Shl => plain("<<"),
        Eq => plain("=="),
        Ne => plain("!="),
        Shr => format!("({} >> {})", u(a), expr_c(b)),
        Sar => format!("({} >> {})", signed_cast(a), expr_c(b)),
        UDiv => un("/"),
        UMod => un("%"),
        Ult => un("<"),
        Ule => un("<="),
        Ugt => un(">"),
        Uge => un(">="),
        SDiv => s("/"),
        SMod => s("%"),
        Slt => s("<"),
        Sle => s("<="),
        Sgt => s(">"),
        Sge => s(">="),
    }
}

/// Collect every `ValueId` referenced anywhere (defs, uses, φ args) so they can
/// all be declared (including undef versions that are read but never assigned).
pub(crate) fn collect_values(func: &IrFunction, out: &mut BTreeSet<u32>) {
    fn walk(e: &Expr, out: &mut BTreeSet<u32>) {
        match e {
            Expr::Use(v) => {
                out.insert(v.0);
            }
            Expr::Phi(args) => {
                for v in args {
                    out.insert(v.0);
                }
            }
            Expr::Load { addr, .. } => walk(addr, out),
            Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => walk(x, out),
            Expr::Binary(_, a, b) => {
                walk(a, out);
                walk(b, out);
            }
            Expr::Call { target, args, .. } => {
                if let CallTarget::Indirect(x) = target {
                    walk(x, out);
                }
                for a in args {
                    walk(a, out);
                }
            }
            Expr::Select { cond, then_, else_ } => {
                walk(cond, out);
                walk(then_, out);
                walk(else_, out);
            }
            _ => {}
        }
    }
    for b in &func.blocks {
        for s in &b.stmts {
            if let Stmt::Assign { dst, .. } = s {
                out.insert(dst.0);
            }
            match s {
                Stmt::Set { expr, .. }
                | Stmt::Assign { expr, .. }
                | Stmt::CallStmt(expr) => walk(expr, out),
                Stmt::Store { addr, value, .. } => {
                    walk(addr, out);
                    walk(value, out);
                }
                Stmt::Branch { cond, .. } => walk(cond, out),
                Stmt::Switch { value, .. } => walk(value, out),
                Stmt::Return(Some(e)) => walk(e, out),
                _ => {}
            }
        }
    }
}

/// Collect recovered stack-frame slot displacements (for declaration).
pub(crate) fn collect_frame_vars(func: &IrFunction, out: &mut BTreeSet<i64>) {
    fn walk(e: &Expr, out: &mut BTreeSet<i64>) {
        match e {
            Expr::Read(Location::Frame(d)) | Expr::Addr(Location::Frame(d)) => {
                out.insert(*d);
            }
            Expr::Load { addr, .. } => walk(addr, out),
            Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => walk(x, out),
            Expr::Binary(_, a, b) => {
                walk(a, out);
                walk(b, out);
            }
            Expr::Call { args, .. } => {
                for a in args {
                    walk(a, out);
                }
            }
            Expr::Select { cond, then_, else_ } => {
                walk(cond, out);
                walk(then_, out);
                walk(else_, out);
            }
            _ => {}
        }
    }
    for b in &func.blocks {
        for s in &b.stmts {
            if let Stmt::Set { dst: Location::Frame(d), .. } = s {
                out.insert(*d);
            }
            match s {
                Stmt::Set { expr, .. }
                | Stmt::Assign { expr, .. }
                | Stmt::CallStmt(expr) => walk(expr, out),
                Stmt::Store { addr, value, .. } => {
                    walk(addr, out);
                    walk(value, out);
                }
                Stmt::Branch { cond, .. } => walk(cond, out),
                Stmt::Return(Some(e)) => walk(e, out),
                _ => {}
            }
        }
    }
}

/// Split recovered frame slots into `(params, locals)`. Parameters are the
/// positive slots at/after the first argument (`saved_bp` at 0, return address
/// at one pointer, first argument at two pointers); everything else is a local.
pub(crate) fn frame_params_locals(func: &IrFunction) -> (Vec<i64>, Vec<i64>) {
    let mut fv = BTreeSet::new();
    collect_frame_vars(func, &mut fv);
    let ptr = (func.bits.max(32) / 8) as i64;
    let first_arg = 2 * ptr;
    let mut params = Vec::new();
    let mut locals = Vec::new();
    for d in fv {
        if d >= first_arg {
            params.push(d);
        } else {
            locals.push(d);
        }
    }
    (params, locals)
}

/// The function's C signature. With `with_params`, recovered frame arguments
/// become real parameters; otherwise `(void)`.
pub(crate) fn signature(func: &IrFunction, with_params: bool) -> String {
    if !with_params {
        return format!("uint64_t sub_{:x}(void)", func.entry);
    }
    let mut p: Vec<String> = Vec::new();
    // Register-passed parameters first (64-bit ABIs), in convention order.
    for v in &func.reg_params {
        p.push(format!("uint64_t v{}", v));
    }
    // Then stack/frame parameters.
    let (params, _) = frame_params_locals(func);
    for d in params {
        p.push(format!("uint64_t {}", frame_name(d)));
    }
    if p.is_empty() {
        format!("uint64_t sub_{:x}(void)", func.entry)
    } else {
        format!("uint64_t sub_{:x}({})", func.entry, p.join(", "))
    }
}

/// Declaration line for frame slots that are *not* parameters (or all of them
/// when `with_params` is false).
pub(crate) fn frame_decls(func: &IrFunction, with_params: bool) -> Option<String> {
    let (params, locals) = frame_params_locals(func);
    let set: Vec<i64> = if with_params {
        locals
    } else {
        params.into_iter().chain(locals).collect()
    };
    if set.is_empty() {
        return None;
    }
    let decls: Vec<String> = set.iter().map(|d| format!("{} = 0", frame_name(*d))).collect();
    Some(format!("    uint64_t {};", decls.join(", ")))
}

/// Collect direct call targets, to forward-declare them.
pub(crate) fn collect_callees(func: &IrFunction, out: &mut BTreeSet<u64>) {
    fn walk(e: &Expr, out: &mut BTreeSet<u64>) {
        match e {
            Expr::Call { target: CallTarget::Direct(a), args, .. } => {
                out.insert(*a);
                for x in args {
                    walk(x, out);
                }
            }
            Expr::Call { args, .. } => {
                for x in args {
                    walk(x, out);
                }
            }
            Expr::Load { addr, .. } => walk(addr, out),
            Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => walk(x, out),
            Expr::Binary(_, a, b) => {
                walk(a, out);
                walk(b, out);
            }
            Expr::Select { cond, then_, else_ } => {
                walk(cond, out);
                walk(then_, out);
                walk(else_, out);
            }
            _ => {}
        }
    }
    for b in &func.blocks {
        for s in &b.stmts {
            match s {
                Stmt::Set { expr, .. } | Stmt::Assign { expr, .. } | Stmt::CallStmt(expr) => {
                    walk(expr, out)
                }
                Stmt::Store { addr, value, .. } => {
                    walk(addr, out);
                    walk(value, out);
                }
                Stmt::Return(Some(e)) => walk(e, out),
                _ => {}
            }
        }
    }
}

fn stmt_c(s: &Stmt, out: &mut String) {
    match s {
        Stmt::Assign { dst, expr } => {
            let _ = writeln!(out, "    v{} = {};", dst.0, expr_c(expr));
        }
        Stmt::Set { dst: Location::Frame(d), expr } => {
            let _ = writeln!(out, "    {} = {};", frame_name(*d), expr_c(expr));
        }
        Stmt::Set { .. } => {}
        Stmt::Store { addr, value, ty } => {
            let _ = writeln!(
                out,
                "    *({}*)({}) = {};",
                ctype(int_bits(ty)),
                expr_c(addr),
                expr_c(value)
            );
        }
        Stmt::Branch { cond, taken, fallthrough } => {
            let _ = writeln!(out, "    if ({}) goto B{};", expr_c(cond), taken.0);
            let _ = writeln!(out, "    goto B{};", fallthrough.0);
        }
        Stmt::Jump(b) => {
            let _ = writeln!(out, "    goto B{};", b.0);
        }
        Stmt::Switch { value, default, .. } => {
            let _ = writeln!(out, "    (void)({}); goto B{};", expr_c(value), default.0);
        }
        Stmt::Return(Some(e)) => {
            let _ = writeln!(out, "    return {};", expr_c(e));
        }
        Stmt::Return(None) => {
            let _ = writeln!(out, "    return 0;");
        }
        Stmt::CallStmt(e) => {
            let _ = writeln!(out, "    (void)({});", expr_c(e));
        }
        Stmt::Asm(t) => {
            let _ = writeln!(out, "    /* asm: {} */", t);
        }
        Stmt::Nop => {}
    }
}

/// Emit one function as compilable C (goto form). `forward` receives the
/// forward declarations this function needs (callees).
pub fn emit_function(func: &IrFunction, forward: &mut BTreeSet<u64>, with_params: bool) -> String {
    let mut f = func.clone();
    destruct_ssa(&mut f);

    let mut values = BTreeSet::new();
    collect_values(&f, &mut values);
    collect_callees(&f, forward);
    if with_params {
        for p in &f.reg_params {
            values.remove(p); // declared as parameters, not locals
        }
    }

    let mut out = String::new();
    let _ = writeln!(out, "{} {{", signature(&f, with_params));
    if !values.is_empty() {
        let decls: Vec<String> = values.iter().map(|v| format!("v{} = 0", v)).collect();
        let _ = writeln!(out, "    uint64_t {};", decls.join(", "));
    }
    if let Some(fd) = frame_decls(&f, with_params) {
        let _ = writeln!(out, "{}", fd);
    }
    // Enter at the function's entry block.
    let entry_id = f
        .blocks
        .iter()
        .find(|b| b.addr == f.entry)
        .map(|b| b.id)
        .unwrap_or(0);
    let _ = writeln!(out, "    goto B{};", entry_id);
    for b in &f.blocks {
        let _ = writeln!(out, "B{}:;", b.id);
        for s in &b.stmts {
            stmt_c(s, &mut out);
        }
        // A block with no terminator falls through; guard with a return.
        if !ends_in_terminator(b) {
            let _ = writeln!(out, "    return 0;");
        }
    }
    let _ = writeln!(out, "}}");
    out
}

/// Make every direct call to a function *defined in this unit* pass exactly the
/// number of arguments that callee's emitted signature declares, so the unit
/// recompiles even though the lifter over-approximates each call to the six
/// SysV integer argument registers.
///
/// Register arguments are kept (truncated, or zero-padded, to the callee's
/// recovered register-parameter count); recovered frame parameters are passed
/// as zero. Calls to a callee *not* defined here are left untouched — their
/// forward declaration uses an empty parameter list (`sub_x();`) and so accepts
/// any arity. This also subsumes recursive self-calls, whose prototype is the
/// function's own definition.
pub(crate) fn fixup_call_arity(funcs: &mut [IrFunction]) {
    use std::collections::HashMap;
    // entry -> (register params, total params incl. frame).
    let arity: HashMap<u64, (usize, usize)> = funcs
        .iter()
        .map(|f| (f.entry, (f.reg_params.len(), param_count(f))))
        .collect();
    for f in funcs.iter_mut() {
        for b in &mut f.blocks {
            for s in &mut b.stmts {
                map_top_exprs(s, &mut |e| fixup_call_expr(e, &arity));
            }
        }
    }
}

fn fixup_call_expr(e: &mut Expr, arity: &std::collections::HashMap<u64, (usize, usize)>) {
    match e {
        Expr::Call { target, args, .. } => {
            for a in args.iter_mut() {
                fixup_call_expr(a, arity);
            }
            match target {
                CallTarget::Indirect(x) => fixup_call_expr(x, arity),
                CallTarget::Direct(addr) => {
                    if let Some(&(nreg, ntot)) = arity.get(addr) {
                        args.truncate(nreg);
                        while args.len() < ntot {
                            args.push(Expr::Const(0, Ty::int(64)));
                        }
                    }
                }
                // A named import has no recovered signature, but well-known libc
                // functions have a fixed arity; trimming the over-approximated
                // argument registers to it removes noise (`strlen(s)` not
                // `strlen(s, …)`). The call has no prototype, so this is safe.
                CallTarget::Named(n) => {
                    if let Some(k) = libc_arity(n) {
                        if args.len() > k {
                            args.truncate(k);
                        }
                    }
                }
            }
        }
        Expr::Load { addr, .. } => fixup_call_expr(addr, arity),
        Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => fixup_call_expr(x, arity),
        Expr::Binary(_, a, b) => {
            fixup_call_expr(a, arity);
            fixup_call_expr(b, arity);
        }
        Expr::Select { cond, then_, else_ } => {
            fixup_call_expr(cond, arity);
            fixup_call_expr(then_, arity);
            fixup_call_expr(else_, arity);
        }
        _ => {}
    }
}

/// Fixed argument count of a common libc function, if known. The name may be a
/// raw import symbol (possibly with a `@plt`/version suffix), so match the stem.
fn libc_arity(name: &str) -> Option<usize> {
    let stem = name
        .split(['@', '+'])
        .next()
        .unwrap_or(name)
        .trim_start_matches("__")
        .trim_end_matches("_chk");
    Some(match stem {
        "strlen" | "free" | "puts" | "fflush" | "fclose" | "perror" | "abort" | "exit"
        | "_exit" | "rewind" | "atoi" | "atol" | "strdup" | "clearerr" | "fileno"
        | "malloc" | "isatty" | "umask" | "close" | "unlink" | "rmdir" | "chdir" => 1,
        "strcmp" | "strcpy" | "strcat" | "fopen" | "fputs" | "fdopen" | "getenv" | "setenv"
        | "rename" | "fseek" | "dup2" | "kill" | "access" | "stat" | "lstat" | "fstat"
        | "strchr" | "strrchr" | "strstr" => 2,
        "memcpy" | "memset" | "memmove" | "strncmp" | "strncpy" | "strncat" | "read"
        | "write" | "memchr" | "fgets" | "lseek" => 3,
        "fread" | "fwrite" => 4,
        _ => return None,
    })
}

/// Apply `f` to each top-level expression of a statement.
fn map_top_exprs(s: &mut Stmt, f: &mut impl FnMut(&mut Expr)) {
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

/// Number of recovered parameters of `func` (register + frame).
pub(crate) fn param_count(func: &IrFunction) -> usize {
    func.reg_params.len() + frame_params_locals(func).0.len()
}

fn ends_in_terminator(b: &Block) -> bool {
    matches!(
        b.stmts.last(),
        Some(Stmt::Jump(_) | Stmt::Branch { .. } | Stmt::Return(_) | Stmt::Switch { .. })
    )
}

/// Emit a complete compilable C translation unit for the given functions.
pub fn emit_unit(funcs: &[IrFunction]) -> String {
    // Always emit real parameter signatures; an interprocedural fixup then makes
    // every call to a function defined here match its callee's arity, so the
    // unit recompiles (call-site arguments — roadmap §15.4 #2).
    let mut funcs = funcs.to_vec();
    fixup_call_arity(&mut funcs);
    let with_params = true;
    let mut body = String::new();
    let mut forward: BTreeSet<u64> = BTreeSet::new();
    let defined: BTreeSet<u64> = funcs.iter().map(|f| f.entry).collect();
    for f in &funcs {
        body.push_str(&emit_function(f, &mut forward, with_params));
        body.push('\n');
    }
    let mut out = String::new();
    out.push_str("#include <stdint.h>\n\n");
    // Forward-declare every function (defined or external) with an empty
    // parameter list, so a call that precedes its callee's definition still has
    // a compatible prototype. The empty list is compatible with the real,
    // parameterised definition emitted below.
    for a in forward.union(&defined) {
        let _ = writeln!(out, "uint64_t sub_{:x}();", a);
    }
    out.push('\n');
    out.push_str(&body);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn emits_and_lowers_phi() {
        // Diamond with a phi -> after destruction, no Phi remains and copy
        // blocks are inserted.
        let rax = Location::Reg(RegId(0));
        let mut f = IrFunction {
            entry: 0,
            name: "t".into(),
            bits: 64,
            reg_params: vec![],
            frame_promotable: false,
            blocks: vec![
                Block { id: 0, addr: 0, stmts: vec![Stmt::Set { dst: rax.clone(), expr: Expr::konst(1, 32) }, Stmt::Branch { cond: Expr::konst(1, 8), taken: BlockId(1), fallthrough: BlockId(2) }], succ: vec![1, 2], pred: vec![] },
                Block { id: 1, addr: 1, stmts: vec![Stmt::Set { dst: rax.clone(), expr: Expr::konst(2, 32) }, Stmt::Jump(BlockId(3)) ], succ: vec![3], pred: vec![0] },
                Block { id: 2, addr: 2, stmts: vec![Stmt::Jump(BlockId(3))], succ: vec![3], pred: vec![0] },
                Block { id: 3, addr: 3, stmts: vec![Stmt::Return(Some(Expr::Read(rax.clone())))], succ: vec![], pred: vec![1, 2] },
            ],
            next_value: 0,
            next_temp: 0,
        };
        crate::ssa::to_ssa(&mut f);
        crate::opt::optimize(&mut f);
        let mut fwd = BTreeSet::new();
        let c = emit_function(&f, &mut fwd, true);
        assert!(c.contains("uint64_t sub_0(void)"));
        assert!(!c.contains("phi"), "phi must be lowered, got:\n{}", c);
        assert!(c.contains("return"));
    }
}
