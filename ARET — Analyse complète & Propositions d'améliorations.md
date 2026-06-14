# ARET — Analyse complète & Propositions d'améliorations

> Document unifié compilé depuis quatre analyses indépendantes du codebase (~7 000 lignes Rust).
> Référentiels parcourus : `main.rs`, `loader/`, `disasm/`, `analysis/`, `ir/types.rs`,
> `ir/lift.rs`, `ir/build.rs`, `ssa/mod.rs`, `opt/mod.rs`, `emit/mod.rs`,
> `emit/structured.rs`, `structure/mod.rs`, `dataflow/mod.rs`, `decompile/mod.rs`,
> `verify/mod.rs`, `cfg/dom.rs`, `bench/`, `ROADMAP.md`.

---

## 1. État du projet — Vue honnête

### Architecture actuelle (deux pipelines en parallèle)

```
                ┌─── Voie texte (pipeline par défaut) ─────────────────────────┐
                │  loader → disasm → analysis → decompile/structure → sortie C │
                │  (dataflow : manipulation textuelle de chaînes C)             │
                └──────────────────────────────────────────────────────────────┘
binary ───────────────────────────────────────────────────────────────────────── → sortie
                ┌─── Voie IR (pilier de la roadmap, en cours) ──────────────────┐
                │  loader → disasm → analysis → ir/build → ssa → opt           │
                │  → emit/structured → verify                                   │
                └──────────────────────────────────────────────────────────────┘
```

Le pipeline **texte** reste le défaut utilisateur. Le pipeline **IR** est déjà supérieur techniquement
mais n'est pas encore exposé par défaut. C'est le premier chantier à finir.

### Ce qui fonctionne vraiment bien

| Module | Qualité | Notes |
|---|---|---|
| `loader/mod.rs` | ★★★★★ | PE/ELF/Mach-O propres, vue uniforme, PLT/IAT résolus, 151 imports ELF sur gzip strippé, chaînes annotées |
| `disasm/mod.rs` | ★★★★★ | `iced-x86`, classification de flot correcte, 3 M instructions / ~50 s sur 27 MB |
| `analysis/mod.rs` | ★★★★☆ | Descente récursive + scan prologue, jump tables, 43 k fonctions sur jeu de 27 MB |
| `cfg/dom.rs` | ★★★★★ | Cooper-Harvey-Kennedy + frontières de Cytron — code propre et testé |
| `ir/types.rs` | ★★★★☆ | IR expressif, deux formes (pré-SSA + SSA), `Expr::Select`, flags explicites |
| `ir/lift.rs` | ★★★★☆ | Basé sur `iced-x86` structured API, `asm_fallback` sain via `instr_info` ; mov, add/sub/and/or/xor, lea, cmp/test, setcc, cmov, imul 2/3-op, shl/shr/sar, push/pop, call/ret |
| `ssa/mod.rs` | ★★★★☆ | Cytron complet, φ-placement + renommage, φ-nodes vérifiés sur diamant CFG, params SysV/Win64 détectés |
| `opt/mod.rs` | ★★★★☆ | Propagation + folding + DCE, reconstruction de conditions signées/non-signées (`SF!=OF → <s`, `CF|ZF → <=u`), `factorial` : B2 passe de ~14 → 4 stmts |
| `emit/structured.rs` | ★★★☆☆ | Structuré `if`/`while`/`break`, 2 000/2 000 fonctions du jeu recompilent, dégradation goto sûre |
| `verify/mod.rs` | ★★★★☆ | Niveau 1 : recompilabilité 100% via `cc -x c - -c` sans fichier temporaire |
| `bench/difftest.sh` | ★★★★☆ | Niveau 2 : équivalence différentielle 16/16 (50 k entrées aléatoires, bug trouvé et corrigé) |
| Strings recovery | ★★★★★ | 50 940 chaînes dans 12 708 fonctions |
| Switch/jump tables | ★★★☆☆ | Résolution CFG correcte (75 cas sur le jeu), cibles ajoutées au CFG |
| `dataflow/mod.rs` | ★★☆☆☆ | Fonctionne (liveness, DCE, propagation mono-usage) mais manipulation de strings — plafond architectural |

### Ce qui manque (les vrais trous)

| Manquant | Impact sur la qualité | Priorité |
|---|---|---|
| Inférence de types par contraintes | `*(uint64_t*)(rdi+8)` reste illisible | 🔴 Critique |
| Récupération args registres (SysV/Win64) | signatures `(void)` sur x64 | 🔴 Critique |
| Arguments aux sites d'appel | `func()` au lieu de `func(rdi, rsi)` | 🔴 Critique |
| Division par constante (magic multiply) | `(x * 2454267027) >> 37` illisible | 🔴 Critique |
| `mul`/`idiv`/`div` 1-opérande | → Asm fallback sur ~30% des fonctions non-triviales | 🟠 Haute |
| Alias analysis + promotion stack | `Frame(d)` non traversable par le propagateur | 🟠 Haute |
| GVN (Global Value Numbering) | expressions redondantes dupliquées | 🟠 Haute |
| SCCP (Sparse Conditional Const Prop) | branches mortes non élaguées | 🟠 Haute |
| Couche LLM de nommage | output reste `sub_401000`, `local_4` | 🟠 Haute |
| Switch complet avec expression d'index | `Stmt::Switch` mal connecté à l'IR | 🟡 Moyenne |
| Signatures de bibliothèque (FLIRT-like) | `memcpy`/`malloc` jamais reconnus statique | 🟡 Moyenne |
| Analyse C++ (vtables, RTTI) | code OO opaque | 🟡 Moyenne |
| Vérification SMT / Z3 (niveau 3) | pas de preuve formelle par fonction | 🟡 Moyenne |
| Calling conventions complètes (float, varargs) | `xmm0` retour non détecté | 🟡 Moyenne |
| Idiomes compilateur (rotation, sign, clamp) | patterns compilés non reconnus | 🟡 Moyenne |
| Recovery structs/tableaux | agrégats non reconstruits | 🟡 Moyenne |
| Analyse inter-procédurale | pas de propagation de signatures sur graphe d'appel | 🟡 Moyenne |
| Variables globales nommées | adresses `.data`/`.bss` non annotées | 🟡 Moyenne |
| Boucles `for`/`do-while` | reconstruction syntaxique non faite | 🟡 Moyenne |
| SIMD/SSE lifting | → Asm fallback massif | 🟡 Moyenne |
| Registres hauts (`ah/bh/ch/dh`) | → Asm fallback (rare mais présent) | 🟡 Moyenne |
| Traitement parallèle (rayon) | pipeline entièrement séquentiel | 🟢 Faible |
| ARM64 / AArch64 | x86/x64 only | 🟢 Faible |
| Taint analysis | pas de recherche de vulnérabilités | 🟢 Faible |
| Export JSON/AST + HTML interactif | sortie texte C uniquement | 🟢 Faible |
| Export LLVM IR | pas d'interopérabilité downstream | 🟢 Faible |
| LSP server | pas d'intégration IDE | 🟢 Faible |
| Binary diff mode | pas de comparaison avant/après patch | 🟢 Faible |
| Annotations de confiance | niveau de certitude non affiché | 🟢 Faible |

