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
- **1b. Bug pointeur-fonction pile cdecl ✅ NON REPRODUIT (résolu autrement)**.
  Le passage d'un pointeur-fonction par la pile cdecl puis son appel **fonctionne**
  (repro minimale `/tmp/fnptr_stack.c` → `r=142 s=130` ; et Lua passe `lua_newstate`,
  donc l'allocateur enregistré est bien dispatché). Le vrai blocage révélé par Lua
  était ailleurs (thunks d'import + setjmp/longjmp), corrigé ci-dessous.
- **1c. setjmp/longjmp + thunks d'import ✅ FAIT** (révélé par Lua 5.4.7).
  (a) **Thunks d'import** : `call <thunk>` où `thunk = jmp *[IAT]` n'était pas
  reconnu comme appel d'import → liaison perdue. `Program::import_thunk` les
  résout ; `call thunk` se lie au shim **au vrai site d'appel**. Corrige aussi
  `IsProcessorFeaturePresent` (atteint enfin son shim). (b) **setjmp/longjmp** :
  macros expansées au site d'appel lifté (le setjmp/longjmp hôte s'exécute dans la
  frame native de la fonction liftée ; la pile native reflète la pile logique 1:1,
  donc longjmp déroule correctement). Support `aret_jmpbuf_for`/`aret_longjmp_do`.
  (c) `time`/`clock` shims ; `aret_fflush` rendu conscient des flux `_iob`.
  *Vérifié* : fixture `setjmp_longjmp` (`caught=42`, longjmp déroule 5 frames),
  72 tests, diff 268/268. *Reste Lua* : tourne jusqu'à l'interpréteur mais une
  erreur VM plus profonde demeure (« error message not a string ») — hors 1c.

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

### Phase 5 — Complétude du lifter (réduire la soupape `asm`) ⏳ EN COURS
- **5a. Retour flottant x87 à travers un appel ✅ FAIT** (révélé par Lua). L'analyse
  de profondeur x87 ne comptait pas le `st(0)` qu'un appel à une fonction renvoyant
  un flottant empile → sous-débordement de pile → tout le bloc flottant retombait en
  `asm` opaque (bug réel : la vérif de version de Lua prenait toujours sa branche
  d'erreur). Fix : ensemble `FP_RETURNING` (fonctions au `ret` à profondeur 1),
  un appel vers l'une d'elles compte `+1` ; **et** la valeur `st(0)` est acheminée
  par un canal de retour fp partagé (`__x87_retstore`/`__x87_retload`,
  `__aret_x87_ret`). *Vérifié* : fixture `fp_return_call` (`match`), 73 tests, diff
  268/268. *Reste Lua* : `luaV_finishget` (indexation de table) dans `openlibs`.
Couvrir les instructions restantes sûres (cf. HANDOFF backlog). Évaluer
l'intégration **Remill** (sémantique complète → LLVM) comme accélérateur. *Critère* :
baisse mesurée de l'incomplétude sur le jeu, différentiel sans régression.

### Phase 5bis — Frontière host/traduit explicite dans l'IR ✅ FAIT
Question soulevée (ChatGPT) : marquer proprement la frontière entre fonctions
**liftables sûres**, **opaques host-backed**, **partiellement simulées**. Avant :
décisions éparpillées (par site d'appel dans `name_calls_in_expr` ; `import_name`/
`import_thunk`/`crt_symbol`/`is_startup_glue` dispersés ; arg `esp` dupliqué 3×).
Fix : **`resolve_call` unique** (`CallBinding::{Shim{name,thread_esp}, Internal}`)
— source de vérité de la liaison d'appel. + requêtes de classification
`host_shim_name` / `has_opaque_asm`, et un **rapport** par fonction (`classes: N
lifted, M partial(asm), K host-backed`). *Vérifié* : comportement préservé (78
tests, diff 268/268, Lua OK) ; Lua = 896 traduites / 1 partielle / 90 host-backed.
*Reste* (si on va plus loin) : attacher la classe à l'`IrFunction` et élaguer les
corps host-backed (sauf appels indirects).

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
- **2026-06-21 — Phase 1b/1c FAITE (thunks d'import + setjmp/longjmp).** Cible
  réelle : Lua 5.4.7 (`lua.exe` mingw, 987 fns liftées depuis `main`). Diagnostic :
  1b (pointeur-fonction cdecl) **ne se reproduit pas** ; le crash venait des
  **thunks d'import** (`call thunk; thunk: jmp *[IAT]`) jamais liés à l'import, et
  de l'absence de **setjmp/longjmp**. Fixes : `loader::import_thunk` (résout le
  thunk → shim au vrai site) ; macros setjmp/longjmp expansées au site lifté
  + `aret_jmpbuf_for`/`aret_longjmp_do` (aret_hle.c, gardé `#ifndef __wasm__`) ;
  shims `time`/`clock` ; `aret_fflush` conscient de `_iob`. Effet bonus :
  `IsProcessorFeaturePresent` atteint son shim (win32sys `feat` 0→1, attendu
  corrigé). **Vérif** : fixture `setjmp_longjmp.{c,exe}` + test
  `setjmp_longjmp_nonlocal_exit` (`caught=42`) ; **72 tests verts** ; diff
  **268/268**. Lua tourne désormais startup→openlibs→interpréteur (setjmp/longjmp
  OK) mais bute sur une **erreur VM plus profonde** (« error message not a
  string ») — chantier séparé (complétude lifter, Phase 5).
- **2026-06-21 — Phase 5a FAITE (retour flottant x87 à travers un appel).**
  Diagnostic Lua : « error message not a string » venait de `luaL_checkversion_`
  qui levait toujours « version mismatch ». Cause : `call lua_version` (renvoie
  `double` en `st(0)`) non compté par l'analyse de profondeur x87 → underflow →
  tout le bloc flottant en `asm` no-op → conditions `jp`/`je` undef → branche
  d'erreur. Fix en 2 temps (sinon sortie *fausse présentée comme correcte*) :
  (1) profondeur — `compute_fp_returning` (point fixe : fonctions au `ret` à
  profondeur 1), un `call` vers l'une compte `+1` ; (2) valeur — canal de retour
  fp partagé : `__x87_retstore` au `ret` d'une fonction fp, `__x87_retload` après
  l'appel, global `__aret_x87_ret` (aret_hle.c). Set `FP_RETURNING` installé par
  le pilote transpile uniquement (verify/decompile inchangés → diff stable).
  **Vérif** : fixture `fp_return_call.{c,exe}` + test `fp_value_returned_across_a_call`
  (`match`) ; **73 tests verts** ; diff **268/268**. Lua : vérif de version OK,
  avance jusqu'à `luaV_finishget` (indexation table) dans `openlibs` — bug suivant.
- **2026-06-21 — Phase 5b FAITE (comparaison signée vs immédiat négatif).**
  Diagnostic Lua : `luaV_finishget` levait « attempt to index » car `index2value`
  renvoyait `&g->nilvalue` (chemin index positif hors-borne) pour un index
  *négatif* `-1`. Cause : `cmp r32, imm32 ; jge` où l'immédiat négatif `-1000999`
  (0xfff0b9d9) était zéro-étendu en `+4293913049` au lieu d'être signé-étendu →
  `-1 >= +4.29e9` faux → mauvaise branche. Les immédiats sont typés `int(64)`
  (lift) ; à l'émission d'une comparaison signée, l'opérande masqué (32 bits)
  était signé-étendu mais pas la constante. Fix (`emit::binary_c`/`signed_cast_w`)
  : largeur commune déduite du masque du frère, la constante est signé-étendue à
  cette largeur. **Général** (toute comparaison/division signée vs immédiat
  négatif). *Vérifié* : fixture `signed_cmp_negimm` (`1 0 1`) ; **74 tests** ;
  diff **268/268**. Lua : passe `openlibs` ; crash suivant plus profond (exécution).
- **2026-06-21 — Phase 5c FAITE (égalité vs immédiat négatif).** Diagnostic Lua :
  crash dans `lua_pushcclosure` (chargement lib `package`) — la boucle de copie
  d'upvalues ne terminait pas. Cause : `cmp r32, -1 ; jne` (compteur 32 bits) où
  l'immédiat `-1` était signé-étendu en `0xffffffffffffffff` (64 bits) mais
  comparé via `Eq/Ne` (`plain`, non masqué) à un opérande masqué 32 bits
  `0xffffffff` → jamais égal → boucle infinie → pointeur hors-borne. Fix
  (`emit::binary_c` `eq`/`mask_w`) : `Eq`/`Ne` comparent à la largeur commune
  (déduite du masque), la constante est tronquée à cette largeur. *Vérifié* :
  fixture `eq_cmp_negimm` (`10 0`) ; **75 tests** ; diff **268/268**. Lua : passe
  `pushcclosure` ; avance jusqu'au **GC** (`reallymarkobject`/`propagatemark`) —
  bug suivant.
- **2026-06-21 — Phase 5d FAITE (table de saut : cibles dupliquées).** Hypothèse
  validée (méthodo ChatGPT) : **PAS l'ABI** (regparm `eax/edx` lu correctement —
  confirmé en lisant `o` via le bon param-registre), mais **construction du graphe
  d'objets via un mauvais dispatch de switch**. Cause exacte : `resolve_jump_table`
  **dédupliquait** les cibles (`seen.insert`). Or l'émetteur structuré fait
  `case k -> successors[k]` ; en collapsant les doublons (plusieurs `case`
  partageant un corps), tous les cas après le 1er doublon glissent sur le mauvais
  bloc. Dans `reallymarkobject`, le userdata (index 3) était routé vers le cas
  **upvalue** → lecture de `userdata[+8]` (=len=8) comme `uv->v` → `[0x8+8]` →
  crash. Fix : garder **toutes** les entrées dans l'ordre (doublons préservés) ;
  exiger ≥2 cibles *distinctes* pour valider une table. **Général** (tout switch à
  cibles partagées : machines à états, dispatch). *Vérifié* : fixture
  `switch_dup_targets` (`1000 2001 2002 3003 2004 5005`) ; **76 tests** ; diff
  268/268. Lua : **plus de crash GC** (exit 0) ; reste un « indirect call to
  unrecovered address » (pointeur de fonction non récupéré) — bug suivant.
- **2026-06-21 — Phase 5e FAITE (résolution computed-goto / table absolue).**
  L'« indirect call to unrecovered 0x4264dc » pointait *à l'intérieur* de
  `luaV_execute` : la boucle d'interprétation Lua utilise un computed-goto
  (`mov reg,[disptab+op*4] ; jmp reg`, `LUA_USE_JUMPTABLE` activé par mingw). Ni
  `resolve_jump_table` (`jmp [mem]`) ni le résolveur PIE ne matchaient ce
  *load-then-jump* à table absolue → repli en appel indirect non résolu. Ajout :
  `resolve_abs_jump_table` (analyse) + `abs_switch_index` (récup de l'index) pour
  l'idiome `mov reg,[table+idx*ptr] ; jmp reg`. **Général** (interpréteurs,
  switches denses, GCC `&&label`). *Vérifié* : fixture `computed_goto`
  (`6 15 105 1005`) ; **77 tests** ; diff 268/268. Lua : dispatch résolu, mais
  `luaV_execute` **boucle** (timeout) — opcode mal lifté en aval, bug suivant.
  Diagnostic amélioré : `aret_call` imprime désormais la VA non résolue.
- **2026-06-21 — Phase 5f FAITE → 🎉 LUA TOURNE NATIVEMENT.** Deux bugs de la
  boucle de dispatch : (1) **switch en tête de boucle** — `emit_loop` ne gérait pas
  un terminateur `Switch` (tombait dans `_ => {}`) → corps de boucle vide →
  `while(1){}` infini. Toute boucle d'interpréteur (en-tête finissant par le
  switch) était cassée. Fix : `emit_loop` émet le switch + les blocs cibles dans la
  boucle. (2) **switch à clé-adresse** — le computed-goto `mov reg,[tab+idx*4];
  jmp reg` **écrase** le registre d'index (et le `mov` est souvent dans un bloc
  prédécesseur), donc l'index est irrécupérable au `jmp`. Solution : switcher sur
  le **registre de saut** (= l'adresse chargée) avec des cas **clés par VA cible**
  (= l'adresse du bloc handler, = l'entrée de table chargée). `Stmt::Switch.cases`
  porte désormais la vraie clé (index OU VA) ; l'émetteur l'utilise. + shim
  `localeconv`. *Vérifié* : fixture `dispatch_loop` (`INTERP: 11`) ; **78 tests** ;
  diff 268/268. **Lua 5.4.7 (PE Windows 612 Ko, 987 fns) → ELF Linux natif** :
  `print(6*7)`=42, `fib(20)`=6765, boucles (5050), tables, `ipairs` (385),
  concaténation, méthodes string, récursion — **tout correct**. Reste :
  **arithmétique flottante** du VM fausse (`2^10==1024`→false, `7/2`→0). Diagnostic :
  `x87_states` **abandonne** pour `luaV_execute` (122 ops x87 → `asm` no-op, 0
  modélisée) → toute l'arith flottante du VM est morte. Cause : `frndint`
  (arrondi-entier) absent de `x87_delta` → x87 désactivé pour toute la (grosse)
  fonction. *Prochain* : ajouter `frndint` (+ ops manquantes) à `x87_delta`/
  `lift_x87` (honorer le mode d'arrondi), revérifier que `luaV_execute` se
  modélise. Tâche x87-complétude ciblée. Imports manquants annexes : `_onexit`.
- **2026-06-21 — Phase 5g (partiel) : `frndint` + `localeconv` modélisés.** Ajout
  `frndint` à `x87_delta`/`lift_x87` (helpers `__x87_rint`/`__x87_trunc`, honore le
  mode d'arrondi via `truncate_active`) + shim `localeconv`. **Mais `luaV_execute`
  abandonne encore** : cause exacte trouvée — **join x87 ambigu** au bloc
  `0x42427b` (profondeur de pile x87 = 1 sur un chemin, 0 sur l'autre). Sur une
  fonction de 4000+ instructions, un seul join divergent désactive *toute*
  l'arithmétique flottante. **Prochain (profond)** : réconcilier les profondeurs
  x87 aux joins (au lieu d'abandonner toute la fonction) — p.ex. autoriser une
  divergence quand un chemin a un `st(0)` non consommé, ou analyser par région.
  78 tests verts ; diff 268/268 (aucune régression).
- **2026-06-21 — Phase 5g SUITE : arithmétique flottante du VM débloquée.** Cause
  réelle des joins ambigus = **appels fp non comptés** (un `call f` où `f` renvoie
  un `double` en `st(0)` doit compter `+1`, sinon la pile x87 désync). Trois fixes :
  (1) **libm reconnu fp par nom** (`pow`/`sqrt`/`exp`/`floor`/… — leur corps libm
  est trop complexe à analyser mais l'ABI garantit le retour fp ; seed du point
  fixe `compute_fp_returning`). (2) **`fprem`** (reste partiel = `fmod` inliné)
  modélisé : `__x87_fmod` + C2=0 → la boucle `do{fprem}while(C2)` sort d'un coup.
  (3) shim `_onexit`. Effet : `luaV_execute` **ne bute plus** ; `7/2`→3.5,
  `1/3`→0.333…, `math.sqrt(2)`→1.414, `math.sin(0)`→0, `math.pi`→3.14159 — **OK**.
  78 tests ; diff 268/268. *Reste* : littéraux flottants (`1.5*2.5`) **crashent**
  (parsing `strtod`/lexer) et `2^10`→0.0 (chemin `OP_POW`) — bugs suivants isolés.
- **2026-06-21 — Phase 5h FAITE → 🎉🎉 LUA ENTIÈREMENT FONCTIONNEL (flottants
  inclus).** Les deux derniers bugs étaient des **fonctions libm non calculées** :
  leur corps x87 dense ne se modélise pas (lifté en `asm` no-op → renvoie 0). Les
  marquer fp-returning corrigeait la profondeur mais pas le **calcul**. Solution :
  **binder les libm au host** (comme printf/malloc via `crt_symbol`/`CRT_FUNCS`) :
  (1) transcendantes `pow`/`exp`/`log`/`sin`/`cos`/`tan`/`fmod`/`hypot`/… →
  shims host (lisent les `double` cdecl, renvoient par le canal fp `__aret_x87_ret`).
  (2) `strtod`/`atof` (bignum de David Gay, crashait en `__lshift_D2A`) → host.
  (3) shim `_errno`. **Lua 5.4.7 (PE Windows → ELF natif) : TOUT marche** —
  `2^10`=1024, `1/3`=0.333, `1.5*2.5`=3.75, `0.1+0.2`=0.3, `string.format("%.3f")`,
  `table.sort`, `math.sin/cos/exp/sqrt/pi`, récursion, tables, chaînes. **78 tests ;
  diff 268/268.** Principe clé : **brancher le vrai runtime** plutôt que lifter du
  code libm/bignum complexe — exactement la vision UBT (réutiliser les briques).
- **2026-06-21 — Phase 5d EN COURS (corruption GC, à investiguer).** Lua : après
  `pushcclosure`, crash dans le **GC** mark : `reallymarkobject` ←
  `propagatemark` ← `propagateall` ← `atomic` ← `entergen` (entrée GC
  générationnel pendant openlibs). Instruction fautive : `movzbl (obj+8)` (lecture
  du tag `tt_` d'un TValue) avec `obj = 0x8` — **champ de référence corrompu** dans
  le graphe d'objets. Contrairement aux bugs 5a–5c (mislifts d'émission nets,
  largeur/signe), ceci ressemble à une **corruption en amont** (construction d'objet
  ou écriture de champ mal liftée), donc forensics plus profonde (suivre l'origine
  du pointeur `0x8`). *Bilan session* : 6 fixes généraux commités (Bug #2, thunks +
  setjmp/longjmp, retour fp x87, cmp signé/égalité vs immédiat négatif) ; Lua passe
  de crash-au-démarrage → vérif version → lib `_G` → lib `package` → phase mark GC.
  Régression stable (75 tests, diff 268/268) à chaque étape. **Prochain : tracer la
  corruption GC (origine du pointeur 0x8) ; chercher d'autres mislifts largeur/signe.**
  *Breadcrumb forensics* : `reallymarkobject`/beaucoup de fonctions internes Lua
  utilisent **regparm** (`eax`/`edx`/`ecx`, pas la pile) — `reallymarkobject` :
  `eax=g, edx=o`. Donc lire les args via les **paramètres-registres** de la
  fonction liftée, pas via `[mesp+…]` (sinon valeurs fantômes). Crash : traversée
  d'objet gris (`propagatemark`) → référence enfant `&array[i]`/champ à une adresse
  minuscule (0x8) → tag TValue lu à `[0x8+8]`. Suspecter la construction de table
  (`luaH_resize`/`setnodevector`/`luaH_new`) ou une écriture de champ 64↔32 bits.
