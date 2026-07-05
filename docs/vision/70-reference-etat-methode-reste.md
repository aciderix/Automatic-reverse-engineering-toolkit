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
| **HANDOFF / 40** | Architecture détaillée, pièges, état des lieux | Complément |

**Comment un agent trouve une info** : `grep` le tag de sous-système dans **71**
(tags définis en tête du 71 : `[X87]`, `[ABI]`, `[RECOV]`, `[HLE-STDIO]`,
`[HLE-FILE]`, `[HLE-WIN32]`, `[LIFT]`, `[ORACLE]`, `[RECOMPILE]`, `[64BIT]`,
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
`ebp`/`last-error`/TEB sont **globaux** ⇒ modèle **mono-thread** aujourd'hui.

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
bash bench/winediff.sh              # axe 2 vs Wine (47/47)
bash bench/funcdiff.sh              # lift-closure + opt-diff vs Unicorn (0 div)
# Sweeps de vrais binaires (téléchargent + comparent à Wine) :
bash bench/sqlite_sweep.sh   bash bench/busybox_sweep.sh   bash bench/corpus_sweep.sh
bash bench/gauntlet/score.sh        # 21 binaires gauntlet, corpus dans le repo

# Produit :
aret <exe> --mode transpile --out-dir OUT [--entry main|0xADDR] [--function NAME]
     [--run -- ARGS…] [--backend llvm | --target wasm] [--strict]
aret <exe> --mode imports           # couverture statique d'imports (axe 2 a-priori)
```

### État régression (référence — doit rester vert)
difftest **271/271** · transpile-diff **4/4** (H=`19acad982194bf07`) · winediff
**47/47** · cpudiff vert · funcdiff corpus **0 divergence** (lift ~12k scorées /
~6k appels, opt ~10k scorées) · SMT **11/11** · in-place **3/3** · magicdiv **2³²** ·
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
  (pcmpeqb/pmovmskb/pshuflw…).
- **Instructions de chaîne** : `movs/stos/lods/scas/cmps` (non-rep) + `rep movs/
  stos/scas` (helpers `__rep_*`). DF=0 assumé ; `std`/DF arrière + `rep cmps/lods`
  = **abort sound**.
- **Divers** : `cpuid`/`xgetbv` (host réel, **AVX/SSE4.2 masqués** → chemins SSE2
  liftables), `bt/bts/btr/btc` (reg + `[mem],imm`), `stmxcsr`/`ldmxcsr`,
  `cmov/setcc`, `xchg` high-byte.
- **Émission (backend) — `imul` 1-opérande signé × opérande constant** : le magic
  d'une division/modulo signé par constante (bit 31 set, ex. `%23`→`0xb21642c9`)
  arrive sous `SignExtend` comme `Const` nue (opt folde `const & 0xffffffff`) ;
  `signed_cast`/`emit_sign_extend` la sign-étendent depuis `int_bits(ty)` (sinon
  zéro-étendue → `mulhs` faux → `%N` faux). Invisible à cpudiff (bug d'**émission**,
  pas d'IR). Débloqué sqlite3 **mingw**. Garde `signed_magicdiv.c`.

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
  fldcw + **compare `fcom/fucom st(i)`** (fix récent : lire `op_count()-1`, pas ST0).
- **Transcendantes = libm host-backed** (pow/sin/cos/exp/log/fmod/atan2… via
  `crt_symbol`/nom/FLIRT) → on branche la vraie libm au lieu de lifter du x87 dense.
  Cause racine du double-`sin` corrigée (helpers effacent **C2**).

### 4.3 ABI / frame / appels
- **Callee-pop `ret N`** modélisé : **imports** (`stdcall_pops.rs`) **et** fonctions
  **internes** (`compute_callee_pops`, direct = N constant, indirect = table runtime
  `__aret_callee_pop`, gardé par `has_callee_pops` → 0 régression cdecl). *(fix
  cksum : la dérive esp −4/itér des drivers FAST_FUNC/stdcall.)*
- **tail-`jmp [import]`/`jmp reg`** reçoit **esp+4** (l'adresse de retour est encore
  sur la pile — TLS/Fls/encoded-ptr, l_alloc→realloc).
- **stdcall pop sur `call reg`** (import chargé en registre puis appelé).
- **Helpers ABI MSVC à réécriture de frame** : `_EH_prolog` **inliné** au site
  d'appel ; `_chkstk`/`_alloca` modélisés `esp -= eax` (détectés par `xchg esp,eax`).
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
  (`abs_store_imm`), **`call/jmp [disp32]`** (`abs_indirect_slot`, contenu du slot),
  **`call [idx*4+base]`** (`indexed_call_table_base`, tables init/atexit NASM).
- **Tables de saut** : bornées par `cmp idx,N;ja` ; doublons préservés (cases
  partagés) ; abs computed-goto ; forme -O0 étagée ; tables de pointeurs
  **NULL-tolérantes** ; **run ≥3× d'une même valeur = switch, pas vtable**.
- **Re-split** : une fonction absorbée après un appel *noreturn* (pas d'analyse
  noreturn au balayage) est **forcée** frontière quand une table de pointeurs/index
  la pointe. `compute_noreturn` = point-fixe **sound** (jamais deviné noreturn).
- **x87 leaf-thunk** (`is_x87_leaf_thunk`) : décode tout le corps (fld arg → ops FPU
  → ret) → amorce atan2/fmod/trunc atteints par pointeur isolé.
- **FLIRT** : opérandes **relocalisés wildcardés** (`.reloc`) ; **thunks jamais
  signaturés** (résolus structurellement) ; glue reconnue = `looks_like_func_start`.
  ⚠️ FLIRT est **cosmétique** pour nos cibles (reconnaît du code de biblio, pas le
  code propre ; sensible à la version) — **le levier réel est le lifter**.

### 4.5 Axe 2 — HLE (couverture OS/CRT/Win32, vérifié vs Wine)
- **stdio** : **tous les FILE** = struct **msvcrt-layout (32 o) fd-backed non
  bufferisés** → getc/putc **inlinés** défèrent à `_filbuf`/`_flsbuf` (read/write
  1 octet). `_iob` et fichiers unifiés. ungetc pushback. `close(std)` fidèle
  (idiome close+réouverture uniq/tac), `isatty` sans fuite ENOTTY.
- **Fichier** : open/read/write/close/lseek/**_lseeki64**/_telli64, famille **stat
  msvcrt ABI-exacte** (`_stat`/`_fstat`/`_stati64`, offsets d'octets explicites),
  `_access`/`_chmod`/`_mkdir`/`_unlink`, mapping mémoire (`CreateFileMapping`/
  `MapViewOfFile` → mmap, `#ifndef __wasm__`), **wide** (`_wfopen`/`CreateFileW`/
  `GetFileAttributesExW`/`GetFullPathNameW`…). **Chemins Unix absolus `/…`
  passent au vrai FS** ; seuls les chemins Windows gardent le préfixe.