**Lacunes architecturales constatées directement dans le code :**
1. `IrFunction.reg_params` déclaré mais jamais peuplé → signatures 64-bit toutes `(void)`.
2. `is_versioned()` dans `ssa/mod.rs` exclut `Frame(d)` et `Mem` → slots de pile non promus en SSA (délibéré mais bloquant).
3. Dans `ir/lift.rs` ligne ~400, le `Call` est créé avec `args: Vec::new()` → zéro argument à tous les call-sites.
4. Le champ `Ty` de `IrFunction` n'est jamais peuplé après lifting — toujours `Ty::Unknown` ou `Ty::int(64)`.
5. `analysis/mod.rs` résout les jump-tables et ajoute les arêtes au CFG, mais `ir/build.rs` ignore `jump_tables` : les `Flow::Indirect` deviennent `Stmt::Asm`.

---

## 2. Propositions d'améliorations — Priorisées et détaillées

---

### 🔴 Proposition 1 — Basculer le défaut sur le pipeline IR

**Impact : ★★★★★ | Effort : ★★☆☆☆ | Priorité : P0**

Le pipeline IR atteint déjà 100% de recompilabilité. La sortie structurée (`emit/structured.rs`) produit du `if`/`while` propre. Garder `--mode decompile` sur le pipeline texte est conservateur mais freine tous les autres chantiers. Le pipeline texte (`dataflow/mod.rs`, ~900 lignes de manipulation de strings) peut être progressivement rétrogradé en `--legacy` puis supprimé, libérant ~2 000 lignes à maintenir.

```rust
// main.rs — Mode::Decompile
Mode::Decompile => {
    // Ancien : render_function (texte)
    // Nouveau :
    let irfs: Vec<_> = functions.iter().map(|f| {
        let mut irf = ir::build::build_ir(&prog, f);
        ssa::to_ssa(&mut irf);
        opt::optimize(&mut irf);
        irf
    }).collect();
    out.push_str(&emit::structured::emit_unit(&irfs));
}
```

**Garde-fou** : conserver `--legacy` pour forcer le pipeline texte pendant la transition.

---

### 🔴 Proposition 2 — Récupération des signatures de fonctions (ABI x64) et arguments aux call-sites

**Impact : ★★★★★ | Effort : ★★★☆☆ | Priorité : P0**

C'est le changement le plus visible immédiatement. Deux sous-problèmes liés :

**2a. Paramètres reçus (entrée de fonction)**

Le SSA détecte déjà les `undef` en entrée de fonction, mais ne les transforme pas en vrais paramètres.

```rust
// src/recover/callconv.rs

/// Registres d'arguments System V AMD64
const SYSV_INT_ARGS: [RegId; 6] = [RegId(7), RegId(6), RegId(2), RegId(1), RegId(8), RegId(9)];
//                                    rdi       rsi       rdx       rcx      r8        r9

/// Pour chaque fonction : un registre est un paramètre si et seulement si
/// il est LU (Read) avant d'être ÉCRIT (Set) sur tout chemin depuis l'entrée.
/// En SSA : toute Read(Reg(x)) dans le bloc d'entrée sans définition antérieure = paramètre.
fn recover_params(func: &mut IrFunction) {
    let entry_reads = regs_read_before_written_at_entry(func);
    for (i, &reg) in SYSV_INT_ARGS.iter().enumerate() {
        if entry_reads.contains(&Location::Reg(reg)) {
            func.reg_params.push(i as u32);
        }
    }
}

/// Pour les fonctions avec retour en xmm0 (float/double) :
fn detect_float_return(func: &IrFunction) -> bool {
    // Cherche une définition de Location::Reg(RegId(16)) // xmm0
    // qui survit au Return(...)
}

/// Win64 : rcx, rdx, r8, r9 (pas rsi, rdi)
/// Varargs : al = nombre de registres XMM utilisés (SysV)
fn calling_convention(func: &IrFunction, prog: &Program) -> CallingConvention {
    match prog.format.as_str() {
        "PE" => CallingConvention::Win64,
        _ => CallingConvention::SystemV,
    }
}
```

À l'émission, `emit/structured.rs` doit lire `func.reg_params` :
```c
// Avant : uint64_t sub_401000(void) { ...
// Après : uint64_t sub_401000(uint64_t rdi_0, uint64_t rsi_0, uint64_t rdx_0) { ...
```

**2b. Arguments aux sites d'appel**

Dans `ir/build.rs`, lors du lifting d'un `Stmt::CallStmt`, analyser les `Assign`/`Set` dans le même bloc qui écrivent dans les registres d'argument **avant** le call, et injecter les versions SSA de ces registres comme `args`.

```rust
// src/recover/callconv.rs
/// Sur les sites d'appel, lire quels registres argument sont vivants
/// juste avant le call → reconstruire la liste d'arguments.
pub fn recover_call_args(func: &mut IrFunction, targets: &FunctionDb) {
    // Pour chaque Stmt::CallStmt(Call { args: [], .. }) :
    // regarder les Use(v) des registres SysV vivants au call-site
    // → peupler args[].
}

// Après SSA, les versions des registres SysV au point du call sont connues :
// rdi → v42, rsi → v17, rdx → v9, etc. → injecter comme args du Call.
Expr::Call { target, args: vec![
    Expr::Use(rdi_version),
    Expr::Use(rsi_version),
    ...
], ret }
```

**Version inter-proc (plus tard)** : dataflow global de liveness des paramètres (quels regs sont lus-avant-écrits dans le callee) → supprimer les args inutilisés.

```rust
/// Propagation inter-procédurale des signatures
/// Si sub_401000 est toujours appelé avec 2 args connus → affiner la signature
pub fn interprocedural_sig_recovery(funcs: &mut [IrFunction]) { ... }
```

**Résultat visible :**
```c
// Avant : uint64_t sub_401000(void) { ... sub_401100(); }
// Après : uint64_t parse_header(User* obj, int count) { ... process(obj, count); }
```

---

### 🔴 Proposition 3 — Démagie de la division par constante (magic multiply)

**Impact : ★★★★☆ | Effort : ★★☆☆☆ | Priorité : P0**

`x / 7` compilé en `-O1` donne `imul rax, rdx, 2454267027; sar rax, 37`. Actuellement → Asm fallback ou expression arithmétique illisible. Très fréquent dans les boucles, algorithmes de hachage, parseurs.

**Solution — pattern matching sur l'IR (`src/opt/divmagic.rs`) :**

```rust
/// Identifier le pattern de division-par-constante :
///   r = (x * M) >> S  où M est un "magic number" (Hacker's Delight §10).
/// Récupère le vrai diviseur D.
pub fn match_div_magic(expr: &Expr) -> Option<(Expr, i128)> {
    // Pattern : Sar(Mul(x, M), S) ou Shr(Mul(x, M), S)
    // Vérifier que (M, S) correspond à un diviseur D par la formule inverse de Granlund-Möller.
    // Si oui → return Some((x, D))
}

fn recognize_div_by_const(mul_magic: i128, sar_shift: u32) -> Option<i128> {
    // Pour chaque diviseur d de 2 à 65536, calculer le magic number
    // et comparer à mul_magic avec la formule de Warren §10-9.
    for d in 2..=65536i128 {
        if compute_magic(d, sar_shift) == mul_magic {
            return Some(d);
        }
    }
    None
}
// Transformation : Sar(Mul(x, magic), shift) → SDiv(x, d)

/// Même chose pour le modulo : x % D = x - D*(x/D)
pub fn match_mod_magic(expr: &Expr) -> Option<(Expr, i128)> { ... }
```

**Table des magic numbers** (statique, ~50 entrées couvrent 99% des cas) : `(magic, shift) → diviseur` pour diviseurs 2–100, signés et non-signés.

**Appel depuis `opt::optimize()`** après la passe de folding.

---

### 🔴 Proposition 4 — Compléter le lifter : `mul`/`idiv`/`div` + rotations + SSE de base

