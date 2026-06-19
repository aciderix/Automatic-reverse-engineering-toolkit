# Projets « poids lourds » de Static Binary Translation (SBT)

> Source : note reçue (document 3/3). Conservée comme veille technologique des
> projets de référence à analyser.

Projets souvent utilisés dans la recherche académique ou par des entreprises de
pointe, qui touchent exactement à la Traduction Binaire Statique (Static Binary
Translation — SBT). Pour construire l'outil ultime en Rust, il faut analyser
comment ils ont résolu les murs qu'on va inévitablement rencontrer.

## 1. Les Transpilateurs Binaires Directs (concurrents et inspirations)

- **Rev.ng** (C++ / LLVM) : probablement le projet existant le plus proche de
  l'objectif final. Prend un binaire (MIPS, ARM, x86), utilise le moteur de QEMU
  (TCG) pour comprendre les instructions, traduit en LLVM IR, puis recompile en
  un nouveau binaire natif. Pipeline exactement visé. Beaucoup d'articles publiés
  sur la gestion de la mémoire et des pointeurs dynamiques.
- **FEX-Emu** (C++) : émulateur et traducteur dynamique pour exécuter des
  binaires x86/x86_64 sur ARM64 (Linux). Masterclass pour la phase de
  « Fallback JIT » : contrairement à Box86 (interpréteur/wrapper), FEX recompile
  le code à la volée (JIT) avec des performances hallucinantes.
- **Anvill** (Trail of Bits) : soulève le code machine pour produire du bitcode
  LLVM de très haute qualité, dans le but précis de le recompiler ou d'y injecter
  du C.

## 2. Les Pépites spécifiques à l'écosystème Rust

- **Iced-x86** (`iced-x86`) : désassembleur **et** assembleur 100 % Rust,
  infiniment plus rapide que Capstone, gère les toutes dernières instructions
  (AVX-512…), parfait pour modifier du code à la volée. *(ARET l'utilise déjà.)*
- **Falcon** (`falcon`) : framework d'analyse binaire pur Rust, avec son propre
  IL (Falcon IL). Sait lever l'assembleur vers cet IL et calculer les CFG. On
  pourrait forker leur lifter.
- **Sleigh-rs / Rust Sleigh** : Sleigh est le langage inventé par la NSA pour
  Ghidra, décrivant le fonctionnement d'un CPU. Plutôt que de coder la logique de
  chaque processeur à la main, un parseur Sleigh en Rust lit les specs de
  Ghidra : dès qu'un nouveau processeur sort, l'outil le supporte
  automatiquement.

## 3. Les Moteurs d'IR alternatifs

Générer du C directement est génial pour la lisibilité, mais un cauchemar pour le
compilateur quand il s'agit d'optimiser. Lever vers une « IR » d'abord est
souvent plus puissant :

- **VEX IR** (Valgrind / angr) : IR conçue pour l'analyse binaire, extrêmement
  précise sur les effets de bord (ex : quand une instruction x86 modifie le flag
  ZF sans qu'on le demande). Le framework Python `angr` l'utilise massivement.
- **BAP** (Binary Analysis Platform — CMU) : framework (OCaml) utilisant la BIL
  (BAP Instruction Language). Très mathématique ; prouve que la traduction
  statique peut être rigoureuse.

## 4. L'idée d'Architecture Ultime : WebAssembly (Wasm / WASI)

As-tu pensé à la **Cible Universelle** ? Au lieu d'un backend par OS (Windows,
puis Mac, puis Android…) :

- **Wasmtime / WASI** (Rust, Bytecode Alliance) : et si ARET prenait un `.exe`
  Windows et, au lieu de le compiler en ELF Linux, le compilait en module
  WebAssembly (`.wasm`) ?
- **L'avantage magique** : un binaire devenu WebAssembly (avec WASI pour les
  appels systèmes virtuels) tourne **partout** sans recompilation — navigateur,
  Mac, frigo connecté — de façon isolée et sécurisée. La véritable définition
  d'un binaire « Universel ».

## 5. Pour la Couche « OS Compatibilité » (HLE)

- **Darling** (Mac → Linux) : comme Wine, mais pour macOS. Pour ingérer un `.app`
  Mac sur Linux, regarder comment Darling traduit l'environnement Mach-O et l'API
  Cocoa/CoreFoundation.
- **Hangover** : fait tourner des applications Windows x86 sur Linux ARM64.
  Assemblage de Wine et QEMU/FEX. Bon cas d'école pour relier « Translation de
  l'Architecture (CPU) » et « Traduction de l'OS (Wine) ».

## Comment intégrer tout ça dans la roadmap

Ne pas jeter ce qui a été fait avec ARET. Si le moteur C actuel marche, le
garder. Mais pour passer à la vitesse supérieure (binaires énormes, multicœurs,
hautement optimisés) : `iced-x86` pour lire le binaire, soulever le code en LLVM
IR, lier les wrappers (API interceptées) écrits en Rust ou C, et laisser
`rustc`/LLVM faire la magie de la recompilation.
