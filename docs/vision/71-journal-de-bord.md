# 71 — Journal de bord structuré (source d'infos précise, cherchable)

> **But.** Contenir le **détail technique complet** de ce qui a été fait, organisé
> pour la **recherche** (grep par tag de sous-système) plutôt que la lecture
> linéaire. Le [`70`](70-reference-etat-methode-reste.md) donne l'**état résumé** et
> pointe ici pour le détail. Le [`50`](50-plan-execution.md) est l'**archive
> chronologique** historique ; chaque fiche ci-dessous renvoie à ses sections.
>
> **Deux parties** : §2 **fiches par sous-système** (le savoir consolidé, cherchable
> par tag) ; §3 **journal chronologique** (entrées datées append-only, go-forward).

## 0. Comment utiliser ce document

- **Trouver une info** : `grep '\[TAG\]'` (liste §1) ou le nom d'une fonction/fichier.
  Les fiches §2 condensent le savoir + pointent vers le 50 (grep le **titre de
  section** cité, ex. `grep "callee-pop" docs/vision/50-plan-execution.md`).
- **Ajouter du travail (chaque agent, obligatoire)** : (1) écrire une **entrée
  chronologique** §3 (format ci-dessous) ; (2) si ça change un invariant/fait
  durable, **mettre à jour la fiche §2** concernée ; (3) résumer l'état dans le 70 ;
  (4) commit + push. **Ne jamais** réécrire l'historique d'une entrée passée.

**Format d'une entrée §3** :
```
### AAAA-MM-JJ — [TAG][TAG2] Titre court
- **Symptôme/cible** : …
- **Cause racine** : … (fichier:fonction)
- **Fix** : … (général, pas de rustine)
- **Vérifié** : fixtures/tests + régression (chiffres)
- **Reste** : … (le cas échéant)
- **Réf 50** : titre de section (si détail additionnel en archive)
```

## 1. Tags (sous-systèmes)

| Tag | Domaine | Fiche |
|-----|---------|-------|
| `[LIFT]` | sémantique par-instruction (drapeaux, entiers, SSE, chaînes, divers) | §2.1 |
| `[X87]` | pile FPU : passe de profondeur + filet runtime + libm | §2.2 |
| `[ABI]` | pile partagée, esp/ebp, appels, callee-pop, frame helpers | §2.3 |
| `[RECOV]` | récupération de fonctions, tables de saut/pointeurs | §2.4 |
| `[HLE-STDIO]` | FILE/stdio msvcrt | §2.5 |
| `[HLE-FILE]` | fichiers, stat, chemins, mmap, wide | §2.6 |
| `[HLE-WIN32]` | console/tls/locale/heap/module/process/find/version/COM | §2.7 |
| `[HLE-CRT]` | printf/scanf, strtoll, atexit, setjmp, temps | §2.7 |
| `[ORACLE]` | cpudiff/funcdiff/difftest/winediff/sweeps/SMT | §2.8 |
| `[RECOMPILE]` | backends C/LLVM/WASM, helpers, chunking | §2.9 |
| `[INFRA]` | conteneur/git, réseau, toolchain, corpus | §2.10 |
| `[DEMO]` | démonstrateurs (Lua/strings/sqlite/NASM/busybox/WASM/gauntlet) | §2.11 |
| `[64BIT]` `[THREAD]` `[GUI]` | chantiers ouverts (roadmap 70 §6) | §2.12 |

---

## 2. Fiches par sous-système (savoir consolidé)

### 2.1 `[LIFT]` — sémantique par-instruction (`src/ir/lift.rs`)
**Validé contre Unicorn** (`cpudiff`, milliers d'états). Bugs corrigés — tous
**généraux**, chacun invisible à difftest/Lua jusqu'à cpudiff :
- **Drapeaux width-aware** : SF/OF via `sign_bit(x,w)` ; CF/ZF sur opérandes **masqués
  à `w`** ; PF via `__ix_pf`, AF `(a^b^r)>>4&1` (pas après logique/shift/mul). Compte :
  retenue width-aware, ZF shift count=0, shl ZF non-masqué, inc/dec ZF+OF, rol/ror CF
  count=0, adc/sbb retenue d'entrée (`*_cin`), `sub_flags` cmp signée 64-bit.
- **Immédiat signé** : `cmp/add/sub r, imm` sign-étend l'immédiat à la largeur
  d'opérande → **masquer a ET b avant `Ult`/`Eq`/carry** (piège : opérande registre
  masqué cachait le bug → difftest vert mais transpile faux).
