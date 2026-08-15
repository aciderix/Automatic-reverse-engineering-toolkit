# 82 — Suivi de l'industrialisation / automatisation (document VIVANT)

> **Complément du 70.** À lire **après le 70** (et avant de reprendre un chantier
> d'automatisation) pour savoir, sans rien reconstruire : quels **générateurs** existent,
> ce qu'ils **produisent**, ce qui est **prouvé**, et le **prochain cran**. Objectif :
> **ne rien perdre sous compression**. Mise à jour **obligatoire** à chaque incrément
> d'automatisation (comme le 71 pour le reste).

---

## 0. Principe directeur (validé utilisateur, 2026-08-02)

On **automatise la fabrication des shims** en **puisant dans des sources ouvertes**,
**compilé DANS le binaire** → **autonome au runtime** (aucun Wine chargé). Détail
d'architecture : **doc 81 §I13**. Deux natures de fichier, deux traitements :
- **Code RÉEL** (app + ses DLL : `WinMergeU.exe`, `mfc90u.dll`…) → **on TRANSPILE** (`--with-dll`).
- **Surface OS que Wine réimplémente** (kernel32, shell32, mlang, gdi32…) → **du C SOURCE de Wine**
  (son builtin PE est un **relais-stub** : transpiler ne donne rien).

Trois **couches** (branchement → comportement), et pour le comportement trois **intensités** :

| Couche | Source | Forme | Coût |
|--------|--------|-------|------|
| `@N` / pops (ABI) | import-libs mingw | script | ✅ fait |
| Signatures / stubs | entêtes mingw + clang (AST JSON) | script | ✅ premier cran (`gen_win32_sigs.py`) : 5066 `@N` mutuellement prouvés + squelettes typés |
| **Comportement (corps)** | **sources Wine** | légère (données) / moyenne (corps) / lourde (DLL entières + plancher ntdll) | 🚧 **légère + moyenne prouvées** ; lourde = milestone |

---

## 1. Générateurs en place (checked-in, ré-exécutables)

### `tools/gen_stdcall_pops.py` — table `@N` (couche ABI) ✅
- **Entrée** : import-libs mingw du cœur système (`/usr/i686-w64-mingw32/lib/lib*.a`).
- **Sortie** : réécrit `src/ir/stdcall_pops.rs` (**merge ADDITIF** : garde les entrées main, ajoute les prouvées).
- **Résultat mesuré** : **963 → 10 140** entrées ; **0 conflit** ; contradictions résolues (15 `Script*` → usp10).
- **Ré-exécuter** : `python3 tools/gen_stdcall_pops.py` (ou `--check` pour un dry-run/diff).
- **Portes** : hash transpile inchangé `19acad982194bf07` + **winediff 210/211** + stdcall_audit PASS.
- **Sûreté** : additif ⇒ le hash ne peut pas changer, aucune régression possible ; les `@N` viennent des mêmes
  libs contre lesquelles les binaires sont liés ⇒ corrects par construction. Détail : 81 §I12.

### `tools/gen_win32_sigs.py` — prototypes typés Win32 (couche SIGNATURES) ✅ premier cran
- **Entrée** : entêtes mingw du cœur système, lus via l'**AST JSON de clang** (`clang-18 -Xclang -ast-dump=json`,
  cible `i686-w64-mingw32`; aucun binding libclang requis).
- **Récupère** ce que les import-libs ne portent PAS : type de retour, **types par argument**, convention d'appel.
  **6494** prototypes `__stdcall` parsés.
- **`--check`** (preuve, ré-exécutable) : recalcule le `@N` de chaque `__stdcall` par **somme des tailles d'arguments**
  (ABI i686) et le compare à `stdcall_pops.rs`. Les deux nombres viennent de **chemins toolchain INDÉPENDANTS**
  (prototype d'entête vs mangling d'import-lib) ⇒ l'accord est une **preuve mutuelle** de la couche ABI :
  **5066 fonctions mutuellement prouvées**, **0 conflit**. On n'affirme que là où **chaque** argument est **prouvablement
  dimensionné** ; un struct-par-valeur/typedef inconnu ⇒ **abstention** (711), jamais un pari (§0).
- **⭐ A découvert un vrai skew entête/lib** : `I_RpcGetAssociationContext` et `mmDrvInstall` — l'entête porte une
  arité plus récente (8/16) que l'import-lib (`@4`/`@12`, vérifié au `nm`). L'import-lib **fait foi** (c'est ce que le
  binaire lie réellement pour nettoyer la pile) ⇒ `stdcall_pops.rs` a raison ; skew **documenté** (allowlist), le check
  reste vert en le **signalant**.
- **`--skeleton NAME…`** (tueur de boilerplate) : émet un shim ARET prêt à remplir — args dépaquetés en **locaux typés**
  via le bon accesseur (`WP`/`WI`/`WU`/`WS`), corps `aret_unimpl` **SOUND** (aborte tant que la logique n'est pas écrite,
  jamais de valeur devinée). Auto-vérification : le squelette de `StrFromTimeIntervalW` **reproduit l'ABI écrite à la main**.
- **`--marshal NAMEA…`** (marshalling A→W automatique) : dérive le point d'entrée **…A** de son jumeau **…W** déjà
  implémenté — élargit les args chaîne **d'entrée** (`u32_a2w`), passe le reste tel quel (`esp[i]` = arg *i* directement),
  appelle le cœur W ; **NULL passe en NULL** (pas de chaîne vide devinée). **⭐ Garde-fou §0 = le cœur du cran** : il
  **REFUSE** (pas de thunk, `aret_unimpl` honnête) toute paire où A/W diffèrent **ailleurs que sur des chaînes d'entrée**
  — struct A/W distincts (`LOGFONTA`≠`LOGFONTW`, le piège documenté au 70), **tampon de SORTIE** (`LPSTR` : exige un
  marshalling de taille non modélisé). Le round-trip `u32_a2w`/`aret_w2n` est **byte-exact 0-255** ⇒ un `…A` dérivé est
  équivalent au `…A` fait-main (ex. `DeleteFileA`). Compile+run vérifiés (thunk généré, NULL propagé).
- **Ré-exécuter** : `python3 tools/gen_win32_sigs.py --check` (ou `--skeleton …`, `--marshal DeleteFileA …`).

### `tools/gen_wine_heavy.py` — mesure/prépare la forme LOURDE (compiler du `.c` Wine entier) 🚧 spike
- **Entrée** : un `.c` ntdll de Wine (nom → récupéré du miroir `wine-9.0`, ou chemin local).
- **Fait** : (1) **splice des forward-decls** extraites des définitions `RET WINAPI Name(...)` du fichier lui-même
  (mingw `winternl.h` n'en déclare qu'un sous-ensemble ⇒ use-before-def sinon) ; (2) **compile INCHANGÉ** contre les
  entêtes NT de mingw + le shim de compat **checké-in** `tools/wine_heavy/` (`wine/debug.h`→no-op `TRACE`/`FIXME`,
  `ddk/ntddk.h` vide, `ARRAY_SIZE`, limites 64-bit) ; (3) **rapporte** les fonctions **définies** + le **plancher**
  (symboles indéfinis) classé libc / heap / à-porter.
- **Mesuré** : `rtlstr.c` → **46 fonctions**, plancher = **12 primitives** à porter (conversions NLS +
  `RtlCompareUnicodeStrings`) ; libc/heap déjà dans le HLE. Coût **par-fichier** (l'outil signale ce qui manque, ex.
  `wcstring.c` : 2 typedefs msvcrt).
- **Nature** : **mesure de faisabilité build-time** (§5.0), rien d'exécuté/deviné, rien câblé encore. **Ré-exécuter** :
  `python3 tools/gen_wine_heavy.py rtlstr.c`.
- **⭐ MÉCANIQUE PROUVÉE bout-en-bout** (`tools/wine_heavy/proof.sh`, 2026-08-02) : `rtlstr.c` compilé **inchangé** +
  **plancher ASCII de 12 primitives** (`tools/wine_heavy/ntdll_floor.c`, sous-ensemble sound) → lié → exécuté sous Wine →
  sortie **bit-identique au vrai ntdll de Wine** (init/convert/upcase/compare/int-to-char). ⇒ « compiler du `.c` Wine +
  porter un petit plancher → ça tourne CORRECTEMENT ». **Piège ABI trouvé et résolu** : le plancher doit avoir **une seule
  convention** (mingw ne déclare qu'un sous-ensemble des primitives en `NTAPI`, laissant le reste **implicite=cdecl** ⇒
  mismatch stdcall/cdecl = dérive esp = crash) → **`ntdll_floor.h`** déclare **tout le plancher `NTAPI`**.
