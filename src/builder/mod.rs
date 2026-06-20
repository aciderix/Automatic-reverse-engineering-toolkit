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
/// CRT forwarding layer (msvcrt → host libc); strong defs override the weak stubs.
const CRT_C: &str = include_str!("../../runtime/aret_hle/aret_crt.c");
/// Win32 layer (kernel32 subset → native POSIX); strong defs override weak stubs.
const WIN32_C: &str = include_str!("../../runtime/aret_hle/aret_win32.c");

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

/// Generated files for the Memory Layout Mapper.
struct Layout {
    /// `aret_layout.c` — the section table + `__aret_map_memory()`.
    c: String,
    /// `aret_layout.S` — `.incbin` of the concatenated section bytes.
    asm: String,
    /// `sections.bin` — the raw concatenated bytes the assembly embeds.
    blob: Vec<u8>,
    /// WASM variant of `aret_layout.c`: the blob as a C array + `memcpy` to VAs
    /// (no `mmap`; wasm linear memory *is* the address space).
    wasm_c: String,
    /// Highest section end VA — used to place the wasm program data above it.
    max_va: u64,
}

/// Memory Layout Mapper (UBT Phase 3, design doc §2): some binaries reference
/// data by its original absolute virtual address (a global string in `.rdata`,
/// a table in `.data`). The transpiled C keeps those as raw addresses, so the
/// recompiled process must place the original section bytes back at those VAs.
///
/// The section bytes are embedded as one raw binary blob (`.incbin` in
/// `aret_layout.S`) rather than C byte arrays — for a real binary the data is
/// megabytes, and a C array is ~18× larger and chokes the compiler. At startup
/// `__aret_map_memory()` `mmap(MAP_FIXED)`s the span covering the data sections
/// and copies each section's slice of the blob to its VA. (The binary is linked
/// `-no-pie` so its own segments sit elsewhere and the original low VAs are free.)
fn emit_layout(prog: &Program, blob_path: &Path) -> Option<Layout> {
    use std::fmt::Write as _;

    // Embed every section's bytes at its original VA, including executable ones:
    // code sections also hold absolute-addressed read-only data (string literals,
    // jump tables, constants), and packers (UPX) merge .rdata into a code section.
    // The transpiled functions run natively from the ELF's own segments, so the
    // mapped original bytes serve only as data — harmless to map.
    let secs: Vec<&crate::loader::Section> = prog
        .sections
        .iter()
        .filter(|s| s.address != 0 && !s.data.is_empty())
        .collect();
    if secs.is_empty() {
        return None;
    }

    // Concatenate section bytes into one blob; record (va, blob offset, len).
    let mut blob: Vec<u8> = Vec::new();
    let mut table: Vec<(u64, usize, usize)> = Vec::new();
    for sec in &secs {
        let off = blob.len();
        blob.extend_from_slice(&sec.data);
        table.push((sec.address, off, sec.data.len()));
    }

    // The assembly embeds the blob at an absolute path so the assembler finds it
    // regardless of the working directory.
    let asm = format!(
        ".section .rodata\n.global aret_blob\naret_blob:\n.incbin \"{}\"\n",
        blob_path.display()
    );

    let mut c = String::new();
    c.push_str("/* Generated by ARET — Memory Layout Mapper (blob-backed). */\n");
    c.push_str("#include <stdint.h>\n#include <string.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <sys/mman.h>\n\n");
    c.push_str("extern const unsigned char aret_blob[];\n");
    c.push_str("struct aret_sec { uint32_t va; uint32_t off; uint32_t len; };\n");
    c.push_str("static const struct aret_sec aret_secs[] = {\n");
    for (va, off, len) in &table {
        let _ = writeln!(c, "    {{ 0x{:x}u, {}u, {}u }},", va, off, len);
    }
    c.push_str("};\n\n");
    c.push_str(
        "void __aret_map_memory(void) {\n\
\x20   const uint32_t PAGE = 0x1000u;\n\
\x20   uint64_t lo = 0xffffffffull, hi = 0;\n\
\x20   unsigned i;\n\
\x20   for (i = 0; i < sizeof(aret_secs)/sizeof(aret_secs[0]); i++) {\n\
\x20       uint64_t a = aret_secs[i].va & ~(uint64_t)(PAGE - 1);\n\
\x20       uint64_t e = (aret_secs[i].va + aret_secs[i].len + PAGE - 1) & ~(uint64_t)(PAGE - 1);\n\
\x20       if (a < lo) lo = a;\n\
\x20       if (e > hi) hi = e;\n\
\x20   }\n\
\x20   void *p = mmap((void *)(uintptr_t)lo, (size_t)(hi - lo),\n\
\x20                  PROT_READ | PROT_WRITE,\n\
\x20                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);\n\
\x20   if (p == MAP_FAILED) { perror(\"aret: mmap layout\"); exit(70); }\n\
\x20   for (i = 0; i < sizeof(aret_secs)/sizeof(aret_secs[0]); i++)\n\
\x20       memcpy((void *)(uintptr_t)aret_secs[i].va, aret_blob + aret_secs[i].off, aret_secs[i].len);\n\
}\n",
    );
    // WASM layout: embed the blob as a C array and memcpy each section to its VA
    // (wasm32 linear memory is flat from 0, so a VA is just an offset). No mmap.
    let mut wasm_c = String::new();
    wasm_c.push_str("/* Generated by ARET — WASM layout (linear memory, no mmap). */\n");
    wasm_c.push_str("#include <stdint.h>\n#include <string.h>\n\n");
    wasm_c.push_str("static const unsigned char aret_blob[] = {");
    for (i, b) in blob.iter().enumerate() {
        if i % 32 == 0 { wasm_c.push('\n'); }
        let _ = write!(wasm_c, "{},", b);
    }
    wasm_c.push_str("\n};\n");
    wasm_c.push_str("struct aret_sec { uint32_t va; uint32_t off; uint32_t len; };\n");
    wasm_c.push_str("static const struct aret_sec aret_secs[] = {\n");
    for (va, off, len) in &table {
        let _ = writeln!(wasm_c, "    {{ 0x{:x}u, {}u, {}u }},", va, off, len);
    }
    wasm_c.push_str("};\n\n");
    wasm_c.push_str(
        "void __aret_map_memory(void) {\n\
\x20   unsigned i;\n\
\x20   for (i = 0; i < sizeof(aret_secs)/sizeof(aret_secs[0]); i++)\n\
\x20       memcpy((void *)(uintptr_t)aret_secs[i].va, aret_blob + aret_secs[i].off, aret_secs[i].len);\n\
}\n",
    );
    let max_va = table.iter().map(|(va, _, len)| va + *len as u64).max().unwrap_or(0);

    Some(Layout { c, asm, blob, wasm_c, max_va })
}

