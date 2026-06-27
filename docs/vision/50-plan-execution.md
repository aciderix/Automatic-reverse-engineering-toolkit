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
   (différentiel **décompile**, -O0→-O3) **et** `bench/difftest_transpile.sh`
   (différentiel **transpile** = le vrai produit) et viser **aucune régression**.
4. Commits petits et fréquents. Pousser souvent.

### Commandes clés
```
cargo build --release
cargo test --release                 # 70 tests (défaut)
cargo test --release --features unpack
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

### Phase 2 — Pruning par accessibilité (CONVERSION CIBLÉE) ⬜
Transpiler **une fonction + uniquement ses callees** (fermeture transitive), pas
tout le binaire. *Livrable* : `--prune`/auto avec `--function`. *Critère* :
transpiler `sub_X` d'un gros binaire ne sort que la fermeture, compile, tourne.

### Phase 3 — Universalité des binaires **strippés** ⏳ EN COURS
Deux volets : (a) **récupération de fonctions** sans symboles, (b) **reconnaissance
CRT** (FLIRT mingw/MSVC). Volet (a) prioritaire car c'est le blocage mesuré.
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
- *Reste 3a* : ~16 fonctions encore manquantes au strippé ; surtout, une fonction
  reste **mal récupérée** (frontières) → Lua strippé bute désormais sur une erreur
  *interne* Lua (« function or expression needs too many registers ») = un sous-
  programme miscompilé parmi les 971. Forensics ciblée à faire (diff symbolé/strippé
  des frontières de fonctions).
- *Reste 3b* : signatures FLIRT pour `ucrtbase`/`msvcr*` (MSVC) + DB plus large
  via `--mode gensig`. *Critère* : un exe MSVC strippé reconnaît son CRT et tourne.

### Phase 4 — vtables / appels indirects C++ (VRAI CODE C++) ⬜
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
