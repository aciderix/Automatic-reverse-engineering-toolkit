//! Binary loader: turns a raw PE/ELF/Mach-O file into a uniform `Program`
//! description the rest of the pipeline can consume without caring about the
//! container format.

use anyhow::{bail, Context, Result};
use object::{Object, ObjectSection, ObjectSymbol, SectionKind, SymbolKind};
use std::collections::{BTreeMap, BTreeSet};

/// Pointer width of the target. Drives the disassembler bitness and the
/// pseudo-C integer widths.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Bitness {
    Bits32,
    Bits64,
}

impl Bitness {
    pub fn bits(self) -> u32 {
        match self {
            Bitness::Bits32 => 32,
            Bitness::Bits64 => 64,
        }
    }
}

/// A loadable section mapped at a virtual address.
#[derive(Debug, Clone)]
pub struct Section {
    pub name: String,
    /// Virtual address the section is mapped to at runtime.
    pub address: u64,
    /// Raw bytes of the section (already file-aligned by `object`).
    pub data: Vec<u8>,
    pub executable: bool,
    pub writable: bool,
}

impl Section {
    pub fn contains(&self, addr: u64) -> bool {
        addr >= self.address && addr < self.address + self.data.len() as u64
    }
}

/// A named location we know about up front (symbol, export, entry point).
#[derive(Debug, Clone)]
pub struct KnownSymbol {
    pub address: u64,
    pub name: String,
    pub is_function: bool,
}

/// Where a PE export points. Either a target inside this module (an address =
/// image_base + RVA, i.e. the lifted `sub_<va>`), or a forward to another DLL
/// (the multi-module loader must resolve it, or abort soundly — never guessed).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PeExportTarget {
    /// Virtual address in this module (image_base + RVA) — a lifted function.
    Address(u64),
    /// Forwarded to `dll`.`name` (e.g. `NTDLL.RtlAllocateHeap`).
    ForwardByName(String, String),
    /// Forwarded to `dll` #`ordinal`.
    ForwardByOrdinal(String, u32),
}

/// One entry of a PE DLL's Export Directory: what a caller of this DLL binds to.
/// This is the first brick of DLL lifting (doc 80 §1.2): to lift a DLL
/// (`comctl32`, `user32`, …) and expose its API surface, the multi-module
/// loader must know which lifted function each exported name/ordinal maps to.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PeExport {
    /// Export ordinal (already includes the DLL's ordinal base).
    pub ordinal: u32,
    /// Export name, if named (name-less exports are reachable by ordinal only).
    pub name: Option<String>,
    /// Where the export points.
    pub target: PeExportTarget,
}

/// A PE import as written in the importer, keeping the **source DLL**: which
/// module it comes from, and the symbol by name and/or ordinal. Distinct from
/// `Program::imports` (IAT slot → resolved shim name, which drops the module):
/// the multi-module loader needs the source DLL to bind an app's import to that
/// DLL's *lifted export* (doc 80 §1.2 brick 2) rather than to an HLE shim.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PeImport {
    /// Name of the DLL the symbol is imported from (e.g. `COMCTL32.dll`).
    pub dll: String,
    /// Import name, if imported by name.
    pub name: Option<String>,
    /// Import ordinal, if imported by ordinal (mutually informative with `name`).
    pub ordinal: Option<u32>,
}

/// C-runtime functions ARET provides natively (aret_hle/aret_crt). A
/// statically-linked call to one of these (recognized by symbol, see
/// `Program::crt_symbol`) is bound to the native shim instead of being lifted —
/// so a real full-CRT binary lifts only the user's code and uses the host
/// runtime for the rest.
const CRT_FUNCS: &[&str] = &[
    // <stdio.h>
    "printf", "fprintf", "sprintf", "snprintf", "puts", "putchar", "fputc",
    "fputs", "fgets", "fwrite", "fread", "fopen", "fclose", "fseek", "ftell",
    "fflush", "remove",
    // <stdlib.h>
    "malloc", "calloc", "realloc", "free", "atoi", "atol", "abs", "labs",
    "strtol", "strtoul", "rand", "srand", "getenv",
    // <string.h>
    "memcpy", "memmove", "memset", "memcmp", "memchr", "strlen", "strcmp",
    "strncmp", "strcpy", "strncpy", "strcat", "strncat", "strchr", "strrchr",
    "strstr", "strspn", "strcspn", "strpbrk", "strtok", "strdup", "strcoll",
    "strerror",
    // <ctype.h>
    "toupper", "tolower", "isalpha", "isdigit", "isalnum", "isspace", "isupper",
    "islower", "ispunct", "iscntrl", "isprint", "isgraph", "isxdigit",
    // <math.h> — transcendentals whose dense x87 bodies don't model; bound to
    // the host libm (the shims return through the x87 fp channel).
    "pow", "exp", "exp2", "expm1", "log", "log10", "log2", "log1p",
    "sin", "cos", "tan", "asin", "acos", "atan", "atan2",
    "sinh", "cosh", "tanh", "fmod", "hypot", "cbrt",
    // string→double (David Gay's bignum strtod doesn't lift; use the host).
    "strtod", "atof",
];

/// How many leading bytes to match a FLIRT signature against.
const FLIRT_WINDOW: usize = 32;

/// Is `name` (a possibly underscore-decorated symbol) a CRT function we shim?
fn is_crt_name(name: &str) -> bool {
    CRT_FUNCS.contains(&name.trim_start_matches('_'))
}

/// Is `name` a mingw/MSVC startup-glue function we bind to a no-op?
fn is_glue_name(name: &str) -> bool {
    name.ends_with("__main")
        || name.contains("do_global_ctors")
        || name.contains("do_global_dtors")
        || name.contains("register_frame")
        || name.contains("pei386_runtime_relocator")
}

