# HANDOFF — Consignes pour l'agent successeur (ARET)

> **Lis ce document EN ENTIER avant de coder.** Il contient l'objectif, la
> méthodologie, l'architecture réelle, les outils, l'état d'avancement honnête,
> les pièges connus et les prochaines étapes. Lis ensuite `ROADMAP.md` (le plan
> technique détaillé) et `README.md` (vue utilisateur).
>
> 🧭 **Objectif « convertir un binaire en natif d'un autre système / WASM » :**
> l'état des lieux consolidé (acquis / perfectible / manque) est dans
> **[`docs/vision/40-etat-des-lieux-conversion.md`](docs/vision/40-etat-des-lieux-conversion.md)**.
> Ce HANDOFF couvre la **couche 1** (décompilateur vérifié) ; le doc 40 couvre la
> **couche 2** (transpileur natif/WASM) qui s'appuie dessus.

---

## 1. Objectif du projet

ARET (Automatic Reverse Engineering Toolkit) : un pipeline Rust qui prend
**n'importe quel binaire** (PE/ELF/Mach-O, x86/x64) et le retraduit en **vrai
code C**, en visant **plus loin que Ghidra/Hex-Rays/LLM4Decompile** grâce à une
**boucle de vérification fermée** (décompile → recompile → prouve l'équivalence).

### Scope honnête (à ne jamais trahir)
- **La compilation est destructrice** : noms de variables/fonctions, commentaires,
  types sémantiques, structure des fichiers/templates sont *perdus*. On ne les
  *récupère* pas, on les *réinvente* au mieux (couche LLM, §9). Ne jamais
  promettre la récupération du source d'origine.
- **Ce qui EST récupérable** (et qu'on vise à 100 %) : la sémantique exacte, la
  structure de contrôle, les bornes de types (largeur/signe/ptr), les signatures.

### Principe directeur ABSOLU
**Jamais de sortie incorrecte présentée comme correcte.** Tout ce qui n'est pas
sûr reste en `Stmt::Asm` / `__asm__` ou est marqué. La sûreté prime sur la
beauté. En cas de doute → ne pas transformer. Ce principe a guidé chaque
décision (ex : promotion SSA différée faute d'analyse d'alias — cf. §6).

### Métrique nord  *(MàJ 2026-06-15, mesurée)*
% de fonctions dont le C émis (a) **recompile** et (b) est **prouvé équivalent**
au binaire (différentiel ou SMT). Niveaux **tous opérationnels** :
1. **Recompile** — `--mode verify` : **100 %** sur gzip (135/135), ls (226/226),
   cat (73/73) + jeu.
2. **Différentiel** (exécution, ~50k–500k entrées, -O0 → -O3) — `bench/difftest.sh`
   : **264/264** (pointeurs/tableaux/boucles/chaînes/SSE scalaire+packé simple &
   double/**x87 80-bit**/div64/rotates/imul/appels indirects/**chaînes adc-sbb**/
   **atomics xadd-cmpxchg-xchg**/shld-shrd…) ; `bench/inplace.sh` : **3/3** ;
   `bench/magicdiv.sh` : équivalence **exhaustive 2^32** de la magic-division.
3. **SMT formel** (Z3) — `bench/smt_rewrites.sh` : **11/11** règles d'opt prouvées.

**Incomplétude — binaires système** : **3** (gzip 1, ls 1, cat 1) = `_start`
(stub d'entrée ELF `call __libc_start_main; hlt` ; `hlt` privilégié, pas de C →
INCOMPLETE est correct). Tombé de ~300 → 26 → 3 ; tout x87, SSE packé, rotates,
imul, rep, tail-calls/appels indirects, CFG (prologue, jump-tables ordre/inter-
blocs/borne) corrigés.

**Test sur vrai binaire (le jeu PE32 32-bit C++, 26 Mo, ~44k fn)** : recompile
**100 %** ; incomplétude ~5900 → **~4370 / 44 183 (≈90 % entièrement modélisées)**
cette session, via x87 status-word (`fcom/fnstsw/sahf`), SSE packé simple,
atomics, + **2 vrais bugs corrigés** (arg cdecl 32-bit non déclaré ; retenue
add/adc non width-aware — bugs latents non couverts par le corpus). Le reste est
le **plancher honnête de cet environnement** : `pushad/pushfd/lahf` (32-bit only
→ non assemblables/exécutables ici pour valider ; et `pushfd`/`lahf` liraient les
flags bruts SF/AF/PF que ce pipeline n'approxime que pour reconstruire des
*conditions* → non sûrs sans refonte du modèle de flags) + x87 transcendantes
(fsin/fcos non bit-exactes). Les laisser INCOMPLETE est correct (jamais faux).

---

## 2. ⚠️ ARCHITECTURE : IL Y A DEUX PIPELINES (point le plus important)

C'est le piège n°1 pour un successeur. Bien le comprendre.

### Pipeline A — TEXTE (sortie par défaut, `--mode decompile`)
`loader → disasm → analysis(CFG) → ir::lift_insn (TEXTE) → dataflow (TEXTE) →
structure (TEXTE) / decompile`
- Produit le pseudo-C lisible (les 44k fichiers `.c` du jeu).
- A : structuration if/while, frame vars (`arg_8`/`local_4`), conditions
  signées/non, **chaînes**, **switch/jump tables**, **imports nommés**, args cdecl.
- **Manipule des chaînes de C** (`ir::lift_insn` → `Vec<String>`, `dataflow`
  re-parse ce texte). C'est le **plafond architectural** dénoncé au §1.2 du
  roadmap : pas de vraie inférence de types ni de propagation inter-jonctions.
- **N'est PAS vérifié** (pseudo-C, ne recompile pas tel quel : `eax`, `int64_t`...).

### Pipeline B — IR SSA TYPÉ (`--mode ir` / `--mode emit` / `--mode verify`)
`loader → disasm → analysis(CFG) → ir::build::build_ir (IR TYPÉ) → ssa::to_ssa →
opt::optimize → emit / emit::structured`
- IR en arbres d'expressions + SSA (`src/ir/types.rs`).
- Produit du **C compilable** et **vérifié** (recompile 100 %, différentiel,
  SMT). C'est la voie d'avenir (roadmap §3.5).
- Moins riche en surface aujourd'hui (variables `vN`, pas encore de types
  fins/structs) MAIS sémantiquement sain et prouvé.

### La migration (roadmap §3.5) — état
Le plan est de **basculer le défaut sur le pipeline B** quand il atteint la
parité de lisibilité (types §5 + meilleures signatures). **Pas encore fait.**
Les deux coexistent. Quand tu améliores la lisibilité, demande-toi : « pipeline
texte (lisible, jeté) ou pipeline IR (vérifié, futur défaut) ? ». Préfère
investir dans **B** sauf gain rapide et sûr pour l'utilisateur via A.

---

## 3. Structure du code (`src/`, ~7000 lignes)

| Module | Rôle | Pipeline |
|---|---|---|
| `loader/mod.rs` | Parse PE/ELF/Mach-O (`object`), sections, symboles, **imports PE (IAT) + ELF (PLT/GOT)**, `read_cstring`, `read_u32/u64` | commun |
| `disasm/mod.rs` | Décode x86/x64 (`iced-x86`), classifie le flot (`Flow`) | commun |
| `analysis/mod.rs` | Decode global unique (scalable), découverte fonctions (entry+symboles+calls+**scan prologues**), CFG, **résolution jump-tables** | commun |
| `cfg/dom.rs` | Dominateurs (Cooper-Harvey-Kennedy), post-dom, **frontière de dominance** (Cytron), partagé | commun |
| `ir/mod.rs` | **lift TEXTE** (`lift_insn`→`Vec<String>`), conditions depuis flags (regex), frame vars, `call_name` (imports) | A |
| `decompile/mod.rs` | Émetteur texte plat (`--flat`), `block_statements`, `annotate_strings` | A |
| `dataflow/mod.rs` | Sur le texte : liveness, DCE, propagation, arg cdecl, binding retour — **le plus gros module, manipule des strings** | A |
| `structure/mod.rs` | Structureur TEXTE (if/while/switch, goto fallback) = **sortie par défaut** | A |
| `ir/types.rs` | **IR typé SSA** : `Expr`/`Stmt`/`Ty`/`Location`/`ValueId`/`FlagKind`/`Select` | B |
| `ir/lift.rs` | **lift IR** via API structurée iced (registres/flags/mémoire), `Frame` slots, `asm_fallback` sain, `cc_to_cond` | B |
| `ir/build.rs` | Construit l'IR-CFG depuis `analysis::Function` + terminateurs ; `dump` (`--mode ir`) | B |
| `ssa/mod.rs` | Construction SSA (Cytron) : φ + renommage ; récup args registres 64-bit | B |
| `opt/mod.rs` | Passes SSA : const-prop, **folding/simplif algébrique** (reconstruit conditions signées/non), DCE, propagation mono-usage + chaînes, `prune_call_args` (ôte args `Undef`), **magic division** (`magicu32`/`try_magic_udiv`, auto-validée) | B |
| `emit/mod.rs` | IR→C **compilable** (goto), destruction SSA (φ→copies), signatures (args), `signed_cast`, `fixup_call_arity` (aligne appels sur l'arité callee + arités libc) | B |
| `emit/structured.rs` | IR→C **structuré** (if/while), réutilise `cfg::dom` | B |
| `verify/mod.rs` | Harness recompilabilité (niveau 1), `--mode verify` | B |
| `main.rs` | CLI (modes : info/asm/cfg/decompile/ir/emit/verify ; `--flat`/`--split`/`--function`/`--limit`/`--no-prologue-scan`) | — |

`bench/` : `corpus.c` (fonctions de test ; ~50), `difftest.sh` (niveau 2,
-O0→-O3), `inplace.sh` (**différentiel in-place** : mappe les PT_LOAD à leur VA
pour valider les accès par adresse absolue — globals/tables), `magicdiv.sh`
(équivalence exhaustive 2^32 magic-division), `smt_rewrites.sh` (niveau 3 Z3),
`regression.sh` (**porte de non-régression unifiée** : build + tests + tous les
niveaux dont in-place ; `bash bench/regression.sh`).
`.claude/hooks/session-start.sh` : hook SessionStart (web) qui build + assure z3
pour que les benches tournent.

---

## 4. Outils & environnement (TESTÉ)

- **Rust** : `cargo 1.94`. Build : `cargo build --release` (binaire `target/release/aret`).
- **C compiler** : `gcc`/`cc` présents (64-bit). **PAS de 32-bit** (`-m32` échoue,
  libs absentes) → la vérification doit être **64-bit**.
- **Z3** : installable via `pip install z3-solver` → binaire `/usr/local/bin/z3`
  + module Python. Utilisé par `bench/smt_rewrites.sh`.
- **Réseau** : crates.io OK, github raw OK, **pip OK**. `apt` non testé/risqué.
- **PAS d'API LLM** configurée → couche §9 non exécutable ici (seulement échafaudable).
- **Binaires de test** :
  - `/tmp/mq.exe` (ou `/root/.claude/uploads/.../...MightyQuest_unpacked_fixed_1.exe`) :
    le jeu, **PE32 32-bit, 27 Mo, strippé, Steam-DRM dépaqueté**. ~44k fonctions.
    ⚠️ IAT non standard (imports peu résolus — propriété du binaire).
  - `/tmp/demo` : ELF 64-bit (compilé depuis `/tmp/demo.c` : factorial/classify/main).
  - binaires système ELF 64-bit : `/usr/bin/gzip`, `/bin/ls`, `/bin/cat`,
    `/usr/bin/sha256sum`, `/usr/bin/base64` (réels, strippés, libc).

### Commandes utiles
```bash
cargo build --release && cargo test --release        # build + 14 tests unitaires
target/release/aret <bin> --mode info                # format/sections/symboles/imports
target/release/aret <bin> --function <nom|hex>       # une fonction (défaut: structuré texte)
target/release/aret <bin> --mode decompile --split out/   # 1 .c par fonction + index.csv
target/release/aret <bin> --mode ir   --function f   # dump IR SSA optimisé (pipeline B)
target/release/aret <bin> --mode emit --function f   # C compilable structuré (pipeline B)
target/release/aret <bin> --mode verify --limit 500  # taux de recompilabilité (niveau 1)
bash bench/difftest.sh                                # équivalence différentielle (niveau 2)
bash bench/smt_rewrites.sh                            # preuves SMT des règles d'opt (niveau 3)
```
⚠️ Sur le jeu (gros), l'analyse complète prend ~50-75 s par invocation (chaque
`--function` ré-analyse tout). Les binaires système sont quasi instantanés.

---

## 5. État d'avancement par pilier (HONNÊTE)

> **MàJ 2026-06-15** : grosse passe « compléter -O0 → -O3 » + infra de
> validation. ✅ **SSE scalaire** (float/double bit-exact via helpers `__fp_*`,
> XMM = motifs de bits) + **SSE packé 128-bit** (XMM = deux moitiés 64-bit,
> helpers `__pi_*`, `write_xmm128` sans aléa inter-moitié) ; ✅ **garde
> vectorielle** `uses_vector_reg` (toute op XMM/YMM/ZMM non modélisée → `Asm` +
> INCOMPLETE, plus de mis-lifting scalaire silencieux — *le « recompile 100 % »
> masquait des fonctions possiblement fausses*) ; ✅ **entiers étendus** (helpers
> `__ix_*` : `__int128` pour mul/div 64-bit, `sbb`/`adc`, `bt`/`btc`/`bts`/`btr`,
> `bswap`, `bsf`/`bsr`/`tzcnt`/`lzcnt`/`popcnt`) ; ✅ **`rep movs` → `memcpy`** ;
> ✅ **tail calls** (`jmp func` → `return func(args)`, `jmp [GOT]` import →
> `return import(args)`) — *cause dominante d'incomplétude, pas x87 : gzip
> 83→10* ; ✅ **tables de saut PIE relatives** (`lea base,[rip+t]; movsxd
> off=[base+i*4]; add; jmp` — reconnaissance dans l'analyse avec **trace de la
> def atteignante** du registre base inter-blocs → `Stmt::Switch` typé) ;
> ✅ **fusion hot/cold** (compagnons gcc `foo.cold` de `.text.unlikely` fusionnés
> dans le parent) ; ✅ **promotion variables de pile** (le frame base pointe sur
> un vrai tableau local `uint8_t __frame[16384]`, FRAME_TOP=14336 → corrige les
> crashs de pointeurs sauvages sur buffers/tableaux de pile) ; ✅ **harnais
> in-place** (`bench/inplace.sh` : mappe les segments PT_LOAD à leur VA pour
> valider les accès par adresse absolue — globals/tables) ; ✅ relocations `.o`
> appliquées, canaris de pile, idiomes libc. **Résultats mesurés** : différentiel
> **196/196** (-O0→-O3), in-place **3/3**, recompile **100 %**, SMT **11/11**,
> magicdiv **2^32**. Incomplétude réelle ramenée de ~300 → **26** fonctions.
> Prochain : **x87** (modèle pile 80-bit — résout l'essentiel des 26) ou
> **inférence de types** (§5).
>
> **MàJ 2025-06-14** (historique) : ✅ Rayon (analyse 60→17 s) ; ✅ lifter
> `mul`/`div`/`idiv` 1-op + `cdq`/`cqo` ; ✅ modélisation des appels (retour
> `rax=call` + clobbers) ; ✅ imports nommés + `verify -fno-builtin` ; ✅ args
> call-sites 64-bit (`prune_call_args`, `emit::fixup_call_arity`, arités libc) ;
> ✅ **magic division unsigned 32-bit** (vérifiée exhaustivement 2^32) ; ✅ 2 bugs
> (`fs:`/`gs:`, `SignExtend` → cast signé) ; ✅ high-byte/`leave`/`cbw`/`cwde`/`cdqe`.


- **Pilier 1 (IR SSA typé)** : ✅ COMPLET. types, dom+frontière, lift IR, SSA, IR-CFG, `--mode ir`.
- **Pilier 2 (passes SSA)** : ✅ const-prop, folding/simplif, DCE, propagation
  (intra + chaînes droites), reconstruction conditions, binding retour, sûreté `Asm`.
  Manque : GVN, propagation à travers les **vraies jonctions φ** (au-delà des chaînes).
- **Pilier 3 (inférence de types §5)** : ❌ **NON COMMENCÉ**. C'est le prochain
  grand levier de lisibilité (largeur/signe/ptr, **champs de struct** `obj->field_8`,
  tableaux). Treillis + union-find. Risque : un type faux change la sémantique C →
  garder les casts sémantiques explicites, n'inférer que pour l'affichage tant que
  pas prouvé.
- **Pilier 4 (recovery)** :
  - §6.1 switch/jump tables : ✅ + ✅ **`Stmt::Switch` typé dans l'IR** (index
    récupéré) + ✅ **tables relatives PIE 4o x64** (trace de la def atteignante du
    base register inter-blocs) + ✅ **fusion hot/cold** (`foo.cold`).
  - §6.2 conventions d'appel : ⚠️ PARTIEL (frame args + args registres 64-bit ;
    args call-sites 64-bit + fixup `emit::fixup_call_arity` ; ✅ **tail calls**
    directs et via import GOT → `return callee(args)`). Reste : **args 32-bit
    cdecl sur la pile**, matching positionnel non-contigu.
  - §6.3 **vtables / appels indirects C++** : ❌ NON FAIT. ⚠️ **Injustement sauté
    vu que le binaire de test EST un jeu C++** : des milliers d'appels
    `(*(*(uint32_t*)(eax+0xN)))()` (vtables) ne sont pas résolus.
  - §6.4 : ✅ chaînes, ✅ imports PE+ELF, ✅ **relocations statiques `.o`** (lues
    sur les sections de code, cible read-only repliée en littéral). ❌ globals
    nommés, ❌ FLIRT/signatures CRT.
