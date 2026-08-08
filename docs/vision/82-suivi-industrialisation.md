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
   registre en interne (`RtlOpenCurrentUser`/`RtlQueryRegistryValues`, choisi par la mesure) vendoré + adaptateur esp +
   fixture PE = premier bout-en-bout non-chaîne. Puis d'autres fichiers, puis DLL.

**Invariants** : registre/état vide ⇒ prouver en round-trip ; jamais une valeur système devinée ; hash inchangé (additif) ;
`@N` Nt\* déjà dans `stdcall_pops` (audit) ; chaque tranche = fixture winediff + entrée 71 + maj ici.

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
