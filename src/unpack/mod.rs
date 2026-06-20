//! Dynamic unpacker — the [0] DÉBALLAGE brick of the pipeline (docs/vision/30 §5).
//!
//! A packer ships the real code *encrypted* and decrypts it in memory at launch,
//! so it cannot be read statically. The classic parry is to **let the stub run**
//! (here in the Unicorn CPU emulator — a reused brick, no Wine) until it has
//! decrypted the payload, then **dump** the now-cleartext memory and recover the
//! original entry point (OEP).
//!
//! Heart of the technique, implemented and tested here: emulate from the entry
//! point while tracking every page the stub *writes*; when execution *fetches*
//! from a page that was written during this run, the code is self-modifying and
//! we have reached freshly-decrypted code — the OEP. Stop, and the decrypted
//! bytes are readable straight out of emulator memory.
//!
//! Honest scope: a real protector also resolves its imports through Win32
//! (LoadLibrary/GetProcAddress) and guards with anti-debug. Emulating those needs
//! a Win32 model on top of the CPU (a large, separate effort); this module
//! provides the CPU+OEP engine and a clean PE mapper, and reports honestly when a
//! stub reaches for an API we do not yet model instead of crashing.

#![cfg(feature = "unpack")]

use std::collections::BTreeSet;
use std::os::raw::{c_int, c_void};

// ---- Minimal FFI to the system libunicorn (x86) ---------------------------

#[allow(non_camel_case_types)]
type uc_engine = c_void;
#[allow(non_camel_case_types)]
type uc_hook = usize;

const UC_ARCH_X86: c_int = 4;
const UC_MODE_32: c_int = 4;
const UC_PROT_ALL: u32 = 7;
const UC_HOOK_CODE: c_int = 4;
const UC_HOOK_MEM_WRITE: c_int = 2048;
// Unmapped read / write / fetch — used to notice a stub reaching for the
// outside world (e.g. an unmodelled API thunk) so we can report it cleanly.
const UC_HOOK_MEM_UNMAPPED: c_int = 16 | 32 | 64;

const UC_X86_REG_EAX: c_int = 19;
const UC_X86_REG_ESP: c_int = 30;
const UC_X86_REG_EIP: c_int = 26;
const UC_X86_REG_FS_BASE: c_int = 250;

#[link(name = "unicorn")]
extern "C" {
    fn uc_open(arch: c_int, mode: c_int, uc: *mut *mut uc_engine) -> c_int;
    fn uc_close(uc: *mut uc_engine) -> c_int;
    fn uc_mem_map(uc: *mut uc_engine, address: u64, size: usize, perms: u32) -> c_int;
    fn uc_mem_write(uc: *mut uc_engine, address: u64, bytes: *const c_void, size: usize) -> c_int;
    fn uc_mem_read(uc: *mut uc_engine, address: u64, bytes: *mut c_void, size: usize) -> c_int;
    fn uc_reg_write(uc: *mut uc_engine, regid: c_int, value: *const c_void) -> c_int;
    fn uc_reg_read(uc: *mut uc_engine, regid: c_int, value: *mut c_void) -> c_int;
    fn uc_emu_start(uc: *mut uc_engine, begin: u64, until: u64, timeout: u64, count: usize) -> c_int;
    fn uc_emu_stop(uc: *mut uc_engine) -> c_int;
    fn uc_hook_add(
        uc: *mut uc_engine,
        hh: *mut uc_hook,
        kind: c_int,
        callback: *const c_void,
        user_data: *mut c_void,
        begin: u64,
        end: u64,
    ) -> c_int;
}

const PAGE: u64 = 0x1000;
fn page_down(a: u64) -> u64 { a & !(PAGE - 1) }
fn page_up(a: u64) -> u64 { (a + PAGE - 1) & !(PAGE - 1) }

// Reserved emulator regions for the Win32 import model (well clear of the image,
// stack, and TEB/PEB).
const TRAP_BASE: u64 = 0xB000_0000; // each imported function = TRAP_BASE + idx*8
const TRAP_SIZE: u64 = 0x0010_0000; // up to 131072 distinct imports
const ALLOC_BASE: u64 = 0x5000_0000; // VirtualAlloc/Heap arena (bump allocator)
const MODULE_BASE: u32 = 0x6000_0000; // fake module handles

/// State shared with the C callbacks (Unicorn passes it back as `user_data`).
struct Trace {
    /// Pages written during emulation (candidate decrypted code).
    written: BTreeSet<u64>,
    /// Pages that held the original entry stub — executing them is *not* an OEP.
    initial_code: BTreeSet<u64>,
    /// Detected original entry point (first fetch from a freshly-written page).
    oep: Option<u64>,
    /// An address the stub touched that is not mapped (likely an API thunk).
    faulted: Option<u64>,
    /// Remaining on-demand zero pages we will map to let a stub that allocates
    /// scratch memory keep running (a standard unpacker tolerance).
    lazy_budget: u32,
    /// sentinel index -> imported function name (extended at run time by
    /// GetProcAddress). Sentinel address = TRAP_BASE + idx*8.
    imports: Vec<String>,
    /// Parallel to `imports`: the DLL each function was resolved from (via the
    /// LoadLibrary handle passed to GetProcAddress), when known.
    import_dll: Vec<Option<String>>,
    /// Fake module handle -> DLL name (from LoadLibrary/GetModuleHandle).
    modules: Vec<(u32, String)>,
    /// Reconstructed IAT: slot VA -> import index (a sentinel was stored there).
    iat: std::collections::BTreeMap<u64, usize>,
    /// Image base (lowest mapped region) — answers GetModuleHandle(NULL).
    image_base: u64,
    /// Bump pointer for VirtualAlloc/Heap-backed memory.
    alloc_cursor: u64,
    /// Next fake module handle.
    module_cursor: u32,
    /// Count of API calls serviced (diagnostics).
    api_calls: u32,
}

