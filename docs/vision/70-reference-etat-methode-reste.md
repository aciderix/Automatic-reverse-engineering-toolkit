# 70 — Référence ARET (état, méthode, reste-à-faire, tips)

> **Document de référence unique — à lire en premier** pour récupérer le contexte
> après compression. Il **remplace** le 50 (ex-journal) et le 60 (doctrine, absorbée
> §1). Il se veut **complet mais synthétique** : tout l'essentiel est ici ; le
> **détail exhaustif** vit dans le journal structuré (doc **71-journal-de-bord**).

## Système documentaire (où aller chercher)

| Doc | Rôle | Quand l'ouvrir |
|-----|------|----------------|
| **70** (ici) | **Référence d'état** : objectif, règles, architecture, FAIT, RESTE, tips, roadmap complète | **Toujours en premier** (contexte après compression) |
| **71-journal-de-bord** | **Journal structuré & cherchable** : fiches par sous-système + entrées datées, détail technique complet de chaque fix | Pour **retrouver une info précise** (grep par tag/sous-système) |
| **50-plan-execution** | **Archive** : journal chronologique append-only historique (3500 l.) | Uniquement pour le récit détaillé d'un vieux fix non encore migré en 71 |
| **00 / 01 / 30** | **Vision stratégique** : roadmap UBT, design, intégration briques (Wine/DXVK/LLVM) | Résumé intégré ici §6 ; ouvrir pour la vision d'origine |
| **80-orientations-architecturales** | **Design des grands chantiers à venir** : fibers (threads), lifting DLL binaire, SEH in-HLE, PGL, SoftFloat, **rétro-cible Windows** (§1.6) — verdicts + conformité au principe sacré | Avant d'engager threads/DLL/SEH/indirects/x87-universel/**rétro-cible-Windows** |
| **81-industrialisation** | **Plan d'industrialisation (document vivant)** : passer à l'échelle (traceur d'exécution, ABI-gen + lifting DLL, classification, EH générique, surface GUI/COM) — analyse critique du doc externe ChatGPT/Gemini + roadmap priorisée par la mesure | Avant/pendant la phase « scale » ; **maj à chaque incrément d'industrialisation** |
| **HANDOFF / 40** | Architecture détaillée, pièges, état des lieux | Complément |

**Comment un agent trouve une info** : `grep` le tag de sous-système dans **71**
(tags définis en tête du 71 : `[X87]`, `[ABI]`, `[RECOV]`, `[HLE-STDIO]`,
`[HLE-FILE]`, `[HLE-WIN32]`, `[LIFT]`, `[SSA]`, `[ORACLE]`, `[RECOMPILE]`, `[64BIT]`,
`[THREAD]`, `[GUI]`, `[DEMO]`, `[INFRA]`). Chaque fiche 71 pointe vers la (les)
section(s) du 50 pour l'historique complet.

**Protocole de mise à jour (obligatoire pour chaque agent)** : après un incrément,
(1) **résumer l'état** ici dans le 70 (§4 FAIT / §5 RESTE / §3 chiffres régression —
on **met à jour**, on n'empile pas) ; (2) **écrire l'entrée détaillée** dans le
**71** (journal, datée + taguée) ; (3) commit + push. Le 70 reste **mince** ; le
détail grossit dans le 71.

---

## 0. Objectif & principe sacré

**Objectif final.** Prendre un programme **Windows (/Linux/macOS)** et le convertir
en programme **natif d'un autre système** (ELF direct **ou** WASM), **entièrement
fonctionnel comme natif, sans émulation** — **universel**.

**Principe sacré (non négociable).**
> **Application 2026-07-26** : un **import non implémenté ABORTE** (`aret_unimpl`) au lieu d'avertir puis de rendre 0 —
> le programme ne peut plus tourner sur une valeur qu'il n'a jamais produite (et `0 == S_OK` faisait croire au succès
> d'une API `HRESULT`). Canal **distinct** `aret_partial` pour une API **modélisée** dont un **sous-cas** ne l'est pas
> et qui rend un **échec défini** : ça, c'est sound, et ça continue.
1. **Jamais de sortie incorrecte présentée comme correcte.** Juste, ou **arrêt
   bruyant** (`aret_unmodelled`/abort). Jamais un no-op silencieux, jamais une
   valeur devinée. Un faux silencieux est **pire que rien**.
2. Tout ce qui n'est pas sûr **reste `Asm`/`__asm__` → abort** au runtime
   (statement *et* expression).
3. **Jamais de rustine par binaire.** On corrige la cause **générale** (une classe
   entière de binaires). La seule rustine ayant existé (stub VFS sqlite) a été
   explicitement levée.
4. **Rien de prouvé = rien de deviné.** Mode d'arrondi x87, profondeur de pile,
   noreturn, cible d'appel indirect : non prouvé ⇒ fallback sûr (runtime x87) ou
   abort, jamais une hypothèse optimiste.

**Garantie réellement atteignable** (cf. limite dure §8) : *« fonctionnel, OU
arrêt qui dit où — jamais faux en silence »*. Le trio « tout binaire + 100 %
fonctionnel + 100 % natif pur » est **prouvé impossible** en général
(indécidabilité). Le vrai logiciel compilé (sqlite, Lua, jeux…) est, lui,
**pleinement atteignable**.

---

## 1. Doctrine — réutilisation vérifiée (ex-doc 60)

L'identité d'ARET n'est **pas** « tout écrire à la main » (ça c'est un choix de
mise en œuvre). Les vraies valeurs : **Sound, Vérifié, Natif, Général**. Elles
vivent dans la **couche de vérification** et la **discipline de frontière**, donc
elles sont **transférables** sur des briques réutilisées.

> **Règle d'or.** Une brique réutilisée n'est jamais une boîte noire de confiance :
> c'est un composant qu'ARET **vérifie et emballe** dans son contrat « correct ou
> abort », avec les mêmes oracles que du code maison.

| Axe | Brique réutilisable | Oracle | Philosophie préservée ? |
|-----|---------------------|--------|--------------------------|
| Instructions CPU | spec sémantique → **notre** backend natif | **Unicorn** | ✅ totale |
| Comptage x87 | filet runtime (pile FPU à l'exécution) | différentiel bout-en-bout | ✅ totale |
| APIs Windows | **Winelib** (implés natives Wine) *ou* shims win32metadata | **Wine** | ⚠️ presque (voir arbitrage) |
| Récupération de fonctions | analyse type Ghidra/angr | différentiel de sortie | ✅ si vérifié en sortie |

**Fait établi (vérifié 2026-07-03)** : **Wine n'est PAS un émulateur.** Ses
`.so` kernel/ntdll sont de l'`ELF 32-bit natif` ; un PE sous Wine exécute son i386
**directement sur le CPU**, aucun qemu/box86. Un binaire `winegcc -m32` appelant
`GetVersionExA`/`GetFileAttributesA`/`GetLastError` sort un ELF natif et rend
**exactement** `lasterr=2` (`ERROR_FILE_NOT_FOUND`) = la valeur codée dans notre
shim → l'implé native de Wine **confirme** notre comportement.

**Seul arbitrage honnête** : si Wine est **à la fois** l'oracle (winediff) **et**
l'implémentation (Winelib), on « vérifie Wine contre Wine » (circulaire). Deux
positions valides : (a) assumer Wine comme vérité terrain pratique (ses trous sont
testés par le monde entier, jamais des faux silencieux) ; (b) voie médiane :
auto-générer la **tuyauterie** des shims depuis win32metadata, garder le
**comportement** vérifié contre un oracle indépendant. **Intégration Winelib =
étape une fois, pas par binaire** (mécanisme prouvé ; toolchain demande un env
propre).

---

## 2. Méthode de travail (à respecter, sans contournement)

1. **Une tâche à la fois**, méthodique. Pas de big-bang.
2. Par tâche : (a) comprendre/**reproduire** → (b) **fixture minimale testable**
   (mieux qu'un gros binaire) → (c) implémenter → (d) **vérifier** → (e) **commit
   descriptif** → (f) **doc** : entrée détaillée datée+taguée dans le **71**
   (journal) **et** mise à jour de l'**état ici** (70 §4/§5/§3). *(Le 50 n'est plus
   alimenté ; il reste l'archive historique.)*
3. **Ne jamais casser la régression.** Portes (voir §3) : `difftest.sh` +
   `difftest_transpile.sh` obligatoires pour tout changement lift/structure ;
   lifter ⇒ **en plus** `cpudiff` + `funcdiff.sh` ; large ⇒ `regression.sh` +
   `winediff.sh`. **Aucune régression** tolérée.
4. **Commits petits, fréquents, poussés.** Le conteneur est **éphémère** : le
   non-committé est perdu au reset. Commit après **chaque** incrément.

**Discipline stratégique (leçons chèrement acquises)**
- **Mesurer, ne pas affirmer.** La complétude se **mesure** contre la vérité
  terrain (Wine/Unicorn/natif) sur un vrai binaire — jamais « ça a l'air de
  marcher ». (Contre-exemples attrapés : `ceil(3.2)=3.0`, `sin(sin(1))`, 2
  incréments x87 révoqués.)
- **Différentiel *large* obligatoire** pour les transcendantes / zones critiques :
  un test étroit (`sin(0)`) masque les faux silencieux.
- **Borner puis pivoter** : dès qu'un bug n'est pas généralisable rapidement, on le
  **documente** et on passe. Pas de forensics mono-binaire infinie.
- **Pas de changement sans bénéfice mesuré dans une zone correctness-critique**
  (x87, esp/frame) : risque sans récompense = révoqué (cf. fp-returning
  auto-récursif : correct mais 0 effet → réverté).
- **Vérifier si le fallback runtime est actif AVANT de conclure à un abandon x87
  statique.**

---

## 3. Architecture, commandes, portes de régression

### Deux pipelines
- **A — décompile** (`--mode verify`/`emit`) : oracle interne, args = paramètres
  uint64 sign-étendus. Gardé par `difftest.sh` (O0→O3, 271/271) + Z3/`smt_rewrites`.
- **B — transpile** (`--mode transpile`) = **le vrai produit** (modèle
  *shared-stack*). Gardé par `difftest_transpile.sh` (4/4, hash **`19acad982194bf07`**
  = empreinte comportementale : inchangé ⇒ produit byte-identique).

### Chaîne transpile
`PE loader` → **`analysis`** (récupération de fonctions, tables de saut) →
**`ir/lift.rs`** (sémantique par-instruction) + **`ir/build.rs`** (modèle
esp/appels/frame) → **`ssa`** → **`opt`** → **`emit`** (`structured.rs` = C,
`llvm.rs` = LLVM ; WASM via backend C) → recompilation ELF/WASM.

**Modèle shared-stack (clé)** : `esp` passé **par valeur** aux fonctions liftées ;
la pile machine est une **région unique partagée** ; `ebp` **threadé** comme 5ᵉ
registre-param (callee-saved) ; l'effet net d'un appel est modélisé statiquement.
`ebp` est un registre-param (per-appel, donc thread-safe). Le multithread se fait
par **fibers coopératifs** (§4.7, doc 80) : une seule coroutine court à la fois, la
pile machine est **par-fiber** (malloc), et l'état global par-thread (`last_error`,
TEB à venir) est **swappé par le scheduler** à chaque bascule. Sans `CreateThread`,
tout reste strictement mono-thread (byte-identique).

### Fichiers importants
- `src/ir/lift.rs` — sémantique par-instruction (le « lifter »).
- `src/ir/build.rs` — esp, appels, callee-pop, frame helpers, self tail-call.
- `src/ir/stdcall_pops.rs` — table `API stdcall → @N` (triée, binary-search).
- `src/analysis/mod.rs` — récupération de fonctions, tables de saut/pointeurs.
- `src/emit/{mod,structured,llvm}.rs` — backends, helpers `__ix_*`/`__fp_*`/`__x87*`.
- `src/cpudiff.rs` — oracles Unicorn (per-instruction + funcdiff closure + opt-diff).
- `runtime/aret_hle/{aret_hle.c,aret_crt.c,aret_win32.c,aret_hle.h}` — HLE.
- `src/flirt.rs` — signatures FLIRT (reloc-wildcarded).

### Commandes
```bash
cargo build --release
cargo test --release                        # suite défaut
cargo test --release --features unpack       # + unpacker + cpudiff/funcdiff
cargo test --release --features unpack cpudiff
bash bench/regression.sh    # PORTE unifiée : difftest 271/271, in-place 3/3,
                            # magicdiv 2^32, funcdiff corpus (0 div), SMT 11/11,
                            # recompilabilité gzip/ls/cat 100%
bash bench/difftest.sh              # décompile O0→O3
bash bench/difftest_transpile.sh    # transpile (hash 19acad982194bf07)
bash bench/winediff.sh              # axe 2 vs Wine — ~235 s cache chaud (385 s a froid) : 151 fixtures en PARALLELE, les 43 qui creent
#   une fenetre en SERIE (Wine GUI concurrent degrade l'ORACLE : listbox vide, focus perdu)
bash bench/winediff.sh NOM          # une seule fixture (~20 s) : la boucle de dev
WINEDIFF_JOBS=1 bash bench/winediff.sh   # force la serie (bisecter une fixture instable)
#   ⚠️ chaque fixture a son PROPRE repertoire ET son PROPRE Xvfb : un display partage
#   deplace les fenetres de l'ORACLE sous concurrence (mesure sur win_timechar), et un
#   `wineboot` sans display change le placement ensuite -> les deux sont necessaires.
bash bench/funcdiff.sh              # lift-closure + opt-diff vs Unicorn (0 div)
bash bench/stdcall_audit.sh         # PORTE ABI : tout shim __stdcall prouve a son @N dans la table
                                    # (la famille esp-drift ; invisible a difftest/cpudiff/funcdiff)
# Sweeps de vrais binaires (téléchargent + comparent à Wine) :
bash bench/sqlite_sweep.sh   bash bench/busybox_sweep.sh   bash bench/corpus_sweep.sh
bash bench/gauntlet/score.sh        # 21 binaires gauntlet, corpus dans le repo

# Produit :
aret <exe> --mode transpile --out-dir OUT [--entry main|0xADDR] [--function NAME]
     [--run -- ARGS…] [--backend llvm | --target wasm] [--strict]
aret <exe> --mode imports           # couverture statique d'imports (axe 2 a-priori)
aret <exe> --mode walls             # CARTE DES MURS statique complète (une passe) :
       # instructions non liftées (par nb de sites) + imports manquants + appels non résolus.
       # Sans émettre/compiler. La vue d'ensemble : voir TOUS les murs d'un coup au lieu de les
       # subir un par un au runtime. Agrégeable sur un corpus (grep les compteurs) pour dégrossir.
bash bench/wallsweep.sh <dir1> [dir2…]  # AGRÈGE --mode walls sur un corpus : murs classés par
       # nb de BINAIRES bloqués (largeur) → prioriser un fix par la donnée. Couplé aux oracles.

# Cache d'objets (doc 81 §I9) — ACTIF PAR DÉFAUT, ne change jamais les octets produits :
#   la clé couvre compilateur+flags+source ET la liste `-MD` complète des headers lus,
#   re-hachée à chaque réutilisation ⇒ un header modifié RECOMPILE (échec fermé, jamais ouvert).
ARET_NO_OBJCACHE=1 …        # désactiver (à faire pour une preuve indépendante du cache)
ARET_OBJCACHE=<dir> …       # emplacement (défaut $XDG_CACHE_HOME/aret/obj)
ARET_OBJCACHE_MAX_MB=<n> …  # budget, éviction LRU en fin de build (défaut 4096)
```

```bash
# ORACLE WINDOWS (doc 81 §I10) — un VRAI Windows via GitHub Actions, PAS une porte :
#   .github/workflows/windows-oracle.yml  (MSVC 32 bits, declenche au push sur bench/win*)
#   bench/winoracle/wine_hashes.sh        (le MEME tableau nom/statut/sha256, cote Wine)
# Usage : diffier les deux listes -> les fixtures dont l'empreinte differe SONT le constat ;
# n'imprimer le detail complet que pour celles-la. Cf. bench/winoracle/README.md.
```

**Déterminisme (propriété acquise 2026-08-01)** : deux transpiles de la même commande produisent
désormais des `.c` **bit-identiques** et le **même ELF**. Ce n'était pas le cas avant (un `HashMap`
itéré au placement des φ, seedé aléatoirement par processus) et **aucune porte ne pouvait le voir**
— le hash de `difftest_transpile` est **comportemental**. Le taux de réutilisation du cache d'objets
sert désormais de **détecteur** : un warm build qui ne réutilise pas ~tout signale un non-déterminisme.

### État régression (référence — doit rester vert)
difftest **272/272** · transpile-diff **4/4** (H=`19acad982194bf07`) · winediff
**193/194** (le seul rouge = `gdi_uifont`, **environnemental** : fontconfig i386, orthogonal au code) · **ehdiff 6/6** (SEH `seh_except`
+ C++ `throw_catch`/`throw_dtor`/`throw_across`/`throw_byval`/`throw_static` — throw/catch, destructeur d'unwind, multi-frames, catch-by-value, CRT statique — bit-identiques Wine) · cpudiff vert (per-instruction + séquences génératives) · funcdiff corpus **0 divergence** (lift **~20,6k** scorées /
**~20k appels** — **imports (stubs `@N` + `@0` scalaires) + appels indirects résolus + intrinsèques mémoire host-backés (memmove/memcpy)** ; scratch sous-esp exclu ; opt ~10k scorées) · SMT **11/11** · in-place **3/3** · magicdiv **2³²** ·
recompilabilité **100 %** · WASM **7/7**.

---

## 4. Ce qui est FAIT (toutes les cartes en main)

### 4.1 Axe 1 — justesse de traduction CPU (validé contre Unicorn)
- **Drapeaux width-aware** : CF/ZF/SF/OF/**PF/AF** corrects par largeur (8/16/32),
  masquage des opérandes, préservation à compte nul (shifts/rotates). 7+ bugs de
  drapeaux trouvés par cpudiff et corrigés (retenue, ZF count=0, inc/dec OF,
  rol/ror CF, adc/sbb carry, `sub_flags` cmp signée 64-bit…).
- **64-bit sur cible 32-bit** : retour `long long` en paire **edx:eax** ; comptage
  de décalage masqué (5/6 bits) ; arithmétique `add/adc`,`sub/sbb` à immédiat signé
  (retenue sur opérandes masqués).
- **div/idiv fidèles** : reproduisent le **#DE** (div0 **et** débordement de
  quotient) via `__ix_*div*/*mod*` → crash exact, jamais une troncature silencieuse.
- **SSE scalaire + SIMD packed** validés bit-à-bit contre Unicorn (`__fp_*`/`__ps_*`/
  `__pi_*`). Bug `ss` (préservation `[63:32]`) corrigé. SSE2-string
  (pcmpeqb/pmovmskb/pshuflw…). **SSE2 packed-integer élargi** (2026-07-16, **piloté par la donnée** — famille
  dominante des lift-gaps sur les vrais binaires WineHQ) : `paddb/psubb/psubw/psubq/pmullw`, décalages de lane
  `pslld/psrld/psrad/psllw/psrlw/psraw` (compte imm **et** registre), `packuswb/packssdw` (saturation),
  `punpcklbw/punpckhbw`, `pextrw`, et `psrldq/pslldq` **généralisés** (tout compte 0-15). Chacun **bit-identique
  Unicorn** (corpus cpudiff). Effet mesuré : gaps de lift `gdi32_test` **147 → 19 sites** (le reste = données
  décodées-en-code : `sti/hlt/in/out/ud2` → abort correct, + MMX). Débloque le code vectorisé (graphisme, boucles
  auto-vectorisées, string/hash).
- **Instructions de chaîne + drapeau de direction (DF)** : `movs/stos/lods/scas/cmps`
  (non-rep) + `rep movs/stos/scas/cmps` (helpers `__rep_*`). `rep(ne) cmps` = **idiome
  memcmp/strcmp** (`repe cmpsb;je…`). **DF modélisé** (`FlagKind::Df`, EFLAGS bit 10) :
  `std`→DF=1, `cld`→DF=0 ; chaque op lit DF pour le **sens d'avance** (avant/arrière),
  les helpers `__rep_*` prennent un flag `back` (memmove-overlap `std;rep movs`, strrchr
  `std;repne scasb`). Avance = pas **signé** `size*(1-2·DF)` ; dernier élément à
  `reg−step`. DF **= 0 à l'entrée** (init SSA = convention ABI) → code sans `cld` tourne
  avant. Gardé `winecorpus/str_direction.c` (movs/stos/scas/cmps + non-rep, **2 sens**,
  bit-identique Wine) + `str_repcmps.c`. Seul `rep lods` (rare) = **abort sound**.
- **Divers** : `cpuid`/`xgetbv` (host réel, **AVX/SSE4.2 masqués** → chemins SSE2
  liftables), `bt/bts/btr/btc` (reg + `[mem],imm` + **`[mem],reg`** = idiome bit-array,
  offset registre non masqué → décale l'adresse `base+SAR(idx,log2w)*(w/8)`, gardé
  `winecorpus/bt_mem_reg.c`), `stmxcsr`/`ldmxcsr`,
  `cmov/setcc`, `xchg` high-byte.
- **Émission (backend) — `imul` 1-opérande signé × opérande constant** : le magic
  d'une division/modulo signé par constante (bit 31 set, ex. `%23`→`0xb21642c9`)
  arrive sous `SignExtend` comme `Const` nue (opt folde `const & 0xffffffff`) ;
  `signed_cast`/`emit_sign_extend` la sign-étendent depuis `int_bits(ty)` (sinon
  zéro-étendue → `mulhs` faux → `%N` faux). Invisible à cpudiff (bug d'**émission**,
  pas d'IR). Débloqué sqlite3 **mingw**. Garde `signed_magicdiv.c`.
- **SSA — split du bloc d'entrée = en-tête de boucle** (`src/ssa/to_ssa`) : quand
  l'entrée d'une fonction est elle-même un en-tête de boucle (back-edge vers
  l'entrée ; typique d'un param **regparm** qui est la variable de boucle), la φ de
  l'en-tête n'avait pas de prédécesseur pour l'edge d'entrée → **valeur initiale
  (le param) perdue** → var d'induction jamais avancée (boucle infinie/valeur
  fausse). Fix : insérer un **pre-header** qui reprend la VA `func.entry`. Gardé par
  funcdiff opt-diff + fixture `loop_header_entry`. Débloqué sqlite3 mingw **CRUD**.

### 4.2 x87 (deux mécanismes : statique + filet runtime)
- **Passe de profondeur statique** (`x87_depth_pass`) : compte la pile FPU
  inter-instructions ; un `call` fp-returning compte `+1`. Ops modélisées : load/
  store (f/fi 32/64/80), constantes (fld1/fldz…), arith (toutes formes),
  `fxch/fabs/fchs/fsqrt/frndint` (**4 modes d'arrondi prouvés** via slot control
  word), `fcom/fucom/fcomi/ftst/fxam/fcmovcc/fprem`. Retour fp acheminé par
  `__aret_x87_ret`.
- **Filet runtime** (`__x87rt_*`, incrément 1, SOUND) : quand la passe statique
  **bail**, les ops FPU s'exécutent contre une **pile FPU runtime** (correcte par
  construction, bornée → abort sur under/overflow). ⚠️ Cet abort était **muet**
  (`__builtin_trap` nu) jusqu'au **2026-07-26** : il tuait le process **stdout bufferisé**,
  donc un run très avancé **paraissait n'avoir rien produit** (fausse piste vécue sur
  WinMerge). Il route désormais sur **`aret_x87_stack_error`** — `fflush(stdout)` d'abord,
  puis op/index/profondeur (UNDERFLOW vs OVERFLOW), puis dump de trace I1, puis `abort` ;
  déclarée `noreturn` (sinon l'accès hors bornes suivant redevient atteignable = UB).
  L'arrêt est inchangé, seule la valeur diagnostique est ajoutée. Gaté transpile-only,
  purement additif. Couvre load/store/const/arith/fxch/fabs/fchs/fsqrt/frndint/
  fldcw + **compare `fcom/fucom st(i)`** (fix récent : lire `op_count()-1`, pas ST0)
  + **les transcendantales brutes** `fsin/fcos/fptan/fpatan/fyl2x/f2xm1/fscale/fsincos`
  (`__x87rt_2xm1`/… ) — **8/8 vérifiées bit-identiques à Wine** (2026-07-10, fixture
  inline-asm `winecorpus/x87_transcendental.c`, non host-backée ⇒ prouve le filet).
- **Frontière statique/runtime x87 : un `call` avec pile NON VIDE ⇒ bail (2026-07-26)**. La passe de profondeur
  *supposait* « la pile x87 est vide aux appels » (commentaire d'origine) : vrai en code compilé normal, **faux** pour
  les helpers CRT prenant leur argument dans `st(0)` — **`_ftol2`/`_ftol`**, soit **tout cast `(__int64)` d'un flottant**
  en MSVC. Un appelant **statique** laissait alors la valeur dans une locale SSA pendant que l'appelé, bailé en **mode
  runtime**, lisait la pile runtime **vide** (le pont existe pour le RETOUR — `__aret_x87_ret` — pas pour l'ARGUMENT).
  Fix = **vérifier l'hypothèse** (§0.4) : `sp > 0` à un `call` ⇒ bail vers le filet runtime ⇒ appelant et appelé
  partagent **une** pile et s'accordent par construction. Conservateur (le `sp>0` à un call est le cas rare du helper).
  Débloqua le mur x87 de **WinMerge** (`sub_791ebc` → `_ftol2`). Hash **inchangé**. Cf. 71 (2026-07-26 [X87][LIFT]).
- **Transcendantes = libm host-backed** (pow/sin/cos/exp/log/fmod/atan2… via
  `crt_symbol`/nom/FLIRT) → on branche la vraie libm au lieu de lifter du x87 dense.
  Cause racine du double-`sin` corrigée (helpers effacent **C2**).
- **`fstcw`/`fnstcw` = store constant `0x037F`** (le mot de contrôle x87 par défaut Linux/ELF), pas un `Nop`
  (2026-07-17, trouvé par funcdiff sur `dxfix.exe`) : le Nop laissait la destination non-écrite → `_control87`/
  `_controlfp` lisait de la **pile non-init = garbage** (faux silencieux). Le constant est portable (WASM, aucun
  global). funcdiff seed le FPCW d'Unicorn à `0x037F` pour matcher. `fldcw` reste Nop (l'arrondi est tracké
  statiquement pour `frndint`/`fist`).

### 4.3 ABI / frame / appels
- **Callee-pop `ret N`** modélisé : **imports** (`stdcall_pops.rs`) **et** fonctions
  **internes** (`compute_callee_pops`, direct = N constant, indirect = table runtime
  `__aret_callee_pop`, gardé par `has_callee_pops` → 0 régression cdecl). *(fix
  cksum : la dérive esp −4/itér des drivers FAST_FUNC/stdcall.)* Sans le pop, un stdcall
  non tabulé dérive esp en **push-model** → miscompile **silencieux** dans le code **FPO**
  (masqué par `mov esp,ebp` là où un frame pointer existe). Table **élargie par la donnée**
  (2026-07-17) : +51 `@N` **prouvés** (vérité terrain = décoration `@N` des import libs mingw
  i686), mesurés depuis les imports de **7za** (`ReadFile@20`/`WriteFile@20`/`Heap*`/`Virtual*`/
  `CreateFileW@28`/`RaiseException@16`/`RtlUnwind@16`…). Bénéfice **mesuré** : funcdiff **16,6k→18,4k**
  scorées (0 div — ces lifts, jadis skippés derrière imports, désormais **prouvés corrects**).
  **Import stdcall appelé register-indirect CROSS-BLOCK ✅ (2026-07-26)** : `mov reg,[iat]` dans un bloc et `call reg` dans un
  autre (MSVC optimisé : import mis en cache dans un registre callee-saved, appelé en boucle) n'était **ni nommé ni poppé** ⇒
  dérive esp de `@N`/appel (mur `0xe` de WinMerge/mfc90u : `GetSysColor@4` en boucle ⇒ un local SEH `[esp+0x30]` aliasait un
  vieux `push`). Fix = **`block_entry_imports`** (`ir/build.rs`) : **dataflow MUST** sur le CFG donnant la carte
  `registre → import` **prouvée** en entrée de bloc — **meet = intersection** (un mapping ne survit que si **tous** les chemins
  s'accordent ⇒ exclut le piège « PeekMessageA nommé GetModuleHandleA »), transfert = le scan intra-bloc existant (tue sur
  écriture, sur les clobbers ecx/edx émis à chaque appel, sur `Asm`), **init optimiste** (survit au back-edge d'une boucle),
  **racine ancrée par adresse** (robuste au bloc d'entrée en-tête de boucle), nommage **après** convergence, repli = ancien
  comportement. **Pas de double-pop** : la table runtime reste inchangée (0 sur un slot d'import), le pop statique in-block
  fournit `@N` une seule fois ⇒ **zéro impact sur le lifting-DLL multi-modules**. ⚠️ Une tentative antérieure *via la table
  runtime* a été **revertée** (elle double-poppait ⇒ cassait `comctl32_imagelist`) — **toujours passer winediff** pour un
  changement d'ABI/import. Cf. 71 (2026-07-26 [ABI][LIFT] ✅).
- **⭐ AUDIT `stdcall_pops` (2026-07-26) — 37 `@N` manquants trouvés d'un coup** : croiser les **shims implémentés**
  (`aret_<Nom>`, 1036), la **vérité terrain** (`nm` sur les import-libs mingw : 6100 symboles décorés `@N`) et le contenu
  de la table. **790** shims sont des `__stdcall` prouvés, **35 n'avaient pas leur `@N`** = **35 dérives esp silencieuses
  latentes** (`SysAllocString@4`/`SysFreeString@4` = tout COM/BSTR, `GetVersionExW@4`, `WriteConsoleA/W@20`, `TlsFree@4`,
  `LockFile@20`…) ; +2 pour des imports appelés mais non implémentés (le pop est une propriété du **site d'appel**).
  Invisible à difftest/cpudiff/funcdiff — **seul un vrai binaire la révèle**. **À faire tourner en test permanent** (I8).
- **Imports par ORDINAL résolus** (`src/ir/ordinal_imports.rs`, 2026-07-17) : un import sans nom (`0x80000000|ord`
  dans l'IAT) était **skippé** par le loader → l'appel indirect abortait sur la valeur opaque. Désormais `(dll, ordinal)`
  → nom d'export via une table **vérité-terrain** ; le routage par-nom (shim) prend le relais. `COMCTL32` extrait
  **verbatim** de l'export table du comctl32.dll **que Wine exécute** (notre oracle → mapping correct *par construction* ;
  ordinaux ABI-stables). Ex. `COMCTL32 #17 = InitCommonControls` (le vrai mur de `itiem95.exe`, CD 1997 → avance à
  `DialogBoxIndirectParamA`). Inconnu ⇒ non résolu (abort sound). Gardé `winecorpus/comctl32_ordinal.{c,def}`
  (import forcé par ordinal via dlltool, bit-identique Wine).
- **Callee-pop PROPAGÉ À TRAVERS UN TAIL CALL (2026-07-26)** : `compute_callee_pops` ne lisait que les `ret N` du **corps**,
  donc une fonction dont le retour est `jmp <autre fonction>` (thunk MSVC : ajustement `this`, forwarder, wrapper) sortait
  à **pop 0** — alors que l'appelant doit honorer le pop de la **cible** (c'est son `ret N` qui rend la main). Tout appelant
  d'un tel thunk laissait donc esp N octets bas **en silence**. Fix = arêtes de tail call (dernier `jmp` sortant vers
  l'entrée d'une fonction **récupérée**) + propagation en **point fixe** (les thunks s'enchaînent), cible inconnue ⇒ pas
  d'arête. Débloqua le mur **/GS de WinMerge** (`0x6d96d0 → jmp 0x6bad9d` : 0 au lieu de 4 ⇒ dérive esp de 4 ⇒ échec du
  cookie). Hash **inchangé**, **15 fixtures lifting-DLL vertes**. Cf. 71 (2026-07-26 [ABI][LIFT] ✅).
