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
mod flirt;
mod structure;
mod types;
#[cfg(feature = "unpack")]
mod cpudiff;
#[cfg(feature = "unpack")]
mod unpack;
mod verify;

use anyhow::{bail, Context, Result};
use clap::{Parser, ValueEnum};
use disasm::Disassembler;
use loader::Program;
use std::path::PathBuf;

#[derive(Copy, Clone, Debug, PartialEq, Eq, ValueEnum)]
enum Mode {
    /// Summarize the binary (format, arch, sections, entry, symbols).
    Info,
    /// Report axis-2 (OS/CRT) import coverage: which of the binary's imports ARET
    /// already shims vs which would hit the unimplemented stub — the a-priori,
    /// known-in-advance measure of how ready ARET is for this binary.
    Imports,
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
    /// Print the complete static "wall map" — every coverage gap the runtime could
    /// hit (unmodelled instructions with site counts, unimplemented imports,
    /// unresolved direct calls) — in one pass, without emitting or compiling.
    Walls,
    /// Dynamically unpack a packed PE: emulate the stub (Unicorn) until it
    /// decrypts the payload, detect the OEP, and report. Requires `--features unpack`.
    Unpack,
    /// Emit FLIRT-lite signatures for the CRT/glue functions of a binary that has
    /// symbols (to build the recognition database for stripped binaries).
    Gensig,
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

    /// Transpile `--run`: arguments to pass to the transpiled program, after a
    /// `--` separator (e.g. `aret prog.exe --mode transpile --run -- foo bar`).
    /// They reach the program as argv (and `GetCommandLineA`/`W`).
    #[arg(last = true)]
    prog_args: Vec<String>,

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

    /// Transpile mode: exit non-zero if the result is not provably sound (any
    /// direct call to an unrecovered function, or any partially-simulated
    /// function with opaque asm). For pipelines that must never ship a binary
    /// known to misbehave.
    #[arg(long)]
    strict: bool,

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

    /// Merge an external IAT symbol map (JSON `{ "0xVA": "Name", ... }`) into the
    /// program's imports — e.g. a Scylla-style reconstruction from an unpacker.
    /// Lets ARET name calls through a rebuilt IAT that the PE headers don't list.
    #[arg(long)]
    iat_symbols: Option<PathBuf>,

    /// Transpile mode: seed initial memory from a runtime snapshot (ARETSNP1)
    /// instead of the static sections — so a lifted function runs against the
    /// program's real post-init state (the A+B "save-state" path).
    #[arg(long)]
    snapshot: Option<PathBuf>,

    /// DLL lifting (doc 80 §1.2): load a DLL alongside the binary and lift it
    /// too, so the app's imports of its exports dispatch to the lifted code
    /// instead of an HLE shim. Repeatable; each value is `name=path` (the name
    /// must match the import DLL name, e.g. `--with-dll mydll.dll=./mydll.dll`).
    #[arg(long, value_name = "NAME=PATH")]
    with_dll: Vec<String>,

    /// Auto-lift the C++/third-party runtime (doc 81 I2.b): read the exe's imports,
    /// and for every NON-system DLL it needs (libstdc++/libgcc/libwinpthread/glib/…),
    /// find the file (beside the exe, then `--dll-path` dirs, then bench/.cache) and
    /// lift it too — recursively through its own imports — so the app's calls dispatch
    /// to lifted code instead of an unimplemented import. System/OS DLLs (kernel32,
    /// user32, ws2_32, msvcrt, …) are always SHIMMED, never lifted. A runtime DLL not
    /// found on disk is left shim-bound (a sound abort on use), never a crash. Opt-in.
    #[arg(long)]
    auto_lift: bool,

    /// Extra directories searched by `--auto-lift` to resolve runtime DLLs (repeatable).
    #[arg(long, value_name = "DIR")]
    dll_path: Vec<PathBuf>,
}

