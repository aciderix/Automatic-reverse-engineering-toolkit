//! Content-addressed object cache for the transpile's compile step (doc 81 §I9).
//!
//! **Why.** The dev loop on a large binary is *edit one HLE shim → rebuild → run*.
//! Measured on WinMerge + mfc90u + shell32 + shlwapi: 254 generated `.c` files,
//! 316 MB, **141 s** of `cc -O0` on 4 cores — and on a shim-only edit **every one
//! of those objects is bit-identical to the previous build**, because the lifted
//! app code does not depend on the runtime sources. Recompiling them is pure waste,
//! and it is paid again by every winediff fixture (194 of them re-compile the same
//! `aret_hle.c`/`aret_crt.c`/`aret_win32.c`).
//!
//! **Soundness (this is a cache in a codebase whose first rule is "never present a
//! wrong result as correct", so the key has to be exact, not approximate).** An
//! object is a pure function of: the compiler, its flags, the source bytes, and the
//! bytes of *every* file the preprocessor read. The last part is the one a naive
//! cache gets wrong — it keys on the `.c` alone and serves a stale object when a
//! header changed. So we do what ccache calls *depend mode*: the first compile also
//! writes a `-MD` dependency list; the cache entry records that list, and a later
//! lookup **re-hashes every listed file** before it will reuse the object. A changed
//! header, a changed system header, a deleted file — each one fails the check and
//! falls back to compiling. The cache can only ever fail *closed* (extra work),
//! never open (wrong bytes).
//!
//! Paths inside the output directory are stored **relative** to it, so an object
//! built in one `--out-dir` is reusable from another — which is the case that
//! matters, since each experiment uses a fresh directory.
//!
//! Off with `ARET_NO_OBJCACHE=1`; directory via `ARET_OBJCACHE` (default
//! `$XDG_CACHE_HOME/aret/obj`, else `~/.cache/aret/obj`); size budget via
//! `ARET_OBJCACHE_MAX_MB` (default 4096).

use std::path::{Path, PathBuf};

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4). Self-contained: the toolkit has no hash dependency and
// this is the one place that needs a collision-resistant digest — a 64-bit hash
// would be a real (if unlikely) way to serve the wrong object. Proven by the
// standard known-answer vectors in the tests below, not by inspection.
// ---------------------------------------------------------------------------

const K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

/// Streaming SHA-256 state (so a multi-megabyte source is hashed without a copy).
pub struct Sha256 {
    h: [u32; 8],
    buf: [u8; 64],
    len: usize,   // bytes buffered
    total: u64,   // total bytes fed
}

impl Default for Sha256 {
    fn default() -> Self {
        Self::new()
    }
}

impl Sha256 {
    pub fn new() -> Self {
        Sha256 {
            h: [
                0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
                0x5be0cd19,
            ],
            buf: [0u8; 64],
            len: 0,
            total: 0,
        }
    }

    pub fn update(&mut self, mut data: &[u8]) {
        self.total = self.total.wrapping_add(data.len() as u64);
        if self.len > 0 {
            let take = (64 - self.len).min(data.len());
            self.buf[self.len..self.len + take].copy_from_slice(&data[..take]);
            self.len += take;
            data = &data[take..];
            if self.len == 64 {
                let block = self.buf;
                self.compress(&block);
                self.len = 0;
            }
        }
        while data.len() >= 64 {
            let (block, rest) = data.split_at(64);
            self.compress(block.try_into().unwrap());
            data = rest;
        }
        if !data.is_empty() {
            self.buf[..data.len()].copy_from_slice(data);
            self.len = data.len();
        }
    }

    pub fn finish(mut self) -> [u8; 32] {
        let bits = self.total.wrapping_mul(8);
        self.update(&[0x80]);
        while self.len != 56 {
            self.update(&[0x00]);
        }
        // `update` above keeps `total` moving; write the length directly.
        self.buf[56..64].copy_from_slice(&bits.to_be_bytes());
        let block = self.buf;
        self.compress(&block);
        let mut out = [0u8; 32];
        for (i, w) in self.h.iter().enumerate() {
            out[i * 4..i * 4 + 4].copy_from_slice(&w.to_be_bytes());
        }
        out
    }

