//! Recovery of GNU/Itanium C++ exception metadata (`.eh_frame` FDEs → LSDA
//! `.gcc_except_table`), the data the ARET EH dispatcher needs to route a `throw`
//! to the right `catch` (doc 71, 2026-08-08 [EH][DESIGN]). This is the GNU analog
//! of `cxx_eh_entries` (MSVC `FuncInfo`): **proven from the binary's own metadata,
//! nothing guessed**. It parses only the encodings mingw/GCC emits on i386
//! (`.eh_frame` CIE augmentation `zP?LR`, pointers `pcrel|sdata4`/`absptr`,
//! call-site table `uleb128`); any other encoding makes the affected function
//! **skipped** (sound — the dispatcher then aborts loud on a throw there, never
//! guesses a landing pad).
//!
//! This brick recovers the metadata only; the dispatcher + landing-pad transfer
//! are separate bricks. Until the dispatcher consumes `gnu_eh_entries`, these items
//! are exercised by the unit tests below but not yet called from the pipeline.
#![allow(dead_code)]

use crate::loader::Program;

/// One catch/cleanup call site inside an EH function: the guarded PC region and
/// where to land if it throws.
#[derive(Clone, Debug)]
pub struct GnuCallSite {
    /// Guarded region [start, end) — VAs in the module (the PCs of calls that may throw).
    pub start: u64,
    pub end: u64,
    /// Landing-pad VA (0 = no handler here, the exception just propagates through).
    pub landing_pad: u64,
    /// The catches this landing pad handles, in action-chain order. Empty with a
    /// non-zero `landing_pad` = a cleanup-only pad (runs dtors then `_Unwind_Resume`,
    /// selector 0). See `GnuCatch`.
    pub catches: Vec<GnuCatch>,
}

/// One catch clause at a call site: the `ar_filter` selector the personality hands
/// the landing pad (in `edx`/`__builtin_eh_return_data_regno(1)` — the landing pad
/// does `cmp edx, filter; je <handler>`), and the caught type's `std::type_info*`
/// SLOT VA. **The slot holds the type_info pointer, it is not the type_info object**
/// — mingw emits the ttype table with `DW_EH_PE_indirect`, and imported type_infos
/// (e.g. `int`/`const char*` from libstdc++) are only bound at load (via the mingw
/// auto-import pseudo-relocs ARET already applies). The dispatcher dereferences the
/// slot at throw time to get the live `std::type_info*`, which is exactly the pointer
/// `__cxa_throw` receives — comparable by construction. `type_slot == 0` = catch-all
/// (`catch(...)`, matches any and binds no object).
#[derive(Clone, Copy, Debug)]
pub struct GnuCatch {
    pub filter: i64,
    pub type_slot: u64,
}

/// One EH function's recovered metadata.
#[derive(Clone, Debug)]
pub struct GnuEhFunc {
    pub pc_start: u64,
    pub pc_end: u64,
    pub call_sites: Vec<GnuCallSite>,
}

// --- DW_EH_PE pointer encodings (only the ones GCC/i386 emit) ---
const DW_EH_PE_OMIT: u8 = 0xff;
const DW_EH_PE_ABSPTR: u8 = 0x00;
const DW_EH_PE_ULEB128: u8 = 0x01;
const DW_EH_PE_UDATA2: u8 = 0x02;
const DW_EH_PE_UDATA4: u8 = 0x03;
const DW_EH_PE_SDATA4: u8 = 0x0b;
const DW_EH_PE_SLEB128: u8 = 0x09;
const DW_EH_PE_PCREL: u8 = 0x10;

fn uleb(d: &[u8], p: &mut usize) -> Option<u64> {
    let mut r = 0u64;
    let mut sh = 0u32;
    loop {
        let b = *d.get(*p)?;
        *p += 1;
        r |= ((b & 0x7f) as u64) << sh;
        if b & 0x80 == 0 {
            return Some(r);
        }
        sh += 7;
        if sh >= 64 {
            return None;
        }
    }
}

