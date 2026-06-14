//! Binary loader: turns a raw PE/ELF/Mach-O file into a uniform `Program`
//! description the rest of the pipeline can consume without caring about the
//! container format.

use anyhow::{bail, Context, Result};
use object::{Object, ObjectSection, ObjectSymbol, SectionKind, SymbolKind};
use std::collections::BTreeMap;

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
    /// Static-relocation site address -> resolved target (object files). A `call`
    /// in a `.o` stores only a placeholder displacement until linked; this maps
    /// the displacement field back to the real target so recursive/cross-function
    /// calls decode to the right address (and external calls get a name).
    pub relocs: BTreeMap<u64, RelocEntry>,
}

/// A resolved static relocation: the branch/data target address (when the symbol
/// is defined in this object) and/or the referenced symbol's name.
#[derive(Clone, Debug, Default)]
pub struct RelocEntry {
    pub target: Option<u64>,
    pub name: Option<String>,
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
            symbols
                .entry(addr)
                .or_insert_with(|| KnownSymbol {
                    address: addr,
                    name,
                    is_function,
                });
        }

        let mut imports = parse_pe_imports(data);
        add_elf_imports(&obj, &sections, bitness, &mut imports);

        let relocs = parse_static_relocs(&obj);

        Ok(Program {
            format: format!("{:?}", obj.format()),
            bitness,
            entry: obj.entry(),
            sections,
            symbols,
            imports,
            relocs,
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
            let (sym_addr, name) = match rel.target() {
                RelocationTarget::Symbol(idx) => match obj.symbol_by_index(idx) {
                    Ok(sym) => {
                        let a = sym.address();
                        let nm = sym.name().ok().filter(|n| !n.is_empty()).map(str::to_string);
                        (if a != 0 { Some(a) } else { None }, nm)
                    }
                    Err(_) => (None, None),
                },
                _ => (None, None),
            };
            // PC-relative 4-byte field ends the instruction: target = S + A + 4.
            let pcrel = matches!(rel.kind(), RelocationKind::Relative | RelocationKind::PltRelative);
            let target = match (sym_addr, pcrel) {
                (Some(a), true) => Some(a.wrapping_add(rel.addend() as u64).wrapping_add(4)),
                (Some(a), false) => Some(a.wrapping_add(rel.addend() as u64)),
                (None, _) => None,
            };
            out.insert(site, RelocEntry { target, name });
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
