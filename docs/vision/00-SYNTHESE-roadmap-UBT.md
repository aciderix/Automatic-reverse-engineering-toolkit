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
| **M1** | **Stubs CRT POSIX** : `printf`/`puts`/`exit` interceptés, recompile Linux | console « hello world » à 1 import | aret-existant + couche C |
| M2 | Modèle d'imports + table de redirection génériques | console n'utilisant que la libc (file I/O, `malloc`, `str*`) | idem |
| M3 | Shims Win32 → POSIX (`kernel32`/`msvcrt` de base) | petit `.exe` Windows console → ELF Linux | s'inspirer de Wine |
| M4 | Memory layout mapper + relocations | binaires supposant une base fixe / données globales | `object` |
| M5 | Fallback émulateur (Unicorn) sur adresse inconnue | code partiellement obfusqué / sauts dynamiques | `unicorn-engine` |
| M6 | Cible WebAssembly/WASI (au lieu d'un OS natif) | « cible universelle » de la note 3 | `wasmtime` |
| M7 | GUI / graphisme (X11, puis DXVK) | applications fenêtrées, puis jeux | Wine/DXVK |

On ne s'engage pas sur M3+ tant que M1–M2 ne tournent pas proprement. L'intérêt :
à chaque palier on a un **artefact démontrable** et un test de non-régression.

---

## 4. M1 en détail — la première marche (« commence par… » des notes)

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
  lieu de les réécrire.
- ❓ **À décider avec l'utilisateur avant de coder** : démarre-t-on M1
  maintenant ? Quelle première cible de paire OS (Windows→Linux comme la note,
  ou Linux→Linux pour valider la mécanique sans même toucher à Win32) ?

> Ce fichier est le point d'entrée « mémoire » du projet UBT. Les trois notes
> d'origine sont conservées à côté, non éditées, comme cap de référence.
