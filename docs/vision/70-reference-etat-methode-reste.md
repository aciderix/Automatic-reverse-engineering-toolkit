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
| **80-orientations-architecturales** | **Design des grands chantiers à venir** : fibers (threads), lifting DLL binaire, SEH in-HLE, PGL, SoftFloat — verdicts + conformité au principe sacré | Avant d'engager threads/DLL/SEH/indirects/x87-universel |
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
bash bench/winediff.sh              # axe 2 vs Wine (114/114)
bash bench/funcdiff.sh              # lift-closure + opt-diff vs Unicorn (0 div)
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
```

### État régression (référence — doit rester vert)
difftest **272/272** · transpile-diff **4/4** (H=`19acad982194bf07`) · winediff
**114/114** · cpudiff vert (per-instruction + séquences génératives) · funcdiff corpus **0 divergence** (lift **~19,8k** scorées /
**~20k appels** — **fonctions à-imports (stubs `@N` prouvés) ET appels indirects résolus** incluses, opt ~10k scorées) · SMT **11/11** · in-place **3/3** · magicdiv **2³²** ·
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
  construction, bornée → `__builtin_trap` sur under/overflow). Gaté transpile-only,
  purement additif. Couvre load/store/const/arith/fxch/fabs/fchs/fsqrt/frndint/
  fldcw + **compare `fcom/fucom st(i)`** (fix récent : lire `op_count()-1`, pas ST0)
  + **les transcendantales brutes** `fsin/fcos/fptan/fpatan/fyl2x/f2xm1/fscale/fsincos`
  (`__x87rt_2xm1`/… ) — **8/8 vérifiées bit-identiques à Wine** (2026-07-10, fixture
  inline-asm `winecorpus/x87_transcendental.c`, non host-backée ⇒ prouve le filet).
- **Transcendantes = libm host-backed** (pow/sin/cos/exp/log/fmod/atan2… via
  `crt_symbol`/nom/FLIRT) → on branche la vraie libm au lieu de lifter du x87 dense.
  Cause racine du double-`sin` corrigée (helpers effacent **C2**).

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
- **tail-`jmp [import]`/`jmp reg`** reçoit **esp+4** (l'adresse de retour est encore
  sur la pile — TLS/Fls/encoded-ptr, l_alloc→realloc).
- **stdcall pop sur `call reg`** (import chargé en registre puis appelé).
- **Helpers ABI MSVC à réécriture de frame** : `_EH_prolog` **inliné** au site
  d'appel ; `_chkstk`/`_alloca` modélisés `esp -= eax` (détectés par `xchg esp,eax`).
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
  (SysAllocString, CoInitialize/CoTaskMemAlloc), temp-fichiers, SetEndOfFile/
  SetFileTime, PeekNamedPipe (FIONREAD), GetThreadLocale (en-US 0x0409), TEB/PEB
  (ProcessParameters), VirtualQuery, LockFile.
- **kernel32 divers (forte largeur corpus Win95)** : `GetVersion` (forme packée, cohérente avec
  GetVersionEx 6.2.9200 NT), `DosDateTimeToFileTime` (FAT→FILETIME, jours civils portables),
  `RtlMoveMemory`=memmove. Gardés par `winecorpus/win32_version_dostime.c`.
  `GetExitCodeProcess` (STILL_ACTIVE), `GetDiskFreeSpaceA` (statvfs), `SetFileAttributesA/W`
  (READONLY↔chmod). Gardés par `winecorpus/win32_file_process.c`. **RtlUnwind = froid** (SEH,
  jamais atteint hors propagation d'exception → abort sound suffit ; tier EH avec la GUI).
- **USER32 message-only** (sans pixels, portable/WASM) : `RegisterClassW`/`Unregister`,
  `CreateWindowExW`/`DestroyWindow`, `DefWindowProcW`, `Get`/`Peek`/`Dispatch`/`Translate`/
  `Post`/`SendMessageW`, `PostQuitMessage`, `SetTimer`/`KillTimer`, `MsgWaitForMultipleObjectsEx` **+ jumeaux A** (M7 G1, doc 72).
  Registre de classes + file de messages mono-thread + timers ; dispatch = **callback WNDPROC
  dans le lifté** (`aret_call`). Débloque le notifier Tcl. Cf. §5 P6.5.
- **MessageBoxA/W** (M7 G5a, doc 72, **display-free**) : renvoie **-1** (repli sound = comportement Wine
  **sans écran**, mesuré : pas de blocage, pas de bouton deviné). Un vrai dialogue (SDL) arrivera avec G2b.
  Gardé `winecorpus/user32_messagebox` (marqueur `.nodisplay` → oracle Wine-sans-écran).
- **Dialogs** (M7 G5b, doc 72, **display-free**) : `DialogBoxParamA/W`/`CreateDialogParamA/W` parsent le
  **DLGTEMPLATE(EX)** → contrôles enfants (réutilise la table de fenêtres, champ `ctrl_id`) + **pompe modale**
  (WM_INITDIALOG → DLGPROC via `aret_call`) ; `EndDialog`, `GetDlgItem`/`GetDlgCtrlID`, `Set`/`GetDlgItemText A/W`,
  `Set`/`GetDlgItemInt`, `SendDlgItemMessageA/W`. DLGPROC qui n'`EndDialog` pas headless → **abort sound**.
  Gardé `winecorpus/user32_dialog.{c,rc}` (bit-identique Wine sous Xvfb).
- **GDI de base** (M7 G6, doc 72) : table d'objets GDI (DC/bitmap/brush/pen/font, handles opaques) + **dessin
  DIB mémoire bit-exact** — `CreateDIBSection`(32bpp)/`CreateCompatibleDC`/`SelectObject`/`DeleteObject`,
  `SetPixel`/`GetPixel`/`FillRect`/`PatBlt`/`BitBlt`(SRCCOPY), `CreateSolidBrush`/`Pen`, `GetStockObject`/
  `GetSysColor`/`GetDeviceCaps` (métriques par invariant), `GetDC`/`ReleaseDC`/`BeginPaint`/`EndPaint`. Cible
  vérifiée = un **DIB qu'on possède** (COLORREF↔`[B,G,R,0]`) → oracle = **hash du framebuffer** vs Wine. Hors
  périmètre (abort sound) : <32bpp. Gardé `winecorpus/gdi_dib.c`.
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
- **USER32 menus** (M7 G7, doc 72, display-free) : modèle de données (items id/flags/submenu/texte) →
  `CreateMenu`/`CreatePopupMenu`/`AppendMenuA/W`/`InsertMenuA`/`Delete`/`Remove`, `EnableMenuItem`/`CheckMenuItem`
  (renvoient l'ancien état), `GetMenuState`/`GetMenuStringA/W`/`GetMenuItemCount`/`GetSubMenu`, `GetMenu`/`SetMenu`/
  `GetSystemMenu` (SC_* par fenêtre), `TrackPopupMenu`→0 sound. Gardé `user32_menu.c`.
- **USER32 helpers fenêtre** (M7 G7, doc 72) : `GetClientRect`/`AdjustWindowRect(Ex)` (modèle no-NC),
  focus/activation (`Set`/`GetFocus`/`ActiveWindow`/`ForegroundWindow`/`BringWindowToTop`), `InvalidateRect`/
  `ValidateRect`/… (no-op sound), `MessageBeep`, `CallWindowProcA/W` (appelle un wndproc lifté), `LoadCursorA/W`/
  `LoadIconA/W` (handles opaques), `MsgWaitForMultipleObjects`. Mesuré bit-identique Wine (`user32_helpers.c`).
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
| **M7** | **GUI / graphisme** (USER32/GDI via **SDL2** portable, puis DXVK/vkd3d) | applis fenêtrées, puis **jeux** | 🚧 **plan doc 72** — **couche USER32/GDI display-free quasi complète** : fenêtres/classes/messages (A+W), modèle fenêtre étendu, ressources/LoadString, MessageBox, **dialogs (DLGTEMPLATE+modal)**, **GDI DIB bit-exact**, menus, helpers, SID/token, rect/char/…, **+ fenêtre SDL VISIBLE (G2b : `SDL_Window`+présentation framebuffer+pompe `SDL_PollEvent`)** **+ GDI texte FreeType bit-identique Wine (G3, autonome)** **+ GDI vectoriel/raster complet (G6 : lignes/Rectangle/Polyline(To)/FrameRect/InvertRect/BitBlt-ROPs)** (winediff **102/102**). **Reste** : widgets natifs (BUTTON/EDIT), Ellipse/courbes (niveau-recherche), + hors-GUI : **threads coopératifs** (fibers, incrément 1 fait — §4.7), **EH/RtlUnwind** |

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
- **Le différentiel *large* attrape les faux silencieux** que les tests étroits
  masquent (`ceil(3.0)` cache `ceil(3.2)` ; `sin(0)` cache `sin(sin(1))`). Toujours
  balayer une grille, pas un point.
- **Un vrai binaire lancé bout-en-bout vs Wine** est le meilleur révélateur de « où
  on en est » — il sort des bugs généraux qu'aucun test synthétique ni sweep
  statique ne révèle (printf %I64, cluster stdin busybox…). Systématisé en sweeps.
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