- **Pilier 5 (émission C)** : ✅ compilable + structuré + ✅ **helpers float/SSE/
  entiers/x87** (`__fp_*`/`__pi_*`/`__ix_*`/`__x87_*`, préambule auto) + ✅ **x87
  80-bit** (`long double`) + ✅ **promotion pile** (tableau `__frame` local) +
  ✅ **appels indirects** (cast pointeur-de-fonction). Manque : variables typées
  (dépend §5), mode `--strict`.
- **Pilier 6 (vérification)** : ✅ niveaux 1, 2, 3. Niveau 3 ne prouve pour l'instant
  que les **règles d'opt isolées** (pas des fonctions entières lift→SMT). Manque la
  **boucle de raffinement** automatique.
- **Pilier 7 (LLM §9)** : ❌ NON FAIT (pas d'API ici). À échafauder : prompt depuis
  l'IR typé, patch de noms/commentaires, re-vérification après renommage.
- **Pilier 8 (bench/CI §10)** : ✅ **porte unifiée** `bench/regression.sh` (build +
  tests + niveaux 1/2/3) et **hook SessionStart** `.claude/hooks/session-start.sh`
  (build + z3) en place. Reste : la brancher en CI GitHub Actions sur push.

---

## 6. Décisions importantes & pièges connus

1. **Promotion des slots de pile (§4.1) — FAITE prudemment.** Plutôt que de
   promouvoir chaque slot en variable SSA (qui exigerait une analyse d'alias
   complète — le frame base peut s'échapper sans `lea`), ARET pointe le **frame
   base sur un vrai tableau local** `uint8_t __frame[16384]` (FRAME_TOP=14336),
   émis par `emit::value_decls`. Sain par construction : tout accès relatif au
   frame (y compris adresse prise, indexation, débordement borné) touche de la
   vraie mémoire au lieu d'un pointeur sauvage → corrige les crashs de
   buffers/tableaux de pile sans inférer faussement la sémantique. La promotion
   SSA fine (slot → `vN` scalaire) reste différée et garderait le besoin d'alias.
2. **Symboles à l'adresse 0 sautés** (`loader`) : dans un `.o`/binaire où une
   fonction est à l'offset 0, son symbole est ignoré (→ `bench/corpus.c` a un
   `_pad` en tête pour cette raison). Sur un exécutable réel (base ≠ 0), non
   problématique.
3. **Binaire de test = Steam-DRM dépaqueté** : IAT non standard (`0xdec0xx`)
   distinct du répertoire d'import reconstruit (`0x1d89000`) → peu d'imports
   résolus sur CE binaire (le parser est correct, testé sur gzip : 151 imports).
4. **`Asm` = soupape de sûreté** : une instruction non liftée devient un appel
   opaque de ses entrées + clobber `Undef` des sorties (via `iced instr_info`),
   pour que DCE/opt restent sains. NE PAS casser cette propriété.
5. **`signed_cast` (emit)** : une valeur masquée 32 bits doit être étendue en signe
   `(int64_t)(int32_t)x` (PAS castée comme positive). Bug trouvé par le différentiel,
   prouvé corrigé par SMT. Vaut pour toute nouvelle op signée.
6. **Récursion** : une fonction émise avec params dont l'auto-appel est `sub_x()`
   → corrigé par `fix_self_calls` (remplit de zéros). Garder à l'esprit pour tout
   appel à arité connue.
7. **Le différentiel a trouvé de vrais bugs** (signé, récursion). **Méthodologie
   gagnante : étendre le corpus → les échecs pointent les bugs → corriger →
   re-prouver.** Continue ainsi.
8. **`--function` sur le jeu ré-analyse tout (~60s)**. Pour itérer vite, teste sur
   les binaires système ELF (instantanés) ou `/tmp/demo`.

---

## 7. Méthodologie / workflow

- **Git** : branche `claude/adoring-rubin-hc8mv8`. Commits clairs, descriptifs.
  `git config user.email noreply@anthropic.com && user.name Claude` (sinon le
  hook signale "Unverified"). Push : `git push -u origin claude/adoring-rubin-hc8mv8`
  (retries avec backoff si erreur réseau). **Ne PAS créer de PR** sauf demande.
- **Toujours** : après un changement, `cargo build --release` + `cargo test --release`
  (14 tests doivent passer) + vérifier la non-régression (`factorial`/`classify`
  identiques sur `/tmp/demo`, `--mode verify` toujours 100 % sur gzip).
- **Chaque incrément** : prouvé sûr (tests/différentiel/SMT) avant commit. Mettre
  à jour `ROADMAP.md` (cocher les jalons) et `README.md` si pertinent.
- **Honnêteté** : documenter franchement ce qui ne marche pas / les limites
  (cf. le binaire DRM). Ne jamais maquiller un échec.
- **Mettre à jour ce HANDOFF.md** quand l'état change significativement.

---

## 8. Prochaines étapes recommandées (par valeur/sûreté)

> **Incomplétude — FAIT (~300 → 3).** Tout le gros œuvre est livré et
> différentiel-validé (-O0→-O3) : **x87 80-bit** (`long double`, suivi de pile
> statique, idiome de troncature ; `ir/lift.rs::lift_x87` + `ir/build.rs::
> x87_states`), **SSE packé double** (addpd/unpckpd/shufpd/movh*/movl*), **imul
> 1-op**, **rol/ror**, **xchg**, **rep stos**, **psubusw**, **appels & tail-calls
> indirects** (`jmp reg` → `return (*reg)(args)`), et **toute la récupération de
> CFG** : sur-segmentation de prologue (`collect_function` absorbe les fausses
> entrées atteintes par fall-through), résolution de jump-table en **point fixe**
> après décodage complet (indépendante de l'ordre), **index de switch inter-blocs**
> (`pie_switch_index` scanne au-delà du bloc du `jmp`, pour les tables à 2 niveaux),
> et borne de table exacte (`cmp r64,imm8` = `Immediate8to64`, sinon sur-lecture).
> Les **3 restantes sont `_start`** (stub d'entrée asm, `hlt`) — pas du C, plancher
> absolu. Le zéro littéral n'existe pas : `_start` ne se décompile pas en C.
>
> **Ordre faisant autorité** : voir `ROADMAP.md` §15.4. Résumé : 1) division
> magique → 2) args call-sites → 3) compléter lifter + idiomes (✅ SSE/entiers/
> tail-calls/jump-tables faits) → 4) inférence de types → 5) polir l'émission IR
> puis basculer le défaut → 6) SCCP/GVN, vtables, LLM. Ci-dessous le détail
> historique (toujours valide) :