**Impact : ★★★☆☆ | Effort : ★★☆☆☆ | Priorité : P1**

Ces instructions tombent systématiquement en `Asm` fallback. Le `asm_fallback` est correctement implémenté (il lit les `instr_info` d'`iced` pour les clobbers) — elles ne cassent rien mais génèrent du bruit visible.

**Instructions non liftées :**

| Catégorie | Instructions |
|---|---|
| Division/multiplication large | `div`, `idiv`, `mul` 1-opérande (résultat double-registre `rdx:rax`) |
| Rotation/carry | `rol`, `ror`, `rcl`, `rcr`, `bt`, `bts`, `btr`, `bswap` |
| Chaînes | `rep movsd`, `rep stosd`, `rep scasb` |
| Atomiques | `xchg`, `lock cmpxchg`, `lock add` |
| SSE/AVX de base | `movaps`, `movdqu`, `paddd`, `pxor`, `psrldq`, `xorps xmm0,xmm0` |
| Contrôle | `cpuid`, `rdtsc` (pas critique, fallback correct) |
| Registres hauts | `ah/bh/ch/dh` (rare mais présent dans le code hérité) |

**Lifting `mul`/`idiv`/`div` dans `ir/lift.rs` :**

```rust
// imul rax (1-op, double-width) : RDX:RAX = RAX * src
Mnemonic::Imul if ins.op_count() == 1 => {
    let src = some_or_asm!(op_value(ins, 0));
    let rax = Location::Reg(RegId(0));
    let rdx = Location::Reg(RegId(2));
    // Cast 64-bit avant multiplication pour capturer les 64 bits
    let product = Expr::Binary(
        BinOp::Mul,
        Box::new(Expr::Cast { to: Ty::int(64), expr: Box::new(Expr::Read(rax.clone())) }),
        Box::new(Expr::Cast { to: Ty::int(64), expr: Box::new(src) }),
    );
    let tmp = Location::Temp(ctx.fresh());
    vec![
        Stmt::Set { dst: tmp.clone(), expr: product },
        Stmt::Set { dst: rax.clone(), expr:
            Expr::Binary(BinOp::And, Box::new(Expr::Read(tmp.clone())), Box::new(konst(0xffffffff)))
        }, // low 32 bits
        Stmt::Set { dst: rdx, expr:
            Expr::Binary(BinOp::Shr, Box::new(Expr::Read(tmp)), Box::new(konst(32)))
        }, // high 32 bits
    ]
}

// div src : quotient dans RAX, reste dans RDX
Mnemonic::Div => {
    let src = some_or_asm!(op_value(ins, 0));
    let rax = Location::Reg(RegId(0));
    let rdx = Location::Reg(RegId(2));
    vec![
        Stmt::Set { dst: rax.clone(), expr: bin(BinOp::UDiv, Expr::Read(rax.clone()), src.clone()) },
        Stmt::Set { dst: rdx, expr: bin(BinOp::UMod, Expr::Read(rax), src) },
    ]
}

// idiv src (signed) — reconstruire le dividende rdx:rax
Mnemonic::Idiv => {
    let src = some_or_asm!(op_value(ins, 0));
    let edx_shifted = Expr::Binary(BinOp::Shl, Box::new(read_reg(Register::EDX)?), Box::new(konst(32)));
    let dividend = Expr::Binary(BinOp::Or, Box::new(edx_shifted), Box::new(read_reg(Register::EAX)?));
    let rax = Location::Reg(RegId(0));
    let rdx = Location::Reg(RegId(2));
    vec![
        Stmt::Set { dst: rax.clone(), expr: Expr::Binary(BinOp::SDiv, Box::new(dividend.clone()), Box::new(src.clone())) },
        Stmt::Set { dst: rdx, expr: Expr::Binary(BinOp::SMod, Box::new(dividend), Box::new(src)) },
    ]
}
```

**`rol`/`ror`** : introduire `BinOp::Rol` / `BinOp::Ror` dans `ir/types.rs` et les émettre en C comme `__builtin_rotateleft32` / `_rotl` (disponibles sur GCC et MSVC).

**`rep movs` / `rep stos`** : émettre un `memcpy` / `memset` intrinsèque — la sémantique est identique pour les usages courants du compilateur.

**SSE de base (`xorps xmm0, xmm0`)** : idiome de mise à zéro → `Set xmm0, Const(0)`.

---

### 🔴 Proposition 5 — Reconnaissance d'idiomes compilateur

**Impact : ★★★★☆ | Effort : ★★☆☆☆ | Priorité : P1**

Le compilateur génère des patterns non-évidents que ARET doit reconnaître. Module `src/opt/idioms.rs` :

**5a. Rotation synthétisée (rol/ror via shl+shr+or)**
```
mov ecx, eax
shl eax, k
shr ecx, (32-k)
or  eax, ecx
→ rol(eax, k)
```
Pattern IR : `Or(Shl(x, k), Shr(x, 32-k))` → `Rol(x, k)`. Émettre `__builtin_rotateleft32`.

**5b. Signe de x (`sign(x)`)**
Pattern : `(x > 0) - (x < 0)` → annoter `sign(x)` avec helper C.

**5c. Clamp / saturate**
Pattern : `x < min ? min : (x > max ? max : x)` → `clamp(x, min, max)`.

**5d. Test de parité / popcount**
Pattern software : boucle `while(x) { c += x&1; x >>= 1; }` → annoter `/* popcount */` ou détecter `popcnt`.

---

### 🟠 Proposition 6 — Inférence de types par contraintes

**Impact : ★★★★★ | Effort : ★★★★☆ | Priorité : P2**

C'est le saut de qualité majeur : transformer `uint64_t x = ...` en `int32_t count`, `uint8_t flag`, `struct Node *node`. Nouveau module `src/types/infer.rs`.

**Architecture (génération de contraintes + union-find) :**

```rust
pub enum TyConstraint {
    /// L'expression est chargée/stockée à une largeur de `bits` bits.
    Width { value: ValueId, bits: u8 },
    /// Comparaison signée → l'opérande est un entier signé.
    Signed(ValueId),
    /// Comparaison non-signée → l'opérande est un entier non-signé.
    Unsigned(ValueId),
    /// L'expression est utilisée comme adresse d'un `Load`/`Store`.
    IsPointer(ValueId),
    /// Type pointé.
    PtrPointsTo(ValueId, Ty),
    /// Deux valeurs ont le même type (unification).
    Unify(ValueId, ValueId),
    /// Accès agrégat : la base est accédée à offset `k` en largeur `w`.
    FieldAt { base: ValueId, offset: i64, width: u8 },
    /// Stride constant → tableau.
    ArrayOf { base: ValueId, stride: u64, elem_ty: Ty },
    /// Prototype connu (libc) → contraintes sur chaque arg.
    CalleeSig { addr: u64, param: usize, ty: Ty },
    /// Division signée.
    SignedDiv(ValueId),
    /// Utilisation en SSE → float.
    Float(ValueId),
}
```

**Génération de contraintes — règles les plus rentables :**

| Construit IR | Contrainte générée |
|---|---|
| `Load { ty: Ty::int(32), addr }` | `Width(value_du_load, 32)`, `IsPointer(addr)` |
| `Binary(Slt/Sle/Sgt/Sge, a, b)` | `Signed(a)`, `Signed(b)` |
| `Binary(Ult/Ule/Ugt/Uge, a, b)` | `Unsigned(a)`, `Unsigned(b)` |
| `Binary(Add, base, Const(8, _))` puis `Load` | `FieldAt { base, offset: 8, .. }` |
| `Binary(Add, base, Binary(Mul, idx, Const(stride)))` | `ArrayOf { base, stride, .. }` |
| `Unary(SignExtend, x)` | source est un entier signé plus étroit |
| `Call { target: Named("strlen"), args: [v] }` | `IsPointer(v)`, `PtrPointsTo(v, Char)` |
| `Call { target: Named("malloc"), args: [v] }` | `v : size_t`, retour : `void*` |
| Arg 0 de `printf@plt` | `char*` |
| `idiv` | dividende et diviseur `Signed` |
| `Phi(args)` | `Unify(dst, arg_i)` pour chaque argument |

**Résolution** : union-find sur `ValueId` → type le plus précis satisfaisant toutes les contraintes. En cas de conflit (`Signed` + `Unsigned`) → `uint64_t` (conservateur, jamais faux).

**Reconstruction de structs :**
```rust
/// Agrège les FieldAt sur la même base → structure synthétique
pub fn reconstruct_structs(env: &TypeEnv) -> Vec<SynthStruct> {
    // Pour chaque base : ordonner les offsets, déduire les paddings
}
```

**Émission C :**
```c
// Avant inférence de types :
uint64_t sub_401000(uint64_t v0) {
    return (*(uint64_t*)((*(uint64_t*)(v0 + 8)) + 16));
}

// Après inférence de types :
struct s_v0 { uint8_t pad[8]; struct s_inner* field_8; };
struct s_inner { uint8_t pad[16]; uint64_t field_10; };
uint64_t sub_401000(struct s_v0* v0) {
    return v0->field_8->field_10;
}
```

**Gain immédiat** : `uint64_t v12 = (v3 & 0xffff)` devient `uint16_t v12 = ...`.

---

### 🟠 Proposition 7 — Alias analysis + promotion des slots de pile en SSA

**Impact : ★★★★☆ | Effort : ★★★☆☆ | Priorité : P2**

Prérequis du ROADMAP §4.1, intentionnellement différé. L'objectif est de promouvoir les `Frame(d)` non-aliasés en vraies variables SSA traversables par le propagateur.

Un slot `Frame(d)` est potentiellement aliasé si `lea rbp, [rbp-d]` prend son adresse et l'écrit dans un registre non tracé.

**Passe 1 — Classification des accès frame :**

```rust
// src/opt/alias.rs
pub enum AliasClass {
    Frame(i64, u32),   // slot à offset d, largeur w bits : disjoint de Frame(d2,_) si d≠d2
    Heap,              // allocation heap (malloc)
    Global(u64),       // adresse statique connue
    Unknown,           // tout le reste (conservateur)
}

pub fn may_alias(a: &AliasClass, b: &AliasClass) -> bool {
    match (a, b) {
        (Frame(d1, w1), Frame(d2, w2)) => {
            let end1 = d1 + (*w1 / 8) as i64;
            let end2 = d2 + (*w2 / 8) as i64;
            d1 < &end2 && d2 < &end1   // chevauchement des plages
        }
        (Frame(..), Heap) | (Heap, Frame(..)) => false,
        (Global(a), Global(b)) => a == b,
        _ => true, // conservateur
    }
}

fn slot_is_aliased(func: &IrFunction, disp: i64) -> bool {
    // Un slot Frame(d) est aliasé si son adresse est prise via Expr::Addr(Frame(d))
    // ou via `lea [rbp + d]` (qui produit Addr(Frame(d)) dans le lifter).
    for b in &func.blocks {
        for s in &b.stmts {
            if expr_takes_addr_of_frame(s, disp) { return true; }
        }
    }
    false
}
```

**Passe 2 — Promotion :**
Les slots `Concrete` (non-aliasés) sont convertis en `Location::Temp(fresh_id)` avant la construction SSA → ils passent par le renommage Cytron comme n'importe quelle variable → deviennent des `ValueId` SSA normaux → propagateur les traverse librement.

**Gain** : dans les fonctions simples (la majorité), `arg_8` et `local_4` disparaissent, remplacés par un flot de valeurs SSA proprement nommées.

---

### 🟠 Proposition 8 — SCCP : Sparse Conditional Constant Propagation

**Impact : ★★★☆☆ | Effort : ★★★☆☆ | Priorité : P2**

Le propagateur actuel (`opt/mod.rs`) est inconditionnel : il substitue les constantes mais ne supprime pas les blocs morts. Si `eax = 0 ; if (eax != 0) { ... }` — le branchement est mort mais n'est pas élagué. SCCP (Wegman-Zadeck 1991) le fait en une seule passe en simulant l'exécution sur un treillis {Undef, Const(v), Top}.

```rust
enum Lattice { Undef, Const(i128), Top }

pub struct ScCpState {
    /// Treillis par ValueId : None = Top, Some(Some(v)) = Const(v), Some(None) = Bottom
    lattice: HashMap<u32, Option<i128>>,
    /// Arêtes CFG dont l'exécutabilité a été résolue
    exec_edges: HashSet<(u32, u32)>,
    block_reachable: Vec<bool>,
    ssa_worklist: VecDeque<u32>,
    cfg_worklist: VecDeque<(u32, u32)>, // (pred_block, succ_block)
}
```

**Algorithme** : pour chaque arête exécutable, propager les définitions ; pour chaque branche dont la condition est une constante → marquer l'arête morte → couper le bloc.

**Impact** : élimine les dead branches (`if (0) { ... }`), le code mort compilateur (inline guards, debug assertions), réduit le bruit après inlining de constantes. Impossible avec la propagation actuelle.

---

### 🟠 Proposition 9 — GVN : Global Value Numbering

**Impact : ★★★☆☆ | Effort : ★★☆☆☆ | Priorité : P2**

Des sous-expressions identiques calculées dans des blocs dominés différents ne sont pas fusionnées. Impact : redondances visibles dans les fonctions avec beaucoup de `lea`/offsets constants.

```rust
/// Hash canonique d'une expression (indépendant des ValueId post-SSA)
fn value_number(e: &Expr, table: &mut GvnTable) -> u32 {
    match e {
        Expr::Const(v, _) => hash_const(*v),
        Expr::Binary(op, a, b) => {
            let na = value_number(a, table);
            let nb = value_number(b, table);
            // Normalisation : opérandes commutatifs triés
            let key = if is_commutative(*op) && na > nb { (*op, nb, na) } else { (*op, na, nb) };
            table.intern(key)
        }
        // etc.
    }
}
```

**Exemple concret :**
```c
// Avant GVN (strlen calculé deux fois)
if (strlen(s) > 10) { use(strlen(s)); }
// Après GVN
uint64_t v_len = strlen(s);
if (v_len > 10) { use(v_len); }
```

---

### 🟠 Proposition 10 — Connecter les jump-tables à `Stmt::Switch` dans l'IR

**Impact : ★★★☆☆ | Effort : ★★☆☆☆ | Priorité : P2**

`analysis/mod.rs` retourne déjà `jump_tables: HashMap<u64, Vec<u64>>` et résout correctement les cibles (75 cas sur le jeu). Mais dans `ir/build.rs`, les sauts indirects (`Flow::Indirect`) deviennent `Stmt::Asm`. Le chemin IR ignore totalement les switch récupérés.

**Fix — passer `jump_tables` à `build_ir` :**

```rust
// ir/build.rs
Flow::Indirect if jump_tables.contains_key(&blk.insns.last().unwrap().address) => {
    let targets = &jump_tables[&addr];
    let cases: Vec<(i128, BlockId)> = targets.iter()
        .enumerate()
        .filter_map(|(i, t)| idx.get(t).map(|&bid| (i as i128, BlockId(bid))))
        .collect();
    stmts.push(Stmt::Switch {
        // L'expression de switch : pattern cmp idx, N ; ja default ; jmp [table+idx*8]
        // Récupérer l'expression d'index depuis les stmts précédant le jmp
        // (le `cmp` précédent + opt SSA raffine vers la vraie expression d'index)
        value: Expr::Read(Location::Reg(RegId(0))), // placeholder raffiné par opt
        cases,
        default: BlockId(cases.len() as u32),
    });
}
```

**Note** : compléter aussi `Stmt::Switch { value: Expr, ... }` pour capturer la vraie expression d'index (la variable testée dans le `cmp idx, N` précédant le jump table).

---

### 🟠 Proposition 11 — Couche LLM de nommage (Claude API)

**Impact : ★★★★☆ | Effort : ★★★☆☆ | Priorité : P2**

La proposition la plus visible pour l'utilisateur final. La distinction fondamentale : l'analyse déterministe donne la **correction** ; le LLM donne la **lisibilité**. Le LLM ne touche qu'aux **étiquettes** (noms, commentaires) — jamais à la logique.

**Architecture (`src/llm/mod.rs`) :**

```rust
pub struct LlmConfig {
    pub model: String,       // "claude-opus-4-5" ou équivalent
    pub api_key: String,
    pub max_tokens: u32,
}

pub struct NamingContext {
    pub signature: FuncSignature,
    pub strings: Vec<String>,         // chaînes littérales référencées
    pub callees: Vec<String>,         // noms déjà résolus
    pub types: Vec<(String, Ty)>,     // types inférés des args et locaux
    pub verified_c: String,           // C émis et vérifié (input au LLM)
}

pub struct NamingPatch {
    /// sub_401234 → parse_http_header
    pub function_name: Option<String>,
    /// local_8 → buf_len, arg_0 → request
    pub renames: HashMap<String, String>,
    /// Types sémantiques proposés : sub_401000.arg_8 → "const char*"
    pub semantic_types: HashMap<String, String>,
    /// Commentaires par bloc : (func_addr, block_id) → commentaire
    pub block_comments: HashMap<(u64, u32), String>,
    /// Commentaire de fonction
    pub doc_comment: Option<String>,
}
```

**Construction du prompt à partir de l'IR (pas du texte brut) :**

```
Tu es un expert en rétro-ingénierie. Voici une fonction C obtenue par décompilation vérifiée
(prouvée sémantiquement équivalente au binaire d'origine).

Fonction : sub_401670
Signature : uint64_t sub_401670(uint64_t arg_8, uint64_t arg_c)
Chaînes référencées : ["Content-Type", "text/html", "400 Bad Request"]
Fonctions appelées : [malloc, strlen, sprintf, sub_401200 → déjà nommée "parse_token"]
Control flow : linéaire avec 1 boucle

Code C vérifié :
[code C émis par ARET]

CONTRAINTE : ne modifie JAMAIS la logique. Propose UNIQUEMENT des noms et commentaires.
Réponds UNIQUEMENT en JSON :
{
  "function": "...",
  "params": {"arg_8": "buf", "arg_c": "buf_len"},
  "locals": {"local_8": "version", "local_c": "result"},
  "comment": "..."
}
```

**Utilisation du prefill JSON** pour forcer la réponse au bon format et éviter les hallucinations.

**Garde-fou fondamental** : après application du patch → recompiler le C renommé → vérifier qu'il compile toujours (verify niveau 1). Si non → rejeter le patch, conserver l'original.

**Propagation cohérente** : utiliser le graphe d'appel pour nommer de bas en haut (les feuilles d'abord → leurs noms informent les appelants). Les noms proposés pour `sub_401000` sont mis à jour dans tous ses call-sites.

