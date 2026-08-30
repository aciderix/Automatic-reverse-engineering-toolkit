#!/usr/bin/env bash
# Provision the GLib/GObject runtime DLLs (and their transitive deps) into
# bench/.cache/glib so the `glib_*` winediff fixtures can prove ARET's GLib lifting:
# `.withlocaldll` lifts libglib/libgobject, `.winelibs` gives the Wine oracle their
# deps. The DLLs are NOT committed (large, trivially re-fetchable); THIS script is the
# reproducible artifact. Idempotent: skips a DLL already present.
#
#   bash bench/glib_fetch.sh          # populate bench/.cache/glib
#   bash bench/winediff.sh glib_core  # then the fixture stops SKIPping
#
# Source: MSYS2 mingw32 (repo.msys2.org), the same FOSS toolchain the wall corpus uses.
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$here/.cache/glib"          # runtime DLLs (beside the exe for ARET/Wine)
dev="$out/dev"                   # headers + import libs (to COMPILE the fixture)
mkdir -p "$out" "$dev"
BASE="https://repo.msys2.org/mingw/mingw32/"

# The exact runtime DLLs the glib_* fixtures need beside the exe. libglib/libgobject are
# lifted by ARET (.withlocaldll); the rest are Wine-only deps (.winelibs).
WANT="libglib-2.0-0.dll libgobject-2.0-0.dll libgcc_s_dw2-1.dll libintl-8.dll \
      libiconv-2.dll libpcre2-8-0.dll libwinpthread-1.dll libcharset-1.dll libffi-8.dll"

have_all() {
  local f; for f in $WANT; do [ -f "$out/$f" ] || return 1; done
  [ -f "$dev/include/glib-2.0/glib.h" ] && [ -f "$dev/lib/libglib-2.0.dll.a" ] \
    && [ -f "$dev/include/libintl.h" ] && [ -f "$dev/lib/libintl.dll.a" ]
}
if have_all; then echo "== GLib runtime already present in $out =="; ls "$out"; exit 0; fi

idx="$(curl -sS -m 60 "$BASE" || true)"
[ -n "$idx" ] || { echo "ERREUR : index MSYS2 injoignable"; exit 1; }
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

# Fetch one package prefix: extract the runtime DLLs we still want (mingw32/bin) and,
# for glib2, the dev tree (headers + import libs) needed to compile the fixture.
fetch() {
  local prefix="$1" want_dev="${2:-}"
  local pk; pk="$(printf '%s\n' "$idx" | grep -oE "mingw-w64-i686-$prefix-[0-9][^\"]+\.pkg\.tar\.zst" \
                   | grep -v '\.sig' | sort -u | tail -1)"
  [ -n "$pk" ] || { echo "  ?? aucun paquet pour $prefix"; return; }
  local members='mingw32/bin'
  [ -n "$want_dev" ] && members='mingw32/bin mingw32/include mingw32/lib'
  curl -sS -m 180 -L "$BASE$pk" -o "$tmp/p.zst" 2>/dev/null \
    && tar --use-compress-program=unzstd -xf "$tmp/p.zst" -C "$tmp" $members 2>/dev/null \
    || { echo "  ?? échec extraction $prefix ($pk)"; return; }
  local f b
  for f in "$tmp"/mingw32/bin/*.dll; do
    [ -f "$f" ] || continue
    b="$(basename "$f")"
    case " $WANT " in *" $b "*) [ -f "$out/$b" ] || cp "$f" "$out/$b" ;; esac
  done
  if [ -n "$want_dev" ]; then
    cp -r "$tmp"/mingw32/include/. "$dev/include/" 2>/dev/null      # glib.h, libintl.h, …
    mkdir -p "$dev/lib"
    cp -r "$tmp"/mingw32/lib/glib-2.0 "$dev/lib/" 2>/dev/null       # glibconfig.h
    cp "$tmp"/mingw32/lib/libg{lib,object,io,module,thread}-2.0.dll.a "$dev/lib/" 2>/dev/null
    cp "$tmp"/mingw32/lib/libintl.dll.a "$dev/lib/" 2>/dev/null     # for the libintl lift gate
  fi
  rm -rf "$tmp/mingw32"
  echo "  ok $prefix ($pk)"
}

echo "== fetching GLib runtime + dev from MSYS2 mingw32 =="
fetch glib2 dev
fetch gettext dev
for p in gcc-libs libiconv pcre2 libwinpthread libffi; do fetch "$p"; done

miss=""
for f in $WANT; do [ -f "$out/$f" ] || miss="$miss $f"; done
[ -f "$dev/include/glib-2.0/glib.h" ] || miss="$miss glib.h"
[ -f "$dev/lib/libglib-2.0.dll.a" ]   || miss="$miss libglib-2.0.dll.a"
if [ -n "$miss" ]; then echo "== INCOMPLET, manque :$miss =="; exit 1; fi
echo "== GLib runtime ready in $out (dev in $dev) =="
ls "$out"