/// Format-agnostic view of the loaded program.
pub struct Program {
    pub format: String,
    pub bitness: Bitness,
    pub entry: u64,
    pub sections: Vec<Section>,
    /// address -> symbol, sorted, used to name functions and resolve call targets.
    pub symbols: BTreeMap<u64, KnownSymbol>,
    /// PE IAT slot virtual address -> imported function name.
    pub imports: BTreeMap<u64, String>,
    /// PE IAT slot virtual address -> import keeping its **source DLL**
    /// (`PeImport{dll, name, ordinal}`). Used by the multi-module loader to bind
    /// a slot to another lifted module's export instead of an HLE shim.
    pub pe_imports: BTreeMap<u64, PeImport>,
    /// Static-relocation site address -> resolved target (object files). A `call`
    /// in a `.o` stores only a placeholder displacement until linked; this maps
    /// the displacement field back to the real target so recursive/cross-function
    /// calls decode to the right address (and external calls get a name).
    pub relocs: BTreeMap<u64, RelocEntry>,
    /// Virtual addresses covered by a PE base relocation (`.reloc`, HIGHLOW/DIR64)
    /// — the bytes of an absolute address the loader patches at a non-default base.
    /// These vary from binary to binary, so FLIRT signature generation wildcards
    /// them (an absolute operand like `mov reg,[abs32]` must not pin a signature).
    pub base_relocs: BTreeSet<u64>,
}

/// A resolved static relocation: the branch/data target address (when the symbol
/// is defined in this object), the referenced symbol's name, and — when the
/// target is read-only data — the first 8 bytes there, so rip-relative constant
/// loads can be folded to literals (read at parse time via the target's section,
/// which disambiguates the address collisions of a base-0 object file).
#[derive(Clone, Debug, Default)]
pub struct RelocEntry {
    pub target: Option<u64>,
    pub name: Option<String>,
    /// First 8 bytes of the read-only target (for folding scalar constants).
    pub data: Option<u64>,
    /// Next 8 bytes (for folding 128-bit vector constants).
    pub datahi: Option<u64>,
}

impl Program {
    /// Parse a binary from raw bytes.
    pub fn load(data: &[u8]) -> Result<Program> {
        let obj = object::File::parse(data).context("failed to parse binary container")?;

        let bitness = if obj.is_64() {
            Bitness::Bits64
        } else {
            Bitness::Bits32
        };

        let arch = obj.architecture();
        use object::Architecture::*;
        if !matches!(arch, I386 | X86_64) {
            bail!(
                "unsupported architecture {:?} — the disassembler currently targets x86/x86-64",
                arch
            );
        }

        let mut sections = Vec::new();
        for sec in obj.sections() {
            let kind = sec.kind();
            // Skip purely metadata sections that never get mapped.
            if matches!(kind, SectionKind::Metadata | SectionKind::Linker) {
                continue;
            }
            let data = match sec.uncompressed_data() {
                Ok(d) => d.into_owned(),
                Err(_) => continue,
            };
            if data.is_empty() {
                continue;
            }
            let executable = matches!(kind, SectionKind::Text);
            let writable = matches!(kind, SectionKind::Data | SectionKind::UninitializedData);
            sections.push(Section {
                name: sec.name().unwrap_or("<unnamed>").to_string(),
                address: sec.address(),
                data,
                executable,
                writable,
            });
        }

        // Map the PE headers at the image base too: the Windows CRT/startup reads
        // its own MZ/PE/section headers (e.g. `cmp word ptr [imagebase], 0x5a4d`),
        // which are not an `object` section, so without this they fault.
        if let Some(hdr) = pe_header_section(data) {
            if !sections.iter().any(|s| s.address == hdr.address) {
                sections.push(hdr);
            }
        }

        if sections.iter().all(|s| !s.executable) {
            // Some stripped PEs mislabel sections; fall back to flag-based detection
            // by trusting any section literally named .text/__text.
            for s in &mut sections {
                if s.name == ".text" || s.name == "__text" || s.name.starts_with(".text") {
                    s.executable = true;
                }
            }
        }

        let mut symbols = BTreeMap::new();
        for sym in obj.symbols().chain(obj.dynamic_symbols()) {
            let name = match sym.name() {
                Ok(n) if !n.is_empty() => n.to_string(),
                _ => continue,
            };
            let addr = sym.address();
            if addr == 0 {
                continue;
            }
            let is_function = sym.kind() == SymbolKind::Text;
            // In an object file every section is based at address 0, so a data
            // label (e.g. `.LC1` in .rodata) can share an address with a real
            // function in .text. Prefer the function symbol so seeding/naming use
            // it rather than the colliding data label.
            match symbols.entry(addr) {
                std::collections::btree_map::Entry::Vacant(e) => {
                    e.insert(KnownSymbol { address: addr, name, is_function });
                }
                std::collections::btree_map::Entry::Occupied(mut e) => {
                    if is_function && !e.get().is_function {
                        e.insert(KnownSymbol { address: addr, name, is_function });
                    }
                }
            }
        }

        let mut imports = parse_pe_imports(data);
        add_elf_imports(&obj, &sections, bitness, &mut imports);
        let pe_imports = parse_pe_imports_detailed(data);

        let relocs = parse_static_relocs(&obj);
        let base_relocs = parse_pe_base_relocs(data);

        Ok(Program {
            format: format!("{:?}", obj.format()),
            bitness,
            entry: obj.entry(),
            sections,
            symbols,
            imports,
            pe_imports,
            relocs,
            base_relocs,
        })
    }

    /// Resolved branch target of a relocation lying within the instruction at
    /// `[addr, addr+len)`, if any (PC-relative call/jump in an object file).
    pub fn reloc_branch_target(&self, addr: u64, len: usize) -> Option<u64> {
        self.relocs
            .range(addr..addr.saturating_add(len as u64))
            .find_map(|(_, r)| r.target)
    }

    /// The relocation entry (if any) covering the instruction at `[addr, addr+len)`.
    /// Presence means the instruction's displacement is a placeholder fixed up by
    /// the linker — its raw decoded target must not be trusted.
    pub fn reloc_in(&self, addr: u64, len: usize) -> Option<&RelocEntry> {
        self.relocs
            .range(addr..addr.saturating_add(len as u64))
            .map(|(_, r)| r)
            .next()
    }

    /// Name referenced by a relocation within `[addr, addr+len)` (for external
    /// calls in object files, whose target symbol is undefined here).
    pub fn reloc_name(&self, addr: u64, len: usize) -> Option<&str> {
        self.relocs
            .range(addr..addr.saturating_add(len as u64))
            .find_map(|(_, r)| r.name.as_deref())
    }

    /// Imported function name for a PE IAT slot address, if any.
    pub fn import_name(&self, addr: u64) -> Option<&str> {
        self.imports.get(&addr).map(|s| s.as_str())
    }