- **⭐⭐ PROUVÉ DANS LE MODÈLE DE BUILD RÉEL D'ARET** (`tools/wine_heavy/proof_native.sh`) : ARET compile son HLE avec **`cc`
  natif → ELF natif** (pas mingw ; Linux n'a **pas** `winnt.h`). `rtlstr.c` compile quand même par `cc -m32` avec une **couche
  NT-types autonome checkée-in** (`tools/wine_heavy/native/` : ~50 typedefs + 9 `STATUS_*` + flags + macros `Rtl*Memory`/`min`)
  **+ `-fshort-wchar`** (le `wchar_t` natif est 32-bit, le `WCHAR` Windows 16-bit) **+ `wcslen`/`wcschr` 16-bit dans le
  plancher** (celles de glibc sont 32-bit). Lié et exécuté comme **ELF Linux natif (AUCUN Wine au runtime)** → **bit-identique
  à l'oracle Wine**. ⇒ **tous les inconnus d'intégration levés** : la forme lourde marche dans le build réel d'ARET, autonome.
  **Reste** : câbler dans `src/builder/mod.rs` (flags par-fichier, source Wine vendorée, adaptateurs `aret_Rtl*` esp→appel,
  gating sur imports `Rtl*`) + fixture winediff.

### `tools/gen_mlang_cp.py` — table de code pages mlang (couche comportement, forme LÉGÈRE) ✅
- **Entrée** : `dlls/mlang/mlang.c` de Wine (récupéré via `curl` github raw ; `WINE_MLANG_C=<path>`).
- **Sortie** : `runtime/aret_hle/mlang_cp_table.h` (**70 code pages** : cp, family cp, flags, desc, charsets, fonts).
- **Consommé par** : `u32_ml_GetCodePageInfo` (`MIMECPINFO`, miroir de `fill_cp_info`) **et** `u32_ml_GetFamilyCodePage`
  (search-loop de Wine, forme MOYENNE = port de LOGIQUE). **73 code pages** (les macros `CP_UNICODE/UTF7/UTF8` résolues,
  UTF-8 65001 inclus — corrigé grâce à l'oracle).
- **⚠️ Leçons oracle (2026-08-02)** : extraire de **la version de Wine == l'oracle** ; l'oracle **corrige** l'extraction
  (a rendu UTF-8 ; a rejeté `GetNumberOfCodePageInfo` dont le `total_cp` runtime=73 ≠ parse source=74, non livré).
- **Câblage build** : `mlang_cp_table.h` embarqué (`include_str!`) + écrit dans l'out-dir (`src/builder/mod.rs`).
- **Ré-exécuter** : `python3 tools/gen_mlang_cp.py`.
- **Portes** : `winecorpus/ole_mlang_getcpinfo` **bit-identique Wine** + hash inchangé + audit PASS.
- **Preuve de concept** : premier « corps depuis Wine, extrait mécaniquement, compilé, autonome » validé (81 §I13).

---

## 2. Plan d'industrialisation — état par phase

- **Phase A — générateur `@N`** ✅ FAIT (voir §1). La classe de bug « `@N` manquant » disparaît pour ~10k API.
- **Phase B — sous-processus** ✅ FAIT (part sound) : `popen`/`pclose`/`system`/`_pipe` via `sh -c` (correct-ou-bruyant ;
  frontière PE-enfant préservée). `CreateProcess`/`_spawn` d'un `.exe` = échec sound maintenu. Détail 71 (2026-08-02).
- **Phase C — mlang `IMultiLanguage`** 🚧 EN COURS :
  - brique 1 : **activation COM** (objet HLE, IUnknown, 15 méthodes instrument-first) ✅ ;
  - brique 2 : **`GetCodePageInfo`** rempli depuis la table Wine ✅ (forme légère) ;
  - brique 3 : **`GetFamilyCodePage`** portée de la LOGIQUE de Wine ✅ (forme MOYENNE prouvée) ; `GetNumberOfCodePageInfo` **non livré** (count runtime≠source) ;
  - suite : autres méthodes **au besoin mesuré** (WinMerge n'en appelle pas d'autre sur le chemin courant).

### Prochains crans (priorisés)
1. **WinMerge** : le mur courant n'est plus mlang mais un **null-deref** (`0xC0000005 at 0x10`, C++ lifté) →
   forensics **instrument-first** (build `ARET_TRACE` → fonction + objet null). *(Chantier profondeur.)*
2. ✅ **FAIT (premier cran) — Signatures / stubs** (couche 2, `gen_win32_sigs.py`) : prototypes typés depuis l'AST clang
   des entêtes mingw ⇒ (a) `--check` = 5066 `@N` mutuellement prouvés (entête vs import-lib), 0 conflit, 1 skew documenté ;
   (b) `--skeleton` = shims typés prêts à remplir, corps `aret_unimpl` sound ; (c) `--marshal` = **marshalling A→W
   automatique** (dérive `…A` de `…W`, élargit les chaînes d'entrée, **refuse** les pièges struct/OUT — garde-fou §0).
   **✅ CÂBLÉ ET PROUVÉ** : le thunk `--marshal DeleteFileA` est câblé dans le HLE (remplace le fait-main) et **bit-identique
   Wine sur 6 fixtures** (`win32_fileops`/`file`/`filetime`/`fileinfo`/`find`/`mmap`) — premier `…A` d'ARET **dérivé** plutôt
   qu'écrit. **Prolonger** : générer des familles entières de squelettes/marshalling sur un mur mesuré.
3. ✅ **FAIT — Corps Wine forme MOYENNE, deux tailles** : `GetFamilyCodePage` (petite boucle) **puis**
   `StrFromTimeIntervalW/A` (shlwapi, **corps entier à algorithme** + sa chaîne de 3 aides internes
   `WriteReverseNum`/`FormatSignificant`/`WriteTimeClass` + chaînes ressource), les deux **bit-identiques Wine**.
   La montée en taille rend visible le **coût cas-par-cas** : chaque corps traîne son propre arbre de dépendances,
   et l'oracle **tranche les cas-limites** (débordement `WideCharToMultiByte` : `cchMax` octets sans NUL ; quirk `iRet`
   toujours 0 pour la variante A). *(La forme LOURDE — §4 — est ce qui casse ce coût par-corps.)*
### 🎯 Plancher ntdll Nt* — ROADMAP COMPLÈTE (le milestone « DLL user-mode entières »)

> **But** : porter **une fois** le plancher syscall `Nt*` d'ntdll (≈**131** `Nt*`, doc 70 §5.0 — la chaîne user-mode
> Wine y bute) pour que **des DLL Wine compilées** (forme lourde) s'exécutent en autonome. **Méthode invariante** :
> chaque `Nt*` est **mesuré vs Wine** (§0), backé par l'état HLE existant quand il y en a un (registre `g_reg`, FS, fibers),
> **round-trip** quand l'état ARET démarre vide (registre), **`aret_partial`/abort** sur tout sous-cas non modélisé.
> **Deux formes par `Nt*`** : (a) **shim app-facing** `aret_Nt*(esp)` (une app importe le `Nt*` directement) ; (b)
> **real-ABI dans le plancher** `wine_heavy` (une DLL Wine **compilée** l'appelle en interne). Le cœur logique est partagé.

**Tranches (dans l'ordre) :**
1. **Registre — écriture/lecture** ✅ **FAIT (2026-08-02)** : `NtCreateKey`/`NtOpenKey`/`NtSetValueKey`/`NtQueryValueKey`
   (`KeyValuePartialInformation`)/`NtDeleteValueKey`/`NtClose`, backés par `g_reg` (mêmes handles que les `Reg*`). Parse
   `OBJECT_ATTRIBUTES` + `\Registry\Machine|User\…`, remplit `KEY_VALUE_PARTIAL_INFORMATION`, NTSTATUS. Prouvé **round-trip**,
   bit-identique Wine (`winecorpus/win32_ntreg`).
2. **Registre — énumération/info** ✅ **FAIT (2026-08-07)** : `NtQueryKey` (`KeyBasic/Node/FullInformation`),
   `NtEnumerateKey`, `NtEnumerateValueKey` (`KeyValueBasic/Full/PartialInformation`), `NtFlushKey`, `NtDeleteKey`, même
   `g_reg`. **3 quirks mesurés vs Wine** (jamais devinés) : (a) `MaxNameLen`/`MaxValueNameLen` en **octets** (chars×2) au
   niveau Nt, pas en caractères comme `RegQueryInfoKey` ; (b) **deux régimes petit-tampon distincts** — info-clé =
   `TOO_SMALL` puis `OVERFLOW`, énum-valeur = **`OVERFLOW` toujours** (chemin Wine différent de `NtQueryValueKey`) ;
   (c) énumération en **ordre trié case-insensible upcasé**, pas ordre de création (`g_reg` insertion-ordonné ⇒ tri à la
   volée, compare upcase-ASCII). `LastWriteTime` environnemental = 0, exclu. Prouvé round-trip bit-identique Wine
   (`winecorpus/win32_ntenum`, création non-alphabétique pour prouver le tri). Reste à la demande : `NtQueryValueKey`
   autres classes (non appelé par un driver mesuré).
3. **Fichiers** 🚧 (le gros des 131) — **première tranche ✅ FAIT (2026-08-07)** : `NtCreateFile`/`NtOpenFile`/`NtReadFile`/
   `NtWriteFile`/`NtQueryInformationFile` (classes `FileStandard`/`FilePosition`), backés par le **même modèle fd POSIX**
   que `CreateFile`/`ReadFile` (**HANDLE == fd**) + `translate_path` ; nom `\??\C:\…`/`\DosDevices\…` strippé. **Mesuré vs
   Wine** : disposition NT(0-5)→`Information` (CREATED/OPENED/OVERWRITTEN/SUPERSEDED selon existence), `ByteOffset` explicite
   **avance** la position (lseek+read, pas pread), 0-octet sur demande >0 = `END_OF_FILE`, `AllocationSize`=`st_blocks*512`
   (formule stat de Wine, sound+portable), IOSB non touché sur échec. Bornes sound (`RootDirectory`-relatif, `\Device\`/UNC,
   classes autres = `aret_partial`). **+ `NtSetInformationFile`** : `FileEndOfFile`(20)=`ftruncate`,
   `FilePosition`(14)=`lseek`, **`FileDisposition`(13)=delete-on-close** (`unlink` au `NtClose`). **+ `NtClose` raffiné** :
   table bornée fd→chemin+delete (peuplée à l'ouverture Nt\*) ⇒ ferme vraiment le fd et honore le delete ; désambiguïsation
   sûre (handles HLE = bases hautes taguées, fd = petit entier ⇒ n'agit que sur nos fd). **+ `NtQueryDirectoryFile`
   (`FileNamesInformation`)** : classe sans champ environnemental ⇒ bit-identique ; `opendir`/`readdir` + snapshot trié
   par handle, `.`/`..` puis tri case-insensible, single-entry + multi-entry empaqueté 8-aligné, épuisé →
   `STATUS_NO_MORE_FILES`, pattern NULL/`"*"` (autre = `aret_partial`). **+ `FileBothDirectoryInformation`(3)** (classe de
   `FindFirstFile`) : attr/EOF/AllocationSize/EaSize/nom déterministes (EOF/Alloc = 0 pour un répertoire, quirk Wine
   mesuré) ; dates env. remplies depuis `stat` mais exclues du fixture ; short-name 8.3 non modélisé (`ShortNameLength=0`,
   sound). Patterns : NULL/`"*"`/`"*.*"` (match-all) bit-identiques ; **glob générique = LIMITE DURE** (Wine matche contre
   le short-name 8.3 environnemental — `*.txt` matche `a.txtx` via `A~1.TXT`) ⇒ `aret_partial` sound, consigné. Round-trip
   bit-identique Wine (`winecorpus/win32_ntfile`, `win32_ntdir`, `win32_ntreg` non régressé). **Reste** :
   `NtDeviceIoControlFile` — au besoin mesuré.
4. **Divers à la demande** 🚧 — **`NtDelayExecution` ✅ FAIT (2026-08-07)** : ≈`Sleep`, intervalle 100 ns négatif=relatif
   → `aret_fiber_sleep` (horloge virtuelle), `STATUS_SUCCESS`, absolu = `aret_partial` ; fixture `win32_ntdelay`
   bit-identique. **`NtAllocateVirtualMemory`/`NtFreeVirtualMemory` ✅ FAIT (2026-08-07)** : ≈`VirtualAlloc` (calloc,
   `MEM_COMMIT` zéro-init), `*RegionSize` arrondi page 4096, `STATUS_SUCCESS` ; adresse non-déterministe ⇒ contrat testé
   (`win32_ntvm`), pas l'adresse. **Reste** : `NtQuerySystemInformation`, `NtQueryPerformanceCounter`,
   `NtQueryInformationProcess`/`Thread`… **piloté par la mesure** (`--mode walls`/besoin d'un driver), pas spéculatif.
5. **Variante real-ABI dans le plancher** 🚧 — **registre ✅ FAIT (2026-08-07)** : cœurs `aret_ntreg_*` exposés
   (`aret_win32.c`, mêmes que les shims esp, behavior-preserving/hash inchangé) + wrappers NTAPI `wine_heavy/ntdll_ntreg.c`
   (fichier séparé) + **preuve autonome sans réseau** (`proof_ntreg.sh` : driver real-ABI + registre de référence ==
   vrai ntdll Wine, bit-identique). Prêt à câbler en production (tranche 6). **Reste** : cœurs real-ABI Nt\* **fichier** au
   besoin.
6. **Driver de bout en bout** 🚧 — **câblage builder ✅ FAIT (2026-08-07)** : `ntdll_ntreg.c` est `include_str!` + écrit
   + compilé/lié dans **tout build natif 32-bit** (boucle heavy-form `rtlstr`/`ntdll_floor`/`ntdll_ntreg`), cœurs
   `aret_ntreg_*` résolus depuis `aret_win32.c` ; **aucun conflit** de symboles (imports PE → shims `aret_*` ; code Wine
   compilé → `Nt*` nus). **Preuve NATIVE** (`proof_ntreg_native.sh`) : plancher registre real-ABI compilé par `cc` natif,
   ELF autonome sans Wine, bit-identique aux valeurs Wine. **Reste** : un **vrai fichier ntdll de Wine** consommant le
   registre en interne (`RtlpNt*` de `reg.c`, choisi par la mesure) vendoré + adaptateur esp + fixture PE = premier
   bout-en-bout non-chaîne. **Plancher COMPLET pour `reg.c` ✅ (2026-08-07)** : cœurs enum/delete exposés + wrappers NTAPI
   (`NtEnumerateKey`/`NtEnumerateValueKey`/`NtDeleteKey`) + stub `NtQueryInformationToken` ; surface Nt\* de `reg.c` (10)
   entièrement couverte, 2 preuves vertes. **🎯 CAPSTONE ✅ FAIT (2026-08-07)** : `reg.c` de Wine **entier** (768 l.,
   inchangé hors splice) **compile par `cc` natif** contre le shim étendu (`native/reg_types.h` : OBJECT_ATTRIBUTES/
   KEY_VALUE_*/RTL_QUERY_REGISTRY_TABLE/TOKEN_* + déclarations NTAPI des fonctions plancher/Nt\* appelées), se lie au
   plancher real-ABI + rtlstr + floor, et **round-trip une clé comme ELF autonome sans Wine**, bit-identique aux valeurs
   Wine (`proof_reg_native.sh`). **Premier fichier ntdll de Wine non-chaîne entier tournant sur le plancher.** Bug §0
   attrapé : décl implicite cdecl d'une fonction plancher stdcall → crash → tout déclarer NTAPI. **PLEINE PRODUCTION ✅ FAIT
   (2026-08-07)** : `reg.c` vendoré + câblé builder (boucle heavy-form) + adaptateurs esp `aret_RtlpNt*` (`aret_ntdll.c`) +
   stubs off-path ⇒ un **vrai PE** important `RtlpNtCreateKey` (fixture `win32_rtlpntreg`, `.def`+`.killat` pour l'export
   ntdll non documenté/non décoré) atteint la logique Wine **compilée-en-ARET** → plancher → g_reg, **bit-identique Wine**.
   Le milestone « DLL user-mode entières » est **franchi** sur un premier fichier. **Reste** : cœurs real-ABI `Nt*` fichier,
   d'autres fichiers ntdll, puis des DLL entières (loader multi-modules, Levier 1).

**Invariants** : registre/état vide ⇒ prouver en round-trip ; jamais une valeur système devinée ; hash inchangé (additif) ;
`@N` Nt\* déjà dans `stdcall_pops` (audit) ; chaque tranche = fixture winediff + entrée 71 + maj ici.

### 🎯 Levier 1 sur une VRAIE DLL binaire tierce à ALGORITHME RÉEL ✅ **FAIT (2026-08-08)**

> Au-delà des DLL-fixtures qu'on compile et des builtins « surface OS » (comctl32→gdi32, `Nt*`→`g_reg`) : **la `zlib1.dll`
> de Wine LIFTÉE** (96 Ko de vrai DEFLATE/inflate/crc32) sort **byte-identique** à Wine sur `compress`/`uncompress`/`crc32`/
> `adler32` (`winecorpus/lift_zlib`). **Aucun code nouveau** — l'intégration loader-multi-modules × vrai code tiers.
> **Le travail = la SÉLECTION mesurée** de la cible (§0), balayage des ~430 DLL i386 sur 3 filtres, chacun a éliminé un
> piège concret : (1) relais-stub (thunks `__wine_spec_imp_` **ET** forwarders — `version`/`lz32`/`msvcrt40`) ; (2) **stub
> Wine** (`RaiseException` ⇒ pas d'oracle — `msvcp140_2` special-math) ; (3) imports non couverts (`ucrtbase`/`user32`).
> `zlib1` = kernel32+**msvcrt** seuls, voie mémoire n'exerce que malloc/memcpy. **Point §0** : crc32 SIMD (pclmulqdq)
> masqué par CPUID ⇒ chemin scalaire liftable, sortie identique par garantie zlib. Détail 71 (2026-08-08).
> **Reste** : router `ucrtbase`→`msvcrt` par nom (débloque `cabinet`/`xmllite`/`mspatcha`…), au besoin mesuré.

### 🎯 Levier 1 sur le RUNTIME C++ GNU — la lacune n°1 MESURÉE 🚧 (libgcc ✅, libstdc++ = suite)

> **Piloté par la donnée (doc 90).** Le corpus de 1240 vrais PE32 FOSS classe la lacune n°1 **par #binaires** : le runtime
> C++ GNU (`libstdc++-6.dll` + `libgcc_s`) bloque **37-47 %** des binaires. Réponse = **Levier 1** (lifter, comme zlib).
> **Test pré-lift §0** (les deux, sur les DLL mingw de l'hôte) : `libgcc_s_dw2-1.dll` = **0 thunk / 0 forwarder**, `.text`
> ~130 Ko, imports **KERNEL32+msvcrt seuls** ⇒ **liftable MAINTENANT, autonome** ; `libstdc++-6.dll` = 0/0, `.text` ~1,3 Mo,
> importe **libgcc+KERNEL32+msvcrt** ⇒ liftable **une fois libgcc lifté** (multi-module). Ordre **forcé par la donnée**.
> **✅ libgcc FAIT (2026-08-08)** : `winecorpus/lift_libgcc.{c,def,withlocaldll}` — les helpers arithmétiques 64 bits
> (`__divdi3`/`__moddi3`/`__udivdi3`/`__umoddi3`/`__muldi3`/shifts, mesurés bloquants sur ~101 binaires) liftés
> **bit-identiques Wine** (grille de signe/overflow/wrap + accumulateur). Nouvelle affordance harnais **`NAME.withlocaldll`**
> (lifte une DLL runtime mingw copiée à côté de l'exe, pas un builtin Wine ; SKIP propre ; inerte si absente). Aucun code
> Rust/runtime touché ⇒ hash inchangé. Détail 71 (2026-08-08).
> **✅ libstdc++ étape 1 FAITE (2026-08-08) — le CHEMIN HEUREUX (sans EH) bit-identique Wine.** Faisabilité mesurée
> d'abord (`--mode walls` : 24 Mo liftés en **12 s**, 6252 fn, murs résiduels = bruit data-en-code + 30 imports
> filesystem/wide-char hors-chemin). Fixture `winecorpus/lift_libstdcxx.cpp` : `std::string`/`std::vector`+`std::sort`/
> `std::map`(`_Rb_tree`) — tout **lifté**, **1er cas d'une DLL liftée important une AUTRE DLL liftée** (libstdc++→libgcc),
> **bit-identique** (36 s transpile+run, 6008 fn). Outillage : **mingw g++ installé** (absent du conteneur de base) +
> affordance harnais **`.cpp`** (compile g++, SKIP propre sinon). Aucun code Rust/runtime ⇒ hash inchangé. Détail 71.
> **🚧 étape 2 (iostream) MESURÉE (2026-08-08) — le mur UNIFIE 2 et 3 : c'est l'EH/UNWIND de libgcc.** `std::cout` crashe au
> **static-init C++** (`0xc0000005 at 0x50746547` = "GetP") : `std::ios_base::Init` tire la machinerie **DWARF-2 EH frame
> registration** de libgcc (`__register_frame_info`, tirée **même sans throw**) + la résolution dynamique de démarrage — une
> routine hand-rolled appelle une **chaîne de nom d'API comme cible**. Pas le trou `GetProcAddress→0` (fault = pointeur mal
> calculé, pas appel-à-0). ⇒ **étape 2 (iostream) et étape 3 (throw/catch) = LE MÊME mur : le runtime EH/unwind GNU**, qui
> marche la **vraie pile machine** (DWARF CFI) — incompatible *shared-stack*. Détail 71 (2026-08-08 [DIAG]).
> **⚠️ NATURE DU 1er MUR iostream ROUVERTE (2026-08-08, gdb).** Le backtrace gdb (autorité) place le crash **dans libstdc++**
> (`sub_531f20`, deref de la valeur `0x50746547`="GetP"), atteint par **un appel indirect depuis `main`** — **pas** dans l'EH
> de libgcc (le ring de trace montrait des fn libgcc **déjà retournées**). Signature = **slot d'import non patché** (nom d'API
> déréférencé comme pointeur) ⇒ piste **loader/import** (tractable, générale), à confirmer par un rebuild `-O0 -g` + gdb à la
> ligne C. La **portée mesurée reste valable** (35,3 % du corpus, doc 90) ; seule la NATURE du 1er mur iostream est rouverte.
> **La brique EH/unwind GNU** (`__register_frame_info`/`_Unwind_*`/`__cxa_throw`/`__gxx_personality_v0`, DWARF-vs-shared-stack)
> reste **structurellement** requise pour l'étape 3 (`throw`/`catch`) — mais le crash iostream n'est **pas prouvé** en être.
> Détail + correction : doc 71 (2026-08-08 [DIAG] CORRECTION).
>
> **✅ RÉSOLU (2026-08-08) — iostream tourne bout-en-bout bit-identique Wine.** La NATURE du 1er mur, tranchée : (1) un slot
> auto-import (`&std::cout`) non propagé ⇒ **fix loader pseudo-reloc** (`d848e86`, cout **résolu**) ; (2) `std::cout`/`cin`/`cerr`
> **jamais construits** car les **ctors globaux de libstdc++** (`__CTOR_LIST__` via `__do_global_ctors`) sont no-op'és ⇒ **fix
> loader ctors** : `recover_ctor_list` par module (signature `8B 1D <imm32> 83 FB FF`, thunks résolus) + rebasing +
> `seed_functions` + émission au démarrage + stub faible no-op pour la glue `register_frame`. **18 ctors** exécutés, cout
> **construit**, `std::cout << …` **bit-identique Wine**. Gate multi-module ⇒ hash inchangé. Ce n'était donc **PAS** la brique
> EH (l'EH reste pour l'étape 3, `throw`/`catch`). **Reste** : EH C++ Itanium, puis **mesure corpus** sur les 463. Détail 71.
>
> **🚧 étape 3 (EH C++ Itanium) ENGAGÉE — sous-brique 1a ✅ (2026-08-09) : le PARSER `.eh_frame`/LSDA.**
> `src/analysis/gnu_eh.rs` (`gnu_eh_entries`, analogue GNU de `cxx_eh_entries`) récupère la métadonnée EH **depuis le binaire**
> (§0, rien de deviné) : CIE→FDE→LSDA (`.gcc_except_table`), call-site table + action table + type table. **Prouvé sur
> `eh.exe`** : FDE de `main` + 11 call-sites + les 3 typeinfo (`std::exception`/`const char*`/`int`). Encodages mingw/i386
> mesurés (lp_enc=omit, ttype=indirect|pcrel|sdata4, cs=uleb128) ; tout autre encodage ⇒ fonction **sautée** (sound).
> **Décision modèle** : `catch_types` = adresses des **slots** `type_info*` (indirect, liés au load par pseudo-reloc) — le
> dispatcher déréférence au throw pour matcher le typeinfo de `__cxa_throw`. Recovery-only ⇒ **hash inchangé**, difftest
> 272/272 ; tests unitaires auto-contenus (uleb/sleb/read_encoded + LSDA synthétique). Détail 71 (2026-08-09).
>
> **✅ 1b + 2a→2d FAITS (2026-08-09) — throw/catch bout-en-bout via le dispatcher ARET, bit-identique Wine.** Sous-briques
> livrées (chacune prouvée `bench/gnuehdiff.sh`, **5/5**) : **1b** setjmp d'établissement à l'entrée d'une fonction EH +
> `setpc` (PC de call actif par frame) + pop ; **2a** premier throw/catch `int` (match par égalité de pointeur de typeinfo) ;
> **2b** sous-typage (`catch(Base&)` attrape `throw Derived`) via la base-chain du type_info (vptrs ABI = slot IAT+8, connus
> à l'analyse sans charger libstdc++) ; **2c** destructeurs pendant l'unwind (`_Unwind_Resume`) + frame de continuation
> robuste (`setpc` capture `(pc, esp=frame base, ebp)`) ; **2d part 1** `catch(...)` + catch-by-value (copie via
> `__cxa_get_exception_ptr`, destruction de l'objet lancé ET copié à `end_catch`, dtor THISCALL) ; **2d part 2**
> `__cxa_rethrow` (`throw;`) + refonte du cycle de vie des frames (la frame établisseuse reste vivante à travers le catch ;
> `end_catch` de fermeture ne détruit pas l'exception rethrown) ; **2d part 3** héritage multiple à offset de base non nul
> (`this`-adjustment vers le sous-objet base attrapé, non-virtuel + public ; `end_catch` détruit la base d'allocation, pas
> le pointeur intérieur) ; **2d part 4** `end_catch` détruit l'exception **snapshotée** au `begin_catch` (fix use-after-free
> latent : throw d'une NOUVELLE exception depuis un catch, qui écrase l'exception en vol avant le `end_catch` de fermeture).
> Runtime `aret_cxa_*`/`aret_gnu_dispatch` (`aret_hle.c`), table call-site/catch émise
> (`aret_dispatch.c`), gates `is_gnu_eh_func`/`is_gnu_eh_frame` (émission inerte ailleurs ⇒ **hash inchangé**). **gnuehdiff 7/7**. **Reste EH** :
> bases **virtuelles** (offset en vtable), throw pendant l'unwind (`std::terminate`), exceptions imbriquées actives, puis
> **mesure corpus** sur les 463 (doc 90). Détail 71 (2026-08-09).

4. **Corps Wine — forme LOURDE** : compiler du `.c` Wine entier + **porter une fois le plancher `ntdll`/win32u**
   → couverture massive. *(Milestone.)* **🚧 OUVERTE ET MESURÉE (2026-08-02, `tools/gen_wine_heavy.py`)** : `rtlstr.c`
   de ntdll **compile INCHANGÉ** en objet i686 → **46 fonctions `Rtl*` réelles d'un coup**, sur un plancher **fini de 12
   primitives à porter** (conversions NLS `RtlMultiByteToUnicodeN`… + `RtlCompareUnicodeStrings`) — le reste du plancher
   (libc, heap) **existe déjà** dans le HLE. Shim de compat Wine (`tools/wine_heavy/` : `wine/debug.h`→no-op, `ddk/ntddk.h`
   vide, `ARRAY_SIZE`/limites 64-bit) **checké-in** (survit au conteneur éphémère — motivé par un wipe). Le coût est
   **par-fichier mesuré** (`wcstring.c` réclame 2 typedefs msvcrt de plus — l'outil le signale).
   **✅ MÉCANIQUE PROUVÉE bout-en-bout** (`tools/wine_heavy/proof.sh` mingw + `proof_native.sh` = build réel `cc` natif) :
   `rtlstr.c` compilé inchangé + **plancher ASCII 12-primitives** (`ntdll_floor.c`) → **bit-identique au vrai ntdll de Wine**.
   **✅✅ CÂBLÉ EN PRODUCTION (2026-08-02)** : `rtlstr.c` est **vendoré** (`runtime/wine_heavy/`), compilé par le builder
   (`src/builder/mod.rs`) en objets séparés à **flags par-fichier** (`-fshort-wchar`, `-I native/`, `-D__WINESRC__`, natif
   32-bit) et **lié dans chaque binaire** ; **24 adaptateurs `aret_Rtl*(esp)`** (`runtime/aret_ntdll.c`) routent les imports
   ntdll vers ces corps Wine. Un vrai PE importe `RtlInitAnsiString`/`RtlAnsiStringToUnicodeString`/`RtlIntegerToChar`/… →
   ARET les sert depuis Wine compilé → **bit-identique Wine** (`winecorpus/ntdll_rtlstr`). Hors i386 (64-bit/wasm) les corps
   Wine ne sont pas liés ⇒ adaptateurs = **abort sound** (garde `#if __i386__`). Hash inchangé, audit PASS. **⇒ la forme
   lourde tourne en production, autonome.**
   **✅ PLAN A (conversion CP1252 unifiée, 2026-08-02)** : `MultiByteToWideChar`(CP_ACP) de kernel32 **et** le plancher ntdll
   (`RtlMultiByteToUnicodeN`) partagent désormais **une seule** conversion `aret_cp1252_to_wc` (table CP1252 `u32_ansi_cp`
   déjà dans le HLE) → **>127 réellement modélisé** (€, guillemets courbes, tirets…), **bit-identique Wine sur les 256 octets**
   (`winecorpus/win_cp1252`). **✅ inverse (UTF16→ANSI) TERMINÉ** : `WideCharToMultiByte`(CP_ACP) + le plancher ntdll
   (`RtlUnicodeToMultiByteN`) partagent `aret_cp1252_from_wc` avec la **table best-fit de Wine MESURÉE** (`tools/gen_cp1252.py`
   balaie les 65536 code points → 696 entrées ; le reste = char défaut `?`, `lpUsedDefaultChar` posé) — **bit-identique Wine**
   incl. best-fit (Ā→A, ⁄→/) et défaut (`winecorpus/win_cp1252_rev`). **✅ OEM (CP437) TERMINÉ** : `tools/gen_cp437.py`
   mesure les tables forward (256) + reverse best-fit (727) sous Wine ; `RtlOemToUnicodeN`/`RtlUnicodeToOemN` + kernel32
   `CP_OEMCP` partagent `aret_cp437_to_wc`/`aret_cp437_from_wc` — **bit-identique Wine** (`winecorpus/win_cp437`, Ç↔0x80,
   α→0xE0, ▓→0xB2, best-fit+défaut). **Reste** (petit sous-cran) : l'**upcase-Unicode** (`RtlUpcase*ToMultiByte/Oem` >127). **🚧 PLAN B ouvert + MESURÉ (2026-08-02)** : `gen_wine_heavy.py` profilé sur 5 fichiers ntdll — chacun a
   un **coût de shim incrémental** (mesuré, pas deviné) : `version.c`/`large_int.c` tirent `ddk/wdm.h` + conflits
   `_Interlocked*` ; `wcstring.c`/`string.c` réclament les typedefs msvcrt (`__msvcrt_long`…). Shim rendu plus robuste
   (`ntdll_misc.h` inclut `<winternl.h>` avant les prototypes du plancher ⇒ `NTSTATUS` garanti pour tout fichier), `rtlstr.c`
   toujours vert. ⇒ **la tuyauterie généralise, mais « DLL user-mode entières » = milestone** (expansion shim/plancher
   soutenue, multi-sessions), conforme au 80 §1.2. Chaque fichier ajouté suit `rtlstr.c` : vendorer + adaptateurs + fixture.

---

## 3. Invariants à ne jamais casser (rappel)

- **Autonome au runtime** : Wine/metadata servent à *fabriquer*, jamais à *exécuter*. Le binaire est ELF/WASM natif.
- **Sound** (§0 du 70) : tout extrait/porté est **vérifié contre l'oracle** (winediff / Windows réel) ; juste, ou
  **arrêt bruyant**. Automatiser retire l'écriture, **pas la preuve**.
- **Portes** : hash transpile inchangé (additif), stdcall_audit PASS, winediff vert — à chaque cran.
- **Licence** : Wine LGPL/GPL — obligations de distribution (décision produit, pas blocage technique).

### 🗂️ Chantier SÉPARÉ à mesurer plus tard — « largeur de DLL tierces » (noté 2026-08-08, NE PAS mélanger avec l'EH)

Constat de l'échantillon non-throw (doc 90) : les binaires C++ **sans** throw/catch du corpus sont bloqués non par l'EH
mais par la **largeur de DLL tierces** (`libLLVM-21`, `libclang-cpp`, `libxapian`, Qt, tesseract…) + des **trous de
récupération d'appels indirects** (vtables/pointeurs non récupérés dans le code propre du binaire). C'est un **axe Levier 1
distinct** (lifter davantage de DLL tierces, ou router/mesurer leurs surfaces) **à ouvrir séparément et à mesurer** — **pas
maintenant**, on ne le mélange pas avec la brique EH (populations disjointes : EH = les 102 throw-users ; DLL-breadth = les
77 non-throw). À reprendre après l'EH, avec sa propre mesure de portée.

### 📊 MESURE Levier 0 — la brique EH sur de VRAIS throw-users (2026-08-14) : le mur n'est PAS l'EH, c'est libstdc++

**Échantillon** (téléchargé de `repo.msys2.org/mingw/mingw32`, mingw32 = repo gelé de 248 paquets ; 15 paquets variés → **62 PE**
exe+dll). Filtre `import __cxa_throw` ⇒ **10 throw-users GNU/Itanium** : apps (`ninja`, `fluidsynth`, `pzstd`) + libs
(`libxapian-45`, `libspdlog`, `libjsoncpp`, `libfmt`, `libcppdap`, `libgraphite2`, `libfluidsynth`). *(harfbuzz absent = compilé
`-fno-exceptions` — donnée réelle, pas tous les C++ font de l'EH.)* `--mode walls` sur chacun (statique, pas d'exécution).

**Résultat décisif (2 constats)** :
1. **La brique EH est COMPLÈTE et confirmée sur du vrai code.** Les imports du **mécanisme** EH — `__cxa_throw`,
   `__cxa_begin/end_catch`, `__cxa_rethrow`, `__cxa_get_exception_ptr`, `_Unwind_Resume`, `_Unwind_RaiseException`,
   `__gxx_personality_v0` — sont **0 non-implémentés** sur les 10 binaires (les shims du brick les couvrent). Les
   **lift-gaps d'instructions = bruit** (SSE `pminub`/`prefetcht0`, `ud2`, `int 0x29`) comme sur tout le corpus.
2. **Le mur = la largeur libstdc++/libgcc/libwinpthread**, pas l'EH. Sur **547** lignes d'import-wall (10 binaires),
   **195 (36 %) sont du C++ manglé `_Z…`** = libstdc++/libgcc. Tête par #binaires bloqués : `operator new`/`delete`
   (`_Znwj`/`_ZdlPvj`/`_Znaj`), les **helpers `std::__throw_*`** (`__throw_length_error`/`__throw_logic_error`/
   `__throw_bad_alloc`/`__throw_out_of_range_fmt`/`__throw_bad_array_new_length` — ceux qui **construisent puis lancent**
   une exception `std::`), `std::string` (`basic_string::_M_*`), `std::map/set` (`_Rb_tree_increment`/`_insert_and_rebalance`),
   `__cxa_guard_acquire/release/abort` (statiques locaux), `__dynamic_cast` (RTTI), `udivdi3`/`divdi3` (**libgcc, déjà
   lifté**), `pthread_mutex_*` (libwinpthread).

**⇒ Conclusion data-driven** : la brique EH (2a→2d) a fait ce qu'elle devait ; **le prochain mur mesuré pour faire tourner un
vrai throw-user bout-en-bout est l'axe DLL-tierces** — lifter **libstdc++** (+ **libgcc** déjà ✅, + **libwinpthread**) à côté
du binaire (`--with-localdll`, doc 82 « libgcc/libstdc++ étapes 1-2 » déjà prouvées). Les `std::__throw_*` liftés
construiront l'exception `std::` puis appelleront `__cxa_throw` (mon shim) → mon dispatcher : c'est **la convergence EH ×
DLL-tierces**, désormais **justifiée par la mesure**, pas supposée. Petits shims EH-adjacents (`__cxa_guard_*`,
`__dynamic_cast`, `__cxa_call_terminate`) = gains bornés, mais **utiles seulement une fois libstdc++ lifté** (la masse est là).
*(Corpus non committé : éphémère + licence GPL/LGPL ; fetcher reconstructible depuis les liens doc 90.)*

**🚧 Sonde de CONVERGENCE EH × libstdc++ (2026-08-14) — le lifting effondre le mur C++, un mur comportemental reste dans libwinpthread.**
Lift multi-module `--with-dll` des **3 runtimes** (libstdc++-6 + libgcc + libwinpthread) à côté du binaire : **ça marche
mécaniquement**. Sur `ninja.exe` le mur d'imports tombe **73 → 58**, **tous les `_ZNSt…` disparaissent** (servis par le
libstdc++ lifté), **0 appel indirect non résolu** ; il ne reste que de la surface **OS** (IOCP/named-pipes/`AddVectoredExceptionHandler`/
volumes) — donc ninja end-to-end = axe **largeur OS**, pas l'EH. Sur une fixture minimale `throw std::runtime_error(std::string)` /
`catch(const std::exception&)` / `.what()` (Wine = `start`/`caught: boom-42`/`done`) : ARET transpile+compile+lie **6531 fn**,
mais **abort au 1ᵉʳ throw**. Diagnostic (gdb) : la pile `main → … → aret_call → sub_19f4a70 → aret_abort` ; `aret_abort` = le
**shim de `abort()`** ⇒ c'est le **code lifté lui-même** qui appelle `abort()`. `sub_19f4a70` = routine **thread/once-init de
libwinpthread** (`GetCurrentThreadId`+`CreateEventA`) ; un helper `sub_19f41f0` **rend 1** sous ARET ⇒ branche `abort()`+`ud2`
que **Wine ne prend pas**. ⇒ **divergence COMPORTEMENTALE dans le libwinpthread lifté** (en amont : un retour de shim Win32 ou
un bug de lift), atteinte parce que **libstdc++ prend un lock au 1ᵉʳ throw**. **La brique EH n'est pas en cause** ; c'est de la
lift-correctness du runtime de threads. **Prochaine étape = forensics dédiée** (relay ARET↔Wine sur `sub_19f41f0`/ses callees),
tâche séparée. *(La convergence EH côté dispatcher reste prête ; il faut d'abord que le runtime de threads lifté ne diverge pas.)*

**✅ Mur 1 RÉSOLU (2026-08-14) — `DuplicateHandle` du pseudo-handle `GetCurrentThread()`.** Le relay (I11) a pointé
`DuplicateHandle(-1,-2,-1,&out)=FALSE` : `aret_DuplicateHandle` faisait `dup(src)` en supposant un fd, mais `-2` = pseudo
courant-thread ⇒ échec ⇒ le libwinpthread lifté abort à l'init pthread. Fix général : résoudre le pseudo (et un vrai handle
thread) en un **vrai handle de fibre distinct** ; pseudo process → lui-même ; event/mutex/sem → même objet ; sinon fd → `dup()`.
Gardé `winecorpus/win32_duphandle` bit-identique Wine (hash inchangé, difftest 272/272, winediff 232/234). La fixture C++
**franchit l'init** (`start`).
**🚧 Mur 2 = LE VRAI cœur d'étape 3 — le routage de la famille EH.** Au `throw`, `__cxa_throw` route vers le **libstdc++
lifté**, qui appelle le **`_Unwind_*` de libgcc lifté** (le **dérouleur DWARF**) ⇒ abort (le DWARF marche la vraie pile machine,
incompatible *shared-stack*). **C'est la raison d'être de la brique EH** : la famille `__cxa_throw`/`__cxa_begin/end_catch`/
`__cxa_rethrow`/`__cxa_get_exception_ptr`/`_Unwind_Resume`/`_Unwind_RaiseException`/`__gxx_personality_v0` doit **router vers les
shims HLE d'ARET** (le dispatcher de la brique), **PAS** vers le code lifté de libstdc++/libgcc — **même quand ces DLL sont
liftées**. Le fix = **override loader** : une denylist de symboles EH-runtime qui gagnent toujours sur l'export d'un module
lifté. Chantier loader dédié (attention aux fixtures lifting-DLL existantes — régression). **C'est le prochain cran de la
convergence.**
**✅ Mur 2 RÉSOLU (2026-08-14) — 🎯 la convergence tourne bout-en-bout.** Deux fixes : (b) **denylist EH dans
`resolve_module_imports`** — la famille EH reste un import ⇒ shims HLE, même DLL liftée (additif, hash inchangé, **0
régression lifting-DLL** : comctl32/zlib/libgcc/libstdcxx verts) ; (c) **`gnu_eh_abi_vptrs` cherche aussi les EXPORTS
liftés** (vtables ABI `_ZTVN10__cxxabiv…` ⇒ VA+8) pour classer `std::runtime_error`. Résultat : `winecorpus/lift_stdexcept.cpp`
(`throw std::runtime_error`/`catch(std::exception&)`/`.what()` + logic_error, sur libstdc++/libgcc/libwinpthread liftés)
**bit-identique Wine**. **1er vrai C++ dont throw/catch tourne end-to-end via le dispatcher ARET sur libstdc++ lifté.**
Portes : hash inchangé, difftest 272/272, gnuehdiff 7/7, winediff 233/235.
**✅ Étape 3b (2026-08-14) — throw ORIGINÉ DANS libstdc++ lifté remonte au catch de l'exe** (`vector::at` → `out_of_range`,
`lift_stdthrow.cpp`, bit-identique Wine). Deux fixes : **host-back de la famille EH exportée** (`crt_symbol` reconnaît
`__cxa_*`/`_Unwind_*` ⇒ les appels DIRECTS intra-libstdc++, invisibles à la denylist d'imports, vont au shim ; + shims
cold-path loud-abort, sinon link error — attrapé sur `lift_libgcc`) et **égalité type_info par NOM** (mingw
`__GXX_MERGED_TYPEINFO_NAMES=0` : l'exe et libstdc++ ont chacun une copie COMDAT faible, adresses distinctes car ARET ne
fusionne pas les symboles faibles). winediff **234/236**, 0 régression lifting-DLL. **Reste** : bases virtuelles,
`std::terminate`, puis mesure corpus.
**🚧 1er VRAI binaire tiers testé (2026-08-14) — jsoncpp : la brique EH n'est PLUS le mur, c'est la lift-correctness.**
Driver minimal `Json::Value(objectValue).asInt()` → jsoncpp lance sa **propre** `Json::LogicError` (: `Json::Exception` :
`std::exception`) **depuis libjsoncpp**, attrapée en `std::exception&` dans l'exe (Wine : `start`/`caught: Value is not
convertible to Int.`/`done`). ARET lifte **4 DLL** (libjsoncpp + libstdc++ + libgcc + libwinpthread), démarre (`start`), puis
**segfault** (`0xc0000005`, `mov %eax,(%edx)` à `sub_454890+15062`, `edx=0xc80e010` = pointeur invalide) sur le chemin
`asInt` (main → jsoncpp/libstdc++) **avant** le throw. ⇒ **la convergence EH tient** (le chemin d'exception n'est pas
atteint), le nouveau mur est un **bug de lift-correctness dans du vrai code C++ dense** (jsoncpp/templates libstdc++ inlinés)
— **axe distinct** (largeur DLL tierces / lift-correctness), forensics dédiée (`-O0 -g`+gdb à la ligne C, ou relay/funcdiff).
Task séparée. C'est la 1ʳᵉ **mesure corpus** réelle : elle confirme que l'EH est fait et pointe le prochain axe par la donnée.
> **LOCALISATION PRÉCISE (2026-08-14, `ARET_DEBUG=1`+gdb)** : crash `chunk_1.c:22234` `*(uint32_t*)v156 = v149`
> (pose d'un **vptr de sous-objet** pendant une construction à **héritage VIRTUEL / VTT**). `sub_454890` = fonction de
> **libstdc++ lifté** (std:: iostream, via `throwLogicError`→`std::ostringstream`). Valeurs : `v147=0x6258c8` = une vtable
> **structurellement valide** (offset-to-top=0, typeinfo=`0x620a9c`, fns=`0x4b7440`/`0x4b7410`), mais `v148 = *(v147-12)`
> (l'entrée **vbase-offset**) = **`0x51dea0`** = valeur ÉNORME (5 Mo) là où un offset de sous-objet doit être minuscule ⇒
> `v156 = v148 + v129(objet) = 0xc80e010` non mappé ⇒ fault. Dump vtable `0x6258b8` : `51dea0 51dea0 51de50 51de90`
> (les slots vbase-offset **corrompus**, ressemblent à des adresses module non/mal relocalisées). **Hypothèse** : bug de
> **relocalisation** sur les entrées vbase-offset des vtables libstdc++ (ne devraient pas être rebasées) **ou** pointeur de
> **construction-vtable/VTT** (`*(0x49038c)`) faux. Session focalisée : mapper `0x454890`→RVA libstdc++ original, comparer
> les vbase-offsets, isoler reloc-vs-lift. **La brique EH n'est pas en cause** (chemin d'exception jamais atteint).
> **DISSECTION APPROFONDIE (2026-08-14) — ARET est FIDÈLE au guest ; la divergence est runtime.** Mappé : `sub_454890` =
> **libjsoncpp** (base merged **0x440000**, RVA 0x14890 = orig `0x65254890`) — un gros ctor global qui bâtit les type_infos.
> `0x49038c`/`0x490390` = `__imp___ZTVN10__cxxabiv117__class_type_infoE`/`__si_...` (auto-imports vtable, résolus au symbole
> vtable, corrects). Guest exact (0x65254930+) : `lea -0xe8(%ebp),%ebx (objet) ; mov 0x6529038c,%eax (vtable) ; mov
> -0xc(%eax),%ecx (=*(vtable-12)) ; add %ebx,%ecx (=*(vtable-12)+objet) ; mov %eax,(%ecx)` — **ARET lifte ça À
> L'IDENTIQUE** (`v156=v148+v129 ; *v156=v149`). Et `*(vtable-12)=0x51dea0` = **exactement** le `0x6febdea0` original
> **rebasé** (`0x4a0000+0x7dea0`) ⇒ **relocalisation correcte, lift correct**. Le motif = pose d'un **2ᵉ vptr** à
> `objet + *(vtable-12)`, où `*(vtable-12)` DEVRAIT être un **petit offset de sous-objet** mais vaut un **pointeur**. ⇒ soit
> l'`__imp` doit résoudre vers un **autre point** de la vtable (adresse-point vs symbole), soit l'adresse de pile de l'objet
> sous Wine fait « marcher » l'addition. **Trancher = winedbg sur l'original** (valeurs runtime de `*(0x6529038c)`,
> `*(vtable-12)`, `ebx`, `ecx`) — analyse statique épuisée. Localisation maximale atteinte ; suite = session winedbg dédiée.

> **✅ CAUSE RACINE TROUVÉE ET CORRIGÉE (2026-08-15, winedbg) — pseudo-relocs multi-module : ARET n'appliquait QU'UNE
> liste sur N.** La session winedbg dédiée a tranché. libjsoncpp désactivée-ASLR (bit `DYNAMIC_BASE` effacé ⇒ charge à
> sa base préférée 0x65240000, adresses fixes), bp au site du crash. Sous Wine, l'instruction `a1 8c 03 29 65`
> (`mov 0x6529038c,%eax` dans le fichier) est **désassemblée `mov 0x781956ac,%eax` = libstdc+++0x1856ac** : **Wine a
> RÉÉCRIT l'opérande** (relocalisation pseudo-runtime mingw, auto-import de **données** inter-DLL). `0x781956ac` =
> **`_ZTTNSt7__cxx1119basic_ostringstreamIcSt11char_traitsIcESaIcEEE + 4`** = la **VTT** (Virtual Table Table) de
> `std::ostringstream` ; `eax=*(VTT+4)`=construction-vtable ; `*(eax-12)=0x40` = **petit offset de base virtuelle**
> `basic_ios` — construction à héritage **virtuel parfaitement légitime** (jsoncpp formate son message d'erreur via
> `throwLogicError`→`ostringstream`). **ARET lisait l'opérande NON réécrit** `0x49038c` → `*(0x49038c)`=0x6258c8
> (la vtable `__class_type_info`, contenu du slot **voisin** 0x5038c) → `*(vt-12)`=0x51dea0 (un pointeur) → écriture
> hors-bornes. **Deux slots adjacents** : 0x50388 importe la VTT ostringstream, 0x5038c importe `__class_type_info` ;
> l'immédiat bakée `0x5038c` = slot(0x50388)+4, et la pseudo-reloc (sym=0x50388) doit le réécrire → VTT+4. **Bug** :
> `apply_runtime_pseudo_relocs` (a) s'arrêtait à la **1ʳᵉ** liste trouvée (`break 'find`) et (b) calculait
> `slot_va = primary.image_base + sym` (base **exe**), alors qu'un lift multi-module a **une liste par module** avec des
> RVA relatives à la base **rebasée de CE module**. ARET appliquait donc la liste de l'**exe** (jtest) et ignorait celles
> de jsoncpp/libstdc++. **Fix général** (`src/loader/mod.rs`) : appliquer **chaque** liste avec la base rebasée de son
> module (modules placés contigus/ascendants ⇒ frontière = base suivante). Portée : **tout auto-import de données inter-DLL**
> (`std::cout`, iostream, VTT…). Portes : hash **19acad982194bf07 inchangé** (no-op hors multi-module), difftest **272/272**,
> cargo test **79+**, **0 régression lifting-DLL** (lift_libgcc/zlib/libstdcxx/stdexcept/stdthrow/stddtor/comctl32 verts).
> **jsoncpp franchit le mur VTT** (le ctor `sub_454890` progresse, construit l'ostringstream) **puis bute plus loin** :
> nouveau crash dans du code **libstdc++** (`sub_526010`, usage de l'ostringstream) sur un **vptr d'objet garbage**
> (`*(objet)=0x7bca08` n'est pas une vtable valide) — **mur suivant distinct** = construction dense de `std::ostringstream`
> (`__cxx11`, héritage virtuel + VTT complet), plus riche que l'`ostream` de `lift_libstdcxx`. Outil-couple validé :
> **winedbg (vérité Wine) ↔ gdb (ARET) sur les mêmes adresses** (le relay/traceur ne servent pas ici — calcul intra-module,
> aucune API). Suite = forensics du vptr d'ostringstream (borné : session suivante).

> **✅ BUG #2 TROUVÉ ET CORRIGÉ (2026-08-15) — DOUBLE application des pseudo-relocs (statique + relocateur runtime du DLL).**
> Après le fix multi-module, jsoncpp construit l'ostringstream **puis crashe dans `std::basic_ostream::sentry`** (via
> `std::__ostream_insert`, l'`<<`) sur un **vptr d'objet faux**. Vérité Wine : le vptr = **0x6ffc66d8 = RVA 0x1866d8 =
> `_ZTVNSt7__cxx1119basic_ostringstream…+0xc`** (vtable propre, `*(vptr-12)=0x40`). ARET : **0x7bca08 = RVA 0x31ca08** (pas
> une vtable). Traçage : la table de données à jsoncpp RVA 0x3d310 (lue par le ctor, `v33=*(0x47d310)`) valait **0x6266d8
> (CORRECT) après le map** mais **0x7bca08 après les ctors** — un **watchpoint** a montré `sub_4686e0` (=
> `_pei386_runtime_relocator` mingw, appelé par le **DllMain** de jsoncpp) la re-patchant : `0x6266d8 → 0x7bca08` (= vtable
> + **2·delta**). **Cause** : ARET applique les pseudo-relocs **statiquement** (avant lift) **ET** exécute le DllMain du DLL
> lifté, dont le relocateur runtime applique **la même formule** `*(t) += (*(slot) − slot_addr)` une 2ᵉ fois. Sur une cible
> **données** c'est une double-application **vivante** (le C lifté lit `*(t)` au runtime) → valeur doublée → vptr non mappé →
> SIGSEGV. Sur une cible **code**, l'écriture runtime tombe dans le `.text` guest **mort** (ARET exécute le C compilé dont
> l'immédiat est figé au transpile) ⇒ inoffensive, mais le patch **statique** du code reste nécessaire.
> **Fix général** (`apply_pseudo_relocs_for_module`, `src/loader/mod.rs`) : pour un module dont le **DllMain tourne**
> (`relocator_bases` = hinstances de `dll_inits`), ne static-patcher **que les cibles CODE** (section exécutable) ; laisser
> les cibles **données** au relocateur runtime. L'EXE (auto-main saute son entrée CRT ⇒ relocateur jamais exécuté) et un DLL
> sans DllMain gardent le patch complet. Portes : hash **inchangé**, difftest transpile 4/4, **0 régression** (lift_libstdcxx
> [iostream/`std::cout`, exactement ce chemin de données] + lift_stdexcept/stdthrow/stddtor + lift_libgcc/zlib +
> comctl32_imagelist/progress **verts**). **Effet** : 0x47d310 reste **0x6266d8** (correct), le sentry passe, **jsoncpp
> progresse encore** (`sub_454890` 22264→22289) et bute au **mur #3** : `Json::LogicError::LogicError(const std::string&)`
> (`sub_450930`), un `string[len]=0` où **`len` (v66) vaut un pointeur** (0xc2f4190) au lieu d'une petite longueur — **même
> signature** (valeur devant être petite = pointeur) dans la construction du `std::string` du message. **Mur suivant, borné.**
> Bilan session : **2 bugs de lift-correctness GÉNÉRAUX corrigés** (pseudo-relocs multi-module + double-patch), jsoncpp
> franchit 3 murs successifs ; le binaire dense en révélera d'autres — continuation en session suivante.

> **🎯 MILESTONE (2026-08-15) — jsoncpp tourne BOUT-EN-BOUT, bit-identique Wine. 1er VRAI binaire tiers complet.**
> Le mur #3 était un **esp-drift de 8** dans le `std::string` du message de `Json::LogicError`. Guest (winedbg + gdb) :
> `_M_construct` (chemin heap, len>15) appelle `basic_string::_M_create(size_type&, uint)` via le thunk
> `sub_4682f8 = jmp *[IAT]` ; `_M_create` est **`__thiscall`** (this en ecx, 2 args pile, **`ret 8`**) — le `sub $8,%esp`
> juste après le call le prouve. **`compute_callee_pops` ne propageait le pop des tail-calls que pour les `jmp` DIRECTS**
> (`near_branch_target`) ; le thunk `jmp *[IAT]` (indirect) était ignoré ⇒ pop=0 ⇒ chaque appelant de `_M_create`
> laissait esp **8 bas** ⇒ la longueur relue depuis le slot dérivé = **pointeur garbage** (0xc2f4190 au lieu de 0x20) ⇒
> `string[len]=0` hors-bornes → SIGSEGV. **Fix général** (`compute_callee_pops`, `src/ir/build.rs`) : suivre AUSSI le
> tail-call d'un thunk `jmp [abs32]` dont le slot IAT (résolu par le loader multi-module) pointe une **fonction récupérée**
> ⇒ le pop de la cible (`ret 8`) se propage. Reçoit `prog` pour lire le contenu du slot ; helper `abs_mem_jmp_slot`.
> **Additif** : un slot non résolu vers une entrée récupérée n'ajoute aucune arête (imports système ⇒ `stdcall_pops`
> inchangé) ⇒ **hash `19acad982194bf07` inchangé**, single-binary intact. **Résultat** : `jtest.exe`
> (`Json::Value(objectValue).asInt()` → `Json::LogicError` : `Json::Exception` : `std::exception`, sur libjsoncpp +
> libstdc++ + libgcc + libwinpthread liftés) sort **`start` / `caught: Value is not convertible to Int.` / `done`** = Wine.
> **Portée** : tout appel à une fonction membre `__thiscall`/`__stdcall` d'une DLL liftée via un thunk d'import (massif en
> C++ : `std::string`, conteneurs, iostream). **Portes** : hash inchangé, difftest **272/272**, **funcdiff 0 divergence**
> (21859 scorées), cargo test **79+**, **0 régression lifting-DLL** (8 fixtures vertes). **Garde ajoutée** :
> `winecorpus/lift_stdstring` (une `std::string` >15 chars = chemin heap `_M_create` thiscall, throw/catch, bit-identique
> Wine — sans le fix : crash/hang). **Bilan session : 3 bugs de lift-correctness GÉNÉRAUX corrigés** (pseudo-relocs
> multi-module + double static/runtime + callee-pop thiscall via thunk d'import) ⇒ **le 1er vrai binaire tiers C++ (jsoncpp)
> tourne bout-en-bout via le dispatcher EH d'ARET sur runtime C++ GNU lifté.** Prochain : d'autres binaires du corpus (doc 90).

### 📊 MESURE post-milestone (2026-08-15) — le mur a BOUGÉ : le runtime C++ n'est plus le mur, c'est la surface OS/CRT

Question data-driven après le milestone jsoncpp : **une fois le runtime C++ lifté (correctement), quel est le prochain mur ?**
Mesure sur les **3 apps throw-users** du corpus EH (`ninja`, `pzstd`, `fluidsynth` — mingw32, échantillon doc 90) : `--mode
walls` **AVEC** les 3 runtimes liftés (`--with-dll` libstdc++/libgcc/libwinpthread), imports non-implémentés restants agrégés.

**Constat n°1 — l'axe runtime C++ est CLOS** : **0** import C++ (`_Z*`/`__cxa_*`/`_Unwind_*`/`__gxx_*`) ne reste
non-implémenté sur les 3 apps une fois le runtime lifté. Les 3 fixes de la session (pseudo-relocs multi-module + double
static/runtime + callee-pop thiscall via thunk) rendent le lift du runtime C++ **correct end-to-end**, ce que jsoncpp prouve
bout-en-bout. Le mur mesuré dominant de doc 90 (37-47 % des binaires importent le runtime C++) est **franchi** pour ces binaires.

**Constat n°2 — le mur suivant = surface OS/CRT, deux familles** (chacune sur les 3/3 apps) :
- **CRT MSVCRT wide-char / locale** : `_wopen`/`_wstat64`/`_wfindfirst32`/`_wfindnext32`/`_wmkdir`/`_wchdir`/`_wgetcwd`/
  `_wfullpath`/`_wchmod`/`_wutime` (famille **fichier Unicode**), `_wcsxfrm`/`_wcsftime`/`wcsxfrm`/`strxfrm` (collation/locale),
  `getwc`/`putwc`/`ungetwc`, `_aligned_malloc`/`_aligned_free`, `_endthreadex`, `_ultoa`, `_p___mb_cur_max`.
- **Win32 OS** : `CreateHardLinkW`/`RemoveDirectoryW`/`GetVolumeInformationW`/`GetDiskFreeSpaceExW`/`Find{First,Next}VolumeW`/
  `FindVolumeClose` (**FS/volumes Unicode**), `GetProcessTimes`/`GetThreadTimes`/`Get/SetThreadContext`/`{Get,Set}ProcessAffinityMask`/
  `GetTickCount64`/`GetSystemTimeAdjustment` (**introspection process/thread + horloge**), `{Add,Remove}VectoredExceptionHandler`.

**⇒ Verdict** : le prochain axe **mesuré** (post-runtime-C++) est la **largeur HLE OS**, dominé par la **famille fichier
Unicode `_w*`/`*W`** (ouvrir/statter/lister/créer des fichiers et volumes en wide-char) + l'introspection process/thread. C'est
un axe **HLE classique** (comme les familles palette/shell32/CSIDL déjà faites), pas une nouvelle brique de fond. **Caveat** :
échantillon de 3 apps (les throw-users runnables du cache) ; carte **statique** post-lift (ne certifie pas le comportement,
cf. §wallsweep). Les apps ne tournent pas bout-en-bout **pas** à cause du C++ mais de cette surface OS — et certaines ne sont
pas des CLI propres (`pzstd --version` timeoute **sous Wine** aussi). jsoncpp reste la preuve end-to-end propre ; l'axe OS/CRT
`_w*` est le prochain chantier data-driven.

### ✅ AXE OS wide-char COUVERT (2026-08-15) — 4 lots, boucle mesure→fix→re-mesure bouclée

Les 4 lots (fichier `_w*`, Win32 FS/volumes `*W`, locale/stdio wide, introspection process/thread), chacun un shim mince
réutilisant les corps narrow + garde bit-identique Wine : `crt_wpath`, `win32_wfs`, `crt_wlocale`, `win32_procinfo`.
**Re-mesure `--mode walls` (runtime lifté) sur les 3 apps** : **0** des familles couvertes ne reste non-implémentée (tout le
set `_w*`/`wcsxfrm`/`wcsftime`/`getwc`/`putwc`/`GetVolumeInformationW`/`RemoveDirectoryW`/`CreateHardLinkW`/`GetDiskFreeSpaceExW`/
`GetProcessTimes`/`GetThreadTimes`/`*AffinityMask`/`GetSystemTimeAdjustment` a **disparu du mur**). **Restent** (3/3 apps) :
(a) **abort sound assumé** — `Find{First,Next}VolumeW`/`FindVolumeClose` (GUID env-dépendants), `Get/SetThreadContext`
(hors shared-stack), `Add/RemoveVectoredExceptionHandler` (livraison non câblée) ; (b) **petits reliquats faciles** non encore
faits — `_wutime`, `_ultoa`, `_aligned_malloc`/`_aligned_free`, `_endthreadex`, `_p___mb_cur_max`, `GetTickCount64`,
`SetSystemTime` (privilégié). Le gros de l'axe OS wide-char mesuré est **franchi** ; le reste est du mop-up borné + 3 aborts
sound documentés. Principe sacré tenu partout (jamais un faux ; abort bruyant sur le non-modélisable).

### 🎯 2ᵉ VRAI BINAIRE TIERS end-to-end (2026-08-15) — ninja 1.13.2 (build tool C++), sortie identique Wine

Après le mop-up OS, re-test d'un vrai binaire au-delà de jsoncpp : **ninja** (le build system, C++ dense — `std::string`/
`std::map`/`std::vector`/iostream/exceptions/dispatch de sous-outils), lifté avec les 3 runtimes (libstdc++/libgcc/
libwinpthread). **`ninja --version` → `1.13.2`** et **`ninja -t list` → les 19 lignes de sous-outils**, **identiques bit-à-bit
à Wine** (modulo la normalisation CRLF du harnais — Wine sort `\r\n`, ARET `\n`, comme toutes les fixtures). ninja exerce
**bien plus** du runtime C++ que le throw unique de jsoncpp : conteneurs, formatage de chaînes, iostream, parsing d'arguments,
table de sous-commandes. **Baseline Wine** : les DLL runtime doivent être **à côté** de l'exe (sinon rc=53 DLL-not-found) —
comme ARET les lifte via `--with-dll`. ⇒ Deux vrais binaires tiers C++ tournent maintenant end-to-end via le dispatcher EH +
runtime C++ GNU lifté + surface OS HLE : **jsoncpp** (throw/catch bout-en-bout) et **ninja** (CLI C++ réaliste). Reste hors
happy-path (ninja *build* réel) : IOCP/named-pipes/VEH — axe OS restant, mesuré, non bloquant pour `--version`/`-t`.