- **tail-`jmp [import]`/`jmp reg`** reçoit **esp+4** (l'adresse de retour est encore
  sur la pile — TLS/Fls/encoded-ptr, l_alloc→realloc).
- **stdcall pop sur `call reg`** (import chargé en registre puis appelé).
- **Helpers ABI MSVC à réécriture de frame** : `_EH_prolog` **inliné** au site
  d'appel ; `_chkstk`/`_alloca` modélisés `esp -= eax` (détectés par `xchg esp,eax`).
  **Élargi (2026-07-26) à la famille `_EH_prolog3(_catch)_GS`** (CRT statique MSVC C++ /GS) : elles relocalisent esp via un **temp**
  (`lea eax,[esp+K]; mov ebp,eax`, K>0), pas `lea ebp,[esp+K]` direct — `frame_setup_helper_body` suit le temp → `mov ebp,R`. Non
  reconnue, la relocalisation esp ne se propageait pas et les frames en aval **écrasaient le nœud SEH** posé par le helper
  (handler `node+4` corrompu) → `fs:[0]` obsolète → un throw C++ **réel** (MFC) tombait sur un handler garbage → abort. **Driver = WinMerge2.14.0/MFC90**
  (franchit désormais l'init statique MFC). Inliner un helper branch-free/`ret` = sémantique préservée ; gaté (nul effet hors `_EH_prolog3`).
- **`___chkstk_ms` (GCC/mingw) = registres préservés** (`is_chkstk_probe_fn` → mask de clobber vide dans
  `compute_call_clobbers`) : sonde de guard-pages pure (save/restore ecx+eax, esp inchangé). Le write-scan
  marquait sinon ecx clobbé (push/pop), perdant la longueur d'un `memset` posée en ecx (idiome alloca
  `mov ecx,len;call ___chkstk_ms;sub esp,eax;rep stos`) → débloqué busybox `sed -n`. ⚠️ **pas** un no-op
  (supprimerait les écritures pile transitoires du call → régression cpudiff).