- **64-bit sur cible 32-bit** : retour `long long` en **paire edx:eax** (`Return`
  combine, `call` scinde edx D'ABORD) ; comptage de décalage **masqué** 5/6 bits.
- **div/idiv** : helpers `__ix_*div*/*mod*` **reproduisent le #DE** (div0 ET
  débordement de quotient, `INT_MIN/-1`) → crash exact, jamais troncature silencieuse.
  `SDiv`/`SMod` width-aware.
- **SSE** : scalaire + packed validés Unicorn (`__fp_*`/`__pi_*`/`__ps_*`). Bug `ss`
  (une op simple-précision préserve `dst[127:32]` → `write_fp` fusionne la lane basse,
  ne zéroïse pas `[63:32]`). SSE2-string : pcmpeqb/eqw/gtb, pmovmskb, pshuflw/hw.
- **Chaînes** : `movs/stos/lods/scas/cmps` non-rep (désambiguïsées de SSE par
  `is_string_instruction()`) + `rep movs/stos/scas` (`__rep_*`). DF=0 assumé ;
  `std`/DF arrière + `rep cmps/lods` = **abort sound**.
- **Divers** : `cpuid`/`xgetbv` (`__ix_cpuid` host réel, **AVX/SSE4.2 masqués** →
  chemins SSE2 liftables), `bt/bts/btr/btc` (reg + `[mem],imm` ; index registre en
  mémoire = exclu), `stmxcsr` (écrit défaut 0x1f80)/`ldmxcsr` (Nop), `xchg` high-byte.
- **DCE & pureté** (`src/opt/mod.rs`) : **appel indirect = impur**
  (`CallTarget::Indirect(_)=>true`) sinon supprimé si résultat mort. Helpers **purs**
  (`__ix_pf`/`__ix_cpuid`/`__fp_*`/`__x87_*`) supprimables SAUF fautants (`__ix_*div*`)
  ou écrivant mémoire (`__rep_*`/`aret_*`) — un `__ix_pf` mort bloquait la
  copy-prop → cassait la division magique.
- **Réf 50** : « Drapeaux de signe/débordement width-aware », « 64-bit sur cible
  32-bit », « div/idiv trap fidèle », « Corpus différentiel élargi », « Axe 1
  consolidé PF/AF + SIMD », « Différentiels div/idiv + flottant SSE ».

### 2.2 `[X87]` — pile FPU (`lift.rs`, `emit/mod.rs`, `ir/build.rs`)
Deux mécanismes complémentaires :
- **Passe de profondeur statique** (`x87_depth_pass`) : compte la pile inter-insn ; un
  `call` fp-returning compte `+1` (`compute_fp_returning`, point-fixe ; libm reconnue
  par nom/FLIRT). Ops : load/store f/fi 32/64/80, constantes, arith, `fxch/fabs/fchs/
  fsqrt/frndint`, `fcom/fucom/fcomi/ftst/fxam/fcmovcc/fprem`. Retour fp via
  `__aret_x87_ret`. Diagnostic : `ARET_X87_DEBUG=1`.
- **4 modes d'arrondi PROUVÉS** (`RoundMode`, `rounding_mode_active` →
  `rc_installed_by_store`) : interprète abstraitement les bits RC (10/11) du control
  word (`mov`/`or`/`and` imm ou `[cw]`), liaison **par slot CW**, voit au-delà du bloc
  (invariant de boucle). **Non prouvé ⇒ `X87Bail`** (→ runtime), **jamais** un
  `Nearest` deviné (un ceil faux violait le principe sacré).
