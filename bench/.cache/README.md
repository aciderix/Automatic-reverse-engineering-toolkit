# Corpus binaires (épinglés)

Ce dossier est **ignoré par git** (`bench/.cache/`) car les benches y téléchargent
des binaires volumineux/versionnés. **Exception** : les fichiers listés ci-dessous
sont **committés** (négations dans `.gitignore` racine) pour qu'un conteneur
éphémère ne les perde jamais — ce sont des cibles de `funcdiff`/régression, tierces
mais publiques et versionnées. Garder cette liste synchronisée avec `.gitignore`.

Tout autre fichier ici (zips téléchargés, autres versions de sqlite, corpus/…)
reste ignoré : n'y committer que des binaires **fixes, publics et référencés**.

| Fichier | Taille | Provenance | sha256 |
|---|---|---|---|
| `busybox-w32-FRP-5579-g5749feb35.exe` | 627 726 | https://frippery.org/files/busybox/busybox-w32-FRP-5579-g5749feb35.exe | `497607849a3e581615e46292d9063313d9a27a54380aad60ba2c5328838e3bb6` |
| `sqlite3-3400100.exe` | 1 123 840 | https://www.sqlite.org/2022/sqlite-tools-win32-x86-3400100.zip (→ `sqlite3.exe`) | `a27ee3e1e37dbd864668d93bfa0797f371ff3a90fb0cf4d7be827bff86e2b748` |
| `winetest.exe` | 91 109 419 | https://gitlab.winehq.org/wine/wine/-/jobs/artifacts/master/raw/winetest.exe?job=build-daily-winetest | `a1a35e6f16e52db1eac5d26b518d7d507d967cbb59e106daf783bc2f6249eaae` |
| `libintl-8.dll` | 202 396 | https://repo.msys2.org/mingw/mingw32/mingw-w64-i686-gettext-0.22.4-3-any.pkg.tar.zst (→ `mingw32/bin/libintl-8.dll`) | `273402f30d21b9ffec2bf96228f291b973563718b1cffc21bf064edbc0e43fd6` |
| `libiconv-2.dll` | 1 146 391 | https://repo.msys2.org/mingw/mingw32/mingw-w64-i686-libiconv-1.19-1-any.pkg.tar.zst (→ `mingw32/bin/libiconv-2.dll`) | `c15983490044742bb096890098d73f31f9cbb979ba52d914f97966b98f959a30` |

## À quoi ils servent
- **busybox-w32** (BusyBox Windows, mingw 32-bit) : cible de `funcdiff` (test
  `funcdiff_busybox`, ignoré par défaut) et de `bench/busybox_sweep.sh`. Fournit les
  applets **grep/sed/cksum** — les fonctions **à appels** (moteur regex, `filltable`)
  que la closure funcdiff vise. Version exacte figée (le sweep la recherche par nom).
- **sqlite3** (MSVC 32-bit strippé, 3.40.1) : cible réelle MSVC de `bench/sqlite_sweep.sh`.
- **winetest** (WineHQ, artefact daily `master`) : gros PE Win32 (143 imports),
  cible de récupération/transpile et amorce du différentiel Wine (axe 2). ~87 Mo.
- **libintl-8.dll** / **libiconv-2.dll** (mingw-w64 i686, gettext 0.22.4 / libiconv 1.19) :
  DLL redistribuables réelles requises par la fixture winediff `win32_gettext` (marqueur
  `.winedll` : l'oracle Wine charge la vraie DLL, ARET route vers ses shims HLE). Sans
  elles la fixture est **SKIP** (le harness reste tolérant conteneur-éphémère) ; pinnées
  ici pour que `win32_gettext` compte réellement (262/264 au lieu de 261/264).

## Re-télécharger (si besoin)
Les benches re-téléchargent automatiquement dans ce dossier si un fichier manque
(voir `busybox_sweep.sh`, `sqlite_sweep.sh`). Le `winetest.exe` daily bouge : la
version committée est un instantané reproductible (sha256 ci-dessus).
