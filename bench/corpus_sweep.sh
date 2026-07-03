#!/usr/bin/env bash
# Broad, STATIC axis-2 measurement across a *corpus* of real binaries — the
# "measure large, don't assert" tool for prioritising HLE work. For every PE in
# the corpus directory it runs `aret --mode imports` (no execution: pure loader +
# import-table classification, safe on any binary of any provenance) and, with
# DEEP=1, `aret --mode transpile` for the soundness verdict. It then AGGREGATES:
# how many distinct binaries each still-unshimmed import appears in — so the top
# of the list is exactly the set of *general* shims that unlock the most programs
# (never a per-binary patch).
#
# Corpus dir: $1, or $CORPUS_DIR, or bench/.cache/corpus. The binaries are NOT
# committed (large / third-party) — point this at a directory you populated. A
# packed binary shows only its packer stub's imports; run `--mode unpack` first.
#
# Skips (does not fail) when the aret binary or the corpus is absent.
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
ARET="${ARET:-$DIR/../target/release/aret}"
ARET="$(readlink -f "$ARET" 2>/dev/null || echo "$ARET")"
CORPUS="${1:-${CORPUS_DIR:-$DIR/.cache/corpus}}"
DEEP="${DEEP:-0}"   # DEEP=1 also runs --mode transpile for the soundness verdict

[ -x "$ARET" ] || { echo "SKIP (aret not built: $ARET)"; exit 0; }
[ -d "$CORPUS" ] || { echo "SKIP (no corpus dir: $CORPUS)"; exit 0; }
shopt -s nullglob
bins=("$CORPUS"/*.exe "$CORPUS"/*.dll)
[ "${#bins[@]}" -gt 0 ] || { echo "SKIP (no binaries in $CORPUS)"; exit 0; }

AGG="$(mktemp)"; trap 'rm -f "$AGG"' EXIT

printf "%-22s %8s %8s %6s" "binary" "imports" "covered" "pct"
[ "$DEEP" = 1 ] && printf "  %-11s" "soundness"
printf "\n"
printf -- "---------------------------------------------------------------\n"

for f in "${bins[@]}"; do
  n="$(basename "$f")"
  rep="$("$ARET" --mode imports "$f" 2>/dev/null)"
  # Anchor with `^ *` so "covered:" does not also match "uncovered:".
  tot=$(printf '%s' "$rep" | sed -n 's/^ *imports:[[:space:]]*\([0-9]*\).*/\1/p')
  cov=$(printf '%s' "$rep" | sed -n 's/^ *covered:[[:space:]]*\([0-9]*\).*/\1/p')
  [ -n "$tot" ] || { printf "%-22s  (not a PE / no imports)\n" "$n"; continue; }
  pct=$(( tot > 0 ? cov * 100 / tot : 100 ))
  # Collect this binary's uncovered import names (one per line after the header).
  printf '%s' "$rep" | awk '/shim gap for this binary/{f=1;next} f&&/^    /{gsub(/^ +/,"");print}' \
    | sort -u >> "$AGG"
  printf "%-22s %8s %8s %5s%%" "$n" "$tot" "$cov" "$pct"
  if [ "$DEEP" = 1 ]; then
    tout="$(mktemp -d)"
    verdict=$(timeout 180 "$ARET" --mode transpile --out-dir "$tout/t" "$f" 2>/dev/null \
      | sed -n 's/.*soundness:[[:space:]]*\([A-Z]*\).*/\1/p')
    printf "  %-11s" "${verdict:-FAIL}"
    rm -rf "$tout"
  fi
  printf "\n"
done

echo ""
echo "=== highest-leverage missing shims (uncovered in the most binaries) ==="
echo "    #bins  import"
sort "$AGG" | uniq -c | sort -rn | head -30 | awk '{printf "    %5d  %s\n", $1, $2}'