**Intégration** : nouveau mode `--mode name` dans `main.rs`. Traitement par batch, cache du prompt prélude (structs partagées).

---

### 🟡 Proposition 12 — FLIRT-style signature matching

**Impact : ★★★☆☆ | Effort : ★★★☆☆ | Priorité : P3**

Dans les binaires statiquement liés (CRT, OpenSSL, etc.), ARET émet `sub_418340` au lieu de `memcpy`. Le loader ELF PLT résout déjà les dynamiques — ici c'est pour le code statiquement lié. Nouveau module `src/signatures/mod.rs`.

```rust
pub struct FlirtSignature {
    /// Masque de bytes (None = wildcard pour les relocations).
    pub pattern: Vec<Option<u8>>,
    /// Masque : 0x00 = masqué, 0xFF = exact
    pub mask: Vec<u8>,
    pub name: String,
    pub aliases: Vec<String>, // aliases CRT
    pub crc: u16,             // CRC16 du reste (bytes non-masqués)
    pub arch: Arch,
}

pub fn match_function(func_bytes: &[u8], sigs: &[FlirtSignature]) -> Option<&str> {
    'sig: for sig in sigs {
        if func_bytes.len() < sig.pattern.len() { continue; }
        for (i, (&b, &m)) in sig.pattern.iter().zip(&sig.mask).enumerate() {
            if (func_bytes[i] & m) != (b & m) { continue 'sig; }
        }
        if crc16(&func_bytes[sig.pattern.len()..]) == sig.crc {
            return Some(&sig.name);
        }
    }
    None
}
```