unsafe fn reg_get(uc: *mut uc_engine, id: c_int) -> u32 {
    let mut v: u32 = 0;
    uc_reg_read(uc, id, &mut v as *mut u32 as *mut c_void);
    v
}
unsafe fn reg_set(uc: *mut uc_engine, id: c_int, v: u32) {
    uc_reg_write(uc, id, &v as *const u32 as *const c_void);
}
unsafe fn rd32(uc: *mut uc_engine, addr: u64) -> u32 {
    let mut v: u32 = 0;
    uc_mem_read(uc, addr, &mut v as *mut u32 as *mut c_void, 4);
    v
}
unsafe fn wr32(uc: *mut uc_engine, addr: u64, v: u32) {
    uc_mem_write(uc, addr, &v as *const u32 as *const c_void, 4);
}
unsafe fn rd_cstr(uc: *mut uc_engine, addr: u64) -> String {
    let mut s = String::new();
    for i in 0..256u64 {
        let mut b: u8 = 0;
        if uc_mem_read(uc, addr + i, &mut b as *mut u8 as *mut c_void, 1) != 0 || b == 0 {
            break;
        }
        s.push(b as char);
    }
    s
}
/// Read a UTF-16LE string (kernel32 `*W` APIs), narrowed to ASCII for DLL names.
unsafe fn rd_wstr(uc: *mut uc_engine, addr: u64) -> String {
    let mut s = String::new();
    for i in 0..256u64 {
        let mut w: u16 = 0;
        if uc_mem_read(uc, addr + i * 2, &mut w as *mut u16 as *mut c_void, 2) != 0 || w == 0 {
            break;
        }
        s.push((w as u8) as char);
    }
    s
}

/// Service an imported call: read stdcall args from the emulated stack, model a
/// native result, then simulate the stdcall return (pop retaddr + callee args).
/// Just enough of the Win32 surface for a packer to resolve its IAT and allocate
/// scratch before it decrypts — the part [3]/Winelib would supply for real.
unsafe fn handle_api(uc: *mut uc_engine, t: &mut Trace, idx: usize) {
    t.api_calls += 1;
    let name = t.imports.get(idx).cloned().unwrap_or_default();
    let esp = reg_get(uc, UC_X86_REG_ESP) as u64;
    let retaddr = rd32(uc, esp);
    let arg = |k: u64| -> u32 { rd32(uc, esp + 4 + 4 * k) };

    let (result, argbytes): (u32, u32) = match name.as_str() {
        "LoadLibraryA" | "LoadLibraryW" | "LoadLibraryExA" | "LoadLibraryExW"
        | "GetModuleHandleA" | "GetModuleHandleW" => {
            let wide = name.ends_with('W');
            let dll = if arg(0) == 0 { String::new() } else if wide { rd_wstr(uc, arg(0) as u64) } else { rd_cstr(uc, arg(0) as u64) };
            let h = if arg(0) == 0 {
                t.image_base as u32
            } else {
                let h = t.module_cursor;
                t.module_cursor = t.module_cursor.wrapping_add(0x10000);
                let _ = uc_mem_map(uc, h as u64, 0x1000, UC_PROT_ALL);
                if !dll.is_empty() { t.modules.push((h, dll)); }
                h
            };
            let argbytes = if name.starts_with("LoadLibraryEx") { 12 } else { 4 };
            (h, argbytes)
        }
        "GetProcAddress" => {
            // Bind a fresh sentinel to the requested proc so a later call traps
            // here too. arg1 is a name pointer (high) or an ordinal (low).
            let p = arg(1);
            let nm = if p >= 0x1_0000 { rd_cstr(uc, p as u64) } else { format!("ordinal_{p}") };
            let dll = t.modules.iter().rev().find(|(h, _)| *h == arg(0)).map(|(_, d)| d.clone());
            let new_idx = t.imports.len();
            t.imports.push(nm);
            t.import_dll.push(dll);
            ((TRAP_BASE + new_idx as u64 * 8) as u32, 8)
        }
        "VirtualAlloc" | "VirtualAllocEx" => {
            let (size_arg, ab) = if name.ends_with("Ex") { (2u64, 20u32) } else { (1u64, 16u32) };
            let size = page_up(arg(size_arg).max(1) as u64);
            let base = t.alloc_cursor;
            let _ = uc_mem_map(uc, base, size as usize, UC_PROT_ALL);
            t.alloc_cursor += size;
            (base as u32, ab)
        }
        "VirtualProtect" => { if arg(3) != 0 { wr32(uc, arg(3) as u64, 0x40); } (1, 16) }
        "VirtualFree" => (1, 12),
        "VirtualQuery" => (0, 12),
        "GetVersion" => (0x0A28_0106, 0),
        "GetVersionExA" | "GetVersionExW" => (1, 4),
        "GetCurrentProcessId" | "GetCurrentThreadId" => (0x1000, 0),
        "GetTickCount" => (1, 0),
        "IsDebuggerPresent" => (0, 0),
        "GetLastError" => (0, 0),
        "SetLastError" => (0, 4),
        // Unknown import: return 0 and assume cdecl (caller cleans). Such calls
        // are rare before the OEP; if one drifts the stack, we report honestly.
        _ => (0, 0),
    };

    reg_set(uc, UC_X86_REG_EAX, result);
    reg_set(uc, UC_X86_REG_ESP, (esp + 4 + argbytes as u64) as u32);
    reg_set(uc, UC_X86_REG_EIP, retaddr); // resume at the caller's return address
}

extern "C" fn on_write(_uc: *mut uc_engine, _ty: c_int, addr: u64, sz: c_int, val: i64, ud: *mut c_void) {
    let t = unsafe { &mut *(ud as *mut Trace) };
    t.written.insert(page_down(addr));
    // The program storing a resolved import (a sentinel) into a slot reveals the
    // IAT layout: record slot VA -> import index, for import-directory rebuild.
    if sz == 4 {
        let v = val as u32 as u64;
        if v >= TRAP_BASE && v < TRAP_BASE + TRAP_SIZE && v % 8 == 0 {
            let idx = ((v - TRAP_BASE) / 8) as usize;
            if idx < t.imports.len() {
                t.iat.insert(addr, idx);
            }
        }
    }
}

