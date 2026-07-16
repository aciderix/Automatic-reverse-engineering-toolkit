//! FLIRT-lite — recognize statically-linked library functions by byte pattern,
//! so the CRT (and mingw startup glue) can be bound to the native runtime even
//! when the binary is **stripped** (no symbols). This is the symbol-free form of
//! `Program::crt_symbol`: IDA's FLIRT, scoped to what ARET needs.
//!
//! A signature is the first N bytes of a function with link-variant bytes
//! wildcarded — the operands of relative `call`/`jmp`/`jcc` (which encode a
//! displacement that changes with the link address). The stable prologue +
//! body opcodes identify the function; the displacements are masked out.
//!
//! The bundled database (`runtime/flirt/mingw_crt.sig`) is generated once from a
//! reference mingw binary that *has* symbols (`--mode gensig`); since every
//! mingw binary shares the identical library code, the same signatures match
//! across binaries.

use std::sync::OnceLock;

/// One recognized function: a masked byte pattern and the symbol it identifies.
struct Sig {
    name: String,
    bytes: Vec<u8>,
    mask: Vec<bool>, // true = must match, false = wildcard
}

/// A loaded signature database.
pub struct FlirtDb {
    sigs: Vec<Sig>,
}

/// How many leading bytes of a function to use for a signature.
const SIG_LEN: usize = 32;

/// Wildcard the 4-byte operand of relative branches in `code[..n]`, returning the
/// per-byte "must match" mask. The opcodes themselves stay significant.
fn branch_mask(code: &[u8]) -> Vec<bool> {
    let mut mask = vec![true; code.len()];
    let mut i = 0;
    while i < code.len() {
        let b = code[i];
        if b == 0xE8 || b == 0xE9 {
            // call/jmp rel32: wildcard the 4 displacement bytes.
            for j in i + 1..(i + 5).min(code.len()) {
                mask[j] = false;
            }
            i += 5;
        } else if b == 0x0F && i + 1 < code.len() && (0x80..=0x8F).contains(&code[i + 1]) {
            // jcc rel32 (two-byte opcode): wildcard the 4 displacement bytes.
            for j in i + 2..(i + 6).min(code.len()) {
                mask[j] = false;
            }
            i += 6;
        } else {
            i += 1;
        }
    }
    mask
}

/// Build a signature line (`name hex-with-..wildcards`) from a function's bytes.
///
/// `reloc` marks, per byte, whether that byte is covered by a base relocation —
/// i.e. part of an absolute address the loader patches (`mov reg,[abs32]`,
/// `push offset`, a rebased pointer). Those bytes differ between binaries, so
/// they are wildcarded alongside the relative-branch operands; otherwise a
/// signature over-pins the *one* binary it was generated from and fails to match
/// any other (the `__pei386_runtime_relocator` miss on stripped mingw output).
pub fn gen_signature(name: &str, code: &[u8], reloc: &[bool]) -> Option<String> {
    let n = code.len().min(SIG_LEN);
    if n < 8 {
        return None; // too short to be distinctive
    }
    let code = &code[..n];
    // An import thunk / tail-call stub — `jmp [IAT]` (ff 25), `call [mem]` (ff 15),
    // `jmp rel32` (e9/eb) — carries no identity: its one variable operand is a
    // relocated address, wildcarded away, leaving a pattern that matches *every*
    // thunk (e.g. atoi's thunk matched `__lc_codepage_func`, misbinding a call to
    // the real atoi → crash). Thunks are already resolved structurally via
    // `import_thunk`, so never signature one.
    if matches!(code[0], 0xe9 | 0xeb)
        || (code[0] == 0xff && matches!(code.get(1), Some(0x25) | Some(0x15)))
    {
        return None;
    }
    let mut mask = branch_mask(code);
    for (i, m) in mask.iter_mut().enumerate() {
        if reloc.get(i).copied().unwrap_or(false) {
            *m = false; // relocated (absolute) byte — wildcard it
        }
    }
    let mut hex = String::new();
    for (b, &keep) in code.iter().zip(mask.iter()) {
        if keep {
            hex.push_str(&format!("{b:02x}"));
        } else {
            hex.push_str("..");
        }
    }
    Some(format!("{name} {hex}"))
}

