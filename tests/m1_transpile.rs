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
fn float_x87_long_double() {
    if !has_m32() {
        eprintln!("skipping x87 test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // Same arithmetic via the x87 FPU (the __x87_* long-double helpers).
    let out = transpile_and_run("hello_float_x87.exe");
    assert!(out.contains("FLOAT: c=8 c10=85"), "x87 float wrong:\n{out}");
}

#[test]
fn crt_forwarding_to_libc() {
    if !has_m32() {
        eprintln!("skipping CRT test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // A spread of msvcrt functions (string build/search, case-insensitive compare,
    // ctype, integer conversion, sprintf) forwarded to the genuine host libc
    // through the shared machine stack (aret_crt.c).
    let out = transpile_and_run("hello_crt.exe");
    assert!(
        out.contains("CRT: s=reverse dot=.c sub=piler ci=0 up=Z dig=1 n=-123 hex=255 abs=42 mc=1"),
        "CRT forwarding wrong:\n{out}"
    );
}

#[test]
fn real_oss_sha256_digest() {
    if !has_m32() {
        eprintln!("skipping SHA-256 test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // A real, recognizable open-source program — Brad Conte's public-domain
    // SHA-256 (crypto-algorithms) — through the pipeline. Pure integer/memory
    // (tables, rotates, loops); the digest must match the reference exactly, so
    // any lowering bug would scramble it.
    let out = transpile_and_run("hello_sha256.exe");
    assert!(
        out.contains("SHA256: df4fe12327c9300aa24d93f0fc01a593ad410dfc83055287040cb416e550921e"),
        "real-OSS SHA-256 digest wrong:\n{out}"
    );
}

#[test]
fn loop_exit_carrying_a_value() {
    if !has_m32() {
        eprintln!("skipping loop-exit test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // Regression for Bug #2: an inline strcmp whose "mismatch" exit edge computes
    // the comparison result (sbb/or) on the way out of the loop, with a second
    // exit edge (the "match" path) to a *different* block. The structurer must
    // not collapse the value-carrying exit into a bare `break` that diverts to
    // the other exit's follow block — that would drop the loop-carried result and
    // the mismatch path would wrongly return 0. Expected: eq=0 lt<0 gt>0.
    let out = transpile_and_run("loop_exit_value.exe");
    assert!(
        out.contains("LOOPEXIT: eq=0 lt=-1 gt=1"),
        "loop-exit value dropped (Bug #2 regression):\n{out}"
    );
}

#[test]
fn bytecode_dispatch_loop() {
    if !has_m32() {
        eprintln!("skipping dispatch-loop test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // An interpreter dispatch loop: each handler re-dispatches via computed goto,
    // so the loop *header* ends in the jump-table switch and the index register is
    // consumed by the table load (often in a predecessor block). Exercises both
    // the switch-headed loop (else an empty `while(1){}`) and the address-keyed
    // switch (switch on the loaded target address). This is Lua's luaV_execute.
    let out = transpile_and_run("dispatch_loop.exe");
    assert!(
        out.contains("INTERP: 11"),
        "bytecode dispatch loop wrong:\n{out}"
    );
}

#[test]
fn computed_goto_dispatch_table() {
    if !has_m32() {
        eprintln!("skipping computed-goto test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // A GCC computed goto (&&label) compiles to `mov reg,[tab+idx*4]; jmp reg` —
    // an absolute-address dispatch table, the shape of an interpreter loop (Lua's
    // luaV_execute). The lifter must resolve the table targets and recover the
    // index. Expected: each op reaches its own label.
    let out = transpile_and_run("computed_goto.exe");
    assert!(
        out.contains("CGOTO: 6 15 105 1005"),
        "computed-goto dispatch mis-resolved:\n{out}"
    );
}

#[test]
fn switch_with_duplicate_case_targets() {
    if !has_m32() {
        eprintln!("skipping switch-dup test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // A jump-table switch where several cases share a body (1,2,4 -> 2000),
    // producing duplicate jump-table entries. The structurer maps case k ->
    // successors[k]; collapsing duplicate targets shifts every later case onto the
    // wrong block (the bug that routed a Lua GC userdata into the upvalue case).
    // Expected: each index reaches its own arm.
    let out = transpile_and_run("switch_dup_targets.exe");
    assert!(
        out.contains("JT: 1000 2001 2002 3003 2004 5005"),
        "switch with duplicate case targets mis-dispatched:\n{out}"
    );
}

#[test]
fn equality_compare_against_negative_immediate() {
    if !has_m32() {
        eprintln!("skipping eq-cmp test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // A countdown loop terminating on `i != -1` (cmp r32, -1; jne). The
    // sign-extended 64-bit immediate must be truncated to the 32-bit counter
    // width, or the loop never sees equality and runs off the end (the bug behind
    // lua_pushcclosure's upvalue loop). Expected: 10 0.
    let out = transpile_and_run("eq_cmp_negimm.exe");
    assert!(
        out.contains("EQIMM: 10 0"),
        "equality compare vs negative immediate wrong:\n{out}"
    );
}

#[test]
fn signed_compare_against_negative_immediate() {
    if !has_m32() {
        eprintln!("skipping signed-cmp test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // `cmp r32, imm32; jge` where the immediate is a negative 32-bit constant
    // (-1000999). It must be sign-extended for the signed comparison, not read as
    // the zero-extended +4293913049 — the bug behind Lua's index2value treating a
    // negative (pseudo) stack index as a huge positive one. Expected: 1 0 1.
    let out = transpile_and_run("signed_cmp_negimm.exe");
    assert!(
        out.contains("SIGNCMP: 1 0 1"),
        "signed compare vs negative immediate wrong:\n{out}"
    );
}

#[test]
fn fp_value_returned_across_a_call() {
    if !has_m32() {
        eprintln!("skipping fp-return test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // A double returned in st(0) and compared by the caller (fucomi). The x87
    // depth analysis must count the call's st(0) push and the value must be
    // threaded through the fp return channel — otherwise the comparison runs on
    // an undefined operand (the bug behind Lua's always-failing version check).
    let out = transpile_and_run("fp_return_call.exe");
    assert!(
        out.contains("FPRET: match"),
        "fp value not returned across the call:\n{out}"
    );
}

#[test]
fn setjmp_longjmp_nonlocal_exit() {
    if !has_m32() {
        eprintln!("skipping setjmp test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // msvcrt setjmp (`_setjmp3`) is reached through an import thunk; longjmp
    // unwinds five frames back to it. Exercises import-thunk resolution and the
    // setjmp/longjmp intrinsics (host setjmp/longjmp expanded at the lifted call
    // site, in the caller's own native frame). Expected: the longjmp value, 42.
    let out = transpile_and_run("setjmp_longjmp.exe");
    assert!(
        out.contains("SETJMP: caught=42"),
        "setjmp/longjmp non-local exit wrong:\n{out}"
    );
}

#[test]
fn win32_native_kernel32_layer() {
    if !has_m32() {
        eprintln!("skipping Win32 test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // kernel32 surface mapped to native POSIX (aret_win32.c): heap (zero-init),
    // interlocked atomics, environment round-trip, lstr*, monotonic timing.
    let out = transpile_and_run("hello_win32api.exe");
    assert!(
        out.contains("W32: zero=1 ctr=41 ev=ok evlen=2 s=win32 slen=5 mono=1"),
        "native Win32 layer wrong:\n{out}"
    );
}

/// Transpile `fixture` starting at the named symbol, run it, return stdout.
fn transpile_entry_run(fixture_name: &str, entry: &str) -> String {
    let fixture = format!("{}/tests/m1/fixtures/{}", env!("CARGO_MANIFEST_DIR"), fixture_name);
    let out_dir = std::env::temp_dir().join(format!(
        "aret_e_{}_{}", fixture_name.replace('.', "_"), std::process::id()));
    let aret = env!("CARGO_BIN_EXE_aret");
    let output = Command::new(aret)
        .args(["--mode", "transpile", "--run", "--entry", entry, "--out-dir"])
        .arg(&out_dir).arg(&fixture)
        .output().expect("failed to run aret");
    let stdout = String::from_utf8_lossy(&output.stdout).into_owned();
    assert!(output.status.success(), "transpile failed:\n{stdout}");
    let _ = std::fs::remove_dir_all(&out_dir);
    stdout
}

#[test]
fn real_full_crt_program_from_main() {
    if !has_m32() {
        eprintln!("skipping full-CRT test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // A normally-compiled mingw program (full CRT statically linked: crt2.o,
    // libmingwex, __main). We lift from `main` (resolved by symbol) and *bind*
    // the statically-linked CRT (printf/malloc/strlen/free) to native shims by
    // recognizing their symbols — instead of lifting the CRT's indirect-call
    // machinery. argc/argv are threaded onto the machine stack.
    let out = transpile_entry_run("hello_realcrt.exe", "main");
    assert!(out.contains("REALCRT: argc=1"), "argc not threaded:\n{out}");
    assert!(
        out.contains("REALCRT: heap=real crt heap len=13"),
        "real CRT heap/string path wrong:\n{out}"
    );
}

#[test]
fn stripped_full_crt_via_flirt() {
    if !has_m32() {
        eprintln!("skipping FLIRT test: `cc -m32` unavailable");
        return;
    }
    // Strip the full-CRT fixture of all symbols, then transpile it. With no
    // symbols, the statically-linked CRT (printf/malloc/strlen/free) and the
    // startup glue (__main) must be recognized by FLIRT-lite *byte signatures*
    // and bound natively — otherwise the CRT's indirect-call machinery crashes.
    let strip = "i686-w64-mingw32-strip";
    if Command::new(strip).arg("--version").stdout(Stdio::null()).stderr(Stdio::null())
        .status().map(|s| !s.success()).unwrap_or(true)
    {
        eprintln!("skipping FLIRT test: {strip} unavailable");
        return;
    }
    let src = format!("{}/tests/m1/fixtures/hello_realcrt.exe", env!("CARGO_MANIFEST_DIR"));
    let stripped = std::env::temp_dir().join(format!("aret_stripped_{}.exe", std::process::id()));
    std::fs::copy(&src, &stripped).unwrap();
    assert!(Command::new(strip).arg(&stripped).status().unwrap().success(), "strip failed");

    let aret = env!("CARGO_BIN_EXE_aret");
    let out_dir = std::env::temp_dir().join(format!("aret_flirt_{}", std::process::id()));
    // Fully symbol-free: `main` is *discovered* from the startup call pattern,
    // and the statically-linked CRT is recognized by FLIRT byte signatures.
    let output = Command::new(aret)
        .args(["--mode", "transpile", "--run", "--entry", "main", "--out-dir"])
        .arg(&out_dir).arg(&stripped)
        .output().expect("failed to run aret");
    let stdout = String::from_utf8_lossy(&output.stdout);
    assert!(output.status.success(), "stripped transpile failed:\n{stdout}");
    assert!(
        stdout.contains("REALCRT: heap=real crt heap len=13"),
        "FLIRT/main-discovery did not handle the stripped binary:\n{stdout}"
    );
    let _ = std::fs::remove_file(&stripped);
    let _ = std::fs::remove_dir_all(&out_dir);
}

#[test]
fn win32_system_info_and_sync() {
    if !has_m32() {
        eprintln!("skipping Win32-sys test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // Broader kernel32: GetSystemInfo, GetACP, GetModuleFileNameA,
    // GetSystemDirectoryA, mutex/wait/release — all mapped to native/benign.
    // feat=1: IsProcessorFeaturePresent is imported and reached via an import
    // thunk; resolving the thunk binds the call to the real shim (returns 1),
    // instead of the old behavior where the unresolved thunk path returned 0.
    let out = transpile_and_run("hello_win32sys.exe");
    assert!(
        out.contains("W32SYS: page=4096 cpus=1 acp=1252 path=C:\\program.exe sysdir=C:\\Windows\\System32 feat=1 mtx=1 wait=0 rel=1"),
        "broader Win32 layer wrong:\n{out}"
    );
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

#[test]
fn auto_main_entry_skips_crt_startup() {
    if !has_m32() {
        eprintln!("skipping auto-entry test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // The PE entry is a CRT bootstrap (`_mainCRTStartup@0`) that prints a "boot"
    // marker before calling main. With auto-`main` selection the transpiler
    // starts at `main` and never runs the bootstrap, so only "main" prints.
    let out = transpile_and_run("auto_main_entry.exe");
    assert!(
        out.contains("AUTOENTRY: main"),
        "main marker missing (did the entry redirect to main?):\n{out}"
    );
    assert!(
        !out.contains("AUTOENTRY: boot"),
        "bootstrap ran — entry was not redirected to main:\n{out}"
    );
}