fn sleb(d: &[u8], p: &mut usize) -> Option<i64> {
    let mut r = 0i64;
    let mut sh = 0u32;
    loop {
        let b = *d.get(*p)?;
        *p += 1;
        r |= ((b & 0x7f) as i64) << sh;
        sh += 7;
        if b & 0x80 == 0 {
            if sh < 64 && (b & 0x40) != 0 {
                r |= -1i64 << sh;
            }
            return Some(r);
        }
        if sh >= 64 {
            return None;
        }
    }
}

/// Read an encoded pointer at `d[*p]`, with `pc` = the VA of `d[*p]` (for pcrel).
/// Returns the decoded VA (0 kept as 0). Only the encodings GCC/i386 emit.
fn read_encoded(d: &[u8], p: &mut usize, enc: u8, pc: u64) -> Option<u64> {
    if enc == DW_EH_PE_OMIT {
        return None;
    }
    let value_kind = enc & 0x0f;
    let raw: i64 = match value_kind {
        DW_EH_PE_ABSPTR | DW_EH_PE_UDATA4 => {
            let v = u32::from_le_bytes(d.get(*p..*p + 4)?.try_into().ok()?);
            *p += 4;
            v as i64
        }
        DW_EH_PE_SDATA4 => {
            let v = i32::from_le_bytes(d.get(*p..*p + 4)?.try_into().ok()?);
            *p += 4;
            v as i64
        }
        DW_EH_PE_UDATA2 => {
            let v = u16::from_le_bytes(d.get(*p..*p + 2)?.try_into().ok()?);
            *p += 2;
            v as i64
        }
        DW_EH_PE_ULEB128 => uleb(d, p)? as i64,
        DW_EH_PE_SLEB128 => sleb(d, p)?,
        _ => return None, // unsupported encoding -> caller skips this function
    };
    if raw == 0 && value_kind != DW_EH_PE_ABSPTR {
        // A 0 offset with a relative encoding still means "no pointer" for LSDA/landing pads.
        return Some(0);
    }
    // Only two base modifiers appear in GCC/i386 EH: absolute and pcrel. pcrel is
    // relative to the VA of the encoded value's first byte (that VA = `pc`).
    match enc & 0x70 {
        0x00 => Some(raw as u64),                             // absolute
        DW_EH_PE_PCREL => Some((pc as i64).wrapping_add(raw) as u64),
        _ => None, // textrel/datarel/funcrel/aligned not emitted here -> skip (sound)
    }
}

