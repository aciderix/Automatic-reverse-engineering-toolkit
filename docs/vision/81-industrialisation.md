# 81 — Plan d'industrialisation (document vivant)

> **Document vivant** : préparation + plan d'industrialisation d'ARET, **mis à jour au
> fur et à mesure**. Complète le **70** (état/méthode/reste), le **71** (journal), le
> **72** (M7-GUI) et le **80** (orientations architecturales). Il ne les remplace pas :
> il **structure la phase « passer à l'échelle »** (absorber des milliers de binaires
> Win32 historiques au lieu de les corriger un par un).
>
> **Origine** : synthèse critique d'un document d'industrialisation externe (ChatGPT +
> Gemini, fourni par l'utilisateur 2026-07-26) **confrontée à la vérité terrain
> mesurée**. On garde ce qui est utile, on **corrige ce qui est mal diagnostiqué**, on
> **améliore** les propositions. Règle constante : **rien n'entre ici sans être conforme
> au principe sacré (§0 du 70)** — soundness d'abord, oracles comme vérité, séparation
> stricte analyse/IR/runtime, jamais de rustine par binaire.

---

## 0. Objectif de la phase & principe directeur

**Objectif.** Faire baisser le **coût marginal** de chaque nouveau binaire : construire
les **outils qui fabriquent les outils** (automatiser l'ABI, instrumenter le difficile,
reconnaître les patterns connus), pour absorber la **diversité** du monde Win32
historique — sans jamais diluer la garantie « correct ou arrêt bruyant ».

**Principe directeur (non négociable, hérité du 70 §0/§2 et du 80 §3).**
1. Chaque brique d'industrialisation reste **emballée dans « correct ou abort »**,
   vérifiée aux **mêmes oracles**. Un générateur qui produit un stub `aret_unmodelled`
   est *sound* ; un générateur qui produit une **valeur devinée** est **interdit**.
2. **Zéro effet quand désactivé.** Toute instrumentation (trace, compteurs) est **gatée
   à la compile**, **off par défaut**, et laisse le **hash transpile `19acad982194bf07`
   inchangé** + le déterminisme intact.
3. **Mesurer avant de coder** (Levier 0, 70 §5.0). On priorise par la **donnée**
   (wallsweep, oracle Wine), jamais à l'intuition.
4. **Séparation des couches** : l'automatisation ABI et la classification **alimentent**
   l'analyse ; elles ne **contaminent** ni l'IR ni le lifter avec des cas spéciaux.

---

## 1. Analyse critique du document externe (le « comment & pourquoi »)

Le document externe est **remarquablement aligné** sur la philosophie d'ARET (soundness,
oracles, séparation des couches) et sa **thèse est juste** : passer du « fix par binaire »
à une **infrastructure**. Mais, faute d'accès à l'exécution, il a deux angles morts :
(a) il **décrit comme "à faire" plusieurs choses déjà faites** ; (b) sa **Priorité 1 est
mal diagnostiquée**. Bilan :

### 1.1 Le point corrigé par la mesure — `_except_handler4` n'est PAS le mur de WinMerge
- Le document pose *« Débloquer WinMerge = `_except_handler4` + MFC EH »*. **Faux
  empiriquement** (mesuré cette session) :
  - Les 3 murs réels de WinMerge n'étaient **pas** de l'EH : `_EH_prolog3_GS` (ABI),
    continuation de catch qui tronquait une fonction (récup), dispatch C++ trop précoce.
    **Tous corrigés** (commits `4b0f301`, `cd30016`, `ba3d139`).
  - Le mur final : un `throw` réel de MFC lance une **`CUserException`**
    (`.PAVCUserException@@`, via `AfxThrowUserException`) ; la chaîne `fs:[0]` = **une
    seule frame** (le `_except_handler4` top-level du CRT, `prev=0xffffffff`).
  - **Oracle décisif (2026-07-26)** : `wine WinMergeU.exe` (+ `mfc90u/msvcr90/msvcp90.dll`,
    Xvfb) **ouvre la fenêtre principale de WinMerge** (menus + toolbar + « Ready »). ⇒
    l'init MFC **réussit** sous un vrai Win32. Donc la `CUserException` d'ARET **n'est pas
    "unhandled by design"** et **pas une barrière headless dure** : c'est **un retour HLE
    incorrect en amont** qui fait abandonner MFC. Implémenter `_except_handler4`
    **parfaitement ne débloquerait rien** ici (l'exception ne devrait pas être lancée).
- **Pourquoi le modèle externe s'est trompé** : raisonnement *top-down* (« l'EH MSVC est
  dur → le prochain obstacle nommé est `_except_handler4` »), hypothèse **plausible mais
  non testée**. La limite structurelle d'un conseil sans exécution : excellent sur les
  principes, faillible sur « quel est le mur *ce* binaire ».

### 1.2 Ce qui est **déjà fait** (le document valide l'archi sans le savoir)
| Doc externe | État réel ARET |
|---|---|
| §7 hybride lift/HLE user32/gdi32 | **Déjà la stratégie**, prouvée (comctl32 lifté sur HLE gdi32, Levier 1, 70 §5.0). |
| §8 fibers + yield sur `PeekMessage` | **Déjà fait** (fibers 5 incréments ; fix mesuré esp-drift spin-loop `PeekMessage`). |
| §9 soundness / oracles / couches | = **littéralement** le §0/§1/§2 du 70. |
| §4 abstraire l'EH, spécifique-compilo isolé runtime | **Déjà l'archi** : le lifter injecte un **marqueur setjmp générique** au SEH-establish ; le runtime dispatche (brick B/C). Pas de `if MSVC6/7/8` dans le lifter. |
| §6 (partie) reconnaissance structurelle | Déjà, chirurgicale : thunks, `_EH_prolog(3)`, `chkstk`, `memmove` (**prouvé 500/500 vs Unicorn**), `cxx_eh_entries` depuis les `FuncInfo`. |
| §2 (partie) ABI data-driven | Déjà partiel : `stdcall_pops` étendu depuis les **décorations `@N`** des import-libs mingw (vérité-terrain) ; ordinaux résolus depuis les **vraies** export-tables de Wine ; callee-pops internes calculés depuis le **corps** (`compute_callee_pops`). Leur exemple « @16 vs @12 → corruption esp silencieuse » est **en grande partie déjà mitigé**. |

### 1.3 Ce qui est **réellement précieux et nouveau** (à retenir, amélioré au §3)
- **§3 traceur ring-buffer** — le meilleur ratio valeur/risque. C'est exactement ce que
  j'ai fait **à la main** cette session (watchpoints matériels gdb pour remonter la
  corruption `fs:[0]`/`node+4`). Le **systématiser** = multiplicateur de vélocité.
- **§2 générateur ABI** (win32metadata + Wine specs) — solide, = « voie médiane » du 70
  §1/§8.5. Manque chez eux : le couplage avec le **lifting DLL** (le vrai effondrement de
  la traîne d'imports).

### 1.4 Ce qui est aligné mais **à cadrer** (piège doctrinal)
- **§6 classification** : bonne idée **pour prioriser/lire**, dangereuse **si elle pilote
  le comportement**. Reconnaître pour *host-backer* n'est sound **que si prouvé** (cf.
  `memmove`). FLIRT reste **cosmétique** pour nos cibles (70 §4.4). ⇒ aide à la
  priorisation, **jamais** raccourci pour deviner.