1. **§6.3 vtables / appels indirects C++** — injustement sauté alors que le test
   est un jeu C++. Résoudre `call [vtable+k]` quand la vtable est en `.rodata`,
   nommer `obj->vtable->method_k(...)`. Gros gain sur CE binaire.
2. **§5 inférence de types** (largeur/signe/ptr puis structs) — le levier majeur
   de lisibilité ; le bug signé montre que les largeurs comptent. Commencer par
   largeur/signe (sûr, n'affecte que l'affichage), puis ptr vs scalaire, puis
   agrégats multi-offset → `obj->field_8`.
3. **§10 CI/bench** — câbler `cargo test` + `difftest.sh` + `smt_rewrites.sh` dans
   un hook (skill `session-start-hook`) pour suivre la métrique et bloquer les
   régressions. Léger, haute valeur.
4. **Index de switch dans l'IR** → `Stmt::Switch` typé (le pipeline texte a déjà
   les switch ; l'IR non).
5. **Niveau-3 étendu** : lever des fonctions entières sans boucle en SMT et prouver
   IR-opt ≡ IR-brut (pas seulement les règles).
6. **§9 LLM** : échafauder le prompt depuis l'IR typé + l'application de patch de
   noms, prêt à brancher dès qu'une API Claude est dispo (modèle récent).
7. **Élargir le lifter** : mul/div 1-op (double largeur), mouvements SSE.
8. **Unifier les pipelines** : une fois B à parité, basculer `--mode decompile`
   par défaut sur l'IR (roadmap §3.5).

---

## 9. Résumé en une phrase
ARET est un décompilateur Rust fonctionnel à **deux pipelines** (texte lisible par
défaut ; IR SSA typé vérifié), avec une **boucle de vérification opérationnelle**
(recompile 100 % / différentiel **196/196** -O0→-O3 + in-place **3/3** / SMT 11/11
/ magicdiv 2^32) — la combinaison analyse-formelle + vérification qu'aucun outil
grand public n'offre. Incomplétude réelle ramenée à **26 fonctions** (surtout x87,
toutes résolubles — §8). Reste surtout : **x87**, **types (§5)**, **vtables
(§6.3)**, **couche LLM (§9)**, et l'**unification** des deux pipelines. Principe
sacré : **jamais de code faux.**
