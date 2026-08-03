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
4. **Corps Wine — forme LOURDE** : compiler du `.c` Wine entier + **porter une fois le plancher `ntdll`/win32u**
   → couverture massive. *(Milestone.)* **🚧 OUVERTE ET MESURÉE (2026-08-02, `tools/gen_wine_heavy.py`)** : `rtlstr.c`
   de ntdll **compile INCHANGÉ** en objet i686 → **46 fonctions `Rtl*` réelles d'un coup**, sur un plancher **fini de 12
   primitives à porter** (conversions NLS `RtlMultiByteToUnicodeN`… + `RtlCompareUnicodeStrings`) — le reste du plancher
   (libc, heap) **existe déjà** dans le HLE. Shim de compat Wine (`tools/wine_heavy/` : `wine/debug.h`→no-op, `ddk/ntddk.h`
   vide, `ARRAY_SIZE`/limites 64-bit) **checké-in** (survit au conteneur éphémère — motivé par un wipe). Le coût est
   **par-fichier mesuré** (`wcstring.c` réclame 2 typedefs msvcrt de plus — l'outil le signale). **Reste** : câbler l'objet
   dans le build HLE, fournir/router les 12 primitives (sous-ensemble ASCII via `MultiByteToWideChar` existant, abort sound
   au-delà), prouver ≥1 fonction bit-identique Wine. C'est ce qui **casse le coût cas-par-cas** de la forme moyenne.

---

## 3. Invariants à ne jamais casser (rappel)

- **Autonome au runtime** : Wine/metadata servent à *fabriquer*, jamais à *exécuter*. Le binaire est ELF/WASM natif.
- **Sound** (§0 du 70) : tout extrait/porté est **vérifié contre l'oracle** (winediff / Windows réel) ; juste, ou
  **arrêt bruyant**. Automatiser retire l'écriture, **pas la preuve**.
- **Portes** : hash transpile inchangé (additif), stdcall_audit PASS, winediff vert — à chaque cran.
- **Licence** : Wine LGPL/GPL — obligations de distribution (décision produit, pas blocage technique).
