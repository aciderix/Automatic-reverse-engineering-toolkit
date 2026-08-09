//! Backend / packaging (UBT Phase 4, milestone M1): turn the transpiled C plus
//! the HLE compatibility layer into a *native, runnable* executable for the
//! target OS.
//!
//! M1 scope: a freestanding Win32 PE (kernel32 imports only) is transpiled to C
//! by the ARET middle-end, its API imports are intercepted into HLE shim calls
//! (see `ir::build::name_calls_in_expr`), and this builder links the result with
//! `runtime/aret_hle` and a generated `main` into a native ELF via the system C
//! compiler. The produced binary runs natively — not under an emulator or Wine.

pub mod objcache;

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
/// Code-page table extracted from Wine's mlang.c (tools/gen_mlang_cp.py), #included by
/// aret_win32.c for IMultiLanguage::GetCodePageInfo. Compiled in; no Wine at runtime.
const MLANG_CP_TABLE_H: &str = include_str!("../../runtime/aret_hle/mlang_cp_table.h");
/// CP1252 reverse (UTF16->ANSI) table, MEASURED from Wine (tools/gen_cp1252.py), #included by
/// aret_win32.c for WideCharToMultiByte(CP_ACP) and the ntdll floor. Compiled in; no Wine at runtime.
const CP1252_REV_TABLE_H: &str = include_str!("../../runtime/aret_hle/cp1252_rev_table.h");
/// CP437 (OEM) tables, MEASURED from Wine (tools/gen_cp437.py), #included by aret_win32.c.
const CP437_TABLES_H: &str = include_str!("../../runtime/aret_hle/cp437_tables.h");
/// Heavy-form (doc 82): ntdll Rtl* adapters that route imports to the REAL Wine bodies
/// compiled from `runtime/wine_heavy/rtlstr.c`. Discovered as normal shims; compiled with
/// standard flags (it treats wide strings as opaque guest pointers).
const NTDLL_C: &str = include_str!("../../runtime/aret_ntdll.c");
/// Heavy-form: a whole Wine ntdll source file compiled UNCHANGED, plus its ASCII floor and
/// the self-contained NT-types layer it needs (native cc has no winnt.h). These build as
/// separate objects with per-file flags (`-fshort-wchar`, `-I <shim>`) and link into every
/// binary; the Rtl* bodies are only reached when the program imports them.
const WINE_RTLSTR_C: &str = include_str!("../../runtime/wine_heavy/rtlstr.c");
const WINE_FLOOR_C: &str = include_str!("../../runtime/wine_heavy/ntdll_floor.c");
/// Real-ABI Nt* registry floor (doc 82 tranche 5/6): NTAPI wrappers routing a COMPILED Wine ntdll
/// .c's registry syscalls to the shared aret_ntreg_* cores (aret_win32.c). Compiled + linked with
/// the heavy floor below; its bare Nt* symbols serve compiled Wine code (a PE's own imports still
/// route to the aret_* esp shims, a different symbol set).
const WINE_NTREG_C: &str = include_str!("../../runtime/wine_heavy/ntdll_ntreg.c");
/// A whole Wine ntdll file (doc 82 tranche 6 capstone): dlls/ntdll/reg.c, UNCHANGED except the
/// forward-decl splice, compiled on the real-ABI Nt* registry floor. Its exported Rtlp*Nt / Rtl*
/// registry functions are reached from a PE via the aret_Rtlp* adapters (aret_ntdll.c).
const WINE_REG_C: &str = include_str!("../../runtime/wine_heavy/reg.c");
const WINE_NT_TYPES_H: &str = include_str!("../../runtime/wine_heavy/native/nt_types.h");
const WINE_REG_TYPES_H: &str = include_str!("../../runtime/wine_heavy/native/reg_types.h");
const WINE_FLOOR_H: &str = include_str!("../../runtime/wine_heavy/native/ntdll_floor.h");
const WINE_DEBUG_H: &str = include_str!("../../runtime/wine_heavy/native/wine/debug.h");

pub struct TranspileReport {
    pub out_dir: std::path::PathBuf,
    pub binary: std::path::PathBuf,
    pub functions: usize,
    pub bits: u32,
    /// Function classification (the IR-level frontier): fully translated, only
    /// *partially* simulated (some opaque `asm`), and host-backed (a call to it is
    /// redirected to a native shim — the real runtime stands in for it).
    pub lifted: usize,
    pub partial: usize,
    pub host_backed: usize,
    /// Soundness: addresses of unrecovered functions that are nonetheless the
    /// target of a *direct* `call` in recovered code. Each is a statically-known
    /// gap — a call site that cannot do the right thing. Empty ⇒ every direct
    /// call resolves to translated code or a native shim.
    pub unresolved: Vec<u64>,
    /// Imports the program *calls* for which no shim is implemented: each link to
    /// the weak stub that warns and returns 0 — a silent wrong result (e.g.
    /// `qsort` leaving its array unsorted). Names are sanitized (`aret_qsort`).
    pub unimplemented_imports: Vec<String>,
    /// Every unmodelled instruction across all recovered functions (the lift
    /// gaps), deduplicated by text with a site count, most-frequent first. The
    /// complete *static* per-instruction wall list — what the runtime would hit
    /// one at a time, seen all at once. Empty ⇒ every recovered instruction lifts.
    pub unmodelled_insns: Vec<(String, usize)>,
    /// Captured stdout if the binary was run, else `None`.
    pub run_output: Option<String>,
}

impl TranspileReport {
    /// A binary is *sound* when nothing in it is known, at translation time, to
    /// misbehave: no direct call to an unrecovered function, and no function left
    /// partially simulated (opaque `asm` emits as a no-op). This is honest
    /// completeness — it does not certify indirect-call coverage (those targets
    /// are not knowable statically; the runtime fails loud on an unrecovered one).
    pub fn is_sound(&self) -> bool {
        self.unresolved.is_empty() && self.partial == 0 && self.unimplemented_imports.is_empty()
    }
}

