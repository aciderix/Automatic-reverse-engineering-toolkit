# État des lieux — ARET comme convertisseur « binaire → natif d'un autre système »

> **Document de recentrage (base saine).** Reprend l'historique des commits, la
> doc (`docs/vision/*`), la `ROADMAP.md` et le `HANDOFF.md`, et fait le point
> **honnête** sur l'objectif de conversion : qu'est-ce qui **marche**, qu'est-ce
> qui est **perfectible**, qu'est-ce qui **manque**.
>
> **Objectif.** Prendre un programme **Windows / Linux / (macOS)** et le
> **convertir** en programme **natif d'un autre système** — soit **directement
> exécutable** (ELF Linux…), soit **WASM** — **entièrement fonctionnel comme s'il
> était natif, sans émulation à l'exécution.**

---

## 0. Le projet a deux couches (ne pas confondre)

1. **Le décompilateur vérifié** (`HANDOFF.md`, `ROADMAP.md`) : lifter n'importe
   quel binaire en **C prouvé équivalent**. Métrique nord = *re-exécutabilité
   prouvée*. **Mûr** : recompile 100 %, différentiel **264/264** (-O0→-O3), SMT
   11/11, jeu PE32 C++ **~90 %** des fonctions entièrement modélisées.
2. **Le transpileur (UBT)** (`docs/vision/*`, cette série de commits) : utiliser
   ce lift pour **fabriquer un exécutable natif/WASM** d'un autre OS, avec une
   **couche de compatibilité OS native** (pas Wine au runtime).

La couche 2 **repose** sur la couche 1 : meilleur lift = meilleure conversion.
L'objectif de l'utilisateur (« convertir A en natif B fonctionnel ») = couche 2.

---

## 1. Le pipeline de conversion (vue d'ensemble)

```
 binaire A (PE/ELF/Mach-O)
    │
 [0] DÉBALLAGE (si packé)        src/unpack (Unicorn, --features unpack)
    │   émule le stub → OEP → dump → PE propre + IAT reconstruite
    ▼
 [R] RECONNAISSANCE              loader::crt_symbol / is_startup_glue, src/flirt
    │   symboles ou FLIRT-lite → CRT/glue reconnus ; find_main (strippé)
    ▼
 [1] LIFT → IR SSA typé          src/ir, src/ssa, src/opt   (le décompilateur)
    │
 [2] BACKEND → code cible        src/emit (C) | src/emit/llvm (LLVM) | --target wasm
    │   C natif -m32/-m64 · LLVM chunké · WASM wasm32-wasi
    ▼
 [3] COUCHE OS NATIVE (HLE)      runtime/aret_hle  (170 shims)
    │   CRT msvcrt→libc · Win32 kernel32→POSIX · pile machine partagée · TEB/PEB
    ▼
 ELF Linux natif   /   module WASM   (exécuté SANS émulateur)

 (option) [A+B] SNAPSHOT         --snapshot + tools/snapshot
     état runtime réel (figé sous Wine) injecté comme mémoire initiale
```

---

## 2. CE QUI MARCHE (acquis, testé) — 70 tests verts

### 2.1 Lift / décompilation (la fondation)
- **Recompile 100 %**, différentiel **264/264** (-O0→-O3), SMT 11/11, magic-div 2^32.
- Vrai jeu PE32 C++ (26 Mo, ~44 k fn) : recompile 100 %, **~90 %** entièrement
  modélisées. x86/x64, SSE scalaire+packé, **x87 80-bit**, atomics, jump-tables,
  tail-calls, imports PE+ELF.

### 2.2 Backends → cible
- **C natif** : un PE Win32 → **ELF Linux qui tourne** (M1–M4, FS, fixtures).
- **LLVM IR** (rev.ng-style, `--backend llvm`) : **parité avec C**, flottant
  SSE+x87, largeurs exactes, **chunké → passe à l'échelle** (jeu 44 183 fn → 221
  modules `.ll` → ELF 127 Mo, sans OOM).