/// OS / CRT DLLs that ARET reimplements as native HLE shims — never lifted by
/// `--auto-lift` (lifting kernel32 would chase Windows syscalls; msvcrt is the C
/// runtime ARET already models). Everything else that the exe imports and that is
/// found on disk is a runtime/third-party DLL we lift. Match is case-insensitive and
/// `.dll`-suffix-insensitive; the `api-ms-win-*`/`ext-ms-*` virtual sets and the
/// versioned MSVC/UCRT CRTs are covered by prefix.
fn is_system_dll(name: &str) -> bool {
    let n = name.trim().to_ascii_lowercase();
    let n = n.strip_suffix(".dll").unwrap_or(&n);
    const SYS: &[&str] = &[
        "kernel32", "kernelbase", "ntdll", "user32", "gdi32", "gdi32full", "advapi32",
        "shell32", "shlwapi", "ole32", "oleaut32", "combase", "rpcrt4", "ws2_32", "wsock32",
        "mswsock", "iphlpapi", "dnsapi", "comctl32", "comdlg32", "imm32", "winmm", "version",
        "crypt32", "bcrypt", "secur32", "userenv", "setupapi", "winspool", "uxtheme",
        "dwmapi", "powrprof", "psapi", "wtsapi32", "netapi32", "msvcrt", "ucrtbase", "win32u",
        "msimg32", "usp10", "oleacc", "msvcp_win", "concrt140",
    ];
    SYS.contains(&n)
        || n.starts_with("api-ms-win-")
        || n.starts_with("ext-ms-")
        || n.starts_with("msvcr")   // msvcr71/90/100/120…  (MSVC C runtimes)
        || n.starts_with("msvcp")   // msvcp*  (MSVC C++ runtime — shimmed, not the GNU one)
        || n.starts_with("vcruntime")
}

/// Auto-resolve the runtime DLLs an exe needs (`--auto-lift`): walk its import table,
/// and for every non-system DLL find the file (beside the exe → `--dll-path` → cache)
/// and read it, recursing through each found DLL's own imports. Returns `(name, bytes)`
/// deduped by name. A DLL we can't find is skipped (its imports stay shim-bound — a
/// sound abort on use, never a crash) and noted. Bounded against import cycles.
fn auto_resolve_dlls(
    exe_path: &std::path::Path,
    exe_data: &[u8],
    search_dirs: &[PathBuf],
) -> Vec<(String, Vec<u8>)> {
    use std::collections::{BTreeMap, BTreeSet, VecDeque};
    let exe_dir = exe_path.parent().map(|p| p.to_path_buf()).unwrap_or_default();
    let cache = std::path::PathBuf::from("bench/.cache");
    let dirs: Vec<PathBuf> = std::iter::once(exe_dir)
        .chain(search_dirs.iter().cloned())
        .chain(std::iter::once(cache))
        .collect();
    let find = |name: &str| -> Option<Vec<u8>> {
        for d in &dirs {
            let p = d.join(name);
            if let Ok(b) = std::fs::read(&p) {
                return Some(b);
            }
            // case-insensitive fallback (import tables disagree on case with the file)
            if let Ok(rd) = std::fs::read_dir(d) {
                for e in rd.flatten() {
                    if e.file_name().to_string_lossy().eq_ignore_ascii_case(name) {
                        if let Ok(b) = std::fs::read(e.path()) {
                            return Some(b);
                        }
                    }
                }
            }
        }
        None
    };
    let imports_of = |data: &[u8]| -> Vec<String> {
        loader::Program::load(data)
            .map(|p| {
                p.pe_imports
                    .values()
                    .map(|i| i.dll.clone())
                    .collect::<BTreeSet<_>>()
                    .into_iter()
                    .collect()
            })
            .unwrap_or_default()
    };
    let mut resolved: BTreeMap<String, Vec<u8>> = BTreeMap::new();
    let mut seen: BTreeSet<String> = BTreeSet::new();
    let mut queue: VecDeque<String> = VecDeque::new();
    for dll in imports_of(exe_data) {
        queue.push_back(dll);
    }
    while let Some(dll) = queue.pop_front() {
        let key = dll.to_ascii_lowercase();
        if !seen.insert(key.clone()) || is_system_dll(&dll) {
            continue;
        }
        match find(&dll) {
            Some(bytes) => {
                for next in imports_of(&bytes) {
                    if !seen.contains(&next.to_ascii_lowercase()) {
                        queue.push_back(next);
                    }
                }
                resolved.insert(dll, bytes);
            }
            None => eprintln!("note: --auto-lift: {dll} not found on disk — left shim-bound"),
        }
    }
    resolved.into_iter().collect()
}

