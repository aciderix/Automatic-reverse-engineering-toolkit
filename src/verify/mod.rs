//! Verification harness (roadmap §8): the closed loop that makes ARET
//! *measurable*. This first level implements §8.2 level 1 — **recompilability**:
//! emit C for each function, compile it with a real C compiler, and report the
//! fraction that recompiles. That fraction is the first form of the north-star
//! metric (§0.3); higher levels (differential execution, SMT equivalence) build
//! on this harness.
//!
//! Compilation is done by piping the emitted C to `cc -x c - -c` (no temp files
//! or linking needed), so a function with undefined callees still compiles via
//! forward declarations.

use crate::analysis::Function;
use crate::emit;
use crate::ir;
use crate::loader::Program;
use crate::ssa;
use std::fmt::Write as _;
use std::io::Write as _;
use std::process::{Command, Stdio};

pub struct Report {
    pub total: usize,
    pub compiled: usize,
    /// (function name, first compiler error line) for a sample of failures.
    pub failures: Vec<(String, String)>,
    pub cc: String,
}

impl Report {
    pub fn render(&self) -> String {
        let mut s = String::new();
        let pct = if self.total == 0 {
            0.0
        } else {
            100.0 * self.compiled as f64 / self.total as f64
        };
        let _ = writeln!(s, "ARET verification — recompilability (level 1)");
        let _ = writeln!(s, "  compiler:   {}", self.cc);
        let _ = writeln!(
            s,
            "  recompiled: {}/{} functions ({:.1}%)",
            self.compiled, self.total, pct
        );
        if !self.failures.is_empty() {
            let _ = writeln!(s, "  sample failures:");
            for (name, err) in self.failures.iter().take(10) {
                let _ = writeln!(s, "    {}: {}", name, err);
            }
        }
        s
    }
}

/// Pick a C compiler: $CC, else cc, else gcc.
fn pick_cc() -> String {
    std::env::var("CC").unwrap_or_else(|_| "cc".to_string())
}

/// Try to compile a C translation unit; returns the first error line on failure.
fn try_compile(cc: &str, src: &str) -> Result<(), String> {
    let mut child = match Command::new(cc)
        .args(["-x", "c", "-", "-c", "-w", "-o", "/dev/null"])
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
    {
        Ok(c) => c,
        Err(e) => return Err(format!("cannot run {}: {}", cc, e)),
    };
    {
        let mut stdin = child.stdin.take().unwrap();
        if let Err(e) = stdin.write_all(src.as_bytes()) {
            return Err(format!("write failed: {}", e));
        }
    } // stdin dropped here -> EOF
    let out = match child.wait_with_output() {
        Ok(o) => o,
        Err(e) => return Err(format!("wait failed: {}", e)),
    };
    if out.status.success() {
        Ok(())
    } else {
        let err = String::from_utf8_lossy(&out.stderr);
        let first = err
            .lines()
            .find(|l| l.contains("error"))
            .or_else(|| err.lines().next())
            .unwrap_or("unknown error")
            .trim()
            .to_string();
        Err(first)
    }
}

/// Run the recompilability check over `funcs` (capped at `limit`).
pub fn run(prog: &Program, funcs: &[&Function], limit: usize) -> Report {
    let cc = pick_cc();
    let mut compiled = 0;
    let mut failures = Vec::new();
    let mut total = 0;

    for f in funcs.iter().take(limit) {
        total += 1;
        let mut irf = ir::build::build_ir(prog, f);
        ssa::to_ssa(&mut irf);
        crate::opt::optimize(&mut irf);
        let src = emit::emit_unit(std::slice::from_ref(&irf));
        match try_compile(&cc, &src) {
            Ok(()) => compiled += 1,
            Err(e) => {
                if failures.len() < 20 {
                    failures.push((f.name.clone(), e));
                }
            }
        }
    }

    Report {
        total,
        compiled,
        failures,
        cc,
    }
}
