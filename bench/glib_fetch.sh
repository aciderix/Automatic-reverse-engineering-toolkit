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

# REPRODUCIBILITY PIN (§0). The winediff gate must prove ARET against the SAME binary in
# dev and CI — an unpinned `tail -1` makes the two machines test DIFFERENT glibs the day
# MSYS2 rolls forward. glib 2.88.3 is proven bit-identique to the Wine oracle (winediff
# 296/296). glib >= 2.90 hits a REAL, separate ARET wall — an unrecovered indirect call
# at 0x503390 (glib_core/glib_object abort). That wall is NOT masked here: it is a tracked
# frontier (its own KN), and abort is the §0-safe state. MSYS2 is a rolling repo (it drops
# old packages), so we select this EXACT version and FAIL LOUDLY if it is gone — never a
# silent drift to latest — so the reserve (a committed DLL snapshot) kicks in visibly.
# libglib/libgobject/libgio/libgmodule all ship in the glib2 package, so one pin fixes the
# whole lifted glib family. Other deps stay at latest: only glib2 carries the measured wall.
PINNED_GLIB="2.88.3"
declare -A PIN=( [glib2]="$PINNED_GLIB" )

# The glib version actually present in the dev tree (the single source of truth for the
# §0 self-check below). Empty if the headers are absent.
glib_version() {
  local gc="$dev/lib/glib-2.0/include/glibconfig.h"
  [ -f "$gc" ] || return 1
  local M m u
  M="$(grep -hoE 'GLIB_MAJOR_VERSION [0-9]+' "$gc" | awk '{print $2}')"
  m="$(grep -hoE 'GLIB_MINOR_VERSION [0-9]+' "$gc" | awk '{print $2}')"
  u="$(grep -hoE 'GLIB_MICRO_VERSION [0-9]+' "$gc" | awk '{print $2}')"
  [ -n "$M" ] && [ -n "$m" ] && [ -n "$u" ] || return 1
  printf '%s.%s.%s' "$M" "$m" "$u"
}

# The exact runtime DLLs the glib_* fixtures need beside the exe. libglib/libgobject are
# lifted by ARET (.withlocaldll); the rest are Wine-only deps (.winelibs).
# The second block adds the gdk-pixbuf PNG-decode closure (glib_gdkpng): gio/gmodule +
# gdk-pixbuf + libpng/zlib (all lifted), plus libgdk_pixbuf's codec siblings
# (jpeg/tiff and libtiff's own deps) — never used on the PNG path but the strict Wine
# loader must resolve them to load libgdk_pixbuf, so the oracle needs them present.
WANT="libglib-2.0-0.dll libgobject-2.0-0.dll libgcc_s_dw2-1.dll libintl-8.dll \
      libiconv-2.dll libpcre2-8-0.dll libwinpthread-1.dll libcharset-1.dll libffi-8.dll \
      libgio-2.0-0.dll libgmodule-2.0-0.dll libgdk_pixbuf-2.0-0.dll libpng16-16.dll zlib1.dll \
      libjpeg-8.dll libtiff-6.dll libLerc.dll libdeflate.dll libjbig-0.dll liblzma-5.dll \
      libwebp-7.dll libzstd.dll libsharpyuv-0.dll"

have_all() {
  local f; for f in $WANT; do [ -f "$out/$f" ] || return 1; done
  [ -f "$dev/include/glib-2.0/glib.h" ] && [ -f "$dev/lib/libglib-2.0.dll.a" ] \
    && [ -f "$dev/include/libintl.h" ] && [ -f "$dev/lib/libintl.dll.a" ] \
    && [ "$(glib_version 2>/dev/null)" = "$PINNED_GLIB" ]   # a drifted cache re-fetches
}
if have_all; then echo "== GLib runtime already present in $out =="; ls "$out"; exit 0; fi

idx="$(curl -sS -m 60 "$BASE" || true)"
[ -n "$idx" ] || { echo "ERREUR : index MSYS2 injoignable"; exit 1; }
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

# Fetch one package prefix: extract the runtime DLLs we still want (mingw32/bin) and,
# for glib2, the dev tree (headers + import libs) needed to compile the fixture.
fetch() {
  local prefix="$1" want_dev="${2:-}"
  local pin="${PIN[$prefix]:-}" pk
  if [ -n "$pin" ]; then
    # Pinned: select this exact version only. If it is gone (rolling repo), DON'T fall
    # back to latest — leave it unfetched so the miss-check below fails loudly.
    pk="$(printf '%s\n' "$idx" | grep -oE "mingw-w64-i686-$prefix-$pin-[0-9][^\"]*\.pkg\.tar\.zst" \
          | grep -v '\.sig' | sort -u | tail -1)"
    [ -n "$pk" ] || { echo "  !! $prefix épinglé $pin ABSENT de msys2 (repo rolling) — réserve = snapshot DLL"; return; }
  else
    pk="$(printf '%s\n' "$idx" | grep -oE "mingw-w64-i686-$prefix-[0-9][^\"]+\.pkg\.tar\.zst" \
          | grep -v '\.sig' | sort -u | tail -1)"
  fi
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
# gdk-pixbuf PNG-decode closure (glib_gdkpng) + libgdk_pixbuf's codec siblings and their
# transitive deps (present only so the Wine oracle's strict loader can resolve
# libgdk_pixbuf; never called on the PNG path). Best-effort package prefixes — a DLL
# whose package is missing just leaves the glib_gdkpng fixture to SKIP cleanly.
for p in gdk-pixbuf2 libpng zlib libjpeg-turbo libtiff libwebp zstd libdeflate xz jbigkit liblerc; do
  fetch "$p"
done

miss=""
for f in $WANT; do [ -f "$out/$f" ] || miss="$miss $f"; done
[ -f "$dev/include/glib-2.0/glib.h" ] || miss="$miss glib.h"
[ -f "$dev/lib/libglib-2.0.dll.a" ]   || miss="$miss libglib-2.0.dll.a"
if [ -n "$miss" ]; then echo "== INCOMPLET, manque :$miss =="; exit 1; fi
# §0 self-check : PROVE we fetched the pinned glib, not a silent substitute. The whole
# gate's reproducibility rests on this exact version; a mismatch here is a loud failure.
gotv="$(glib_version || true)"
if [ "$gotv" != "$PINNED_GLIB" ]; then
  echo "== ERREUR §0 : glib épinglé $PINNED_GLIB mais dev = '${gotv:-absent}' — fetch non reproductible =="
  exit 1
fi
echo "== GLib runtime ready in $out (dev in $dev), glib $gotv (épinglé) =="
ls "$out"