extern "C" fn on_code(uc: *mut uc_engine, addr: u64, _sz: c_int, ud: *mut c_void) {
    let t = unsafe { &mut *(ud as *mut Trace) };
    // A fetch inside the import trap region is a call through the IAT: model it
    // natively and resume at the caller (never an OEP).
    if addr >= TRAP_BASE && addr < TRAP_BASE + TRAP_SIZE {
        let idx = ((addr - TRAP_BASE) / 8) as usize;
        unsafe { handle_api(uc, t, idx); }
        return;
    }
    let pg = page_down(addr);
    // Executing from a page we saw written this run, and which was not part of
    // the original stub: freshly-decrypted code => OEP reached.
    if t.written.contains(&pg) && !t.initial_code.contains(&pg) {
        t.oep = Some(addr);
        unsafe { uc_emu_stop(uc); }
    }
}

const UC_MEM_FETCH_UNMAPPED: c_int = 21;

extern "C" fn on_unmapped(uc: *mut uc_engine, ty: c_int, addr: u64, _sz: c_int, _val: i64, ud: *mut c_void) -> bool {
    let t = unsafe { &mut *(ud as *mut Trace) };
    // An unmapped *data* read/write (anywhere — packers and the CRT/SEH startup
    // probe scratch and near-null fields) is tolerated by backing it with a zero
    // page, up to a budget. An unmapped *fetch* is lost control flow: we cannot
    // fabricate code, so stop and report it honestly.
    if ty != UC_MEM_FETCH_UNMAPPED && t.lazy_budget > 0 {
        t.lazy_budget -= 1;
        let pg = page_down(addr);
        if unsafe { uc_mem_map(uc, pg, PAGE as usize, UC_PROT_ALL) } == 0 {
            return true; // page now backed with zeros — retry the access
        }
    }
    if t.faulted.is_none() {
        t.faulted = Some(addr);
    }
    unsafe { uc_emu_stop(uc); }
    false
}

/// Outcome of an unpack attempt.
#[derive(Debug)]
pub struct Unpacked {
    pub oep: u64,
    /// Recovered (decrypted) memory image: (virtual address, bytes) per region.
    pub regions: Vec<(u64, Vec<u8>)>,
    /// Number of imported calls serviced by the Win32 model before the OEP.
    pub api_calls: u32,
    /// Full decrypted image as contiguous runs (VA, bytes) — the basis for a
    /// rebuilt clean PE.
    pub image_pages: Vec<(u64, Vec<u8>)>,
    /// Reconstructed import table (slot VA, DLL, function), recovered from the
    /// GetProcAddress bindings the program stored into its IAT.
    pub iat: Vec<IatEntry>,
}

/// A region to load into the emulator: virtual address + initial bytes.
pub struct Region {
    pub va: u64,
    pub bytes: Vec<u8>,
    /// True for the region that contains the entry point (the original stub).
    pub is_entry: bool,
}

