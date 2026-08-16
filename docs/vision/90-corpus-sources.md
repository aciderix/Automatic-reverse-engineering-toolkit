# 90 — Sources de binaires PE32 x86 (référence corpus / mesure)

> **But** : réservoir de **liens** vers des binaires PE32 x86 **FOSS** pour bâtir un
> **corpus de mesure** (Levier 0 — `wallsweep.sh` sur un grand corpus classe les murs
> par #binaires bloqués, doc 70 §5.0). On ne code jamais un fix à l'intuition ; on
> priorise par la donnée. Ce doc = **les liens**, distillés d'un catalogue fourni par
> l'utilisateur (2026-08-08). Le reste (scripts de pipeline auto-générés) est écarté :
> quand on en aura besoin, on écrira nos propres fetchers, minces et idempotents.
>
> ⚠️ **Réseau** : le proxy bloque `github.com` (doc 70 §7). Joignables vérifiés :
> `ftp.gnu.org`, `nasm.us`, `curl.se`, `zlib.net`, `sourceware.org`, `sqlite.org`,
> `pypi.org`, `archive.org`. **SourceForge/repo.msys2.org/cygwin.com = à tester** avant
> de s'appuyer dessus. Chaque source ci-dessous est un **candidat** : vérifier
> l'atteignabilité au moment de bâtir le corpus.

## Sources « OR » (vérité terrain pour ARET)

Ce qui vaut le plus pour **valider le lifting** (pas juste avoir du volume) :

- **Fedora MinGW** — `https://src.fedoraproject.org/projects/mingw-*` (RPM via `dnf download --arch i686 mingw32-*` + `rpm2cpio`). ⭐ **Les mêmes libs C compilées en PE32 ET en ELF** ⇒ paire ELF↔PE = oracle de lifting direct. GPL/LGPL/BSD/MIT, redistribuable.
- **ReactOS** — `https://sourceforge.net/projects/reactos/files/` (ISO → `7z x`, filtrer `system32/`). Source C complet + PE32 natif ⇒ vérification instruction-par-instruction. GPLv2+/LGPL.
- **Wine / winetest** — `https://dl.winehq.org/` (ou `make winetest` depuis les sources). Tests API Win32 exhaustifs (edge cases). LGPLv2.1+. *(On l'a déjà : `bench/.cache/winetest.exe`.)*

## Quick-wins FOSS (petit corpus, compilateurs variés)

| Source | URL | Ce que ça apporte | Licence |
|--------|-----|-------------------|---------|
| UnxUtils | `https://sourceforge.net/projects/unxutils/` | 119 EXE MSVC6/MinGW-GCC2.95 (~1999), CLI statiques — **starter idéal** | GPL/BSD/PD |
| GnuWin32 | `https://sourceforge.net/projects/gnuwin32/` | ~450 coreutils MinGW GCC 2.x–3.x (prendre les `.zip`, pas l'Inno Setup) | GPL/LGPL/BSD |
| ezwinports | `https://sourceforge.net/projects/ezwinports/` | ~350 MinGW-w64 GCC 6–14 (DWARF2), moderne/actif | GPL/LGPL/MIT |
| Open Watcom V2 | `https://github.com/open-watcom/open-watcom-v2` (releases `.tar.xz`) | 137 binaires **Watcom** (`__watcall` registres) = compilo non-GCC/MSVC | Sybase OWL (OSI) |
| TinyCC | `https://download.savannah.gnu.org/releases/tinycc/` | codegen **minimaliste** non-GCC/MSVC ; **génère** aussi des milliers de PE synthétiques | LGPL 2.1 |
| MSYS2 mingw32 | `https://repo.msys2.org/mingw/mingw32/` | ~9000 PE GCC 4.9–15 (DWARF/SJLJ), `.pkg.tar.zst` | FOSS mixte |
| MSYS2 clang32 | `https://repo.msys2.org/mingw/clang32/` | ~7500 PE **Clang/LLVM** 14–19 (codegen complémentaire) | Apache/LLVM |
| Cygwin 32-bit | `https://cygwin.com/pub/cygwin-archive/` | 10k+ POSIX→PE (fork/signal/socket), `setup.ini` + mirror | GPL/LGPL/BSD |
| WinLibs | `https://winlibs.com/` | toolchains GCC 10–16 **UCRT** moderne | GPL/permissif |
| niXman MinGW-Builds | (GitHub releases) | GCC 4.8–16, **SJLJ vs DWARF** sur mêmes versions | GPL/permissif |
| LLVM-MinGW | (GitHub releases, mstorsjo) | LLVM 10–23 pur (sans GCC) | Apache/LLVM |

## Applis GUI/CLI shippées (drivers réels, diversité fonctionnelle)

Liens directs de binaires 32-bit (⚠️ `github.com` bloqué par le proxy — prévoir un miroir/archive.org) :

- **7-Zip** `github.com/ip7z/7zip/releases/download/26.02/7z2602-extra.7z` (MSVC, CLI+GUI+DLL)
- **curl** `curl.se/windows/dl-8.21.0_6/curl-8.21.0_6-win32-mingw.zip` (MinGW ; **curl.se joignable**)
- **wget** `eternallybored.org/misc/wget/1.21.4/32/wget.exe`
- **Vim/gVim** `github.com/vim/vim-win32-installer/releases/.../gvim_*_x86.zip` (MSVC, GUI)
- **PuTTY** `the.earth.li/~sgtatham/putty/latest/w32/putty.zip` (MSVC, GUI+CLI) *(déjà un driver connu)*
- **SQLite** `sqlite.org/2026/sqlite-dll-win-x86-*.zip` (**sqlite.org joignable** ; on l'a déjà)
- **Notepad++** `github.com/notepad-plus-plus/.../npp.*.portable.zip` (MSVC, GUI, Scintilla)
- **VLC** `get.videolan.org/vlc/3.0.21/win32/vlc-3.0.21-win32.zip` (MSVC, ~500 DLL)
- **ImageMagick** `github.com/ImageMagick/ImageMagick/releases/...-x86.7z` (MSVC, CLI+20 DLL)
- **DB Browser SQLite** / **Audacity** / **GIMP** / **0 A.D.** / **SuperTux** — GUI Qt/wx/jeux (voir catalogue).

## Meta (packagers, gros volume — pour plus tard)

`Scoop` (JSON manifests), `Chocolatey` (OData `.nupkg`=zip), `Winget` (YAML), `PortableApps` (SFX). Gros volume mais bcp d'installers/.NET à filtrer — **pas prioritaire** tant qu'un petit corpus varié suffit à faire parler `wallsweep`.

## À écarter (mesuré)

- **Digital Mars / Borland** : freeware **non-redistribuable** (OK en test local, pas dans un corpus distribué).
- **FossHub** : Cloudflare + TOS anti-scraping.
- **Malware corpora / abandonware** : hors principe.
- Tout ce qui est **packé** (UPX/ASPack, entropie > 7.2) ou **.NET/MSIL** (`COM_DESCRIPTOR`≠0) ou **PE32+/x64** : hors périmètre lifter i386 actuel ⇒ à filtrer (`file`/`pefile`).

## Comment ça sert la mesure (rappel doctrine)

Corpus varié → `bash bench/wallsweep.sh <dir>` agrège `--mode walls` → **liste finie des murs classés par #binaires bloqués** ⇒ on priorise les fixes par la donnée (jamais à l'intuition). Diversité **compilateur × époque × modèle d'EH** (GCC-DWARF/SJLJ, MSVC-SEH, Clang, Watcom) = ce qui fait ressortir les vraies classes de bugs (comme le `ret` d'épilogue tabulé trouvé sur WinMerge/MSVC). **Un fetcher par source, checksums, idempotent** — on l'écrira quand on lancera la phase corpus.

---

## Résultat du 1er sweep (2026-08-08) — la donnée a parlé

**Corpus** : 1313 binaires PE32 x86 réels (MSYS2 mingw32 = GCC/Clang, + UnxUtils = MSVC6),
construit par `bench/wallcorpus_fetch.sh`. **1240 analysés** (`--mode walls`, 8-way) —
**140 (11 %) totalement propres**, 1100 avec des murs.

**Instructions non-liftées = BRUIT (comme attendu, §5.0)** : la tête (`int`/`ud2`/`in`/`insb`/
`outsb`/`popad`/`daa`/`arpl`/`bound`/`les`/`int3`…) = **data-décodée-en-code** + I/O port privilégié
⇒ **abort correct**, pas des cibles. *(Seuls `cvtdq2pd` (38 bin) et `pinsrd` (64 bin) = vraies
lacunes SSE éventuelles, à vérifier au cas par cas.)*

**Imports manquants = LE SIGNAL, et il est SANS APPEL : le RUNTIME C++ GNU domine.** Comptage
direct sur le corpus : **473/1266 (37 %) importent `libstdc++-6.dll`**, **598/1266 (47 %)
importent `libgcc`**. Top des imports manquants par #binaires (démanglé) :

| bins | symbole |
|-----:|---------|
| 336 | `operator new(unsigned int)` (`_Znwj`) |
| 281 | `_Unwind_Resume` (dépile EH GCC) |
| 246 | `std::__throw_length_error` · `operator delete(void*,uint)` |
| 226 | `std::__throw_logic_error` |
| 185 | `operator delete(void*)` |
| 179 | `std::__cxx11::basic_string::_M_create` (et toute la famille `basic_string`) |
| 177 | `std::__ostream_insert` (iostream) |
| 168 | `__cxa_begin_catch` (ABI EH Itanium) |
| 148 | `std::ios_base::Init` · `std::locale` |
| 131 | `__cxa_throw` |
| 126 | `std::_Rb_tree_decrement` (map/set) |
| 101 | `__divdi3`/`__udivdi3` (libgcc 64-bit) |
|  88 | `__dynamic_cast` |

⇒ **Verdict Levier 0** : pour le vrai logiciel FOSS compilé, **la lacune n°1 (≈37–47 % des binaires)
est le RUNTIME C++ (libstdc++-6.dll + libgcc_s)** — `operator new`/`delete`, `std::string`, iostream,
`std::map`/`_Rb_tree`, `std::locale`, EH C++ (`__cxa_*`/`_Unwind_Resume`/`__throw_*`), `dynamic_cast`.
ARET couvre le CRT **C** (msvcrt) ; il ne couvre **pas** le runtime **C++**.

**Réponse stratégique (doctrine)** : **Levier 1** — **lifter `libstdc++-6.dll` + `libgcc_s_dw2-1.dll`**
(vraies DLL à vrai code, comme `zlib1.dll` aujourd'hui). Un seul investissement fournit `operator new`,
`std::string`, iostream, conteneurs, EH — tout — depuis du **code lifté prouvé**, sans écrire 40 shims.
C'est **le** multiplicateur. **Convergence** : le driver WinMerge (MSVC/MFC) butait déjà sur le **runtime
C++** (CString, EH) — GNU **et** MSVC pointent la même frontière : **le runtime C++ est LE mur du vrai
logiciel**. *(Nuance corpus : MSYS2 biaise vers GNU ; un corpus MSVC montrerait msvcp/MFC — même thème.)*

## Suite — la mesure passe à l'ACTE (2026-08-08) : libgcc liftée

La donnée a désigné le runtime C++ GNU comme mur n°1. **Premier pas exécuté** (doctrine §0, « la sélection mesurée de
la cible EST le travail ») : **test pré-lift §0** sur les deux DLL (présentes sur l'hôte mingw) — `libgcc_s_dw2-1.dll`
= **0 thunk / 0 forwarder**, imports **KERNEL32+msvcrt seuls** ⇒ **liftable autonome** ; `libstdc++-6.dll` = 0/0,
importe **libgcc+KERNEL32+msvcrt** ⇒ liftable **par-dessus libgcc**. **libgcc LIFTÉE ✅** : ses helpers arithmétiques
64 bits (`__divdi3`/`__moddi3`/`__udivdi3`/`__umoddi3`/`__muldi3`/shifts — mesurés bloquants sur ~101 binaires)
**bit-identiques Wine** (`winecorpus/lift_libgcc`). **Reste** : `libstdc++-6.dll` par-dessus (le multiplicateur), puis
**re-mesurer** ce corpus (le levier change après chaque vague). Détail : doc 71 (2026-08-08) + doc 82.

## 2e sweep (2026-08-08) — corpus rafraîchi 1256 binaires : le verdict TIENT

Re-mesure sur **1256 PE32 analysés** (1266 collectés, 10 DLL lourdes échouent l'analyse : libpython3.13,
perl532, libtriton, cobc, flex — hors périmètre) — **140 propres / 1116 avec murs**. **Stable vs le 1er sweep**
(mêmes têtes, comptes un peu plus hauts) ⇒ la donnée est **reproductible** : le **runtime C++ GNU reste le mur n°1**.

Top imports manquants (démanglé) : `operator new` (**345**), **`_Unwind_Resume` (286)**, `std::__throw_length_error`
(254), `operator delete(void*,uint)` (251), `std::__throw_logic_error` (234), `basic_string::_M_create` (188),
`std::__ostream_insert` (183), `operator new[]`/`__cxa_begin_catch` (172), `std::locale`/`ios_base::Init` (~150),
`__cxa_throw` (137), `_Rb_tree_decrement` (133), **`__divdi3` (110)**. Instructions = bruit confirmé (data-en-code +
I/O privilégié → abort correct ; seules `pinsrd` 64 / `cvtdq2pd` 40 / `psllq` 38 = lacunes SSE éventuelles).

**⇒ La mesure VALIDE l'incrément du jour** : `_Unwind_Resume` (286 bin) et `__divdi3` (110 bin) sont des symboles
**libgcc** — le lift `libgcc_s_dw2-1.dll` (2026-08-08, bit-identique Wine) frappe pile dans le top mesuré. Le reste de
la tête (`operator new/delete`, `std::string`, iostream, `_Rb_tree`, `std::locale`, `__cxa_*`) vit dans
**`libstdc++-6.dll`** ⇒ le multiplicateur suivant, lifté PAR-DESSUS libgcc.

## Mesure de PORTÉE de la brique EH/unwind GNU (2026-08-08) — scan d'imports, 1313 binaires

Question : la brique EH/unwind (le mur unique du runtime C++ GNU, cf. doc 71/82) toucherait **combien** de binaires ?
Mesure directe (scan `objdump -p` des imports de chaque binaire du corpus, `scratchpad/ehscan/flags.txt`), **pas** une
extrapolation. Flags par binaire = [importe libstdc++][importe libgcc][EH throw/catch][iostream].

| catégorie | bins | % |
|---|---:|---:|
| total | 1313 | 100 |
| pur C (aucun runtime C++) | 660 | 50,3 |
| **couvert** : libgcc arith-only (`__divdi3`…) | 146 | 11,1 |
| **couvert** : C++ happy-path (string/conteneurs, étape 1) | 45 | 3,4 |
| EH C++ throw/catch (`_Unwind_`/`__cxa_throw`) → unwind complet | 401 | 30,5 |
| iostream (frame-reg au static-init, cf. étape 2) | 361 | 27,5 |
| **⟹ a besoin de la brique EH/unwind (union)** | **463** | **35,3** |

**Le « 37-47 % » était une vraie mesure de « importe le runtime C++ » (38,4 % libstdc++, 48,1 % libgcc) ; la brique EH,
mesurée finement, en touche 35,3 %** (on retranche les 191 déjà couverts : arith-only + happy-path). Deux couches :
**401** (throw/catch réel = unwind complet) + **62** de plus (iostream sans throw = au moins le static-init).
**Caveats** : nécessaire mais pas forcément suffisant par binaire (borne haute sur cette dimension) ; corpus MSYS2 =
GNU-biaisé (un corpus MSVC montrerait l'EH MSVC, **déjà fait**). ⇒ **priorité confirmée par la donnée : on engage la brique.**

## Échantillon « non-throw C++ » (2026-08-08) — l'effet des 2 fixes loader, isolé de l'EH

Question utilisateur : sur les binaires C++ **sans** throw/catch, combien tournent **déjà** grâce aux 2 fixes loader du jour
(pseudo-reloc + ctors de DLL), séparément de l'EH ? Mesure sur le corpus (qui **contient** la libstdc++/libgcc MSYS2 exacte).

**Composition (179 exes C++ importent libstdc++)** : **102 utilisent throw/catch** (attendent l'EH), **77 non**. Mais des 77
non-throw, **quasi tous** tirent 1+ DLL tierce **lourde** (`libLLVM-21` ×21, `libclang-cpp` ×14, `libxapian` ×13, Qt, `libraw`,
tesseract…) : 42 en tirent 1, 25 en tirent 2, etc. **Seuls ~2 sont autonomes** (libstdc++/libgcc + DLL système). ⇒ ce corpus
est **du gros logiciel C++**, pas des CLI autonomes.

**Runs des 2 autonomes** (`mlir-tblgen`, `qtcreator_ctrlc_stub`, `--with-dll` libstdc++/libgcc du corpus) : **les deux
passent l'init du runtime C++** (imports libstdc++ résolus — mlir-tblgen : 78 symboles, **0 skew** de version — ctors
exécutés, `main` atteint) **puis abortent PLUS LOIN, dans LEUR PROPRE code**, sur un **appel indirect vers une fonction non
récupérée** (`0x6fddf0` / `0x42ddf0`) — un **trou de récupération** (cible de vtable/pointeur ratée), **orthogonal** aux fixes
loader **et** à l'EH. Wine ne donne pas de baseline `--version` (outils lourds à invocation spécifique) ⇒ pas de « works »
bit-comparable.

**Verdict honnête.** Un compte « N tournent bout-en-bout » **n'est pas extractible** de ce corpus : les 77 non-throw sont
bloqués par la **largeur de DLL tierces** (+ des trous de récupération d'appels indirects), **PAS par l'EH**. Les 2 fixes
loader sont une **suppression de mur GÉNÉRALE et PROUVÉE** — bout-en-bout bit-identique Wine sur la fixture `lift_libstdcxx`
(iostream), **et** vérifiés sur 2 vrais binaires du corpus (l'init runtime C++ passe, l'abort est ailleurs). ⇒ **EH et fixes
loader adressent des populations DISJOINTES** : l'EH débloque les 102 throw-users ; les 77 non-throw attendent surtout
**plus de DLL liftées** (axe Levier 1 distinct) + de la récupération d'appels indirects. La preuve la plus propre des fixes
reste la **fixture** (iostream), le corpus étant trop lourd pour un compte end-to-end net.

## 3e sweep (2026-08-15) — post-runtime-C++/OS : le verdict TIENT, et le mur #1 est désormais LIFT-COVERED

Re-mesure `wallsweep.sh` (désormais **parallélisé 4-way**, ~20 min au lieu de ~2 h) sur les mêmes **1313 PE32** après
les gains de la session (3 fixes de lift-correctness du runtime C++ + axe OS wide-char couvert + jsoncpp/ninja
end-to-end). **1276 analysés** (37 gros blobs LLVM/Qt/z3 dépassent le plafond mémoire/timeout ⇒ échec propre, comme
prévu) — **119 propres / 1157 avec murs**.

**Instructions non-liftées = BRUIT confirmé** (3e fois) : `int`/`ud2`/`in`/`int3`/`insb`/`outsd`/`popad`/`daa`/`outsb`/
`pushad`/`arpl`/`insd`/`hlt`/`das`/`bound`/`les`/`aaa`/`sti` + même `push`/`pop`/`mov`/`jmp`/`shl`/`div` à bas compte
dispersés = **data-décodée-en-code** + I/O port privilégié → abort correct. **SEULE lacune plausiblement RÉELLE** : le
**trio SSE `psllq` (45 bins/6369 sites), `pinsrd` (69/4346), `cvtdq2pd` (48/4610)** — **non modélisés** dans `lift.rs`
(vérifié : 0 match, alors que ~41 autres ops packed le sont) ⇒ vraies cibles de vectorisation, liftables+vérifiables vs
Unicorn (cpudiff), touchant ~45-69 binaires. À confirmer au cas par cas (exécutable vs data) avant d'implémenter.

**Imports manquants = LE SIGNAL, et il est IDENTIQUE aux 2 sweeps précédents : le RUNTIME C++ GNU domine** — `operator
new`/`delete` (`Znwj` 365, `ZdlPvj` 275, `ZdlPv` 202, `ZdaPv` 188, `Znaj` 183), la famille `std::__throw_*`
(`__throw_length_error` 278, `__throw_logic_error` 251, `__throw_bad_array_new_length` 163, `__throw_bad_alloc` 148,
`__throw_bad_cast` 141, `__throw_out_of_range_fmt` 123, `__throw_bad_function_call` 104), `std::string`
(`_M_create` 197, `_M_replace` 167, `_M_assign` 162, `_M_append` 113, `_M_dispose` 105), iostream (`__ostream_insert`
188, `ios_base::Init` 158, `basic_ios::init` 157, `ostream::put`/`flush`/`operator<<`), `std::locale` (186/159),
`ctype::_M_widen_init` 132, `_Rb_tree_*` (144/143/132), `cxa_guard_acquire/release` (137), `divdi3`/`udivdi3` (119/108).

**⇒ VERDICT (l'interprétation est le point) :** le raw-sweep **reconfirme** le runtime C++ GNU comme mur n°1 par largeur
(**reproductible sur 3 sweeps**) — MAIS ce mur est désormais **LIFT-COVERED** : la session a prouvé **2 vrais binaires
tiers end-to-end** (jsoncpp throw/catch, ninja `-n`) en **liftant** libstdc++/libgcc/libwinpthread à côté (`--with-dll`,
Levier 1), pas en écrivant 40 shims. Le raw-sweep ne **voit pas** le lift (il compte les imports statiques). Donc la tête
n'est **pas** « 40 shims à écrire » : c'est « le lift du runtime C++ est la réponse, et il marche ». **Le vrai prochain
mur mesuré est POST-LIFT** (ce qui bloque APRÈS avoir lifté le runtime = surface OS + lift-correctness C++ dense), qu'un
raw-sweep ne montre pas ⇒ il faut un sweep **`--with-dll`** ciblé (comme la mesure 3-apps de 2026-08-15) pour le
cartographier. Cibles data-désignées immédiates, indépendantes du lift : le **trio SSE** (général, vérifiable Unicorn).

## Mur POST-LIFT (2026-08-15) — une fois le runtime C++ lifté, la surface OS restante

Le raw-sweep compte les imports statiques ⇒ il ne voit pas le lift du runtime C++. Pour cartographier le **vrai mur
post-lift** sans re-lifter 24 Mo de libstdc++ ×505 (~1-2 h), on filtre le sweep raw : les symboles fournis par le runtime
lifté (`^Z`/`cxa_`/`Unwind`/`gxx_personality`/`pthread_`/`dynamic_cast`/`guard_`/libgcc-arith) sont marqués **lift-covered**
et retirés du classement (`WALLSWEEP_COVERED`, harnais). Retirer un symbole ne change pas le compte d'un autre ⇒ **exact**,
sur les **1279 binaires**. Résultat : **427/1279 (33 %) n'ont AUCUN import restant** une fois le runtime lifté (vs 119
propres en raw) — **c'est la valeur mesurée du Levier 1** (lifter libstdc++/libgcc/libwinpthread débloque l'import-wall de
33 % du corpus). Les **15236 symboles C++ filtrés couvrent 567 binaires**.

**MàJ 2026-08-16** : le **trio SSE** (`psllq`/`pinsrd`/`cvtdq2pd` + `pinsrw`) — seules lacunes de lift plausiblement
réelles de ces sweeps — est désormais **lifté et prouvé bit-identique Unicorn** (cpudiff), hash inchangé. La partie SSE de
l'axe lifter est close ; reste x87 `fldenv`/`fnstenv` (incrément séparé). Détail : doc 71 (2026-08-16) + doc 82.

**Mur POST-LIFT restant (par #binaires) — 3 familles nettes :**
1. **Winsock2 / sockets BSD (`ws2_32.dll`) = LA surface OS dominante** : `WSAGetLastError` 67, `closesocket` 66,
   `WSAStartup` 62, `setsockopt` 59, `ioctlsocket` 58, `connect` 55, `htons` 55, `recv` 54, `socket` 54, `htonl` 53,
   `send` 53, `freeaddrinfo` 51, `getaddrinfo` 50, `select` 46, `ntohs` 44, `bind` 40… (~16 fns, 40-67 bins chacune).
   **Famille HLE-OS classique** (comme fichier/registre/wide-char déjà faites) : mappable sur les **sockets POSIX de
   l'hôte** (socket/connect/bind/send/recv/select = vrais syscalls), vérifiable vs Wine (fixture client/serveur
   localhost). **C'est le prochain axe OS que la donnée désigne.**
2. **GLib/GTK (`g_*`) + gettext (`libintl_*`) = largeur DLL tierces** : `libintl_gettext` 78, `g_free` 73,
   `libintl_bindtextdomain` 70, `g_log` 66, `libintl_textdomain` 63, `g_object_unref` 62, `g_strdup` 60, `g_malloc` 51,
   `g_type_*`/`g_hash_table_*`/`g_list_*`, `libintl_setlocale` 47… = **glib-2.0-0.dll + libintl-8.dll** (DLL tierces).
   **Axe Levier 1 distinct** (lifter la DLL) — plus gros, séparé de la surface OS. `dllrunscript` 76 = à identifier
   (probablement une DLL vendeur partagée du corpus MSYS2, pas une fonction OS générale).
3. **Reliquats CRT msvcrt (petits, généraux)** : `_fpreset` 58, `_fpecode` 44, `__pxcptinfoptrs` 44, `ctime` 48 —
   environnement FP + divers, shims minces.

**⇒ Verdict post-lift** : le mur n°1 réel après le runtime C++ est le **réseau (Winsock)** — famille OS bornée (~16 fns),
générale, sound, POSIX-backée, Wine-vérifiable. Puis la **largeur DLL tierces** (GLib/gettext, Levier 1). Puis le mop-up
CRT (`_fpreset`/`_fpecode`/`ctime`). Le raw-sweep ne pouvait pas montrer ça (tête noyée sous le runtime C++ lift-covered).

## Re-mesure POST-WINSOCK (2026-08-15) — le mur bascule sur les DLL tierces GLib/gettext

Après Winsock (inc 1+2) + mop-up CRT, re-mesure **exacte et instantanée** : on ré-agrège les walls persistés en ajoutant
au filtre `WALLSWEEP_COVERED` les symboles nouvellement implémentés cette session (Winsock + `ctime`/`_fpreset`/
`__fpecode`/`__pxcptinfoptrs`). Retirer un symbole ne change pas le compte d'un autre ⇒ classement restant exact. Résultat :
**476/1279 (37 %) import-clean** une fois runtime C++ + Winsock + CRT couverts (vs 427 pré-Winsock ⇒ **+49 binaires**
débloqués par la session ; Winsock touchait 93 binaires, les reliquats CRT 127). **Le mur restant est TOTALEMENT dominé par
GLib/gettext** : `libintl_*` (gettext i18n : `gettext` 78, `bindtextdomain` 70, `textdomain` 63, `setlocale` 47,
`dgettext` 44…) et `g_*` (GLib/GObject : `g_free` 73, `g_log` 66, `g_object_unref` 62, `g_strdup` 60, `g_malloc` 51,
`g_type_*`, `g_hash_table_*`, `g_list_*`, `g_signal_connect_data`, `g_string_*`, `g_once_init_*`…). **Stragglers** : `zlib`
(`inflate`/`inflateEnd` 38 — **déjà liftable**, cf. lift_zlib) ; 2 symboles **libgcc** que le filtre a ratés
(`__emutls_get_address`/`__udivmoddi4` 38 — lift-covered en réalité) ; `wcstombs` 36 (vrai petit gap CRT wide→mb) ;
`dllrunscript` 76 / `printf__` 38 (à identifier). **⇒ Verdict data-driven : le prochain axe est SANS APPEL GLib/gettext**
(`libglib-2.0-0.dll` + `libintl-8.dll`) = **Levier 1** (lifter la DLL, comme libstdc++/zlib) ou shims — chantier séparé,
plus gros. La boucle « mesurer après chaque vague » (§2) a tranché.
