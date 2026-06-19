//! The experimental LLVM IR backend (`--mode llvm`) must emit *valid* LLVM IR —
//! i.e. the output is accepted by the official LLVM assembler (`llvm-as`). Skips
//! when `llvm-as` is unavailable.

use std::process::{Command, Stdio};

fn has_llvm_as() -> bool {
    Command::new("llvm-as")
        .arg("--version")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

/// Emit LLVM IR for `fixture` and assert `llvm-as` accepts it.
fn assert_valid_ir(fixture: &str) {
    let path = format!("{}/tests/m1/fixtures/{}", env!("CARGO_MANIFEST_DIR"), fixture);
    let aret = env!("CARGO_BIN_EXE_aret");

    let ir = Command::new(aret)
        .args(["--mode", "llvm"])
        .arg(&path)
        .output()
        .expect("failed to run aret");
    assert!(ir.status.success(), "aret --mode llvm failed");
    assert!(
        ir.stdout.windows(7).any(|w| w == b"define "),
        "no functions emitted for {fixture}"
    );

    // Pipe the IR through llvm-as; a non-zero exit means invalid IR.
    let mut child = Command::new("llvm-as")
        .args(["-", "-o", "/dev/null"])
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .expect("failed to spawn llvm-as");
    use std::io::Write;
    child.stdin.take().unwrap().write_all(&ir.stdout).unwrap();
    let out = child.wait_with_output().unwrap();
    assert!(
        out.status.success(),
        "llvm-as rejected the emitted IR for {fixture}:\n{}",
        String::from_utf8_lossy(&out.stderr)
    );
}

fn has_m32_and_llc() -> bool {
    if !has_llvm_as() {
        return false;
    }
    let llc = Command::new("llc").arg("--version").stdout(Stdio::null()).stderr(Stdio::null()).status();
    if !llc.map(|s| s.success()).unwrap_or(false) {
        return false;
    }
    // cc -m32 must compile+link (the runtime is built as a 32-bit process).
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
    use std::io::Write;
    if let Some(mut s) = child.stdin.take() {
        let _ = s.write_all(b"int main(void){return 0;}\n");
    }
    child.wait().map(|s| s.success()).unwrap_or(false)
}

/// End-to-end through the LLVM backend: transpile a real Windows PE to a native
/// ELF *via LLVM IR* (llc), run it, and check the output — at parity with the C
/// backend.
#[test]
fn llvm_backend_runs_native() {
    if !has_m32_and_llc() {
        eprintln!("skipping LLVM run test: need llc + cc -m32");
        return;
    }
    let path = format!("{}/tests/m1/fixtures/hello_printf.exe", env!("CARGO_MANIFEST_DIR"));
    let out_dir = std::env::temp_dir().join(format!("aret_llvm_run_{}", std::process::id()));
    let aret = env!("CARGO_BIN_EXE_aret");
    let output = Command::new(aret)
        .args(["--mode", "transpile", "--backend", "llvm", "--run"])
        .arg("--out-dir")
        .arg(&out_dir)
        .arg(&path)
        .output()
        .expect("failed to run aret");
    let stdout = String::from_utf8_lossy(&output.stdout);
    assert!(output.status.success(), "llvm transpile failed:\n{stdout}");
    assert!(
        stdout.contains("M4: int=42 hex=0xff str=hello char=Z pct=%"),
        "LLVM-backed binary wrong output:\n{stdout}"
    );
    let _ = std::fs::remove_dir_all(&out_dir);
}

#[test]
fn llvm_ir_is_valid_for_fixtures() {
    if !has_llvm_as() {
        eprintln!("skipping LLVM backend test: llvm-as unavailable");
        return;
    }
    // A range of recovered code: stack-built strings, register-indirect imports,
    // global data, internal calls, the CRT runtime.
    for fixture in [
        "hello_win32.exe",
        "hello_globals.exe",
        "hello_callchain.exe",
        "hello_printf.exe",
        "hello_heap.exe",
        "hello_float.exe",
    ] {
        assert_valid_ir(fixture);
    }
}
