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
### Fait : le chaînon — modèle Win32 **dans** le moteur de déballage

> La couche Win32 branchée **à l'intérieur de l'émulateur** : un packer peut
> maintenant résoudre son IAT et allouer sa mémoire pendant qu'on l'émule.

- Mécanisme : chaque slot de l'IAT (`prog.imports`, VA → nom) est rempli d'une
  **sentinelle** pointant dans une région-piège ; un `call [iat]` amène EIP sur
  la sentinelle, interceptée par le hook de code → on lit les arguments stdcall
  sur la pile émulée, on **modèle un résultat natif**, et on **simule le retour
  stdcall** (pop retaddr + args, EIP = retour). Surface servie :
  LoadLibrary*/GetModuleHandle* (handle factice), **GetProcAddress** (lie une
  nouvelle sentinelle au nom demandé → trappée à son tour), **VirtualAlloc**
  (arène bump réellement mappée), VirtualProtect/Free, GetVersion, etc.
- Tolérance générique ajoutée : un accès **données** non mappé (n'importe où — le
  CRT/SEH sonde near-null, le packer gratte du scratch) est **comblé par une page
  zéro** ; seul un **fetch** non mappé (contrôle perdu, code non fabricable)
  arrête et est signalé.
- Validé : fixture packer qui **résout VirtualAlloc via son IAT**, écrit le
  payload déchiffré dans le buffer rendu, et y saute → l'appel est servi (1 API)
  et l'OEP tombe dans la mémoire allouée fraîchement écrite (`src/unpack` test
  `iat_call_resolved_then_oep_in_allocated_memory`).
- Sur `MightyQuest` : le moteur **passe désormais** le prologue near-null (plus de
  faute) et tourne jusqu'au budget **sans code auto-modifié** (0 octet déchiffré)
  → diagnostic honnête « rien à déballer ». Cohérent avec le nom du fichier
  (`_unpacked_fixed`) : ce binaire est **déjà déballé** (sections `.UBX` dormantes,
  entrée = vrai démarrage CRT). Le moteur est correct : il n'invente pas d'OEP.
- Reste pour un *vrai* packer AAA : élargir la surface d'API servie en émulation
  (la même `aret_win32` mais côté émulateur) et gérer l'anti-debug/TLS — chemin
  ouvert, sans changement d'architecture.

### Fait : reconstruction d'un PE propre + validation sur un *vrai* packer (UPX)

> Le bout de la brique [0] : du dump mémoire déchiffré on **reconstruit un `.exe`
> statiquement analysable** qui re-rentre dans `--mode transpile`.

- `build_dump_pe` : capture **toute** l'image déchiffrée (pages du stub + tout ce
  qu'il a écrit, p.ex. le `.text` original que UPX décompresse à sa VA d'origine),
  coalesce en runs contigus, et écrit un PE32 plat (FileAlignment = SectionAlignment
  = 0x1000, le raw recopie la mémoire), **entry = OEP**. `--mode unpack --out-dir X`
  produit `X/unpacked.exe`.
- **Validé bout en bout sur UPX** (vrai packer du monde réel) : `hello_printf`
  packé → `--mode unpack` →
  `OEP 0x401000, 6 appels d'API servis (LoadLibraryA/GetProcAddress/VirtualProtect),
  PE reconstruit` → le code au point d'entrée du PE reconstruit est
  **octet-pour-octet identique** au programme original déballé. Test
  `upx_real_packer_recovers_original_code` (skip si `upx` absent). C'est la preuve
  que la chaîne `packé → émulation+IAT → OEP → dump → PE propre → ré-analyse`
  fonctionne sur un packer réel, pas seulement un jouet.
- Reconstruction **fidèle** quand le packer a **restauré les en-têtes PE
  d'origine** en mémoire (cas UPX : `MZ`/`PE` au base de l'image) : on réutilise
  ces en-têtes et on pose le fichier en **raw==RVA**, ce qui **préserve la table
  des sections d'origine** (le PE reconstruit ré-expose UPX0/UPX1/UPX2 avec le
  code déballé, entrée = OEP). Sinon, repli sur un PE plat mono-section.
### ✅ FERMÉ : « packé → ELF natif qui imprime » (classe UPX)

> La chaîne complète tourne de bout en bout sur un **vrai packer** :
> `UPX .exe → déballage → PE propre → transpile → ELF Linux natif qui s'exécute`.

- **Reconstruction d'imports (Scylla-style)** : le moteur trace désormais quelle
  adresse résolue (`GetProcAddress`) est **écrite dans quel slot IAT** (hook
  d'écriture filtrant les sentinelles) et quelle **DLL** (handle `LoadLibrary`).
  Il regroupe par DLL et **synthétise un répertoire d'imports standard**
  (IMAGE_IMPORT_DESCRIPTOR + ILT + IMAGE_IMPORT_BY_NAME) injecté dans le PE
  reconstruit (layout raw==RVA, FirstThunk pointant sur les vrais slots).
- **Mapper de mémoire élargi** : il embarque maintenant les sections
  **exécutables** aussi — UPX fusionne `.rdata` dans une section de code, donc les
  littéraux chaîne adressés en absolu (`%s` → `0x402000`) doivent être mappés.
- **Résultat mesuré** : `hello_printf` packé UPX →
  `--mode unpack` → `OEP 0x401000, imports recouvrés (KERNEL32!ExitProcess,
  msvcrt!printf), unpacked.exe` → `--mode transpile --run` →
  **`M4: int=42 hex=0xff str=hello char=Z pct=%` / `M4: malloc sum=100`**.
  Test e2e `tests/unpack_e2e.rs` (skip sans `--features unpack`/`upx`/`cc -m32`).
- Portée honnête : validé sur UPX (packer compresseur classique). Un protecteur
  AAA (VM, anti-debug, TLS, IAT non-contiguë) demande plus de surface d'API
  émulée et la gestion de l'anti-tamper — l'**architecture** est en place
  (moteur CPU + modèle Win32 + reconstruction d'imports + rebuild PE), reste à
  élargir la couverture.

### Validé sur du *vrai* code open-source (pas que des fixtures jouets)

- **SHA-256 de Brad Conte** (`crypto-algorithms`, domaine public) compilé en PE :
  6 fonctions, manipulation de bits/tables/boucles réelles. Transpilé (backends
  **C et LLVM**) → **digest cryptographique exact** = la référence native
  `sha256sum` (`df4fe123…550921e`). Et **chaîne complète** : packé UPX → déballé
  (OEP + 4 imports reconstruits : ExitProcess/memset/printf/strlen) → transpilé →
  **même digest correct**. Tests `real_oss_sha256_digest` + `tests/unpack_e2e.rs`.
- Couverture **kernel32** élargie (system info, chemins, mutex/event/wait,
  GetACP, GetModuleFileNameA…) → fixture `hello_win32sys`, parité C/LLVM.

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