    fn compress(&mut self, block: &[u8; 64]) {
        let mut w = [0u32; 64];
        for i in 0..16 {
            w[i] = u32::from_be_bytes(block[i * 4..i * 4 + 4].try_into().unwrap());
        }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16]
                .wrapping_add(s0)
                .wrapping_add(w[i - 7])
                .wrapping_add(s1);
        }
        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut hh] = self.h;
        for i in 0..64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let ch = (e & f) ^ (!e & g);
            let t1 = hh
                .wrapping_add(s1)
                .wrapping_add(ch)
                .wrapping_add(K[i])
                .wrapping_add(w[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let maj = (a & b) ^ (a & c) ^ (b & c);
            let t2 = s0.wrapping_add(maj);
            hh = g;
            g = f;
            f = e;
            e = d.wrapping_add(t1);
            d = c;
            c = b;
            b = a;
            a = t1.wrapping_add(t2);
        }
        for (dst, val) in self
            .h
            .iter_mut()
            .zip([a, b, c, d, e, f, g, hh])
        {
            *dst = dst.wrapping_add(val);
        }
    }
}

pub fn hex(d: &[u8; 32]) -> String {
    let mut s = String::with_capacity(64);
    for b in d {
        s.push_str(&format!("{b:02x}"));
    }
    s
}

fn hash_file(path: &Path) -> Option<[u8; 32]> {
    let data = std::fs::read(path).ok()?;
    let mut h = Sha256::new();
    h.update(&data);
    Some(h.finish())
}

// ---------------------------------------------------------------------------
// The cache itself
// ---------------------------------------------------------------------------

/// One compile's cache coordinates: the key that identifies (compiler, flags,
/// source) and the output directory that relative dependency paths resolve against.
pub struct ObjCache {
    dir: PathBuf,
    /// Identity of the compiler: its path, its `--version` banner, and the size +
    /// mtime of the binary. A toolchain upgrade that keeps the version string would
    /// still move the mtime, so the key moves with it.
    cc_id: String,
}

/// What a lookup found, and what a later `store` needs to record it.
pub struct Pending {
    key0: String,
    /// Where the `-MD` dependency list must be written by the compile.
    pub depfile: PathBuf,
}

impl ObjCache {
    /// Open the cache, or `None` when disabled / unusable. Never fails a build:
    /// an unusable cache simply means every compile runs.
    pub fn open(cc: &str) -> Option<ObjCache> {
        if std::env::var("ARET_NO_OBJCACHE").is_ok_and(|v| v != "0" && !v.is_empty()) {
            return None;
        }
        let dir = match std::env::var("ARET_OBJCACHE") {
            Ok(d) if !d.is_empty() => PathBuf::from(d),
            _ => {
                let base = std::env::var("XDG_CACHE_HOME")
                    .ok()
                    .filter(|s| !s.is_empty())
                    .or_else(|| std::env::var("HOME").ok().map(|h| format!("{h}/.cache")))?;
                PathBuf::from(base).join("aret").join("obj")
            }
        };
        std::fs::create_dir_all(&dir).ok()?;
        // Compiler identity. `--version` alone is not enough (a rebuild of the same
        // version can change codegen), so fold in the binary's size and mtime.
        let ver = std::process::Command::new(cc)
            .arg("--version")
            .output()
            .ok()
            .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
            .unwrap_or_default();
        let mut id = format!("{cc}\n{ver}");
        if let Ok(p) = which(cc) {
            if let Ok(m) = std::fs::metadata(&p) {
                let mtime = m
                    .modified()
                    .ok()
                    .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                    .map(|d| d.as_secs())
                    .unwrap_or(0);
                id.push_str(&format!("\n{}\n{}\n{}", p.display(), m.len(), mtime));
            }
        }
        Some(ObjCache { dir, cc_id: id })
    }

