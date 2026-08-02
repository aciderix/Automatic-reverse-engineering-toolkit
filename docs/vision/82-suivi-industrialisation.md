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
| Signatures / stubs | win32metadata / entêtes mingw + clang | script | 🔜 pas fait |
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
2. **Signatures / stubs** (couche 2) : générer les squelettes de shims + le marshalling A/W depuis les entêtes
   mingw (clang AST) ou win32metadata. *(Multiplicateur ; pas commencé.)*
3. ✅ **FAIT — Corps Wine forme MOYENNE** (`GetFamilyCodePage`, bit-identique Wine). Prolonger sur des corps plus gros
   (ex. une fonction `shlwapi`/`msvcrt` avec algorithme) pour éprouver la montée en taille.
4. **Corps Wine — forme LOURDE** : compiler une DLL Wine entière + **porter une fois le plancher `ntdll`/win32u**
   (~131 syscalls, doc 70 §5.0) → couverture massive. *(Milestone.)*

---

## 3. Invariants à ne jamais casser (rappel)

- **Autonome au runtime** : Wine/metadata servent à *fabriquer*, jamais à *exécuter*. Le binaire est ELF/WASM natif.
- **Sound** (§0 du 70) : tout extrait/porté est **vérifié contre l'oracle** (winediff / Windows réel) ; juste, ou
  **arrêt bruyant**. Automatiser retire l'écriture, **pas la preuve**.
- **Portes** : hash transpile inchangé (additif), stdcall_audit PASS, winediff vert — à chaque cran.
- **Licence** : Wine LGPL/GPL — obligations de distribution (décision produit, pas blocage technique).