    /// Imported function name reached through an *import thunk* at `addr` — a
    /// one-instruction trampoline `jmp dword/qword ptr [IAT_slot]` the linker
    /// emits when an import is referenced by its plain (non-`__imp_`) name. A
    /// `call <thunk>` is therefore really a call to the import: resolving it here
    /// binds it directly to the shim, so the binding lands at the *real* call site
    /// rather than inside the throwaway thunk (essential for setjmp/longjmp, which
    /// must be expanded in the caller's own frame).
    pub fn import_thunk(&self, addr: u64) -> Option<&str> {
        use iced_x86::{Decoder, DecoderOptions, Mnemonic, OpKind};
        if self.imports.is_empty() {
            return None;
        }
        let code = self.code_at(addr, 8)?;
        let bits = self.bitness.bits() as u32;
        let mut dec = Decoder::with_ip(bits, code, addr, DecoderOptions::NONE);
        let insn = dec.decode();
        if insn.mnemonic() != Mnemonic::Jmp || insn.op0_kind() != OpKind::Memory {
            return None;
        }
        // Absolute IAT slot the jmp dereferences: `[abs32]` (32-bit) or rip-relative
        // (64-bit). Both surface through the memory-displacement absolute address.
        let slot = if insn.is_ip_rel_memory_operand() {
            insn.ip_rel_memory_address()
        } else if insn.memory_base() == iced_x86::Register::None
            && insn.memory_index() == iced_x86::Register::None
        {
            insn.memory_displacement64()
        } else {
            return None; // indexed/based: a jump table, not an import thunk
        };
        self.imports.get(&slot).map(|s| s.as_str())
    }

    /// Recognize a *statically-linked* C-runtime function by its symbol, so a
    /// call to it can be bound to the native HLE shim instead of lifting the
    /// CRT's own (often indirect-call-heavy) implementation. Returns the symbol
    /// name when `addr` is a function symbol whose base name (minus a leading
    /// `_`) is a CRT function ARET provides natively (aret_crt/aret_hle). This is
    /// symbol-based library recognition — the cheap form of IDA FLIRT, the lever
    /// for real full-CRT binaries (lift the user's code, link the real runtime).
    pub fn crt_symbol(&self, addr: u64) -> Option<&str> {
        // Symbol table is authoritative when present.
        if let Some(s) = self.symbols.get(&addr) {
            if s.is_function {
                return if is_crt_name(&s.name) { Some(s.name.as_str()) } else { None };
            }
        }
        // Stripped binary: recognize by FLIRT-lite signature instead.
        let code = self.code_at(addr, FLIRT_WINDOW)?;
        let name = crate::flirt::bundled().match_at(code)?;
        if is_crt_name(name) { Some(name) } else { None }
    }

    /// Leading bytes of the code at `addr` (for signature matching).
    fn code_at(&self, addr: u64, len: usize) -> Option<&[u8]> {
        let sec = self.section_at(addr)?;
        let off = (addr.checked_sub(sec.address)?) as usize;
        if off >= sec.data.len() {
            return None;
        }
        Some(&sec.data[off..(off + len).min(sec.data.len())])
    }

    /// Recognize a mingw/MSVC *startup-glue* function — the global ctor/dtor
    /// runners, EH-frame registration, and pseudo-relocator the CRT startup runs
    /// before/around `main`. These walk compiler-built tables by indirect calls
    /// that static recovery cannot fully resolve; binding them to a native no-op
    /// lets the user's `main` run. (Honest cost: C++ global constructors are not
    /// executed — a documented limitation, not a crash.)
    pub fn is_startup_glue(&self, addr: u64) -> bool {
        if let Some(s) = self.symbols.get(&addr) {
            if s.is_function {
                return is_glue_name(&s.name);
            }
        }
        // Stripped: FLIRT-lite signature.
        self.code_at(addr, FLIRT_WINDOW)
            .and_then(|c| crate::flirt::bundled().match_at(c))
            .map(is_glue_name)
            .unwrap_or(false)
    }

    /// Return the section containing `addr`, if any.
    pub fn section_at(&self, addr: u64) -> Option<&Section> {
        self.sections.iter().find(|s| s.contains(addr))
    }

    /// Read a slice of program memory starting at virtual address `addr`.
    /// Returns the longest available contiguous slice within the section.
    pub fn read_from(&self, addr: u64) -> Option<&[u8]> {
        let sec = self.section_at(addr)?;
        let off = (addr - sec.address) as usize;
        Some(&sec.data[off..])
    }

    /// Whether `addr` falls inside an executable section.
    pub fn is_executable(&self, addr: u64) -> bool {
        self.section_at(addr).map(|s| s.executable).unwrap_or(false)
    }