/// Emit a weak stub for every imported function (`aret_<name>`), so a binary that
/// references imports without a real HLE shim still links. A stub warns once (via
/// `aret_unimpl`) and returns 0; the HLE's strong definitions override the weak
/// stubs for the imports that *are* implemented. The runtime warnings are the
/// shopping list for extending the HLE (or bridging to Wine).
fn emit_import_stubs(prog: &Program) -> String {
    use std::collections::BTreeMap;
    use std::fmt::Write as _;

    // sanitized identifier -> raw import name (dedup; keep one raw for the label).
    let mut stubs: BTreeMap<String, String> = BTreeMap::new();
    for raw in prog.imports.values() {
        stubs
            .entry(ir::build::sanitize_import(raw))
            .or_insert_with(|| raw.clone());
    }

    let mut s = String::new();
    s.push_str("/* Generated by ARET — weak stubs for unimplemented imports. */\n");
    s.push_str("#include <stdint.h>\n#include \"aret_hle.h\"\n\n");
    for (sym, raw) in &stubs {
        let raw_escaped = raw.replace('\\', "\\\\").replace('"', "\\\"");
        let _ = writeln!(
            s,
            "__attribute__((weak)) uint32_t {sym}(uint32_t esp) {{ (void)esp; aret_unimpl(\"{raw_escaped}\"); return 0; }}"
        );
    }
    s
}

