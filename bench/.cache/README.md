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

## À quoi ils servent
- **busybox-w32** (BusyBox Windows, mingw 32-bit) : cible de `funcdiff` (test
  `funcdiff_busybox`, ignoré par défaut) et de `bench/busybox_sweep.sh`. Fournit les
  applets **grep/sed/cksum** — les fonctions **à appels** (moteur regex, `filltable`)
  que la closure funcdiff vise. Version exacte figée (le sweep la recherche par nom).
- **sqlite3** (MSVC 32-bit strippé, 3.40.1) : cible réelle MSVC de `bench/sqlite_sweep.sh`.
- **winetest** (WineHQ, artefact daily `master`) : gros PE Win32 (143 imports),
  cible de récupération/transpile et amorce du différentiel Wine (axe 2). ~87 Mo.

## Re-télécharger (si besoin)
Les benches re-téléchargent automatiquement dans ce dossier si un fichier manque
(voir `busybox_sweep.sh`, `sqlite_sweep.sh`). Le `winetest.exe` daily bouge : la
version committée est un instantané reproductible (sha256 ci-dessus).
