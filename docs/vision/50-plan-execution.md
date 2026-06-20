# Plan d'exécution & suivi — compléter l'objectif de conversion universelle

> **Document vivant.** Sert de **repère, contexte et suivi**, y compris après
> compression du contexte. Mis à jour à chaque étape (journal §4). Lire d'abord
> [`40-etat-des-lieux-conversion.md`](40-etat-des-lieux-conversion.md) (acquis /
> perfectible / manque) et `HANDOFF.md` (architecture, 2 pipelines, principe).
>
> **Objectif final.** Prendre un programme **Windows/Linux/(macOS)** et le
> convertir en programme **natif d'un autre système** (ELF direct **ou** WASM),
> **entièrement fonctionnel comme natif, sans émulation** — **universel**.
>
> **Principe sacré (non négociable).** Jamais de sortie incorrecte présentée
> comme correcte. Tout ce qui n'est pas sûr reste `Asm`/`__asm__`. Toute
> modification du lifter/structureur **doit** passer la régression (§3) avant
> commit. Vérif + commit **réguliers**.

---

## 1. Méthode de travail (à respecter)

1. **Une tâche à la fois**, méthodique. Pas de big-bang.
2. Pour chaque tâche : (a) comprendre/repro, (b) **fixture minimale testable** si
   possible (mieux qu'un gros binaire), (c) implémenter, (d) **vérifier** (tests +
   régression + différentiel si lifter), (e) **commit** descriptif, (f) **mettre à
   jour le journal §4**.
3. **Jamais casser la régression** : `cargo test --release` (70 tests) doit rester
   vert ; pour tout changement de lift/structure, lancer aussi `bench/difftest.sh`
   (différentiel -O0→-O3) et viser **aucune régression**.
4. Commits petits et fréquents. Pousser souvent.

### Commandes clés
```
cargo build --release
cargo test --release                 # 70 tests (défaut)
cargo test --release --features unpack
bash bench/regression.sh             # porte unifiée (build+tests+niveaux)
bash bench/difftest.sh               # différentiel lift (-O0→-O3)
# transpile : aret --mode transpile [--backend llvm|--target wasm] [--entry main] [--snapshot|--iat-symbols] --run --out-dir OUT prog.exe
# repro Bug #2 : /tmp/mq_unpack/MightyQuest_unpacked.exe (sub_493440) + iat_symbols_full.json
```

---

## 2. Plan par phases (ordre = valeur × sûreté)

> Chaque phase a un **livrable** et un **critère d'acceptation** vérifiable.
> Une phase n'est « faite » que **vérifiée + commitée + journalisée**.

### Phase 1 — Correction des bugs d'arête de flot (FIABILITÉ) ⏳ EN COURS
Le lift/structureur perd parfois l'**effet d'une arête** (def de valeur, ajustement
de pile). Même famille que le tail-call déjà corrigé. Affecte **tout** vrai code.
- **1a. Bug #2 ✅ FAIT** — arête de sortie de boucle effondrée en `break` nu qui
  court-circuite le bloc de résultat (cf. §3.0 de l'état des lieux ; `strcmp`
  inline, `sub_493440`). *Fix* : `emit::structured::emit_loop` route la sortie de
  l'en-tête par `emit_seq` (break seulement si cible = follow). *Vérifié* :
  fixture `loop_exit_value` (`eq=0 lt=-1 gt=1`), 71 tests, diff 268/268.
