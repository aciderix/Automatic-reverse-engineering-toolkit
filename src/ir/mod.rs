//! Lightweight instruction lifting: translate a single x86 instruction's
//! Intel-syntax text into one or more readable pseudo-C statements.
//!
//! This is deliberately a *local* semantic translation (no global SSA yet):
//! it maps mnemonics to C operators and rewrites memory operands as typed
//! dereferences. Control-flow instructions are handled by the decompiler, not
//! here.

/// Typed SSA IR (roadmap foundation; being built in parallel with the text
/// pipeline below until it reaches parity).
pub mod types;
/// IR lifter (machine code -> typed IR), parallel to the text lifting below.
pub mod lift;

use crate::disasm::Insn;
use crate::loader::Program;
use iced_x86::ConditionCode;

/// Split an Intel-formatted instruction into `(mnemonic, operands)`.
pub fn split_insn(text: &str) -> (String, Vec<String>) {
    let text = text.trim();
    match text.split_once(' ') {
        Some((mn, rest)) => {
            let ops = rest
                .split(',')
                .map(|s| s.trim().to_string())
                .filter(|s| !s.is_empty())
                .collect();
            (mn.to_lowercase(), ops)
        }
        None => (text.to_lowercase(), Vec::new()),
    }
}

/// Map an x86 size prefix to a C integer type for casts.
fn c_type_for_prefix(prefix: &str) -> &'static str {
    let p = prefix.trim();
    if p.contains("byte") {
        "uint8_t"
    } else if p.contains("word ptr") && !p.contains("dword") && !p.contains("qword") {
        "uint16_t"
    } else if p.contains("dword") {
        "uint32_t"
    } else if p.contains("qword") {
        "uint64_t"
    } else {
        "uint32_t"
    }
}

/// Translate one operand token into a C expression.
pub fn operand_to_c(op: &str) -> String {
    let op = op.trim();
    if let (Some(lb), Some(rb)) = (op.find('['), op.rfind(']')) {
        let prefix = &op[..lb];
        let inner = op[lb + 1..rb].trim();
        // A pure frame slot (ebp/rbp +/- disp) becomes a named variable.
        if let Some((name, _, _)) = frame_var(inner) {
            return name;
        }
        let ty = c_type_for_prefix(prefix);
        return format!("*({}*)({})", ty, inner);
    }
    op.to_string()
}

/// Classification of a stack-frame slot.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FrameKind {
    Arg,
    Local,
}

/// Recognise a *pure* `ebp`/`rbp`-relative slot and name it. Returns
/// `(name, signed_displacement, kind)`, or `None` for indexed expressions,
/// the saved frame pointer, and the return-address slot. Pointer width is
/// inferred from the base register (`ebp` = 4, `rbp` = 8).
pub fn frame_var(inner: &str) -> Option<(String, i64, FrameKind)> {
    let s = inner.trim();
    let (reg, ptr): (&str, i64) = if s.starts_with("rbp") {
        ("rbp", 8)
    } else if s.starts_with("ebp") {
        ("ebp", 4)
    } else {
        return None;
    };
    let rest = &s[reg.len()..];
    let disp: i64 = if rest.is_empty() {
        0
    } else {
        let sign = match rest.as_bytes()[0] {
            b'+' => 1,
            b'-' => -1,
            _ => return None,
        };
        let body = &rest[1..];
        // iced prints small displacements bare (e.g. `8`) and larger ones with
        // a `0x` prefix; accept both, but reject indexed/compound expressions.
        let magnitude = if let Some(hex) = body.strip_prefix("0x") {
            if hex.is_empty() || !hex.bytes().all(|c| c.is_ascii_hexdigit()) {
                return None;
            }
            i64::from_str_radix(hex, 16).ok()?
        } else if !body.is_empty() && body.bytes().all(|c| c.is_ascii_digit()) {
            body.parse::<i64>().ok()?
        } else {
            return None; // indexed / compound expression, not a plain slot
        };
        sign * magnitude
    };

    if disp == 0 {
        return None; // saved frame pointer
    }
    if disp > 0 {
        if disp == ptr {
            return None; // return address
        }
        Some((format!("arg_{:x}", disp), disp, FrameKind::Arg))
    } else {
        Some((format!("local_{:x}", -disp), disp, FrameKind::Local))
    }
}

/// Map a size prefix to a `(C type, width-in-bytes)`.
fn type_and_width(prefix: &str) -> (&'static str, u8) {
    match c_type_for_prefix(prefix) {
        "uint8_t" => ("uint8_t", 1),
        "uint16_t" => ("uint16_t", 2),
        "uint64_t" => ("uint64_t", 8),
        _ => ("uint32_t", 4),
    }
}