    /// Key for (compiler, flags, source bytes). Independent of the output directory
    /// so the same source compiled from a fresh `--out-dir` hits the same entry.
    pub fn begin(&self, args: &[String], src: &Path, out_dir: &Path) -> Option<Pending> {
        let src_bytes = std::fs::read(src).ok()?;
        let mut h = Sha256::new();
        h.update(b"aret-objcache-v1\0");
        h.update(self.cc_id.as_bytes());
        h.update(b"\0args\0");
        for a in args {
            // The `-I <out_dir>`, `-o <obj>` and source paths vary per run and do not
            // affect the object's bytes (no `__FILE__` in generated or runtime code,
            // no `-g`), so they are replaced by a marker rather than hashed.
            let a = if Path::new(a).starts_with(out_dir) || Path::new(a) == src {
                "<outdir>".to_string()
            } else {
                a.clone()
            };
            h.update(a.as_bytes());
            h.update(b"\0");
        }
        h.update(b"\0src\0");
        h.update(&src_bytes);
        let key0 = hex(&h.finish());
        std::fs::create_dir_all(self.shard(&key0)).ok()?;
        // The dependency list is scratch, so it lives beside the object in the output
        // directory: source file names are unique there, which keeps two rayon threads
        // (or two parallel winediff fixtures) from ever writing the same path.
        let name = src.file_name()?.to_string_lossy().into_owned();
        Some(Pending {
            depfile: out_dir.join(format!("{name}.d")),
            key0,
        })
    }

    fn shard(&self, key0: &str) -> PathBuf {
        self.dir.join(&key0[..2])
    }

    /// Digest of the dependency set recorded for `key0`, or `None` when the manifest
    /// is missing or any listed file no longer hashes (deleted / changed is fine —
    /// both simply mean "not this entry").
    fn dep_digest(&self, key0: &str, out_dir: &Path) -> Option<String> {
        let manifest = self.shard(key0).join(format!("{key0}.dep"));
        let text = std::fs::read_to_string(manifest).ok()?;
        let mut h = Sha256::new();
        h.update(b"deps-v1\0");
        for line in text.lines() {
            let (kind, rel) = line.split_once('\t')?;
            let path = match kind {
                "o" => out_dir.join(rel), // inside the output directory: relocatable
                "a" => PathBuf::from(rel),
                _ => return None,
            };
            let d = hash_file(&path)?;
            h.update(line.as_bytes());
            h.update(b"\0");
            h.update(&d);
        }
        Some(hex(&h.finish()))
    }

    /// Reuse a cached object for `p` if one matches, copying it to `obj`.
    pub fn lookup(&self, p: &Pending, out_dir: &Path, obj: &Path) -> bool {
        let Some(dd) = self.dep_digest(&p.key0, out_dir) else {
            return false;
        };
        let cached = self.shard(&p.key0).join(format!("{}-{}.o", p.key0, dd));
        if !cached.is_file() {
            return false;
        }
        if std::fs::copy(&cached, obj).is_err() {
            return false;
        }
        // Touch so the size trim evicts genuinely cold entries, not hot ones.
        let _ = filetime_touch(&cached);
        true
    }

    /// Record the object just compiled, together with the dependency list the
    /// compiler reported, so the next lookup can validate it exactly.
    pub fn store(&self, p: &Pending, out_dir: &Path, obj: &Path) {
        let Ok(dep_text) = std::fs::read_to_string(&p.depfile) else {
            return;
        };
        let _ = std::fs::remove_file(&p.depfile);
        let mut manifest = String::new();
        for d in parse_depfile(&dep_text) {
            let path = PathBuf::from(&d);
            let line = match path.strip_prefix(out_dir) {
                Ok(rel) => format!("o\t{}", rel.display()),
                Err(_) => format!("a\t{}", path.display()),
            };
            manifest.push_str(&line);
            manifest.push('\n');
        }
        let shard = self.shard(&p.key0);
        let mpath = shard.join(format!("{}.dep", p.key0));
        // Write the manifest first, then compute the digest the same way a lookup
        // will (from the manifest text), so the two can never disagree.
        if atomic_write(&mpath, manifest.as_bytes()).is_err() {
            return;
        }
        let Some(dd) = self.dep_digest(&p.key0, out_dir) else {
            return;
        };
        let target = shard.join(format!("{}-{}.o", p.key0, dd));
        let tmp = shard.join(format!("{}-{}.o.tmp{}", p.key0, dd, std::process::id()));
        if std::fs::copy(obj, &tmp).is_ok() {
            let _ = std::fs::rename(&tmp, &target);
        } else {
            let _ = std::fs::remove_file(&tmp);
        }
    }

