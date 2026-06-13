//! ARET — Automatic Reverse Engineering Toolkit.
//!
//! Pipeline: load (PE/ELF/Mach-O) -> disassemble (x86/x64) -> recover
//! functions & CFG -> lift -> emit pseudo-C.

mod analysis;
mod decompile;
mod disasm;
mod ir;
mod loader;
mod structure;

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
}

/// Render one function as pseudo-C, structured unless `--flat` was given.
fn render_function(prog: &Program, func: &analysis::Function, flat: bool) -> String {
    if flat {
        decompile::decompile_function(prog, func)
    } else {
        structure::structure_function(prog, func)
    }
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

    let mut index = String::from("name,entry,basic_blocks,callees\n");
    for f in functions {
        let fname = format!("{}.c", sanitize(&f.name));
        let body = render_function(prog, f, flat);
        std::fs::write(dir.join(&fname), body)
            .with_context(|| format!("failed to write {}", fname))?;
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