**Base de signatures initiale (suffisante pour démarrer) :**
- CRT x64 : `memcpy`, `memset`, `memmove`, `strlen`, `strcmp`, `malloc`, `free`, `printf`, `sprintf`
- OpenSSL : `SHA256_Update`, `AES_encrypt`
- zlib : `deflate`, `inflate`

50 fonctions CRT couvrent 80% des cas. Sur un jeu Steam (binaire 27 Mo avec OpenSSL statique), ~300 fonctions nommées immédiatement.

**Intégration** : dans `analysis/mod.rs`, après la découverte des fonctions, tenter le matching pour chaque entrée → substituer `sub_401000` par `malloc` si correspondance.

**Bootstrap** : extraire les patterns depuis les bibliothèques statiques standard (`libc.a`, `libgcc.a`, `msvcrt.lib`) à l'aide d'un outil de génération de signatures.

---

### 🟡 Proposition 13 — Analyse C++ : récupération de vtables et RTTI

**Impact : ★★★☆☆ | Effort : ★★★★☆ | Priorité : P3**

Sur les binaires C++, les méthodes virtuelles sont appelées via `call [rax + 8]` où `rax` est le pointeur de vtable. Nouveau module `src/cpp/vtables.rs`.

**Étape 1 — Détection de vtables dans `.rodata`**

```rust
/// Une vtable valide = bloc de pointeurs consécutifs dans .rodata
/// tous pointant vers du code exécutable (≥ 2 entrées valides → candidate vtable).
fn find_vtables(prog: &Program) -> Vec<VTable> {
    for section in prog.sections.iter().filter(|s| !s.writable && !s.executable) {
        // Lire les blocs de pointeurs consécutifs vers .text
    }
}
```

**Étape 2 — Association vtable → classe (via RTTI si non-strippé)**

```rust
// RTTI x64 : [vtable - 8] pointe vers type_info → mangled name → classe C++
fn rtti_class_name(prog: &Program, vtable_addr: u64) -> Option<String> {
    let ti_ptr = prog.read_u64(vtable_addr - 8)?;
    let name_ptr = prog.read_u64(ti_ptr + 8)?;
    let mangled = prog.read_cstring(name_ptr)?;
    demangle(&mangled)   // crate: cpp_demangle
}
```

