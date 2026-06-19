//! UBT transpiler, end-to-end: a freestanding Win32 PE is transpiled to C, its
//! API imports are intercepted into native HLE shims, recompiled into a native
//! ELF, and run — producing the same output the original Windows program would.
//!
//!   * M1 — direct import calls + arguments built on the stack.
//!   * M2 — indirect import calls through a register (`mov reg,[iat]; call reg`)
//!     and global data in `.rdata` mapped back to its original virtual address.
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

/// Transpile `fixture` to a native binary, run it, and return its stdout.
fn transpile_and_run(fixture_name: &str) -> String {
    let fixture = format!(
        "{}/tests/m1/fixtures/{}",
        env!("CARGO_MANIFEST_DIR"),
        fixture_name
    );
    assert!(
        std::path::Path::new(&fixture).exists(),
        "missing fixture {fixture}"
    );

    let out_dir = std::env::temp_dir().join(format!(
        "aret_{}_{}",
        fixture_name.replace('.', "_"),
        std::process::id()
    ));
    let aret = env!("CARGO_BIN_EXE_aret");

    let output = Command::new(aret)
        .args(["--mode", "transpile", "--run"])
        .arg("--out-dir")
        .arg(&out_dir)
        .arg(&fixture)
        .output()
        .expect("failed to run aret");

    let stdout = String::from_utf8_lossy(&output.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        output.status.success(),
        "aret transpile failed:\nstdout:\n{stdout}\nstderr:\n{stderr}"
    );
    let _ = std::fs::remove_dir_all(&out_dir);
    stdout
}

#[test]
fn m1_direct_imports_stack_args() {
    if !has_m32() {
        eprintln!("skipping M1 test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    let out = transpile_and_run("hello_win32.exe");
    assert!(
        out.contains("Hello from Windows, running native on Linux"),
        "unexpected output:\n{out}"
    );
}

#[test]
fn m2_register_indirect_imports_and_global_data() {
    if !has_m32() {
        eprintln!("skipping M2 test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    let out = transpile_and_run("hello_globals.exe");
    assert!(
        out.contains("M2: first global string in .rdata"),
        "missing first global line:\n{out}"
    );
    assert!(
        out.contains("M2: second global, mapped at its original VA"),
        "missing second global line:\n{out}"
    );
}

#[test]
fn m3_internal_call_register_args() {
    if !has_m32() {
        eprintln!("skipping M3 test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // A helper called twice through an internal call, arguments passed in
    // registers (gcc -O1 regparm) — must cross the call via the shared machine
    // stack's threaded registers.
    let out = transpile_and_run("hello_callchain.exe");
    assert!(
        out.contains("M3: passed through an internal call (1)")
            && out.contains("M3: passed through an internal call (2)"),
        "internal register-arg call did not convey arguments:\n{out}"
    );
}

#[test]
fn m3_internal_call_stack_args() {
    if !has_m32() {
        eprintln!("skipping M3 test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // A 6-argument helper: arguments 4-6 are passed on the stack, so they must
    // travel from caller to callee through the single shared machine stack.
    let out = transpile_and_run("hello_stackargs.exe");
    assert!(
        out.contains("M3: argument arrived via the shared STACK"),
        "stack-passed argument did not cross the call:\n{out}"
    );
}