/// Parse a `{ "0xhexva": "Name" }` IAT map and merge it into `prog.imports`.
/// Does symbol `sym` name the user-requested function `want` (for `--entry` /
/// `--function`)? Accepts the exact name or the single-underscore cdecl
/// decoration a C symbol carries (`_main` for `main`) — but strips **at most one**
/// underscore per side, so `main` resolves to the C `_main` and never to the
/// mingw startup glue `__main`/`___main`, a wholly different function. (Matching
/// all underscores made `--entry main` run `___main`'s init guard, so nothing ran.)
fn symbol_matches(sym: &str, want: &str) -> bool {
    fn stem(s: &str) -> &str {
        s.strip_prefix('_').unwrap_or(s)
    }
    sym == want || stem(sym) == stem(want)
}

fn merge_iat_symbols(prog: &mut Program, path: &std::path::Path) -> Result<usize> {
    let text = std::fs::read_to_string(path)
        .with_context(|| format!("failed to read {}", path.display()))?;
    // Minimal JSON-object-of-strings parser (avoids a serde dependency).
    let mut n = 0;
    for (k, v) in parse_str_map(&text) {
        let hk = k.trim().trim_start_matches("0x");
        if let Ok(va) = u64::from_str_radix(hk, 16) {
            prog.imports.entry(va).or_insert(v);
            n += 1;
        }
    }
    if n == 0 {
        bail!("no usable entries in {}", path.display());
    }
    Ok(n)
}