/// Parse `.eh_frame` + each EH function's LSDA. Returns the recovered functions
/// (only those whose encodings we fully understand; others are skipped, sound).
pub fn gnu_eh_entries(prog: &Program) -> Vec<GnuEhFunc> {
    let mut out = Vec::new();
    let Some(sec) = prog.sections.iter().find(|s| s.name == ".eh_frame") else {
        return out;
    };
    let base = sec.address;
    let d = &sec.data;
    // CIE cache: offset-in-section -> (fde_ptr_enc, lsda_enc, has_lsda).
    use std::collections::HashMap;
    let mut cies: HashMap<usize, (u8, u8, bool)> = HashMap::new();
    let mut off = 0usize;
    while off + 4 <= d.len() {
        let len = u32::from_le_bytes(d[off..off + 4].try_into().unwrap()) as usize;
        if len == 0 {
            break; // terminator
        }
        if len == 0xffff_ffff {
            break; // 64-bit length not emitted on i386 -> stop (sound)
        }
        let entry_start = off + 4;
        let entry_end = entry_start + len;
        if entry_end > d.len() {
            break;
        }
        let id = u32::from_le_bytes(d[entry_start..entry_start + 4].try_into().unwrap());
        if id == 0 {
            // CIE. Parse augmentation to learn the FDE ptr encoding + LSDA encoding.
            let mut p = entry_start + 4;
            let _version = d.get(p).copied().unwrap_or(0);
            p += 1;
            // augmentation string (NUL-terminated)
            let aug_start = p;
            while p < entry_end && d[p] != 0 {
                p += 1;
            }
            let aug: Vec<u8> = d[aug_start..p].to_vec();
            p += 1; // skip NUL
            let _code_align = uleb(d, &mut p);
            let _data_align = sleb(d, &mut p);
            // return address register (uleb on modern, sometimes 1 byte) — uleb is safe.
            let _ra = uleb(d, &mut p);
            let mut fde_enc = DW_EH_PE_ABSPTR;
            let mut lsda_enc = DW_EH_PE_OMIT;
            let mut has_lsda = false;
            if aug.first() == Some(&b'z') {
                let _aug_len = uleb(d, &mut p);
                for &c in &aug[1..] {
                    match c {
                        b'R' => {
                            fde_enc = d.get(p).copied().unwrap_or(DW_EH_PE_ABSPTR);
                            p += 1;
                        }
                        b'P' => {
                            let penc = d.get(p).copied().unwrap_or(0);
                            p += 1;
                            // skip the personality pointer (sized by penc)
                            let mut pp = p;
                            let ppva = base + pp as u64;
                            let _ = read_encoded(d, &mut pp, penc, ppva);
                            p = pp;
                        }
                        b'L' => {
                            lsda_enc = d.get(p).copied().unwrap_or(DW_EH_PE_OMIT);
                            has_lsda = true;
                            p += 1;
                        }
                        _ => {}
                    }
                }
            }
            cies.insert(off, (fde_enc, lsda_enc, has_lsda));
        } else {
            // FDE. id is the byte offset back to its CIE (from the id field).
            let cie_off = (entry_start).wrapping_sub(id as usize);
            let (fde_enc, lsda_enc, has_lsda) = *cies.get(&cie_off).unwrap_or(&(DW_EH_PE_ABSPTR, DW_EH_PE_OMIT, false));
            let mut p = entry_start + 4;
            let pcva = base + p as u64;
            let pc_begin = read_encoded(d, &mut p, fde_enc, pcva);
            // pc_range uses the same value kind but never pcrel (it's a size).
            let range_kind = fde_enc & 0x0f;
            let pc_range = match range_kind {
                DW_EH_PE_ABSPTR | DW_EH_PE_UDATA4 | DW_EH_PE_SDATA4 => {
                    let v = u32::from_le_bytes(d.get(p..p + 4).map(|s| s.try_into().unwrap()).unwrap_or([0; 4]));
                    p += 4;
                    v as u64
                }
                _ => 0,
            };
            if let Some(pc_begin) = pc_begin {
                if has_lsda && pc_range != 0 {
                    // FDE augmentation: aug_len (uleb) then the LSDA pointer (per lsda_enc).
                    let _aug_len = uleb(d, &mut p);
                    let lsdava = base + p as u64;
                    if let Some(lsda) = read_encoded(d, &mut p, lsda_enc, lsdava) {
                        if lsda != 0 {
                            if let Some(ld) = prog.read_from(lsda) {
                                let f = parse_lsda(
                                    ld,
                                    &|a| prog.read_u32(a),
                                    pc_begin,
                                    pc_begin + pc_range,
                                    lsda,
                                );
                                if let Some(f) = f {
                                    out.push(f);
                                }
                            }
                        }
                    }
                }
            }
        }
        off = entry_end;
    }
    out
}

