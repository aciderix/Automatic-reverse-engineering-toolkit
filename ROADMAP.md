# ARET — Roadmap technique vers la décompilation vérifiée

> Document de conception destiné à l'agent de code qui développe ARET.
> Objectif : aller plus loin et faire mieux que tous les décompilateurs
> existants (Ghidra/Hex-Rays/rev.ng/LLM4Decompile), en récupérant
> **automatiquement** un code source **réel, compilable et prouvé équivalent**
> au binaire d'origine.

---

## 0. Lire ceci en premier : la définition du succès

Le but final — « obtenir automatiquement le code source » — doit être redéfini
de façon **mesurable**, sinon il est impossible de savoir si on progresse ou si
on régresse.

### 0.1 Ce qui est physiquement impossible (ne jamais le promettre)

La compilation **détruit** de l'information qui n'est plus dans les octets :

- noms de variables / fonctions / champs,
- commentaires,
- types sémantiques (un `int` qui était un `enum Color`, un `char*` qui était
  un `const char* nom_utilisateur`),
- la structure exacte des templates C++, des macros, du découpage en fichiers.

Aucun outil ne peut *récupérer* cette information : au mieux il l'**invente** de
façon plausible. Toute autre prétention est du marketing.

### 0.2 Ce qui EST récupérable et vérifiable (la vraie frontière)

Ce qui reste entièrement déterminé par les octets, et qu'on peut viser à 100 % :

- la **sémantique** exacte (ce que le code calcule),
- la **structure de contrôle** (boucles, conditions, switch),
- les **bornes de types** (largeurs, signé/non-signé, pointeur vs scalaire,
  agrégats accédés par offset),
- les **conventions d'appel** et signatures (nombre/largeur d'arguments).

### 0.3 La métrique nord (north-star metric)

> **Re-exécutabilité prouvée** : le pourcentage de fonctions dont le C émis
> (a) recompile sans erreur avec le même compilateur/flags, et (b) est prouvé
> sémantiquement équivalent au bloc binaire d'origine (différentiel ou SMT).

C'est la métrique que **personne ne publie honnêtement en boucle fermée**.
Quand ARET pourra dire *« 78 % des fonctions de ce binaire recompilent en code
prouvé équivalent »*, le projet aura dépassé l'état de l'art grand public.

Métriques secondaires (lisibilité, pour la couche LLM) :
- **recompilabilité** seule (compile, sans preuve d'équivalence),
- **similarité d'édition** au source d'origine quand on l'a (benchmark),
- densité de `goto` résiduels, de `__asm__` résiduels, de casts.

---

## 1. État actuel et le plafond architectural

### 1.1 Ce qui marche déjà (à préserver)

- `src/loader/mod.rs` — parsing PE/ELF/Mach-O via `object`, vue mémoire unifiée.
- `src/disasm/mod.rs` — décodage x86/x64 via `iced-x86`, classification du flot.
- `src/analysis/mod.rs` — decode global unique (scalable), découverte de
  fonctions par descente récursive + scan de prologues, construction CFG.
- `src/structure/mod.rs` — dominateurs/post-dominateurs (Cooper-Harvey-Kennedy),
  détection de boucles naturelles, émission récursive `if`/`while` avec
  dégradation en `goto` sûre.
- `src/dataflow/mod.rs` — liveness globale, DCE, propagation mono-usage,
  binding des valeurs de retour d'appel — **le tout prouvé sûr**.

C'est une base solide. **On ne jette rien** : on insère un IR sous la couche
de lifting et on rebranche l'émission par-dessus.

### 1.2 Le plafond : l'IR est du texte C

Le problème structurel n°1, à corriger avant tout le reste :

- `src/ir/mod.rs:213` — `lift_insn` produit directement des `Vec<String>` de C.
- `src/dataflow/mod.rs:141` — `parse_stmt` **re-parse** ce C avec `find(" = ")`,
  `strip_suffix(';')`, etc.
- `src/dataflow/mod.rs:726` — la propagation fait du `replace_word()` (substitution
  textuelle) sur les lignes de C.

Manipuler des chaînes de C interdit fondamentalement :

- le **constant folding** (`eax = 2; edx *= eax` → `edx *= 2`),
- la **propagation à travers les jointures** de branches (φ-nodes),
- l'**inférence de types** réelle,
- la **simplification algébrique** (`x ^ x → 0`, `(a+0) → a`, `x*2 → x<<1` inverse),
- la **désambiguïsation d'alias** mémoire propre.

Tous les décompilateurs sérieux ont un IR **typé en arbres d'expressions + SSA**
comme structure centrale, et ne génèrent le C qu'à la toute fin :

- Hex-Rays → *microcode* (mba),
- Ghidra → *P-Code* (+ Varnodes, HighFunction),
- rev.ng / RetDec → *LLVM IR*.

**Conclusion : le premier chantier est un vrai IR. Tout le reste en dépend.**

---

## 2. Architecture cible

Nouveau pipeline (les modules existants en gras sont conservés, les autres
sont nouveaux ou refondus) :

```
**loader** → **disasm** → **analysis (CFG)**
   → lift  : machine code → IR concret (par instruction, sémantique des flags)
   → ssa   : construction SSA (φ-nodes) sur le CFG
   → opt   : passes sur SSA (const-prop, folding, copy-prop, DCE, GVN, simplif)
   → types : inférence de types par contraintes (largeur, signe, ptr, agrégats)
   → recover : switch/jump-tables, conventions d'appel, idiomes, strings, globals
   → **structure** : if/while/switch (réutilise dominateurs existants)
   → emit  : IR typé → C **compilable**
   → verify : recompile + équivalence (boucle de raffinement)
   → llm   : noms/commentaires/types plausibles par-dessus la structure vérifiée
```

Le cœur : **un IR unique** qui circule de `lift` jusqu'à `emit`, transformé par
des passes successives, chacune **prouvée correcte** ou **gardée derrière un
flag de confiance**.

---

## 3. Pilier 1 — IR SSA typé (la fondation)

### 3.1 Pourquoi

C'est le déblocage de tout. Sans IR, chaque feature ci-dessous se bat contre
des strings. Avec IR, elles deviennent des passes de quelques centaines de
lignes chacune.

### 3.2 Structures de données (proposition Rust)

Créer `src/ir/types.rs` (ou un nouveau crate-module `ir2` pendant la migration) :

```rust
/// Identifiant SSA : un (registre/emplacement) versionné.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct ValueId(pub u32);

/// Emplacement abstrait avant SSA (registre, slot de pile, flag, mémoire).
#[derive(Clone, PartialEq, Eq, Hash)]
pub enum Location {
    Reg(RegId),          // famille canonique + sous-registre (cf. reg_family existant)
    Flag(FlagKind),      // ZF, SF, OF, CF, PF — modélisés explicitement !
    Frame(i64),          // [rbp+disp] / [rsp+disp] normalisé
    Mem,                 // mémoire générique (raffinée par l'alias analysis)
    Temp(u32),           // temporaires introduits par le lifting
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FlagKind { ZF, SF, OF, CF, PF, AF }

/// Type récupéré (treillis ; cf. pilier 3).
#[derive(Clone, PartialEq, Eq)]
pub enum Ty {
    Unknown,
    Int { bits: u8, signed: Option<bool> }, // signed=None tant qu'indéterminé
    Ptr(Box<Ty>),
    Float { bits: u8 },
    Aggregate(AggId),    // struct/array reconstruit
    Code,                // pointeur de fonction
    Bool,
}

/// Expression : arbre, pas du texte.
#[derive(Clone)]
pub enum Expr {
    Const(i128, Ty),
    Use(ValueId),
    Load { addr: Box<Expr>, ty: Ty },
    Unary(UnOp, Box<Expr>),
    Binary(BinOp, Box<Expr>, Box<Expr>),
    Cast { to: Ty, expr: Box<Expr> },
    Addr(Location),                       // &local, &global
    Call { target: CallTarget, args: Vec<Expr>, ret: Ty },
    Phi(Vec<ValueId>),                    // SSA join
    Undef,
}

#[derive(Clone)]
pub enum Stmt {
    Assign { dst: ValueId, expr: Expr },  // SSA : dst défini ici
    Store { addr: Expr, value: Expr, ty: Ty },
    Branch { cond: Expr, taken: BlockId, fallthrough: BlockId },
    Jump(BlockId),
    Switch { value: Expr, cases: Vec<(i128, BlockId)>, default: BlockId },
    Return(Option<Expr>),
    CallStmt(Expr),                       // appel dont le retour est ignoré
    Asm(String),                          // irréductible : on garde l'asm brut
    Nop,
}
```

Points clés de conception :

1. **Modéliser les flags explicitement** (`FlagKind`). Aujourd'hui les
   conditions sont reconstruites en re-parsant le dernier `cmp`
   (`decompile/mod.rs:177 parse_cmp`, `ir/mod.rs:309 branch_condition`). Avec
   l'IR, `cmp a,b` écrit `ZF = (a-b)==0`, `CF = a<b` (unsigned), etc., et
   `jcc` lit ces définitions de flags. La passe de folding élimine ensuite les
   flags morts et reconstruit `if (a <= b)` **par dataflow**, pas par regex.
   C'est plus correct (gère les flags définis loin du `jcc`, réutilisés, etc.).

2. **Tout en SSA** : chaque écriture crée un nouveau `ValueId`. Les `Phi`
   matérialisent les jointures. C'est ce qui permet la propagation **à travers
   les branches** que la roadmap actuelle liste comme manquante.

3. **`Asm(String)`** : la soupape de sûreté. Tout ce qu'on ne sait pas lifter
   (SIMD complexe, instructions privilégiées) reste de l'asm inline → la sortie
   est **toujours sémantiquement honnête**, jamais fausse.

4. **Deux formes dans un seul IR (amélioration apportée à l'implémentation).**
   Le §3.2 d'origine ne décrivait que la forme *SSA* (`Assign{dst: ValueId}`,
   `Use(ValueId)`), mais le lifter produit du **pré-SSA** : destinations =
   `Location`, lectures = `Location`. On a donc ajouté `Expr::Read(Location)` et
   `Stmt::Set{dst: Location, expr}`. Le lifter émet `Read`/`Set` ; la
   construction SSA (§3.4) les réécrit en `Use`/`Assign`/`Phi`. Les deux formes
   cohabitent dans les mêmes enums `Expr`/`Stmt` — implémenté dans
   `src/ir/types.rs`.

5. **Sémantique partielle de registre (point de correction important).** Les
   écritures 32-bit zéro-étendent (x64), les écritures 8/16-bit préservent les
   bits hauts (`combine_write` dans `src/ir/lift.rs`). Les octets *hauts*
   (`ah/bh/ch/dh`) en lecture/écriture ne sont pas encore modélisés → repli
   `Asm` (sûr). À compléter.

### 3.3 Lifting (refonte de `src/ir/mod.rs`)

Remplacer `lift_insn(&Insn) -> Vec<String>` par
`lift_insn(&Insn, &mut LiftCtx) -> Vec<Stmt>` qui produit de l'IR.

- Idéal : utiliser les `instr_info` d'`iced-x86` (déjà activé dans `Cargo.toml`)
  pour énumérer **registres lus/écrits et flags affectés** par instruction, au
  lieu de re-parser le texte Intel. iced expose `RflagsModified`,
  `used_registers()`, etc. → lifting beaucoup plus robuste que le split textuel
  actuel (`ir/mod.rs:14 split_insn`).
- Chaque instruction définit ses flags. Exemple `sub eax, ebx` :
  ```
  t0 = eax - ebx
  ZF = (t0 == 0); SF = (t0 < 0); CF = (eax <u ebx); OF = signed_overflow(eax,ebx)
  eax = t0
  ```
- `lea` → `Addr`/arithmétique d'adresse (pas un load). Déjà géré textuellement
  en `ir/mod.rs:178 lea_src`, à porter sur l'IR.

### 3.4 Construction SSA (`src/ssa/mod.rs`, nouveau)

Algorithme standard, on a déjà tout l'outillage de dominateurs :

1. Réutiliser `Structurer::compute_dominators` (`structure/mod.rs:255`) — le
   sortir dans un module partagé `src/cfg/dom.rs` pour qu'`analysis`, `ssa` et
   `structure` le partagent.
2. Calculer la **dominance frontier** (Cytron et al.).
3. Placer les φ-nodes pour chaque `Location` aux frontières de dominance.
4. Renommer en versions SSA (parcours du dominator tree).

Référence : Cytron-Ferrante-Rosen-Wegman-Zadeck, *Efficiently Computing SSA*.

### 3.5 Migration sans tout casser

Stratégie incrémentale recommandée :

1. Construire l'IR + SSA **en parallèle** du pipeline texte existant.
2. Ajouter un `emit_c_from_ir()` minimal qui reproduit la sortie actuelle.
3. Mettre les deux derrière un flag (`--ir` vs legacy), comparer sur le corpus.
4. Quand l'IR atteint la parité, basculer `structure`/`decompile` dessus et
   supprimer la manipulation de strings de `dataflow/mod.rs`.

### 3.6 Jalons

- [x] `Expr`/`Stmt`/`Ty` définis et documentés (`src/ir/types.rs`, + formes
      pré-SSA `Read`/`Set`). Tests unitaires verts.
- [x] Module dominateurs partagé + frontière de dominance (`src/cfg/dom.rs`).
- [~] Lifting IR des mnémoniques courants (en cours, `src/ir/lift.rs`) : mov,
      movzx/sx, lea, add/sub/and/or/xor (+ flags), cmp/test (flags), inc/dec,
      neg/not, shl/shr/sar, push/pop, call, ret. Le reste → `Asm`. À étendre
      (idiv/mul, setcc, cmov, SIMC, octets hauts).
- [x] Construction SSA avec φ vérifiée sur petits cas (`src/ssa/mod.rs`,
      algorithme de Cytron sur `cfg::dom`). Tests : φ placé à la jonction d'un
      diamant et référencé par l'usage ; unicité des définitions SSA.
- [x] Construction de l'IR-CFG depuis `analysis::Function` (`src/ir/build.rs` :
      lifting des blocs + terminateurs `Branch`/`Jump`/`Return`, condition de
      `jcc` dérivée des flags). Observable via `aret <bin> --mode ir`. Vérifié
      de bout en bout : code machine → IR typé SSA avec φ sur `factorial` et sur
      de vraies fonctions du jeu (pas de panique).