/// Emit the indirect-call dispatch table: a sorted VA -> `sub_<va>` map plus
/// `aret_call`, which binary-searches it. Function pointers in transpiled code
/// carry the original code address; `aret_call` turns that back into a call to the
/// transpiled function (or warns and returns 0 if the address was not recovered).
fn emit_dispatch(entries: &[u64]) -> String {
    use std::fmt::Write as _;
    let mut s = String::new();
    s.push_str("/* Generated by ARET — indirect-call dispatch (VA -> sub_<va>). */\n");
    s.push_str("#include <stdint.h>\n#include \"aret_hle.h\"\n\n");
    s.push_str("typedef uint64_t (*aret_fn)(uint64_t,uint64_t,uint64_t,uint64_t);\n");
    for &va in entries {
        let _ = writeln!(s, "uint64_t sub_{va:x}(uint64_t,uint64_t,uint64_t,uint64_t);");
    }
    s.push_str("struct aret_e { uint32_t va; aret_fn fn; };\n");
    s.push_str("static const struct aret_e aret_tab[] = {\n");
    for &va in entries {
        let _ = writeln!(s, "    {{ 0x{va:x}u, sub_{va:x} }},");
    }
    s.push_str("};\n\n");
    s.push_str(
        "uint64_t aret_call(uint32_t va, uint64_t esp, uint64_t a, uint64_t c, uint64_t d) {\n\
\x20   long lo = 0, hi = (long)(sizeof(aret_tab)/sizeof(aret_tab[0])) - 1;\n\
\x20   while (lo <= hi) {\n\
\x20       long m = (lo + hi) / 2;\n\
\x20       if (aret_tab[m].va == va) return aret_tab[m].fn(esp, a, c, d);\n\
\x20       if (aret_tab[m].va < va) lo = m + 1; else hi = m - 1;\n\
\x20   }\n\
\x20   aret_unimpl(\"indirect call to unrecovered address\");\n\
\x20   return 0;\n\
}\n",
    );
    s
}

/// Emit `__aret_patch_iat`: for every import whose name maps to a synthetic data
/// object (`aret_data_import`), overwrite its IAT slot with that object's address.
/// Function imports (no data object) are left untouched — their calls are already
/// redirected to shims. Runs after the layout mapper (which maps the IAT) and
/// before the entry point.
fn emit_iat_patch(prog: &Program) -> String {
    use std::fmt::Write as _;
    let mut s = String::new();
    s.push_str("/* Generated by ARET — patch IAT slots of data imports. */\n");
    s.push_str("#include <stdint.h>\n#include \"aret_hle.h\"\n\n");
    s.push_str("void __aret_patch_iat(void) {\n");
    for (va, name) in &prog.imports {
        let esc = name.replace('\\', "\\\\").replace('"', "\\\"");
        let _ = writeln!(
            s,
            "    {{ uint32_t p = aret_data_import(\"{esc}\"); if (p) *(uint32_t *)(uintptr_t)0x{va:x}u = p; }}"
        );
    }
    s.push_str("}\n");
    s
}

