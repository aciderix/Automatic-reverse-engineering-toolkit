//! UBT milestone M1, end-to-end: a freestanding Win32 PE (kernel32 imports
//! only) is transpiled to C, its API imports are intercepted into native HLE
//! shims, recompiled into a native ELF, and run — producing the same output the
//! original Windows program would.
//!
//! Skips gracefully when the 32-bit native toolchain (`cc -m32`) is unavailable,
//! since the stack-model C must be built as a 32-bit process.

use std::io::Write;
use std::process::{Command, Stdio};

/// Is `cc -m32` able to compile and link a trivial program here?
fn has_m32() -> bool {
    let cc = std::env::var("CC").unwrap_or_else(|_| "cc".to_string());
    let mut child = match Command::new(&cc)
        .args(["-m32", "-x", "c", "-", "-o", "/dev/null"])
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
    {
        Ok(c) => c,
        Err(_) => return false,
    };
    if let Some(mut stdin) = child.stdin.take() {
        let _ = stdin.write_all(b"int main(void){return 0;}\n");
    }
    child.wait().map(|s| s.success()).unwrap_or(false)
}

#[test]
fn transpile_freestanding_win32_pe_to_native_linux() {
    if !has_m32() {
        eprintln!("skipping M1 test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }

    let fixture = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/m1/fixtures/hello_win32.exe");
    assert!(
        std::path::Path::new(fixture).exists(),
        "missing fixture {fixture}"
    );

    let out_dir = std::env::temp_dir().join(format!("aret_m1_{}", std::process::id()));
    let aret = env!("CARGO_BIN_EXE_aret");

    let output = Command::new(aret)
        .args(["--mode", "transpile", "--run"])
        .arg("--out-dir")
        .arg(&out_dir)
        .arg(fixture)
        .output()
        .expect("failed to run aret");

    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        output.status.success(),
        "aret transpile failed:\nstdout:\n{stdout}\nstderr:\n{stderr}"
    );
    assert!(
        stdout.contains("Hello from Windows, running native on Linux"),
        "transpiled binary did not produce the expected output.\nstdout:\n{stdout}"
    );

    let _ = std::fs::remove_dir_all(&out_dir);
}
