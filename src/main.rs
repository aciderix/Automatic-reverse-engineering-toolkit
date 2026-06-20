//! ARET — Automatic Reverse Engineering Toolkit.
//!
//! Pipeline: load (PE/ELF/Mach-O) -> disassemble (x86/x64) -> recover
//! functions & CFG -> lift -> emit pseudo-C.

mod analysis;
mod builder;
mod cfg;
mod dataflow;
mod decompile;
mod disasm;
mod emit;
mod ir;
mod loader;
mod opt;
mod ssa;
mod structure;
mod types;
#[cfg(feature = "unpack")]
mod unpack;
mod verify;

use anyhow::{Context, Result};
use clap::{Parser, ValueEnum};
use disasm::Disassembler;
use loader::Program;
use std::path::PathBuf;

#[derive(Copy, Clone, Debug, PartialEq, Eq, ValueEnum)]
enum Mode {
    /// Summarize the binary (format, arch, sections, entry, symbols).
    Info,
    /// Linear disassembly listing of recovered functions.
    Asm,
    /// Dump the control-flow graph (blocks and edges) per function.
    Cfg,
    /// Emit pseudo-C (default).
    Decompile,
    /// Dump the typed SSA IR (lifter + SSA construction) — work in progress.
    Ir,
    /// Emit compilable C from the SSA IR (goto form) — work in progress.
    Emit,
    /// Emit LLVM IR from the SSA IR (experimental rev.ng-style backend).
    Llvm,
    /// Verify recompilability: emit C per function, recompile, report the rate.
    Verify,
    /// Transpile to a native executable for the target OS (UBT M1): intercept
    /// API imports into HLE shims, link, and recompile natively.
    Transpile,
    /// Dynamically unpack a packed PE: emulate the stub (Unicorn) until it
    /// decrypts the payload, detect the OEP, and report. Requires `--features unpack`.
    Unpack,
}

#[derive(Parser, Debug)]
#[command(
    name = "aret",
    about = "Automatic Reverse Engineering Toolkit — machine code to pseudo-C",
    version
)]
struct Args {
    /// Path to the target binary.
    binary: PathBuf,

    /// What to produce.
    #[arg(short, long, value_enum, default_value_t = Mode::Decompile)]
    mode: Mode,

    /// Only process the function whose name or hex address matches this.
    #[arg(short, long)]
    function: Option<String>,

    /// Write output to a file instead of stdout.
    #[arg(short, long)]
    output: Option<PathBuf>,

    /// Decompile mode only: write one .c file per function into this directory,
    /// plus an index.csv. Produces a browsable tree instead of one huge file.
    #[arg(long)]
    split: Option<PathBuf>,

    /// Emit flat goto-based output instead of structured if/while.
    #[arg(long)]
    flat: bool,

    /// Disable prologue scanning (faster, but only finds directly-called
    /// functions; skips code reached solely through vtables/callbacks).
    #[arg(long)]
    no_prologue_scan: bool,

    /// Cap the number of functions processed (used by --mode verify).
    #[arg(long)]
    limit: Option<usize>,

    /// Transpile mode: output directory for the generated C + native binary.
    #[arg(long, default_value = "aret_out")]
    out_dir: PathBuf,

    /// Transpile mode: run the produced native binary and print its output.
    #[arg(long)]
    run: bool,

    /// Transpile mode: override the entry point (hex address), e.g. to start at
    /// `main` and skip a heavy CRT startup.
    #[arg(long)]
    entry: Option<String>,

    /// Transpile mode: code-generation backend (`c` or `llvm`).
    #[arg(long, default_value = "c")]
    backend: String,

    /// Transpile mode: target triple (informational for M1; the host C
    /// compiler builds a native binary at the source's bitness).
    #[arg(long)]
    target: Option<String>,
}

/// Render one function as pseudo-C, structured unless `--flat` was given.
fn render_function(prog: &Program, func: &analysis::Function, flat: bool) -> String {
    if flat {
        decompile::decompile_function(prog, func)
    } else {
        structure::structure_function(prog, func)
    }
}

/// `--mode unpack`: emulate the packer stub until it decrypts, report the OEP,
/// and (with `--out-dir`) write a rebuilt clean PE of the decrypted image.
#[cfg(feature = "unpack")]
fn run_unpack(prog: &Program, out_dir: &std::path::Path) -> Result<String> {
    use std::fmt::Write as _;
    let mut out = String::new();
    out.push_str("ARET dynamic unpack (Unicorn)\n");
    let _ = writeln!(out, "  entry (packed): 0x{:x}", prog.entry);
    match unpack::unpack_program(prog, 50_000_000) {
        Ok(r) => {
            let _ = writeln!(out, "  OEP recovered:  0x{:x}", r.oep);
            let _ = writeln!(out, "  API calls:      {} import(s) serviced by the Win32 model", r.api_calls);
            let _ = writeln!(
                out,
                "  decrypted:      {} / {} bytes rewritten by the stub",
                r.decrypted_bytes, r.total_bytes
            );
            out.push_str("  status:         OEP reached — payload is now in cleartext in memory\n");
            std::fs::create_dir_all(out_dir).ok();
            let pe_path = out_dir.join("unpacked.exe");
            std::fs::write(&pe_path, &r.dump_pe)
                .with_context(|| format!("failed to write {}", pe_path.display()))?;
            let _ = writeln!(out, "  rebuilt PE:     {} ({} bytes, entry=OEP)", pe_path.display(), r.dump_pe.len());
            out.push_str("  next:           feed it back to `--mode transpile`\n");
        }
        Err(e) => {
            let _ = writeln!(out, "  status:         {}", e);
        }
    }
    Ok(out)
}