/// Emulate `regions` from `entry` until the stub decrypts and jumps to fresh
/// code (OEP), then return the decrypted image. `regions` must cover the entry.
/// `imports` are IAT slots (slot VA, function name): each slot is filled with a
/// trap sentinel so a call through it is serviced by the Win32 model.
pub fn emulate_until_oep(
    regions: &[Region],
    entry: u64,
    imports: &[(u64, String)],
    max_insns: usize,
) -> Result<Unpacked, String> {
    unsafe {
        let mut uc: *mut uc_engine = std::ptr::null_mut();
        if uc_open(UC_ARCH_X86, UC_MODE_32, &mut uc) != 0 {
            return Err("uc_open failed".into());
        }
        // RAII close.
        struct Closer(*mut uc_engine);
        impl Drop for Closer { fn drop(&mut self) { unsafe { uc_close(self.0); } } }
        let _closer = Closer(uc);

        let image_base = regions.iter().map(|r| page_down(r.va)).min().unwrap_or(0);
        let mut trace = Box::new(Trace {
            written: BTreeSet::new(),
            initial_code: BTreeSet::new(),
            oep: None,
            faulted: None,
            lazy_budget: 256,
            imports: Vec::new(),
            import_dll: Vec::new(),
            modules: Vec::new(),
            iat: std::collections::BTreeMap::new(),
            image_base,
            alloc_cursor: ALLOC_BASE,
            module_cursor: MODULE_BASE,
            api_calls: 0,
        });

        // Map and load each region (page-aligned).
        for r in regions {
            let start = page_down(r.va);
            let end = page_up(r.va + r.bytes.len() as u64);
            let size = (end - start) as usize;
            // Tolerate overlap errors from adjacent rounded regions by mapping
            // page by page only where needed.
            if uc_mem_map(uc, start, size, UC_PROT_ALL) != 0 {
                // Likely already mapped by a neighbour; fall through to write.
            }
            if uc_mem_write(uc, r.va, r.bytes.as_ptr() as *const c_void, r.bytes.len()) != 0 {
                return Err(format!("uc_mem_write failed at {:#x}", r.va));
            }
            if r.is_entry {
                let mut p = start;
                while p < end { trace.initial_code.insert(p); p += PAGE; }
            }
        }

        // Import trap region: filled with `ret` as a safety net (we normally
        // redirect EIP in the code hook before it executes). Each IAT slot gets a
        // sentinel pointing here, bound to its import name.
        let _ = uc_mem_map(uc, TRAP_BASE, TRAP_SIZE as usize, UC_PROT_ALL);
        let rets = vec![0xC3u8; TRAP_SIZE as usize];
        let _ = uc_mem_write(uc, TRAP_BASE, rets.as_ptr() as *const c_void, rets.len());
        for (slot_va, name) in imports {
            let idx = trace.imports.len();
            trace.imports.push(name.clone());
            trace.import_dll.push(None); // the packed file's own (stub) imports
            wr32(uc, *slot_va, (TRAP_BASE + idx as u64 * 8) as u32);
        }

        // A stack well away from the image.
        let stack_base: u64 = 0x10_0000;
        let stack_size: usize = 0x10_0000;
        let _ = uc_mem_map(uc, stack_base, stack_size, UC_PROT_ALL);
        let esp = stack_base + stack_size as u64 - 0x100;
        uc_reg_write(uc, UC_X86_REG_ESP, &esp as *const u64 as *const c_void);

        // Synthetic TEB/PEB so the near-universal Windows stub prologue
        // (`mov eax, fs:[0x30]` → PEB, `fs:[0x18]` → TEB self) does not fault
        // immediately. Real protectors then read the PEB (e.g. BeingDebugged);
        // a zeroed block answers the common probes. FS base points at the TEB.
        let teb: u64 = 0x7000_0000;
        let peb: u64 = 0x7001_0000;
        let _ = uc_mem_map(uc, teb, 0x1000, UC_PROT_ALL);
        let _ = uc_mem_map(uc, peb, 0x1000, UC_PROT_ALL);
        let put = |addr: u64, val: u32| {
            uc_mem_write(uc, addr, &val as *const u32 as *const c_void, 4);
        };
        put(teb + 0x18, teb as u32); // TEB.Self
        put(teb + 0x30, peb as u32); // TEB.ProcessEnvironmentBlock
        put(peb + 0x02, 0);          // PEB.BeingDebugged = 0
        uc_reg_write(uc, UC_X86_REG_FS_BASE, &teb as *const u64 as *const c_void);

        let ud = trace.as_mut() as *mut Trace as *mut c_void;
        let mut h: uc_hook = 0;
        uc_hook_add(uc, &mut h, UC_HOOK_MEM_WRITE, on_write as *const c_void, ud, 1, 0);
        uc_hook_add(uc, &mut h, UC_HOOK_CODE, on_code as *const c_void, ud, 1, 0);
        uc_hook_add(uc, &mut h, UC_HOOK_MEM_UNMAPPED, on_unmapped as *const c_void, ud, 1, 0);

        // Run. A non-zero return is expected when a hook stops us (OEP/fault).
        let _ = uc_emu_start(uc, entry, 0, 0, max_insns);

        if let Some(f) = trace.faulted {
            if trace.oep.is_none() {
                return Err(format!(
                    "stub reached unmapped {:#x} after {} API call(s) — needs a wider Win32 model",
                    f, trace.api_calls
                ));
            }
        }
        let api_calls = trace.api_calls;
        let oep = trace.oep.ok_or_else(|| {
            format!(
                "no OEP after {} API call(s) — no self-modified code executed within the budget",
                api_calls
            )
        })?;

        // Dump the (now decrypted) memory back out for every region.
        let mut out = Vec::new();
        for r in regions {
            let mut buf = vec![0u8; r.bytes.len()];
            if uc_mem_read(uc, r.va, buf.as_mut_ptr() as *mut c_void, buf.len()) == 0 {
                out.push((r.va, buf));
            }
        }

        // Capture the *full* decrypted image: every page the stub touched
        // (original-stub pages + everything it wrote) within a sane window around
        // the image base — this is where a packer reconstructs the real program
        // (e.g. UPX decompresses into the original .text at its original VA).
        let window = image_base..(image_base + 0x1000_0000);
        let mut pages: BTreeSet<u64> = BTreeSet::new();
        for &p in trace.initial_code.iter().chain(trace.written.iter()) {
            if window.contains(&p) { pages.insert(p); }
        }
        for r in regions {
            let mut p = page_down(r.va);
            let end = page_up(r.va + r.bytes.len() as u64);
            while p < end { if window.contains(&p) { pages.insert(p); } p += PAGE; }
        }
        // Coalesce contiguous pages into runs, reading each from the emulator.
        let mut image_pages: Vec<(u64, Vec<u8>)> = Vec::new();
        let mut cur_va: Option<u64> = None;
        let mut cur: Vec<u8> = Vec::new();
        let mut prev = 0u64;
        for p in pages {
            let mut buf = vec![0u8; PAGE as usize];
            if uc_mem_read(uc, p, buf.as_mut_ptr() as *mut c_void, PAGE as usize) != 0 {
                continue;
            }
            match cur_va {
                Some(_) if p == prev + PAGE => cur.extend_from_slice(&buf),
                _ => {
                    if let Some(v) = cur_va { image_pages.push((v, std::mem::take(&mut cur))); }
                    cur_va = Some(p);
                    cur = buf;
                }
            }
            prev = p;
        }
        if let Some(v) = cur_va { image_pages.push((v, cur)); }

        // Reconstructed IAT: each slot the program filled with a resolved import,
        // paired with the (DLL, function) it was bound to. Skip the packed file's
        // own stub imports (no DLL recorded) — we want the *program's* imports.
        let mut iat: Vec<IatEntry> = Vec::new();
        for (&slot_va, &idx) in trace.iat.iter() {
            if let Some(dll) = trace.import_dll.get(idx).cloned().flatten() {
                iat.push(IatEntry { slot_va, dll, func: trace.imports[idx].clone() });
            }
        }

        Ok(Unpacked { oep, regions: out, api_calls, image_pages, iat })
    }
}

/// One recovered import: the IAT slot, its DLL, and the function name.
#[derive(Debug, Clone)]
pub struct IatEntry {
    pub slot_va: u64,
    pub dll: String,
    pub func: String,
}

/// Read `len` bytes starting at virtual address `va` from the dumped runs
/// (zero-filled where the image has no page).
fn read_va(runs: &[(u64, Vec<u8>)], va: u64, len: usize) -> Vec<u8> {
    let mut out = vec![0u8; len];
    for (base, bytes) in runs {
        let end = base + bytes.len() as u64;
        if va < end && va + len as u64 > *base {
            let lo = va.max(*base);
            let hi = (va + len as u64).min(end);
            for a in lo..hi {
                out[(a - va) as usize] = bytes[(a - *base) as usize];
            }
        }
    }
    out
}