**Étape 3 — Résolution des appels virtuels**

```c
// Avant :
v3 = *(*(uint64_t*)obj + 16);
v3(obj, arg);

// Après :
obj->_vtable->method_2(obj, arg); // vtable slot 2 = méthode à offset 16
```

**Étape 4 — Injection dans l'IR** : les `CallTarget::Indirect(...)` deviennent `CallTarget::Named("vtable_403000_method_1")`.

**Dépendance** : crate `cpp_demangle` pour le demangling.

---

### 🟡 Proposition 14 — Vérification SMT formelle (Z3) — Niveau 3

**Impact : ★★★★☆ | Effort : ★★★★☆ | Priorité : P3**

Prouver formellement `∀ inputs. original(x) == recompiled(x)` pour les fonctions sans boucle ou à boucles bornées. La ROADMAP mentionne Z3 disponible. Module `src/verify/smt.rs`.

**Option A** : API FFI Z3 via crate `z3` (`Cargo.toml: z3 = "0.12"`).
**Option B** : export SMT-LIB2 + appel subprocess → plus portable, pas de dépendance native.

```rust
pub fn to_smt2(func: &IrFunction, bound: u32) -> String {
    // SSA est parfait pour SMT : chaque Assign est une équation.
    // Phi → ITE (if-then-else) sur la condition du prédécesseur.
    // Return(v) → valeur de sortie.
    // Boucles bornées : dérouler `bound` fois.
}

pub enum SmtResult {
    Equivalent,                           // UNSAT → prouvé ∀ inputs
    Counterexample(Vec<(String, i64)>),   // inputs qui divergent
    Timeout,
    Unbounded,                            // boucles non bornées → fallback différentiel
}

pub fn prove_equivalent(orig: &IrFunction, recompiled: &IrFunction) -> SmtResult {
    // ∃ input tel que orig(input) ≠ recomp(input) ?
    // SAT → contre-exemple. UNSAT → prouvé équivalent.
}
```

**Périmètre raisonnable** : fonctions sans boucle (getters, conversions, dispatch, accesseurs C++) ou avec boucles à borne constante. Sur le corpus actuel de 16 fonctions, 8-10 seraient prouvables en SMT (`add1`, `addsub`, `mulshift`, `maxi`, `mini`, `sign`, `third`, `mix`).

**Résultats attendus :**
```
factorial:  PROVED EQUIVALENT (∀ n ∈ [0..2^31))
strlen_c:   PROVED EQUIVALENT (∀ s : bounded string)
sort_func:  TIMEOUT (boucle non bornée → fallback différentiel)
```

---

### 🟡 Proposition 15 — Analyse inter-procédurale

**Impact : ★★★☆☆ | Effort : ★★★☆☆ | Priorité : P3**

Une fois les signatures récupérées (Proposition 2), propager les informations aux sites d'appel sur le graphe d'appel (déjà disponible via `Function.callees`).

**DFS bottom-up sur le call graph :**
1. Inférer la signature de chaque fonction feuille.
2. Propager vers les appelants : si `f` appelle `g` et connaît la signature de `g`, les arguments aux call-sites de `g` sont identifiés.
3. Itérer jusqu'à point fixe.

```rust
/// Propagation inter-procédurale des signatures
pub fn interprocedural_sig_recovery(funcs: &mut [IrFunction]) {
    // Si sub_401000 est toujours appelé avec 2 args connus → affiner la signature
    // Les regs non utilisés dans le callee sont supprimés de la liste d'args
}
```

---

### 🟡 Proposition 16 — Récupération de structs, tableaux et variables globales

**Impact : ★★★☆☆ | Effort : ★★★☆☆ | Priorité : P3**

**16a. Structs et tableaux (complémentaire à Proposition 6)**

Observer un même `ValueId` de base utilisé comme adresse de Load/Store à plusieurs offsets → champs d'une struct.

```rust
struct StructCandidate {
    base: ValueId,
    fields: BTreeMap<i64, u8>, // offset → largeur en bits
}
```

Pour chaque `Load { addr: Binary(Add, base, Const(k)), ty }`, enregistrer offset `k` et largeur. Si `base` a ≥ 2 offsets distincts → générer :
```c
struct s_rdi {
    uint32_t field_0;
    uint64_t field_8;
    uint16_t field_10;
};
```

Détection de tableau : `base + i * stride` avec `stride` constant et `i` variable → `arr[i]`.

**16b. Variables globales nommées**

Dans le lifter, les accès IP-relatifs (`rip + disp` → adresse absolue dans `.data`/`.bss`/`.rodata`) produisent `konst(addr)`.

```rust
pub struct Global {
    pub addr: u64,
    pub name: String,     // g_401234 ou name depuis les symboles
    pub ty: Ty,
    pub section: String,
}
```

À l'émission : émettre `extern uint64_t g_401234;` en prélude et remplacer `0x401234` par `g_401234` dans le code.

---

### 🟡 Proposition 17 — Calling conventions complètes et récupération de boucles

**Impact : ★★☆☆☆ | Effort : ★★☆☆☆ | Priorité : P3**

**17a. Conventions d'appel — cas manquants**

- Retour en `xmm0` (float/double) : détecter `Location::Reg(RegId(16))` (xmm0) survie au `Return`.
- Varargs (SysV) : `al` = nombre de registres XMM utilisés.

**17b. Reconstruction `for` et `do-while`**

Pattern `for` dans le CFG :
```
bloc_init → bloc_header → bloc_body → bloc_increment → bloc_header (back edge)
                        ↘ bloc_exit
```
Si `bloc_increment` contient uniquement `i += k` et `bloc_init` initialise `i` → émettre `for (int i = init; cond; i += k)`.

Pattern `do-while` : back edge dont le bloc cible est le bloc d'entrée et le test suit le corps → `do { ... } while (cond)`.

Pattern `while (true) { ...; if (!cond) break; }` avec variable d'induction → `for (int i = 0; i < n; i++)`. Déjà visible sur `factorial`, à formaliser.

---

### 🟡 Proposition 18 — Annotations de confiance dans l'output

**Impact : ★★☆☆☆ | Effort : ★☆☆☆☆ | Priorité : P3**

```c
/* confidence: HIGH — proved equivalent (level 3, SMT) */
uint64_t parse_header(char* buf, int len) { ... }

/* confidence: MEDIUM — recompiles, 50k inputs match (level 2) */
uint64_t sub_40a210(uint64_t p0) { ... }

/* confidence: LOW — goto-only, asm residuals */
void sub_40b890(void) { /* asm: lock cmpxchg ... */ }
```

---

### 🟢 Proposition 19 — Traitement parallèle avec Rayon

**Impact : ★★★☆☆ | Effort : ★★☆☆☆ | Priorité : P4**

Le pipeline est entièrement séquentiel. Les fonctions sont indépendantes (sauf les forward declarations inter-fonctions à la fin). `prog` est en lecture seule dans `build_function` → thread-safe.

```toml
# Cargo.toml
rayon = "1"
```

```rust
// main.rs — Mode::Emit / Mode::Decompile
use rayon::prelude::*;

let irfs: Vec<_> = functions.par_iter().map(|f| {
    let mut irf = ir::build::build_ir(&prog, f);
    ssa::to_ssa(&mut irf);
    opt::optimize(&mut irf);
    irf
}).collect();
```

**Gain estimé** : le projet mentionne 43 k fonctions / ~50 s. Avec 8 cœurs → ~8-10 s.

---

### 🟢 Proposition 20 — Taint analysis pour la recherche de vulnérabilités