- **§4-5 `_except_handler4`** : **vraie brique future** (80 §1.3), méthode
  *instrument-first* correcte — mais **découplée de WinMerge** (à faire quand un binaire
  l'**exige** réellement, pas spéculativement).

---

## 2. État terrain vérifié (chiffres réels, à maintenir)

- difftest **272/272** · transpile hash **`19acad982194bf07`** · ehdiff **6/6** ·
  funcdiff **0 divergence** (~20,5k scorées) · winediff **177/178** (seul rouge =
  `gdi_uifont`, **environnemental** fontconfig i386).
- WinMerge/MFC90 (~40k fn, driver réel) : traverse **tout le CRT + une grande partie de
  l'init statique MFC** ; s'arrête sur une `CUserException` = **manque HLE amont**
  (WinMerge tourne sous Wine → tractable). Briques EH B/C **prouvées sur vrai MFC**.
- Oracles disponibles : cpudiff (Unicorn), funcdiff, difftest(+transpile), winediff,
  DIB-hash, **Wine+Xvfb+capture** (GUI bout-en-bout), sweeps.

---

## 3. Chantiers d'industrialisation (propositions améliorées)

> Chaque chantier : **problème → proposition (améliorée) → conformité → coût/risque →
> oracle → statut**. Statuts : ⬜ à faire · 🚧 engagé · ✅ fait.

### I1 — Traceur d'exécution intégré (ring buffer) ✅ **FAIT (2026-07-26)** · 🔧 **affûté sur le terrain** **[reco n°1]**
> **Affûtage 2026-07-26** : le dump était plafonné à **400** lignes alors que le ring en garde **65536** — l'entrée d'une
> grosse fonction MFC (~600 appels) tombait hors fenêtre, donc l'info **existait mais restait invisible**. Ajout de
> **`ARET_TRACE_DUMP=N`** (0 = tout le ring). A permis de ramener le mur /GS de WinMerge à une dérive esp de **4 octets
> exactement**, en **un run**. **Prochaine amélioration ciblée** : journaliser aussi l'esp **au RETOUR** de chaque appel —
> le traceur ne voit que les entrées, si bien qu'une dérive se **mesure** mais ne se **localise** pas encore ; avec les
> retours, l'appel fautif serait désigné directement.
> **Livré** : `ARET_TRACE=1` (env) préfixe chaque fonction d'un `aret_trace_push(va, esp, eax,ecx,edx,ebp,esi,edi,ebx)` (codegen gaté,
> `emit::set_trace`) ; ring buffer `aret_trace_buf[65536]` (`aret_hle.c`), dump des ~400 dernières entrées sur `aret_unmodelled`/faute non
> gérée/boucle I7. **Off par défaut = byte-identique** (hash `19acad982194bf07` inchangé, difftest 272/272, ehdiff 6/6). Vérifié sur une
> fixture crash. C'est l'accélérateur du mop-up lift-correctness MFC (voir la chaîne d'appels + registres menant à un crash en 1 run).

- **Problème.** Les bugs des gros binaires = *instruction fautive → corruption d'état →
  crash 1000 instructions plus loin*. gdb voit le crash, pas la cause. (Vécu cette
  session : 3 runs de watchpoints gdb pour remonter à `_EH_prolog3`.)
- **Proposition (améliorée).** Un **ring buffer en mémoire** d'événements d'exécution,
  **dumpé uniquement** sur `aret_unmodelled`/`abort`/signal. **Amélioration clé que le
  doc externe rate** : ARET **transpile vers du C**, il n'interprète pas ⇒ la trace doit
  être **émise par le backend** (`structured.rs`) à chaque **entrée de fonction** (et,
  option, à chaque `call`/site EH), pas seulement fournie par une lib runtime.
  - `runtime` : `struct { u32 va, esp, eax, ecx, edx, ebp; u16 flags; } aret_trace[1<<16];`
    + index atomique `aret_trace_head` (lock-free, mono-fiber → trivial ; multi-fiber →
    un buffer **par fiber**). `aret_trace_push(va, regs…)` = écriture O(1) sans I/O.
  - `emit` : sous un flag `--trace` (ou `#ifdef ARET_TRACE`), préfixer chaque fonction
    d'un `aret_trace_push(entry_va, esp, …)`. Le dump (`aret_trace_dump`) est appelé par
    `aret_unmodelled`, le handler `SIGSEGV`/`SIGABRT`, et l'abort EH.
  - Sortie : les N derniers `va esp eax…` → l'équivalent ARET de `WINEDEBUG=+relay` /
    `rr` / trace QEMU, **mais natif et déterministe**.
- **Conformité.** ✅ **Gaté à la compile, off par défaut** ⇒ hash transpile **inchangé**,
  déterminisme intact (règle §0.2). Purement additif. Aucune valeur devinée.
- **Coût/risque.** Moyen (touche `emit` + runtime) ; **risque faible** si strictement
  gaté. Piège : ne pas régresser le hash → vérifier `difftest_transpile` **avec et sans**
  le flag.
- **Oracle.** difftest/difftest_transpile (hash inchangé sans le flag) ; un test dédié
  « crash → dump contient la chaîne d'appels attendue » sur une fixture qui abort.
- **Améliorations au-delà du doc** : (a) **anneau par fiber** (multi-thread propre) ;
  (b) capturer aussi le **dernier `mov fs:[0]`** et le **dernier appel indirect** (les
  deux familles de corruption vues cette session) ; (c) un mode `--trace-teb` qui garde
  un mini-journal des écritures `fs:[0]` (aurait donné la corruption `_EH_prolog3` en 1
  run) ; (d) symbolisation via la table `va→nom` déjà émise.

### I2 — Automatisation ABI + lifting DLL (fusion) ⬜
- **Problème.** Maintenir à la main signatures / `@N` / prototypes / imports = risque de
  dérive **et** temps humain sur la tuyauterie.
- **Proposition (améliorée = leur §2 + le levier qu'ils ratent).**
  1. **Base ABI générée** depuis **win32metadata** (types/signatures/structs modernes) +
     **Wine `.spec`** (ordinals/exports/décorations/ABI historique). Produit : squelettes
     de shims (`aret_X(...) { return aret_unimplemented("X"); }`), table `@N` **prouvée**,
     résolveur d'imports/ordinals. **Sound par construction** (les squelettes abortent).
  2. **Le multiplicateur manquant : lifting DLL** (Levier 1, 80 §1.2) — passer
     comctl32/msvcrt/VB/MFC à *notre* lifter effondre la **traîne** d'imports « gratis »
     (déjà démontré : contrôle comctl32 stateful bit-identique Wine). L'ABI-gen traite la
     **tête** (shims à fort levier) ; le lifting DLL la **traîne** + les runtimes tiers.
- **Conformité.** ✅ La tuyauterie est générée ; le **comportement** de chaque shim non
  trivial reste **vérifié contre un oracle indépendant** (winediff). Un squelette = abort
  sound tant que non implémenté. **Décision licence** (win32metadata = MIT ; ReactOS =
  GPL ; Wine = LGPL) à trancher avant d'embarquer du code lifté redistribué (80 §3.5).
- **Coût/risque.** Moyen-élevé (nouveau générateur + intégration loader). Risque : une
  décoration `@N` fausse = corruption esp → la table doit rester **prouvée** (décorations
  d'import-lib), pas devinée ; test `table_is_sorted` + `stdcall_pops` diff.
- **Oracle.** winediff (comportement), funcdiff (lifts DLL), difftest (non-régression),
  `--mode imports`/`walls` (couverture mesurée avant/après).
- **Amélioration** : générer aussi les **prototypes typés** pour caster correctement les
  args libc (évite le bug « uint64 passé en 2 mots », 70 §7 pièges).

### I3 — Classification automatique des fonctions ⬜ (aide à la priorisation)
- **Problème.** ~40k fonctions ; analyse manuelle impossible à l'échelle.
- **Proposition.** Phase de **reconnaissance** taguant chaque fonction : CRT MSVC / thunk
  MFC / handler EH / wrapper Win32 / utilisateur / inconnu — via imports, RTTI, `FuncInfo`
  MSVC, patterns, (FLIRT en dernier). **But mesuré : reconnaître les milliers de fonctions
  DÉJÀ connues** pour concentrer l'effort sur le reste.
- **Conformité (garde-fou dur).** La classification **priorise et documente** ; elle ne
  **pilote le comportement que si PROUVÉ** (host-back par signature ⇒ vérif Unicorn/Wine
  **avant** d'ajouter la signature, comme `memmove`). Jamais « je reconnais donc je
  devine ». FLIRT reste **cosmétique** pour nos cibles.
- **Coût/risque.** Moyen ; risque **faible** si cantonné à la priorisation. Risque
  **élevé** si on l'utilise pour sauter du lifting → **interdit** hors preuve.
- **Oracle.** Métriques de couverture (combien de fonctions reconnues, combien restent) ;
  aucun oracle de sortie tant que ça ne pilote rien.

### I4 — Architecture EH générique + `_except_handler4` ⬜ (découplé de WinMerge)
- **Problème.** Éviter que le lifter devienne un catalogue de versions MSVC.
- **Proposition.** **Déjà l'archi** : le lifter produit une abstraction (marqueur
  setjmp au SEH-establish) ; le runtime porte le spécifique (`_except_handler3`, C++,
  MFC). **Prochaine brique = `_except_handler4`** (MSVC /GS moderne) : ScopeTable
  **encodée** (XOR `__security_cookie`), validation cookie, layout de frame.
  **Méthode obligatoire = instrument-first** (leur §5, correct) : capturer
  `ExceptionRecord / EstablisherFrame / HandlerData / ScopeTable encodée+décodée /
  SecurityCookie / résultat`, **reproduire l'encodage** avant d'interpréter les
  pointeurs, comparer à Wine. **Testabilité = vrai binaire MSVC `__try` /GS** (mingw ne
  l'émet pas) → fixture inline-asm à la brick C.
- **Conformité.** ✅ in-HLE + abort sound sur le non-modélisé ; WASH : pas de signaux ⇒
  abort sound.
- **Priorité honnête.** **Pas** « la » Priorité 1, et **ne débloque pas WinMerge** (§1.1).
  À planifier quand un binaire l'exige (mesure). Reste une vraie brique de valeur.
- **Oracle.** Fixture SEH inline-asm `/GS` bit-identique Wine + winediff.

### I5 — Surface GUI/COM (le VRAI prochain pas WinMerge) 🚧
- **Problème.** WinMerge (et toute app MFC/GUI) bute sur un **retour HLE incorrect** qui
  fait abandonner MFC (`CUserException`) — alors que **Wine ouvre la fenêtre**. La cause
  est **findable** (data-driven).
- **Proposition.** (1) **Diagnostiquer** : comparer l'exécution ARET vs Wine autour de
  l'init MFC (le traceur I1 accélère énormément) → identifier LA/les API HLE dont le
  retour diverge (registre absent ? COM ? ressource ? `GetModuleFileName` ? locale ?).
  (2) **Combler** la (les) API fautive(s), **vérifiée(s) vs Wine**. (3) Itérer jusqu'à la
  fenêtre. La surface COM/OLE/GUI (272 imports statiques) se traite **par la donnée**
  (wallsweep + oracle), pas en bloc.
- **Conformité.** ✅ chaque API comblée = comportement **vérifié vs Wine** ; non modélisé
  = abort sound.
- **Oracle (nouveau, prouvé cette session).** **Wine+Xvfb+capture** = oracle **GUI
  bout-en-bout** : lancer l'app ARET (SDL) **et** le PE sous Wine sur le même display
  virtuel, comparer les captures (qualitatif) ; DIB-hash pour le bit-exact des
  primitives. Recette au 70 §7 (« écran virtuel »).
- **Statut.** 🚧 driver validé tractable (Wine ouvre la fenêtre) ; prochain incrément =
  isoler la 1ʳᵉ API HLE divergente de l'init MFC.

### I9 — Cache d'objets adressé par contenu ✅ **FAIT (2026-08-01)** · *et il a servi d'oracle*
- **Problème (mesuré, WinMerge + 3 DLL).** `--mode imports` 0,086 s · `--mode walls` 116 s · **141 s** de `cc -O0` pour
  les 254 `.c` générés (316 Mo). La boucle I5 réelle est *éditer un shim → rebuild → relancer* : sur cette édition, tous
  les objets du code lifté sont **bit-identiques** au build précédent. Même gaspillage sur les **194 fixtures winediff**,
  qui recompilent chacune les mêmes trois fichiers runtime.
- **Proposition livrée.** `src/builder/objcache.rs`. Un objet est fonction pure de (compilateur, flags, octets de la
  source, octets de **tout** fichier lu par le préprocesseur). C'est ce dernier point qu'un cache naïf rate. Donc *depend
  mode* : le premier build écrit sa liste **`-MD`**, et une réutilisation **re-hache chaque fichier listé** (headers
  générés **et** système) avant de servir l'objet. Le cache ne peut échouer que **fermé**. Chemins internes à l'out-dir
  stockés **relatifs** ⇒ réutilisables depuis un `--out-dir` neuf, le cas qui compte.
- **Conformité §0.** ✅ La règle « jamais un faux présenté comme correct » s'applique **au cache lui-même** : c'est
  pourquoi la clé est exacte et non heuristique, et pourquoi le SHA-256 est **prouvé sur les vecteurs FIPS 180-4** plutôt
  que supposé (un hash 64 bits serait une vraie façon de servir le mauvais objet). Off par `ARET_NO_OBJCACHE=1`.
- **Effet mesuré** (après le fix de déterminisme ci-dessous — la 1ʳᵉ mesure ne mesurait que le bug) : **WinMerge + 3 DLL**
  4 min 35 → **1 min 58** (254/255 objets réutilisés, seul `aret_layout.S` recompile) · **winediff complet** 6 min 25 →
  **3 min 56** (CPU total 10 min 22 → 3 min 08), verdict **identique** 193/194 et 194 lignes de fixtures byte-identiques.
- **Oracle.** Test dédié contre un vrai compilateur, **dans les deux sens** : *warm* sert des octets identiques ; un
  **header modifié rate**, avec preuve que l'objet périmé aurait été différent. Plus difftest/difftest_transpile passés
  **avec et sans** cache, pour que la preuve ne dépende pas de lui.
- **⭐ Bénéfice inattendu — le cache est un détecteur de non-déterminisme.** Le taux de réutilisation attendu est une
  **mesure** ; l'écart à cette mesure est un bug. Ici : 42 réutilisations sur 255 au lieu de ~255 ⇒ le C généré n'était
  **pas déterministe** (`HashMap` itéré dans le placement des φ, seedé aléatoirement par processus ⇒ 212 des 254 `.c`
  différaient entre deux runs de la **même** commande). Invisible à toutes les portes existantes, parce que le hash
  transpile est **comportemental**. Corrigé (`IndexMap`), vérifié bit-identique sur `sqlite3.exe` **et sur WinMerge + 3 DLL (254/254 `.c`, ELF de 172 Mo)**. Détail 71.

### I10 — Oracle **Windows réel** (GitHub Actions) ✅ **FAIT (2026-08-01)** · *idée utilisateur*
- **Problème.** Toutes les portes comparent ARET à **Wine**. Le 70 §1 enregistre depuis toujours la faiblesse
  honnête : *là où Wine est l'oracle **et** la référence, on vérifie Wine contre Wine*. C'était présenté comme un
  arbitrage théorique.
- **Proposition livrée.** `windows-latest` sur GitHub Actions = le **vrai Win32** contre lequel les binaires
  d'origine ont été construits. `.github/workflows/windows-oracle.yml` (MSVC **32 bits** via `vcvars32`, donc ABI,
  `wchar_t` et layouts identiques à la cible) + `bench/winoracle/` (sondes, `wine_hashes.sh`, README).
- **Conformité §0.** ✅ **Ce n'est pas une porte** : elle produit des **mesures**, pas un rouge. Une divergence
  Windows/Wine est un constat à instruire ; une porte qui rougit pour des raisons que personne ne doit corriger par
  réflexe est **pire qu'aucune porte** (même principe que le rouge instable, 70 §7).
- **Méthode d'échelle.** Comparaison du corpus **en deux temps** : empreinte `nom statut sha256` par fixture des
  deux côtés (le runner, et `wine_hashes.sh` sous Wine) → les fixtures dont l'empreinte diffère **sont** le constat ;
  détail complet imprimé **seulement** pour celles-là. Éligibilité **dupliquée à l'identique** des deux côtés, skips
  **rapportés** (un ensemble qui rétrécit en silence ressemble à un problème qui disparaît).
- **Rendement immédiat** — et c'est ce qui justifie le chantier :
  1. **Le dépôt était inclonable sous Windows** (`:eoy`, `:` interdit par NTFS ⇒ checkout entier avorté). Invisible
     depuis toujours car toutes les portes tournent sous Linux. **Le premier truc que l'oracle a mesuré, c'est le
     projet lui-même.**
  2. **2 divergences sur les 4 premières fixtures**, sur du comportement **déjà livré et déjà vert**
     (`PathAddExtension(…, NULL)` ; code d'erreur de `PathFileExists`).
  3. **Un bug de Wine confirmé** (`PathIsUNCServerA`) et **2 fonctions débloquées** que Wine ne pouvait pas trancher
     (`PathCommonPrefix`/`PathIsPrefix`).
  4. **Un résultat négatif utile** : gatées sur les mêmes lignes, `CommonPrefix`/`IsPrefix` sont identiques sous
     Wine ⇒ Wine se trompe **rarement**, et on sait désormais **où**.
- **Conséquence de porte à connaître.** Quand Windows tranche **contre** Wine, notre shim diverge volontairement de
  Wine et la case **ne peut plus être gatée** en winediff. À écrire dans l'en-tête de la fixture, sinon une session
  future « corrige » en réalignant sur le bug. Le mécanisme générique (divergence **déclarée et surveillée**, qui
  rougit si Wine, Windows ou nous changeons) reste **à poser** — c'est le prochain incrément de ce chantier.
- **Pièges d'infra.** Le workflow déclenche sur `paths:` (un commit hors de ces chemins ne relance rien) ;
  `workflow_dispatch` par l'API répond **403** avec le jeton de session — le **push** est le déclencheur.

---

## 4. Oracles & outillage à ajouter (transverses)

- **Wine+Xvfb+capture comme oracle GUI bout-en-bout** (prouvé 2026-07-26 sur WinMerge :
  fenêtre ouverte). Recette : poser les DLL redist à côté de l'exe, `Xvfb :99`, `wine
  <exe>` vs `./app` (SDL), comparer `import -window root`. Qualitatif (compositing WM) ;
  bit-exact = DIB-hash.
- **Traceur I1** = l'outil de diagnostic central des corruptions tardives.
- **`--mode walls` / `wallsweep`** = déjà là ; à coupler systématiquement à chaque
  chantier pour mesurer l'avant/après (Levier 0).
- **Diff d'exécution ARET↔Wine** (piste) : un mode qui logue les appels d'imports (nom +
  args + retour) des deux côtés et **diffe** — révélerait la 1ʳᵉ divergence d'un gros
  binaire GUI directement (généralise le winediff aux vrais binaires).

---

## 5. Roadmap priorisée (recommandation mesurée)

| Ordre | Chantier | Pourquoi ici |
|------|----------|--------------|
| **1** | **I1 Traceur** | Meilleur ratio valeur/risque ; débloque le diagnostic de tout le reste ; aligné soundness ; gaté. |
| **2** | **I5 Surface GUI/COM** (piloté par I1) | Le **vrai** prochain pas WinMerge, tractable (Wine ouvre la fenêtre) ; data-driven. |
| **3** | **I2 ABI-gen + lifting DLL** | Effondre la traîne d'imports à l'échelle Win32 ; le multiplicateur. |
| **4** | **I3 Classification** (priorisation) | Passage à l'échelle du corpus historique ; garde-fou doctrinal strict. |
| **5** | **I4 `_except_handler4`** | Vraie brique, mais **découplée** de WinMerge ; sur exigence mesurée. |

**Note.** Cet ordre **diffère** de celui du document externe (qui met `_except_handler4`
en #1) — parce que la **mesure** montre que #1 ne débloque pas son propre objectif
affiché. On priorise par la donnée.

---

## 6. Journal d'avancement (mis à jour à chaque incrément)

> Section vivante : une ligne par incrément livré, avec l'oracle qui le prouve. Le détail
> technique va dans le **71** (daté+tagué) ; ici, la **trace de progression** du plan.

- **2026-07-26** — Doc créé. Analyse critique du document externe intégrée. **Corrigé** le
  diagnostic `_except_handler4`/WinMerge par la mesure (Wine ouvre la fenêtre de WinMerge
  → `CUserException` = manque HLE amont, pas EH). Fixes de session livrés en amont
  (`_EH_prolog3`, continuation de catch, dispatch C++ « unhandled » typé). Roadmap
  priorisée posée (I1 traceur en tête). Prochain pas proposé : **I1**, puis **I5** piloté
  par I1.

- **2026-07-26 (I5, incrément 1)** — **Oracle Wine posé** (`wine WinMergeU.exe` + redist → fenêtre WinMerge ouverte : driver tractable,
  pas une impasse). **Cause de la `CUserException` trouvée data-driven** (les `unimplemented import` d'ARET suffisent, sans traceur) : 4
  imports HLE manquants (`_malloc_crt`, `RegisterClipboardFormatW`, `memcpy_s`, `PathFindExtensionW`). **Implémentés + vérifiés
  bit-identiques Wine** (`winecorpus/crt_secure_path.c`). ⇒ WinMerge **franchit l'init statique MFC** et tourne. **Nouveau mur** : hang
  (boucle infinie) dans un ctor global mfc90u — prochain incrément. **Leçon d'infra** : runtime `include_str!`'d ⇒ `cargo build`
  obligatoire après édition `runtime/*.c`. **Valide la méthode I5** (combler les gaps HLE par la donnée, chacun prouvé vs Wine).
  ⇒ Le **traceur I1** reste utile pour le hang (voir où il boucle), mais l'incrément 1 a été craqué sans lui — priorité I1 confirmée
  pour les divergences **non nommées** (comme ce hang).

- **2026-07-26 (I5, incrément 1 — suite : mur suivant classifié)** — Après les 4 shims, WinMerge **tourne à travers une grande partie
  des ctors globaux MFC** puis « hang ». **Classifié (gdb) = boucle de SIGSEGV** : un ctor (`sub_6ac51f`) reçoit `this` dans **`esi`
  (registre callee-saved)** — convention d'aide optimisée MSVC — mais ARET ne **thread pas** `esi/edi/ebx` entrants ⇒ `this=0` ⇒ store
  NULL ⇒ faute réarmée en boucle. **Deux nouveaux chantiers** (voir 71) : **[I6 LIFT-ABI]** threader les callee-saved utilisés en live-in
  (comme `ebp`) — changement de modèle cœur, portes complètes, **session focalisée dédiée** ; **[I7 SOUNDNESS]** une faute sans handler
  qui reprend doit **aborter bruyamment** (pas de hang silencieux) — filet indépendant, moins risqué. ⇒ Le vrai « reste » du blob MFC est
  la **lift-correctness** (registres/ABI), pas l'EH ni les imports (traités). Confirme aussi que le **traceur I1** aurait directement
  montré la boucle de faute (item d'infra toujours prioritaire pour les divergences non nommées).

- **2026-07-26 (I7 — ✅ FAIT)** — Filet soundness : une faute matérielle non résolue qui bouclait en silence (handler rendant
  ExceptionContinueExecution sans corriger la cause — ici `_except_handler4_common` non implémenté → 0) **aborte désormais bruyamment**
  (`aret_hw_fault` : garde no-progrès keyé sur `si_addr`, seuil 16). WinMerge : le « hang » devient un abort typé pointant le NULL (le bug
  I6). Portes vertes (difftest 272/272, ehdiff 6/6, hash inchangé, `seh_hwfault` toujours attrapé). **Reste I6** (threader esi/edi/ebx
  callee-saved live-in) = le vrai fix du store-NULL, chantier cœur dédié (mappé : `ssa/mod.rs` liste reg-param, `build.rs`
  internal_call_args, `structured.rs` sig, `builder/mod.rs` emit_dispatch, `aret_call` .h + sites, backends). Fait proprement en session
  focalisée (hash rebaseline + portes complètes).

- **2026-07-26 (I6 — ✅ FAIT)** — Threader les registres callee-saved `esi/edi/ebx` (live-in) comme `ebp` : liste reg-params
  `[eax,ecx,edx,ebp]`→`[…,esi,edi,ebx]`, threadés à chaque appel direct+indirect, `aret_call` 9-arg, 17 sites runtime alignés, backends
  C+LLVM. **Strictement additif** (le code standard save/restore ⇒ valeur entrante morte ; hash comportemental **inchangé**). Fixe le
  `this`-en-`esi` des helpers MSVC : WinMerge **dépasse le store-NULL** et avance jusqu'à une faute distincte plus profonde
  (`0xc0000005 at 0xe`), désormais **bruyante** (I7). **Portes** : difftest 272/272, cpudiff 6/0, funcdiff 0 div, hash inchangé, winediff
  0 régression. ⇒ Les deux chantiers cœur signalés (I6+I7) sont **faits et vérifiés**. Prochain mur MFC = un accès `[ptr+0xe]` (lift
  distinct, plus profond) — data-driven au coup par coup, ou traceur I1 pour accélérer.

- **2026-07-26 (I5, incrément 2 — mur `0xe` : cause racine prouvée, 1er fix reverté, fix propre borné)** — Le mur `0xc0000005 at 0xe`
  (`sub_7924d5`, mfc90u) diagnostiqué **first-hand** (C lifté + watchpoint matérielle gdb sur l'adresse exacte — la version *sound* de la
  « capture directe de l'instruction corruptrice », pas l'heuristique petite-valeur écartée §1.4/règle §0). **Cause prouvée** : un import
  `__stdcall` (`GetSysColor@4`) appelé **register-indirect CROSS-block** (`v39 = *(iat)` un bloc ; `call v39` en boucle d'autres blocs) ne
  poppait pas ses args (`__aret_callee_pop`=0 sur la VA du slot ; la passe de nommage `held` est remise à zéro par bloc) ⇒ **dérive esp** ⇒ un
  local SEH `[esp+0x30]` aliasait un vieux `push 0xe`. **1er fix** (ajouter les slots d'import à `__aret_callee_pop`) **REVERTÉ** : régressait
  `comctl32_imagelist` (lifting DLL) par **double-pop** in-block (le filet runtime `callee_pop_adjust` + le pop statique existant) puis, après
  retrait du statique, **sous-pop** des slots **fusionnés** multi-modules → interaction trop large avec le chemin lifting-DLL. **Code reverté**
  (`688bee0`), branche re-correcte, 0 régression. **Leçon d'industrialisation** : un changement de callee-pop touche **3 mécanismes** (pop
  statique in-block, filet runtime, pop par nom) **+** la résolution d'imports multi-modules → **lancer winediff (fixtures DLL-lifting) AVANT
  de conclure** ; difftest/cpudiff/funcdiff ne l'attrapent pas (pas d'imports). **Fix propre borné** (prochain incrément) : étendre la seule
  passe `held` au cross-block **sûr** (registres import-invariants, single-def depuis un slot) — sans toucher le filet runtime ni le
  lifting-DLL ; validé par comctl32_imagelist **+** winediff complet **+** WinMerge. Détail 71 (2026-07-26 [ABI][LIFT]).

- **2026-07-26 (I5, incrément 3 — ✅ mur `0xe` RÉSOLU proprement ; WinMerge atteint l'init GUI de MFC)** — Après le revert, fix repris **au bon
  endroit** : pas le filet runtime (qui double-poppait), mais la passe qui suit les registres porteurs d'import, jusque-là **remise à zéro par
  bloc**. Nouveau `block_entry_imports()` = **dataflow MUST** sur le CFG (meet = **intersection** aux jointures ⇒ un mapping ne survit que si
  **tous** les chemins s'accordent ; init **optimiste** ⇒ survit au back-edge d'une boucle ; racine ancrée par **adresse** ⇒ robuste au bloc
  d'entrée qui est lui-même en-tête de boucle ; nommage **après** convergence ; repli = ancien comportement). **Pas de double-pop** : la table
  runtime reste inchangée (rend 0 sur les slots d'import) donc le pop statique in-block fournit `@N` **une seule fois** — et **zéro impact
  multi-modules** (la cause de la régression précédente). **Portes** : **winediff 178/179, 0 FAIL** (la porte qui avait attrapé la régression),
  **comctl32_imagelist MATCH**, difftest 272/272, hash **inchangé**, cpudiff/funcdiff. **Effet** : WinMerge **dépasse** le `0xe`, traverse les
  ctors globaux MFC et **atteint l'init GUI** (`wcscat_s`, `SystemParametersInfoA`), puis **abort sound** sur `SystemParametersInfoA action
  0x29`. ⇒ **Le driver bascule du lift-correctness vers la surface GUI/HLE** = exactement le périmètre **I5**, désormais data-driven au coup
  par coup (chaque API comblée vérifiée vs Wine). Détail 71 (2026-07-26 [ABI][LIFT] ✅).

- **2026-07-26 (I5, incrément 4 — 2 API de l'init GUI MFC comblées, bit-identiques Wine)** — Première application de la méthode I5 **après** la
  bascule vers la surface GUI/HLE : `SPI_GETNONCLIENTMETRICS` (action `0x29`) et `wcscat_s`, **mesurées sous Wine puis reproduites à l'octet
  près** (jamais déduites). Trois pièges attrapés **par la mesure seule** : (a) **A et W n'ont pas le même layout** (`LOGFONTA` 60 o vs
  `LOGFONTW` 92) alors que le shim W **renvoyait vers A** ⇒ mauvais offsets (faux silencieux évité) ; (b) c'est **`cbSize`**, pas `uiParam`,
  qui choisit le layout, et la taille pré-Vista doit laisser `iPaddedBorderWidth` **intact** ; (c) les valeurs **ne se dérivent pas** de notre
  `GetSystemMetrics` (Wine : `SM_CYCAPTION`=26 mais `iCaptionHeight`=25). **Leçon d'oracle réutilisable** : la fixture compare **tous les
  octets bruts** sur un tampon **pré-rempli d'un motif poison** — c'est la seule raison pour laquelle le comportement « le chemin A n'écrit le
  nom de police que jusqu'au NUL et laisse la queue du tableau intacte (sauf le dernier octet), alors que W zéro-remplit » a été vu. **À
  généraliser** : pour toute API qui remplit une structure, poison + dump brut, sinon on valide un sous-ensemble et on laisse passer des
  divergences. Portes : 2 fixtures bit-identiques, `user32_spi` non régressée, difftest 272/272, hash inchangé, winediff complet. **Effet** :
  WinMerge franchit les deux murs et avance jusqu'à **`EnumFontFamiliesW`** (API à **callback** → prochain incrément). Détail 71.

- **2026-07-26 (I5, incrément 5 — `EnumFontFamilies`, 1ʳᵉ API à CALLBACK du chantier)** — Énumération de polices : le callback lifté est
  rappelé par famille (même mécanique que `u32_call_wndproc`). **Apport méthodologique** : c'est la première API du chantier dont la **sortie
  est franchement environnementale** (399 familles installées — autant sous Wine), d'où la règle appliquée ici et **à réutiliser** :
  *séparer le CONTRAT (déterministe, bit-comparé) de la DONNÉE (environnementale, non comparée)*. Le fixture n'assert que des **booléens et
  invariants** (callback rendant 0 ⇒ arrêt immédiat **et retour 0** ; famille inexistante ⇒ 0 callback + retour 1 ; A ≡ W) — **jamais un
  compteur**, qui serait le nombre de polices de la machine. La donnée reste **réelle** : liste depuis **fontconfig** (source de Wine),
  métriques via les **formules déjà vérifiées** de `GetTextMetrics` (`u32_fill_textmetric` refactorisée en `u32_tm_from_face`), famille non
  chargeable **sautée** plutôt que rapportée avec des métriques inventées. **Piège attrapé par la mesure** : `lfPitchAndFamily` ≠
  `tmPitchAndFamily` **toujours** (lf `0x22` vs tm `0x27` : même nibble FF_*, bits bas différents) — ma 1ʳᵉ version copiait l'un dans
  l'autre, divergence invisible sans mesure. **`@N` manquants** (`EnumFontFamilies*` absents de `stdcall_pops`) ajoutés depuis la **vérité
  terrain** (`nm` sur l'import-lib mingw) — la classe de bug qui faisait dériver esp. Portes : fixture identique Wine, difftest 272/272,
  hash inchangé, `table_is_sorted` OK. Détail 71.

- **2026-07-26 (I5, incrément 6 — `_wcsicoll`/`wcscoll` : le nom ment, la mesure tranche)** — Cas d'école pour la règle « mesurer, ne pas
  déduire ». Le nom *collate* pointe vers la machinerie de **sort-keys linguistiques** qu'on possède déjà ; la mesure montre qu'en locale
  **C** (celle d'avant tout `setlocale`) msvcrt collationne **ORDINALEMENT** — `_wcsicoll` ≡ `_wcsicmp`, `wcscoll` ≡ `wcscmp` — et que le
  brancher sur le linguistique aurait donné l'ordre **inverse** sur `readme/read-me`, `~/a`, `O'Brien/OBrien`, soit exactement les cas qui
  justifient cette machinerie. **Technique de fixture réutilisable** : imprimer, sur des paires **choisies pour discriminer**, le résultat de
  la fonction **à côté** des deux réponses candidates (ordinale et linguistique) — le test devient alors une **preuve de la sémantique**, pas
  seulement une non-régression. **Trou de soundness trouvé et consigné sans le corriger à la volée** (70 §P1bis) : `setlocale` accepte
  n'importe quelle locale et rend `"C"` au lieu de NULL ⇒ la valeur C-locale n'est garantie par rien ; fix cadré + précaution (`""` est
  courant ⇒ mesurer le corpus avant de trancher). Portes : fixture identique Wine, difftest 272/272, hash inchangé. Détail 71.

- **2026-07-26 (incrément 7 — `jcc <fonction>` = tail call conditionnel ; fin de la remontée d'API sur WinMerge)** — **Jalon** : c'est le
  premier mur de WinMerge qui n'est **plus un import** — la mop-up d'API est terminée, le reste est du **lift**. Un `jcc` dont la cible sort
  vers une autre fonction récupérée dégradait en `Asm`/abort ; il est désormais lifté comme le `jmp` sortant l'était déjà — un **tail call**,
  simplement **sous condition** (bloc synthétique portant le `Return(tail_call)`). **Propriété de sûreté à réutiliser** : le nouveau bras ne
  capture **que** des cas qui tombaient dans l'abort juste en dessous ⇒ il ne peut que transformer un abort en code modélisé, **jamais**
  changer un programme qui marche — vérifié par le **hash transpile inchangé**. C'est le profil de risque idéal pour toucher au cœur du
  lifter (à opposer à I6/au callee-pop, qui eux modifiaient des chemins déjà exercés). Portes : difftest 272/272, hash inchangé, cpudiff 5/0,
  funcdiff 0 div. **Effet** : WinMerge entre dans `sub_867436` et bute sur un mur **indépendant** — le garde de pile **x87 runtime** (`ud2`).
  **Item de soundness relevé** : ce garde est loud mais **muet** (aucun message, stdio bufferisée perdue ⇒ run « sans sortie » trompeur) ;
  il devrait diagnostiquer avant de trapper, comme `aret_unmodelled`. **Piège d'infra** : `/tmp` plein a produit **104 FAIL winediff
  « PE build: »** — des échecs de **build**, pas de comportement ; toujours qualifier la *nature* d'un FAIL (`df -h`) avant de crier à la
  régression. Détail 71.

- **2026-07-26 (incrément 8 — le garde x87 était sound mais MUET ; il diagnostique désormais)** — Suite directe de l'item de
  soundness relevé à l'incrément 7. `__x87rt_at`/`__x87rt_psh` trappaient par `__builtin_trap()` nu : conforme au §0 sur le fond
  (arrêt, pas de lecture périmée) mais **non diagnostique** — et surtout le trap tue le process **stdout encore bufferisé**, donc
  un run très avancé **paraît n'avoir rien produit** (la fausse piste vécue sur WinMerge). Mesuré avant/après : ancien = **zéro
  sortie**, exit 132 ; nouveau = sortie du programme **préservée** + `x87 runtime stack UNDERFLOW … requested st(0) at depth 0`,
  exit 134. Fix = `aret_x87_stack_error` (flush stdout **d'abord**, puis op/index/profondeur, puis dump de trace I1, puis abort),
  déclarée **`noreturn`** (sinon le compilateur rend atteignable l'accès hors bornes qui suit — on aurait troqué un chemin terminé
  contre de l'UB). **Zéro dépendance de lien nouvelle**, vérifié avant d'écrire : `__x87rt_s`/`__x87rt_p` vivent déjà dans le
  runtime HLE. Portes : difftest 272/272, hash inchangé, **winediff 182/183** (la porte qui compte : changement de runtime).
  **Leçon transverse** : « arrêt bruyant » (§0) et « arrêt diagnostique » ne sont pas la même chose — un abort muet respecte la
  lettre de la règle et en rate l'intention. Détail 71.

- **2026-07-26 (incrément 9 — le mur x87 de WinMerge : une hypothèse d'ABI non vérifiée, trouvée grâce au diagnostic de
  l'incrément 8)** — Boucle vertueuse : le garde x87 rendu **diagnostique** dit immédiatement `UNDERFLOW, st(0) à profondeur 0`,
  ce qui pointe la chaîne `sub_791ebc → _ftol2` en un run. **Cause** : la passe de profondeur x87 *supposait*, commentaire à
  l'appui, que « la pile x87 est vide aux appels » — faux pour `_ftol2` (**tout cast `(__int64)` d'un flottant** en MSVC), dont
  l'argument **arrive dans `st(0)`**. L'appelant statique laissait la valeur en locale SSA, l'appelé runtime lisait une pile
  vide : les deux mécanismes x87 **n'ont de pont que dans le sens RETOUR**. **Fix = vérifier l'hypothèse au lieu de la supposer**
  (§0.4) : `call` avec pile non vide ⇒ bail vers le filet runtime ⇒ une seule pile partagée, accord par construction.
  Conservateur, hash **inchangé**. **Effet** : WinMerge franchit le mur x87 et avance jusqu'au **chargement de polices GDI**,
  mur suivant = `mov [mem], ss` (instruction non liftée). **Leçon d'industrialisation** : les **commentaires qui affirment une
  invariante d'ABI** sont des hypothèses non vérifiées en puissance — à auditer systématiquement dans les zones
  correctness-critiques. Détail 71.

- **2026-07-26 (mur suivant classifié + ⭐ un oracle gratuit découvert)** — WinMerge atteint le chargement de polices GDI puis
  abort sur `mov [mem], ss` dans **`__report_gsfailure`** (MSVC /GS). L'instruction non liftée n'est pas le problème : c'est le
  **chemin d'échec du cookie**. **Découverte structurelle à exploiter** : lifté, le cookie /GS se réduit à
  `esp_épilogue == esp_prologue` (prologue : `[frame] = cookie ^ esp` ; épilogue : relecture ^ esp) ⇒ **tout binaire MSVC /GS
  embarque un détecteur de dérive esp gratuit à chaque épilogue protégé** — précisément la famille de bugs la plus coûteuse
  d'ARET. Atteindre `__report_gsfailure` **prouve** une dérive esp dans l'appelant. Modèle /GS vérifié correct (4 checks sur 5
  passent). Mur = dérive esp réelle dans `sub_791ebc`, **classifié et cadré**, non résolu : session dédiée (`-O0 -g` ou
  watchpoint matérielle sur le slot du cookie, méthode déjà éprouvée sur le mur `0xe`). **Honnêteté** : code nouvellement
  atteignable, donc « préexistant ou non » n'est **pas** testé — les portes (funcdiff 20558/0 div) disent qu'il n'y a pas de
  régression, mais c'est un raisonnement, pas une mesure directe. Détail 71.

- **2026-07-26 (I1 appliqué : la dérive mesurée, et un trou de soundness général mis au jour)** — Le traceur, une fois son
  plafond de dump levé (`ARET_TRACE_DUMP=N`), ramène le mur /GS de WinMerge à **4 octets exactement** en un run. **Point de
  méthode important découvert au passage** : dans le modèle *shared-stack*, `esp` est passé **par valeur** et l'esp de
  l'appelant après un appel est calculé **statiquement** ⇒ une dérive esp n'est **pas** un phénomène d'exécution, elle est
  **figée dans le C généré**. Journaliser l'esp au retour n'apprendrait donc rien : la chaîne se remonte **statiquement**
  (`v609 ← … ← v22`), ce qui a directement désigné un **appel virtuel** comme suspect. **Hypothèse `__aret_callee_pop` posée puis CORRIGÉE par la mesure** : les 55 VAs « non
  récupérées » sont des **slots IAT** (677 déclarés), où rendre 0 est **voulu par le design** (§4.3, c'est ce qui évite le
  double-pop) — **pas** un trou de soundness, **zéro instance nuisible**. Leçon : j'avais tiré une cause d'un **compteur**
  sans qualifier la **nature** des adresses — même piège que les « 104 FAIL » winediff qui étaient des échecs de build.
  Bénéfice net : deux pistes éliminées proprement (l'appel virtuel suspecté est dans une branche **jamais exécutée**). Échafaudage de diagnostic **retiré** (§0.2 « zéro effet quand désactivé »). Détail 71.

- **2026-07-26 (✅ mur /GS RÉSOLU — le callee-pop ne traversait pas les tail calls ; validation de bout en bout de la chaîne I1)**
  — `compute_callee_pops` ne lisait que les `ret N` du corps : une fonction terminée par `jmp <cible>` (thunk MSVC, idiome
  omniprésent) sortait à **pop 0** alors que l'appelant doit honorer le pop de la **cible**. Mesuré : `0x6d96d0 → jmp
  0x6bad9d` (0 au lieu de 4) ⇒ `sub_791ebc` finit **4 octets bas** ⇒ échec du cookie /GS. Fix = arêtes de tail call +
  propagation en **point fixe**. Portes de la zone à haut risque toutes vertes, **dont les 15 fixtures de lifting-DLL**
  (`comctl32_imagelist` incluse — celle qui avait attrapé le revert précédent). WinMerge passe le /GS et avance jusqu'à un
  mur **HLE** simple (`SystemParametersInfoW` action `0x1002`) : **le driver rebascule du lift vers la surface API**.
  **Enseignement d'outillage (le plus réutilisable)** : la chaîne qui a produit ce diagnostic est faite d'incréments ajoutés
  cette session — garde x87 diagnostique → `ARET_TRACE_DUMP=N` → constat que **la dérive esp est STATIQUE** (esp par valeur
  ⇒ figée dans le C) → **instrumentation directe du C généré** (sondes par **numéro de ligne**, recompilation d'un seul
  chunk + relink des `.o` existants). Ce dernier outil est le bon niveau pour toute dérive esp future : il donne une
  timeline sans ambiguïté, là où gdb (l'`__esp` modélisé ≠ `$esp` hôte), la remontée arrière (boucles) et les marqueurs de
  trace par callee (**10 sites** pour un même appelé) échouent tous. Détail 71.

- **2026-07-26 (I5 — famille SPI « UI effects » ; ⭐ et I4 devient exigé PAR LA MESURE)** — Mur `SystemParametersInfoW 0x1002`
  (**SPI_GETMENUANIMATION**) traité **en famille** (§0.3) : les BOOLs par-effet interrogés au démarrage par tout shell/framework.
  **Deux fois la mesure contredit l'intuition** : les valeurs **ne sont pas uniformes** (4 à 1, 12 à 0, non dérivables les unes
  des autres) et les rejets **non plus** (`0x102a`/`0x1082` posent `ERROR_INVALID_SPI_VALUE`, `0x0042` échoue **sans** toucher
  au last-error). **Piège d'oracle attrapé par la fixture** : ma 1ʳᵉ sonde ne remettait pas `SetLastError(0)` avant chaque
  appel ⇒ le 1439 d'une action **fuyait** sur les suivantes et j'ai publié une conclusion fausse ; la fixture, qui remet à
  zéro, l'a fait ressortir en winediff. **Règle à généraliser** : une sonde lisant un **état global** doit le **réinitialiser
  avant chaque appel**, sinon elle mesure la rémanence. Technique du tampon **poisonné** + dump brut reconduite (prouve
  qu'exactement un BOOL 32 bits est écrit ; un test relisant un `int` passerait sur un shim écrivant 8 octets ou aucun).
  Portes : fixture identique Wine, **winediff 183/184**, difftest 272/272, hash inchangé.
  **⭐ JALON** : WinMerge franchit le mur et réclame désormais **`_except_handler4_common`**. Le §I4 posait cette brique comme
  « découplée de WinMerge, à planifier **quand un binaire l'exige** » — **c'est le cas maintenant**. La priorisation par la
  donnée a tenu : I4 n'a pas été construit spéculativement (contre l'avis du document externe qui le mettait en #1), et il
  arrive au moment où la mesure le réclame. Détail 71.

- **2026-08-01 (Levier 1 — la limite mesurée ; I9 — cache d'objets ; ⭐ non-déterminisme du C généré)** — Trois choses liées
  par la même question : *comment baisser le coût marginal sans rien diluer*. (1) **Pourquoi lifter shlwapi n'a rien
  débloqué** : son export `PathAddBackslashW` est un `___wine_spec_imp_*` = `jmp *[IAT kernelbase]`, pas une implémentation
  — le loader a bien routé l'appel vers du code lifté, mais ce code saute dans l'IAT d'un module non chargé ⇒ **le mur
  recule d'un module au lieu de tomber**. D'où une **règle testable en une commande avant de payer un lift** :
  `objdump -t <dll> | grep -c __wine_spec_imp_` vs les exports nommés (comctl32 0/126, ole32 0/301, shell32 4/362,
  kernelbase 2/1402 = implémentent ; **shlwapi 198/362**, advapi32 196/582, version 12/16 = **relais**). Chaîne terminale
  mesurée : kernelbase n'importe **que ntdll** (131 `Nt*` + 212 `Rtl*` + 74 divers) ⇒ le user-mode est **fini**, il bute sur
  131 syscalls NT. ⇒ `Path*`/`Str*` (pur, déterministe) = **shim HLE**, pas lifting. (2) **I9 cache d'objets** livré (voir
  §3 I9). (3) **Le cache a servi d'oracle** et a révélé que le **C généré n'était pas déterministe** — `HashMap` itéré au
  placement des φ ⇒ 212 des 254 `.c` différaient entre deux runs de la même commande, invisible aux portes parce que le
  hash transpile est **comportemental**. Corrigé (`IndexMap`), `sqlite3.exe` bit-identique sur deux transpiles.
  **Leçon d'industrialisation** : un outil de vélocité bien construit produit une **mesure**, et l'écart à cette mesure
  est un bug qu'aucune porte ne cherchait. Détail 71 (2026-08-01).

- **2026-08-01 (I5 — famille `Path*` en 4 vagues ; ⭐ I10 — oracle Windows réel)** — Deux mouvements liés.
  (1) **Bascule du « mur par mur » au « par famille »**, sur remarque de l'utilisateur et conformément au 70 §5.0
  qu'on n'appliquait pas : ~52 shims `Path*` livrés en 4 vagues au lieu d'attendre que chacun devienne un mur, plus
  `GetUserName`. Chaque vague = **une grille de mesure**, et la grille a été **élargie en cours de dérivation**
  plutôt que raisonner par-dessus un trou (deux fois). Trois choses **non livrées volontairement** parce que la
  mesure ne les prouvait pas — c'est le point de l'incrément, pas un manque. (2) **Oracle Windows** (§3 I10) :
  la circularité du 70 §1 passe d'argument à **mesure**, avec un rendement immédiat (dépôt inclonable sous Windows,
  2 divergences sur 4 fixtures, 1 bug de Wine, 2 fonctions débloquées, 1 résultat négatif utile).
  **Leçon d'industrialisation** : un second oracle ne sert pas qu'à trancher les cas ouverts — il **audite les cas
  qu'on croyait clos**, et ceux-là étaient verts.

<!-- NOUVELLES LIGNES D'AVANCEMENT ICI (plus récent en bas) -->

- **2026-08-01 (I5 — ⭐ l'activation COM tourne sur du code LIFTÉ ; I10 — l'infra de l'oracle réparée sur deux points)**
  — (1) `CoCreateInstance` sert désormais une classe **depuis une DLL liftée** de bout en bout (mlang :
  `DllGetClassObject` lifté → `IClassFactory::CreateInstance` à travers la **vtable liftée** → méthodes de l'objet),
  **sans aucune table de CLSID** : on demande à chaque module lifté qui exporte `DllGetClassObject` s'il sert la
  classe, et celui qui ne la sert pas répond lui-même — donc c'est du **vrai code qui répond**, pas nous qui
  devinons, et le mécanisme vaut pour toute DLL COM in-proc future. **La leçon d'industrialisation** : ce que le 70
  §5.0 appelait « le mécanisme de vtable COM » n'a encore une fois **pas existé** ; ce qui manquait était une brique
  d'une trentaine de lignes — publier au runtime les **exports** des DLL liftées, parce que le lifting liait
  jusqu'ici les imports **statiques** (slot IAT) et que COM entre par une porte qui n'est dans **aucune** table
  d'imports. Deux « gros chantiers » annoncés (vtables COM, puis celui-ci) se sont réduits à des briques d'un
  après-midi : **une difficulté nommée dans une roadmap n'est pas une difficulté mesurée**.
  (2) **I10 réparé sur deux points d'éligibilité** — l'étape « sondes » du workflow compile **tout `.c` de
  `bench/winoracle`** (ajouter une sonde = ajouter un fichier ; une sonde cassée ne masque plus les autres), et
  `wine_hashes.sh` **ignore le code de sortie** comme le runner le faisait déjà : `crt_assert` **meurt exprès**, et
  les deux côtés étaient donc éligibles sur des règles différentes — précisément ce que le README interdit, et
  invisible tant qu'on ne relit pas les deux implémentations côte à côte. **Trois cases** de
  `TranslateCharsetInfo` mises en file pour le runner plutôt que tranchées par Wine seul.

- **2026-08-01 (I5 — trois murs de WinMerge, et une prédiction de mesure qui paie six jours plus tard)** —
  `CoCreateInstance` → `SHGetSpecialFolderLocation` → `StrSpnW` → `GetThreadDesktop`, tous franchis, tous de la
  **couverture d'API**. **Le fait marquant est le deuxième** : il a coûté `--with-dll shell32.dll=…` et **rien
  d'autre**. Le 26 juillet, la mesure « shell32 implémente (362 exports, 4 thunks, 36 forwarders) » avait été
  écrite au 70 §5.0 avec la prédiction qu'un lift effacerait toute cette famille ; six jours plus tard la
  prédiction se vérifie **sans une ligne de code**. C'est l'argument le plus concret qu'on ait produit pour la
  règle « mesurer avant de coder » — et le contre-exemple utile est juste à côté : la même méthode, appliquée à
  **shlwapi**, dit « relais » et interdit de le lifter, ce qui a évité de payer un lift inutile une deuxième fois.
  **Deux familles comblées** (shlwapi `Str*` vague 1, window-station/desktop), les deux **bit-identiques Wine du
  premier coup**, ce qui n'était pas le cas des vagues `Path*` — la technique de grille est en train de se
  stabiliser. **Deux cellules de plus mises en file pour l'oracle Windows** (`StrChrW` sur le terminateur, et une
  taille requise qui dépend de la réussite dans `GetUserObjectInformationA`) : le rythme auquel un second oracle
  fait remonter des questions **sur du code déjà vert** ne faiblit pas.