/// Scan a function's instructions for frame variables, returning the recovered
/// argument and local declarations as `(name, c_type)`, each sorted by offset.
pub fn scan_frame_vars(
    func: &crate::analysis::Function,
) -> (Vec<(String, &'static str)>, Vec<(String, &'static str)>) {
    use std::collections::BTreeMap;
    // disp -> (name, kind, type, width)
    let mut map: BTreeMap<i64, (String, FrameKind, &'static str, u8)> = BTreeMap::new();

    for blk in func.blocks.values() {
        for insn in &blk.insns {
            let (_, ops) = split_insn(&insn.text);
            for op in &ops {
                let (lb, rb) = match (op.find('['), op.rfind(']')) {
                    (Some(l), Some(r)) => (l, r),
                    _ => continue,
                };
                let prefix = &op[..lb];
                let inner = op[lb + 1..rb].trim();
                if let Some((name, disp, kind)) = frame_var(inner) {
                    let (ty, w) = type_and_width(prefix);
                    let e = map.entry(disp).or_insert((name, kind, ty, w));
                    if w > e.3 {
                        e.2 = ty;
                        e.3 = w;
                    }
                }
            }
        }
    }

    let mut args: Vec<(i64, String, &'static str)> = Vec::new();
    let mut locals: Vec<(i64, String, &'static str)> = Vec::new();
    for (disp, (name, kind, ty, _)) in map {
        match kind {
            FrameKind::Arg => args.push((disp, name, ty)),
            FrameKind::Local => locals.push((-disp, name, ty)),
        }
    }
    args.sort_by_key(|(d, _, _)| *d);
    locals.sort_by_key(|(d, _, _)| *d);
    (
        args.into_iter().map(|(_, n, t)| (n, t)).collect(),
        locals.into_iter().map(|(_, n, t)| (n, t)).collect(),
    )
}

/// For `lea`, we want the *address* expression, not a dereference. A frame
/// slot becomes `&local_x` / `&arg_x`.
fn lea_src(op: &str) -> String {
    if let (Some(lb), Some(rb)) = (op.find('['), op.rfind(']')) {
        let inner = op[lb + 1..rb].trim();
        if let Some((name, _, _)) = frame_var(inner) {
            return format!("&{}", name);
        }
        inner.to_string()
    } else {
        op.to_string()
    }
}

/// Resolve a call instruction to its callable C name.
pub fn call_name_of(insn: &Insn, prog: &Program) -> String {
    let (_, ops) = split_insn(&insn.text);
    call_name(insn, prog, &ops)
}

/// Resolve a call target to a callable C name.
fn call_name(insn: &Insn, prog: &Program, ops: &[String]) -> String {
    if let Some(t) = insn.target {
        if let Some(name) = prog.symbol_name(t) {
            return name.to_string();
        }
        return format!("sub_{:x}", t);
    }
    // Indirect: call through the operand expression.
    ops.first()
        .map(|o| format!("(*({}))", operand_to_c(o)))
        .unwrap_or_else(|| "(*indirect)".to_string())
}

/// Translate a non-control instruction into pseudo-C statements.
/// Returns an empty vec for instructions that produce no visible effect
/// (nop, endbr, alignment).
pub fn lift_insn(insn: &Insn, prog: &Program) -> Vec<String> {
    let (mn, ops) = split_insn(&insn.text);
    let m = mn.as_str();

    // Helpers for common arities.
    let bin = |op: &str| -> Vec<String> {
        if ops.len() == 2 {
            vec![format!("{} {} {};", operand_to_c(&ops[0]), op, operand_to_c(&ops[1]))]
        } else {
            vec![format!("// {}", insn.text)]
        }
    };

    match m {
        "nop" | "endbr64" | "endbr32" | "fnop" | "hint_nop" => vec![],

        "mov" | "movabs" => {
            if ops.len() == 2 {
                vec![format!("{} = {};", operand_to_c(&ops[0]), operand_to_c(&ops[1]))]
            } else {
                vec![format!("// {}", insn.text)]
            }
        }
        "movzx" | "movsx" | "movsxd" => {
            if ops.len() == 2 {
                let sign = if m == "movzx" { "(unsigned)" } else { "(signed)" };
                vec![format!(
                    "{} = {}{};",
                    operand_to_c(&ops[0]),
                    sign,
                    operand_to_c(&ops[1])
                )]
            } else {
                vec![format!("// {}", insn.text)]
            }
        }
        "lea" => {
            if ops.len() == 2 {
                vec![format!("{} = {};", operand_to_c(&ops[0]), lea_src(&ops[1]))]
            } else {
                vec![format!("// {}", insn.text)]
            }
        }

        "add" => bin("+="),
        "sub" => bin("-="),
        "and" => bin("&="),
        "or" => bin("|="),
        "xor" => {
            if ops.len() == 2 && ops[0] == ops[1] {
                vec![format!("{} = 0;", operand_to_c(&ops[0]))]
            } else {
                bin("^=")
            }
        }
        "shl" | "sal" => bin("<<="),
        "shr" | "sar" => bin(">>="),
        "imul" | "mul" if ops.len() == 2 => bin("*="),

        "inc" => vec![format!("{}++;", operand_to_c(&ops[0]))],
        "dec" => vec![format!("{}--;", operand_to_c(&ops[0]))],
        "neg" if !ops.is_empty() => {
            let d = operand_to_c(&ops[0]);
            vec![format!("{} = -{};", d, d)]
        }
        "not" if !ops.is_empty() => {
            let d = operand_to_c(&ops[0]);
            vec![format!("{} = ~{};", d, d)]
        }

        "push" if !ops.is_empty() => vec![format!("push({});", operand_to_c(&ops[0]))],
        "pop" if !ops.is_empty() => vec![format!("{} = pop();", operand_to_c(&ops[0]))],

        // Flag-setting compares are consumed by the decompiler when forming
        // branch conditions; keep a comment so blocks without a branch still
        // show intent.
        "cmp" | "test" => vec![format!("// flags = {}", insn.text)],

        "call" => {
            let name = call_name(insn, prog, &ops);
            // We don't recover the real argument list; record the call site.
            vec![format!("{}();", name)]
        }

        "leave" => vec!["// leave (restore frame)".to_string()],
        "cdq" | "cqo" | "cwde" | "cdqe" => vec![format!("// sign-extend ({})", m)],

        _ => vec![format!("__asm__(\"{}\");", insn.text)],
    }
}

/// Render a conditional-branch condition as a C boolean expression, given the
/// most recent flag-setting compare in the block (if known).
///
/// `cmp` is the `(lhs, rhs, was_test)` of the preceding cmp/test, where
/// `was_test` distinguishes `test a,b` (AND) from `cmp a,b` (subtract).
pub fn branch_condition(insn: &Insn, cmp: Option<&(String, String, bool)>) -> String {
    let cc = insn.raw.condition_code();
    match cmp {
        Some((lhs, rhs, is_test)) => render_with_cmp(cc, lhs, rhs, *is_test),
        None => render_flag_only(cc).to_string(),
    }
}

fn render_with_cmp(cc: ConditionCode, lhs: &str, rhs: &str, is_test: bool) -> String {
    use ConditionCode as C;
    // `test a, a` followed by je/jne is the canonical "is zero" test.
    if is_test {
        let zero_form = lhs == rhs;
        return match cc {
            C::e if zero_form => format!("{} == 0", lhs),
            C::e => format!("({} & {}) == 0", lhs, rhs),
            C::ne if zero_form => format!("{} != 0", lhs),
            C::ne => format!("({} & {}) != 0", lhs, rhs),
            C::s => format!("(int64_t)({} & {}) < 0", lhs, rhs),
            C::ns => format!("(int64_t)({} & {}) >= 0", lhs, rhs),
            C::le | C::l => format!("(int64_t)({} & {}) <= 0", lhs, rhs),
            C::g | C::a => format!("(int64_t)({} & {}) > 0", lhs, rhs),
            _ => format!("/*test*/ ({} & {})", lhs, rhs),
        };
    }
    match cc {
        C::e => format!("{} == {}", lhs, rhs),
        C::ne => format!("{} != {}", lhs, rhs),
        // signed comparisons
        C::l => format!("(int64_t){} < (int64_t){}", lhs, rhs),
        C::ge => format!("(int64_t){} >= (int64_t){}", lhs, rhs),
        C::le => format!("(int64_t){} <= (int64_t){}", lhs, rhs),
        C::g => format!("(int64_t){} > (int64_t){}", lhs, rhs),
        // unsigned comparisons
        C::b => format!("{} < {}", lhs, rhs),
        C::ae => format!("{} >= {}", lhs, rhs),
        C::be => format!("{} <= {}", lhs, rhs),
        C::a => format!("{} > {}", lhs, rhs),
        C::s => format!("(int64_t)({} - {}) < 0", lhs, rhs),
        C::ns => format!("(int64_t)({} - {}) >= 0", lhs, rhs),
        _ => format!("/*cc*/ {} ?? {}", lhs, rhs),
    }
}

fn render_flag_only(cc: ConditionCode) -> &'static str {
    use ConditionCode as C;
    match cc {
        C::e => "ZF",
        C::ne => "!ZF",
        C::l => "SF != OF",
        C::ge => "SF == OF",
        C::le => "ZF || SF != OF",
        C::g => "!ZF && SF == OF",
        C::b => "CF",
        C::ae => "!CF",
        C::be => "CF || ZF",
        C::a => "!CF && !ZF",
        C::s => "SF",
        C::ns => "!SF",
        C::o => "OF",
        C::no => "!OF",
        C::p => "PF",
        C::np => "!PF",
        C::None => "true",
    }
}