**Impact : ★★★★☆ | Effort : ★★★★☆ | Priorité : P4**

Module `src/taint/mod.rs` — analyse de propagation de flux d'information sur l'IR SSA. Marquer les sources non-fiables (entrées réseau, fichiers, argv) et propager les "taint tags" jusqu'aux sinks dangereux (copies mémoire, indices de tableau, cibles de call indirect).

```rust
pub enum TaintSource {
    Arg(usize),         // argument de fonction (entrée externe)
    Return(u64),        // valeur de retour d'un appel externe (read, recv, etc.)
    GlobalRead(u64),    // lecture d'une adresse mémoire connue
}

pub enum TaintSink {
    MemWrite { addr: Expr, tainted: bool },
    ArrayIndex { base: Expr, index: Expr, tainted: bool },
    IndirectCall { target: Expr, tainted: bool },
    BranchCond { cond: Expr, tainted: bool },
}
```

**Sortie** : rapport de chemins taintés avec trace complète (source → transformations → sink). Directement utilisable pour la recherche de buffer overflows, command injections, etc.

---

### 🟢 Proposition 21 — Support ARM64 / AArch64

**Impact : ★★★★☆ | Effort : ★★★★★ | Priorité : P4**

iOS, Android NDK, Apple Silicon, firmwares embarqués. L'IR est déjà architecture-agnostique — les passes SSA, opt, emit et verify ne changent pas.

**Architecture modulaire :**

```rust
// src/arch/mod.rs
pub trait Arch {
    fn decode(&self, data: &[u8], ip: u64) -> Option<Insn>;
    fn bitness(&self) -> u32;
}

pub struct X86Arch { inner: iced_x86::Decoder }
pub struct Arm64Arch { /* capstone ou bad64 */ }
```

```toml
# Cargo.toml
bad64 = "0.4"   # décodeur AArch64 pur Rust
# OU
capstone = "0.12"  # multi-arch via libcapstone
```

**Lifter ARM64 (`src/ir/lift_arm64.rs`)** :
- Registres : `x0`-`x30`, `sp`, `xzr` → mapping vers `RegId` existants
- ABI : Apple/Linux ARM64 ABI — `x0`-`x7` pour args entiers, `v0`-`v7` pour floats, `x0` retour
- Différence clé : flags dans `NZCV` (pas EFLAGS séparés), lus par `b.cond`
- Pas de pile variable (SP toujours aligné 16 bytes)
- Adressage PC-relatif : `ADRP + ADD` idiom
- `ldr x0, [x1, #8]` → `Load { addr: Binary(Add, x1, 8), .. }` — identique à x86

**Adapter** `analysis::resolve_jump_table` pour les tables ARM64 (offsets relatifs 4 bytes courants).

**Effort estimé** : 3-4 semaines pour un lifter couvrant 80% du code ARM64 compilé avec GCC/Clang `-O1`.

---

### 🟢 Proposition 22 — Export JSON/AST structuré + HTML interactif

**Impact : ★★★☆☆ | Effort : ★★☆☆☆ | Priorité : P4**

**22a. Export JSON (`--mode json`) pour l'intégration avec des outils tiers :**

```rust
// Format de sortie
{
  "functions": [{
    "name": "parse_header",
    "entry": "0x401000",
    "signature": "uint64_t parse_header(uint8_t *buf, uint32_t len)",
    "blocks": [{ "id": 0, "stmts": [...] }],
    "callees": ["malloc", "sub_401100"],
    "strings": ["%d %d\n"],
    "recompilable": true
  }]
}
```

Usage : scripts Python d'analyse, intégration dans des éditeurs (VS Code extension), pipelines CI.

**22b. Sortie HTML interactive (`src/emit/html.rs`) :**

- **Index** par adresse / nom / module (filtrable)
- **Corps de fonction** avec syntaxe colorée
- **Liens croisés** : cliquer sur `sub_401000` dans un appel → naviguer à sa définition
- **CFG visualisé** en SVG (via Graphviz DOT → SVG inline)
- **Annotations** : strings, imports, types, niveau de confiance

---

### 🟢 Proposition 23 — Export LLVM IR

**Impact : ★★★☆☆ | Effort : ★★★☆☆ | Priorité : P4**