/// Extract `"key": "value"` string pairs from a flat JSON object.
fn parse_str_map(text: &str) -> Vec<(String, String)> {
    let mut out = Vec::new();
    let b = text.as_bytes();
    let mut i = 0;
    let read_string = |b: &[u8], mut i: usize| -> Option<(String, usize)> {
        while i < b.len() && b[i] != b'"' {
            i += 1;
        }
        if i >= b.len() {
            return None;
        }
        i += 1;
        let start = i;
        while i < b.len() && b[i] != b'"' {
            i += 1;
        }
        if i >= b.len() {
            return None;
        }
        Some((String::from_utf8_lossy(&b[start..i]).into_owned(), i + 1))
    };
    while i < b.len() {
        let Some((key, ni)) = read_string(b, i) else { break };
        i = ni;
        while i < b.len() && b[i] != b':' && b[i] != b'}' {
            i += 1;
        }
        if i >= b.len() || b[i] == b'}' {
            break;
        }
        i += 1; // skip ':'
        let Some((val, ni)) = read_string(b, i) else { break };
        i = ni;
        out.push((key, val));
    }
    out
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
            if !r.imports_recovered.is_empty() {
                let _ = writeln!(out, "  imports:        {} recovered (IAT reconstructed)", r.imports_recovered.len());
                for e in r.imports_recovered.iter().take(8) {
                    let _ = writeln!(out, "                    {} :: {} @ 0x{:x}", e.dll, e.func, e.slot_va);
                }
            }
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

/// `--mode gensig`: for each CRT/glue function the symbols name, emit a FLIRT
/// signature (leading bytes with relative-branch operands wildcarded).
fn run_gensig(prog: &Program) -> String {
    // Sorted function-symbol addresses, to bound each function's bytes by the
    // next symbol.
    let mut addrs: Vec<u64> = prog.symbols.values().filter(|s| s.is_function).map(|s| s.address).collect();
    addrs.sort_unstable();
    addrs.dedup();

    let mut out = String::from(
        "# ARET FLIRT-lite signature database (mingw CRT).\n\
         # Generated by `aret --mode gensig <mingw-binary-with-symbols>`.\n\
         # Format: <name> <hex pattern, '..' = wildcard byte>\n",
    );
    for (i, &a) in addrs.iter().enumerate() {
        let sym = match prog.symbols.get(&a) {
            Some(s) => s,
            None => continue,
        };
        // Keep only the functions we bind: recognized CRT or startup glue.
        let is_crt = prog.crt_symbol(a).is_some();
        let is_glue = prog.is_startup_glue(a);
        if !is_crt && !is_glue {
            continue;
        }
        let Some(sec) = prog.section_at(a) else { continue };
        let start = (a - sec.address) as usize;
        let end = addrs
            .get(i + 1)
            .map(|&n| ((n - sec.address) as usize).min(sec.data.len()))
            .unwrap_or(sec.data.len());
        if start >= end || start >= sec.data.len() {
            continue;
        }
        let code = &sec.data[start..end.min(sec.data.len())];
        // Per-byte "is this byte part of a patched absolute address?" flags, so
        // the signature wildcards relocated operands (which vary per binary).
        let reloc: Vec<bool> = (0..code.len())
            .map(|k| prog.base_relocs.contains(&(a + k as u64)))
            .collect();
        if let Some(line) = flirt::gen_signature(&sym.name, code, &reloc) {
            out.push_str(&line);
            out.push('\n');
        }
    }
    out
}

fn main() -> Result<()> {
    let args = Args::parse();
    let data = std::fs::read(&args.binary)
        .with_context(|| format!("failed to read {}", args.binary.display()))?;

    // Collect the DLLs to lift alongside the exe: explicit `--with-dll` first, then
    // (if `--auto-lift`) every non-system runtime DLL the exe transitively imports and
    // that is found on disk. Explicit entries win on a name clash.
    let mut dlls: Vec<(String, Vec<u8>)> = Vec::new();
    for spec in &args.with_dll {
        let (name, path) = spec
            .split_once('=')
            .with_context(|| format!("--with-dll expects NAME=PATH, got `{spec}`"))?;
        let d = std::fs::read(path).with_context(|| format!("failed to read DLL {path}"))?;
        dlls.push((name.to_string(), d));
    }
    if args.auto_lift {
        for (name, d) in auto_resolve_dlls(&args.binary, &data, &args.dll_path) {
            if !dlls.iter().any(|(n, _)| n.eq_ignore_ascii_case(&name)) {
                dlls.push((name, d));
            }
        }
    }
    let mut prog = if dlls.is_empty() {
        Program::load(&data)?
    } else {
        let p = loader::load_with_modules(&data, &dlls)?;
        eprintln!(
            "note: lifted {} DLL module(s) alongside the binary{}",
            dlls.len(),
            if args.auto_lift { " (auto)" } else { "" }
        );
        p
    };

    if let Some(path) = &args.iat_symbols {
        let n = merge_iat_symbols(&mut prog, path)?;
        eprintln!("note: merged {n} IAT symbols from {}", path.display());
    }

    if args.mode == Mode::Info {
        let out = render_info(&prog);
        return emit(&args, out);
    }

    if args.mode == Mode::Imports {
        // Static, pre-recovery: classifies the whole import table (not just the
        // calls recovery reaches), so it works without analysing the code.
        return emit(&args, render_import_coverage(&prog));
    }

    if args.mode == Mode::Unpack {
        // Dynamic unpacking does not use static function recovery (the real code
        // is still encrypted) — emulate the stub instead.
        return emit(&args, run_unpack(&prog, &args.out_dir)?);
    }

    if args.mode == Mode::Gensig {
        return emit(&args, run_gensig(&prog));
    }

    let disasm = Disassembler::new(prog.bitness);
    let result = analysis::analyze(&prog, &disasm, !args.no_prologue_scan);

    let functions: Vec<_> = result
        .functions
        .iter()
        .filter(|f| match &args.function {
            None => true,
            Some(sel) => {
                symbol_matches(&f.name, sel)
                    || format!("0x{:x}", f.entry) == sel.to_lowercase()
                    || format!("{:x}", f.entry) == sel.to_lowercase()
            }
        })
        .collect();

    let mut out = String::new();
    match args.mode {
        Mode::Info | Mode::Imports => unreachable!(),
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
        Mode::Transpile | Mode::Walls => {
            let walls_only = args.mode == Mode::Walls;
            let wasm = args.target.as_deref() == Some("wasm");
            if let Some(t) = &args.target {
                if wasm {
                    eprintln!("note: --target wasm (compile the recovered C to WebAssembly via clang/wasi)");
                } else {
                    eprintln!("note: --target {} (M1 builds a native host binary at the source bitness)", t);
                }
            }
            // `--entry` accepts a hex address or a function symbol name (e.g.
            // `main`) — the latter lets us skip a fragile CRT startup and lift
            // from the user's entry, binding the recognized CRT natively.
            let entry_override = match &args.entry {
                Some(s) => {
                    let h = s.trim_start_matches("0x");
                    if let Ok(a) = u64::from_str_radix(h, 16) {
                        Some(a)
                    } else if let Some(a) = prog
                        .symbols
                        .values()
                        .find(|k| k.is_function && symbol_matches(&k.name, s))
                        .map(|k| k.address)
                    {
                        eprintln!("note: --entry {s} resolved to 0x{a:x}");
                        Some(a)
                    } else if matches!(s.as_str(), "main" | "auto") {
                        // Stripped binary: discover `main` from the CRT startup's
                        // call pattern (argc/argv setup then `call main`).
                        match analysis::find_main(&prog, &result) {
                            Some(a) => {
                                eprintln!("note: --entry {s} discovered main at 0x{a:x} (no symbol)");
                                Some(a)
                            }
                            None => bail!("--entry {s}: could not discover main (no symbol, no startup pattern)"),
                        }
                    } else {
                        bail!("--entry: '{s}' is neither hex nor a known function symbol");
                    }
                }
                // `--function` (no `--entry`): the pruning below drives the
                // selected function directly, so don't run CRT-startup detection
                // (its note would be misleading).
                None if args.function.is_some() => None,
                // No `--entry`: if the program entry is a CRT bootstrap
                // (`*CRTStartup`) and a distinct `main` symbol exists, start at
                // `main` and skip the startup we do not model. Guarded so a
                // freestanding binary (entry *is* its logic, no separate `main`)
                // is untouched — its entry keeps driving. `--entry <addr>` forces
                // the original entry back if the full startup is ever wanted.
                None => analysis::auto_main_entry(&prog).inspect(|&a| {
                    eprintln!("note: entry 0x{:x} is CRT startup; starting at main 0x{a:x} (use --entry to override)", prog.entry);
                }),
            };
            // Phase 2 — pruning by reachability. With `--function`, transpile
            // only the selected function and its transitive *direct-call* callees
            // (its closure), not the whole binary — targeted conversion of one
            // feature of a large binary. The entry defaults to the selected
            // function (so `main` drives it), unless `--entry` overrode it. Code
            // reached solely through indirect calls/vtables is not pulled in; such
            // a call fails loud at runtime rather than being faked (see
            // `reachable_closure`).
            let mut entry_override = entry_override;
            let pruned;
            let functions: &[&analysis::Function] = if let Some(sel) = &args.function {
                let root = functions
                    .first()
                    .map(|f| f.entry)
                    .ok_or_else(|| anyhow::anyhow!("--function {sel}: no matching recovered function"))?;
                let closure = analysis::reachable_closure(&result.functions, root);
                pruned = result
                    .functions
                    .iter()
                    .filter(|f| closure.contains(&f.entry))
                    .collect::<Vec<&analysis::Function>>();
                eprintln!(
                    "note: --function {sel}: pruned to {} function(s) (transitive closure of 0x{root:x})",
                    pruned.len()
                );
                if args.entry.is_none() {
                    entry_override = Some(root);
                }
                &pruned
            } else {
                &functions
            };
            let snapshot = match &args.snapshot {
                Some(p) => {
                    let r = builder::load_snapshot(p)?;
                    eprintln!("note: seeding memory from snapshot {} ({} regions)", p.display(), r.len());
                    Some(r)
                }
                None => None,
            };
            let report = builder::transpile(
                &prog,
                functions,
                &args.out_dir,
                args.run,
                entry_override,
                &args.backend,
                wasm,
                snapshot.as_deref(),
                &args.prog_args,
                walls_only,
            )?;
            if walls_only {
                out.push_str(&report.render_walls());
                return emit(&args, out);
            }
            out.push_str(&report.render());
            if args.strict && !report.is_sound() {
                emit(&args, out)?;
                bail!(
                    "--strict: result is not sound ({} unresolved direct call(s), {} partial(asm) function(s))",
                    report.unresolved.len(),
                    report.partial
                );
            }
        }
        Mode::Unpack => unreachable!("handled before function recovery"),
        Mode::Gensig => unreachable!("handled before function recovery"),
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

/// Render the axis-2 import-coverage report: how much of *this* binary's OS/CRT
/// surface ARET already provides natively, and — the actionable part — the exact
/// list of imports that are still gaps (each to be closed by a general shim).
fn render_import_coverage(prog: &Program) -> String {
    use std::fmt::Write;
    let cov = builder::import_coverage(prog);
    let total = cov.total();
    let mut s = String::new();
    let _ = writeln!(s, "Import coverage (axis 2 — OS/CRT surface)");
    let _ = writeln!(s, "  binary:    {}", prog.format);
    let pct = if total == 0 { 100 } else { cov.covered.len() * 100 / total };
    let _ = writeln!(s, "  imports:   {total}");
    let _ = writeln!(s, "  covered:   {} ({pct}%) — native ARET shim present", cov.covered.len());
    let _ = writeln!(s, "  uncovered: {} — no native aret_ shim", cov.uncovered.len());
    if cov.uncovered.is_empty() {
        let _ = writeln!(s, "  verdict:   FULLY COVERED — every import has a native shim");
    } else {
        // Honest scope: a function import with no shim hits the weak stub at
        // runtime (a real gap); a *data* import (e.g. `_iob`, `__initenv`) may be
        // satisfied by the IAT/layout path instead — so this is the shim gap, a
        // conservative upper bound on the true runtime gap, never an under-count.
        let _ = writeln!(s, "  --- axis-2 shim gap for this binary (close each with a general shim) ---");
        for n in &cov.uncovered {
            let _ = writeln!(s, "    {n}");
        }
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

#[cfg(test)]
mod tests {
    use super::{parse_str_map, symbol_matches};

    #[test]
    fn symbol_matches_respects_single_underscore_decoration() {
        // The C `main` decorates to `_main`; mingw's startup glue is `__main`/
        // `___main` — a different function. `--entry main` must pick the former.
        assert!(symbol_matches("main", "main"));
        assert!(symbol_matches("_main", "main"), "cdecl `_main` is the C main");
        assert!(!symbol_matches("__main", "main"), "`__main` glue is NOT main");
        assert!(!symbol_matches("___main", "main"), "`___main` glue is NOT main");
        // User may include the underscore; extra decoration still must not leak.
        assert!(symbol_matches("_feature_a", "feature_a"));
        assert!(symbol_matches("_feature_a", "_feature_a"));
        assert!(!symbol_matches("__feature_a", "feature_a"));
    }

    #[test]
    fn parses_flat_iat_symbol_map() {
        let json = r#"{ "0xdec020": "RegQueryValueExA", "0xdec07c": "GetTickCount" }"#;
        let m = parse_str_map(json);
        assert_eq!(m.len(), 2);
        assert_eq!(m[0], ("0xdec020".to_string(), "RegQueryValueExA".to_string()));
        assert_eq!(m[1].1, "GetTickCount");
    }

    #[test]
    fn tolerates_whitespace_and_newlines() {
        let json = "{\n  \"0x1000\" : \"foo\" ,\n  \"0x2000\":\"bar\"\n}";
        let m = parse_str_map(json);
        assert_eq!(m.len(), 2);
        assert_eq!(m[1], ("0x2000".to_string(), "bar".to_string()));
    }
}