/// Build a statically-analysable PE32 from a dumped memory image, entry = OEP.
/// This is the "rebuild a clean .exe" step ([0] in the pipeline): the output
/// re-enters `--mode transpile`.
///
/// Preferred path: when the unpacked image has restored the **original PE
/// headers** at the image base (UPX-class packers do), reuse them and lay the
/// file out raw==RVA — this preserves the original **import directory**, so the
/// rebuilt PE is fully faithful (imports intact). Fallback: a flat single-section
/// dump when no header is present.
///
/// Honest scope: it does not *reconstruct* a destroyed import directory (Scylla's
/// job) — but it preserves an intact one, and the engine traces GetProcAddress
/// bindings for the rebuild-from-scratch case.
pub fn build_dump_pe(image_base: u64, oep: u64, runs: &[(u64, Vec<u8>)], iat: &[IatEntry]) -> Vec<u8> {
    // Try the faithful, header-preserving rebuild first (raw==RVA, so we can also
    // inject a reconstructed import directory).
    if let Some(mut pe) = build_from_restored_headers(image_base, oep, runs) {
        inject_import_directory(&mut pe, iat);
        return pe;
    }
    build_synthetic_pe(image_base, oep, runs)
}

/// Append a reconstructed import directory to a raw==RVA PE and point the data
/// directory at it — so a loader (and ARET's `--mode transpile`) names the IAT
/// slots. This is the Scylla-style step: from the recovered (slot, DLL, func)
/// bindings, synthesise standard IMAGE_IMPORT_DESCRIPTORs. Per DLL the slots are
/// assumed contiguous (sorted); FirstThunk points at the real IAT.
fn inject_import_directory(f: &mut Vec<u8>, iat: &[IatEntry]) {
    use std::collections::BTreeMap;
    if iat.is_empty() { return; }
    let rd16 = |f: &[u8], o: usize| u16::from_le_bytes([f[o], f[o + 1]]);
    let rd32 = |f: &[u8], o: usize| u32::from_le_bytes([f[o], f[o + 1], f[o + 2], f[o + 3]]);
    let e_lfanew = rd32(f, 0x3C) as usize;
    if e_lfanew + 0x78 >= f.len() || &f[e_lfanew..e_lfanew + 4] != b"PE\0\0" { return; }
    let coff = e_lfanew + 4;
    let nsec = rd16(f, coff + 2) as usize;
    let opt_sz = rd16(f, coff + 16) as usize;
    let o = coff + 20;
    let image_base = rd32(f, o + 28) as u64;
    let st = o + opt_sz;

    // Group recovered imports by DLL, ordered by slot.
    let mut groups: BTreeMap<String, Vec<(u64, String)>> = BTreeMap::new();
    for e in iat {
        groups.entry(e.dll.clone()).or_default().push((e.slot_va, e.func.clone()));
    }
    for v in groups.values_mut() { v.sort(); v.dedup(); }

    // The blob is appended at the end; raw==RVA means file offset == RVA.
    while f.len() % 4 != 0 { f.push(0); }
    let blob_rva = f.len() as u32;
    let ndesc = groups.len();
    let desc_bytes = (ndesc + 1) * 20;
    // Build the blob with a two-pass layout: reserve descriptors, then ILTs,
    // name tables, and strings, tracking each RVA.
    let mut blob = vec![0u8; desc_bytes];
    let put32 = |b: &mut Vec<u8>, off: usize, v: u32| b[off..off + 4].copy_from_slice(&v.to_le_bytes());
    let mut descs: Vec<(u32, u32, u32)> = Vec::new(); // (ilt_rva, name_rva, first_thunk_rva)

    for (dll, funcs) in &groups {
        // IMAGE_IMPORT_BY_NAME entries + ILT.
        let mut ilt: Vec<u32> = Vec::new();
        for (_, func) in funcs {
            let ibn_rva = blob_rva + blob.len() as u32;
            blob.extend_from_slice(&0u16.to_le_bytes()); // Hint
            blob.extend_from_slice(func.as_bytes());
            blob.push(0);
            if blob.len() % 2 != 0 { blob.push(0); }
            ilt.push(ibn_rva);
        }
        ilt.push(0); // null-terminate ILT
        let ilt_rva = blob_rva + blob.len() as u32;
        for v in &ilt { blob.extend_from_slice(&v.to_le_bytes()); }
        let name_rva = blob_rva + blob.len() as u32;
        blob.extend_from_slice(dll.as_bytes());
        blob.push(0);
        if blob.len() % 2 != 0 { blob.push(0); }
        let first_thunk = (funcs[0].0 - image_base) as u32; // real IAT slot RVA
        descs.push((ilt_rva, name_rva, first_thunk));
    }
    // Fill the descriptor array.
    for (i, (ilt_rva, name_rva, first_thunk)) in descs.iter().enumerate() {
        let d = i * 20;
        put32(&mut blob, d, *ilt_rva);       // OriginalFirstThunk
        put32(&mut blob, d + 12, *name_rva); // Name
        put32(&mut blob, d + 16, *first_thunk); // FirstThunk
    }

    let blob_len = blob.len() as u32;
    f.extend_from_slice(&blob);

    // Point the import data directory (index 1) at the descriptors.
    let dd_import = o + 96 + 8;
    f[dd_import..dd_import + 4].copy_from_slice(&blob_rva.to_le_bytes());
    f[dd_import + 4..dd_import + 8].copy_from_slice(&(desc_bytes as u32).to_le_bytes());
    if rd32(f, o + 92) < 2 {
        f[o + 92..o + 96].copy_from_slice(&2u32.to_le_bytes()); // NumberOfRvaAndSizes
    }

    // Extend the highest-RVA section to cover the appended blob.
    let mut best = 0usize;
    let mut best_va = 0u32;
    for i in 0..nsec {
        let s = st + i * 40;
        let va = rd32(f, s + 12);
        if va >= best_va { best_va = va; best = s; }
    }
    let new_end = blob_rva + blob_len;
    let cover = new_end - best_va;
    f[best + 8..best + 12].copy_from_slice(&cover.to_le_bytes());  // VirtualSize
    f[best + 16..best + 20].copy_from_slice(&cover.to_le_bytes()); // SizeOfRawData
    f[best + 36..best + 40].copy_from_slice(&0xE000_0060u32.to_le_bytes()); // RWX
    // Grow SizeOfImage if needed.
    let sect_align = rd32(f, o + 32).max(1);
    let img = (best_va + cover + sect_align - 1) & !(sect_align - 1);
    if rd32(f, o + 56) < img {
        f[o + 56..o + 60].copy_from_slice(&img.to_le_bytes());
    }
}

