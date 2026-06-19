# Document de Conception Architecturale : Universal Binary Transpiler (UBT)

> Source : note de conception reçue (document 1/3). Conservée telle quelle
> comme cap à long terme. La lecture critique et le plan réaliste qui la
> relient au code ARET existant sont dans [`00-SYNTHESE-roadmap-UBT.md`](00-SYNTHESE-roadmap-UBT.md).

C'est un projet d'une ambition phénoménale. Puisque tu as déjà les bases avec
ton Automatic Reverse Engineering Toolkit (ARET) pour lever le code machine en
C, nous allons concevoir l'architecture globale de la suite de l'outil.

Pour accomplir cette prouesse (traduire n'importe quel binaire vers n'importe
quel autre système avec une exécution native), l'outil ne peut pas se contenter
de traduire les instructions CPU. Il doit agir comme un Transpilateur Binaire
Statique et Dynamique couplé à une Couche de Compatibilité Universelle (HLE -
High Level Emulation).

## Projet de base : Automatic Reverse Engineering Toolkit (ARET)

## 1. Objectif du Projet

Créer un framework modulaire en Rust capable de :

1. Ingérer un fichier exécutable arbitraire (PE, ELF, Mach-O, APK, etc.) issu
   d'une architecture arbitraire (x86, x86_64, ARM, RISC-V, etc.).
2. Désassembler et décompiler ce binaire en une Représentation Intermédiaire
   (IR), puis en code C via le moteur ARET.
3. Traduire et lier les appels système (syscalls) et les API spécifiques de
   l'OS source vers l'OS cible.
4. Recompiler et packager le code généré pour produire un exécutable natif sur
   la plateforme cible, s'exécutant exactement comme à l'origine.

## 2. Architecture Globale (Pipeline en 4 Phases)

L'outil fonctionnera comme un compilateur classique (Frontend, Middle-end,
Backend) mais adapté aux binaires pré-compilés, avec l'ajout crucial d'un
Environnement d'Exécution Synthétique (Runtime).

### Phase 1 : Le Frontend (Ingestion et Analyse)

Ce composant lit le fichier source et extrait le code machine et les
métadonnées.

- **Analyseur de Formats (Container Parser)** : Doit parser les en-têtes
  complexes des formats PE (Windows), ELF (Linux), Mach-O (macOS), DEX/APK
  (Android). Il extrait les sections de code (.text), de données (.data,
  .rodata), et la table des imports/exports.
- **Lifter d'Architecture** : Convertit les instructions CPU brutes (x86, ARM,
  etc.) en une Représentation Intermédiaire (IR) agnostique du processeur
  d'origine.
- **Analyseur de Flux de Contrôle (CFG Builder)** : Reconstruit le graphe
  d'exécution pour comprendre les boucles, les conditions et les sauts.

### Phase 2 : Le Middle-end (Transformation et Décompilation — cœur actuel ARET)

Ce composant transforme l'IR en code de haut niveau.

- **Résolveur de Sauts Indirects** : Identifie les pointeurs de fonctions et les
  tables de sauts (switch cases) statiquement.
- **Typage Inférentiel** : Déduit les types de variables (entiers, pointeurs,
  structures) à partir de l'utilisation des registres CPU.
- **Générateur de code AST (Abstract Syntax Tree)** : Convertit l'IR optimisée
  en un AST C compilable.

### Phase 3 : Le Sous-Système d'OS et API (L'Étape Critique)

Traduire le code C ne suffit pas. Le programme source appelle des API Windows
(Win32) ou Linux (POSIX) qui n'existent pas sur la cible.

- **API Wrapper / Intercepteur** : Le code C généré ne fera pas directement un
  `MessageBoxA` ou un `sys_write`. Le transpilateur remplacera ces appels par
  des appels à une bibliothèque universelle (ex : `ubt_gui_alert()` ou
  `ubt_fs_write()`).