- **Filet runtime** (`__x87rt_*`, incrément 1, **SOUND**) : quand la passe bail,
  ops FPU contre une pile FPU runtime (correcte par construction, bornée →
  `__builtin_trap`). Gaté transpile-only (`x87.is_none() && shared_stack()`),
  additif. ⚠️ Incréments 2/3 (transcendantes) **révoqués** (faux `sin(sin(1))`) puis
  refaits avec effacement **C2** (helper efface bit 10, notre libm gère toute plage).
- **Bug fucom runtime (2026-07-05)** : lire `op_register(op_count()-1)` (dernier ST),
  pas `op_register(0)` (= ST0 → compare avec soi-même → C3=1 → `awk /` = « div by
  zero »). Le statique était déjà correct.
- **Transcendantes = libm host-backed** (pow/sin/cos/exp/log/fmod/atan2… via
  `crt_symbol`/nom) — on branche la vraie libm, on ne lifte pas le x87 dense.
- **OUVERT (P2)** : réconciliation des **joins ambigus** (Lua `intarith`/`forprep`,
  busybox `seq`, join libm `awk` 0x429129) — la vraie difficulté ; + fp-returning
  auto-récursif (prouvé, à intégrer avec bénéfice) ; + ops transcendantales x87
  (`fldl2e`/`f2xm1`/`fscale`/`fyl2x`/`fsin`/`fcos`) OU host-back par signature d'idiome.
- **Réf 50** : « frndint honore 4 modes », « rounding_mode_active voit au-delà du
  bloc », « x87 fxam/fcmov/ftst », « Filet x87 runtime », « transcendantes C2 »,
  « x87/awk CAUSE RÉELLE fucom ».

### 2.3 `[ABI]` — pile partagée, appels, frame (`src/ir/build.rs`, `stdcall_pops.rs`)
- **Modèle shared-stack** : `esp` **par valeur** ; pile machine = région unique
  partagée ; **`ebp` threadé 5ᵉ reg-param** (callee-saved) — funclets EH sans frame
  héritent le `ebp` de l'appelant. Signatures **5 args partout** (sinon wasm-ld
  `signature_mismatch` → trap).
- **Callee-pop `ret N`** : imports via `stdcall_pops.rs` (table `@N` triée,
  binary-search, **test `table_is_sorted`**) ; **internes** via `compute_callee_pops`
  (scan `ret imm16`=`C2`, `C3`/cdecl=0) → `callee_pop_adjust` injecte `esp += N` (direct
  = constant, indirect = `__aret_callee_pop(va)` table runtime), **gardé par
  `has_callee_pops`** → binaires cdecl byte-identiques. *(fix cksum : dérive esp
  −4/itér des drivers FAST_FUNC/stdcall.)*
- **tail-`jmp [import]`/`jmp reg`** → **esp+4** (retaddr encore sur la pile). Aussi
  **`call reg`** d'un import stdcall → pop `@N` (`stdcall_pop_for_regcall`).
