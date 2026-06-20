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

### 3.0 Bugs lifter/structureur identifiés sur du vrai code (à corriger)

- **Bug #2 — arête de sortie de boucle effondrée en `break` nu qui court-circuite
  le bloc de résultat** *(trouvé par un autre agent sur MQEL `sub_493440`, un
  désérialiseur)*. Une boucle de `strcmp` inline : le `jne mismatch` saute vers
  un bloc (`sbb eax,eax; or eax,1`) qui **calcule le résultat** avant le point de
  re-convergence `done`. Le structureur replie ce saut en un **`break` nu** qui
  atterrit directement au test post-boucle, **en sautant le bloc de résultat** →
  la valeur loop-carried (`v29`, initialisée à `0` = « match ») reste à son init.
  Conséquence : tous les noms de champ « matchent » la 1ʳᵉ entrée → toutes les
  valeurs écrites au mauvais offset (2/8 champs au lieu de 8/8).
  - **Cause racine** : une arête qui **quitte la boucle vers un bloc autre que le
    point de re-convergence** doit préserver les définitions (φ/copies) de ce bloc.
    En SSA, la valeur lue après la boucle doit recevoir la def de **l'arête de
    sortie « mismatch »**, pas seulement celle du back-edge. Analogue au bug
    tail-call déjà corrigé (`internal_tailcall_args`) : l'**effet d'une arête de
    flot** (ici la def de la valeur ; là l'ajustement d'`esp`) était omis.
  - **Correctif attendu** : émettre le bloc `mismatch` sur le chemin du `break`
    (break vers un label qui exécute `sbb;or`), **ou** ne pas effondrer ce saut en
    `break` (le traiter comme un `goto` vers le bloc de résultat).
  - **Pointeurs** : `src/structure/` (effondrement boucle/break), `src/ssa/`
    (placement des φ sur les arêtes de sortie). Repro : `--mode transpile
    --entry 0x493440 --iat-symbols iat_symbols_full.json`. Doit passer **8/8**.
- **Bug — pointeur-fonction passé en pile cdecl corrompu.** Lua 5.4 (`lua.exe`)
  transpile (939 fn) et démarre, mais un pointeur-fonction passé en **pile cdecl**
  (après `push ebx`) arrive corrompu (`l_alloc` 0x403913 → 0xc42) → dispatch VM
  cassé. Cas limite du modèle de pile machine / suivi d'`esp`. *Même famille que
  Bug #2 : un effet d'arête/pile non préservé.*

### 3.1 Autres axes perfectibles

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