- **Couche de Compatibilité (HLE)** : Tu devras coder (ou intégrer) une
  librairie d'abstraction massive :
  - **Système de Fichiers** : Traduction des chemins (ex : `C:\Windows\` vers
    `/ubt_prefix/drive_c/`).
  - **Graphisme** : Traduction des appels DirectX (Windows) ou Metal (Mac) vers
    Vulkan ou OpenGL.
  - **Multithreading** : Mapper les `CreateThread` ou `pthreads` sur le système
    de threading natif de Rust.
- **Moteur d'Émulation de Mémoire (Memory Layout Mapper)** : Certains binaires
  assument qu'ils sont chargés à une adresse mémoire précise (ex : `0x00400000`).
  L'outil devra générer un code qui alloue un bloc de mémoire virtuel respectant
  cet agencement d'origine.

### Phase 4 : Le Backend (Compilation et Packaging)

Ce composant transforme le C et la couche de compatibilité en exécutable final.

- **Moteur de Compilation croisée (Cross-Compiler)** : L'outil fera appel à un
  compilateur embarqué (via libclang ou GCC) pour compiler le code généré pour
  la cible désirée (ex : ARM64 macOS).
- **Packageur Cible** :
  - Si cible = Windows : Génère un `.exe` avec l'icône et les ressources
    d'origine.
  - Si cible = Android : Pack le binaire dans un `.apk`, génère un wrapper JNI
    et signe l'application.
  - Si cible = macOS : Crée un `.app` avec son fichier `Info.plist`.

## 3. Ce que l'outil devra être capable de faire (Spécifications Techniques)

Pour que l'exécutable final s'exécute comme à l'origine sans crasher, l'outil
Rust devra gérer les défis techniques suivants :

### A. Gestion des Bibliothèques Dynamiques (.dll, .so)

Un programme n'est jamais seul. Il dépend de librairies.

- **Mode Statique** : L'outil devra être capable d'extraire les fonctions des
  `.dll`/`.so` du système source et de les recompiler avec l'outil pour les lier
  statiquement au nouveau programme.
- **Mode Dynamique (Wrappers)** : Si la librairie cible des fonctions
  matérielles (comme l'audio), il faudra intercepter l'appel dans le code C
  généré et le rediriger vers la librairie native de l'OS cible.

### B. Fallback Dynamique (Le filet de sécurité)

Il est factuellement impossible de traduire 100 % du code statiquement à
l'avance (à cause du code auto-modifiant, de l'obfuscation, ou des sauts
calculés dynamiquement).

- **Ce que l'outil doit faire** : Le code généré doit inclure un
  mini-interpréteur/JIT (Just-In-Time emulator). Pendant l'exécution du
  programme final, si le programme tente d'exécuter une instruction qui n'avait
  pas été découverte lors de la décompilation, il tombe dans ce mini-émulateur
  qui lira l'instruction brute et l'exécutera à la volée.

### C. Traduction de l'Endianness et de l'Architecture Mémoire

- Si le binaire d'origine vient d'un système Big-Endian (ex : vieux PowerPC)
  vers du Little-Endian (ex : x86), le code C généré devra inclure des macros
  BSWAP sur toutes les lectures/écritures mémoire.
- La gestion des exceptions (SEH sur Windows, Signals sur Linux) devra être
  simulée via des blocs `setjmp`/`longjmp` ou de la gestion d'exceptions C++
  dans le code généré.

## 4. Architecture logicielle en Rust

La structure du projet Rust (le `Cargo.toml` et les crates) devrait ressembler à
ceci :

- `aret_core` : Le parseur actuel, qui lit le binaire et génère le code C.
- `aret_lifter` : Les modules spécifiques pour décoder x86, ARM, MIPS. (Peut
  utiliser des librairies comme `capstone-rs`.)
- `aret_ir` : La définition du langage intermédiaire.
- `aret_os_hle` : La librairie (écrite en Rust et en C) qui sera injectée et
  compilée avec chaque binaire final. C'est elle qui contient les faux
  `Windows.h` ou `<unistd.h>` pour tromper le programme décompilé.
- `aret_builder` : Le module qui orchestre `rustc` ou `clang` pour générer le
  binaire final.

## 5. Résumé du Workflow (Exemple d'utilisation)

1. L'utilisateur lance : `aret-cli --input game.exe --target linux-x86_64`
2. Rust lit `game.exe`, extrait l'assembleur x86.
3. Le lifter transforme l'asm en IR.
4. L'analyseur détecte tous les appels à `user32.dll` et `kernel32.dll`.
5. Le traducteur génère le code `game_recompiled.c`.
6. Le traducteur lie `game_recompiled.c` avec le HLE Engine (qui traduit les
   appels Windows vers X11/Wayland sur Linux).
7. Le compilateur embarqué compile le tout dans un ELF Linux natif.
8. L'utilisateur lance `./game_recompiled` sur son Linux, et il tourne
   nativement à 60 FPS.

## Conclusion et Prochaine Étape

Puisque le décompilateur génère déjà du C, la priorité absolue maintenant est le
sous-système de Compatibilité d'OS (Phase 3). Le C généré ne pourra être compilé
sur une autre plateforme que si on lui fournit des « stubs » (des fonctions de
remplacement) pour toutes les API de l'OS d'origine.

Commence par créer un « Hello World » en C pour Windows (qui utilise `printf`),
décompile-le avec l'outil actuel, et écris le script Rust qui injecte une
implémentation compatible POSIX avant de le recompiler pour Linux.