- **Frame helpers MSVC** : `_EH_prolog` **inliné** au site d'appel (détecté par `lea
  ebp,[esp+K]` K>0) ; `_chkstk`/`_alloca` = `esp -= eax` (détecté `xchg esp,eax`).
- **self tail-call** : `jmp func.entry` = tail-call frais (pas boucle) → passe les
  reg-args à jour (whereSplit sqlite).
- **auto-main** : sas CRT + `main` distinct ⇒ démarre au main (frame cdecl
  synthétique) ; `symbol_matches` ne retire **qu'UN** underscore (`main`↔`_main`, pas
  `___main`) ; `_initterm` **dispatche réellement** la table (argv/ctors).
- **Réf 50** : « cksum FIX LIVRÉ », « ABI __stdcall dépilage @N », « tail-jmp import
  esp+4 », « _EH_prolog », « _chkstk/_alloca », « CREATE TABLE self tail-call »,
  « threading d'ebp ».

### 2.4 `[RECOV]` — récupération de fonctions (`src/analysis/mod.rs`)
- **Sources d'entrées** : prologue scan (`looks_like_func_start`) ; **address-taken**
  = scan de données (pointeurs code alignés) + immédiats (`push imm`/`mov [esp],imm` =
  callback par valeur `stack_arg_code_imm`) + `mov reg,imm;…;call *reg`
  (`reg_imm_reaches_indirect_call`) + `mov [g],imm;…;call [g]` (`abs_store_imm`) +
  `call/jmp [disp32]` (`abs_indirect_slot`, contenu du slot) + `call [idx*4+base]`
  (`indexed_call_table_base`, tables init/atexit NASM).
- **Tables de saut** (`resolve_jump_tables_fixpoint`, avant le seed) : bornées par
  `cmp idx,N;ja` (`jump_index_bound`) ; **doublons préservés** (cases partagés) ;
  abs computed-goto (`resolve_abs_jump_table`) ; forme -O0 étagée ; **run ≥3× d'une
  valeur = switch, pas vtable** ; post-élagage des cibles (un corps de case ≠ entrée).
- **Re-split** : fonction absorbée après un appel **noreturn** (pas d'analyse noreturn
  au balayage) → **forcée** frontière quand une table de pointeurs/index la pointe.
  `compute_noreturn` = point-fixe **sound** (may_return si un succ n'est pas interne &
  pas noreturn ; jamais deviné).
- **x87 leaf-thunk** (`is_x87_leaf_thunk`) : décode tout le corps (fld arg→ops FPU→ret)
  → amorce atan2/fmod/trunc atteints par pointeur isolé.
- **FLIRT** (`src/flirt.rs`) : opérandes **relocalisés wildcardés** (`.reloc`,
  `Program::base_relocs`) ; **thunks jamais signaturés** (résolus par `import_thunk`) ;
  glue reconnue élargit `looks_like_func_start`. ⚠️ **FLIRT = cosmétique** (code de
  biblio, pas le code propre ; sensible à la version) — **le levier réel est le lifter**.
- **Sûreté** : une fausse entrée **tronque** une vraie fonction → miscompile ⇒
  régression complète obligatoire à chaque ajout de recovery.
- **OUVERT (P3)** : pointeur isolé atteint par **adresse calculée/indexée** (NASM
  `-f obj`, plink) = points-to (Phase 4).
- **Réf 50** : « address-taken », « Sur-récupération switch », « Borne de table de
  saut », « Table de saut dense ≠ table de pointeurs », « Fonction absorbée noreturn »,
  « compute_noreturn », « x87 leaf-thunk », « NASM indexed-call table », « Lua strippé
  frontières vérifiées ».

### 2.5 `[HLE-STDIO]` — FILE/stdio (`runtime/aret_hle/aret_hle.c`)
- **Tous les FILE = struct msvcrt-layout (32 o) fd-backed non bufferisés** :
  `_ptr`@0/`_cnt`@4/`_base`@8/`_flag`@12/`_file`@16, pushback ungetc @20 (char+1).
  On garde `_cnt≤0` pour que getc/putc **inlinés** défèrent à `_filbuf`/`_flsbuf`
  (read/write 1 octet sur le fd). `_iob` + fichiers **unifiés** (`file_fd()` reconnaît
  les deux). Pool de FILE (`alloc_dynfile`) pour fopen/freopen/tmpfile/fdopen.
- **Piège** : un FILE **glibc** hôte a `_flags`@0 = `0xfbad2488` → getc inliné lit ça
  comme pointeur → crash (`movzbl (%eax)`, eax=0xfbad2488). D'où le layout msvcrt.
- **close/isatty** : `aret_close` honore **tous** les fds (idiome close+réouverture
  uniq/tac ; `fclose` protège `_iob` séparément) ; `aret_isatty` **sauve/restaure
  errno** (sinon ENOTTY d'un `ioctl` sur fichier fuit → faux échec de tac).
- **putc** importé par nom (non inliné selon opt) → `aret_putc = aret_fputc`.
- **Réf 50** : « busybox wc FILE msvcrt-layout », « uniq/tac/tail close/isatty ».

### 2.6 `[HLE-FILE]` — fichiers, stat, chemins, mmap, wide
- **I/O** : open/read/write/close/lseek/**_lseeki64**/_telli64/_ftelli64, `_access`/
  `_chmod`/`_mkdir`/`_unlink`/`_getpid`, `SetEndOfFile`(=ftruncate)/`_chsize`.
- **Famille stat ABI-exacte** : `_stat`(36o)/`_stati64`(48o)/`_fstat`/`_fstati64` —
  layout Windows **fixe** (`__int64 st_size` aligné 8 → écriture à **offsets d'octets
  explicites**, sinon décalage silencieux). `st_mode` POSIX→msvcrt (`_S_IFDIR/REG/CHR`).
- **Chemins** (`translate_path`) : **Unix absolu `/…` passe au vrai FS** ; seuls les
  chemins Windows gardent le préfixe (`C:\`→`prefix/drive_c/`).
- **mmap** : `CreateFileMapping`/`MapViewOfFile`/… → mmap hôte (pointeurs guest plats),
  HANDLE mapping = pointeur tas, `#ifndef __wasm__` (WASI n'a pas mmap).