    /// Read a little-endian u32 from program memory.
    pub fn read_u32(&self, addr: u64) -> Option<u32> {
        let b = self.read_from(addr)?;
        if b.len() < 4 {
            return None;
        }
        Some(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }

    /// Read a little-endian u64 from program memory.
    pub fn read_u64(&self, addr: u64) -> Option<u64> {
        let b = self.read_from(addr)?;
        if b.len() < 8 {
            return None;
        }
        let mut a = [0u8; 8];
        a.copy_from_slice(&b[..8]);
        Some(u64::from_le_bytes(a))
    }

    /// If `addr` points to a printable NUL-terminated string in a read-only
    /// section, return it. Used to annotate string-literal references.
    pub fn read_cstring(&self, addr: u64) -> Option<String> {
        let sec = self.section_at(addr)?;
        if sec.writable || sec.executable {
            return None; // strings live in read-only data (.rdata/.rodata)
        }
        let off = (addr - sec.address) as usize;
        let bytes = sec.data.get(off..)?;
        let mut s = String::new();
        for &b in bytes.iter().take(512) {
            if b == 0 {
                return if s.len() >= 2 { Some(s) } else { None };
            }
            match b {
                b' '..=b'~' => s.push(b as char),
                b'\t' => s.push_str("\\t"),
                b'\n' => s.push_str("\\n"),
                b'\r' => s.push_str("\\r"),
                _ => return None, // non-printable -> not a clean C string
            }
        }
        None
    }

    /// All addresses we have a reason to treat as function entry points.
    pub fn seed_functions(&self) -> Vec<u64> {
        let mut seeds = Vec::new();
        if self.is_executable(self.entry) {
            seeds.push(self.entry);
        }
        for sym in self.symbols.values() {
            if sym.is_function && self.is_executable(sym.address) {
                seeds.push(sym.address);
            }
        }
        seeds.sort_unstable();
        seeds.dedup();
        seeds
    }

    pub fn symbol_name(&self, addr: u64) -> Option<&str> {
        self.symbols.get(&addr).map(|s| s.name.as_str())
    }

    /// Scan executable sections for common function prologues, to recover
    /// functions that are only reached indirectly (vtables, callbacks) and so
    /// never appear as a direct call target. This is a heuristic: false
    /// positives just produce extra `sub_*` functions, they do not corrupt
    /// the ones found by recursive descent.
    pub fn prologue_seeds(&self) -> Vec<u64> {
        // 32-bit: push ebp; mov ebp, esp        => 55 8B EC
        // 64-bit: push rbp; mov rbp, rsp        => 55 48 89 E5
        let mut seeds = Vec::new();
        for sec in &self.sections {
            if !sec.executable {
                continue;
            }
            let d = &sec.data;
            match self.bitness {
                Bitness::Bits32 => {
                    for i in 0..d.len().saturating_sub(2) {
                        if d[i] == 0x55 && d[i + 1] == 0x8b && d[i + 2] == 0xec {
                            seeds.push(sec.address + i as u64);
                        }
                    }
                }
                Bitness::Bits64 => {
                    for i in 0..d.len().saturating_sub(3) {
                        if d[i] == 0x55 && d[i + 1] == 0x48 && d[i + 2] == 0x89 && d[i + 3] == 0xe5
                        {
                            seeds.push(sec.address + i as u64);
                        }
                    }
                }
            }
        }
        seeds
    }
}

/// A synthetic section holding the PE headers (DOS + NT + section table) mapped
/// at the image base, so transpiled code that reads its own headers works.
fn pe_header_section(data: &[u8]) -> Option<Section> {
    use object::read::pe::{ImageNtHeaders, ImageOptionalHeader};
    use object::pe;

    fn build<Nt: ImageNtHeaders>(data: &[u8]) -> Option<Section> {
        let dos = pe::ImageDosHeader::parse(data).ok()?;
        let mut offset = dos.nt_headers_offset() as u64;
        let (nt, _dirs) = Nt::parse(data, &mut offset).ok()?;
        let oh = nt.optional_header();
        let base = oh.image_base();
        let soh = oh.size_of_headers() as usize;
        let n = soh.clamp(1, data.len());
        Some(Section {
            name: ".pe_header".to_string(),
            address: base,
            data: data[..n].to_vec(),
            executable: false,
            writable: false,
        })
    }

    build::<pe::ImageNtHeaders32>(data).or_else(|| build::<pe::ImageNtHeaders64>(data))
}

/// Parse a PE base-relocation table (`.reloc`) into the set of virtual addresses
/// whose bytes hold an absolute address the loader patches (HIGHLOW = 4 bytes,
/// DIR64 = 8). Best-effort: empty for non-PE, a stripped `.reloc`, or any parse
/// error. Used only to wildcard those bytes in FLIRT signatures (they differ
/// between binaries), so a false-empty just falls back to the prior behaviour.
fn parse_pe_base_relocs(data: &[u8]) -> BTreeSet<u64> {
    use object::read::pe::{ImageNtHeaders, ImageOptionalHeader};
    use object::pe;

    fn collect<Nt: ImageNtHeaders>(data: &[u8]) -> Option<BTreeSet<u64>> {
        let dos = pe::ImageDosHeader::parse(data).ok()?;
        let mut offset = dos.nt_headers_offset() as u64;
        let (nt, dirs) = Nt::parse(data, &mut offset).ok()?;
        let sections = nt.sections(data, offset).ok()?;
        let base = nt.optional_header().image_base();
        let mut set = BTreeSet::new();
        let mut blocks = match dirs.relocation_blocks(data, &sections) {
            Ok(Some(b)) => b,
            _ => return Some(set),
        };
        while let Ok(Some(block)) = blocks.next() {
            for reloc in block {
                // HIGHLOW (3) = 32-bit absolute; DIR64 (10) = 64-bit absolute.
                // ABSOLUTE (0) is padding. Record the covered VA(s) either way.
                let va = base + reloc.virtual_address as u64;
                if reloc.typ == pe::IMAGE_REL_BASED_HIGHLOW
                    || reloc.typ == pe::IMAGE_REL_BASED_DIR64
                {
                    set.insert(va);
                }
            }
        }
        Some(set)
    }

    collect::<pe::ImageNtHeaders32>(data)
        .or_else(|| collect::<pe::ImageNtHeaders64>(data))
        .unwrap_or_default()
}

/// Parse a PE import table into a map of IAT slot virtual address -> imported
/// name. Returns empty for non-PE inputs or on any parse error (best-effort).
fn parse_pe_imports(data: &[u8]) -> BTreeMap<u64, String> {
    use object::read::pe::{ImageNtHeaders, ImageOptionalHeader, ImageThunkData};
    use object::{pe, LittleEndian as LE};

    fn collect<Nt: ImageNtHeaders>(data: &[u8], ptr: u64) -> Option<BTreeMap<u64, String>> {
        let dos = pe::ImageDosHeader::parse(data).ok()?;
        let mut offset = dos.nt_headers_offset() as u64;
        let (nt, dirs) = Nt::parse(data, &mut offset).ok()?;
        let sections = nt.sections(data, offset).ok()?;
        let base = nt.optional_header().image_base();
        let mut map = BTreeMap::new();
        let it = match dirs.import_table(data, &sections) {
            Ok(Some(it)) => it,
            _ => return Some(map),
        };
        let mut descs = it.descriptors().ok()?;
        while let Ok(Some(desc)) = descs.next() {
            let iat = desc.first_thunk.get(LE); // IAT slots (addresses at runtime)
            let int = desc.original_first_thunk.get(LE); // names (import name table)
            // Names come from the INT when present, else the IAT.
            let name_rva = if int != 0 { int } else { iat };
            let mut thunks = match it.thunks(name_rva) {
                Ok(t) => t,
                Err(_) => continue,
            };
            let mut k = 0u64;
            while let Ok(Some(thunk)) = thunks.next::<Nt>() {
                let addr = base + iat as u64 + k * ptr; // the IAT slot a `call` targets
                k += 1;
                if thunk.is_ordinal() {
                    // Import by ordinal (no name in the importer): resolve it to the
                    // export name via the ground-truth (dll, ordinal) map so the normal
                    // name-based shim routing applies (e.g. COMCTL32 #17 =
                    // InitCommonControls). Unknown -> left unresolved (sound abort on use).
                    if let Ok(dll) = it.name(desc.name.get(LE)) {
                        let dll = String::from_utf8_lossy(dll).into_owned();
                        if let Some(n) =
                            crate::ir::ordinal_imports::ordinal_import_name(&dll, thunk.ordinal())
                        {
                            map.insert(addr, n.to_string());
                        }
                    }
                    continue;
                }
                if let Ok((_hint, name)) = it.hint_name(thunk.address()) {
                    let n = String::from_utf8_lossy(name).into_owned();
                    if !n.is_empty() {
                        map.insert(addr, n);
                    }
                }
            }
        }
        Some(map)
    }

    collect::<pe::ImageNtHeaders32>(data, 4)
        .filter(|m| !m.is_empty())
        .or_else(|| collect::<pe::ImageNtHeaders64>(data, 8))
        .unwrap_or_default()
}

/// Parse a PE's import table keeping the **source DLL** of each import: IAT slot
/// virtual address → `PeImport{dll, name, ordinal}`. Parallel to
/// `parse_pe_imports` (which resolves to a shim name and drops the module) —
/// here the module is preserved so the multi-module loader can bind the slot to
/// another lifted module's export. Brick 2 of DLL lifting (doc 80 §1.2).
fn parse_pe_imports_detailed(data: &[u8]) -> BTreeMap<u64, PeImport> {
    use object::read::pe::{ImageNtHeaders, ImageOptionalHeader, ImageThunkData};
    use object::{pe, LittleEndian as LE};

    fn collect<Nt: ImageNtHeaders>(data: &[u8], ptr: u64) -> Option<BTreeMap<u64, PeImport>> {
        let dos = pe::ImageDosHeader::parse(data).ok()?;
        let mut offset = dos.nt_headers_offset() as u64;
        let (nt, dirs) = Nt::parse(data, &mut offset).ok()?;
        let sections = nt.sections(data, offset).ok()?;
        let base = nt.optional_header().image_base();
        let mut map = BTreeMap::new();
        let it = match dirs.import_table(data, &sections) {
            Ok(Some(it)) => it,
            _ => return Some(map),
        };
        let mut descs = it.descriptors().ok()?;
        while let Ok(Some(desc)) = descs.next() {
            let iat = desc.first_thunk.get(LE);
            let int = desc.original_first_thunk.get(LE);
            let name_rva = if int != 0 { int } else { iat };
            let dll = match it.name(desc.name.get(LE)) {
                Ok(d) => String::from_utf8_lossy(d).into_owned(),
                Err(_) => continue,
            };
            let mut thunks = match it.thunks(name_rva) {
                Ok(t) => t,
                Err(_) => continue,
            };
            let mut k = 0u64;
            while let Ok(Some(thunk)) = thunks.next::<Nt>() {
                let addr = base + iat as u64 + k * ptr;
                k += 1;
                if thunk.is_ordinal() {
                    map.insert(
                        addr,
                        PeImport {
                            dll: dll.clone(),
                            name: None,
                            ordinal: Some(thunk.ordinal() as u32),
                        },
                    );
                } else if let Ok((_hint, name)) = it.hint_name(thunk.address()) {
                    let n = String::from_utf8_lossy(name).into_owned();
                    if !n.is_empty() {
                        map.insert(
                            addr,
                            PeImport { dll: dll.clone(), name: Some(n), ordinal: None },
                        );
                    }
                }
            }
        }
        Some(map)
    }

    collect::<pe::ImageNtHeaders32>(data, 4)
        .filter(|m| !m.is_empty())
        .or_else(|| collect::<pe::ImageNtHeaders64>(data, 8))
        .unwrap_or_default()
}

/// Parse a PE DLL's Export Directory: the ordered list of exports (name and/or
/// ordinal → target). A local target is resolved to a virtual address
/// (`image_base + RVA`), i.e. the address of the lifted `sub_<va>`; a forwarded
/// export is recorded verbatim (`kernel32.HeapAlloc` etc.) for the multi-module
/// loader to resolve later — never guessed. Empty EAT slots (address 0, the
/// gaps in a sparse ordinal range) are skipped: RVA 0 is the DOS header, never
/// a valid export. Brick 1 of DLL lifting (doc 80 §1.2).
pub fn parse_pe_exports(data: &[u8]) -> Vec<PeExport> {
    use object::read::pe::{ExportTarget, ImageNtHeaders, ImageOptionalHeader};
    use object::pe;

    fn collect<Nt: ImageNtHeaders>(data: &[u8]) -> Option<Vec<PeExport>> {
        let dos = pe::ImageDosHeader::parse(data).ok()?;
        let mut offset = dos.nt_headers_offset() as u64;
        let (nt, dirs) = Nt::parse(data, &mut offset).ok()?;
        let sections = nt.sections(data, offset).ok()?;
        let base = nt.optional_header().image_base();
        let table = match dirs.export_table(data, &sections) {
            Ok(Some(t)) => t,
            _ => return Some(Vec::new()),
        };
        let exports = table.exports().ok()?;
        let mut out = Vec::with_capacity(exports.len());
        for e in exports {
            let target = match e.target {
                // An EAT entry of RVA 0 is a hole in the sparse ordinal range,
                // not an export — skip it (RVA 0 = DOS header, never valid).
                ExportTarget::Address(0) => continue,
                ExportTarget::Address(rva) => PeExportTarget::Address(base + rva as u64),
                ExportTarget::ForwardByName(dll, name) => PeExportTarget::ForwardByName(
                    String::from_utf8_lossy(dll).into_owned(),
                    String::from_utf8_lossy(name).into_owned(),
                ),
                ExportTarget::ForwardByOrdinal(dll, ord) => {
                    PeExportTarget::ForwardByOrdinal(String::from_utf8_lossy(dll).into_owned(), ord)
                }
            };
            out.push(PeExport {
                ordinal: e.ordinal,
                name: e.name.map(|n| String::from_utf8_lossy(n).into_owned()),
                target,
            });
        }
        Some(out)
    }

    collect::<pe::ImageNtHeaders32>(data)
        .filter(|v| !v.is_empty())
        .or_else(|| collect::<pe::ImageNtHeaders64>(data))
        .unwrap_or_default()
}

/// Parse static relocations (object-file `.rela.text` etc.). For each
/// relocation, resolve the referenced symbol to its address (when defined in
/// this object) and name. PC-relative 4-byte relocations (the `call`/`jmp`/
/// `jcc rel32` and rip-relative `lea`/`mov` of object files) have their target
/// computed as `symbol + addend + 4`, because the displacement field ends the
/// instruction; that is the address the branch actually goes to once linked.
fn parse_static_relocs(obj: &object::File) -> BTreeMap<u64, RelocEntry> {
    use object::{Object, ObjectSection, ObjectSymbol, RelocationKind, RelocationTarget};
    let mut out: BTreeMap<u64, RelocEntry> = BTreeMap::new();
    for sec in obj.sections() {
        // Only code-section relocations are consumed (to fix call/jump targets).
        // In an object file every section is based at address 0, so including
        // data sections' relocations would collide with .text addresses and
        // falsely override intra-function branches.
        if sec.kind() != SectionKind::Text {
            continue;
        }
        let base = sec.address();
        for (off, rel) in sec.relocations() {
            let site = base + off;
            // Resolve the target's address, name, and its section (to read
            // read-only constants directly, sidestepping base-0 address collisions).
            let (sym_addr, name, tsec) = match rel.target() {
                RelocationTarget::Symbol(idx) => match obj.symbol_by_index(idx) {
                    Ok(sym) => {
                        let nm = sym.name().ok().filter(|n| !n.is_empty()).map(str::to_string);
                        // A *defined* symbol has a section (even at offset 0 in a
                        // base-0 object); an undefined external (libc) has none.
                        match sym.section() {
                            object::SymbolSection::Section(s) => {
                                (Some(sym.address()), nm, obj.section_by_index(s).ok())
                            }
                            _ => (None, nm, None),
                        }
                    }
                    Err(_) => (None, None, None),
                },
                RelocationTarget::Section(idx) => {
                    let sec = obj.section_by_index(idx).ok();
                    (sec.as_ref().map(|s| s.address()), None, sec)
                }
                _ => (None, None, None),
            };
            // PC-relative 4-byte field ends the instruction: target = S + A + 4.
            let pcrel = matches!(rel.kind(), RelocationKind::Relative | RelocationKind::PltRelative);
            let target = match (sym_addr, pcrel) {
                (Some(a), true) => Some(a.wrapping_add(rel.addend() as u64).wrapping_add(4)),
                (Some(a), false) => Some(a.wrapping_add(rel.addend() as u64)),
                (None, _) => None,
            };
            // If the target is read-only data, capture the first 16 bytes so a
            // rip-relative scalar (8) or vector (16) constant load can be folded.
            let (data, datahi) = match (target, &tsec) {
                (Some(t), Some(s))
                    if matches!(
                        s.kind(),
                        SectionKind::ReadOnlyData | SectionKind::ReadOnlyString
                    ) =>
                {
                    s.data()
                        .ok()
                        .and_then(|d| {
                            let o = t.checked_sub(s.address())? as usize;
                            let avail = d.get(o..)?;
                            let rd = |start: usize| {
                                let mut buf = [0u8; 8];
                                let n = avail.get(start..)?.len().min(8);
                                if n == 0 {
                                    return None;
                                }
                                buf[..n].copy_from_slice(&avail[start..start + n]);
                                Some(u64::from_le_bytes(buf))
                            };
                            Some((rd(0), rd(8)))
                        })
                        .unwrap_or((None, None))
                }
                _ => (None, None),
            };
            out.insert(site, RelocEntry { target, name, data, datahi });
        }
    }
    out
}

/// Resolve ELF imports: map GOT slots (for `call [GOT]`) and PLT stub entries
/// (for `call plt_stub`) to their imported symbol names, so libc/external calls
/// get named. GOT names come from the dynamic relocations; PLT stub addresses
/// are found by decoding the `.plt*` sections' indirect jumps to the GOT.
fn add_elf_imports(
    obj: &object::File,
    sections: &[Section],
    bitness: Bitness,
    imports: &mut BTreeMap<u64, String>,
) {
    use iced_x86::{Decoder, DecoderOptions, Instruction, Mnemonic, OpKind, Register};
    use object::{Object, ObjectSymbol, RelocationTarget};

    if obj.format() != object::BinaryFormat::Elf {
        return;
    }

    // Dynamic-relocation symbol indices refer to the *dynamic* symbol table
    // (the static table is often stripped), so resolve names through it.
    let dynsym: std::collections::HashMap<usize, String> = obj
        .dynamic_symbols()
        .filter_map(|s| s.name().ok().map(|n| (s.index().0, n.to_string())))
        .collect();

    // GOT slot address -> imported symbol name (from JUMP_SLOT/GLOB_DAT relocs).
    let mut got: BTreeMap<u64, String> = BTreeMap::new();
    if let Some(relocs) = obj.dynamic_relocations() {
        for (addr, rel) in relocs {
            if let RelocationTarget::Symbol(idx) = rel.target() {
                if let Some(name) = dynsym.get(&idx.0) {
                    if !name.is_empty() {
                        got.insert(addr, name.clone());
                        imports.insert(addr, name.clone()); // call [GOT]
                    }
                }
            }
        }
    }
    if got.is_empty() {
        return;
    }

    // Decode each .plt* section; an indirect jump to a named GOT slot identifies
    // the 16-byte stub whose start address callers `call`.
    let bits = bitness.bits();
    for sec in sections {
        if !sec.name.starts_with(".plt") {
            continue;
        }
        let mut dec = Decoder::with_ip(bits, &sec.data, sec.address, DecoderOptions::NONE);
        let mut insn = Instruction::default();
        while dec.can_decode() {
            dec.decode_out(&mut insn);
            if insn.mnemonic() != Mnemonic::Jmp || insn.op0_kind() != OpKind::Memory {
                continue;
            }
            let target = if insn.is_ip_rel_memory_operand() {
                insn.ip_rel_memory_address()
            } else if insn.memory_base() == Register::None && insn.memory_index() == Register::None
            {
                insn.memory_displacement64()
            } else {
                continue;
            };
            if let Some(name) = got.get(&target) {
                let stub = sec.address + ((insn.ip() - sec.address) / 16) * 16;
                imports.insert(stub, name.clone());
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Hand-craft a minimal but valid PE32 DLL whose Export Directory has:
    ///   - ordinal base 5;
    ///   - ord 5 "Alpha"  -> local RVA 0x2000 (named);
    ///   - ord 6 (no name) -> local RVA 0x2100 (ordinal-only);
    ///   - ord 7          -> EAT hole (address 0, must be skipped);
    ///   - ord 8 "Gamma"  -> forward "OTHER.Delta".
    /// Every RVA referenced (dir, tables, strings) lives inside the export data
    /// directory range, so `object` slices the whole blob self-containedly.
    fn craft_export_dll() -> Vec<u8> {
        const IMAGE_BASE: u32 = 0x1000_0000;
        const SEC_VA: u32 = 0x1000; // export blob RVA
        const RAW_PTR: u32 = 0x200; // export blob file offset

        // --- Export blob (based at RVA SEC_VA) ------------------------------
        // Fixed inner offsets:
        let eat_rva = SEC_VA + 0x28; // after the 40-byte directory
        let ent_rva = SEC_VA + 0x38; // after 4 EAT entries (16 bytes)
        let ord_rva = SEC_VA + 0x40; // after 2 ENT entries (8 bytes)
        let str_rva = SEC_VA + 0x44; // after 2 ordinal entries (4 bytes)
        // String RVAs (packed from str_rva):
        let dll_rva = str_rva; // "MYDLL.dll\0"  (10)
        let alpha_rva = dll_rva + 10; // "Alpha\0"      (6)
        let gamma_rva = alpha_rva + 6; // "Gamma\0"      (6)
        let fwd_rva = gamma_rva + 6; // "OTHER.Delta\0" (12)
        let blob_end = fwd_rva + 12;
        let blob_len = (blob_end - SEC_VA) as usize;

        let mut blob = vec![0u8; blob_len];
        let put32 = |b: &mut [u8], rva: u32, v: u32| {
            let o = (rva - SEC_VA) as usize;
            b[o..o + 4].copy_from_slice(&v.to_le_bytes());
        };
        let put16 = |b: &mut [u8], rva: u32, v: u16| {
            let o = (rva - SEC_VA) as usize;
            b[o..o + 2].copy_from_slice(&v.to_le_bytes());
        };
        let puts = |b: &mut [u8], rva: u32, s: &[u8]| {
            let o = (rva - SEC_VA) as usize;
            b[o..o + s.len()].copy_from_slice(s);
        };
        // IMAGE_EXPORT_DIRECTORY (40 bytes) at SEC_VA.
        put32(&mut blob, SEC_VA + 0x0c, dll_rva); // Name
        put32(&mut blob, SEC_VA + 0x10, 5); // Base (ordinal base)
        put32(&mut blob, SEC_VA + 0x14, 4); // NumberOfFunctions
        put32(&mut blob, SEC_VA + 0x18, 2); // NumberOfNames
        put32(&mut blob, SEC_VA + 0x1c, eat_rva); // AddressOfFunctions
        put32(&mut blob, SEC_VA + 0x20, ent_rva); // AddressOfNames
        put32(&mut blob, SEC_VA + 0x24, ord_rva); // AddressOfNameOrdinals
        // EAT: idx0 local, idx1 local(no name), idx2 hole, idx3 forward.
        put32(&mut blob, eat_rva, 0x2000);
        put32(&mut blob, eat_rva + 4, 0x2100);
        put32(&mut blob, eat_rva + 8, 0);
        put32(&mut blob, eat_rva + 12, fwd_rva);
        // ENT (name pointers) + ordinal table (0-based EAT indices).
        put32(&mut blob, ent_rva, alpha_rva);
        put32(&mut blob, ent_rva + 4, gamma_rva);
        put16(&mut blob, ord_rva, 0); // "Alpha" -> EAT idx0
        put16(&mut blob, ord_rva + 2, 3); // "Gamma" -> EAT idx3
        // Strings.
        puts(&mut blob, dll_rva, b"MYDLL.dll\0");
        puts(&mut blob, alpha_rva, b"Alpha\0");
        puts(&mut blob, gamma_rva, b"Gamma\0");
        puts(&mut blob, fwd_rva, b"OTHER.Delta\0");

        // --- Headers --------------------------------------------------------
        let mut f = vec![0u8; RAW_PTR as usize];
        f[0] = b'M';
        f[1] = b'Z';
        f[0x3c..0x40].copy_from_slice(&0x40u32.to_le_bytes()); // e_lfanew
        let pe = 0x40usize;
        f[pe..pe + 4].copy_from_slice(b"PE\0\0");
        // FileHeader @ 0x44
        let fh = pe + 4;
        f[fh..fh + 2].copy_from_slice(&0x014cu16.to_le_bytes()); // Machine i386
        f[fh + 2..fh + 4].copy_from_slice(&1u16.to_le_bytes()); // NumberOfSections
        f[fh + 16..fh + 18].copy_from_slice(&0xE0u16.to_le_bytes()); // SizeOfOptionalHeader
        f[fh + 18..fh + 20].copy_from_slice(&0x2102u16.to_le_bytes()); // DLL | 32-bit
        // OptionalHeader32 @ 0x58
        let oh = fh + 20;
        f[oh..oh + 2].copy_from_slice(&0x010bu16.to_le_bytes()); // PE32 magic
        f[oh + 28..oh + 32].copy_from_slice(&IMAGE_BASE.to_le_bytes()); // ImageBase
        f[oh + 32..oh + 36].copy_from_slice(&0x1000u32.to_le_bytes()); // SectionAlignment
        f[oh + 36..oh + 40].copy_from_slice(&0x200u32.to_le_bytes()); // FileAlignment
        f[oh + 56..oh + 60].copy_from_slice(&0x3000u32.to_le_bytes()); // SizeOfImage
        f[oh + 60..oh + 64].copy_from_slice(&RAW_PTR.to_le_bytes()); // SizeOfHeaders
        f[oh + 68..oh + 70].copy_from_slice(&2u16.to_le_bytes()); // Subsystem GUI
        f[oh + 92..oh + 96].copy_from_slice(&16u32.to_le_bytes()); // NumberOfRvaAndSizes
        // DataDirectory[0] = export table @ oh+96
        let dd = oh + 96;
        f[dd..dd + 4].copy_from_slice(&SEC_VA.to_le_bytes());
        f[dd + 4..dd + 8].copy_from_slice(&(blob_len as u32).to_le_bytes());
        // Section header @ oh+224
        let sh = oh + 224;
        f[sh..sh + 6].copy_from_slice(b".rdata");
        f[sh + 8..sh + 12].copy_from_slice(&(blob_len as u32).to_le_bytes()); // VirtualSize
        f[sh + 12..sh + 16].copy_from_slice(&SEC_VA.to_le_bytes()); // VirtualAddress
        f[sh + 16..sh + 20].copy_from_slice(&0x200u32.to_le_bytes()); // SizeOfRawData
        f[sh + 20..sh + 24].copy_from_slice(&RAW_PTR.to_le_bytes()); // PointerToRawData
        f[sh + 36..sh + 40].copy_from_slice(&0x4000_0040u32.to_le_bytes()); // init data, read

        // Append the export blob padded to FileAlignment.
        f.resize(RAW_PTR as usize + 0x200, 0);
        f[RAW_PTR as usize..RAW_PTR as usize + blob.len()].copy_from_slice(&blob);
        f
    }

    #[test]
    fn parse_pe_exports_reads_the_export_directory() {
        let dll = craft_export_dll();
        let exports = parse_pe_exports(&dll);
        // The EAT hole (ordinal 7) is skipped; the rest come back in EAT order.
        assert_eq!(
            exports,
            vec![
                PeExport {
                    ordinal: 5,
                    name: Some("Alpha".into()),
                    target: PeExportTarget::Address(0x1000_2000),
                },
                PeExport {
                    ordinal: 6,
                    name: None,
                    target: PeExportTarget::Address(0x1000_2100),
                },
                PeExport {
                    ordinal: 8,
                    name: Some("Gamma".into()),
                    target: PeExportTarget::ForwardByName("OTHER".into(), "Delta".into()),
                },
            ]
        );
    }

    #[test]
    fn parse_pe_exports_empty_on_non_pe() {
        assert!(parse_pe_exports(b"not a pe file at all").is_empty());
    }

    /// Measured against a real system DLL: Wine's own `comctl32.dll` (a genuine
    /// PE32, and a real Levier-1 target). Gated on Wine's PE builtin being
    /// present (like winediff needs Wine) — skips otherwise. Asserts only
    /// version-robust, ABI-stable invariants (verified bit-exact vs
    /// `objdump -p`: 191 exports = 160 address + 31 forward, 126 named), never
    /// brittle version-specific magic counts.
    #[test]
    fn parse_pe_exports_matches_wine_comctl32() {
        let candidates = [
            "/usr/lib/i386-linux-gnu/wine/i386-windows/comctl32.dll",
            "/usr/lib/wine/i386-windows/comctl32.dll",
            "/opt/wine-stable/lib/wine/i386-windows/comctl32.dll",
        ];
        let Some(path) = candidates.iter().find(|p| std::path::Path::new(p).exists()) else {
            return; // no Wine PE builtin here — skip (measurement-only test)
        };
        let data = std::fs::read(path).unwrap();
        let exports = parse_pe_exports(&data);
        assert!(!exports.is_empty(), "comctl32 should export symbols");

        // InitCommonControls is COMCTL32 ordinal 17 by ABI (stable by design —
        // this is exactly why import-by-ordinal resolution works), a local
        // address (comctl32 is user-mode), and named.
        let init = exports
            .iter()
            .find(|e| e.name.as_deref() == Some("InitCommonControls"))
            .expect("comctl32 exports InitCommonControls");
        assert_eq!(init.ordinal, 17);
        assert!(matches!(init.target, PeExportTarget::Address(_)));

        // comctl32 forwards some exports (to kernelbase/shlwapi); each forward
        // must carry a non-empty target DLL and name/ordinal — never guessed.
        let mut saw_forward = false;
        for e in &exports {
            match &e.target {
                PeExportTarget::Address(va) => assert!(*va != 0),
                PeExportTarget::ForwardByName(dll, name) => {
                    assert!(!dll.is_empty() && !name.is_empty());
                    saw_forward = true;
                }
                PeExportTarget::ForwardByOrdinal(dll, _) => {
                    assert!(!dll.is_empty());
                    saw_forward = true;
                }
            }
        }
        assert!(saw_forward, "comctl32 has forwarded exports");
    }

    /// Import parsing keeps the source DLL, measured against Wine's real
    /// comctl32.dll (verified vs `objdump -p`: it imports gdi32.dll!BitBlt,
    /// advapi32.dll!RegCloseKey, …). Gated on the Wine PE builtin; ABI-stable
    /// imports asserted, not version-specific counts.
    #[test]
    fn parse_pe_imports_detailed_keeps_source_dll() {
        let candidates = [
            "/usr/lib/i386-linux-gnu/wine/i386-windows/comctl32.dll",
            "/usr/lib/wine/i386-windows/comctl32.dll",
            "/opt/wine-stable/lib/wine/i386-windows/comctl32.dll",
        ];
        let Some(path) = candidates.iter().find(|p| std::path::Path::new(p).exists()) else {
            return;
        };
        let data = std::fs::read(path).unwrap();
        let imports = parse_pe_imports_detailed(&data);
        assert!(!imports.is_empty());
        // Every import carries a non-empty source DLL and at least a name or ordinal.
        for imp in imports.values() {
            assert!(!imp.dll.is_empty());
            assert!(imp.name.is_some() || imp.ordinal.is_some());
        }
        // Case-insensitive check for two stable, named cross-module imports.
        let has = |dll: &str, sym: &str| {
            imports.values().any(|i| {
                i.dll.eq_ignore_ascii_case(dll) && i.name.as_deref() == Some(sym)
            })
        };
        assert!(has("gdi32.dll", "BitBlt"), "comctl32 imports gdi32.BitBlt");
        assert!(has("advapi32.dll", "RegCloseKey"), "comctl32 imports advapi32.RegCloseKey");
    }
}