- **`push [esp+d]` / `push esp` — source pré-décrément** : le lift capture la source qui lit esp (mémoire
  esp-relative **ou** le registre esp lui-même) dans un temp **avant** `esp-=4` (sinon `Read(esp)` se résout
  post-décrément → source décalée de 4 ; `push esp` = sémantique 286+, pousse l'esp *avant* baisse). `push [esp+d]`
  a débloqué plink (forward d'arg pile `sub_431310` → segfault config résolu) ; `push esp` trouvé par la
  **couche cpudiff-séquences** (2026-07-10), pas par le per-instruction (qui ne comparait la page que sur
  opérande mémoire explicite — élargi à `stack_pointer_increment()!=0` pour voir les écritures pile implicites).
- **self tail-call** (`jmp func.entry`) = tail-call frais (pas une boucle) → passe
  correctement les registres-args mis à jour (whereSplit sqlite).
- **`jcc <autre fonction>` = TAIL CALL CONDITIONNEL** (2026-07-26) : un saut conditionnel dont l'arête **prise** sort vers une
  adresse exécutable (une autre fonction récupérée) pendant que la **chute** reste interne — idiome MSVC pour partager une queue
  commune. La cible n'étant pas un bloc de la fonction, tout le `jcc` dégradait en `Asm`/**abort**. Désormais : branchement vers un
  **bloc synthétique** portant exactement le `Return(tail_call(...))` du cas `jmp` sortant. **Additif par construction** — ce bras ne
  reprend que des cas qui abortaient, donc aucun programme fonctionnel ne change (**hash inchangé**). Débloqua **WinMerge/mfc90u**
  (`sub_867400` : `je sub_867436`), 1ᵉʳ mur non-import du driver. Cf. 71 (2026-07-26 [LIFT]).
- **auto-main** : si l'entrée PE est un sas CRT et qu'un `main`/`_main` distinct
  existe, on démarre au main (frame cdecl synthétique argc/argv) ; `--entry`
  force l'entrée d'origine ; `_initterm` **dispatche réellement** la table d'init
  (argv/ctors construits).

### 4.4 Récupération de fonctions (strippés)
- Prologue scan + **address-taken** : scan de données (pointeurs de code alignés),
  **immédiats** (`push imm`/`mov [esp],imm` = callback par valeur), `mov reg,imm;…;
  call *reg` (`reg_imm_reaches_indirect_call`), `mov [g],imm;…;call [g]`
  (`abs_store_imm`), **`mov [reg+d],imm`** (`mem_store_code_imm` = pointeur de méthode dans un objet,
  accepte le stub `ret` nu via `is_bare_ret_stub` — NASM OMF `struct ofmt`), **`call/jmp [disp32]`**
  (`abs_indirect_slot`, contenu du slot), **`call [idx*4+base]`** (`indexed_call_table_base`, tables
  init/atexit NASM).
- **Tables de saut** : bornées par `cmp idx,N;ja` ; doublons préservés (cases
  partagés) ; abs computed-goto ; forme -O0 étagée ; tables de pointeurs
  **NULL-tolérantes** ; **run ≥3× d'une même valeur = switch, pas vtable**.
- **Re-split** : une fonction absorbée après un appel *noreturn* (pas d'analyse
  noreturn au balayage) est **forcée** frontière quand une preuve la pointe : table de
  pointeurs/index, **callback par valeur** (`stack_arg_code_imm`, atexit/qsort) ou
  **slot `call/jmp [slot]`** (`abs_indirect_slot`) — gardé par `looks_like_func_start`.
  `compute_noreturn` = point-fixe **sound** (jamais deviné noreturn).
- **x87 leaf-thunk** (`is_x87_leaf_thunk`) : décode tout le corps (fld arg → ops FPU
  → ret) → amorce atan2/fmod/trunc atteints par pointeur isolé.
- **Prologue de réalignement de pile GCC sans frame-pointer** (`lea ecx,[esp+4]; and esp,imm` = `8d 4c 24 04 83
  e4`) reconnu par `looks_like_func_start` (`known_prologue_bytes`, testé) : toute une classe de fonctions mingw
  (alignement 16 o, ebp omis) atteintes via une **table `{nom, func}`** (winetest/busybox/interpréteurs) était non
  récupérée → `call [table]` indirect abortait. Débloque le dispatch de test de `kernel32_test.exe`.
- **Pointeur de fonction FPO isolé précédé d'un terminateur** (`preceded_by_terminator`, 2026-07-17) : un pointeur-code
  **isolé** (hors table ≥3) initialisé statiquement en `.data`, dont la cible est une fonction **FPO** (ouvre sur `push
  imm`/`cmp [m],imm`, pas de prologue reconnu), échouait `looks_like_func_start` → l'appel indirect abortait. Fix
  **sound par frontière** : la cible est un vrai début de fonction dès qu'une frontière **prouvée** la précède — (A) une
  insn **déjà décodée** (dans `global`) finit exactement là et est un terminateur (`ret`/`ret N`/`jmp`), ou (B) l'octet
  avant = **`int3`(0xCC)** padding **ou `ret`(0xC3)** — 1 octet, donc `addr` est une vraie frontière même quand la
  fonction *précédente* n'est pas récupérée (son `ret` absent de `global`, cas `slidelib 0x404926`). ⚠️ **Pas de
  décodage frais** à `addr-k` (x86 non auto-synchronisant : un octet `ret`/imm intérieur donnerait une fausse insn courte
  finissant à `addr` — cassait `0x405f22`). Ne peut pas tronquer (rien ne franchit un terminateur). Mur mesuré dominant
  sur le corpus MSVC 1997 (avance slidelib/DEMO32/itiem95) ; **+89 fonctions récupérées dans busybox/sqlite** (funcdiff
  20501→20590, 0 div ; l'extension `0xC3` : slidelib 103→111 fn, régression complète verte).
- **FLIRT** : opérandes **relocalisés wildcardés** (`.reloc`) ; **thunks jamais
  signaturés** (résolus structurellement) ; glue reconnue = `looks_like_func_start`.
  ⚠️ FLIRT est **cosmétique** pour nos cibles (reconnaît du code de biblio, pas le
  code propre ; sensible à la version) — **le levier réel est le lifter**.
- **Host-back d'intrinsèques non-liftables (`runtime/flirt/msvc_crt.sig`)** : certaines routines CRT hand-assemblées
  ne **peuvent pas** se lifter — l'intrinsèque **`memmove` MSVC** entrelace ses tables de saut d'alignement *dans* le
  code (une entrée de table lit dans les octets d'une instruction voisine), donc les `jmp [idx*4+table]` restent
  unmodelled (abort sound). Réponse doctrine-pure (comme libm §4.2) : **reconnaître** la fonction par signature FLIRT
  → `crt_symbol=memmove` → brancher sur `aret_memmove`, corps non émis. **Reconnaissance PROUVÉE, pas devinée** : la
  fonction réelle est exécutée sous **Unicorn vs `memmove` libc** (500/500 cas aléatoires, recouvrements avant/arrière
  bit-identiques) avant d'ajouter la signature (byte-exacte ⇒ zéro faux positif ; une autre version reste en abort).
  Débloque `putty.exe` (archive.org, vieux MSVC) ; niche (le MSVC moderne — sqlite/nasm — n'a pas cet intrinsèque).
  Gardé `flirt::bundled_recognises_msvc_memmove`. *(Reste : les case-bodies morts promus par le scan address-taken
  depuis les tables entrelacées — 0 appel, code mort sound.)*

### 4.5 Axe 2 — HLE (couverture OS/CRT/Win32, vérifié vs Wine)
- **stdio** : **tous les FILE** = struct **msvcrt-layout (32 o) fd-backed non
  bufferisés** → getc/putc **inlinés** défèrent à `_filbuf`/`_flsbuf` (read/write
  1 octet). `_iob` et fichiers unifiés. ungetc pushback. `close(std)` fidèle
  (idiome close+réouverture uniq/tac), `isatty` sans fuite ENOTTY.
- **Fichier** : open/read/write/close/lseek/**_lseeki64**/_telli64, famille **stat
  msvcrt ABI-exacte** (`_stat`/`_fstat`/`_stati64`, offsets d'octets explicites),
  `GetFileSize(Ex)`/`GetFileTime`/**`GetFileInformationByHandle`** (fstat → attrs +
  3 FILETIMEs + serial + taille 64-bit + nlink + file-index=inode),
  **Win16 file API** (`_lopen`/`_lcreat`/`_lclose`/`_lread`/`_lwrite`/`_llseek`/`_hread`/`_hwrite` = fd POSIX),
  `_access`/`_chmod`/`_mkdir`/`_rmdir`/`_unlink`, mapping mémoire (`CreateFileMapping`/
  `MapViewOfFile` → mmap, `#ifndef __wasm__`), **wide** (`_wfopen`/`CreateFileW`/
  `GetFileAttributesExW`/`GetFullPathNameW`…). **Chemins Unix absolus `/…`
  passent au vrai FS** ; seuls les chemins Windows gardent le préfixe.
  **Itération de répertoire CRT** (`_findfirst`/`_findnext`/`_findclose`, msvcrt — famille
  #1 par largeur mesurée : **10 binaires** du gauntlet) sur `struct _finddata_t` (280 o,
  offsets mesurés) : même machinerie opendir/fnmatch que `FindFirstFileA` mais layout **et**
  encodage `attrib` **distincts, MESURÉS vs Wine** — fichier=`_A_ARCH(0x20)`, répertoire=
  `_A_SUBDIR(0x10)` (taille 0), read-only ajoute `_A_RDONLY(0x01)` ; `.`/`..` énumérés ;
  no-match → handle `-1` + `errno=ENOENT`. Gardé `winecorpus/crt_findfirst.c`.
- **CRT** : printf/scanf complets + **`%I64`/`%I32`** MSVC, `snprintf` C99,
  strtoll/strtoull/div/ldiv (retour **edx:eax** via `import_returns_u64`), `atexit`
  (via `_onexit`), setjmp/longjmp, `_getcwd`/`_chdir`/`_fullpath`, rand LCG msvcrt,
  gmtime/localtime/mktime/strftime (struct tm Windows). **`_assert`/`_wassert`**
  (11 binaires du gauntlet — **gain de soundness** : le stub faible renvoyait 0, le
  programme **continuait après une assertion violée** ; désormais message stderr au
  format Wine exact + `abort`). Gardé `winecorpus/crt_assert.c`.
- **CRT wide-string** (`<wchar.h>`, code-units **16-bit** Windows, **piloté par la donnée** — famille dominante
  d'imports manquants sur le corpus WineHQ) : `wcslen/cpy/cat/cmp/ncmp/ncpy/chr/rchr/str/dup`, `_wcsicmp/_wcsnicmp`
  (**fold ASCII = exact en locale C**, ordinal comme msvcrt — mesuré ≠ collation linguistique), `towlower/towupper`,
  + kernel32 `lstrlenW/lstrcpyW/lstrcatW` (ordinaux). Le besoin de compare **ordinal** est couvert exact par
  `wcscmp`/`_wcsicmp`. Gardé `winecorpus/crt_widestr.c` (bit-identique Wine, ASCII). *(Piège : `sanitize_import`
  retire l'underscore de tête → shim `aret_wcsicmp`, pas `aret__wcsicmp`.)*
- **Collation linguistique** (`lstrcmpW`/`lstrcmpiW`/`CompareStringW`/`CompareStringA`, kernel32) — **le levier
  dur, fait proprement** : Windows compare **linguistiquement** (word-sort : minuscule < majuscule, chiffres <
  lettres, `~` < lettres…), pas ordinalement. Le résultat = `sign(memcmp(sortkey(a), sortkey(b)))`. On **reproduit
  la sort-key BIT-À-BIT** (poids par caractère **MESURÉS** de `LCMAP_SORTKEY` de Wine, pas devinés) pour un
  **sous-ensemble ASCII prouvé** : `PRI(2 o/car) 01 01 CASE(0x12 maj/0x02 sinon, 0x02 de queue élagués ; retiré si
  insensible à la casse) 01 01 SPECIAL 00`, où **SPECIAL** gère les **ignorables au niveau primaire** `-`/`'`
  (contribuent `ff (0xff-nAvant) <poids> 12` — mesuré : "read-me" trié après "readme", "O'Brien"…). **Hors du
  sous-ensemble** (contrôles, non-ASCII) → **abort sound** (jamais deviné). **Chemin rapide égalité** : deux chaînes
  binairement identiques sont linguistiquement égales pour **tout** contenu (une comparaison d'égalité n'aborte
  jamais). Gardé `winecorpus/win_collate.c` (**2500 paires**, hash `63c659d1` = Wine ; `Hello/hello=1`, `~/a=-1`,
  `readme/read-me=-1` — que l'ordinal ratait).
- **Formatage wide** : **`%ls`/`%S`/`%lc`/`%C`** dans le formateur narrow (`aret_vformat` : lit une chaîne/car
  **16-bit**, largeur/précision correctes) ; **formateur wide `aret_wvformat`** (sortie 16-bit, réutilise la logique
  numérique éprouvée puis élargit) branché sur **`wsprintfW`** (user32) / **`_snwprintf`** (sém. troncature MS :
  `-1` si tronqué, pas de NUL sur remplissage exact) / **`_vsnwprintf`** (va_list = pointeur d'args). **`swprintf`
  NON modélisé** (signature ambiguë selon le CRT : `(buf,fmt,…)` legacy vs `(buf,count,fmt,…)` C99 que Wine utilise
  — deviner mis-parse les args, Wine lui-même faute) → **abort sound**. Gardé `winecorpus/crt_wideprintf.c`
  (bit-identique Wine).
- **Win32** : console/TTY (GetConsoleMode/SetConsoleMode/GetFileType), Tls, locale/
  codepage (GetACP/GetStringTypeW/LCMapStringW/MultiByte↔Wide), heap/module,
  process/thread (`CreatePipe`=pipe() fidèle ; **`CreateThread` = fibers coopératifs réels**, cf.
  §4.7 ; `CreateProcess` = **échec sound**, pas simulé), Find\*File (opendir+fnmatch), env, temps figé,
  Interlocked, **version-info** (VS_VERSIONINFO parsé), **BSTR/COM minimal**
  (SysAllocString, CoInitialize/**OleInitialize** partagent la profondeur COM, CoTaskMemAlloc), **DDE param**
  (`Pack/Unpack/FreeDDElParam` : alloc pour ADVISE/ACK/DATA/POKE, MAKELONG sinon), temp-fichiers, SetEndOfFile/
  SetFileTime, PeekNamedPipe (FIONREAD), GetThreadLocale (en-US 0x0409), TEB/PEB
  (ProcessParameters), VirtualQuery, LockFile.
- **Registre en mémoire** (advapi32, 2026-07-19, la tête mesurée — `RegCreateKey` 17/29 binaires) : arbre clés+valeurs
  **borné**, démarre **VIDE** → un réglage écrit ce run se **relit exact** (round-trip), une valeur jamais écrite reste
  `ERROR_FILE_NOT_FOUND` (jamais devinée). `RegCreateKey(Ex)`/`RegOpenKey(Ex)`/`RegSetValueEx`/`RegQueryValueEx`
  (size-query, MORE_DATA+taille)/`RegEnumValue`/`RegEnumKey(Ex)`/`RegDeleteKey`(récursif)/`RegDeleteValue`/`RegQueryInfoKey`/
  `RegCloseKey`, A complet + W (conversion de nom). Disposition CREATED(1)/OPENED(2). Gardé `winecorpus/win32_registry.c`
  (round-trip bit-identique Wine).
- **TEB `StackBase`/`StackLimit` = vraies bornes de la pile machine** (`fs:[4]`/`fs:[8]`, 2026-07-17, bug **général**) :
  l'entrée émise (`aret_main.c`) publie `__aret_set_stack_bounds(top=aret_stack+taille, bottom=aret_stack)` avant de
  lancer le programme → un sas CRT MSVC qui lit `fs:[4]` (StackBase) et déréférence `[StackBase-8]` (idiome
  stack-cookie / bornes) tombe dans `aret_stack` (valide), plus sur un placeholder bidon (`0x7FFF0000`→faute). Trouvé
  par sweep Win95 (gifcon32, faute captée par le dispatch fautes matérielles). Placeholder gardé en repli (pas d'entrée,
  ex. test unitaire). Hash inchangé, régression verte.
- **`_controlfp`/`_controlfp_s`** (CRT, 2026-07-17) : mot de contrôle FP msvcrt **stateful** (défaut `0x0008001f` mesuré
  Wine ; set = `(cur & ~mask)|(new & mask)`, query = courant) — bit-identique Wine. Débloque v/mpegplayer (sweep Win95).
- **kernel32 divers (forte largeur corpus Win95)** : `GetVersion` (forme packée, cohérente avec
  GetVersionEx 6.2.9200 NT), `DosDateTimeToFileTime` (FAT→FILETIME, jours civils portables),
  `RtlMoveMemory`=memmove. Gardés par `winecorpus/win32_version_dostime.c`.
  `GetExitCodeProcess` (STILL_ACTIVE), `GetDiskFreeSpaceA` (statvfs), `SetFileAttributesA/W`
  (READONLY↔chmod). Gardés par `winecorpus/win32_file_process.c`. **`RtlUnwind` = local unwind SEH implémenté**
  (2026-07-17, brique EH 2) : parcourt `fs:[0]` jusqu'à (exclu) la TargetFrame, appelle chaque handler intermédiaire
  avec `EH_UNWINDING(0x2)`, pop chaque frame, **retourne normalement** (`fs:[0]`=target) — le modèle i386 fidèle
  (TargetIp ignoré ; le saut non-local est fait par l'appelant). Gardé `winecorpus/seh_unwind.c` (bit-identique Wine).
- **USER32 message-only** (sans pixels, portable/WASM) : `RegisterClassW`/`Unregister`,
  `CreateWindowExW`/`DestroyWindow`, `DefWindowProcW`, `Get`/`Peek`/`Dispatch`/`Translate`/
  `Post`/`SendMessageW`, `PostQuitMessage`, `SetTimer`/`KillTimer`, `MsgWaitForMultipleObjectsEx` **+ jumeaux A** (M7 G1, doc 72).
  Registre de classes + file de messages mono-thread + timers ; dispatch = **callback WNDPROC
  dans le lifté** (`aret_call`). Débloque le notifier Tcl. Cf. §5 P6.5.
- **MessageBoxA/W** (M7 G5a, doc 72, **display-free**) : renvoie **-1** (repli sound = comportement Wine
  **sans écran**, mesuré : pas de blocage, pas de bouton deviné). Un vrai dialogue (SDL) arrivera avec G2b.
  Gardé `winecorpus/user32_messagebox` (marqueur `.nodisplay` → oracle Wine-sans-écran).
- **Dialogs** (M7 G5b, doc 72, **display-free**) : `DialogBoxParamA/W`/`CreateDialogParamA/W` **et
  `DialogBoxIndirectParamA/W`/`CreateDialogIndirectParamA/W`** (2026-07-17 — template **en mémoire** = pointeur direct,
  pas une ressource ; core modal/modeless factorisé, débloque `itiem95`) parsent le **DLGTEMPLATE(EX)** → contrôles
  enfants (réutilise la table de fenêtres, champ `ctrl_id`) + **pompe modale** (WM_INITDIALOG → DLGPROC via `aret_call`) ;
  `EndDialog`, `GetDlgItem`/`GetDlgCtrlID`, `Set`/`GetDlgItemText A/W`, `Set`/`GetDlgItemInt`, `SendDlgItemMessageA/W`.
  DLGPROC qui n'`EndDialog` pas headless → **abort sound** (limite honnête : un dialogue modal attend une entrée
  utilisateur absente headless). Gardé `winecorpus/user32_dialog.{c,rc}` + `user32_dlgindirect.c` (bit-identique Wine).
  **Unités de dialogue → pixels** (2026-07-19, fondation « dialogue visible ») : `MapDialogRect` + **base-units par-dialogue
  calculées façon Wine** (`GdiGetCharDimensions` réimplémenté en autonome : `du_x=(extent(alphabet 52)/26+1)/2`, `du_y=tmHeight`,
  police du template `lfHeight=-MulDiv(pt,96,72)`), sur nos métriques FreeType existantes → **bit-exact vs Wine** (son propre
  trace imprime `units = 7,13` = les nôtres ; même caveat unique que gdi_uifont). Sans FreeType ⇒ abort sound. Gardé
  `winecorpus/user32_dlgunits.c` (DejaVu Sans, `base 7 13`/`rect 4 5 280 163`). **Géométrie + classes des contrôles**
  (2026-07-19) : `u32_dialog_create` extrait désormais `x/y/cx/cy` (→ pixels via base-units) + la **classe** de chaque contrôle
  (atome prédéfini ou nom) + la taille client du dialogue ; base-units en **best-effort** (`g_dc_font_quiet` : police non
  résolue ⇒ pas d'abort). Vérifié bit-exact **relatif au client** (`GetWindowRect`+`MapWindowPoints(NULL,hDlg)` annule le
  placement WM), `winecorpus/user32_dlgcontrols.c` (Button `18,33,88,23` / Edit `18,65,105,20`, client `350×195`).
  **✅ DIALOGUE VISIBLE (2026-07-19)** : `u32_dialog_composite` remplit le fond (COLOR_3DFACE) + peint chaque enfant à son offset
  (BUTTON via `u32_button_paint` blitté), font du dialogue appliquée aux contrôles (captions FreeType) → **un dialogue à
  contrôles natifs s'affiche en ELF autonome (SDL)**. Fenêtre créée à la bonne taille (base-units avant `u32_window_create`) ;
  créateurs de dialogue ajoutés au gate SDL. **Vérif qualitative écran virtuel** (Wine ne compose pas dans un DIB —
  `WM_PRINT PRF_CHILDREN` ne peint pas les enfants — donc pas de DIB-hash ; layout identique Xvfb, briques bit-exactes ⇒ correct
  par composition). Garde non-crash `winecorpus/user32_dlgpaint.c`. **✅ STATIC + EDIT ajoutés (2026-07-19)** : `u32_static_paint`
  (fond COLOR_3DFACE + texte gauche/haut COLOR_WINDOWTEXT) et `u32_edit_paint` (client COLOR_WINDOW + texte ; bordure creusée
  `EDGE_SUNKEN` non-cliente ajoutée par le composite) — **bit-exact WM_PRINTCLIENT** (`user32_static_paint.c`/`user32_edit_paint.c`,
  structurel index-based). `u32_control_proc` dispatch par classe (button/static/edit) ; composite via `u32_control_paint_full`.
  Rendu du formulaire (labels + champs + bouton) identique à Wine (capture Xvfb). **✅ CHECKBOX (2026-07-19)** : glyphe 13×13
  `DrawFrameControl(DFCS_BUTTONCHECK)` = `DrawEdge(SUNKEN)` + intérieur COLOR_WINDOW + coche **Marlett** (21px mesurés) —
  **bit-exact** (`gdi_framecontrol_check.c`) ; `u32_check_paint` (glyphe + label), re-composite sur `UpdateWindow` (état coché
  reflété). ⚠️ Wine ne peint pas checkbox/radio via WM_PRINTCLIENT → contrôle entier vérifié qualitativement, primitive bit-exact.
  **✅ GROUP BOX (2026-07-19)** : `DrawEdge EDGE_ETCHED` (indices mesurés, bit-exact `gdi_drawedge_etched.c`) + `u32_group_paint`
  (cadre gravé + label opaque coupant la bordure). **⇒ Contrôles statiques courants COMPLETS** (bouton/label/champ/case/group box).
  **✅ RADIO (2026-07-19)** : glyphe 13×13 fixe mesuré (bitmap bit-exact `gdi_framecontrol_radio.c`), `u32_radio_paint`. **Reste** : combo/list (complexe), repaint général sur invalidation, **interaction** (clic →
  hit-test → WM_COMMAND, focus, saisie clavier) = prochain grand chantier.
- **GDI de base** (M7 G6, doc 72) : table d'objets GDI (DC/bitmap/brush/pen/font, handles opaques) + **dessin
  DIB mémoire bit-exact** — `CreateDIBSection`(32bpp)/`CreateCompatibleDC`/`SelectObject`/`DeleteObject`,
  `SetPixel`/`GetPixel`/`FillRect`/`PatBlt`/`BitBlt`(SRCCOPY), `CreateSolidBrush`/`Pen`, `GetStockObject`/
  `GetSysColor`/`GetDeviceCaps` (métriques par invariant), `GetDC`/`ReleaseDC`/`BeginPaint`/`EndPaint`. Cible
  vérifiée = un **DIB qu'on possède** (COLORREF↔`[B,G,R,0]`) → oracle = **hash du framebuffer** vs Wine. Hors
  périmètre (abort sound) : <32bpp. Gardé `winecorpus/gdi_dib.c`.
- **Famille PALETTE (2026-07-25, `GDIT_PALETTE`, sound truecolor, bit-exact vs Wine)** — piloté par la donnée (wall sweep Win95,
  5-7 binaires abortaient à `CreatePalette`). Sur 32bpp une palette ne remappe rien, mais le modèle d'objet + les requêtes
  matchent Windows : `CreatePalette`/`Get`/`SetPaletteEntries`/`GetNearestPaletteIndex`(euclidien)/`ResizePalette`/`RealizePalette`
  (→0)/`UnrealizeObject`/`SelectPalette`(rend DEFAULT_PALETTE non-null)/`Get`/`SetSystemPaletteUse`(→1 SYSPAL_STATIC)/
  `GetSystemPaletteEntries`(→0 mais **remplit les 20 couleurs statiques** 0-9/246-255, mesurées) + `GetObject(hpal)`=count +
  `GetStockObject(DEFAULT_PALETTE)`. `GetNearestColor`=identité. Gardé `winecorpus/gdi_palette.c`.
- **GDI vectoriel + raster** (M7 G6, doc 72, **bit-identique à Wine**, oracle DIB-hash) : `MoveToEx`/`LineTo`/
  `GetCurrentPositionEx` (**Bresenham** entier, point final exclu, position courante sur le DC), `Rectangle`
  (bord stylo `[l,r-1]×[t,b-1]` + remplissage pinceau `[l+1,r-1)×[t+1,b-1)`), `Polyline` (segments connectés,
  n'utilise pas la position courante), `PolylineTo` (suite de `LineTo` depuis la position courante, la met à
  jour), `FrameRect` (bord 1px du **pinceau argument** sur `[l,r-1]×[t,b-1]`), `InvertRect` (XOR 32 bits sur
  `[l,r)×[t,b)`), `BitBlt` ROP3 **binaires** (S,D : SRCCOPY/AND/PAINT/INVERT/NOTSRC/ERASE/MERGE/DSTINVERT/
  BLACK/WHITE). Stylo : **solide largeur ≤1** seulement (`PS_NULL`=rien ; styles/largeurs >1 = **abort sound**).
  Hors périmètre = **abort sound** : Ellipse/Polygon/RoundRect/Arc (midpoint-ellipse à centre demi-entier =
  match niveau-recherche), ROP à motif (pinceau). Gardés `winecorpus/gdi_{lineto,rectangle,polyline,framerect,
  bitblt_rop}.c`.
- **GDI texte via FreeType** (M7 G3, doc 72, **bit-identique à Wine, autonome**) : `TextOutA/W` rastérise avec
  **FreeType** — le rasterizer **que Wine utilise** — donc glyphes, ligne de base, positionnement, avances
  **identiques à Wine au pixel**, sans dépendance runtime Wine (FreeType lié dans l'ELF, statiquement liable →
  WASM ; **vraie police**, pas substitution). Recette minée de Wine : face résolue par **fontconfig** (comme Wine
  sous Linux ; `Arial`→Liberation Sans = mesuré identique), ligne de base `tmAscent=(FT_MulFix(usWinAscent,
  y_scale)+32)>>6` (Wine lit `OS/2.usWinAscent`, pas l'ascender hhea). `CreateFontA/W`+`CreateFontIndirectA/W`
  parsent le LOGFONT. **Sous-ensemble prouvé exact** ; le reste = **abort sound** (jamais faux silencieux) :
  antialiasing, gras/italique, alignements ≠ TA_TOP|TA_LEFT, stock font sans face, cible ≠ DIB 32bpp
  — chacun un incrément suivant vérifié vs Wine. Build : `builder` gate `-DARET_HAVE_FREETYPE` sur un import texte
  + pkg-config (sonames i386 liés explicitement), **dégradation propre** (byte-identique) sinon. Gardé
  `winecorpus/gdi_textout.c` (carte ASCII + hash FNV du DIB, bbox `3 6 92 19`, `hash=79741f6c`).
  **Texte tabulé** (2026-07-19) : `TabbedTextOutA/W`+`GetTabbedTextExtentA/W` — expansion `\t` **mesurée bit-exact vs
  Wine** (crayon → prochain tab-stop **strictement supérieur** ; >1 stops = absolu `org+lpTabPos[j]` ; ≤1 ou au-delà =
  multiples de `defWidth = lpTabPos[0]` ou **`8*tmAveCharWidth`** ; retour `MAKELONG(largeur, tmHeight)`), rendu via
  `u32_textout_core` → **pixel-identique**. Négatifs (right-align) / largeur ≤0 = abort sound. Gardé
  `winecorpus/gdi_tabbedtext.c` (`dibhash=ca64e7d7`, `arr=0013008c`).
- **USER32 menus** (M7 G7, doc 72, display-free) : modèle de données (items id/flags/submenu/texte) →
  `CreateMenu`/`CreatePopupMenu`/`AppendMenuA/W`/`InsertMenuA`/`Delete`/`Remove`, `EnableMenuItem`/`CheckMenuItem`
  (renvoient l'ancien état), `GetMenuState`/`GetMenuStringA/W`/`GetMenuItemCount`/`GetSubMenu`, `GetMenu`/`SetMenu`/
  `GetSystemMenu` (SC_* par fenêtre), `TrackPopupMenu`→0 sound. Gardé `user32_menu.c`.
  **Traîne menu** (2026-07-19, famille mesurée du plateau Win95) : `ModifyMenuA/W` (remplace l'item trouvé en place —
  nouveaux flags/id/texte, absent→FALSE), `SetMenuItemBitmaps` (stocke les 2 handles check-mark, TRUE/FALSE selon item),
  `GetMenuCheckMarkDimensions` = `MAKELONG(SM_CXMENUCHECK, SM_CYMENUCHECK)` = **13×13** (mesuré ; +GetSystemMetrics 71/72).
  Gardé `user32_menu2.c` (bit-identique Wine).
- **USER32 helpers fenêtre** (M7 G7, doc 72) : `GetClientRect`/`AdjustWindowRect(Ex)` (modèle no-NC),
  focus/activation (`Set`/`GetFocus`/`ActiveWindow`/`ForegroundWindow`/`BringWindowToTop`), `InvalidateRect`/
  `ValidateRect`/… (no-op sound), `MessageBeep`, `CallWindowProcA/W` (appelle un wndproc lifté), `LoadCursorA/W`/
  `LoadIconA/W` (handles opaques), `MsgWaitForMultipleObjects`. Mesuré bit-identique Wine (`user32_helpers.c`).
- **✅ FOCUS complet (2026-07-25, gestion + rendu, bit-identique Wine)** : **`SetFocus`** fire `WM_KILLFOCUS`(wParam=gagnant)
  puis `WM_SETFOCUS`(wParam=perdant), `g_u32_focus` à jour avant (GetFocus correct dans les handlers) — `user32_focusmsg` ;
  **`IsDialogMessageA/W`** (était un stub) navigue au clavier : Tab→`GetNextDlgTabItem`, flèches→`GetNextDlgGroupItem`, Entrée→
  bouton défaut/IDOK, Échap→IDCANCEL (`WM_COMMAND`), focus via `u32_set_focus` — `user32_isdlgmsg` ; **focus initial de
  dialogue** = premier tab-stop après `WM_INITDIALOG` **gated sur son retour** (TRUE→gestionnaire pose le focus, FALSE→le
  DLGPROC l'a fait) — `user32_dlgfocus` (`TRUE→100`, `FALSE→101`) ; **rendu** = un changement de focus **repeint** les
  contrôles affectés (`u32_set_focus` recompose leur dialog) → un **`CBS_DROPDOWNLIST` focalisé** montre sa sélection sur
  `COLOR_HIGHLIGHT`+`COLOR_HIGHLIGHTTEXT` (mesuré sur FishTank = Wine ; teinte = caveat classique-vs-uxtheme des composités).
- **`SystemParametersInfo(A/W)`** (2026-07-17) : actions GET **display-indépendantes** modélisées à valeur
  déterministe vérifiée vs Wine (`SPI_GETBEEP`=1/`GETBORDER`=1/`GETSCREENSAVEACTIVE`=1/`GETDRAGFULLWINDOWS`=0/
  `GETWHEELSCROLLLINES`=3), `SPI_GETWORKAREA` = invariant écran 1024×768 (comme GetSystemMetrics), et la paire
  **stateful** `SPI_{GET,SET}SCREENSAVEACTIVE` (SET stocke, GET relit — vérifié : SET(0);GET=0). SET non modélisés +
  GET inconnus ⇒ **abort sound** (jamais un pvParam non écrit relu = faux silencieux). Débloque `ARTLANT` (avance au
  rendu de texte GDI). Gardé `winecorpus/user32_spi.c` (valeurs display-indépendantes + round-trip stateful,
  bit-identique Wine ; le workarea env-dépendant testé hors oracle bit-exact).
  **+ `SPI_GETNONCLIENTMETRICS` (0x29, 2026-07-26)** — métriques non-client + les **5 polices shell** lues par tout framework au
  démarrage (mur réel de l'init GUI **MFC/WinMerge**). **A et W traités séparément** (`LOGFONTA` 60 o ≠ `LOGFONTW` 92 — le shim W
  renvoyait vers A, donc écrivait aux mauvais offsets) ; le layout vient du champ **`cbSize`** de l'appelant (pas `uiParam`), taille
  pré-Vista (340/500) acceptée en laissant `iPaddedBorderWidth` **intact**, taille inconnue ⇒ FALSE **sans écrire**. Valeurs **mesurées**
  (⚠️ **non dérivables** de `GetSystemMetrics` : Wine rend `SM_CYCAPTION` 26 vs `iCaptionHeight` 25). Gardé `winecorpus/user32_ncm.c` —
  **dump de tous les octets bruts sur tampon poisonné**, qui seul révèle que le chemin **A** n'écrit le nom de police que jusqu'au NUL
  (queue du tableau **laissée intacte**, dernier octet forcé à 0) là où **W** zéro-remplit. **+ `wcscat_s`** (CRT, 7 cas mesurés,
  `crt_wcscat_s.c`).
- **Famille SPI « UI EFFECTS » (`0x1000`-`0x1042`, 2026-07-26)** : les BOOLs par-effet que tout shell/framework interroge au
  démarrage (mur MFC/WinMerge = `SPI_GETMENUANIMATION 0x1002`). **Valeurs non uniformes** (`GRADIENTCAPTIONS`/`KEYBOARDCUES`/
  `FLATMENU`/`CLIENTAREAANIMATION` = 1, les 12 autres = 0) et **rejets non uniformes** (`0x102a`/`0x1082` → FALSE + err 1439 ;
  `0x0042` → FALSE **sans** toucher au last-error) — tout **mesuré**, rien de dérivé. Chaque action écrit **exactement un BOOL
  32 bits**, prouvé au tampon poisonné. ⚠️ Une 1ʳᵉ sonde sans `SetLastError(0)` avant chaque appel a fait **fuir** le 1439
  d'une action sur les suivantes : une sonde qui lit un état global doit le **remettre à zéro avant chaque appel**. Gardé
  `winecorpus/user32_spi_uieffects.c`.
- **`EnumFontFamilies(A/W)` (2026-07-26, 1ʳᵉ API à CALLBACK du chantier GUI)** : énumère les familles installées en rappelant un callback
  **lifté** (`aret_call`, frame stdcall comme `u32_call_wndproc` ; les structures `LOGFONT`+`TEXTMETRIC` sont posées **entre l'esp appelant
  et la frame du callback**, hors d'atteinte de la pile du callback). **Séparation clé** : le **contrat** est déterministe et **bit-identique
  Wine** (callback rendant 0 ⇒ arrêt immédiat **et retour 0** ; famille inexistante ⇒ 0 callback + retour 1 ; A ≡ W) ; la **liste** des
  familles et leurs métriques sont **environnementales** (399 ici, autant sous Wine) ⇒ **non bit-comparées**. Données **réelles** : liste
  depuis **fontconfig** (source de Wine), triée/dédupliquée ; métriques par les **formules déjà vérifiées** de `GetTextMetrics`
  (`u32_tm_from_face`, extraite de `u32_fill_textmetric`) ; famille non chargeable ⇒ **sautée**, jamais de métriques inventées. ⚠️ **Mesuré** :
  `lfPitchAndFamily` **≠** `tmPitchAndFamily` toujours (lf `0x22` vs tm `0x27` — même nibble FF_*, bits bas = pas demandé vs `TMPF_*`).
  `@N` (`@16`/Ex `@20`) ajoutés à `stdcall_pops` depuis l'import-lib mingw. Gardé `winecorpus/gdi_enumfonts.c` (invariants en **booléens**,
  jamais de compteur machine-dépendant).
- **Famille shlwapi `Path*` COMPLÈTE côté lexical (2026-08-01, ~52 shims en 4 vagues, bit-identiques Wine)** :
  racine (`IsUNC`/`IsRoot`/`IsRelative`/`SkipRoot`/`AddBackslash`/`RemoveBackslash`/`StripPath`/`RemoveFileSpec`),
  combinaison (`Canonicalize`/`Combine`/`Append`), extensions & composants (`AddExtension`/`RemoveExtension`/
  `RenameExtension`/`FindNextComponent`/`GetArgs`/`GetDriveNumber`/`IsFileSpec`/`IsUNCServerShare`/`StripToRoot`/
  `IsSameRoot`), comparaison (`CommonPrefix`/`IsPrefix`/`IsUNCServer` — **tranchées par l'oracle Windows**), et
  filesystem (`FileExists`/`IsDirectory`, dans `aret_hle.c` pour partager `translate_path`/`aret_attr_named`).
  ⚠️ Contre-intuitions **mesurées** à ne pas « corriger » : `/` **n'est pas** un séparateur pour cette famille alors
  qu'il l'est pour `PathFindFileName` ; un **espace** coupe la recherche d'extension (`"x.exe arg1 arg2"` n'a pas
  d'extension) ; `PathIsDirectory` rend **0x10**, pas 1 ; `"C:"` → `"C:\"` mais `"C:a\..\b"` → `"\b"` (lecteur
  **sans** séparateur = pas une racine) ; aucun cas spécial `\\?\`. Gardé par `win32_pathroot`/`pathcombine`/
  `pathparts`/`pathexists`. Détail 71 (2026-08-01).
- **`GetUserNameA/W`** (2026-08-01, advapi32) : nom depuis la **même source que Wine** (compte Unix), donc comparable
  et non « environnemental » ; `*pcb` = taille requise **NUL compris** en succès **et** en échec ; tampon trop court ⇒
  `ERROR_INSUFFICIENT_BUFFER` et tampon **intact** (aucun nom tronqué). Aucun nom disponible ⇒ **abort**, jamais un
  substitut. Gardé `win32_username.c`.
- **`GetClassInfo(Ex)A/W`** (2026-07-17) : le registre de classes stocke désormais **tous** les champs
  `WNDCLASS(EX)` (style, cbClsExtra/WndExtra, hInstance, hIcon, hCursor, hbrBackground, menu, hIconSm) à
  l'enregistrement → `GetClassInfo` les rend **verbatim** (round-trip register→query exact, atome non-nul en retour ;
  0 si non enregistrée ; `cbSize` **non écrit** = fourni par l'appelant, comme Wine). Débloque `DEMO32` (avance au mur
  suivant `CharToOemA`). Gardé `winecorpus/user32_getclassinfo.c` (bit-identique Wine).
- **Ressources PE `.rsrc`** (M7 G4, doc 72, **display-free**) : walker de l'arbre `IMAGE_RESOURCE_DIRECTORY`
  **en mémoire** (en-têtes PE déjà mappés à l'image base → `DataDirectory[2]` lu direct, 0 changement loader) →
  `FindResourceA`/`LoadResource`/`LockResource`/`SizeofResource`/`FreeResource` (id **ou** nom UTF-16) +
  **`LoadStringA`** (RT_STRING, 16/bloc). Ressource absente → NULL/0 (**sound**). Gardé `winecorpus/
  user32_resources.{c,rc}` (blob RCDATA + table multi-blocs + troncature, bit-identique Wine).
- **USER32 modèle fenêtre étendu** (M7 G2a, doc 72, **display-free**) : état window-manager (rect/style/
  visible/enabled/userdata/titre) → `GetWindowRect`/`SetWindowPos`/`MoveWindow`/`ShowWindow`/`UpdateWindow`/
  `EnableWindow`/`Get`-`SetWindowLongA/W` ; texte `Set`/`GetWindowTextA/W`+`GetWindowTextLengthA/W` **via
  WM_SETTEXT/GETTEXT** (le WNDPROC lifté les voit) ; `GetSystemMetrics`/`GetDesktopWindow`/`IsWindow(Visible/
  Enabled)`/`IsIconic`/`GetParent`. Valeurs écran env-dépendantes = **invariant** (écran virtuel 1024×768).
- **Fenêtre VISIBLE via SDL2** (M7 G2b, doc 72, **la 1ʳᵉ marche graphique**) : une fenêtre Win32 visible
  **s'affiche réellement**. Build (`builder/mod.rs`) : `-DARET_HAVE_SDL`+cflags SDL à la compile et `-lSDL2` au
  link **seulement** si le binaire importe `CreateWindowExA/W` **et** `pkg-config sdl2` (i386) répond — sinon
  compile/link **byte-identiques** (dégradation display-free : CLI/message-only/wasm/hôte sans SDL). Runtime
  (`aret_win32.c`, `#ifdef ARET_HAVE_SDL`) : chaque **fenêtre top-level visible** reçoit un **framebuffer client**
  (DIB 32bpp) que `GetDC(hwnd)`/`BeginPaint` **lient** (le GDI du programme y dessine, comme Wine) + une vraie
  **`SDL_Window`+Renderer+Texture** (`RGB888` = octets DIB `[B,G,R,0]`), présentée sur `UpdateWindow`/`EndPaint`/
  `ReleaseDC` ; **pompe `SDL_PollEvent`** → `WM_CLOSE`/`WM_MOUSE*`/`WM_KEY*`. **Modèle de peinture `WM_PAINT`**
  (display-free, sound) : région d'invalidation coalescée par fenêtre (`needs_paint`) — `InvalidateRect`/`Rgn`
  (NULL = toutes), `ValidateRect`/`Rgn`, une fenêtre visible fraîche l'active ; `WM_PAINT` **généré à la demande**
  (jamais mis en file, priorité basse après file postée + `WM_QUIT`) par `Get`/`PeekMessage`, **et** livré
  **synchrone** par `UpdateWindow` ; `BeginPaint` valide la région ; `DefWindowProc(WM_PAINT)`=peinture par défaut
  (valide → pas de boucle infinie), `DefWindowProc(WM_CLOSE)`=`WM_DESTROY`+destruction (bouton X SDL ferme). ⇒ le
  handler `WM_PAINT` du programme **s'exécute et dessine** = le contenu s'affiche vraiment. **Strictement additif** : seule
  l'entrée réelle devient un message (le bruit WM expose/focus **n'est pas** synthétisé) → séquence déterministe
  **inchangée** ; l'**entrée** (souris/clavier) est env-dépendante ⇒ hors oracle bit-exact (on compare le
  **contenu** : peintures, pixels — pas les events) ; pas d'écran ⇒ repli display-free sound, jamais d'abort.
  Oracles : `user32_sdlwindow.c` (client-rect + `GetDC`/`SetPixel`/`GetPixel` round-trip + cycle paint) et
  `user32_paint.c` (handler `WM_PAINT` dessine → `UpdateWindow`/`InvalidateRect`/`PeekMessage` livrent, comptes de
  peinture + pixel) **bit-identiques à Wine** headless ; **et** les fixtures GUI existantes **relient SDL** en
  winediff sans rien perturber (preuve d'additivité).
- **Fond de fenêtre `WM_ERASEBKGND` + double-buffering** (M7 G2b, complète l'affichage) : la classe porte son
  `hbrBackground` (RegisterClass A/W, offset +28) → à la peinture, `BeginPaint` envoie **`WM_ERASEBKGND`** et
  `DefWindowProc` **remplit le client avec le pinceau de classe** (région d'erase suivie par `needs_erase`, posée
  au show et par `InvalidateRect(bErase)`) → une vraie fenêtre montre **son fond voulu** (plus du noir). Le
  **double-buffering** (DIB offscreen dans un DC mémoire → `BitBlt` SRCCOPY vers le DC fenêtre, l'idiome de rendu
  dominant) compose **out-of-the-box** avec le framebuffer client. Oracles `user32_erasebg.c` (pixel non dessiné =
  couleur du pinceau de classe) + `user32_dbuffer.c` (offscreen→BitBlt→relecture) **bit-identiques à Wine**. Reste :
  widgets natifs (BUTTON/EDIT), texte étendu (antialiasing/gras/fond opaque/DrawText/substitution), WASM-GUI (Emscripten).
- **`RegisterClassExA/W`** (WNDCLASSEX — la forme utilisée par **quasi toutes** les applis GUI modernes) : parse
  les offsets décalés (+8 wndproc, +32 hbrBackground, +40 className), partage le registre de classes A/W. Sans lui,
  une appli moderne **abortait** à l'enregistrement de classe. Gardé `winecorpus/user32_classex.c` (RegisterClassEx
  + fond de classe + fenêtre + peinture, **bit-identique à Wine**).

### 4.6 Démonstrateurs prouvés (bit-identiques à Wine)
- **Lua 5.4.7** (mingw, 650 Ko) **symbolé ET strippé** → ELF natif : batterie
  **35/35** sous-systèmes (closures, métatables/POO, coroutines, patterns, pack/utf8,
  math flottant, table.sort, pcall, 64-bit, varargs, goto, GC stress). Oracle de
  référence axe 1.
- **strings.exe** (Sysinternals, **MSVC static-CRT C++**) → ELF natif, sortie
  **100 % bit-identique à Wine** (bannière version-info comprise), exit 0. ~16 fixes
  généraux (cpuid/EH_prolog/chkstk/tail-jmp/cast-libc/TEB…).
- **sqlite3.exe** (**MSVC 32-bit strippé**, 2958 fn) → moteur SQL complet
  bit-identique à Wine : CRUD, JOIN, GROUP BY, window functions, CTE, index, JSON,
  dates, triggers. Sweep **30/30** (:memory: + on-disk). Toute la math scalaire.
- **NASM 2.16.01** (**MSVC strippé**, 1,5 Mo) → `nasm -v`/`-f elf`/`-f win32`/`-f
  bin`/**`-f obj`** = objets **bit-identiques à Wine** (`-f obj` débloqué par `mem_store_code_imm`,
  cf. §4.4/§5 P3 ; gardé dans le gauntlet).
- **busybox-w32** (mingw strippé) : cksum/md5sum/sha1sum/echo/sort/wc/cat/head/tail/
  uniq/tac/od/nl/cut/rev/expr/**awk** (÷)/seq/basename/tr/pwd/**grep**/**sed** (dont `-n`)/… bit-identiques ;
  sweep **60/60**.
- **WASM** : PE Windows → WebAssembly, **7/7** fixtures (pile, globals, indirects,
  CRT, x87, Win32, SHA-256).
- **Corpus gauntlet** (`bench/gauntlet/`, 21 PE variés committés) : **19/21** au
  score (**21/21 fonctionnels**). MATCH : bzip2/grep/gzip/hello/lua/minigzip/nasm/
  sed + **sqlite3 ×4** + **m4 ×2** + strippés. Seul reste : **units ×2** (cherche
  `units.dat` — **environnemental**, pas un bug ; message d'erreur diffère de Wine).

### 4.7 Threads coopératifs (fibers) — incrément 1 (doc 80), vérifié vs Wine
- **`CreateThread` = coroutine réelle** (fiber `ucontext`/`swapcontext`) multiplexée sur l'unique thread hôte ;
  bascule **uniquement** aux points bloquants ⇒ zéro data-race, **ordonnancement round-robin déterministe** ⇒
  oracle reproductible bit-à-bit. Fiber 0 = thread principal ; **sans aucun `CreateThread`, le scheduler ne
  démarre jamais** ⇒ un programme mono-thread est **byte-identique** à avant.
- **Livré (incrément 1)** : table de fibers `g_fiber[64]` (pile hôte 4 Mo + pile machine émulée 1 Mo par thread,
  malloc), `CreateThread`(+`CREATE_SUSPENDED`/`ResumeThread`), `ExitThread`, `GetExitCodeThread` (STILL_ACTIVE
  tant que vivant), **`WaitForSingleObject`/`WaitForMultipleObjects` = vrai join** (bloque→scheduler ; timeout 0
  = poll→`WAIT_TIMEOUT` ; sinon traité comme INFINITE), `Sleep` = **point de yield** coopératif. **`last_error`
  par-fiber** : le scheduler le swappe (global `g_last_error`) à chaque bascule (prouvé isolé à travers un yield).
  Deadlock (tous les fibers bloqués) → **abort sound** ; `SuspendThread` d'un thread courant → **abort sound**.
- **Autonomie/universalité** : `ucontext` = libc natif, lié statiquement. **WASM n'a pas `ucontext`** ⇒
  `CreateThread` y est un **abort sound** (Asyncify plus tard), jamais une divergence silencieuse (règle doc 80 §3).
- **Incrément 2 — `CRITICAL_SECTION` réelle** : table keyée par le pointeur `&cs` (struct laissé opaque, comme
  Wine) portant **owner (fiber) + compteur de récursion**. `Enter` : libre ou déjà à moi → prends (rec++) ; sinon
  **bloque** (le fiber pose `wait_cs`, le scheduler le réveille quand la CS se libère) → **exclusion mutuelle vraie
  même si l'owner yield en la tenant**. `TryEnter` (non bloquant), `Leave` (rec--→ libère à 0, réservé à l'owner),
  `Initialize`(+SpinCount/Ex)/`Delete`. Récursif. Table pleine → abort sound. WASM : no-op correct (jamais de
  contention). `Sleep`/Wait dans la CS restent des points de yield sûrs.
- **Incrément 3 — Events** (`CreateEventA/W`/`SetEvent`/`ResetEvent`) : table d'événements (range handle
  `0x71……`) portant **manual-reset** (reste signalé jusqu'à `ResetEvent`) ou **auto-reset** (libère **exactement
  un** waiter puis se réarme, **consommé à l'attente**). `WaitForSingle/MultipleObjects` reconnaissent les events
  (et les threads) : bloquent tant que non signalé, **re-vérifient après chaque réveil** (auto-reset : plusieurs
  réveillés, un seul consomme, les autres se re-bloquent). Noms intra-process partagés (hash FNV,
  `ERROR_ALREADY_EXISTS`). Handles non-thread/non-event gardent l'immédiat legacy.
- **Incrément 4 — Mutex, Semaphore, TLS par-fiber, `_beginthread(ex)`** (clôt le chantier) :
  - **Mutex** (`CreateMutexA/W`/`OpenMutexA/W`/`ReleaseMutex`, range `0x72……`) : ownable **récursif**, waitable
    via `WaitForSingle/MultipleObjects` (acquisition = consommation : owner=moi, rec++) ; `ReleaseMutex` réservé
    à l'owner ; owner mort sans release = **abandonné → traité libre** (pas de faux deadlock). `bInitialOwner`.
  - **Semaphore** (`CreateSemaphoreA/W`/`ReleaseSemaphore`/`OpenSemaphoreA/W`, range `0x73……`) : compteur borné
    (signalé si `count>0`, wait décrémente, release ajoute `n` plafonné à `max`, écrit le compte précédent).
  - **TLS par-fiber** : `aret_tls[fiber][slot]` — allocation d'index process-globale (`aret_tls_used`), **valeurs
    par-fiber** (un thread frais démarre à NULL) via `aret_current_fiber()`.
  - **`_beginthreadex`/`_beginthread`** (msvcrt, **cdecl**) : factorisés sur `u32_spawn` = `CreateThread` (même
    layout d'args / même trampoline). Modèle d'acquisition **unifié** (`u32_handle_signaled_for`/`u32_handle_acquire`
    par-fiber) : event auto-reset, mutex, sémaphore, avec **re-vérification après chaque réveil** ⇒ un seul consomme.
- **Vérifié bit-identique Wine** : `thread_join.c` (`25800`), `thread_critsec.c` (`4000`), `thread_event.c`
  (`60`/`15`), `thread_mutex_sem.c` (**mutex** `mcounter=2000` RMW coupé sous le lock, **TLS par-fiber** `tls_ok=1`,
  **sémaphore** `ssum=21`, spawné par **`_beginthreadex`**). `stdcall_pops` : +CreateMutexA/W=12, OpenMutexA/W=12,
  ReleaseMutex=4, CreateSemaphoreA/W=16, OpenSemaphoreA/W=12, ReleaseSemaphore=12 (`_beginthread*` = cdecl, pas de
  pop). Portes : hash transpile inchangé, winediff **105/105**.
- **Incrément 5 — timeouts finis via horloge virtuelle déterministe** (débloqué par un vrai workload) : `WaitFor*
  (h, ms)` et `Sleep(ms)` **finis** enregistrent une échéance sur une **horloge virtuelle** ; quand aucun fiber
  n'est *signal-runnable*, le scheduler **avance l'horloge à la plus proche échéance** et réveille les timed-out
  (`WAIT_TIMEOUT`) — au lieu de dead-locker. Déterministe (l'horloge n'avance que par la logique du scheduler, pas
  le wall-clock ⇒ oracle reproductible). Deadlock réel (tout bloqué, **aucun** timeout en attente) = abort sound.
  Motivé par le mur mesuré : le pattern **`WaitForSingleObject(h, 50)` comme sonde de vivacité** (ultra-courant en
  vrai) dead-lockait avant. `thread_pool.c` (pool de threads réel : file + mutex + sémaphore + event + TLS + timeout
  fini, somme parallèle `2686700`) = **bit-identique Wine**.
- **Chantier fibers = complet** (doc 80 §2, incréments 1-5). Reste hors-scope (abort sound) : préemption d'un thread
  CPU-bound qui ne yield jamais (hang→abort), WAIT_ABANDONED distinct, `SuspendThread` d'un thread courant, WASM
  (Asyncify).
- **Contrôle de thread (traîne, 2026-07-19)** : `SetThreadPriority`/`GetThreadPriority` (round-trip d'un hint ; le
  scheduler round-robin déterministe ne réordonne pas), `OpenProcess` (own pid → handle, autre → err 87), et
  `TerminateThread` (fiber cible → FST_DONE + exit code, jamais réordonnancé, piles fuient comme Windows ; lock tenu →
  deadlock→abort). Bit-identique Wine (`win32_thread_tail.c`).
- **Test « vrai binaire » honnête (2026-07-16)** : `kernel32_test.exe` (conformance WineHQ, extrait de `winetest`,
  3 Mo). **Mur franchi** : la table de dispatch `{nom, func}` (fonction de test atteinte via `0x446680`, prologue
  de réalignement) est désormais récupérée (cf. §4.4) → le sous-test `thread` **exécute du vrai code de test réel**
  (**0 → 5+ lignes `thread.c:`**, correctes). Il aborte *sound* sur `CreateProcessA` (pas de process enfant), puis
  **segfault** plus loin dans la surface process/remote-thread/thread-pool/APC (très au-delà des primitives) — **pas**
  une régression. ⇒ les primitives threads sont prouvées (dont un pool réaliste), **du vrai code de conformance
  threads tourne maintenant**, mais faire passer ce binaire **entièrement** reste un gros chantier (process-création
  + API haut-niveau). C'est le mur **points-to/Phase-4** (§5 P3) qui reste le levier principal, pas les threads.

---

## 5. Ce qui RESTE — précis, ordonné par valeur × sûreté

> Signal stratégique (2026-07-04) : les **victoires générales faciles** (shims,
> stdcall_pops, récup simple) sont **quasi épuisées**. Ce qui reste = un ensemble
> **borné** de problèmes **profonds**, chacun ≈ une session dédiée de forensics.
> On passe de « largeur de shims » à « profondeur lifter ». Fini, mais plus lent.

### §5.0 — STRATÉGIE BÉTON « zéro-abort 32-bit » (objectif : couvrir tout vrai binaire 32-bit, sans y passer des années)
**But** : plus aucun abort sur le **vrai logiciel compilé** (pas en le silençant — en le couvrant parfaitement). Le
résidu qui abort restera l'**obfusqué/fait-main/VM-packé** (indécidable, §9) — que le vrai logiciel ne contient pas.
**Règle anti-années** : on ne code **jamais** un fix à l'intuition — **toujours en tête d'une liste MESURÉE**.
- **Levier 0 — MESURER (fondation).** `wallsweep.sh` (agrège `--mode walls`) sur un **grand corpus** → liste **finie,
  classée par #binaires bloqués** des causes d'abort. Transforme « couvrir tout » (infini ressenti) en **liste finie
  priorisée**. **Mesure 2026-07-18 (corpus Win95, 37 PE32)** : instructions non-liftées = **bruit** (`outs`/`into`/`daa`
  = I/O port privilégié + data-en-code → abort correct ; le lift est complet). Tête des imports : **UnhandledExceptionFilter
  31/37**, **CreateProcessA 27**, **FormatMessageA 11**, puis traîne **cohérente** GDI mapping-mode
  (`SetViewportOrgEx`/`SetWindowExtEx`/`Scale*`/`PtVisible`…), **DDE**, **imprimante**.
- **Levier 1 — Effondrer la traîne d'imports d'un coup : LIFTING DE DLL** (doc 80 §1.2, **la forme pure de la doctrine**,
  **autonome + WASM**). Passer `user32`/`gdi32`/`comctl32`/`msvcrt`/`VB40032` (ReactOS) à *notre* lifter → chaque API =
  code lifté **prouvé** (cpudiff/funcdiff), plus un shim main. **Un seul investissement** (loader multi-modules + Export
  Directory + router le fond `win32k` vers le HLE) couvre **toute** la traîne **et** les runtimes tiers (VB/MFC) « gratis ».
  C'est LE multiplicateur qui évite d'écrire 2000 shims. Décision licence (ReactOS=GPL). *Le shim-main reste la voie
  rapide pour la tête à fort levier ; le lifting DLL pour la traîne + les runtimes.*
  **🚧 ENGAGÉ (2026-07-18, par petits incréments) — couche LOADER FAITE, prouvée sur vraies DLL Wine** (`src/loader/mod.rs`,
  tout testé bit-exact vs objdump / vs les tables réelles de Wine comctl32/user32/gdi32/comdlg32) :
  **Inc.1** `parse_pe_exports` (Export Directory → `{ordinal, name, target=Address(base+RVA)|Forward}`, trous sautés,
  forwards verbatim) · **2.1** `parse_pe_imports_detailed` (imports gardant le DLL source) · **2.2** `resolve_module_imports`
  (imports app → VA d'export liftée) · **2.3a** `apply_base_relocations` (rebaser, sound sur site hors-section) · **2.3b**
  `merge_modules` (fusion app+DLL rebasés, exports→symboles-fonction, anti-chevauchement) · **2.3c** `load_with_modules`
  (assemble tout + **route** chaque slot IAT résolu → patch VA + retrait de `imports`).
  **✅ 1ʳᵉ MARCHE END-TO-END PROUVÉE (2026-07-18)** : flag CLI **`--with-dll nom=chemin`** → une app qui importe d'un DLL
  est liftée **avec** lui, ses appels d'import dispatchent vers le **code DLL lifté** (le dispatch indirect existant
  suffit — **0 changement emit**), **bit-identique à Wine** (`bench/winecorpus/dll_lifting`, winediff 128/128 ; contrôle :
  sans le flag, imports non résolus → la sortie `42` ne peut venir que du code lifté). Défaut inchangé (sans flag,
  `Program::load`). **Carte mesurée (2026-07-18, `--mode walls --with-dll` sur vraies DLL Wine)** : comctl32+gdi32+user32
  liftées ensemble = **7150 fonctions liftées**, le tail user-mode nommé **s'effondre**, il reste **356 imports** =
  **250 syscalls `NtGdi*`/`NtUser*` (LE MUR WIN32K, à router vers le HLE qui rend déjà DIB/BitBlt/paint)** + **106 shims
  kernel32/CRT ordinaires** (atoms/locale/version/IME/char-class, data-driven). **Config correcte : lifter comctl32
  SEULE** (nos user32/gdi32 HLE marchent déjà → pas de win32k) ; ses imports se lient aux shims HLE.
  **✅ INCRÉMENT 3 DÉMONTRÉ (2026-07-18) — de vrais contrôles comctl32 tournent bit-identiques à Wine** (liftés de la vraie
  comctl32.dll de Wine, sur le HLE gdi32) : **ImageList** (API stateless, `winecorpus/comctl32_imagelist`) **et surtout
  une progress bar STATEFUL complète** (`winecorpus/comctl32_progress` : `pos=50 lo=0 hi=100`, PBM_DELTAPOS = Wine). La
  **machinerie générale des contrôles est complète** : DllMain lifté exécuté au démarrage (enregistre les classes) →
  `CreateWindowEx` (WM_NCCREATE/WM_CREATE → état dans `cbWndExtra`) → messages dispatchés au **WNDPROC comctl32 lifté** →
  **résolveur delay-load runtime** (uxtheme→classic ; VA synthétique + `aret_delay_dispatch`/`aret_delay_pop`). Familles
  socle comblées au passage (atoms A/W, `GetDIBits`/`CreatePatternBrush`/`StretchDIBits`, `DisableThreadLibraryCalls`).
  **Reste** : les autres contrôles (trackbar/toolbar/listview…) = **même machinerie**, data-driven (shims gdi32/user32
  manquants selon le contrôle) ; + les 106 shims socle ; + (optionnel, pour lifter gdi32/user32 eux-mêmes) FLIRT
  chirurgical `NtGdi*`/`NtUser*`→HLE ; + ~17 unresolved-direct / `jl 0x100afcd4`×16 (récup mineure).
- **⭐ LEVIER 1 VALIDÉ SUR WinMerge (2026-07-26)** : lifter **shell32** (builtin Wine) efface d'un coup
  `SHGetSpecialFolderLocation`/`SHGetMalloc`/`SHGetPathFromIDListW`/`ShellExecuteW`/`DragQueryFileW`/… **sans un seul
  shim**. ⇒ **le « mécanisme de vtable COM » n'a pas lieu d'être** : une DLL liftée crée ses objets **et vtables** en
  mémoire liftée, l'appelant lifté dispatche par `aret_call`. ⚠️ **Ne pas arbitrer sur le compteur statique** : lifter
  shell32 fait *monter* les imports manquants (270→394) car la carte compte ce que la DLL **pourrait** appeler
  (DDE/services/MSI), pas ce qu'elle appelle — **seule l'exécution juge**. Prérequis débloqué au passage : le
  **résolveur delay-load voit désormais tout le HLE** (table générée de 1043 shims, cf. 71).
  **Stratégie qui en découle** : *lifter* les DLL **user-mode** (shell32, ole32, oleaut32, comdlg32, comctl32 ✅) qui
  reposent sur nos user32/gdi32 ; *écrire à la main* la surface **user32/gdi32** (MDI, accélérateurs, presse-papier,
  dessin GDI, metafiles, impression) qui, elle, bute sur **win32k** (doc 80 §1.2).
  **⚠️ PRÉ-REQUIS MESURABLE AVANT DE LIFTER (2026-08-01) — une builtin Wine peut n'être qu'un RELAIS.** Lifter **shlwapi**
  n'a **rien** débloqué : son export `PathAddBackslashW` est un `___wine_spec_imp_*` = `jmp *[IAT kernelbase]`, pas une
  implémentation ⇒ le mur **recule d'un module** au lieu de tomber. Test en une commande, avant de payer le lift :
  `objdump -t <dll> | grep -c __wine_spec_imp_` rapporté aux exports nommés. Mesuré : comctl32 **0**/126, ole32 **0**/301,
  comdlg32 **0**/28, oleaut32 3/418, shell32 4/362, kernelbase 2/1402 (= **la vraie couche d'implémentation**) — mais
  advapi32 **196/582**, **shlwapi 198/362**, version 12/16 = **relais**. Fin de chaîne mesurée : kernelbase n'importe **que
  ntdll** (131 `Nt*` syscalls + 212 `Rtl*` + 74 divers) ⇒ la chaîne user-mode est **finie**, elle bute sur 131 syscalls NT
  (jumeau du mur win32k). ⇒ Une famille **pure et déterministe** (`Path*`/`Str*`) se comble au **shim HLE** (I5), pas en
  liftant kernelbase. Détail 71 (2026-08-01 [LIFT-DLL][INFRA]).
- **Levier 2 — Finir les mécanismes bornés qui débloquent une CLASSE** : EH C++ (`__CxxFrameHandler`), bitmap-fonts,
  runtime VB. Chacun = une session, des milliers de binaires.
- **Levier 3 — Mop-up data-driven** du résidu, trié par le Levier 0.
- **Ordre d'exécution** : (0) mesurer → (tête shim-main à fort levier) → (2 mécanismes de classe) → (1 lifting DLL pour
  la traîne) → (3 mop-up). Re-mesurer après chaque vague (le levier change).
- **PLAN ORDONNÉ ACTUEL (2026-07-18, post-lifting-DLL — validé utilisateur)** : le Levier 1 (lifting DLL) est démontré
  (contrôle comctl32 stateful bit-identique Wine, §5.0 supra). Ordre de la **prochaine vague**, du plus rendement/moins
  risque au plus gros :
  1. ✅ **RE-MESURÉ (Levier 0, 2026-07-18)** — 37 PE32 Win95 : instructions = bruit (lift complet) ; imports **plats**
     (tête traitée) **sauf une famille cohérente dominante : le GDI mapping-mode** (~12 fn, 3 binaires chacune). Détail
     journal 71.
  2. ✅ **Famille GDI mapping-mode FAITE** (la tête mesurée) : `SetViewportOrgEx`/`SetViewportExtEx`/`SetWindowExtEx`/
     `Scale*ExtEx`/`OffsetViewportOrgEx`/`SetMapMode`/`DPtoLP`/`LPtoDP` — transforme logique→device ⚠️ correctness-critique,
     vérifié **DIB-hash vs Wine**. **Seconde moitié = familles socle du plateau** : ✅ **menu** (`ModifyMenuA/W`/
     `SetMenuItemBitmaps`/`GetMenuCheckMarkDimensions`, 2026-07-19, `user32_menu2.c`) ; ✅ **thread**
     (`SetThreadPriority`/`GetThreadPriority`/`OpenProcess`/`TerminateThread`, 2026-07-19, `win32_thread_tail.c`, sur le
     modèle fiber coopératif) ; ✅ **divers non-display** (`WaitForInputIdle` harnais + `WinHelpA/W` hors-harnais —
     WinHelp spawn un winhlp32 qui hangerait le pipe ; 2026-07-19, `win32_misc_tail.c`) ; ✅ **texte tabulé GDI**
     (`TabbedTextOutA/W`+`GetTabbedTextExtentA/W`, 2026-07-19, `gdi_tabbedtext.c`, FreeType DIB-hash bit-identique Wine) ;
     ✅ **imprimante** (`Enum`/`GetDefault`/`Open`/`ClosePrinter`, 2026-07-19, `win32_printer_tail.c` — état « zéro
     imprimante » déterministe, bit-identique Wine). **⇒ Plateau Win95 ENTIÈREMENT couvert.** Reste hors-plateau (sur
     demande) : `GrayStringA` (callback de dessin custom, cosmétique) et `DocumentPropertiesA` (inatteignable headless).
  2bis. ✅ **RE-MESURÉ (Levier 0, 2026-07-19, IA revenu)** — 29 PE32 Win95 (+34 NE 16-bit) : instructions = bruit ; **la tête
     a changé** (l'ancien plateau est traité). Nouvelle tête par #binaires /29 : **Registry** (`RegCreateKeyA` 17 + Reg*),
     **DDE** (`Unpack/PackDDElParam` 17/15), **OLE init** (`OleInitialize/Uninitialize` 16/15, `CoCreateInstance` 16),
     **misc k32/u32** (`IsDBCSLeadByte`/`OpenFile`/`wvsprintfA` 16, `GetLogicalDrives`/`VerInstallFileA` 15), puis un cluster
     **GUI profond** à 5–7 (palette/capture/clip/scroll/SetROP2/SetStretchBltMode/WindowFromPoint…). Détail journal 71.
     Traité (2026-07-19) : ✅ **DDE-param** (`Pack/Unpack/FreeDDElParam`) + ✅ **Ole init** (`OleInitialize/Uninitialize`),
     `win32_dde_ole.c` ; ✅ **Registry en mémoire** (create/open/set/query/enum/delete round-trip bit-identique Wine,
     `win32_registry.c`, la tête à 17 binaires) ; ✅ **misc k32/u32** (`IsDBCSLeadByte`/`wvsprintfA/W`/`OpenFile`/
     `GetLogicalDrives`, `win32_misc_k32.c`) ; 🚧 **cluster GUI profond ENGAGÉ** : ✅ **capture + scroll** (état pur,
     `win32_capture_scroll.c`, bit-identique Wine) ; **reste du cluster** : clip (`IntersectClipRect`/`RectVisible`),
     `SetROP2`/`SetStretchBltMode` (correction-dessin → à gater), palette (32bpp=no-op sound), `WindowFromPoint`,
     `CreateDIBitmap`/`CreateRectRgn` — **converge avec le step 3** (vrai binaire GUI). `VerInstallFileA` (complexe) +
     `CoCreateInstance` (vrais objets COM) → plus tard.
  3. ✅ **DÉMONTRÉ (2026-07-19)** — **un vrai binaire GUI Win32 shippé (`FishTank.exe`, aquarium Win95, pur comctl32) atteint
     sa message loop sous ARET**, comme Wine (1380 fn, 0 unresolved call, instructions non-liftées = bruit data-en-code).
     Sur son chemin de démarrage réel il n'appelait que **3 imports** manquants (`GetProcessVersion`/`SetMessageQueue`/
     `GetCursorPos`, implémentés+mesurés vs Wine) ; les 25 autres du listing statique sont derrière l'interaction (drag-drop/
     menu/impression), non atteints headless. Prouve la machinerie contrôles + le chemin de démarrage sur du réel shippé.
     Tire le reste (CreateDIBitmap, clip, DrawIcon, `DragQueryFileA`, `LoadMenuA`/accél., GrayString) **au fil des besoins**.
  4. **MFC / VB40032** (le gros multiplicateur) — l'**EH C++** (`__CxxFrameHandler`) est **FAIT** (P3.10) et **prouvé sur du vrai MFC**.
     🚧 **Driver = WinMerge 2.14.0 / MFC90** (~40k fn, lifté **avec** `mfc90u.dll`). **État 2026-07-26** : franchit tout le CRT, toute la
     série de **ctors globaux MFC**, et **atteint l'init GUI de MFC** — 8 incréments vérifiés cette session (`_EH_prolog3_GS`,
     continuation de catch, dispatch C++ typé, 4 shims HLE, **I6** esi/edi/ebx threadés, **I7** abort bruyant, **I1** traceur, **dataflow
     MUST des registres porteurs d'import**). Mur courant = **surface GUI/HLE** (`SystemParametersInfoA` action `0x29`, `wcscat_s`),
     traité **data-driven** au coup par coup (chantier **I5** du doc 81), chacun vérifié vs Wine. ⇒ le blocage n'est **plus** l'EH ni le
     lift-correctness : c'est la **couverture d'API**. ✅ **1ʳᵉ vague comblée (2026-07-26)** : `SPI_GETNONCLIENTMETRICS` (+`wcscat_s`),
     **bit-identiques Wine**. Puis (2026-07-26) `EnumFontFamilies` (callback), `_wcsicoll`/`wcscoll`, le **tail call
     conditionnel** (`jcc <fonction>`), le **garde x87 rendu diagnostique**, et la **frontière statique/runtime x87**
     (`call` avec pile non vide ⇒ bail) qui fait tomber le mur x87. **État : WinMerge atteint le chargement de polices
     GDI.** Mur courant = **échec du cookie /GS dans `sub_791ebc` = dérive esp** (classifié, non résolu ; le modèle /GS
     est correct — 4 checks sur 5 passent). Prochaine étape = `-O0 -g` ou watchpoint sur le slot du cookie.
  5. **win32k `NtGdi*`/`NtUser*`** — **différé** (inutile tant qu'on ne lifte pas gdi32/user32 eux-mêmes ; notre HLE les
     couvre). Priorité basse, sur demande de la mesure seulement.

### P1 — sqlite3 mingw ✅ FONCTIONNEL (2026-07-05)
**2 bugs généraux résolus, sqlite3 mingw = bit-identique à Wine** (scalaire, CRUD,
agrégats, jointures, index, CTE, window, IN) :
1. **Émission `imul` 1-op signé × const magic** zéro-étendu → `%23` négatif → OOB
   (crash `SELECT`). Cf. §4.1 + 71 `[LIFT][RECOMPILE]`.
2. **SSA split du bloc d'entrée = en-tête de boucle** (φ entry value perdue pour un
   param regparm variable de boucle) → boucle infinie (crash `CREATE TABLE`). Cf.
   §4.1 + 71 `[LIFT][SSA]`.
*Reste (mesure future)* : passer le `gauntlet/score.sh` complet + le sweep sqlite
sur le build mingw pour re-mesurer la surface (FTS/RTREE non balayés → abort sound
s'ils butent).

### P1bis — ⚠️ **Trou de soundness connu : `setlocale` accepte tout et rend `"C"`** (trouvé 2026-07-26, non corrigé)
`aret_setlocale` (`aret_hle.c`) renvoie **`"C"` quelle que soit la locale demandée**, au lieu de rendre **NULL** (= échec) pour celles
qu'on ne modélise pas. Un programme qui fait `setlocale(LC_ALL, "French")` croit donc avoir changé de locale alors qu'on reste en C ⇒ tout
ce qui en dépend (**collation** `_wcsicoll`/`wcscoll`, formats de nombres/dates, casse non-ASCII) diverge **en silence** — exactement le
mode d'échec que le §0 interdit. Trouvé en mesurant `_wcsicoll` (ordinal en locale C, cf. §4.5) : la valeur implémentée est **juste pour la
locale C**, mais rien ne garantit qu'on y est. **Fix propre** : accepter `NULL` (requête), `"C"` et `"POSIX"` → `"C"` ; **abort sound** sur
toute autre locale (y compris `""`, qui demande la locale système). ⚠️ À faire avec la porte winediff complète : `""` est courant et
certains binaires du corpus peuvent l'appeler — mesurer d'abord combien, puis trancher (abort vs modéliser la locale système).

### P1ter — `__aret_callee_pop` : « inconnu » vs « cdecl prouvé » (nuance mesurée 2026-07-26 — **PAS le bug qu'on croyait**)
Première lecture (fausse) : « 55 VAs non récupérées reçoivent un pop deviné à 0 ». **Mesure de contrôle** : ces VAs sont des
**slots IAT** (677 déclarés dans la plage `0x651xxx` de WinMerge) — et le §4.3 dit que le design **repose** sur
`__aret_callee_pop` rendant **0 sur un slot d'import** (c'est ce qui évite le double-pop : le pop statique in-block fournit
`@N` une seule fois). Donc **0 y est voulu et correct**, pas une devinette. ⇒ **aucune instance mesurée de nuisance.**
Ce qui subsiste, en théorie et sans cas observé : la table ne stockant que les pops **non nuls**, « absent » ne distingue pas
« récupérée et cdecl » (0 **prouvé**) de « adresse vraiment inconnue, ni fonction ni slot IAT » (0 **deviné**). À **ne pas**
traiter spéculativement : zone à haut risque (un 1ᵉʳ fix callee-pop a été reverté pour double-pop sur le lifting-DLL), et
**aucun binaire ne l'exige aujourd'hui**. Rouvrir seulement si une mesure exhibe une VA de ce troisième type.
Cf. 71 (2026-07-26 [ABI][SOUNDNESS], entrée **corrigée**).

### P1quater — ⚠️ **`aret_GetProcAddress` renvoie TOUJOURS 0** (trouvé 2026-07-26, non corrigé)
`aret_GetProcAddress(esp) { return 0; }`. Tout programme qui **lie une API à l'exécution** (le motif normal pour les API
optionnelles, les chemins de compatibilité de version, les plugins) reçoit « fonction absente » et bascule sur son repli —
**y compris pour des fonctions qu'on implémente réellement**. Ce n'est pas une valeur fausse (c'est « indisponible »), mais
c'est une **divergence silencieuse** avec Wine qui change le chemin d'exécution. **Fix cadré** : résoudre les noms pour
lesquels un shim existe (table nom → adresse d'un trampoline), 0 sinon. Bloque aussi les fixtures : une fixture ne peut pas
tester par `GetProcAddress` une fonction que l'import-lib mingw n'expose pas (cas `_wcslwr_s` & co., cf. 71).

### P2 — Robustesse x87 : joins ambigus + transcendantales ⇒ **QUALITÉ, pas correction** (MESURÉ 2026-07-10)
**Mesure décisive (règle « vérifier si le filet runtime est actif AVANT de conclure ») :**
tous les chemins x87 qui *bail* statiquement sont **corrects via le filet runtime**,
**bit-identiques à Wine**. Vérifié end-to-end :
- **Joins ambigus** (`awk` `0x428500`/`0x429c14`, bail `ambiguous join depth 1 vs 0`) :
  `busybox awk` exp/log/sqrt/^/sin/cos/atan2/`exp(log 5)`/`3^3`/`10^-2` = **tous OK vs Wine**.
- **Transcendantales** (bail `unmodelled x87 op` = `fsin`/`fcos`/`fptan`/`fpatan`/`fyl2x`/
  `f2xm1`/`fscale`/`fsincos`) : fixture inline-asm brute (non host-backée) → filet
  `__x87rt_*` → **8/8 bit-identiques à Wine**. Gardé par `winecorpus/x87_transcendental.c`.
⇒ **Il n'y a PAS de feu x87 correctness.** P2 est un gain de **qualité** (lifter
statiquement au lieu du filet = C plus propre/rapide), **pas** de justesse. Or notre
étoile est la soundness, pas la vitesse ⇒ **P2 déprioritisé** (risque sans récompense
correctness-critique = à ne shipper qu'avec bénéfice mesuré, cf. règle §2). Reste
documenté pour plus tard ; *ne pas* y consacrer une session de forensics sans un binaire
qui **échoue réellement** (le filet couvre tout le testé). Pistes si un jour un op sort
du filet : suivre les valeurs conservées `fstp st(i)`/`fxch` ; fp-returning auto-récursif
(prouvé) ; host-back par signature d'idiome. Délicat (une fn à la fois, toutes portes).

### P3.10 — EH MSVC : brick B — **C++ EH base** (`_CxxThrowException` + `__CxxFrameHandler3`) ✅ FAIT (2026-07-25)
`throw`/`catch` C++ MSVC bit-identique Wine (`bench/eh/throw_catch.cpp` → `r=49`). `_CxxThrowException(pobj,pThrowInfo)` bâtit
l'`EXCEPTION_RECORD` C++ (`0xE06D7363`) et dispatche `fs:[0]` ; le handler du frame (`mov eax,&FuncInfo; jmp __CxxFrameHandler[3]`)
est détecté par son octet `0xB8` (le `.text` de l'image EST mappé à son VA) et routé droit sur `aret_CxxFrameHandler3` (arg à
`hesp+4` — ABI shim cdecl). Celui-ci parse `FuncInfo`→`TryBlockMap`→`HandlerType`, matche le type (`ThrowInfo`→`CatchableTypeArray`,
noms manglés), lie le param catch (`dispCatchObj`, réf/valeur), et **transfère** via la machinerie brick C (setjmp injecté au
SEH-establish — gate `uses_seh` élargi à `__CxxFrameHandler*` — + longjmp). **Continuation de catch** (≠ SEH) : le funclet retourne
l'adresse de reprise en eax ; `aret_seh_run` (branche `g_seh_is_cxx`) l'appelle puis `aret_call(continuation, …, ebp)` ; throw
imbriqué re-longjmpe au même setjmp (boucle). **ebp funclet = `frame+0xc`** (`&frame->ebp` de Wine). **Récupération SOUND des
funclets + continuations** par parse des tables EH du binaire (`analysis::cxx_eh_entries` — rien de deviné, prouvé par la
métadonnée). **Destructeurs d'unwind exécutés** (`aret_cxx_local_unwind` = `cxx_local_unwind` de Wine : parcourt l'`UnwindMap`,
avance `frame->state` avant chaque action, appelle le funclet destructeur ebp=`frame+0xc` ; à l'entrée du catch `state`→`tryLow`
puis `state:=tryHigh+1`) — oracle `throw_dtor.cpp` (dtor à `printf`) bit-identique Wine (`dtor`+`r=42`). **Unwind à deux passes multi-frames**
(`aret_cxx_global_unwind` : à la frame catchante, re-marche `fs:[0]` tête→cible avec `EH_UNWINDING`, lance les destructeurs de
chaque frame intermédiaire innermost-first ; dispatch 0xB8-aware) — oracle `throw_across.cpp` (throw dans callee, catch dans
caller, `Guard` dans l'intermédiaire) bit-identique Wine. Funclets destructeurs récupérés via l'**UnwindMap** (`parse_cxx_func_info` :
une frame peut n'avoir qu'une UnwindMap sans try). Portes : difftest **272/272**, hash **inchangé** (gaté sur l'import), ehdiff
**4/4**. **Reste** : rethrow (`throw;`), catch-by-value copy-ctor non trivial, driver réel WinZip `WZ32.DLL` (v1 `__CxxFrameHandler`,
mêmes offsets), MFC (FishTank).

### P3.9 — EH MSVC : brick C — `__except_handler3` réel (SEH scope-table) ✅ FAIT (2026-07-25)
`__try/__except/__finally` MSVC bit-identique Wine (`bench/eh/seh_except.c` → `a=42 b=1 c=3 d=5 fin=110`). `aret_except_handler3`
lit la registration `{prev,handler,scopetable,trylevel}`, marche la scope-table `{EnclosingLevel,FilterFunc,HandlerFunc}[trylevel]`,
appelle le **filtre** (avec `GetExceptionInformation` publié à `[EstablisherFrame-4]`), et sur `EXECUTE_HANDLER` fait global-unwind
(`RtlUnwind`) + local-unwind (`__finally`) + transfert non-local. **Transfert = setjmp injecté par le lifter** au SEH-establish
(`mov fs:[0],reg`, gaté sur l'import `_except_handler3` → tout le reste **byte-identique**, hash inchangé) : `emit::set_seh_active`
+ marqueur `__aret_seh_establish` (lift.rs) rendu en setjmp gardé par un check runtime establish-vs-restore (`newframe->prev ==
fs:[0]`, exclut `0xffffffff`/`0` avant deref) ; le handler stashe funclet/ebp en globals (survivent au longjmp) puis longjmpe ;
l'établisseur exécute le funclet via `aret_seh_run`. **ebp funclet = `EstablisherFrame+16`**. Portes : difftest **272/272**, hash
`19acad982194bf07` **inchangé**, winediff **177/178**, ehdiff.

### P3.6 — EH MSVC lourd : brique 1 — dispatch SEH `RaiseException` ✅ FAIT (2026-07-17)
`aret_RaiseException` (`aret_hle.c`) **dispatche** une exception software-raised dans la chaîne SEH `fs:[0]` (déjà
maintenue dans le TEB synthétique) : parcourt les frames, appelle chaque handler cdecl via `aret_call`, `ContinueSearch`
→ frame suivante, catch → transfert non-local (longjmp / scope-jump). Chaîne épuisée → abort bruyant (le stub no-op
précédent continuait en silence = faux). Testé par `winecorpus/seh_raise.c` (frame SEH manuel inline-asm — mingw n'a
pas `__try` ; 2 frames imbriqués, chain-walk) bit-identique Wine. winediff **115/115**. **Reste EH** : `__except_handler3`
réel (scope-table + CONTEXT peuplé + local-unwind), fautes matérielles (SIGSEGV→dispatch, natif), C++
(`_CxxThrowException`/`__CxxFrameHandler`).

### P3.8 — EH MSVC lourd : brique 3 — fautes matérielles (SIGSEGV→dispatch SEH) ✅ FAIT (2026-07-17)
`aret_hw_fault` (`aret_hle.c`, **natif seulement**) route un **trap CPU** (SIGSEGV/SIGFPE, car ARET exécute les
accès mémoire du programme comme de vrais load/store hôte) dans le **même dispatch `fs:[0]`** que `RaiseException` :
un programme qui protège un accès fautif par `__try/__except` (ou un frame SEH manuel) **catch et continue** au lieu
de mourir — comme Wine. Signal→code NT : SIGSEGV→`STATUS_ACCESS_VIOLATION(0xC0000005)` (+ExceptionInformation
read/write depuis `REG_ERR` + adresse `si_addr`), SIGFPE→`STATUS_INTEGER_DIVIDE_BY_ZERO(0xC0000094)`. **Pile scratch
dédiée** (`aret_eh_stack`) pour exécuter le handler : l'esp machine du point de faute est enfoui dans un registre hôte
et **irrécupérable** du contexte signal, mais **inutile** — un handler qui catch restaure l'esp depuis son *propre*
registration record (scope-jump `__except_handler3` / longjmp), piloté par la donnée du frame, pas par l'esp du
dispatcher. `SA_NODEFER` garde le signal catchable à travers le longjmp de sortie. **Chaîne épuisée sans catch ⇒
faute réelle non gérée : `SIG_DFL` + re-faute → le process meurt du vrai signal** (bruyant, jamais avalé en silence).
WASM : pas de signaux POSIX ⇒ mécanisme **exclu** (faute = trap sound). Gardé `winecorpus/seh_hwfault.c` (NULL-deref
catchée → `r=42 code=0xc0000005`, bit-identique Wine). winediff **117/117**. **Reste EH** : `__except_handler3` réel
(scope-table — testabilité = vrai binaire MSVC `__try`), C++ (`__CxxFrameHandler`/`_CxxThrowException`).

### P3.7 — EH MSVC lourd : brique 2 — local unwind `RtlUnwind` ✅ FAIT (2026-07-17)
`aret_RtlUnwind` (`aret_hle.c`) implémente le primitif d'**unwind local** que `__except_handler3` (via
`__global_unwind2`) utilise pour dérouler les frames entre le raise et la frame qui catch. **i386 (≠ x64)** : TargetIp
**ignoré**, parcourt `fs:[0]` jusqu'à (exclu) la TargetFrame, appelle chaque handler intermédiaire cdecl avec
`EH_UNWINDING(0x2)`, pop chaque frame, **retourne normalement** (`fs:[0]`=target). Bornes sound : chaîne cyclique →
abort, target introuvable → abort (`STATUS_INVALID_UNWIND_TARGET`). **La « muraille de testabilité » était une erreur
de mental-model x64** (« ne revient pas ») ; la mesure i386 prouve le retour normal → testable **directement** vs Wine :
`winecorpus/seh_unwind.c` (appel direct à `RtlUnwind`, 2 frames, `inner=1 flags=0x2 fs0_target=1` bit-identique Wine ;
pointeurs stashés en globals pour survivre au restore de registres non-volatils). winediff **116/116**. **Reste EH** :
`__except_handler3` réel (scope-table + filtre + `local_unwind2` — suite directe, mais testabilité = **vrai binaire MSVC
`__try/__except`** requis, mingw ne l'émet pas), fautes matérielles (SIGSEGV/#DE), C++ (`__CxxFrameHandler`).

### P3.5 — EH MSVC : 1re brique `push imm; …; ret` ✅ FAIT (2026-07-17)
`find_ret_jumps` (`analysis/mod.rs`) reconnaît l'idiome de continuation `__finally` (`push <cont>; …; ret` = `ret`
utilisé comme **saut** vers l'adresse poussée) par **interprétation abstraite forward** (pile symbolique, point-fixe) :
un `ret` est réécrit en `jmp <imm>` seulement quand `[esp]` est **prouvé** = la même constante-code poussée sur tous
les chemins (build.rs ajoute le `esp += 4` du pop). Sound par construction (ne peut que rater un saut, jamais en
inventer). Trouvé/fixé via sweep de `Ppview32.exe` (6 div → 0), 0 régression. **Reste EH** : frame SEH complet (7za, 96
frames), `__except_handler3`, exceptions C++ — cf. doc 80 §1.3. La carte de prévalence (SEH-frames) : nasm/sqlite=0
(complets), Ppview32=13, **7za=96** (le plus lourd).

### P3 — Récupération points-to (Phase 4 vtables / dispatch calculé)
- **NASM `-f obj` (OMF) ✅ RÉSOLU (2026-07-09)** : le stub `ret` nu (méthode no-op d'un
  `struct ofmt`) est **stocké via `mov [reg], imm`** (pointeur de méthode dans un objet pointé par
  registre) puis appelé par `call [obj+disp]`. `imm_code_ptrs` captait déjà l'immédiat mais
  `looks_like_func_start` rejetait le `ret` nu. Fix : détecteur **`mem_store_code_imm`** (`mov [base+…],
  code_imm`) + acceptation du stub `ret` nu (`is_bare_ret_stub`) **uniquement** via cette preuve
  address-taken (jamais en balayage linéaire → pas de faux positif sur du padding). `nasm -f obj` =
  **bit-identique à Wine** (cf. §4.4 + 71 `[RECOV]`).
- **plink** (PuTTY, clang) : points-to `0x450058` résolu. **Avancé (2026-07-09)** — env-block
  (`GetEnvironmentStringsW`), registre vide sound, et exemption jt des cibles d'appel direct (les 3 stubs
  no-op : **0 unresolved** désormais). Recovery complète, ne segfault plus sur l'env. **Reste** : segfault
  dans le chargement de config (`sub_4845d0` compare "SerialLine" vs NULL) — plink dépasse la détection de
  `-V` (Wine imprime la version et sort) → divergence de flot / miscompile amont dans le parsing d'args.
- Vrai **C++ g++** (exceptions, RTTI, thiscall) : non testable sur l'hôte (pas de
  mingw g++) ; le **dispatch vtable lui-même fonctionne** (fixture validée).

### P4 — busybox grep/sed ✅ FONCTIONNEL (regex + `sed -n` résolus)
Le SIGSEGV regex ancien **ne se reproduit plus** (résorbé par les fixes récup/SSA). Le dernier bug concret,
**`sed -n` (compteur `-n`) ignoré**, est **RÉSOLU (2026-07-09)** : cause = `___chkstk_ms` (mingw) traité
comme clobbering ecx alors qu'il **préserve les registres** → longueur de `memset` perdue → tableau
long-options de getopt32 non zéroé (cf. §4.3 + 71 `[ABI][DEMO]`). grep/sed (`-i/-v/-c/-o/-E`, classes,
`s///g`, `Nd`, `-n p/Np//re/p`) = **bit-identiques à Wine** (12/12 batterie). *Reste* : `sed -i` bute sur des
imports Win32 non implémentés (`_mktemp`, `GetCompressedFileSizeA`, `OpenProcessToken`…) — indépendant.

### P5 — m4 (mingw) ✅ FONCTIONNEL (2026-07-09). units = environnemental
**m4 = bit-identique à Wine** (macros, eval, translit, ifelse, récursion, `--version`). Cause du blocage
historique : `signal()` (`aret_signal`) était un **stub retournant 0** ; le bookkeeping mingw/gnulib de
blocage de signaux installe `_blocked_handler` puis, au déblocage, rappelle `signal()` en **assertant qu'il
rend l'ancien handler** — le 0 du stub faisait échouer l'assert `.cold` de `_sigprocmask` → abort. Fix :
table de handlers par signal (retourne l'ancien, stocke le nouveau ; pas de délivrance = inchangé). Cf. §4.5
+ 71 `[HLE-WIN32] signal`. units = **environnemental** (`units.dat` absent, pas un bug).

### P6 — Outillage funcdiff : appels indirects résolus ✅ FAIT (2026-07-17)
funcdiff **suit maintenant les appels indirects** (`call reg`, `call [table+idx*4]`, vtable
`call [obj+k]`) : l'interpréteur **évalue l'expression d'adresse** → cible concrète `t` ; si `t`
est une fonction récupérée à `ret N` connu → récurse via `call_direct` (mêmes mécaniques
push-retaddr / pop). **Sound double sens** : lift correct ⇒ même fonction qu'Unicorn (lockstep) ;
lift **faux** ⇒ cibles différentes ⇒ **divergence = vrai bug** (la classe invisible à la carte
statique) ; cible non-fonction (vtable via pointeur seedé aléatoire) ⇒ skip, jamais de faux verdict.
Effet mesuré : lift **18,4k→19,8k** scorées, appels suivis **7k→20k** (sqlite 4384→**17149**),
**0 divergence** (donc ces 13k+ cibles indirectes sont liftées correctement). Les cibles **basées
image** (tables `.rdata`, pointeur chargé d'une constante) résolvent déterministe → nouvelle
couverture ; les vtables à objet aléatoire restent skippées (borne honnête).
*(Ancienne P6 — closure SSA opt-diff — reste à faire séparément : threader tout l'état CPU au call.)*

### P6.5 — Fenêtre message-only USER32 ✅ FAIT (2026-07-10). Analyzer avance, nouveaux gaps
**Sous-système message-only livré et vérifié** (`aret_win32.c`, ~15 fns : Register/
Unregister/CreateWindowEx/Destroy/DefWindowProc/Get/Peek/Dispatch/Translate/Post/Send/
PostQuit/Set/KillTimer/MsgWait) : registre de classes + table de fenêtres + file de
messages mono-thread + timers ; chaque dispatch = **vrai callback WNDPROC dans le code
lifté via `aret_call`** (frame stdcall posée sous esp, réentrant). **Zéro graphisme** ⇒
standalone + WASM-portable. Gardé par `winecorpus/user32_msgwindow.c` = **bit-identique à
Wine** (register/create/Send synchrone/Post+Get+Dispatch/PeekvIde/WM_QUIT). winediff
50→**51/51**. `GetMessageW` sans message/quit/timer = **abort sound** (jamais hang/faux quit).
**Analyzer débloqué du notifier** (plus de `Tcl_Panic`) → avance et révèle la **suite
bornée** : `WSAStartup`/Winsock, `CreateEventW`, `wcschr`, `LoadLibraryW`, et le lift de
`repe cmpsb` (`rep cmps` = abort sound aujourd'hui). Prochains incréments si on poursuit Tcl.
*(sqldiff.exe testé au passage = bit-identique à Wine, tous modes.)*

### P7 — Chantiers longs : couverture « n'importe quel programme »
> Détaillé au **§6** (roadmap complète M5→M7 + 64-bit + threads + GUI + graphisme +
> macOS). Résumé de l'ordre : **types (Phase 6)** → **threads** → **64-bit
> (Phase 8)** → **USER32/GUI (Phase 7 / M7)** → **graphisme DXVK (M7)** →
> **macOS**. À lancer sur demande ou quand un binaire mesuré l'exige (pas
> spéculativement) — les démonstrateurs actuels (CLI console) n'en dépendent pas.

---

## 6. Roadmap complète — « faire tourner N'IMPORTE QUEL programme »

> Intègre en détail les milestones UBT (ex-doc 00), le design (ex-doc 01) et la
> stratégie d'intégration des briques (ex-doc 30). **Principe directeur : réutiliser
> les projets matures (Wine, DXVK, LLVM) comme back-ends vérifiés, ARET = le chef
> d'orchestre + la colle + la couche de soundness.** Viser leur union d'un bloc =
> ne rien livrer ; on avance par **tranches qui tournent de bout en bout**.

### 8.1 Milestones (tranches livrables — chacune = une classe de binaires qui tourne)
| # | Tranche | Classe débloquée | État |
|---|---------|------------------|------|
| M1 | Interception d'API → shims HLE natifs, recompile ELF | PE freestanding (kernel32) | ✅ |
| M2 | Imports indirects via registre + Memory Layout Mapper (globals à leur VA) | PE multi-réf données globales | ✅ |
| M3 | Pile machine partagée (args pile stdcall/cdecl + registres regparm/fastcall) | Win32 multi-fonctions | ✅ |
| M4 | Shims CRT msvcrt (printf variadique, malloc, mem*/str*) | .exe console CRT | ✅ |
| FS | Sous-système fichiers + traduction de chemins | prog. lisant/écrivant | ✅ |
| LLVM | Backend LLVM IR chunké (passe à l'échelle : 44k fn → 221 `.ll` → ELF) | mêmes binaires via LLVM (multi-arch futur) | ✅ |
| CRT+/W32 | Vrai CRT (forward libc) + Win32 native (kernel32→POSIX) | prog. C large + Win32 hors-GUI | ✅ |
| UNPACK | Déballage dynamique Unicorn (émule stub → OEP → dump) | packers non-VM | ✅ |
| M6 | Cible **WebAssembly** (`--target wasm`, wasmtime) | cible universelle | ✅ (7/7) |
| **M7** | **GUI / graphisme** (USER32/GDI via **SDL2** portable, puis DXVK/vkd3d) | applis fenêtrées, puis **jeux** | 🚧 **plan doc 72** — **couche USER32/GDI display-free quasi complète** : fenêtres/classes/messages (A+W), modèle fenêtre étendu, ressources/LoadString, MessageBox, **dialogs (DLGTEMPLATE+modal)**, **GDI DIB bit-exact**, menus, helpers, SID/token, rect/char/…, **+ fenêtre SDL VISIBLE (G2b : `SDL_Window`+présentation framebuffer+pompe `SDL_PollEvent`)** **+ GDI texte FreeType bit-identique Wine (G3, autonome)** **+ GDI vectoriel/raster complet (G6 : lignes/Rectangle/Polyline(To)/FrameRect/InvertRect/BitBlt-ROPs)** **+ LIFTING DLL : vrais contrôles comctl32 (ImageList, progress bar stateful) bit-identiques Wine (§5.0 Levier 1)**. **Reste** : autres contrôles comctl32 (même machinerie, data-driven), Ellipse/courbes (niveau-recherche), + hors-GUI : **threads coopératifs** (fibers, §4.7), **EH/RtlUnwind** |

> **Règle** : on ne s'engage pas sur M_n+1 tant que M_n ne tourne pas proprement ;
> chaque palier = un artefact démontrable + un test de non-régression.

### 8.2 Phase 6 — Inférence de types (LISIBILITÉ + JUSTESSE)
Largeur/signe/pointeur à partir de l'usage des registres, puis agrégats
(`obj->field_8`). *Critère* : types affichés, **jamais** au prix de la sémantique
(casts explicites conservés). Non bloquant pour l'exécution ; améliore la lisibilité
du C généré et peut aider les autres passes.

### 8.3 Multithreading — **fibers coopératifs** (doc 80). Incrément 1 fait
**Choix d'architecture** (doc 80 §2, > pthread) : `CreateThread` = **coroutine**
(`ucontext`) multiplexée sur l'unique thread hôte, bascule **seulement** aux points
bloquants ⇒ **zéro data-race, ordonnancement déterministe** ⇒ oracle différentiel
valide. Autonome (`ucontext` = libc statique) ; WASM ⇒ abort sound (Asyncify plus tard).
La pile machine est **par-fiber** ; `last_error`/TEB (globaux) sont **swappés par le
scheduler** à chaque bascule. Plan incrémental piloté par fixture :
1. ✅ **FAIT** — infra fibers + `CreateThread`/`ExitThread`/`ResumeThread`/`GetExitCodeThread`
   + `WaitForSingle/MultipleObjects` (join) + `Sleep`=yield + `last_error` par-fiber.
   Oracle `thread_join.c` (somme déterministe vs Wine). Détail §4.7.
2. ✅ **FAIT** — **CriticalSection réelle** (owner + récursion, blocage sur CS tenue) — oracle
   `thread_critsec.c` `counter=4000` (RMW coupé par un yield sous le lock = discriminant réel).
3. ✅ **FAIT** — **Events** (manual/auto-reset, Set/Reset/Wait) — oracle `thread_event.c`
   (gate release-all `60` + ping-pong auto-reset `15`).
4. ✅ **FAIT** — **Mutex/Semaphore** (waitable, récursif/borné), **TLS par-fiber**,
   **`_beginthread(ex)`** — oracle `thread_mutex_sem.c` (`2000`/`tls_ok=1`/`21`). **Chantier fibers complet.**
*Frontière dure* : `CreateProcessA/W` (lancer un `.exe` enfant) — pas de Windows pour
l'exécuter ; reste **échec sound**, pas simulé. `CreatePipe` (anonyme) est déjà fidèle
(`pipe()`).

### 8.4 Phase 8 — Lift 64-bit (multi-arch réel)
Porter le lifter/modèle en **64-bit** : préfixes **REX**, registres 64-bit
(rax..r15), ABI SysV/Win64 (args en registres), tailles de pointeur 8 o. Débloque les
binaires 64-bit **et** l'émission d'ELF **ARM** (le backend LLVM est déjà multi-cible).
Rattaché : le contrôle de **débordement de quotient `div`/`idiv` 64-bit** (chemin
software `-m32` aujourd'hui non vérifié — noté en commentaire dans `emit`). *Gros
chantier* ; le socle 32-bit (axe 1 blindé) est le prérequis prouvé.

### 8.4bis Rétro-cible **Windows moderne** (doc 80 §1.6, orientation enregistrée 2026-07-19)
Troisième famille de cibles (après ELF et WASM) : **vieux binaire Windows → tourne sur Windows 11**. Deux cas
disjoints. **Cas 1 (bon marché)** : vieux **32-bit → PE moderne autonome** — backend émet un PE, le HLE *forwarde au vrai
Win32* pour ce qui existe encore et **embarque** sa réimplé des API **retirées** (`WinHelp`/`.hlp`, vieux DirectDraw…) ;
bénéfice = **bundling** (zéro « runtime VB6/MFC42 manquant ») + ressusciter les apps cassées par les suppressions d'API.
**Cas 2 (le grand prix)** : vieux **16-bit NE → PE 64-bit** — seul moyen **natif** de faire tourner du 16-bit sur Windows
64-bit (NTVDM retiré) ; le **volume** du vieux logiciel est là (Chip CD : majorité NE 16-bit). Nécessite un **nouveau
frontend lifter** (segmentation, real/protected-mode, Win16, loader NE), jalon dédié, **prérequis lifter partagé avec la
Phase 8**. Conformité totale (autonomie redéfinie : « zéro dépendance au runtime **supprimé** » ; oracle = vrai Win32).

### 8.5 Phase 7 / M7 — Couche OS élargie : Winelib / USER32 (vers la GUI)
Deux voies (cf. doctrine §1, arbitrage indépendance de la preuve) :
- **Winelib** : router les imports de la sortie transpilée vers les implés **natives
  de Wine** (kernel32/user32/gdi32…) au lieu de `aret_hle`. **Étape une fois, pas par
  binaire** ; mécanisme **prouvé** (un `winegcc -m32` produit un ELF natif, zéro
  émulateur CPU). Toolchain (`wine32-tools` + `libwine-dev:i386`) demande un env
  propre. Couvre la surface Win32/USER32 **d'un coup**.
- **Voie médiane** : auto-générer la **tuyauterie** des shims (ABI, `@N` stdcall)
  depuis **win32metadata**, garder le **comportement** vérifié contre un oracle
  indépendant (moins « tout d'un coup », indépendance totale de la preuve).
*Critère M7 (fenêtré)* : une appli Win32 GUI (USER32 : CreateWindowEx/DefWindowProc/
message loop) s'affiche sous X11 via Winelib. C'est la **1ʳᵉ marche graphique**.

### 8.6 M7 (suite) — Graphisme / jeux (DXVK / vkd3d)
**Ne pas réécrire DirectX → rediriger vers DXVK/vkd3d** (D3D→Vulkan, natif). ARET
lifte le **code propre** du jeu ; les appels D3D sont **branchés sur DXVK**, la GUI
sur USER32/Winelib, le CPU tourne **natif** (pas d'émulateur). C'est **le « mur » des
jeux** — le plus lourd, documenté, non prioritaire. Note honnête : pour *jouer* tout
de suite, Wine+DXVK suffit ; l'intérêt ARET est le **code natif ré-exécutable** (mods,
serveurs, portage) sans émulation CPU.

### 8.7 Phase 9/10 — macOS (Mach-O) & au-delà
Charger/émettre du **Mach-O**, HLE macOS (BSD syscalls, Cocoa minimal), cible
ARM64. Chantier long, après le socle 64-bit. Packaging cible (`.app`, `.apk`+JNI)
= design d'origine (doc 01), non amorcé.

### 8.8 Ce que la réutilisation vérifiée atteint réellement (rappel doctrine)
Le trio « **tout** binaire + 100 % fonctionnel + 100 % natif pur » est **prouvé
impossible** (indécidabilité). Atteignable : **vrai logiciel compilé → pleinement
fonctionnel** ; le résidu (obfusqué/fait-main) **se signale** (abort), ne ment
jamais. Chaque brique lourde (Wine, DXVK, Unicorn, WASI) se **branche** derrière la
couche de vérification d'ARET — la finalité et les garanties sont inchangées, seule
la **vitesse** change.

---

## 7. TIPS — astuces & découvertes (accès rapide)

### Environnement / conteneur
- **Le conteneur est éphémère et peut revenir à un état antérieur.** Au démarrage :
  `git fetch origin <branche>` **puis** comparer à `origin` avant de conclure quoi
  que ce soit — le travail est sur `origin`, pas forcément en local. Restaurer :
  `git reset --hard origin/<branche>`. Le hook `.claude/hooks/session-start.sh`
  resynchronise un arbre **propre** ancêtre d'origin, mais **pas** le non-committé.
  ⇒ **commit + push après chaque incrément.**
- **RESTAURATION TOOLCHAIN après un reset conteneur (2026-07-18, recette vérifiée)** : le reset peut effacer les outils
  **installés à la main** (mingw, wine 32-bit, gcc-multilib, SDL2:i386, polices) — le hook n'installe que z3/SDL2/Xvfb.
  Le **git reste intact** (tout est sur origin). Recette :
  1. `sudo apt-get install -y gcc-mingw-w64-i686 gcc-multilib g++-multilib` (build fixtures + portes natives `-m32`).
  2. **Wine 32-bit** (bloqué par un skew `libgd3` PPA-sury(amd64) vs Ubuntu(i386)) : d'abord **downgrade** l'amd64 pour
     aligner les arches — `sudo apt-get install -y --allow-downgrades libgd3=2.3.3-9ubuntu5 libgd3:i386=2.3.3-9ubuntu5` —
     puis `sudo apt-get install -y --no-install-recommends wine wine32:i386 wine64`. (Sans le downgrade, `wine32:i386` est
     bloqué et le wine WoW64 seul **ne lance pas** les PE 32-bit.) Vérif : `wine hello32.exe` doit sortir.
  3. `sudo apt-get install -y --no-install-recommends libsdl2-dev:i386 fonts-liberation fonts-wine fontconfig xvfb`
     puis `sudo ln -sfn /usr/share/wine/fonts /usr/share/fonts/wine && sudo fc-cache -f` (les fixtures peinture/police).
  Portes **natives** (difftest/funcdiff/cpudiff/SMT/recompile) ne dépendent que de `cc`+`-m32` ; **winediff** dépend de
  wine 32-bit. *(Fixture `gdi_uifont` sensible au fontconfig **i386** : peut diverger post-reset tant que le cache i386 ne
  voit pas les polices Wine — orthogonal au code.)*
- **Réseau** : `github.com` est **BLOQUÉ** par la policy proxy. Joignables :
  `ftp.gnu.org`, `nasm.us`, `curl.se`, `zlib.net`, `sourceware.org`, `sqlite.org`,
  `pypi.org`, **`archive.org`** (HTTP/2 200 via proxy, vérifié 2026-07-11). Cross-compile :
  `i686-w64-mingw32-gcc` (mingw). Pas de `mingw g++` ni de toolchain MSVC → binaires MSVC
  **téléchargés prébuild** (nasm.us, sqlite.org précompilés). `winetest.exe` (WineHQ) bundle
  ~367 modules de conformance.
- **Corpus GUI Win95 (pour le re-sweep M7/G7)** : `https://archive.org/download/BestOfWindows95DotCom`
  — collection « Best of Windows95.com », **4 ISO** `WIN95_09961.iso`…`WIN95_09964.iso` (vraies
  applis GUI Win95 shareware). Reachable via proxy ; extraire les `.exe` (HTTP-range sur l'ISO
  ou `7z x`/`bsdtar` en local) → `--mode walls` + `wallsweep.sh` pour prioriser les murs GUI
  restants **par la donnée**. (Le corpus « 41 exe Win95 » d'une session antérieure venait d'une
  source ISO similaire, non committée — conteneur éphémère.) Listing/metadata :
  `https://archive.org/metadata/BestOfWindows95DotCom`.
- **Corpus MSVC 32-bit 1997 — CD magazine Chip (ita)** (fourni par l'utilisateur, gardé en mémoire) :
  `https://archive.org/download/chip-cd-ita-7-8-97/Chip_CD.iso` (ISO 657 Mo, curl **`-L`** obligatoire —
  redirect vers un miroir `dnNNN.*.archive.org`) et `https://archive.org/details/chip-cd-ita-3-97` (image
  BIN/CUE 742 Mo, non balayée). `network-32` **ne résout pas** (item dark, 0 fichier — écarté). **Mesure
  (7-8-97, 2026-07-17)** : 424 PE au total, dont **49 PE32** (le reste = **16-bit NE** Win3.x ou
  self-extractors InstallShield). Les EXE 32-bit runnables (`itiem95`, `ARTLANT`, `DEMO32`/`DEMODS3D`
  DomuS3D, `slidelib`, + les connus `Ppview32`/`dxfix`/`itmnm2095`/`wzbeta32`) → **funcdiff 0 divergence**
  (~34k fonctions scorées) : lift **prouvé correct**, **aucun bug neuf**. Matériel MSVC-C++/MFC lourd
  présent (**AutoCAD LT** : `acis.dll` noyau ACIS, `mfcans32`/`mfcuia32`, `msvcrt20`) = bon vivier pour les
  **briques EH C++/`__except_handler3`** futures, mais les `.dll` scorent 0 en funcdiff (il faut le
  **loader multi-modules**, doc 80 §1.2, pour les piloter). ⇒ Utile comme **corpus de code MSVC frais**,
  mais ne débloque pas seul la brique 3 (il faut un binaire qui **exécute** un unwind `__try/__except`).
- Binaires de test committés : `bench/.cache/` (busybox/sqlite3/winetest, négations
  `.gitignore` + README sha256) et `bench/gauntlet/gauntlet-bins.tar.gz` (21 PE,
  auto-extrait par `score.sh`).
- **Wine EST natif** (pas un émulateur) — winediff exécute de l'i386 réel sur le CPU.

### Diagnostic
- **Diagnostiquer l'x87 avec un vrai `--out-dir`** : `--out-dir /dev/null`
  court-circuite l'abaissement des fonctions → « pas de bail loggé » **trompeur**.
- **`ARET_X87_DEBUG=1`** (`x87dbg`) imprime `fn=… @… : raison` à chaque bail x87.
- **Vérifier si le fallback runtime x87 est actif** avant de conclure à un abandon
  statique (les `__x87rt_*` marchent là où l'asm no-op abort).
- **Localiser un crash natif** : recompiler le C généré en **`-O0 -g`** (mapping
  propre, pas d'inline LLVM) puis gdb → instruction + champ exacts.
- **Vérifier la config du binaire cible avant de présumer un bug** : Lua semblait
  tronquer le 64-bit → `string.packsize("j")=4` prouve `LUA_32BITS` (comportement
  correct). Toujours confirmer.
- **⭐ LE COOKIE /GS EST UN DÉTECTEUR DE DÉRIVE ESP GRATUIT (2026-07-26)**. Lifté, le contrôle /GS de MSVC se réduit
  à `esp_épilogue == esp_prologue` : le prologue écrit `cookie ^ esp` dans le frame, l'épilogue relit et re-XOR avec
  esp — ça ne passe que si esp est revenu à l'identique. ⇒ **tout binaire MSVC /GS embarque, à chaque épilogue
  protégé, un test de la famille de bugs la plus coûteuse d'ARET** (esp-drift : cksum, 7za, mur `0xe`). Atteindre
  `__report_gsfailure` **prouve** une dérive esp dans la fonction appelante — signal bien plus fort qu'un sweep
  statique, et gratuit. Vérifier au passage que le modèle tient (compter les checks qui **passent** : sur WinMerge,
  4 sur 5) avant d'accuser le lift.
- **⭐ « ENVIRONNEMENTAL » n'est PAS le bon critère — « GARDABLE » l'est (2026-07-26)**. La règle `EnumFontFamilies`
  (« modéliser le contrat, pas la donnée ») se raffine : ce qui interdit d'embarquer une donnée, c'est qu'elle soit
  **machine-dépendante** (liste de polices : deux exécutions honnêtes diffèrent ⇒ aucune porte ne peut la fixer). Une
  donnée **version-dépendante mais déterministe** — la **table de locales de Wine**, compilée dans Wine — est
  **gardable exhaustivement** : on peut l'embarquer **à condition** que la porte la **balaie entièrement**, si bien
  qu'un changement de version vire au rouge au lieu de pourrir en silence. Précédents : sort-keys de collation,
  `ConvertDefaultLocale` (65440 valeurs balayées).
- **Tout changement de PARALLÉLISME se valide sur ≥3 exécutions complètes (2026-07-26)**. Un défaut de concurrence est
  **intermittent** : un run vert ne prouve rien. Ma 1ʳᵉ validation de winediff parallèle (un seul run byte-identique)
  a laissé passer deux fixtures GUI qui flappaient — et c'était l'**ORACLE** qui se dégradait. Corrigé (GUI en série),
  revalidé sur 3 runs.
- **Une FIXTURE doit séparer contrat et environnement, sinon elle flappe (2026-07-26)**. `win_timechar` assertait la
  position **absolue** d'une fenêtre : sous charge parallèle le serveur X ne l'honore pas toujours, et c'est **l'ORACLE**
  qui bougeait ⇒ rouge intermittent. Réécrit en **contrat** (écart client↔écran = origine cliente ; les deux conversions
  s'inversent), vrai où que la fenêtre atterrisse. **Une porte instable est pire qu'une porte lente** : elle apprend à
  ignorer le rouge.
- **Pour toute fixture d'API `*_s` : vérifier à l'`objdump` qu'elle IMPORTE bien msvcrt.** mingw fournit ses **propres**
  corps pour plusieurs d'entre elles (`memmove_s`, `memcpy_s`, `_strupr_s`…) ⇒ sans `.def` forçant l'import, la fixture
  mesure **mingw des deux côtés** et ne garde **rien**. Mécanisme : `winecorpus/NOM.def` (cf. `crt_mem_s.def`).
- **⭐ WINE SE TROMPE — RAREMENT, MAIS IL SE TROMPE (2026-08-01)**. La doctrine §1 notait la circularité
  « Wine oracle *et* implémentation » comme un arbitrage théorique ; un runner **`windows-latest`** la rend
  **mesurable**. Résultat sur les 4 premières fixtures passées au runner : **2 divergences** sur du comportement
  **déjà livré et déjà vert** (`PathAddExtension(…, NULL)`, le code d'erreur de `PathFileExists`), plus un
  **bug de Wine confirmé** (`PathIsUNCServerA` rend FAUX pour toute entrée, là où Windows fait `A ≡ W`). ⚠️ Deux
  conséquences de méthode : (a) quand Windows tranche contre Wine, notre shim **diverge volontairement** de Wine et
  la case correspondante **ne peut plus être gatée** en winediff — l'écrire dans l'en-tête de la fixture, sinon une
  session future « corrigera » en réalignant sur le bug ; (b) le **résultat négatif compte** — gatées sur les mêmes
  lignes, `PathCommonPrefix`/`PathIsPrefix` reviennent identiques, donc on sait **où** Wine suffit.
- **Avant de croire un ROUGE de winediff, le relancer SEUL (2026-08-01)**. Signature à reconnaître : le côté
  **ORACLE** est **vide** et ARET, lui, a produit sa sortie — ce n'est pas ARET qui se trompe, c'est Wine qui n'a
  rien rendu sous charge. Vécu sur `comctl_loadbitmap` (init image-list) et `console_cp` (attache console), verts
  seuls. Traitement : marqueur **`winecorpus/NOM.serial`** (les sérialiser), pas tolérer. Même famille que les
  « 104 FAIL » qui étaient un `/tmp` plein : **toujours qualifier la NATURE d'un rouge avant d'en tirer une cause**.
- **⭐ UN OUTIL DE VÉLOCITÉ BIEN CONSTRUIT EST UN ORACLE GRATUIT (2026-08-01)**. Le cache d'objets a été écrit pour
  aller plus vite ; sa **première** trouvaille a été un bug. Le mécanisme est général : le cache **prédit** un taux de
  réutilisation (rien n'a changé ⇒ tout doit être réutilisé), donc l'écart à cette prédiction est un **signal**. Ici :
  42 réutilisations sur 255 ⇒ le C généré n'était **pas déterministe** (212 des 254 `.c` différaient entre deux runs de
  la même commande). Aucune porte ne pouvait le voir : le hash `difftest_transpile` est **comportemental**, donc
  indifférent à la numérotation SSA. À retenir avant de bâtir un outil d'infra : *quelle mesure produit-il, et que
  saurais-je si elle sortait fausse ?*
- **Vérifier une porte AVEC et SANS le nouvel outil** : difftest/difftest_transpile ont été passés `ARET_NO_OBJCACHE=1`
  **puis** avec le cache — sinon la preuve du fix dépendrait de l'outil qu'elle est censée valider.
- **funcdiff `0 divergence` ≠ pas de bug** : ça dit *où il n'est pas* (lift brut des
  fonctions scorées sain). Les bugs profonds sont dans les fonctions **skippées**
  (derrière imports) ou **en aval** (SSA/opt — couvert par l'opt-diff).

### Pièges du lifter (récurrents)
- **iced `condition_code()` renvoie `None` pour FCMOVcc** → mapper le mnémonique
  explicitement, sinon `cc_to_cond(None)=konst(1)` = move inconditionnel (faux).
- **iced modélise `fucom st(1)` en 2 opérandes** (ST0 implicite + ST1) → lire
  `op_register(op_count()-1)`, **jamais `op_register(0)`** (= ST0 = compare avec
  soi-même). Bug awk `/`.
- **Valeur 32-bit modélisée en paire 64-bit** occupe 8 o sur la pile machine → décale
  l'arg suivant de +4 (famille esp-drift : cksum, 7za). Un arg/valeur 32-bit ne doit
  occuper que 4 o à la poussée.
- **Appels libc émis sans prototype** passent les `uint64_t` en **2 mots** (32-bit) →
  `memcpy(dst, src.hi=0, n=src.lo)` cassé. **Caster les args libc en `(uint32_t)`**.
- **Immédiat signé** : `cmp/add/sub r32, imm` sign-étend l'immédiat à la largeur
  d'opérande ; masquer `a` et `b` à `w` bits avant `Ult`/`Eq`/carry (opérande
  registre masqué cachait le bug, d'où difftest vert mais transpile faux).
- **`jmp import` (tail-call) ≠ `call import`** : pas d'adresse de retour poussée →
  args de l'import à `[esp+4]`, pas `[esp]` → passer **esp+4**.
- **DCE** : un **appel indirect est impur** (effets inconnus) → `CallTarget::
  Indirect(_) => true`, sinon supprimé quand son résultat est mort. Les **helpers
  purs** (`__ix_pf`/`__ix_cpuid`/`__fp_*`/`__x87_*`) sont supprimables **sauf** ceux
  qui peuvent fauter (`__ix_*div*`) ou écrire (`__rep_*`/`aret_*`) — sinon un
  `__ix_pf` mort empêche la copy-propagation → casse la division magique.
- **`stdcall_pops` doit rester trié** (binary-search) — test `table_is_sorted_by_name`.
- **`--entry main` ne retire qu'UN underscore** (`main`↔`_main`), sinon `___main`
  (glue mingw) matche « main » et gagne par adresse basse → sortie vide.

### Pièges HLE / vrais binaires
- **FILE msvcrt (32 o)** : `_ptr`@0/`_cnt`@4/`_base`@8/`_flag`@12/`_file`@16. glibc
  FILE `_flags`@0 = `0xfbad2488`. getc/putc inlinés lisent les champs directement →
  un FILE glibc rendu par `fopen` crashe. ⇒ tous les FILE en layout msvcrt fd-backed.
- **busybox** : `argv[0]` doit commencer par `busybox` (sinon `basename` → applet not
  found — même sous Wine) ; **pas de métacaractère glob** dans les args (CRT_glob →
  Wine expanse `*` contre le cwd, non déterministe).
- **sqlite VFS** : `ReadFile`/`WriteFile` prennent l'offset via **`OVERLAPPED`** (pas
  SetFilePointer) → `pread`/`pwrite`. `GetFileAttributes*` **doit poser
  `GetLastError`** (`ERROR_FILE_NOT_FOUND`) sinon `winAccess` conclut IOERR.
- **`--run` doit `Stdio::inherit()`** (pas `output()` qui ferme stdin) sinon EOF
  immédiat pour tout programme lisant stdin.
- **Un shim qui appelle un helper `static` du runtime doit être défini APRÈS lui**
  (sinon déclaration implicite → symbole fort non émis → stub faible gagne).

### Méthode qui marche
- **Wine (et sa source) = livre de recettes, PAS runtime.** L'oracle Wine dit *quoi*
  produire (différentiel) ; quand on bute pour **reproduire** un comportement en
  autonome, **lire la source de Wine** (gdi32/user32/…) donne la **recette exacte**
  (formule de transforme, poids de sort-key, layout de struct), qu'on **réimplémente
  proprement dans ARET** — jamais on ne lie/dépend de Wine au runtime (autonomie
  préservée). Précédents : FreeType (rasterizer), sort-keys de collation, tables
  CharToOem. C'est la doctrine §1 rendue tactique.
- **Le différentiel *large* attrape les faux silencieux** que les tests étroits
  masquent (`ceil(3.0)` cache `ceil(3.2)` ; `sin(0)` cache `sin(sin(1))`). Toujours
  balayer une grille, pas un point.
- **Un vrai binaire lancé bout-en-bout vs Wine** est le meilleur révélateur de « où
  on en est » — il sort des bugs généraux qu'aucun test synthétique ni sweep
  statique ne révèle (printf %I64, cluster stdin busybox…). Systématisé en sweeps.
- **ÉCRAN VIRTUEL = ORACLE GUI PIXEL (Xvfb + SDL + capture, outillé 2026-07-19).** Pour un binaire **fenêtré**, la preuve
  ultime = les **pixels**. Recette : `Xvfb :99 -screen 0 WxHx24 &` puis `DISPLAY=:99` ; lancer **l'app ARET** (`./app`, SDL
  lié dès qu'un import `CreateWindowExA` + `pkg-config sdl2` → vraie `SDL_Window` sur le X virtuel) **et** le PE sous **Wine**
  sur le même display ; **capturer** avec `import -window root out.png` (ImageMagick ; `apt-get install imagemagick x11-apps
  xdotool`). Comparer les deux captures (visuel, ou hash/pixel-diff). **Distinction clé** : ce capteur d'écran inclut le
  compositing (bordures WM, position) ⇒ **oracle qualitatif/visuel**, pas bit-exact ; pour du **bit-exact** on garde le
  **DIB-hash** (dessiner dans un DIB mémoire qu'on possède, hasher — cf. `gdi_textout`/`gdi_tabbedtext`). Les deux se
  complètent : DIB-hash prouve la primitive GDI exacte, l'écran virtuel prouve que le **pipeline fenêtre→GDI→SDL** compose
  et affiche sur du réel. **Découverte 2026-07-19** : `FishTank.exe` (calculateur d'aquarium) tourne sound sous ARET
  (message loop, 0 mur) mais s'affiche **noir** — son UI est un **dialogue à contrôles natifs** (BUTTON/EDIT/COMBOBOX,
  user32) dont la **peinture est display-free** aujourd'hui. C'est le prochain grand chantier visuel (widgets natifs, ci-dessous).
- **Widgets natifs (BUTTON/EDIT/COMBOBOX/LISTBOX) — où ça se place (stratégie 2026-07-19).** ⚠️ **Le lifting DLL (Levier 1)
  ne les couvre PAS gratuitement** : les contrôles **de base** vivent dans **user32.dll**, qui descend aux syscalls
  **win32k** (`NtUser*`/`NtGdi*`) pour dessiner → lifter user32 = heurter le **mur win32k** (doc 80 §1.2). En revanche les
  contrôles **comctl32** (progress/trackbar/toolbar/listview) sont **user-mode** et peignent en **GDI** qu'on a → **ceux-là**
  le lifting comctl32 les couvre (logique prouvée sur la progress bar). **✅ FAIT (2026-07-24) : composite parent→enfant généralisé**
  → un contrôle **comctl32 lifté** (progress bar) **peint à l'écran** : chaque enfant reçoit son framebuffer client, le composite
  **pilote son `WM_PAINT`** (esp threadé) puis blitte à l'offset (clip), pour dialogue **et** fenêtre simple (`u32_present_toplevel`).
  Rendu classique authentique (uxtheme→classique), état déjà bit-identique Wine (`comctl32_progress`). Détail journal 71 (2026-07-24).
  Donc **deux voies** : (a) contrôles **de base user32** → **réimplémenter leur peinture
  dans le HLE** (`aret_win32.c`, dessin via notre GDI, vérifié DIB-hash vs Wine — comme Wine le fait dans user32) **ou**
  lifter user32+router win32k ; (b) contrôles **comctl32** → **lifting DLL** + peinture GDI→SDL. **Calendrier** : non
  planifié comme jalon dédié à ce jour ; listé « reste » (§4.5/§8.5). Fondation à poser d'abord = les **primitives de
  peinture de contrôle** (`DrawEdge`/`DrawFrameControl`/`DrawFocusRect`, ce que TOUT contrôle et Wine utilisent), vérifiées
  **DIB-hash** — puis câblées au WM_PAINT des contrôles, puis composées à l'écran (oracle Xvfb).
- **Carte des murs statique AVANT de dérouler au runtime** (`--mode walls`) : le runtime
  ne frappe qu'un mur à la fois (un seul chemin) ; l'analyse statique voit **tout le code
  récupéré** en une passe → énumère **d'un coup** les murs de couverture (instructions non
  liftées par nb de sites, imports manquants, appels non résolus). Ça transforme « mur après
  mur » en **liste priorisable** : souvent « des dizaines de murs » = quelques **familles
  bornées** (ex. analyzer : 149× `repe cmpsb` = **1** fix ; 29 imports = **1** famille socket ;
  le reste = `ud2`=abort correct ou data-décodée-en-code non atteinte). ⚠️ La carte ne couvre
  que les murs **de couverture** (statiquement énumérables) — **pas** les bugs de comportement
  (miscompiles, indécidables) qui, eux, restent du ressort des oracles différentiels.
- **Dégrossir un corpus** : `--mode walls` sur N binaires + agrégation des compteurs → les
  familles qui débloquent le plus de binaires (prioriser par la donnée, pas à l'intuition).
- **`--mode imports`** mesure l'axe 2 **a-priori** (statique, énumérable) → prioriser
  les shims par la donnée (winetest : `SetConsoleMode`/`GetExitCodeProcess`
  dominent — jamais deviné à la main). Puis **filtrer par fidélité** (n'expédier que
  ce qu'on rend exact).
- **Toute recovery ajoutée est risquée** (une fausse entrée tronque une vraie
  fonction → miscompile) → régression complète obligatoire, `has_callee_pops`-style
  gating pour rester byte-identique sur les binaires non concernés.

---

## 8. Oracles différentiels (le cœur de la garantie)

| Oracle | Ce qu'il prouve | Commande |
|--------|-----------------|----------|
| **cpudiff** (Unicorn, per-instruction) | justesse d'**une instruction** liftée (registres+flags+mémoire) sur milliers d'états | `cargo test --features unpack cpudiff` |
| **cpudiff-séquences** (Unicorn, blocs 2-3 insns) | **composition** (ordre SSA, snapshot esp, aliasing) — curatée + **générative** (4000 blocs aléatoires, 0 div) | idem |
| **funcdiff-closure** (Unicorn, fonction) | lift **brut** d'une fonction + ses callees directs | `bench/funcdiff.sh` |
| **funcdiff opt-diff** (post-opt vs pré-opt) | SSA + passes d'**opt** préservent la sémantique | idem |
| **difftest** (natif, décompile O0→O3) | pipeline A | `bench/difftest.sh` |
| **difftest_transpile** (natif -m32) | pipeline B = **produit** (hash) | `bench/difftest_transpile.sh` |
| **winediff** (Wine) | couverture **OS-API** bit-à-bit | `bench/winediff.sh` |
| **DIB-hash** (Wine) | primitive **GDI** exacte (DIB mémoire hashé) | fixtures `gdi_*` dans winediff |
| **écran virtuel** (Xvfb+SDL vs Wine) | **pipeline fenêtre→GDI→SDL** compose/affiche (qualitatif, cf. §7) | `import -window root` sur `:99` |
| **ORACLE WINDOWS** (GitHub Actions, `windows-latest`) | le **vrai** contrat Win32 — casse la circularité « Wine vérifié contre Wine » (§1). **Pas une porte** : produit des mesures | `.github/workflows/windows-oracle.yml` + `bench/winoracle/wine_hashes.sh` |
| **sweeps** (Wine, vrais binaires) | fonctionnel bout-en-bout | `sqlite_sweep`/`busybox_sweep`/`gauntlet/score` |
| **Z3/SMT** | équivalence de réécritures | `bench/smt_rewrites.sh` |

**Invariant** : chemin non modélisé ⇒ l'oracle **skippe** (jamais un faux verdict).
Une divergence = un bug **prouvé** ; une correspondance = une correction **prouvée**.

---

## 9. Limite dure (honnêteté)

Le trio **« tout binaire + 100 % fonctionnel + 100 % natif pur »** est **prouvé
impossible** en toute généralité (indécidabilité, famille de l'arrêt). Ce qu'ARET
atteint réellement : **vrai logiciel compilé → pleinement fonctionnel** ; le résidu
théoriquement impossible (obfusqué / fait main) **se signale** (abort), il ne ment
jamais. La finalité et les garanties sont **inchangées** ; la réutilisation vérifiée
ne change que la **vitesse** pour y arriver.

> **En une phrase.** ARET = une **couche de vérification + soundness** au-dessus de
> briques (maison ou réutilisées) qu'il **prouve** correctes ou **rejette
> bruyamment**. Binaire Windows → ELF/WASM natif, vraie logique, directement, même
> soundness, même vérification.
