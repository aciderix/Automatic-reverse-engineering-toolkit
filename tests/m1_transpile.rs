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

/// Transpile `fixture` into a fresh out-dir and return the concatenated generated
/// C (the `chunk_*.c` files) so a test can inspect the recovered structure.
fn transpile_to_c(fixture_name: &str) -> String {
    let fixture = format!(
        "{}/tests/m1/fixtures/{}",
        env!("CARGO_MANIFEST_DIR"),
        fixture_name
    );
    let out_dir = std::env::temp_dir().join(format!(
        "aretc_{}_{}",
        fixture_name.replace('.', "_"),
        std::process::id()
    ));
    let aret = env!("CARGO_BIN_EXE_aret");
    let output = Command::new(aret)
        .args(["--mode", "transpile"])
        .arg("--out-dir")
        .arg(&out_dir)
        .arg(&fixture)
        .output()
        .expect("failed to run aret");
    assert!(output.status.success(), "aret transpile failed");
    let mut c = String::new();
    if let Ok(entries) = std::fs::read_dir(&out_dir) {
        for e in entries.flatten() {
            let p = e.path();
            if p.file_name().and_then(|n| n.to_str()).is_some_and(|n| n.starts_with("chunk_")) {
                c.push_str(&std::fs::read_to_string(&p).unwrap_or_default());
            }
        }
    }
    let _ = std::fs::remove_dir_all(&out_dir);
    c
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
fn reads_real_absolute_unix_path() {
    if !has_m32() {
        eprintln!("skipping abs-path test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // A native Linux tool must read real files by absolute Unix path. The path
    // sandbox used to prepend the ARET prefix to "/"-rooted paths, so a
    // pre-existing "/tmp/x" was never found. translate_path now passes
    // "/"-absolute paths through to the real filesystem. Write the file on the
    // host, then have the transpiled program read it back.
    std::fs::write("/tmp/aret_abs_fixture.txt", "HELLO_ABS\n").expect("write fixture file");
    let out = transpile_and_run("read_abs_path.exe");
    assert!(out.contains("ABSREAD=HELLO_ABS"), "absolute-path read failed:\n{out}");
}

#[test]
fn sub_flags_mask_operands_for_wide_signed_compare() {
    if !has_m32() {
        eprintln!("skipping sub-flags test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // A 64-bit signed compare `n <= -64` lowers to `cmp lo,-63; sbb hi,-1; jl`.
    // The `cmp r/m32, imm8` sign-extends the immediate to 32 bits (0xffffffc1),
    // but ARET masks values into a 64-bit C int — so CF used the unmasked
    // operands and came out wrong, the high-word sbb then mis-set SF/OF, and
    // Lua's `256 >> 2` (via luaV_shiftl(x,-n)) returned 0 instead of 64.
    // sub_flags now masks both operands to the op width before CF/ZF.
    let out = transpile_and_run("sub_flags_wide_cmp.exe");
    assert!(out.contains("r1=64 r2=1024 r3=0 r4=128"), "wide signed compare wrong:\n{out}");
}

#[test]
fn file_io_roundtrip_through_crt() {
    if !has_m32() {
        eprintln!("skipping file-io test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // fopen/fputs/fclose then fopen/fread/fclose/remove. mingw's stdio bottoms out
    // in the low-level msvcrt I/O imports (_open/_read/_write/_close), so this
    // exercises the GetFileAttributesA / _open / _lseek HLE shims. The shim must be
    // defined *after* translate_path/make_parents in aret_hle.c (else _open hits an
    // implicit declaration and the weak unimplemented stub wins).
    let out = transpile_and_run("file_io.exe");
    assert!(out.contains("FILEIO n=12 a=line1 b=line2"), "file io wrong:\n{out}");
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

#[test]
fn find_main_discovers_real_main_in_stripped_crt() {
    if !has_m32() {
        eprintln!("skipping find_main test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // A *stripped* binary whose startup calls a 3-stack-arg decoy before `main`.
    // `--entry auto` runs the call-pattern heuristic (no symbols); it must land
    // on `main` (which prints) and not the decoy (which prints nothing).
    let fixture = format!(
        "{}/tests/m1/fixtures/crt_main_discovery.exe",
        env!("CARGO_MANIFEST_DIR")
    );
    let out_dir =
        std::env::temp_dir().join(format!("aret_findmain_{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&out_dir);
    let output = Command::new(env!("CARGO_BIN_EXE_aret"))
        .args(["--mode", "transpile", "--entry", "auto", "--run"])
        .arg("--out-dir")
        .arg(&out_dir)
        .arg(&fixture)
        .output()
        .expect("failed to run aret");
    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        output.status.success(),
        "transpile failed:\nstdout:\n{stdout}\nstderr:\n{stderr}"
    );
    assert!(
        stdout.contains("MAIN argc="),
        "discovery did not land on main (decoy chosen?):\nstdout:\n{stdout}\nstderr:\n{stderr}"
    );
    let _ = std::fs::remove_dir_all(&out_dir);
}

#[test]
fn address_taken_callback_recovered_when_stripped() {
    if !has_m32() {
        eprintln!("skipping address-taken test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // A stripped, frame-pointer-omitted callback reached only through a
    // function-pointer table in .data. Neither recursive descent nor the
    // `push ebp` prologue scan finds it; address-taken discovery must, or the
    // indirect call returns 0 (unrecovered) instead of 42.
    let out = transpile_and_run("address_taken_callback.exe");
    assert!(
        out.contains("RESULT=42"),
        "address-taken callback not recovered (indirect call unresolved?):\n{out}"
    );
}

#[test]
fn soundness_verdict_reported() {
    if !has_m32() {
        eprintln!("skipping soundness test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // A fully-recovered freestanding fixture must be reported SOUND, and --strict
    // must accept it (exit 0).
    let fixture = format!(
        "{}/tests/m1/fixtures/hello_win32.exe",
        env!("CARGO_MANIFEST_DIR")
    );
    let out_dir = std::env::temp_dir().join(format!("aret_sound_{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&out_dir);
    let output = Command::new(env!("CARGO_BIN_EXE_aret"))
        .args(["--mode", "transpile", "--strict"])
        .arg("--out-dir")
        .arg(&out_dir)
        .arg(&fixture)
        .output()
        .expect("failed to run aret");
    let stdout = String::from_utf8_lossy(&output.stdout);
    assert!(output.status.success(), "--strict rejected a sound binary:\n{stdout}");
    assert!(
        stdout.contains("soundness:  SOUND"),
        "expected SOUND verdict:\n{stdout}"
    );
    let _ = std::fs::remove_dir_all(&out_dir);
}

#[test]
fn jmp_through_memory_pointer_is_an_indirect_tailcall() {
    if !has_m32() {
        eprintln!("skipping jmp[mem] test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // A no-arg thunk that tail-jumps through a function pointer in .data
    // (`jmp dword ptr [g_hook]`). It must lift to a real indirect tail call; left
    // as opaque asm it would abort (aret_unmodelled) and never print.
    let out = transpile_and_run("jmp_mem_tailcall.exe");
    assert!(
        out.contains("JT=42"),
        "jmp [mem] not lifted as an indirect tail call:\n{out}"
    );
}

#[test]
fn qsort_callback_into_transpiled_comparator() {
    if !has_m32() {
        eprintln!("skipping qsort test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // qsort's comparator is a transpiled sub_<va> with the machine-stack ABI, so
    // the shim must call it back through aret_call (a host->guest callback). If
    // qsort were an unimplemented no-op the array would print unsorted.
    let out = transpile_and_run("qsort_cb.exe");
    assert!(
        out.contains("1,2,3,5,7,9"),
        "qsort callback not dispatched into the transpiled comparator:\n{out}"
    );
}

#[test]
fn x87_fabs_is_modelled() {
    if !has_m32() {
        eprintln!("skipping fabs test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // The x87 `fabs` instruction (d9 e1) has no explicit st operand, so it was
    // missing from is_x87 and skipped by the depth pass — left as a no-op,
    // silently dropping the abs. It must now be modelled: fabs(-3.5) = 3.50.
    let out = transpile_and_run("fabsfix.exe");
    assert!(out.contains("ABS=3.50"), "x87 fabs not modelled:\n{out}");
}

#[test]
fn x87_fxam_classifies_into_status_word() {
    if !has_m32() {
        eprintln!("skipping fxam test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // The x87 `fxam` instruction classifies st(0) (NaN/Inf/zero/normal/denormal)
    // into the FPU condition codes C3/C2/C0, read back by `fnstsw ax`. It was
    // unmodelled, so the depth pass bailed the whole function to opaque asm —
    // which aborted at runtime (mingw's __sqrt/fpclassify use this idiom, so
    // math.sqrt etc. crashed). Now modelled via __x87_fxam: the mask sw & 0x4500
    // must yield NaN=256, +Inf=1280, normal=1024, zero=16384.
    let out = transpile_and_run("x87_fxam.exe");
    assert!(
        out.contains("NAN=256 INF=1280 ONE=1024 ZERO=16384"),
        "x87 fxam not modelled:\n{out}"
    );
}

#[test]
fn x87_fcmov_uses_the_real_condition() {
    if !has_m32() {
        eprintln!("skipping fcmov test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // The x87 conditional-move family (fcmovcc) selects st(0) from st(i) based on
    // the EFLAGS a prior fucomi/cmp set. iced's `condition_code()` does not cover
    // FCMOVcc (returns None), so a naive lift made the move unconditional — a
    // double `a>b?a:b` then silently returned the wrong operand, and the float
    // printf path (mingw dtoa, which runs fcmov) misformatted (7.0 -> 7.1). The
    // mnemonic now maps to its real condition. The %d columns pin the value; the
    // %f columns pin the dtoa path.
    let out = transpile_and_run("x87_fcmov.exe");
    assert!(
        out.contains("F mx=7.0 mn=-2.5 | I mx7=1 mn=1"),
        "x87 fcmov condition wrong:\n{out}"
    );
}

#[test]
fn x87_frndint_honours_rounding_mode() {
    if !has_m32() {
        eprintln!("skipping rounding test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // `frndint` rounds st(0) per the x87 control-word RC field (bits 10-11):
    // floor sets RC=01 (down), ceil sets RC=10 (up). The lifter previously only
    // distinguished truncate (RC=11) from round-to-nearest, so floor/ceil both
    // collapsed to nearest: floor(2.75) wrongly gave 3.0, floor(5.5/2)=2.0
    // happened to be right by luck but floor(-2.5) gave -3.0 (nearest-even) only
    // by coincidence. `rounding_mode_active` now links each `fldcw [X]` back to
    // the `or`-immediate that built control word [X], so every mode is honoured.
    let out = transpile_and_run("rounding.exe");
    assert!(
        out.contains("floor=2.0 ceil=3.0 fdiv=2.0 nfloor=-3.0"),
        "x87 frndint rounding mode not honoured:\n{out}"
    );
}

#[test]
fn x87_fist_truncate_with_hoisted_control_word() {
    if !has_m32() {
        eprintln!("skipping fist-trunc test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // An optimised `(long)x` cast inside a loop installs the truncate control word
    // (`or ah,0xc; mov [X]; fldcw [X]; fist`) in a block that *dominates* the loop,
    // while the `fldcw [X]; fist` sit in the loop body — separated by the loop
    // header (a join). A block-local rounding scan can't see the writer, so the
    // `fist` could not be proven truncating and the whole function bailed to asm
    // (it would abort at runtime). `rounding_mode_active` now proves the control
    // word loop-invariant by checking every writer of slot X agrees, so the cast
    // is modelled: sum of trunc(2.9*i) for i=1..5 = 2+5+8+11+14 = 40.
    let out = transpile_and_run("truncloop.exe");
    assert!(out.contains("sum=40"), "hoisted-CW truncating fist not modelled:\n{out}");
}

#[test]
fn wide_64bit_return_and_shift_on_32bit() {
    if !has_m32() {
        eprintln!("skipping wide-64 test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // Two 32-bit-ABI bugs, both silent wrong results:
    //  * a `long long` is returned in the edx:eax pair; modelling the return as
    //    eax alone dropped the high half (and dead-code-eliminated the shld/cdq
    //    that built it). `shift(1, 32)` then returned 0 instead of 4294967296.
    //  * `shl eax, cl` masks the count to 5 bits (x86), so `shl eax, 32` is a
    //    no-op; lifting it as a full `1 << 32` corrupted 64-bit shifts.
    // The matching caller-side edx:eax split and the count mask make both exact.
    let out = transpile_and_run("wide_shift.exe");
    assert!(
        out.contains("r=4294967296 m=12884901888"),
        "64-bit return / shift on 32-bit not exact:\n{out}"
    );
}

#[test]
fn wide_add_adc_carry_on_32bit() {
    if !has_m32() {
        eprintln!("skipping wide-carry test: `cc -m32` unavailable (install gcc-multilib)");
        return;
    }
    // 64-bit `(key-1) < alimit` built from `add $-1; adc $-1; cmp; sbb` (Lua's
    // luaH_getint array bound). The 32-bit add's carry-out was lifted from a
    // sign-extended immediate (0xffffffff -> 0xffff_ffff_ffff_ffff), so it came
    // out 0 instead of 1 and (2-1) became 0xffffffff00000001 -> the bound test
    // returned the wrong answer. That made Lua's registry[2] (the globals table)
    // miss the array part, so `_G` read back nil ("attempt to index a nil value").
    // The fix computes the carry from operands masked to the operation width.
    let out = transpile_and_run("wide_carry.exe");
    assert!(
        out.contains("CARRY: 1 1 0 0"),
        "64-bit add/adc carry on 32-bit not exact (the _G=nil bug):\n{out}"
    );
}

#[test]
fn adjacent_jump_tables_are_bounded() {
    if !has_m32() {
        eprintln!("skipping jump-table-bound test: `cc -m32` unavailable");
        return;
    }
    // Two dense switches (12 and 11 cases) whose jump tables gcc lays out back to
    // back in .rdata. Reading a table until the first non-code word over-reads the
    // first table straight into the second (all targets executable), merging the
    // two functions and corrupting both CFGs (this is what made Lua's intarith
    // absorb numarith and fall to asm). The `cmp idx, N; ja` bound must cap each
    // table, so no recovered function has more cases than its switch really has.
    let c = transpile_to_c("two_switch.exe");
    // opA (sub_40150e) has a 12-case switch bounded by `cmp eax,0xb; ja`. Before
    // the bound was honoured its table over-read into opB's, recovering 23 cases.
    let op_a = c
        .split("uint64_t sub_")
        .find(|f| f.starts_with("40150e("))
        .expect("opA (sub_40150e) not found in recovered output");
    let cases = op_a.matches("case ").count();
    assert!(
        cases <= 12,
        "opA's jump table over-read into opB's: {cases} cases recovered (real switch has 12)"
    );
    // And it still produces the right answer.
    let out = transpile_and_run("two_switch.exe");
    assert!(out.contains("t=3293"), "two-switch result wrong:\n{out}");
}

#[test]
fn signed_compare_of_memory_operand() {
    if !has_m32() {
        eprintln!("skipping signed-compare test: `cc -m32` unavailable");
        return;
    }
    // Recursive fib at -O0 keeps the base-case test `cmp [n],1; jle` on a memory
    // operand. The result `(uint32)0 - 1 == 0xFFFFFFFF` was sign-tested at 64-bit
    // width, where it reads as *positive*, so the sign flag (and `jle`) took the
    // wrong edge: fib(0) recursed into fib(-1)/fib(-2) and fib(5) returned -4.
    // Sign/overflow flags now read bit w-1 of the result, so a w-bit compare is
    // correct whatever C width the operand has. (Register operands masked to
    // uint64 hid the bug — which is why the differential corpus never caught it.)
    let out = transpile_and_run("recursion.exe");
    assert!(
        out.contains("fib5=5 fib10=55 fact5=120"),
        "signed compare of a memory operand is wrong:\n{out}"
    );
}

#[test]
fn computed_goto_switch_at_o0() {
    if !has_m32() {
        eprintln!("skipping -O0 switch test: `cc -m32` unavailable");
        return;
    }
    // A varied -O0 program. Its `switch` compiles to the computed-address idiom
    // `shl idx,2; add idx,table; mov tgt,[idx]; jmp tgt` (not a single base+index
    // load), which the jump-table resolver must trace through the `add idx,table`
    // to find the table — else `jmp tgt` is an unresolved indirect jump and the
    // dispatch aborts. Also exercises -O0 memory-operand signed compares (fib),
    // 64-bit mixing, and float, all of which must match the native run.
    let out = transpile_and_run("varied_o0.exe");
    for expect in ["fib=6765 pop=24", "mix=327750336 vowels=8", "ops=13921", "poly=37.8750"] {
        assert!(out.contains(expect), "missing `{expect}` in -O0 varied output:\n{out}");
    }
}

#[test]
fn prune_to_function_closure() {
    // Phase 2 — targeted conversion. `--function feature_a` must transpile ONLY
    // feature_a's transitive direct-call closure (feature_a + helper_add +
    // helper_mul = 3 functions), drive it from `main`, print "FEATURE_A: 42",
    // and leave feature_b / helper_sub / the original `main` out of the binary.
    if !has_m32() {
        eprintln!("skipping prune test: `cc -m32` unavailable");
        return;
    }
    let fixture = format!("{}/tests/m1/fixtures/prune_closure.exe", env!("CARGO_MANIFEST_DIR"));
    let out_dir = std::env::temp_dir().join(format!("aret_prune_{}", std::process::id()));
    let aret = env!("CARGO_BIN_EXE_aret");
    let output = Command::new(aret)
        .args(["--mode", "transpile", "--function", "feature_a", "--run"])
        .arg("--out-dir")
        .arg(&out_dir)
        .arg(&fixture)
        .output()
        .expect("failed to run aret");
    let stdout = String::from_utf8_lossy(&output.stdout).into_owned();
    assert!(output.status.success(), "prune transpile failed:\n{stdout}");
    // Only the closure was emitted, and it ran standalone.
    assert!(stdout.contains("functions:  3"), "expected 3-function closure:\n{stdout}");
    assert!(stdout.contains("FEATURE_A: 42"), "feature_a did not run standalone:\n{stdout}");
    assert!(!stdout.contains("FEATURE_B"), "feature_b should have been pruned:\n{stdout}");
    // The pruned code must not contain feature_b or its callee.
    let mut c = String::new();
    if let Ok(entries) = std::fs::read_dir(&out_dir) {
        for e in entries.flatten() {
            let p = e.path();
            if p.file_name().and_then(|n| n.to_str()).is_some_and(|n| n.starts_with("chunk_")) {
                c.push_str(&std::fs::read_to_string(&p).unwrap_or_default());
            }
        }
    }
    assert!(!c.contains("feature_b") && !c.contains("helper_sub"),
        "pruned closure leaked feature_b/helper_sub into the generated C");
    let _ = std::fs::remove_dir_all(&out_dir);
}