#[cfg(not(feature = "unpack"))]
fn run_unpack(_prog: &Program, _out_dir: &std::path::Path) -> Result<String> {
    anyhow::bail!("--mode unpack requires building with `--features unpack` (needs libunicorn-dev)")
}

fn main() -> Result<()> {
    let args = Args::parse();
    let data = std::fs::read(&args.binary)
        .with_context(|| format!("failed to read {}", args.binary.display()))?;

    let prog = Program::load(&data)?;

    if args.mode == Mode::Info {
        let out = render_info(&prog);
        return emit(&args, out);
    }

    if args.mode == Mode::Unpack {
        // Dynamic unpacking does not use static function recovery (the real code
        // is still encrypted) — emulate the stub instead.
        return emit(&args, run_unpack(&prog, &args.out_dir)?);
    }

    let disasm = Disassembler::new(prog.bitness);
    let result = analysis::analyze(&prog, &disasm, !args.no_prologue_scan);

    let functions: Vec<_> = result
        .functions
        .iter()
        .filter(|f| match &args.function {
            None => true,
            Some(sel) => {
                f.name == *sel
                    || format!("0x{:x}", f.entry) == sel.to_lowercase()
                    || format!("{:x}", f.entry) == sel.to_lowercase()
            }
        })
        .collect();

    let mut out = String::new();
    match args.mode {
        Mode::Info => unreachable!(),
        Mode::Asm => {
            for f in &functions {
                out.push_str(&render_asm(f));
                out.push('\n');
            }
        }
        Mode::Cfg => {
            for f in &functions {
                out.push_str(&render_cfg(f));
                out.push('\n');
            }
        }
        Mode::Decompile => {
            if let Some(dir) = &args.split {
                return write_split(dir, &prog, &functions, result.instruction_count, args.flat);
            }
            out.push_str(&format!(
                "// Decompiled by ARET — {} functions recovered, {} instructions decoded\n\n",
                result.functions.len(),
                result.instruction_count
            ));
            for f in &functions {
                out.push_str(&render_function(&prog, f, args.flat));
                out.push('\n');
            }
        }
        Mode::Ir => {
            for f in &functions {
                let mut irf = ir::build::build_ir(&prog, f);
                // Stack-slot recovery (§4.1): rewrite safe rsp/rbp-relative
                // accesses into named `Frame` locals — both a readability win and
                // a correctness fix (a standalone recompile's frame register is
                // uninitialised). Runs pre-SSA, where the frame base is a register
                // read. The diagnostic reports how many slots were promoted.
                let promoted = opt::frame::promote_stack_slots(&mut irf);
                ssa::to_ssa(&mut irf);
                opt::optimize(&mut irf);
                if promoted > 0 {
                    out.push_str(&format!("// stack: {} slot(s) promoted to locals\n", promoted));
                }
                out.push_str(&ir::build::dump(&irf));
                out.push('\n');
            }
        }
        Mode::Emit => {
            let irfs: Vec<_> = functions
                .iter()
                .map(|f| {
                    let mut irf = ir::build::build_ir(&prog, f);
                    opt::frame::promote_stack_slots(&mut irf);
                    ssa::to_ssa(&mut irf);
                    opt::optimize(&mut irf);
                    irf
                })
                .collect();
            if args.flat {
                out.push_str(&emit::emit_unit(&irfs));
            } else {
                out.push_str(&emit::structured::emit_unit(&irfs));
            }
        }
        Mode::Llvm => {
            let irfs: Vec<_> = functions
                .iter()
                .map(|f| {
                    let mut irf = ir::build::build_ir(&prog, f);
                    opt::frame::promote_stack_slots(&mut irf);
                    ssa::to_ssa(&mut irf);
                    opt::optimize(&mut irf);
                    irf
                })
                .collect();
            out.push_str(&emit::llvm::emit_unit(&irfs));
        }
        Mode::Verify => {
            let limit = args.limit.unwrap_or(200);
            let report = verify::run(&prog, &functions, limit, &args.backend);
            out.push_str(&report.render());
        }
        Mode::Transpile => {
            if let Some(t) = &args.target {
                eprintln!("note: --target {} (M1 builds a native host binary at the source bitness)", t);
            }
            let entry_override = match &args.entry {
                Some(s) => {
                    let h = s.trim_start_matches("0x");
                    Some(u64::from_str_radix(h, 16).context("invalid --entry hex")?)
                }
                None => None,
            };
            let report = builder::transpile(
                &prog,
                &functions,
                &args.out_dir,
                args.run,
                entry_override,
                &args.backend,
            )?;
            out.push_str(&report.render());
        }
        Mode::Unpack => unreachable!("handled before function recovery"),
    }

    if functions.is_empty() {
        eprintln!("warning: no functions matched / recovered");
    }

    emit(&args, out)
}