- **CRT** : printf/scanf complets + **`%I64`/`%I32`** MSVC, `snprintf` C99,
  strtoll/strtoull/div/ldiv (retour **edx:eax** via `import_returns_u64`), `atexit`
  (via `_onexit`), setjmp/longjmp, `_getcwd`/`_chdir`/`_fullpath`, rand LCG msvcrt,
  gmtime/localtime/mktime/strftime (struct tm Windows).
- **Win32** : console/TTY (GetConsoleMode/SetConsoleMode/GetFileType), Tls, locale/
  codepage (GetACP/GetStringTypeW/LCMapStringW/MultiByte↔Wide), heap/module,
  process/thread **partiel** (`CreatePipe`=pipe() fidèle ; CreateThread/CreateProcess
  = **échec sound**, pas simulé), Find\*File (opendir+fnmatch), env, temps figé,
  Interlocked, **version-info** (VS_VERSIONINFO parsé), **BSTR/COM minimal**
  (SysAllocString, CoInitialize/CoTaskMemAlloc), temp-fichiers, SetEndOfFile/
  SetFileTime, PeekNamedPipe (FIONREAD), GetThreadLocale (en-US 0x0409), TEB/PEB
  (ProcessParameters), VirtualQuery, LockFile.

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
  bin` = objets **bit-identiques à Wine**.
- **busybox-w32** (mingw strippé) : cksum/md5sum/sha1sum/echo/sort/wc/cat/head/tail/
  uniq/tac/od/nl/cut/rev/expr/**awk** (÷)/seq/basename/tr/pwd/… bit-identiques ;
  sweep **60/60**.
- **WASM** : PE Windows → WebAssembly, **7/7** fixtures (pile, globals, indirects,
  CRT, x87, Win32, SHA-256).
- **Corpus gauntlet** (`bench/gauntlet/`, 21 PE variés committés) : **12/21**
  bit-identiques (bzip2/grep/gzip/hello/lua/minigzip/nasm/sed + strippés).

---

## 5. Ce qui RESTE — précis, ordonné par valeur × sûreté

> Signal stratégique (2026-07-04) : les **victoires générales faciles** (shims,
> stdcall_pops, récup simple) sont **quasi épuisées**. Ce qui reste = un ensemble
> **borné** de problèmes **profonds**, chacun ≈ une session dédiée de forensics.
> On passe de « largeur de shims » à « profondeur lifter ». Fini, mais plus lent.

### P1 — sqlite3 mingw (levier fort, profond) 🎯 *en cours*
- **1er bug RÉSOLU (2026-07-05)** : le crash `SELECT` était un bug d'**émission**
  (`imul` 1-op signé × const magic zéro-étendu → `% 23` négatif → OOB). Corrigé
  (cf. §4.1 + 71 `[LIFT][RECOMPILE]`). sqlite3 mingw scalaire = **bit-identique à Wine**.
- **RESTE (prochaine cible)** : le **CRUD** (`CREATE TABLE`/`INSERT`) segfaulte
  encore — bug **distinct plus profond**. `sub_429330=sqlite3ExprAffinity` deref un
  `Expr*` null, via `sqlite3Select→findConstInWhere→constInsert` (optimisation WHERE
  const-propagation). Repro : `bench/gauntlet/` (`/tmp/g/sqlite3.exe`), méthode C
  `-O0 -g` + gdb + watchpoint (voir 71 pour le workflow qui a cracké le 1er bug).

### P2 — Robustesse x87 : réconciliation des joins ambigus (session dédiée)
La **vraie difficulté récurrente** (Lua `intarith`/`forprep`, busybox `seq`, le
join libm de `awk` `0x429129`). Quand un bloc est atteint à deux profondeurs de
pile x87 différentes, la passe **abandonne toute la fonction**. À faire :
1. **Suivre les valeurs conservées** par `fstp st(i)`/`fxch` dans les idiomes de
   comparaison NaN (le vrai bug n'est pas un « join » mais une profondeur d'entrée
   de bloc mal propagée après `fstp st(1)`).
2. Intégrer le fix **fp-returning auto-récursif** (retry en supposant `f ∈ fp` si
   le pass bail ; accepter si tous les rets restent à profondeur 1) — **prouvé
   correct, réutilisable**, mais à ne shipper qu'avec bénéfice mesuré.
3. Soit **modéliser les transcendantales x87** (`fldl2e`/`f2xm1`/`fscale`/`fyl2x`/
   `fsin`/`fcos`, précision 80-bit, correctness-sensible), soit — **recommandé,
   esprit UBT** — **reconnaître + host-backer** les exp/log/pow libm statiques par
   signature d'idiome x87 (FLIRT ne les voit pas ici).
Délicat : **une fonction à la fois, difftest + cpudiff + winediff + filet Lua à
chaque pas.**

### P3 — Récupération points-to (Phase 4 vtables / dispatch calculé)
- **NASM `-f obj` (OMF)** : abort sur un stub `ret` nu (méthode no-op d'un `struct
  ofmt`) stocké en immédiat et appelé par **adresse calculée/indexée** (ni immédiat
  simple, ni `call [slot]`, ni run≥3) — le cas de récup **le plus dur**.
- **plink** (MSVC/clang) : abort sur `0x450058`, même classe (pointeur isolé atteint
  par adresse calculée).
- Vrai **C++ g++** (exceptions, RTTI, thiscall) : non testable sur l'hôte (pas de
  mingw g++) ; le **dispatch vtable lui-même fonctionne** (fixture validée).

### P4 — busybox regex (grep/sed) : miscompile profond derrière `malloc`
SIGSEGV dans le moteur regex lifté (`sub_42f6d4` : `mov (eax),eax`, base≈0 = store
d'init droppé). funcdiff-closure suit déjà 6000 appels **sans divergence** → le bug
est **derrière un import** (`malloc`, non franchi par la closure). Frontière
suivante : **modéliser quelques imports purs** (`memcpy`/`memset`/`memcmp`/`strlen`
— *déjà partiellement fait pour memcpy/rep-stos*) dans l'interpréteur funcdiff pour
scorer la logique applicative (sain, mais un import mal modélisé = faux positif).

### P5 — m4 (mingw) : abort au démarrage (locale/CRT plus profond). units cherche
`units.dat` (**environnemental**, pas un bug).

### P6 — Outillage funcdiff : closure SSA (opt-diff à travers les appels)
Tentée, **retirée** (faux positif : `esp` fantôme incohérent à la frontière
run_ssa↔run_closure). À reprendre en **threadant tout l'état CPU** (GP+flags+xmm+esp)
proprement au call, testé par la garde opt + teeth-check avant de croire un verdict.

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
| **M7** | **GUI / graphisme** (X11/USER32, puis DXVK/vkd3d) | applis fenêtrées, puis **jeux** | ⬜ |

> **Règle** : on ne s'engage pas sur M_n+1 tant que M_n ne tourne pas proprement ;
> chaque palier = un artefact démontrable + un test de non-régression.

### 8.2 Phase 6 — Inférence de types (LISIBILITÉ + JUSTESSE)
Largeur/signe/pointeur à partir de l'usage des registres, puis agrégats
(`obj->field_8`). *Critère* : types affichés, **jamais** au prix de la sémantique
(casts explicites conservés). Non bloquant pour l'exécution ; améliore la lisibilité
du C généré et peut aider les autres passes.

### 8.3 Multithreading (chantier dédié — modèle actuel mono-thread)
**Blocage de fond** : `ebp`/`last-error`/TEB/PEB sont **globaux**, la pile machine
est **une région unique partagée** ⇒ modèle **fondamentalement mono-thread**
aujourd'hui. Les registres sont des variables C locales (thread-safe), mais l'état
partagé ne l'est pas. Chemin **clair et mesuré** (à faire dans l'ordre) :
1. **TEB + last-error thread-locaux** (`__thread`) — la fondation.
2. **Pile machine par thread** dans `CreateThread` (malloc 32-bit, `__esp` initial au
   sommet) + dispatch `aret_call(startAddr, esp, param)` (ABI stdcall du thread-proc).
3. **Sync réelle** : `CRITICAL_SECTION` → `pthread_mutex` **récursif** ;
   `CreateEvent`/`SetEvent`/`WaitForSingleObject`/`WaitForMultipleObjects` →
   `pthread` cond/join. (Aujourd'hui : CriticalSection = no-op *correct sans
   concurrence*, WaitForSingleObject = WAIT_OBJECT_0 immédiat — **sound en
   mono-thread**, à remplacer par du réel en MT.)
4. **Validation MT vs Wine** : N threads + compteur sous section critique → somme
   déterministe ; signalisation d'événement → déterministe.
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
  `pypi.org`. Cross-compile : `i686-w64-mingw32-gcc` (mingw). Pas de `mingw g++`
  ni de toolchain MSVC → binaires MSVC **téléchargés prébuild** (nasm.us, sqlite.org
  précompilés). `winetest.exe` (WineHQ) bundle ~367 modules de conformance.
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
