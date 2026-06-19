# Synthèse — d'ARET (décompilateur) vers UBT (transpilateur universel)

> Ce document relie la **vision UBT** des trois notes voisines
> ([1](01-design-document-UBT.md) · [2](02-briques-open-source.md) ·
> [3](03-projets-avances-SBT.md)) à l'**état réel du code ARET** dans ce dépôt.
> Objectif : un cap clair, honnête sur la difficulté, et une **première marche
> concrète** plutôt qu'un grand saut.

---

## 1. Où on en est vraiment (point de départ)

ARET n'est pas un brouillon : c'est un décompilateur fonctionnel et mesuré.

| Brique UBT (Phase) | État dans ARET aujourd'hui |
|---|---|
| Phase 1 — Container parser (PE/ELF/Mach-O) | ✅ `src/loader` (crate `object`) |
| Phase 1 — Désassemblage x86/x64 | ✅ `src/disasm` (`iced-x86`) |
| Phase 1 — CFG builder | ✅ `src/analysis` + `src/cfg` (dominateurs, frontières) |
| Phase 2 — Lifting vers IR | 🟡 deux IR : texte-C (mûr) + SSA typée naissante (`src/ir`, `src/ssa`) |
| Phase 2 — Typage inférentiel | 🟡 `src/types` (largeurs, signé/non-signé, agrégats partiels) |
| Phase 2 — Récup. switch / sauts indirects | 🟡 résolution de jump-tables faite ; vtables C++ partielles |
| Phase 2 — Émission C compilable | ✅ recompilable ~100 % sur gzip / coreutils ; diff-equiv 16/16 |
| **Phase 3 — Sous-système OS / HLE** | ❌ **inexistant** ← le vrai chaînon manquant |
| Phase 4 — Cross-compile / packaging | ❌ inexistant (au-delà du recompile local de vérification) |
| Filet — Fallback JIT / émulateur | ❌ inexistant |

**Conclusion identique à celle des notes** : le décompilateur (Phases 1–2) est
déjà la partie la plus dure et elle marche. Le pas suivant pour transformer
« décompiler » en « ré-exécuter ailleurs » est la **Phase 3 (HLE)**, pas un
nouveau lifter ni LLVM.

---

## 2. Honnêteté sur l'ambition (à garder en tête, pas à oublier)

Le cap « n'importe quel binaire → n'importe quel OS, exécution native à 60 FPS »
est la réunion de plusieurs projets qui ont chacun coûté des **centaines
d'années-personnes** : Wine (Win32), DXVK (DirectX→Vulkan), QEMU/FEX (CPU),
rev.ng (SBT). Viser leur union d'un bloc est le moyen sûr de ne rien livrer.

La façon réaliste d'« aller jusque-là » est de **réutiliser** ces projets comme
back-ends (cf. note 2) et de faire d'ARET le **chef d'orchestre** :

- ne pas réécrire Win32 → générer du C qui **lie** ou **imite** Wine ;
- ne pas réécrire DirectX → rediriger vers DXVK ;
- ne pas réécrire un JIT CPU → embarquer Unicorn/FEX pour le fallback.

Et avancer par **tranches qui marchent de bout en bout** : un programme trivial
qui se ré-exécute vraiment sur l'autre OS vaut mieux que dix sous-systèmes à
moitié faits. Chaque tranche élargit la classe de binaires supportés.

---

## 3. Découpage en tranches livrables (chaque ligne = un binaire qui tourne)