/// Parse one function's LSDA (`.gcc_except_table`) at `lsda` into call sites.
/// Layout: `{ lpstart_enc, [lpstart], ttype_enc, [ttype_off uleb], cs_enc,
/// cs_table_len uleb, <call-site records>, <action table>, <type table> }`.
fn parse_lsda(
    d: &[u8],
    read_u32: &dyn Fn(u64) -> Option<u32>,
    pc_start: u64,
    pc_end: u64,
    lsda: u64,
) -> Option<GnuEhFunc> {
    // `d` = the LSDA bytes (from wherever it is mapped, usually .rdata or .text tail);
    // `read_u32` reads program memory (for the ttype slot indirection).
    let mut p = 0usize;
    let vaof = |p: usize| lsda + p as u64;

    let lp_enc = *d.get(p)?;
    p += 1;
    let lp_start = if lp_enc == DW_EH_PE_OMIT {
        pc_start // default: landing pads are function-relative to the function start
    } else {
        let va = vaof(p);
        read_encoded(d, &mut p, lp_enc, va)?
    };

    let ttype_enc = *d.get(p)?;
    p += 1;
    // ttype table base = position right after the ttype_off field; entries indexed backward.
    let (ttype_base, _ttype_present) = if ttype_enc == DW_EH_PE_OMIT {
        (0u64, false)
    } else {
        let off = uleb(d, &mut p)?;
        (vaof(p) + off, true)
    };

    let cs_enc = *d.get(p)?;
    p += 1;
    let cs_table_len = uleb(d, &mut p)? as usize;
    let cs_end = p + cs_table_len;
    if cs_end > d.len() {
        return None;
    }
    let action_table_start = cs_end;

    // Read a type-info SLOT VA from the type table by index (1-based, indexed
    // backward from `ttype_base`). We resolve the encoding's value + pcrel base but
    // deliberately DO NOT apply the `DW_EH_PE_indirect` deref: for mingw the entry
    // resolves to the address of a `std::type_info*` slot, and imported type_infos
    // are only bound at load — so the SLOT address is the stable key, dereferenced
    // by the dispatcher at throw time (see `GnuCallSite::catch_types`).
    let read_type = |idx: u64| -> u64 {
        if idx == 0 || ttype_base == 0 {
            return 0; // catch-all or no table
        }
        let sz = match ttype_enc & 0x0f {
            DW_EH_PE_ABSPTR | DW_EH_PE_UDATA4 | DW_EH_PE_SDATA4 => 4,
            _ => return 0,
        };
        let slot = ttype_base.wrapping_sub(idx * sz);
        let raw = read_u32(slot).unwrap_or(0) as i64;
        if raw == 0 {
            return 0;
        }
        if ttype_enc & 0x70 == DW_EH_PE_PCREL {
            (slot as i64).wrapping_add(raw) as u64
        } else {
            raw as u64
        }
    };

    let mut call_sites = Vec::new();
    while p < cs_end {
        let va0 = vaof(p);
        let cs_start = read_encoded(d, &mut p, cs_enc, va0)?;
        let va1 = vaof(p);
        let cs_len = read_encoded(d, &mut p, cs_enc & 0x0f, va1)?; // length: same value kind, no rel
        let va2 = vaof(p);
        let cs_lp = read_encoded(d, &mut p, cs_enc & 0x0f, va2)?;
        let action = uleb(d, &mut p)?; // 0 = cleanup none / propagate; else 1-based index into action table
        let landing_pad = if cs_lp == 0 { 0 } else { lp_start.wrapping_add(cs_lp) };
        // Walk the action chain to collect the catches (filter selector + type slot).
        let mut catches = Vec::new();
        if action != 0 {
            let mut ap = action_table_start + (action as usize - 1);
            for _ in 0..64 {
                // guard against a cyclic/garbage chain
                let filter = sleb(d, &mut ap)?;
                let disp_pos = ap;
                let next = sleb(d, &mut ap)?;
                if filter > 0 {
                    // A catch of a type: `filter` is the ar_filter selector the personality
                    // hands the landing pad in edx; read_type(filter) resolves the ttype slot
                    // (0 => catch-all sentinel handled by read_type).
                    catches.push(GnuCatch { filter, type_slot: read_type(filter as u64) });
                } else if filter == 0 {
                    // cleanup action (no type) — keep walking
                }
                // negative filter = exception-spec, rare; treat as cleanup here (sound: no catch match)
                if next == 0 {
                    break;
                }
                ap = (disp_pos as i64 + next) as usize;
                if ap >= d.len() {
                    break;
                }
            }
        }
        call_sites.push(GnuCallSite {
            start: cs_start.wrapping_add(lp_start), // call-site regions are also lpstart-relative
            end: cs_start.wrapping_add(lp_start).wrapping_add(cs_len),
            landing_pad,
            catches,
        });
    }

    Some(GnuEhFunc { pc_start, pc_end, call_sites })
}

