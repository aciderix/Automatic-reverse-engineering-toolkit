# 83 — Propositions externes (Gemini) : analyse critique & triage

> **Rôle** : conserver et **instruire** un document de stratégie fourni par l'utilisateur
> (Gemini, 2026-08-17, `uploads/8bd6a361-propositions_gemini.txt`), sans rien implémenter
> à chaud. Même discipline que le **81 §1** (qui a fait ça pour un doc ChatGPT/Gemini
> antérieur) : on garde ce qui est utile, on **corrige ce qui est déjà fait ou mal
> priorisé**, on **confronte chaque idée à la MESURE** (doc 90) et au **principe sacré
> §0** (doc 70). L'utilisateur lui-même juge ces propositions « pas sûres d'être
> pertinentes dans l'immédiat » — cette analyse dit **pourquoi**, et **quand** rouvrir
> chaque item.
>
> **Le filtre décisif (leçon récurrente, 81 §1.1)** : un conseiller externe, faute
> d'exécution, (a) **décrit comme "à faire" des choses déjà faites**, et (b) n'est **pas
> piloté par le mur mesuré**. *« Une difficulté nommée dans une roadmap n'est pas une
> difficulté mesurée »* (81, activation COM). On priorise par la donnée.

## 0. Le contexte que le document ignore : le mur mesuré AUJOURD'HUI

Re-mesure du 2026-08-17 (doc 90, corpus 789 PE32, post-2ᵉ-palier) : **la surface OS est
essentiellement close** (aucune API kernel/user/gdi/shell dans la tête du sweep post-lift)
et le **prochain mur mesuré = largeur de bibliothèques applicatives tierces C++** — **LLVM**
(40–76 bins), **mbedTLS/PSA** (57–58), **ITK** (48–50), + Qt/tesseract. C'est un axe
**Levier 1** (lifter ces DLL), déjà éprouvé (libstdc++/zlib/comctl32).

**Aucune** des propositions Gemini n'attaque **ce** mur. C'est le point central du triage
ci-dessous : le document est **bon**, souvent **aligné**, mais **orthogonal à l'immédiat**.

## 1. Triage synthétique