- **Wide** : `_wfopen`/`CreateFileW`/`GetFileAttributesExW`/`GetFullPathNameW`/
  `_wstati64`/`_waccess`/`_wremove`/`FindFirstFileW` (helpers `aret_w2n`/`aret_n2w`).
- **sqlite VFS** : `ReadFile`/`WriteFile` prennent l'offset via **`OVERLAPPED`**
  (→pread/pwrite) ; `GetFileAttributes*` **doit poser `GetLastError`**
  (ERROR_FILE_NOT_FOUND) sinon winAccess conclut IOERR ; LockFile* accordés (mono-proc).
- **Réf 50** : « Shims POSIX _getcwd », « translate_path Unix absolu », « famille stat
  msvcrt », « Rustine VFS sqlite SUPPRIMÉE », « mapping mémoire ».

### 2.7 `[HLE-WIN32]`/`[HLE-CRT]` — API (`aret_win32.c`, `aret_crt.c`, `aret_hle.c`)
- **Console/TTY** : Get/SetConsoleMode (FALSE si redirigé), GetFileType (fstat→
  CHAR/PIPE/DISK), GetStdHandle, GetConsoleCP.
- **Locale/codepage** : GetACP=1252/GetOEMCP=437, GetStringTypeW, LCMapStringW,
  MultiByte↔Wide (CP_ACP=Latin-1), GetCPInfo. Figé `LC_ALL=C`/`TZ=UTC` dans le harness.
- **Tls** (table de slots réelle), EncodePointer/DecodePointer (identité).
- **Heap/module** : HeapAlloc/…/HeapSize, LocalAlloc, GetModuleHandle(Ex)A/W,
  GetProcAddress=**0** (on ne distribue pas de pointeurs appelables — honnête/sound).
- **Process/thread (partiel, mono-thread)** : `CreatePipe`=pipe() **fidèle** ;
  CriticalSection/WaitForSingleObject = no-op/immédiat **sound sans concurrence** ;
  `CreateThread`/`CreateProcess`/`OpenProcess` = **échec sound** (pas simulé —
  frontière dure, cf. chantier threads §2.12).
- **Find/dir** : FindFirstFile(A/W)+fnmatch case-fold, CreateDirectory/RemoveDirectory.
- **version-info** : GetFileVersionInfo(Size)A/VerQueryValueA (parse VS_VERSIONINFO par
  signature `0xFEEF04BD`, rétrécit UTF-16→ANSI in place).
- **BSTR/COM minimal** : SysAllocString(Len)/… (ABI `[u32 byteLen][wchar[]][NUL]`),
  CoInitialize(Ex) (S_OK/S_FALSE compteur), CoTaskMemAlloc/…
- **Divers Win32** : temp-fichiers (GetTempPathW/GetTempFileName*), SetFileTime/
  GetFileTime (futimens), Local↔FileTime, VirtualQuery (MEMORY_BASIC_INFO valide,
  requis pei386 reloc mingw), Interlocked*, PeekNamedPipe (FIONREAD), GetThreadLocale
  (0x0409 en-US), TEB/PEB (`[fs:0x30]`→PEB→ProcessParameters), InitializeSListHead,
  __p__acmdln/__lconv_init (NASM), MapViewOfFile.
