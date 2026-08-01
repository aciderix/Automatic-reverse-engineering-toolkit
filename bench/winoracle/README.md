# `bench/winoracle` — the real-Windows oracle

`winediff` compares ARET against **Wine**. That is the working oracle and it stays
the gate. This directory is the *second* oracle: the same probes run on a real
`windows-latest` runner, which is the Win32 the original binaries were built against.

It exists because of a weakness the doctrine (70 §1) always recorded honestly:
where Wine is both the oracle and the reference, we verify Wine against Wine. The
first four fixtures put through the runner produced **two** divergences on behaviour
ARET had already shipped and the Wine gate had already passed — so the weakness is
demonstrated, not theoretical.

## Deliberately not a gate

Nothing here can turn red and block work. A Windows/Wine disagreement is a finding
to think about, not a regression to silence, and a check that goes red for reasons
nobody should fix reflexively teaches people to ignore red.

## How it is used, in two steps

Whole-corpus comparison is done by **fingerprint first, detail second**, so the
signal is a short list rather than a hundred pages of log:

1. `.github/workflows/windows-oracle.yml` builds every eligible fixture with 32-bit
   MSVC, runs it, and prints one line per fixture: `name  status  sha256`.
2. `bench/winoracle/wine_hashes.sh` produces the *same* list locally under Wine.
   Diff the two: the fixtures whose hashes differ are the entire finding.
3. For those, and only those, a targeted probe prints full output on both sides —
   the way `win32_pathdisputed.c` was used to settle PathIsUNCServer.

Both sides normalise CRLF to LF before hashing, because the two toolchains differ on
line endings for reasons that have nothing to do with Win32 behaviour.

## Which fixtures are eligible

A fixture is skipped when the comparison would measure the toolchain rather than the
API: GCC inline assembly (MSVC cannot compile it), a companion `.rc` or `.def`, a
window-creating program (no interactive desktop on the runner), or a `.nodisplay`
marker. Skips are REPORTED, never silent — a shrinking eligible set would otherwise
look like a shrinking set of problems.

## Adding a probe

Put it in this directory and it runs. Note the workflow's `paths:` filter: a commit
touching only files outside those paths will not re-run anything.
