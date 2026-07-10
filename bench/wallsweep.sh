#!/usr/bin/env bash
# Corpus wall sweep — aggregate `aret --mode walls` across a directory of 32-bit
# PEs to find the coverage gaps that block the MOST binaries (breadth), so a fix
# is prioritised by data, not intuition. This is the "dégrossissement" pass over a
# large corpus: one static map per binary, aggregated.
#
#   bash bench/wallsweep.sh <dir-of-exes>   [more-dirs...]
#
# Ranks each gap by the number of DISTINCT binaries it appears in (a family in 40
# programs beats one with 500 sites in a single program), then by total sites.
#
# IMPORTANT — this is a COVERAGE map, not a correctness check. `--mode walls`
# enumerates statically-decidable gaps (unmodelled instructions, unimplemented
# imports, unresolved calls). It says WHERE to look. Every fix it motivates MUST
# still be proven by the differential oracles (cpudiff/funcdiff/winediff/sweeps) —
# walls never certifies behaviour, and behaviour bugs (miscompiles) are invisible
# to it by construction. The two are used together, always.
set -u
ARET="${ARET:-target/release/aret}"
[ $# -ge 1 ] || { echo "usage: bash bench/wallsweep.sh <dir-of-exes> [more-dirs...]"; exit 2; }

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
n=0; skipped=0
for dir in "$@"; do
  for f in "$dir"/*; do
    [ -f "$f" ] || continue
    # PE check: DOS 'MZ' magic (0x4d5a). Skip non-PE files silently.
    magic="$(od -An -tx1 -N2 "$f" 2>/dev/null | tr -d ' ')"
    [ "$magic" = "4d5a" ] || { skipped=$((skipped+1)); continue; }
    name="$(basename "$f")"
    if "$ARET" "$f" --mode walls >"$tmp/$name.walls" 2>/dev/null && \
       grep -q "ARET wall map" "$tmp/$name.walls"; then
      n=$((n+1))
    else
      echo "  (analysis failed: $name)" >&2
      rm -f "$tmp/$name.walls"
    fi
  done
done
echo "analyzed $n PE(s) ($skipped non-PE skipped)"

python3 - "$tmp" "$n" <<'PY'
import sys, os, glob, re
d, ntotal = sys.argv[1], int(sys.argv[2])
PREFIXES = {"rep","repe","repne","repz","repnz","lock"}
def mnem(text):
    t = text.split()
    if not t: return text
    if t[0] in PREFIXES and len(t) > 1: return t[0] + " " + t[1]
    return t[0]

insn_bins, insn_sites = {}, {}     # mnemonic -> set(bin) / total sites
imp_bins = {}                      # import   -> set(bin)
clean = 0
for wf in sorted(glob.glob(os.path.join(d, "*.walls"))):
    b = os.path.basename(wf)[:-6]
    sec = None; n_insn = n_imp = n_unres = 0
    for line in open(wf, encoding="utf-8", errors="replace"):
        if "UNMODELLED INSTRUCTIONS" in line:
            sec = "insn"; m = re.search(r'(\d+) distinct', line); n_insn = int(m.group(1)) if m else 0; continue
        if "UNIMPLEMENTED IMPORTS" in line:
            sec = "imp"; m = re.search(r'—\s*(\d+)', line); n_imp = int(m.group(1)) if m else 0; continue
        if "UNRESOLVED DIRECT CALLS" in line:
            sec = "unres"; m = re.search(r'—\s*(\d+)', line); n_unres = int(m.group(1)) if m else 0; continue
        s = line.strip()
        if not s or s.startswith("("): continue
        if sec == "insn":
            m = re.match(r'(\d+)\s+(.*)', s)
            if m:
                mn = mnem(m.group(2)); insn_bins.setdefault(mn, set()).add(b)
                insn_sites[mn] = insn_sites.get(mn, 0) + int(m.group(1))
        elif sec == "imp":
            imp_bins.setdefault(s, set()).add(b)
    if n_insn == 0 and n_imp == 0 and n_unres == 0:
        clean += 1

print(f"\n=== CORPUS WALL SWEEP — {ntotal} binaries ({clean} fully clean / {ntotal-clean} with gaps) ===")

print("\nTOP UNMODELLED INSTRUCTIONS (lift gaps) — ranked by #binaries blocked:")
print(f"  {'bins':>4}  {'sites':>6}  mnemonic")
rows = sorted(insn_bins.items(), key=lambda kv: (-len(kv[1]), -insn_sites[kv[0]], kv[0]))
for mn, bins in rows[:30]:
    print(f"  {len(bins):>4}  {insn_sites[mn]:>6}  {mn}")
if not rows: print("  (none)")

print("\nTOP UNIMPLEMENTED IMPORTS (HLE gaps) — ranked by #binaries:")
print(f"  {'bins':>4}  import")
rows = sorted(imp_bins.items(), key=lambda kv: (-len(kv[1]), kv[0]))
for name, bins in rows[:40]:
    print(f"  {len(bins):>4}  {name}")
if not rows: print("  (none)")
PY