/// Referenced-but-undefined `Direct` call targets across `irfs` (for the LLVM
/// backend, which does not go through `emit_split`'s forward set). These get weak
/// stubs so the program links despite incomplete recovery.
fn collect_undef_subs(irfs: &[ir::types::IrFunction]) -> Vec<u64> {
    use ir::types::{CallTarget, Expr, Stmt};
    use std::collections::BTreeSet;
    let defined: BTreeSet<u64> = irfs.iter().map(|f| f.entry).collect();
    let mut called: BTreeSet<u64> = BTreeSet::new();
    fn ex(e: &Expr, out: &mut BTreeSet<u64>) {
        match e {
            Expr::Call { target, args, .. } => {
                if let CallTarget::Direct(a) = target {
                    out.insert(*a);
                }
                if let CallTarget::Indirect(x) = target {
                    ex(x, out);
                }
                for a in args {
                    ex(a, out);
                }
            }
            Expr::Load { addr, .. } => ex(addr, out),
            Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => ex(x, out),
            Expr::Binary(_, a, b) => { ex(a, out); ex(b, out); }
            Expr::Select { cond, then_, else_ } => { ex(cond, out); ex(then_, out); ex(else_, out); }
            _ => {}
        }
    }
    fn st(s: &Stmt, out: &mut BTreeSet<u64>) {
        match s {
            Stmt::Set { expr, .. } | Stmt::Assign { expr, .. } | Stmt::CallStmt(expr) => ex(expr, out),
            Stmt::Store { addr, value, .. } => { ex(addr, out); ex(value, out); }
            Stmt::Branch { cond, .. } => ex(cond, out),
            Stmt::Switch { value, .. } => ex(value, out),
            Stmt::Return(Some(e)) => ex(e, out),
            _ => {}
        }
    }
    for f in irfs {
        for b in &f.blocks {
            for s in &b.stmts {
                st(s, &mut called);
            }
        }
    }
    called.difference(&defined).copied().collect()
}