- [x] Passes SSA : propagation (constantes/copies/mono-usage) + folding +
      simplification algébrique + DCE (`src/opt/mod.rs`). Sur `factorial`, B2
      passe de ~14 à 4 statements (flags morts supprimés) et le `jne` est
      reconstruit en `!=` par dataflow. Conditions non-signées/égalité
      reconstruites (not-through-relational, `CF|ZF`→`<=`). Tests verts.
- [x] Reconstruction des conditions **signées** (`SF!=OF` → `<s`, `ZF|(SF!=OF)`
      → `<=s`) par pattern sur les expressions de flags du lifter (`src/opt`).
      Sur `factorial` : `if (n <= 1)` et la boucle `if (i != n+1)` reconstruites,
      `imul` lifté → `result *= i` ; l'algorithme complet est récupéré en SSA.
- [x] Sûreté `Asm` : instructions non liftées modélisées par leurs effets
      (appel opaque des entrées + clobber `Undef` des sorties) → DCE/opt saines.
- [x] Valeur de retour modélisée (`Return` lit `rax`) → le calcul du résultat
      n'est plus supprimé (ex. `classify` : `setne` → `(x != 0)`, φ du retour).
- [~] Couverture lifter : mov/movzx/sx, lea, add/sub/and/or/xor (+flags),
      cmp/test, inc/dec, neg/not, shl/shr/sar, push/pop, call, ret, imul 2/3-op,
      **setcc** faits ; reste mul/idiv/div, cmov, SSE.
- [x] `emit` IR→C **qui compile** (`src/emit/mod.rs`, `--mode emit`) : destruction
      SSA (abaissement des φ en blocs de copie sur chaque arête), variables
      `uint64_t` typées, prélude `#include <stdint.h>` + forward-decls des
      callees, forme goto. Vérifié : `factorial` recompile et la logique est
      correcte (boucle, copies de φ) ; l'unité demo entière (12 fonctions) et
      des fonctions du jeu recompilent. → débloque le niveau « recompile » du §8.
- [x] Émission **structurée** (`src/emit/structured.rs`) : même algorithme
      dominateurs/post-dominateurs + boucles naturelles que le pipeline texte,
      adapté à l'IR (sur l'IR post-destruction-SSA), `if`/`else`/`while`/`break`,
      dégradation `goto` sûre. `factorial` se lit comme du C structuré. Vérifié :
      **2000/2000 fonctions du jeu recompilent (100 %)** via le chemin structuré.
- [x] **Variables de frame récupérées dans l'IR** : le lifter reconnaît les
      accès `[ebp/rbp ± disp]` comme des emplacements `Frame(disp)` (lecture/
      écriture/adresse), nommés `arg_N`/`local_N` à l'émission et déclarés.
      Approche **saine** (pas de promotion SSA, les slots restent mémoire). Plus
      une correction DCE : un load mort est supprimable (≠ non-hoistable) →
      disparition des calculs de flags morts. `sub_401670` : `arg_8` récupéré,
      condition réduite à `if (*arg_8 == 0)`. Recompilabilité 600/600 (100 %).
- [~] Couverture lifter étendue : **cmov** (via une primitive `Expr::Select`,
      émise en `? :`) et **setcc** ajoutés ; reste mul/idiv/div (résultat
      double-largeur → `asm_fallback` sain pour l'instant), SSE.
- [ ] **Promotion des slots non-aliasés en SSA (§4.1) — DIFFÉRÉE volontairement.**
      La faire *sainement* exige une vraie analyse d'alias : le frame base peut
      s'échapper sans `lea` (`mov reg, ebp` puis `[reg-4]`), et un slot promu
      pourrait alors être aliasé par un store générique → code faux. Plutôt que
      bâcler (violation du principe « jamais de sortie incorrecte »), prérequis =
      l'analyse d'alias frame/heap/inconnu du §4.1. À faire avant la promotion.
- [x] **Récupération des arguments (signatures réelles).** Les slots `Frame`
      positifs au-delà de `saved_bp`/retaddr deviennent de vrais paramètres :
      `uint64_t sub_401670(uint64_t arg_8, uint64_t arg_c)`. Les unités à une
      fonction utilisent les paramètres (le chemin de `verify` compile chaque
      fonction isolément) ; les unités multi-fonctions restent en `(void)` tant
      que la récupération des arguments aux sites d'appel n'est pas faite.
      Recompilabilité maintenue 700/700 (100 %).
- [ ] Inférence de types (§5) + récupération des arguments registres (System V/
      Win64) + arguments aux sites d'appel, puis bascule du pipeline par défaut.

---

## 4. Pilier 2 — Passes d'optimisation sur SSA (`src/opt/`)

Une fois en SSA, chaque passe est petite, locale, et composable. Ordre
d'application typique (itérer jusqu'à point fixe) :