- **1b. Bug pointeur-fonction pile cdecl** (Lua : `l_alloc` 0x403913 → 0xc42).
  *Livrable* : fixture minimale (passer un pointeur-fonction par la pile cdecl puis
  l'appeler) + fix. *Critère* : la fixture appelle la bonne fonction ; Lua avance.

### Phase 2 — Pruning par accessibilité (CONVERSION CIBLÉE) ⬜
Transpiler **une fonction + uniquement ses callees** (fermeture transitive), pas
tout le binaire. *Livrable* : `--prune`/auto avec `--function`. *Critère* :
transpiler `sub_X` d'un gros binaire ne sort que la fermeture, compile, tourne.

### Phase 3 — Reconnaissance CRT MSVC (UNIVERSALITÉ « exe quelconque ») ⬜
Signatures FLIRT pour `ucrtbase`/`msvcr*` (MSVC), pas seulement mingw. *Livrable* :
DB MSVC via `--mode gensig` + reconnaissance. *Critère* : un exe MSVC strippé
reconnaît son CRT et tourne.

### Phase 4 — vtables / appels indirects C++ (VRAI CODE C++) ⬜
Résoudre `call [vtable+k]` quand la vtable est en `.rodata` ; nommer la méthode.
*Livrable* : recovery vtables (analysis/ir) + fixture C++ virtuelle. *Critère* :
un appel virtuel se résout et s'exécute correctement (différentiel).

### Phase 5 — Complétude du lifter (réduire la soupape `asm`) ⬜
Couvrir les instructions restantes sûres (cf. HANDOFF backlog). Évaluer
l'intégration **Remill** (sémantique complète → LLVM) comme accélérateur. *Critère* :
baisse mesurée de l'incomplétude sur le jeu, différentiel sans régression.

### Phase 6 — Inférence de types (LISIBILITÉ + JUSTESSE) ⬜
Largeur/signe/ptr puis agrégats `obj->field_8` (HANDOFF §5). *Critère* : types
affichés, **jamais** au prix de la sémantique (casts explicites conservés).

### Phase 7 — Couche OS élargie : Winelib / Win32-USER32 (vers GUI) ⬜
Évaluer/brancher Winelib pour couverture Win32/CRT totale + amorce USER32.
*Critère* : un programme Win32 utilisant une API hors-shim tourne via la vraie lib.

### Phase 8 — Multi-arch / runtime 64-bit (ARM, LLVM multi-cible réel) ⬜
Porter le runtime/modèle en 64-bit pour émettre un ELF ARM exécutable. *Gros.*

### Phase 9 — macOS (Mach-O) HLE ⬜ · Phase 10 — Graphique (DXVK/vkd3d) ⬜
Chantiers longs (le « mur » des jeux). Documentés, pas prioritaires.

---

## 3. Définition de « fait » (Definition of Done)
- Code + **fixture/test** automatisé.
- `cargo test --release` vert (+ `--features unpack` si touché) et, pour le
  lift/structure, **différentiel sans régression**.
- Commit descriptif poussé. **Journal §4 mis à jour.**

---

## 4. Journal d'avancement (append-only ; le plus récent en bas)

- **2026-06-20 — Plan créé.** État de départ : 70 tests verts ; lift mûr
  (264/264 diff, jeu ~90 %) ; backends C/LLVM(chunké)/WASM ; HLE 170 shims ;
  reconnaissance CRT+FLIRT(mingw)+main-discovery ; déballeur Unicorn (boucle UPX
  fermée) ; snapshot A+B (validé sur le vrai jeu). Bugs ouverts : #2 (loop-exit),
  cdecl-ptr (Lua). **Focus courant : Phase 1a (Bug #2).**
- **2026-06-20 — Phase 1a FAITE (Bug #2 corrigé).** Cause racine isolée dans
  `src/emit/structured.rs::emit_loop` : la sortie conditionnelle de l'en-tête de
  boucle émettait un `break;` nu, qui saute toujours vers l'**unique** `follow_idx`
  calculé par `loop_info`. Quand l'arête de sortie vise un bloc **différent** du
  follow (cas `strcmp` inline : la branche *mismatch* calcule `(ca<cb)?-1:1` via
  l'idiome sbb/or sur le bloc de sortie), ce bloc était court-circuité → résultat
  porté par la boucle perdu, code mort. **Fix** : router l'arête de sortie de
  l'en-tête par `emit_seq(cible, lctx, …)` au lieu d'un `break;` codé en dur —
  `emit_seq` n'émet `break` que si la cible **est** le follow, sinon émet le vrai
  bloc cible (inline / `goto`). Symétrique pour les deux orientations taken/fall.
  **Vérif** : fixture permanente `tests/m1/fixtures/loop_exit_value.{c,exe}` +
  test `loop_exit_carrying_a_value` (`LOOPEXIT: eq=0 lt=-1 gt=1`, conforme à la
  réf. Wine ; avant : `eq=0 lt=0 gt=0`). **71 tests verts** (m1_transpile 17→18) ;
  différentiel **268/268** (-O0→-O3) sans régression. **Prochain : Phase 1b**
  (pointeur-fonction sur pile cdecl, Lua `l_alloc`).
