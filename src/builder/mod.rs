//! Backend / packaging (UBT Phase 4, milestone M1): turn the transpiled C plus
//! the HLE compatibility layer into a *native, runnable* executable for the
//! target OS.
//!
//! M1 scope: a freestanding Win32 PE (kernel32 imports only) is transpiled to C
//! by the ARET middle-end, its API imports are intercepted into HLE shim calls
//! (see `ir::build::name_calls_in_expr`), and this builder links the result with
//! `runtime/aret_hle` and a generated `main` into a native ELF via the system C
//! compiler. The produced binary runs natively — not under an emulator or Wine.

use crate::analysis::Function;
use crate::emit;
use crate::ir;
use crate::loader::Program;
use crate::opt;
use crate::ssa;
use anyhow::{bail, Context, Result};
use std::path::Path;
use std::process::Command;

/// The HLE runtime, embedded so the built `aret` binary is self-contained.
const HLE_H: &str = include_str!("../../runtime/aret_hle/aret_hle.h");
const HLE_C: &str = include_str!("../../runtime/aret_hle/aret_hle.c");

pub struct TranspileReport {
    pub out_dir: std::path::PathBuf,
    pub binary: std::path::PathBuf,
    pub functions: usize,
    pub bits: u32,
    /// Captured stdout if the binary was run, else `None`.
    pub run_output: Option<String>,
}

impl TranspileReport {
    pub fn render(&self) -> String {
        let mut s = String::new();
        s.push_str("ARET transpile (UBT M1) — native recompile\n");
        s.push_str(&format!("  functions:  {}\n", self.functions));
        s.push_str(&format!("  bitness:    {}-bit\n", self.bits));
        s.push_str(&format!("  output dir: {}\n", self.out_dir.display()));
        s.push_str(&format!("  binary:     {}\n", self.binary.display()));
        if let Some(out) = &self.run_output {
            s.push_str("  --- program output ---\n");
            for line in out.lines() {
                s.push_str("  | ");
                s.push_str(line);
                s.push('\n');
            }
        }
        s
    }
}

/// Build IR for one recovered function and lower it through the standard chain.
fn lower(prog: &Program, f: &Function) -> ir::types::IrFunction {
    let mut irf = ir::build::build_ir(prog, f);
    opt::frame::promote_stack_slots(&mut irf);
    ssa::to_ssa(&mut irf);
    opt::optimize(&mut irf);
    irf
}

/// Transpile `funcs` to C, link with the HLE runtime, and produce a native
/// executable in `out_dir`. When `run` is set, execute it and capture stdout.
pub fn transpile(
    prog: &Program,
    funcs: &[&Function],
    out_dir: &Path,
    run: bool,
) -> Result<TranspileReport> {
    if funcs.is_empty() {
        bail!("no functions to transpile");
    }
    std::fs::create_dir_all(out_dir)
        .with_context(|| format!("failed to create {}", out_dir.display()))?;

    // Lower every function and emit one translation unit.
    let irfs: Vec<_> = funcs.iter().map(|f| lower(prog, f)).collect();
    let unit = emit::structured::emit_unit(&irfs);

    // The program's entry point is the C function `sub_<entry>`. Confirm it is
    // among the emitted functions so the generated `main` can call it.
    let entry = prog.entry;
    if !irfs.iter().any(|f| f.entry == entry) {
        bail!(
            "entry function 0x{:x} was not recovered/emitted (try without --function)",
            entry
        );
    }

    // program.c — the transpiled unit, with the HLE declarations in scope.
    let program_c = format!("#include \"aret_hle.h\"\n{}", unit);
    // aret_main.c — a native entry that drives the transpiled entry point.
    let main_c = format!(
        "#include <stdint.h>\n\nuint64_t sub_{:x}(void);\n\nint main(void) {{\n    return (int)sub_{:x}();\n}}\n",
        entry, entry
    );

    let write = |name: &str, body: &str| -> Result<()> {
        std::fs::write(out_dir.join(name), body)
            .with_context(|| format!("failed to write {}", name))
    };
    write("aret_hle.h", HLE_H)?;
    write("aret_hle.c", HLE_C)?;
    write("program.c", &program_c)?;
    write("aret_main.c", &main_c)?;

    // Compile + link natively. The stack-model C masks pointers to 32 bits, so a
    // 32-bit source must be built as a 32-bit process (-m32); 64-bit as -m64.
    let bits = prog.bitness.bits() as u32;
    let march = if bits == 32 { "-m32" } else { "-m64" };
    let cc = std::env::var("CC").unwrap_or_else(|_| "cc".to_string());
    let binary = out_dir.join("app");

    let output = Command::new(&cc)
        .args([
            march,
            "-w",
            "-fno-strict-aliasing",
            "-fno-builtin",
            "-O0",
        ])
        .arg(out_dir.join("program.c"))
        .arg(out_dir.join("aret_hle.c"))
        .arg(out_dir.join("aret_main.c"))
        .arg("-I")
        .arg(out_dir)
        .arg("-o")
        .arg(&binary)
        .output()
        .with_context(|| format!("failed to run {}", cc))?;

    if !output.status.success() {
        let err = String::from_utf8_lossy(&output.stderr);
        bail!("native recompile failed:\n{}", err.trim());
    }

    let run_output = if run {
        let out = Command::new(&binary)
            .output()
            .with_context(|| format!("failed to run {}", binary.display()))?;
        let mut s = String::from_utf8_lossy(&out.stdout).into_owned();
        if !out.stderr.is_empty() {
            s.push_str(&String::from_utf8_lossy(&out.stderr));
        }
        Some(s)
    } else {
        None
    };

    Ok(TranspileReport {
        out_dir: out_dir.to_path_buf(),
        binary,
        functions: irfs.len(),
        bits,
        run_output,
    })
}
