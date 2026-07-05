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

## 0. Stratégie de couverture — 2 axes + oracles différentiels

« Universel » se décompose en **deux axes indépendants** ; un binaire peut casser
sur l'un sans toucher l'autre, et on ne peut pas substituer l'un à l'autre :

| Axe | Quoi | Où vit le bug | Oracle automatisé |
|-----|------|---------------|-------------------|
| **1. Justesse de traduction CPU** | chaque instruction x86 → C correcte | le **lifter** (`ir/lift.rs`) | **Unicorn** (`src/cpudiff.rs`) |
| **2. Couverture des API OS** | les appels Win32/CRT que le programme fait | le **HLE** / Winelib | **Wine** (à venir) |

**Accélérateur = automatiser l'oracle** (au lieu de débugger un programme à la
main avec gdb) :

- **Axe 1 — `src/cpudiff.rs`** (`cargo test --features unpack cpudiff`) : lifte
  chaque instruction d'un corpus, l'exécute sur **des milliers d'états aléatoires**
  via un interpréteur IR **et** via Unicorn, diffe registres + flags. L'interp
  renvoie `None` pour ce qu'il ne modélise pas → **case sautée, jamais de faux
  positif**. Trouve les bugs lifter **en lot**, avant qu'un programme ne les
  exhibe par un crash. (A déjà trouvé : retenue width-aware, shift ZF à count=0.)
- **Axe 2 — différentiel Wine** (à construire) : un **corpus de binaires** exécuté
  sous Wine (vérité terrain Win32) **vs** ARET, diff stdout/exit. Donne le **%
  de programmes qui matchent** = métrique chiffrée de l'avancée globale, et tire
  la couverture API. À amorcer **quand l'axe 1 est blindé**.

Ces oracles **renforcent** le principe sacré : une divergence = bug prouvé ; une
correspondance = correction prouvée ; les chemins non modélisés restent `Asm`/abort.

**Démonstrateurs de référence** : **Lua 5.4** (PE 650 Ko → ELF natif, **35/35** sur
une batterie de 14 sous-systèmes) pour l'axe 1 ; **busybox** (echo/true/false/
basename/pwd/cat) pour l'axe 2.

---

## 1. Méthode de travail (à respecter)

1. **Une tâche à la fois**, méthodique. Pas de big-bang.
2. Pour chaque tâche : (a) comprendre/repro, (b) **fixture minimale testable** si
   possible (mieux qu'un gros binaire), (c) implémenter, (d) **vérifier** (tests +
   régression + différentiel si lifter), (e) **commit** descriptif, (f) **mettre à
   jour le journal §4**.
3. **Jamais casser la régression** : `cargo test --release` doit rester vert ;
   pour tout changement de lift/structure, lancer aussi `bench/difftest.sh`
   (différentiel **décompile**, -O0→-O3) **et** `bench/difftest_transpile.sh`
   (différentiel **transpile** = le vrai produit) et viser **aucune régression**.
   Pour un changement du **lifter**, lancer en plus le **différentiel Unicorn**
   `cargo test --features unpack cpudiff` (axe 1, §0).
4. Commits petits et fréquents. Pousser souvent.

### Commandes clés
```
cargo build --release
cargo test --release                 # suite par défaut
cargo test --release --features unpack          # + unpacker
cargo test --release --features unpack cpudiff  # différentiel lifter ↔ Unicorn (axe 1)
bash bench/regression.sh             # porte unifiée (build+tests+niveaux)
bash bench/difftest.sh               # différentiel décompile (Pipeline A, -O0→-O3)
bash bench/difftest_transpile.sh     # différentiel transpile (Pipeline B = produit réel)
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

### Phase 2 — Pruning par accessibilité (CONVERSION CIBLÉE) ✅ FAIT
Transpiler **une fonction + uniquement ses callees** (fermeture transitive), pas
tout le binaire. *Livrable* : `--prune`/auto avec `--function`. *Critère* :
transpiler `sub_X` d'un gros binaire ne sort que la fermeture, compile, tourne.

### Phase 3 — Universalité des binaires **strippés** ⏳ EN COURS (mingw solide, MSVC restant)
Deux volets : (a) **récupération de fonctions** sans symboles, (b) **reconnaissance
CRT** (FLIRT mingw/MSVC). Volet (a) prioritaire car c'est le blocage mesuré.
> **Jalon (2026-07-02) : Lua mingw strippé tourne bit-identique à Wine** (650 Ko →
> ELF natif, `table.sort`/`string.format` flottant/`gsub`/`gmatch`/`os.exit`, exit
> propre). Le chemin **mingw-strippé** est validé bout-en-bout sur un vrai gros
> binaire. Détail des 7 fixes généraux de la session : journal §4 (opérandes FLIRT
> relocalisés, recovery amorcée par signature, cases de switch ≠ fonctions,
> prologue `__do_global_dtors_aux`, libm fp-returning par FLIRT, thunks jamais
> signaturés, callback passé par valeur). **Reste : signatures MSVC (volet 3b).**
- **3a. Découverte des fonctions adressées (address-taken) ✅ FAIT.** Mesure : Lua
  symbolé récupère **987** fonctions, strippé seulement **314** — le scan de
  prologue ne connaît que `55 8B EC` (cadre de pile), or `-O2` omet le pointeur de
  cadre → 673 fonctions (cibles d'appels indirects : callbacks, vtables, tables
  `luaL_Reg`) jamais trouvées. Fix : passe 2b dans `global_decode` — scan des mots
  alignés des sections pour des **pointeurs vers du code** visant un début de
  fonction plausible (`looks_like_func_start` : `55/53/56/57`, `83 ec`, `81 ec`,
  `8b ff`, `ff 25`, `e9` — 90 % des entrées) **non encore décodé** (garde
  `!global.contains_key` → ne scinde jamais une fonction déjà récupérée, donc
  corpus régression intact). Lua strippé : **314 → 762** fonctions, tourne bien plus
  loin (plus de crash alloc/`_lock`). Symbolé : **987 inchangé** (la garde évite le
  bruit), tourne parfaitement. *Vérifié* : fixture strippée FPO
  `address_taken_callback` (callback via table de pointeurs, `RESULT=42`) ; **81
  tests** (m1 27→28) ; diff **268/268**.
- **3a-bis. Address-taken par immédiats + prologues feuilles ✅ FAIT.** Deux
  ajouts portant le recall strippé **762 → 971/987** : (1) passe 2c — scan du flux
  décodé (à point fixe) pour un **immédiat**/`[imm32]` absolu visant un début de
  fonction non décodé (callback passé par valeur : `push imm32`, `mov [esp+d],
  imm32` — le `Pfunc` des appels protégés Lua, ex. `_f_luaopen`) que le scan de
  données ne voit pas ; (2) `looks_like_func_start` accepte aussi `mov reg,[esp+
  disp]` (fonctions feuilles lisant leur 1er arg pile, ex. `_getS`). Les **appels
  indirects non résolus disparaissent**. Garde `!global.contains_key` conservée →
  corpus intact. *Vérifié* : 81 tests, diff 268/268, Lua symbolé inchangé (987).
- **3b-amorce. Shims CRT « morts » (double tiret) corrigés ✅ FAIT.** Même bug que
  `_initterm` : `sanitize_import` retire les tirets de tête → le générateur appelle
  `aret_lock` (import `_lock`), `aret_getmainargs` (`__getmainargs`), etc., mais une
  série de shims manuels avaient gardé les tirets (`aret__lock`, `aret___getmainargs`,
  `aret___p__fmode`, `aret___acrt_iob_func`…) → **morts** (chaque appel retombait sur
  le stub faible « unimplemented »). Renommés vers la forme assainie (les défs fortes
  écrasent les stubs faibles) ; supprimé les doublons de `aret_crt.c` (`aret_errno`,
  `aret_onexit`). Effet : Lua strippé ne meurt plus sur `_lock` ; ces internes msvcrt
  font enfin leur vrai travail (no-op / retour de pointeur) pour tout binaire. 81
  tests, diff 268/268, Lua symbolé toujours parfait.
- **3c. Frontières & reconnaissance strippé durcies (session 2026-07-02) ✅ FAIT.**
  Le trou « frontières » ci-dessus était la **sur-récupération** : les cibles de cas
  d'un `switch` (table `.rdata`) prises pour des fonctions, tronquant la vraie. Résolu
  (jump-tables résolues avant le seed + post-élagage des cibles). Plus : `--entry main`
  visait `___main` (glue) au lieu de `_main` ; libm statique non fp-returning en
  strippé (profondeur x87 désync) ; faux positif FLIRT sur les thunks ; callbacks par
  valeur non récupérés. Tous corrigés **généralement**, régression complète verte à
  chaque commit. Résultat : Lua strippé **complet** (voir jalon ci-dessus). Détail :
  journal §4.
- *Reste 3b (ouvert)* : signatures FLIRT pour **`ucrtbase`/`msvcr*` (MSVC)** — nécessite
  un **corpus MSVC symbolé** (indisponible sur l'hôte Linux/mingw actuel) ; le fix
  reloc-wildcarding profitera à ces signatures. Élargissement de la DB mingw par version
  = travail de données incrémental. *Critère* : un exe MSVC strippé reconnaît son CRT et
  tourne.

### Phase 4 — vtables / appels indirects C++ (VRAI CODE C++) ⏳ CŒUR VALIDÉ
Résoudre `call [vtable+k]` quand la vtable est en `.rodata` ; nommer la méthode.
*Livrable* : recovery vtables (analysis/ir) + fixture C++ virtuelle. *Critère* :
un appel virtuel se résout et s'exécute correctement (différentiel).

### Phase 5-soup — Réduire `partial(asm) → 0` : indirections (modèle 3 niveaux) ⏳ EN COURS
Cadre proposé (ChatGPT) pour ramener proprement le compteur `partial` à zéro, du
plus déterministe au plus dynamique. Mesuré par le verdict de solidité + `--strict`.
- **Niveau 1 — thunks mémoire `jmp [mem]` / `call [mem]` ✅ FAIT.** `jmp [mem]`
  (pointeur non indexé) lifté en **appel indirect tail sound** (lit le pointeur au
  runtime, dispatch) ; `call [mem]` déjà géré. Lua → `0 partial`, SOUND. (Détail :
  journal §4.) Une **résolution statique** vers appel direct (quand le slot est
  prouvé constant, ex. `.rdata`/IAT) reste une *optimisation* future, jamais au
  prix de la justesse (slot `.data` inscriptible).
- **Niveau 2 — indirections CRT / ABI runtime ⬜.** Symboles structurels du runtime
  (`____lc_codepage_func`, `__pctype`, `__mb_cur_max`, tables `_pctype`/locale…) :
  indirection déterministe mais propre au CRT. À traiter une fois le niveau 1
  stabilisé pour ne pas polluer la lecture. *Livrable* : reconnaissance + liaison
  host de ces tables/accesseurs. *Critère* : un binaire qui les exerce reste SOUND.
- **Niveau 3 — indirects dynamiques vrais ⬜.** `call eax`, tables de saut non
  résolues, pointeurs calculés → **analyse points-to / reconstruction CFG**, autre
  ordre de complexité (recouvre Phase 4 vtables). *Livrable* : sur-ensemble de
  cibles prouvé par fonction, sinon échec bruyant. *Critère* : un appel virtuel /
  pointeur calculé se résout, différentiel sans régression.

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

### Phase 5ter — Classification *structurelle* (contrainte, pas observation) ✅ FAIT
Demande (ChatGPT) : passer de l'observation à la **contrainte** dans le compilateur.
Un Shim ne doit **pas** générer de corps IR traduit ; un Internal suit une
traduction stricte ; aucune fonction host-backed ne doit apparaître comme
entièrement traduite ; les cas ambigus échouent en debug. Fix dans `builder` :
**partition** des fonctions récupérées en `internal_funcs` (lowerées normalement)
vs `host_funcs` (VA + nom de shim, **jamais lowerées** — corps élagué). On
n'élague **jamais** le point d'entrée. Le **dispatch indirect** route désormais
chaque VA host-backed vers un adaptateur `aret_disp_<va>` qui appelle le shim
natif — donc *aucun* chemin (direct ou indirect) ne peut atteindre un corps
non émis. **Invariant** (debug_assert) : les deux classes sont disjointes (une VA
n'est jamais à la fois traduite et host-backed). Source unique : `resolve_call`.
Bug réel trouvé et corrigé en passant : `_initterm` était assaini en
`aret_initterm` mais le shim manuel s'appelait `aret__initterm` (double tiret,
mort) → le no-op faible était utilisé. Renommé en `aret_initterm` (+ `_e`), gardé
en **no-op** (sous notre HLE le CRT est remplacé en bloc ; exécuter les ctors
d'origine atteint du MSVC interne non modélisé). *Vérifié* : 78 tests, diff
268/268, Lua entièrement fonctionnel (`--entry <main>` pour sauter le démarrage
CRT, comme prévu) — tamis d'Ératosthène, `table.sort`, `string.format`,
`math.pi^2`, `//`, `gsub`. Classes Lua inchangées : 896 / 1 / 90.

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
  *(Note : ce breadcrumb est antérieur ; la corruption GC a été résolue par 5d/5e
  — Lua tourne. Conservé pour mémoire de méthode.)*
- **2026-06-21 — Phase 5ter FAITE (classification structurelle).** La demande
  ChatGPT « passer de l'observation à la contrainte » est implémentée : partition
  `internal_funcs` (traduites) / `host_funcs` (jamais lowerées, corps élagué),
  dispatch indirect routé vers des adaptateurs `aret_disp_<va>` → aucun chemin ne
  peut atteindre un corps non émis, invariant de disjonction en `debug_assert`,
  source unique `resolve_call`. Bug réel corrigé : shim `_initterm` au nom mort
  (`aret__initterm` vs nom assaini `aret_initterm`) → renommé, gardé no-op.
  **Clarification importante (anti-régression mentale)** : en revérifiant Lua j'ai
  cru à une régression (crash) — en fait le crash existe **aussi sans mes
  changements** (et au commit « Lua tourne nativement »). Cause : transpilé via le
  **point d'entrée PE par défaut** (`_mainCRTStartup`, sas CRT Microsoft) au lieu
  de `--entry <main>`. Le démarrage CRT MS n'est pas modélisé (HLE remplace le
  runtime en bloc) → il faut entrer au `main`. **Tous** les builds « Lua
  fonctionnel » utilisaient `--entry`. Donc **pas de régression** ; le code committé
  est sain. *Vérifié* : 78 tests, diff 268/268, Lua entièrement fonctionnel avec
  `--entry 0x420009` (tamis, `table.sort`, `string.format`, `math.pi^2`, `//`,
  `gsub`). **Trou révélé (prochain)** : l'entrée **par défaut** (sas CRT) devrait
  marcher seule (auto-`main` ou amorçage CRT survivable) pour ne plus dépendre du
  flag — chantier « robustesse du démarrage », lié à Phase 3.
- **2026-06-21 — Robustesse du démarrage : auto-`main` par défaut.** Comble le
  trou ci-dessus. `analysis::auto_main_entry(prog)` : si le point d'entrée du PE
  est un amorçage CRT (symbole `*CRTStartup` ou `is_startup_glue`) **et** qu'il
  existe un symbole de fonction `main`/`_main` à une adresse distincte, le
  transpileur démarre au `main` (note explicite affichée) et saute le sas CRT non
  modélisé ; le `main` généré pose une frame cdecl synthétique (`argc`/`argv`).
  **Garde-fou** : un binaire *freestanding* (entrée = sa propre logique, **pas**
  de symbole `main` séparé — cas de toutes les fixtures `_mainCRTStartup@0`) est
  laissé intact. Effet : `aret -m transpile lua.exe` **marche sans `--entry`**.
  `--entry <addr>` force l'entrée d'origine si l'on veut le sas complet. *Vérifié* :
  nouvelle fixture+test `auto_main_entry` (l'amorçage imprime « boot » puis appelle
  `main` ; le redirect ne laisse passer que « main ») ; **79 tests** (m1 25→26) ;
  diff **268/268** ; Lua OK sans flag. *Reste* (binaires **strippés**) : rendre
  `find_main` (motif argc/argv→`call main`) plus robuste — il échoue encore sur le
  mingw de Lua ; couvert par Phase 3 (reconnaissance CRT MSVC).
- **2026-06-21 — `find_main` corrigé : ne renvoyait pas le bon `main` (strippés).**
  Le motif d'origine renvoyait le **premier** `call` à 3 args-pile vers une fonction
  utilisateur. Or l'amorçage MSVC/mingw fait **plusieurs** appels à des helpers CRT
  (3 args pile) **avant** `call main` → sur Lua strippé il renvoyait `0x431420`
  (helper) au lieu du vrai `main` `0x420009`. **Renvoyer un faux `main`, c'est de
  la sortie incorrecte présentée comme correcte** (principe sacré). Fix : exiger
  le **signal du code de sortie** — le résultat de `main` (`eax`) est sauvegardé
  (`mov [mem], eax`) juste après l'appel, avant clobber, alors que les helpers
  consomment le leur dans une boucle de setup → ils sont rejetés. On prend le
  **premier** candidat ainsi qualifié (l'amorçage est à basse adresse) ; sinon
  `None` (→ l'utilisateur passe `--entry <addr>`, pas de devinette). Sur Lua
  strippé : discovery correcte → `main` `0x420009`. *(Le binaire strippé ne tourne
  pas encore complètement — helpers CRT non reconnus, `_lock`/indirects ; c'est la
  reconnaissance FLIRT CRT de Phase 3, distincte.)* *Vérifié* : fixture **strippée**
  `crt_main_discovery` (decoy 3-args avant `main`, `main` imprimé) + test
  `find_main_discovers_real_main_in_stripped_crt` ; **80 tests** (m1 26→27) ;
  diff **268/268** ; Lua par défaut OK.
- **2026-06-21 — Phase 3a FAITE (découverte address-taken) → strippés tournent
  bien plus loin.** Blocage mesuré : Lua strippé ne récupérait que **314/987**
  fonctions (le scan de prologue ne voit que `55 8B EC`, mais `-O2` omet le cadre).
  Les 673 manquantes sont des cibles d'appels **indirects** (callbacks, vtables,
  tables `luaL_Reg`). Fix : passe 2b dans `analysis::global_decode` — scan des
  mots **alignés** de toutes les sections pour des pointeurs dans une section
  exécutable visant un **début de fonction plausible** (`looks_like_func_start`,
  ~90 % des entrées) et **non déjà décodé** (`!global.contains_key` → garde
  anti-scission : les fonctions atteignables/corpus restent intactes). Résultat :
  Lua strippé **314 → 762** fonctions, ne plante plus sur l'allocateur/`_lock` ;
  Lua symbolé **987 inchangé** et toujours parfait (la garde évite tout bruit).
  **Vérif** : fixture strippée FPO `address_taken_callback` (callback atteint
  uniquement via une table de pointeurs, `RESULT=42`) + test
  `address_taken_callback_recovered_when_stripped` ; **81 tests** (m1 27→28) ; diff
  **268/268** (aucune régression — confirmé que la garde protège le corpus). *Reste*
  : recall strippé encore partiel (762/987 — feuilles `8b 44 24`, pointeurs non
  alignés), un appel indirect non résolu (`0x418643`) ; puis volet 3b (FLIRT MSVC).
- **2026-06-21 — Phase 3 suite : faux-splits, DB FLIRT enrichie, désambiguïsation.**
  Trois correctifs après l'address-taken : (1) **faux-splits** — l'address-taken
  pouvait semer l'intérieur d'une fonction (cibles de `case` d'un jump-table en
  `.rdata`, qui commencent par `mov reg,[esp+disp]`) → fonction tronquée →
  miscompilée (Lua strippé : « too many registers », son propre code-gen). Fix :
  motif feuille réservé aux candidats venant d'un **immédiat** (vrai callback par
  valeur, ex. `_getS`), pas des mots de **données** ; drain **ascendant +
  incrémental** (le parent absorbe son intérieur) ; exclusion des cibles de
  jump-table résolues. Faux-splits Lua strippé : **21 → 0**, miscompile disparue.
  (2) **DB FLIRT 24 → 74** : fusion des signatures `--mode gensig` d'un binaire
  symbolé (libm `pow/exp/sin/cos/log/fmod/strtod` + CRT). Un strippé reconnaît
  enfin sa libm/CRT statique → liaison host au lieu de lifter un corps x87 dense.
  (3) **Match ambigu → `None`** : `sprintf`/`fprintf` mingw ont une signature
  **identique** (seul le `call __mingw_v{s,f}printf` wildcardé diffère) →
  l'ancien `match_at` prenait le premier (`fprintf`) → `sprintf` strippé lié à
  `aret_fprintf` → buffer passé comme `FILE*` → glibc abort. Désormais deux noms
  différents à égalité ⇒ aucune liaison (principe sacré). Bilan strippé : Lua
  passe de **314 fns/crash immédiat** à **949 fns**, démarre, imprime (`42`),
  calcule ; reste un segfault en aval (queue longue de recall ~38 fns + libm
  `sprintf` traduit). Symbolé : **987, parfait, inchangé**. **82 tests** (lib
  48→49, +unit `ambiguous_match`), diff **268/268** à chaque étape.

### Porte de solidité (sûreté du pipeline = principe sacré rendu vérifiable) ✅ FAIT
Objectif : ne **jamais** livrer un binaire qui « a l'air de marcher » mais ment ;
rendre toute incomplétude **visible à la conversion** et **bruyante au runtime**.
Distinction clé : on ne peut pas garantir la **complétude** (récupérer 100 % d'un
strippé quelconque est indécidable), mais on garantit la **solidité** (ce qu'on
produit marche, ou le dit). Trois apports :
- **Verdict de solidité** dans le rapport transpile : `SOUND` (tout appel direct
  résout, aucun asm opaque) vs `INCOMPLETE — N appels directs non résolus, M
  fonctions partial(asm)` + liste des adresses. Mesure honnête (ne certifie pas la
  couverture des appels **indirects**, non connaissable statiquement).
- **`--strict`** : sortie non-nulle si non-sound → un pipeline ne livre jamais un
  binaire connu défaillant.
- **Échec bruyant au runtime** : (1) une **instruction non modélisée** (`Stmt::Asm`)
  n'est plus un commentaire no-op silencieux dans le chemin transpile — elle appelle
  `aret_unmodelled(texte)` qui **abort** (un no-op serait une sortie fausse
  présentée comme correcte) ; (2) un appel (direct stub faible / indirect hors table)
  vers une **fonction non récupérée** abort aussi (au lieu de renvoyer 0 qui se
  propage). Les **imports** sans shim gardent warn+0 (fonctions *connues*, souvent
  no-op-ables, listées dans le rapport).
Découvertes : les fixtures freestanding sont **SOUND** ; Lua symbolé a **0 appel
direct non résolu** mais **1 fonction partial(asm)** (`____lc_codepage_func`, un
thunk locale hors chemin chaud) → le vrai reste à ramener à zéro est la
modélisation d'instructions, pas des fonctions manquantes ; Lua strippé idem (0
non résolu, 3 partial) → graphe d'appels directs entièrement récupéré. Sûr par
construction : l'abort ne se déclenche que sur un trou réellement atteint, donc
les binaires qui marchaient marchent toujours (Lua symbolé reste parfait).
**83 tests** (m1 +soundness_verdict), diff **268/268**.

### Réduction `partial(asm) → 0` (niveau 1 : thunks `jmp [mem]`) ✅ FAIT
Plan ChatGPT (3 niveaux : thunks mémoire constants → indirections CRT → indirects
dynamiques) — cohérent et raccord. Commencé par le **niveau 1**, mais corrigé sur
la **justesse** : la « résolution statique vers appel direct » n'est qu'une
*optimisation* (le slot `.data` est inscriptible, peut être repatché). Le fix
**sound** est de lifter `jmp [mem]` (pointeur non indexé) en **vrai appel indirect
tail** : lire le pointeur au runtime et dispatcher (`return (*ptr)(args)`).
`mem_indirect_target` exclut les opérandes indexés (`jmp [tbl+idx*4]` = table, géré
ailleurs) et la mémoire à segment. **Jalon** : le dernier `partial` de Lua
(`____lc_codepage_func`, `jmp [0x432090]` vers une table de pointeurs `.data`)
disparaît → **Lua symbolé = `0 partial`, verdict SOUND**, et **toujours
entièrement fonctionnel** (tamis, tri, flottants, format, gsub). Lua strippé : 0
partial aussi. **Premier vrai programme transpilé avec une preuve statique de
solidité, pas seulement « observé comme marchant ».** Fixture+test
`jmp_mem_tailcall` (`jmp *0x402000` → `JT=42`) ; **84 tests** ; diff **268/268**.
*Reste* (niveaux 2/3, si besoin) : indirections CRT structurelles, puis indirects
dynamiques (`call eax`, tables, points-to) — non requis pour Lua.

