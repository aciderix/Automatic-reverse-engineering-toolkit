# ARET face à rev.ng & RetDec — différences, améliorations, plan d'intégration

> Recherche pour situer ARET parmi les outils existants, en tirer des
> améliorations concrètes, puis planifier l'assemblage des briques (Wine/CRT/
> DXVK) et le **déballage** des exécutables protégés.

## 1. Les trois outils côte à côte

| | **ARET** (le nôtre) | **rev.ng** | **RetDec** (Avast) |
|---|---|---|---|
| Décodeur | iced-x86 (maison) | **QEMU** (réutilisé) | Capstone (réutilisé) |
| Représentation interne (IR) | IR + SSA **maison** (Rust) | **LLVM IR** (via TCG de QEMU) | **LLVM IR** |
| Sortie | **C recompilable** | LLVM IR → binaire **réexécutable**, + C lisible | C / pseudo-Python **pour lecture** |
| But | re-fabriquer du code **qui re-marche** | analyse **+ traduction binaire** (re-run) | **lire** pour le reverse |
| Sortie qui re-tourne ? | **oui** (c'est le but) | **oui** (translateur revamb) | **non** (lecture seulement) |
| Vérif. d'équivalence | **oui** (recompile + SMT/Z3) | non (mise en avant) | non |
| Archis | x86 / x86-64 | MIPS, ARM, x86-64 | beaucoup (retargetable) |
| Maturité | **prototype** jeune | mature (académique + produit) | mature, mais lent sur gros fichiers |
| Langage | Rust | C++ / LLVM | C++ / LLVM |

Sources : [rev.ng (LLVM dev mtg)](https://llvm.org/devmtg/2016-11/Slides/DiFederico-rev.ng.pdf),
[rev.ng open-source](https://rev.ng/blog/open-sourcing-renvg-decompiler-ui-closed-beta),
[RetDec outputs (wiki)](https://github.com/avast/retdec/wiki/Decompiler-outputs),
[RetDec v4.0](https://www.gendigital.com/blog/insights/research/retdec-v4-0-is-out).

## 2. La grande différence de philosophie

- **RetDec** vise la **lisibilité** : son C est *pour les yeux d'un humain*, il **ne recompile pas** en programme qui marche. → super pour comprendre/patcher (ton jeu), pas pour notre objectif « ELF natif ».
- **rev.ng** vise la **traduction qui re-tourne** : son translateur (revamb) prend un binaire et en re-émet un **exécutable** (même pour une autre archi). C'est **exactement notre objectif**, et c'est mature. Sa clé : **réutiliser QEMU** (sémantique complète de toutes les instructions) **et LLVM** (SSA, optimisations, génération de code multi-archi) — au lieu de tout réécrire.
- **ARET** vise aussi le code qui re-marche, **plus** une garantie rare : il **vérifie automatiquement** que la sortie recompile et (via Z3) qu'elle est **équivalente** à l'origine. Ça, ni rev.ng ni RetDec ne le mettent en avant. C'est notre **vraie valeur ajoutée**.

## 3. Ce qu'ARET peut emprunter (améliorations concrètes)

Le même réflexe que pour le CRT : **ARET réécrit à la main ce que rev.ng réutilise.**

1. **Backend LLVM au lieu d'émettre du C.** Aujourd'hui ARET écrit du C puis appelle `cc`. En émettant du **LLVM IR**, on récupère gratuitement : les optimisations LLVM, la génération de code multi-archi (x86, **ARM**, WASM…), et un meilleur passage à l'échelle. → c'est ce qui m'a coûté cher cette session (gros `.c`, lenteur).
2. **Complétude des instructions via réutilisation.** ARET a une « soupape » honnête (`Asm` = instruction non gérée). rev.ng n'en a pas : QEMU couvre *tout*. On pourrait réutiliser une **bibliothèque de sémantique** (TCG de QEMU, ou *remill* de McSema) au lieu de lifter chaque instruction à la main.
3. **Gestion des sauts/appels indirects généralisée.** rev.ng garde **tout** le code original adressable + un « dispatcher ». J'ai bricolé une table VA→fonction (ça marche) ; la version rev.ng est plus générale (utile pour le code auto-modifiant léger).
4. **Garder notre point fort** : la **vérification d'équivalence** (recompile + Z3). C'est différenciant — à conserver et muscler.

> Question stratégique honnête : pour l'objectif « binaire → natif », **rev.ng est plus
> proche du produit** sur le *cœur* (lift + recompile). Deux voies :
> - **(i)** muscler ARET (backend LLVM + complétude) → on se rapproche de rev.ng ;
> - **(ii)** **se tenir sur rev.ng** comme brique de lift/recompile, et concentrer
>   notre effort unique sur ce que rev.ng **ne fait pas** : la **couche Windows
>   (HLE/Wine)** et le **déballage**. rev.ng est ELF/Linux, sans couche Win32.

## 4. Le plan d'intégration de toutes les briques

L'objectif : `programme Windows → ELF Linux natif`, en assemblant. Pipeline cible :

```
  .exe Windows
     │
     ▼
 [0] DÉBALLAGE (si protégé)  ───────────────  Scylla / x64dbg / dump dynamique
     │   (le packer s'ouvre une fois → photo mémoire + reconstruction de l'IAT)
     ▼  .exe « propre »
 [1] LIFT binaire → IR        ───────────────  ARET (ou rev.ng / RetDec comme base)
     │   (= traduire SEULEMENT le code propre du programme)
     ▼
 [2] BACKEND → code natif     ───────────────  LLVM (idéalement) → ELF
     │
     ▼
 [3] COUCHE WINDOWS (HLE)     ───────────────  Wine / Winelib (Win32) + CRT
     │   (= NE PAS réécrire : brancher l'existant)        mingw-w64 (kit C)
     ▼
 [4] GRAPHISME (si jeu)       ───────────────  DXVK / vkd3d (D3D → Vulkan)
     │
     ▼
  ELF Linux natif (sans émulateur)
```

Répartition « réutiliser vs faire » :
- **[0] Déballage** → réutiliser (outils existants), voir §5.
- **[1] Lift** → ARET (à muscler) **ou** rev.ng/RetDec.
- **[2] Backend** → réutiliser **LLVM**.
- **[3] Win32 + CRT** → réutiliser **Wine/Winelib + CRT mingw-w64** (la colle = notre boulot).
- **[4] D3D** → réutiliser **DXVK/vkd3d**.

→ Notre travail **unique** se réduit à : la **colle** entre le code lifté et ces
briques, + (éventuellement) muscler le lift. Tout le reste existe déjà.

## 5. Le déballage (« unpacking ») expliqué

Un packer **chiffre** le vrai code et le **déchiffre en mémoire au lancement**. On
ne peut donc pas le lire statiquement. La parade classique :

1. **Laisser le programme se lancer** dans un environnement contrôlé (débogueur /
   loader instrumenté) jusqu'à ce que le packer ait fini de déchiffrer.
2. **Dumper** la mémoire (photo du vrai code en clair).
3. **Reconstruire l'IAT** (la table des imports que le packer a remplie à la
   volée) — c'est ce que fait **Scylla** (plugin x64dbg) ou ImpRec.
4. Récrire un `.exe` « propre », **statiquement analysable**.

Outils réutilisables : **x64dbg + Scylla**, **PE-sieve / Mage** (capture de modules
déballés), ou un dump piloté par **QEMU/Unicorn**. *(Ton fichier `_unpacked_fixed`
montre qu'une partie a déjà été faite.)*

Limite honnête : même déballé, un AAA reste plein de comportements **dynamiques**
(résolution d'API au vol, callbacks) durs en statique pur. Le déballage rend
faisable la classe « gros programme non-VM-protégé », pas forcément un AAA complet.

## 5 bis. Démarré : backend LLVM (`--mode llvm`)

> Première pierre de l'amélioration #1 (§3). ARET sait maintenant émettre du
> **LLVM IR** directement (`src/emit/llvm.rs`), pas seulement du C.

- Modèle : valeurs et base de pile = `alloca i64` (le `mem2reg`/`-O2` de LLVM
  reconstruit le SSA), mémoire via `inttoptr`. Couvre le sous-ensemble
  entier/mémoire/branchements/`switch`/appels + la soupape `asm:`.
- Validé : les **130 fonctions** d'un vrai `.exe` CRT mingw produisent un module
  **accepté par `llvm-as`** et **compilé en objet par `llc -O2`**. Test :
  `tests/llvm_backend.rs`.
- Portée honnête (proof of concept) : reste à faire — flottant/x87, largeurs
  exactes (extend/truncate), un **chemin LLVM exécutable de bout en bout** (pile
  partagée + HLE + dispatch, comme le transpileur C), et la **vérif d'équivalence**
  sur le chemin LLVM (notre point fort à conserver).

## 6. Reco

1. **Court terme, gros gain** : pour [3], **brancher le vrai CRT natif** (mingw-w64)
   au lieu de mes shims → débloque « vrai programme qui imprime ».
2. **Moyen terme** : évaluer sérieusement **rev.ng** comme base de [1]+[2] (lift +
   backend LLVM) ; concentrer ARET sur la **vérif d'équivalence** + la **colle Windows**.
3. **Pour ton jeu** : [0] déballage déjà amorcé, mais l'objectif « natif sans
   émulateur » d'un AAA packé reste hors de portée réaliste → Wine pour jouer,
   ARET/Ghidra pour le protocole.
