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

### Fait : moteur de déballage dynamique (`--mode unpack`, `--features unpack`)

> Le brique [0] amorcée pour de vrai, **sans Wine** : on réutilise l'émulateur
> CPU **Unicorn** (lib système `libunicorn`, FFI fine dans `src/unpack/`).

- Principe (cœur du déballage générique) : émuler depuis l'entry point en
  **traçant chaque page écrite** par le stub ; dès qu'une instruction est
  **fetchée depuis une page qui vient d'être écrite**, c'est du code
  auto-modifié fraîchement déchiffré → on a atteint l'**OEP**. On s'arrête et le
  payload est lisible en clair dans la mémoire de l'émulateur.
- Implémenté : mapping des sections aux VA d'origine, pile, **TEB/PEB synthétique
  + base FS** (pour passer le prologue Windows `fs:[0x30]`/`fs:[0x18]`),
  **mapping paresseux** des pages allouées au vol (tolérance scratch), détection
  d'OEP, dump de l'image déchiffrée, mesure des octets réécrits.
- Validé : un mini-packer XOR auto-déchiffrant (le stub déchiffre 4 octets puis
  saute dedans) → OEP détectée à la bonne adresse, payload récupéré en clair ;
  un code non auto-modifiant ne produit **pas** de faux OEP. Tests
  `src/unpack` (`cargo test --features unpack`).