impl TranspileReport {
    pub fn render(&self) -> String {
        let mut s = String::new();
        s.push_str("ARET transpile (UBT M1) — native recompile\n");
        s.push_str(&format!("  functions:  {}\n", self.functions));
        s.push_str(&format!(
            "  classes:    {} lifted, {} partial(asm), {} host-backed\n",
            self.lifted, self.partial, self.host_backed
        ));
        s.push_str(&format!("  bitness:    {}-bit\n", self.bits));
        // Soundness verdict — the honest answer to "will this binary behave?".
        if self.is_sound() {
            s.push_str("  soundness:  SOUND — every direct call resolves, no opaque asm\n");
        } else {
            s.push_str(&format!(
                "  soundness:  INCOMPLETE — {} unresolved direct call(s), {} partial(asm) function(s), {} unimplemented import(s)\n",
                self.unresolved.len(),
                self.partial,
                self.unimplemented_imports.len(),
            ));
            for &a in self.unresolved.iter().take(10) {
                s.push_str(&format!("              ! direct call to unrecovered 0x{a:x}\n"));
            }
            if self.unresolved.len() > 10 {
                s.push_str(&format!("              … and {} more\n", self.unresolved.len() - 10));
            }
            for n in self.unimplemented_imports.iter().take(10) {
                let raw = n.strip_prefix("aret_").unwrap_or(n);
                s.push_str(&format!("              ! calls unimplemented import: {raw}\n"));
            }
            if self.unimplemented_imports.len() > 10 {
                s.push_str(&format!("              … and {} more\n", self.unimplemented_imports.len() - 10));
            }
            // Lift gaps (unmodelled instructions) — the same wall list `--mode
            // walls` prints in full. Surfaced here (top few) so a plain transpile
            // already shows *which* instructions block, not just a partial count.
            for (t, c) in self.unmodelled_insns.iter().take(6) {
                s.push_str(&format!("              ! unmodelled instruction (×{c}): {t}\n"));
            }
            if self.unmodelled_insns.len() > 6 {
                s.push_str(&format!("              … and {} more distinct (see --mode walls)\n", self.unmodelled_insns.len() - 6));
            }
        }
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

    /// The complete static "wall map" for a binary: every coverage gap the runtime
    /// could hit, enumerated in one pass instead of one-at-a-time at execution.
    /// Three sections — unmodelled instructions (lift gaps, by site count),
    /// unimplemented imports (HLE gaps), and unresolved direct calls (recovery
    /// gaps). This turns "walk into wall after wall" into a prioritisable list, and
    /// its stable sections aggregate across a corpus (grep the counts). Behaviour
    /// bugs (miscompiles) are *not* here — those are undecidable statically and
    /// surface only via the differential oracles.
    pub fn render_walls(&self) -> String {
        let mut s = String::new();
        let sites: usize = self.unmodelled_insns.iter().map(|(_, n)| n).sum();
        s.push_str("ARET wall map — complete static coverage gaps\n");
        s.push_str(&format!(
            "  recovered:  {} functions ({} lifted, {} partial-asm, {} host-backed), {}-bit\n\n",
            self.functions, self.lifted, self.partial, self.host_backed, self.bits
        ));
        s.push_str(&format!(
            "UNMODELLED INSTRUCTIONS (lift gaps) — {} distinct / {} sites\n",
            self.unmodelled_insns.len(), sites
        ));
        if self.unmodelled_insns.is_empty() {
            s.push_str("  (none — every recovered instruction lifts)\n");
        }
        for (t, n) in &self.unmodelled_insns {
            s.push_str(&format!("  {n:>6}  {t}\n"));
        }
        s.push_str(&format!(
            "\nUNIMPLEMENTED IMPORTS (HLE gaps) — {}\n",
            self.unimplemented_imports.len()
        ));
        if self.unimplemented_imports.is_empty() {
            s.push_str("  (none)\n");
        }
        let mut imps: Vec<&str> = self
            .unimplemented_imports
            .iter()
            .map(|n| n.strip_prefix("aret_").unwrap_or(n))
            .collect();
        imps.sort_unstable();
        for n in imps {
            s.push_str(&format!("  {n}\n"));
        }
        s.push_str(&format!(
            "\nUNRESOLVED DIRECT CALLS (recovery gaps) — {}\n",
            self.unresolved.len()
        ));
        if self.unresolved.is_empty() {
            s.push_str("  (none)\n");
        }
        for a in &self.unresolved {
            s.push_str(&format!("  0x{a:x}\n"));
        }
        s
    }
}

/// Load a memory snapshot: the magic `ARETSNP1` then repeated records of
/// `va: u64-LE, len: u64-LE, bytes[len]`. Produced by `tools/snapshot` from a
/// frozen process's `/proc/<pid>/maps`+`mem` — the post-init game state that A+B
/// seeds into the lifted code's initial memory.
pub fn load_snapshot(path: &Path) -> Result<Vec<(u64, Vec<u8>)>> {
    let data = std::fs::read(path).with_context(|| format!("failed to read {}", path.display()))?;
    if data.len() < 8 || &data[..8] != b"ARETSNP1" {
        bail!("{}: not an ARET snapshot (bad magic)", path.display());
    }
    let mut regions = Vec::new();
    let mut i = 8;
    while i + 16 <= data.len() {
        let va = u64::from_le_bytes(data[i..i + 8].try_into().unwrap());
        let len = u64::from_le_bytes(data[i + 8..i + 16].try_into().unwrap()) as usize;
        i += 16;
        if i + len > data.len() {
            bail!("{}: truncated region at va {:#x}", path.display(), va);
        }
        regions.push((va, data[i..i + len].to_vec()));
        i += len;
    }
    if regions.is_empty() {
        bail!("{}: no regions", path.display());
    }
    Ok(regions)
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
fn emit_layout(prog: &Program, blob_path: &Path, snapshot: Option<&[(u64, Vec<u8>)]>) -> Option<Layout> {
    use std::fmt::Write as _;

    // The mapped initial memory comes from either a runtime snapshot (A+B: the
    // game's post-init state, so lifted functions see real globals/heap) or the
    // static section image. Executable sections are included too: code sections
    // also hold absolute-addressed read-only data (string literals, jump tables,
    // constants), and packers (UPX) merge .rdata into a code section. The
    // transpiled functions run from the ELF's own segments, so the mapped bytes
    // serve only as data.
    let regions: Vec<(u64, Vec<u8>)> = match snapshot {
        Some(s) => s.iter().filter(|(va, b)| *va != 0 && !b.is_empty()).cloned().collect(),
        None => prog
            .sections
            .iter()
            .filter(|s| s.address != 0 && !s.data.is_empty())
            .map(|s| (s.address, s.data.clone()))
            .collect(),
    };
    if regions.is_empty() {
        return None;
    }

    // Concatenate region bytes into one blob; record (va, blob offset, len).
    let mut blob: Vec<u8> = Vec::new();
    let mut table: Vec<(u64, usize, usize)> = Vec::new();
    for (va, bytes) in &regions {
        let off = blob.len();
        blob.extend_from_slice(bytes);
        table.push((*va, off, bytes.len()));
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
    // Bounds of the mapped image, exported so the HLE can safely scan it (e.g. the
    // version-info APIs locate the PE's VS_VERSIONINFO resource in the mapped .rsrc).
    c.push_str("uint32_t aret_image_lo = 0, aret_image_hi = 0;\n");
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
\x20   aret_image_lo = (uint32_t)lo; aret_image_hi = (uint32_t)hi;\n\
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

/// setjmp/longjmp are not ordinary shims: they are expanded as macros at the
/// lifted call site (see `setjmp_macros`) so the real setjmp/longjmp run in the
/// lifted function's own native frame. We must therefore NOT emit a prototype or
/// a weak stub for these sanitized names (the macro would clobber them).
fn is_setjmp_intrinsic(sym: &str) -> bool {
    matches!(sym, "aret_setjmp" | "aret_setjmp3" | "aret_longjmp")
}

/// The macro block routing setjmp/longjmp imports to the host's setjmp/longjmp,
/// expanded inline at each lifted call site. Appended to `aret_decls.h` only when
/// the binary actually imports one of them, so other programs are byte-identical.
fn setjmp_macros() -> &'static str {
    "\n/* setjmp/longjmp intrinsics (see aret_hle.c::aret_jmpbuf_for). Expanded at\n\
     \x20* the lifted call site so the jump runs in the lifted function's own native\n\
     \x20* frame; the native call stack mirrors the logical one 1:1, so longjmp\n\
     \x20* unwinds it correctly. The arg slots are the program's cdecl stack:\n\
     \x20* [esp+0] = jmp_buf address (our key), [esp+1] = longjmp value. */\n\
     #include <setjmp.h>\n\
     jmp_buf *aret_jmpbuf_for(uint32_t key);\n\
     void aret_longjmp_do(uint32_t key, int val);\n\
     #define ARET_SJ_KEY(esp) (((const uint32_t *)(uintptr_t)(esp))[0])\n\
     #define aret_setjmp(esp)  ((uint32_t)setjmp(*aret_jmpbuf_for(ARET_SJ_KEY(esp))))\n\
     #define aret_setjmp3(esp) ((uint32_t)setjmp(*aret_jmpbuf_for(ARET_SJ_KEY(esp))))\n\
     #define aret_longjmp(esp) (aret_longjmp_do(ARET_SJ_KEY(esp), \\\n\
     \x20   (int)((const uint32_t *)(uintptr_t)(esp))[1]), 0u)\n"
}

/// Does this program import a setjmp/longjmp intrinsic?
fn uses_setjmp(prog: &Program) -> bool {
    prog.imports
        .values()
        .any(|raw| is_setjmp_intrinsic(&ir::build::sanitize_import(raw)))
}

/// Does this program use table-driven exception handling — SEH (`_except_handler3`
/// or its /GS successor `_except_handler4_common`, __try/__except) or C++
/// (`__CxxFrameHandler`/`2`/`3`, throw/catch)? All of them dispatch through the same
/// non-local transfer: a handler longjmps to a setjmp the lifter must inject at the
/// SEH-establish (`mov fs:[0],reg`). Gates that injection + its runtime declarations
/// (so a binary using none of them stays byte-identical).
///
/// `_except_handler4_common` had to be added here explicitly: a modern MSVC /GS binary
/// imports **only** that one, so the gate would otherwise stay closed and the handler
/// would have nowhere to longjmp to — the handler itself working perfectly and the
/// transfer silently never happening.
fn uses_seh(prog: &Program) -> bool {
    prog.imports.values().any(|raw| {
        let s = ir::build::sanitize_import(raw);
        s == "aret_except_handler3"
            || s == "aret_except_handler4_common"
            || s.starts_with("aret_CxxFrameHandler")
    })
}

/// Declarations for the SEH-establish injection (see structured.rs): the setjmp keyed
/// directly by the frame address (not via ARET_SJ_KEY, which reads [esp]), the handler
/// runner, and the frame carried across the longjmp. Emitted only when uses_seh.
fn seh_decls() -> &'static str {
    "\n/* SEH __try/__except: setjmp injected at each `mov fs:[0],esp`, keyed by the frame\n\
     address so _except_handler3's aret_longjmp_do(frame,…) lands here (see aret_hle.c). */\n\
     #include <setjmp.h>\n\
     jmp_buf *aret_jmpbuf_for(uint32_t key);\n\
     uint64_t aret_seh_run(uint32_t frame, uint32_t level);\n\
     extern uint32_t g_seh_frame;\n\
     #define aret_seh_setjmp(frame) ((uint32_t)setjmp(*aret_jmpbuf_for((uint32_t)(frame))))\n\n"
}

/// Imports whose return value occupies the full `edx:eax` pair — a `long long`,
/// or an 8-byte struct (`div_t`/`ldiv_t`) the 32-bit ABI returns in edx:eax.
/// Their shim must be declared returning `uint64_t` so the call site reads both
/// halves (the call lift already splits a 32-bit call's result into edx:eax); a
/// `uint32_t` declaration would silently drop edx (the remainder / high word).
fn import_returns_u64(sym: &str) -> bool {
    // Names are post-`sanitize_import` (leading underscores stripped), so the
    // msvcrt `_strtoi64` import is `aret_strtoi64`, etc.
    matches!(
        sym,
        "aret_div" | "aret_ldiv" | "aret_lldiv" | "aret_imaxdiv"
            | "aret_strtoll" | "aret_strtoull" | "aret_wcstoll" | "aret_wcstoull"
            | "aret_strtoi64" | "aret_strtoui64" | "aret_atoi64" | "aret_wtoi64"
            | "aret_lseeki64" | "aret_telli64" | "aret_ftelli64" | "aret_filelengthi64"
    )
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
        // setjmp/longjmp keep a weak stub here so the LLVM backend (which calls
        // them as external symbols) still links; in the C backend the macro block
        // in aret_decls.h shadows the call sites, so this stub stays unused.
        let raw_escaped = raw.replace('\\', "\\\\").replace('"', "\\\"");
        let ret = if import_returns_u64(sym) { "uint64_t" } else { "uint32_t" };
        let _ = writeln!(
            s,
            "__attribute__((weak)) {ret} {sym}(uint32_t esp) {{ (void)esp; aret_unimpl(\"{raw_escaped}\"); return 0; }}"
        );
    }
    s
}

/// Emit the indirect-call dispatch table: a sorted VA -> callee map plus
/// `aret_call`, which binary-searches it. Function pointers in transpiled code
/// carry the original code address; `aret_call` turns that back into a call.
///
/// Two kinds of entry, keeping the host/translate frontier consistent for
/// *indirect* calls too: an **internal** VA dispatches to its translated
/// `sub_<va>`; a **host-backed** VA (libm/CRT/glue we did not translate)
/// dispatches through a thin adapter onto its native shim — so no indirect path
/// can ever reach a pruned (un-emitted) body.
/// Table of every HLE shim, by its Win32/CRT name, for the runtime delay-load
/// resolver.
///
/// Why this is generated rather than hand-listed: a lifted DLL resolves an import on
/// first use through `ResolveDelayLoadedAPI`, and the resolver previously matched
/// against nine hard-coded uxtheme entries and hard-aborted on anything else — while
/// the HLE already implements over a thousand APIs. Wine's shell32, lifted, delay-loads
/// `ole32.CoTaskMemAlloc`, which we HAVE; it aborted purely because the resolver could
/// not see it. Enumerating the shims here turns "do we model this API" into one answer
/// used by both the static path and the delay-load path, instead of two lists that
/// drift apart.
///
/// The names come from the runtime sources the builder already embeds, so the table
/// cannot fall behind the shims themselves. The pop comes from `stdcall_pops` — the
/// same ground truth (`@N` import-library decorations) the static path uses, so a
/// delay-loaded call pops exactly what a direct one would.
fn emit_hle_shim_table() -> String {
    use std::collections::BTreeMap;
    use std::fmt::Write as _;
    let mut shims: BTreeMap<&str, u32> = BTreeMap::new();
    for src in [HLE_C, CRT_C, WIN32_C] {
        for line in src.lines() {
            let Some(rest) = line.strip_prefix("uint32_t aret_") else { continue };
            let Some(name) = rest.split('(').next() else { continue };
            if !rest[name.len()..].starts_with("(uint32_t esp)") {
                continue;
            }
            if name.is_empty() || !name.chars().all(|c| c.is_ascii_alphanumeric() || c == '_') {
                continue;
            }
            let pop = crate::ir::stdcall_pops::stdcall_pop_bytes(name).unwrap_or(0);
            shims.insert(name, pop);
        }
    }
    let mut s = String::new();
    s.push_str("/* Generated by ARET — HLE shims by name, for the delay-load resolver. */\n");
    for name in shims.keys() {
        let _ = writeln!(s, "uint32_t aret_{name}(uint32_t);");
    }
    s.push_str("static const struct { const char *fn; uint32_t (*shim)(uint32_t); uint16_t pop; }\n");
    s.push_str("    aret_hle_shims[] = {\n");
    for (name, pop) in &shims {
        let _ = writeln!(s, "    {{ \"{name}\", aret_{name}, {pop} }},");
    }
    s.push_str("};\n");
    s.push_str(
        "int aret_hle_shim_lookup(const char *fn, uint32_t (**shim)(uint32_t), uint16_t *pop) {\n\
\x20   long lo = 0, hi = (long)(sizeof(aret_hle_shims)/sizeof(aret_hle_shims[0])) - 1;\n\
\x20   while (lo <= hi) {\n\
\x20       long m = (lo + hi) / 2;\n\
\x20       int c = __builtin_strcmp(aret_hle_shims[m].fn, fn);\n\
\x20       if (c == 0) { *shim = aret_hle_shims[m].shim; *pop = aret_hle_shims[m].pop; return 1; }\n\
\x20       if (c < 0) lo = m + 1; else hi = m - 1;\n\
\x20   }\n\
\x20   return 0;\n\
}\n",
    );
    s
}

/// Emit the table of every lifted DLL's named exports — `(dll, name) -> VA` — so
/// the HLE can reach lifted code by NAME.
///
/// Why this exists at all: DLL lifting binds what the app **imports statically**
/// (the loader writes the export VA into the IAT slot). In-proc COM works the
/// other way round — the activator asks the module for its class object through
/// `DllGetClassObject`, a name that appears in **no** import table. Without this
/// table `CoCreateInstance` could only ever abort, no matter how many DLLs were
/// lifted alongside the app.
///
/// It is deliberately just the facts the PE stated (rebased): no filtering to
/// "recovered" functions, because a VA that was not recovered still resolves to a
/// **named loud abort** inside `aret_call` ("indirect call to unrecovered
/// function 0x…"), which says more than a lookup silently reporting "absent".
/// Empty (and the lookups return 0/absent) when no DLL was merged in, so a plain
/// exe is byte-identical to before.
fn emit_lifted_exports(exports: &[(String, String, u64)]) -> String {
    use std::fmt::Write as _;
    let mut s = String::new();
    s.push_str(
        "\n/* Generated by ARET — named exports of every LIFTED DLL, for the HLE\n\
        \x20  (COM activation reaches lifted code by name, not through an IAT). */\n",
    );
    s.push_str("static const struct { const char *dll; const char *fn; uint32_t va; }\n");
    s.push_str("    aret_lifted_exports[] = {\n");
    for (dll, fn_name, va) in exports {
        let _ = writeln!(s, "    {{ \"{dll}\", \"{fn_name}\", 0x{va:x}u }},");
    }
    // A C array must not be empty; a terminating NULL row also makes the loops
    // below read naturally without a separate count for the common empty case.
    s.push_str("    { 0, 0, 0 },\n};\n");
    s.push_str(
        "/* Iterate: returns 1 and fills the outputs while `i` is in range. */\n\
        int aret_lifted_export_iter(int i, const char **dll, const char **fn, uint32_t *va) {\n\
        \x20   int n = (int)(sizeof(aret_lifted_exports)/sizeof(aret_lifted_exports[0])) - 1;\n\
        \x20   if (i < 0 || i >= n) return 0;\n\
        \x20   *dll = aret_lifted_exports[i].dll;\n\
        \x20   *fn  = aret_lifted_exports[i].fn;\n\
        \x20   *va  = aret_lifted_exports[i].va;\n\
        \x20   return 1;\n\
        }\n\
        /* Look one up; `dll` NULL matches any module. 0 = no such export. */\n\
        uint32_t aret_lifted_export(const char *dll, const char *fn) {\n\
        \x20   for (int i = 0; aret_lifted_exports[i].fn; i++) {\n\
        \x20       if (dll && __builtin_strcmp(aret_lifted_exports[i].dll, dll) != 0) continue;\n\
        \x20       if (__builtin_strcmp(aret_lifted_exports[i].fn, fn) == 0)\n\
        \x20           return aret_lifted_exports[i].va;\n\
        \x20   }\n\
        \x20   return 0;\n\
        }\n",
    );
    s
}

/// GNU/Itanium C++ EH metadata (recovered by `analysis::gnu_eh` from `.eh_frame`/
/// LSDA), emitted as flat tables the runtime dispatcher (`aret_cxa_throw`, aret_hle.c)
/// consults at throw time. Two tables: the call sites (each guarded PC region ->
/// landing pad + a slice of catches) and the catches (ar_filter selector + the
/// `std::type_info*` SLOT VA — dereferenced at runtime, since imported type_infos are
/// only bound at load). Empty (terminator row only) for any binary without an EH
/// LSDA, so a plain program is byte-identical and the runtime's extern still links.
fn emit_gnu_eh_tables(prog: &Program) -> String {
    use std::fmt::Write as _;
    let funcs = crate::analysis::gnu_eh::gnu_eh_entries(prog);
    let mut s = String::new();
    s.push_str(
        "\n/* Generated by ARET — GNU/Itanium C++ EH call-site + catch tables\n\
        \x20  (recovered from .eh_frame/LSDA; consumed by aret_cxa_throw in aret_hle.c). */\n",
    );
    // Flatten catches; each call site records its [catch_off, catch_count) slice.
    let mut catches: Vec<(i64, u64)> = Vec::new();
    let mut sites: Vec<(u64, u64, u64, usize, usize)> = Vec::new();
    for f in &funcs {
        for cs in &f.call_sites {
            // Only sites that can transfer control (a landing pad) are useful to the
            // dispatcher; a site with no landing pad just propagates, same as absent.
            if cs.landing_pad == 0 {
                continue;
            }
            let off = catches.len();
            for c in &cs.catches {
                catches.push((c.filter, c.type_slot));
            }
            sites.push((cs.start, cs.end, cs.landing_pad, off, cs.catches.len()));
        }
    }
    s.push_str("static const struct { int32_t filter; uint32_t slot; } aret_gnu_eh_catches[] = {\n");
    for (filter, slot) in &catches {
        let _ = writeln!(s, "    {{ {filter}, 0x{slot:x}u }},");
    }
    s.push_str("    { 0, 0 },\n};\n");
    s.push_str(
        "static const struct { uint32_t start, end, lp; int catch_off, catch_count; }\n\
        \x20   aret_gnu_eh_sites[] = {\n",
    );
    for (start, end, lp, off, cnt) in &sites {
        let _ = writeln!(
            s,
            "    {{ 0x{start:x}u, 0x{end:x}u, 0x{lp:x}u, {off}, {cnt} }},"
        );
    }
    s.push_str("    { 0, 0, 0, 0, 0 },\n};\n");
    s.push_str(
        "/* Find the call site whose guarded region covers `pc`; fills the landing pad\n\
        \x20  and the catch slice. Returns 1 on a hit, 0 otherwise. */\n\
        int aret_gnu_eh_site(uint32_t pc, uint32_t *lp, int *catch_off, int *catch_count) {\n\
        \x20   int n = (int)(sizeof(aret_gnu_eh_sites)/sizeof(aret_gnu_eh_sites[0])) - 1;\n\
        \x20   for (int i = 0; i < n; i++)\n\
        \x20       if (pc >= aret_gnu_eh_sites[i].start && pc < aret_gnu_eh_sites[i].end) {\n\
        \x20           *lp = aret_gnu_eh_sites[i].lp;\n\
        \x20           *catch_off = aret_gnu_eh_sites[i].catch_off;\n\
        \x20           *catch_count = aret_gnu_eh_sites[i].catch_count;\n\
        \x20           return 1;\n\
        \x20       }\n\
        \x20   return 0;\n\
        }\n\
        /* Read catch `i` (absolute index into the flat catches table). */\n\
        void aret_gnu_eh_catch(int i, int32_t *filter, uint32_t *slot) {\n\
        \x20   *filter = aret_gnu_eh_catches[i].filter;\n\
        \x20   *slot = aret_gnu_eh_catches[i].slot;\n\
        }\n",
    );
    s
}

fn emit_dispatch(internal: &[u64], host: &[(u64, String)], iat: &[(u64, String)]) -> String {
    use std::collections::BTreeSet;
    use std::fmt::Write as _;
    let mut s = String::new();
    s.push_str("/* Generated by ARET — indirect-call dispatch (internal sub_<va>;\n   host-backed VAs adapt onto their native shim). */\n");
    s.push_str("#include <stdint.h>\n#include \"aret_hle.h\"\n\n");
    s.push_str("typedef uint64_t (*aret_fn)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);\n");
    for &va in internal {
        let _ = writeln!(s, "uint64_t sub_{va:x}(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);");
    }
    // Shim prototypes (deduped) + a per-VA adapter that hands the shim the machine
    // stack pointer (it reads its cdecl args off it, as at a direct call site).
    let mut seen = BTreeSet::new();
    for (_, name) in host.iter().chain(iat.iter()) {
        if seen.insert(name.as_str()) {
            let _ = writeln!(s, "uint32_t {name}(uint32_t);");
        }
    }
    // Indirect call to a host-backed function (a recovered CRT/glue routine, or a
    // `jmp [IAT]` import thunk, reached through a function pointer). `aret_call`
    // always hands over the caller's `esp` with the pushed return address at
    // [esp+0] (the lifted indirect call models `esp -= 4`, as for the internal
    // `sub_` ABI which reads its args at [esp+4]); the shim reads cdecl args at
    // [esp+0], so undo that push with `esp + 4` — exactly like the IAT trampoline
    // below. Without it the shim reads the return address as its first argument
    // (a real crash: sqlite's `GetSystemInfo` wrote a SYSTEM_INFO through it).
    for (va, name) in host {
        let _ = writeln!(
            s,
            "static uint64_t aret_disp_{va:x}(uint64_t esp,uint64_t a,uint64_t c,uint64_t d,uint64_t b,uint64_t si,uint64_t di,uint64_t bx){{(void)a;(void)c;(void)d;(void)b;(void)si;(void)di;(void)bx;return {name}((uint32_t)esp + 4);}}"
        );
    }
    // Indirect call straight through an IAT slot: the slot holds its own VA (set by
    // __aret_patch_iat), so a `call [slot]` dispatches here. The indirect call
    // pushed a return address (esp -= 4 in the lifted call), but an import shim
    // reads its cdecl args at [esp+0] — so undo that push with `esp + 4`.
    for (va, name) in iat {
        let _ = writeln!(
            s,
            "static uint64_t aret_iatdisp_{va:x}(uint64_t esp,uint64_t a,uint64_t c,uint64_t d,uint64_t b,uint64_t si,uint64_t di,uint64_t bx){{(void)a;(void)c;(void)d;(void)b;(void)si;(void)di;(void)bx;return {name}((uint32_t)esp + 4);}}"
        );
    }
    s.push_str("struct aret_e { uint32_t va; aret_fn fn; };\n");
    s.push_str("static const struct aret_e aret_tab[] = {\n");
    // Merge all kinds, sorted by VA (binary search requires it).
    let mut all: Vec<(u64, String)> =
        internal.iter().map(|&va| (va, format!("sub_{va:x}"))).collect();
    all.extend(host.iter().map(|(va, _)| (*va, format!("aret_disp_{va:x}"))));
    all.extend(iat.iter().map(|(va, _)| (*va, format!("aret_iatdisp_{va:x}"))));
    all.sort_by_key(|(va, _)| *va);
    all.dedup_by_key(|(va, _)| *va);
    for (va, callee) in &all {
        let _ = writeln!(s, "    {{ 0x{va:x}u, {callee} }},");
    }
    s.push_str("};\n\n");
    s.push_str(
        "uint64_t aret_call(uint32_t va, uint64_t esp, uint64_t a, uint64_t c, uint64_t d, uint64_t b, uint64_t si, uint64_t di, uint64_t bx) {\n\
\x20   long lo = 0, hi = (long)(sizeof(aret_tab)/sizeof(aret_tab[0])) - 1;\n\
\x20   while (lo <= hi) {\n\
\x20       long m = (lo + hi) / 2;\n\
\x20       if (aret_tab[m].va == va) return aret_tab[m].fn(esp, a, c, d, b, si, di, bx);\n\
\x20       if (aret_tab[m].va < va) lo = m + 1; else hi = m - 1;\n\
\x20   }\n\
\x20   { uint64_t r; if (aret_delay_dispatch(va, (uint32_t)esp, &r)) return r; }  /* runtime delay-load */\n\
\x20   { char msg[64]; snprintf(msg, sizeof msg, \"indirect call to unrecovered function 0x%x\", va); aret_unmodelled(msg); }\n\
\x20   return 0;\n\
}\n",
    );
    // Callee-pops-args (`ret N`) table for INDIRECT calls: an internal VA that is a
    // __stdcall/FAST_FUNC callee maps to the bytes it pops. After an indirect
    // `call`, the lifted code raises esp by `__aret_callee_pop(target_va)` (see
    // ir::build::callee_pop_adjust) — without which esp drifts N low per call
    // (a real BusyBox `cksum` crash on the indirectly dispatched CRC handler).
    let mut pops: Vec<(u64, u16)> = internal
        .iter()
        .filter_map(|&va| {
            let n = crate::ir::build::callee_pop_bytes(va);
            (n > 0).then_some((va, n))
        })
        .collect();
    pops.sort_by_key(|(va, _)| *va);
    s.push_str(&emit_hle_shim_table());
    if pops.is_empty() {
        s.push_str("uint32_t __aret_callee_pop(uint32_t va){ return aret_delay_pop(va); }\n");
    } else {
        s.push_str("struct aret_pe { uint32_t va; uint16_t pop; };\n");
        s.push_str("static const struct aret_pe aret_poptab[] = {\n");
        for (va, n) in &pops {
            let _ = writeln!(s, "    {{ 0x{va:x}u, {n} }},");
        }
        s.push_str("};\n");
        s.push_str(
            "uint32_t __aret_callee_pop(uint32_t va) {\n\
\x20   long lo = 0, hi = (long)(sizeof(aret_poptab)/sizeof(aret_poptab[0])) - 1;\n\
\x20   while (lo <= hi) {\n\
\x20       long m = (lo + hi) / 2;\n\
\x20       if (aret_poptab[m].va == va) return aret_poptab[m].pop;\n\
\x20       if (aret_poptab[m].va < va) lo = m + 1; else hi = m - 1;\n\
\x20   }\n\
\x20   return aret_delay_pop(va);\n\
}\n",
        );
    }
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
        // A data import → point its slot at the synthetic data object. A function
        // import → store the slot's own VA, so a `call [slot]` (or a call through a
        // function pointer the program copied out of the slot) dispatches via
        // aret_call to the slot's `esp+4` trampoline (see emit_dispatch).
        let _ = writeln!(
            s,
            "    {{ uint32_t p = aret_data_import(\"{esc}\"); *(uint32_t *)(uintptr_t)0x{va:x}u = p ? p : 0x{va:x}u; }}"
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

/// Every `aret_*` shim with a real definition in the embedded runtime sources.
/// A called shim *not* in this set resolves to the weak "unimplemented" stub at
/// link time — i.e. it warns and returns 0, a guess. Parsed from the source
/// (rather than hand-listed) so it never drifts from what is actually shimmed.
fn implemented_shims() -> std::collections::BTreeSet<String> {
    let sources = [HLE_C, CRT_C, WIN32_C, NTDLL_C];
    let mut set = std::collections::BTreeSet::new();
    // 1. Direct definitions: `aret_x(uint32_t`.
    for src in sources {
        let b = src.as_bytes();
        let mut i = 0;
        while let Some(p) = src[i..].find("aret_") {
            let start = i + p;
            let mut end = start;
            while end < b.len() && (b[end].is_ascii_alphanumeric() || b[end] == b'_') {
                end += 1;
            }
            if src[end..].trim_start().starts_with("(uint32_t") {
                set.insert(src[start..end].to_string());
            }
            i = end.max(start + 1);
        }
    }
    // 2. Macro-generated definitions (e.g. `MATH1(pow, pow)` → `aret_pow`):
    // discover any macro whose body token-pastes `aret_##<first-param>`, then add
    // `aret_<name>` for each invocation's first argument. Self-maintaining: a new
    // shim macro is picked up without touching this code.
    let firstarg = |s: &str| -> Option<String> {
        let p = s.find('(')?;
        let name: String = s[p + 1..]
            .chars()
            .take_while(|&c| c.is_ascii_alphanumeric() || c == '_')
            .collect();
        (!name.is_empty()).then_some(name)
    };
    let mut gen_macros: Vec<(String, String)> = Vec::new(); // (macro, first-param)
    for src in sources {
        for line in src.lines() {
            if let Some(rest) = line.trim_start().strip_prefix("#define ") {
                if let (Some(paren), Some(param)) = (rest.find('('), firstarg(rest)) {
                    let mac = rest[..paren].trim().to_string();
                    if rest.contains(&format!("aret_##{param}")) {
                        gen_macros.push((mac, param));
                    }
                }
            }
        }
    }
    for src in sources {
        for line in src.lines() {
            let t = line.trim();
            for (mac, _) in &gen_macros {
                if t.starts_with(&format!("{mac}(")) {
                    if let Some(name) = firstarg(t) {
                        set.insert(format!("aret_{name}"));
                    }
                }
            }
        }
    }
    set
}

/// Static axis-2 coverage of a binary's import table: for every distinct import
/// the PE declares, whether ARET ships a native shim for it (`aret_<sanitized>`
/// present in the HLE/CRT/Win32 layers, or a setjmp/longjmp intrinsic). Unlike
/// the transpile report's `unimplemented_imports` — which lists only the imports
/// the recovered code is *proven to call* — this classifies the **whole** import
/// table, appelés ou non. It is the a-priori, *known-in-advance* measure of how
/// ready ARET is for a given binary (axis 2), independent of function recovery:
/// an uncovered import is one that would hit the weak stub at runtime, so the
/// list is exactly the axis-2 gap to close (by a general shim, never a per-binary
/// patch). Names are raw import names, deduplicated and sorted.
pub struct ImportCoverage {
    /// Raw import names ARET already shims natively.
    pub covered: Vec<String>,
    /// Raw import names with no shim — would reach the unimplemented weak stub.
    pub uncovered: Vec<String>,
}

impl ImportCoverage {
    pub fn total(&self) -> usize {
        self.covered.len() + self.uncovered.len()
    }
}

/// Classify a program's whole import table against the shipped shim set.
pub fn import_coverage(prog: &crate::loader::Program) -> ImportCoverage {
    let impl_set = implemented_shims();
    let mut covered = std::collections::BTreeSet::new();
    let mut uncovered = std::collections::BTreeSet::new();
    for raw in prog.imports.values() {
        let shim = crate::ir::build::sanitize_import(raw);
        if impl_set.contains(&shim) || is_setjmp_intrinsic(&shim) {
            covered.insert(raw.clone());
        } else {
            uncovered.insert(raw.clone());
        }
    }
    ImportCoverage {
        covered: covered.into_iter().collect(),
        uncovered: uncovered.into_iter().collect(),
    }
}

/// Collect every unmodelled instruction (a lift gap) across all functions,
/// deduplicated by text with a site count, most-frequent first. An unmodelled
/// instruction surfaces two ways — a `Stmt::Asm` (statement form) or an
/// `asm:`-named call *expression* (an op that yields no value) — mirroring
/// `has_opaque_asm`; both are counted. This is the complete, static
/// per-instruction wall list the runtime would otherwise reveal one at a time.
fn collect_unmodelled_insns(irfs: &[ir::types::IrFunction]) -> Vec<(String, usize)> {
    use ir::types::{CallTarget, Expr, Stmt};
    use std::collections::BTreeMap;
    fn expr_walk(e: &Expr, c: &mut BTreeMap<String, usize>) {
        match e {
            Expr::Call { target, args, .. } => {
                if let CallTarget::Named(n) = target {
                    if let Some(insn) = n.strip_prefix("asm:") {
                        *c.entry(insn.to_string()).or_insert(0) += 1;
                    }
                }
                if let CallTarget::Indirect(x) = target {
                    expr_walk(x, c);
                }
                for a in args {
                    expr_walk(a, c);
                }
            }
            Expr::Load { addr, .. } => expr_walk(addr, c),
            Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => expr_walk(x, c),
            Expr::Binary(_, a, b) => {
                expr_walk(a, c);
                expr_walk(b, c);
            }
            Expr::Select { cond, then_, else_ } => {
                expr_walk(cond, c);
                expr_walk(then_, c);
                expr_walk(else_, c);
            }
            _ => {}
        }
    }
    fn stmt_walk(s: &Stmt, c: &mut BTreeMap<String, usize>) {
        match s {
            Stmt::Asm(t) => {
                *c.entry(t.clone()).or_insert(0) += 1;
            }
            Stmt::Set { expr, .. } | Stmt::Assign { expr, .. } | Stmt::CallStmt(expr) => {
                expr_walk(expr, c)
            }
            Stmt::Store { addr, value, .. } => {
                expr_walk(addr, c);
                expr_walk(value, c);
            }
            Stmt::Branch { cond, .. } => expr_walk(cond, c),
            Stmt::Switch { value, .. } => expr_walk(value, c),
            Stmt::Return(Some(e)) => expr_walk(e, c),
            _ => {}
        }
    }
    let mut counts: BTreeMap<String, usize> = BTreeMap::new();
    for irf in irfs {
        for b in &irf.blocks {
            for st in &b.stmts {
                stmt_walk(st, &mut counts);
            }
        }
    }
    let mut v: Vec<(String, usize)> = counts.into_iter().collect();
    v.sort_by(|a, b| b.1.cmp(&a.1).then_with(|| a.0.cmp(&b.0)));
    v
}

/// Named shims a recovered function actually *calls* (`CallTarget::Named`), e.g.
/// `aret_qsort` for an intercepted `qsort` import. Intersected against
/// `implemented_shims` to find calls that would hit the unimplemented stub.
fn collect_named_calls(irfs: &[ir::types::IrFunction]) -> std::collections::BTreeSet<String> {
    use ir::types::{CallTarget, Expr, Stmt};
    use std::collections::BTreeSet;
    let mut names: BTreeSet<String> = BTreeSet::new();
    fn ex(e: &Expr, out: &mut BTreeSet<String>) {
        match e {
            Expr::Call { target, args, .. } => {
                if let CallTarget::Named(n) = target {
                    out.insert(n.clone());
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
    fn st(s: &Stmt, out: &mut BTreeSet<String>) {
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
                st(s, &mut names);
            }
        }
    }
    names
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

/// Query pkg-config for a set of packages' compile/link flags (looking under the
/// i386 multiarch pkgconfig dir too). Returns `(cflags, libs)`, or `None` when
/// *any* package is missing — the dependent feature layer then degrades to its
/// sound fallback (a no-op / abort), never a build failure. All packages must be
/// present for the feature to activate (e.g. FreeType text needs both freetype2
/// and fontconfig).
fn pkgconfig_flags(pkgs: &[&str]) -> Option<(Vec<String>, Vec<String>)> {
    let extra = "/usr/lib/i386-linux-gnu/pkgconfig";
    let mut pc_path = std::env::var("PKG_CONFIG_PATH").unwrap_or_default();
    if !pc_path.split(':').any(|p| p == extra) {
        pc_path = if pc_path.is_empty() { extra.to_string() } else { format!("{extra}:{pc_path}") };
    }
    let run = |arg: &str| -> Option<Vec<String>> {
        let o = Command::new("pkg-config")
            .env("PKG_CONFIG_PATH", &pc_path)
            .arg(arg).args(pkgs).output().ok()?;
        if !o.status.success() { return None; }
        Some(String::from_utf8_lossy(&o.stdout).split_whitespace().map(str::to_string).collect())
    };
    Some((run("--cflags")?, run("--libs")?))
}

/// SDL2 flags for the visible-window layer (G2b). `None` ⇒ display-free fallback.
fn sdl2_flags() -> Option<(Vec<String>, Vec<String>)> { pkgconfig_flags(&["sdl2"]) }

/// In `dir`, find the shortest filename matching `<stem>.*` — i.e. the soname
/// symlink (`libfreetype.so.6`) rather than the fully-versioned real file
/// (`libfreetype.so.6.20.1`). Used to link the i386 runtime `.so.N` explicitly
/// when no unversioned `-dev` symlink is installed.
fn find_soname(dir: &str, stem: &str) -> Option<String> {
    let prefix = format!("{stem}.");
    std::fs::read_dir(dir).ok()?
        .filter_map(|e| e.ok())
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| n.starts_with(&prefix))
        .min_by_key(|n| n.len())
}

/// FreeType + fontconfig flags for the GDI text-raster layer (G3-text). FreeType
/// rasterizes glyphs bit-identically to Wine (Wine uses FreeType too); fontconfig
/// resolves a logical face name to a real font file the same way Wine does on
/// Linux. Include paths come from pkg-config; the libs are the i386 runtime
/// sonames linked explicitly (the multiarch dir ships `.so.N` but often no
/// unversioned `-dev` symlink, so bare `-lfreetype` would grab the wrong arch).
/// `None` (headers or i386 libs absent) ⇒ TextOut stays a sound abort, never a
/// build failure. Statically linkable later for full autonomy (the `.so` here
/// mirrors how SDL2 is linked); the binary carries the real font glyphs, no Wine
/// runtime dependency.
fn freetype_flags() -> Option<(Vec<String>, Vec<String>)> {
    let (cflags, _) = pkgconfig_flags(&["freetype2", "fontconfig"])?;
    let dir = "/usr/lib/i386-linux-gnu";
    let ft = find_soname(dir, "libfreetype.so")?;
    let fc = find_soname(dir, "libfontconfig.so")?;
    let libs = vec![format!("-L{dir}"), format!("-l:{ft}"), format!("-l:{fc}")];
    Some((cflags, libs))
}

/// Transpile `funcs` to C, link with the HLE runtime, and produce a native
/// executable in `out_dir`. When `run` is set, execute it and capture stdout.
#[allow(clippy::too_many_arguments)]
pub fn transpile(
    prog: &Program,
    funcs: &[&Function],
    out_dir: &Path,
    run: bool,
    entry_override: Option<u64>,
    backend: &str,
    wasm: bool,
    snapshot: Option<&[(u64, Vec<u8>)]>,
    prog_args: &[String],
    walls_only: bool,
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
    // Identify functions that return an fp value in st(0), so the x87 depth
    // analysis counts the result a `call` to one of them pushes. Without this a
    // call to e.g. `double lua_version(...)` underflows the modelled x87 stack and
    // the whole function's float ops degrade to opaque asm (a real-program bug:
    // Lua's version check then always takes its error path).
    ir::build::set_noreturn(ir::build::compute_noreturn(funcs));
    ir::build::set_fp_returning(ir::build::compute_fp_returning(prog, funcs));
    // Per-callee ecx clobber masks: a direct call to a function that provably
    // preserves ecx must not discard the caller's live ecx (GCC -O2 relies on
    // this for static functions; blanket clobbering corrupts e.g. BusyBox).
    ir::build::set_call_clobbers(ir::build::compute_call_clobbers(funcs));
    // Per-callee `ret N` pop bytes: a call to a __stdcall/FAST_FUNC internal
    // function (which pops its own stack args) must raise the caller's esp by N,
    // or esp drifts N low per call (BusyBox `cksum` crashes on the indirectly
    // called FAST_FUNC CRC handler). Empty unless the program has such a function.
    ir::build::set_callee_pops(ir::build::compute_callee_pops(funcs));

    std::fs::create_dir_all(out_dir)
        .with_context(|| format!("failed to create {}", out_dir.display()))?;

    // Lower every function and emit modular C using the shared machine stack (so
    // both stack- and register-passed arguments cross function calls). The mode
    // flag is read during SSA *and* emission, so it wraps both. The program is
    // split into chunks of functions so the compiler never sees one giant TU.
    const CHUNK_FUNCS: usize = 200;
    emit::set_shared_stack(true);
    // Inject the SEH setjmp only when the program uses _except_handler3 (__try/__except),
    // so every other program's lifted code stays byte-identical.
    emit::set_seh_active(uses_seh(prog));
    // Execution trace (doc 81 §I1): `ARET_TRACE=1` prefixes each function with an entry
    // record (VA + esp + regs) dumped by the runtime on a crash, to reconstruct the call
    // chain leading to a late corruption. Off by default → default build byte-identical.
    emit::set_trace(std::env::var_os("ARET_TRACE").is_some());
    // Import-relay build (doc 81 §4 execution diff): wrap every HLE shim call in a
    // runtime-gated relay log, comparable to Wine's +relay. Off unless ARET_RELAY is
    // set at BUILD time; when off, byte-identical output (no wrapper emitted).
    emit::set_relay(std::env::var_os("ARET_RELAY").is_some());
    // Partition recovered functions at the host/translate frontier and make it
    // *structural*: a host-backed function (libm/CRT/glue recognized by symbol) is
    // NOT translated — its body would be dead for direct calls (redirected to the
    // shim) and, for lifted libm, simply wrong. We never prune the entry point.
    let mut internal_funcs: Vec<&Function> = Vec::new();
    let mut host_funcs: Vec<(u64, String)> = Vec::new();
    for &f in funcs {
        match ir::build::host_shim_name(prog, f.entry) {
            Some(name) if f.entry != entry => host_funcs.push((f.entry, name)),
            _ => internal_funcs.push(f),
        }
    }
    host_funcs.sort_by_key(|(va, _)| *va);
    host_funcs.dedup_by_key(|(va, _)| *va);
    // Invariant: the two classes are disjoint — no VA is both translated and
    // host-backed (would mean a call site contradicts the dispatch). Holds by
    // construction (one `resolve_call` decision per address); asserted in debug.
    debug_assert!(
        {
            let h: std::collections::BTreeSet<u64> = host_funcs.iter().map(|(va, _)| *va).collect();
            internal_funcs.iter().all(|f| !h.contains(&f.entry))
        },
        "frontier contradiction: a function is both internal and host-backed"
    );

    let irfs: Vec<_> = internal_funcs.iter().map(|&f| lower(prog, f)).collect();
    let n_funcs = funcs.len();
    // Classification (for honest reporting): translated functions split into fully
    // lifted vs partially simulated (some opaque `asm`); the rest are host-backed.
    let n_host = host_funcs.len();
    let mut n_partial = 0usize;
    for irf in &irfs {
        if ir::build::has_opaque_asm(irf) {
            n_partial += 1;
        }
    }
    let n_lifted = irfs.len() - n_partial;
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
    emit::set_seh_active(false);
    // Soundness: which called shims have no real implementation (they would hit
    // the weak "unimplemented" stub — warn + return 0, a silent wrong result).
    let unimplemented_imports: Vec<String> = {
        let impl_set = implemented_shims();
        collect_named_calls(&irfs)
            .into_iter()
            .filter(|n| n.starts_with("aret_") && !impl_set.contains(n) && !is_setjmp_intrinsic(n))
            .collect()
    };
    // The complete per-instruction lift-gap list (for the wall map + the report).
    let unmodelled_insns = collect_unmodelled_insns(&irfs);
    // `--mode walls`: the caller only wants the coverage-gap map, so stop here —
    // before the expensive emit / layout / compile — with everything the report
    // needs already computed. Same recovery + lift as a real transpile, so the
    // map is exactly the one the produced binary would have.
    if walls_only {
        let unresolved = collect_undef_subs(&irfs);
        return Ok(TranspileReport {
            out_dir: out_dir.to_path_buf(),
            binary: std::path::PathBuf::new(),
            functions: n_funcs,
            bits: prog.bitness.bits() as u32,
            lifted: n_lifted,
            partial: n_partial,
            host_backed: n_host,
            unresolved,
            unimplemented_imports,
            unmodelled_insns,
            run_output: None,
        });
    }
    // The IR is no longer needed; free it before spawning the parallel compilers
    // (which need the memory) on a large program.
    drop(irfs);

    // Memory Layout Mapper: restore data sections at their original VAs so
    // absolute pointers (global strings/tables) resolve at runtime.
    let blob_path = out_dir.join("sections.bin");
    let layout = emit_layout(prog, &blob_path, snapshot);
    let map_call = if layout.is_some() {
        // Map sections, then patch data-import IAT slots (which live in a mapped
        // section) to synthetic objects.
        "    __aret_map_memory();\n    __aret_patch_iat();\n"
    } else {
        ""
    };
    // A `wmain`-style entry (the program imports `__wgetmainargs`, the wide CRT
    // arg fetcher) reads `argv[i]` as `wchar_t*`. Handing it the native *narrow*
    // argv makes it read every other byte (`SELECT` -> `SLC`, then
    // WideCharToMultiByte keeps the low byte of each misread wchar). Build UTF-16
    // copies of the real args and hand those over instead.
    let wide_args = prog
        .imports
        .values()
        .any(|n| n.trim_start_matches('_') == "wgetmainargs");
    let (argv_prep, argv_expr) = if wide_args {
        (
            "\x20   /* Wide (UTF-16) argv — the entry imports __wgetmainargs (wmain). */\n\
             \x20   static uint16_t aret_wbuf[1u << 16]; static uint32_t aret_wargv[1024];\n\
             \x20   { uint32_t wo = 0; for (int i = 0; i < argc && i < 1024; i++) {\n\
             \x20       aret_wargv[i] = (uint32_t)(uintptr_t)&aret_wbuf[wo];\n\
             \x20       for (const unsigned char *p = (const unsigned char *)argv[i]; *p && wo < (1u << 16) - 1; p++) aret_wbuf[wo++] = *p;\n\
             \x20       aret_wbuf[wo++] = 0; } }\n",
            "(uint32_t)(uintptr_t)aret_wargv",
        )
    } else {
        ("", "(uint32_t)(uintptr_t)argv")
    };

    // aret_main.c — a native entry that maps memory, sets up the single shared
    // machine stack, then drives the transpiled entry point with the stack-top
    // pointer. Every transpiled function threads this `__esp` through its calls.
    // Lifted DLL initializers (DllMain), run before the app entry so a merged
    // DLL registers its window classes / inits globals. Each is called as
    // `_DllMainCRTStartup(hinstDLL, DLL_PROCESS_ATTACH=1, 0)` on the shared
    // machine stack; it returns before the app entry frame is laid at `top`.
    let mut dll_init_decls = String::new();
    let mut dll_init_calls = String::new();
    for &(entry, base) in &prog.dll_inits {
        dll_init_decls.push_str(&format!(
            "         uint64_t sub_{entry:x}(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);\n"
        ));
        dll_init_calls.push_str(&format!(
            "    {{ uint32_t *dsp=(uint32_t*)top; dsp[0]=0; dsp[1]=0x{base:x}u; dsp[2]=1u; dsp[3]=0u; \
             sub_{entry:x}((uint64_t)(uintptr_t)top, 0x{base:x}u, 1u, 0, 0); }}\n"
        ));
    }
    // C++ global constructors (doc 71 2026-08-08): mingw defers these to
    // `___main`→`__do_global_ctors`, which ARET no-ops — so lifted libstdc++'s
    // `_GLOBAL__sub_I` (which builds std::cout/cin/cerr) would never run. Run them
    // here in call order (recover_ctor_list already reversed __CTOR_LIST__), on the
    // shared stack, as a cdecl `void ctor(void)`. Only non-empty for a multi-module
    // lift, so C/MSVC single-binary transpiles are unaffected (hash unchanged).
    let mut ctor_decls = String::new();
    let mut ctor_calls = String::new();
    for &va in &prog.ctor_list {
        ctor_decls.push_str(&format!(
            "         uint64_t sub_{va:x}(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);\n"
        ));
        ctor_calls.push_str(&format!(
            "    {{ uint32_t *csp=(uint32_t*)top; csp[0]=0; sub_{va:x}((uint64_t)(uintptr_t)top, 0, 0, 0, 0); }}\n"
        ));
    }
    let main_c = format!(
        "#include <stdint.h>\n\n\
         /* One shared machine stack for all transpiled functions (UBT M3). */\n\
         static uint8_t aret_stack[1u << 20];\n\n\
         /* The process's real args, published for the CRT's `__getmainargs` shim so\n\
         the Windows program sees its actual argc/argv/environ (a multi-call binary\n\
         like BusyBox dispatches on argv[0], so it must be invoked under that name). */\n\
         int aret_real_argc = 0;\n\
         char **aret_real_argv = 0;\n\n\
         void __aret_map_memory(void);\n\
         void __aret_patch_iat(void);\n\
         void __aret_set_stack_bounds(uint32_t base, uint32_t limit);\n\
         uint64_t sub_{entry:x}(uint64_t __esp, uint64_t a, uint64_t c, uint64_t d, uint64_t b);\n\
         {dll_init_decls}\
         {ctor_decls}\
         int main(int argc, char **argv) {{\n\
         \x20   aret_real_argc = argc; aret_real_argv = argv;\n\
         {map_call}    uint8_t *top = aret_stack + sizeof(aret_stack) - 64;\n\
         \x20   /* Publish the real machine-stack bounds to the synthetic TEB (fs:[4]=\n\
         \x20      StackBase highest addr, fs:[8]=StackLimit lowest) so a CRT that reads\n\
         \x20      them and dereferences near the top hits real memory, not a fake VA. */\n\
         \x20   __aret_set_stack_bounds((uint32_t)(uintptr_t)(aret_stack + sizeof(aret_stack)),\n\
         \x20                           (uint32_t)(uintptr_t)aret_stack);\n\
         {dll_init_calls}\
         {ctor_calls}\
         {argv_prep}\
         \x20   /* Lay a cdecl frame so an entry at `main` reads argc/argv from the\n\
         \x20      shared machine stack at [esp+4]/[esp+8]; harmless for a no-arg\n\
         \x20      startup entry. */\n\
         \x20   uint32_t *sp = (uint32_t *)top;\n\
         \x20   sp[0] = 0;                                /* return address */\n\
         \x20   sp[1] = (uint32_t)argc;                   /* argc  @ esp+4 */\n\
         \x20   sp[2] = {argv_expr};        /* argv  @ esp+8 */\n\
         \x20   uint64_t esp = (uint64_t)(uintptr_t)top;\n\
         \x20   return (int)sub_{entry:x}(esp, (uint32_t)argc, {argv_expr}, 0, 0);\n\
         }}\n",
    );

    let write = |name: &str, body: &str| -> Result<()> {
        std::fs::write(out_dir.join(name), body)
            .with_context(|| format!("failed to write {}", name))
    };
    // Weak stubs: one per unimplemented import (warns, returns 0 — a known
    // function with no shim yet), plus one per referenced-but-unrecovered
    // function address (aborts via aret_unmodelled — unknown code, must not be
    // faked). The program links even though static recovery is incomplete, and
    // the gaps fail loud at runtime instead of returning a wrong result.
    let mut stubs = emit_import_stubs(prog);
    for &addr in &undef_subs {
        stubs.push_str(&format!(
            "__attribute__((weak)) uint64_t sub_{addr:x}(uint64_t e,uint64_t a,uint64_t c,uint64_t d,uint64_t b){{ (void)e;(void)a;(void)c;(void)d;(void)b; aret_unmodelled(\"sub_{addr:x} (unrecovered function)\"); return 0; }}\n"
        ));
    }
    // A global ctor that ARET recovered gets a strong `sub_<va>` in a chunk (it runs and
    // e.g. constructs std::cout); one it deliberately no-ops as startup glue — chiefly
    // `__gcc_register_frame` (matched by is_glue_name) — has none, so give every ctor a
    // WEAK no-op fallback. The strong definition wins when present; the glue ctor links as
    // the harmless no-op ARET already intends. (undef_subs' abort-stub can't be used here:
    // a no-op'd glue ctor is not an "unrecovered function", it is meant to do nothing.)
    for &va in &prog.ctor_list {
        stubs.push_str(&format!(
            "__attribute__((weak)) uint64_t sub_{va:x}(uint64_t e,uint64_t a,uint64_t c,uint64_t d,uint64_t b){{ (void)e;(void)a;(void)c;(void)d;(void)b; return 0; }}\n"
        ));
    }
    // Indirect-call dispatch table: internal entries (translated) + host-backed
    // entries (adapted onto their shims). Sorted/deduped inside emit_dispatch.
    let mut internal_entries: Vec<u64> = internal_funcs.iter().map(|f| f.entry).collect();
    internal_entries.sort_unstable();
    internal_entries.dedup();
    write("aret_hle.h", HLE_H)?;
    write("aret_hle.c", HLE_C)?;
    write("aret_crt.c", CRT_C)?;
    write("aret_win32.c", WIN32_C)?;
    write("aret_ntdll.c", NTDLL_C)?;
    write("mlang_cp_table.h", MLANG_CP_TABLE_H)?;
    write("cp1252_rev_table.h", CP1252_REV_TABLE_H)?;
    write("cp437_tables.h", CP437_TABLES_H)?;
    // Heavy-form (doc 82): the vendored Wine ntdll source + its ASCII floor + the
    // self-contained NT-types layer, written under out_dir/wine_heavy/ so the special
    // -fshort-wchar compile below can find the shim headers via -I.
    {
        let wh = out_dir.join("wine_heavy");
        std::fs::create_dir_all(wh.join("native/wine"))?;
        std::fs::create_dir_all(wh.join("native/ddk"))?;
        std::fs::write(wh.join("rtlstr.c"), WINE_RTLSTR_C)?;
        std::fs::write(wh.join("ntdll_floor.c"), WINE_FLOOR_C)?;
        std::fs::write(wh.join("ntdll_ntreg.c"), WINE_NTREG_C)?;
        std::fs::write(wh.join("reg.c"), WINE_REG_C)?;
        std::fs::write(wh.join("native/nt_types.h"), WINE_NT_TYPES_H)?;
        std::fs::write(wh.join("native/reg_types.h"), WINE_REG_TYPES_H)?;
        std::fs::write(wh.join("native/ntdll_floor.h"), WINE_FLOOR_H)?;
        std::fs::write(wh.join("native/wine/debug.h"), WINE_DEBUG_H)?;
        std::fs::write(wh.join("native/ddk/ntddk.h"), "")?;
        std::fs::write(wh.join("native/ntdll_misc.h"),
            "#ifndef MISC_H\n#define MISC_H\n#ifndef ARRAY_SIZE\n#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))\n#endif\n#include \"ntdll_floor.h\"\n#endif\n")?;
        for h in ["windef.h", "winnt.h", "winternl.h", "ntstatus.h"] {
            std::fs::write(wh.join("native").join(h), "#include \"nt_types.h\"\n")?;
        }
    }
    write("aret_stubs.c", &stubs)?;
    // Every IAT slot, as a VA -> import-shim trampoline, so an indirect call
    // through the slot (a function pointer the program copied out of it) resolves
    // to the shim instead of aborting in aret_call.
    let iat_slots: Vec<(u64, String)> = prog
        .imports
        .iter()
        .map(|(&va, name)| (va, ir::build::sanitize_import(name)))
        .collect();
    write(
        "aret_dispatch.c",
        &format!(
            "{}{}{}",
            emit_dispatch(&internal_entries, &host_funcs, &iat_slots),
            emit_lifted_exports(&prog.dll_exports),
            emit_gnu_eh_tables(prog)
        ),
    )?;
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
                if is_setjmp_intrinsic(&sym) {
                    continue; // provided by the macro block below, not a prototype
                }
                if seen.insert(sym.clone()) {
                    let ret = if import_returns_u64(&sym) { "uint64_t" } else { "uint32_t" };
                    let _ = writeln!(protos, "{ret} {sym}(uint32_t);");
                }
            }
            decls_h.push_str(&protos);
            if uses_setjmp(prog) {
                decls_h.push_str(setjmp_macros());
            }
            if uses_seh(prog) {
                decls_h.push_str(seh_decls());
            }
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

    // G2b (doc 72): a program that creates a window links SDL2 so its visible GUI
    // actually shows on screen. Gated on a window-creating import AND pkg-config
    // finding SDL2 (32-bit only, native target). A non-GUI program, a wasm build,
    // or a host without SDL2 gets the exact same compile/link as before (the
    // window layer stays display-free), so this is byte-identical for them.
    let sdl = if !wasm && bits == 32
        && prog.imports.values().any(|n| matches!(n.as_str(),
            "CreateWindowExA" | "CreateWindowExW"
            // A visible dialog needs a real window too (its controls are composited
            // into the dialog framebuffer and presented), even if the app never calls
            // CreateWindowEx directly.
            | "DialogBoxParamA" | "DialogBoxParamW"
            | "DialogBoxIndirectParamA" | "DialogBoxIndirectParamW"
            | "CreateDialogParamA" | "CreateDialogParamW"
            | "CreateDialogIndirectParamA" | "CreateDialogIndirectParamW"))
    {
        sdl2_flags()
    } else {
        None
    };
    // Extra C flags for every compile when SDL is in play: SDL's own cflags plus
    // the feature switch that activates the `#ifdef ARET_HAVE_SDL` window layer.
    let sdl_cflags: Vec<String> = sdl.as_ref().map(|(c, _)| {
        let mut v = c.clone();
        v.push("-DARET_HAVE_SDL".to_string());
        v
    }).unwrap_or_default();
    let sdl_libs: Vec<String> = sdl.as_ref().map(|(_, l)| l.clone()).unwrap_or_default();

    // G3-text (doc 72): a program that draws text (TextOut/ExtTextOut/DrawText/
    // TabbedText) links FreeType+fontconfig so GDI text rasterizes bit-identically
    // to Wine (Wine rasterizes with FreeType too). Gated on a text-drawing import
    // AND pkg-config finding both libs (32-bit only, native target). A program that
    // draws no text, a wasm build, or a host without the libs gets the exact same
    // compile/link as before (TextOut stays a sound abort), so this is byte-identical
    // for them.
    let ft = if !wasm && bits == 32
        && prog.imports.values().any(|n| matches!(n.as_str(),
            "TextOutA" | "TextOutW" | "ExtTextOutA" | "ExtTextOutW"
            | "DrawTextA" | "DrawTextW" | "TabbedTextOutA" | "TabbedTextOutW"
            | "GetTextExtentPoint32A" | "GetTextExtentPoint32W"
            | "GetTextExtentPointA" | "GetTextExtentPointW" | "GetTextMetricsA" | "GetTextMetricsW"
            | "GetTabbedTextExtentA" | "GetTabbedTextExtentW"
            // Per-character metrics (comctl32 socle): all share the DC-font FreeType path.
            | "GetTextExtentExPointA" | "GetTextExtentExPointW"
            | "GetCharWidthA" | "GetCharWidthW" | "GetCharABCWidthsW" | "GdiGetCharDimensions"
            // A program that creates fonts renders text — its native controls (BUTTON…)
            // paint their captions internally even if it never calls DrawText directly.
            | "CreateFontA" | "CreateFontW" | "CreateFontIndirectA" | "CreateFontIndirectW"
            // Enumerating the installed families reads them through fontconfig and
            // measures each face with FreeType (same stack as the text path).
            | "EnumFontFamiliesA" | "EnumFontFamiliesW"
            | "EnumFontFamiliesExA" | "EnumFontFamiliesExW"
            // A dialog maps dialog-units to pixels via its font's metrics (base units,
            // Wine's GdiGetCharDimensions) — needs FreeType to measure that font. This
            // covers both MapDialogRect and the dialog creators (a DS_SETFONT dialog
            // places its controls via those base units); base-unit computation is
            // best-effort (an unresolved font just leaves geometry unscaled, no abort).
            | "MapDialogRect"
            | "DialogBoxParamA" | "DialogBoxParamW"
            | "DialogBoxIndirectParamA" | "DialogBoxIndirectParamW"
            | "CreateDialogParamA" | "CreateDialogParamW"
            | "CreateDialogIndirectParamA" | "CreateDialogIndirectParamW"))
    {
        freetype_flags()
    } else {
        None
    };
    let ft_cflags: Vec<String> = ft.as_ref().map(|(c, _)| {
        let mut v = c.clone();
        v.push("-DARET_HAVE_FREETYPE".to_string());
        v
    }).unwrap_or_default();
    let ft_libs: Vec<String> = ft.as_ref().map(|(_, l)| l.clone()).unwrap_or_default();
    // Merge the optional feature flags: both are additive and empty when inactive.
    let feat_cflags: Vec<String> = sdl_cflags.iter().chain(ft_cflags.iter()).cloned().collect();
    let feat_libs: Vec<String> = sdl_libs.iter().chain(ft_libs.iter()).cloned().collect();

    let mut sources: Vec<std::path::PathBuf> = vec![
        out_dir.join("aret_hle.c"),
        out_dir.join("aret_crt.c"),
        out_dir.join("aret_win32.c"),
        out_dir.join("aret_ntdll.c"),
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
            let o = Command::new(&rt).arg(&binary).arg("--").args(prog_args).output()
                .with_context(|| format!("failed to run {rt} (set WASM_RUNTIME?)"))?;
            let mut s = String::from_utf8_lossy(&o.stdout).into_owned();
            if !o.stderr.is_empty() { s.push_str(&String::from_utf8_lossy(&o.stderr)); }
            Some(s)
        } else { None };
        return Ok(TranspileReport {
            out_dir: out_dir.to_path_buf(),
            binary,
            functions: n_funcs,
            lifted: n_lifted,
            partial: n_partial,
            host_backed: n_host,
            unresolved: undef_subs.clone(),
            unimplemented_imports: unimplemented_imports.clone(),
            unmodelled_insns: unmodelled_insns.clone(),
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
    // Flags shared by every C compile. Held in one place because the object cache
    // keys on them: a build with different flags must not reuse another's objects.
    let mut c_flags: Vec<String> = [march, "-w", "-fno-strict-aliasing", "-fno-builtin", "-fno-pie", "-O0", "-c"]
        .iter()
        .map(|s| s.to_string())
        .collect();
    // ARET_DEBUG=1 adds `-g` so the emitted C carries DWARF line info (gdb/addr2line
    // map a fault back to the exact generated-C statement). Off by default; `-g` does
    // not change `-O0` codegen, so the produced program is byte-identical (the object
    // cache keys on c_flags, so a debug build simply gets its own cache entries).
    if std::env::var_os("ARET_DEBUG").is_some() {
        c_flags.push("-g".to_string());
    }
    c_flags.extend(feat_cflags.iter().cloned());
    // Content-addressed object cache (doc 81 §I9). On a run that only changed an HLE
    // shim, every lifted-app object is bit-identical to the previous build — measured
    // 141 s of pure waste on WinMerge — and every winediff fixture re-compiles the
    // same three runtime files. Reuse is validated against the full `-MD` dependency
    // list, so a changed header always recompiles (see objcache.rs).
    let cache = objcache::ObjCache::open(&cc);
    let cache_hits = std::sync::atomic::AtomicUsize::new(0);
    use rayon::prelude::*;
    let objs: Result<Vec<std::path::PathBuf>> = sources
        .par_iter()
        .map(|src| {
            // Unique object name per source (so .c and .S of the same stem don't clash).
            let obj = out_dir.join(format!("{}.o", src.file_name().unwrap().to_string_lossy()));
            let ext = src.extension().and_then(|e| e.to_str());
            // LLVM IR chunks go through llc; C / asm through the C compiler.
            if ext == Some("ll") {
                let out = Command::new(&llc)
                    .args([&format!("-mtriple={triple}"), "-filetype=obj", "-O2", "-relocation-model=static"])
                    .arg(src)
                    .arg("-o")
                    .arg(&obj)
                    .output()
                    .with_context(|| format!("failed to run {}", llc))?;
                if !out.status.success() {
                    bail!("compile {} failed:\n{}", src.display(),
                          String::from_utf8_lossy(&out.stderr).trim());
                }
                return Ok(obj);
            }
            // Cached only for C (the `.S` is one tiny file, and llc has its own flags).
            let pending = if ext == Some("c") {
                cache.as_ref().and_then(|c| c.begin(&c_flags, src, out_dir).map(|p| (c, p)))
            } else {
                None
            };
            if let Some((c, p)) = &pending {
                if c.lookup(p, out_dir, &obj) {
                    cache_hits.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                    return Ok(obj);
                }
            }
            let mut cmd = Command::new(&cc);
            cmd.args(&c_flags)
                .arg(src)
                .arg("-I")
                .arg(out_dir)
                .arg("-o")
                .arg(&obj);
            if let Some((_, p)) = &pending {
                // Record what the preprocessor actually read, so a later lookup can
                // re-hash it instead of trusting the source alone.
                cmd.arg("-MD").arg("-MF").arg(&p.depfile);
            }
            let out = cmd.output().with_context(|| format!("failed to run {}", cc))?;
            if !out.status.success() {
                bail!(
                    "compile {} failed:\n{}",
                    src.display(),
                    String::from_utf8_lossy(&out.stderr).trim()
                );
            }
            if let Some((c, p)) = &pending {
                c.store(p, out_dir, &obj);
            }
            Ok(obj)
        })
        .collect();
    let mut objs = objs?;
    if let Some(c) = &cache {
        let hits = cache_hits.load(std::sync::atomic::Ordering::Relaxed);
        eprintln!("note: {} object(s) compiled, {hits} reused from cache", objs.len() - hits);
        c.trim();
    }

    // Heavy-form (doc 82): compile the whole Wine ntdll source UNCHANGED, plus its ASCII
    // floor, as SEPARATE objects with per-file flags the main loop can't carry — -fshort-wchar
    // (native wchar_t is 32-bit; Windows WCHAR is 16), the self-contained NT-types shim on -I,
    // and -D__WINESRC__. Native only (wasm returns earlier). Proven bit-identical Wine by
    // tools/wine_heavy/proof_native.sh; the aret_Rtl* adapters (aret_ntdll.c) route imports here.
    if !wasm && bits == 32 {
        let shim = out_dir.join("wine_heavy/native");
        for stem in ["rtlstr", "ntdll_floor", "ntdll_ntreg", "reg"] {
            let src = out_dir.join(format!("wine_heavy/{stem}.c"));
            let obj = out_dir.join(format!("wine_{stem}.o"));
            let out = Command::new(&cc)
                .args(["-m32", "-fshort-wchar", "-O0", "-w", "-fno-pie", "-fno-strict-aliasing", "-c", "-D__WINESRC__"])
                .arg("-I").arg(&shim)
                .arg(&src).arg("-o").arg(&obj)
                .output()
                .with_context(|| format!("failed to run {}", cc))?;
            if !out.status.success() {
                bail!("heavy-form compile {} failed:\n{}", src.display(),
                      String::from_utf8_lossy(&out.stderr).trim());
            }
            objs.push(obj);
        }
    }

    let link = Command::new(&cc)
        .args([march, "-no-pie"])
        .args(&objs)
        .arg("-lm") // the float helpers use sqrtf/sqrtl
        .args(&feat_libs) // SDL2 (G2b) + FreeType/fontconfig (G3-text); empty when unused
        .arg("-o")
        .arg(&binary)
        .output()
        .with_context(|| format!("failed to run {}", cc))?;
    if !link.status.success() {
        let err = String::from_utf8_lossy(&link.stderr);
        bail!("native link failed:\n{}", err.trim());
    }

    let run_output = if run {
        // Inherit the parent's stdin so a redirected/piped input (`aret --run <
        // file`, or a shell pipeline) reaches the transpiled program. `.output()`
        // otherwise hands the child a closed stdin → it reads immediate EOF, which
        // silently zeroed every stdin-reading program run this way.
        //
        // The child's stderr is *inherited* (flows to ARET's own stderr), NOT
        // captured into the framed program output. Merging it made the two streams
        // indistinguishable and broke the winediff oracle's symmetry: Wine's stderr
        // is discarded (`2>/dev/null`), so a program whose correct behaviour writes
        // to stderr (a failed `assert`, a diagnostic) looked like a divergence only
        // because ARET's copy landed in the compared stdout. Keeping stderr on fd2
        // lets the harness discard it identically for both engines. Only stdout —
        // the stream winediff actually compares — is captured and framed.
        let child = Command::new(&binary)
            .args(prog_args)
            .stdin(std::process::Stdio::inherit())
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::inherit())
            .spawn()
            .with_context(|| format!("failed to run {}", binary.display()))?;
        let out = child
            .wait_with_output()
            .with_context(|| format!("failed to run {}", binary.display()))?;
        Some(String::from_utf8_lossy(&out.stdout).into_owned())
    } else {
        None
    };

    Ok(TranspileReport {
        out_dir: out_dir.to_path_buf(),
        binary,
        functions: n_funcs,
        lifted: n_lifted,
        partial: n_partial,
        host_backed: n_host,
        unresolved: undef_subs.clone(),
        unimplemented_imports: unimplemented_imports.clone(),
        unmodelled_insns: unmodelled_insns.clone(),
        bits,
        run_output,
    })
}

#[cfg(test)]
mod snapshot_tests {
    use super::load_snapshot;
    use std::io::Write;

    #[test]
    fn loads_aretsnp1_regions() {
        let mut buf = Vec::new();
        buf.extend_from_slice(b"ARETSNP1");
        // region 1: va 0x401000, "code"
        buf.extend_from_slice(&0x401000u64.to_le_bytes());
        buf.extend_from_slice(&4u64.to_le_bytes());
        buf.extend_from_slice(b"code");
        // region 2: va 0x402000, "DATA!"
        buf.extend_from_slice(&0x402000u64.to_le_bytes());
        buf.extend_from_slice(&5u64.to_le_bytes());
        buf.extend_from_slice(b"DATA!");
        let path = std::env::temp_dir().join(format!("aret_snap_{}.snap", std::process::id()));
        std::fs::File::create(&path).unwrap().write_all(&buf).unwrap();
        let r = load_snapshot(&path).expect("load");
        assert_eq!(r.len(), 2);
        assert_eq!(r[0], (0x401000, b"code".to_vec()));
        assert_eq!(r[1], (0x402000, b"DATA!".to_vec()));
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn rejects_bad_magic() {
        let path = std::env::temp_dir().join(format!("aret_badsnap_{}.snap", std::process::id()));
        std::fs::write(&path, b"NOTASNAP").unwrap();
        assert!(load_snapshot(&path).is_err());
        let _ = std::fs::remove_file(&path);
    }
}