/// Faithful rebuild reusing the restored original headers (raw==RVA), preserving
/// the import directory. Returns None if no valid PE header sits at the base.
fn build_from_restored_headers(image_base: u64, oep: u64, runs: &[(u64, Vec<u8>)]) -> Option<Vec<u8>> {
    let hdr = read_va(runs, image_base, 0x1000);
    if hdr.len() < 0x40 || &hdr[0..2] != b"MZ" { return None; }
    let rd16 = |b: &[u8], o: usize| u16::from_le_bytes([b[o], b[o + 1]]);
    let rd32 = |b: &[u8], o: usize| u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]);
    let e_lfanew = rd32(&hdr, 0x3C) as usize;
    if e_lfanew + 0x78 >= hdr.len() || &hdr[e_lfanew..e_lfanew + 4] != b"PE\0\0" { return None; }
    let coff = e_lfanew + 4;
    let nsec = rd16(&hdr, coff + 2) as usize;
    let opt_sz = rd16(&hdr, coff + 16) as usize;
    let o = coff + 20;
    if rd16(&hdr, o) != 0x010B { return None; } // PE32 only
    let sect_align = rd32(&hdr, o + 32);
    let size_of_headers = rd32(&hdr, o + 60) as usize;
    let st = o + opt_sz;
    if st + nsec * 40 > hdr.len() || nsec == 0 || sect_align == 0 { return None; }

    // Determine SizeOfImage from the section table (max RVA end).
    let mut image_size: u32 = size_of_headers as u32;
    let mut sects: Vec<(u32, u32)> = Vec::new(); // (rva, vsize)
    for i in 0..nsec {
        let s = st + i * 40;
        let vsize = rd32(&hdr, s + 8);
        let rva = rd32(&hdr, s + 12);
        let raw_sz = rd32(&hdr, s + 16);
        let vs = vsize.max(raw_sz);
        let end = (rva + vs + sect_align - 1) & !(sect_align - 1);
        image_size = image_size.max(end);
        sects.push((rva, vs));
    }
    if image_size as usize > 0x4000_0000 { return None; } // sanity

    // Lay the file out raw==RVA so the original directory RVAs stay valid.
    let fa = sect_align;
    let mut f = vec![0u8; image_size as usize];
    let hb = read_va(runs, image_base, size_of_headers);
    f[..hb.len()].copy_from_slice(&hb);

    let w32 = |f: &mut [u8], o: usize, v: u32| f[o..o + 4].copy_from_slice(&v.to_le_bytes());
    // Patch optional header: FileAlignment = SectionAlignment, entry = OEP.
    w32(&mut f, o + 16, (oep - image_base) as u32);
    w32(&mut f, o + 36, fa);
    // Copy each section's memory image to file offset == RVA, patch raw ptr/size.
    for (i, (rva, vs)) in sects.iter().enumerate() {
        let s = st + i * 40;
        let raw_sz = (vs + fa - 1) & !(fa - 1);
        w32(&mut f, s + 16, raw_sz);  // SizeOfRawData
        w32(&mut f, s + 20, *rva);    // PointerToRawData == VirtualAddress
        let body = read_va(runs, image_base + *rva as u64, *vs as usize);
        let dst = *rva as usize;
        if dst + body.len() <= f.len() {
            f[dst..dst + body.len()].copy_from_slice(&body);
        }
    }
    Some(f)
}