- Mur honnête sur le jeu réel : le protecteur de `MightyQuest` **appelle son IAT
  (thunk near-null) avant de déchiffrer** → le moteur le **signale proprement**
  (`stub reached unmapped 0x10 — needs an API/Win32 model`) au lieu de planter.
  La suite logique est exactement la **couche Win32** (§4 [3], reco #2) branchée
  sur ce moteur : stubber LoadLibrary/GetProcAddress/VirtualAlloc/… pour laisser
  le stub résoudre puis déchiffrer. Le moteur est prêt à recevoir cette couche.

## 5 bis. Démarré : backend LLVM (`--mode llvm`)

> Première pierre de l'amélioration #1 (§3). ARET sait maintenant émettre du
> **LLVM IR** directement (`src/emit/llvm.rs`), pas seulement du C.

- Modèle : valeurs et base de pile = `alloca i64` (le `mem2reg`/`-O2` de LLVM
  reconstruit le SSA), mémoire via `inttoptr`. Couvre le sous-ensemble
  entier/mémoire/branchements/`switch`/appels + la soupape `asm:`.
- Validé : les **130 fonctions** d'un vrai `.exe` CRT mingw produisent un module
  **accepté par `llvm-as`** et **compilé en objet par `llc -O2`**. Test :
  `tests/llvm_backend.rs`.
- ✅ **Chemin LLVM exécutable de bout en bout** (`--mode transpile --backend llvm`) :
  un vrai PE Windows → LLVM IR → `llc` → **ELF natif qui tourne**, en réutilisant
  tout le runtime C (HLE, main, dispatch, layout). **Parité avec le backend C** :
  les 8 fixtures (string sur pile, imports indirects, données globales, appels
  internes pile+registres, `printf`, tas, pointeurs de fonction, TEB) donnent la
  même sortie correcte. Le backend LLVM supporte la pile partagée (param `%esp`,
  appels via `aret_call`).
- ✅ **Flottant complet** : SSE (`__fp_*`) **et x87** (`long double` → `x86_fp80`,
  émetteur type-conscient sur les valeurs `fp80`). Fixtures `hello_float`
  (SSE) et `hello_float_x87` → `c=8 c10=85` corrects via les deux backends.
- ✅ **Largeurs exactes** : `SignExtend`/`Cast` sign/zero-étendus à la bonne
  largeur (plus d'identité i64).
- ✅ **Vérification sur le chemin LLVM** : `--mode verify --backend llvm` mesure
  la validité IR par fonction (100% sur 130 fonctions CRT et 300 fonctions d'un
  vrai ELF, à parité avec le backend C). *(Pas de SMT/Z3 dans le code — la vérif
  d'équivalence formelle reste aspirationnelle pour les deux backends.)*
- ✅ **Passage à l'échelle (émission chunkée)** : un seul module `.ll` pour un
  gros binaire devenait énorme (≈1,3 Go pour ~44 k fonctions) et faisait OOM
  `llc` (≈8,5 Go de RSS). Corrigé comme côté C : `emit_split` produit **un module
  LLVM autonome par paquet** (200 fonctions), chacun déclarant les symboles des
  autres paquets (pas de redéfinition), compilés **en parallèle** par `llc` puis
  liés. Le pic mémoire est borné à un petit module par cœur. Validé en multi-
  paquets : 130 fonctions → 5 modules `.ll` indépendamment acceptés par `llvm-as`,
  liés en un ELF qui tourne. **Validé à l'échelle réelle** : le jeu déballé
  (`MightyQuest`, **44 183 fonctions**) → **221 modules `.ll`** compilés en
  parallèle par `llc` (≈120 Mo de RSS chacun, 4 en parallèle au lieu d'un seul
  `llc` à 8,5 Go) → **ELF natif de 127 Mo** lié sans OOM.
- Portée honnête : reste un **chemin d'exécution LLVM** robuste sur de gros
  binaires (testé sur les fixtures) et le différentiel C↔LLVM automatisé.

## 5 ter. Fait : brancher le vrai CRT (reco #1)

> Plutôt que réécrire le runtime C à la main, on **forwarde chaque point d'entrée
> msvcrt vers la libc hôte** avec un marshalling fin et ABI-exact (`aret_crt.c`).

- Mécanisme : chaque `aret_<name>` lit ses arguments cdecl sur la pile machine
  partagée (`[esp+0]`, `[esp+4]`, …), appelle la **vraie** fonction libc, renvoie
  le résultat dans le slot `eax`. Ce sont des **définitions fortes** qui priment
  sur les stubs faibles du builder → lier l'unité élargit la couverture CRT, sans
  toucher au dispatch. msvcrt ≈ libc pour le sous-ensemble C standard : c'est le
  **vrai** runtime, pas une imitation.
- Couverture ajoutée : `<string.h>` (strncpy/strcat/strrchr/strstr/strspn/strtok/
  strdup/memcmp/memchr/_stricmp/_strnicmp…), `<stdlib.h>` (atol/abs/strtol/strtoul/
  rand/srand/getenv), `<stdio.h>` (sprintf/snprintf/fflush — variadiques via le
  formateur partagé `aret_vformat`), `<ctype.h>` (toupper/isdigit/…).
- Validé : fixture `hello_crt` →
  `CRT: s=reverse dot=.c sub=piler ci=0 up=Z dig=1 n=-123 hex=255 abs=42 mc=1`,
  **identique** via les backends C **et** LLVM (`tests/m1_transpile.rs` +
  différentiel `tests/llvm_backend.rs`).
- Limite honnête : les fonctions à **callback** (comparateurs `qsort`/`bsearch`,
  retour d'écriture `sscanf`) ne sont pas forwardées — un callback transpilé est
  un `sub_<va>` à l'ABI pile-machine, pas une fonction cdecl native ; il faut le
  chemin `aret_call`. Laissé à des shims dédiés.

## 5 quater. Fait : la couche Win32 native (reco #2 / brique [3])

> La **colle Windows** — ce que ni rev.ng ni RetDec ne fournissent. Implémentation
> **native** (pas Wine) du sous-ensemble kernel32 qui a un équivalent POSIX propre
> (`aret_win32.c`), même modèle que le CRT (lecture pile machine → POSIX, défs
> fortes qui priment sur les stubs faibles).

- Couverture : **timing** (GetTickCount, QueryPerformanceCounter/Frequency,
  GetSystemTimeAsFileTime → `clock_gettime`/`gettimeofday`), **environnement/chemins**
  (Get/SetEnvironmentVariableA, Get/SetCurrentDirectoryA, GetTempPathA,
  OutputDebugStringA), **lstr*** (lstrlen/cpy/cat/cmp/cmpi → libc str*),
  **tas** (GetProcessHeap, HeapAlloc/Free/ReAlloc, VirtualAlloc/Free,
  Global/LocalAlloc → allocateur C, HEAP_ZERO_MEMORY → calloc), **atomiques**
  (Interlocked Increment/Decrement/ExchangeAdd/Exchange/CompareExchange →
  builtins `__atomic`).
- Validé : fixture `hello_win32api` →
  `W32: zero=1 ctr=41 ev=ok evlen=2 s=win32 slen=5 mono=1`, **identique** via les
  backends C **et** LLVM. Un vrai programme Windows fait tourner ses appels
  kernel32 directement sur des primitives Linux.
- Portée honnête : sous-ensemble POSIX-mappable seulement (pas d'USER32/GUI, pas
  de registre, pas de vraie machinerie de modules PE). Le reste = **Winelib**
  (§4 [3]) — et c'est précisément cette couche qu'il faudra brancher sur le moteur
  de déballage (§5) pour que le stub d'un packer résolve son IAT et déchiffre.

## 6. Reco — état

1. ✅ **CRT natif** (§5 ter) et ✅ **couche Win32 native** (§5 quater) : le vrai
   runtime C + le sous-ensemble kernel32 POSIX-mappable tournent, testés C↔LLVM.
   Reste, pour aller plus loin que le sous-ensemble POSIX : **Winelib** (USER32,
   registre, modules).
2. ✅ **Backend LLVM** chunké, à l'échelle réelle (§5 bis) ; ✅ **moteur de
   déballage** Unicorn (§5). Le **chaînon manquant** est unique et clair :
   brancher la **couche Win32 (stubs d'API) dans le moteur de déballage** pour
   que le stub d'un packer résolve son IAT et déchiffre — alors le pipeline
   [0]→[1]→[2]→[3] se referme sur la classe « packé non-VM ».
3. **Pour ton jeu** : moteur de déballage prêt, mais son protecteur appelle l'IAT
   avant de déchiffrer → il faut la couche Win32 dans l'émulateur (point 2).
   L'objectif « natif sans émulateur » d'un AAA reste un objectif de longue
   haleine ; les briques (lift, backend LLVM, CRT, Win32, déballage) sont en
   place et testées une à une.