1. **Constant propagation + folding** : `Const op Const → Const`. Élimine la
   majorité du bruit (`xor eax,eax` → `0`, calculs d'offsets constants).
   Algorithme : **SCCP** (Sparse Conditional Constant Propagation, Wegman-Zadeck)
   — élimine aussi les branches mortes (conditions constantes) gratuitement.
2. **Copy propagation** : `b = a; use(b)` → `use(a)`. Trivial en SSA.
3. **Expression propagation cross-block** : remplace la propagation textuelle
   straight-line actuelle (`dataflow/mod.rs:658`). En SSA, traverse les φ.
4. **Simplification algébrique** : `x+0`, `x*1`, `x^x`, `x&0`, `(x<<n)>>n`,
   reconstruction de `x*2 → x+x` inverse, idiomes de division par constante
   (multiplication magique → `x / k`), `x & (p-1)` quand `p` puissance de 2.
5. **Reconstruction de conditions depuis les flags** : pattern-match sur les
   définitions de flags consommées par un `Branch` → opérateur relationnel C
   correct (porte la table signé/non-signé de `ir/mod.rs:317 render_with_cmp`).
6. **Global Value Numbering (GVN)** : fusionne les expressions équivalentes.
7. **DCE** sur SSA : trivial (une valeur sans use est morte) — remplace
   `dataflow/mod.rs:598 dce`, en gérant correctement les effets de bord
   (stores, calls, asm jamais supprimés).
8. **Élimination du code de flags mort** : les flags non lus disparaissent,
   nettoyant la majeure partie du bruit du lifting de §3.3.

> Garde-fou : chaque passe doit préserver la sémantique observable
> (stores, appels, valeurs de retour, accès volatils). En cas de doute → ne
> pas transformer. La sûreté prime sur la beauté.

> **Garde-fou `Asm` — RÉSOLU.** Une instruction non liftée est désormais
> modélisée par ses **effets réels** (lus via `iced` `instr_info` :
> `used_registers`, `rflags_read/written`) plutôt que par un `Stmt::Asm` opaque :
> - un appel opaque `asm:<texte>(entrées)` qui **garde les sources vivantes**,
> - un clobber `Set(W, Undef)` par registre/flag écrit, de sorte que toute
>   lecture ultérieure obtient une **version fraîche honnête** (pas de valeur
>   périmée). Vérifié sur `lock cmpxchg` : la branche dépendante devient
>   `if (undef == 0)` (drapeau inconnu), correct.
> La DCE/propagation sont donc saines même en présence d'instructions non
> modélisées. Reste à élargir la couverture du lifter pour réduire les `undef`.

### 4.1 Analyse d'alias mémoire (`src/opt/alias.rs`)

Nécessaire pour propager à travers les `Load`/`Store` sans bug. Niveau
pragmatique suffisant au début :

- distinguer **frame** (pile, `rbp/rsp + disp` connu) vs **heap/global** vs
  **inconnu** ;
- deux accès frame à des offsets/largeurs disjoints **ne s'aliasent pas** ;
- tout accès via pointeur inconnu aliase tout (conservateur).

Cela permet de promouvoir des slots de pile non-aliasés en variables SSA
(« stack variable promotion ») — gros gain de lisibilité.

**État — ✅ FAIT (oracle + verdict de promouvabilité)** : `src/opt/alias.rs`.
`classify(addr, width)` range chaque accès en `Frame{off,width}` /
`Global{width}` / `Unknown` (sur l'IR *pré-SSA*, où la base `rbp/rsp` est encore
un `Read(Reg)` syntaxique). `may_alias` applique les règles : deux `Frame` ne
s'aliasent que si leurs plages d'octets se recouvrent, `Frame` vs `Global`
jamais, tout ce qui touche `Unknown` peut aliaser. `frame_promotable(func)` =
aucune adresse de slot matérialisée (`&local`) **et** aucun accès pointeur
inconnu — verdict *sound* (sur-approximé : un `false` peut être prudent, un
`true` garantit qu'une promotion ne changerait pas le comportement). Analyse
pure (ne mute pas l'IR → zéro risque). Diagnostic *display-only* dans le dump IR.
8 tests unitaires. Mesuré sur `ls` : **111 fonctions promouvables / 107 non**.

Le verdict est exposé via le champ `IrFunction.frame_promotable` (calculé dans
`build_ir`) et affiché dans le dump IR.

**Promotion — tentée puis retirée (preuve à l'appui).** Une passe de promotion
*saine* a été écrite (substitution des lectures d'un slot local à assignation
unique dont la def domine tous les usages, puis suppression du store mort —
noms préservés, jamais de `vN` anonyme). Elle passe le gate (équivalence
différentielle 26/26). **Mais mesure sur le corpus : elle ne se déclenche
jamais.** Raison structurelle : avec le gate sain `!adresse_prise &&
!accès_inconnu`, **aucune** fonction promouvable n'a de slot local rbp
(0/ls, 0/gzip, 0/cat) — car toute fonction ayant des locaux nommés fait aussi
des accès heap (→ `accès_inconnu`) ou prend une adresse. Le sous-ensemble
{promouvable} ∩ {a des locaux} est vide. Plutôt que livrer une transformation
mutant la sortie mais morte sur du vrai code (violation de « rien d'inutile »),
la passe a été retirée ; le champ + l'oracle restent.

**Analyse de régions de frame — ✅ FAIT (`src/opt/frame.rs`).** L'analyse
*consciente des offsets* demandée : (1) **taint** flow-insensitive des registres
dérivés de `rsp`/`rbp` (un accès via base non-taintée est du heap → disjoint
d'une frame non-échappée) ; (2) **offset de `rsp`** flow-sensitive (worklist sur
le CFG) suivi à travers `push`/`pop`/`sub rsp`/`mov rsp,rbp`, plaçant `rbp`
(`mov rbp,rsp`) et chaque accès `rsp`/`rbp`-relatif sur un axe relatif à
l'entrée ; (3) **chevauchement** : un local `[rbp-d]` est promouvable ssi sa
plage d'octets ne recoupe aucun accès frame générique (spill, arg sortant,
registre sauvegardé). *Sound by bail* : tout ce que le modèle ne suit pas
(écriture `rsp` non reconnue, `rsp` incohérent à une jointure, index variable,
pointeur de frame échappé par copie/store/call/`lea`) ⇒ **aucun** slot promu.
4 tests unitaires (dont « local promouvable malgré un accès heap »). Pur
(ne mute pas l'IR). Diagnostic *display-only* dans le dump : `// frame: N
promotable local slot(s): …`.

**Constat clé (chiffré).** Le payoff de la promotion de slots `[rbp-k]` est
**faible sur un corpus optimisé** : `cat` 1 fonction / 9 slots, `ls`≈0, `gzip`≈0.
Catégorisation sur `ls` (45 fonctions à locaux) : **30 BAIL** (alignement
`and rsp`, tableaux de pile à index variable, jointures `rsp` incohérentes),
**11 ESCAPE** (`lea` d'un local), **4 OK mais 0 promu** (le local recoupe un
push de registre sauvegardé). Raison de fond : le code **-O2 omet souvent le
frame pointer** → les locaux chauds sont **relatifs à `rsp`** et ne deviennent
jamais des slots `Location::Frame` (qui ne reconnaît que `[rbp±d]`).

**Récupération des locaux relatifs à `rsp` — ✅ FAIT (`promote_stack_slots`).**
Le modèle d'offset `rsp` donne à chaque accès `rsp`/`rbp`-relatif à offset
constant une identité stable (coordonnée frame), puis la passe réécrit les accès
**sûrs** en slots nommés `Location::Frame` : `*(T*)(reg+k)` → `local_…`. Un slot
n'est réécrit que si (a) le modèle de frame a tenu (`ok`, rien d'échappé),
(b) une **largeur unique** (le modèle `Frame` ne porte pas la largeur), et
(c) sa plage **ne recoupe aucun autre slot**. Le reste reste en mémoire — sain
par omission. Tourne pré-SSA ; activé par défaut dans les pipelines emit/ir/verify.

**C'est aussi une correction de justesse**, pas qu'un gain de lisibilité : un
accès pile via le registre de frame (non initialisé dans une recompilation
isolée) déréférence sinon un pointeur sauvage. Deux fonctions corpus à spill
(`spill2`, `spill3`, locaux `volatile`) ont été ajoutées : **avant** elles
*crashaient* en test différentiel (déréférencement de base non initialisée),
**après** elles passent. Suite différentielle portée à **28/28**, gate complet
vert (recompile 100 %, SMT 11/11, magicdiv 2³²). 6 tests unitaires dans
`frame.rs`.

Reste : étendre aux slots multi-largeur / recouvrants (aujourd'hui laissés en
mémoire), et l'émission typée des locaux récupérés (via §5).

---

## 5. Pilier 3 — Inférence de types par contraintes (`src/types/`)

### 5.1 Pourquoi

Transformer `*(uint64_t*)(rdi + 8)` en `rdi->field_8` (puis, via LLM, en
`user->id`). C'est ce qui fait passer la sortie de « asm habillé en C » à
« code structuré ».

### 5.2 Approche : génération + résolution de contraintes

Treillis de types (`Ty` du §3.2) avec ⊤ = `Unknown`, ⊥ = conflit. Générer des
contraintes depuis l'usage, puis unifier (union-find) :

| Construit IR | Contrainte |
|---|---|
| `Load { addr, .. }` / `Store { addr, .. }` | `typeof(addr) <: Ptr(_)` |
| `addr = base + k` puis `Load(addr)` | `base` est `Ptr` vers agrégat ; champ à l'offset `k` |
| comparaison signée (`jl/jg`) | l'opérande est `signed` |
| comparaison non-signée (`jb/ja`) | l'opérande est `unsigned` |
| division `idiv`/`div` | signé / non-signé respectivement |
| argument passé à une fonction connue (libc) | type du prototype (cf. pilier 6.4) |
| registre XMM / instruction SSE | `Float` |
| valeur utilisée comme cible d'appel indirect | `Code` (ptr de fonction) |

### 5.3 Reconstruction d'agrégats

Quand un même pointeur de base est accédé à plusieurs offsets `{0, 8, 16}` avec
des largeurs cohérentes → synthétiser une `struct` :

```c
struct s_rdi { uint64_t field_0; void* field_8; uint32_t field_10; };
```

Accès tableau : base + `i*stride` (stride constant, `i` variable) → `T arr[]`.
Étend `scan_frame_vars` (`ir/mod.rs:131`), qui ne fait aujourd'hui que la
largeur sur des slots `[rbp±disp]` purs.

### 5.4 Jalons

- [x] Treillis + union-find + résolution. — `src/types/mod.rs` : `TypeEnv`
  (union-find par rang + path-halving), `join`/`join_sign` totaux (un conflit
  retombe sur un scalaire 64 bits, jamais ⊥), génération de contraintes depuis
  l'usage (load/store → `Ptr`, comparaison/division signée vs non-signée →
  `signed`, `sar`/`shr`, `movsx`/sign-extend, cible d'appel indirect → `Code`,
  copies + φ → union). 10 tests unitaires.
- [x] Pointeurs vs scalaires distingués sur le corpus. — annotation
  *display-only* dans le dump IR (`// inferred types:`), vérifiée sur `ls` :
  pointeurs (`int8*`, `int32*`, `int16*`), pointeurs de code (`code*`, issus
  des appels indirects / jump tables), scalaires signés (`int64_t`) et non
  signés (`uint64_t`). L'émission garde le stockage `uint64_t` + masques
  explicites, donc la sémantique (et le gate de régression) est inchangée.
- [x] Synthèse de structs pour accès multi-offset. — `recover_aggregates`
  (`src/types/mod.rs`) : décompose chaque accès `Load`/`Store` en `(base,
  offset)`, canonicalise la base via l'union-find (copies/φ fusionnées), et
  promeut en `struct` toute base touchée à ≥2 offsets distincts. Type de champ =
  `join` des accès au même offset. Rendu *display-only* dans le dump IR
  (`// struct s_vN { /*+0x8*/ … }`). Vérifié sur `ls` (buffers d'octets,
  enregistrements 64 bits alignés, paires d'int32).
- [x] Détection de tableaux à stride constant. — `recover_arrays`
  (`src/types/mod.rs`) : reconnaît `base + i*stride` et `base + (i << shift)`
  (indice variable, échelle constante), base canonicalisée par l'union-find,
  type d'élément = `join` des accès. Rendu *display-only* (`// T arr_vN[]; /*
  stride 0xN */`). Distinct des structs (offset constant). Vérifié sur `ls`
  (tableaux de pointeurs stride 8, tableau d'int16 stride 2).
- [ ] Utiliser les types inférés dans l'émission (déclarations typées) une fois
  la promotion de pile / l'analyse d'alias en place — aujourd'hui *display-only*
  par sûreté.

---

## 6. Pilier 4 — Récupération de constructs haut-niveau (`src/recover/`)

### 6.1 Switch / jump tables (priorité haute) — ✅ FAIT (résolution + couverture)

`analysis::resolve_jump_table` reconnaît `jmp [table + idx*ptr]`, lit la table
dans le binaire (entrées pointeur, base absolue/rip-rel, tant que les cibles
sont exécutables), décode les **cas** (couverture étendue) et ajoute les arêtes
au CFG. La sortie texte par défaut émet un `switch` avec la liste des cas, et un
passage final émet les blocs de cas atteints uniquement par la table. Vérifié
sur le jeu : switch de 4 et 75 cas récupérés ; combiné aux chaînes, un mapping
index→nom (`case 0 → "None"`, `1 → "Area"`, `2 → "Cone"`). Recompilabilité IR
maintenue 700/700.

Reste : récupérer l'**index** (expression du switch) dans l'IR → vrai
`Stmt::Switch` typé ; tables relatives 4 octets sur x64 ; idiome ci-dessous.

Détection de référence — le pattern :

```
cmp idx, N ; ja default ; jmp [base + idx*ptr]
```

1. Reconnaître l'idiome (borne + jump indexé).
2. Lire la table dans la section `.rodata` (via `loader`).
3. Résoudre les `N` cibles → ajouter les arêtes au CFG **avant** structuration.
4. Émettre un `Stmt::Switch` → `switch/case` C.

Gère aussi les tables relatives (offsets 32-bit ajoutés à une base, courant sur
x64/PIC) et les tables imbriquées.

### 6.2 Conventions d'appel et signatures

Remplacer le `ARG_REGS` conservateur (`dataflow/mod.rs:59`) par une vraie
**analyse de liveness inter-procédurale** des registres d'arguments :

- détecter quels registres d'argument (System V : rdi,rsi,rdx,rcx,r8,r9,xmm0-7 ;
  Win64 : rcx,rdx,r8,r9) sont **lus avant écrits** dans une fonction → ce sont
  ses paramètres ;
- détecter le registre de retour réellement défini-puis-live à la sortie ;
- propager ces signatures aux sites d'appel pour reconstruire les **vrais
  arguments** des appels indirects/registres (le `cdecl_arg_count` actuel,
  `decompile/mod.rs:46`, ne gère que la pile 32-bit).

### 6.3 Appels indirects et vtables (C++)

- résoudre `call [vtable + k]` quand la vtable est identifiable en `.rodata` ;
- nommer les sites d'appel virtuels même quand la cible exacte est inconnue
  (`obj->vtable->method_k(...)`).

### 6.4 Signatures de bibliothèque (FLIRT-like) et données

- **Matching de fonctions connues** : empreintes de la CRT/libc (memcpy, malloc,
  printf…) pour les nommer et **sauter le boilerplate runtime**. Démarrer avec
  une petite base d'empreintes par hash de motif d'octets masqués.
- **Strings** : ✅ **fait** (`loader::read_cstring` + `decompile::annotate_strings`).
  Les adresses pointant vers des chaînes lisibles d'une section read-only sont
  annotées inline (`0x402004 /* "%d %d\n" */`) dans la sortie texte par défaut.
  Sur le jeu : **50 940 chaînes récupérées dans 12 708 fonctions** — révélant
  qu'OpenSSL + curl sont liés statiquement. Affichage seul → sûr.
- **Globals** : nommer/typer les accès aux données statiques.
- **PLT/GOT/imports** : ✅ PE (IAT) **et ELF** (relocations dynamiques → GOT, décodage des stubs `.plt*` → nom). Sur gzip (ELF strippé) : 151 imports résolus, les appels libc nommés dans la sortie (`free`, `strlen`, `malloc`, `strcmp`, `memcpy`...). Sur le jeu Steam-DRM, l_IAT non standard limite la résolution (propriété du binaire).

---

## 7. Pilier 5 — Émission de C compilable (`src/emit/`)

Aujourd'hui la sortie est du pseudo-C (`int64_t`, registres bruts `eax`,
`__asm__("...")`). Pour fermer la boucle de vérification (§8), il faut du C qui
**compile vraiment**.

Exigences :

1. **En-tête de prélude** auto-généré : `#include <stdint.h>`, typedefs
   (`uint128`, helpers), déclarations forward de toutes les `sub_xxxx`, structs
   reconstruites, déclarations des globals.
2. **Variables, pas registres** : après SSA + types, émettre des variables C
   nommées et typées, pas `eax`/`rax`. Les registres bruts ne survivent que
   dans les blocs `Asm` irréductibles.
3. **Helpers sémantiques** pour les idiomes non exprimables directement en C :
   `__rol32`, `__sar`, `__builtin_*` pour les flags d'overflow, intrinsics SSE
   (`<immintrin.h>`) pour le SIMD lifté.
4. **Asm inline GCC** pour les blocs `Asm` (`asm volatile(...)`), de sorte que
   même une fonction partiellement décompilée **compile et s'exécute**.
5. Mode `--strict` : n'émettre une fonction que si elle est 100 % liftée (pas
   d'`Asm` résiduel) — utile pour mesurer la re-exécutabilité « pure ».

---

## 8. Pilier 6 — Boucle de recompilation vérifiée (`src/verify/`) — LE différenciateur

C'est ici qu'ARET dépasse l'état de l'art grand public. Personne n'offre une
boucle fermée *décompile → recompile → prouve équivalence → raffine*.

> **Implémenté (niveau 1).** `src/verify/mod.rs` + `aret <bin> --mode verify`
> émet le C de chaque fonction, le recompile (`cc -x c - -c`, via stdin, sans
> fichiers temporaires ni édition de liens) et rapporte le taux de
> recompilabilité — la première forme de la métrique nord. Résultats : **demo
> 12/12 (100 %)**, **échantillon du jeu 500/500 (100 %)**. `--limit N` borne le
> nombre de fonctions. Reste : niveaux 2 (différentiel par exécution) et 3 (SMT).

### 8.1 Harness de round-trip

```
binaire d'origine
   → ARET → fonction.c (compilable, §7)
   → recompile (même compilateur/flags si connus, sinon gcc/clang -O0..-O2)
   → objet/fonction recompilé
   → comparaison (§8.2)
   → score + diff exploitable comme signal de raffinement
```

Implémentation : un sous-module qui invoque `cc`, isole une fonction (objet
relogeable), et compare.

### 8.2 Niveaux d'équivalence (du plus faible au plus fort)

1. **Recompile** : ça compile sans erreur. ✅ `--mode verify` (100 % sur le jeu).
2. **Différentiel par exécution** : ✅ **implémenté** (`bench/difftest.sh` +
   `bench/corpus.c`). Pour chaque fonction du corpus : ARET la décompile, on
   recompile, et on compare à la fonction d'origine sur 200k entrées aléatoires.
   **Résultat : 16/16 fonctions prouvées empiriquement équivalentes** (dont pointeurs/tableaux/boucles/chaînes — `arraysum`, `arraymax`, `counteq`, `strlen`). Le
   harness a immédiatement **trouvé un vrai bug** (comparaisons signées 32-bit :
   il faut étendre le signe depuis la largeur masquée, `(int64_t)(int32_t)x`, et
   non caster la valeur masquée positive) — corrigé, d'où 6/11 → 11/11. C'est
   exactement la valeur de la boucle : elle rend les bugs visibles et mesurables.
   Reste : étendre le corpus, et le différentiel sur binaires sans source (via
   chargement/émulation de la fonction d'origine).

   **Vérification sur code optimisé (multi-niveaux) — ✅ -O0/-O1/-O2 verts.**
   `difftest.sh` tourne à plusieurs niveaux d'optimisation et le gate par défaut
   couvre désormais **-O0, -O1 et -O2 : 102/102 fonctions** prouvées équivalentes
   (le harness a d'abord exposé que « ça recompile » ≠ « c'est correct », puis
   guidé les corrections). Bugs trouvés **et corrigés** :
   - **Relocations en .o — ✅ CORRIGÉ.** Un `call`/`jmp` dans un objet ne porte
     qu'un déplacement-placeholder jusqu'à l'édition de liens ; ARET décodait donc
     les appels récursifs/croisés/externes vers une adresse fausse (corrompant le
     CFG : les instructions après l'appel devenaient orphelines → valeur de retour
     perdue). Le loader applique `.rela.text` (`parse_static_relocs`, **seulement
     les sections code** — sinon les relocations de `.rodata`/`.eh_frame`, toutes
     basées à 0 dans un .o, écrasaient les branches intra-fonction). `decode_at`
     supprime le placeholder : cible résolue pour un symbole défini, sinon appel
     externe **nommé** via le nom de relocation (`strlen`).
   - **Canaries de pile (`fs:[0x28]`) — ✅ CORRIGÉ.** Lecture TLS modélisée comme
     une pseudo-valeur constante, écriture ignorée ; le test du canary se replie
     en no-op (`0-0==0` → chemin normal), équivalent au canary aléatoire réel.
   - **Idiome libc — ✅ CORRIGÉ.** `gcc` remplace `strlen_c` par un appel externe
     `strlen` ; nommé correctement + valeur de retour `strlen(s+1)+1` rétablie
     (conséquence de la suppression du placeholder qui réincorpore le bloc tail).
   - **SSE/AVX mal-lifté en scalaire — ✅ rendu honnête.** `reg_id` mappe XMM/YMM/
     ZMM vers un id de registre, donc une instruction packed (`paddd`, `movdqu`…)
     était liftée comme une op scalaire 64 bits — **silencieusement fausse**. Un
     garde (`uses_vector_reg`) force désormais le fallback `asm` pour toute
     instruction vectorielle, et `count_unlifted` détecte les fallbacks (calls
     `asm:…` + `Stmt::Asm`). L'émission préfixe alors un avertissement
     `// WARNING: N unmodelled instruction(s) — decompilation INCOMPLETE`, et le
     harness classe ces fonctions **INCOMPLETE** (non testées pour l'équivalence)
     plutôt que FAIL. Résultat : différentiel **-O0/-O1/-O2/-O3 = 131/131
     complètes** + 5 incomplètes assumées (vectorisation -O3).

   - **SSE scalaire flottant — ✅ FAIT + validé.** Lifté via des helpers C
     bit-exacts (`__fp_*`) : les XMM restent des motifs de bits en `uint64_t`, les
     helpers réinterprètent, calculent en `float`/`double` natif, et réinterprètent
     — donc bit-exact avec l'original. Couvre `movss/movsd/movd/movq`, `addss…divsd`,
     `cvtsi2ss/sd`, `cvttss2si/sd`, `cvtss2sd/cvtsd2ss`, `comiss/comisd` (→ flags),
     `pxor/xorps` (mise à zéro), `movaps/movapd` reg-reg. **Folding des constantes
     `.rodata`** (rip-relative) en littéraux — lu via la section cible de la
     relocation (contourne la collision base-0 des `.o`) — sans quoi un `movss
     xmm,[rip+c]` déréférençait un pointeur sauvage. Validé : `favg/fcmp/fmix/fpoly`
     passent à -O0/-O1/-O2 ; différentiel **155/155 complètes**.
   - **SSE *packed* 128-bit — ✅ FAIT (sous-ensemble réductions).** Un XMM est
     modélisé en **deux moitiés de 64 bits** (basse `RegId(16+n)`, haute `64+n`).
     Couvre : moves 128-bit (`movaps/movapd/movups/movupd/movdqa/movdqu`, reg +
     mémoire 16 octets), `pxor/xorps/xorpd` (xor 128-bit), `paddd/psubd`
     (lanes 32-bit via `__pi_add32/sub32`), `psrldq` (4/8/12), `pand/pandn/por`,
     `pcmpeqd/pcmpgtd` (masques de lanes), `pshufd` (permutation de dwords), et le
     **folding des constantes vectorielles `.rodata`** (16 octets via la
     relocation). Validé : **toutes** les boucles auto-vectorisées du corpus
     (`arraysum/sumto/arraymax/counteq/sumrec`) passent à -O3. **Différentiel
     complet à TOUS les niveaux -O0/-O1/-O2/-O3 : 160/160, zéro INCOMPLETE.**
   - **Reste pour le vrai code : x87** (`fld/fstp/fadd…`, pile st0-st7) et des
     ops SSE/string moins courantes (`pshuflw`, `pmovmskb`, `pcmpistri`…). Les
     boucles vectorisées entières sont désormais couvertes. *(NB : à ce stade du
     récit l'incomplétude était encore élevée ; elle a depuis chuté à 26 — voir
     les bullets « Tail calls » et « Jump tables PIE » ci-dessous, qui étaient en
     réalité le levier dominant, pas le x87.)*

   - **Tail calls — ✅ FAIT, et c'était LE levier dominant.** La mesure par
     fonction a montré que l'incomplétude du vrai code n'était PAS due au x87/SSE
     (≈1 fonction x87) mais aux **`jmp` indirects/externes** liftés en `Stmt::Asm` :
     surtout `jmp qword [GOT]` (tail call vers un import) et `jmp func` direct.
     `build_ir` les modélise désormais en `return f(args)` (direct → `sub_`/import
     nommé ; `jmp [GOT]` → import). **Effondrement de l'incomplétude : gzip
     83→10, ls 164→23, cat 55→4** (de ~300 à 36 fonctions). Validé par un tail
     call libc (`tlen → jmp strlen`), différentiel 192/192.
   - **`rep movs` → memcpy — ✅ FAIT** (recompile-validé ; ls 23→21). Les
     fonctions de copie mémoire ne sont pas testables en différentiel standalone
     (elles déréférencent le registre de frame non initialisé, comme tout buffer
     de pile).
   - **Jump tables PIE relatives — ✅ FAIT depuis** (`analysis::resolve_pie_jump_table`
     + harnais in-place). Le motif `lea base,[rip+t]; movsxd off=[base+idx*4]; add;
     (notrack) jmp` est reconnu **dans l'analyse** (pour que les cibles de cas
     soient décodées), avec **trace de la def atteignante** du registre base
     inter-blocs ; l'IR le lève en `Stmt::Switch` typé. La validation
     comportementale est faite par le **harnais in-place** (`bench/inplace.sh`,
     qui mappe les PT_LOAD à leur VA → la table relative se lit réellement) : 3/3
     dont `swc` (value-table). La **fusion hot/cold** (`foo.cold`) complète le
     décodage des cibles renvoyées vers `.text.unlikely`.
   - **x87 80-bit — ✅ FAIT** (`ir/lift.rs::lift_x87`, `ir/build.rs::x87_states`).
     Pile FPU suivie **statiquement** (profondeur par instruction, bail si
     ambigu), slots émis en `long double` (= le format 80-bit x87 sur x86-64 →
     recompile en x87 équivalent, bit-exact). Couvre fild/fld/fstp, faddp/fsubp/
     fmulp/fdivp (+ formes reg/mem), fabs/fchs/fsqrt, fxch, fcomi/fucomi(p), et la
     **troncature float→int** prouvée (idiome fnstcw/or/fldcw/fistp). fist/fistp
     en arrondi non prouvé, comparaisons via status-word, transcendantes → `Asm`.
   - **Reste du lifter — ✅ FAIT** : `imul` 1-op, `rol`/`ror` (CF correct, OF
     laissé — indéfini par la spec), `xchg` reg, **SSE packé double** (addpd/subpd/
     mulpd/divpd, unpcklpd/unpckhpd, movhlps/movlhps/movhps/movlps, shufpd),
     `psubusw`, `rep stos`, **appels & tail-calls indirects** (`call/jmp reg` →
     `(*reg)(args)`). Différentiel 160 → **232** (-O0→-O3).
   - **Récupération de CFG — ✅ FAIT** (`analysis/mod.rs`, `ir/build.rs`).
     Corrigés : (a) **sur-segmentation de prologue** — un `push rbp; mov rbp,rsp`
     mi-fonction (après une sortie anticipée) passe pour une entrée ; `collect_
     function` absorbe désormais une entrée prologue-seule atteinte par
     *fall-through* (l'exécution ne tombe jamais d'une fonction dans une autre) ;
     (b) **résolution de jump-table en point fixe** après décodage complet
     (l'inline échouait quand un `jmp` était décodé avant son bloc de setup) ;
     (c) **index de switch inter-blocs** — `pie_switch_index` scanne au-delà du
     bloc du `jmp` (tables à 2 niveaux : la case-cible est un leader de bloc) ;
     (d) **borne de table exacte** — le scan du `cmp idx,N` manquait
     `Immediate8to64` (`cmp r64,imm8`), sautait la vraie borne et sur-lisait la
     table (p. ex. 48 cas pour un switch à 10).
   - **Reste réel : 3 fonctions = le plancher absolu** (gzip 1, ls 1, cat 1) :
     toutes **`_start`** (le stub d'entrée ELF asm, `call __libc_start_main; hlt`).
     `hlt` est privilégié, sans sémantique C ; `_start` n'est pas une fonction C →
     `__asm__`/INCOMPLETE est la seule sortie correcte. Jamais silencieusement faux.

   - **Test sur vrai binaire 32-bit C++ (le jeu PE32, 26 Mo, ~44k fn) — ✅ FAIT.**
     Recompile **100 %** ; incomplétude **~5900 → ~4360 / 44 183 (~90 %
     entièrement modélisées)**. Différentiel **160 → 268** (-O0→-O3). Ajouts (tous
     généraux, différentiel-validés ; les ops 32-bit-only validées via assembleur
     `as --64` écrit à la main) :
     - **x87 status-word** `fcom/fcomp/fcompp/fucom*/fnstsw/sahf` (compare flottant
       32-bit, ≡ fcomi ; pseudo-registre FSW C0/C2/C3) + `fnstsw [mem]`.
     - **SSE packé simple** addps/subps/mulps/divps, minps/maxps, sqrtps, cmpps
       (imm8 predicat), andps/orps/andnps, shufps, unpcklps/unpckhps, movmskps,
       cvtdq2ps. **rcpps/rsqrtps → Asm** (approximations dépendantes du CPU).
     - **Atomics** `lock xadd`, `cmpxchg`, `xchg [mem]` (sémantique valeur mono-thread).
     - `rcl/rcr` (count imm, rotate (w+1)-bit à travers CF), `shld/shrd` (forme CL),
       `stc/clc/cmc`, `shl/shr/sar` posent **CF** (manquait).
     - **3 vrais bugs corrigés** : arg cdecl 32-bit non déclaré (`collect_frame_vars`
       ne parcourait pas la cible d'appel indirect) ; **retenue add/adc non
       width-aware** (CF toujours 0 sur 32-bit) + capture de la retenue d'entrée
       adc/sbb/rcl/rcr dans un temp (sinon SSA résout l'entrée sur la sortie) ;
       **émission `Shl`** sans cast → `1 << 32` = UB C (bit perdu) → cast uint64_t.

   ### Backlog incomplétude (TODO, par catégorie)

   **Mesure honnête (jeu) : 89 % des fonctions incomplètes sont du *plancher
   irréductible*, 10 % d'une queue récupérable mais co-bloquée.**

   - **PLANCHER — ne pas modéliser (non sûr OU non validable OU non bit-exact)** :
     - `pushad/pushfd/popad/popfd` (≈12k) : **32-bit only** — *rejetées par
       l'assembleur 64-bit* → invalidables ici ; et `pushfd`/`lahf` liraient les
       flags bruts SF/AF/PF que ce pipeline n'approxime que pour reconstruire des
       *conditions* → **non sûrs** sans refonte du modèle de flags (cf. ci-dessous).
     - x87 transcendantes `fsin/fcos/fptan/fpatan/f2xm1/fyl2x/fprem/frndint/fscale`
       (microcode, précision dépendante du CPU → pas bit-exact).
     - `rcpps/rsqrtps/rcpss/rsqrtss` (approximations ~12 bits, dépendantes du CPU).
     - Privilégiées / I-O : `hlt/sti/cli/in/out/cpuid/rdtsc/int/iret/lgdt/...`.
     - BCD 32-bit-only : `aaa/aad/aam/daa/das/aas/salc`.
     - `cmpxchg8b/cmpxchg16b` (CAS 64/128-bit — modélisable mais rare ; bas ROI).
   - **QUEUE RÉCUPÉRABLE (sûr + validable, mais faible rendement : co-bloquée par
     le plancher → ~15 fonctions complétées par lot)** — à faire si on y revient :
     - `rcl/rcr` **forme CL** (count variable : rotate (w+1)-bit à count runtime,
       cas count=0 par Select) — ~71 fn.
     - `bt/bts/btr/btc` **forme mémoire** (`[mem], reg` : adresse de bit) — ~18 fn.
     - `punpcklbw/punpckhbw` (unpack octets), `cvtps2pd/cvtpd2ps` (conversion
       packée simple↔double) — ~15 fn.
     - Ops chaîne non-`rep` : `stos/lods/scas/cmps` (single-step) — modélisables
       comme la version `rep` déjà faite.
     - Formes de registre segment/contrôle de `mov/push/pop` (`mov eax,cr0`,
       `push ds`…) — surtout plancher en pratique (registres système).
   - **CHANTIER OPTIONNEL (débloquerait `lahf`/`pushfd`, gros + risqué)** :
     **rendre le modèle de flags exact** (SF/ZF/AF/PF/OF posés width-aware comme
     valeurs brutes par cmp/sub/add/...). Prérequis pour `lahf`/`pushfd`/`pushad`
     *sûrs*. ⚠️ risque de régression sur la reconstruction de conditions de
     `opt` (qui matche des patterns `SF!=OF` etc.) ; **et** ne lève pas la
     non-validabilité des ops 32-bit-only ici (pas d'exécution 32-bit). À ne
     tenter qu'avec un environnement d'exécution 32-bit pour valider.


   **Constat majeur (le plus important de cette série).** Une fois la détection
   d'incomplétude en place, on mesure la *vraie* fidélité sur du vrai code :
   **gzip 98/131, ls 184/200, cat 57/67 fonctions contiennent ≥1 instruction non
   modélisée** (SSE et autres). Autrement dit ARET ne décompile *fidèlement* que
   ~25 % (gzip) / ~8 % (ls) / ~15 % (cat) des fonctions — et le « recompile 100 % »
   **masquait** cela (les fonctions incomplètes compilent quand même : asm →
   commentaire, clobbers → `Undef`). La recompilabilité surestimait le succès ;
   la mesure honnête est le différentiel sur les fonctions *complètes*. Prochain
   levier clair et chiffré : **couverture lifter SSE (§3)** — c'est ce qui débloque
   la majorité du vrai code.
3. **Équivalence symbolique (SMT)** : ✅ **niveau 3 amorcé** (`bench/smt_rewrites.sh`,
   Z3). Chaque règle de réécriture de l'optimiseur (`src/opt`) est **prouvée
   formellement** correcte pour *toutes* les entrées 64 bits : reconstruction
   signée `SF!=OF ⟺ a<s b`, `ZF|(SF!=OF) ⟺ a<=s b`, `ULT|EQ ⟺ a<=u b`,
   négation des relationnels, masque-de-masque, identités, et l'extension de
   signe 32→64 (`signed_cast`). 11/11 prouvé (dont un test sanity attendu `sat`).
   Reste à étendre : lever des **fonctions entières** (sans boucle) en formules et
   demander à un solveur (Z3 via crate `z3`, ou export SMT-LIB) de prouver
   `∀ entrées. orig(x) == recompiled(x)`. **Garantie formelle**, par fonction.
   Faisable pour les fonctions sans boucle / à boucles bornées ; pour le reste,
   se rabattre sur le niveau 2.

### 8.3 Boucle de raffinement

Quand l'équivalence échoue, le diff localise la divergence (quel bloc, quelle
valeur). Cela pilote :
- le choix de la passe d'optimisation à désactiver/activer,
- la détection d'un lifting incorrect d'instruction (régression test),
- l'escalade vers `Asm` brut pour le fragment fautif (garantit la correction).

### 8.4 Pourquoi c'est décisif

Cela transforme ARET d'un outil « best-effort » en un outil **mesurable et
auto-vérifiant**. La métrique nord (§0.3) tombe directement de ce harness.
C'est aussi un filet anti-régression : chaque amélioration de lifting est
validée par round-trip.

---

## 9. Pilier 7 — Couche neuro-symbolique (`src/llm/`)

L'information détruite par la compilation (noms, commentaires, intention) ne
peut être que **prédite**. Un LLM est l'outil idéal — **mais uniquement
par-dessus une structure déjà vérifiée**, jamais à la place de l'analyse.

### 9.1 Principe : structure prouvée + sémantique plausible

- L'analyse déterministe (piliers 1–6) garantit la **correction** : le code
  fait bien ce que fait le binaire.
- Le LLM **renomme** (`sub_401000` → `parse_header`, `local_18` → `buf_len`),
  **commente**, **propose des types sémantiques** (`int` → `enum State`), et
  **regroupe** les fonctions en fichiers/modules plausibles.
- **Invariant de sûreté** : le LLM ne touche qu'aux *étiquettes* (noms,
  commentaires, types non contraints). Il ne réécrit jamais la logique. Après
  son pass, on **re-vérifie l'équivalence** (§8) : un renommage ne doit pas
  changer la sémantique. Si la sortie LLM ne compile pas / diverge → rejet.

### 9.2 Intégration concrète

- Construire le prompt à partir de l'IR typé (pas du texte brut) : signature
  récupérée, types, chaînes référencées, fonctions appelées (avec leurs noms
  déjà résolus), structure de contrôle.
- Utiliser un modèle Claude récent via l'API Anthropic (le plus capable
  disponible). Traitement par lots, cache de prompt pour le prélude commun
  (types/structs partagés), propagation des noms entre fonctions appelantes et
  appelées (analyse du graphe d'appel pour nommer de façon cohérente).
- Sortie : un *patch de noms/commentaires* appliqué à l'AST C, pas une
  régénération libre du code (qui casserait l'équivalence vérifiée).

### 9.3 Pourquoi c'est « jamais bien fait »

Les décompilateurs LLM actuels (LLM4Decompile, etc.) génèrent du C *de bout en
bout* par le modèle → souvent ni équivalent, ni garanti. Les décompilateurs
classiques (Ghidra/IDA) n'ont pas de couche sémantique. **L'intégration des
deux** — analyse formelle vérifiée pour la correction + LLM pour la lisibilité,
avec re-vérification — est exactement le créneau non couvert.

---

## 10. Pilier 8 — Benchmark et métriques (`bench/`)

Sans mesure, « plus loin que jamais » est invérifiable. À construire **tôt**
(en parallèle du pilier 1) pour piloter par les chiffres.

### 10.1 Corpus

- un ensemble de programmes C/C++ dont on a la **source** (coreutils, zlib,
  SQLite, petits programmes synthétiques couvrant chaque construit) ;
- compilés avec plusieurs (compilateur × niveau d'opti × arch) :
  gcc/clang, -O0/-O1/-O2/-O3, x86/x64 ;
- on garde source + binaire + infos de debug (oracle de noms/types/lignes pour
  *évaluer*, jamais en entrée de la décompilation).

### 10.2 Métriques automatisées

- **re-exécutabilité prouvée** (§0.3) — la principale ;
- **recompilabilité** seule ;
- **précision des types** (vs DWARF) : % de largeurs/signes/pointeurs corrects ;
- **précision des signatures** : arité/types d'arguments corrects ;
- **similarité d'édition** au source (CodeBLEU ou distance d'édition AST) pour
  la lisibilité ;
- **densité de bruit** : `goto`/`__asm__`/casts résiduels par fonction ;
- **couverture** : % de fonctions/instructions liftées sans `Asm`.

### 10.3 CI

Brancher le bench sur un `SessionStart` hook / job CI pour suivre les métriques
à chaque commit et **bloquer les régressions** (cf. la skill
`session-start-hook` du projet pour configurer l'environnement de test).

---

## 11. Roadmap ordonnée (dépendances et jalons)

```
P1 IR SSA typé ───────────────┬─→ P2 Passes SSA (opt) ─┐
                              │                         ├─→ P4 Constructs HN
P8 Benchmark (en parallèle) ──┘   P3 Types (constraints)┘     (switch, sigs, vtables)
                                                              │
                                          P5 Émission C compilable
                                                              │
                                          P6 Recompile + vérif ←── boucle de raffinement
                                                              │
                                          P7 Couche LLM (re-vérifiée)
```

Ordre recommandé d'exécution :

1. **P1 (IR SSA)** + **P8 (bench minimal)** en parallèle. Rien de sérieux n'est
   possible sans IR ; rien n'est mesurable sans bench.
2. **P2 (passes SSA)** : const-prop/folding/DCE/conditions-depuis-flags. Gros
   saut de qualité immédiat, et nettoie le bruit du lifting.
3. **P5 (C compilable)** + **P6 (boucle de vérif)** : dès que la sortie compile,
   la métrique nord devient mesurable et pilote tout le reste.
4. **P4 (switch/sigs/strings)** : déverrouille de vraies fonctions complètes.
5. **P3 (types/agrégats)** : lisibilité structurelle (structs, tableaux).
6. **P7 (LLM)** : la couche sémantique, par-dessus une base vérifiée.

Principe directeur à chaque étape : **mesurer avant/après sur le bench**, et ne
jamais régresser la re-exécutabilité prouvée.

---

## 12. Anti-objectifs et garde-fous (à ne jamais violer)

- **Jamais de sortie incorrecte présentée comme correcte.** Tout fragment non
  prouvé reste en `Asm` brut ou est marqué `/* unverified */`. La confiance de
  l'utilisateur est le seul actif qui ne se reconstruit pas.
- **Pas de promesse de récupérer noms/commentaires d'origine.** On les
  *prédit* (LLM) et on le dit explicitement.
- **La sûreté prime sur la beauté.** Une passe qui rend le code plus joli mais
  pas prouvée sûre est désactivée par défaut (flag opt-in).
- **Chaque transformation est testée par round-trip** (§8) — c'est le filet.
- **Pas de mélange entrée/oracle** : les infos de debug (DWARF) servent à
  *évaluer*, jamais à alimenter la décompilation (sinon le bench ment).

---

## 13. État de l'art (pour situer « plus loin que jamais »)

- **Hex-Rays (IDA)** : microcode IR, très bon, propriétaire, pas de boucle de
  vérification formelle ni de couche sémantique LLM intégrée.
- **Ghidra** : P-Code, open-source, excellent CFG/types, idem pas de round-trip
  vérifié ni de garanties formelles.
- **rev.ng / RetDec** : lift vers LLVM IR, recompilable partiellement, mais
  l'équivalence prouvée par fonction n'est pas le produit.
- **LLM4Decompile et consorts** : génération C end-to-end par LLM ; mesurent la
  re-exécutabilité (bonne idée à reprendre) mais **sans garantie** — le modèle
  peut produire du code plausible mais faux.
- **Verified lifting / decompilation (recherche)** : prouve l'équivalence, mais
  outils de recherche, pas de pipeline complet et utilisable.

> Le créneau non occupé, et donc l'objectif d'ARET : **un pipeline complet,
> open-source, qui combine (a) lifting + SSA + types déterministes, (b) une
> boucle de recompilation avec équivalence prouvée par fonction, et (c) une
> couche LLM re-vérifiée pour la lisibilité.** Aucun outil ne réunit les trois.
> C'est là que « plus loin et mieux que tout ce qui existe » devient concret et
> mesurable.

---

## 14. Premiers pas concrets pour l'agent de code

1. Extraire les dominateurs de `structure/mod.rs` vers `src/cfg/dom.rs` partagé.
2. Créer `src/ir/types.rs` avec `Expr`/`Stmt`/`Ty`/`Location`/`ValueId` (§3.2).
3. Réécrire `lift_insn` pour produire des `Vec<Stmt>` en s'appuyant sur
   `iced-x86 instr_info` (registres/flags lus-écrits) plutôt que sur le split
   textuel.
4. Implémenter la construction SSA (dominance frontier + φ + renommage).
5. Implémenter SCCP (const-prop + branches mortes) et DCE SSA — première passe
   visible.
6. En parallèle : monter `bench/` avec 5–10 binaires + source et un script de
   mesure recompilabilité.
7. Émettre du C compilable minimal et brancher la boucle de round-trip (§8.1)
   au niveau « recompile ».

À partir de là, chaque pilier s'ajoute en mesurant systématiquement l'impact
sur la métrique nord.

---

## 15. Intégration de l'analyse externe (4 rapports croisés)

> Source : `ARET — Analyse complète & Propositions d'améliorations.md` (à la racine,
> 25 propositions détaillées avec esquisses de code). Section de synthèse : ce qui
> est **déjà fait depuis cette analyse**, les corrections, et les **nouveautés**
> intégrées à notre plan.

### 15.1 Corrections (l'analyse était partiellement datée)
- ❌→✅ « `reg_params` jamais peuplé, signatures 64-bit toutes `(void)` » : **fait**.
  `ssa::to_ssa` peuple `reg_params` (ordre SysV) ; `emit` les rend (`sub_x(uint64_t v0)`).
- ✅ Imports **ELF PLT/GOT** : faits (151 sur gzip). Imports PE : faits.
- ✅ **Niveau 3 SMT** : amorcé (`bench/smt_rewrites.sh`, 11/11 règles d'opt prouvées).
  La Prop. 14 (lever des *fonctions entières* en SMT) reste, elle, à faire.

### 15.2 Nouveautés à fort intérêt **non présentes** dans notre roadmap (intégrées)
- **Division/modulo par constante (« magic multiply »)** [Prop. 3, P0] : reconnaître
  `Sar(Mul(x, M), S)` → `x / D` (table magic Granlund-Möller / Hacker's Delight).
  Très fréquent, très illisible sinon. Module `src/opt/divmagic.rs`. Vérifiable
  par différentiel **et** SMT.
- **Idiomes compilateur** [Prop. 5, P1] : `Or(Shl(x,k),Shr(x,32-k))` → `rol(x,k)` ;
  `(x>0)-(x<0)` → `sign(x)` ; clamp ; popcount logiciel. Module `src/opt/idioms.rs`.
- **Reconstruction `for`/`do-while`** [Prop. 17b] : à partir des boucles naturelles
  (déjà détectées) + variable d'induction → `for(i=…;…;i++)`.
- **Annotations de confiance** [Prop. 18, effort minimal] : commenter chaque fonction
  `/* confidence: HIGH (SMT) / MEDIUM (diff) / LOW (asm résiduel) */`.
- ✅ **Parallélisme Rayon** [Prop. 19] FAIT (analyse 60→17s, split 44k fn 73→16s, verify parallèle) ; : `functions.par_iter()` dans emit/verify
  (fonctions indépendantes, `prog` en lecture seule) → ~50 s → ~8 s.
- **Backlog d'outillage (P4)** : export JSON/AST + HTML interactif [22], export
  **LLVM IR** [23], serveur **LSP** [24], **binary diff** sémantique [25],
  **taint analysis** [20], **ARM64** [21] (l'IR est déjà arch-agnostique).

### 15.3 Confirmations (déjà dans notre roadmap, priorités affinées par l'analyse)
- **P0 — Args aux sites d'appel** [Prop. 2b] : lire les registres d'arg vivants au
  call-site → peupler `Call.args`. Débloque `func(a,b)` et l'inter-procédural.
- **P0 — Basculer le défaut sur le pipeline IR** [Prop. 1] : MAIS nuance — l'émission
  IR doit d'abord atteindre la **parité de lisibilité** (chaînes inline, switch,
  noms de frame vars, types) que le pipeline texte a déjà. Sinon bascule = régression
  de lisibilité. → polir l'émission IR **avant** de basculer ; garder `--legacy`.
- **P2** — types (§5), alias+promotion (§4.1), SCCP/GVN (§4), switch→`Stmt::Switch` IR.
- **P2/P3** — LLM (§9, nécessite API), FLIRT (§6.4), vtables C++ (§6.3), inter-proc (§6.2).

### 15.4bis — Fait depuis l'intégration de §15 (mise à jour 2025-06-14)
- ✅ **Rayon** (analyse 60→17 s, split 44k 73→16 s, verify parallèle).
- ✅ **Lifter `mul`/`div`/`idiv` 1-op (32-bit) + `cdq`/`cqo`** (3 bugs trouvés+
  corrigés par le différentiel → corpus 21/21).
- ✅ **Modélisation des appels** : retour (`rax = call`) + clobbers caller-saved.
- ✅ **Noms d'imports dans le pipeline IR** + `verify` en `-fno-builtin`.
- ✅ **Args aux call-sites (64-bit)** [Prop. 2b] : le lifter sur-approxime
  l'appel en lisant les 6 registres d'arg SysV ; une passe `prune_call_args`
  (opt) supprime les args `Undef` traînants ; un *fixup inter-procédural* à
  l'émission (`emit::fixup_call_arity`) ajuste chaque appel d'une fonction
  définie dans l'unité à l'arité de son callee (`func(a, b)` au lieu de
  `func()`), et une table d'arités libc nettoie les imports (`strlen(s)`,
  `memcpy(d,s,n)`). Émission toujours avec vraies signatures + fwd-decls
  empty-paren → gzip recompile **131/131**, différentiel **21/21**, SMT 11/11.
  Reste : **args 32-bit cdecl sur la pile** ; matching positionnel exact quand
  les `reg_params` du callee ne sont pas un préfixe contigu.
- ✅ **Magic division (unsigned 32-bit)** [Prop. 3] : `opt::try_magic_udiv`
  reconnaît `(x * M) >> t` (t≥32, forme high-multiply sans correction) et
  recouvre `x / d`. `d` est **auto-validé** : on recalcule le magic canonique de
  `d` (`opt::magicu32`, Hacker's Delight Fig. 10-1) et on exige un match exact
  `(M, t-32, add=false)` → aucune séquence non-divisive n'est jamais réécrite.
  Vérifié : équivalence **exhaustive sur les 2^32 entrées** (`bench/magicdiv.sh`,
  9 diviseurs, 8/9 réécrits, l'add-form restant resté correct) + 2 tests
  unitaires. (Z3 bit-blaste le `bvudiv` 32-bit symbolique et ne converge pas →
  l'exhaustif est ici la preuve plus forte et plus rapide.)
- ✅ **Deux bugs de correction corrigés** (principe « jamais de code faux ») :
  (a) la **mémoire à override de segment** (`fs:`/`gs:`, p.ex. canary `fs:0x28`)
  était liftée comme un load absolu (segment perdu) → `mem_addr`/`frame_disp`
  abandonnent désormais sur un préfixe de segment → `Stmt::Asm` honnête ;
  (b) `Unary(SignExtend)` était émis en identité → `movsx`/`movsxd` zéro-
  étendaient au lieu d'étendre en signe → émission via cast signé **conscient de
  la largeur** (`signed_cast` gère aussi les loads sous-mot via pointeur signé).
- ✅ **Lifter élargi** : registres **high-byte** (`ah/bh/ch/dh`, lecture
  `(r>>8)&0xff` + écriture partielle exacte), `leave`, `cbw`/`cwde`/`cdqe`
  (extension de signe de l'accumulateur). Fait tomber les fallbacks `asm` de
  `movzx`/`movsx`/`test`/`leave`/`cdqe`. Différentiel élargi à **26/26**
  (high-byte, sign-ext, index cdqe ajoutés au corpus).

### 15.4 Ordre d'exécution recommandé (révisé)
1. ✅ **Magic division (unsigned 32-bit)** [Prop. 3] — `(x * M) >> t` → `x / d`,
   recouvrement *auto-validé* (Hacker's Delight `magicu` recalculé, match exact
   requis), équivalence **exhaustive 2^32** (`bench/magicdiv.sh`).
   *(fait, cf. 15.4bis)*.
   Reste : magic **signée**, magic **64-bit**, reconstruction du **modulo**
   (`x - (x/d)*d` → `x % d`).
2. ✅ **Args aux call-sites** [Prop. 2b] — `func(a, b)` au lieu de `func()` *(fait, cf. 15.4bis)*.
3. ⏳ **Compléter le lifter** [Prop. 4] + **idiomes** [Prop. 5] — moins d'`Asm`.
   Fait : high-byte, `leave`, `cbw/cwde/cdqe`, correction segment + sign-ext.
   Reste : `rol`/`ror`, `adc`/`sbb`, `rep movs/stos`→`memcpy`/`memset`, SSE/float
   (`movaps`/`pxor`/`mulsd`/`cvtsi2sd`… — le plus gros bloc restant).
4. **Inférence de types** [Prop. 6/§5] — le grand saut (largeur/signe → ptr → structs).
5. **Polissage émission IR** (chaînes/switch/confiance) puis **bascule défaut** [Prop. 1].
6. **SCCP/GVN** [Prop. 8/9], **vtables** [Prop. 13/§6.3], **LLM** [Prop. 11/§9].
7. Backlog P4 (Rayon tôt car trivial ; ARM64/LSP/diff/taint ensuite).
