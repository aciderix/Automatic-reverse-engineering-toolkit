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
}

extern "C" fn on_write(_uc: *mut uc_engine, _ty: c_int, addr: u64, _sz: c_int, _val: i64, ud: *mut c_void) {
    let t = unsafe { &mut *(ud as *mut Trace) };
    t.written.insert(page_down(addr));
}

extern "C" fn on_code(uc: *mut uc_engine, addr: u64, _sz: c_int, ud: *mut c_void) {
    let t = unsafe { &mut *(ud as *mut Trace) };
    let pg = page_down(addr);
    // Executing from a page we saw written this run, and which was not part of
    // the original stub: freshly-decrypted code => OEP reached.
    if t.written.contains(&pg) && !t.initial_code.contains(&pg) {
        t.oep = Some(addr);
        unsafe { uc_emu_stop(uc); }
    }
}

extern "C" fn on_unmapped(uc: *mut uc_engine, _ty: c_int, addr: u64, _sz: c_int, _val: i64, ud: *mut c_void) -> bool {
    let t = unsafe { &mut *(ud as *mut Trace) };
    // A near-null access is almost always a call/read through an unresolved
    // import thunk: that needs a Win32 model, so stop and report it.
    if addr >= 0x1_0000 && t.lazy_budget > 0 {
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
pub fn emulate_until_oep(
    regions: &[Region],
    entry: u64,
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

        let mut trace = Box::new(Trace {
            written: BTreeSet::new(),
            initial_code: BTreeSet::new(),
            oep: None,
            faulted: None,
            lazy_budget: 256,
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
                    "stub reached unmapped {:#x} before decrypting — needs an API/Win32 model",
                    f
                ));
            }
        }
        let oep = trace.oep.ok_or_else(|| {
            "no OEP detected (no self-modified code executed within the instruction budget)".to_string()
        })?;

        // Dump the (now decrypted) memory back out for every region.
        let mut out = Vec::new();
        for r in regions {
            let mut buf = vec![0u8; r.bytes.len()];
            if uc_mem_read(uc, r.va, buf.as_mut_ptr() as *mut c_void, buf.len()) == 0 {
                out.push((r.va, buf));
            }
        }
        // eax is sometimes used by stubs; read to keep the API exercised/tested.
        let mut _eax: u32 = 0;
        uc_reg_read(uc, UC_X86_REG_EAX, &mut _eax as *mut u32 as *mut c_void);

        Ok(Unpacked { oep, regions: out })
    }
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
    let res = emulate_until_oep(&regions, prog.entry, max_insns)?;

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
    Ok(UnpackReport { oep: res.oep, decrypted_bytes: changed, total_bytes: total, image: res.regions })
}

pub struct UnpackReport {
    pub oep: u64,
    pub decrypted_bytes: usize,
    pub total_bytes: usize,
    pub image: Vec<(u64, Vec<u8>)>,
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

        let r = emulate_until_oep(&regions, stub_va, 1_000_000).expect("unpack");
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
        let err = emulate_until_oep(&regions, va, 1000).unwrap_err();
        assert!(err.contains("no OEP"), "unexpected: {err}");
    }
}