impl FlirtDb {
    /// Parse the bundled text database (one `name hexpattern` per line; `..` is a
    /// wildcard byte). Lines starting with `#` or blank are ignored.
    pub fn parse(text: &str) -> FlirtDb {
        let mut sigs = Vec::new();
        for line in text.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let Some((name, hex)) = line.split_once(char::is_whitespace) else { continue };
            let hex = hex.trim();
            if hex.len() % 2 != 0 {
                continue;
            }
            let mut bytes = Vec::new();
            let mut mask = Vec::new();
            let h = hex.as_bytes();
            let mut ok = true;
            let mut i = 0;
            while i + 1 < h.len() {
                let pair = &hex[i..i + 2];
                if pair == ".." {
                    bytes.push(0);
                    mask.push(false);
                } else if let Ok(b) = u8::from_str_radix(pair, 16) {
                    bytes.push(b);
                    mask.push(true);
                } else {
                    ok = false;
                    break;
                }
                i += 2;
            }
            if ok && !bytes.is_empty() {
                sigs.push(Sig { name: name.to_string(), bytes, mask });
            }
        }
        FlirtDb { sigs }
    }

    /// Recognize the function whose code begins at `code`: the signature whose
    /// masked pattern matches the most leading bytes. If two signatures with
    /// *different* names match equally well (e.g. mingw `sprintf`/`fprintf`,
    /// byte-identical but for the wildcarded `call __mingw_v{s,f}printf` target),
    /// the match is ambiguous and we return `None` — binding the wrong CRT
    /// function would be an incorrect result presented as correct.
    pub fn match_at(&self, code: &[u8]) -> Option<&str> {
        let mut best: Option<(&str, usize)> = None;
        let mut ambiguous = false;
        for s in &self.sigs {
            if code.len() < s.bytes.len() {
                continue;
            }
            let hit = s
                .bytes
                .iter()
                .zip(s.mask.iter())
                .enumerate()
                .all(|(i, (b, &keep))| !keep || code[i] == *b);
            if !hit {
                continue;
            }
            let len = s.mask.iter().filter(|&&m| m).count();
            match best {
                None => best = Some((s.name.as_str(), len)),
                Some((bn, bl)) => {
                    if len > bl {
                        best = Some((s.name.as_str(), len));
                        ambiguous = false;
                    } else if len == bl && s.name != bn {
                        ambiguous = true;
                    }
                }
            }
        }
        if ambiguous {
            return None;
        }
        best.map(|(n, _)| n)
    }

    pub fn len(&self) -> usize {
        self.sigs.len()
    }
    pub fn is_empty(&self) -> bool {
        self.sigs.is_empty()
    }
}

/// The bundled CRT signature database, parsed once: the mingw CRT/libm set plus
/// the MSVC static-CRT intrinsics (e.g. the hand-assembled `memmove` whose
/// interleaved jump tables don't lift — bound to the native shim instead).
const BUNDLED: &str = concat!(
    include_str!("../runtime/flirt/mingw_crt.sig"),
    "\n",
    include_str!("../runtime/flirt/msvc_crt.sig"),
);