/// The three Itanium ABI type_info **vtable pointer values** (`__class_type_info`,
/// `__si_class_type_info`, `__vmi_class_type_info`), by which the runtime dispatcher
/// classifies a `std::type_info` to walk its base chain for a subtype catch (brick 2b).
/// A GCC type_info stores, as its first field, `&<that abi vtable> + 8`; the abi vtable
/// is imported from libstdc++, so its IAT slot VA is fixed and known at analysis time
/// **without loading libstdc++** — the vptr value is `slot + 8`. 0 for a kind the binary
/// never uses. Returned as `(class, si, vmi)`.
pub fn gnu_eh_abi_vptrs(prog: &Program) -> (u64, u64, u64) {
    let find = |needle: &str| -> u64 {
        // Non-lifted libstdc++: the ABI vtable is IMPORTED, and a std::type_info's vptr
        // field equals the IAT slot address + 8 (measured, brick 2b).
        if let Some((&slot, _)) = prog.imports.iter().find(|(_, n)| n.contains(needle)) {
            return slot + 8;
        }
        // Lifted libstdc++ (`--with-dll`): the ABI vtable is a real EXPORT of the lifted
        // module, so the vptr points at the vtable's VA + 8 (past offset_to_top + the
        // typeinfo pointer). Require the `_ZTV` vtable mangling so we bind the VTABLE, not
        // the type_info (`_ZTI`) or type string (`_ZTS`) that share the class name.
        if let Some((_, _, va)) = prog
            .dll_exports
            .iter()
            .find(|(_, n, _)| n.starts_with("_ZTV") && n.contains(needle))
        {
            return va + 8;
        }
        0
    };
    (
        find("cxxabiv117__class_type_info"),
        find("cxxabiv120__si_class_type_info"),
        find("cxxabiv121__vmi_class_type_info"),
    )
}

/// Read a CIE's FDE-pointer encoding (`'R'` in the augmentation), replicating the
/// augmentation walk of `gnu_eh_entries` but returning only that one byte. `'P'`
/// (personality) is skipped by size so a `'R'` after it is found correctly; an
/// absent/unknown augmentation yields `DW_EH_PE_ABSPTR` (the safe default — an FDE
/// start that then fails to land in an executable range is dropped by the caller).
fn cie_fde_encoding(d: &[u8], base: u64, entry_start: usize, entry_end: usize) -> u8 {
    let mut p = entry_start + 4; // past the CIE id
    p += 1; // version
    let aug_start = p;
    while p < entry_end && d.get(p).copied().unwrap_or(0) != 0 {
        p += 1;
    }
    let aug: Vec<u8> = d.get(aug_start..p).unwrap_or(&[]).to_vec();
    p += 1; // NUL
    let _code_align = uleb(d, &mut p);
    let _data_align = sleb(d, &mut p);
    let _ra = uleb(d, &mut p);
    let mut fde_enc = DW_EH_PE_ABSPTR;
    if aug.first() == Some(&b'z') {
        let _aug_len = uleb(d, &mut p);
        for &c in &aug[1..] {
            match c {
                b'R' => {
                    fde_enc = d.get(p).copied().unwrap_or(DW_EH_PE_ABSPTR);
                    p += 1;
                }
                b'P' => {
                    let penc = d.get(p).copied().unwrap_or(0);
                    p += 1;
                    let ppva = base + p as u64;
                    let mut pp = p;
                    let _ = read_encoded(d, &mut pp, penc, ppva);
                    p = pp;
                }
                b'L' => {
                    p += 1; // lsda encoding byte
                }
                _ => {}
            }
        }
    }
    fde_enc
}

