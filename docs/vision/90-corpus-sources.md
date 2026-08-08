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