pub fn bundled() -> &'static FlirtDb {
    static DB: OnceLock<FlirtDb> = OnceLock::new();
    DB.get_or_init(|| FlirtDb::parse(BUNDLED))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip_match_with_branch_wildcard() {
        // push ebp; mov ebp,esp; call rel32; ret  — two binaries differ only in
        // the call displacement; the signature must match both.
        let a = [0x55, 0x89, 0xe5, 0xe8, 0x11, 0x22, 0x33, 0x44, 0xc3];
        let b = [0x55, 0x89, 0xe5, 0xe8, 0xaa, 0xbb, 0xcc, 0xdd, 0xc3];
        let line = gen_signature("_demo", &a, &[false; 9]).unwrap();
        let db = FlirtDb::parse(&line);
        assert_eq!(db.match_at(&a), Some("_demo"));
        assert_eq!(db.match_at(&b), Some("_demo"), "branch displacement must be wildcarded");
        // A different prologue must not match.
        let c = [0x53, 0x89, 0xe5, 0xe8, 0x11, 0x22, 0x33, 0x44, 0xc3];
        assert_eq!(db.match_at(&c), None);
    }

    #[test]
    fn ambiguous_match_returns_none() {
        // mingw `sprintf`/`fprintf` are byte-identical but for the wildcarded
        // `call __mingw_v{s,f}printf` target, so both signatures match the same
        // code equally well. Binding either would be a guess → must be None.
        let code = [0x83, 0xec, 0x1c, 0xe8, 0x11, 0x22, 0x33, 0x44, 0xc3];
        let f = gen_signature("_fprintf", &code, &[false; 9]).unwrap();
        let s = gen_signature("_sprintf", &code, &[false; 9]).unwrap();
        let db = FlirtDb::parse(&format!("{f}\n{s}"));
        assert_eq!(db.match_at(&code), None, "ambiguous CRT match must not guess");
        // A single signature for the same bytes is unambiguous.
        let db1 = FlirtDb::parse(&f);
        assert_eq!(db1.match_at(&code), Some("_fprintf"));
    }

    #[test]
    fn absolute_reloc_operand_is_wildcarded() {
        // push ebp; mov ebp,esp; mov edi,[abs32]; ret — two binaries link the
        // absolute operand to different addresses (the 4 bytes are base-relocated).
        // The signature must wildcard them and match both.
        let a = [0x55, 0x89, 0xe5, 0x8b, 0x3d, 0x6c, 0x40, 0x44, 0x00, 0xc3];
        let b = [0x55, 0x89, 0xe5, 0x8b, 0x3d, 0x68, 0xc0, 0x40, 0x00, 0xc3];
        // Bytes 5..9 (the disp32 of `mov edi,[abs32]`) are relocation-covered.
        let mut reloc = [false; 10];
        for m in reloc.iter_mut().take(9).skip(5) {
            *m = true;
        }
        let line = gen_signature("_relreloc", &a, &reloc).unwrap();
        let db = FlirtDb::parse(&line);
        assert_eq!(db.match_at(&a), Some("_relreloc"));
        assert_eq!(db.match_at(&b), Some("_relreloc"), "absolute operand must be wildcarded");
    }

    #[test]
    fn thunk_functions_get_no_signature() {
        // `jmp [IAT]; nop…` (an import thunk) and `jmp rel32` (tail stub): the
        // only identity is a relocated operand, so a signature would match every
        // thunk. gen_signature must refuse them (they resolve via import_thunk).
        let jmp_iat = [0xff, 0x25, 0x88, 0x52, 0x44, 0x00, 0x90, 0x90, 0x90];
        assert_eq!(gen_signature("_atoi", &jmp_iat, &[false; 9]), None);
        let call_mem = [0xff, 0x15, 0x00, 0x10, 0x40, 0x00, 0xc3, 0x90, 0x90];
        assert_eq!(gen_signature("_x", &call_mem, &[false; 9]), None);
        let jmp_rel = [0xe9, 0x11, 0x22, 0x33, 0x44, 0x90, 0x90, 0x90, 0x90];
        assert_eq!(gen_signature("_y", &jmp_rel, &[false; 9]), None);
        // A real prologue is still signatured.
        let real = [0x55, 0x89, 0xe5, 0x83, 0xec, 0x18, 0x8b, 0x45, 0x08];
        assert!(gen_signature("_real", &real, &[false; 9]).is_some());
    }

    #[test]
    fn bundled_db_loads() {
        // The committed database must parse and be non-trivial.
        assert!(bundled().len() >= 4, "bundled FLIRT db too small");
    }

    #[test]
    fn bundled_recognises_msvc_memmove() {
        // The MSVC static-CRT `memmove` intrinsic prologue (32 bytes: load
        // (dst,src,n), the overlap check, then the alignment dispatch). Proven
        // behaviourally equal to libc memmove (Unicorn, 500/500 random cases incl.
        // overlap). The bundled db must bind it to `memmove` so its interleaved,
        // unliftable jump tables are host-backed instead of aborting. The trailing
        // jcc rel32 displacement is wildcarded, so any link address matches.
        let mut code = [
            0x55, 0x8b, 0xec, 0x57, 0x56, 0x8b, 0x75, 0x0c, 0x8b, 0x4d, 0x10, 0x8b, 0x7d, 0x08,
            0x8b, 0xc1, 0x8b, 0xd1, 0x03, 0xc6, 0x3b, 0xfe, 0x76, 0x08, 0x3b, 0xf8, 0x0f, 0x82,
            0x78, 0x01, 0x00, 0x00,
        ];
        assert_eq!(bundled().match_at(&code), Some("memmove"));
        // A different jcc displacement (link address) still matches.
        code[28] = 0xab;
        code[29] = 0xcd;
        assert_eq!(bundled().match_at(&code), Some("memmove"));
        // A one-byte change in the significant prologue must NOT match (no misfire).
        let mut bad = code;
        bad[6] = 0x7d; // mov esi,[ebp+0xc] -> different modrm
        assert_ne!(bundled().match_at(&bad), Some("memmove"));
    }
}