1. **Corriger les bugs d'arête de flot (§3.0)** : Bug #2 (sortie de boucle qui
   court-circuite le bloc de résultat) et le pointeur-fonction pile cdecl. Même
   famille (effet d'arête non préservé en SSA/structuration) → fiabilise le vrai
   code et débloque la classe « interpréteurs ». *Lift, haute valeur.*
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

---

## 7. Briques externes à intégrer (réutiliser plutôt que réécrire)

> Évaluées contre les trous des §3–§4. Statut : 🟢 à intégrer · 🟡 au choix
> (recoupements) · 🟠 s'en inspirer (réutiliser le savoir/les tables) · 🔴
> marginal/non-intégrable · ⚠️ à clarifier.

### 7.1 Cœur — complétude du lift & backend (le plus structurant)

| Brique | Apport | Trou comblé (§) | Statut |
|---|---|---|---|
| **Remill** (Trail of Bits) | Sémantique **complète** x86/amd64/aarch64 → **LLVM IR**, par instruction | **Complétude du lifter** (§3.0/§3) : supprime la soupape `asm`, supprime l'incomplétude. Approche rev.ng « réutiliser une sémantique complète ». **Levier n°1.** | 🟢 |
| **rev.ng** | SBT mature : lift (QEMU/TCG) + backend LLVM → binaire **re-exécutable** | Cœur [1]+[2] entier. Soit base, soit inspiration (récupération de fonctions, dispatcher d'indirects généralisé) | 🟢/🟠 |
| **LLVM** | Codegen multi-arch + opti + WASM | Backend [2] — **déjà intégré** ; approfondir pour **multi-arch natif (ARM)** | 🟢 (en place) |

### 7.2 Couche OS / glue (couverture CRT-Win32 et **GUI**)

| Brique | Apport | Trou comblé | Statut |
|---|---|---|---|
| **Winelib** | Lier le code lifté contre l'implémentation **native** de Win32/CRT/**USER32**/GDI de Wine | **Couverture totale CRT/Win32** (au-delà de mes 170 shims) **+ la couche GUI** = le mur des jeux (§4) | 🟢 |
| **Wine** (exécution/extraction dynamique) | Faire **tourner** le programme (déballage, dump `/proc/mem`, extraction d'assets/ressources en cours d'exécution) | Brique **[0] déballage** + **[A] snapshot** (déjà utilisée), + extraction dynamique | 🟢 (en place) |
| **Box86 / Box64** | Tables de **« wrapped libraries »** (mapping appel lib hôte → natif, conventions d'appel à la frontière) | **Mine pour étoffer la HLE** — même approche que la mienne, en plus complet. Réutiliser le *savoir*, pas le code | 🟠 |

### 7.3 Récupération & vérification (vtables, indirects, équivalence)

> Fort recoupement (sémantique + exécution symbolique). **En choisir un**, pas trois.

| Brique | Apport | Trou comblé | Statut |
|---|---|---|---|
| **Triton** | Sémantique x86/x64/ARM (AST) + **exéc. symbolique** + taint, **C++ embarquable** | **vtables/indirects** (résolution par VSA/symbolique) + **vérif d'équivalence fonction entière** + déobfuscation. *Reco si un seul.* | 🟡 |
| **angr** | CFG recovery, exéc. symbolique, VSA, VEX (Python) | Idem (récupération/indirects/vérif). Intégration par sous-process | 🟡 |
| **Miasm** | IR + symbolique + émulation multi-arch | Idem ; recoupe angr/Triton | 🟡 |
| **Unicorn** | Émulateur CPU | **Déjà intégré** (déballeur) ; élargir : analyse dynamique, exécution de snapshot | 🟢 (en place) |

### 7.4 Lisibilité (couche LLM, strictement post-vérif)

| Brique | Apport | Trou comblé | Statut |
|---|---|---|---|
| **LLM4Decompile** | LLM spécialisé binaire→C | **Couche LLM de lisibilité** (pilier 7 : noms/commentaires) — **uniquement après** la vérif, jamais présenté comme prouvé | 🟠 |

### 7.5 Marginal / non-intégrable / à clarifier

| Brique | Verdict |
|---|---|
| **Rosetta 2** (Apple) | 🔴 **Propriétaire/fermé → non intégrable.** Référence conceptuelle x86→ARM AOT seulement |
| **Uroboros** | 🔴 But différent (**désassemblage réassemblable**, pas C/LLVM/WASM). Techniques de symbolisation marginalement utiles |
| **Kaitai Struct** | 🔴 Niche : parser des **formats de données** exotiques (assets, `.UBX`, saves) — utile côté RE/protocole, pas pour le cœur de conversion |
| **RAMF** | ⚠️ **Non reconnu de façon fiable** comme projet de binary-analysis. À ne pas inventer (principe « jamais de faux comme vrai ») → **fournir un lien / nom complet** pour évaluation |

### 7.6 Si l'on n'en intègre que **trois** (par valeur pour « A → natif B »)

1. **Remill** (ou rev.ng/QEMU) → complétude du lifter.
2. **Winelib + Wine** → couverture Win32/CRT totale **+ GUI**.
3. **Triton** → vtables C++ + vérif d'équivalence.

LLVM et Unicorn (déjà en place) restent les fondations backend/dynamique.
