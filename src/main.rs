//! ARET — Automatic Reverse Engineering Toolkit.
//!
//! Pipeline: load (PE/ELF/Mach-O) -> disassemble (x86/x64) -> recover
//! functions & CFG -> lift -> emit pseudo-C.

mod analysis;
mod decompile;
mod disasm;
mod ir;
mod loader;

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
    let result = analysis::analyze(&prog, &disasm);

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
            out.push_str(&format!(
                "// Decompiled by ARET — {} functions recovered\n\n",
                result.functions.len()
            ));
            for f in &functions {
                out.push_str(&decompile::decompile_function(&prog, f));
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