/// Build IR for one recovered function and lower it for shared-stack transpilation
/// (UBT M3). Unlike the verify/decompile chain, this does *not* promote stack
/// slots to private per-function locals: every stack access stays raw memory into
/// one global stack, and `__esp` is threaded through calls so stack-passed
/// arguments cross functions.
fn lower(prog: &Program, f: &Function) -> ir::types::IrFunction {
    // Force raw frame memory so `[ebp+d]` stays a load from the shared stack
    // (build_ir ORs this in, then resets it afterwards).
    ir::lift::set_frames_off(true);
    let mut irf = ir::build::build_ir(prog, f);
    ir::build::thread_internal_call_esp(&mut irf);
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
    entry_override: Option<u64>,
    backend: &str,
    wasm: bool,
) -> Result<TranspileReport> {
    // WebAssembly target: the recovered C is portable, and wasm32's linear memory
    // *is* the 32-bit address space, so it is a natural target (32-bit pointers,
    // no mmap — sections are memcpy'd to their VAs). The LLVM IR backend is
    // i386-specific, so wasm always uses the C backend.
    let use_llvm = !wasm && backend.eq_ignore_ascii_case("llvm");
    if funcs.is_empty() {
        bail!("no functions to transpile");
    }
    // The program's entry point is the C function `sub_<entry>`; confirm it was
    // recovered before doing any work. `--entry` can override it (e.g. start at
    // `main`, skipping a heavy CRT startup).
    let entry = entry_override.unwrap_or(prog.entry);
    if !funcs.iter().any(|f| f.entry == entry) {
        bail!(
            "entry function 0x{:x} was not recovered (try without --function)",
            entry
        );
    }
    std::fs::create_dir_all(out_dir)
        .with_context(|| format!("failed to create {}", out_dir.display()))?;

    // Lower every function and emit modular C using the shared machine stack (so
    // both stack- and register-passed arguments cross function calls). The mode
    // flag is read during SSA *and* emission, so it wraps both. The program is
    // split into chunks of functions so the compiler never sees one giant TU.
    const CHUNK_FUNCS: usize = 200;
    emit::set_shared_stack(true);
    let irfs: Vec<_> = funcs.iter().map(|f| lower(prog, f)).collect();
    let n_funcs = irfs.len();
    // Program emission: either the LLVM IR backend (chunked .ll modules) or the
    // chunked C backend. The runtime (HLE, main, dispatch, layout, stubs) is C
    // either way.
    let llvm_chunks: Vec<String> = if use_llvm {
        emit::llvm::emit_split(&irfs, CHUNK_FUNCS)
    } else {
        Vec::new()
    };
    let (decls_h, chunks, undef_subs) = if use_llvm {
        (String::new(), Vec::new(), collect_undef_subs(&irfs))
    } else {
        emit::structured::emit_split(&irfs, CHUNK_FUNCS)
    };
    emit::set_shared_stack(false);
    // The IR is no longer needed; free it before spawning the parallel compilers
    // (which need the memory) on a large program.
    drop(irfs);

    // Memory Layout Mapper: restore data sections at their original VAs so
    // absolute pointers (global strings/tables) resolve at runtime.
    let blob_path = out_dir.join("sections.bin");
    let layout = emit_layout(prog, &blob_path);
    let map_call = if layout.is_some() {
        // Map sections, then patch data-import IAT slots (which live in a mapped
        // section) to synthetic objects.
        "    __aret_map_memory();\n    __aret_patch_iat();\n"
    } else {
        ""
    };
    // aret_main.c — a native entry that maps memory, sets up the single shared
    // machine stack, then drives the transpiled entry point with the stack-top
    // pointer. Every transpiled function threads this `__esp` through its calls.
    let main_c = format!(
        "#include <stdint.h>\n\n\
         /* One shared machine stack for all transpiled functions (UBT M3). */\n\
         static uint8_t aret_stack[1u << 20];\n\n\
         void __aret_map_memory(void);\n\
         void __aret_patch_iat(void);\n\
         uint64_t sub_{entry:x}(uint64_t __esp, uint64_t a, uint64_t c, uint64_t d);\n\n\
         int main(void) {{\n\
         {map_call}    uint64_t esp = (uint64_t)(uintptr_t)(aret_stack + sizeof(aret_stack) - 16);\n\
         \x20   return (int)sub_{entry:x}(esp, 0, 0, 0);\n\
         }}\n",
    );

    let write = |name: &str, body: &str| -> Result<()> {
        std::fs::write(out_dir.join(name), body)
            .with_context(|| format!("failed to write {}", name))
    };
    // Weak stubs: one per unimplemented import, plus one per referenced-but-
    // unrecovered function address — so the program links even though the static
    // recovery is incomplete, and reports the gaps at runtime.
    let mut stubs = emit_import_stubs(prog);
    for &addr in &undef_subs {
        stubs.push_str(&format!(
            "__attribute__((weak)) uint64_t sub_{addr:x}(uint64_t e,uint64_t a,uint64_t c,uint64_t d){{ (void)e;(void)a;(void)c;(void)d; aret_unimpl(\"sub_{addr:x} (unrecovered)\"); return 0; }}\n"
        ));
    }
    // Indirect-call dispatch table over every recovered function entry (sorted).
    let mut entries: Vec<u64> = funcs.iter().map(|f| f.entry).collect();
    entries.sort_unstable();
    entries.dedup();
    write("aret_hle.h", HLE_H)?;
    write("aret_hle.c", HLE_C)?;
    write("aret_crt.c", CRT_C)?;
    write("aret_win32.c", WIN32_C)?;
    write("aret_stubs.c", &stubs)?;
    write("aret_dispatch.c", &emit_dispatch(&entries))?;
    write("aret_iat.c", &emit_iat_patch(prog))?;
    write("aret_main.c", &main_c)?;
    // Program: one LLVM IR module, or chunked C translation units.
    let mut chunk_srcs: Vec<std::path::PathBuf> = Vec::new();
    if use_llvm {
        for (i, m) in llvm_chunks.iter().enumerate() {
            let name = format!("program_{i}.ll");
            write(&name, m)?;
            chunk_srcs.push(out_dir.join(name));
        }
        // The C backend inlines the float helpers per chunk; the LLVM backend
        // calls them as external symbols, so provide them as a linkable unit
        // (the same helpers, with external linkage).
        let float_c = format!("#include <stdint.h>\n{}", emit::FLOAT_HELPERS.replace("static inline ", ""));
        write("aret_float.c", &float_c)?;
    } else {
        // Prototype every intercepted import shim with its real signature
        // (`uint32_t aret_<name>(uint32_t)`). Without this the chunks call them on
        // an implicit `()` declaration — fine on x86, but WASM's typed
        // `call_indirect` traps on the arity mismatch.
        let mut decls_h = decls_h;
        {
            use std::collections::BTreeSet;
            use std::fmt::Write as _;
            let mut seen = BTreeSet::new();
            let mut protos = String::new();
            for raw in prog.imports.values() {
                let sym = ir::build::sanitize_import(raw);
                if seen.insert(sym.clone()) {
                    let _ = writeln!(protos, "uint32_t {sym}(uint32_t);");
                }
            }
            decls_h.push_str(&protos);
        }
        write("aret_decls.h", &decls_h)?;
        for (i, body) in chunks.iter().enumerate() {
            let name = format!("chunk_{i}.c");
            write(&name, &format!("#include \"aret_decls.h\"\n{}", body))?;
            chunk_srcs.push(out_dir.join(name));
        }
    }
    // Memory layout: a blob-backed mapper, or a no-op when there is no addressable
    // data (so aret_main.c always links).
    let has_layout = layout.is_some();
    // The wasm path embeds the blob in C (no .S/.incbin) and computes where the
    // program's own data must sit (above the image) to avoid colliding with the
    // VA-mapped sections in the shared linear memory.
    let wasm_global_base = layout.as_ref().map(|l| (l.max_va + 0xffff) & !0xffff).unwrap_or(0x100000).max(0x100000);
    match &layout {
        Some(l) if wasm => write("aret_layout.c", &l.wasm_c)?,
        Some(l) => {
            std::fs::write(&blob_path, &l.blob)
                .with_context(|| "failed to write sections.bin".to_string())?;
            write("aret_layout.c", &l.c)?;
            write("aret_layout.S", &l.asm)?;
        }
        None => write("aret_layout.c", "void __aret_map_memory(void) {}\n")?,
    }

    // Compile every source to a .o in parallel, then link. The stack-model C masks
    // pointers to 32 bits, so a 32-bit source must be built as a 32-bit process
    // (-m32). Objects are non-PIC (-fno-pie) and linked -no-pie so our segments
    // sit elsewhere and the original low VAs stay free for the layout mapper.
    let bits = prog.bitness.bits() as u32;
    let march = if bits == 32 { "-m32" } else { "-m64" };
    let cc = std::env::var("CC").unwrap_or_else(|_| "cc".to_string());
    let binary = if wasm { out_dir.join("app.wasm") } else { out_dir.join("app") };

    let mut sources: Vec<std::path::PathBuf> = vec![
        out_dir.join("aret_hle.c"),
        out_dir.join("aret_crt.c"),
        out_dir.join("aret_win32.c"),
        out_dir.join("aret_stubs.c"),
        out_dir.join("aret_dispatch.c"),
        out_dir.join("aret_iat.c"),
        out_dir.join("aret_main.c"),
        out_dir.join("aret_layout.c"),
    ];
    if has_layout && !wasm {
        sources.push(out_dir.join("aret_layout.S"));
    }

    // WebAssembly: compile the recovered C with clang for wasm32-wasi and link to
    // a single .wasm. The program's static data is placed above the VA-mapped
    // image (`--global-base`) so they share linear memory without colliding.
    if wasm {
        // WASI lacks a few POSIX symbols the runtime references (single-process
        // model); provide trivial fallbacks so the module links.
        write(
            "aret_wasm_compat.c",
            "/* WASI compatibility shims for symbols absent from wasi-libc. */\n\
             int getpid(void) { return 1; }\n",
        )?;
        sources.push(out_dir.join("aret_wasm_compat.c"));
        sources.extend(chunk_srcs);
        let clang = std::env::var("WASM_CC").unwrap_or_else(|_| "clang".to_string());
        let sysroot = std::env::var("WASI_SYSROOT").unwrap_or_else(|_| "/usr".to_string());
        let out = Command::new(&clang)
            .args(["--target=wasm32-wasi", "-O0", "-w", "-Wno-implicit-function-declaration",
                   "-fno-strict-aliasing", "-fno-builtin"])
            .arg(format!("--sysroot={sysroot}"))
            .arg(format!("-Wl,--global-base={wasm_global_base}"))
            .arg("-Wl,-z,stack-size=2097152")
            .arg("-I").arg(out_dir)
            .args(&sources)
            .arg("-o").arg(&binary)
            .output()
            .with_context(|| format!("failed to run {clang} (set WASM_CC?)"))?;
        if !out.status.success() {
            bail!("wasm build failed:\n{}", String::from_utf8_lossy(&out.stderr).trim());
        }
        let run_output = if run {
            let rt = std::env::var("WASM_RUNTIME").unwrap_or_else(|_| "wasmtime".to_string());
            let o = Command::new(&rt).arg(&binary).output()
                .with_context(|| format!("failed to run {rt} (set WASM_RUNTIME?)"))?;
            let mut s = String::from_utf8_lossy(&o.stdout).into_owned();
            if !o.stderr.is_empty() { s.push_str(&String::from_utf8_lossy(&o.stderr)); }
            Some(s)
        } else { None };
        return Ok(TranspileReport {
            out_dir: out_dir.to_path_buf(),
            binary,
            functions: n_funcs,
            bits,
            run_output,
        });
    }
    if use_llvm {
        sources.push(out_dir.join("aret_float.c")); // external float helpers
    }
    sources.extend(chunk_srcs);

    let triple = if bits == 32 { "i386-pc-linux-gnu" } else { "x86_64-pc-linux-gnu" };
    let llc = std::env::var("LLC").unwrap_or_else(|_| "llc".to_string());
    use rayon::prelude::*;
    let objs: Result<Vec<std::path::PathBuf>> = sources
        .par_iter()
        .map(|src| {
            // Unique object name per source (so .c and .S of the same stem don't clash).
            let obj = out_dir.join(format!("{}.o", src.file_name().unwrap().to_string_lossy()));
            // LLVM IR chunks go through llc; C / asm through the C compiler.
            let out = if src.extension().and_then(|e| e.to_str()) == Some("ll") {
                Command::new(&llc)
                    .args([&format!("-mtriple={triple}"), "-filetype=obj", "-O2", "-relocation-model=static"])
                    .arg(src)
                    .arg("-o")
                    .arg(&obj)
                    .output()
                    .with_context(|| format!("failed to run {}", llc))?
            } else {
                Command::new(&cc)
                    .args([march, "-w", "-fno-strict-aliasing", "-fno-builtin", "-fno-pie", "-O0", "-c"])
                    .arg(src)
                    .arg("-I")
                    .arg(out_dir)
                    .arg("-o")
                    .arg(&obj)
                    .output()
                    .with_context(|| format!("failed to run {}", cc))?
            };
            if !out.status.success() {
                bail!(
                    "compile {} failed:\n{}",
                    src.display(),
                    String::from_utf8_lossy(&out.stderr).trim()
                );
            }
            Ok(obj)
        })
        .collect();
    let objs = objs?;

    let link = Command::new(&cc)
        .args([march, "-no-pie"])
        .args(&objs)
        .arg("-lm") // the float helpers use sqrtf/sqrtl
        .arg("-o")
        .arg(&binary)
        .output()
        .with_context(|| format!("failed to run {}", cc))?;
    if !link.status.success() {
        let err = String::from_utf8_lossy(&link.stderr);
        bail!("native link failed:\n{}", err.trim());
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
        functions: n_funcs,
        bits,
        run_output,
    })
}