/// Flat single-section PE for a dump with no usable headers.
fn build_synthetic_pe(image_base: u64, oep: u64, runs: &[(u64, Vec<u8>)]) -> Vec<u8> {
    const FA: u32 = 0x1000; // file & section alignment (raw mirrors memory)
    let nsec = runs.len() as u32;
    // headers: DOS(0x40) + PE sig(4) + COFF(20) + optional(0xE0) + sections(40*n)
    let opt = 0xE0u32;
    let hdr_unpadded = 0x40 + 4 + 20 + opt + 40 * nsec;
    let hdr = (hdr_unpadded + FA - 1) & !(FA - 1);

    // Assign each run a raw offset (page-aligned, packed after the header).
    let mut raws: Vec<u32> = Vec::new();
    let mut off = hdr;
    for (_, bytes) in runs {
        raws.push(off);
        off += ((bytes.len() as u32 + FA - 1) & !(FA - 1)).max(FA);
    }
    let file_size = off as usize;
    let mut f = vec![0u8; file_size];

    let w16 = |f: &mut [u8], o: usize, v: u16| f[o..o + 2].copy_from_slice(&v.to_le_bytes());
    let w32 = |f: &mut [u8], o: usize, v: u32| f[o..o + 4].copy_from_slice(&v.to_le_bytes());

    // DOS header: "MZ" + e_lfanew at 0x3C.
    f[0] = b'M'; f[1] = b'Z';
    let pe_off = 0x40usize;
    w32(&mut f, 0x3C, pe_off as u32);
    // PE signature + COFF header.
    f[pe_off] = b'P'; f[pe_off + 1] = b'E';
    let coff = pe_off + 4;
    w16(&mut f, coff, 0x014C);                 // Machine = i386
    w16(&mut f, coff + 2, nsec as u16);        // NumberOfSections
    w16(&mut f, coff + 16, opt as u16);        // SizeOfOptionalHeader
    w16(&mut f, coff + 18, 0x0102 | 0x0002);   // Characteristics: EXE | 32BIT
    // Optional header (PE32).
    let o = coff + 20;
    let last_end = runs.last().map(|(va, b)| {
        (va - image_base) as u32 + ((b.len() as u32 + FA - 1) & !(FA - 1))
    }).unwrap_or(0);
    w16(&mut f, o, 0x010B);                     // Magic PE32
    w32(&mut f, o + 16, (oep - image_base) as u32); // AddressOfEntryPoint (RVA)
    w32(&mut f, o + 28, image_base as u32);     // ImageBase
    w32(&mut f, o + 32, FA);                    // SectionAlignment
    w32(&mut f, o + 36, FA);                    // FileAlignment
    w16(&mut f, o + 40, 6); w16(&mut f, o + 42, 0); // OS version 6.0
    w16(&mut f, o + 48, 6);                     // Subsystem-major (cosmetic)
    w32(&mut f, o + 56, ((hdr + last_end + FA - 1) & !(FA - 1)).max(hdr)); // SizeOfImage
    w32(&mut f, o + 60, hdr);                   // SizeOfHeaders
    w16(&mut f, o + 68, 3);                     // Subsystem = CONSOLE
    w32(&mut f, o + 92, 16);                    // NumberOfRvaAndSizes

    // Section table.
    let mut st = o + opt as usize;
    for (i, (va, bytes)) in runs.iter().enumerate() {
        let name = format!(".dmp{i}");
        let nb = name.as_bytes();
        f[st..st + nb.len().min(8)].copy_from_slice(&nb[..nb.len().min(8)]);
        let vsize = (bytes.len() as u32 + FA - 1) & !(FA - 1);
        w32(&mut f, st + 8, vsize);                 // VirtualSize
        w32(&mut f, st + 12, (va - image_base) as u32); // VirtualAddress (RVA)
        w32(&mut f, st + 16, vsize);                // SizeOfRawData
        w32(&mut f, st + 20, raws[i]);              // PointerToRawData
        w32(&mut f, st + 36, 0xE000_0060);          // RWX | code | initialized
        // Copy the dumped bytes into the file at the raw offset.
        let r = raws[i] as usize;
        f[r..r + bytes.len()].copy_from_slice(bytes);
        st += 40;
    }
    f
}

/// Drive the unpacker over a loaded program: map its sections, run the stub, and
/// report the OEP plus how much of the image changed (was decrypted).
pub fn unpack_program(prog: &crate::loader::Program, max_insns: usize) -> Result<UnpackReport, String> {
    let mut regions: Vec<Region> = Vec::new();
    for s in &prog.sections {
        if s.data.is_empty() { continue; }
        let is_entry = prog.entry >= s.address && prog.entry < s.address + s.data.len() as u64;
        regions.push(Region { va: s.address, bytes: s.data.clone(), is_entry });
    }
    if regions.is_empty() {
        return Err("no loadable sections".into());
    }
    let before: Vec<Vec<u8>> = regions.iter().map(|r| r.bytes.clone()).collect();
    let imports: Vec<(u64, String)> =
        prog.imports.iter().map(|(va, name)| (*va, name.clone())).collect();
    let res = emulate_until_oep(&regions, prog.entry, &imports, max_insns)?;

    // Measure how many bytes the stub rewrote (decrypted), per region.
    let mut changed = 0usize;
    let mut total = 0usize;
    for ((va, after), orig) in res.regions.iter().zip(before.iter()) {
        let _ = va;
        total += orig.len();
        for (a, b) in after.iter().zip(orig.iter()) {
            if a != b { changed += 1; }
        }
    }
    let image_base = res.image_pages.first().map(|(va, _)| *va).unwrap_or(0);
    let dump_pe = build_dump_pe(image_base, res.oep, &res.image_pages, &res.iat);
    Ok(UnpackReport {
        oep: res.oep,
        decrypted_bytes: changed,
        total_bytes: total,
        image: res.regions,
        api_calls: res.api_calls,
        image_pages: res.image_pages,
        imports_recovered: res.iat,
        dump_pe,
    })
}

pub struct UnpackReport {
    pub oep: u64,
    pub decrypted_bytes: usize,
    pub total_bytes: usize,
    pub image: Vec<(u64, Vec<u8>)>,
    pub api_calls: u32,
    /// Full decrypted image as contiguous (VA, bytes) runs.
    pub image_pages: Vec<(u64, Vec<u8>)>,
    /// Imports recovered from the GetProcAddress/IAT bindings (slot, DLL, func).
    pub imports_recovered: Vec<IatEntry>,
    /// A rebuilt, statically-analysable PE32 of the decrypted image (entry=OEP).
    pub dump_pe: Vec<u8>,
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A minimal self-decrypting "packer": the stub XOR-decrypts a 4-byte payload
    /// in place and jumps to it. The unpacker must run the stub, notice the
    /// payload page being written then executed (OEP), and hand back the
    /// decrypted bytes — exactly the generic-unpacking primitive.
    #[test]
    fn xor_self_decryptor_reaches_oep_and_recovers_payload() {
        let stub_va = 0x401000u64;
        let payload_va = 0x402000u64;

        // mov esi, payload; mov ecx, 4; loop: xor byte[esi],0xAA; inc esi; dec ecx;
        // jnz loop; jmp payload.
        let stub: Vec<u8> = vec![
            0xBE, 0x00, 0x20, 0x40, 0x00, // mov esi, 0x402000
            0xB9, 0x04, 0x00, 0x00, 0x00, // mov ecx, 4
            0x80, 0x36, 0xAA, //             xor byte [esi], 0xAA
            0x46, //                         inc esi
            0x49, //                         dec ecx
            0x75, 0xF9, //                   jnz -7 (back to xor)
            0xE9, 0xEA, 0x0F, 0x00, 0x00, // jmp 0x402000
        ];
        // Decrypted payload = [nop, nop, nop, ret]; stored XOR 0xAA (encrypted).
        let plain = [0x90u8, 0x90, 0x90, 0xC3];
        let payload: Vec<u8> = plain.iter().map(|b| b ^ 0xAA).collect();

        let regions = vec![
            Region { va: stub_va, bytes: stub, is_entry: true },
            Region { va: payload_va, bytes: payload, is_entry: false },
        ];

        let r = emulate_until_oep(&regions, stub_va, &[], 1_000_000).expect("unpack");
        assert_eq!(r.oep, payload_va, "OEP should be the decrypted payload");
        // The recovered payload region must now be the cleartext code.
        let (_, recovered) = r.regions.iter().find(|(va, _)| *va == payload_va).unwrap();
        assert_eq!(&recovered[..4], &plain, "payload was not decrypted in memory");
    }

