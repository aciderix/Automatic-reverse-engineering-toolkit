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
    ] {
        assert_valid_ir(fixture);
    }
}
