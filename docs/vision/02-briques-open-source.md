# Briques open source à intégrer / étudier (par étape du pipeline)

> Source : note reçue (document 2/3). Conservée comme référence des projets
> existants à réutiliser plutôt que réinventer.

Pour un projet d'une telle envergure, réinventer la roue sur chaque composant
prendrait des décennies. L'écosystème open source (notamment en Rust et C/C++)
regorge de « briques » (frameworks et bibliothèques) qui font exactement une
partie de ce dont l'outil a besoin.

## 1. Ingestion et Parsing de Binaires (Le Frontend)

Avant de désassembler, il faut comprendre le format du fichier (.exe, .elf,
etc.) et charger virtuellement ses sections.

- **Goblin** (Rust — `m4b/goblin`) : crate Rust multi-plateforme qui parse les
  formats ELF (Linux), PE (Windows), Mach-O (macOS) et d'autres. Donne
  instantanément les points d'entrée, les sections `.text` et les tables
  d'import/export. *(Note : ARET utilise déjà la crate `object`, équivalente.)*
- **LIEF** (C++/Python/Rust bindings — `lief-project/LIEF`) : Library to
  Instrument Executable Formats. Plus lourd que Goblin mais très puissant pour
  **modifier** des binaires (utile pour le backend de packaging).

## 2. Désassemblage et Levage vers une IR (Lifting)

- **Capstone** (C, crate Rust `capstone-rs`) : standard de l'industrie pour le
  désassemblage multi-architectures (x86, ARM, MIPS, RISC-V).
- **Remill** (C++ — `trailofbits/remill`) : « lève » le code machine (x86,
  aarch64) vers LLVM IR. Mine d'or conceptuelle pour voir comment ils traduisent
  les effets de bord du CPU (flags, registres) en code agnostique.
- **Ghidra P-Code** (Java/C++) : IR universelle « P-Code ». Des projets
  extraient les specs SLEIGH de Ghidra (qui décrivent chaque CPU) pour les
  réutiliser ailleurs — évite d'écrire à la main la logique de chaque
  processeur.

## 3. Décompilation et Traduction (Le Middle-end)

- **RetDec** (C++ — `avast/retdec`) : Retargetable Decompiler basé sur LLVM,
  prend un binaire de n'importe quelle architecture et recrache du C.
  L'équivalent open source le plus proche du projet actuel. Étudier la
  reconstruction du CFG et la déduction de types pour générer du C.
- **McSema** (C++ — `trailofbits/mcsema`) : utilise Remill pour transformer un
  binaire en LLVM IR, recompilable pour une autre architecture. Prouve que la
  traduction statique de binaires fonctionne.

## 4. Couche de Compatibilité et API (HLE — High Level Emulation)

Le plus gros défi : faire croire au programme qu'il est sur son OS d'origine.

- **Wine** (C) / `libwine` : le monument de la traduction Win32 → POSIX. Plutôt
  que de réécrire les 50 000 fonctions de l'API Windows, le C généré pour une
  cible Linux pourrait lier `libwine` ou s'inspirer fortement de leur
  implémentation des appels systèmes Windows.
- **Darling** (C/C++) : l'équivalent de Wine, mais pour exécuter des binaires
  macOS sur Linux.
- **DXVK** (C++) : traducteur DirectX (Windows) → Vulkan (multiplateforme). Pour
  convertir un jeu Windows vers Mac/Linux, on intègre DXVK pour le graphisme.

## 5. Fallback Dynamique et JIT (Le filet de sécurité)

Pour le code impossible à décompiler statiquement (chiffré, obfusqué, sauts
dynamiques complexes), le binaire final devra embarquer un mini-émulateur.

- **Unicorn Engine** (C, crate Rust `unicorn-engine`) : framework d'émulation de
  CPU (basé sur QEMU) léger. Embarquable dans le code final ; si le programme
  saute à une adresse inconnue à l'exécution, on passe le relais à Unicorn.
- **Cranelift** (Rust — `bytecodealliance/wasmtime`) : générateur de code natif
  rapide (JIT) 100 % Rust. Pour recompiler à la volée les blocs découverts
  dynamiquement.
- **Box86 / Box64** (C — `ptitSeb/box86`) : émulateurs espace-utilisateur
  performants (x86 Linux sur ARM Linux). Font de la traduction d'API dynamique
  (« wrapping »). Leur approche pour intercepter les appels `.so` est brillante.

## Résumé des briques pour l'outil en Rust

1. **Frontend** : `goblin`/`object` pour parser EXE/ELF + `iced-x86`/`capstone-rs`
   pour désassembler.
2. **Lifting** : s'inspirer de RetDec et Remill. Envisager de générer du **LLVM
   IR** au lieu du C pur : LLVM sait déjà compiler son IR vers n'importe quelle
   architecture de façon optimisée.
3. **OS Translation** : emprunter massivement aux implémentations de Wine (Win32)
   ou de Box86.
4. **Fallback Emulation** : embarquer `unicorn-engine` dans l'exécutable généré
   pour gérer le code obscur à la volée.

**Le conseil en or** : regarder de très près LLVM (via la crate Rust `inkwell`).
Si ARET arrive à lever le binaire en structure LLVM, on gagne gratuitement des
dizaines d'années de travail sur l'optimisation et la recompilation
cross-platform.
