#!/bin/bash
# Grandeur-nature gauntlet: transpile 21 varied Win32 binaries and compare
# ARET-native output vs Wine reference, bit-for-bit, in one pass.
#
# Binaries live compressed in gauntlet-bins.tar.gz (committed) and are
# auto-extracted to a scratch dir on first run. See README.md for provenance.
#
# Usage:   bench/gauntlet/score.sh            # extract + build ARET + run
#          BINS=/path/to/bins bench/gauntlet/score.sh   # use existing bins dir
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
ARET="$ROOT/target/release/aret"
BINS="${BINS:-$HERE/bins}"
OUT="${OUT:-${TMPDIR:-/tmp}/aret-gauntlet-out}"
export WINEPREFIX="${WINEPREFIX:-${TMPDIR:-/tmp}/aret-gauntlet-wp}" WINEDEBUG=-all

# Auto-extract the committed corpus if not already present.
if [ ! -d "$BINS" ] || [ -z "$(ls -A "$BINS"/*.exe 2>/dev/null)" ]; then
  mkdir -p "$BINS"
  tar xzf "$HERE/gauntlet-bins.tar.gz" -C "$BINS"
fi
[ -x "$ARET" ] || { echo "building ARET (release)..."; (cd "$ROOT" && cargo build --release -q) || exit 1; }
mkdir -p "$OUT"

norm(){ tr -d '\r' | sed -E 's#[A-Za-z]:\\[^ ]*##g; s#\./app##g; s#/tmp/[^ ]*##g; s#^(app|[a-zA-Z0-9_.-]+) ([0-9])#PROG \2#'; }
# invoke a binary ($2 = runner cmd prefix e.g. "./app" or "wine /path"); prints normalized output
invoke(){ local b="$1" R="$2"; local IN='the quick brown fox 42 times'
  case "$b" in
    nasm) $R -v 2>&1 ;;
    lua) $R -e "print(6*7, math.floor(2.5), ('ab'):rep(3), 355/113)" 2>&1 ;;
    sqlite3|sqlite3_stripped|sqlite3_full|sqlite3_full_stripped)
        $R :memory: "SELECT 6*7, hex(255), length('abcde'), abs(-9);" 2>&1 ;;
    grep|grep_stripped) printf 'apple\nbanana\nband\n' | $R -c 'ban' 2>&1 ;;
    sed) printf 'hello world\n' | $R 's/o/0/g' 2>&1 ;;
    m4|m4_stripped) printf 'define(sq,($1*$1))sq(12)\n' | $R 2>&1 ;;
    gzip|gzip_stripped)   printf '%s' "$IN" | $R -c 2>/dev/null | $R -dc 2>/dev/null ;;
    bzip2|bzip2_stripped) printf '%s' "$IN" | $R -c 2>/dev/null | $R -dc 2>/dev/null ;;
    minigzip|minigzip_stripped) printf '%s' "$IN" | $R 2>/dev/null | $R -d 2>/dev/null ;;
    units|units_stripped) $R -t '3 ft' 'cm' 2>&1 ;;
    hello|hello_stripped) $R --version 2>&1 ;;
    *) $R --version 2>&1 ;;
  esac; }
printf "%-22s %-6s %-8s %-8s %s\n" BINARY LIFTED SOUND VERDICT NOTE
printf '%.0s-' {1..96}; echo
tot=0; ok=0; declare -A byv
for exe in "$BINS"/*.exe; do
  b=$(basename "$exe" .exe); tot=$((tot+1)); od="$OUT/$b"; rm -rf "$od"
  rep=$($ARET "$exe" --mode transpile --out-dir "$od" 2>&1)
  cls=$(echo "$rep"|grep -oE '[0-9]+ lifted'|grep -oE '^[0-9]+'); snd=$(echo "$rep"|grep -qi "SOUND —"&&echo SND||echo INC)
  t=$(cd "$od" && timeout 30 bash -c "$(declare -f invoke norm); invoke '$b' './app'" 2>&1 | grep -v "unimplemented import" | norm)
  w=$(timeout 45 bash -c "$(declare -f invoke norm); invoke '$b' 'wine $exe'" 2>/dev/null | norm)
  if echo "$t"|grep -qiE "unmodelled|aborting"; then v=ABORT
  elif echo "$t"|grep -qi "segmentation"; then v=CRASH
  elif [ -z "$t$w" ]; then v=EMPTY2
  elif [ "$t" = "$w" ]; then v=MATCH; ok=$((ok+1))
  else v=DIFF; fi
  byv[$v]=$(( ${byv[$v]:-0} + 1 ))
  note=""; case "$v" in ABORT) note=$(echo "$t"|grep -oiE "unrecovered function 0x[0-9a-f]+|instruction: [a-z0-9 ]+"|head -1);;
    DIFF) note="t[$(echo "$t"|head -1|cut -c1-20)] w[$(echo "$w"|head -1|cut -c1-20)]";; esac
  printf "%-22s %-6s %-8s %-8s %s\n" "$b" "${cls:-?}" "$snd" "$v" "$note"
done
printf '%.0s-' {1..96}; echo
echo "SCORE: $ok/$tot MATCH   | breakdown: ${!byv[@]} => ${byv[@]}"