| # | Proposition | Verdict | Pourquoi |
|---|-------------|---------|----------|
| A1 | Wine-Slicer (extraction AST clang) + thunks dynamiques | **DÉJÀ EN COURS** | = 81 I2/I12/I13 : `gen_win32_sigs.py --marshal` (thunks A/W, **refuse** l'unsound), `gen_mlang_cp.py` (spike Wine-source PROUVÉ), générateur `@N` (6487). Attaque l'OS (≈clos), pas le mur mesuré. |
| A2 | LLVM IR first-class + LTO cross-boundary | **DIFFÉRÉ (perf)** | Backend LLVM **existe déjà** (`src/emit/llvm.rs`). LTO pour inliner les shims = **perf**, pas couverture. Le défaut C est **prouvé** (hash `19acad982194bf07`) ; le non-déterminisme C a déjà été corrigé (81 I9). À rouvrir quand la perf devient le mur. |
| A3 | WASM EH natif, Wasm threads/weak-memory, SIMDe | **DIFFÉRÉ (cible WASM/ARM)** | Territoire doc 80 §3/§4.7. Correct et pertinent **pour la cible WASM/ARM**, qui n'est pas le front actuel (x86→ELF, couverture). Le point weak-memory (atomiques LLVM + élision des barrières sur accès locaux) est juste et rejoint l'évolution fibers→threads. |
| A4a | Fallback JIT « sound » (Unicorn tiered-execution) | **⚠️ CHARGÉ DOCTRINALEMENT** | Idée réelle : interpréter un bloc récalcitrant, puis rendre au natif. MAIS embarque **de l'émulation au runtime** — contredit l'étoile *« natif, sans émulation, universel »* (70 §0). Transforme le `abort` §0 en `emulate`. À **débattre consciemment** (change l'identité du produit), pas à glisser. |
| A4b | Rétro-cible 16-bit→64-bit (software TLB selector:offset) | **DIFFÉRÉ (niche)** | Doc 80 §1.6 pour le 16-bit. Niche DOS/Win3.x, hors mur mesuré. |
| **2.1** | **Fuzzing différentiel (AFL++/libFuzzer, ARET vs Wine)** | **✅ RETENIR (le meilleur)** | Extension **naturelle** des oracles existants (cpudiff/funcdiff/winediff, aujourd'hui à vecteurs fixes) vers du **coverage-guided** qui chasse les faux-silencieux automatiquement = cœur du §0. Vraie brique d'industrialisation (candidat 81). |
| 2.2 | Time-Travel Debug (ring buffer + fork/COW + replay inverse) | **DIFFÉRÉ (marginal)** | Le **traceur I1** (`ARET_TRACE`) existe, et *« la dérive esp est STATIQUE dans le C généré »* (81 I1) a rendu l'essentiel du diagnostic **statique**. TTD = valeur ajoutée faible sur l'existant. |
| 2.3 | Exécution symbolique (Z3/angr) pour points-to / appels indirects | **✅ PROMETTEUR (aligné §0.4)** | Z3 déjà utilisé (SMT 11/11) + récup de tables de saut bornée. **Prouver** une cible d'appel indirect (au lieu de deviner) = exactement §0.4, et attaque le **gap récup d'appels indirects** noté doc 90. angr est lourd ; à cadrer. Medium. |
| 3.1 | APR (auto_fixer.sh : winediff→GDB→prompt API→boucle) | **⚠️ PRUDENCE** | Auto-réparation en boucle fermée. Danger §0 : une correction IA peut introduire un **faux-silencieux** (cf. mon miscompile de session, attrapé seulement au run end-to-end). Acceptable **seulement** si **chaque** correction passe TOUTES les portes avant commit. Mécaniquement OK, correctness-critique dangereux. |
| 3.2 | Red-teaming (session isolée : « 3 façons de corrompre la mémoire ») | **✅ RETENIR (discipline)** | Bon marché, aligné, = la leçon de mon miscompile (un 2ᵉ regard). Recoupe le skill `code-review`. À adopter comme discipline avant tout code correctness-critique (ESP/SEH/lift). |
| 3.3 | Test-gen (l'IA écrit le GÉNÉRATEUR, tests de propriétés/invariants) | **DÉJÀ FAIT** | = le style maison : fixtures à **invariants** (« écrire X puis lire rend X »), cpudiff génératif, *« séparer le CONTRAT de la DONNÉE »* (EnumFontFamilies, `win32_wvolpath`). Valide la pratique. |
| 3.4 | RAG WineHQ/MSDN + triage IA sur `wallsweep` | **DIFFÉRÉ (commodité)** | La **vérité ABI** d'ARET = les import-libs mingw (`@N`) + sources Wine **vérifiées**, pas la prose MSDN. Le générateur `@N` fait déjà le mécanique. RAG = confort, faible priorité. |
| 3.5 | Constitution machine-readable (.claude.md : §0, hash, aret_unimpl) | **DÉJÀ FAIT** | = le **rituel de reprise** (`.claude/hooks/reprise.sh`), docs 70 §0/§2, ce système documentaire. En place. |
| P4 | Littérature (rev.ng, McSema, Alive2, x86-TSO/Rosetta2, Wasm EH) | **RÉFÉRENCE** | Contexte utile. rev.ng/McSema = pairs (x86→LLVM IR). **Alive2** (translation-validation SMT) pourrait compléter le travail Z3 **si** on passe LLVM-IR-first (A2). x86-TSO/Rosetta2 → futur ARM/weak-memory (A3). Pas des actions. |

## 2. Ce qui vaut vraiment la peine d'être retenu (aligné + non-fait + sound)

Par ordre de valeur, **si/quand** on ouvre un chantier « qualité/soundness » (indépendant
du mur de couverture actuel) :

1. **Fuzzing différentiel (2.1)** — coverage-guided, ARET vs Wine/Unicorn, minimisation
   auto à la moindre divergence de retour/mémoire. C'est la montée en puissance logique de
   nos oracles différentiels, et le meilleur filet anti-faux-silencieux au-delà des corpus
   fixes. **Candidat brique 81** quand la phase « durcissement » arrive.
2. **Résolution symbolique des appels indirects (2.3)** — prouver les cibles (§0.4) au lieu
   d'aborter, pour le gap **récup d'appels indirects** que la donnée a déjà pointé (doc 90,
   les 2 binaires C++ autonomes abortent là-dessus). Cadrer léger (Z3 sur un slice) avant
   d'invoquer angr.
3. **Red-teaming (3.2)** — discipline de revue gratuite avant tout code ESP/SEH/lift.
   À adopter tout de suite comme **habitude**, sans chantier.

## 3. Le point de friction à trancher un jour (pas à glisser)

**Fallback tiered-execution via Unicorn (A4a)** est la seule proposition qui **change
l'identité du produit**. Elle ferait « tourner » plus de binaires en **interprétant** les
morceaux durs — c'est-à-dire en **réintroduisant l'émulation** que toute l'architecture
ARET existe pour éviter (*« entièrement fonctionnel comme natif, sans émulation »*, 70 §0).
Deux lectures :
- **Contre** : viole l'étoile. Un binaire « qui tourne à moitié émulé » n'est pas le produit.
- **Pour** : borné et **sound** (Unicorn est déjà notre oracle ; l'état est synchronisé
  exactement), ce serait un *mode dégradé explicite* — « natif sauf ces N blocs, signalés ».

⇒ **Décision utilisateur**, consciente, le jour où la couverture native plafonne. À **ne pas**
implémenter par opportunisme. Consigné ici pour ne pas le re-débattre à froid.

## 4. Conclusion (honnête)

Le document Gemini est **de bonne qualité et largement aligné** sur la doctrine ARET
(soundness, oracles, mesure) — mais il reproduit exactement le biais du conseiller sans
exécution (81 §1) : il **redécrit des acquis** (backend LLVM, générateur de thunks A/W,
spike Wine-source, traceur, tests-générateurs, constitution) et n'est **pas piloté par le
mur mesuré**. L'instinct de l'utilisateur (« pas pertinent dans l'immédiat ») est **correct
et prouvé par la mesure** : le front actuel est le **lift des grosses libs applicatives
C++** (LLVM/mbedTLS/ITK, Levier 1), qu'aucune de ces propositions n'adresse.

**À rouvrir** : 2.1 (fuzzing) et 2.3 (symbolique indirects) quand on lance une phase
durcissement ; A2 (LTO) quand la perf devient le mur ; A3 (WASM EH/threads) quand la cible
WASM/ARM redevient prioritaire ; A4a (fallback émulé) **seulement** sur décision explicite.
**À adopter tout de suite** : 3.2 (red-teaming) comme discipline. Le reste est **déjà fait**
ou **différé par la donnée**.
