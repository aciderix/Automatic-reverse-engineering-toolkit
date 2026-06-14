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

        Ok(Program {
            format: format!("{:?}", obj.format()),
            bitness,
            entry: obj.entry(),
            sections,
            symbols,
        })
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