- **CRT** : printf/scanf complets + **`%I64`/`%I32`** MSVC (`aret_vformat` traduit
  I64→ll), `snprintf` C99 (longueur qui *aurait* été écrite), strtoll/strtoull/div/ldiv
  (retour **edx:eax** via `import_returns_u64` du builder), `atexit`(via `_onexit`,
  rejeu LIFO), setjmp/longjmp (macros au site lifté), rand LCG msvcrt
  (`seed*214013+2531011`), gmtime/localtime/mktime/strftime (struct tm Windows 9 ints).
- **Piège** : un shim appelant un helper `static` du runtime doit être défini
  **APRÈS** lui (sinon symbole fort non émis → stub faible gagne).
- **Réf 50** : nombreuses entrées « Axe 2 : … », « Shims — lot … winetest », « strings.exe
  … », « sqlite3.exe … ».

### 2.8 `[ORACLE]` — différentiels (`src/cpudiff.rs`, `bench/*.sh`)
- **cpudiff** (Unicorn, per-instruction) : ~120 encodages, milliers d'états ; interp
  renvoie `None` sur non-modélisé → **case sautée, jamais faux positif**. Couvre
  entier/div-idiv/SSE scalaire/SIMD packed + CF/ZF/SF/OF/PF/AF.
- **funcdiff** (Unicorn, fonction) : **closure** (suit les appels directs récupérés,
  discipline call/ret exacte, retaddr sentinelle non-mappée, frames OFF) + **opt-diff**
  (post-opt SSA vs pré-opt : DCE ne supprime jamais un Store, opt ne touche pas le
  CFG). memcpy/rep-stos modélisés ; adresses masquées 32-bit. **`0 divergence`
  ≠ pas de bug** : dit *où il n'est pas* (bugs profonds derrière imports/skips).
- **Portes** : difftest (décompile O0→O3, **271/271**), transpile-diff (produit, **4/4**,
  hash **`19acad982194bf07`**), winediff (Wine, **46/46**), sweeps (sqlite/busybox/
  gauntlet), SMT (Z3, 11/11), magicdiv (2³²), in-place (3/3), recompilabilité (100%).
- **`--mode imports`** : couverture d'imports **statique a-priori** (borne supérieure
  du trou runtime). Prioriser par la donnée, **filtrer par fidélité**.