### Test de binaires variés → trou de solidité comblé (imports non implémentés) ✅ FAIT
Passé au crible `--strict` un corpus mingw réel (full CRT) : `floatmath` (libm),
`qsort_cb` (callback qsort), `structs` (malloc/listes). **Découverte majeure** :
`qsort_cb` était annoncé **SOUND mais imprimait son tableau NON trié** — `qsort`
est un import sans shim → stub faible (warn + return 0, no-op) → sortie fausse
présentée comme correcte. Le verdict **ignorait les imports non implémentés**.
Fix : le verdict compte désormais les imports **appelés** sans shim réel. L'ensemble
des shims implémentés est **parsé depuis les sources runtime embarquées**, y compris
les shims **générés par macro** (`MATH1(pow,…)` → `aret_pow`) en découvrant toute
macro qui colle `aret_##<param>` (auto-maintenu, pas de liste à la main qui dérive) ;
les intrinsèques setjmp/longjmp sont exclus. Gaps comblés : **qsort/bsearch** (un
**trampoline host→guest** qui dispatche le comparateur transpilé via `aret_call` sur
une frame cdecl scratch) + **wcslen** 16-bit. Résultat : le corpus C (qsort_cb,
floatmath, structs) est **SOUND et correct** (`qsort_cb` trie `1,2,3,5,7,9`, passe
`--strict`) ; Lua symbolé est honnêtement **INCOMPLETE** (12 imports os/io non
implémentés avec sites d'appel, hors chemin de base) au lieu de faussement SOUND.
Fixture+test `qsort_callback_into_transpiled_comparator` ; **85 tests** ; diff
**268/268**. *Leçon* : tester des binaires variés est le bon moteur pour durcir la
garantie — chaque nouveau programme révèle un gap mesurable. *Reste mesuré* (Lua) :
difftime/freopen/gmtime/localtime/mktime/rename/setvbuf… (lib `os`/`io`).
*Pas de C++ (vtables) : pas de `g++` mingw ici — à tester ailleurs (niveau 3).*

### Poursuite Lua SOUND → 2 angles morts du verdict révélés (honnêteté) ✅ FAIT
Décision (je suis décisionnaire) : **consolider Lua → SOUND** plutôt que C++ (niveau 3
bloqué : pas de `g++` mingw ici). En implémentant les imports `os`/`io` manquants, la
poursuite a révélé que **le verdict lui-même avait 2 angles morts** — précisément ce
que les tests doivent sortir :
1. **`has_opaque_asm` ne comptait que `Stmt::Asm`**, pas la forme **appel `asm:`**
   (instruction non modélisée sans valeur, ex. `fstp [mem]` → `0 /*asm:…*/`). Une
   fonction qui no-op'ait silencieusement une telle instruction était comptée comme
   lifted. Corrigé (les deux formes comptent). **Vérité révélée** : les binaires full-CRT
   ont des internes CRT statiques non modélisés (Lua **31** partial, corpus C 14-22) —
   ils tournent car hors chemin exécuté, mais **pas prouvés SOUND**. Les freestanding
   (`-nostdlib`) restent **réellement SOUND**.
2. **`call_returns_fp` ne gérait que les appels directs** → un import fp-returning appelé
   via l'IAT (`call [imm32]`, `FlowControl::IndirectCall`, ex. `difftime` → `double`)
   n'était pas reconnu → le `fstp` du caller sous-débordait le modèle x87, le store était
   **perdu → valeur fausse silencieuse**. Reconnaissance des imports fp-returning
   (IAT-indirect + thunk) ; `prog` câblé dans l'analyse x87. **os.difftime** corrigé
   (60.0/750.0, était un dénormal) ; −1 partial.
+ shims `os`/`io` Lua : gmtime/localtime/mktime/strftime (**marshalling `struct tm`**,
layout Windows 9 ints), difftime, rename, freopen, tmpfile, tmpnam, setvbuf, system →
os.date/os.time/os.difftime/io fonctionnent. **85 tests** ; diff **268/268**
(changement de classification seul, lifting inchangé). **Leçon clé** : pousser vers
« 100 % SOUND » est le meilleur révélateur des angles morts de la métrique elle-même —
le verdict est maintenant beaucoup plus digne de confiance. *Reste pour Lua SOUND* :
modéliser les ~31 internes CRT statiques (x87/SSE dans printf/locale), OU les reconnaître
host-backed (FLIRT élargi). *Cohérence à finir* : la forme `asm:` no-op encore au runtime
au lieu d'`abort` (Stmt::Asm, lui, abort) — à uniformiser.

### Ordre logique : (1) sûreté runtime ✅ FAIT — (2) réduire partial ⏳ DIAGNOSTIQUÉ
Philosophie : **d'abord la sûreté (rien de faux en silence), puis la complétude**.
- **(1) Cohérence runtime ✅ FAIT.** La forme **appel `asm:`** (instruction non
  modélisée en position d'expression, ex. `fstp [mem]`) émettait `0 /*asm:…*/` —
  no-op silencieux. En mode transpile elle émet maintenant `(aret_unmodelled("…"),0)`
  → **abort**. Combiné à `Stmt::Asm` (déjà abort), **toute** instruction non
  modélisée, en statement OU expression, échoue bruyamment quand atteinte. Vérifié :
  Lua tourne entièrement (rien d'atteint → rien n'abort ; l'abort est un filet, pas
  un changement de comportement). 85 tests, diff 268/268. → **le principe sacré tient
  désormais au runtime, pas seulement à la conversion.**
- **(2) Réduire `partial → 0` — DIAGNOSTIC PRÉCIS (pas encore corrigé).** Les 31
  partial de Lua se répartissent en : (a) **~4 fonctions Lua VIVANTES** (`forprep`,
  `intarith`, `math_abs`, `lua_number2strx`) dont le chemin **flottant** fait
  **abandonner l'analyse de profondeur x87** (joins de pile ambigus : un bloc atteint
  avec deux profondeurs différentes → bail → *toutes* les ops x87 de la fonction en
  asm). Leurs chemins entiers marchent (d'où Lua fonctionnel). (b) **~27 internes CRT
  flottants MORTS** (famille `_D2A`/dtoa de David Gay, `__mingw_pformat`, libm
  `__*l_internal`) — injoignables car leurs points d'entrée publics (printf/sprintf/
  strtod/math) sont **host-backed** ; ils sont liftés mais jamais appelés. **Vérité
  importante** : le verdict les compte (conservateur — on ne *prouve* pas qu'ils sont
  morts, un appel indirect pourrait les viser), donc 31 est honnête.
  *Voies pour (2)* — par ordre de valeur × sûreté :
  1. **Réconciliation des joins x87** (les 4 vivantes) — cœur Phase 5, délicat
     (correctness x87 critique) → session dédiée, difftest à chaque pas.
  2. **Host-back des libm restantes** (sqrt, ldexp, variantes `*l`) — *réel* (vraie
     libm) mais l'ABI `long double` (args 80-bit sur la pile) demande un marshalling
     dédié, ≠ macro `MATH1` (qui lit des `double`). Gain modeste (~4-6).
  3. **Élimination de code mort** (ne pas lifter une fonction sans site d'appel) —
     réduirait les 27 mortes, mais conservatisme requis (appels indirects).

### Réduction partial : 2 angles morts du lifter x87 corrigés (31 → 20) ✅ FAIT
En poursuivant Voie 1 (joins x87), deux bugs *simples* du lifter ont été trouvés
et corrigés AVANT le vrai travail de joins — gros gain à faible risque :
- **`is_x87` ne listait pas `Fabs`** (mais avait `Fchs`). `fabs` (d9 e1) prend st(0)
  implicitement (pas d'opérande st explicite côté iced), donc il passait à travers
  → la passe de profondeur le **sautait** → jamais modélisé → no-op silencieux
  (valeur absolue perdue). `x87_delta`/`x87_try` le géraient déjà ; seul le portier
  était faux. Fix : ajouter `Fabs`. **math.abs(-1.5)=1.5** correct (était faux).
  partial 31→30. Fixture+test `x87_fabs_is_modelled`.
- **`lea` bailait sur préfixe de segment** (`mem_addr` refuse tout segment), or
  `lea` ne calcule que l'adresse effective (jamais la base de segment) → le NOP
  multi-octets `cs:` (`2e 8d b4 26… = lea esi,cs:[esi]`, padding d'alignement)
  tombait en asm, rendant **toute sa fonction partial** (et risque d'abort sur un
  NOP). Fix : `lea` utilise `mem_addr_raw` (ignore le segment, *sound* par sémantique
  x86). **partial 30→20** (10 fonctions n'étaient partial QUE pour ce NOP → désormais
  propres) ; plus aucun `lea` non modélisé.
Les deux : diff **268/268**, 86 tests, Lua entièrement fonctionnel. **Bilan partial
Lua : 31 → 20.** *Reste précis* : (a) **fonctions Lua VIVANTES** (`forprep`,
`intarith`, `lua_number2strx`) bailent sur **joins x87 ambigus** (toutes leurs ops
sont modélisables → c'est la profondeur au join qui diverge) — **le vrai travail
Voie 1**, à faire prudemment (correctness x87 critique, difftest à chaque pas) ;
(b) **internes libm MORTS** bailent sur `fxam` (classification, absent de
`x87_delta`), transcendantes (`fyl2x`/`f2xm1`/`fscale`/`fsin`/`fcos`/`fptan`) et
chargements de constantes (`fldpi`/`fldl2e`/`fldln2`…) — modéliser ces dernières est
sûr mais à précision 80-bit, valeur faible (code mort).

### Diagnostic précis des 3 partial vivantes (corrige l'hypothèse « joins ») 🔬
Instrumentation des points de bail x87 sur `forprep`/`intarith`/`lua_number2strx` :
**ce ne sont PAS des joins ambigus** (aucun conflit de profondeur au join détecté).
Causes réelles :
- **`forprep` (0x42151a)** et **`intarith` (0x413d54)** : **sous-débordement** (`fstp`
  à profondeur 0 → -1). Un bloc est entré à la **mauvaise profondeur** : dans l'idiome
  de comparaison NaN `fldz; fld x; fucomi st,st(1); fstp st(1)`, le `fstp st(1)`
  **conserve** une valeur (copie st0→st1 puis pop ⇒ st0=x garde l'opérande), mais la
  propagation avant assigne `entry_sp=0` à un bloc en aval (0x42159e) qui a en fait
  une valeur vivante (devrait être 1) → `fstp` suivant sous-déborde → bail → toutes
  les ops x87 de la fonction en asm. Le delta de `fstp st(1)` (-1) est correct ; c'est
  la **profondeur d'entrée d'un bloc** qui est fausse (à creuser : ordre de
  propagation / prédécesseur mal compté dans la chaîne `0x421570→0x421578→0x42176e`).
- **`lua_number2strx` (0x41b7a8)** : `fist` **sans mode troncature prouvé**
  (`truncate_active` ne reconnaît pas le `fldcw` de cette fonction) → bail sûr.
*Conclusion* : le vrai travail restant est une **passe de profondeur x87 plus robuste**
(suivi correct des valeurs conservées par `fstp st(i)`/`fxch` dans les idiomes de
comparaison à branche NaN), pas une réconciliation de joins. Délicat (correctness
flottante), à faire en session dédiée, une fonction à la fois, difftest à chaque pas.

### `frndint` honore les 4 modes d'arrondi (résultat faux silencieux corrigé) ✅ FAIT
- **2026-06-22 — Bug de résultat faux silencieux (PAS une partial) : `5.5//2` rendait
  `3.0` au lieu de `2.0`, `floor(2.75)` rendait `3.0`.** L'idiome `floor`/`ceil` de la
  CRT règle le champ **RC** du control word x87 (bits 10-11 : `00` nearest, `01` down,
  `10` up, `11` truncate) via `or $imm,%ah; mov [X],ax; fldcw [X]; frndint`. Le lifter
  ne distinguait que **troncature vs nearest** (`truncate_active`), donc floor (RC=01)
  et ceil (RC=10) tombaient tous deux sur round-to-nearest → arithmétique fausse mais
  présentée comme correcte (**viole le principe sacré**). 
- **Fix** : `enum RoundMode { Nearest, Down, Up, Trunc }` (lift.rs) ; helpers
  `__x87_floor`/`__x87_ceil` (`__builtin_floorl`/`ceill`) en plus de `__x87_rint`/
  `__x87_trunc` ; `lift_x87`/`x87_try` prennent un `RoundMode` ; bras `Frndint` honore
  les 4 modes ; `Fist/Fistp` exige `RoundMode::Trunc`. La carte des ops devient
  `(profondeur, RoundMode)`. `truncate_active` remplacé par **`rounding_mode_active`** :
  trouve le `fldcw [X]` précédent le plus proche, **capture le slot mémoire X**, puis
  remonte au `mov [X], reg` qui l'a construit et au `or`-immédiat ~6 instr. avant —
  **liaison par slot CW** (essentiel : une fonction faisant floor `or 0x4`→slotA *et*
  ceil `or 0x8`→slotB ne doit pas croiser les deux).
- **Vérifié** : fixture `rounding.exe` → `floor=2.0 ceil=3.0 fdiv=2.0 nfloor=-3.0` ;
  Lua `5.5//2=2.0`, `7.0//2.0=3.0`, `math.floor(2.75)=2`, `math.ceil(2.75)=3`,
  `math.floor(-2.5)=-3`. Régression : **difftest 268/268**, suite complète verte.
  Test ajouté : `x87_frndint_honours_rounding_mode` (+ fixture `rounding.c`/`.exe`).

### `rounding_mode_active` voit au-delà du bloc (CW invariant de boucle) ✅ FAIT — Lua 19 → 18 partial
- **2026-06-22 — `lua_number2strx` (`string.format("%a")`, 0x41b7a8) bailait alors
  que son `fist` EST en mode troncature.** Diagnostic (instrumentation par-bail) :
  le `fist @0x41b94d` rendait `mode=Nearest`. Cause : le control word troncature
  (`or ah,0xc; mov [esp+0x2a]; …`) est installé **avant** une boucle (bloc 0x41b8fe),
  mais le `fldcw [esp+0x2a]; fist` sont **dans** le corps de boucle (entête 0x41b930,
  cible d'un `ja 41b930` en arrière → **join à 2 prédécesseurs**). L'ancien scan,
  **local au bloc**, ne voyait pas le `mov` qui construit le CW → `Nearest` → bail.
- **Fix (sain)** : `rounding_mode_active` prend désormais le flux d'instructions
  **de toute la fonction** (trié par adresse) + l'ensemble des **joins**. (1) Il trouve
  le `fldcw [X]` qui alimente l'op en remontant la **fenêtre straight-line** (s'arrête
  au 1er branchement/join → identifie X de façon sûre). (2) Il prouve la **valeur**
  de X en inspectant **tous** les `mov [X]` de la fonction : le slot CW est typiquement
  posé une fois avant la boucle et **invariant de boucle** ; si tous les writers
  installent le **même** mode, le `fldcw` charge prouvablement ce mode sur **tout**
  chemin. Un writer non-classifiable OU deux writers en désaccord ⇒ `Nearest`
  (bail sûr — `fist` reste asm, abort runtime si atteint).
- **Vérifié** : `string.format("%a", 1.5/0.1/255)` → `0x1.8p+0 0x1.999999999999ap-4
  0x1.fep+7` (correct). **difftest 268/268**, suite verte. Test +`x87_fist_truncate_with_hoisted_control_word`
  (fixture `truncloop.c/.exe` : `(long)(x*i)` en boucle, CW hoisté → `sum=40`).
- **Reste 18 partial** : 17 internes libm morts (fxam/transcendantes/load-constantes,
  jamais appelés) + `intarith` (0x413d54, sous-débordement x87 réel dans l'idiome de
  comparaison NaN — la prochaine cible Voie 1).

### 64-bit sur cible 32-bit : paire edx:eax + masque de comptage de décalage ✅ FAIT
- **2026-06-22 — Deux résultats faux silencieux (principe sacré) découverts en sondant
  Lua (`1<<62=0`).** (1) **Retour 64-bit perdu** : un `long long` se retourne dans la
  **paire edx:eax** en cdecl 32-bit ; modéliser le retour comme `eax` seul jette la
  moitié haute (et le DCE supprime alors le `shld`/`cdq` qui la construit). `shift(1,32)`
  rendait `0` au lieu de `4294967296`. (2) **Comptage de décalage non masqué** : x86
  masque le comptage à 5 bits (6 si opérande 64-bit) — `shl eax,32` est un no-op — mais
  le lifter décalait par le comptage brut, produisant `1<<32` en arithmétique 64-bit.
- **Fix** : `ir::build` `Return` combine `(edx<<32)|(eax&0xffffffff)` en 32-bit ; `lift`
  `call` **scinde** le résultat 64-bit en edx:eax (edx D'ABORD, puis masquer eax — sinon
  edx lirait le eax déjà tronqué = 0) ; `Shl`/`Shr`/`Sar` décalent par le comptage masqué.
- **Vérifié** : `wcall.exe` → `r=4294967296 m=12884901888` (retour 64-bit + décalage var
  via appel interne). **difftest 268/268**, suite verte. Test +`wide_64bit_return_and_shift_on_32bit`
  (fixture `wide_shift.c/.exe`).
- ✅ **FAUSSE ALERTE LEVÉE — aucun bug Lua** : en sondant Lua j'ai d'abord cru à une
  troncature 64-bit généralisée (`math.maxinteger=2147483647`, `1<<32=0`,
  `0xFFFFFFFF*0xFFFFFFFF=1`, `4294967296` lu en float). **Vérification décisive** :
  `string.packsize("j") = 4` → **ce binaire Lua est compilé avec `lua_Integer` 32-bit**
  (LUA_32BITS). Donc `maxinteger=INT32_MAX`, `1<<32=0` (décalage 32-bit), le produit qui
  wrappe à 2^32, et le littéral hors plage qui retombe en float sont **le comportement
  EXACT du binaire d'origine**. Le transpileur reproduit fidèlement la sémantique
  entière 32-bit du programme — `intarith` (sub_413d54) fait bien une addition/mul 32-bit
  (`v26 & 0xffffffff + uint32`), ce qui est correct ici. **Rien à corriger côté Lua.**
  Les corrections paire-retour/décalage ci-dessus restent valables pour du **vrai code
  `long long`** (vérifié par `wcall`/`wide`, qui utilisent réellement des entiers 64-bit).
  Leçon : toujours vérifier la *config du binaire cible* (`packsize`) avant de présumer
  un bug de transpilation.

### Borne de table de saut → frontières de fonctions correctes ✅ FAIT — Lua 18 → 17 partial
- **2026-06-22 — `intarith` (sub_413d54) absorbait `numarith` (sub_413e0e).** En instrumentant
  son bail x87 (`fstp` à profondeur -1 @0x413e7b) j'ai trouvé que **0x413e7b appartient à
  `numarith`**, une fonction *distincte* (0x413e0e). `intarith` a un switch entier de **14
  cas** (`cmp edx,0xd; ja; jmp [edx*4+0x4356c0]`) mais en récupérait **27** : `read_jump_table`
  lisait jusqu'au 1er mot non-exécutable, donc la table entière (14) débordait dans la table
  flottante adjacente (13 entrées, toutes exécutables) → 14+13=27, fusionnant les deux
  fonctions et faisant bailler la passe x87 (un chemin entier-profondeur-0 atteint du code
  flottant-profondeur-2).
- **Fix** : `resolve_jump_table`/`resolve_abs_jump_table` lisent la borne `cmp idx, N; ja
  default` (via `jump_index_bound`, nouvelle) qui précède le saut → table plafonnée à N+1
  entrées. Sans elle, deux switches adjacents se fusionnent.
- **Vérifié** : `intarith` a maintenant 14 cas (était 27) et est **entièrement lifté**
  (partial 18→17). Fixture `two_switch.exe` (2 switches denses 12+11, tables adjacentes en
  .rdata) : opA récupère **12 cas** (était 23 sans borne), résultat `t=3293` correct.
  **difftest 268/268**, suite verte. Test +`adjacent_jump_tables_are_bounded`.
  *Note* : le débordement corrompt la *récupération* (CFG/structure), pas la sortie runtime
  des indices in-range (le `ja` borne le dispatch à l'exécution) — d'où l'assertion sur le
  nombre de cas récupérés plutôt que sur la sortie.
- **Reste 17 partial** : 17 internes libm morts (fxam/transcendantes/load-constantes, jamais
  appelés). Plus aucune fonction *atteignable* n'est partial → la prochaine étape pour SOUND
  est le **pruning par accessibilité (Phase 2)** : prouver ces 17 inatteignables depuis main
  et les exclure du verdict (ou ne pas les émettre).

### Drapeaux de signe/débordement *width-aware* (compare signée d'opérande mémoire) ✅ FAIT
- **2026-06-22 — Résultat faux silencieux trouvé en testant des binaires variés (récursion).**
  `fib(20)` à -O0 rendait **-5778** au lieu de 6765 ; `fib(0)` partait en récursion
  `fib(-1)/fib(-2)`. Cause : le test de cas de base `cmp [n],1; jle` sur un **opérande
  mémoire**. `op_value` charge la mémoire typée `uint32_t` ; pour `[n]==0`, le résultat
  `(uint32)0 - 1 == 0xFFFFFFFF` était testé par `sign_neg = Slt(r,0)` en **64 bits**, où
  0xFFFFFFFF est **positif** → SF faux → `jle` prenait la mauvaise arête. Les opérandes
  **registre** (masqués en uint64, `0u64-1 == -1`) cachaient le bug — d'où difftest vert.
- **Fix** : `sign_bit(x, w) = (x >> (w-1)) & 1` (nouveau), correct quelle que soit la largeur
  C de l'opérande. `sub_flags`/`logic_flags` prennent `w` et calculent SF/OF via `sign_bit` ;
  `inc`/`dec`/`neg`/`cmp`/`test`/`sub`/`sbb`/`cmpxchg` passent `op0_width`. (`add_flags` était
  déjà width-aware.)
- **Vérifié** : `fib5=5 fib10=55 fact5=120`, `varied fib=6765`. Lua toujours OK (tri, `%a`,
  `-5//2=-3`, `floor(-2.5)=-3`). **difftest 268/268**, suite verte. Test
  +`signed_compare_of_memory_operand` (fixture `recursion.c/.exe`).
- *Reste à -O0* : un switch via `jmp [table]` non résolu (saut indirect vers 0x401672, milieu
  de fonction) → abort sûr. À diagnostiquer (idiome de table de saut -O0 différent).

### Table de saut -O0 « adresse calculée » résolue ✅ FAIT
- **2026-06-22 — Le `switch` à -O0 abortait** (saut indirect non résolu). À -O0 GCC ne fait
  pas un `mov tgt, [table+idx*4]` unique mais calcule l'adresse en étapes :
  `mov idx,[mem]; shl idx,2; add idx,table; mov tgt,[idx]; jmp tgt`. `resolve_abs_jump_table`
  n'attrapait que la forme base+index. Étendu : si la def atteignante est `mov tgt,[base]`
  (déréférencement simple), remonter à `add base, table` pour la base de table. La table se
  termine naturellement au 1er mot non-code (ici une chaîne `"fib=%d"`).
- **Vérifié** : `varied.exe` -O0..-O3 = sortie native identique (`fib=6765 pop=24
  mix=327750336 vowels=8 ops=13921 poly=37.8750`). **difftest 268/268**, suite verte.
  Test +`computed_goto_switch_at_o0` (fixture `varied_o0.c/.exe`).

### Différentiel du pipeline *transpile* — passer du réactif au proactif ✅ FAIT
- **2026-06-22 — Constat stratégique** (suite à une parenthèse de réflexion) : tous les
  bugs « modélisé-mais-faux » trouvés cette session (drapeaux de signe, masque de décalage…)
  l'ont été **réactivement**, en testant des binaires à la main. Le `difftest.sh` ne les
  attrapait pas car il n'exerce que `--mode emit` (**Pipeline A / décompile**), où un argument
  est un **paramètre uint64 sign-extended** ; or le produit réel est le **transpileur
  (Pipeline B)**, où un argument est un **chargement mémoire uint32 zero-extended**. Sémantiques
  différentes → le bug de signe ne se voyait que côté transpile.
- **Fix structurel** : `bench/difftest_transpile.sh` + `bench/gen_transpile_driver.py`. Le
  générateur assemble les 57 fonctions buildables du corpus (exclut `__int128`/SIMD que le
  i686-mingw ne compile pas) en **un seul programme** qui les appelle sur une grille d'entrées
  (incluant des **négatifs** — suffisant pour le signe, sans UB d'overflow), replie tout en un
  checksum. On transpile le PE, on exécute, on compare au **natif `-m32`** (même ABI 32-bit :
  long/pointeur 4 octets, flottants x87). Vérité-terrain stable (aucun UB) car valeurs dans la
  plage sûre du corpus.
- **Validé qu'il a des dents** : au commit *avant* le fix de signe (`fc681b1`), le checksum
  transpile **diverge** (`6c313d50…` ≠ réf) → ce banc **aurait attrapé** le résultat faux
  silencieux. Au HEAD : **4/4 niveaux -O0..-O3 identiques** (réf `4b0121f1…`).
- **Désormais** : tout changement de lift/structure doit passer `difftest.sh` (décompile) **et**
  `difftest_transpile.sh` (transpile). Prochaine amélioration possible : étendre la grille aux
  frontières non-UB (comparaisons/bitops avec INT_MIN, sûres) et au full-64 pour plus de couverture.

### Automatisation des patches BusyBox (méga-script → code ARET) ⏳ EN COURS
- **2026-06-22 — Contexte** : un utilisateur a fait tourner BusyBox-w32 transpilé via un
  méga-script de patches manuels (post-processing). Objectif : porter ces patches DANS ARET
  pour qu'aucun script externe ne soit nécessaire. Lecture critique d'abord : plusieurs
  « patches » n'étaient que des contournements d'**un seul** vrai bug (esp+4 des appels
  indirects) — une fois ce bug corrigé proprement, les rustines (write fd=1, isatty=1, stub
  crypto) deviennent inutiles.
- **Fait, général et sans régression** (Lua OK, difftest 268/268, transpile-diff 4/4) :
  - **Obj 4 (fninit)** : `finit`/`fninit` modélisés (reset pile FPU), plus de bail au startup.
  - **Obj 3 (argv/envp)** : `__getmainargs` rend les vrais argc/argv/environ (publiés par
    aret_main) au lieu d'un faux `{1,"program"}`. `__p___initenv`/`__p__environ` → vrai environ.
  - **Obj 1 (appels indirects IAT, esp+4)** : `__aret_patch_iat` écrit dans chaque slot
    d'import-fonction sa propre VA ; `emit_dispatch` enregistre chaque slot comme trampoline
    `esp+4` vers le shim (l'appel indirect a poussé une adresse retour, esp-=4, mais le shim
    lit ses args en [esp+0]). (Obj 2 / -no-pie : déjà dans le build ARET.)
  - **Shims CRT/IO** : `_write/_read/_close/_isatty/_setmode/_fileno/__p__iob` (forward host ;
    `__p__iob` initialise les FILE `_iob` : `_file=fd`, `_IONBF`), `vsnprintf/vsprintf/putenv/
    tzset`, console Win32 `SetErrorMode/GetConsoleMode/GetConsoleScreenBufferInfo` (échec si
    non-tty = comportement correct en pipe), `_open_osfhandle/_get_osfhandle` (identité).
  - **Récupération** : `looks_like_func_start` accepte le prologue garde-init `mov eax,[moffs32];
    test eax,eax` (initialiseur `_initterm` adressé seulement dans une table de pointeurs).
- **BusyBox va maintenant loin** : startup CRT → routage argv → écriture via IAT indirecte →
  init crypto → détection console → IO bas niveau. **Reste un crash** : déréférencement d'un
  pointeur de chaîne `0x1` dans une structure interne busybox (`mov (eax),eax` sur
  `[base+0x18]==0x1`) — problème d'**initialisation de données** (relocation/global) ou subtilité
  de lift, à débugger en session dédiée (busybox est invoqué sous le nom `busybox` pour le
  routage multi-call). Lua et le corpus ne sont pas affectés.

### FLIRT évalué empiriquement → cosmétique pour nos cibles (à ne pas re-creuser) ⚠️ MESURÉ
- **2026-06-26 — Question stratégique** : peut-on éviter le travail « un par un » via FLIRT
  (reconnaissance de fonctions de biblio par empreinte, déjà dans `src/flirt.rs`) ?
  **Mesure faite** :
  - **Sensible à la version du compilateur.** Référence MinGW locale vs binaire : Lua → **86**
    fonctions de biblio reconnues (sin/cos/exp/log/pow, tout le moteur strtod `*_D2A`,
    `__mingw_pformat`, internes libm) ; BusyBox (autre version MinGW) → **6** seulement.
  - **Les 16 `partial` de Lua = code MORT.** Identifiés par nom (`sqrt`, `ldexp`, `exp2l`,
    `__cosl_internal`, `__sinl_internal`, `__strtodg`, `__gdtoa`, `__mingw_pformat`, bignum
    `*_D2A`…). Vérifié que `tonumber`, `string.format("%g/%e/%.20f")`, `math.sqrt`, `2^0.5`,
    `tonumber("1e300")` **marchent tous sans jamais les toucher** (Lua passe par les imports
    msvcrt host-backed). Les vider via FLIRT serait **purement cosmétique** (verdict
    INCOMPLETE→SOUND, comportement identique) — beaucoup de shims pour zéro correction.
- **Conclusion** : FLIRT **n'est pas le raccourci** pour « gérer n'importe quel binaire ».
  - Reconnaît seulement du **code de bibliothèque** (jamais le code *propre* du programme,
    qui domine — ex. les applets/getopt de BusyBox où vivent les crashes).
  - Exige des empreintes de la **bonne version** (sinon ~0 match).
  - Pour nos cibles : Lua = cosmétique (code mort), BusyBox = version + hors-sujet.
  - Vraie valeur *future* (confort, pas débloquant) : (a) rendre le verdict SOUND *exact* en
    ignorant le code de biblio mort ; (b) filet de sécurité contre une mauvaise traduction de
    code de biblio complexe ; (c) si un jour on veut large couverture → **importer des bases
    `.pat` publiques** (les « décennies de travail » d'IDA/Ghidra) plutôt que régénérer.
- **Le levier réel reste le LIFTER** (clobber, signes, tables de pointeurs, esp+4…) : pas de
  raccourci, mais automatique et général — chaque fix profite à *tout* binaire. Aucun outil
  mûr (Wine = exécute, ne traduit pas ; Ghidra = décompile, non re-exécutable ; FLIRT =
  reconnaît, ne traduit pas) ne remplace ce cœur en sortie *native exécutable*.

### stdcall : compensation du sur-dépilage `ret N` (général, sans régression) ✅ FAIT
- **2026-06-26 — Bug** : une fonction `__stdcall` 32 bits dépile elle-même ses arguments
  (`ret N`). Sous *accumulate-outgoing-args* (GCC/MSVC -O2, args écrits par `mov [esp+k]` sans
  bouger esp), le compilateur émet juste après l'appel un `sub esp, N` pour **annuler** ce
  sur-dépilage — net : esp inchangé de part et d'autre de l'appel. Les shims d'import d'ARET
  lisent les args sur la pile modélisée mais **ne dépilent jamais** : l'appel laisse donc esp
  déjà net-correct, et appliquer le `sub esp, N` le descend de N trop bas → **corruption de
  tous les accès pile suivants** (setup d'arguments de l'appel d'après). Bug **général** des
  binaires Windows qui appellent des API stdcall à -O2 (constaté sur BusyBox :
  `mov [esp+8],0 ; mov [esp+4],0 ; mov [esp],0x47d008 ; call ds:0x49664c ; sub esp,0xc`).
- **Correctif** (`src/ir/build.rs`) : dans la boucle `build_ir`, on **ignore** un `sub esp, imm`
  qui suit *immédiatement* un appel lié à un shim hôte (`is_import_call` : appel direct
  import/thunk/CRT/glue résolu par `resolve_call`, **ou** `call [abs]` indirect via slot IAT
  fixe). Garde esp net-correct. Les noms d'import PE étant **non décorés** (pas de `@N`), le
  `sub esp, N` du compilateur **est** la source autoritative du compte de dépilage stdcall, et
  il n'apparaît qu'après un appel stdcall (le cdecl nettoie par `add esp` côté appelant — pas
  de faux positif). Gating étroit sur « appel d'import » = sûr pour le corpus existant
  (appels internes / pipeline décompile non touchés).
- **Non-régression** : difftest 268/268, transpile-diff 4/4 (`H=4b0121f1…`), `cargo test`
  49/49 + tous bancs verts. Lua inchangé.

### BusyBox re-testé sur build neuf (master) → crash `0x1`-deref localisé précisément 🔬
- **2026-06-26 — Setup re-établi** : le binaire BusyBox de la session précédente vivait dans le
  scratchpad (effacé à la compression). **Reconstruit depuis les sources** (rmyorston/busybox-w32
  master, `mingw32_defconfig`, `i686-w64-mingw32-gcc 13`) → `busybox.exe` **PE32 i386 634 Ko**.
  C'est une version **différente** de celle d'avant (donc comportement non identique au journal
  ci-dessus — à ne pas confondre).
- **Transpilation OK** : `aret --mode transpile` → **2218 fonctions** (2008 lifted, 83 partial,
  127 host-backed), binaire natif `out/app` produit, verdict INCOMPLETE (166 imports Win32 non
  implémentés — AccessCheck, CreateProcessA, DeviceIoControl… normal, hors chemin de base).
- **Crash immédiat sur tout applet** (`true`/`echo`/`pwd`) : **SIGSEGV**, pas un abort (donc faute
  mémoire réelle, pas une instruction non modélisée). **Localisé factuellement** (gdb + objdump +
  C généré) :
  - Frame : `main → … → sub_440514+…` (PE `0x440514`, parse du préfixe applet `:/1`/`:/2` de
    l'argv). Instruction fautive native `movzbl (%eax),%eax` = PE `cmpb $0x3a,0x1(%eax)`
    (`argv0[1]==':'`), `eax = *ebx = 0` → déréférence l'adresse `1`. **C'est exactement le
    `[…]==0x1` resté ouvert** dans la section « Automatisation BusyBox » ci-dessus.
  - Chaîne de données (C généré, `chunk_5.c`/`chunk_0.c`) : l'appelant écrit les args sur la pile
    partagée puis `sub_440514(esp, …)` ; à l'intérieur `v10 = [esp+0x18]` (= 2e arg, un pointeur
    type `argv`), `v18 = *v10` revient **0**, et `[v18+1]` faute. Donc `argv[0] == NULL` (ou le
    pointeur `argv` global `*0x493eb8` pointe sur 0) au moment du parse de préfixe applet.
- **Verdict** : ce crash est un **bug distinct du stdcall** (le fix stdcall de ce jour est correct
  et prouvé sans régression — il ne prétendait pas régler ça). C'est bien le **bug d'init/flux de
  données argv** annoté « session dédiée » : il faut tracer pourquoi `argv[0]`/le pointeur argv
  vaut 0 ici (mauvais setup au démarrage CRT, ou store/lift d'un global argv) — à faire en suivant
  l'origine du pointeur, une étape à la fois, sans supposition. Lua et le corpus **inchangés**.

### Cause racine du crash argv = `_initterm` no-op → CRT init sautée ✅ FAIT (général)
- **2026-06-26 — Diagnostic complet (faits, gdb + objdump + C généré)** : `sub_440514` est le
  `_main` de BusyBox ; il reçoit `argv = _argv` (global `0x493eb8`) et lit `argv[0]`. Au runtime
  `_argc (0x493ebc) = 0` et `argv = [NULL]` → crash. Remontée de la chaîne CRT : `_argc`/`_argv`
  sont peuplés par `__getmainargs`, appelé par **`_pre_cpp_init` (0x401110)**, lui-même invoqué
  **non pas directement** mais via la table d'initialiseurs `_initterm(__xc_a..__xc_z)` (vérifié :
  `0x498004 = 0x401110 = _pre_cpp_init`, `0x498010 = 0x401010 = _pre_c_init`). Or `__initterm` est
  un thunk d'import → **ARET le shimait en NO-OP** (décision Phase 5ter « CRT remplacé en bloc »).
  Résultat : **`_pre_c_init`/`_pre_cpp_init` ne tournent jamais → argv jamais construit → `_main`
  plante sur `argv[0]`**.
- **Le no-op violait le principe sacré** : sauter silencieusement l'initialisation du programme
  (argv vide) = comportement faux présenté comme correct. Le shim `_initterm` n'était sound que
  pour les binaires où la table ne contient que du glue MSVC interne ; pour une CRT mingw statique,
  elle contient le **propre code d'init du programme** (qui appelle le shim `__getmainargs`).
- **Correctif (général, `runtime/aret_hle/aret_hle.c`)** : `aret_initterm`/`aret_initterm_e`
  **parcourent réellement la table** `[first, last)` et **dispatchent chaque pointeur** non nul via
  `aret_call` sur la pile machine vive — exactement comme le vrai `_initterm`. Une entrée qui
  résout vers du code non récupéré **abort bruyamment dans aret_call** (sound), jamais un saut
  silencieux. (`_e` propage un retour non nul = échec d'init.) Les VAs de la table sont 32-bit
  (binaire `-m32`).
- **Effet mesuré** : BusyBox **route enfin ses applets** — `busybox true` → exit 0,
  `busybox uname -a` → `Windows_NT 148.0 0 i386 MS/Windows`. (Reste des bugs **plus profonds**,
  distincts : `echo` segfault, `pwd` `free(): invalid pointer` — + imports non implémentés
  `_fullpath`/`_chdir`/`_getcwd`/`GetEnvironmentVariableW` qui renvoient 0.)
- **Non-régression** : difftest 268/268, transpile-diff 4/4 (`H=4b0121f1…`), `cargo test` 49/49 +
  bancs verts. Lua inchangé (atteint son `main` via auto-main, ne dépend pas de cette table).
  *Bénéfice général* : tout binaire Windows à CRT statique exécute désormais ses constructeurs/
  initialiseurs C/C++ (argv, fmode, ctors globaux), au lieu de les sauter.

### ABI `__stdcall` des imports : modéliser le dépilage `@N` (echo marche) ✅ FAIT (général)
- **2026-06-26 — Diagnostic (faits, gdb + C généré)** : `busybox echo` plantait car l'écriture
  stdout échouait. Chaîne : `echo_main` → `full_write` → `safe_write` → `winansi_write` → `_write`.
  Au shim `_write`, **`fd=0` au lieu de 1** (`write(0,…)` → -1 → `bb_perror_msg` → crash). Cause :
  juste avant `_write`, `winansi_write` appelle `SetLastError@4` (`call *[IAT]`), un import **stdcall
  qui dépile son arg** (`ret 4`). En *accumulate-outgoing-args*, `esp` est **invariant** dans le
  corps ; le compilateur compense le dépilage par un `push`/`sub esp,4`. ARET modélisait l'appel
  d'import comme **esp-neutre** (le shim lit les args mais ne dépile pas) → après la compensation,
  `esp` finissait **4 trop bas** → le `fd` spillé était rechargé du **mauvais slot** (`[esp+0xc]`
  décalé) → `fd=0`.
- **Pourquoi pas le fix `sub esp` précédent** : ici la compensation est un **`push`** (1 octet),
  pas un `sub esp,N` — non capté. Et les noms d'import PE des API Win32 sont **non décorés**
  (`SetLastError`, pas `SetLastError@4`), donc le compte de dépilage n'est pas lisible au site.
- **Correctif (général)** : `src/ir/stdcall_pops.rs` — table `nom → @N` des API stdcall Win32
  (kernel32/user32/advapi32/ws2_32). Le `@N` est une **propriété fixe de chaque API** (ABI Windows,
  identique dans tout binaire), encodée une fois (154 entrées, extraites des décorations
  `__imp__X@N` de l'import-lib mingw ; purement additif, réutilisable). Dans `build_ir`, après un
  appel d'import dont on connaît `@N`, on **émet `esp += N`** (modélise le `ret N` du callee) →
  l'invariant esp tient, la compensation du compilateur s'applique normalement. Imports d'arité
  **inconnue** : repli sur le saut du `sub esp,N` (ancien fix, désormais réservé à ce cas).
- **Effet mesuré** : `busybox echo hello world` → `hello world`, `echo -n abc` → `abc` (sans LF),
  `echo -e 'a\tb'` → `a<TAB>b` — **tous corrects**. `true`/`false`/`uname -a` toujours OK.
- **Non-régression** : difftest 268/268, transpile-diff 4/4 (`H=4b0121f1…`), `cargo test` 49/49 +
  bancs verts. *Reste* (couche suivante, distincte) : `pwd`/`ls`/`expr`/`seq` **abort** (rc=134) —
  imports non implémentés (`_fullpath`/`_getcwd`/`GetEnvironmentVariableW`) qui renvoient 0, ou une
  instruction non modélisée atteinte (`aret_unmodelled`). À traiter applet par applet.

### Shims POSIX `_getcwd`/`_chdir`/`_fullpath` (pwd marche) ✅ FAIT
- **2026-06-26** — `pwd` abortait sur `free(): invalid pointer` : `xrealloc_getcwd_or_warn` appelait
  l'import `_getcwd` (stub → 0), laissant un buffer mal initialisé ensuite `free`é. Implémenté
  `aret_getcwd`/`aret_chdir`/`aret_fullpath` (`aret_crt.c`) en forward host (getcwd/chdir/realpath).
  Les pointeurs invités sont des adresses natives plates (`-m32`) → le host écrit directement
  dedans ; le `free` invité passe par l'import msvcrt (→ host free) donc un retour alloué côté host
  (forme buffer NULL) est libéré de façon cohérente. **`busybox pwd` → exit 0** (imprime le cwd).
  echo/uname/true inchangés. Non-régression : transpile-diff 4/4, `cargo test` 49/49.
- *Reste* : `expr` (sortie applet brouillée « : pr  syntax error » + heap), `ls` (rc=134),
  `seq` (l'analyse de profondeur x87 **abandonne** sur sa fonction → `fld1` non modélisé atteint :
  c'est le chantier « robustesse passe x87 », délicat, à part).

### Fonction absorbée après un appel *noreturn* → récupération via table de pointeurs ✅ FAIT
- **2026-06-26 — `basename` abortait** : appel indirect vers `0x40ef04` (`basename_main`) **non
  récupéré**. Cause (faits, dispatch + objdump) : la fonction précédente `baseNUM_main` finit par
  `call fflush_stdout_and_exit_SUCCESS` (**noreturn** : → `xfunc_die` → `exit`, sans `ret`). Le
  balayage linéaire, ignorant que l'appel ne revient pas, **continue** sur le `nop` de padding puis
  le prologue de `basename_main` → l'**absorbe**. `0x40ef04` se retrouve donc *déjà décodé* dans
  `global` → le scan de table de pointeurs (Pass 2b) le **saute** (garde `!global.contains_key`,
  anti-scission) → jamais une entrée → l'appel indirect de la table d'applets `applet_main` plante.
- **Correctif (`src/analysis/mod.rs`)** : dans Pass 2b, une **table de pointeurs confirmée** (≥3
  pointeurs de code consécutifs) dont une entrée est *déjà décodée* mais a la **forme d'un début
  de fonction** (`looks_like_func_start`) et n'est **pas** une cible de jump-table est **forcée**
  comme entrée — la table est autoritative (chaque entrée est un vrai début de fonction). Devenue
  frontière, le prédécesseur sur-absorbant est **re-scindé** au bon endroit. `looks_like_func_start`
  exclut les corps de `case` de jump-table (intérieurs) ; les cibles de jump-table sont filtrées.
- **Pourquoi ciblé/sûr** : seules les fonctions atteintes **uniquement** par table de pointeurs
  sont vulnérables (les cibles d'appel direct sont déjà des entrées/frontières). Le combo
  « run ≥3 + forme de début + pas jump-table » rend un faux positif très improbable.
- **Effet** : `0x40ef04` récupéré, **+6 fonctions** (2091→2097), `basename` + `tr` marchent
  (**11 applets** au total). difftest 268/268, transpile-diff 4/4, `cargo test` 49/49. *Note* :
  la cause profonde reste l'absence d'**analyse noreturn** (le balayage déborde tout appel
  `*_and_die`/exit) ; ici on la rattrape via la table. Une vraie passe noreturn serait plus
  générale (corrigerait aussi les absorptions hors-table) — chantier futur.

### x87 `ftst` modélisé ✅ FAIT — et blocage `seq` profond identifié
- **2026-06-26** — `seq` abortait sur `fld1` car `seq_main` faisait **abandonner toute** l'analyse
  x87 : `ftst` (compare st(0) à 0.0, met C0/C2/C3) était dans `is_x87` mais **absent** de
  `x87_delta` ET du lift de comparaison → `x87_delta`→None → bail. Ajouté `Ftst` (delta 0) +
  lift via le chemin C0/C2/C3 existant avec `b = __x87_zero`. **Fix général** (toute fonction à
  `ftst`). difftest 268/268, transpile-diff 4/4.
- **Reste pour `seq`** (blocage **distinct, profond**) : `seq` parse ses nombres via le `__strtodg`
  **propre à busybox** (bignum de David Gay, statiquement lié). Son switch dense
  `jmp *0x47c14c(,%ecx,4)` a son cas **default** (`0x405388`, atteint par `ja`) **mal récupéré
  comme une fonction** (problème d'ordre de résolution table-de-saut / scan address-taken, pas
  causé par le fix de récup ci-dessus — présent avant). Du coup le `ja 0x405388` intra-fonction
  ne peut former de Branch → asm → abort. **Piste recommandée** (cf. leçon Lua) : **host-backer
  strtod/strtodg** plutôt que lifter le bignum — mais exige de reconnaître l'entrée strtod de
  busybox (FLIRT faible ici). Chantier à part.

### Table de saut dense ≠ table de pointeurs de fonctions ✅ FAIT (général)
- **2026-06-26** — En creusant `seq` : son `__strtodg` (0x405330) était **scindé** parce que le
  scan address-taken prenait la **table de saut** dense `jmp *0x47c14c(,%ecx,4)` (en `.rdata`) pour
  une table de pointeurs de fonctions. Le cas `default` `0x405388` y apparaît **~20×** → récupéré
  comme fausse fonction → `__strtodg` coupé en deux → le `ja 0x405388` intra-fonction ne formait
  plus de Branch → abort.
- **Correctif (`analysis`)** : une vraie table de pointeurs (vtable, tableau d'applets) a des
  entrées **distinctes** ; une table de saut **répète** ses cibles. Une valeur répétée **≥3×** dans
  un run est donc un cas de switch (corps intérieur, pas un prologue) → exigée de passer la garde
  de prologue au lieu d'être acceptée pour sa seule présence. Appliqué **par valeur** (pas par run)
  pour ne pas jeter une table qui a une entrée aliasée. **−7 fausses entrées**, `__strtodg`
  re-fusionné. difftest 268/268, transpile-diff 4/4, 11/11 applets OK.
- **Reste pour `seq`** : `__strtodg` lift maintenant plus loin mais son **analyse de profondeur x87
  abandonne** (idiome de comparaison bignum — `fcomip`/`fildll`/joins) → `fild qword` non modélisé
  atteint → abort. C'est la **robustesse passe x87** (chantier délicat déjà identifié pour Lua
  `intarith`/`forprep`), OU host-back strtod (reconnaissance requise). À part.

### État des 3 cibles demandées (expr / host-back strtod / passe noreturn) — évaluation honnête
- **`expr`** : abort dans `bb_verror_msg` sur `free()` d'un pointeur invalide ; le message d'erreur
  est **brouillé** (« : pr » au lieu de « expr: »). Cause : `vasprintf` **propre à busybox** (lifté,
  pas un shim) produit un buffer/longueur faux — soupçon **gestion de `va_list`** à travers
  plusieurs appels (le format sort du garbage). Chantier ABI va_list, profond et risqué.
- **Host-back strtod (`seq`)** : exige de **reconnaître** l'entrée strtod de busybox (statiquement
  liée, pas un import) — FLIRT faible sur ce binaire. Mécanisme de reconnaissance à concevoir.
- **Passe noreturn générale** : analysée comme **marginale** par rapport au fix table-de-pointeurs
  déjà livré — les fonctions atteintes uniquement par table sont déjà récupérées (forced) ; les
  cibles d'appel direct sont déjà des frontières ; une absorption post-noreturn est inoffensive au
  runtime (l'appel noreturn ne revient pas, le code absorbé n'est jamais atteint). Bénéfice résiduel
  faible (rares callbacks hors-table) pour un risque réel (mal classer noreturn = tronquer une
  fonction qui revient = sortie fausse). À ne faire qu'avec un filet de régression Lua.

### Filet de régression Lua : reconstruit, mais bug de lift `lua_newstate` (mingw-13) 🔬
- **2026-06-27** — Pour attaquer la **robustesse x87** en toute sécurité (levier commun `seq` +
  Lua `intarith`/`forprep`), reconstruit **Lua 5.4.7** depuis les sources (`i686-w64-mingw32-gcc 13`,
  `-O2 -DLUA_USE_WINDOWS`) → `lua.exe` PE32 657 Ko. **Différent du Lua d'origine du journal** (autre
  version mingw → autre codegen).
- **Ce build abort au démarrage** (avant même `lua -v`) : `luaM_malloc_` appelle `g->frealloc` qui
  vaut **`0x41eb00`** (`panic+0x30`) au lieu de **`0x41eb50`** (`l_alloc`, décalage 0x50). Vérifié :
  `l_alloc` est **bien récupéré** (0x41eb50, pas de scission), son adresse est **bien passée**
  (`movl $0x41eb50,(%esp)` dans `luaL_newstate`). Donc **`lua_newstate` (0x4155a0) stocke une valeur
  fausse pour `g->frealloc`** → bug de **lift** (corruption de valeur), pas de récupération. **Pas une
  régression de cette session** : les 8 commits gardent l'équivalence corpus (transpile-diff 4/4) à
  chaque pas, et ne touchent pas les sémantiques de stockage de valeur. C'est un comportement de lift
  pré-existant exposé par le codegen mingw-13, à débugger en session dédiée (tracer le store
  `g->frealloc = f` dans `lua_newstate`).
- **Conséquence** : le filet Lua n'est pas encore opérationnel ; la robustesse x87 attend soit la
  correction de ce bug `lua_newstate`, soit une **fixture minimale** reproduisant l'idiome de
  comparaison NaN x87 (`fldz; fld x; fucomi; fstp st(1)`) — approche ciblée recommandée pour la
  prochaine session.
- **2026-06-27 — Diagnostic approfondi (watchpoint matériel, confirmé PRÉ-EXISTANT** : ARET au
  commit pré-session `c016432` échoue à l'identique → **pas une régression de cette session ;
  l'équivalence corpus transpile-diff 4/4 tenue partout le confirme aussi).** Mécanisme exact :
  1. `lua_newstate` stocke **correctement** `g->frealloc = 0x41eb50` (l_alloc).
  2. Puis **`stack_init` (sub_415380)** corrompt cette valeur : sa boucle `setnilvalue`
     (`movb $0,(slot+8)`) écrit des octets nil **par-dessus `g->frealloc`** → octet de poids faible
     mis à 0 → `0x41eb50` → `0x41eb00`. Plus tard `luaM_malloc_` appelle `0x41eb00` (non récupéré)
     dans le chemin dtoa (`__pow5mult_D2A`) → abort.
  3. Cause racine : l'**allocation de pile** dans `stack_init` (via `luaM_malloc_`→`l_alloc`→
     `realloc`) **renvoie un pointeur qui chevauche le bloc global_State** (`0x860d210` = bloc+0x70,
     alors que le bloc LG est à `0x860d1a0`). Les shims `aret_malloc/realloc/free` sont **corrects**
     (simple forward host) → le `malloc` hôte renvoie une vraie adresse fraîche, mais **le lift
     mal-propage la valeur de retour** (pointeur) entre `aret_realloc` et l'usage dans `stack_init`,
     produisant une adresse relative au bloc.
- **CAUSE RACINE TROUVÉE ET CORRIGÉE ✅ (général)** : `l_alloc` est un **wrapper mince** qui
  **tail-jumpe** vers l'import `realloc` (`jmp realloc`). Un `jmp import` (≠ `call import`) **ne
  pousse PAS d'adresse de retour** : l'adresse de retour de l'appelant reste en `[esp+0]`, donc les
  arguments de l'import commencent à `[esp+4]`. Or ARET passait au shim `esp` (comme pour un
  `call`, où le `call` pousse le retaddr et les args sont en `[esp+0]`) → le shim lisait le
  **retaddr comme arg0** et **tous les arguments décalés d'un slot**. Pour `l_alloc`→`realloc` :
  `realloc(retaddr_garbage, ptr_au_lieu_de_size)` → pointeur de pile qui chevauche `global_State`
  → `setnilvalue` écrase `g->frealloc`.
- **Correctif (`src/ir/build.rs`, Flow::Jump)** : un tail-call (`return f(args)`) vers un import
  `thread_esp` 32-bit reçoit désormais **`esp+4`** (saute le retaddr toujours sur la pile), au lieu
  du `esp` ajouté par `name_calls_in_expr` pour les `call`. **Bug général** : tout wrapper mince qui
  `jmp` vers un import cdecl/stdcall (très courant) était cassé.
- **Effet** : **Lua démarre et affiche sa bannière** (`Lua 5.4.7 Copyright…`) — la corruption mémoire
  a disparu. Non-régression : difftest 268/268, transpile-diff 4/4, `cargo test` 5/5, **busybox 6/6
  applets** OK. *Reste Lua* : « attempt to index a nil value » (erreur **bien plus tardive**,
  distincte) → Lua tourne maintenant assez loin pour servir bientôt de filet.
- **Bug `_G` nil = retenue (carry) d'un `add` 32-bit à immédiat signé ✅ CORRIGÉ (général)** :
  `luaopen_base` lisait `registry[LUA_RIDX_GLOBALS]` = nil. Trace (watchpoint) : la table globale
  **EST bien stockée** par `init_registry` et **jamais écrasée** → le bug est dans la **lecture**
  (`luaH_getint(registry, 2)`). Sa borne de tableau `(key-1) < alimit` est calculée en **64 bits**
  via `add $-1,%ecx ; adc $-1,%ebx`. ARET **sign-étendait** l'immédiat 32-bit `0xffffffff` en
  `0xffffffffffffffff` pour calculer la **retenue** du `add` bas → retenue=0 au lieu de 1 → l'`adc`
  produisait `(key-1)` = `0xffffffff00000001` au lieu de `1` → `(key-1) < alimit` faux → `registry[2]`
  ratait la partie tableau → nil. **Bug général** : toute arithmétique 64-bit construite par
  `add`/`adc` (ou `sub`/`sbb`) avec immédiat négatif sur cible 32-bit.
  **Correctif (`add_flags`, lift.rs)** : la retenue se calcule sur les opérandes **masqués à `w`
  bits** (`((a&mask)+(b&mask))>>w`), au lieu de `r>>w` qui héritait des bits hauts de l'immédiat
  sign-étendu.
- **Effet** : **Lua exécute de vrais scripts** — `print`, `6*7=42`, `table.sort` → `1,2,3`. Non-
  régression : difftest 268/268, transpile-diff 4/4, `cargo test` 5/5, **busybox 6/6**. *Reste Lua* :
  le **formatage flottant** (`2^10`, `math.sqrt`) bute sur `fld qword [ecx]` non modélisé (= la
  **robustesse passe x87**, maintenant abordable avec ce **filet Lua quasi-opérationnel**).

### 🎯 FILET LUA OPÉRATIONNEL + prochaine cible x87 (luaH_newkey)
- **2026-06-27 — Bilan** : 2 bugs **généraux** corrigés ce tour (tail-call import esp+4 ; retenue
  `add` 32-bit à immédiat signé). **Lua reconstruit (mingw-13) tourne** : démarrage, `print`,
  arithmétique entière, `table.sort`. Le **filet de régression Lua est désormais utilisable** pour
  attaquer l'x87 en sûreté (en plus de difftest/transpile-diff/busybox).
- **Prochaine cible (x87, localisée)** : `print(2.5)`/`2^10` → `luaH_newkey` (sub_417190) **abandonne
  son analyse de profondeur x87** sur l'**idiome de normalisation de clé flottante** (`luaV_flttointeger` :
  `n == floor(n)` via `fldl/fucomip/fstp` dans du code très branchy). **Tous les ops sont dans
  `x87_delta`** → c'est un **bail de profondeur/join** (même famille que Lua `forprep`/`intarith` et
  busybox `seq`/`__strtodg`). C'est LE chantier « robustesse passe x87 » (réconciliation des
  profondeurs aux joins / suivi des valeurs conservées par `fstp st(i)` dans l'idiome NaN) — délicat
  (correctness x87 critique), à faire **une fonction à la fois avec difftest + le filet Lua à chaque
  pas**. Débloque d'un coup : Lua flottant, `seq`, et les partials libm.

### 🎯 Cause racine x87 = chute *noreturn* fantôme + `compute_noreturn` rendu SOUND ✅ FAIT
- **2026-06-27 — Diagnostic corrigé** : le bail `luaH_newkey` n'était PAS une réconciliation de
  profondeur profonde mais une **chute fantôme depuis un appel *noreturn*** : la passe x87 ne coupait
  l'arête de chute *que si l'appel noreturn était la **dernière** instruction du bloc*. Or la balayage
  linéaire laisse souvent un `nop` de padding après l'appel → l'appel n'est pas le dernier insn → la
  chute (profondeur 0, code mort) restait et empoisonnait le `fucomip` du chemin NaN (join 1 vs 0).
  **Fix général** (`x87_depth_pass`) : dès qu'un `call` vers une fonction noreturn est atteint **où
  que ce soit** dans le bloc, on stoppe le walk et on **droppe les successeurs**.
- **Régression révélée puis corrigée (le vrai travail du tour)** : ce durcissement a fait chuter
  transpile-diff 4/4 → 2/4. Cause : `compute_noreturn` marquait noreturn **toute** fonction sans
  bloc `ret` — donc aussi les fonctions *tail-call* (`foo: jmp bar`, pas de `ret`) qui **retournent
  pourtant via `bar`**. Dropper leurs successeurs = miscompilation. **`compute_noreturn` réécrit en
  point-fixe SOUND** : une fonction « peut retourner » si un bloc finit en `ret`, **ou** en tail-jmp
  sans successeur interne vers une cible *non* prouvée noreturn, **ou** en indirect non résolu. On ne
  marque noreturn que si **aucun** bloc ne peut rendre la main. Faux négatif = on perd une optim ;
  faux positif = miscompile → on ne devine **jamais** noreturn. Le point-fixe attrape les chaînes de
  tail-calls vers un noreturn.
- **Diagnostic permanent ajouté** (`x87dbg`, `ARET_X87_DEBUG=1`) : imprime `fn=… @… : raison` à
  chaque bail x87 (générique, aucune adresse en dur) — c'est ce qui a permis de localiser la chute.
- **Régression** : transpile-diff **4/4** (H=4b0121f182554d40), difftest **268/268**, `cargo test`
  **93/0**. Lua : `luaH_newkey` ne bail plus (le bail flottant a avancé de `0x4174c8` →
  `fld dword [0x440d20]`, une **autre** fonction de formatage de flottants). **Prochaine cible** :
  ce `fld [0x440d20]` (constante flottante chargée dans la conversion nombre→chaîne, frappé même par
  `print(42)` via le chemin commun).

### 🎯 `compute_noreturn` : un tail-jmp a un *successeur* (arête inter-fonction) — faux positifs corrigés ✅ FAIT
- **2026-06-27 — Bug trouvé via le filet Lua** : `print(2.5)` abandonnait sur `fld [0x440d20]` dans
  `_luaopen_package`. Cause : le bloc `... call _lua_setfield ; fld [const] ...` voyait son **bloc de
  chute (le `fld`) supprimé** parce que `_lua_setfield` était **faussement marqué noreturn**.
  `_lua_setfield` n'a **aucun `ret`** : il retourne par des **tail-jmp** `jmp 0x4027e0`. Ma première
  réécriture testait `Flow::Jump if b.successors.is_empty()` pour détecter un tail-jmp — **mais le CFG
  enregistre la cible inter-fonction comme *successeur*** (`block [Jump] -> [0x4027e0]`), donc
  `successors` n'est **pas** vide → on tombait dans `_ => {}` → `may_return` jamais posé → marqué
  noreturn à tort.
- **Fix général** : pour `Flow::Jump | CondJump | Indirect`, « may_return » si un successeur n'est
  **pas un bloc interne** de la fonction (`!f.blocks.contains_key(&s)`) et n'est pas lui-même noreturn
  — c'est une cible de tail-call qui rend la main. Successeurs vides (saut calculé non résolu) ⇒
  may_return aussi. Détection robuste, indépendante de la façon dont le CFG peuple `successors`.
- **Impact mesuré (Lua)** : ensemble noreturn **86 → 22 fonctions** (les ~64 fonctions tail-call
  étaient toutes des faux positifs). La passe x87 ne supprime plus leurs chutes vivantes.
  **Monotone & sûr** : le nouveau `may_return` est un sur-ensemble de l'ancien ⇒ l'ensemble noreturn
  ne fait que **rétrécir** ⇒ on ne peut que **récupérer** du code, jamais en casser.
- **Lua** : `print(42)` **OK** (banner + nombre) ; `t.lua` imprime `hello 42` puis avance jusqu'au
  **prochain** bail x87 `fld qword [ebp+8]` (chemin de **formatage de flottants**, `print("fp:", 2^10…)`).
- **Régression** : transpile-diff **4/4**, difftest **268/268**, `cargo test` **93/0**, busybox
  (echo/true/false/basename) OK. **Prochaine cible** : `fld qword [ebp+8]` (conversion double→chaîne).

### 🎯 x87 `fxam` modélisé → Lua flottant COMPLET (sqrt, formatage) ✅ FAIT
- **2026-06-27 — `t.lua` passe en entier** : `hello 42 / fp: 1024.0 3.5 2 1.4142135623731 / sum 5050 /
  fmt 3.142 0.1 / sort 1,2,3`. Cause racine du dernier bail : l'instruction **`fxam`** (classification
  IEEE de st(0) → codes condition C3/C2/C1/C0, lus par `fnstsw ax`) était **absente de `x87_delta`**
  → la passe de profondeur abandonnait **toute** la fonction `_sqrt` en asm opaque → abort runtime sur
  le premier op x87 (le `fld [ebp+8]`). C'est l'idiome que le `__sqrt`/`fpclassify` de mingw utilise
  pour traiter NaN/Inf/zéro/négatif.
- **Fix général** (3 points, sans rustine) : (1) `x87_delta(Fxam) = 0` ; (2) `lift_x87` émet
  `fsw() = __x87_fxam(st0)` — exactement comme l'idiome `fcom`/`ftst` qui pose déjà les bits C0/C2/C3
  aux positions matérielles (8/10/14) ; (3) helper runtime `__x87_fxam` qui encode la classe via
  `__builtin_isnan/isinf/isnormal/signbitl` aux mêmes positions, plus C1=signe (bit 9). `fnstsw` lit
  déjà `fsw()` → tout le pattern `fnstsw ax; and ax,0x4500; cmp/test` fonctionne.
- **Note diagnostic** : les runs `--out-dir /dev/null` court-circuitent l'abaissement des fonctions
  (d'où « pas de bail loggé » trompeur au début) — toujours diagnostiquer l'x87 avec un vrai
  `--out-dir`.
- **Filet** : nouvelle fixture `x87_fxam.exe` (asm inline `fxam; fnstsw`) + test
  `x87_fxam_classifies_into_status_word` (NaN=256, Inf=1280, normal=1024, zéro=16384 ; valeurs
  vérifiées contre un build hôte `-m32`). Régression : transpile-diff **4/4**, difftest **268/268**,
  `cargo test` **94/0**, busybox OK. **Lua = filet de régression pleinement opérationnel**.

### 🎯 x87 `fcmovcc` modélisé — et un faux positif `condition_code()` attrapé par la vérif ✅ FAIT
- **2026-06-27 — Honnêteté de la vérif (principe sacré en action)** : en visant busybox `seq`, la
  cause convergente (Lua libm + seq) est le **déplacement conditionnel x87 `fcmovcc`**
  (`fcmove`/`fcmovne`/…). Première implé : `Select(cc_to_cond(ins.condition_code()), st(i), st0)`.
  Un fixture `test al,al; fcmove` passait — **par chance**. Un 2ᵉ fixture (`fmax`/`fmin`,
  idiome `fucomi; fxch; fcmove`) a donné **`fmax(3,7)=3`** (faux) et le `printf` flottant `-O2`
  (dtoa mingw, qui exécute fcmov) **`7.0 → 7.000001`**. Avant modélisation, ces fonctions
  **abandonnaient en abort** (sûr) ; les modéliser naïvement = **abort sûr → sortie fausse
  silencieuse** = violation du principe. **Donc pas de commit tant que faux.**
- **Forensics** : bissection — passage de 2 `double` ✓, `fucomi`+branche ✓, `fxch` seul ✓,
  max via `fcom/fnstsw` ✓ ; seul `fucomi+fxch+fcmove` ✗. Dump C non optimisé : `v41 = (1 ? v29 : v28)`
  → **la condition était la constante 1**. Cause racine : **iced `condition_code()` ne couvre PAS la
  famille FCMOVcc** (renvoie `None` → `cc_to_cond(None)=konst(1)` → move inconditionnel).
- **Fix général** : mapper explicitement le mnémonique → condition
  (`Fcmovb→b, Fcmovbe→be, Fcmove→e, Fcmovu→p, Fcmovnb→ae, Fcmovnbe→a, Fcmovne→ne, Fcmovnu→np`) puis
  `cc_to_cond`. Résultat : `fmax/fmin` corrects, **formatage `printf` flottant `-O2` correct**
  (`7.0 7.000 7 7.000000e+00`), `seq` **avance au-delà du dtoa x87** (bute désormais sur un op non-x87
  `movsd` chaîne — autre chantier). Lua inchangé (libm host-backed).
- **Filet** : fixture `x87_fcmov.exe` + test `x87_fcmov_uses_the_real_condition` (colonnes `%d` =
  valeur, `%f` = chemin dtoa ; réf. build hôte `-m32`). Régression : transpile-diff **4/4**, difftest
  **268/268**, `cargo test` **95/0**.
- **Leçon (réponse à la question « monotonie inter-procédurale » de ChatGPT)** : un op modélisé
  *correctement en isolation* peut **désamorcer le bail** de fonctions entières dont la sémantique
  combinée est fausse → le filet (2ᵉ fixture) l'a attrapé **avant commit**, l'abort sûr a servi de
  recul propre. Pas de monotonie globale prouvée, mais **toute incohérence reste bruyante**.

### 🎯 Shims I/O fichier (`GetFileAttributesA`/`_open`/`_lseek`) — débloque 9+ applets busybox ✅ FAIT
- **2026-06-27 — Levier mesuré par sondage** : un sondage large des applets busybox a révélé qu'un
  **seul** import manquant — `GetFileAttributesA` — bloquait **cat, wc, head, tail, sort, uniq, grep,
  cut, dirname** (tous les applets lisant un fichier). Puis le suivant : `_open` (I/O bas niveau
  msvcrt). Ajouté `aret_GetFileAttributesA` (stat → masque d'attributs Win32 / INVALID),
  `aret_open` (oflag msvcrt → flags POSIX : O_CREAT=0x100, O_TRUNC=0x200, O_APPEND=0x8, O_EXCL=0x400),
  `aret_lseek`. (`_read`/`_close` existaient déjà via `aret_read`/`aret_close` + strip du tiret.)
- **Bug d'override trouvé (mécanisme, général)** : `aret_open` ne remplaçait pas son stub faible.
  Cause : je l'avais placé **avant** la définition de `translate_path`/`make_parents` (fonctions
  `static` définies plus bas dans `aret_hle.c`) → déclaration implicite + types conflictuels → le
  symbole fort n'était pas émis proprement → le stub faible gagnait (warning silencieux sous `-w`).
  **Fix** : placer les shims qui utilisent `translate_path` **après** sa définition (zone des shims
  fichier). Leçon générale : un shim qui appelle un helper `static` du runtime doit être défini après lui.
- **Vérifié** : fixture `file_io.exe` (fopen/fputs/fread/remove → `_open`/`_read`/`_write`/`_close`) +
  test `file_io_roundtrip_through_crt` (`FILEIO n=12 a=line1 b=line2`, réf. build hôte `-m32`).
  Régression : transpile-diff **4/4**, difftest **268/268**, `cargo test` **96/0**.
- **Reste busybox** : les applets fichier avancent au-delà de `_open` mais butent sur un **segfault
  aval** (`sub_4104d0`, hors shim) + `GetEnvironmentVariableW` non implémenté — chantiers distincts.
  L'infrastructure I/O fichier est en place et **prouvée** par la fixture.

### 🎯 `sub_flags` : masquer les opérandes pour CF/ZF (compare signée 64-bit) → Lua 35/35 ✅ FAIT
- **2026-06-27 — Mesure concrète de l'avancée** : transpilé `lua.exe` → ELF natif Linux et passé une
  **batterie de 14 domaines** (arith/bitwise, strings, tables, math, **I/O fichier**, OS, goto/repeat,
  closures/varargs/multiret, **héritage POO/métatables**, **coroutines**, pcall/xpcall/error-objects,
  **load+env sandboxé**, itérateurs, modules). Résultat **34/35** ; seul échec : `256 >> 2 = 0`.
- **Cause racine (même famille que le bug retenue `add_flags`)** : `sub_flags` calculait **CF (`a <u b`)
  et ZF (`a == b`) sur les opérandes NON masqués**. Or `cmp r/m32, imm8` **sign-étend** l'immédiat à la
  largeur d'opérande (`0xffffffc1`), tandis que le registre est zero-étendu — dans l'i128 du modèle,
  CF devenait `(u64)lo < (u64)0xffffffffffffffc1` au lieu de `(u32)lo < (u32)0xffffffc1`. CF faux → le
  `sbb` du mot haut d'une compare signée 64-bit posait mal SF/OF → `jl`/`setl` inversé. Lua `>>` passe
  par `luaV_shiftl(x, -n)` qui teste `n <= -64` ⟹ rendait 0 pour tout décalage.
- **Fix général** : masquer `a` et `b` à `w` bits avant `Ult`/`Eq` (no-op pour w=64 ; SF/OF lisaient
  déjà le signe `w`-bit). Touche **toute** compare/soustraction à immédiat signé sur cible 32-bit.
- **Vérifié** : fixture `sub_flags_wide_cmp.exe` (réplique exacte de `luaV_shiftl` → `r1=64 r2=1024
  r3=0 r4=128`) + test. **Lua passe 35/35.** Régression : transpile-diff **4/4**, difftest **268/268**,
  `cargo test` **96/0** (→ 97 avec le nouveau test).
- **Note d'avancée globale** : ce Lua 35/35 (interpréteur réel 650 Ko, PE Windows → ELF natif Linux,
  **sans émulation**) est le démonstrateur de référence de l'axe « justesse de traduction CPU ».

### 🎯 `translate_path` : les chemins Unix absolus atteignent le vrai FS ✅ FAIT
- **2026-06-27 — Diagnostic busybox `cat <fichier>`** : le segfault aval `sub_4104d0` n'était PAS un
  bug de lift ni un layout `FILE` (cf. Gemini) — c'était le **sandbox de chemins**. `translate_path`
  préfixait les chemins **`/`-enracinés** sous `aret_prefix/`, donc `/tmp/in.txt` → `aret_prefix/tmp/in.txt`
  (inexistant) → `_open` échouait (fd=-1) → `cat` formatait son message d'erreur « cat: can't open … »
  et **c'est le formateur d'erreur qui crashait** (la struct à `aret_stack+…` contenait « cat\0can't o… »,
  champ `+0x18 = 1` lu comme `char*`). La fixture `file_io` passait car elle utilise un chemin **relatif**
  (write-then-read cohérents sous le préfixe) ; `cat` lit un **vrai fichier pré-existant** → cassé.
- **Fix général (objectif « outil natif »)** : un chemin **Unix absolu `/…` passe désormais tel quel au
  vrai système de fichiers** ; seuls les vrais chemins Windows gardent le préfixe (`C:\…` → `<prefix>/drive_c/…`,
  `\…` backslash-enraciné → `<prefix>/…`). Un outil Unix ne produit jamais de backslash-rooted, donc aucun
  risque ; les relatifs passent toujours (round-trips cohérents).
- **Effet mesuré** : `cat /tmp/in.txt` **ouvre maintenant le vrai fichier** (plus de « can't open »,
  formateur d'erreur plus atteint). cat avance puis crashe ailleurs (`sub_418710`, `memcpy` depuis NULL
  — chantier suivant) + `LoadLibraryExA` non implémenté.
- **Vérifié** : fixture `read_abs_path.exe` (lit un vrai `/tmp/…` créé par le test → `ABSREAD=HELLO_ABS`)
  + test. **Lua toujours 35/35.** Régression : transpile-diff **4/4**, difftest **268/268**,
  `cargo test` **98/0**.

### 🎯 `LoadLibraryExA` → fausse handle non-NULL → busybox `cat` MARCHE ✅ FAIT
- **2026-06-27 — Suite du diagnostic cat** : après le fix de chemin, `cat` crashait dans
  `concat_path_file("kernel32.dll", NULL)` — pas dans la lecture du fichier ! La glue **delay-load** de
  msvcrt sonde `kernel32.dll` via **`LoadLibraryExA`** (variante *Ex*, **non shimée** → stub renvoie 0).
  Sur ce NULL, busybox tente une recherche disque et construit un chemin avec un nom NULL → `memcpy`
  depuis NULL. (`LoadLibraryA` renvoyait déjà une fausse handle ; seul `LoadLibraryExA` manquait.)
- **Fix** : `aret_LoadLibraryExA`/`ExW` renvoient une fausse handle non-NULL (`0x10000000`, comme
  `LoadLibraryA`). Les vrais symboles sont déjà interceptés comme imports/shims, donc la handle n'a
  pas besoin d'être réelle.
- **Effet** : **`busybox cat /tmp/in.txt` fonctionne** (`hello world hello`) — rejoint
  echo/true/false/basename/pwd. (grep/cut sortent vide, sort/uniq bouclent — bugs distincts à part.)
- **Vérifié** : Lua toujours **35/35**, régression transpile-diff **4/4**, difftest **268/268**,
  `cargo test` **98/0**.

### 🎯 Harness différentiel lifter ↔ Unicorn (axe 1 automatisé) — 2 bugs de shift trouvés ✅ FAIT
- **2026-06-27 — Accélérateur de découverte** : au lieu d'attendre qu'un programme (Lua, busybox)
  exhibe une instruction mal liftée via un crash, on **valide le lifter directement contre Unicorn**.
  Nouveau `src/cpudiff.rs` (gated `feature=unpack`, réutilise la libunicorn déjà liée) : pour un corpus
  curé d'instructions entières (add/sub/adc/sbb/cmp/and/or/xor/test, immédiat sign-étendu, shl/shr/sar/
  shld/shrd, inc/dec/neg/not, cmovcc/setcc, 8/16/32-bit), on tire **des milliers d'états aléatoires**
  (registres + flags), on exécute chaque instruction **liftée (interpréteur IR) vs Unicorn**, et on
  diffe registres + flags CF/ZF/SF/OF. L'interpréteur renvoie `None` pour tout ce qu'il ne modélise
  pas (mémoire, sign-extension à largeur, calls, x87) → la case est **sautée, jamais de faux positif**.
- **2 bugs réels trouvés du premier coup** (invisibles à difftest/Lua jusqu'ici) :
  1. **ZF des décalages non préservé à count=0** : x86 laisse *tous* les flags inchangés quand le
     compte masqué est 0 ; le lifter ne préservait que CF (via `Select(c==0,…)`), posant ZF
     inconditionnellement. Un `shl r, cl` à `cl==0` (compte souvent runtime, cf. `luaV_shiftl`)
     cassait ZF → branche suivante fausse. Corrigé sur les 4 handlers (shl/shr/sar/shld/shrd).
  2. **ZF de `shl` sur résultat non masqué** : `0x80000000 << 1` = 0 sur 32 bits mais `0x100000000`
     en brut ; l'écriture registre masquait, pas le calcul de ZF. Corrigé (masque `w` bits).
- **Limite honnête de l'interpréteur** : il travaille en u64 sans suivi de largeur, donc ne peut pas
  reproduire un `Sar` (décalage arithmétique : signe au bit 31, pas 63). Vérifié que le **backend C est
  correct** (`x>>3` de -256 → -32 dans le binaire transpilé) ⇒ c'était un **faux positif de l'interp**,
  pas un bug lifter. La comparaison *valeur* est donc sautée quand le lift contient un `Sar` (les flags
  restent comparés). Améliorer = suivre les `Ty` dans l'interp (chantier futur).
- **Vérifié** : `cargo test --features unpack cpudiff` **vert** ; régression transpile-diff **4/4**,
  difftest **268/268**, `cargo test` **98/0**, Lua **35/35**.
- **Ceci est l'accélérateur demandé** : on passe de « débugger un programme à la main » à « une machine
  trouve les bugs lifter en lot, je corrige le général ». Même qualité, mêmes standards, bien plus vite.

### 🎯 Corpus différentiel élargi → 4 bugs de flags trouvés et corrigés ✅ FAIT
- **2026-06-27 — Axe 1, corpus complet** : `src/cpudiff.rs` étendu (~75 instructions : arith/logique
  reg+imm sign-étendu, shifts, **rotates**, **imul/mul**, **bt/bts/btr/btc**, **movzx/movsx**, toute la
  famille **cmovcc/setcc**, largeurs 8/16/32). Interp rendu **fidèle pour `Sar`** (extension de signe à
  la largeur inférée de l'`And(x, mask)`, comme le `signed_cast` du backend C) → plus de faux positif,
  comparaison valeur réactivée.
- **4 bugs réels trouvés et corrigés** (tous généraux, invisibles à difftest/Lua) :
  1. **`inc`/`dec` ZF sur résultat non masqué** (`inc al` de 0xff → 0 sur 8 bits) **+ OF manquant**
     (inc/dec posent OF). Corrigé : ZF sur résultat masqué, OF ajouté, CF préservé.
  2. **`rol`/`ror` CF non préservé à count=0** (x86 laisse les flags inchangés). Corrigé.
  3. **`adc`/`sbb` retenue d'entrée perdue au masquage** : `bc = b + cin` puis `bc & mask` **droppe la
     retenue** quand `b` est au max (ex. `adc r, -1` avec CF=1) — **affecte aussi le 32-bit** (paires
     add/adc de l'arithmétique 64-bit). `add_flags`/`sub_flags` refactorés en `*_cin` : retenue traitée
     à pleine précision (`(a_w + b_w + cin) >> w`), `b` original pour OF.
- **Vérifié** : `cargo test --features unpack cpudiff` **vert** (corpus complet) ; régression
  transpile-diff **4/4**, difftest **268/268**, `cargo test` **98/0**, Lua **35/35**.
- **Bilan axe 1** : le harness différentiel a maintenant trouvé **6 bugs de lifter** au total
  (retenue width-aware, shift ZF count=0, shl ZF non masqué, inc/dec ZF+OF, rol/ror CF count=0,
  adc/sbb retenue) — tous **généraux**, tous corrigés. Socle de justesse nettement durci.

### 🎯 Corpus axe 1 quasi complet (sous-ensemble entier) — opérandes mémoire + divers ✅ FAIT
- **2026-06-27 — Couverture mémoire** : `src/cpudiff.rs` étendu d'une **page scratch partagée**
  interp↔Unicorn. Pour une instruction à opérande mémoire, on force le registre de **base** vers la
  page, on remplit la page de bytes aléatoires (mêmes deux côtés), on évalue `Load`/`Store` dans
  l'interp, et on **compare aussi la mémoire** après (valide les stores / read-modify-write). Corpus
  porté à **~120 encodages** : reg+imm+**mémoire** (lecture / RMW / store, base+disp, 8/16/32-bit),
  rotates, bt/bts/btr/btc, movzx/movsx (reg+mem), lea, xchg, bswap, bsf/bsr, cdq/cwde, toute la
  famille cmovcc/setcc. **0 nouveau bug** sur le chemin mémoire → le lift des opérandes mémoire (calcul
  d'adresse, largeurs de load/store, RMW) est **correct**.
- **Périmètre honnête (ce que l'axe 1 NE couvre PAS — gaps identifiés)** :
  1. **Flottant (x87/SSE)** : l'interp ne modélise pas les helpers `__x87_*`/`__fp_*` → cases sautées.
     Demanderait un **différentiel FP séparé** (Unicorn sait émuler le x87). Sous-chantier distinct.
  2. **`div`/`idiv`** : scorer fidèlement demande à l'interp de modéliser le dividende 64-bit edx:eax,
     l'extension de signe du diviseur (idiv) et le trap #DE comme Unicorn le remonte. Retirés du corpus
     (le lift de la division est exercé bout-en-bout par `//`/`%` de Lua, qui passent 35/35).
  3. **Pipeline aval** (SSA / optimiseur / structureur / backend C) : couvert par difftest (268/268) et
     transpile-diff (4/4), pas par ce harness per-instruction. **Couverture complémentaire.**
- **Bilan** : la justesse du *lift entier* est désormais **très solide** (validée contre Unicorn sur un
  corpus large, des milliers d'états). Combinée à difftest/transpile-diff (pipeline) + Lua 35/35, l'axe
  « traduction CPU » est aussi blindé qu'on peut l'être sans preuve formelle. **Prêt pour l'axe 2 (Wine).**

### 🎯 Différentiels `div`/`idiv` + flottant SSE comblés → bug de lifter `ss` corrigé ✅ FAIT
- **2026-06-28 — Les 2 gaps de l'axe 1 fermés.** Suite au périmètre honnête ci-dessus (gaps « flottant »
  et « div/idiv »), les deux sont désormais couverts par le différentiel Unicorn.
- **`div`/`idiv` (entier 64-bit edx:eax)** ajoutés au corpus. L'interp rendu fidèle :
  - `SDiv`/`SMod` **width-aware** (extension de signe du diviseur depuis sa largeur, comme le
    `signed_cast` du backend C — même correctif que `Sar`).
  - **Détection de faute par EIP** plutôt que par `rc` : sur un `#DE` (diviseur nul *ou* quotient qui
    déborde la destination) Unicorn **rembobine l'instruction et laisse EIP au départ sans toujours
    remonter un `rc != 0`** ; si EIP n'a pas avancé, l'état a fauté → on le saute (au lieu de comparer le
    résultat tronqué de l'interp aux registres rembobinés d'Unicorn). Général : couvre div0 et overflow.
  - Trap par état (None de l'interp / load hors page) saute **l'état** (`continue`), plus l'instruction
    entière → ~1000 div / ~500 idiv valides scorés par 2000 états, **0 divergence** (le lift était juste).
- **Flottant SSE scalaire** : interp étendu pour modéliser les **registres XMM** (low `RegId(16+n)`,
  high `RegId(64+n)`) et évaluer les helpers `__fp_*` en **f64/f32 hôte** — qui, sur hôte x86-64,
  compilent vers les **mêmes instructions SSE IEEE-754** que la softfloat d'Unicorn reproduit → résultats
  **bit-à-bit** identiques, payload NaN et arrondi compris (vérifié directement). Conversions
  flottant→entier reproduisant le `cvtt` x86 (troncature vers zéro + **indéfini entier** `0x80000000` /
  `0x8000…` sur overflow/NaN, *pas* le `as` saturant de Rust). MXCSR mis au défaut. Corpus : add/sub/mul/
  div **sd & ss**, cvtsi2sd/ss, cvttsd2si/ss2si, cvtsd2ss, cvtss2sd, comisd/ucomisd/comiss/ucomiss.
- **1 bug réel de lifter trouvé et corrigé** (général, invisible à difftest/transpile/Lua car Lua est
  x87) : une op SSE **simple précision** (`addss`, `mulss`, `cvtsi2ss`, `cvtsd2ss`, …) met à jour
  **seulement** `dst[31:0]` et préserve `dst[127:32]` ; le lifter écrivait le résultat 32-bit dans la
  demi-lane 64-bit en **zéroïsant `[63:32]`** — divergence dès que cette demi est relue en lane 64-bit
  (movq/movsd, ops packed). `write_fp` fusionne maintenant le résultat simple précision dans la lane
  basse en préservant `[63:32]` ; les cas `sd` / destination entière / mémoire restent `write_op0`.
- **Vérifié** : `cargo test --features unpack cpudiff` **vert** (div/idiv + SSE) ; régression complète
  **difftest 268/268, transpile-diff 4/4 (hash inchangé), `cargo test` vert**.
- **Bilan axe 1** : **7 bugs de lifter** trouvés au total par le harness (les 6 entiers + ce `ss`),
  tous généraux et corrigés. Les sous-ensembles **entier, div/idiv et flottant SSE scalaire** sont
  maintenant validés contre Unicorn. Reste hors-différentiel : **x87** (lift par helpers `__x87_*` +
  passe de profondeur de pile inter-instructions → mal adapté au harness per-instruction ; couvert
  bout-en-bout par Lua 35/35) et les ops **SIMD packed** `__ps_*`/`__pi_*`. **Prêt pour l'axe 2 (Wine).**

### `div`/`idiv` : trap fidèle du #DE (plus de troncature silencieuse) ✅ FAIT
- **2026-06-28 — Gap de fidélité révélé par le skip EIP du différentiel, corrigé sur le général.**
  En expliquant *pourquoi* le harness saute les états fautants de `div`/`idiv` (cf. entrée
  précédente : sur un #DE le matériel rembobine, EIP n'avance pas → état sauté), vérification du C
  réellement émis : le chemin 32-bit faisait une division **64-bit** `(edx:eax)/diviseur` puis
  **tronquait** à 32 bits. Conséquence : sur un **débordement de quotient** (ex. `edx:eax` grand /
  petit diviseur → quotient > 2³²), l'original x86 **faute (#DE → crash)** mais le transpilé
  **continuait en silence avec une valeur tronquée**. = une sortie produite là où l'original plante →
  **viole le principe sacré**. (La division par zéro, elle, fautait déjà fidèlement : `n/0` en C → SIGFPE.)
- **Fix général** (pas une rustine binaire) : le lift de `div`/`idiv` 32-bit passe par des helpers
  `__ix_udiv32`/`__ix_umod32`/`__ix_idiv32`/`__ix_imod32` (`src/emit/mod.rs`) qui **reproduisent le #DE** —
  `__ix_diverr()` (`__builtin_trap`, crash déterministe, zéro dépendance) sur diviseur nul **ou**
  quotient hors de la largeur destination (et le cas `INT64_MIN/-1`). Le binaire transpilé plante
  désormais **exactement où l'original plante**, jamais de valeur fausse en silence. Les helpers
  64-bit (`__ix_udiv`/… via `__int128`/software) trappent aussi sur **diviseur nul** (ils
  renvoyaient `0` en silence — même classe de bug) ; le contrôle de **débordement 64-bit** côté
  software `-m32` reste à faire quand le lift 64-bit sera exercé (noté en commentaire — ne pas
  livrer une logique de division non testée).
- **L'oracle reste fidèle** : l'interp de `cpudiff` modélise ces helpers (`helper_call`) et renvoie
  `None` sur tout état fautant (div0, overflow, `INT64_MIN/-1`) — exactement les états où Unicorn
  faute et que le harness saute. Donc `div`/`idiv` restent **validés** (états valides comparés à
  Unicorn, ~1000/~500 par 2000), et le skip est désormais **pleinement honnête** (il ne masque plus
  une divergence produit↔matériel : sur ces états le produit *trappe* comme le matériel).
- **Vérifié** : helper en isolation (division valide correcte `udiv=805309116`, `idiv=-3` ; overflow
  et div0 → **crash**, pas de troncature) ; branche `__int128` compilée+exécutée en 64-bit
  (`-Wall -Wextra`, divisions valides correctes) ; `cargo test` vert ; **cpudiff vert** ; **difftest
  268/268** ; **transpile-diff 4/4, hash inchangé** (comportement des divisions valides identique).
  LLVM auto-déclare les helpers (`declare i64 @__ix_udiv32(...)`), WASM utilise le backend C → tous
  couverts. **Bilan** : la seule divergence que le skip EIP masquait est fermée ; `div`/`idiv` sont
  sound (valeur juste, ou crash fidèle — jamais de faux silencieux).

### Axe 1 consolidé : PF/AF modélisés + différentiel SIMD packed ✅ FAIT
- **2026-06-28 — Deux tronçons axe-1 fermés** (suite à l'inventaire des gaps : « lifté mais non
  validé » = risque de faux silencieux, à prioriser).
- **PF/AF (drapeaux parité / retenue auxiliaire)** — le harness ne comparait que CF/ZF/SF/OF, et le
  lifter ne posait PF qu'à de rares endroits flottants ; un `jp`/`jnp` (ou cmovp/setp) après une op
  entière lisait donc un drapeau **éventé** (faux silencieux latent). Corrigé **généralement** :
  - **PF** = parité de l'octet bas du résultat, via un helper `__ix_pf` (nommé dans la famille `__ix_`
    pour que le préambule C à la demande l'inclue — sinon lien cassé en mode décompile). Posé sur
    add/sub/adc/sbb/cmp, and/or/xor/test, inc/dec, neg, et les **shifts** (shl/shr/sar/shld/shrd).
  - **AF** = retenue/emprunt hors du bit 3, `(a ^ b ^ r) >> 4 & 1`, posé sur add/sub/adc/sbb/inc/dec/
    neg/cmp. **Pas** émis après logique/shift/mul (où x86 le laisse *indéfini*).
  - **SF des shifts** aussi : ils ne posaient que ZF/CF (un `shr; js` lisait un SF éventé) — SF ajouté
    avec préservation à compte nul, comme ZF/CF.
  Le harness compare maintenant PF et AF et modélise `__ix_pf` ; **tout matche Unicorn** sur des
  milliers d'états pour chaque op qui les pose.
- **SIMD packed** — le lifter gérait déjà `__pi_*`/`__ps_*` (paddd, pcmpeqd, addps, pshufd, punpck…)
  mais c'était **non fuzzé**. Ajouté au différentiel : interp étendu pour modéliser les helpers SIMD
  (entiers lane-wise + flottants `f32` hôte = même softfloat qu'Unicorn) et **33 instructions packed**
  au corpus (paddd/psubd/paddw, pcmpeqd/gtd/gtw, psubusw, pmuludq, pand/pandn/por/pxor, punpck l/h
  dq/qdq, pshufd, addps/subps/mulps/divps, minps/maxps, sqrtps, cvtdq2ps, andps/andnps/orps/xorps,
  cmpps, unpckl/hps, movmskps). **Coverage vérifiée** (chaque op scorée 2000/2000, pas de skip
  vacant ; corrigé en passant un encodage de test : `pshufd` exige le préfixe `66`, sans quoi c'est
  `pshufw` MMX). **0 bug trouvé** → le lift packed était déjà correct.
- **Vérifié** : `cargo test` vert (cmp = 6 défs de drapeaux), **cpudiff vert**, **difftest 268/268**,
  **transpile-diff 4/4 (hash inchangé** — l'optimiseur DCE les drapeaux non consommés, comportement
  et taille de code inchangés). **Bilan** : sur l'axe 1 per-instruction, les drapeaux **CF/ZF/SF/OF/
  PF/AF** sont tous vérifiés, et les sous-ensembles **entier, div/idiv, SSE scalaire ET SIMD packed**
  sont validés contre Unicorn. Restent hors-différentiel : **x87** (couvert end-to-end par Lua) et le
  64-bit (axe futur).

### Backlog axe 1 (documenté avant de passer à l'axe 2) 📋
Tronçons axe-1 **restants**, classés par risque. Aucun n'est bloquant (cœur blindé) ; conservés ici
pour ne rien perdre après compression. Distinction clé : « **lifté mais non fuzzé** » = risque de faux
silencieux (à valider un jour) ; « **non lifté** » = `Asm`/abort = **sound** (sans risque).
- **Lifté mais non fuzzé par cpudiff** (reachable, bas risque, à ajouter au corpus quand utile) :
  - **Atomiques** : `cmpxchg`, `xadd` (liftés) ; `cmpxchg8b`/`lock`/`xchg [mem]` à vérifier. Code threadé / compteurs de réf.
  - **Chaînes/`rep`** : `rep movs/stos/scas/cmps` (helpers `__rep_*`). Mal adapté au harness *per-instruction* (région + compteur + DF) → mini-différentiel « région » ou fixtures. Couvert end-to-end aujourd'hui.
  - **Transfert de drapeaux** : `sahf`/`lahf`, `pushf`/`popf` (liftés, niche).
- **Couvert autrement (pas par cpudiff)** :
  - **x87** : lift par helpers `__x87_*` + passe de profondeur de pile *inter-instructions* → inadapté au per-instruction. **Couvert end-to-end par Lua 35/35** (pow/sqrt/sin/floor-ceil/`%a`…).
  - **Pipeline aval** (SSA/opt/structureur/backend) : difftest 268/268 + transpile-diff 4/4.
- **Sound par construction (non lifté → abort, sans risque)** : **BCD** (aaa/aas/aam/aad/daa/das), arrondi **SSE non-défaut** (MXCSR ; l'x87 honore déjà RC), divers exotiques.
- **Axe futur (hors périmètre actuel 32-bit)** : **lift 64-bit** (REX/registres 64-bit) — Phase 8 ; le contrôle de débordement de quotient `div`/`idiv` 64-bit (chemin software `-m32`) y est rattaché.

### Axe 2 amorcé : différentiel Wine (couverture OS-API) ✅ FAIT (fondation)
- **2026-06-28 — L'axe 2 existe.** `bench/winediff.sh` + corpus `bench/winecorpus/*.c` : pour chaque
  programme, build PE (mingw) → exécution sous **Wine** (vérité terrain Win32) **vs** ARET transpilé
  natif → diff stdout (fins de ligne normalisées). Gated SKIP si wine/mingw absents (comme les autres
  harness). **Wine installé et fonctionnel** dans l'env (run d'un PE 32-bit OK). Donne la **métrique
  chiffrée** d'avancée OS-API demandée au §0.
- **Baseline : 5/7 programmes** identiques à Wine. ✅ **printf** (largeurs/flags/précision/l/ll/h/hh,
  %x/%o/%e/%g/%%), **malloc/calloc/realloc/free** (+ liste chaînée), **qsort/bsearch**, **math**
  (sqrt/pow/floor/ceil/sin/cos/exp/log/fmod/hypot/modf/ldexp), **fichier** (fopen/fprintf/fputs/fwrite/
  fgets/fseek/fgetc/remove) — tous **bit-identiques au vrai Windows**. Excellent socle HLE.
- **2 divergences réelles trouvées d'emblée** (c'est le but de l'axe 2 — « tirer la couverture API ») :
  1. **`sscanf`** → ARET **abort sound** sur `movsd [edi],[esi]` (instruction de **chaîne** MOVS, pas
     la SSE) non liftée. = le gap **rep/string** du backlog, exhibé par un vrai programme. À lifter.
  2. **`snprintf`** renvoie **4** au lieu de **6** : le shim rend la longueur *tronquée* au lieu de la
     longueur *qui aurait été écrite* (sémantique C99 que le binaire mingw attend). Bug de shim HLE.
- *Note* : winediff est une **métrique de couverture** (diagnostique), pas une porte dure comme
  difftest/transpile (qui restent à 100 %). Elle monte au fur et à mesure que le HLE se complète.

### Axe 2 : instructions de chaîne simples liftées → winediff 7/7 ✅ FAIT
- **2026-06-28 — Les 2 trouvailles de l'axe 2 corrigées ; corpus 7/7.**
- **`snprintf`/`vsnprintf`** : renvoyaient la longueur *tronquée* ; corrigés pour rendre la longueur
  C99 *qui aurait été écrite* (format dans un tampon de mesure puis copie tronquée). 5/7 → 6/7.
- **Instructions de chaîne simples (non-rep)** : `movs`/`stos`/`lods`/`scas`/`cmps` (b/w/d/q) n'étaient
  pas liftées (le `movsd` de chaîne partage le mnémonique avec le `movsd` SSE → abort sound).
  Ajoutées : opèrent sur un élément à `[esi]`/`[edi]` et avancent le(s) pointeur(s), **désambiguïsées
  par `is_string_instruction()`. `scas`/`cmps` posent les drapeaux** (via `sub_flags`). `cld` → no-op
  (DF=0 = la direction *avant* assumée, comme le `rep movs` existant) ; `std`/DF=1 et `rep scas/cmps/
  lods` restent non modélisés → **abort sound** (jamais d'avance arrière silencieuse). `sscanf` (qui
  utilise un `movsd` interne) matche maintenant Wine → **winediff 7/7**.
- *Validation* : end-to-end via winediff (sscanf = Wine) + régression. Pas dans cpudiff (les ops de
  chaîne à double pointeur esi+edi demanderaient un setup mémoire spécial dans le harness
  per-instruction ; couvertes end-to-end). **Régression : cargo vert, cpudiff vert, difftest 268/268,
  transpile-diff 4/4 (hash inchangé).**
- **Bilan axe 2** : socle solide (printf/malloc/qsort/math/fichier/chaînes/scan **= vrai Windows**), et
  la boucle « différentiel Wine → trouve un gap → corrige → métrique monte » **fonctionne**. Prochaines
  cibles quand on élargira le corpus : plus d'API Win32 (handles, temps, env, registre…), `rep` scas/
  cmps, `std`/DF arrière (modéliser DF).

### Axe 2 : corpus élargi 7→13, méthode + 4 shims corrigés ✅ FAIT
- **2026-06-28 — Méthode d'élargissement du corpus axe-2** (documentée pour la suite) : chaque
  programme doit être (1) **déterministe** (bannir time/rand-sans-srand/PID/adresses/chemins/locale/
  mémoire non-init), (2) **cibler une surface API non couverte** (par catégorie), (3) petit + résultats
  imprimés, (4) via des **imports dynamiques** (qu'ARET intercepte). Boucle : ajouter un lot → winediff
  → les `DIFF`/`FAIL` pointent les shims manquants → corriger → la métrique monte.
- **+6 programmes** (ctype, plus de string, wide-char/MBCS, rand déterministe, Win32 heap/lstr/wsprintf,
  Win32 fichier). D'emblée **9/13**, révélant **4 gaps réels** — tous corrigés :
  1. **`rand`/`srand`** utilisaient le rand() glibc → séquence différente. Remplacés par le **LCG exact
     msvcrt** (`seed=seed*214013+2531011; (seed>>16)&0x7fff`) → `srand(1);rand()=41` comme Windows.
  2. **`MultiByteToWideChar`/`WideCharToMultiByte`** étaient des stubs `return 0` ; implémentés (CP_ACP
     = Latin-1, octet↔WCHAR, gestion srclen<0 et longueur 0 = mesure). **`wcscpy`/`wcscat`/`wcscmp`**
     ajoutés (wchar_t Windows = 16-bit).
  3. **`GetFileSize`** non implémenté → `fstat` du handle (= fd dans le modèle).
  4. **`wsprintfA`** (USER32) non implémenté → routé vers le moteur de format (comme sprintf).
- **Résultat : winediff 13/13.** Régression : cargo vert, **difftest 268/268, transpile-diff 4/4 (hash
  inchangé)**. *Cibles futures* : plus de Win32 (temps/env/registre/handles avancés), `rep scas/cmps`,
  DF arrière. La boucle d'élargissement est rodée et reproductible.

### Axe 2 : corpus 13→19, inventaire des manques + 64-bit edx:eax shims ✅ FAIT
- **2026-06-28 — « Lister ce qui manque et élargir ».** +6 programmes (stdio binaire/seek, stdlib
  div/strtoll/atexit, math étendu, temps figé, **Interlocked** Win32, env Win32) ; `TZ=UTC` figé dans
  le harness pour le déterminisme des conversions de date. La sortie de winediff = **l'inventaire des
  manques** (`unimplemented import called: X`). D'emblée 15/19 ; passes notables : **Interlocked\*** et
  tout le **math étendu** (atan2/asin/acos/log10/sinh/round/cbrt/frexp/fmin…) **= vrai Windows**.
- **Manques trouvés et comblés** → **19/19** :
  - **`rewind`**, **`asctime`** (formaté à la main : msvcrt zéro-pad le jour `09`, glibc espace ` 9`),
    **`ExpandEnvironmentStringsA`** (substitution `%VAR%`).
  - **`atexit`** : mingw lie `atexit` en statique mais enregistre via l'**import `_onexit`** → `aret_onexit`
    enregistre désormais le callback ; un handler hôte `atexit` unique les rejoue via `aret_call` (LIFO).
  - **`div`/`ldiv`/`strtoll`/`strtoull`** (+ noms msvcrt `_strtoi64`/`_strtoui64`) : retour **64-bit dans
    la paire `edx:eax`** (un `long long`, ou `div_t`/`ldiv_t` 8 octets). Cause racine : le builder
    déclarait **tous** les shims `uint32_t` → le site d'appel **droppait edx**. Fix général :
    `import_returns_u64` (builder) déclare un ensemble curé de shims `uint64_t` (stub faible **et**
    prototype) → le split `edx:eax` déjà présent dans le lift d'appel capte les deux moitiés. Débloque
    toute la classe (div/ldiv/strtoll/strtoull/_strtoi64/_atoi64…).
- **Vérifié** : winediff **19/19** ; régression **cargo vert, cpudiff vert, difftest 268/268,
  transpile-diff 4/4 (hash inchangé)** — le builder ne change rien pour un binaire sans import à retour
  64-bit (opt-in). *Limite notée* : un `atexit` purement statique (sans `_onexit`) demanderait de
  rejouer le flux de sortie CRT (hors périmètre). La boucle d'élargissement est rodée : corpus = tests
  permanents **et** révélateurs de gaps.

### Axe 2 : corpus 19→24, fichiers/wildcards + dossiers + TTY ✅ FAIT
- **2026-06-28 — 3 catégories prioritaires (suggestion Gemini, alignée backlog).** +5 programmes
  (FindFirstFile+wildcard, stress rep/string, console/TTY, dossiers, chemins). Inventaire winediff →
  20/24, gaps comblés → **24/24** :
  - **`FindFirstFileA`/`FindNextFileA`/`FindClose`** : énumération avec wildcard, pontée sur
    opendir/readdir + **fnmatch insensible à la casse** (lowercasing manuel, FNM_CASEFOLD = extension
    GNU non portable). Remplit `WIN32_FIND_DATAA` (attributs, taille, `cFileName` @offset 44 ; le
    HANDLE = pointeur d'état d'itération). Cœur des outils CLI (`*.txt`).
  - **`CreateDirectoryA`/`RemoveDirectoryA`** → mkdir/rmdir (via `translate_path`).
  - **`GetFileType`** : ne renvoyait plus `FILE_TYPE_CHAR`(2) en dur → `fstat` du fd : FIFO/socket →
    PIPE(3), char → CHAR(2), sinon DISK(1). **Crucial** : un outil CLI sait enfin distinguer TTY vs
    redirection (winediff capture en pipe → le test attendait 3).
  - **`_splitpath`/`_makepath`** (chemins Windows : drive/dir/fname/ext) ; `_fullpath` déjà présent.
  - **`GetStdHandle`/`GetConsoleMode`** : déjà corrects (non-console sous redirection).
- *Note* : `rep_strings` (memcmp/memchr massifs) **passe sans** déclencher `rep scas/cmps` — mingw ne
  les émet pas ici ; le support `rep scas/cmps` + DF arrière reste au backlog (non bloquant).
- **Vérifié** : winediff **24/24** ; régression **cargo vert, difftest 268/268, transpile-diff 4/4
  (hash inchangé)**. Corpus axe-2 = **24 programmes**, tous = vrai Windows.

### Axe 2 : passage des arguments CLI (le vrai unlock des outils) ✅ FAIT
- **2026-06-28 — Avant strings.exe : les arguments de ligne de commande.** Diagnostic (sur le vrai
  `strings.exe` 32-bit fourni, transpilé en *transpile-only*) : ARET le **lift entièrement** (2837 fns,
  2653 liftées, **0 appel direct non résolu**) — le blocage est 100 % axe-2 (**77 imports non
  implémentés** + 178 partial-asm). Tête de liste : **`GetCommandLineA`/`W`** (les arguments) — qui
  bloque **tout** outil CLI, pas seulement strings.
- **Implémenté** : (1) `aret --mode transpile --run -- arg1 arg2…` **transmet** les arguments au binaire
  natif (`clap last=true` → `prog_args` → `Command::args`) ; (2) `GetCommandLineA`/**`GetCommandLineW`**
  reconstruisent la ligne de commande depuis le vrai argv (quoting des args à espaces — inverse de
  `CommandLineToArgv`) ; argc/argv passaient déjà par la frame `main` synthétique. (3) le harness
  winediff lit un fichier `NAME.args` optionnel (un arg par ligne) et le passe **à l'identique** à Wine
  et à ARET.
- **Vérifié** : nouveau `argv_echo` (argc + argv[1..] + somme numérique + `GetCommandLineA`) = **bit-
  identique à Wine** ; winediff **25/25**. Régression : cargo vert, **difftest 268/268, transpile-diff
  4/4 (hash inchangé)**. **Unlock général** : tout outil CLI (cat/strings/makecab…) reçoit enfin ses
  arguments. Prochaine étape vers strings.exe : **mapping mémoire** (`CreateFileMapping`/`MapViewOfFile`).

### Axe 2 : mapping mémoire de fichiers (vers strings.exe) ✅ FAIT
- **2026-06-28 — `CreateFileMapping`/`MapViewOfFile`/`UnmapViewOfFile`/`FlushViewOfFile`** (+ `…W`),
  pontés sur **mmap** hôte : le guest tourne en pointeurs natifs plats, donc l'adresse mmap est
  directement utilisable. Un HANDLE de mapping est un **pointeur tas** (pas un fd) → `CloseHandle`
  distingue (registre des mappings) ; les vues base→len sont suivies pour `munmap`. Lecture (cas
  strings.exe) **et** écriture (PAGE_READWRITE → `ftruncate` + `MAP_SHARED` + writeback) gérées.
- **Piège WASM corrigé** : WASI n'a pas de vrai `mmap` → section gardée `#ifndef __wasm__` (comme
  setjmp/longjmp), avec un `CloseHandle` simple côté wasm. Détecté par le test `wasm_target` (qui
  compile tout le runtime en wasm32-wasi).
- **Vérifié** : `win32_mmap` (checksum d'un fichier mappé en lecture + écriture-au-travers relue) =
  **bit-identique à Wine** ; winediff **26/26**. Régression : cargo **98/0**, **difftest 268/268,
  transpile-diff 4/4 (hash inchangé)**, **wasm OK**, cpudiff OK.

### Axe 2 (P1) : lot wide I/O + console ✅ FAIT
- **2026-06-28 — Décision de principe : P1 (boucle synthétique vérifiée) plutôt que « stubber 76 d'un
  coup pour voir si strings.exe tourne »** — ce dernier viole « pas de rustine par binaire » et « jamais
  de sortie fausse présentée comme vraie » (tourner ≠ prouvé correct). Chaque shim est vérifié bit-à-bit
  contre Wine *avant* d'être cru.
- **Lot wide I/O + console** (+3 programmes). Inventaire → comblé :
  - **`CreateFileW`/`GetFileAttributesW`/`DeleteFileW`** : helper `aret_w2n` (UTF-16→narrow) + cœurs
    partagés `aret_open_named`/`aret_attr_named` refactorés depuis les variantes A.
  - **`FindFirstFileW`/`FindNextFileW`** : `aret_find_t` gagne un flag `wide`, `aret_fill_find` écrit
    `cFileName` en WCHAR (layout `WIN32_FIND_DATAW`) ; cœur `aret_find_first` partagé A/W. **`_wcsdup`**.
  - **`GetConsoleCP`/`GetConsoleOutputCP`** : **0** sans console (sortie redirigée, comme sous le
    harness), code page réel sinon — *valeur calquée sur Wine* (qui renvoie 0 en pipe). **`WriteConsoleW`**
    échoue sur un handle redirigé (le programme bascule alors sur WriteFile) → pas de double sortie.
- **Vérifié** : `win32_filew`/`win32_findw`/`console_cp` = **bit-identiques à Wine** ; winediff **29/29**.
  Régression : cargo **98/0** (wasm OK), **difftest 268/268, transpile-diff 4/4 (hash inchangé)**.

### Axe 2 (P1) : lot locale/codepage + Tls + stubs de démarrage ✅ FAIT
- **2026-06-28 — +2 programmes** (`tls_test`, `locale_cp`), `LC_ALL=C` figé dans le harness (avec TZ=UTC)
  pour le déterminisme des codepages. Valeurs **calquées sur Wine** (sondé : `acp=1252 oem=437`,
  GetCPInfo MaxCharSize=1, GetStringTypeW classe A/a/1/!/space, LCMapStringW `hello`→`HELLO`, Tls
  round-trip, Encode/DecodePointer round-trip).
- **Comblé** : **Tls** (étaient des stubs `TlsAlloc=0` → réels, table de slots, +`TlsFree`) ;
  **`EncodePointer`/`DecodePointer`** (identité — la valeur est opaque, round-trip correct) ;
  **`GetCPInfo`**, **`IsValidCodePage`**, **`GetStringTypeW`** (bits C1_* via ctype, sous-ensemble
  ASCII), **`LCMapStringW`** (UPPER/LOWER). `GetACP`/`GetOEMCP` étaient déjà bons.
- **Vérifié** : `tls_test`/`locale_cp` = **bit-identiques à Wine** ; **winediff 31/31** (les 29
  précédents inchangés malgré `LC_ALL=C`). Régression : cargo **98/0** (wasm OK), **difftest 268/268,
  transpile-diff 4/4 (hash inchangé)**.

### Axe 2 (P1) : lot process/module ✅ FAIT
- **2026-06-28 — +1 programme `win32_proc`.** La plupart du heap/module existait déjà (HeapAlloc/Free/
  ReAlloc/Size, LocalAlloc/Free, GetModuleHandleA/W, GetProcAddress, GetProcessHeap — vérifié contre
  Wine). Manquaient, comblés :
  - **`GetCurrentProcess`/`GetCurrentThread`** : pseudo-handles constants -1 / -2 (calqués sur Wine).
  - **`GetStartupInfoW`** : zéro-remplit la STARTUPINFOW + `cb` (console sans customisation → dwFlags=0).
  - **`GetModuleFileNameW`** (variante wide), **`GetModuleHandleExW`** (pose `*phModule`, renvoie succès).
- *Choix de principe noté* : `GetProcAddress` **reste à 0** (« non trouvé ») — on ne peut pas distribuer
  de pointeurs appelables (nos shims ont l'ABI esp, pas l'ABI Win32). Renvoyer un faux pointeur qui
  planterait à l'appel serait une rustine ; **0 est honnête et sound** (le programme gère « absent »).
  Le test n'assert donc pas `GetProcAddress!=NULL` (on n'assert pas un comportement qu'on ne fait pas).
- **Vérifié** : `win32_proc` = **bit-identique à Wine** ; winediff **32/32**. Régression : cargo **98/0**
  (wasm OK), **difftest 268/268, transpile-diff 4/4 (hash inchangé)**. strings.exe : imports manquants
  encore en baisse.

### Axe 2 (P1) : lot fichier/format kernel ✅ FAIT
- **2026-06-28 — +1 programme `win32_fileops`.** Comblé : **`SetFilePointerEx`** (offset 64-bit par
  valeur lo@1/hi@2 → lseek), **`GetFileSizeEx`** (fstat → LARGE_INTEGER), **`GetFullPathNameA`**
  (résout contre le cwd, `*filePart` = dernier composant ; on teste le **basename**/filePart, stables,
  pas le préfixe cwd qui diffère par hôte). `SetFilePointer`/`FlushFileBuffers` existaient déjà.
- **`FormatMessageA` reporté** : sa gestion des inserts `%1` (tableau d'args) + le texte exact des
  messages système (locale-dépendant, ≠ strerror POSIX) demandent un travail dédié ; on ne le bâcle
  pas (principe). Noté au backlog. *(Mon test initial avait d'ailleurs un `va_list` malformé qui a fait
  planter Wine — bug de test, pas de signal réel.)*
- **Vérifié** : `win32_fileops` = **bit-identique à Wine** ; winediff **33/33**. Régression : cargo
  **98/0** (wasm OK), **difftest 268/268, transpile-diff 4/4 (hash inchangé)**.

### strings.exe (Sysinternals) — déblocage par run différentiel : cpuid/xgetbv + récupération de fonctions ✅ FAIT
- **2026-06-28 — diagnostic par *run différentiel*** (transpile+run de strings.exe comparé à Wine,
  itéré bloqueur par bloqueur ; principé car différentiel, pas « ça a l'air plausible »). Chaque
  bloqueur résolu sur le **général**, jamais par rustine spécifique au binaire :
  1. **`cpuid`** (instruction non modélisée) → handler lift renvoyant les 4 leaves via `__ix_cpuid`
     (cpuid hôte réel, `__get_cpuid_count`, repli wasm). *(déjà posé au lot précédent)*
  2. **`xgetbv`** (le CRT lit XCR0 après cpuid pour confirmer l'état AVX OS-activé) → handler lift
     `Mnemonic::Xgetbv` (ecx → edx:eax) + helper `__ix_xgetbv` (XGETBV hôte par octets bruts
     `0f 01 d0`, repli wasm = x87|SSE).
  3. **`InitializeCriticalSectionAndSpinCount`** (import manquant) → renvoyait 0 (FALSE) par le stub
     faible, ce qui faisait `__fastfail` (int 0x29) au CRT. Shim renvoyant **TRUE** (modèle mono-thread,
     pas de contention) ; +`InitializeCriticalSectionEx`. `int 0x29` n'est alors plus atteint.
  4. **Appel indirect vers fonction non récupérée `0x404aa4`** (thunk `ret` par défaut du
     Control-Flow-Guard, `call [__guard_check_icall_fptr]`) → **récupération générale** : récolte du
     *contenu* des slots d'appel/jmp indirect absolu (`call/jmp [disp32]`). Un `call [slot]` dont le
     slot contient un pointeur vers une section exécutable est la **preuve définitive** d'une entrée de
     fonction — contourne l'heuristique de prologue (un `ret` nu n'en est pas une). Slot IAT (pointe en
     données non-exec) naturellement filtré.
  5. **Appel indirect vers `0x4283b3`** (initialiseur CRT style `_initterm`, début `mov [mem],imm32`)
     → **récupération générale** : le scan des tables de pointeurs de fonctions **tolère désormais des
     trous NULL** (une liste `_initterm`/TLS-callback a légitimement des slots NULL/terminateur), avec
     un plafond de NULL consécutifs pour ne pas fusionner des régions sans rapport.
- **Régression de fond trouvée et corrigée (DCE)** : mon ajout antérieur du flag **PF** sur les shifts
  laissait un `__ix_pf` **mort** sur le résultat du décalage. Comme `__ix_pf` est un appel, la DCE le
  gardait → **deuxième usage** de l'opérande → la copy-propagation n'inlinait plus `x*M` dans le shift
  → la **recovery de division magique ne firait plus** (`x*M >> s` au lieu de `x / c`). Fix général :
  notion de **helper pur** (`__ix_pf`, `__ix_cpuid`, `__ix_xgetbv`, `__fp_*`, `__pi_*`, `__ps_*`,
  `__x87_*`) supprimable par la DCE quand mort — en **excluant** ce qui peut fauter (`__ix_*div*/*mod*`
  trappent #DE) ou écrire en mémoire (`__rep_*`, `aret_*`). `magicu`/`x / c` re-firé.
- **Harness magicdiv réparé** (cassé **avant** mes changements) : concaténait N sorties `--mode emit`
  portant chacune le préambule C → typedef `__fp32` redéfini (« conflicting types »). Préambule gardé
  **une seule fois**, fonctions ajoutées seules ensuite.
- **État strings.exe** : passe désormais **tout l'init CRT statique** (cpuid→xgetbv→critsec→_initterm).
  Prochain bloqueur **réel** isolé : `_EH_prolog` (helper SEH MSVC qui **réécrit le frame de
  l'appelant** — nouveau `ebp`/`esp` persistant après `ret`). Le modèle `__esp`-par-valeur d'ARET ne
  propage pas ces changements inter-frame → `[ebp±x]` faux dans l'appelant (déréférence `5`, crash).
  **Tâche dédiée suivante** (réécriture de frame inter-fonction ; ni cpuid ni récupération ne sont en
  cause). Documenté ici pour ne pas le perdre.
- **Vérifié** : régression **complète PASS** — cargo (tests OK), **difftest 268/268**, in-place 3/3,
  **magicdiv ALL 2^32 EQUIVALENT** (re-firé), SMT 11/11, recompilabilité gzip/ls/cat **100%**,
  **transpile-diff 4/4 (hash 4b0121f182554d40 inchangé)**, **winediff 33/33**, cpudiff OK. cpuid/xgetbv
  ne sont pas validables par cpudiff (le cpuid d'Unicorn ≠ cpuid hôte) ; validés par le run
  différentiel strings.exe + la régression.

### strings.exe — inline des helpers SEH à réécriture de frame (`_EH_prolog`) ✅ FAIT
- **2026-06-28 — cause générale du crash post-init isolée** : `_EH_prolog` (prologue SEH MSVC,
  statiquement lié, ~4 occurrences). Ce helper **réécrit le frame de l'appelant** : il consomme les
  deux `push` de l'appelant (taille de frame + scopetable), sauve l'`ebp` appelant dans un slot,
  repointe `ebp` **au-dessus** de l'adresse de retour dans le frame appelant (`lea ebp,[esp+K]`, K>0),
  alloue les locals, sauve les registres, puis `ret`. L'appelant utilise ce nouvel `ebp`/`esp` *après*
  l'appel. Or ARET passe `esp` **par valeur** et modélise le delta d'un appel statiquement → la
  réécriture `ebp`/`esp` du callee ne remontait pas → `[ebp±x]` faux dans l'appelant (un pointeur d'arg
  lu comme une petite constante de frame, déréférencé → crash).
- **Solution générale (pas une rustine)** : **inliner le corps du helper au site d'appel** (mode
  shared-stack/transpile uniquement). Tout devient une seule fonction → `esp`/`ebp`/mémoire circulent
  en SSA exactement comme sur le matériel. Recette : émuler le `push` de l'adresse de retour du `call`,
  inliner le corps lifté (jusqu'avant le `ret` terminal), émuler le `pop` du `ret` (`esp += ptr`) ;
  chute vers l'instruction suivante. Le `push [ebp-0x8]; ret` final de `_EH_prolog` (retour via
  l'adresse relogée) s'élide exactement (push+ret = esp net 0).
- **Détection générale** : idiome de réécriture `lea ebp,[esp+K]` (K>0) dans une routine courte, sans
  branche, finissant par un `ret` proche unique. Le code normal pointe `ebp` ≤ `esp` (`mov ebp,esp`) ;
  seul un prologue à réécriture de frame le pointe *au-dessus* de sa propre adresse de retour. C'est la
  routine de l'ABI MSVC, identique d'un binaire à l'autre. Garde : si une instruction du corps ne se
  lifte pas pleinement (`Stmt::Asm`), on **n'inline pas** (refus de deviner). `fs:[0]` (chaîne SEH) est
  déjà modélisé via `__aret_fs()` en transpile, donc `_EH_prolog` se lifte entièrement.
- **Effet vérifié sur strings.exe** : le crash de l'indirection `ebp` (refcount locale dans
  `sub_42b776`) **disparaît** ; l'exécution progresse jusqu'au cœur du traitement des **tables de
  locale** du CRT (prochain bloqueur : déréférence d'un pointeur source NULL dans une copie de table
  NLS — chaîne d'init `setlocale` du CRT statique, en cours d'analyse).
- **Régression complète PASS** (l'inline est gaté au mode transpile : `--mode emit`/`verify` inchangés
  par construction) : difftest **268/268**, in-place 3/3, magicdiv ALL 2³², SMT 11/11, recompilabilité
  gzip/ls/cat **100%**, **transpile-diff 4/4 (hash 4b0121f182554d40 inchangé)**, **winediff 33/33**.

### strings.exe — threading d'`ebp` (helpers sans frame partageant le frame parent) ✅ FAIT
- **2026-06-28 — 2ᵉ cause générale du même symptôme** : après l'inline de `_EH_prolog`, le crash s'est
  déplacé vers `sub_42b7bc`, un **helper sans prologue** appelé par `sub_42b776` qui lit `[ebp+0x10]`
  en **héritant du `ebp` de l'appelant** (posé par `_EH_prolog`). Or ARET initialisait le `ebp` d'entrée
  de chaque fonction à son **propre** esp d'entrée (`__esp`) → `[ebp+0x10]` lisait au mauvais endroit
  → pointeur NULL → crash. Ce sont les funclets EH de MSVC (sous-routines partageant le frame parent).
- **Fix général (ABI)** : `ebp` est **callee-saved** (c'est le frame pointer) → le **threader** comme
  4ᵉ registre passé aux appels internes en mode shared-stack (après eax/ecx/edx). Un helper sans frame
  reçoit ainsi le `ebp` de l'appelant ; une fonction à prologue normal l'écrase (inchangé). Touche :
  `ssa::to_ssa` (ebp = reg_param au lieu de frame_base en shared-stack), `internal_call_args`/
  `internal_tailcall_args` (+ebp), prototype forward (`structured.rs`, 5 args), typedef `aret_fn` +
  trampolines + `aret_call` (5 args ABI), sites runtime de `aret_call` (+0).
- **Effet vérifié** : tout l'init locale du CRT **passe** désormais ; le crash saute très loin en aval,
  dans un **clonage de catégorie de locale** (`sub_4314c9` : `malloc(0x220)` + copie de 0x88 dwords) —
  prochain bloqueur, à analyser (pointeur NULL/forme de données NLS — `malloc` qui rendrait 0, ou champ
  de structure locale non conforme à l'attente du CRT).
- **Régression complète PASS** : difftest **268/268**, in-place 3/3, magicdiv ALL 2³², SMT 11/11,
  recompilabilité **100%**, **transpile-diff 4/4 (hash 4b0121f182554d40 inchangé — empreinte
  comportementale ; les programmes du corpus n'ont pas de helper sans frame)**, **winediff 33/33**,
  cargo (wasm inclus) OK.

### strings.exe — état après threading d'ebp : blocage profond dans le clonage de locale
- **2026-06-28 — où on en est** : strings.exe exécute désormais **tout** le démarrage du CRT statique
  (cpuid → xgetbv → critical sections → `_initterm` → init locale, refcount, allocation du locinfo
  par-thread via `sub_42bf72`+`sub_42bb2b`). Le crash restant est dans `sub_4314c9`
  (`__updatetlocinfoEx`-style), au `memcpy` qui clone une catégorie de locale (`rep movsd` d'origine,
  0x88 dwords depuis `[locinfo+0x48]`).
- **Diagnostic précis (gdb)** : à l'entrée de `sub_4314c9`, `arg2=locinfo` est **valide** et
  `locinfo[0x48]=0x4562f0` (correct). Mais au `memcpy`, les arguments réels sont
  `memcpy(dst=0x889d690 [malloc OK], src=0x0, n=0x4562f0)` — **src et count corrompus** : `n` reçoit la
  valeur qui devrait être la *source* (0x4562f0), et `src`=0. Les slots de pile de l'appel
  (`[ebp-0x1c4]`, `[ebp-0x188]`) sont écrasés.
- **Ce n'est PAS un bug général de `rep movs`** : le test `rep_strings` du corpus winediff passe
  (33/33), donc le lift de `rep movsd` est validé. La corruption vient d'une **écriture antérieure**
  hors-limites dans ce chemin précis (frame corrompu), pas d'une erreur de traduction générale.
- **Prochain pas (continuation)** : tracer quelle écriture antérieure dans `sub_4314c9` (ou un callee :
  `sub_4315dd`, `sub_43124d`, `sub_42a0be`) corrompt `[ebp-0x1c4]`/`[ebp-0x188]`. Hypothèses : (a) un
  store dont l'adresse est mal calculée suite à un reste de modèle esp/ebp dans une de ces fonctions ;
  (b) une lecture `[locinfo+0x48]` OOB (0x4562f0+0x220 dépasse la fin de `.data` à 0x456c00) si le
  layout mémoire d'ARET laisse un trou — à vérifier (watchpoint gdb sur `[ebp-0x188]`). C'est du
  débogage mono-binaire profond ; à n'engager que si la cause s'avère **générale**.
- **Acquis de la session (5 fixes généraux, tous commités/poussés, régression complète verte)** :
  cpuid/xgetbv ; récupération (slots d'appel indirect absolu + tables NULL-tolérantes) ; DCE des
  helpers purs (+ harness magicdiv) ; inline `_EH_prolog` ; threading d'`ebp`. Le transpileur est
  **généralement** plus correct (instructions, récupération, funclets EH/sans-frame), bien au-delà de
  strings.exe.

### strings.exe — bug GÉNÉRAL trouvé : args libc 64-bit non rétrécis (memcpy) + `bt [mem],imm` ✅ FAIT
- **2026-06-28 — la cause du crash « clonage locale » était GÉNÉRALE** (pas une rustine). En recompilant
  le C généré en `-O0 -g` (sans inline LLVM) pour un mapping propre, le crash s'est révélé à
  `chunk_11.c:384` = `memcpy(v52, v113, 0x88*4)`. Le désassemblage `-O0` montre **6 push** pour 3
  arguments : nos opérandes sont `uint64_t` et le chunk n'inclut **pas** `<string.h>`, donc sans
  prototype le compilateur passe chaque `uint64_t` en **64 bits (2 mots)** sur cible 32-bit. `memcpy`
  lit alors `dst=v52.lo` (OK), `src=v52.**hi**=0` (faux), `n=v113.lo=0x4562f0` (faux) → copie folle
  depuis NULL → crash. **Tout appel libc émis (memcpy issu de `rep movs`, etc.) était cassé** dès que
  ses arguments étaient des `uint64_t` 32-bit-larges sans prototype.
- **Fix général** (`emit::expr_c`, mode shared-stack) : pour un appel `Named` reconnu comme fonction
  libc (`libc_arity` Some), **caster chaque argument en `(uint32_t)`** — un seul mot, ABI i386 correcte
  (pointeurs/size_t/int tous 32-bit). `memcpy`/`memset`/`strncpy`/`read`/`write`/… réparés d'un coup.
- **`bt [mem], imm` non modélisé** (révélé juste après) : le lifter ne gérait `bt`/`bts`/`btr`/`btc`
  que pour un destinataire **registre**. La forme **mémoire avec index immédiat** est sûre (le bit
  reste dans l'opérande, `idx mod width` — pas d'adressage bit-string) → gate élargi à
  `op0=Register || op1=Immediate8`. (La forme mémoire avec index **registre** reste exclue : elle
  adresse une chaîne de bits, non modélisée.)
- **Effet sur strings.exe** : passe désormais **tout** le clonage de locale ; prochain bloqueur =
  `int 0x29` (`__fastfail`) atteint via un **échec de check du cookie de pile** (`__security_check_cookie`,
  `sub_403d2f`) : le slot cookie `[ebp-4]` est écrasé entre son store (`cookie^ebp`) et sa
  vérification (lu == `ebp` → `ecx=0` ≠ cookie). Le C généré du cookie est correct → corruption de pile
  en amont, à pister (prochaine étape).
- **Régression complète PASS** : difftest **268/268**, magicdiv 2³², SMT 11/11, recompilabilité 100%,
  **transpile-diff 4/4 (hash 4b0121f182554d40 inchangé)**, **winediff 33/33** (le cast libc valide tout
  le corpus axe 2), cargo (wasm) OK.

### strings.exe — `_chkstk`/`_alloca` (esp inter-frame) + `stmxcsr`/`ldmxcsr` ✅ FAIT
- **2026-06-28 — la corruption du cookie était GÉNÉRALE** (3ᵉ helper à réécriture d'esp). Watchpoint sur
  le slot cookie : `sub_42f381` le stocke (`cookie^ebp`), puis un **`memset(dest, 0, 512)`** appelé par
  `sub_42f381` lui-même l'écrase. Cause : `sub_42f381` fait un **`_alloca`** via `call 0x4426c0`
  (`_alloca_probe16` → tail-jmp `0x403d40` = `_chkstk`), qui **abaisse `esp` de `eax` octets** et
  relocalise l'adresse de retour (comme `_EH_prolog`). ARET ne propageant pas ce changement d'esp,
  `esi = esp` prenait l'**ancien** esp (haut dans le frame) → le `memset` du buffer débordait sur le
  cookie/retour → check `__security_check_cookie` (`sub_403d2f`) en échec → `int 0x29` (`__fastfail`).
- **Fix général** (`ir::build`) : détecter la famille `_chkstk`/`_alloca_probe` par le marqueur unique
  **`xchg esp, eax`** (scan borné suivant le tail-`jmp` des variantes alignées) et modéliser l'appel
  comme **`esp = esp - eax`** (le buffer est au nouvel esp ; on n'a pas besoin du probing de pages — la
  pile native est un grand mapping). Helper ABI MSVC standard (tout binaire avec `_alloca`/gros frame).
- **`stmxcsr`/`ldmxcsr` non modélisés** (révélés ensuite) : registre de contrôle SSE. `ldmxcsr` → Nop
  (on ne modélise pas l'état d'arrondi/exceptions SSE — défaut hôte, comme `fldcw` pour le x87) ;
  `stmxcsr [m]` → écrit le défaut **0x1f80** (exceptions masquées, arrondi au plus proche), ce que
  l'init FP du CRT relit.
- **Effet** : strings.exe **dépasse tout l'init locale ET la corruption de cookie** ; prochain bloqueur
  = segfault dans `sub_42697f` (deref `eax=0x351`) via une autre phase du démarrage CRT (`sub_404019`
  → `sub_425e97`), + import `InitializeSListHead` (stub non-fatal). À analyser.
- **Régression complète PASS** : difftest **268/268**, magicdiv 2³², SMT 11/11, recompilabilité 100%,
  **transpile-diff 4/4 (hash 4b0121f182554d40 inchangé)**, **winediff 33/33**, cargo (wasm) OK.

### strings.exe — après _chkstk : import `InitializeSListHead` + blocage dans la machinerie C++/CRT thread-state
- **2026-06-28** : `InitializeSListHead` implémenté (zéro le SLIST_HEADER de 16 octets ; le stub no-op
  laissait le header non initialisé). + pop stdcall (1 arg).
- **Blocage suivant** (segfault `sub_42697f`, deref `[v16+0x350]` avec `v16=1`) : `v16 = sub_42beb5()`
  (= `__updatetlocinfo`, getter du locinfo par-thread) retourne **1** au lieu d'un pointeur locinfo.
  La chaîne descend dans `sub_4296a0` = **machinerie `__crt_state_management` à dispatch indirect
  CFG-gardé** (`call [0x445214]` = `__guard_check_icall_fptr`, puis `call esi` indirect vers un
  pointeur de fonction issu de `sub_4292f2`). C'est le `_Init_thread_*`/thread-safe-init C++ du MSVCRT.
  Diagnostic : à ce point, **une seule `TlsAlloc`** a tourné et **aucun `TlsSetValue`** — l'index TLS
  locale `[0x456210]=1` n'est pas encore peuplé ; sur Windows réel `__updatetlocinfo` renverrait le
  locinfo **global initial**, là où notre état rend 1. **Complétude HLE/CRT très spécifique** (C++
  thread-state + dispatch indirect), pas un bug général de traduction — non poursuivi (principe).
- **Bilan strings.exe** : de « crash sur la 1ʳᵉ instruction `cpuid` » à « traverse cpuid/xgetbv,
  critical sections, `_initterm`, init du cookie de sécurité, **tout** le clonage/refcount de locale,
  `_chkstk`/`_alloca`, FP env (`stmxcsr`) » — bute désormais dans la machinerie C++/CRT de thread-state.
- **Régression complète PASS** (incl. winediff 33/33, transpile 4/4 hash inchangé).

#### Récap des fixes GÉNÉRAUX de la session (tous commités/poussés, régression verte à chaque pas)
1. `cpuid`/`xgetbv` (instructions). 2. Récupération : slots d'appel indirect absolu + tables de
pointeurs NULL-tolérantes. 3. DCE des helpers purs (restaure la division magique) + harness magicdiv.
4. Inline `_EH_prolog` (helper SEH à réécriture de frame). 5. Threading d'`ebp` (funclets EH sans
frame). 6. **Cast des args libc en uint32_t** (memcpy 64-bit cassé sans prototype — bug majeur).
7. `bt [mem],imm`. 8. `_chkstk`/`_alloca` (esp inter-frame). 9. `stmxcsr`/`ldmxcsr`. 10.
`InitializeSListHead`. → Le transpileur est **généralement** plus correct (instructions, récupération,
helpers ABI MSVC EH/alloca, appels libc), bénéfique à tout binaire PE.

### strings.exe — bug GÉNÉRAL : tail-`jmp [import]` passait le mauvais esp (TLS/Fls/encoded-ptr cassés) ✅ FAIT
- **2026-06-28 — la machinerie thread-state MSVCRT était bloquée par un vrai bug général.** Trace TLS
  instrumentée : `aret_TlsGetValue` était appelé avec un **index garbage** (`0x44CB48` = une adresse
  empilée), retournant 0/garbage, et `TlsSetValue(0,1)` corrompait le slot 0 → `__updatetlocinfo`
  rendait 1 au lieu du locinfo.
- **Cause** : les wrappers `__acrt_TlsGetValue`/Fls du MSVCRT finissent par un **tail-`jmp [IAT]`**
  (`pop esi; pop ebp; jmp [TlsGetValue]`). Un `jmp [import]` ne pousse pas d'adresse de retour : celle
  de l'appelant reste à `[esp]`, donc les arguments de l'import commencent à `[esp+4]`. Mais
  `name_calls_in_expr` passait `esp` (correct pour un `call [import]` normal — pas de retaddr poussé
  pour un import — mais **faux pour un tail-`jmp`**) → l'import lisait l'adresse de retour comme 1ᵉʳ
  argument. Les tail-calls **directs** (`jmp import`) avaient déjà la compensation +4 ; la forme
  **indirecte** (`jmp [IAT]`) ne l'avait pas.
- **Fix général** (`ir::build::name_calls`) : thread d'un flag `is_tail` ; en position de tail-call
  (`Stmt::Return(Call)`), un shim d'import reçoit **`esp+4`** (l'adresse de retour de l'appelant est
  encore sur la pile). Couvre `jmp [IAT]` ET `jmp reg` (reg chargé depuis l'IAT).
- **Effet vérifié** : `TlsGetValue(1)` retourne désormais le **locinfo valide** (pointeur heap) ; tout
  l'état TLS/Fls/encoded-pointer du MSVCRT fonctionne. strings.exe dépasse la machinerie thread-state
  C++ ; prochains bloqueurs = import `GetEnvironmentStringsW` + instruction **AVX/VEX** `vpxor`
  (ARET modélise `pxor` SSE mais pas la forme VEX 3-opérandes ; chantier AVX à part).
- **Régression complète PASS** : difftest **268/268**, magicdiv 2³², SMT 11/11, recompilabilité 100%,
  **transpile-diff 4/4 (hash 4b0121f182554d40 inchangé)**, **winediff 33/33**, cargo (wasm) OK.

### strings.exe — SORTIE CORRECTE ✅ (SSE2 string ops + masquage CPUID AVX/SSE4.2)
- **2026-06-28 — strings.exe imprime `hello`/`world`/`tiny`, identique à Wine.** Après le fix
  tail-jmp, le `main()` tourne et atteint les routines de chaîne SSE optimisées du CRT.
- **Masquage CPUID (général, sain)** : `__ix_cpuid` masque désormais les bits **SSE4.1/SSE4.2 et
  AVX/AVX2/AVX-512** (+ FMA/F16C/OSXSAVE). Les dispatchers de features (le `__isa_available` du CRT,
  etc.) choisissent alors les chemins **SSE2**, que ARET lifte exactement — les ops SSE4.2
  (`pcmpistri`) et VEX/AVX (`vpxor`, `vmovdqu`) ne sont pas modélisées. Sain : un CPU SSE2-seul est une
  config valide, les chemins SSE2 calculent à l'identique. Général : bénéficie à tout binaire à
  fallback SSE2.
- **Ops SSE2 ajoutées** (révélées par les scanners de chaîne SSE2 du CRT) : `pcmpeqb`/`pcmpeqw`/
  `pcmpgtb` (compare octets/words, helpers `__pi_eq8`/`eq16`/`gt8`), **`pmovmskb`** (masque de bits de
  signe des 16 octets → registre GP, `__pi_mskb`), `pshuflw`/`pshufhw` (shuffle des words bas/haut,
  `__pi_shufw` — idiome de broadcast memchr/memset).
- **État** : la **sortie est correcte et bit-identique à Wine** pour l'extraction de chaînes. Reste un
  **segfault dans le cleanup post-sortie** (`sub_42e9a7` ← `sub_421d8b`, après les écritures registry
  EULA ; imports version/registry stubés → bannière `(null)`). Le cœur fonctionne ; le chemin de sortie
  est à finir.
- **Régression complète PASS** : difftest **268/268**, magicdiv 2³², SMT 11/11, recompilabilité 100%,
  **transpile-diff 4/4 (hash 4b0121f182554d40 inchangé)**, **winediff 33/33**, **cpudiff OK**.

### strings.exe — EXIT PROPRE ✅ (PEB.ProcessParameters) — milestone cœur ATTEINT
- **2026-06-28 — strings.exe tourne intégralement et exit 0**, extraction de chaînes **bit-identique à
  Wine** (`hello`/`world`/`tiny`…). Le segfault de cleanup venait du TEB/PEB synthétique : le CRT lit
  `[fs:0x30]`=PEB puis `[PEB+0x10]`=ProcessParameters, qui était **NULL** → deref `[0+8]`.
- **Fix (complétude TEB/PEB, général)** : bloc `RTL_USER_PROCESS_PARAMETERS` zéro-rempli (Length,
  Flags=NORMALIZED) pointé depuis `[PEB+0x10]`. Tout binaire lisant les paramètres de process en
  bénéficie.
- **Seule différence restante vs Wine = la bannière de version** (`(null) v(null) - (null)` au lieu de
  `Strings v2.54 - ...`) : strings.exe lit sa propre ressource VERSIONINFO via
  `GetFileVersionInfoSizeA`/`GetFileVersionInfoA`/`VerQueryValueA` (non implémentés → `%s` sur NULL).
  **Cosmétique** — l'extraction de chaînes (la fonction du programme) est exacte. Implémenter le
  parsing de la ressource VS_VERSIONINFO du PE rendrait la sortie totalement identique (feature HLE
  générale, à faire).
- **Régression complète PASS** : difftest 268/268, magicdiv 2³², SMT 11/11, recompilabilité 100%,
  transpile 4/4 (hash inchangé), winediff 33/33, cpudiff OK.

#### 🏁 MILESTONE : un vrai binaire MSVC static-CRT C++ (Sysinternals strings.exe) transpilé en ELF
#### Linux natif, tourne sans Wine, extraction de chaînes bit-identique à Wine, exit 0.
Débloqué cette session par ~16 fixes **généraux** (instructions cpuid/xgetbv/bt/stmxcsr/SSE2-string ;
récupération de fonctions ; DCE ; inline `_EH_prolog` + `_chkstk` ; threading `ebp` ; cast args libc ;
tail-jmp esp+4 ; masquage CPUID AVX/SSE4.2 ; TEB/PEB) — chacun bénéficiant à toute la classe des
binaires MSVC, pas seulement strings.exe.

### strings.exe — SORTIE 100% BIT-IDENTIQUE À WINE ✅✅✅ (version-info)
- **2026-06-28 — strings.exe produit une sortie totalement identique à Wine, bannière comprise :**
  ```
  Strings v2.54 - Search for ANSI and Unicode strings in binary images.
  Copyright (C) 1999-2021 Mark Russinovich
  Sysinternals - www.sysinternals.com
  hello / world / tiny
  ```
- **APIs version-info implémentées** (`GetFileVersionInfoSizeA`/`GetFileVersionInfoA`/`VerQueryValueA`) :
  - Bornes de l'image mappée exportées (`aret_image_lo`/`hi` dans `aret_layout.c`) pour scanner en
    sécurité.
  - `aret_find_versioninfo` localise le `VS_VERSIONINFO` du PE par la signature `VS_FIXEDFILEINFO`
    `0xFEEF04BD` (offset 0x28 du début), valide la clé `VS_VERSION_INFO`.
  - `VerQueryValueA` parse l'arbre `VS_VERSIONINFO` (root → StringFileInfo/VarFileInfo → lang → clé).
    Comparaison de clés **insensible à la casse** (le bloc langue est stocké `040904b0` mais interrogé
    `040904B0`).
  - `GetFileVersionInfoA` (variante ANSI) **rétrécit les valeurs de chaîne UTF-16→ANSI in place** —
    sans quoi `printf("%s")` ne lit que le 1ᵉʳ caractère (`S` au lieu de `Strings`).
  - Général : tout programme lisant sa propre ressource VERSIONINFO en bénéficie.
- **Régression complète PASS** : difftest 268/268, magicdiv 2³², SMT 11/11, recompilabilité 100%,
  transpile 4/4 (hash inchangé), winediff 33/33, cpudiff OK.

#### 🏁🏁 MILESTONE COMPLET : Sysinternals strings.exe (MSVC static-CRT C++) → ELF Linux natif,
#### tourne sans Wine, sortie **100% bit-identique à Wine**, exit 0.

### Consolidation — validation cpudiff des ops SSE2-string ajoutées ✅
- **2026-06-28** : les instructions SSE2 ajoutées pour strings.exe (`pcmpeqb`/`pcmpeqw`/`pcmpgtb`/
  `pmovmskb`/`pshuflw`/`pshufhw`) n'étaient validées que par le run différentiel strings.exe. Ajoutées
  au **corpus cpudiff** (axe 1) : encodages + implémentation des helpers `__pi_eq8`/`eq16`/`gt8`/
  `shufw`/`mskb` dans l'interpréteur. **Hard-validées contre Unicorn** (vérifié : casser `__pi_eq8` fait
  bien échouer le test avec des mismatchs `pcmpeqb` → l'instruction est scorée, pas skippée).
- **Robustesse strings.exe confirmée** sur tous les flags fonctionnels (`-a`/`-u`/`-n`/`-o` et combos) :
  bit-identique à Wine. Seule « différence » (`-b`/`-q` → usage) = le nom du programme dans `argv[0]`
  (`./out/app` vs `Z:\tmp\...\strings.exe`) — **environnemental**, pas un bug de traduction.
- **Régression complète PASS** : difftest 268/268, magicdiv 2³², SMT 11/11, recompilabilité 100%,
  transpile 4/4 (hash inchangé), winediff 33/33, **cpudiff OK (scoring élargi)**.

### Consolidation — garde de régression permanent pour les APIs version-info ✅
- **2026-06-28** : les APIs version-info (`GetFileVersionInfoSizeA`/`GetFileVersionInfoA`/
  `VerQueryValueA`) n'étaient gardées que par strings.exe (propriétaire, hors dépôt). Ajout d'un
  programme de corpus **`version_info.c` + `version_info.rc`** (ressource VS_VERSIONINFO embarquée via
  windres) : il lit sa propre ressource et imprime les valeurs de chaîne. `winediff.sh` compile
  désormais un `.rc` optionnel (windres → COFF, link `-lversion`).
- **Effet** : **winediff 34/34** — les APIs version-info sont validées contre Wine sur un programme
  **indépendant de strings.exe** (preuve de généralisation + garde CI permanent, dans le dépôt).
- *Note* : le fix `_chkstk`/`_alloca` MSVC ne peut pas être gardé par mingw (GCC utilise `___chkstk_ms`,
  probe-only, sans réécriture de frame — vérifié : mon détecteur `xchg esp,eax` ne s'y déclenche pas,
  winediff intact). Ce fix reste validé par le run strings.exe (documenté). Les instructions SSE2-string
  sont, elles, gardées par cpudiff.
- **Régression complète PASS** : difftest 268/268, magicdiv 2³², SMT 11/11, recompilabilité 100%,
  transpile 4/4 (hash inchangé), **winediff 34/34**, cpudiff OK.

### Généralisation — 2ᵉ binaire MSVC (distlib `t32.exe`) : fix récupération call-thunk ✅
- **2026-06-28 — 2ᵉ vrai PE MSVC testé** : `t32.exe` (lanceur console distlib, KERNEL32+SHLWAPI, 304
  fonctions, section .tls statique). ARET le transpile ; 1ᵉʳ bloqueur trouvé = **appel indirect vers
  fonction non récupérée `0x405c3f`** — un thunk `call [import]; ret 4` dont l'adresse est prise comme
  immédiat de code (callback). `looks_like_func_start` reconnaissait `jmp [mem]` (`ff 25`) mais pas
  `call [mem]` (`ff 15`).
- **Fix général** : ajout de `ff 15` (`call [mem]; ret`, thunk d'appel d'import / wrapper de callback
  adressé) aux prologues reconnus. Bénéficie à tout binaire utilisant ce motif de thunk.
- **Prochain bloqueur t32.exe** (documenté, non corrigé) : **R6016 « not enough space for thread
  data »** — `TlsSetValue` reçoit un slot garbage (`0x40F0D0`, une adresse IAT) avec les args décalés
  d'un slot. C'est le dispatch indirect CFG-gardé de la table d'abstraction Tls du CRT (`_getptd` via
  des pointeurs résolus par GetProcAddress → fallback direct). Même famille que le dispatch de
  strings.exe, à investiguer.
- **Régression complète PASS** : difftest 268/268, magicdiv 2³², SMT 11/11, recompilabilité 100%,
  transpile 4/4 (hash inchangé), winediff 34/34, cpudiff OK.

### Généralisation — 2ᵉ binaire MSVC (`t32.exe`) : bug général du pop __stdcall des imports appelés via registre ✅
- **2026-07-02 — R6016 résolu, cause *générale* trouvée**. Le bloqueur R6016 de `t32.exe` n'était PAS
  spécifique au CRT : c'était un **décalage d'un slot des arguments** d'un `call eax` indirect, causé par
  un pop `__stdcall` manquant sur l'appel *précédent*. Chaîne exacte (`_getptd` MSVC) :
  `push [ptr_encodé]; call edi (=DecodePointer); call eax (=TlsSetValue)`.
- **Cause racine (2 volets, tous deux généraux)** :
  1. **`DecodePointer`/`EncodePointer` absents de la table `stdcall_pops`** → leur `ret 4` n'était pas
     modélisé. Ajoutés (4 octets chacun).
  2. **Le pop `__stdcall` n'était appliqué qu'aux `call [import]` directs, jamais aux `call reg`** où le
     registre tient un pointeur d'import (motif compilateur courant : `mov edi,[iat]` une fois, puis
     `call edi` répété). `import_call_raw_name` (passe pop par-instruction) ne voit que `call [abs]` ;
     la résolution registre→import n'existe que dans la passe de nommage ultérieure. Résultat : `esp`
     dérivait de `@N` après chaque `call reg` d'import stdcall, décalant tous les accès pile suivants —
     ici les args du `call eax` d'après, d'où `TlsSetValue(slot=0x40F0D0, …)` → R6016.
- **Fix général** : dans la passe de résolution registre→import (`build_ir`), injection du même
  `esp += @N` après un `call reg` résolu vers un import stdcall (`stdcall_pop_for_regcall`). Le cas
  `call [abs]` reste géré par la passe par-instruction (pas de double pop). Bénéficie à **tout** binaire
  chargeant un import stdcall dans un registre avant de l'appeler.
- **Bug latent corrigé au passage** : la table `stdcall_pops` avait `InitializeSListHead` avant
  `InitializeCriticalSectionEx` (ordre alphabétique cassé → la recherche binaire ne trouvait plus `Ex`,
  son pop de 12 était silencieusement ignoré). Réordonné + **test unitaire `table_is_sorted_by_name`**
  ajouté pour verrouiller l'invariant (la recherche binaire suppose l'ordre).
- **Effet t32.exe** : R6016 disparu, l'init des données par-thread du CRT réussit ; le programme
  progresse jusqu'à `GetEnvironmentStringsW` (import non implémenté ; `t32.exe` est un lanceur de
  processus qui exige `CreateProcessW`/job objects — hors périmètre M1, mais la *traduction CPU* est
  désormais correcte au-delà de l'init CRT).
- **Régression complète PASS** : difftest 268/268, in-place 3/3, magicdiv 2³², SMT 11/11,
  recompilabilité 100 %, transpile 4/4 (hash inchangé `4b0121f182554d40`), **winediff 34/34**, cpudiff
  268/268. (L'unique échec du test unitaire WASM `signature_mismatch:sub_401000` est **préexistant** —
  vérifié par `git stash` sur `c95ca7f` — sans rapport avec ce fix.)

### Cible WASM — signature d'entrée désynchronisée du threading ebp ✅
- **2026-07-02** : le test unitaire `windows_pe_to_webassembly` échouait (`signature_mismatch:sub_401000`,
  trap `unreachable`). **Régression latente** introduite par le threading ebp (signatures 4→5 args) : le
  `typedef aret_fn`, les trampolines de dispatch et les déclarations `aret_decls.h` étaient passés à 5
  args, **mais `aret_main.c` (déclaration + appel de l'entrée) et les stubs faibles `undef_subs` étaient
  restés à 4 args**. Le backend C natif tolère l'écart (ABI x86-64 laxiste, 5ᵉ arg = registre poubelle) ;
  **wasm-ld non** : signatures conflictuelles entre TU → il génère un stub piège `signature_mismatch:<fn>`
  qui exécute `unreachable`. D'où le trap dès `main → sub_401000`.
- **Fix** : `aret_main.c` déclare et appelle l'entrée avec 5 args (`…, uint64_t b` ; ebp initial = 0) ;
  le stub `undef_subs` prend aussi 5 args. Toutes les signatures de la passe transpile sont désormais
  cohérentes (5 args partout).
- **Effet** : **WASM 7/7 fixtures** (chaîne pile, données globales, appels indirects, CRT printf/heap,
  x87, couche Win32, SHA-256 réel) — le PE Windows tourne nativement en WebAssembly. La suite complète
  `cargo test --release` est **entièrement verte** (plus aucun échec préexistant).
- **Régression complète PASS** : transpile 4/4 (hash inchangé `4b0121f182554d40`), winediff 34/34,
  suite unitaire 50+3+44+1+1 verte (dont WASM).

### Phase 2 — Pruning par accessibilité (conversion ciblée) ✅ FAIT
- **2026-07-02** : `--function <nom|addr>` en mode transpile ne sort plus tout le binaire ni la seule
  fonction sélectionnée (qui casserait ses callees), mais sa **fermeture transitive d'appels directs**
  (`analysis::reachable_closure`, BFS sur `Function::callees`). L'entrée bascule automatiquement sur la
  fonction choisie (`main` la pilote), sauf `--entry` explicite.
- **Sûreté** : seuls les appels **directs** sont suivis ; le code atteint uniquement par appel
  indirect/vtable/callback n'est **pas** tiré dans la fermeture — un tel appel échoue bruyamment au
  runtime (`aret_call` → unmodelled) plutôt que d'être deviné (principe sacré). Le matcher `--function`
  tolère aussi le tiret de tête des symboles C (`_feature_a` ↔ `feature_a`), comme `--entry`.
- **Vérifié** : fixture `prune_closure.exe` (2 features indépendantes pilotées par `main`).
  `--function feature_a` → **3 fonctions** (feature_a + helper_add + helper_mul), tourne seul
  (`FEATURE_A: 42`), et `feature_b`/`helper_sub`/`main` sont **absents** du C généré. Test
  `prune_to_function_closure` (m1 44→45), suite complète verte, transpile 4/4 (hash inchangé
  `4b0121f182554d40`).
- **Bénéfice** : transpiler une seule feature d'un gros binaire sans payer (ni risquer) tout le reste.

### Phase 3 — Robustesse FLIRT (opérandes relocalisés) + recovery amorcée par signature ✅ (avancée majeure)
- **2026-07-02 — cause générale trouvée sur un binaire mingw strippé** (CRT : qsort+callback, sprintf,
  printf ; glue `atexit(___do_global_dtors)`). Symbolé : SOUND, tourne parfait. **Strippé : sortie
  correcte puis abort à la sortie** — deux trous généraux :
  1. **Signatures FLIRT sur-spécifiques.** `gen_signature` ne masquait que les branches relatives, pas
     les **opérandes absolus relocalisés** (`mov reg,[abs32]`, pointeurs de tables) qui **varient d'un
     binaire à l'autre**. Résultat : la signature de `__pei386_runtime_relocator` (et des runners de
     ctors/dtors) était épinglée sur *le* binaire d'origine et ne matchait aucun autre → glue liftée →
     abort sur x87 non modélisé. **Fix** : parse de la table de base-relocation PE (`.reloc`,
     HIGHLOW/DIR64 → `Program::base_relocs`) ; `gen_signature(name, code, reloc)` masque les octets
     relocalisés (comme le vrai FLIRT d'IDA). DB régénérée et **fusionnée** (53 noms conservés, variantes
     glue re-wildcardées ajoutées). Test `absolute_reloc_operand_is_wildcarded`.
  2. **Glue atteinte par appel indirect jamais récupérée.** `___do_global_dtors` (enregistré par
     `mov [esp],0x<dtors>; call atexit`) commence par `mov eax,[abs];mov eax,[eax];test` — prologue hors
     heuristique → jamais récupéré comme fonction → appel indirect vers adresse non récupérée → abort.
     **Fix général** : `looks_like_func_start` accepte désormais toute adresse **reconnue par signature**
     (`crt_symbol`/`is_startup_glue`) — le signal le plus fort qui soit. La recovery address-taken amorce
     alors la glue, qui est ensuite **host-backed** (no-op).
- **Effet** : le binaire **strippé** tourne proprement jusqu'à la sortie, **bit-identique à Wine**
  (`functions 107→108`, `host-backed 31→33`, plus d'abort). Bénéficie à **tout** binaire strippé à CRT
  statique + glue mingw. Fixture committée `stripped_crt.exe` + test
  `stripped_crt_glue_recovered_and_hostbacked`.
- **Régression complète PASS** : cargo test (51 unit + m1 45→46), difftest 268/268, transpile 4/4 (hash
  inchangé `4b0121f182554d40`), winediff 34/34, recompilabilité 100 %, SMT 11/11, magicdiv, in-place 3/3.
- **Reste Phase 3** : signatures **MSVC** (`ucrtbase`/`msvcr*`) — nécessite un corpus MSVC symbolé (indispo
  sur cet hôte Linux/mingw) ; le chemin mingw-strippé est désormais solide et le fix FLIRT (reloc) est
  générique (profitera aux signatures MSVC quand on aura le corpus). Le volet 3a (Lua strippé, quelques
  fonctions aux frontières) reste ouvert.

### Lua débloqué — `--entry main` résolvait `___main` (glue) au lieu de `_main` ✅
- **2026-07-02** : Lua produisait une **sortie vide** avec `--entry main` (symbolé *et* strippé) — un
  faux « ça tourne » : `main` s'exécutait proprement (exit 0) mais **aucune** option (`-e`, `-v`, `-Q`)
  n'était traitée. Diagnostic : `--entry main` était résolu vers **`___main`** (0x42f900, la glue mingw
  qui roule les ctors) et **non** vers `_main` (0x43ae90, le vrai `main` C). Le résolveur faisait
  `k.name.trim_start_matches('_') == s`, qui retire **tous** les underscores → `___main` matche « main »,
  et son adresse plus basse gagnait l'itération. On exécutait donc la garde d'init de la glue, pas main.
- **Fix général** : `symbol_matches(sym, want)` ne retire **qu'un** underscore par côté (la décoration
  cdecl standard) → `main` ↔ `_main`, jamais `__main`/`___main`. Appliqué à `--entry` **et** `--function`.
  Bug latent pour **tout** binaire mingw (dépendait de l'ordre d'adresses `___main` vs `_main`). Test
  unitaire `symbol_matches_respects_single_underscore_decoration`.
- **Effet** : **Lua symbolé tourne, bit-identique à Wine** (`table.sort`, `string.format`, `math.pi^2`,
  méthodes de chaîne, `-e`). Lua redevient un oracle. Le **strippé** trouve bien `_main` (via `find_main`)
  mais reste limité par la couverture FLIRT (≈20 fonctions CRT liftées au lieu d'être shimées : 31
  partial vs 14) — élargissement de la DB = travail incrémental restant.
- **Régression PASS** : cargo test (52 unit + m1 46), transpile 4/4 (hash inchangé `4b0121f182554d40`).

### Phase 3a — Sur-récupération : cases de `switch` prises pour des fonctions ✅
- **2026-07-02** : en mode strippé, Lua **sur-récupérait** massivement dans la région du compilateur
  (79 fonctions vs 50 symbolé) → fonctions **tronquées** → `luaK_exp2K` segfault, sortie vide. Cause
  générale : un `switch` range ses adresses de cas dans une table `.rdata`. Le scan de données voit cette
  suite dense de pointeurs de code et, si la fonction du `switch` n'était **pas encore décodée** quand il
  tourne (son `jmp [table+idx*4]` non résolu — la table est globale, la fonction atteinte tard), prend
  chaque **corps de cas** (adresse intérieure) pour une entrée de table de pointeurs de fonctions → seed
  d'une fausse fonction qui tronque la vraie au niveau de la frontière.
- **Fix propre, indépendant de l'ordre** : (1) résolution des jump-tables extraite en
  `resolve_jump_tables_fixpoint`, lancée **avant** le seed des candidats (haut de la boucle) *et* en
  final ; (2) **post-élagage** : après le point fixe (toutes tables résolues), on retire de `entries` /
  `prologue_only` toute adresse qui est une **cible de jump-table résolue** — un corps de cas intérieur
  n'est jamais une entrée de fonction, et son retrait laisse la vraie fonction se recomposer à travers
  lui. La course (scan global vs fonction atteinte tard) est ainsi corrigée après coup.
- **Effet** : Lua strippé passe de **79→49** fonctions dans la région (symbolé : 50), partial 31→23 ; le
  **compilateur bytecode tourne** (imprime « STRIP 42 » au lieu de vide/segfault). Bénéficie à **tout**
  binaire strippé à `switch` (le motif est universel).
- **Reste** : Lua strippé bute maintenant à la *sortie* sur `___do_global_dtors` (0x42f860) atteint par
  appel indirect, non reconnu par FLIRT (signature d'une autre version mingw). Séparé (couverture FLIRT
  par version), pas la sur-récupération.
- **Régression complète PASS** : cargo test (52 unit + m1 46), difftest 268/268, transpile 4/4 (hash
  inchangé `4b0121f182554d40`), recompilabilité gzip/ls/cat 100 %, winediff 34/34, SMT 11/11, in-place 3/3.

### Phase 3a — Prologue de garde `__do_global_dtors_aux` reconnu (version-indépendant) ✅
- **2026-07-02** : Lua strippé, après le fix de sur-récupération, tournait (compilateur OK, « STRIP 42 »,
  `table.sort`) mais butait à la **sortie** sur `___do_global_dtors` (atteint par `atexit` → appel
  indirect), non récupéré. Sa signature FLIRT (autre version mingw) ne matchait pas. **Fix général,
  indépendant de la version** : `looks_like_func_start` reconnaît le prologue de garde
  `mov eax,[abs32]; mov eax,[eax]; test eax,eax` (9 octets) — l'idiome `__do_global_dtors_aux`/
  `__do_global_ctors_aux` — en plus de la variante sans déréférencement déjà gérée. Appliqué aux seuls
  candidats *address-taken* (l'immédiat d'un `atexit`), donc sûr.
- **Effet** : `os.exit(5)` propage enfin (exit 5, plus d'abort), et Lua strippé égale Wine sur
  `table.sort`/`print`/arithmétique. Reste un `fld qword [eax]` non modélisé sur le chemin de formatage
  **flottant** (`string.format("%.4f", math.pi^2)`) — complétude lifter x87 (Phase 5), séparé.
- **Régression PASS** : difftest 268/268, transpile 4/4 (hash inchangé), recompilabilité gzip/ls/cat 100 %,
  52 tests.

### Phase 5 — libm statiquement liée reconnue comme fp-returning en strippé ✅
- **2026-07-02** : dans `luaV_execute` (boucle VM), `fld qword [eax]` retombait en asm opaque **en strippé**
  alors que le symbolé liftait proprement. Cause : `call_returns_fp`/`compute_fp_returning` ne
  reconnaissaient une fonction libm fp-returning que par **nom de symbole** (`pow`, `sin`…). En strippé
  ces fonctions sont `sub_<addr>` (pas de symbole) et leur corps x87 complexe fait **bailler** l'analyse
  de profondeur → jamais classées fp-returning → l'`OP_POW` de Lua (`call pow`) ne compte pas le `st(0)`
  poussé → la profondeur x87 de tout `luaV_execute` se désynchronise → ses `fld` retombent en asm.
- **Fix général** : reconnaître aussi la libm par **signature FLIRT** via `prog.crt_symbol(addr)` (le
  nom CRT reconnu à l'entrée), en plus du symbole et du thunk d'import. Appliqué au seed de
  `compute_fp_returning` et au site d'appel `call_returns_fp`.
- **Effet** : `luaV_execute` strippé **0 `fld` non modélisé** (comme le symbolé), partial 23→19. Général
  pour tout binaire strippé appelant une libm statique (le motif `OP_POW`→`pow` est universel).
- **Soundness préservée** : cpudiff OK, difftest 268/268, transpile 4/4 (hash inchangé), recompilabilité
  gzip/ls/cat 100 %, 52 tests. (Lua strippé progresse ensuite jusqu'à un segfault **atoi** dans une
  autre fonction — trou séparé en cours.)

### Phase 3 — Signatures de thunks supprimées (faux positif FLIRT introduit par le reloc-wildcarding) ✅
- **2026-07-02** : Lua strippé segfaultait dans `atoi` sur le chemin de formatage flottant de printf.
  Cause : `____lc_codepage_func` (un thunk `jmp [0x43c090]; nop`) était reconnu comme **`atoi`** →
  l'appel liait le vrai `atoi(pointeur_poubelle)` → crash. Un thunk `jmp [IAT]` n'a pour seule identité
  que son opérande d'adresse **relocalisé**, que mon reloc-wildcarding (ec72102) masquait → la signature
  d'atoi devenait `ff25..........90`, matchant **n'importe quel** thunk.
- **Fix général et principiel** : un thunk n'est **jamais** signaturé (il est résolu structurellement par
  `import_thunk`). `gen_signature` refuse les corps commençant par `jmp [mem]` (ff25) / `call [mem]`
  (ff15) / `jmp rel32` (e9/eb) ; les 80 signatures de thunks de la DB (sur 111) sont retirées (il en
  reste 31, les vrais corps de fonctions). Test `thunk_functions_get_no_signature`.
- **Effet** : Lua strippé imprime enfin `9.8696 ff` / `1,3,5,9` — **bit-identique à Wine** sur
  `string.format` flottant + `table.sort`. host-backed 71→70 (les thunks étaient redondants). Bute
  ensuite à la sortie sur `0x439460` (encore une glue par appel indirect, séparé).
- **Régression PASS** : difftest 268/268, transpile 4/4 (hash inchangé), recompilabilité gzip/ls/cat
  100 %, winediff 34/34, 53 tests.

### Phase 3a — Callback passé par valeur (`push imm`/`mov [esp],imm`) reconnu → Lua strippé COMPLET ✅
- **2026-07-02** : dernier trou de Lua strippé — `_dtoa_lock_cleanup` (nettoyage dtoa en sortie,
  enregistré par `atexit`, prologue exotique `mov eax,imm; xchg [mem],eax`) atteint par appel indirect,
  non récupéré → abort après la sortie flottante correcte.
- **Fix général** : un immédiat placé dans un **slot d'argument pile** (`push imm32` /
  `mov [esp+d], imm32`) qui pointe vers du code exécutable est un **pointeur de fonction passé par
  valeur** — un callback (`atexit(cleanup)`, `qsort(…, cmp)`). La position d'argument le prouve, quel
  que soit le prologue de la callee. `stack_arg_code_imm` le seed sans l'heuristique de prologue
  (restreint aux formes push/mov-vers-pile pour ne pas confondre un `mov reg,imm` scalaire).
- **Effet** : **Lua strippé complet, bit-identique à Wine** — `table.sort`, `string.format` flottant
  (π/e/√2), `gsub`, `gmatch`/patterns, `os.exit`, **sortie propre exit 0**. D'inutilisable
  (vide/segfault) à pleinement fonctionnel. Général pour tout binaire strippé enregistrant des callbacks.
- **Régression PASS** : difftest 268/268, transpile 4/4 (hash inchangé `4b0121f182554d40`),
  recompilabilité gzip/ls/cat 100 %, winediff 34/34, 53 tests.
- **Bilan Lua** : symbolé ET strippé tournent bit-identique à Wine. Le vrai binaire mingw Lua 5.4
  (650 Ko, strippé 320 Ko) → ELF natif fonctionnel, sans émulation.

### Phase 4 — Dispatch virtuel C++ (vtable en `.rodata`) : cœur validé ✅
- **2026-07-02** : un appel virtuel C++ compile en `call [vtable + k]` via une vtable en `.rodata`. Testé
  avec un modèle C fidèle (tables de pointeurs de méthodes `const` en `.rodata`, dispatch
  `obj->vt->method(obj)`) : fixture `vtable_dispatch.exe`. Les méthodes ne sont atteintes **que** par la
  vtable → la recovery address-taken (durcie en Phase 3) les trouve, et chaque appel indirect dispatche
  vers la bonne. **Résultat : SOUND, bit-identique à Wine** (`square=25 rect=12`, `total=37`), symbolé
  **et** strippé. Le mécanisme cœur de Phase 4 est donc couvert par la machinerie générale existante
  (indirect-call + address-taken). Test `cpp_style_vtable_dispatch`.
- *Reste Phase 4* : vrai C++ compilé par g++ (exceptions, RTTI, noms manglés, `thiscall`) — non testable
  sur l'hôte actuel (pas de `mingw g++`). Le dispatch vtable lui-même fonctionne.

### Phase 3b — Vrai binaire MSVC 32-bit (sqlite3.exe) : bug général aret_disp + shims CRT ✅ (en cours)
- **2026-07-02** : téléchargé un **vrai `sqlite3.exe` MSVC 32-bit strippé** (SQLite 3.40.1, 2958 fonctions
  récupérées) — le premier vrai binaire MSVC piloté bout-en-bout. Révèle et corrige plusieurs bugs :
  1. **Bug général `aret_disp`** (commité) : un appel indirect vers une fonction host-backed passait
     `esp` au lieu de `esp+4` → le shim lisait l'adresse de retour comme 1er argument. `GetSystemInfo`
     de sqlite écrivait un `SYSTEM_INFO` à travers → **segfault**. Corrigé (compense le push comme le
     trampoline IAT). Régression complète verte.
  2. **Shims CRT ajoutés** : `_access`/`_chmod` (POSIX via `translate_path`), `HeapSize`/`_msize`
     (`malloc_usable_size` — sqlite traçait 0 octet par bloc → « out of memory »).
- **État sqlite** : ne segfaulte plus, passe l'init ; bute maintenant sur un **`malloc(0)` dans `main`**
  (taille mal liftée parmi 2958 fonctions → sqlite « out of memory »). Forensics profonde restante (vrai
  gros binaire MSVC = cible multi-session). **Le gain général (`aret_disp`) est acquis et validé.**
- **Régression PASS** : winediff 34/34, 53 tests, transpile 4/4 (hash inchangé).

### Phase 3b — sqlite3.exe MSVC : 3 bugs généraux (aret_disp, rep scas, wmain wide-argv) ✅ (progrès)
- **2026-07-02** : en pilotant un vrai `sqlite3.exe` MSVC 32-bit strippé (2958 fn), 3 bugs **généraux**
  trouvés et corrigés (chacun touche une classe entière de binaires) :
  1. **`aret_disp` esp+4** : dispatch host-backed indirect lisait l'adresse de retour comme 1er arg →
     `GetSystemInfo` segfaultait.
  2. **`rep(ne) scas`** (inline-strlen MSVC) non modélisé → `strlen`=0 → `malloc(0)` → « out of memory ».
     Modélisé (helper `__rep_scasN`, edi/ecx/flags).
  3. **wmain wide-argv** : sqlite importe `__wgetmainargs` → son entrée est un `wmain` qui lit `argv[i]`
     en `wchar_t*`. L'argv narrow était lu un octet sur deux (`SELECT`→`SLC`). Le wrapper `aret_main`
     construit maintenant un **argv UTF-16** quand `__wgetmainargs` est importé.
- **État sqlite** : lit ses arguments correctement (`sqlite3 -version` → `3.40.1 …` bit-identique). Le
  **moteur SQL** (open `:memory:` + exécution + impression des lignes) ne sort pas encore — couche plus
  profonde (cible multi-session). Mais **3 bugs universels acquis** depuis un seul binaire, validant la
  stratégie « creuser un vrai gros binaire intelligemment ».
- **Régression PASS** (chaque commit) : cpudiff, difftest 268/268, transpile 4/4 (hash inchangé),
  winediff 34/34, recompilabilité 100 %, 53 tests.

### Phase 3b/5 — sqlite3.exe : moteur SQL scalaire bit-identique à Wine (5 bugs généraux) ✅ JALON
- **2026-07-02** : un vrai `sqlite3.exe` MSVC 32-bit strippé (2958 fn) exécute désormais les **requêtes
  SQL scalaires bit-identiques à Wine** : `SELECT 42`, `hex(255)`→`323535`, `abs`, `substr`, `upper`,
  `length`, gestion d'erreurs (`no such table`), méta-commandes (`.databases`), `sqlite3 -version`.
- **5 bugs GÉNÉRAUX tirés d'un seul binaire** (chacun touche une classe entière) :
  1. `aret_disp` esp+4 (dispatch host-backed indirect).
  2. `rep(ne) scas` (inline-strlen MSVC → `malloc(0)`).
  3. wmain wide-argv (`__wgetmainargs` → argv UTF-16).
  4. Masque du compteur `rep` sur 32-bit (débordement bit 32 → fill de 4 milliards → segfault).
  5. `xchg` high-byte (`xchg al,ah`, byte-swap de `hex()`).
  \+ shims CRT `_access`/`_chmod`/`HeapSize`/`_msize`/`GetSystemTime`/`SystemTimeToFileTime`.
- **Reste** : le chemin **b-tree** (`CREATE TABLE`/`INSERT`) corrompt la base (`database disk image is
  malformed`) — un miscompile subtil dans du code entier **entièrement lifté** (pas un asm fallback ;
  les 60 partial(asm) sont tous x87, hors chemin b-tree). Forensics profonde restante (cible
  multi-session). *Validation de la stratégie « creuser un vrai gros binaire » : 5 bugs universels.*
- **Régression PASS** (chaque commit) : cpudiff, difftest 268/268, transpile 4/4 (hash inchangé),
  winediff 34/34, recompilabilité 100 %, 53 tests.

### Phase 3b/5 (suite) — 5 bugs HLE généraux + localisation du miscompile b-tree
- **2026-07-02** : reprise de sqlite3.exe. Le **chemin lecture** (`SELECT … FROM sqlite_master`, ouvrir
  une base valide) et les écritures **INSERT** marchent désormais bit-identique à Wine. **5 nouveaux
  bugs GÉNÉRAUX du runtime HLE** (chacun touche toute une classe de binaires Win32/mingw) :
  1. **`VirtualQuery`** renvoyait 0 → la relocation pseudo-reloc de mingw (`_pei386_runtime_relocator`)
     faisait `abort()` (« VirtualQuery failed … »). Renvoie maintenant une `MEMORY_BASIC_INFORMATION`
     valide (page committée, image, RWX). *Tout PE lié mingw en avait besoin.*
  2. **`__wgetmainargs`** (CRT wide `wmainCRTStartup`) non implémenté → argc/argv non initialisés →
     SIGABRT. Implémenté `aret_wgetmainargs` (argv UTF-16 construit depuis les args réels).
  3. **`fgets` sur stdin** : castait un handle `_iob` synthétique en `FILE*` hôte → segfault (le shell
     sqlite lit son script depuis stdin). Ajout du chemin fd brut (comme `getc`/`fputs`).
  4. **`ReadFile`/`WriteFile` ignoraient `lpOverlapped`** : la VFS Win32 de sqlite passe l'offset du
     fichier **via la struct OVERLAPPED** (pas `SetFilePointer`), donc chaque lecture/écriture tapait
     l'offset du pointeur de fichier (page 0) → « file is not a database » / « malformed » à la lecture.
     Corrigé avec `pread`/`pwrite` à l'offset OVERLAPPED. **→ débloque TOUTE la lecture de base sqlite.**
  5. **Verrous de fichier** `LockFile`/`LockFileEx`/`Unlock*` absents → « database is locked ». Accordés
     (modèle mono-processus).
- **Miscompile b-tree LOCALISÉ** (reste à corriger) : le bug résiduel est **la suppression d'une seule
  cellule** (`DELETE FROM u WHERE a=1` sur 2 lignes → malformed ; `DELETE` global via `OP_Clear` OK ;
  INSERT/UPDATE-de-valeur/splits de page OK). Chemin : `sqlite3BtreeDelete`→`dropCell`. La page est
  **valide** (pc=0x0ffc, taille cellule=4, usableSize=4096) mais le contrôle `if(pc+sz>usableSize)
  return SQLITE_CORRUPT` se déclenche à tort (freeSpace n'est jamais atteint). → miscompile entier dans
  `dropCell`/`BtreeDelete` (pc ou sz ou la comparaison). La page modifiée vit dans un **buffer pager
  distinct** de celui journalisé, d'où la difficulté de capture gdb. Repro minimal acquis.
- **Régression PASS** : cpudiff, difftest 268/268, transpile 4/4 (hash `4b0121f182554d40` inchangé),
  winediff 34/34.

### Phase 3b/5 (suite) — bug #6 RÉSOLU : appels indirects supprimés par la DCE
- **2026-07-02** : le miscompile de suppression de cellule est **trouvé et corrigé** — c'était un vrai
  **bug de lifter général**, pas propre à sqlite. Forensics :
  1. `DELETE FROM u WHERE a=1` (2 lignes) → malformed. Réduit à : `dropCell`/`clearCell`.
  2. Via `sqlite3CorruptError` (trouvé par la string « database corruption » → VA `sub_42a0fd`),
     capture du **numéro de ligne** `__LINE__` = **74862** = `clearCellOverflow` : `if(pCell+nSize >
     aDataEnd) return CORRUPT`. Or `clearCellOverflow` n'est appelé que si la cellule a un overflow —
     ce qui est faux pour une cellule de 4 octets. → la **CellInfo est corrompue** (nPayload=0,
     nLocal=57020, nSize=2349 pour une cellule valide `02 01 02 09`).
  3. Désassemblage à `0x452029` : `call *0x50(%esi)` = `pPage->xParseCell(pPage, pCell, &info)`
     **présent dans le binaire mais absent du C transpilé** — l'appel indirect est **supprimé**, donc
     `info` reste non initialisé.
  4. **Cause racine** (`src/opt/mod.rs::has_impure_call`) : `CallTarget::Indirect(x) =>
     has_impure_call(x)` — un appel indirect n'était jugé impur que si son **expression cible** (le
     pointeur de fonction, un simple `Load`) contenait un appel impur, donc **jamais**. La DCE le
     classait pur et le supprimait quand son résultat était mort (ici `eax` réécrit juste après par
     `movzwl`). **Fix** : `CallTarget::Indirect(_) => true` (un appel indirect a des effets de bord
     inconnus, toujours impur — comme un appel direct).
- **Effet** : `DELETE`/`UPDATE` marchent désormais **bit-identique à Wine** (fichier vérifié). Test de
  régression ajouté (`indirect_call_is_impure_and_survives_dce`).
- **Matrice de capacités sqlite (post-fix, vérifiée Wine)** : scalaire ✅, lecture de base ✅,
  `INSERT` ✅, `DELETE` ✅ (persisté, relu par Wine), `UPDATE` ✅ (persisté, relu par Wine).
  `CREATE TABLE` ❌ → **RÉSOLU** (jalon suivant).

### Phase 3b/5 (suite) — CREATE TABLE RÉSOLU : appel-tail sur soi-même mal structuré ✅ CRUD COMPLET
- **2026-07-03** : `CREATE TABLE` (et `WHERE … AND/OR …`) marchaient pas — bug de lifter général.
  Forensics :
  1. La grammaire CREATE parse bien (`EXPLAIN CREATE` OK). C'est l'**exécution** qui boucle
     (récursion non déterministe dans `sub_41bf36`→`sub_41be70`, éléments de 48 o = pile de termes).
  2. Isolé : le reparse `OP_ParseSchema` lance `SELECT … WHERE tbl_name=… AND type!='trigger'`. Or
     **tout `WHERE … AND/OR …` renvoyait vide / bouclait** (condition simple OK, `1 AND 1` KO), alors
     que `SELECT 1 AND 1` scalaire est correct.
  3. `v21=44` = **TK_AND** → `sub_41bf36` = `whereSplit` (découpe récursive de l'arbre AND),
     `sub_41be70` = `whereClauseInsert` (grossit le tableau de `WhereTerm`).
  4. Désassemblage `0x41bf36` (`__fastcall`, args en eax/edx/ecx) : `whereSplit(pWC,pE2->pLeft,op)`
     est un **appel**, puis `whereSplit(pWC,pE2->pRight,op)` est un **tail-call** via `jmp 0x41bf36`
     (l'entrée). Le transpileur transformait ce `jmp` vers l'entrée en **boucle** — ce qui (a) rejoue
     le prologue (fuite de la pile partagée) et (b) ne peut pas ré-injecter l'argument registre mis à
     jour (`edx=pE2->pRight`) car le bloc d'entrée n'a pas de φ pour l'appel initial. La boucle
     relisait `pExpr` d'origine à l'infini → insertion illimitée de termes.
- **Cause racine** (`ir/build.rs`, `Flow::Jump`) : un `jmp` vers **l'entrée de la fonction elle-même**
  est un **self tail-call** (le compilo a transformé une tail-récursion en jmp-sur-soi ; rejouer
  l'entrée = un appel *frais*, pas une arête arrière). **Fix** : quand la cible du `jmp` == `func.entry`,
  émettre un tail-call (`return sub_self(args)`), pas un `Jump` de boucle — les registres args courants
  (eax/ecx/edx/ebp, dont `edx=pRight`) sont alors passés correctement.
- **Effet** : **moteur SQL sqlite complet bit-identique à Wine** (aux fins de ligne CRLF↔LF près, que
  winediff normalise déjà). Vérifié sur `:memory:` ET fichier neuf (créé par nous, relu par Wine) :
  `CREATE`/`INSERT`/`DELETE`/`UPDATE`, `GROUP BY`+agrégats, `JOIN`, `CREATE INDEX`+usage indexé, `IN`,
  `HAVING`, CTE (`WITH`), sous-requête scalaire, **fonctions fenêtre** (`OVER (PARTITION BY …)`),
  `ORDER BY`, cross-join. Un vrai `sqlite3.exe` MSVC 32-bit strippé exécute donc son moteur SQL
  nativement sous Linux. (Le « hang » sur fichier neuf était le même bug whereSplit. ⚠️ **Note
  corrigée** : la version d'origine de cette note disait que laisser `GetVersionExA`/`AreFileApisANSI`
  en stub-à-0 était un « choix » car les rendre fidèles cassait un chemin VFS récent — **c'était en
  réalité la seule rustine par binaire du projet, masquant un bug HLE général**. Levée à l'entrée
  suivante : chemin VFS wide implémenté proprement + cause racine `SetLastError` corrigée.)
- Test corpus `tailrec` ajouté (garde le chemin de saut normal).
- **Régression PASS** : cpudiff, difftest 272/272, transpile 4/4 (= réf native, 58 fn), winediff 34/34,
  55 tests.
- **Régression PASS** : cpudiff, difftest 268/268, transpile 4/4 (hash `4b0121f182554d40` inchangé),
  winediff 34/34, 54 tests unitaires.

### Phase 3b/7 — Rustine VFS sqlite SUPPRIMÉE : couche fichier wide + cause racine `SetLastError` ✅
- **2026-07-03 — la seule vraie rustine du projet levée**, en corrigeant un bug HLE **général**. Le
  jalon sqlite ci-dessus laissait `GetVersionExA`/`AreFileApisANSI` en **stub-à-0 exprès** pour garder
  sqlite sur son chemin VFS ANSI simple ; la note d'alors justifiait ce « choix » par « les rendre
  fidèles casse le chemin VFS récent ». C'était contraire au **principe sacré** (« jamais de rustine
  par binaire, corriger le général ») : on masquait un bug au lieu de le surfacer.
- **Diagnostic (différentiel Wine)** : rendus fidèles (`GetVersionExA`/W → 6.2.9200 NT, `AreFileApisANSI`
  → TRUE, exactement ce que Wine rapporte), sqlite bascule sur son **vrai chemin VFS wide (Win8+)** et
  échoue `disk I/O error (10)` en appelant `GetFullPathNameW`/`GetFileAttributesExW` (**non
  implémentées**) puis `FormatMessageW`. Donc le bug caché n'était **pas** un miscompile lifter mais un
  **manque d'APIs fichier wide** + une **cause racine générale**.
- **Cause racine (générale, `runtime/aret_hle/aret_hle.c`)** : les shims d'attributs fichier
  (`GetFileAttributes*`) renvoyaient bien « absent » mais **ne posaient pas `GetLastError`**. sqlite
  `winAccess` lit `GetLastError` pour distinguer *fichier absent* (normal) d'une *vraie erreur d'accès* ;
  sans `ERROR_FILE_NOT_FOUND` il concluait `SQLITE_IOERR_ACCESS`. `aret_attr_named`/`aret_attr_ex_named`
  posent maintenant `g_last_error` (2=FILE, 3=PATH) à l'échec et 0 au succès. **Touche tout binaire Win32
  sondant l'existence d'un fichier via `GetFileAttributes*` + `GetLastError`.**
- **APIs wide implémentées** : `GetFullPathNameW` (+ helper `aret_n2w` narrow→UTF-16),
  `GetFileAttributesExA`/`W` (`WIN32_FILE_ATTRIBUTE_DATA` via `stat`), `GetVersionExA`/`W` +
  `AreFileApisANSI` fidèles. `FormatMessageW` s'est avéré n'être qu'un **symptôme** (chemin d'erreur),
  non requis une fois la cause corrigée.
- **Effet** : le vrai `sqlite3.exe` MSVC prend désormais son **chemin VFS wide réel** (celui de
  Windows/Wine, plus le chemin ANSI « de contournement »), et `CREATE`/`INSERT`/`UPDATE`/`DELETE` +
  le fichier `.db` produit sont **bit-identiques à Wine octet à octet**. **Plus aucune rustine.**
- **Garde** : `bench/winecorpus/wide_fileattr.c` (GetFullPathNameW, GetFileAttributesExW, et le contrat
  `GetLastError=FILE_NOT_FOUND` sur fichier absent) — indépendante de sqlite (hors dépôt). **winediff
  34/34 → 35/35.**
- **Régression PASS** : cargo test (54+3+48+1+1), **difftest 271/271** (0 divergence), **transpile-diff
  4/4** (H=`19acad982194bf07`, O0–O3 cohérents), **cpudiff vert**, **winediff 35/35**. Fix HLE-only (le
  lifter n'est pas touché).

### Phase 5 — Filet x87 runtime (fonctions qui bailent la profondeur) ✅ incrément 1 ; ⚠️ 2/3 révoqués
- **2026-07-03 — le vrai chantier « robustesse passe x87 » attaqué par un filet runtime.**
  Quand l'analyse statique de profondeur x87 abandonne sur une fonction (`x87_states`→None), ses ops
  FPU tombaient en `Asm`→abort. Nouveau : les modéliser contre une **pile FPU runtime** (`__x87rt_*`,
  `src/emit/mod.rs`), correcte par construction (la pile est un état runtime, plus besoin de profondeur
  statique). Chaque op = un `CallStmt` **impur** (l'opt ne le supprime/réordonne jamais). C'est la
  technique « compter à l'exécution » de Remill/QEMU, appliquée **chirurgicalement au seul FPU, en code
  natif auto-contenu** (aucune dépendance runtime — respecte la contrainte « standalone »).
- **Incrément 1 (commit `9898287`, CONSERVÉ) — SOUND, zéro régression.** load/store (f/fi 32/64/80),
  constantes fld1/fldz, arithmétique (toutes formes reg/mem/entières via `__x87rt_ar`), fxch/fabs/fchs/
  fsqrt/frndint, fldcw/fnstcw (mode d'arrondi runtime). Injection gatée **transpile-only** (`x87_rt =
  x87.is_none() && shared_stack()`) et **purement additive** : le chemin statique (Lua, corpus) est
  intact (transpile-diff hash `19acad982194bf07` **inchangé**). Soundness : accès pile **bornés →
  `__builtin_trap`** sur under/overflow ; op non couverte → `Asm`→abort. **Jamais faux.** Effet mesuré
  (sqlite3.exe MSVC) : `round`, division/format flottants qui abortaient tournent, bit-identiques à Wine.
- **Incréments 2 et 3 (commits `315ebbd`/`96d94e6`) — RÉVOQUÉS (`77155e8`/`00f8163`).** Ajoutaient pile
  partagée + compares/status word/**transcendantes** (fyl2x/f2xm1/fscale/fsin/fcos…) + fp-return des
  appels indirects (drapeau `__aret_x87_ret_valid`). En apparence : `power`/`exp`/`log`/`sin(0)`/`sqrt`
  bit-identiques à Wine. **MAIS un différentiel large a exposé un FAUX SILENCIEUX** : `sin(1)` →
  `0.7456` = **`sin(sin(1))`** (double application ; idem `cos(1)`). Le bug était masqué dans mes tests
  car je n'avais essayé que `sin(0)` (où `sin(sin(0))=0` = résultat correct par coïncidence). Cause non
  encore élucidée (double application sur le chemin `fsin`/`fcos` de la CRT liftée — probable interaction
  reconnaissance-fp-par-nom × calcul lifté). **Un faux présenté comme correct viole le principe sacré →
  révoqué**, retour à l'incrément 1 (sound : `sin`/`sqrt` → abort propre, jamais faux).
- **Leçon (le principe sacré en action)** : le différentiel **large** contre Wine (pas juste quelques
  cas) est ce qui a rattrapé le faux silencieux **avant** qu'il ne soit cru. Un test trop étroit
  (`sin(0)`) l'avait masqué. Le filet reste à compléter (transcendantes correctes) en session dédiée,
  avec un différentiel math **exhaustif** comme garde. L'incrément 1 (arith/load/store/round) est acquis
  et sound.
- **Régression (état conservé, incrément 1)** : difftest 271/271, transpile-diff 4/4 (hash inchangé),
  cargo, winediff 35/35.

### Phase 5 — Filet x87 runtime : transcendantes RÉSOLUES (cause racine C2) + garde permanente ✅
- **2026-07-03 (suite) — le faux silencieux `sin(1)→sin(sin(1))` diagnostiqué et corrigé à la racine.**
  Forensics : `__x87rt_sin` seul est correct (fixture `fsin` isolée = 0.841471) ; une fonction *nommée*
  `cos` faisant fsin est correcte aussi → ni le helper ni la reconnaissance-par-nom. La double
  application venait de la **CRT sin/cos** (`sub_4e5750`) qui fait une **réduction d'argument pilotée
  par le bit C2** du status word : `fsin; fnstsw; test C2` — si C2=1 (|x|≥2^63) elle réduit et refait
  fsin. Or `__x87rt_sin/cos/sincos/ptan` **ne touchaient pas le status word** → C2 lu périmé (1) →
  chemin de réduction pris à tort → 2ᵉ fsin. **Fix** : ces helpers effacent C2 (bit 10) — notre libm
  hôte gère toute plage, donc toujours « dans la plage » (C2=0). Réappliqué (commit `37eed79`).
- **Vérifié par différentiel math EXHAUSTIF** (sin/cos/tan/sqrt/power/exp/log/asin/acos/atan/round sur
  ~50 valeurs, imbriquées) = bit-identique à Wine. Le test étroit initial (`sin(0)`, où `sin(sin(0))=0`)
  avait masqué le bug — **leçon** : différentiel large obligatoire pour les transcendantes.
- **Garde permanente** : `bench/winecorpus/x87_trig.c` exerce `fsin`/`fcos` + le **contrat C2** explicite
  (in-range ⇒ C2=0) sur le chemin runtime — rattraperait toute régression du double. **winediff 35→36.**
- **Résultat** : le vrai `sqlite3.exe` MSVC exécute son moteur SQL **et toute sa math scalaire flottante**
  nativement, bit-identique à Wine. (Seul `atan2` abort : appel indirect non récupéré `0x4e4a70` —
  trou de récupération Phase 3 sans rapport avec le x87, sound.)
- **Régression PASS** : difftest 271/271, transpile-diff 4/4 (hash inchangé), cargo, **winediff 36/36**,
  CRUD/GROUP BY/avg/window inchangés.
- **Leçon méta (principe sacré)** : le différentiel *large* contre Wine a d'abord rattrapé un faux
  silencieux (→ révocation honnête), puis validé le vrai fix. La discipline « ne jamais présenter un
  faux comme correct » a fonctionné de bout en bout, y compris contre ma propre hâte.

### Méthode — balayage systématique de la surface d'un vrai binaire (sqlite_sweep) ✅
- **2026-07-03 — comble un trou de méthode réel** : les différentiels existants couvraient le corpus
  interne (difftest/transpile-diff) et de **petits programmes synthétiques** (winediff), mais **aucun
  balayage large de la surface fonctionnelle d'un vrai binaire cible**. La « complétude sqlite » était
  *affirmée* jalon par jalon selon les requêtes qui se trouvaient testées — jamais **mesurée**. C'est ce
  trou qui a laissé « math COMPLET » s'écrire avec un `sin` cassé (rattrapé par un test large *a
  posteriori*).
- **`bench/sqlite_sweep.sh` + `bench/sqlite_sweep.sql`** : télécharge le vrai `sqlite3.exe` MSVC (3.40.1,
  versionné/public, caché), le transpile avec ARET, exécute une **batterie DÉTERMINISTE large** (30 aires
  étiquetées) et diffe **ARET vs Wine** (vérité terrain). Toute ligne divergente = un trou/bug réel.
  SKIP propre si wine/réseau/binaire absents.
- **Résultat mesuré (pas affirmé)** : **30/30 aires bit-identiques à Wine** — select/where, agrégats,
  HAVING, joins (inner/left), sous-requêtes, CTE (+récursive), UNION/INTERSECT/EXCEPT, window (row_number/
  rank/dense_rank/lag/lead/frames/ntile), strings, printf/quote/hex/unicode, **toute la math scalaire**,
  LIKE/GLOB/ESCAPE, CASE/coalesce/nullif, cast/typeof, JSON, dates, group_concat ORDER BY, DISTINCT/
  FILTER, UPDATE…RETURNING, collate NOCASE, index partiel, vues, colonnes générées STORED, triggers.
- **Trous connus (sound, non silencieux)** : `atan2` (appel indirect non récupéré `0x4e4a70`, axe
  récupération Phase 3) ; **non balayés** : FTS/RTREE/tables virtuelles/ATTACH/backup/I-O blob — pourraient
  buter, aborteraient (jamais faux). **« Fonctionnel prouvé là où balayé + sound partout »**, pas « 100 % ».
- **Leçon systématisée** : mesurer la surface d'un vrai binaire est désormais un **outil répétable**, pas
  un poke ad-hoc. À faire pour Lua/busybox aussi.

### Résolution — x87 : mode d'arrondi prouvé (ceil/floor) + récupération des leaf-thunks (atan2/fmod/trunc) ✅
- **2026-07-03 — le balayage large a rattrapé un faux silencieux ET des trous sound.** Le sweep
  systématique + un différentiel math *exhaustif* (252 cas ceil/floor/trunc/mod/atan2/… × args aléatoires)
  contre le vrai `sqlite3.exe` sous Wine a révélé **deux classes** de défaut restées invisibles aux tests
  étroits :
  1. **`ceil(3.2)=3.0` / `floor(3.8)=4.0` — FAUX SILENCIEUX (violation du principe sacré).** La CRT MSVC
     implémente `ceil`/`floor`/`trunc` par `fld [esp+d]; fstcw; **mov reg,IMM; or reg,[cw]; and reg,MASK**;
     fldcw; frndint; fldcw; ret` — le champ RC (bits 10-11) est **forcé** par l'immédiat du `or` (met un
     bit) combiné à celui du `and` (efface l'autre). `rounding_mode_active` ne reconnaissait que la forme
     `or imm, cw` (immédiat unique) ; l'idiome réel `or reg,[cw]` (or avec la **mémoire** vivante) n'était
     pas prouvé → repli sur `Nearest` → `frndint` faisait un arrondi-au-plus-proche → **ceil qui n'arrondit
     pas vers le haut**. Piège classique : `ceil(3.0)` (nearest et up coïncident sur les entiers) masquait
     le bug ; seul un argument fractionnaire le révèle.
     - **Fix (`src/ir/build.rs`)** : `rounding_mode_active` renvoie désormais `Option<RoundMode>` et
       `rc_installed_by_store` **interprète abstraitement** les deux bits RC (10, 11) de la valeur écrite
       dans le slot du control word — `mov reg,IMM` initialise, `or imm` force à 1, `and imm` force à 0,
       `or/and [cw]|reg` (mot ancien inconnu) préserve un bit déjà connu sinon le rend inconnu. Gère les
       sous-registres (`or ah,0xc` pour trunc). Les deux bits doivent finir **définis** sinon `None`.
     - **Principe sacré recâblé** : un `frndint` dont le mode n'est **pas** prouvé (`None`) ne se rabat
       **plus** sur nearest (ça *expédiait* un ceil faux) — il **fait échouer la fonction entière**
       (`X87Bail`), qui bascule alors sur la pile x87 runtime (qui suit le control word) ou une décompile
       inline-asm saine. Plus jamais de mode d'arrondi *deviné*.
  2. **`atan2`/`fmod`/`trunc` — abort sound (trou de récupération, jamais faux).** Ces fonctions sont de
     minuscules leaf-helpers CRT (`fld [esp+d]; …; fpatan|fprem|frndint; ret`) atteintes **uniquement** par
     un pointeur isolé (user-data d'une fonction SQL — pas un run ≥3 que l'heuristique de table valide).
     Leur prologue `fld m64,[esp+d]` ne matchait aucune forme de `looks_like_func_start` → le pointeur
     restait de la donnée → l'appel indirect atterrissait sur du code non récupéré → **abort** (sound).
     - **Fix (`src/analysis/mod.rs`)** : `is_x87_leaf_thunk` **décode tout le corps** depuis l'adresse — il
       *commence* par un chargement x87 d'argument pile (`fld|fild [esp+d]`), n'est composé que d'ops FPU +
       la colle entière que ces helpers utilisent (ajustement pile, manip du control word, `sahf`), avec
       une seule branche conditionnelle **locale** (la boucle de complétion `fprem`), et se termine sur un
       `ret` sous une borne serrée. Signal *corps entier* bien plus fort qu'un motif d'octets de prologue —
       assez fort pour amorcer une fonction depuis un pointeur de donnée isolé sans risque de faux positif
       (de la donnée aléatoire ne se décode pas en un leaf x87 propre finissant par `ret`).
     - **Bug attrapé en cours** : une branche conditionnelle locale validée passait le test de flow puis
       échouait le test de mnémonique (« x87 ou colle ») car un `jcc` n'est ni l'un ni l'autre → `fmod`
       (seul helper avec la boucle `jp`) restait non récupéré. Corrigé : une `CondJump` locale validée est
       acceptée telle quelle.
- **Résultat mesuré** : `ceil`/`floor`/`trunc`/`atan2`/`fmod` = **bit-identiques à Wine**. **sqlite feature
  sweep : ALL bit-identical** (29 aires + DDL/DML). **Différentiel math 252/252.** Plus aucun `<none>` ni
  faux sur la math scalaire du vrai binaire.
- **Garde permanente** : `bench/winecorpus/x87_round.c` reproduit l'idiome `mov reg,IMM; or reg,[cw]; and
  reg,MASK; fldcw; frndint` en asm inline (mode d'arrondi à prouver) + `fpatan` + la boucle `fprem` —
  rattraperait toute régression des deux classes. **winediff 36→37.**
- **Régression PASS** : cargo (54+48+3), difftest **271/271**, transpile-diff **4/4** (hash inchangé),
  winediff **37/37**, sqlite sweep **bit-identique**, math **252/252**.
- **Réponse à « pourquoi ce balayage n'est pas fait d'office ? »** : il l'est désormais — `sqlite_sweep`
  est le balayage large répétable, et c'est *précisément* lui (pas un poke étroit) qui a exposé ces deux
  défauts. Le faux `ceil` prouve une fois de plus la leçon : la complétude se **mesure** contre la vérité
  terrain sur un vrai binaire, elle ne s'**affirme** pas jalon par jalon.

### Outil — couverture d'imports statique (`--mode imports`) : mesurer l'axe 2 D'AVANCE ✅
- **2026-07-03 — opérationnalise « mesurer plutôt qu'affirmer » pour l'axe 2.** Discussion de fond
  (avec l'utilisateur) sur la nature du problème : les « aires » d'un binaire (FTS5, RTREE, JSON…) ne
  sont **pas** des aires qu'ARET implémente — une feature applicative n'est, une fois compilée, que
  (a) des instructions x86 (axe 1, fini, balayé par cpudiff/Unicorn) + (b) des appels OS/CRT (axe 2,
  la table d'imports). Donc **on ne couvre pas des features une par une** ; on ferme deux dimensions
  finies et **connues d'avance**. Pour l'axe 2, « connu d'avance » = la table d'imports du PE se lit
  **statiquement**.
- **`--mode imports`** : classe **toute** la table d'imports d'un binaire contre l'ensemble des shims
  livrés (`implemented_shims()` — parse HLE/CRT/Win32 + macros génératrices), et sort couvert / non
  couvert (%) **+ la liste nominative du trou d'axe 2**. Complémentaire du verdict transpile, qui ne
  liste que les imports *effectivement appelés* par le code récupéré (plafonné, dépend de la
  récupération) : ici c'est **a-priori**, indépendant de la récupération — la vraie mesure de
  préparation d'ARET pour un binaire.
- **Honnêteté du tampon (principe sacré appliqué à l'outil lui-même)** : un import *fonction* sans shim
  touche le stub faible au runtime (vrai trou) ; un import *donnée* (`_iob`, `__initenv`, `__mb_cur_max`)
  peut être satisfait par le chemin IAT/layout. Le rapport dit donc « trou de **shim** » = **borne
  supérieure conservatrice** du vrai trou runtime, jamais une sous-estimation. Formulation calibrée pour
  ne pas sur-affirmer.
- **Mesuré** : `hello_realcrt.exe` 53 imports → 50 couverts (94 %), trou = `__initenv`/`__mb_cur_max`/
  `_iob` (données) ; un exe synthétique à API rares → 87 %, trou nominatif (`FindFirstChangeNotificationA`,
  `GetEnvironmentVariableW`, `GetTickCount64`). Zéro dépendance toolchain (lecture du loader seule).
- **Fixture permanente** : `tests/import_coverage.rs` (2 tests) — cas *entièrement couvert* (verdict
  FULLY COVERED, compte = table PE) et cas *trou réel* (liste nominative, somme couvert+non-couvert =
  total, jamais de FULLY COVERED menteur).
- **Régression** : cargo **54+2** verts, transpile-diff **4/4** (hash `19acad982194bf07` inchangé — outil
  de lecture, zéro dérive sémantique).
- **Cadre** : reformule « élargir le balayage » — au lieu de couvrir RTREE/FTS5 une par une, la question
  générale et **bornée** devient « ce binaire importe-t-il un appel Win32/CRT non shimé ? », réponse
  statique et énumérable. Prochain levier de diversité : passer quelques binaires variés à `--mode
  imports` pour prioriser les shims généraux qui débloquent le plus de binaires (jamais une rustine
  par binaire).

### Shims — famille stat msvcrt + groupe fichiers CRT (débloque le sweep on-disk) ✅
- **2026-07-03 — mesure d'abord, puis fix général.** `--mode imports` sur le vrai `sqlite3.exe` listait
  41 imports non shimés, dont un groupe fichiers/répertoires. **Mais on ne devine pas depuis la liste**
  (elle contient des chemins morts/diagnostic) : j'ai *reproduit* un workload **sur disque** vs Wine.
  Résultat mesuré : le cœur on-disk (sweep complet sur fichier, persistance inter-process, `ATTACH`,
  `.backup`) **marchait déjà** bit-identique ; **seul `.read`** (import d'un script SQL) divergeait —
  et **bruyamment** (`Error: cannot open`, jamais un faux). Cause exacte tracée : `.read` appelle
  **`_fstat`** (non shimé → stub faible qui ne remplit pas le buffer) → sqlite lit un stat nul → échoue.
- **Fix général (`runtime/aret_hle/aret_hle.c`)** : famille stat msvcrt + groupe fichiers CRT —
  `_fstat`/`_stat`/`_stati64`/`_fstati64`, `_mkdir`, `_unlink`, `_getpid`. Marshalling **ABI-exact** :
  `struct _stat` (36 o) et `struct _stati64` (48 o) ont une disposition Windows **fixe** qui **ne
  correspond pas** à une struct i386 naturelle — MSVC aligne le `__int64 st_size` sur 8 octets (offset
  24), l'ABI i386 SysV l'aligne sur 4 → une struct naturelle mettrait `st_size` à 20 et **décalerait
  tous les champs suivants** (taille fausse silencieuse). Donc **écriture à offsets d'octets explicites**,
  seule voie sûre. `st_mode` traduit POSIX → bits msvcrt (`_S_IFDIR`/`_S_IFREG`/`_S_IFCHR` + permissions
  reflétées) pour les tests is-dir/is-reg que font les programmes.
- **Validation ABI (principe sacré appliqué au marshalling)** : garde permanente
  `bench/winecorpus/crt_stat.c` — `_stat`/`_fstat`/`_stati64` sur un fichier de taille connue + un
  répertoire, compare les champs **déterministes et sémantiques** (`st_size` exact, classification
  reg/dir) contre Wine ; dev/ino/uid/timestamps (spécifiques à l'hôte) volontairement non comparés.
  **Bit-identique à Wine → l'ABI est prouvée, pas juste « `.read` passe »**. winediff 37→38.
- **Sweep élargi (mesurer, pas affirmer)** : `bench/sqlite_sweep.sh` a désormais une **passe on-disk** en
  plus de `:memory:` — même batterie de features sur une base **fichier**, + persistance inter-process,
  `ATTACH`, `.backup`, `.read`. Résultat : **bit-identique à Wine** sur les deux passes. La couche
  fichier est maintenant *mesurée* en continu, plus supposée.
- **Couverture** : sqlite 146→**151** shims couverts (78 %→80 %). `.read` fonctionne (`42` = Wine).
- **Reste du groupe (honnête, non bloquant)** : `_findfirst`/`_findnext`/`_findclose` **différés** — ABI
  distincte et plus risquée (`struct _finddata_t` + expansion glob `*.txt`), et **non déclenchés** par
  les workloads mesurés (aucune divergence sans eux). À faire dans un commit dédié *quand* un workload
  mesuré les exige (listing de répertoire, `.import` glob). Idem `_popen`/`_pclose` (shell) et
  `LoadLibraryW` (extensions) — chemins non balayés, sound s'ils sont atteints.
- **Régression** : cargo **54+2**, transpile-diff **4/4** (hash `19acad982194bf07` inchangé — shims
  runtime, zéro effet sur le lifting), **winediff 38/38**, **sweep :memory: + on-disk bit-identiques**.

### Outil + mesure — balayage de corpus (`corpus_sweep.sh`) : prioriser les shims par la donnée ✅
- **2026-07-03 — « mesurer large » pour orienter l'axe 2, sans exécuter.** Suite logique de `--mode
  imports` : au lieu de creuser un seul binaire, on **mesure un corpus varié** en STATIQUE (lecture du
  loader + classification de la table d'imports — sûr sur n'importe quelle provenance, aucune exécution).
  `bench/corpus_sweep.sh` prend un dossier de PE, sort par binaire {imports, couverts, %} (+ verdict de
  solidité avec `DEEP=1`), puis **agrège** : combien de binaires distincts réclament chaque import non
  shimé → le **haut de la liste = les shims généraux qui débloquent le plus de programmes**. SKIP propre
  sans corpus. Les binaires ne sont PAS commités (tiers/volumineux) ; on pointe l'outil sur un dossier.
- **Corpus mesuré** (6 vrais PE 32-bit variés, provenance sûre : busybox, plink/pscp PuTTY, 7za,
  sqlite3, + une variante UPX de busybox) :

  | binaire | imports | couverts | % |
  |---|---|---|---|
  | sqlite3 | 187 | 151 | **80 %** |
  | busybox_upx | 8 | 5 | 62 %* |
  | 7za | 140 | 85 | 60 % |
  | pscp | 145 | 85 | 58 % |
  | busybox | 307 | 174 | 56 % |
  | plink | 145 | 82 | 56 % |

  \* le binaire packé n'expose que les imports du **stub** du packer (8) — il faut `--mode unpack` d'abord.
  Confirme empiriquement qu'un PE packé ne se mesure pas tel quel.
- **Classement mesuré des shims manquants les plus rentables** (nb de binaires) : `TerminateProcess`(5),
  `SetEndOfFile`(5), `FormatMessageA`(5), `WaitForMultipleObjects`(4), `UnhandledExceptionFilter`(4),
  `SetFileTime`(4), `SetConsoleMode`(4), `GetProcessTimes`(4), `EqualSid`(4), puis un cluster
  **process/thread/pipe** (`CreateThread`/`CreateProcessA`/`CreatePipe`/`CreateNamedPipeA`/`OpenProcess`/
  `GetExitCodeProcess`, 3 chacun), **fichier-métadonnées** (`SetStdHandle`/`SetHandleInformation`/
  `LocalFileTimeToFileTime`), **exceptions** (`RtlUnwind`/`RaiseException`), **env**
  (`GetEnvironmentStringsW`/`FreeEnvironmentStringsW`).
- **Lecture honnête** : couverture ~56–60 % pour un vrai binaire full-CRT typique (sqlite à 80 % car
  déjà travaillé). **Tous INCOMPLETE** au verdict — normal et conservateur (un seul import non shimé,
  même sur chemin mort, suffit ; ce n'est PAS un échec runtime : sqlite est INCOMPLETE *et* bit-identique
  à Wine). Le verdict est une borne supérieure de risque statique, pas un pronostic d'exécution.
- **Priorisation qui en découle** (valeur × sûreté), pour un prochain lot **général** (jamais par binaire) :
  1. **Trivialités haute-fréquence** : `SetEndOfFile` (=ftruncate), `SetFileTime`/`LocalFileTimeToFileTime`
     (=utimes), `FormatMessageA` (formatage d'erreur), `GetExitCodeProcess`, `SetStdHandle`,
     `SetConsoleMode`, `sscanf`, `putc` — ~15 lignes chacun, débloquent 2–5 binaires.
  2. **Cluster process/thread/pipe** : `CreateThread`/`CreateProcessA`/`CreatePipe`/`WaitForMultipleObjects`
     — vrai modèle (threads/process POSIX), plus lourd mais très rentable (shells, SSH, archiveurs).
  3. **SID/sécurité** : `EqualSid`/`GetUserNameA` — stubs raisonnables.
- **Régression** : outil de lecture seule ; n'affecte ni le lifting ni le runtime.

### Mesure — corpus WineTest (367 modules de conformance) : axe 2 chiffré à l'échelle ✅
- **2026-07-03 — la meilleure source possible, mesurée.** `winetest.exe` (build CI winehq, 91 Mo, 32-bit)
  **bundle ~380 modules de test par-DLL** en PE embarqués. Carvés (scan MZ/PE) → 367 modules mesurables,
  passés au `--mode imports` (statique, aucune exécution). Ces modules sont **auto-oracles** (tests de
  conformance : comportement connu-correct, cross-validé Windows↔Wine) et **énumèrent toute la surface
  API** — les « aires », déjà écrites par les auteurs Wine.
- **Résultat chiffré (mesuré, pas affirmé)** : couverture d'imports par module — **min 0, p25 57 %,
  médiane 75 %, p75 85 %, max 100 %**. **Le module de conformance médian est déjà couvert à 75 %** par le
  HLE actuel d'ARET. Signal fort de préparation axe 2 sur du code de test qui pousse les cas limites.
- **Classement des manquants sur TOUTE la suite** (nb de modules, top) : **`SetConsoleMode` (343) et
  `GetExitCodeProcess` (343)** dominent — présents dans quasi **chaque** module (le harness winetest les
  appelle au démarrage de chaque test), **et triviaux à shimer**. Puis `IsBadStringPtrW/A` (188/94),
  cluster **COM** (`CoUninitialize`/`CoCreateInstance`/`CoInitialize`, 100-122), `lstrcmpW` (104),
  temp-fichiers (`GetTempPathW` 78, `GetTempFileNameA/W` 34/31), sync/thread (`CreateEventW` 74,
  `CreateThread` 59, `CreateProcessA` 41), **USER32** (`DestroyWindow`/`CreateWindowExA`/`DefWindowProcA`
  — longue traîne GUI), **registre** (`RegCloseKey`/`RegOpenKeyExA`/`RegQueryValueExA` — besoin d'un
  backing store), **ressources PE** (`LoadResource`/`SizeofResource`/`FindResourceW`), **BSTR/OLE**
  (`SysAllocString`/`SysFreeString`/`VariantClear`), **WinRT** (`Ro*`/`Windows*String`), `_assert`,
  `_vsnwprintf`.
- **Leçon (mesurer > deviner)** : jamais je n'aurais priorisé `SetConsoleMode`/`GetExitCodeProcess` — le
  balayage large les fait remonter en tête, mécaniquement. **La priorisation devient une donnée, pas une
  intuition.** Le tri valeur × sûreté qui en découle pour un lot **général** :
  1. **Triviales ultra-fréquentes** : `GetExitCodeProcess`, `SetConsoleMode`, `lstrcmpW`,
     `IsBadStringPtrA/W`, `GetTempPathW`/`GetTempFileNameA/W`, `_assert`, `_vsnwprintf` — ~15 lignes,
     validées contre Wine, débloquent des **centaines** de modules d'un coup.
  2. **COM/OLE minimal** : `CoInitialize(Ex)`/`CoUninitialize`/`CoTaskMemFree`, `SysAllocString`/
     `SysFreeString` — init/alloc simples (le vrai `CoCreateInstance` reste longue traîne).
  3. **Registre** (backing store) et **USER32/WinRT** = longue traîne, Phase 7+ (Winelib candidat idéal).
- **Note honnête** : les noms de module par-PE ne sont pas capturés (l'index winetest demande un parsing
  de la table de ressources — différé) ; l'**agrégat d'imports** (le livrable priorisant) est indépendant
  des noms. `corpus_sweep.sh` couvre le format (SKIP propre, agrégation), les binaires restent non commités.

### Shims — lot 1 winetest (SetConsoleMode + BSTR + COM minimal), priorisé par la donnée ✅
- **2026-07-03 — d'abord re-trier par *fidélité*, pas juste par fréquence.** Le classement winetest
  mettait `GetExitCodeProcess`/`SetConsoleMode` en tête (343 chacun). Re-triage honnête avant d'écrire :
  `GetExitCodeProcess` est **stateful** (couplé au cluster process) → **différé** ; `lstrcmpW` est une
  **collation locale** (pas un `wcscmp`) → différé ; `IsBadStringPtr` exige un **vrai sondage mémoire** →
  différé. Ne garder que le sous-ensemble qu'on peut rendre **exact** (principe sacré : pas d'approximation
  silencieuse).
- **Implémenté (haute confiance, tous validés bit-à-bit contre Wine)** :
  - `SetConsoleMode` (`aret_win32.c`) — miroir exact de `GetConsoleMode` existant : console ssi `isatty`
    → sur handle redirigé (pipe = harness winetest/diff), renvoie **FALSE** comme Windows/Wine.
  - **BSTR oleaut32** — `SysAllocString`/`SysAllocStringLen`/`SysFreeString`/`SysStringLen`/
    `SysStringByteLen` : ABI **contractuelle** `[u32 byteLen][wchar[len]][NUL]`, pointeur retourné *après*
    le préfixe → impl. native **exacte** (pas approximative).
  - **ole32 minimal** — `CoInitialize`/`CoInitializeEx` (S_OK au 1er init, **S_FALSE** imbriqué, via
    compteur), `CoUninitialize`, `CoTaskMemAlloc`/`Realloc`/`Free` (=malloc/realloc/free). Exact pour le
    code qui n'init COM et (dé)alloue ; le vrai `CoCreateInstance` reste longue traîne.
- **Garde permanente** : `bench/winecorpus/win32_console_ole.c` exerce les trois groupes (console
  redirigé → FALSE ; layout BSTR : longueur/accès char/round-trip ; COM : S_OK/S_FALSE + alloc round-trip)
  et **matche Wine bit-à-bit**. `bench/winediff.sh` lie désormais `-lole32 -loleaut32`. **winediff 38→39.**
- **Gain MESURÉ (pas affirmé)** sur les 367 modules winetest : **médiane 75 % → 78 %** (p25 57→61,
  p75 85→87 — toute la distribution monte). `SetConsoleMode` **disparaît** du sommet des trous ; BSTR/COM
  aussi. Nouveau #1 : `GetExitCodeProcess` (le stateful correctement différé).
- **Régression** : cargo **54+2**, transpile-diff **4/4** (hash inchangé — shims runtime), winediff **39/39**.
- **Leçon** : « priorisé par la donnée » **puis** « filtré par la fidélité » — la fréquence dit *quoi*
  regarder, le principe sacré dit *quoi expédier*. Prochain lot : `GetTempPathW`/`GetTempFileName`
  (fonctionnel), puis le cluster process/thread (`GetExitCodeProcess`/`CreateProcess`/`CreateThread` —
  vrai modèle), et évaluer `IsBadStringPtr` avec un sondage `/proc/self/maps`.

### Shims — groupe temp-fichiers (GetTempPathW + GetTempFileNameA/W + _wremove) ✅
- **2026-07-03 — lot suivant priorisé par la donnée** (winetest : `GetTempPathW` 78, `GetTempFileName*`
  34/31 ; aussi dans le trou sqlite). `GetTempPathA` existait ; ajout de `GetTempPathW`,
  `GetTempFileNameA`/`GetTempFileNameW` (`aret_hle.c`, avec `translate_path`/`aret_n2w`/`aret_w2n`) +
  correctif `GetTempPathA` (garantie de séparateur final). `GetTempFileName` compose
  `<dir>\<pre><hhhh>.TMP` et, si `unique==0`, **trouve un nom libre et CRÉE** le fichier vide
  (`O_CREAT|O_EXCL`), créé à `translate_path(nom)` pour que la ré-ouverture par l'appelant tombe sur le
  même fichier.
- **Validation FONCTIONNELLE (le chemin natif diffère légitimement de Windows)** :
  `bench/winecorpus/win32_tempfile.c` valide le **contrat**, pas la chaîne — chemin non vide finissant par
  un séparateur, fichier temp créé/unique/inscriptible, round-trip write/read identique. Bit-à-bit avec
  Wine. **winediff 39→40.**
- **Attrapé en passant** : la garde utilisait `_wremove` (non shimé) → l'avertissement `aret_unimpl` (sur
  **stderr**, vérifié — pas de pollution stdout) était capturé dans le bloc de sortie par `--run` →
  divergence. Fix : shim `_wremove` (sibling wide de `remove()`, complète la couche fichier wide) —
  légitime, pas une rustine.
- **Gain MESURÉ** (367 modules winetest) : médiane **78 % → 79 %** ; `GetTempPathW` disparaît des trous.
- **Régression** : cargo **54+2**, transpile-diff **4/4** (hash inchangé), **winediff 40/40**.

### Cluster process/thread/pipe — audit du modèle + part fidèle (CreatePipe) ✅ / threads = chantier dédié
- **2026-07-03 — « proprement » = auditer AVANT d'écrire.** Le cluster (nouveau #1 winetest :
  `GetExitCodeProcess` 343, `CreateProcessA`, `CreateThread`, `WaitForMultipleObjects`, `CreatePipe`…)
  n'est PAS uniforme. Audit du modèle d'exécution ARET :
  - **Registres** = variables C locales par appel (`uint64_t v0,v7,…`) → **thread-safe par construction**.
  - **TEB/PEB** = `static uint8_t aret_teb[0x1000]` **global**, **`g_last_error` global** → **PAS**
    thread-local. La **pile machine** est une **région unique partagée**.
  - ⇒ le modèle est **fondamentalement mono-thread** aujourd'hui.
- **Conséquence (principe sacré : ne jamais simuler ce qu'on ne peut rendre fidèle)** :
  - **`CreatePipe` (anonyme)** → mappe exactement sur `pipe()` POSIX (le HANDLE de ce modèle == fd, donc
    WriteFile/ReadFile/CloseHandle marchent dessus). **Ajouté, fidèle, validé** :
    `bench/winecorpus/win32_pipe.c` (round-trip write/read) = bit-à-bit Wine. **winediff 40→41.**
  - **Déjà sound en mono-thread** (aucune action) : `GetCurrentProcess/Thread/ThreadId` (existants),
    `CriticalSection` Init/Enter/Leave (no-op — **correct sans concurrence réelle**),
    `WaitForSingleObject` (WAIT_OBJECT_0 immédiat — cohérent sans objet bloquant réel), `CloseHandle`.
  - **`CreateThread`/`CreateProcessA/W`/`OpenProcess`/`TerminateProcess`** → **NON simulés** : ils
    échouent (stub faible → 0/FALSE, sound) plutôt que feindre un thread qui *race* ou un enfant qui ne
    tourne jamais (ce serait un faux silencieux). `CreateProcess` est une **frontière dure** : il n'y a pas
    de Windows pour exécuter un `.exe` enfant.
- **Chantier threads (dédié, quand un binaire cible réel l'exige)** — le chemin est clair, mesuré :
  1. **TEB + last-error thread-locaux** (`__thread`) — fondation.
  2. **Pile machine par thread** dans `CreateThread` (malloc 32-bit, `__esp` initial au sommet) +
     dispatch `aret_call(startAddr, esp, param)` (ABI stdcall du thread-proc sur la pile partagée).
  3. **Sync réelle** : `CRITICAL_SECTION` → `pthread_mutex` **récursif**, `CreateEvent`/`SetEvent`/
     `WaitForSingleObject`/`MultipleObjects` → `pthread` cond/join.
  4. **Validation MT** : garde multi-thread vs Wine (N threads + compteur sous section critique →
     somme déterministe ; signalisation d'événement → déterministe).
- **Note honnête de priorité** : les démonstrateurs actuels (sqlite CLI, Lua, la plupart des applets
  busybox) sont **mono-thread** → le chantier threads est de la **complétude** (couvre les binaires
  thread-lourds / la suite winetest), pas un déblocage des cibles actuelles. À lancer sur demande ou quand
  un binaire mesuré le réclame — pas spéculativement.
- **Régression** : cargo, transpile-diff (hash inchangé), **winediff 41** (à confirmer).

### Shims — groupe fichier-métadonnées (SetEndOfFile + SetFileTime/GetFileTime + Local↔File) ✅
- **2026-07-03 — priorisé par le CROISEMENT winetest × corpus réel.** Nuance de méthode : winetest (code
  de test) sur-pondère les fonctions du harness ; on croise donc avec les 6 vrais binaires pour ne pas
  sur-ajuster. Le croisement (haut dans le corpus RÉEL) pointe le groupe fichier-métadonnées :
  `SetEndOfFile` (5), `SetFileTime` (4), `LocalFileTimeToFileTime` (3) — fidèle et validable.
- **Implémenté (`aret_hle.c`)** : `SetEndOfFile` (=ftruncate au SEEK_CUR), `SetFileTime`/`GetFileTime`
  (FILETIME↔timespec via futimens/fstat ; pas de birthtime Linux → creation = ctime, comme Wine ; champ
  NULL/zéro laissé inchangé via `UTIME_OMIT`), `LocalFileTimeToFileTime`/`FileTimeToLocalFileTime`
  (décalage constant par le biais TZ *courant* — `tm_gmtoff` — comme Windows ; identité sous TZ=UTC).
- **Validation FONCTIONNELLE (round-trip déterministe, indépendant de l'hôte)** :
  `bench/winecorpus/win32_filetime.c` — SetEndOfFile tronque à 40 o ; SetFileTime sur un instant à la
  seconde (granularité FS non pertinente) → GetFileTime round-trip exact ; Local↔File identité sous UTC.
  **Bit-à-bit Wine. winediff 41→42.**
- **Gain mesuré** : `SetEndOfFile`/`SetFileTime` **disparaissent** du sommet des trous du corpus réel.
  Restent en tête : le **cluster process/thread** (TerminateProcess/WaitForMultipleObjects/GetProcessTimes/
  CreateThread — chantier threads), `FormatMessageA` (chaînes fragiles), `EqualSid`/`UnhandledExceptionFilter`.
- **Constat** : les trous **fichier/CRT faciles et fidèles** du corpus réel sont désormais **quasi
  épuisés**. Le reste est soit le chantier threads, soit des sous-systèmes à fidélité délicate (messages
  d'erreur exacts, SID/sécurité, SEH). Prochain choix de valeur × sûreté à faire en conséquence.
- **Régression** : cargo **54+2**, transpile-diff **4/4** (hash inchangé), **winediff 42/42**.

### Axe 1×2 — vrai binaire de bout en bout (busybox) : printf %I64 corrigé ✅
- **2026-07-03 — mesurer le PRODUIT, pas un proxy.** Le statique (couverture d'imports) *ment* sur le
  fonctionnel (sqlite est « INCOMPLETE » à 80 % *et* marche). Seul un vrai binaire lancé de bout en bout
  vs Wine dit « où on en est vraiment » — et fait remonter le prochain bug **réel** (corrigé général).
  busybox-w32 (des dizaines d'outils Unix en un binaire) = énorme surface fonctionnelle.
- **Piège d'invocation (pas un bug ARET)** : busybox choisit l'applet par `argv[0]` (mode multiplexeur
  seulement si `basename(argv[0])=="busybox"`). Nommé « app », il répond « applet not found » — **exactement
  comme Wine avec le même renommage** (vérifié). Nommé `busybox`, tout tourne (`busybox echo hello` → OK).
- **Bug RÉEL trouvé et corrigé (général)** : busybox `expr` formate son résultat avec **`%I64d`** (préfixe
  de taille 64-bit MSVC). glibc ne connaît pas `I64` → `aret_vformat` tombait sur le défaut « spec
  littérale » et imprimait **`%I64d`** au lieu de `42`. **Fix** : le reformatteur partagé traduit le
  préfixe MSVC — `I64` → `ll` (64-bit), `I32` → int 32-bit, `I` nu → largeur pointeur (32-bit Win32) —
  au lieu de le copier dans la spec glibc. **Général** : bénéficie à tout binaire MSVC utilisant le printf
  64-bit (un import unique : le reformatteur `aret_vformat` sert printf/sprintf/snprintf/vsnprintf).
- **Vérifié** : busybox `expr 100+23`/`6+7`/`50-8`/`12/4` = **bit-identique à Wine** ; garde permanente
  `bench/winecorpus/crt_printf_i64.c` (`%I64d/u/x/X`, négatifs, largeur `%08I64x`, mix) = bit-à-bit Wine.
  **winediff 42→43.**
- **Régression** : cargo **54+2**, transpile-diff **4/4** (hash inchangé), winediff **43/43**.
- **Leçon** : un vrai binaire varié a immédiatement exposé un bug printf **général** qu'aucun test synthétique
  ni sweep statique n'avait révélé. C'est le meilleur révélateur de « où on en est ». À systématiser
  (harness busybox).

### 🎯 Axe 1×2 — harness busybox systématisé (`busybox_sweep.sh`) → 5 bugs stdin/RNG généraux ✅
- **2026-07-04 — la leçon de l'entrée précédente exécutée : « systématiser le harness busybox ».**
  Nouveau `bench/busybox_sweep.sh` (modèle `sqlite_sweep`) : télécharge le vrai **BusyBox-w32** (MinGW
  strippé, ~390 applets en un PE, versionné/sha256/caché), le transpile, et diffe une **batterie
  DÉTERMINISTE** d'invocations d'applets — **stdout + stderr + code de sortie**, octet pour octet — contre
  le MÊME binaire sous Wine. **59/59 bit-identiques.** SKIP propre si wine/réseau/aret absents.
- **Deux invariants rendant ARET et Wine comparables** (documentés dans le script) : (1) `argv[0]` doit
  commencer par `busybox` — sinon BusyBox prend le basename pour l'applet (« applet not found »), donc les
  deux moteurs sont invoqués via un fichier nommé `busybox.exe` ; (2) **aucun métacaractère glob**
  (`* ? [`) dans les args — BusyBox-w32 est bâti avec `CRT_glob`, donc sous Wine `*` est expansé contre le
  cwd (non déterministe) alors que le transpilé ne globbe pas → pas une vérité terrain stable.
- **5 bugs HLE GÉNÉRAUX trouvés par le harness et corrigés** (couche shim, réutilisables sur tout binaire
  MSVC/MinGW) :
  1. **`CryptAcquireContextA`/`CryptGenRandom`/`CryptReleaseContext`** (advapi32) : BusyBox amorce son PRNG
     au démarrage ; sans ces shims **AUCUN applet ne se route** (« applet not found »). + `stdcall_pops`.
  2. **`_filbuf`** : primitive de remplissage derrière la macro `getc`/`getchar` ; le stub faible renvoyait
     0 sans fin → **toute lecture stdin (wc, sort, head…) bouclait à l'infini** au lieu de s'arrêter à EOF.
  3. **`getchar`** : idem quand le CRT importe l'entrée directe au lieu d'inliner `_filbuf`.
  4. **`fclose(stdin)`** : un flux `_iob` synthétique passé au `fclose()` hôte déréférence notre struct de
     32 o comme un `FILE` glibc → **segfault** (BusyBox `rev`/`nl` à la sortie). Même garde `iob_fd` que
     `fwrite`/`fputs`.
  5. **`fread(stdin)`** : même piège → crash (`od`/`cksum`) ou lecture corrompue (`base64`). Lecture fd
     directe.
  \+ **`GetEnvironmentVariableW`** (jumeau large de la variante A, retire le bruit d'un import non shimé).
- **Bug `--run` corrigé (général)** : `--mode transpile --run` utilisait `Command::output()`, qui **ferme
  le stdin de l'enfant** → tout programme lisant stdin lancé ainsi voyait un EOF immédiat (silencieusement
  vidé). Désormais `Stdio::inherit()` — un `--run < fichier` ou une pipe atteint le programme.
- **Nouvelle garde synthétique + convention `NAME.in`** : `winediff.sh` alimente `winecorpus/NAME.in` à
  l'identique aux deux moteurs ; `crt_stdin.c` exerce `getchar`→EOF (chemin `_filbuf`) + `fclose(stdin)` →
  **verrouille les fixes stdin dans la porte rapide** (indépendant de busybox). winediff **43→44**.
- **Gaps connus documentés dans le harness** (domaine récupération/lifting, PAS le HLE — prochaine cible) :
  `grep`/`sed` (SIGSEGV dans le moteur regex lifté), `cksum`/`od -tx1` (abort : appel indirect vers une
  fonction **non récupérée**), `base64` (encode encore divergent). Exclus de la porte pour qu'elle reste un
  vrai signal de régression ; **consignés, pas cachés**.
- **Régression complète PASS** : `cargo test`, **difftest 271/271**, **transpile-diff 4/4** (hash
  `19acad982194bf07` inchangé — HLE + `--run` + table de pops, zéro dérive du lifting), **cpudiff vert**
  (2000 états aléatoires), **winediff 44/44**, **busybox sweep 59/59**.
- **Leçon** : comme prédit, un vrai binaire lancé de bout en bout a immédiatement exposé un **cluster de
  bugs stdin généraux** qu'aucun test synthétique ni sweep statique n'avait révélé. Le harness est
  désormais **porte de régression verte + outil de découverte répétable** — la 3ᵉ cible (après sqlite/Lua)
  systématiquement mesurée contre Wine.

### Phase 3a — Récupération : immédiat-code atteignant un appel indirect (busybox od) ✅
- **2026-07-04 — 1er gap busybox du harness attaqué : `cksum`/`od -An -tx1` abortaient** sur « indirect
  call to unrecovered function » (`0x41abec`/`0x42f160`). Diagnostic (objdump + scan des pointeurs) : les
  deux cibles sont des pointeurs de fonction **matérialisés comme IMMÉDIATS** dans le code, puis appelés
  indirectement — mais leur callee n'a **pas de prologue reconnu** (`0x42f160` = un `ret` nu, handler
  no-op ; `0x41abec` = une fonction regparm débutant `add edx,ecx`), donc `looks_like_func_start` les
  rejetait, et le seed d'immédiats ne couvrait que la forme **push/mov-vers-pile** (callback par valeur).
- **Deux formes, deux preuves ajoutées** (même force que `stack_arg_code_imm`/`abs_indirect_slot`, donc
  contournant l'heuristique de prologue) :
  1. **`mov reg, imm ; … ; call *reg`** (registre) — `reg_imm_reaches_indirect_call` : scan straight-line
     **borné et sûr** depuis le `mov` (s'arrête au 1er gap d'adresse, un `ret`, une réécriture du registre
     via `InstructionInfoFactory`, ou — registre caller-saved — un `call` qui le clobbe). Ne peut jamais
     attribuer une valeur ultérieure au call. (busybox `cksum` choisit sa variante CRC ainsi.)
  2. **`mov [g], imm ; … ; call [g]`** (slot global écrit au **runtime**) — `abs_store_imm` + l'ensemble
     `icall_slots` : `abs_indirect_slot` ne lit que le contenu **statique** du slot (0 pour un `.data`) ;
     un immédiat-code stocké dans un slot prouvé cible de `call [g]` est le pointeur runtime. (busybox
     `od` installe son handler `ret`-nu par défaut ainsi.)
- **Effet** : **`od -An -tx1` bit-identique à Wine**, ajouté à la batterie gardée du sweep → **60/60**.
  `cksum` **avance** d'abort « non récupéré » à un **segfault distinct** : l'ABI de dispatch indirect
  d'une fonction **regparm + argument pile** (la fonction CRC `0x41abec`, désormais récupérée, lit la
  table CRC en `[esp+0xc]`) mis-passe l'arg pile. Chantier séparé (lifting/ABI, pas récupération) —
  documenté dans le harness. (Ni sortie fausse dans un cas : cksum reste en crash, jamais un faux CRC.)
- **Sûreté (une fausse entrée tronquerait une vraie fonction → miscompile)** : **difftest 271/271**,
  **transpile-diff 4/4** (hash `19acad982194bf07` **inchangé**), **cpudiff vert**, **sqlite sweep
  bit-identique** (récupération du vrai binaire MSVC intacte), **busybox sweep 60/60**. Général : bénéficie
  à tout binaire choisissant un pointeur de fonction par immédiat (dispatch par option, handler par
  défaut). Garde : le `od -An -tx1` du sweep (une callee `ret`-nu atteinte uniquement par immédiat→`call
  [g]` ne se reproduit pas fidèlement en C mingw, d'où le guard end-to-end plutôt qu'une fixture synthétique).

### Infra + sonde multi-binaires (7za, plink, grep/sed, cksum) : le point d'inflexion des gains faciles ✅/🔬
- **2026-07-04 — deux acquis concrets + un signal stratégique fort.**
- **Infra (`.claude/hooks/session-start.sh`)** : le hook SessionStart **resynchronise le checkout sur
  origin AVANT de builder** (le conteneur éphémère peut revenir sur une base locale périmée — vécu en
  début de session : HEAD local `ec72102` alors qu'origin était à `1dfa372`). Sûr : n'avance qu'un arbre
  PROPRE ancêtre strict d'origin ; commits locaux non poussés ou arbre modifié ⇒ avertit sans rien
  détruire. (Ne protège pas le non-commité → commit+push fréquents restent la règle.)
- **Fix général (`src/ir/stdcall_pops.rs`, commit `0c24822`)** : la famille locale kernel32
  (`LCMapStringA/W`, `CompareStringA/W`, `GetStringTypeA/W`, `FoldStringW`) était **absente** de la table
  `@N` → `ret N` non modélisé → dérive esp possible pour tout binaire les appelant. Ajoutés (@N MSDN
  vérifiés ; le corpus winetest les classe haute-fréquence). Sûr : winediff 44/44 (locale_cp les exerce),
  difftest 271/271, transpile-diff 4/4 (hash inchangé), `table_is_sorted` vert.
- **Sonde de 4 vrais binaires (mesurer où on en est)** — chacun transpile proprement (**0 appel direct
  non résolu**), mais bute sur un bloqueur **profond**, pas un shim/récupération facile :
  - **cksum** (busybox) : dispatch indirect regparm+arg-pile OK (vérifié dans le C généré) ; la table CRC
    vient d'un `call crc32_filltable` dont le **résultat = 0** au runtime (dataflow/lift spécifique).
  - **grep/sed** (busybox) : SIGSEGV dans le moteur regex lifté (`sub_42f6d4` : `mov (eax),eax`, eax=0x38
    = deref d'un champ `*(base+0x38)`, base≈0) — **miscompile profond**.
  - **7za** (MSVC) : à un `LCMapStringW` d'une fonction SEH construisant une table de 256 chars, **esp
    pointe dans le buffer** au lieu de la liste d'args — erreur de modélisation **esp/frame**.
  - **plink** (MSVC/clang) : abort sur appel indirect vers `0x450058` non récupéré — un pointeur de
    fonction **isolé, atteint par adresse calculée/indexée** (ni immédiat, ni `call [slot]`, ni run≥3) =
    le cas de récupération le plus dur (points-to).
- **Signal stratégique (réponse à « on ne tourne pas en rond ? »)** : les **victoires générales faciles**
  (shims OS/CRT, `stdcall_pops`, récupération simple) sont **quasi épuisées** — elles ont fait converger
  Lua/strings/sqlite/busybox vite. Ce qui reste sur ces 4 binaires est un **ensemble borné de problèmes
  PROFONDS** (miscompiles lifter, modélisation esp/frame SEH, récupération points-to) — chacun une
  **session dédiée de forensics**. On passe de la phase « largeur de shims » à la phase « profondeur
  lifter ». Ce n'est **pas infini** (mesurable, borné), mais le **rythme ralentit** : la maille devient
  « une session = un bug profond général », plus « un binaire = 10 bugs faciles ». La discipline
  « borner puis pivoter dès qu'un bug n'est pas généralisable rapidement » est ce qui garde ça fini.

### Accélérateur — funcdiff v0 : différentiel par FONCTION entière vs Unicorn (le « moment cpudiff » des bugs profonds) ✅ socle
- **2026-07-04 — attaquer la classe profonde par un OUTIL, pas du gdb manuel.** Diagnostic de grep : la
  méthode pinpoint C `-O0 -g` localise vite (crash exact, champ null), mais le dernier maillon (quel store
  d'init a disparu) devient un trace multi-couches **non-déterministe** (adresses d'alloc qui bougent) —
  exactement ce que le manuel fait mal. La réponse : étendre l'oracle différentiel de l'INSTRUCTION à la
  FONCTION, sur les fonctions du vrai binaire.
- **`src/cpudiff.rs` funcdiff v0** : exécute les bytes d'une fonction récupérée dans **Unicorn** et son IR
  liftée dans l'**interpréteur**, depuis un état registres+mémoire **identique** (image PE + pile miroir),
  et compare registres finaux + toute la mémoire. **Une divergence = un vrai bug de LIFT** — notamment un
  **store droppé au lift** (Unicorn l'exécute, l'IR ne l'a pas → mémoire différente) = précisément la classe
  grep/cksum, trouvée **sans forensics manuelle**.
- **Sain par construction (principe sacré)** : appel / switch / instruction non modélisée / accès hors-image
  / faute Unicorn / non-retour ⇒ état **skippé**, jamais un faux verdict. Additif à `Interp` (mémoire
  multi-régions ; le chemin per-instruction, régions vides, est inchangé). Test-only (`#[cfg(unpack)]`),
  **zéro effet sur le produit**.
- **Garde de soundness committée** (`functions_match_unicorn_on_fixtures`, 0 divergence + assert
  non-vacuous) : elle a **immédiatement attrapé un faux positif du harness lui-même** — un tail-call
  (`jmp [mem]` lifté `Return(call)`) dont l'interp ignorait les effets du callee ; corrigé. C'est le
  principe en action : l'oracle se prouve sain avant de croire ses verdicts.
- **v0 = fonctions LEAF** (un appel → skip). Mesuré : **600 itérations scorées sur busybox, 0 divergence**
  (les fonctions de calcul leaf sont déjà blindées par le per-instruction). **La valeur vient avec
  l'extension CLOSURE** : interpréter récursivement les callees (Unicorn le fait déjà) pour atteindre les
  fonctions **à appels** — c'est là que vivent grep (moteur regex), cksum (filltable), etc. C'est le
  **prochain incrément** ; il transformera « heures de forensics réactive » en « la machine pointe
  l'instruction mal-liftée ».
- **Régression** : suite unit complète verte (60 lib dont cpudiff+funcdiff), build produit inchangé (code
  test-only). Commit `a1fb0b6`.
- **État des 4 bloqueurs profonds** (diagnostiqués cette session, à cueillir par funcdiff-closure) : grep/sed
  (store d'init droppé, moteur regex), cksum (table filltable=0), 7za (esp/frame SEH), plink (récup
  points-to). Le premier trois sont des bugs de lift/opt/data que funcdiff-closure devrait pointer ; plink
  est de la récupération (autre axe).
### Accélérateur — funcdiff CLOSURE : l'oracle par fonction suit les appels (atteint les fonctions à appels) ✅
- **2026-07-04 — l'extension promise par funcdiff v0 est faite.** v0 ne scorait que les fonctions
  **feuilles** (`is_leaf_pure` rejetait tout appel) → il ne testait pas les fonctions **à appels**, là où
  vivent les bugs profonds (moteur regex de grep, `filltable` de cksum). La closure **interprète
  récursivement les callees directs récupérés** — Unicorn le fait déjà nativement en exécutant les octets ;
  côté IR, on recurse dans l'IR liftée de la callee. Une divergence sur une fonction à appels = un vrai bug
  de lift (dans le caller *à travers* l'appel, ou dans la callee).
- **Modélisation EXACTE de la discipline call/ret (sinon faux positif mémoire)** : au site d'appel `esp=S`
  (args déjà poussés), le `call` matériel empile l'adresse de retour (`esp=S-4`) ; la callee restaure `esp`
  avant `ret N` qui dépile retour + N octets d'args (`esp=S+N`). `call_direct` reproduit exactement cet
  effet net (`ret N` lu par `compute_ret_pops` depuis le bloc terminal ; pop ambigu/absent ⇒ appel non
  suivi, skip). La valeur de retour = l'expr `Return` de la callee (paire `(edx<<32)|eax` 32-bit), rendue au
  caller pour que son split edx:eax fonctionne. Le clobber `Set{ecx,Undef}` post-appel est **ignoré** (la
  récursion a déjà laissé la vraie valeur = celle d'Unicorn ; c'est ce qui rend une fonction à appels
  scorable, et c'est sain — nul code correct ne lit un registre caller-saved à travers un appel).
- **Sain par construction (principe sacré, un oracle à faux positifs est pire que rien)** :
  - **Adresse de retour** : sentinelle **non mappée** (`0xdead1000`) — une callee qui la déréférence
    (thunk get-pc) faute → skip. Ses slots de pile sont **exclus** du diff mémoire (Unicorn y écrit la vraie
    adresse — plomberie ABI, pas un signal de lift). Fuite résiduelle dans un registre comparé ⇒ **garde**
    explicite qui skippe.
  - **Frames OFF forcés** (comme le transpileur, le vrai produit) : `[esp±d]`/`[ebp±d]` deviennent des
    loads bruts sur la pile miroir (un slot `Frame` nommé est opaque → skip) — mode *plus* fidèle à Unicorn,
    pas un raccourci. Bonus : on teste exactement le lowering **shippé**.
  - Appel indirect / import non modélisé / `switch` / `asm` / accès hors-région / dépassement de budget /
    récursion trop profonde ⇒ **état skippé, jamais un verdict faux**. Pré-filtre statique
    `is_closure_modelable` (BFS de la fermeture d'appels directs) + filet d'exécution `run_closure`
    (`None` par-instruction).
- **Vérifié (preuve que la closure s'exécute ET est saine)** : sur `recursion.exe` (fib), `call_direct` **a
  suivi 11 600 appels** (fib récurse à travers l'interpréteur — args pile, valeur de retour, discipline
  call/ret complète) avec **0 divergence** vs Unicorn ; `varied_o0.exe` : 6 200 appels, 0 divergence. La
  garde de soundness `functions_match_unicorn_on_fixtures` **exige désormais `calls>0`** (la closure n'est
  pas vacante) en plus de `scored>0` et 0 divergence — verte. Test-only (`#[cfg(feature=unpack)]`),
  **produit inchangé**.
- **Régression complète PASS** : cargo test (54 unit + intégration verts), cpudiff+funcdiff verts,
  **difftest 271/271**, **transpile-diff 4/4 (hash `19acad982194bf07` inchangé)** ⇒ produit byte-identique.
- **Prochain** : lâcher funcdiff-closure sur busybox/grep/cksum (corpus non caché sur cet hôte) pour
  **pointer l'instruction mal-liftée** de la classe profonde (store d'init droppé du moteur regex, table
  `filltable=0`) — « la machine pointe le bug » au lieu du forensics gdb manuel.

### funcdiff-closure lâché sur busybox + corpus binaires épinglé (jamais perdu) ✅/🔬
- **2026-07-04 — les binaires du corpus sont committés** (négations `.gitignore` sur `bench/.cache/*` +
  `README.md` provenance/sha256) pour survivre au conteneur éphémère : **busybox-w32** (grep/sed/cksum,
  613 Ko), **sqlite3** MSVC strippé (1,1 Mo), **winetest.exe** WineHQ daily (87 Mo, 143 imports). Plus de
  re-téléchargement perdu entre sessions.
- **funcdiff-closure sur busybox** : **3457 itérations scorées, 2100 appels suivis, 0 divergence** vs
  Unicorn (v0 feuilles-seules : ~600 scorées, 0 appel). La closure multiplie ~×3,5 la surface et exerce
  2100 vrais appels — **le lift `build_ir` brut est sain** sur toutes les fonctions scorées de busybox.
- **Insight de ciblage (0 divergence ≠ pas de bug ; ça dit *où il n'est pas*)** : les bugs profonds
  grep/cksum ne sont donc **PAS dans le lift brut** des fonctions scorées. Ils sont soit (a) dans des
  fonctions **skippées** (le moteur regex appelle `malloc`/imports → non-closure-modelable), soit (b) **en
  aval de `build_ir`** — funcdiff teste l'IR **pré-SSA** ; un store supprimé par la **DCE/opt** ou une
  faute de **SSA** n'y apparaît pas (l'interp ne modélise pas `Use(ValueId)`/`Phi`). **Prochaine frontière
  funcdiff** : différencier l'IR **post-opt** (interpréteur SSA) pour couvrir la couche où vivent
  probablement grep/cksum — incrément distinct et substantiel (à faire proprement, pas en fin de session).

### Frontière SSA/opt — funcdiff différentie l'IR POST-OPT (le compilateur, pas juste le lifter) ✅
- **2026-07-04 — la « prochaine frontière » identifiée par le run busybox est attaquée.** funcdiff-closure
  différenciait le lift **pré-SSA** (`build_ir`) vs Unicorn ; les bugs profonds grep/cksum étaient donc soit
  dans des fonctions skippées, soit **en aval** (construction SSA + passes d'opt). Ce couche est maintenant
  couverte : un **interpréteur SSA** exécute l'IR **post-opt** (`to_ssa`+`optimize`) et la compare à l'IR
  **pré-opt** — l'oracle déjà validé bit-à-bit contre Unicorn par funcdiff-closure. Une divergence = un vrai
  bug de **construction SSA ou d'une passe d'optimisation**.
- **Sain par construction (prouvé, pas supposé)** : deux invariants du compilateur rendent la comparaison
  mémoire saine — (1) la **DCE ne supprime jamais un `Store`** (pas d'analyse d'alias → tous les stores
  conservés ; seuls les `Assign` purs morts tombent) ; (2) `optimize` **ne touche pas le CFG** (il ne fait
  que replier les *expressions* dans les statements — jamais `Branch`→`Jump`, jamais de bloc/arête retirés,
  `succ`/`pred` intacts). Donc un opt correct ⇒ **mémoire byte-identique + valeur de retour égale** ; l'ordre
  et l'ensemble des stores sont préservés, seul le *calcul* des valeurs change. Comparaison : valeur de
  retour (l'expr `Return`, observable) + **toute** la mémoire mappée (image + pile). Pas besoin d'Unicorn
  (le pré-opt est l'oracle). Leaf-only pour l'instant (la closure SSA — threading d'esp à travers les
  *valeurs* — est un incrément suivant).
- **Détails** : `IrFunction.entry_values` (Location→ValueId undef) exposé par `to_ssa` pour amorcer les
  versions d'entrée (registres/flags) depuis l'état aléatoire ; l'interp SSA gère `Assign`/`Use`/`Phi`
  (φ résolu par le prédécesseur d'arrivée `prev_block`), mémoire partagée avec l'interp pré-opt. Additif au
  produit (le champ est ignoré à l'émission).
- **Vérifié — sain ET sensible** : `optimizer_preserves_semantics_on_fixtures` = **0 divergence** sur tout
  le corpus fixtures (fonctions à boucles/φ/mémoire : recursion, truncloop, two_switch, wide_carry…
  scorées) + **busybox 600 itérations scorées, 0 divergence**. **Test de dents** : perturber la valeur de
  retour post-opt de `^1` fait immédiatement apparaître des divergences « ret » (post-opt ≠ pré-opt) — la
  garde n'est pas vacante. Non-vacuité assertée (`scored>0`). Test-only (`cfg unpack`).
- **Régression** : IrFunction gagne un champ (produit) mais **transpile-diff 4/4 (hash `19acad982194bf07`
  inchangé)** ⇒ émission byte-identique ; difftest 271/271 ; cargo test complet vert (dont les 3 gardes
  funcdiff : per-instruction, closure, opt).
- **Prochain** : (a) **closure SSA** (suivre les appels dans l'interp post-opt) pour couvrir les fonctions
  à appels post-opt ; (b) lâcher sur un corpus plus large (winetest/sqlite/grep) — l'outil pointera
  désormais un bug **d'opt/SSA** aussi précisément qu'un bug de lift.

### funcdiff en PORTE DE RÉGRESSION sur binaires réels (busybox mingw + sqlite MSVC) ✅ + sweep sqlite
- **2026-07-04 — les deux différentiels deviennent un gate permanent.** Après avoir bâti l'opt-diff, la
  suite naturelle : lâcher sur un corpus plus large ET banquer le tout en régression. Sweep :
  - **sqlite3 (MSVC 32-bit strippé)** : LIFT-closure **7926 scorées / 3854 appels**, OPT-diff **2403
    scorées**, **0 divergence** — validation **cross-toolchain** (mingw *et* MSVC) du lift ET de l'opt.
  - **winetest (87 Mo)** : 0 scorée — image > cap de 64 Mo de `diff_function` (mapper 87 Mo par fonction
    n'est de toute façon pas praticable) ; limitation connue, pas un bug.
- **`bench/funcdiff.sh`** (nouveau) : exécute LIFT-closure (vs Unicorn) + OPT-diff (post-opt SSA vs
  pré-opt) sur le corpus épinglé, **assert 0 divergence** + non-vacuité (appels suivis, opt scoré). Skip
  propre si libunicorn absent (build unpack indisponible) ou corpus absent. Câblé dans
  `bench/regression.sh`. Total gate : **lift 11383 scorées / 5954 appels, opt 3003 scorées, 0 divergence →
  PASS**. Les 3 diagnostics ignorés (scratch/busybox×2) fusionnés en un seul `funcdiff_corpus`.
- **Constat honnête sur les bugs profonds restants** : la LIFT-closure suit déjà **5954 appels** (busybox
  2100 + sqlite 3854) **sans une seule divergence** → le bug grep « store d'init droppé » n'est **pas** dans
  une fonction *modélisable*. Il est atteint **via un import** (`malloc` du moteur regex) que ni la closure
  ni l'opt-diff ne franchissent (un `Named` non-shim ⇒ skip). **La vraie frontière suivante pour cette
  classe = modéliser quelques imports purs/déterministes** (`memcpy`/`memset`/`memcmp`/`strlen`) dans
  l'interpréteur pour scorer la logique applicative — sain (sémantique exacte, exécutée à l'identique par
  Unicorn) mais à faire prudemment (un import mal modélisé = faux positif). L'opt-diff **closure** (suivre
  les appels post-opt, threading d'esp à travers les *valeurs* SSA) reste l'autre incrément ouvert.
- **Régression** : produit **inchangé ce tour** (seuls tests/bench touchés) ; gate funcdiff PASS ; suite
  unpack verte (3 gardes : per-instruction, closure, opt).

### funcdiff — adresses 32 bits + modélisation memcpy/rep-stos (couverture ×3 de l'opt-diff) ✅
- **2026-07-04 — deux fixes qui débloquent une grande part de la logique applicative** (chacun committé
  séparément, sain, teeth-checké). *Note infra* : conteneur réinitialisé 2× cette session ; travail
  non-committé perdu puis refait — désormais **commit après chaque incrément**.
- **(1) Adresses effectives masquées à 32 bits** (`mem_read`/`mem_write`, mode fonction-entière = cible
  32 bits). L'interp calculait les adresses en 64 bits sans wrap ; `[edi-1]` lifté `(edi&0xffffffff)+
  0xffffffff` lisait à `edi+0xffffffff` (33 bits, hors région → skip parasite). Le matériel et Unicorn
  wrappent mod 2³² ; masquer reproduit exactement ça — **plus correct, jamais un faux verdict**. Débloque
  toute fonction à accès pile déplacement-négatif.
- **(2) memcpy/rep-stos modélisés** (`do_memcall`). Le lifter émet un `memcpy(edi,esi,n)` synthétique pour
  `rep movs` et `__rep_stos*` pour `rep stos` (args explicites). L'interp exécute l'effet mémoire exact ;
  Unicorn exécute l'instruction string → **comparable octet-à-octet**. Les deux différentiels (LIFT-closure
  ET OPT-diff) acceptent ces appels → les fonctions à copie/zéro-init de structures (très fréquentes)
  deviennent scorables. Copie forward octet-à-octet = `rep movsb` exact ; hors-région ou > 1 MiB → skip.
- **Effet combiné mesuré (corpus gate, 0 divergence)** : **OPT-diff 3003 → 10581 scorées (×3,5)**
  (busybox 600→2919, sqlite 2403→7662) ; LIFT-closure 11383 → 12369 (5954→6584 appels). Tout **SOUND**.
- **Vérifié sain ET sensible** : fixture `rep_movsb_copy.exe` (copie 16 octets fixes entre buffers pile →
  scorée vs Unicorn) + test `memcall_model_matches_unicorn` (0 divergence, non-vacant). Teeth-check :
  perturber la copie `^1` → divergence `reg r0` (0x33 vs 0x34). Garde opt-diff `optimizer_preserves_
  semantics_on_fixtures` **restaurée** (supprimée par mégarde en 5714558). Produit inchangé (test-only).
- **Reste ouvert** : imports **dynamiques** (msvcrt/IAT) — Unicorn faute dessus (DLL non mappée) donc non
  comparables en LIFT-closure ; modélisables en OPT-diff (pas d'Unicorn) via la closure SSA. Et le moteur
  regex de grep reste derrière `malloc` (état) — modélisation d'allocateur = frontière plus lourde.

### Frontière 2 — closure SSA (opt-diff à travers les appels) : TENTÉE, DIAGNOSTIQUÉE, RETIRÉE (principe sacré) 🔬
- **2026-07-04 — un oracle à faux positifs est pire que rien : la tentative n'est PAS shippée.** Design :
  exposer `value_regs` (vid→registre) depuis `to_ssa`, et faire tenir à `run_ssa` un **fichier de
  registres fantôme** (`self.regs`, MàJ à chaque `Assign` versionnant un registre) pour, au `call
  Direct`, réutiliser l'interpréteur **pré-opt** register-based (`call_direct`→`run_closure`) sur le
  callee — l'oracle pré-opt étant déjà validé bit-à-bit contre Unicorn.
- **Symptôme** : la garde `optimizer_preserves_semantics_on_fixtures` **échoue** sur du code connu-bon
  (`address_taken_callback.exe`, `f(a,b)=g(a)+g(b)` avec `g=ret` nu) — divergences `ret` ET une octet de
  pile au **slot d'adresse de retour** (`0x10007ff8`, post-opt=0xd0 vs pré-opt=0x00). Le callee ayant
  écrit des octets différents ⇒ il a reçu un **état différent** : c'est un **bug du HARNESS** (l'`esp`
  fantôme au site d'appel diverge de celui du côté pré-opt), pas un bug d'opt (une miscompile d'opt
  donnerait une valeur fausse avec pile identique).
- **Cause profonde (non résolue rapidement)** : l'inconsistance vit à la **frontière** entre l'exécution
  vid-based de `run_ssa` et l'exécution register-based de `run_closure` du callee — au minimum l'`esp`
  fantôme, possiblement aussi les flags/xmm non-threadés que le callee lit à l'entrée. Threader **tout**
  l'état CPU (GP + flags + xmm + esp) proprement à travers cette frontière est un chantier à part.
- **Décision** : **retirée** (revert complet ; Frontier 1 memcpy/rep-stos + fix adresses-32b conservés,
  déjà committés/verts). Conforme à la discipline « borner puis pivoter dès qu'un incrément n'est pas
  sûr » et au principe sacré. **Reste ouvert** : closure SSA = session dédiée (threader l'état CPU complet
  au call, tester par la garde opt + teeth-check avant de croire un verdict). L'opt-diff reste **leaf +
  memcpy/rep-stos**, sain (10581 scorées, 0 divergence).

### Pivot cible réelle — cksum (busybox) : crash LOCALISÉ à l'instruction (base de table CRC = NULL) 🎯
- **2026-07-04 — repro concrète depuis le busybox committé.** Décision : après la saturation de funcdiff
  (0 divergence partout, bugs profonds derrière imports), pivot vers l'objectif nord (convertir un vrai
  binaire). cksum = bloqueur le plus **borné** (« table CRC = 0 » du journal). Repro :
  ```
  aret --mode transpile --out-dir T busybox-w32-...exe      # OK (app 11 Mo)
  cp T/app /tmp/busybox && printf 'hello world\n' | /tmp/busybox cksum
  # attendu (wine)   : 3733384285 12
  # obtenu           : SEGFAULT (rc=139)   ;  `busybox echo` marche (conversion OK par ailleurs)
  ```
  (Dispatch multi-appel : le binaire doit s'appeler `busybox*` — `basename(argv[0])` sélectionne le
  multiplexeur ; sinon « applet not found ».)
- **Localisation à l'instruction** (gdb sur le binaire natif) : SIGSEGV dans `sub_41abec+637`,
  `mov (%eax),%eax` avec **`eax=0x2e0`**, atteint via 2 dispatches indirects
  (`sub_45b0fc → [aret_call] sub_416574 → [aret_call] sub_41abec`). Contexte = **boucle CRC** :
  `... and $0xff,%eax (index octet) ; shl $2,%eax (×4) ; add %esi,%eax (base table) ; mov (%eax),%eax
  (= table[octet])`. Registres au crash : **`esi=0`** ⇒ base de la table CRC **NULLE** ⇒
  `*(0 + 0xb8*4=0x2e0)` ⇒ segfault.
- **Confirme le diagnostic « crc32_filltable = 0 » du journal, au niveau instruction.** `esi ←`
  `mov %eax,%esi @822f2f0 ← eax = [ebp-0x38]` (pointeur de table, paire 64-bit avec demi-haute forcée à
  0) **= 0**. La table CRC (globale remplie par `crc32_filltable`, ou son retour) n'est jamais posée.
- **Reste (trace du root cause)** : remonter où `[ebp-0x38]` est stocké dans `sub_41abec` (paramètre, ou
  retour d'un `call` à la fonction filltable) et pourquoi il vaut 0 — un **store d'init droppé** ou un
  **retour de fonction mal modélisé** (classe « valeur fausse silencieuse »). C'est le prochain pas de
  forensics ciblée (une fonction, dataflow d'un pointeur). NB : les `mov $0x0,%edx` répétés (zéro de la
  demi-haute 64-bit après chaque op 32-bit) sont un artefact de codegen à surveiller dans la trace.

### cksum — ROOT CAUSE affiné : PAS « filltable=0 », mais un DÉCALAGE d'arg de +4 (valeur 32-bit modélisée 64-bit) 🎯🎯
- **2026-07-04 (suite) — le diagnostic du journal (« crc32_filltable=0 ») est FAUX ; le vrai est plus
  général.** Preuves runtime (gdb sur le binaire natif) :
  - `crc32_filltable` = **`sub_41ac18`** (contient le polynôme CRC `0xedb88320`). Il **EST appelé** (par
    `sub_416574`) et **retourne un pointeur VALIDE `0x8c2a400`** — pas 0.
  - La struct/pile de contexte CRC (sur `aret_stack`) : `+0x8=0xffffffff` (init CRC), **`+0xc=0`** (lu comme
    base de table → NULL → crash), **`+0x10=0x8c2a400`** (= le retour de filltable !).
  - Donc la table est **écrite à `+0x10`** mais **lue à `+0xc`** → **décalage de 4 octets**.
- **Qui a raison ?** L'ORIGINAL `sub_41abec` lit la table via `mov 0xc(%esp),%esi` (4ᵉ **argument pile**,
  offset `+0xc`). Le transpilé lit aussi `+0xc` ⇒ **la LECTURE est correcte**. C'est donc l'**APPELANT**
  (`sub_416574`) qui a poussé l'argument table à `+0x10` au lieu de `+0xc` — **sur-décalage de +4**.
- **Mécanisme (forte hypothèse)** : la table (un pointeur **32-bit**) transite par des **paires 64-bit
  `eax:edx`** dans le transpilé (`mov %eax,-0x230(ebp); mov %edx,-0x22c(ebp); mov $0x0,%edx; …` — la
  demi-haute forcée à 0 partout). Un argument 32-bit modélisé **64-bit** occupe **8 octets** au lieu de 4
  sur la pile machine partagée ⇒ décale l'argument suivant de +4. **Bug GÉNÉRAL** (même famille que le
  « esp pointe dans le buffer » de 7za) — bénéficierait à tout binaire poussant un tel argument.
- **Reste (le fix)** : dans le lift/build de l'ABI shared-stack, un argument/valeur 32-bit ne doit occuper
  que **4 octets** à la poussée sur la pile machine (pas la paire 64-bit). Localiser la poussée d'arg dans
  `sub_416574` avant l'`aret_call` vers `sub_41abec`, corriger la largeur, revérifier par régression
  complète (difftest + transpile-diff + winediff) puis re-tester `busybox cksum` = `3733384285 12`.

### cksum — tentative de fix : repro minimale NON reproduite → esp-drift spécifique au driver CRC (honnête)
- **2026-07-04 (suite) — le fix proprement dit n'est PAS trouvé ; voici l'état exact pour ne pas repartir
  de zéro.** Méthodo plan §1 : construire une **fixture minimale** avant de toucher au produit.
  - `push` est lifté correctement (esp-=4, store int(32)) — pas la source.
  - Fixture 1 (struct `{uint32 crc; ptr table;}` + appels directs) : transpile → **correct** (r=10).
  - Fixture 2 (appel **indirect** `f(0xffffffff, buf, len, table)`, table = 4ᵉ arg pile) : transpile →
    **correct** (r=14). Donc le motif simple « arg pointeur 4ᵉ position + premier arg 0xffffffff » ne
    suffit pas à déclencher le décalage.
- **Ce que ça dit** : le drift de 4 octets n'est **pas** un push/arg simple — c'est une **dérive d'esp
  spécifique à la chaîne du driver CRC** de busybox (`sub_416574` stocke la table dans un local `[esp+0x28]`
  (vérifié sur l'original) puis appelle `0x4334dc` = driver, qui itère et appelle le handler `sub_41abec`
  avec la table en arg). Runtime : la table (0x8c2a400) est bien en mémoire à `0x8c28cf4`, mais `sub_41abec`
  lit `0x8c28cf0` (−4). L'esp du guest dérive de 4 quelque part **entre le stockage (416574) et la lecture
  (41abec)**, à travers `0x4334dc` — un `push`/`pop`/`ret N` mal apparié sur un des appels de la chaîne.
- **Prochain pas précis** (session dédiée) : tracer l'esp guest le long de `416574 → 4334dc → … → 41abec`
  (watchpoint/print de l'esp modélisé à chaque frontière d'appel) pour localiser l'appel où esp dérive de
  4 vs l'original ; OU élargir la fixture au motif « driver qui itère et appelle un handler par bloc avec
  la table héritée d'un local ». Repro : `busybox cksum` doit rendre `3733384285 12` (obtient segfault).
  **Acquis solide et committé** : filltable OK (retour valide), diagnostic « filltable=0 » réfuté, drift
  localisé à ±4 dans la chaîne driver — reste l'isolation exacte de l'esp mal apparié.

### cksum — forensics exhaustive : mécanisme cerné, trigger NON isolé, fix NON shippé (principe sacré)
- **2026-07-04 (fin) — investigation approfondie, honnête sur la limite.** Le décalage est **réel et
  confirmé au runtime** mais son déclencheur exact résiste à la réduction en fixture.
- **Confirmé** : `sub_41abec` = bloc CRC `FAST_FUNC` busybox = `__attribute__((regparm(3),stdcall))`
  (args eax=crc/edx=buf/ecx=len + 1 arg pile = table à `[esp+4]`, **`ret $0x4`**), appelé **indirectement**
  dans une boucle driver. Il lit la table via `[esp+0xc]` après `push esi;push ebx` (= `[entry_esp+4]`).
  Runtime esp aux appels successifs : **#1=0x8c28cf0** (lit la table correctement à 0x8c28cf4), **#2/#3=
  0x8c28cec** (−4, puis stable) → à partir de #2, lit `0x8c28cf0` (≠ table) = 0. Décalage **ponctuel de −4
  entre le 1ᵉʳ et le 2ᵉ appel**, pas une accumulation linéaire.
- **Ruled out (4 fixtures minimales, toutes CORRECTES au transpile)** : (1) struct `{u32;ptr}` + appels
  directs ; (2) appel indirect `f(0xffffffff,buf,len,table)` arg pile ; (3) `__stdcall` `ret 4` indirect en
  boucle (-O2 ET -O0) ; (4) **exactement** `FAST_FUNC` `regparm(3)+stdcall` `ret 4` indirect + driver.
  Aucune ne reproduit → le bug dépend d'un détail spécifique à la **chaîne multi-hop réelle** de busybox
  (`416574 → 4334dc → … → 41abec`) non capturé.
- **Fait de code** : le pop `ret N` n'est modélisé que pour les **imports** (`stdcall_pops`, `build.rs`
  L299) ; les fonctions internes `ret N` s'appuient sur la compensation `sub esp,N` du compilateur (vérifié
  au désassemblage). Le cas simple marche ; le cas busybox dérive — l'interaction exacte reste à trouver.
- **Décision (principe sacré)** : **pas de fix produit shippé** sans repro + vérification. Un patch deviné
  sur l'esp/frame (zone la plus sujette aux régressions) casserait probablement difftest/transpile-diff/
  winediff — pire que rien. **Prochain pas** : tracer l'esp guest à CHAQUE frontière d'appel de la chaîne
  `416574→4334dc→…→41abec` (entre l'appel #1 et #2 de 41abec) pour trouver le −4 ponctuel exact, OU
  reproduire le driver multi-hop complet. Acquis solide committé : diagnostic « filltable=0 » réfuté,
  décalage localisé, 4 hypothèses éliminées, mécanisme FAST_FUNC identifié.

### cksum — TRIGGER ISOLÉ + REPRODUIT (drift6.c), mécanisme prouvé par preuve runtime
- **2026-07-05 — percée.** Le déclencheur exact est trouvé, réduit en fixture minimale qui **crashe au
  transpile** (native `c=226`, transpilé **segfault**), et la cause racine est prouvée au niveau instruction.
- **Preuve runtime (busybox transpilé instrumenté)** : `fprintf` avant chaque `aret_call` du driver CRC
  `sub_416574` → `esp=v254` passe de `0x08c29260` (appel #0, table=`0x09809400` correcte) à `0x08c2925c`
  (appel #1, **−4**, table=`0x00406f80` FAUSSE). La table est lue via `*[v254+0x28]` (slot esp-relatif) :
  quand `v254` dérive de −4, `[v254+0x28]` pointe un autre slot → mauvaise table → sortie fausse/crash.
- **Preuve désassemblage original** (`objdump` busybox, `sub_416574`) : `-fomit-frame-pointer` (ebp =
  **pointeur de fonction**, pas frame pointer → tous les locaux esp-relatifs). Motif de la boucle :
  `mov eax,[esp+0x28]` (recharge table) → `mov [esp],eax` → `call ebp` (`FAST_FUNC ret 4` → esp+=4) →
  **`push edx`** (compensation gcc, esp−=4) → net **0** sur l'original. Le transpilé modélise le `push edx`
  (esp−4) mais **pas** le pop `ret 4` du callee (+4) → net **−4/itération** = la dérive.
- **Cause racine (définitive)** : un `call` interne/indirect est modélisé net-0 sur l'esp de l'appelant
  (push+pop de l'adresse retour s'annulent), **mais le pop d'arguments d'un callee `ret N`
  (stdcall/FAST_FUNC) n'est jamais appliqué à l'esp de l'appelant**. Le compilateur a émis sa compensation
  (`push`/`sub esp,N`) en supposant que le callee pop N ; sans modéliser ce pop, net = −N/appel.
- **Pourquoi 5 fixtures avaient échoué** : drift5 a bien la dérive (`call [slot];sub esp,4` en boucle) mais
  la SEULE valeur esp-relative (la table, constante `mov [esp],0x408004`) est écrite ET lue au même esp
  (dérivé) → auto-cohérente → sortie correcte. Ingrédient manquant = **une valeur spillée AVANT la boucle
  et rechargée depuis un slot esp-relatif DANS la boucle** (le `[esp+0x28]` de busybox).
- **drift6.c (repro minimale)** : FAST_FUNC `regparm(3)+stdcall ret 4` indirect via `slot` global ;
  driver `-O2 -fomit-frame-pointer` qui spille `tp` au stack (`&tp` s'échappe via asm) puis recharge
  `*tpp` (= `[esp+0x1c]`) à chaque itération. gcc émet exactement `mov [esp+0x1c],table` (avant boucle) /
  `mov edx,[esp+0x1c]` (recharge) / `call [slot]` / `sub esp,0x4`. **native `c=226`, transpilé SEGFAULT.**
  Généré (dt6/chunk_0.c `sub_401500`) : `v27` (esp) → `v45=v27−4` → `continue: v27=v45` (dérive −4/itér),
  recharge `*[v27+0x1c]` corrompue dès l'itér 1.
- **Le fix (à implémenter, vérifié + régression avant commit)** : modéliser le pop `ret N` du callee pour
  les fonctions **internes** (pas seulement les imports) au site d'appel : après un call interne
  direct/indirect vers un callee-pop, `esp += N`. Direct → N constant (scan du `ret N` du callee, `C2 imm16`
  vs `C3` = 0, non ambigu). Indirect (`aret_call`) → lookup runtime `addr → N` (table analogue à
  `stdcall_pops`). **Non-régression par construction** : les fonctions cdecl ont N=0 (no-op) ; seules les
  fonctions callee-pop (déjà cassées aujourd'hui) reçoivent le +N. Garde-fou : `ret N` (`C2`) est le seul
  encodage callee-pop, sans ambiguïté, donc pas de faux positif sur du cdecl.

### cksum — FIX LIVRÉ, vérifié, régression complète PASS (fermeture propre)
- **2026-07-05 — le fix est implémenté, vérifié bout-en-bout, et toute la régression passe.**
- **Le fix** : modéliser le pop `ret N` du callee pour les fonctions **internes** (pas seulement les
  imports). `compute_callee_pops(funcs)` scanne chaque fonction pour un `ret imm16` (opcode `C2`, encodage
  callee-pop non ambigu ; `C3`/cdecl = 0) → map `entry → N`. Au site d'appel (`ir::build`,
  `callee_pop_adjust`) : après un `call` interne vers un callee-pop, `esp += N`.
  - **Direct** : `N` constant (`callee_pop_bytes(target)`), no-op si cdecl.
  - **Indirect** : `esp += __aret_callee_pop(target_va)` — table runtime (générée dans `aret_dispatch.c`,
    binary-search VA→pop), capturée AVANT l'appel dans un temp (cible possiblement dans un registre
    caller-saved). Injecté seulement si le programme a ≥1 fonction callee-pop (`has_callee_pops`) → les
    autres binaires restent **byte-identiques**.
  - **Pourquoi net-0** : le compilo a émis sa compensation (`sub esp,N`/`push`) en supposant le pop ;
    `+N` (nouveau) `−N` (compensation existante) = 0. cdecl (N=0) → aucune injection → zéro régression.
- **Repro minimale committée** : `tests/m1/fixtures/indirect_stdcall_pop.{c,exe}` (drift6) — FAST_FUNC
  `regparm(3)+stdcall ret 4` indirect + driver `-O2 -fomit-frame-pointer` qui recharge la table depuis
  `[esp+0x1c]`. Pré-fix : **segfault**. Post-fix : `c=226` (= native). Test `internal_stdcall_callee_pop_esp`
  dans `tests/m1_transpile.rs`.
- **Vérification bout-en-bout** :
  - busybox `cksum in.txt` (12 o) : pré-fix `3690874878 3096396776` (FAUX) → post-fix **`3733384285 12`**
    (= `cksum` système = busybox sous wine). Vérifié aussi sur 5000 o : `2522930177 5000` (= référence).
  - busybox `md5sum`/`sha1sum`/`echo`/`sort`/`wc` : inchangés (wc segfaultait DÉJÀ avant le fix — bug
    pré-existant distinct, hors périmètre ; baseline construit et comparé pour le prouver).
- **Régression complète (principe sacré) — TOUT PASS** :
  - `bench/regression.sh` : difftest **271/271**, in-place 3/3, magicdiv 2³², funcdiff corpus (busybox+
    sqlite, **0 divergence**), SMT 11/11, recompilabilité gzip/ls/cat 100 % → **REGRESSION GATE: PASS**.
  - `bench/winediff.sh` : **44/44** programmes (sortie = ground truth Wine).
  - `bench/difftest_transpile.sh` : **4/4** niveaux d'opt (H=19acad982194bf07).
  - `cargo test` (défaut + `--features unpack`) : tout vert, funcdiff 0 divergence.
- **Bilan** : la frontière cksum est fermée proprement. Cause racine prouvée, fixe minimal et borné
  (direct N constant / indirect lookup runtime, gardé par `has_callee_pops`), repro committée, régression
  intégrale verte. Aucune sortie fausse présentée comme correcte ; le principe sacré est tenu.

### busybox `wc` (+ sort/head/od/…) : FILE msvcrt-layout, fd-backed → getc/putc inlinés marchent ✅ FAIT
- **2026-07-05 — crash SIGSEGV général sur les applets à stdio-fichier.** `busybox wc <fichier>`
  segfaultait sur `movzbl (%eax),%eax` avec **`eax=0xfbad2488`** = le `_flags` `_IO_MAGIC` d'un **FILE
  glibc**. Cause racine : le CRT msvcrt statique **inline `getc`/`putc`** en accès directs aux champs du
  FILE (`--f->_cnt >= 0 ? *f->_ptr++ : _filbuf(f)` — `_ptr`@0, `_cnt`@4, layout msvcrt), mais `aret_fopen`
  rendait un **FILE glibc hôte** (offset 0 = flags `0xfbad…`, pas `_ptr`) → le getc inliné déréférençait
  la valeur de flags comme un pointeur → crash. Les flux `_iob` (stdin/out/err) étaient déjà en layout
  msvcrt (d'où `wc < fichier` marchait), mais **pas** les fichiers `fopen`.
- **Fix général (`runtime/aret_hle/aret_hle.c`)** : **tous** les FILE de la HLE sont désormais des structs
  **msvcrt-layout** (32 o : `_ptr`/`_cnt`/`_base`/`_flag`/`_file`/pushback), **adossés à un fd** et
  **non bufferisés** — on garde `_cnt ≤ 0` pour que le macro inliné défère toujours à `_filbuf`/`_flsbuf`,
  qui font un `read`/`write` d'un octet sur le fd. `fopen`/`freopen`/`tmpfile`/`fdopen` allouent dans un
  pool de FILE (fd via `open`) ; `file_fd()` reconnaît _iob **et** pool (fd à l'offset 16) ; toutes les
  fonctions stdio (`fread`/`fwrite`/`fgets`/`fputs`/`getc`/`fgetc`/`ungetc`/`feof`/`ferror`/`fseek`/`ftell`/
  `rewind`/`fflush`/`fclose`) opèrent sur le fd — **plus aucun FILE glibc hôte**. Ajout de `_flsbuf`
  (fallback putc inliné), `_fdopen`, et une **pushback ungetc 1 octet** (offset 20, stockée `char+1` pour
  qu'un slot zéro = vide) partagée par getc/fgets/fread. Bug annexe corrigé : **`putc`** importé par nom
  (non inliné selon la version/opt du compilo) n'avait pas de shim (`aret_putc` = `aret_fputc`) → fichier
  jamais écrit. Le modèle unifie _iob et fichiers : un seul chemin, testé.
- **Effet** : `busybox wc` (fichier ET stdin) = **bit-identique à l'original sous wine** ; cat/head/sort/
  nl/od/cksum/md5sum idem. Bénéficie à **tout** binaire msvcrt qui lit/écrit un fichier via getc/putc
  inlinés (la classe entière des outils Unix-like statiquement liés msvcrt).
- **Garde permanent** : nouveau programme corpus `bench/winecorpus/stdio_getc.c` — écrit un fichier par
  `putc` en boucle (→ `_flsbuf`), le relit par `getc` (→ `_filbuf`), teste `ungetc`+`rewind`. Il **a
  attrapé** le gap `putc`-non-shimé (que le test applet manuel avait manqué). = preuve d'indépendance +
  CI permanent dans le dépôt.
- **Régression complète PASS** : difftest 271/271, in-place 3/3, magicdiv 2³², funcdiff corpus (0
  divergence), SMT 11/11, recompilabilité 100 %, **winediff 45/45** (les 44 existants + `stdio_getc`),
  cargo test (dont fixture `file_io` fopen/fread/fseek) vert. cksum (fix précédent) inchangé.
- *Reste (sweep, tâche suivante)* : `uniq <fichier>` lit stdin au lieu du fichier (argv/getopt, **pas**
  stdio — `uniq < fichier` marche) ; `tac` sort correct mais imprime un message `ioctl` parasite sur
  stderr (ENOTTY sur le fd fichier). Distincts du stdio, à traiter applet par applet.

### busybox sweep : uniq/tac/tail réparés — close(std) fidèle, isatty sans ENOTTY, _lseeki64 ✅ FAIT
- **2026-07-05 — sweep coreutils après le fix stdio.** Trois causes **générales** trouvées et corrigées
  (uniq/tac/tail passent, 11 applets = bit-identiques à wine : cat/head/tail/sort/uniq/wc/od/nl/tac/cut/rev).
- **1. `close()` des fds standard bloqué → idiome de réouverture cassé (uniq, tac).** busybox `uniq FILE`
  fait `close(STDIN_FILENO); xopen(FILE, O_RDONLY)` — l'idiome Unix classique : refermer fd 0 puis rouvrir
  le fichier, qui **réutilise fd 0** (le plus bas libre), pour ensuite lire le fichier **via stdin**. Or
  `aret_close` refusait de fermer les fds ≤ 2 (`if (fd<=2) return 0`, garde « never close std streams ») →
  `close(0)` no-op → la réouverture prenait fd 3 → uniq lisait le **vrai** stdin (vide) → sortie vide.
  **Fix** : `aret_close` honore `close()` fidèlement (fds standard inclus, sémantique POSIX). `fclose()`
  protège toujours les structs `_iob` séparément (`aret_fclose` ne ferme que fd>2). Bénéficie à tout outil
  utilisant le motif close+réouverture (uniq, tac, …).
- **2. `isatty()` fuyait `ENOTTY` dans errno → faux échec de lecture (tac).** busybox-w32 sonde
  `isatty(fd)` à **chaque** `read`. Sous Linux `isatty` sur un fichier régulier fait un `ioctl(TCGETS)`
  qui **pose `errno=ENOTTY`** ; le `_isatty` Windows ne touche pas errno. `aret_errno` rend le **même**
  errno hôte au guest → à l'EOF, tac voyait `errno=ENOTTY` résiduel et imprimait « tac: FILE:
  Inappropriate ioctl for device ». **Fix** : `aret_isatty` sauve/restaure errno (sémantique Windows).
  Général : tout programme qui `_isatty` sur un non-tty puis teste errno.
- **3. `_lseeki64` non implémenté (tail).** `tail` avertissait `unimplemented import: _lseeki64` (retombait
  correctement mais bruyamment). Ajout `aret_lseeki64` (offset 64-bit lo@1/hi@2, origin@3, retour edx:eax
  via `import_returns_u64`), + `_telli64`/`_ftelli64`. Général : tail/od/split sur gros fichiers.
- **Vérifié** : 11 applets (uniq/uniq -c/tac/tail/cat/wc/head/sort/od/nl/rev) = **bit-identiques à wine** ;
  **winediff 45/45** (close/isatty ne régressent pas stdin/pipe/console) ; wc/cksum inchangés. Régression
  complète (difftest/transpile-diff/funcdiff/SMT/recompilabilité) : voir commit.

### x87 robustesse — awk `/` cerné : cascade fp-returning + joins ambigus (diagnostic, pas de fix shippé)
- **2026-07-05 — cible concrète trouvée : busybox `awk` `22/7` → « Division by zero »** (wine : 3.14286).
  `+`/`*` sont **constant-foldés au parse** (marchent), mais `/`/`%` sont évalués au **runtime** dans
  `evaluate` (0x428500), dont l'analyse de profondeur x87 **abandonne** → le compare `if (R_d == 0)`
  (idiome `fldz; fxch; fucom; fnstsw; sahf; jp/jne`) devient asm no-op → drapeaux indéfinis → branche
  div-par-zéro toujours prise. (dc : abort sur fonction non récupérée 0x4119cc — distinct, recovery.)
- **Cascade fp-returning diagnostiquée** : `evaluate` → `call 0x433c58` (= parse-nombre, renvoie un
  `double` en st(0)) → `0x4082a0` (renvoie double) → `0x402220` (dtoa, **bail propre**). Pour que le
  `fstp`/`fucom` après `call 0x433c58` ne sous-déborde pas, `0x433c58` doit être **fp-returning**.
- **Sous-bug RÉEL trouvé (et fix correct identifié, non shippé)** : `compute_fp_returning` ne pouvait
  pas marquer une fonction fp-returning **auto-récursive** (`0x433c58` s'appelle elle-même puis fait un
  `ftst` sur le résultat) — poule-et-œuf : l'analyse de profondeur a besoin de savoir la fonction
  fp-returning pour compter le `+1` de l'appel récursif, mais le point-fixe ne l'ajoute qu'après. Fix
  identifié : si `x87_depth_pass` **bail**, réessayer en **supposant** `f ∈ fp` (assumption coinductive) ;
  accepter seulement si tous les rets restent à profondeur 1 (une fonction pure-entier analyse proprement
  à profondeur 0 → jamais l'assumption → pas de faux positif). **Vérifié** que ce fix marque correctement
  `0x433c58`/`0x4082a0` fp-returning.
- **Pourquoi NON shippé (principe sacré)** : le fix, bien que **correct et sûr**, ne change **rien
  d'observable** — partial busybox **5** et sqlite **14 inchangés**, et `awk /` reste cassé. Le vrai
  bloqueur est ailleurs : `evaluate` abandonne sur des **joins de profondeur ambigus** (« 1 vs 0 »,
  « 1 vs 2 » à 0x4285ac/0x4285ed/0x429129) **et** la cascade dépend de `0x402220` (dtoa) qui a son propre
  bail. Livrer un changement sans bénéfice mesuré dans une zone **critique pour la justesse x87** viole la
  discipline (risque sans récompense). Réverté ; diagnostic conservé ici.
- **Ce que la session dédiée x87 doit faire** (dans l'ordre) : (1) **réconciliation des joins ambigus** —
  la vraie difficulté récurrente (Lua `intarith`/`forprep`, busybox `awk`/`seq`) : suivre les valeurs
  conservées par `fstp st(i)`/`fxch` dans les idiomes de comparaison NaN, réconcilier les profondeurs
  divergentes aux joins au lieu d'abandonner toute la fonction ; (2) intégrer le fix fp-returning
  auto-récursif ci-dessus (prérequis prouvé, réutilisable) ; (3) résoudre le bail `0x402220` (dtoa).
  Délicat, une fonction à la fois, **difftest + cpudiff + winediff + un filet Lua à chaque pas**.

### Phase 3a — Lua strippé : frontières VÉRIFIÉES saines (le trou « too many registers » est résorbé) ✅
- **2026-07-05 — vérification propre du volet 3a resté ouvert** (Lua strippé, « quelques fonctions aux
  frontières » mal récupérées → miscompile « function needs too many registers »). **Conclusion : le trou
  ne se manifeste plus** — Lua strippé est **pleinement fonctionnel et correct**.
- **Mesures** (mingw-13, `lua_stripped.exe` vs `lua.exe` symbolé) : strippé **833 fonctions** (755 lifted,
  8 partial, 70 host-backed) vs symbolé **903** (804/8/91). **Les deux : 0 appel direct non résolu**, même
  compte de partial (8). L'écart de 70 = surtout reconnaissance host-backed (le symbolé reconnaît 21
  fonctions CRT de plus par symbole/FLIRT) + fonctions non atteintes.
- **Preuve de justesse (2 batteries exigeantes + stress)** : closures/upvalues, métatables/POO,
  coroutines, patterns `gmatch`+captures, `string.pack`/`utf8`, math/flottant, `table.sort`, `pcall`/error,
  ops entières 64-bit (`1<<20`, `//`, `2^10`), varargs, `goto`, GC stress (5000+10000 objets), boucle
  2M itérations. **Sortie strippé = symbolé (byte-exacte) = native** (seule différence : CRLF Windows vs
  LF Unix = mode texte, correct pour un outil natif Linux).
- **Frontières validées** : les 14 entrées présentes au strippé mais absentes du symbolé sont **toutes des
  prologues valides** et de vraies cibles d'appel direct (ex. 0x429d90 : 12 sites d'appel) — des fonctions
  CRT correctement bornées que le strippé **lifte** là où le symbolé les **shim** (différence de
  host-backing, pas d'erreur de frontière). Les 63 entrées symbolé-seulement sont host-backed ou non
  atteintes (0 appel direct non résolu au strippé le confirme).
- **Cause de la résorption** : les correctifs de récupération accumulés dans cette lignée (address-taken
  par immédiats/prologues feuilles, **bornes de table de saut**, tables de pointeurs NULL-tolérantes,
  thunk `call [mem];ret` = `ff 15`, **FLIRT masquant les opérandes relocalisés**) ont éliminé les
  faux-splits qui produisaient la miscompile. Aucune fonction *atteinte* n'est mal bornée.
- **Bilan** : Phase 3a (Lua strippé) est **close** côté justesse — le strippé tourne comme le symbolé. Ce
  qui reste est de la *complétude* de reconnaissance host-backed (cosmétique : verdict INCOMPLETE via
  quelques internes CRT partial, non atteints), pas un bug de frontière.