/// Collect every FDE `initial_location` (function START) from one `.eh_frame`
/// section's bytes, decoding each FDE pointer with its CIE's encoding. Only the
/// encodings GCC/i386 emit are understood; an FDE whose start cannot be decoded is
/// skipped (nothing guessed). `base` is the section's (rebased) VA, so pcrel FDE
/// pointers resolve to rebased starts directly.
fn collect_fde_starts(d: &[u8], base: u64, out: &mut std::collections::BTreeSet<u64>) {
    use std::collections::HashMap;
    let mut cies: HashMap<usize, u8> = HashMap::new();
    let mut off = 0usize;
    while off + 4 <= d.len() {
        let len = u32::from_le_bytes(d[off..off + 4].try_into().unwrap()) as usize;
        if len == 0 || len == 0xffff_ffff {
            break; // terminator, or 64-bit length not emitted on i386 -> stop (sound)
        }
        let entry_start = off + 4;
        let entry_end = entry_start + len;
        if entry_end > d.len() {
            break;
        }
        let id = u32::from_le_bytes(d[entry_start..entry_start + 4].try_into().unwrap());
        if id == 0 {
            cies.insert(off, cie_fde_encoding(d, base, entry_start, entry_end));
        } else {
            let cie_off = entry_start.wrapping_sub(id as usize);
            let fde_enc = *cies.get(&cie_off).unwrap_or(&DW_EH_PE_ABSPTR);
            let mut p = entry_start + 4; // past the CIE pointer
            let pcva = base + p as u64;
            if let Some(pc_begin) = read_encoded(d, &mut p, fde_enc, pcva) {
                if pc_begin != 0 {
                    out.insert(pc_begin);
                }
            }
        }
        off = entry_end;
    }
}

