#!/usr/bin/env python3
"""Diff the real-Windows fingerprints against the Wine fingerprints — the finding.

The windows-oracle `probe` job (windows-latest) uploads, for every eligible fixture it
built and ran, the RAW output file `fp/<name>.txt`. `bench/winoracle/wine_hashes.sh`
prints `<name> OK <sha256>` for the SAME fixtures under Wine. We re-hash the Windows raw
outputs here (CRLF-stripped, exactly as both sides hash) and compare to the Wine hashes.

The fixtures whose hashes DIFFER on the two Win32 implementations ARE the finding
(doc 70 §1: where Wine is both oracle and reference, we verify Wine against Wine; a real
Windows disagreement breaks that circle). This is a MEASUREMENT, never a gate — it always
exits 0. A divergence is something to think about, not a regression to silence.

  argv[1] = dir holding the downloaded probe artifact (expects <dir>/fp/<name>.txt)
  argv[2] = the wine_hashes.sh output file (`<name> OK <sha256>` / `<name> <SKIP…>` lines)
"""
from __future__ import annotations

import hashlib
import sys
from pathlib import Path


def win_hashes(artifact_dir: Path) -> dict[str, str]:
    """name -> sha256 of the CRLF-stripped Windows output (matches both sides' hashing)."""
    out: dict[str, str] = {}
    fp = artifact_dir / "fp"
    for f in sorted(fp.glob("*.txt")) if fp.is_dir() else []:
        raw = f.read_bytes().replace(b"\r", b"")
        out[f.stem] = hashlib.sha256(raw).hexdigest()
    return out


def wine_hashes(path: Path) -> tuple[dict[str, str], dict[str, str]]:
    """Parse wine_hashes.sh output -> (name->hash for OK, name->status for non-OK)."""
    ok: dict[str, str] = {}
    other: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] == "OK":
            ok[parts[0]] = parts[2].lower()
        elif len(parts) >= 2:
            other[parts[0]] = " ".join(parts[1:])
    return ok, other


def main() -> int:
    art = Path(sys.argv[1] if len(sys.argv) > 1 else "winruntime")
    wine_file = Path(sys.argv[2] if len(sys.argv) > 2 else "wine_fp.txt")

    win = win_hashes(art)
    wine_ok, _wine_other = wine_hashes(wine_file)

    both = sorted(set(win) & set(wine_ok))
    win_only = sorted(set(win) - set(wine_ok))
    wine_only = sorted(set(wine_ok) - set(win))

    diverging = [(n, win[n], wine_ok[n]) for n in both if win[n] != wine_ok[n]]
    agreeing = [n for n in both if win[n] == wine_ok[n]]

    print(f"compared {len(both)} fixtures OK on BOTH Windows and Wine")
    print(f"  agree:     {len(agreeing)}")
    print(f"  DIVERGE:   {len(diverging)}")
    print(f"  windows-only OK (not comparable): {len(win_only)}")
    print(f"  wine-only OK    (not comparable): {len(wine_only)}")
    print()
    if diverging:
        print("=== DIVERGENCES (Windows vs Wine) — the finding ===")
        print(f"  {'fixture':<32}  {'windows sha256':<16}  {'wine sha256'}")
        for n, wh, eh in diverging:
            print(f"  {n:<32}  {wh[:16]}  {eh[:16]}")
    else:
        print("no Windows/Wine divergence among comparable fixtures")

    # MEASUREMENT, never a gate: a divergence is a finding, not a red build.
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