fn emit(args: &Args, out: String) -> Result<()> {
    match &args.output {
        Some(path) => {
            std::fs::write(path, out)
                .with_context(|| format!("failed to write {}", path.display()))?;
            eprintln!("wrote {}", path.display());
        }
        None => print!("{}", out),
    }
    Ok(())
}

/// Replace characters that are unsafe in a filename.
fn sanitize(name: &str) -> String {
    name.chars()
        .map(|c| if c.is_ascii_alphanumeric() || c == '_' { c } else { '_' })
        .collect()
}

/// Write one .c file per function plus an index.csv into `dir`.
fn write_split(
    dir: &std::path::Path,
    prog: &Program,
    functions: &[&analysis::Function],
    insn_count: usize,
    flat: bool,
) -> Result<()> {
    std::fs::create_dir_all(dir)
        .with_context(|| format!("failed to create {}", dir.display()))?;

    // Render + write each function in parallel (independent, prog is read-only).
    use rayon::prelude::*;
    let written: Result<()> = functions.par_iter().try_for_each(|f| {
        let fname = format!("{}.c", sanitize(&f.name));
        let body = render_function(prog, f, flat);
        std::fs::write(dir.join(&fname), body)
            .with_context(|| format!("failed to write {}", fname))
    });
    written?;

    let mut index = String::from("name,entry,basic_blocks,callees\n");
    for f in functions {
        index.push_str(&format!(
            "{},0x{:x},{},{}\n",
            f.name,
            f.entry,
            f.blocks.len(),
            f.callees.len()
        ));
    }
    std::fs::write(dir.join("index.csv"), index)?;

    eprintln!(
        "wrote {} function files (+ index.csv) to {} — {} instructions decoded",
        functions.len(),
        dir.display(),
        insn_count
    );
    Ok(())
}

fn render_info(prog: &Program) -> String {
    use std::fmt::Write;
    let mut s = String::new();
    let _ = writeln!(s, "Format:   {}", prog.format);
    let _ = writeln!(s, "Bitness:  {}-bit", prog.bitness.bits());
    let _ = writeln!(s, "Entry:    0x{:x}", prog.entry);
    let _ = writeln!(s, "Sections: {}", prog.sections.len());
    for sec in &prog.sections {
        let _ = writeln!(
            s,
            "  {:<16} 0x{:<10x} {:>8} bytes  {}{}",
            sec.name,
            sec.address,
            sec.data.len(),
            if sec.executable { "X" } else { "-" },
            if sec.writable { "W" } else { "-" },
        );
    }
    let _ = writeln!(s, "Imports:  {}", prog.imports.len());
    for (addr, name) in prog.imports.iter().take(8) {
        let _ = writeln!(s, "  0x{:<10x} {}", addr, name);
    }
    let _ = writeln!(s, "Symbols:  {}", prog.symbols.len());
    for sym in prog.symbols.values().take(40) {
        let _ = writeln!(
            s,
            "  0x{:<10x} {} {}",
            sym.address,
            if sym.is_function { "fn " } else { "var" },
            sym.name
        );
    }
    if prog.symbols.len() > 40 {
        let _ = writeln!(s, "  ... and {} more", prog.symbols.len() - 40);
    }
    s
}

fn render_asm(f: &analysis::Function) -> String {
    use std::fmt::Write;
    let mut s = String::new();
    let _ = writeln!(s, "; {} @ 0x{:x}", f.name, f.entry);
    for blk in f.blocks.values() {
        for insn in &blk.insns {
            let _ = writeln!(s, "  0x{:08x}:  {}", insn.address, insn.text);
        }
    }
    s
}

fn render_cfg(f: &analysis::Function) -> String {
    use std::fmt::Write;
    let mut s = String::new();
    let _ = writeln!(s, "function {} @ 0x{:x}", f.name, f.entry);
    for blk in f.blocks.values() {
        let succ: Vec<String> = blk.successors.iter().map(|a| format!("0x{:x}", a)).collect();
        let _ = writeln!(
            s,
            "  block 0x{:x}..0x{:x}  [{:?}] -> [{}]",
            blk.start,
            blk.end(),
            blk.terminator,
            succ.join(", ")
        );
    }
    if !f.callees.is_empty() {
        let callees: Vec<String> = f.callees.iter().map(|a| format!("0x{:x}", a)).collect();
        let _ = writeln!(s, "  calls: {}", callees.join(", "));
    }
    s
}
