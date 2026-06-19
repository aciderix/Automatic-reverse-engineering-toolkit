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

/// Transpile `fixture` to a native binary, run it (with optional extra env), and
/// return its stdout.
fn transpile_and_run_env(fixture_name: &str, env: &[(&str, &str)]) -> String {
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

    let mut cmd = Command::new(aret);
    cmd.args(["--mode", "transpile", "--run"])
        .arg("--out-dir")
        .arg(&out_dir)
        .arg(&fixture);
    for (k, v) in env {
        cmd.env(k, v);
    }
    let output = cmd.output().expect("failed to run aret");

    let stdout = String::from_utf8_lossy(&output.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        output.status.success(),
        "aret transpile failed:\nstdout:\n{stdout}\nstderr:\n{stderr}"
    );
    let _ = std::fs::remove_dir_all(&out_dir);
    stdout
}

/// Transpile `fixture` to a native binary, run it, and return its stdout.
fn transpile_and_run(fixture_name: &str) -> String {
    transpile_and_run_env(fixture_name, &[])
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

#[test]
fn m4_variadic_printf() {
    if !has_m32() {
        eprintln!("skipping M4 test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // printf with several conversions: the HLE shim reads the variadic arguments
    // from the shared machine stack and reproduces libc formatting.
    let out = transpile_and_run("hello_printf.exe");
    assert!(
        out.contains("M4: int=42 hex=0xff str=hello char=Z pct=%"),
        "printf conversions wrong:\n{out}"
    );
    assert!(out.contains("M4: malloc sum=100"), "second printf wrong:\n{out}");
}

#[test]
fn m4_heap_and_string_runtime() {
    if !has_m32() {
        eprintln!("skipping M4 test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // malloc + an internal (shared-stack) fill + strlen + printf %s: a real heap
    // and string runtime through the CRT shims.
    let out = transpile_and_run("hello_heap.exe");
    assert!(
        out.contains("M4: heap=ABCDE len=5"),
        "heap/string runtime wrong:\n{out}"
    );
}

#[test]
fn indirect_call_dispatch_through_function_pointer() {
    if !has_m32() {
        eprintln!("skipping fnptr test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // A call through a function pointer (holding the original code VA) must be
    // dispatched by aret_call to the transpiled function.
    let out = transpile_and_run("hello_fnptr.exe");
    assert!(
        out.contains("INDIRECT: result=42"),
        "indirect call did not dispatch correctly:\n{out}"
    );
}

#[test]
fn teb_synthetic_process_environment() {
    if !has_m32() {
        eprintln!("skipping TEB test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // fs:[0x18] (TEB Self) and fs:[0x30] (PEB) must resolve to a consistent
    // synthetic TEB so Windows-startup environment reads work natively.
    let out = transpile_and_run("hello_teb.exe");
    assert!(
        out.contains("TEB: nonnull=1 self_consistent=1 peb_nonnull=1"),
        "synthetic TEB/PEB not consistent:\n{out}"
    );
}

#[test]
fn float_sse_arithmetic() {
    if !has_m32() {
        eprintln!("skipping float test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // SSE double arithmetic (mul/add) + int conversion via the __fp_* helpers.
    let out = transpile_and_run("hello_float.exe");
    assert!(out.contains("FLOAT: c=8 c10=85"), "float arithmetic wrong:\n{out}");
}

#[test]
fn fs_file_roundtrip_with_path_translation() {
    if !has_m32() {
        eprintln!("skipping FS test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // A program that writes then reads a file through a Windows `C:\` path. The
    // HLE translates the path under ARET_PREFIX and maps stdio onto POSIX.
    let prefix = std::env::temp_dir().join(format!("aret_fs_prefix_{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&prefix);
    let prefix_str = prefix.to_str().unwrap();

    let out = transpile_and_run_env("hello_file.exe", &[("ARET_PREFIX", prefix_str)]);
    assert!(
        out.contains("FS: round-trip through a C:\\ path"),
        "file round-trip output wrong:\n{out}"
    );
    // The Windows path C:\aret_fs_test.txt must have landed under <prefix>/drive_c.
    let translated = prefix.join("drive_c").join("aret_fs_test.txt");
    assert!(
        translated.exists(),
        "translated file not created at {}",
        translated.display()
    );
    let _ = std::fs::remove_dir_all(&prefix);
}