    /// Keep the cache under its byte budget by deleting the least recently used
    /// objects. Manifests are tiny and are left alone (a manifest whose object is
    /// gone just yields a miss).
    pub fn trim(&self) {
        let max_mb: u64 = std::env::var("ARET_OBJCACHE_MAX_MB")
            .ok()
            .and_then(|v| v.parse().ok())
            .unwrap_or(4096);
        let budget = max_mb.saturating_mul(1024 * 1024);
        let mut entries: Vec<(std::time::SystemTime, u64, PathBuf)> = Vec::new();
        let mut total = 0u64;
        let Ok(shards) = std::fs::read_dir(&self.dir) else {
            return;
        };
        for shard in shards.flatten() {
            let Ok(files) = std::fs::read_dir(shard.path()) else {
                continue;
            };
            for f in files.flatten() {
                let p = f.path();
                if p.extension().and_then(|e| e.to_str()) != Some("o") {
                    continue;
                }
                let Ok(m) = f.metadata() else { continue };
                total += m.len();
                entries.push((m.modified().unwrap_or(std::time::UNIX_EPOCH), m.len(), p));
            }
        }
        if total <= budget {
            return;
        }
        entries.sort_by_key(|(t, _, _)| *t); // oldest first
        for (_, len, path) in entries {
            if total <= budget {
                break;
            }
            if std::fs::remove_file(&path).is_ok() {
                total = total.saturating_sub(len);
            }
        }
    }
}

fn atomic_write(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    let tmp = path.with_extension(format!("tmp{}", std::process::id()));
    std::fs::write(&tmp, bytes)?;
    std::fs::rename(&tmp, path)
}

/// Bump an entry's mtime (used as an LRU stamp). Best-effort: rewriting the file is
/// too costly, so we open it for append with no data, which updates nothing on some
/// filesystems — a miss here only makes eviction slightly less accurate.
fn filetime_touch(path: &Path) -> std::io::Result<()> {
    use std::fs::OpenOptions;
    let f = OpenOptions::new().append(true).open(path)?;
    f.set_len(f.metadata()?.len())?;
    Ok(())
}

fn which(cmd: &str) -> std::io::Result<PathBuf> {
    if cmd.contains('/') {
        return Ok(PathBuf::from(cmd));
    }
    let path = std::env::var("PATH").unwrap_or_default();
    for dir in path.split(':') {
        let p = Path::new(dir).join(cmd);
        if p.is_file() {
            return Ok(p);
        }
    }
    Err(std::io::Error::new(
        std::io::ErrorKind::NotFound,
        "not on PATH",
    ))
}