- **OUVERT (P6)** : closure SSA (threader tout l'état CPU au call) — tentée, retirée
  (faux positif esp fantôme) ; modéliser imports purs (memcpy/memset/strlen) pour
  scorer la logique applicative derrière `malloc`.
- **Réf 50** : « Harness différentiel Unicorn », « funcdiff v0/CLOSURE », « Frontière
  SSA/opt », « funcdiff PORTE DE RÉGRESSION », « closure SSA RETIRÉE ».

### 2.9 `[RECOMPILE]` — backends & émission (`src/emit/`, `src/builder/`)
- **C** (`structured.rs`, défaut), **LLVM** (`llvm.rs`, `--backend llvm`, **chunké** :
  gros binaire → N modules `.ll` sans OOM), **WASM** (`--target wasm`, via backend C,
  `clang wasm32-wasi`+wasmtime). Préambule C de helpers `__ix_*`/`__fp_*`/`__x87*`/
  `__rep_*` à la demande.
- **Cast args libc en `(uint32_t)`** (ABI i386) : sans prototype, un `uint64_t`
  passe en 2 mots → `memcpy(dst, src.hi=0, n=src.lo)` cassé.
- **Verdict de solidité** : `SOUND` vs `INCOMPLETE — N non résolus, M partial(asm)` ;
  `--strict` (exit non-nul si non-sound). Abort bruyant runtime (`aret_unmodelled`,
  `aret_call` sur VA non récupérée).
- **Réf 50** : « Porte de solidité », « Différentiel du pipeline transpile », « Cible
  WASM signature ».

### 2.10 `[INFRA]` — conteneur, git, réseau, corpus
- **Conteneur éphémère, peut revenir à un état antérieur.** Au démarrage : `git fetch`
  + comparer à `origin` **avant** de conclure ; restaurer `git reset --hard
  origin/<branche>`. Hook `.claude/hooks/session-start.sh` resync un arbre **propre**
  ancêtre d'origin (**pas** le non-committé). ⇒ **commit + push après chaque incrément.**
- **Réseau** : `github.com` **BLOQUÉ**. Joignables : ftp.gnu.org, nasm.us, curl.se,
  zlib.net, sourceware.org, sqlite.org, pypi.org. Cross-compile `i686-w64-mingw32-gcc`.
  Pas de mingw g++ ni MSVC → binaires MSVC **téléchargés prébuild**.
- **Corpus committé** : `bench/.cache/` (busybox/sqlite3/winetest, négations .gitignore
  + README sha256) ; `bench/gauntlet/gauntlet-bins.tar.gz` (21 PE, auto-extrait par
  `score.sh`).
- **Diagnostic** : x87 avec un **vrai `--out-dir`** (pas /dev/null) ; recompiler le C
  généré **`-O0 -g`** pour un mapping gdb propre ; vérifier la **config du binaire
  cible** avant de présumer un bug (Lua `string.packsize("j")=4` = LUA_32BITS).

### 2.11 `[DEMO]` — démonstrateurs (bit-identiques à Wine sauf note)
- **Lua 5.4.7** (mingw, symbolé+strippé) : batterie **35/35**. Oracle axe 1.
- **strings.exe** (MSVC static-CRT C++) : sortie **100 % bit-identique** (version-info
  comprise), exit 0.
- **sqlite3.exe** (MSVC strippé, 2958 fn) : moteur SQL complet, sweep **30/30**
  (:memory: + on-disk).
- **NASM 2.16.01** (MSVC strippé) : `-v`/`-f elf`/`-f win32`/`-f bin` = objets
  identiques. Reste `-f obj` (points-to).
- **busybox-w32** (mingw strippé) : sweep **60/60** + awk `/`, cksum, wc, uniq/tac/tail…
  Reste grep/sed (regex, P4), m4 (P5).
- **WASM** : **7/7** fixtures. **Gauntlet** : **12/21** (`bench/gauntlet/`).
- Note : seule diff légitime = CRLF↔LF (mode texte) et `argv[0]` (environnemental).

### 2.12 `[64BIT]`/`[THREAD]`/`[GUI]` — chantiers ouverts
Détail : **70 §6** (roadmap). Résumé :
- **[THREAD]** : modèle actuel **mono-thread** (TEB/last-error/pile globaux). Chemin :
  TEB `__thread` → pile par thread + `aret_call` → sync pthread. CreateProcess = dur.
- **[64BIT]** : lift REX/registres 64-bit → binaires 64-bit + ELF ARM (LLVM multi-cible).
  Rattaché : overflow div/idiv 64-bit software.
- **[GUI]** : M7 — USER32/Winelib (couverture d'un coup) puis DXVK/vkd3d (jeux). Winelib
  = étape une fois, mécanisme prouvé (winegcc → ELF natif).

---

## 3. Journal chronologique (append-only, go-forward)

> Entrées datées, taguées, **les plus récentes en bas**. L'historique **avant
> 2026-07-05** est dans le [`50`](50-plan-execution.md) (indexé par les fiches §2).
> Utiliser le format §0.

### 2026-07-05 — [INFRA][DEMO] Corpus gauntlet sécurisé dans git
- **Cible** : 21 binaires du gauntlet ne vivaient que dans `/tmp` (perdus au reset).
- **Fix** : `bench/gauntlet/` = `gauntlet-bins.tar.gz` (6,2 Mo, auto-extrait) +
  `score.sh` (repo-relatif) + `build.sh` + README (provenance/sha256) + `.gitignore`.
- **Vérifié** : tarball ré-extrait 21 exes, sha256 concordants. Régression non touchée.
- **Réf 50** : « Corpus gauntlet sécurisé dans git ».

### 2026-07-05 — [DOC] Refonte documentaire : 70 (référence) + 71 (journal)
- **But** : le 50 (3491 l.) est trop lourd à relire après compression.
- **Fait** : créé **70** (état/méthode/reste/tips/roadmap complète M1→M7+64bit+threads+
  GUI, absorbe le 60) et **71** (ce journal structuré, cherchable par tag). 50 → archive,
  60 → absorbé ; bannières + pointeur HANDOFF. Protocole : agents mettent à jour 70
  (état) **et** 71 (détail).
- **Réf 50** : « Corpus gauntlet sécurisé » (dernière entrée avant refonte).

### 2026-07-05 — [LIFT][RECOMPILE] imul 1-opérande : sign-extension d'un opérande constant (sqlite3 mingw)
- **Cible/symptôme** : `sqlite3.exe` **mingw** (gauntlet) segfaultait sur toute requête
  (`SELECT 6*7;` → SIGSEGV). Deref `movzbl (%eax)` dans `sub_4558a0=sqlite3FindFunction`,
  chaîne `sqlite3_open_v2→openDatabase→sqlite3_overload_function→FindFunction`.
- **Forensics** (watchpoints) : FindFunction("MATCH") non-trouvé → `findElementWithHash`
  renvoie `&nullElement` (0x533160, sentinelle censée `{0,0,0,0}`), mais `nullElement.data`
  = 0x4f88a8 (FuncDef garbage) → walk du chain pNext → deref code → crash. `nullElement`
  corrompu par `sqlite3InsertBuiltinFuncs` (`a[h]=pDef` avec **h=0xffffff52** négatif →
  `[h*4+0x533420]` wrappe dans `nullElement`). h = `hash % 23` par division magique.
- **Cause racine** (`src/emit/mod.rs::signed_cast`, idem `emit/llvm.rs::emit_sign_extend`) :
  l'`imul` 1-opérande (signé) est lifté `sext(eax)*sext(r/m)`. Le magic `0xb21642c9` (bit 31
  set) arrive sous `SignExtend` comme **`Const` nue** (l'optimiseur folde `const & 0xffffffff
  → const`, `fold_binary` type `Ty::int(32)`). `signed_cast` ne gérait que `x & mask` et les
  loads sous-mot → la Const nue tombait sur `(int64_t)(0xb21642c9ULL)` = **zéro-étendu**
  (positif) au lieu de sign-étendu (négatif) → `mulhs` faux → `% 23` faux → h négatif → OOB.
- **Fix** : `signed_cast`/`emit_sign_extend` gèrent `Expr::Const(c, ty)` en sign-étendant
  depuis `int_bits(ty)` (< 64) — comme `signed_cast_w` le fait déjà avec une largeur explicite.
- **Portée** : bug **backend d'émission** (invisible à cpudiff qui teste l'IR, pas le C ;
  invisible à difftest/transpile-diff car le corpus n'exerce pas `imul const` haut-bit). Touche
  tout binaire avec division/modulo signé par constante (magic-multiply) — très courant.
- **Vérifié** : sqlite3 mingw `SELECT 6*7,hex(255),length,abs` = **bit-identique à Wine**
  (scalaire, variantes full/stripped). Fixture permanente `bench/winecorpus/signed_magicdiv.c`
  (grille `n%23`/`n/23`/`%7`/`%3`/`%365`, magics bit-31, = Wine). Régression : **difftest
  271/271, transpile-diff 4/4 (hash inchangé), funcdiff 0 divergence, magicdiv 2³², SMT 11/11,
  recompilabilité 100 %, winediff 47/47, cpudiff 0 fail**.
- **Reste sqlite3 mingw** : le **CRUD** (`CREATE TABLE`/`INSERT`) segfaulte encore, bug
  **distinct plus profond** : `sub_429330=sqlite3ExprAffinity` deref Expr null, via
  `sqlite3Select→findConstInWhere→constInsert` (optimisation WHERE const-propagation). Prochaine
  cible.

<!-- NOUVELLES ENTRÉES ICI (garder l'ordre chronologique, plus récent en bas) -->
