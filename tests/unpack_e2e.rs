//! Full pipeline on a *real* packer: a UPX-packed Windows PE is dynamically
//! unpacked (Unicorn stub emulation + in-emulator Win32 import model), its OEP
//! and import table recovered, rebuilt into a clean PE, then transpiled to a
//! native Linux ELF that runs — `packed .exe -> native ELF that prints`.
//!
//! Skips unless the unpack feature is built in AND `upx` + `cc -m32` are present.

use std::process::{Command, Stdio};

fn tool_ok(cmd: &str, arg: &str) -> bool {
    Command::new(cmd).arg(arg).stdout(Stdio::null()).stderr(Stdio::null())
        .status().map(|s| s.success()).unwrap_or(false)
}

fn has_m32() -> bool {
    let cc = std::env::var("CC").unwrap_or_else(|_| "cc".to_string());
    let mut child = match Command::new(&cc)
        .args(["-m32", "-x", "c", "-", "-o", "/dev/null"])
        .stdin(Stdio::piped()).stdout(Stdio::null()).stderr(Stdio::null())
        .spawn() { Ok(c) => c, Err(_) => return false };
    use std::io::Write;
    if let Some(mut s) = child.stdin.take() { let _ = s.write_all(b"int main(void){return 0;}\n"); }
    child.wait().map(|s| s.success()).unwrap_or(false)
}

#[test]
fn upx_packed_pe_unpacks_then_transpiles_and_runs() {
    // hello_printf: M4 line; a real-OSS SHA-256 program: its exact digest.
    run_pipeline("hello_printf.exe", "M4: int=42 hex=0xff str=hello char=Z pct=%");
    run_pipeline(
        "hello_sha256.exe",
        "SHA256: df4fe12327c9300aa24d93f0fc01a593ad410dfc83055287040cb416e550921e",
    );
}

/// Pack `fixture` with UPX, unpack it, transpile the recovery, and assert the
/// native run prints `expect`. Skips unless upx + llc + cc -m32 + the unpack
/// feature are all available.
fn run_pipeline(fixture: &str, expect: &str) {
    if !tool_ok("upx", "--version") || !tool_ok("llc", "--version") || !has_m32() {
        eprintln!("skipping unpack e2e: need upx + llc + cc -m32");
        return;
    }
    let aret = env!("CARGO_BIN_EXE_aret");
    let fixtures = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/m1/fixtures");
    let orig = format!("{fixtures}/{fixture}");
    let tmp = std::env::temp_dir().join(format!("aret_e2e_{}_{}", fixture.replace('.', "_"), std::process::id()));
    let _ = std::fs::remove_dir_all(&tmp);
    std::fs::create_dir_all(&tmp).unwrap();
    let packed = tmp.join("packed.exe");

    // Pack the fixture with a real packer.
    let s = Command::new("upx").args(["-q", "-o"]).arg(&packed).arg(&orig).status().unwrap();
    assert!(s.success(), "upx failed to pack");

    // Unpack: emulate the stub, recover the OEP + imports, rebuild a clean PE.
    let unpack_dir = tmp.join("unpacked");
    let out = Command::new(aret)
        .args(["--mode", "unpack", "--out-dir"]).arg(&unpack_dir).arg(&packed)
        .output().unwrap();
    let report = String::from_utf8_lossy(&out.stdout);
    if report.contains("requires building with `--features unpack`")
        || String::from_utf8_lossy(&out.stderr).contains("--features unpack")
    {
        eprintln!("skipping unpack e2e: aret built without --features unpack");
        return;
    }
    assert!(out.status.success(), "unpack failed:\n{report}");
    assert!(report.contains("OEP recovered"), "no OEP:\n{report}");
    assert!(report.contains("printf"), "imports not reconstructed:\n{report}");

    let rebuilt = unpack_dir.join("unpacked.exe");
    assert!(rebuilt.exists(), "rebuilt PE not written");

    // Transpile the recovered program to a native ELF and run it.
    let run_dir = tmp.join("native");
    let out = Command::new(aret)
        .args(["--mode", "transpile", "--run", "--out-dir"]).arg(&run_dir).arg(&rebuilt)
        .output().unwrap();
    let stdout = String::from_utf8_lossy(&out.stdout);
    assert!(out.status.success(), "transpile failed:\n{stdout}");
    assert!(
        stdout.contains(expect),
        "unpacked-then-transpiled {fixture} wrong output (want {expect:?}):\n{stdout}"
    );

    let _ = std::fs::remove_dir_all(&tmp);
}
