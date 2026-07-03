//! Axis-2 (OS/CRT) import-coverage report: `aret --mode imports` classifies a
//! binary's *whole* import table against the shipped shim set — the a-priori,
//! known-in-advance measure of how ready ARET is for a binary, independent of
//! function recovery. A measurement tool (no code generation), so it just needs
//! the loader; no toolchain gate.

use std::process::Command;

fn coverage_of(fixture: &str) -> String {
    let path = format!("{}/tests/m1/fixtures/{}", env!("CARGO_MANIFEST_DIR"), fixture);
    assert!(std::path::Path::new(&path).exists(), "missing fixture {path}");
    let out = Command::new(env!("CARGO_BIN_EXE_aret"))
        .args(["--mode", "imports", &path])
        .output()
        .expect("run aret --mode imports");
    assert!(out.status.success(), "aret --mode imports failed");
    String::from_utf8(out.stdout).expect("utf8")
}

/// A binary whose every import ARET already shims reports FULLY COVERED, with the
/// import count matching the PE's import table (here `ExitProcess` + `printf`).
#[test]
fn fully_covered_binary_reports_no_gap() {
    let r = coverage_of("address_taken_callback.exe");
    assert!(r.contains("imports:   2"), "expected 2 imports, got:\n{r}");
    assert!(r.contains("covered:   2"), "expected 2 covered, got:\n{r}");
    assert!(r.contains("FULLY COVERED"), "expected FULLY COVERED, got:\n{r}");
}

/// A real-CRT binary surfaces its exact axis-2 shim gap by name — the actionable
/// list to close with general shims. The gap must be *listed*, not hidden, and
/// the covered/uncovered split must sum to the total (honest, no double-count).
#[test]
fn real_crt_binary_lists_its_gap_by_name() {
    let r = coverage_of("hello_realcrt.exe");
    // Deterministic total for this committed fixture.
    assert!(r.contains("imports:   53"), "expected 53 imports, got:\n{r}");
    // The three data-imports this fixture pulls in have no aret_ shim — they must
    // appear verbatim in the gap list so the report is actionable, not a bare count.
    for gap in ["__initenv", "__mb_cur_max", "_iob"] {
        assert!(r.contains(gap), "gap `{gap}` missing from report:\n{r}");
    }
    assert!(r.contains("uncovered: 3"), "expected 3 uncovered, got:\n{r}");
    assert!(!r.contains("FULLY COVERED"), "must not claim full coverage:\n{r}");
}
