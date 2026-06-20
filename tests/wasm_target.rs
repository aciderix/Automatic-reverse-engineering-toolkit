//! WebAssembly target (`--target wasm`): the recovered C is compiled with
//! clang for `wasm32-wasi` and linked to a `.wasm` that runs in a WASM runtime.
//! wasm32's linear memory *is* the 32-bit address space, so sections are
//! memcpy'd to their VAs (no mmap) and 32-bit pointers map directly.
//!
//! Skips unless `clang` (with the wasi sysroot) and a WASM runtime are present.

use std::process::{Command, Stdio};

fn have(cmd: &str, arg: &str) -> bool {
    Command::new(cmd).arg(arg).stdout(Stdio::null()).stderr(Stdio::null())
        .status().map(|s| s.success()).unwrap_or(false)
}

/// Can clang link a trivial wasm32-wasi program (i.e. is the sysroot installed)?
fn wasm_toolchain() -> bool {
    if !have("wasmtime", "--version") && !have("wasmer", "--version") {
        return false;
    }
    let tmp = std::env::temp_dir().join(format!("aret_wasmprobe_{}.c", std::process::id()));
    let out = tmp.with_extension("wasm");
    std::fs::write(&tmp, "int main(void){return 0;}\n").unwrap();
    let ok = Command::new("clang")
        .args(["--target=wasm32-wasi", "--sysroot=/usr"])
        .arg(&tmp).arg("-o").arg(&out)
        .stdout(Stdio::null()).stderr(Stdio::null())
        .status().map(|s| s.success()).unwrap_or(false);
    let _ = std::fs::remove_file(&tmp);
    let _ = std::fs::remove_file(&out);
    ok
}

/// Transpile `fixture` to wasm, run it, and assert stdout contains `expect`.
fn wasm_run(fixture: &str, expect: &str) {
    let path = format!("{}/tests/m1/fixtures/{}", env!("CARGO_MANIFEST_DIR"), fixture);
    let out_dir = std::env::temp_dir().join(format!("aret_wasm_{}_{}", fixture.replace('.', "_"), std::process::id()));
    let aret = env!("CARGO_BIN_EXE_aret");
    let output = Command::new(aret)
        .args(["--mode", "transpile", "--target", "wasm", "--run", "--out-dir"])
        .arg(&out_dir).arg(&path)
        .output().expect("failed to run aret");
    let stdout = String::from_utf8_lossy(&output.stdout);
    assert!(output.status.success(), "wasm transpile failed for {fixture}:\n{stdout}");
    assert!(stdout.contains(expect), "wasm {fixture} wrong output (want {expect:?}):\n{stdout}");
    let _ = std::fs::remove_dir_all(&out_dir);
}

#[test]
fn windows_pe_to_webassembly() {
    if !wasm_toolchain() {
        eprintln!("skipping wasm test: need clang+wasi-sysroot and wasmtime/wasmer");
        return;
    }
    // A spread: stack-built string, global data, internal+indirect calls, the CRT
    // (printf/heap), x87 floats, the native Win32 layer, and a real OSS program
    // (SHA-256) — all the same recovered C, retargeted to WebAssembly.
    wasm_run("hello_win32.exe", "Hello from Windows, running native on Linux");
    wasm_run("hello_globals.exe", "M2: first global string in .rdata");
    wasm_run("hello_fnptr.exe", "INDIRECT: result=42");
    wasm_run("hello_printf.exe", "M4: int=42 hex=0xff str=hello char=Z pct=%");
    wasm_run("hello_float_x87.exe", "FLOAT: c=8 c10=85");
    wasm_run("hello_win32api.exe", "W32: zero=1 ctr=41 ev=ok evlen=2 s=win32 slen=5 mono=1");
    wasm_run(
        "hello_sha256.exe",
        "SHA256: df4fe12327c9300aa24d93f0fc01a593ad410dfc83055287040cb416e550921e",
    );
}