    /// A stub that never self-modifies must NOT be reported as unpacked.
    #[test]
    fn non_self_modifying_code_has_no_oep() {
        let va = 0x401000u64;
        // mov eax,1; jmp $ (infinite loop, no writes) — runs to the budget.
        let code = vec![0xB8, 0x01, 0x00, 0x00, 0x00, 0xEB, 0xFE];
        let regions = vec![Region { va, bytes: code, is_entry: true }];
        let err = emulate_until_oep(&regions, va, &[], 1000).unwrap_err();
        assert!(err.contains("no OEP"), "unexpected: {err}");
    }

    /// A packer that resolves an import through its IAT (`call [iat]` ->
    /// VirtualAlloc), writes the decrypted payload into the returned buffer, and
    /// jumps to it. The Win32 model must service the call and return a usable
    /// pointer, then the OEP must land in the allocated, freshly-written page.
    #[test]
    fn iat_call_resolved_then_oep_in_allocated_memory() {
        let stub_va = 0x401000u64;
        let iat_va = 0x402000u64; // one IAT slot, bound to VirtualAlloc

        // VirtualAlloc(NULL, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        // mov byte [eax], 0x90 ; jmp eax
        let stub: Vec<u8> = vec![
            0x6A, 0x40, //                   push 0x40
            0x68, 0x00, 0x10, 0x00, 0x00, // push 0x1000
            0x68, 0x00, 0x10, 0x00, 0x00, // push 0x1000
            0x6A, 0x00, //                   push 0
            0xFF, 0x15, 0x00, 0x20, 0x40, 0x00, // call dword [0x402000]
            0xC6, 0x00, 0x90, //             mov byte [eax], 0x90
            0xFF, 0xE0, //                   jmp eax
        ];
        let regions = vec![
            Region { va: stub_va, bytes: stub, is_entry: true },
            Region { va: iat_va, bytes: vec![0u8; 4], is_entry: false },
        ];
        let imports = vec![(iat_va, "VirtualAlloc".to_string())];

        let r = emulate_until_oep(&regions, stub_va, &imports, 1_000_000).expect("unpack");
        assert_eq!(r.api_calls, 1, "the IAT call should have been serviced once");
        assert_eq!(r.oep, ALLOC_BASE, "OEP should be the VirtualAlloc'd buffer");
    }

    /// End-to-end on a *real* packer (UPX): pack a fixture, run the unpacker, and
    /// check it services the stub's imports, recovers the OEP, and that the dumped
    /// image at the OEP is byte-identical to the original unpacked program. Skips
    /// when `upx` is unavailable.
    #[test]
    fn upx_real_packer_recovers_original_code() {
        use std::process::Command;
        let fixtures = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/m1/fixtures");
        let orig = format!("{fixtures}/hello_printf.exe");
        let packed = std::env::temp_dir().join(format!("aret_upx_{}.exe", std::process::id()));
        let _ = std::fs::remove_file(&packed);

        let status = Command::new("upx")
            .args(["-q", "-o"]).arg(&packed).arg(&orig)
            .status();
        match status {
            Ok(s) if s.success() => {}
            _ => { eprintln!("skipping UPX test: upx unavailable"); return; }
        }

        let packed_bytes = std::fs::read(&packed).unwrap();
        let prog = crate::loader::Program::load(&packed_bytes).expect("load packed");
        let r = unpack_program(&prog, 50_000_000).expect("unpack UPX");

        // The UPX stub resolves its imports (LoadLibrary/GetProcAddress/...) and
        // jumps to the original entry, away from the packed stub's entry.
        assert!(r.api_calls >= 1, "UPX stub should call imports");
        assert_ne!(r.oep, prog.entry, "OEP must differ from the packed entry");

        // The dumped image at the OEP must match the original program's bytes.
        let orig_bytes = std::fs::read(&orig).unwrap();
        let orig_prog = crate::loader::Program::load(&orig_bytes).unwrap();
        let oep = r.oep;
        let orig_at_oep = orig_prog.sections.iter().find_map(|s| {
            if oep >= s.address && oep < s.address + s.data.len() as u64 {
                let off = (oep - s.address) as usize;
                Some(s.data[off..off + 16.min(s.data.len() - off)].to_vec())
            } else { None }
        }).expect("OEP not in original sections");

        let dumped_at_oep = r.image_pages.iter().find_map(|(va, bytes)| {
            if oep >= *va && oep < *va + bytes.len() as u64 {
                let off = (oep - *va) as usize;
                Some(bytes[off..off + 16.min(bytes.len() - off)].to_vec())
            } else { None }
        }).expect("OEP not in dumped image");

        assert_eq!(orig_at_oep, dumped_at_oep, "decompressed code != original at OEP");

        // The rebuilt PE must advertise the OEP as its entry point.
        let rebuilt = crate::loader::Program::load(&r.dump_pe).expect("load rebuilt PE");
        assert_eq!(rebuilt.entry, oep, "rebuilt PE entry should be the OEP");

        let _ = std::fs::remove_file(&packed);
    }
}