| # | Tranche | Classe de binaires nouvellement exécutable | Briques |
|---|---|---|---|
| **M1** ✅ | **Interception d'API → shims HLE natifs**, recompile native | PE Win32 *freestanding* (imports kernel32) → ELF Linux natif | aret-existant + `runtime/aret_hle` |
| **M2** ✅ | **Imports indirects via registre** (`mov reg,[iat]; call reg`) + **Memory Layout Mapper** (données globales `.rdata`/`.data` remises à leur VA) | PE Win32 multi-références à données globales en mémoire absolue | `object` |
| **M3** ✅ | **Pile machine partagée** : passage d'arguments inter-fonctions, par la **pile** (stdcall/cdecl) **et par registres** (regparm/fastcall) | programmes Win32 multi-fonctions à appels internes | nouveau lowering gated |
| **M4** ✅ | **Shims CRT** (`msvcrt`) : `printf` **variadique**, `malloc`/`free`/`calloc`/`realloc`, `mem*`/`str*`, `puts` | `.exe` console utilisant le runtime C | shims natifs |
| **FS** ✅ | **Sous-système Fichiers** : file I/O Win32/CRT + **traduction de chemins** `C:\…` → `prefix/drive_c/…` | programmes lisant/écrivant des fichiers | shims natifs |
| M5 | Fallback émulateur (Unicorn) sur adresse inconnue ⏸️ *différé : pas de libunicorn 32-bit dans l'env* | code partiellement obfusqué / sauts dynamiques | `unicorn-engine` |
| M6 | Cible WebAssembly/WASI (au lieu d'un OS natif) | « cible universelle » de la note 3 | `wasmtime` |
| M7 | GUI / graphisme (X11, puis DXVK) | applications fenêtrées, puis jeux | Wine/DXVK |

On ne s'engage pas sur M_n+1 tant que M_n ne tourne pas proprement. L'intérêt :
à chaque palier on a un **artefact démontrable** et un test de non-régression.

---

## 4. M1 — ✅ FAIT (première marche bouclée de bout en bout)

> Statut : implémenté et testé. Un PE Windows 32-bit *freestanding* (imports
> `kernel32` : `GetStdHandle`/`WriteFile`/`ExitProcess`) est transpilé en C par
> ARET, ses imports sont **interceptés** vers des shims HLE natifs, le tout est
> recompilé en **ELF Linux natif** qui imprime exactement la sortie d'origine.

```
$ aret tests/m1/fixtures/hello_win32.exe --mode transpile --run
ARET transpile (UBT M1) — native recompile
  functions:  1
  bitness:    32-bit
  binary:     aret_out/app
  --- program output ---
  | Hello from Windows, running native on Linux
$ file aret_out/app
  ELF 32-bit LSB pie executable, Intel 80386 … for GNU/Linux  (aucun Wine, lié à la libc)
```

Pièces livrées :
- **Interception d'API** (`src/ir/build.rs`, `name_calls_in_expr`) : un appel
  indirect `call [IAT]` vers un import connu devient un appel nommé au shim HLE ;
  en 32-bit stdcall, le shim reçoit le pointeur de pile et lit ses arguments à
  `esp+0, esp+4, …` (ABI-exact, car l'appel modélisé n'empile pas d'adresse de
  retour).
- **Couche HLE** (`runtime/aret_hle/`) : embryon d'`aret_os_hle`. Implémente les
  API Win32 en termes POSIX/libc, compilé **dans** le binaire final (natif, pas
  d'émulateur).
- **Builder** (`src/builder/`) : embryon d'`aret_builder`. Émet le C, lie le HLE
  + un `main` généré, et invoque `cc -m32` pour produire l'ELF natif.
- **Test e2e** (`tests/m1_transpile.rs`) : transpile + exécute + vérifie la
  sortie (skip propre si `cc -m32` absent).

Outillage requis pour rejouer/étendre M1 dans une session : `gcc-multilib`
(`cc -m32`) ; `gcc-mingw-w64-i686` pour régénérer les fixtures PE.

### 4 bis. M1 en détail — la première marche (« commence par… » des notes)

La note 1 le dit explicitement : *« Commence par créer un Hello World Windows
qui utilise printf, décompile-le, et écris le script Rust qui injecte une
implémentation compatible POSIX avant de recompiler pour Linux. »*

C'est exactement la bonne première marche car elle **boucle tout le pipeline UBT
en miniature** sans rien réécrire d'énorme.

### Forme cible (CLI)

```
aret <binary> --target linux-x86_64 -o ./out      # transpile + recompile natif
```

### Étapes techniques de M1

1. **Détecter les imports** dans le loader (table d'import PE / PLT ELF) et les
   exposer au décompilateur — partiellement déjà disponible via `object`.
2. **Table de redirection** : `printf` (source) → `aret_printf` (HLE). Au lieu
   d'émettre un appel à un symbole externe inconnu, l'émetteur émet l'appel vers
   le stub HLE.
3. **Couche HLE C minimale** (`runtime/aret_hle/`) : un `aret_hle.h` + `.c` qui
   réimplémentent le sous-ensemble d'API touché en termes POSIX/libc standard.
   Pour `printf`, c'est un passe-plat vers la libc de la cible — trivial, mais
   ça pose **toute la mécanique d'injection** réutilisée par M2…M7.
4. **Builder** (`aret_builder`, peut commencer comme une fonction dans `src/`) :
   orchestrer `cc`/`clang` pour compiler `programme.c` + HLE → ELF Linux, et
   rapporter le succès. La boucle `--mode verify` actuelle est déjà à 90 % ce
   builder ; M1 = la généraliser avec la couche HLE liée.
5. **Test de bout en bout** : exécuter l'ELF produit et comparer sa sortie
   stdout à celle du programme d'origine (différentiel, comme les 16/16 actuels).

### Pourquoi commencer ainsi (et pas par LLVM / Wine / Unicorn)

- réutilise à 100 % le moteur de décompilation qui marche déjà ;
- introduit le **mécanisme d'interception d'API** (le vrai cœur de la Phase 3)
  sur le cas le plus simple possible ;
- produit un **exécutable qui tourne**, donc un test de non-régression réel ;
- chaque brique lourde (Wine, DXVK, Unicorn, WASI) se branche **plus tard** sur
  cette même table de redirection, sans rien refondre.

---

## 4 ter. M2 — ✅ FAIT (imports indirects + données globales)

> Statut : implémenté et testé. Un PE qui charge le pointeur d'import dans un
> registre (`mov esi,[IAT]; call esi`, plusieurs fois) et lit ses chaînes depuis
> `.rdata` en **mémoire absolue** se transpile, se recompile et s'exécute
> nativement avec la bonne sortie.

```
$ aret tests/m1/fixtures/hello_globals.exe --mode transpile --run
  --- program output ---
  | M2: first global string in .rdata
  | M2: second global, mapped at its original VA
```

Pièces livrées :
- **Interception des appels d'import indirects via registre** (`src/ir/build.rs`) :
  un suivi `registre → import` repère `reg = *(IAT)` puis résout chaque `call reg`
  vers le shim HLE nommé (en pelant le masque de troncature 32-bit `reg & 0xffffffff`).
  Les registres callee-saved survivant aux appels, le motif « charger une fois,
  appeler plusieurs fois » est couvert.
- **Memory Layout Mapper** (`src/builder/`, génère `aret_layout.c`) : embryon du
  composant du design doc. À l'exécution, `__aret_map_memory()` fait un
  `mmap(MAP_FIXED)` couvrant les sections de données et y recopie les octets
  d'origine, de sorte que les pointeurs **absolus** (chaînes/tables globales)
  résolvent. Le binaire est lié `-no-pie` pour libérer les VAs basses d'origine.

Limite connue (→ M3) : les **arguments passés par la pile entre fonctions
transpilées** ne transitent pas, car chaque fonction modélise sa pile dans un
`__frame[]` privé. Un appelant qui pousse `(h, s, n)` puis `call helper` ne les
transmet pas — l'appelé lit des entrées indéfinies. C'est ce que résout la pile
machine partagée de M3.

---

## 4 quater. M3 — ✅ FAIT (pile machine partagée : appels inter-fonctions)

> Statut : implémenté et testé. Un programme Win32 multi-fonctions dont l'entrée
> appelle un helper interne se transpile, se recompile et s'exécute nativement,
> les arguments traversant l'appel **qu'ils soient passés par la pile** (args 4+
> en cdecl/stdcall) **ou par registres** (gcc `-O1` regparm(3), `__fastcall`).

```
$ aret tests/m1/fixtures/hello_stackargs.exe --mode transpile --run
  | M3: argument arrived via the shared STACK       # arg 6 passé sur la pile
$ aret tests/m1/fixtures/hello_callchain.exe --mode transpile --run
  | M3: passed through an internal call (1)          # args en registres eax/edx/ecx
  | M3: passed through an internal call (2)
```

Conception (lowering **dédié au transpile**, isolé derrière un flag thread-local
`emit::set_shared_stack` — les chemins verify/decompile et l'équivalence
différentielle 16/16 sont inchangés, 42 tests verts) :

- **Une seule pile machine globale** (`aret_stack`, dans `aret_main.c`) au lieu
  d'un `__frame[]` par fonction. Chaque fonction transpilée prend le pointeur de
  pile de l'appelant en paramètre (`uint64_t __esp`) ; ses accès `[ebp±d]`/
  `[esp±d]` lisent/écrivent donc la **même** mémoire que l'appelant (on force le
  lifting à garder ces accès en mémoire brute via `frames_off`, sans les replier
  en locaux nommés).
- **Modélisation du `call`** : un appel interne passe `__esp - 4` (l'adresse de
  retour qu'empile le vrai `call`), de sorte que l'appelé relit ses arguments
  pile à `[__esp + 4]`, `[__esp + 8]`, … — exactement là où l'appelant les a
  écrits.
- **Registres volatils threadés** : l'appel passe aussi `eax/ecx/edx` (liste
  fixe), reçus comme paramètres, pour les conventions à registres. Réutilise et
  généralise le mécanisme `reg_params` existant (jusque-là réservé au 64-bit
  SysV) au 32-bit en mode partagé.

Portée : convention d'appel à pile/registres standard couverte. Restent pour plus
tard les cas pointus (variadique inter-fonctions complexe, retour de struct par
valeur, `alloca`, nettoyage stdcall `ret N` dans du code esp-tracké exotique).

---

## 4 quinquies. M4 — ✅ FAIT (couche HLE pour le C runtime)

> Statut : implémenté et testé. Un `.exe` console qui utilise le runtime C
> (`printf`, `malloc`, `strlen`…) se transpile et s'exécute nativement.

```
$ aret tests/m1/fixtures/hello_printf.exe --mode transpile --run
  | M4: int=42 hex=0xff str=hello char=Z pct=%
  | M4: malloc sum=100
$ aret tests/m1/fixtures/hello_heap.exe --mode transpile --run
  | M4: heap=ABCDE len=5        # malloc + fill() interne (pile partagée) + strlen + printf %s
```

Pièces livrées (`runtime/aret_hle/`) :
- **`printf` variadique** — le morceau central. Le shim lit le format à `[esp+0]`
  et **consomme ses arguments variadiques sur la pile machine partagée**
  (`[esp+4]`, `[esp+8]`, …). Chaque conversion est reformatée via le `snprintf`
  natif avec **la même spec**, donc flags / largeur / précision / `%*d` / `%lld` /
  `%f` se comportent exactement comme une libc réelle. Les pointeurs `%s` et la
  chaîne de format résolvent grâce au Memory Layout Mapper (M2).
- **Tas** : `malloc`/`calloc`/`realloc`/`free` (la libc native en `-m32` renvoie
  des pointeurs 32-bit, valides dans le modèle).
- **Mémoire / chaînes** : `mem{cpy,set,move}`, `str{len,cmp,cpy}`, `puts`,
  `putchar`.
- **Convention de nommage** : tout import intercepté devient `aret_<nom>`
  (`aret_printf`, `aret_malloc`…), ce qui évite toute collision avec les vraies
  fonctions libc que les shims appellent.

---

## 4 sexies. Sous-système Fichiers — ✅ FAIT (Phase 3, traduction de chemins)

> Statut : implémenté et testé. Un programme qui écrit puis relit un fichier via
> un chemin Windows `C:\…` se transpile et s'exécute nativement ; le fichier
> atterrit au chemin natif traduit.

```
$ ARET_PREFIX=/tmp/aretfs aret tests/m1/fixtures/hello_file.exe --mode transpile --run
  | FS: round-trip through a C:\ path
$ cat /tmp/aretfs/drive_c/aret_fs_test.txt
  FS: round-trip through a C:\ path        # C:\aret_fs_test.txt -> $ARET_PREFIX/drive_c/...
```

Pièces livrées (`runtime/aret_hle/`, purement additif — aucun changement Rust) :
- **Traduction de chemins** (design doc §2, « Système de Fichiers ») :
  `C:\dir\file` → `$ARET_PREFIX/drive_c/dir/file` (lettre de lecteur →
  `drive_<x>`, `\` → `/`, chemins relatifs inchangés) ; `$ARET_PREFIX` par défaut
  `aret_prefix`. Création récursive des dossiers parents à l'écriture.
- **stdio CRT** : `fopen`/`fclose`/`fread`/`fwrite`/`fputs`/`fgets`/`fseek`/
  `ftell`/`remove`.
- **API fichier Win32** : `CreateFileA`/`CloseHandle`/`DeleteFileA`/
  `SetFilePointer` — les HANDLE sont des fds POSIX dans le modèle, donc les
  `aret_ReadFile`/`aret_WriteFile` existants fonctionnent tels quels avec les
  handles renvoyés.

---

## 5. Décisions d'architecture à trancher au moment de coder M1

- **Mono-crate vs workspace** : la note 1 propose un workspace
  (`aret_core` / `aret_lifter` / `aret_ir` / `aret_os_hle` / `aret_builder`).
  À faire **quand** la couche HLE et le builder existent — pas avant, pour ne
  pas créer des crates vides. M1 peut vivre dans l'arbre actuel
  (`src/builder.rs`, `runtime/aret_hle/`) puis être éclaté en workspace à M2/M3.
- **C vs LLVM IR comme cible** : garder le **C** pour M1–M4 (lisible, le moteur
  le produit déjà, recompilable). Réévaluer LLVM IR (note 2/3) seulement si
  l'optimisation du code transpilé devient le facteur limitant.
- **Natif vs WASM** : les deux cibles partagent tout l'amont ; WASM (M6) est un
  back-end de plus sur la même table de redirection, pas un chemin concurrent.

---

## 6. Ce qui est déjà acté, ce qui reste à décider

- ✅ **Acté** : on garde ARET (C) comme socle ; la Phase 3 (HLE) est la priorité ;
  on avance par tranches de bout en bout ; on réutilise les briques externes au
  lieu de les réécrire. **M1, M2, M3, M4 et le sous-système Fichiers sont
  livrés** (cf. §4 → §4 sexies).
- 🏗️ **Mise à l'échelle (vers de vrais gros binaires)** — chantier ouvert après
  le test sur un AAA packé (44 183 fonctions, `program.c` de 403 Mo) :
  - ✅ **Données en blob** : les sections sont embarquées via `.incbin`
    (`aret_layout.S` + `sections.bin`) au lieu de tableaux d'octets C — ~×18 plus
    petit (59 Mo → 3,3 Mo pour le jeu).
  - ✅ **Stubs d'imports faibles** : un stub `aret_<import>` par import non shimé
    (override par le HLE quand il existe) → le binaire **lie toujours** et
    **signale à l'exécution** chaque API manquante (liste de courses du HLE / cible
    du pont Wine).
  - ➡️ **Reste le mur principal** : l'émission en **un seul** `.c` de 403 Mo ne
    compile pas. Prochaine brique = **émission modulaire** (un `.c` par paquet de
    fonctions + en-tête de déclarations partagé, compilation parallèle puis lien).
- ⏸️ **M5 (fallback Unicorn) différé** : il faudrait lier un émulateur **dans le
  binaire généré**, qui est `-m32` ; or l'environnement n'a pas de `libunicorn`
  32-bit (le paquet apt est 64-bit), donc un M5 réellement exécutable n'est pas
  démontrable ici. À reprendre dans un environnement avec une toolchain Unicorn
  32-bit, ou en ciblant un binaire source 64-bit.
- ➡️ **Pistes immédiates testables nativement** : élargir la couverture Win32
  réelle (`MessageBoxA` → stderr/console, `GetCommandLineA`, environnement,
  horloge), un test **différentiel** comparant la sortie transpilée à une
  référence connue, puis M6 (cible WASI) et M7 (GUI/DXVK).

> Ce fichier est le point d'entrée « mémoire » du projet UBT. Les trois notes
> d'origine sont conservées à côté, non éditées, comme cap de référence.