Une fois l'IR SSA typé complet, l'exporter en LLVM IR (`.ll`) pour bénéficier de tous les passes LLVM (opt, analyse d'alias, vectorisation inverse) et compiler vers n'importe quelle cible.

```rust
pub fn emit_llvm_ir(func: &IrFunction) -> String {
    // IR SSA → LLVM IR est naturel (les deux sont en forme SSA)
    // BinOp::Add → `add`, BinOp::Slt → `icmp slt`, Load → `load`
    // Les φ-nodes SSA → `phi` LLVM directement
}
```

**Valeur ajoutée** : round-trip LLVM → re-optimiser → re-émettre C via `clang -S -emit-llvm` inverse. Ouvre aussi la porte à l'analyse de sécurité via les passes LLVM.

---

### 🟢 Proposition 24 — LSP server pour naviguer le binaire

**Impact : ★★★☆☆ | Effort : ★★★★☆ | Priorité : P4**

Exposer ARET comme serveur LSP que VSCode/Neovim peut consommer. L'utilisateur ouvre le binaire, navigue dans le pseudo-C comme dans du vrai code.

```
Fonctionnalités LSP :
- go to definition : click sur sub_401000 → navigate to its decompilation
- hover : affiche les types inférés, les métriques de vérification
- find references : où est appelée cette fonction ?
- rename symbol : déclenche le renommage LLM + re-vérification
- diagnostics : marque les blocs non liftés (Asm fallback) en orange
```

**Implémentation** : crate `tower-lsp` (Rust) + exposer un thread de serveur dans `aret`.

---

### 🟢 Proposition 25 — Mode `--diff` : binary diffing sémantique

**Impact : ★★★★☆ | Effort : ★★★☆☆ | Priorité : P4**

Comparer deux versions d'un binaire (avant/après patch sécurité, analyse de CVE, évolution de malware). En diffant les CFG et les IR typés SSA, ARET produit un diff _sémantique_ — pas un diff de bytes, mais un diff de _comportement_.

```bash
aret diff original.exe patched.exe --function parse_auth
```

**Algorithme** :
1. Matcher les fonctions par hash de CFG (structure de blocs)
2. Rapporter les fonctions ajoutées / supprimées / modifiées
3. Sur les modifiées : diff textuel du C décompilé + diff sémantique (blocs modifiés, nouvelles conditions, boucles supprimées)

Sortie : « 2 nouvelles conditions, 1 boucle supprimée ». Feature unique par rapport à Ghidra/IDA.

---

## 3. Récapitulatif priorisé

| # | Proposition | Impact | Effort | Priorité |
|---|---|---|---|---|
| 1 | Basculer le défaut sur le pipeline IR | ★★★★★ | ★★☆☆☆ | **P0** |
| 2 | Signatures ABI + arguments call-sites | ★★★★★ | ★★★☆☆ | **P0** |
| 3 | Division par constante (magic multiply) | ★★★★☆ | ★★☆☆☆ | **P0** |
| 4 | Compléter le lifter (mul/div/rotations/SSE) | ★★★☆☆ | ★★☆☆☆ | **P1** |
| 5 | Idiomes compilateur (rotation, sign, clamp) | ★★★☆☆ | ★★☆☆☆ | **P1** |
| 6 | Inférence de types par contraintes | ★★★★★ | ★★★★☆ | **P2** |
| 7 | Alias analysis + promotion stack | ★★★★☆ | ★★★☆☆ | **P2** |
| 8 | SCCP | ★★★☆☆ | ★★★☆☆ | **P2** |
| 9 | GVN | ★★★☆☆ | ★★☆☆☆ | **P2** |
| 10 | Connecter jump-tables → `Stmt::Switch` IR | ★★★☆☆ | ★★☆☆☆ | **P2** |
| 11 | Couche LLM (Claude API) | ★★★★☆ | ★★★☆☆ | **P2** |
| 12 | FLIRT-style signatures | ★★★☆☆ | ★★★☆☆ | **P3** |
| 13 | Analyse C++ / vtables / RTTI | ★★★☆☆ | ★★★★☆ | **P3** |
| 14 | Vérification SMT / Z3 | ★★★★☆ | ★★★★☆ | **P3** |
| 15 | Analyse inter-procédurale | ★★★☆☆ | ★★★☆☆ | **P3** |
| 16 | Structs, tableaux, variables globales | ★★★★☆ | ★★★☆☆ | **P3** |
| 17 | Calling conv. complètes + for/do-while | ★★☆☆☆ | ★★☆☆☆ | **P3** |
| 18 | Annotations de confiance | ★★☆☆☆ | ★☆☆☆☆ | **P3** |
| 19 | Traitement parallèle (Rayon) | ★★★☆☆ | ★★☆☆☆ | **P4** |
| 20 | Taint analysis | ★★★★☆ | ★★★★☆ | **P4** |
| 21 | Support ARM64 / AArch64 | ★★★★☆ | ★★★★★ | **P4** |
| 22 | Export JSON/AST + HTML interactif | ★★★☆☆ | ★★☆☆☆ | **P4** |
| 23 | Export LLVM IR | ★★★☆☆ | ★★★☆☆ | **P4** |
| 24 | LSP server | ★★★☆☆ | ★★★★☆ | **P4** |
| 25 | Mode `--diff` binary diffing | ★★★★☆ | ★★★☆☆ | **P4** |

---

## 4. Roadmap d'implémentation

```
Phase A — Fondations visibles (semaines 1-3)
  ├── Prop. 1 : basculer pipeline IR comme défaut   → débloque tout le reste
  ├── Prop. 2 : args registres + call-sites          → signatures réelles
  ├── Prop. 3 : division par constante              → lisibilité immédiate
  ├── Prop. 4 : mul/div/rotations                   → moins d'Asm fallback
  └── Prop. 19 : rayon                              → perf (trivial à ajouter)

Phase B — Qualitative leap (semaines 4-8)
  ├── Prop. 7 : alias analysis                      → débloque promotion pile
  ├── Prop. 6 : inférence de types (contraintes)    → struct, int32_t, pointeurs typés
  ├── Prop. 8 : SCCP                                → branches mortes supprimées
  ├── Prop. 9 : GVN                                 → nettoie bruit SSA
  └── Prop. 10 : jump-tables → Stmt::Switch         → switch propres

Phase C — Différenciateur absolu (semaines 9-12)
  ├── Prop. 11 : couche LLM (Claude)                → nommage vérifiable
  ├── Prop. 14 : SMT équivalence (Z3)               → level 3, preuve formelle
  └── Prop. 5 : idiomes compilateur                 → code encore plus propre

Phase D — Richesse sémantique (semaines 13-18)
  ├── Prop. 12 : FLIRT signatures                   → noms fonctions connues
  ├── Prop. 13 : C++ vtables/RTTI                   → code OO récupéré
  ├── Prop. 15 : inter-procédural                   → signatures en cascade
  ├── Prop. 16 : structs/tableaux/globaux           → agrégats nommés
  └── Prop. 17-18 : conv./boucles/confiance         → finitions

Phase E — Extension et outillage (mois 5+)
  ├── Prop. 21 : ARM64                              → audience ×10
  ├── Prop. 20 : taint analysis                     → sécurité offensive
  ├── Prop. 22 : JSON/AST + HTML                    → intégration outils tiers
  ├── Prop. 23 : LLVM IR export                     → interopérabilité
  ├── Prop. 24 : LSP server                         → UX professionnelle
  └── Prop. 25 : binary diff mode                   → niche RE unique
```

---

## 5. Métriques de suivi

| Métrique | Aujourd'hui | Cible Phase B | Cible finale |
|---|---|---|---|
| Recompilabilité | 100% | 100% (maintenu) | 100% |
| Équivalence différentielle | 16/16 | 30/30 (corpus étendu) | 100/100 |
| Preuve SMT | 0/16 | — | 8+/16 |
| Fonctions sans `Asm` résiduel | ~70% | ~85% | ~95% |
| Types précis (vs DWARF) | 0% | ~50% | ~80% |
| Fonctions nommées (LLM) | 0% | — | 100% |
| Signatures ABI correctes | 0% (void) | ~90% | ~98% |

---

## 6. Ce qui fait dépasser Ghidra — Tableau comparatif

| Capacité | Ghidra | IDA Hex-Rays | ARET actuel | ARET avec les propositions |
|---|---|---|---|---|
| IR typé propre | P-Code | microcode | ✅ SSA | ✅ |
| Recompilabilité mesurée | ❌ non publiée | ❌ | ✅ 100% | ✅ 100% |
| Équivalence différentielle | ❌ | ❌ | ✅ 16/16 | ✅ corpus étendu |
| Équivalence SMT (formelle) | ❌ | ❌ | ❌ | ✅ (Prop. 14) |
| Inférence de types | ✅ bon | ✅ excellent | ❌ | ✅ (Prop. 6) |
| Couche LLM re-vérifiée | ❌ | ❌ | ❌ | ✅ (Prop. 11) |
| Open source | ✅ | ❌ | ✅ | ✅ |
| Boucle raffinement auto | ❌ | ❌ | ✅ partiel | ✅ complet |
| ABI x64 recovery | ✅ | ✅ | ❌ | ✅ (Prop. 2) |
| Division magique recovery | ✅ | ✅ | ❌ | ✅ (Prop. 3) |
| ARM64 | ✅ | ✅ | ❌ | ✅ (Prop. 21) |
| Binary diffing | ✅ basique | ✅ | ❌ | ✅ sémantique (Prop. 25) |
| Taint analysis | ⚠️ plugin | ⚠️ plugin | ❌ | ✅ natif (Prop. 20) |

**Le créneau unique d'ARET :**

> Un pipeline open-source qui combine **(a)** lifting SSA + types déterministes,
> **(b)** une boucle de recompilation avec équivalence prouvée **par fonction**,
> et **(c)** une couche LLM re-vérifiée pour la lisibilité.
> **Aucun outil ne réunit les trois.** C'est là que "dépasser Ghidra" devient concret et mesurable.

---

## 7. Observation architecturale finale

La décision la plus importante à court terme est **P0** : faire du pipeline IR le chemin par défaut. Tout le reste (types, propagation, LLM) s'implante plus proprement une fois que la base IR est ce que les utilisateurs voient. Le pipeline texte (`dataflow/mod.rs`) peut progressivement être rétrogradé en `--legacy` puis supprimé.

L'architecture actuelle est saine. Les fondations (CFG, dominateurs, SSA, IR typé) sont correctes et testées. C'est une base plus solide que la plupart des projets open-source de décompilation. Le gap avec Ghidra/IDA n'est plus architectural — il est dans la richesse des passes analytiques (types, alias, signatures, LLM) qu'on peut maintenant construire proprement par-dessus.

---

*Document consolidé — analyse croisée de 4 rapports indépendants, ~7 000 lignes de Rust parcourues.*