/// Dependencies out of a `-MD` file: `target: dep dep \<newline> dep …`.
/// A path we fail to un-escape simply will not hash later, which is a miss — the
/// parser can cost a rebuild, never a wrong object.
pub fn parse_depfile(text: &str) -> Vec<String> {
    let body = match text.find(':') {
        Some(i) => &text[i + 1..],
        None => text,
    };
    body.replace("\\\n", " ")
        .replace("\\\r\n", " ")
        .split_whitespace()
        .filter(|t| *t != "\\")
        .map(|t| t.to_string())
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sha(data: &[u8]) -> String {
        let mut h = Sha256::new();
        h.update(data);
        hex(&h.finish())
    }

    /// FIPS 180-4 known-answer vectors. The digest is what makes the cache safe to
    /// key on, so it is proven against the standard, not assumed.
    #[test]
    fn sha256_known_answers() {
        assert_eq!(
            sha(b""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );
        assert_eq!(
            sha(b"abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
        assert_eq!(
            sha(b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
        );
        // A million 'a' — exercises the multi-block path and the length encoding.
        let mut h = Sha256::new();
        for _ in 0..1000 {
            h.update(&[b'a'; 1000]);
        }
        assert_eq!(
            hex(&h.finish()),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"
        );
    }

    /// Feeding the same bytes in different chunk sizes must give the same digest
    /// (the cache streams a source file in whatever pieces it arrives).
    #[test]
    fn sha256_chunking_is_irrelevant() {
        let data: Vec<u8> = (0..5000u32).map(|i| (i % 251) as u8).collect();
        let whole = sha(&data);
        for chunk in [1usize, 7, 63, 64, 65, 1024] {
            let mut h = Sha256::new();
            for part in data.chunks(chunk) {
                h.update(part);
            }
            assert_eq!(hex(&h.finish()), whole, "chunk size {chunk}");
        }
    }

    /// The property the whole cache rests on: a hit must reproduce the object the
    /// compiler would have produced, and a **header** change must miss. The second
    /// half is the one a source-only key gets wrong — it is the difference between a
    /// cache and a silent wrong result, so it is measured against a real compile.
    #[test]
    fn hit_reproduces_the_object_and_a_header_change_misses() {
        let cc = std::env::var("CC").unwrap_or_else(|_| "cc".to_string());
        if std::process::Command::new(&cc)
            .arg("--version")
            .output()
            .is_err()
        {
            eprintln!("skip: no C compiler");
            return;
        }
        let root = std::env::temp_dir().join(format!("aret_objcache_t{}", std::process::id()));
        let out_dir = root.join("out");
        let cache_dir = root.join("cache");
        std::fs::create_dir_all(&out_dir).unwrap();
        std::env::set_var("ARET_OBJCACHE", &cache_dir);
        std::env::remove_var("ARET_NO_OBJCACHE");

        let src = out_dir.join("t.c");
        let hdr = out_dir.join("t.h");
        std::fs::write(&src, "#include \"t.h\"\nint f(void){return VALUE;}\n").unwrap();
        std::fs::write(&hdr, "#define VALUE 1\n").unwrap();

        let flags: Vec<String> = ["-w", "-fno-pie", "-O0", "-c"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        let cache = ObjCache::open(&cc).expect("cache opens");

        // A compile that also records its dependency list, exactly as the builder does.
        let compile = |p: &Pending, obj: &Path| {
            let ok = std::process::Command::new(&cc)
                .args(&flags)
                .arg(&src)
                .arg("-I")
                .arg(&out_dir)
                .arg("-o")
                .arg(obj)
                .arg("-MD")
                .arg("-MF")
                .arg(&p.depfile)
                .output()
                .unwrap()
                .status
                .success();
            assert!(ok, "compile failed");
        };

        // Cold: miss, then store.
        let o1 = out_dir.join("cold.o");
        let p1 = cache.begin(&flags, &src, &out_dir).unwrap();
        assert!(!cache.lookup(&p1, &out_dir, &o1), "cold build must miss");
        compile(&p1, &o1);
        cache.store(&p1, &out_dir, &o1);
        let cold = std::fs::read(&o1).unwrap();

        // Warm: hit, and the served object is the same bytes.
        let o2 = out_dir.join("warm.o");
        let p2 = cache.begin(&flags, &src, &out_dir).unwrap();
        assert!(cache.lookup(&p2, &out_dir, &o2), "warm build must hit");
        assert_eq!(cold, std::fs::read(&o2).unwrap(), "cached object differs");

        // Header changed, source untouched: must MISS, and the fresh object must
        // differ (proving the stale one would have been wrong).
        std::fs::write(&hdr, "#define VALUE 2\n").unwrap();
        let o3 = out_dir.join("changed.o");
        let p3 = cache.begin(&flags, &src, &out_dir).unwrap();
        assert!(
            !cache.lookup(&p3, &out_dir, &o3),
            "a changed header must invalidate the entry"
        );
        compile(&p3, &o3);
        assert_ne!(
            cold,
            std::fs::read(&o3).unwrap(),
            "the header change should have changed the object"
        );

        // And the original header comes back to the original object.
        std::fs::write(&hdr, "#define VALUE 1\n").unwrap();
        let o4 = out_dir.join("back.o");
        let p4 = cache.begin(&flags, &src, &out_dir).unwrap();
        assert!(cache.lookup(&p4, &out_dir, &o4), "restored header must hit");
        assert_eq!(cold, std::fs::read(&o4).unwrap());

        std::env::remove_var("ARET_OBJCACHE");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn depfile_parses_continuations() {
        let d = "foo.o: foo.c /usr/include/stdint.h \\\n  bar.h \\\n  baz.h\n";
        assert_eq!(
            parse_depfile(d),
            vec!["foo.c", "/usr/include/stdint.h", "bar.h", "baz.h"]
        );
    }
}