- **WASM** (`--target wasm`) : même C recouvré → **`.wasm` qui tourne sous
  wasmtime**. **14 fixtures OK** (string/globals/appels internes **et indirects**/
  CRT printf+tas/TEB/SSE+x87/Win32/**SHA-256**).

### 2.3 Couche OS native (HLE) — 170 shims
- **CRT** (`aret_crt.c`) : msvcrt → **libc hôte** (string/stdlib/stdio/ctype).
- **Win32** (`aret_win32.c`) : kernel32 → **POSIX** (timing, env, tas, atomiques,
  lstr*, system-info, sync, chemins).
- **Pile machine partagée** (param `__esp`), TEB/PEB synthétiques, dispatch
  d'appels indirects (`aret_call`), Memory Layout Mapper (sections à leurs VAs).

### 2.4 Reconnaissance (pour « n'importe quel exe »)
- **Reconnaissance CRT par symbole** : un CRT statiquement lié est **branché sur
  le runtime natif** au lieu d'être lifté. Un vrai programme mingw à **CRT
  complet** tourne (`hello_realcrt` → `argc/argv` réels + `heap=real crt…`).
- **FLIRT-lite** (`src/flirt`, `--mode gensig`) : reconnaît le CRT **sans
  symboles** (binaire **strippé**) par motif d'octets. Strippé → tourne quand même.
- **Découverte de `main`** (`--entry main`) : trouve `main` dans un binaire
  strippé via le motif d'appel du démarrage. → strippé **entièrement automatique**.

### 2.5 Déballage (brique [0], `--features unpack`)
- Moteur **Unicorn** : émule le stub → **OEP** (code auto-modifié) → dump.
- **Modèle Win32 in-emulator** : résout l'IAT du packer (LoadLibrary/GetProcAddress/
  VirtualAlloc…). **Reconstruction d'imports** (Scylla-style) → répertoire standard.
- **Rebuild PE propre** (en-têtes restaurés, raw==RVA).
- **Boucle fermée validée sur un vrai packer (UPX)** : packé → déballé → transpilé →
  **ELF natif qui imprime** (`tests/unpack_e2e.rs`).

### 2.6 Composition avec un déballeur externe + snapshot (A+B)
- `--iat-symbols` : ingère une IAT reconstruite (JSON) → noms d'imports.
- `--snapshot` + `tools/snapshot/dump_snapshot.py` : **figer un process (Wine) →
  dumper `/proc/mem` → injecter comme mémoire initiale** d'une fonction liftée.
- **Validé sur le vrai jeu** : lancé sous Wine (déballage), **snapshot 29 Mo**
  capturé (OEP vérifié), une vraie fonction du jeu transpilée en **natif + WASM**.

### 2.7 Vrais programmes passés de bout en bout
- **SHA-256** (Brad Conte, OSS) : digest **exact** en natif **et** WASM, **même
  packé UPX**.
- **CRT complet** (mingw normal) : tourne via reconnaissance.

---

## 3. CE QUI EST PERFECTIBLE (marche, mais à muscler)

- **Complétude du lifter sur le code lourd en pointeurs de fonction.** Lua 5.4
  (`lua.exe`) transpile (939 fn) et démarre mais **casse** : un pointeur-fonction
  passé en **pile cdecl** (après `push ebx`) arrive corrompu → dispatch VM cassé.
  Cas limite du modèle de pile machine / suivi d'`esp` — **bug réel à corriger**.
- **Backend LLVM = i386 seulement.** Le datalayout/triple sont 32-bit. Un ELF
  **ARM64** *exécutable* exige de porter tout le runtime en 64-bit (chantier).
- **WASM** : pas d'I/O fichier sans `--dir` (bac à sable WASI) ; la **soupape asm**
  (`__asm__`) ne compile pas en wasm ; le layout WASM embarque le blob en **tableau
  C** (lourd pour de très gros binaires).
- **Transpile par fonction non élagué.** `--function` isole une fonction mais le
  builder n'émet pas seulement ses **callees** → pour piloter *un sous-système*
  proprement il manque un **pruning par accessibilité**.
- **Base FLIRT = CRT mingw seulement.** Un autre CRT (MSVC **ucrtbase**, autre
  version) demande ses propres signatures (`--mode gensig` les génère).
- **Recompile 100 % ≠ équivalence prouvée à 100 %.** La vérif formelle (Z3) ne
  prouve que des **règles d'opt isolées**, pas des fonctions entières.

---

## 4. CE QUI MANQUE (non commencé / mur connu)

- **Couche GUI / graphique.** USER32/GDI, et surtout **Direct3D**. Un programme
  **fenêtré / jeu** ne peut pas devenir « natif » sans cette couche = **DXVK/vkd3d
  + Winelib** (réutilisation, mais lourde). **C'est le mur pour les jeux** (ex.
  MightyQuest : CEF + D3D). Le code *logique* se convertit ; le **rendu** non.
- **vtables / appels indirects C++** non résolus (HANDOFF §6.3) — pourtant le
  binaire de test EST un jeu C++. Levier majeur pour le vrai code.
- **Inférence de types** (HANDOFF §5) : variables typées, `obj->field_8`, structs.
  Améliore la lisibilité et la justesse des conversions.
- **Source macOS (Mach-O)** : le **loader** lit Mach-O, mais **aucune couche HLE
  macOS** (frameworks). Conversion *depuis*/*vers* macOS = à faire.
- **Multi-arch natif** (ARM ELF) : voir §3 (portage runtime 64-bit).
- **Glue Winelib / vrai msvcrt** comme alternative aux shims (couverture totale du
  CRT/Win32) — voie documentée, `winegcc` indisponible dans cet environnement.
- **Unification des deux pipelines** (texte lisible vs IR vérifié) — roadmap §3.5.
- **Boucle de raffinement SMT** sur fonctions entières (HANDOFF §6, pilier 6).

---

## 5. Verdict honnête par classe de programme

| Classe de programme | Convertible natif/WASM **fonctionnel** ? |
|---|---|
| **Console / calcul** (CRT + kernel32 hors-GUI) | ✅ **oui, prouvé** (SHA-256, full-CRT), natif **et** WASM |
| Même chose **packé** (UPX-classe) | ✅ oui (déballage → transpile) |
| **Strippé** (sans symboles) | ✅ oui (FLIRT + découverte de main) |
| **Interpréteur / très pointeur-fonction** | ⚠️ partiel (bug de lift à corriger, cf. §3) |
| **GUI / fenêtré** (USER32/GDI) | ❌ pas encore (couche USER32) |
| **Jeu 3D** (Direct3D) | ❌ non (DXVK requis — le rendu n'est pas du transpile) |
| **macOS (source ou cible)** | ❌ loader oui, HLE non |

**Résumé en une phrase.** ARET **convertit aujourd'hui, sans émulation, un
programme Windows console/calcul (même packé, même strippé) en ELF Linux natif
ET en WASM, entièrement fonctionnel** — prouvé sur du vrai code OSS. Au-delà
(interpréteurs lourds, GUI, 3D, macOS), ce sont les chantiers §3–§4.

---

## 6. Prochaines étapes concrètes (par valeur pour l'objectif de conversion)

1. **Corriger le bug pointeur-fonction en pile cdecl** (§3) → débloque la classe
   « interpréteurs » (Lua) et fiabilise le vrai code. *Lift, haute valeur.*
2. **vtables / appels indirects C++** (HANDOFF §6.3) → indispensable pour les
   vrais programmes C++. *Lift.*
3. **Pruning par accessibilité** dans le transpile (`--function` + callees only)
   → convertir un **sous-système** autonome en commande native/WASM propre.
4. **Signatures FLIRT MSVC** (`ucrtbase`/`msvcr*`) → « n'importe quel exe MSVC
   strippé », pas seulement mingw.
5. **Inférence de types** (HANDOFF §5) → lisibilité + justesse.
6. **Couche graphique** (objectif long terme, jeux) : évaluer **DXVK + Winelib**
   branchés en sortie. C'est de la **réutilisation**, mais c'est le gros morceau.
7. **Multi-arch / runtime 64-bit** → ELF ARM, et débloque un LLVM multi-cible réel.

> Principe sacré (hérité du décompilateur) : **jamais de sortie incorrecte
> présentée comme correcte.** Tout ce qui n'est pas sûr reste `Asm`/`__asm__`.
