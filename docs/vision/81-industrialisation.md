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

### I1 — Traceur d'exécution intégré (ring buffer) ⬜ **[reco n°1]**
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

<!-- NOUVELLES LIGNES D'AVANCEMENT ICI (plus récent en bas) -->