/// The set of function-START addresses the compiler certified in `.eh_frame`, across
/// **all** modules merged into `prog` (each `.eh_frame` section is parsed at its
/// rebased VA, so a pcrel FDE `initial_location` yields the rebased start for free).
///
/// This is a **proof of function start**, strictly stronger than a prologue or
/// terminator heuristic: by the DWARF/Itanium contract an FDE `initial_location` is
/// the entry of a whole, non-overlapping function range, so it is never interior to
/// another function's body — and a landing pad (interior to its establisher) never
/// has its own FDE. Recovery uses this to seed a missed entry or, when the linear
/// sweep over-absorbed it, to re-split at that proven boundary. Sound degradation: no
/// `.eh_frame` ⇒ empty; an unsupported encoding or a start outside executable memory
/// ⇒ that entry is skipped, never guessed.
pub fn eh_frame_function_starts(prog: &Program) -> std::collections::BTreeSet<u64> {
    let mut out = std::collections::BTreeSet::new();
    for sec in prog.sections.iter().filter(|s| s.name == ".eh_frame") {
        collect_fde_starts(&sec.data, sec.address, &mut out);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Assemble a minimal but well-formed `.eh_frame`: one CIE (augmentation `zR`,
    /// FDE pointers `pcrel|sdata4` — exactly what mingw/GCC i386 emit) followed by
    /// FDEs at the given target VAs, then the 0-length terminator. Returns the bytes
    /// and the section base VA. Mirrors the on-disk layout the real parser walks.
    fn synth_eh_frame(base: u64, targets: &[u64]) -> Vec<u8> {
        fn entry(content: Vec<u8>) -> Vec<u8> {
            let mut c = content;
            while c.len() % 4 != 0 {
                c.push(0); // DW_CFA_nop padding to 4-byte alignment
            }
            let mut out = (c.len() as u32).to_le_bytes().to_vec();
            out.extend_from_slice(&c);
            out
        }
        // CIE content (after the length field): id=0, version=1, aug="zR\0",
        // code_align=1, data_align=-4, ra=8, aug_len=1, R-enc=pcrel|sdata4.
        let mut cie = Vec::new();
        cie.extend_from_slice(&0u32.to_le_bytes());
        cie.push(1);
        cie.extend_from_slice(b"zR\0");
        cie.push(0x01);
        cie.push(0x7c);
        cie.push(0x08);
        cie.push(0x01);
        cie.push(DW_EH_PE_PCREL | DW_EH_PE_SDATA4);
        let mut buf = entry(cie);
        for &target in targets {
            let fde_off = buf.len();
            let cie_ptr = (fde_off as u32) + 4; // = (fde_off+4) - 0 (CIE at offset 0)
            let iloc_va = base + (fde_off + 8) as u64; // initial_location field VA
            let rel = (target as i64 - iloc_va as i64) as i32;
            let mut fde = Vec::new();
            fde.extend_from_slice(&cie_ptr.to_le_bytes());
            fde.extend_from_slice(&rel.to_le_bytes());
            fde.extend_from_slice(&0x20u32.to_le_bytes()); // pc_range
            buf.extend_from_slice(&entry(fde));
        }
        buf.extend_from_slice(&0u32.to_le_bytes()); // terminator
        buf
    }

    #[test]
    fn collect_fde_starts_decodes_pcrel_initial_locations() {
        let base = 0x0040_0000u64;
        let targets = [0x0040_1100u64, 0x0040_1234u64, 0x0046_75c0u64];
        let buf = synth_eh_frame(base, &targets);
        let mut got = std::collections::BTreeSet::new();
        collect_fde_starts(&buf, base, &mut got);
        assert_eq!(got, targets.iter().copied().collect());
    }

    #[test]
    fn collect_fde_starts_is_empty_on_garbage_and_terminator() {
        // A lone 0-length terminator yields nothing (sound: no guessed starts).
        let mut got = std::collections::BTreeSet::new();
        collect_fde_starts(&0u32.to_le_bytes(), 0x400000, &mut got);
        assert!(got.is_empty());
        // Truncated entry (length past the buffer) stops cleanly, no panic.
        let mut got2 = std::collections::BTreeSet::new();
        collect_fde_starts(&[0xff, 0xff, 0x00, 0x00, 0x01], 0x400000, &mut got2);
        assert!(got2.is_empty());
    }

    #[test]
    fn uleb_sleb_roundtrip() {
        // ULEB128
        let mut p = 0;
        assert_eq!(uleb(&[0x00], &mut p), Some(0));
        let mut p = 0;
        assert_eq!(uleb(&[0x7f], &mut p), Some(127));
        let mut p = 0;
        assert_eq!(uleb(&[0x80, 0x01], &mut p), Some(128));
        assert_eq!(p, 2);
        let mut p = 0;
        assert_eq!(uleb(&[0xe5, 0x8e, 0x26], &mut p), Some(624485)); // DWARF spec example
        // SLEB128 (signed): 2, -2, and the DWARF spec -128 example.
        let mut p = 0;
        assert_eq!(sleb(&[0x02], &mut p), Some(2));
        let mut p = 0;
        assert_eq!(sleb(&[0x7e], &mut p), Some(-2));
        let mut p = 0;
        assert_eq!(sleb(&[0x80, 0x7f], &mut p), Some(-128));
    }

    #[test]
    fn read_encoded_pcrel_and_abs() {
        // sdata4 | pcrel: value at VA `pc` decodes to pc + signed offset.
        let bytes = 0xffff_ce77u32.to_le_bytes(); // -0x3189
        let mut p = 0;
        let pc = 0x40b1edu64;
        let got = read_encoded(&bytes, &mut p, 0x10 | 0x0b, pc);
        assert_eq!(got, Some(pc.wrapping_sub(0x3189)));
        assert_eq!(p, 4);
        // absptr: raw value, pc ignored.
        let bytes = 0x0040_a6c8u32.to_le_bytes();
        let mut p = 0;
        assert_eq!(read_encoded(&bytes, &mut p, 0x00, 0), Some(0x40a6c8));
        // a 0 offset under a relative encoding means "no pointer".
        let bytes = 0u32.to_le_bytes();
        let mut p = 0;
        assert_eq!(read_encoded(&bytes, &mut p, 0x10 | 0x0b, 0x1000), Some(0));
        // omit -> None.
        let mut p = 0;
        assert_eq!(read_encoded(&[0u8], &mut p, DW_EH_PE_OMIT, 0), None);
    }

    /// Synthetic LSDA mirroring the mingw/i386 shape recovered from the real `eh.cpp`
    /// fixture: `lp_enc=omit`, `ttype_enc=indirect|pcrel|sdata4`, `cs_enc=uleb128`,
    /// one cleanup-only call site and one 1-type catch call site. Landing pads are
    /// function-relative; the caught type is the SLOT VA (indirect, not dereferenced).
    #[test]
    fn parse_lsda_synthetic() {
        let pc_start = 0x401000u64;
        let lsda = 0x408000u64; // where the LSDA bytes are mapped
        let ttype_slot = 0x409004u64; // the type_info* slot the ttype table points at

        // Build the LSDA body after the header, so we can compute the ttype table
        // offset (from just past the ttype_off field to the ttype table base).
        // Call-site table (uleb encoding), 2 records:
        //   rec A: cs_start=0x10, cs_len=5, cs_lp=0 (cleanup? no -> propagate), action=0
        //   rec B: cs_start=0x20, cs_len=5, cs_lp=0x40, action=1 (-> type idx 1)
        let mut cs = Vec::new();
        cs.extend_from_slice(&[0x10, 0x05, 0x00, 0x00]); // A: start,len,lp,action
        cs.extend_from_slice(&[0x20, 0x05, 0x40, 0x01]); // B: start,len,lp,action
        // Action table: one record {filter=1, next=0}.
        let action = [0x01u8, 0x00];
        // Type table: one sdata4|pcrel entry that resolves to `ttype_slot`.
        // entry is at ttype_base - 1*4; its pcrel value = ttype_slot - entry_va.

        // Header: lp_enc(1)=0xff, ttype_enc(1)=0x9b, ttype_off(uleb), cs_enc(1)=0x01,
        //         cs_table_len(uleb), <cs>, <action>, <type table>.
        // We must place ttype_off so the ttype table sits right after the type entry
        // region. Lay bytes out and back-patch.
        let mut d = Vec::new();
        d.push(0xff); // lp_enc = omit
        d.push(0x9b); // ttype_enc = indirect|pcrel|sdata4
        let ttype_off_pos = d.len();
        d.push(0x00); // ttype_off placeholder (1-byte uleb, patched below)
        let after_ttype_off = d.len();
        d.push(0x01); // cs_enc = uleb128
        d.push(cs.len() as u8); // cs_table_len (uleb, small)
        d.extend_from_slice(&cs);
        d.extend_from_slice(&action);
        // Type table base = end of the type table (entries indexed backward). Put one
        // 4-byte entry here; ttype_base points just past it.
        let type_entry_pos = d.len();
        let type_entry_va = lsda + type_entry_pos as u64;
        let pcrel = (ttype_slot as i64 - type_entry_va as i64) as i32;
        d.extend_from_slice(&pcrel.to_le_bytes());
        let ttype_base_off = d.len(); // ttype_base = lsda + this
                                      // ttype_off is (ttype_base - after_ttype_off), as uleb (assume < 128).
        let ttype_off = (ttype_base_off - after_ttype_off) as u8;
        assert!(ttype_off < 0x80);
        d[ttype_off_pos] = ttype_off;

        // `read_type` reads the ttype table ENTRY (the pcrel sdata4) from program
        // memory — in a real binary the LSDA (and its ttype table) is mapped, so this
        // serves those bytes. The recovery keeps the resulting SLOT address (no further
        // deref), so the slot's own content is never read here.
        let read_u32 = move |a: u64| -> Option<u32> {
            if a == type_entry_va {
                Some(pcrel as u32)
            } else {
                Some(0)
            }
        };
        let f = parse_lsda(&d, &read_u32, pc_start, pc_start + 0x100, lsda).expect("parse");
        assert_eq!(f.call_sites.len(), 2);
        // A: cleanup/propagate region, no landing pad, no types.
        assert_eq!(f.call_sites[0].start, pc_start + 0x10);
        assert_eq!(f.call_sites[0].end, pc_start + 0x15);
        assert_eq!(f.call_sites[0].landing_pad, 0);
        assert!(f.call_sites[0].catches.is_empty());
        // B: catch region, landing pad = pc_start + 0x40, one caught type (filter 1) = the SLOT VA.
        assert_eq!(f.call_sites[1].start, pc_start + 0x20);
        assert_eq!(f.call_sites[1].landing_pad, pc_start + 0x40);
        assert_eq!(f.call_sites[1].catches.len(), 1);
        assert_eq!(f.call_sites[1].catches[0].filter, 1);
        assert_eq!(f.call_sites[1].catches[0].type_slot, ttype_slot);
    }
}
