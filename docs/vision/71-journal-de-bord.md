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
| `[SSA]` | construction SSA / structureur (φ, split d'entrée, opt-diff) | §2.1 + entrées |
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
- **Tout-ou-rien statique/runtime (2026-07-10)** : les slots statiques `fpr()` et la
  pile runtime `__x87rt_s[]` sont **deux représentations distinctes** — un corps ne
  peut **pas** les mélanger. Donc le filet ne s'active que si **toute** la fonction
  bail. Conséquence : un bloc **émis non atteint** par la passe (`entry_sp=-1`,
  typiquement une **fonction absorbée** après un `call` noreturn, sans arête CFG)
  laisserait ses ops x87 **non mappées** → `aret_unmodelled` (abort) dans une fonction
  `Some`. Fix : `x87_depth_pass` **bail la fonction entière** si un bloc non atteint
  porte des ops x87 → filet runtime. Jamais un mélange, jamais un abort sur une op
  modélisable.
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
- **`___chkstk_ms` (GCC/mingw) = préserve tous les registres** (`is_chkstk_probe_fn`, prologue
  `push ecx;push eax;cmp eax,0x1000;lea ecx,[esp+…]`) : sonde de guard-pages pure (save/restore ecx+eax,
  esp inchangé, le caller fait son propre `sub esp,eax`). Le write-scan de `compute_call_clobbers` voyait
  les `push/pop/lea/sub` toucher ecx → marquait ecx clobbé → perdait la longueur d'un `memset` posée en ecx
  avant l'appel (idiome `mov ecx,len;call ___chkstk_ms;sub esp,eax;rep stos` = alloca+memset) → tableau non
  zéroé, terminateur manquant (getopt32 long-options busybox → `sed -n` cassé). Fix : **mask de clobber
  vide** (préserve tout) — l'appel est **lifté/appelé normalement** (la sonde est un leaf fidèle inoffensif,
  donc cpudiff matche Unicorn). ⚠️ **NE PAS** le modéliser en no-op : ça supprimait les écritures pile
  transitoires du `call` → **régression cpudiff** (`fn 0x4014e0 stack +0x7ff4`), corrigée par cette approche.
  Distinct de la variante MSVC `xchg esp,eax`. edx échappait au bug (préservé par le split edx:eax).
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
  au balayage) → **forcée** frontière quand une preuve la pointe : table de pointeurs
  (≥3), table `call [idx*4+base]`, **callback par valeur `stack_arg_code_imm`** (atexit/
  qsort), ou **slot `abs_indirect_slot`** (`call/jmp [slot]`). Gardé par
  `looks_like_func_start` (exclut les corps de case). `compute_noreturn` = point-fixe
  **sound** (may_return si un succ n'est pas interne & pas noreturn ; jamais deviné).
- **x87 leaf-thunk** (`is_x87_leaf_thunk`) : décode tout le corps (fld arg→ops FPU→ret)
  → amorce atan2/fmod/trunc atteints par pointeur isolé.
- **`mem_store_code_imm`** (`mov [base+d], code_imm`) : pointeur de méthode écrit dans un objet pointé
  par registre puis appelé via `call [obj+d]`. Address-taken (même force qu'un callback stack-arg) →
  amorce la fonction quel que soit son prologue, **y compris un stub `ret` nu** (`is_bare_ret_stub`).
  Accepté **uniquement** via ce store (jamais en balayage linéaire → padding non seedé) ; force-resplit
  d'une cible absorbée réservé aux vrais prologues (un `ret` interne ne tronque pas). Débloque NASM
  `-f obj` (méthode no-op du `struct ofmt`).
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
- **Info par handle** : `GetFileSize(Ex)`/`GetFileType`/`GetFileTime`/**`GetFileInformationByHandle`**
  (fstat sur le fd → `BY_HANDLE_FILE_INFORMATION` 52 o : attrs, 3 FILETIMEs, dwVolumeSerialNumber=st_dev,
  taille 64-bit, nNumberOfLinks, nFileIndex=inode 64-bit). Handles = fds (modèle mono-proc). Gardé par
  winecorpus `win32_fileinfo` vs Wine. Requis par la CRT mingw (fstat/isatty) et m4.
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
- **`signal(sig,handler)`** (`aret_hle.c`) : table de handlers par signal (`aret_sig_handlers[64]`),
  retourne **l'ancien** (SIG_DFL=0 au départ), stocke le nouveau ; SIG_ERR si hors bornes. **Pas de
  délivrance** async (modèle shared-stack) mais le retour fidèle est requis par le bookkeeping mingw/gnulib
  de blocage de signaux (installe `_blocked_handler`, puis au déblocage rappelle `signal()` en assertant
  qu'il rend l'ancien). Ex-stub `return 0` → abort `.cold` de `_sigprocmask` (m4). Gardé winecorpus `win32_signal`.
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
- **cpudiff** (Unicorn, per-instruction) : ~150 encodages, milliers d'états ; interp
  renvoie `None` sur non-modélisé → **case sautée, jamais faux positif**. Couvre
  entier/div-idiv/SSE scalaire/SIMD packed + CF/ZF/SF/OF/PF/AF + **famille pile/frame**
  (push/pop reg/mem/imm/16-bit, esp/ebp-relatif — le hotspot des bugs de composition).
  **Câblé en test de régression** (`per_instruction_corpus_matches_unicorn`, 2026-07-09 ;
  auparavant `run()`/`corpus()` existaient mais n'étaient exercés par aucun test). esp placé
  mid-page pour les insns modifiant esp (sinon écriture pile hors-page = case skippée).
  **Méthode d'énumération des classes de miscompile par construction** (vs subir binaire par binaire).
- **cpudiff-séquences** (`run_sequences`/`diff_seq`/`seq_corpus`, 2026-07-10) : **couche
  de composition** au-dessus du per-instruction. Une insn correcte *isolément* peut être
  fausse *en contexte* (une insn décale esp, une autre lit ensuite contre cet esp). Chaque
  insn est décodée à **son propre offset** (`decode_at` → ids de temp distincts, pas
  d'aliasing des temps de scratch entre insns du bloc), liftée, statements concaténés en un
  bloc droit ; interp vs Unicorn (`count=n_insns`), esp mid-page + ebp quart-page → tout
  accès pile/frame tombe dans la page comparée. **Trouvé au 1ᵉʳ run : `push esp` poussait
  l'esp *post*-décrément** (le patron `push esp;pop eax` rendait `esp-4`). Fix général :
  `src_uses_sp` élargi (sémantique 286+ : `push esp`/`push [esp+d]` lisent la source **avant**
  de baisser esp → snapshot temp). **Double trou fermé** : le per-instruction avait `push esp`
  au corpus mais ne comparait la page que si `mem_base.is_some()` (opérande mémoire explicite) —
  or push écrit la pile via opérande *implicite* → comparaison page élargie à
  `stack_pointer_increment() != 0`. Câblé `sequence_corpus_matches_unicorn`.
- **cpudiff-séquences-génératif** (`run_sequences_random`/`seq_pool`, 2026-07-10) :
  compose **au hasard** des blocs de 2-3 insns depuis `seq_pool` (GP + pile/frame +
  mémoire esp/ebp/edi) — l'achèvement honnête de « énumérer par construction » (les 12
  séquences curatées ratent les paires non imaginées). `diff_seq` sème aussi **edi/esi**
  dans la page (bases `[edi]`/`[esi]` → scorées au lieu de fauter). **Piège clé résolu :
  drapeaux indéfinis**. `and/or/xor/test` laissent **AF indéfini**, un shift multi-bit
  laisse **OF/AF indéfinis** ; comparer l'*union* des flags écrits par le bloc diffait un
  don't-care contre le résultat indéfini d'Unicorn (52 faux positifs au 1ᵉʳ run). Fix :
  `cmp_flags` = flags de la **dernière insn seulement** (les seuls définis en fin de bloc ;
  leurs entrées sont recalculées identiquement par les 2 moteurs sauf vrai bug de
  composition — exactement la cible). 4000 blocs × 150 états = **0 divergence** →
  couche de composition **prouvée saine**. Câblé `sequence_random_matches_unicorn`.
- **funcdiff** (Unicorn, fonction) : **closure** (suit les appels directs récupérés,
  discipline call/ret exacte, retaddr sentinelle non-mappée, frames OFF) + **opt-diff**
  (post-opt SSA vs pré-opt : DCE ne supprime jamais un Store, opt ne touche pas le
  CFG). memcpy/rep-stos modélisés ; adresses masquées 32-bit. **`0 divergence`
  ≠ pas de bug** : dit *où il n'est pas* (bugs profonds derrière imports/skips).
- **Portes** : difftest (décompile O0→O3, **271/271**), transpile-diff (produit, **4/4**,
  hash **`19acad982194bf07`**), winediff (Wine, **57/57**), sweeps (sqlite/busybox/
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
- **NASM 2.16.01** (MSVC strippé) : `-v`/`-f elf`/`-f win32`/`-f bin`/**`-f obj`** = objets
  bit-identiques à Wine (`-f obj` débloqué 2026-07-09 par `mem_store_code_imm`).
- **busybox-w32** (mingw strippé) : sweep **60/60** + awk `/`, cksum, wc, uniq/tac/tail, **grep/sed
  (dont `sed -n`)**.
- **m4 (GNU M4 1.4.19, mingw)** : macros/eval/translit/ifelse/récursion/`--version` **bit-identiques à Wine**.
- **WASM** : **7/7** fixtures. **Gauntlet** : **19/21** MATCH (21/21 fonctionnels ; reste units ×2 = `units.dat` absent, environnemental).
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
- **Reste sqlite3 mingw** : voir l'entrée [LIFT][SSA] ci-dessous (2ᵉ bug — RÉSOLU).

### 2026-07-05 — [LIFT][SSA] Split du bloc d'entrée = en-tête de boucle (sqlite3 mingw CRUD)
- **Cible/symptôme** : sqlite3 mingw `CREATE TABLE`/`INSERT` segfaultait/bouclait (après le fix
  imul). `sub_429330=sqlite3ExprAffinity` deref `Expr*` null, via `sqlite3Select→findConstInWhere
  →constInsert`.
- **Cause racine** (`src/ssa/mod.rs::to_ssa`) : quand le **bloc d'entrée de la fonction EST un
  en-tête de boucle** (atteint par un back-edge — cas des fonctions **regparm** dont le param
  arrive en registre et est la variable de boucle, lue d'emblée sans pre-header), la φ placée à
  l'en-tête pour ce registre n'avait **pas de prédécesseur pour l'edge d'entrée** → ses args ne
  couvraient que les back-edges → la **valeur initiale (le paramètre) était perdue** → la
  variable d'induction n'avançait jamais (boucle infinie / valeur fausse ; l'arg n'était jamais
  relu). `preds[entry]` = back-edges seulement.
- **Fix** : **split du bloc d'entrée** — si `blocks[entry].pred` non vide, insérer un pre-header
  vide qui **reprend la VA `func.entry`** et `jmp` vers l'ancien en-tête (qui reçoit une adresse
  synthétique unique). L'en-tête a alors un edge d'entrée propre (depuis le pre-header) → sa φ
  gagne un slot initial seedé avec la valeur du paramètre. Construction SSA standard. `id==index`
  préservé (pre-header appended à l'index n). Ne se déclenche **que** si l'entrée a des préds
  (0 régression sur le cas normal — hash transpile inchangé).
- **Distinction** avec le self-tail-call (2026-07-03) : celui-ci gère `jmp entry` **inconditionnel**
  (→ tail-call, param passé frais) ; ce fix gère le back-edge **conditionnel** vers l'entrée
  (→ vraie boucle, φ à seeder).
- **Vérifié** : sqlite3 mingw **CRUD/index/CTE/window/IN bit-identiques à Wine**. Fixture
  permanente `tests/m1/fixtures/loop_header_entry.{c,exe}` (walk asm : entrée=en-tête, back-edge
  conditionnel `jne _walk`, param regparm eax ; `7 7 7 0`) + test `loop_header_is_entry_block_phi_
  seeded`. Pré-fix : boucle infinie ; post-fix : `7 7 7 0`. Régression : **difftest 271/271,
  transpile-diff 4/4 (hash inchangé), funcdiff OPT-diff 10581 scored 0 divergence** (gate SSA sur
  busybox+sqlite réels), magicdiv 2³², SMT 11/11, recompilabilité 100 %, winediff 47/47.
- **sqlite3 mingw = fonctionnel** (scalaire + CRUD + agrégats + jointures + index + CTE + window).

### 2026-07-05 — [RECOV] Callback atexit absorbé par un noreturn → re-split forcé (sqlite strippé)
- **Cible/symptôme** : `sqlite3_stripped` + `sqlite3_full_stripped` (gauntlet) abortaient sur
  `indirect call to unrecovered function 0x40c600`.
- **Cause racine** (`src/analysis/mod.rs`) : `0x40c600 = _sayAbnormalExit`, enregistré par
  `atexit` (`movl $0x40c600,(%esp)` = callback par valeur, atteint par appel indirect au exit).
  Il suit immédiatement `call _shell_out_of_memory` (**noreturn**) + padding NOP → le balayage
  linéaire (noreturn non détecté) l'**absorbe** dans `_save_err_msg`. Le seed
  `stack_arg_code_imm` ne prend un candidat que si `!global.contains_key` → 0x40c600 déjà
  décodé → skippé → appel indirect vers fonction non récupérée → abort. (`looks_like_func_start`
  le reconnaît pourtant : prologue `mov eax,[moffs32]; test eax,eax`, opcode `a1`.)
- **Fix** : étendre le **re-split forcé** (déjà en place pour les tables de pointeurs ≥3 et les
  tables `call [idx*4+base]`) aux cibles **`stack_arg_code_imm`** ET **`abs_indirect_slot`**
  absorbées : si `global.contains_key(v) && looks_like_func_start(v)`, `forced.insert(v)` (au
  lieu de skip). La position de callback/le contenu du slot prouvent que `v` est une fonction ;
  `looks_like_func_start` (qui exclut les corps de case de jump-table) garde le re-split sûr.
- **Portée** : général — tout binaire strippé enregistrant un callback (`atexit`/`qsort`/vtable
  via slot) placé juste après un appel noreturn (`*_and_die`/`exit`/`abort`). Même classe que le
  fix table-de-pointeurs (2026-07-02) mais pour un callback **isolé**.
- **Vérifié** : `sqlite3_stripped` (`42|323535;14`) + `sqlite3_full_stripped` (`2|3|x,y`)
  **bit-identiques à Wine** (scalaire + CRUD). Régression : transpile-diff 4/4 (hash inchangé),
  + difftest/funcdiff/winediff/sqlite-sweep (voir commit). *(Note : la cause profonde reste
  l'absence d'analyse noreturn au balayage — on la rattrape par la preuve callback, comme les
  autres cas.)*
- **Gauntlet** : 14/21 → **16/21** (sqlite : 4/4 variantes MATCH).

### 2026-07-05 — [HLE?/LIFT?] m4 mingw : abort dans le bookkeeping signaux (DIAGNOSTIQUÉ, non corrigé)
- **Symptôme** : m4 (gauntlet) ne sort **rien** (même `--version`) puis **abort** (SIGABRT).
- **Localisation** : `main → _sigaction (0x42a0d0) → _sigprocmask (0x42a420)`. Dans le
  bookkeeping de blocage de signaux mingw statique, une assertion `.cold` échoue :
  `mov 0x461de0,%eax` (ancien handler du signal 13/SIGPIPE) ; `cmp $0x42a2c0,%eax`
  (`_blocked_handler`) ; `jne .cold → abort()`. L'ancienne valeur lue ≠ `_blocked_handler`
  alors que sous Wine elle l'est → **divergence d'un state signal en amont**.
- **Nature** : fonctions **statiquement liées** (pas des imports) → ARET les lifte/exécute ;
  divergence de state signal. Abort **sound** (aucune sortie fausse).
- **Précisé (2026-07-05)** : `[0x461de0]` = slot spécial du handler SIGPIPE (signal 13). Il a un
  writer **`movl $0x42a2c0,0x461de0`** (constante = `_blocked_handler`) au chemin **block** de
  `_sigprocmask` (0x42a5bd) — mais un **watchpoint (après mapping) ne le voit qu'UNE fois**, au
  chemin `esi`-store (0x42a4ed), jamais le block direct. Donc **le chemin block de sigprocmask
  n'exécute pas** dans ARET → `[0x461de0]` reste 0 → le chemin assert (`mov 0x461de0,%eax; cmp
  $0x42a2c0,%eax; jne .cold→abort` à 0x42a4f3) échoue. **NON causé par auto-main** (l'entrée CRT
  complète `--entry 0x4013e0` abort au même endroit → les ctors ne sont pas en cause).
- **Prochain pas** : comparer le `how` de sigprocmask (regparm : eax/edx) et le **flot de contrôle
  de `_sigprocmask`** ARET vs Wine — pourquoi le chemin block (`cmp $edx,1/2` en tête) n'est pas
  pris. Soit `how` mal lifté, soit une branche divergente. funcdiff sur `_sigprocmask`/`_sigaction`
  pourrait pointer un mislift. Intriqué → session dédiée ; alternative : host-backer sigaction/
  sigprocmask (FLIRT faible ici). *(Niche : seul m4 du corpus exerce ce chemin.)*

### 2026-07-05 — [DEMO] busybox regex OK ; nouveau bug localisé : `sed -n` (auto-print) — breadcrumb
- **Bonne nouvelle** : le SIGSEGV regex de busybox grep/sed (P4 du journal) **ne se reproduit
  plus** — grep (`-i/-v/-c/-o/-E`, littéral, classes, `-o [A-Z][a-z]*`) et sed (`s///g`, `2d`,
  `p`) = **bit-identiques à Wine**. Probablement résorbé par les fixes récup/SSA accumulés.
- **Nouveau bug trouvé (concret, reproductible)** : **`sed -n` (suppression de l'auto-print) est
  ignoré** → l'auto-print reste actif. `printf 'X\n' | busybox sed -n p` → ARET `X\nX\n` (2×),
  Wine `X\n`. Toutes les variantes `-n`/`-ne`/`-n 2p`/`-n /re/p` échouent ; `sed p` (sans -n),
  `sed 2d`, et **grep -q/-n** (même getopt32) marchent → **spécifique au flag `-n` de sed**.
- **Localisé** (busybox_g `-g` + gdb) : l'auto-print est dans `sub_44f090` (sed process loop),
  chunk_5.c:39215, gardé par `v217 = ([0x47a2f0]==0); if(v217==0) skip else autoprint`. Donc
  **`[0x47a2f0]` = `be_quiet`** ; l'auto-print est sauté ssi `be_quiet != 0`. Watchpoint : ARET
  n'écrit `[0x47a2f0]` **qu'une fois à 0** (init) et **jamais à 1** pour `-n` → auto-print actif.
- **Approfondi (2026-07-05) — cause = getopt32 (compteur `-n`)** :
  - **`0x47a2f0` = be_quiet CONFIRMÉ** : forcer `set *(u8*)0x47a2f0=1` à l'entrée du process loop
    supprime l'auto-print (`sed -n p` → `X` au lieu de `X X`).
  - **sed_main (`sub_45c1f4` = 0x45c1f4)** passe **`&be_quiet` (`movl $0x47a2f0,0x18(%esp)`)** à
    **getopt32 (`0x433500`)**. Chaîne d'options sed = **`^i::rEne:*f:*`** (`-n` = **bit 3**, `-i`
    bit 0, `-r` bit 1, `-E` bit 2). Le `^` = *opt_complementary* embarqué → **`-n` est un COMPTEUR**
    (getopt32 incrémente `*be_quiet`), pas un simple bit.
  - **Symptôme runtime** : après getopt32, `option_mask32 (0x47a1b0)` = **0x2 constant** quel que
    soit `-n/-r/-nn/-rn`, et `be_quiet (0x47a2f0)` reste **0**. → **getopt32 ne fait pas
    l'incrément compteur** de `-n`. getopt32 marche pour **grep** (pas de compteur `^`) → c'est le
    **chemin compteur/opt_complementary de getopt32** qui est mal lifté.
  - **Prochain pas** : diaguer `getopt32` (0x433500) — le code qui, pour une option marquée
    compteur dans la chaîne `^…`, écrit `(*ptr)++`. Candidats : mislift d'un store indirect via un
    pointeur vararg, ou une branche du parseur de la chaîne complementary. funcdiff peu utile
    (dépend de l'argv/opts précis). Repro : `printf 'X\n' | busybox sed -n p` (ARET `X X`, Wine `X`).

### 2026-07-09 — [ABI][DEMO] `___chkstk_ms` (mingw) préserve les registres → `sed -n` RÉSOLU
- **Cible/symptôme** : busybox `sed -n` (suppression auto-print) **ignoré** → `printf 'X\n' | sed -n p`
  sortait `X\nX\n` (ARET) au lieu de `X\n` (Wine). `sed p` correct. Spécifique au flag compteur `-n`.
- **Forensics** (busybox transpilé instrumenté, `fprintf` aux sites clés de `vgetopt32` = `sub_46d5ac`) :
  1. Le compteur `on_off->counter = va_arg(p, int*)` **s'exécute** (garde `if(c==*s)` correcte) mais lit
     **0** au lieu de `&be_quiet` (0x47a2f0). Trace du curseur va_list : les 3 va_arg des options courtes
     (`&opt_i/&opt_e/&opt_f`) sont OK, puis la **boucle LONG_OPTS consomme 2 va_arg parasites** — le 1er
     vole `&be_quiet` (destiné au compteur) → curseur 8 o trop loin → compteur lit 0 → `counter=NULL` →
     `(*counter)++` sauté → `be_quiet` reste 0 → auto-print jamais supprimé.
  2. Les 2 va_arg parasites viennent de long-opts **garbage** ajoutés par `for(l_o=long_options; l_o->name;
     l_o++)` : le tableau `long_options` (alloca'd) est **rempli correctement** (7 entrées i/r/n/n/e/f/b) mais
     **son terminateur NUL manque** → la boucle de parcours court au-delà, lit du garbage (`name≠0`), croit à
     de vrais long-opts et consomme leur `va_arg`.
  3. Terminateur manquant = `memset(long_options, 0, count*sizeof)` lifté **`__rep_stos8(dst, 0, undef)`** :
     **longueur 0**.
- **Cause racine** (`src/ir/lift.rs` modèle de clobber d'appel + `src/ir/build.rs`) : le code compilé est
  `mov ecx, count*16 ; call ___chkstk_ms ; sub esp, eax ; rep stosb` (alloca puis memset, longueur en **ecx**).
  `___chkstk_ms` (0x40a5c8, helper GCC/mingw de sonde de guard-pages) fait **`push ecx`/`pop ecx`,
  `push eax`/`pop eax`** et **ne modifie pas esp** (le caller fait son propre `sub esp,eax`) → il **préserve
  tous les registres GP**. Mais ARET modèle un `call` comme clobbering le caller-saved **ecx** (`lift.rs`
  §2210 : `preserves_ecx` via `call_clobber_mask`, sinon `ecx=Undef` ; edx est préservé par le split edx:eax,
  d'où le biais ecx-only). `___chkstk_ms` n'était **pas reconnu** : la détection `is_stack_alloc_helper`
  cherche `xchg esp,eax` (variante MSVC `_chkstk`/`_alloca_probe` qui abaisse esp elle-même) — absente ici.
  → `call` normal → ecx clobbé → longueur du memset `undef` → 0.
- **Fix** (`src/ir/build.rs`) : nouveau détecteur **`is_chkstk_probe`** (prologue à 4 insns non ambigu
  `push ecx ; push eax ; cmp eax,0x1000 ; lea ecx,[esp+…]`) → le `call ___chkstk_ms` est modélisé
  **no-op** (aucun stmt émis) : esp inchangé, tous les registres GP préservés, la sonde de pages inutile sur
  la pile plate native. La taille dans `eax`/`ecx` survit au `sub esp,eax` / `rep stos` suivant.
- **Portée** : **général** — tout binaire mingw utilisant `alloca`/VLA/grande frame via `___chkstk_ms`
  suivi de l'usage d'un registre caller-saved posé avant l'appel (idiome alloca+memset très courant). Distinct
  de la variante MSVC `xchg esp,eax` (toujours modélisée `esp -= eax`).
- **Vérifié** : `sed -n p/-nn/-ne/-n Np/-n /re/p` + combos `-rn` = **bit-identiques à Wine** (12/12 batterie
  sed/grep). Fixture permanente `tests/m1/fixtures/chkstk_ms_preserves_ecx.{c,exe}` (inline-asm : stage
  `0x1234` en ecx à travers `call ___chkstk_ms`, relit ecx ; pré-fix `LOST`/0, post-fix `OK`) + test
  `chkstk_ms_probe_preserves_registers`. Régression : **difftest 271/271, transpile-diff 4/4
  (hash `19acad982194bf07` inchangé), winediff 47/47, funcdiff 0 divergence, busybox sweep 60/60, cargo test
  vert**.
- **Reste** : `sed -i` bute sur des imports Win32 non implémentés (`GetFileInformationByHandle`, `_mktemp`…)
  — indépendant, non lié au compteur.

### 2026-07-09 — [HLE-FILE][DEMO] `GetFileInformationByHandle` (fstat) + statut m4 précisé
- **Cible** : m4 (gauntlet), après le fix `___chkstk_ms`, abortait sur `unimplemented import:
  GetFileInformationByHandle` (aussi requis par `sed -i` et la CRT mingw `fstat`/`isatty`).
- **Fix** (`runtime/aret_hle/aret_hle.c`) : `aret_GetFileInformationByHandle(h, BY_HANDLE_FILE_INFORMATION*)`
  = `fstat(fd)` → remplit les 52 o (attrs via S_ISDIR/!S_IWUSR ; 3 FILETIMEs via `aret_ts_to_ft` ;
  dwVolumeSerialNumber=st_dev ; taille 64-bit ; nNumberOfLinks=st_nlink ; nFileIndex=inode 64-bit). Handles
  = fds (comme GetFileSize/GetFileTime). Erreur → `g_last_error=6` (INVALID_HANDLE), retourne 0. Reconnu
  auto par le scan `aret_X(uint32_t` du builder ; `@8` déjà dans `stdcall_pops`.
- **Vérifié** : winecorpus `win32_fileinfo.c` (écrit 16 o, ouvre, interroge → `size=16 dir=0 links=1`)
  **bit-identique à Wine**. **winediff 48/48** (47→48). Additif pur : difftest/transpile-hash/funcdiff
  inchangés.
- **Statut m4 mis à jour** : ne bloque **plus** sur cet import → retombe sur l'abort **pré-existant**
  (P5) **confirmé** dans `_sigprocmask` (`sub_42a420`) ← `_sigaction` (`sub_42a0d0`) ← main (backtrace gdb :
  `aret_abort` = assertion `.cold` de m4). L'abort du bookkeeping signaux reste la vraie frontière m4
  (session dédiée, cf. entrée `[HLE?/LIFT?] m4` du 2026-07-05). **Abort sound**, aucune sortie fausse.

### 2026-07-09 — [HLE-WIN32][DEMO] `signal()` retourne l'ancien handler → m4 FONCTIONNEL (gauntlet 19/21)
- **Cible/symptôme** : GNU m4 (gauntlet) abortait (SIGABRT) au démarrage, même sur input vide. Backtrace
  gdb : `aret_abort` ← `_sigprocmask` (`sub_42a420`) ← `_sigaction` (`sub_42a0d0`) ← main. (Après le fix
  `GetFileInformationByHandle` du même jour, qui avait levé un abort d'import antérieur.)
- **Forensics** (m4 transpilé instrumenté, `fprintf` dans `sub_42a420`) : trace des appels sigprocmask :
  appel 1 `how=0` (BLOCK) bloque 0x7fffbf → écrit bien `[0x461de0]=_blocked_handler` (SIGPIPE) ; appel 2
  `how=1` (UNBLOCK) → l'assert `.cold` (`cmp eax,0x42a2c0; jne→abort`) **échoue avec eax=0**. Clé : pour les
  signaux **non-SIGPIPE** débloqués, l'asm fait `call _signal(sig, saved); jmp <assert>` avec **eax = retour
  de signal()**, et **asserte que signal() rend `_blocked_handler` (0x42a2c0)** = le handler installé au
  blocage. (Corrige l'hypothèse du 2026-07-05 « le chemin block n'exécute pas » : il exécute bien ; le vrai
  bug est le **retour de signal()**.)
- **Cause racine** (`runtime/aret_hle/aret_hle.c`) : `aret_signal` était un **stub `{ return 0; }`**. Le
  bookkeeping mingw/gnulib de blocage installe `signal(sig, _blocked_handler)` au blocage puis, au déblocage,
  rappelle `signal(sig, saved)` et **asserte que le retour == le handler précédent** (`_blocked_handler`). Le
  stub rendant 0, l'assert échouait → abort.
- **Fix** : table `aret_sig_handlers[64]` — `signal()` retourne l'**ancien** handler (SIG_DFL=0 au départ),
  stocke le nouveau ; SIG_ERR (-1) si sig hors bornes. **Pas de délivrance** async (inchangé vs stub ; modèle
  shared-stack sans signaux async), mais le retour fidèle rend le bookkeeping cohérent. **Sound** : strictement
  mieux que le stub (qui provoquait un faux abort).
- **Vérifié** : m4 macros (`sq(12)`→`(12*12)`), eval/translit/ifelse/récursion, `--version` **bit-identiques
  à Wine**. **Gauntlet 16→19/21** (m4 ×2 MATCH ; 21/21 fonctionnels, reste units ×2 = environnemental).
  Fixture winecorpus `win32_signal.c` (`p1_dfl=1 p2_h1=1 p3_dfl=1`) ; **winediff 49/49**. Additif pur.
- **Reste** : la CRLF de m4 (stdout `_O_TEXT`) est normalisée par le harness (`tr -d '\r'`) — diff texte
  légitime, pas un bug.

### 2026-07-09 — [RECOV] `mem_store_code_imm` : pointeur de méthode `ret` nu (NASM `-f obj`)
- **Cible/symptôme** : `nasm -f obj` **abortait** (`indirect call to unrecovered function 0x43e0b0`) —
  Wine sort un `.obj` de 108 o, ARET rien. 0x43e0b0 = **`ret` nu** (méthode no-op du `struct ofmt` OMF).
- **Cause racine** (`src/analysis/mod.rs`) : 0x43e0b0 est installé par `mov DWORD PTR [ebx], 0x43e0b0`
  (pointeur de méthode dans un objet pointé par **registre**) puis appelé via `call [obj+disp]`. Atteint
  par **aucun** call direct, dans un trou du balayage linéaire. `imm_code_ptrs` captait déjà l'immédiat,
  mais le seul chemin de seed (ligne 746) exige `looks_like_func_start`, qui **rejette le `ret` nu** (0xc3
  ne matche aucun prologue). Les détecteurs existants ne couvraient que `mov [global],imm` (`abs_store_imm`,
  base=None) ou les stack-args (`stack_arg_code_imm`, base=esp) — pas `mov [reg],imm` (base registre).
- **Fix** : détecteur **`mem_store_code_imm`** (`mov [base+…], code_imm`, base register) → seed la cible
  via `looks_like_func_start` **OU** `is_bare_ret_stub` (première insn = `ret`/`retf`). Accepté seulement
  sur espace **non réclamé** (`!global.contains_key`) ; force-resplit d'une cible absorbée réservé aux
  vrais prologues (un `ret` nu interne ne doit jamais tronquer une fonction). Address-taken = preuve forte,
  comme le callback stack-arg. **Général** : tout objet/struct-de-pointeurs initialisé par `mov [reg],method`.
- **Vérifié** : `nasm -f obj` (simple + data/labels/externs/relocations) **bit-identique à Wine**
  (`cmp` = 0). Guard : `-f obj` ajouté au cas nasm du gauntlet (assemble + `od` de l'.obj sur stdout).
  Régression : **difftest 271/271, transpile-diff 4/4 (hash `19acad982194bf07` inchangé), funcdiff 0
  divergence (12467 lift / 10688 opt scorées — plus de fonctions récupérées dans busybox/sqlite, toutes
  correctes), winediff 49/49, busybox sweep 60/60, gauntlet 19/21**. Fixture C minimale non retenue (le
  balayage linéaire absorbe un stub adjacent → ne reproduit pas ; NASM garde le vrai cas isolé).
- **Reste** : **plink** re-mesuré (snapshot PuTTY actuel, téléchargé de `tartarus.org`) — l'abort
  points-to historique `0x450058` **a disparu** (résolu par les fixes récup accumulés). Nouveaux blocages
  **différents** : imports `GetEnvironmentStringsW`/`RegOpenKeyExA`/`RegCloseKey` (registre = émulation non
  bornée, non prioritaire) + 3 stubs `ret`/`xor eax,eax;ret` appelés directement mais non récupérés
  (appelants non atteints par la descente ; hors chemin `-V`). Binaire externe hors corpus → borné, pas chassé.

### 2026-07-09 — [RECOV][HLE-WIN32] plink : env-block + registre vide + exemption jt des cibles d'appel direct
- **Cible** : plink (PuTTY, clang, téléchargé `tartarus.org`). `-V` **segfaultait**. Fix par 3 causes
  **générales** distinctes, chacune vérifiée sans régression.
- **(1) `GetEnvironmentStringsW/A`** (`aret_win32.c`) : le stub retournait NULL → plink déréférençait le bloc
  d'environnement NULL → SIGSEGV. Fix : construire un vrai bloc `VAR=VALUE\0…\0\0` (wide/ansi) depuis
  `environ` hôte + `FreeEnvironmentStrings{W,A}`. Général (tout programme lisant l'environnement complet).
- **(2) Registre vide sound** (`aret_win32.c`) : `RegOpenKeyExA`/`RegCreateKeyExA`/`RegQueryValueExA`/
  `RegSetValueExA`/`RegEnumKeyA`/`RegCloseKey` retournaient 0 (SUCCESS) avec un HKEY garbage = **unsound**
  (ment). Fix : modéliser un **hive vide read-only** — opens/queries → `ERROR_FILE_NOT_FOUND` (2), enum →
  `ERROR_NO_MORE_ITEMS` (259), écritures → `ERROR_ACCESS_DENIED` (5, pas de no-op silencieux), close → 0.
  Le programme prend son chemin « clé absente » (état valide). + `@N` stdcall ajoutés (`stdcall_pops`).
- **(3) [RECOV] Exemption jt des cibles d'appel direct** (`src/analysis/mod.rs`) : 3 stubs no-op
  (`ret`/`xor eax,eax;ret`/`mov al,1;ret`) étaient **à la fois** cases-default d'une jump-table **et**
  fonctions appelées directement + entrées d'une vtable (null-handlers PuTTY). Le pruning
  `entries.retain(|e| !jt_targets.contains(e))` les retirait → `call`/dispatch indirect abortait sur code
  non récupéré. Cause : une adresse **directement appelée est sans ambiguïté une fonction**, même si son
  adresse sert aussi de case-default. Fix : exempter les **cibles d'appel direct** (`call_targets` collecté
  du global) du pruning jt (`!jt_targets.contains(e) || call_targets.contains(e)`) ; + accepter un stub
  `ret` nu comme entrée d'une table de pointeurs confirmée (`bare_stub_in_table`, force-resplit). **0
  unresolved direct call** (était 3).
- **Vérifié** : difftest 271/271, transpile-diff hash `19acad982194bf07` inchangé, funcdiff 0 divergence,
  winediff 49/49, busybox sweep 60/60, gauntlet 19/21 (aucune régression). plink : recovery **complète** +
  imports gérés ; ne segfault plus sur l'env.
- **Reste plink** : segfault résiduel dans le chargement de **config** (`sub_4845d0` compare "SerialLine"
  vs NULL) — plink dépasse la détection de `-V` et entre dans le démarrage complet (Wine imprime la version
  et sort). Divergence de flot / miscompile en amont dans le parsing d'arguments — forensics à part.

### 2026-07-09 — [LIFT][ABI] `push [esp+d]` (esp pré-décrément) + chkstk clobber-mask (fix régression cpudiff)
- **(1) [LIFT] `push [esp+d]`** (`src/ir/lift.rs`) : le lift de `push` émettait `Set{esp=esp-4}` **avant**
  le `Store{[esp]=v}`, et `v` (=`[esp+d]`, contenant `Read(esp)`) se résolvait en SSA à la position du Store
  → **esp post-décrément** → source lue à `[esp-4+d]` au lieu de `[esp+d]` (**décalé de 4**). Tout
  `push [esp+d]` (idiome clang/MSVC très courant de forward d'un arg pile) poussait la mauvaise valeur.
  Trouvé sur plink : `sub_431310` faisait `push [esp+8]` (forward de son arg1) → NULL au lieu du pointeur →
  `strcmp(NULL,"SerialLine")` → segfault dans le chargement de config. Fix : quand la source référence esp,
  la capturer dans un **temp avant** le décrément (`t=[esp+d]; esp-=4; [esp]=t`). Autres pushs (reg/imm/mem
  non-esp) inchangés (hash préservé).
- **(2) [ABI] chkstk clobber-mask remplace le no-op** (`src/ir/build.rs`) : le fix `___chkstk_ms` du
  2026-07-09 le modélisait en **no-op** — ce qui **supprimait les écritures pile transitoires** du `call`
  (retaddr/regs sauvés dans la région de frame). cpudiff (comparaison pile complète, dont sous-esp) →
  **régression `fn 0x4014e0 stack +0x7ff4: lifted=0x0 unicorn=0x10`** (introduite par 1dffe90, non détectée
  car cpudiff pas lancé après). Fix : **ne pas no-op** ; à la place `is_chkstk_probe_fn` → `compute_call_
  clobbers` reporte un **mask vide** (chkstk préserve tout via save/restore) → ecx survit **sans** sauter le
  call → la sonde est liftée/exécutée fidèlement → cpudiff matche Unicorn.
- **Vérifié** : cpudiff **PASS** (régression résolue), difftest 271/271, transpile-diff hash
  `19acad982194bf07` inchangé, funcdiff 0 divergence, winediff 49/49, busybox sweep 60/60
  (**`sed -n` toujours OK**), gauntlet 19/21. plink : le segfault "SerialLine" est **résolu** (push fix) ;
  plink progresse dans son démarrage (crash résiduel plus loin, `sub_47fe86`, à suivre).

### 2026-07-09 — [ORACLE][LIFT] Différentiel par-instruction CÂBLÉ + corpus pile/frame → 4 bugs push/pop
- **Contexte** : question stratégique « combien de classes de miscompile général reste-t-il, mesurable en
  une fois ? ». Réponse mécanique : la **couche par-instruction** (`cpudiff::run`/`corpus`) existait mais
  **n'était câblée à aucun test** — seuls les tests niveau-fonction tournaient. Et le corpus échantillonnait
  l'arithmétique/logique/SSE, **pas les hotspots esp/frame** (là où vit une classe entière de bugs de
  composition, invisible aux tests étroits — cf. `push [esp+d]` trouvé via plink).
- **Fait** (`src/cpudiff.rs`) : (1) **câblé** le corpus par-instruction en test de régression
  (`per_instruction_corpus_matches_unicorn`, 4000 états/insn) ; (2) placé esp au **milieu de la page
  scratch** pour toute instruction modifiant esp (`stack_pointer_increment()!=0`) — sinon l'écriture pile
  tombait hors page → cas **silencieusement skippé** (toute la famille push/pop non testée) ; (3) étendu le
  corpus avec ~35 encodages **pile/frame** (push/pop reg/mem/imm, esp/ebp-relatif, 16-bit, leave, lea/mov
  [esp+d]).
- **Bugs trouvés PAR CONSTRUCTION et corrigés** (`src/ir/lift.rs`, tous généraux, tous faux-silencieux) :
  1. **`push`/`pop word` (16-bit)** : slot hardcodé `bits/8`=4 → esp décalé de 2 + store/load 4 o au lieu
     de 2. Fix : taille via `stack_pointer_increment()`.
  2. **`pop esp`** : l'incrément esp était appliqué après l'écriture (esp=[old]+4) alors que le pop dans esp
     **supersede** l'incrément (esp=[old]). Fix : pas d'incrément si dest = esp.
  3. **`pop [esp+d]`** : adresse dest calculée avec l'**ancien** esp ; x86 la calcule **après** l'incrément.
     Fix : snapshot valeur (ancien esp) → incrément → write (miroir de `push [esp+d]`).
  4. **`pop ax` (16-bit reg)** : `combine_write` court-circuitait `w>=bits` (16≥16) → écriture pleine →
     zéroïait [31:16] au lieu de les préserver. Fix : passer `bits`(32) à `write_op0` (le load reste 16-bit).
  - Progression du fuzzer : **106 → 63 → 42 → 21 → 0** divergences.
- **Portée** : c'est **la** réponse à « énumérer les classes restantes » — le corpus par-instruction câblé
  couvre l'espace i386 **par construction** (au lieu de subir binaire par binaire). Les hotspots restants
  (aliasing multi-sortie, séquences 2-3 insns) sont énumérables de la même façon (extension future).
- **Vérifié** : per-instruction corpus **0 divergence**, cpudiff suite 4/4, difftest 271/271, transpile-diff
  hash `19acad982194bf07` **inchangé** (push/pop 32-bit produisent une IR identique), funcdiff 0 divergence,
  winediff 49/49, busybox sweep 60/60, gauntlet 19/21. (plink : segfault résiduel plus profond, non lié.)

### 2026-07-10 — [ORACLE][LIFT] Couche différentielle de séquences → `push esp` post-décrément
- **Contexte** : suite directe de l'entrée 2026-07-09 (qui annonçait « séquences 2-3 insns énumérables de la
  même façon, extension future »). Une instruction juste **isolément** peut être fausse **en composition** :
  une insn décale esp, une suivante lit contre cet esp — classe de bugs qu'aucun test par-instruction
  n'atteint (c'est exactement ainsi que vivait `push [esp+d]`, trouvé par accident via plink).
- **Fait** (`src/cpudiff.rs`) : (1) `decode_at(bytes, addr)` — décode chaque insn à **son propre offset**
  pour que les ids de temp soient distincts (pas d'aliasing des temps scratch entre insns du bloc) ;
  (2) `diff_seq` — concatène les statements liftés d'une **séquence droite** (skip si Asm/Branch/Jump/Return),
  interprète vs Unicorn avec `count=n_insns`, esp mid-page + ebp quart-page → tout accès pile/frame tombe dans
  la page comparée ; compare regs+flags+page ; (3) `seq_corpus` — 12 séquences de composition de frame
  (`push imm;push [esp+8]`, `push ebp;mov ebp,esp;mov eax,[ebp+8]`, `sub esp,N;mov [esp+d],r;mov r,[esp+d]`,
  `push esp;pop eax`, `leave`, …) ; (4) câblé `sequence_corpus_matches_unicorn` (3000 états/séq).
- **Bug trouvé au 1ᵉʳ run et corrigé** (`src/ir/lift.rs`, général, faux-silencieux) : **`push esp`** poussait
  l'esp **post**-décrément (`push esp;pop eax` rendait `esp-4`). x86 286+ pousse l'esp **avant** baisse. Fix :
  `src_uses_sp` (déjà utilisé pour `push [esp+d]`) **élargi** au cas où la source est le **registre esp
  lui-même** → snapshot dans un temp avant `esp-=4`. Même principe, une classe close pour de bon.
- **Double trou de couverture fermé** : le per-instruction avait pourtant `push esp` (0x54) au corpus, mais
  `diff_one` ne comparait la page **que si l'instruction avait un opérande mémoire explicite**
  (`mem_base.is_some()`) — or un push écrit la pile via opérande **implicite** → son store n'était jamais
  comparé. Élargi à `mem_base.is_some() || stack_pointer_increment()!=0` : le per-instruction couvre
  désormais cette classe **indépendamment** de la couche séquences.
- **Vérifié** : `sequence_corpus_matches_unicorn` **0 divergence**, per-instruction 0 divergence, transpile-diff
  hash `19acad982194bf07` **inchangé**, difftest 271/271, funcdiff 0 divergence, winediff 49/49, busybox
  sweep 60/60, gauntlet 19/21. Commit `fea03f3`.

### 2026-07-10 — [ORACLE] Couche de séquences GÉNÉRATIVE (composition par construction, drapeaux indéfinis)
- **Contexte** : la couche de séquences (entrée précédente) était **curatée** (12 idiomes de frame écrits à
  la main) — pas encore la vraie méthode « par construction ». Objectif : composer **au hasard** des blocs
  2-3 insns pour échantillonner l'espace d'interaction lui-même, pas seulement les paires que j'imagine.
- **Fait** (`src/cpudiff.rs`) : (1) `seq_pool` — l'alphabet de composition (GP arith/logique/shift/mov/lea/
  xchg + toute la famille pile/frame + mémoire esp/ebp/edi-relative ; float/SSE exclus, n'interagissent pas
  avec esp) ; (2) `run_sequences_random(n, iters)` — compose au hasard len∈{2,3} insns du pool, `diff_seq`
  chacun ; (3) `diff_seq` sème aussi **edi (¾ page) + esi (⅝ page)** — les opérandes `[edi]`/`[esi]` du corpus
  tombent en page (scorées au lieu de fauter) ; (4) câblé `sequence_random_matches_unicorn` (4000 blocs × 150).
- **Piège clé résolu — drapeaux ARCHITECTURALEMENT INDÉFINIS** (52 faux positifs au 1ᵉʳ run) : `and/or/xor/
  test` laissent **AF indéfini** ; un shift multi-bit laisse **OF indéfini** (count≠1) **et AF indéfini**.
  `diff_seq` comparait `flags_written` de **l'union** du bloc → un flag posé par un `dec`/`add` précoce mais
  laissé indéfini par le `and`/`shl` final était diffé contre le résultat indéfini d'Unicorn (matériel réel) →
  fausse divergence. Ce n'est **pas** un bug de lift (l'IR ne modélise pas « indéfinit » ; le lift n'écrit
  simplement pas le flag). **Fix sound** : `cmp_flags` = flags écrits par la **dernière insn seulement** — les
  seuls définis en fin de bloc ; leurs entrées sont recalculées à l'identique par les 2 moteurs **sauf** vrai
  bug de composition (registres/mémoire), ce qu'on veut attraper. C'est exactement la règle per-instruction de
  `diff_one`, étendue au bloc. (Sur-conservateur si la dernière insn n'écrit aucun flag : perte de couverture
  flag, jamais de faux positif — acceptable, la cible de la couche est esp/registres/mémoire.)
- **Résultat** : après le fix, **4000 blocs aléatoires × 150 états = 0 divergence**. La couche de composition
  (ordonnancement SSA, snapshot esp, aliasing de base partagée) est **prouvée saine par construction** — verdict
  clair : le socle de justesse CPU est verrouillé aux niveaux instruction ET composition. Prochains hotspots
  éventuels : séquences plus longues / à branchement (couvertes par funcdiff), non prioritaires sans signal.
- **Portée** : round **cpudiff-only** (aucun code produit touché → hash transpile, difftest, funcdiff, winediff,
  sweeps inchangés par construction). Vérifié : `sequence_random_matches_unicorn` 0 div, per-instruction 0 div,
  `sequence_corpus` 0 div.

### 2026-07-10 — [X87][ORACLE] P2 MESURÉ : joins ambigus + transcendantales = filet SOUND, pas un feu correctness
- **Contexte** : « Poursuit » sur P2 (joins x87 ambigus, « la vraie difficulté récurrente »). Règle applicable :
  **« Mesurer, ne pas affirmer »** + **« Vérifier si le filet runtime est actif AVANT de conclure à un
  abandon x87 »**. Donc 1ᵉʳ pas = diagnostic, pas du code.
- **Mesure 1 — joins ambigus** : `ARET_X87_DEBUG=1` sur busybox montre le bail `ambiguous join depth (1 vs 0)`
  à `fn 0x428500 @0x429129` (= le join libm awk du doc) et `0x429c14`. **Mais** `busybox awk` exp/log/sqrt/
  `2^0.5`/sin/cos/atan2/`exp(log 5)`/`3^3`/`10^-2`/`log(exp 3)` = **tous bit-identiques à Wine**. Le bail tombe
  au filet `__x87rt_*`, correct. ⇒ **quality gap, pas correctness.**
- **Mesure 2 — « unmodelled x87 op »** (bail DOMINANT, plus fréquent que les joins) : `objdump` aux sites →
  ce sont les **transcendantales brutes** `fsin`/`fcos`/`fpatan`/`fldln2`/`fyl2x`. Fixture inline-asm minimale
  par op (`fldl;fsin;fstpl` etc., **non host-backable** car asm brut) : ARET rend **8/8 bit-identiques à Wine**
  (`fsin`/`fcos`/`fptan`/`fpatan`/`fyl2x`/`f2xm1`/`fscale`/`fsincos`). `ARET_X87_DEBUG` + grep du C émis
  confirment le chemin filet (`main` bail `unmodelled x87 op` → `__x87rt_2xm1()` dans `chunk_0.c`).
- **Conclusion (evidence-backed)** : **tout le x87 qui bail statiquement est correct via le filet runtime.**
  Il n'y a **pas** de feu x87 correctness. P2 (lifter statiquement au lieu du filet) est un gain de **qualité**
  (C plus propre/rapide), pas de justesse. Notre étoile = soundness, pas vitesse ⇒ **P2 déprioritisé** ; pas de
  session forensics sans un binaire qui échoue *réellement* (règle « pas de changement sans bénéfice mesuré en
  zone correctness-critique »).
- **Fait durable** : ajouté `bench/winecorpus/x87_transcendental.c` — le filet transcendantal n'était **gardé par
  aucun test** (les fixtures `mathfns.c`/`math_more.c` appellent la libm par nom = host-backée, ne touchent pas
  le lifter). Cette fixture force les instructions x87 brutes ⇒ garde le chemin `__x87rt_*` vs Wine. **winediff
  49/49 → 50/50.** Aucun code produit touché (fixture + docs) → hash transpile inchangé.

### 2026-07-10 — [DEMO][HLE-WIN32] Nouveaux binaires : sqldiff ✅ bit-identique ; sqlite3_analyzer → abort SOUND (Tcl notifier)
- **Contexte** : « tester un nouveau binaire » (meilleur révélateur de bugs généraux). curl.se et sqlite.org ne
  livrent plus de win32 récent → pris le bundle `sqlite-tools-win32-x86-3400100` (2022, même ère que notre
  sqlite3.exe) : **`sqldiff.exe`** (583 Ko, C pur) + **`sqlite3_analyzer.exe`** (2 Mo, embarque **Tcl**).
- **sqldiff = ✅ bit-identique à Wine** sur tous les modes testés : diff réel (schéma/update/delete/insert),
  `--schema`/`--summary`/`--table`/`--primarykey`, BLOB/NULL/int64/unicode, diff vide. Réutilise le moteur
  sqlite déjà maîtrisé → aucun bug. (Candidat gauntlet.)
- **sqlite3_analyzer = abort SOUND** (respecte le principe sacré, **jamais faux en silence**). Tcl initialise,
  exécute son script de démarrage, puis son **notifier** (boucle d'événements Windows) crée une fenêtre
  message-only via `RegisterClassW` → notre stub échoue → le code Tcl imprime lui-même « Unable to register
  TclNotifier window class » → `Tcl_Panic` → `ud2` → **abort bruyant**. Diagnostic précis : tout (lift CPU +
  CRT + Tcl) marche jusqu'au notifier ; **seul** le sous-système fenêtre-message USER32 manque.
- **Surface requise mesurée** (`--mode imports`) = jeu message-loop **borné** (~15) : `RegisterClassW`/
  `UnregisterClassW`, `CreateWindowExW`/`DestroyWindow`, `DefWindowProcW`, `GetMessageW`/`PeekMessageW`/
  `DispatchMessageW`/`TranslateMessage`, `PostMessageW`/`SendMessageW`/`PostQuitMessage`, `SetTimer`/`KillTimer`,
  `MsgWaitForMultipleObjectsEx`. **Une fenêtre message-only n'affiche RIEN** → sous-système implémentable
  **sound, sans graphisme/X11** (registre de classes + file de messages par thread + dispatch WNDPROC + timers).
  Débloquerait une **classe** (tout programme Tcl + tout usage de fenêtre cachée) = **1ᵉʳ pas concret de M7**,
  déclenché par un binaire mesuré (pas spéculatif). **Décision produit ouverte** (chantier neuf, ~15 fns).

### 2026-07-10 — [HLE-WIN32] Sous-système fenêtre message-only USER32 (débloque le notifier Tcl)
- **Contexte** : suite directe de la sonde nouveau-binaire — `sqlite3_analyzer` (Tcl) abortait sound sur
  `RegisterClassW` (notifier Tcl). Surface mesurée bornée (~15 fns message-loop), fenêtre message-only = **zéro
  pixel** ⇒ implémentable sound **sans X11/graphisme** (garde standalone + WASM). Décision : coder à la main
  (vs Winelib qui casse WASM/autonomie).
- **Fait** (`runtime/aret_hle/aret_win32.c`, ~180 l. additives) : registre de classes (nom large 16-bit →
  WNDPROC, ATOM `0xC000+i`), table de fenêtres (HWND = idx+1), **file de messages mono-thread** (ring), timers
  (horloge mono). Les 15 : `RegisterClassW`/`UnregisterClassW`, `CreateWindowExW`/`DestroyWindow`,
  `DefWindowProcW`, `GetMessageW`/`PeekMessageW`/`DispatchMessageW`/`TranslateMessage`, `PostMessageW`/
  `SendMessageW`, `PostQuitMessage`, `SetTimer`/`KillTimer`, `MsgWaitForMultipleObjectsEx`. **Clé** : le
  dispatch appelle le **WNDPROC dans le code lifté** via `aret_call`, frame stdcall posée **sous l'esp vif**
  (`(esp-64)&~15`, [esp+0]=retaddr, [esp+4..]=hwnd/msg/wParam/lParam) — **réentrant** (un WNDPROC peut
  SendMessage). `SendMessageW` = synchrone (appel direct) ; `PostMessageW`+`GetMessageW`+`DispatchMessageW` =
  file. `+14` entrées `stdcall_pops.rs` triées (`TranslateMessage` existait déjà).
- **Soundness** : `GetMessageW` sur file vide sans quit **ni timer** = **abort loud** (`aret_unimpl`,
  « would block forever ») — **jamais** un faux WM_QUIT (qui tronquerait la sortie = faux silencieux). Timers
  via horloge réelle (un batch qui ne boucle pas n'en déclenche aucun → déterministe).
- **Vérifié** : fixture `winecorpus/user32_msgwindow.c` (register/create/**Send synchrone**/**Post+Get+Dispatch**/
  Peek vide/**WM_QUIT+code**, le WNDPROC recalcule une valeur renvoyée à SendMessage = 4200+wParam) =
  **bit-identique à Wine**. winediff 50→**51/51**. Harness : `-luser32` ajouté (comme -lversion/-lole32,
  inoffensif). Hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, busybox sweep OK, table triée.
- **Effet analyzer** : **débloqué du notifier** (plus de `Tcl_Panic`/`Unable to register TclNotifier`). Avance
  puis révèle la suite **bornée** (tous abort sound) : `WSAStartup`/Winsock, `CreateEventW`, `wcschr`,
  `LoadLibraryW`, lift `repe cmpsb`. Tcl est un puits profond (multi-incréments) ; le sous-système message-only,
  lui, est **fini et général** (débloque toute la classe « fenêtre cachée / notifier »).

### 2026-07-10 — [ORACLE][INFRA] `--mode walls` : carte statique complète des murs de couverture
- **Contexte** : critique juste — dérouler les blocages **un par un au runtime** est inefficace s'il y en a
  des dizaines en série. Or le runtime ne suit qu'**un** chemin ; l'analyse statique voit **tout le code
  récupéré**. Donc on peut énumérer **tous** les murs de couverture d'un coup, sans exécuter.
- **Fait** (`src/builder/mod.rs`, `src/main.rs`) : nouveau `TranspileReport.unmodelled_insns`
  (`collect_unmodelled_insns` : parcourt toutes les fns, dédup les instructions opaques — `Stmt::Asm` **et**
  appels-expr `asm:` (mirroir de `has_opaque_asm`) — triées par nb de sites). Nouveau **`--mode walls`** :
  chemin rapide dans `transpile(walls_only=true)` qui **s'arrête après recovery+lift** (avant émission/
  compilation) et rend `render_walls()` : 3 sections — **instructions non liftées** (par sites), **imports
  manquants** (triés), **appels directs non résolus**. Même recovery+lift qu'un vrai transpile ⇒ carte
  **exacte**. Le rapport transpile normal montre désormais aussi le **top des instructions non liftées** (plus
  seulement un compteur) — intégration au pipeline de base.
- **Démonstration (sqlite3_analyzer)** : « 106 murs » → en réalité **14 instructions distinctes / 179 sites**
  dont **`repe cmpsb` ×149 = 1 fix** (`rep cmps`), `ud2` ×16 = **abort correct** (Tcl_Panic), le reste ×1-2 =
  data décodée comme code (non atteint) ; et **102 imports** dont **~29 = famille socket** (mappable POSIX
  d'un bloc, probablement jamais appelés — analyzer ne réseaute pas) + 6 wide-CRT triviaux. La « série de
  murs » s'effondre en **une poignée de familles bornées**. But suivant : agréger `--mode walls` sur un corpus
  de ~100 exe pour dégrossir par la donnée.
- **Portée honnête** : la carte couvre les murs **de couverture** (instructions/imports/récupération —
  statiquement énumérables), **pas** les bugs de **comportement** (miscompiles, indécidables) qui restent du
  ressort des oracles différentiels (cpudiff/funcdiff/winediff).
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, tests compilent. Robuste sur
  sqldiff (4 insns/5 imports/0 non résolu) et analyzer (34 Mo, rapide car pas de compilation).

### 2026-07-10 — [INFRA][ORACLE] 1er corpus externe réel : 41 exe Win95 (extraits ISO via HTTP-range) + dégrossissement
- **Méthode d'acquisition (réutilisable)** : archive.org est joignable. Les CD de shareware sont des **ISO**
  (600 Mo). Au lieu de tout télécharger : **lecture ISO-9660 par requêtes HTTP-range** (pycdlib sur un
  file-object `Range:`-backed, avec retry/backoff sur les 500 transitoires) → on ne récupère que le répertoire
  + les octets de chaque fichier voulu. `BestOfWindows95DotCom/WIN95_09964.iso` : 196 exe → classés par
  en-tête PE (`e_lfanew`→`PE`/`NE`/magic 0x10b) → **59 NE16 rejetés, 96 DOS/short rejetés, 41 PE32 gardés**.
  Zéro téléchargement du 600 Mo. (Il reste 3 ISO + 510 zips sur ce seul item = extension facile.)
- **`wallsweep` sur les 41** (binaires jamais vus, vieux MSVC/Borland/Watcom) : **0 crash de l'analyseur**
  (robustesse recovery) ; **0/41 clean** (tous ont des gaps). Le classement par **nb de binaires bloqués** est
  sans appel :
  - **La GUI EST le mur** (Win95 = apps desktop) : MessageBoxA 37, GetWindowRect/SetWindowPos/SetWindowTextA
    ~35, GetDlgItem 33, LoadStringA/SendMessageA 32, GetDC/ReleaseDC ~30, Find/Load/Sizeof/Lock/FreeResource
    ~26, RegisterClassA 26, dialogs (CreateDialogParamA/EndDialog/SetDlgItemTextA), GDI (GetDeviceCaps/
    GetStockObject). ⇒ **M7 confirmé par la donnée** : USER32/GDI32 **variantes A** + chargement de ressources
    + dialogs. NB : mon sous-système fenêtre est en **W** ; ces apps sont **ANSI (A)** → il faut les **siblings
    A** (RegisterClassA/CreateWindowExA/DefWindowProcA/Send/Peek/DispatchMessageA, 20-26 binaires, **peu cher**
    car la machinerie W existe déjà).
  - **Gains universels FACILES à forte largeur (manquants)** : **`GetVersion` (38/41)**, `DosDateTimeToFileTime`
    (28), `RtlMoveMemory`=memmove (20) — shims triviaux touchant beaucoup de binaires.
  - **Instruction #1** : `repe cmpsb` (**17/41**, confirmé aussi sur analyzer) = 1 fix lift `rep cmps`. Puis
    `std` (14, DF=1), `bt/bts` (7, formes mémoire/index exclues). Le reste (arpl/insd/outsb/in/bound/daa/aam/
    into/verw) = **privilégié/DOS = data décodée comme code** (bruit attendu, non atteint).
- **Enseignement** : le dégrossissement par corpus **marche** et **priorise objectivement**. Pour ce type
  (desktop Win95) le mur dominant est la GUI (gros chantier M7) ; en marge, 3-4 shims triviaux (`GetVersion`…)
  et `rep cmps` sont des gains transverses immédiats. Couplage oracles **obligatoire** : la carte dit *où*,
  chaque shim/lift devra passer winediff/cpudiff + régression avant d'être expédié.

### 2026-07-10 — [LIFT] `rep(ne) cmps` lifté (idiome memcmp/strcmp) — mur #1 du corpus abattu
- **Contexte** : premier mur descendu par la méthode corpus. `rep cmps` était le **#1 instruction** partout
  (analyzer **149 sites**, **17/41** Win95). C'est l'idiome `memcmp`/`strcmp` que les vieux MSVC/Borland
  inlinent (`repe cmpsb; je …`). Non lifté → memcmp = no-op asm opaque → toute comparaison « égale » (faux).
- **Fait** : (1) helpers runtime `__rep_cmps{8,16,32}(s,d,n,repe)` (emit/mod.rs, demand-load) — comparent n
  éléments [esi] vs [edi], `repe`(F3) s'arrête au 1er différent (memcmp), `repne`(F2) au 1er égal, renvoient le
  compte k ; (2) bloc de lift (lift.rs, calqué sur `rep scas`) : k dans un temp, puis esi/edi += k*taille,
  ecx -= k, **flags du dernier couple** comparé ([esi-taille] vs [edi-taille]) via `sub_flags`. F2 géré (iced
  ne le remonte pas via `has_rep_prefix`). DF=0 assumé (comme movs/scas) ; `std`/arrière = abort sound.
- **Oracle** : per-instruction cpudiff ne modélise pas les helpers value-returning (skip) → oracle **end-to-end**
  `winecorpus/str_repcmps.c` (inline-asm brut repe cmpsb equal/diff/reverse, repne cmpsw stop-on-match, repe
  cmpsd equal/differ + comptes) = **bit-identique à Wine**. (Même logique que rep_movsb_copy vérifie rep movs.)
- **Effet mesuré (re-sweep)** : analyzer **179 → 30 sites** (les 149 `repe cmpsb` disparus) ; Win95 : les 17
  binaires perdent ce mur. La boucle mesure→fix→re-mesure **confirmée** : le mur ciblé s'efface de la carte.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07` (les fixtures n'utilisent pas rep cmps),
  difftest 271/271, winediff **52/52** (nouvelle fixture), cpudiff inchangé.

### 2026-07-10 — [HLE-WIN32] Grappe de shims à forte largeur (GetVersion/DosDateTimeToFileTime/RtlMoveMemory)
- **Contexte** : suite du dégrossissement corpus — 3 imports manquants à **forte largeur** sur les 41 Win95 :
  `GetVersion` **38/41**, `DosDateTimeToFileTime` 28, `RtlMoveMemory` 20. Petits, sound, transverses.
- **Fait** (`aret_win32.c`) : (1) **`GetVersion`** = forme packée héritée **cohérente avec `GetVersionEx`**
  (6.2.9200 NT) : `6|2<<8|9200<<16` = 0x23F00206 (LOBYTE major, HIBYTE minor, HIWORD build, bit31=0 NT) ;
  (2) **`RtlMoveMemory`** = `memmove` (overlap-safe) ; (3) **`DosDateTimeToFileTime`** = date/heure FAT packée →
  FILETIME (100ns depuis 1601), calcul **jours civils portable** (algo Hinnant, pas de `timegm` → marche aussi
  wasi), sans décalage TZ (canonique = Wine). +3 entrées `stdcall_pops` triées (GetVersion = 0 arg).
- **Oracle** : `winecorpus/win32_version_dostime.c`. `GetVersion` : valeur = version OS **définie par
  l'environnement** (non comparable en brut) → on teste l'**invariant stable** (packé cohérent avec
  GetVersionEx + NT + major plausible), vrai sous Wine comme Windows réel. `DosDateTimeToFileTime` : conversion
  **déterministe** → FILETIME exact `01c7661ae6295600` (bit-identique Wine). `RtlMoveMemory` : import explicite
  (windows.h le macro-expanse sinon en memmove) + move chevauchant. **Bit-identique à Wine.**
- **Effet mesuré (re-sweep)** : les 3 imports **éliminés des 41 Win95** (GetVersion touchait 38/41).
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, winediff **53/53**, table triée.

### 2026-07-10 — [LIFT] `bt/bts/btr/btc [mem], reg` (idiome bit-array, offset registre non masqué)
- **Contexte** : nouveau top instruction Win95 après rep cmps. La forme `bt [mem], reg` (offset de bit dans un
  **registre**, base mémoire) était exclue → asm. Sémantique délicate : l'offset **n'est pas masqué** à la
  largeur ; il **décale l'adresse** (élément = `base + SAR(idx, log2 w)*(w/8)`, bit testé = `idx & (w-1)`) —
  l'idiome tableau-de-bits `bt [arr], eax`.
- **Fait** (`lift.rs`) : nouveau bras (avant celui reg/imm8) pour op0=Memory ∧ op1=Register. Adresse ajustée
  via `mem_addr` + byteoff signé. **idx sign-étendu par `shl/sar`** (pas `SignExtend` : l'interp cpudiff le
  skippe) avant l'arithmétique. Adresse snapshotée dans un temp (CF read + RMW store cohérents). BT = CF seul ;
  bts/btr/btc = load|set/clear/toggle → store. Base `fs:/gs:` = abort sound (`mem_addr` renvoie None).
- **Oracle** : cpudiff ne peut pas tester (idx aléatoire 32-bit → adresse hors page scratch → skip). Donc
  **winediff** `bt_mem_reg.c` (inline-asm bt/bts/btr/btc, offsets 0/31/32/63/64/79/96/127 traversant les dwords,
  RMW + dump tableau) = **bit-identique à Wine**. cpudiff per-instruction (forme registre) **inchangé/vert**.
- **Effet mesuré** : `bt`/`bts` (formes mémoire) **éliminés des 41 Win95**.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, cpudiff vert, winediff **54/54**.

### 2026-07-10 — [LIFT] Drapeau de direction (DF) : `std`/`cld` + chaîne bidirectionnelle
- **Contexte** : top instruction Win95 après les précédents = **`std`** (14 binaires). Mesure des sites :
  vrai usage backward — `std;repne scasb` (strrchr) et `std;…;rep movs` (memmove overlap). `std` était abort,
  DF=0 assumé partout. **Tout-ou-rien** : modéliser DF partiellement = **unsound** (std→DF=1 mais une op qui
  ignore DF irait en avant = corruption mémoire silencieuse).
- **Fait** : `FlagKind::Df` (EFLAGS bit 10) ajouté (matches génériques → 0 ripple ; `flag_bit` cpudiff = 10).
  `cld`→DF=0, `std`→DF=1 (plus des no-op/abort). **Chaque op de chaîne lit DF** : non-rep avance de pas
  **signé** `size*(1-2·DF)` ; rep movs/stos/scas/cmps → helpers `__rep_*` prennent un flag **`back`** (0 avant,
  1 arrière) et walk `p±=1` ; registres avancés du total signé ; dernier élément (scas/cmps) à `reg−step`.
  `rep movs` : passe de `memcpy` à `__rep_movs{8,16,32,64}` (memcpy = avant seulement). cpudiff `do_memcall` +
  `is_modeled_memcall` mis à jour (movs/stos 4 args ; back≠0 → skip, testé end-to-end). **DF=0 à l'entrée** :
  `value_decls` initialise tout SSA à 0 → un DF lu-avant-écrit = 0 = convention ABI ⇒ code sans `cld` = avant.
- **Oracle** : `winecorpus/str_direction.c` — inline-asm des 2 sens (backward movsb overlap, stosb, repne scasb
  strrchr, repe cmpsb, non-rep lods/stos) = **bit-identique à Wine**. (Piège attrapé : bug de fixture double-
  offset écrivant hors zone → UB pile non-init divergente ; corrigé, pas un bug de lift.)
- **Effet mesuré** : `std` + toutes les ops de chaîne **éliminés des 41 Win95**.
- **Vérifié (gros changement, tout passé)** : hash transpile **inchangé** `19acad982194bf07` (le corpus des 58
  fns ne change pas : DF folde à 0 en avant), difftest 271/271, **busybox sweep bit-identique** (usage chaîne
  intensif en avant intact), **cpudiff 6/6** (per-instruction non-rep + memcall `__rep_movs`/`__rep_stos` +
  funcdiff), winediff **55/55**.

### 2026-07-10 — [ORACLE][HLE-WIN32] RtlUnwind = FROID (mesuré) + grappe file/process (dernier non-GUI facile)
- **B — RtlUnwind chaud/froid ?** Mesuré (règle « vérifier si le fallback suffit avant de conclure »). C'est le
  **dérouleur SEH** (control-flow non-local + chaîne `fs:[0]` + callbacks) — **pas** un shim facile. Preuve
  froid : nos démos MSVC qui tournent **bit-identiques à Wine** (sqlite3/nasm/sqldiff) référencent RtlUnwind
  **0 fois** en code atteignable ; sur Win95 il n'est appelé que depuis la plomberie `_global_unwind2`/CRT,
  invoquée **seulement quand une exception se propage**. ⇒ **abort sound suffit** ; RtlUnwind ira au tier EH
  **avec** la GUI/C++ exceptions, pas en standalone. *(Le vrai « dernier facile » = les petits shims ci-dessous,
  pas RtlUnwind — correction de mon cadrage initial.)*
- **A — grappe file/process** (`aret_win32.c` + `aret_hle.c`) : (1) **`GetExitCodeProcess`** → STILL_ACTIVE
  (259) (pas de process enfant créé) ; (2) **`GetDiskFreeSpaceA`** → `statvfs` + géométrie fixe (512 o/secteur,
  8 secteurs/cluster) ; (3) **`SetFileAttributesA/W`** → seul FILE_ATTRIBUTE_READONLY est POSIX-mappable
  (bits d'écriture via `chmod`, `translate_path` dans aret_hle.c) ; autres attrs acceptés-ignorés (= Wine).
  +2 `stdcall_pops` triés (GetDiskFreeSpaceA=20, SetFileAttributesW=8).
- **Oracle** : `winecorpus/win32_file_process.c`. Valeurs dépendantes de l'hôte (tailles disque) testées par
  **invariant** (bps puissance de 2, spc≥1, total≥free) et non en brut ; GetExitCodeProcess sur le process
  courant = STILL_ACTIVE déterministe ; SetFileAttributes **round-trip** via GetFileAttributes (READONLY ↔ bit
  d'écriture). **Bit-identique à Wine.**
- **Effet mesuré** : les 3 imports **éliminés des 41 Win95**.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, winediff **56/56**, table triée.
- **État corpus** : après cette grappe, le **top imports Win95 est ~100% GUI** (USER32-A/GDI/ressources/dialogs)
  et le top instructions = **bruit** (privilégié/DOS misdécodé). ⇒ « descendre à zéro » = **chantier GUI (M7)**.

### 2026-07-10 — [GUI] M7 — plan (doc 72) + G1 : jumeaux ANSI du modèle fenêtre/message
- **Plan doc 72** : chantier GUI USER32/GDI via **SDL2** (portable Linux/macOS/WASM, pas X11), incréments
  **G1→G7** chacun avec son oracle Wine (contenu, pas pixels ; SDL headless ; framebuffer-hash pour GDI ;
  dialogs auto-répondus). Fenêtre message-only reste sans graphisme. EH (RtlUnwind/C++) rangé dans ce tier.
- **G1 fait** (`aret_win32.c`) : **jumeaux A** du sous-système message-only (W déjà fait). Cœurs partagés
  extraits (`u32_class_register`, `u32_window_create`, `u32_a2w`) : `RegisterClassA`/`CreateWindowExA` widen
  le nom de classe narrow→wide et **partagent le registre W unique** (Windows partage la table d'atomes) ;
  `DefWindowProcA`/`GetMessageA`/`PeekMessageA`/`DispatchMessageA`/`PostMessageA`/`SendMessageA` = **forwarders
  vers W** (identiques pour message-only : pas de texte/WM_CHAR à cette couche) ; `UnregisterClassA`. +7
  `stdcall_pops` A triés.
- **Oracle** : `winecorpus/user32_msgwindow_a.c` (round-trip A : register/create/Send synchrone/Post+Get+
  Dispatch/Peek vide/WM_QUIT via les APIs **A**) = **bit-identique à Wine** (send=8400+wParam via le WNDPROC).
- **Effet mesuré** : les imports fenêtre/message **A** (RegisterClassA 26, SendMessageA 32, DefWindowProcA 24,
  DispatchMessageA 24, PeekMessageA 22, CreateWindowExA 20, …) **éliminés des 41 Win95**. *(Les binaires ne
  tournent pas encore — il leur faut G2 fenêtres visibles + GDI + ressources + dialogs.)*
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, winediff **57/57**, table triée.

### 2026-07-10 — [GUI] M7 — G2a : modèle fenêtre étendu (géométrie / état / texte), display-free
- **G2a fait** (`aret_win32.c`) : la table de fenêtres passe du simple sink message-only à un vrai **état de
  window manager** — `x,y,w,h`, `style/exstyle`, `visible/enabled`, `userdata` (GWL_USERDATA), `title[256]`.
  `u32_window_create` capture la géométrie/style/texte de `CreateWindowEx(A/W)` (helper `u32_coord` pour
  CW_USEDEFAULT) ; les deux appelants A/W passent désormais les 9 args (title widené narrow via `u32_w2n`).
- **APIs (18)** : `GetWindowRect` (= `{x,y,x+w,y+h}`, Wine renvoie exactement la géométrie CreateWindow),
  `SetWindowPos` (respecte SWP_NOMOVE/NOSIZE/SHOW/HIDE), `MoveWindow`, `ShowWindow` (renvoie l'ancienne
  visibilité), `UpdateWindow` (no-op sound : pas de région invalide tant que GDI paint = G6), `EnableWindow`
  (renvoie l'ancien état **disabled**), `GetParent`, `GetDesktopWindow`/`IsWindow`/`IsWindowVisible`/
  `IsWindowEnabled`/`IsIconic`, `Get/SetWindowLongA/W` (STYLE/EXSTYLE/USERDATA/WNDPROC=subclassing),
  `GetSystemMetrics`. **Texte** : `Set/GetWindowTextA/W` + `GetWindowTextLengthA/W` **routés via
  WM_SETTEXT/WM_GETTEXT/WM_GETTEXTLENGTH** → le WNDPROC lifté les voit, `DefWindowProcA/W` stocke/rapporte
  (vrai chemin Windows ; un subclass les intercepte comme sous Wine). +20 `stdcall_pops` triés.
- **Sound sur l'env-dépendant** (règle « invariant, pas valeur brute ») : `GetSystemMetrics` renvoie les
  métriques fixes classiques + une taille d'écran virtuel **définie** (1024×768, comme les valeurs host de
  GetDiskFreeSpace) ; `GetDesktopWindow`/`GetWindowRect(desktop)` = l'écran virtuel. Ces valeurs brutes sont
  testées **par invariant** (>0), jamais bit-comparées à Wine (headless-dummy vs Xvfb 1280×1024).
- **Oracle** : `winecorpus/user32_windowstate_a.c` (WS_POPUP → pas de clamping WM) : rect après create /
  SetWindowPos move-only / size-only / MoveWindow, show+hide, enable+disable, GWL_USERDATA round-trip, texte
  round-trip via le callback, parent/IsWindow/desktop/metrics par invariant = **bit-identique à Wine** headless
  (Xvfb). Infra : `winediff.sh` démarre un Xvfb pour le run + lie `-lgdi32` ; `session-start.sh` installe Xvfb.
- **Effet mesuré** : GetWindowRect 35, SetWindowPos 34, ShowWindow 21, GetSystemMetrics 17, GetDesktopWindow 21
  (+ Set/GetWindowText, GetWindowLong…) **éliminés des 41 Win95**. *(Les binaires ne tournent toujours pas :
  reste G2b fenêtre SDL visible + pompe SDL_PollEvent, puis GDI/ressources/dialogs.)*
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, winediff **58/58**, cargo test
  (hors 1 échec **pré-existant** non lié : `atexit_callback_after_noreturn` a des `fld/fstp qword` x87 non
  modélisés — le programme tourne pourtant jusqu'à `cleanup 1` ; l'assertion trébuche sur le warning honnête
  « unmodelled »), table triée.

### 2026-07-10 — [X87][RECOV] x87 statique = tout-ou-rien (bloc absorbé non atteint → filet runtime, pas abort)
- **Symptôme** : `atexit_callback_after_noreturn.exe` (régression pré-existante sur la branche) tournait
  correctement (`work 1`/`cleanup 1`) **mais** émettait 6 `aret_unmodelled("fld/fstp qword…")`. Le programme
  ne les atteint pas, mais la garde `!contains("unmodelled")` du test trébuchait — et surtout, atteints par
  appel indirect (helper de formatage float), ils **aborteraient à tort**.
- **Cause racine** (`ir/build.rs:x87_depth_pass`) : ces `fld/fstp m64`/`fstp st(0)`/`fxch` appartiennent à une
  **fonction distincte absorbée** (`0x401c90`, prologue `push ebp` propre) dans `sub_4019a0` par le balayage
  linéaire **après un `call` noreturn** — **sans arête CFG** vers elle. La passe de profondeur ne visite jamais
  son bloc (`entry_sp=-1`) → ses ops ne sont **pas mappées**. La fonction restant statiquement modélisée
  (`x87 = Some`), le filet runtime (`x87_rt`) **ne s'active pas** (il exige que **toute** la fonction bail), donc
  ces ops tombent sur `lift()` → `Asm`/`aret_unmodelled`. Un repli **par-op** vers le filet serait **unsound** :
  les slots statiques `fpr()` et la pile runtime `__x87rt_s[]` sont deux représentations qui **ne se mélangent
  pas** dans un même corps.
- **Fix (général, sound)** : `x87_depth_pass` **bail la fonction entière** (→ filet runtime) si un bloc **émis
  non atteint** (`entry_sp=-1`) porte des ops x87. **Tout-ou-rien** : soit tout statique (toutes ops mappées),
  soit tout le filet runtime — jamais un mélange, jamais un abort sur une op modélisable. Les 6 ops passent en
  `__x87rt_ld64`/`st64`/`xch`/`sti`.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, **cargo test complet vert**,
  **busybox 60/60** bit-identique (x87 dense), **funcdiff 0 divergence** (12467 lift/10688 opt scorées),
  winediff 58/58. Test `atexit_…_recovered` **repasse**.

### 2026-07-10 — [INFRA][RECOMPILE] Build WASM cassé par `<sys/statvfs.h>` (GetDiskFreeSpaceA) — guardé
- **Symptôme** : `windows_pe_to_webassembly` (régression pré-existante) échouait — `aret --target wasm` sur
  `hello_win32.exe` : `unknown type name '__BEGIN_DECLS'` dans `/usr/include/sys/statvfs.h`. clang wasm32 ne
  compile pas l'en-tête statvfs de l'hôte.
- **Cause racine** : `aret_win32.c` incluait **inconditionnellement** `<sys/statvfs.h>` (ajouté avec
  `GetDiskFreeSpaceA`, grappe file/process) — or wasm32-wasi n'a pas de `statvfs` de système de fichiers.
- **Fix (général)** : include + usage `statvfs` guardés `#ifndef __wasm__` (comme le mmap, doc 70 §4.5).
  Sur wasm, `GetDiskFreeSpaceA` **renvoie FALSE** (échec) plutôt qu'une géométrie fabriquée — **sound** (pas de
  taille disque fausse en silence ; wasm n'a pas de vrai FS).
- **Vérifié** : `windows_pe_to_webassembly` **repasse** (WASM 7/7 réellement vert), build natif -m32 inchangé,
  toutes les autres portes vertes (cf. entrée x87 ci-dessus).

### 2026-07-10 — [GUI][HLE-WIN32] M7 — G4 (ressources .rsrc) priorisé par la donnée AVANT G2b (SDL)
- **Re-mesure corpus (wallsweep 41 Win95, après G2a)** : top instructions = **bruit** (popad/arpl/insd/outsb/
  bound/daa = opcodes privilégiés/DOS misdécodés en data-as-code). Top imports = **100 % GUI**, mais dominés par
  du **display-free** : MessageBoxA 37, GetDlgItem 33, **LoadStringA 32**, ReleaseDC 30, GetDeviceCaps 28,
  **FindResourceA/LoadResource/SizeofResource 26**, **LockResource 24**, **FreeResource 25**, LoadIcon/Cursor 19/23.
- **Décision (règle « prioriser par la donnée, pas l'intuition »)** : faire **G4 (ressources + LoadString)**
  **avant** G2b (fenêtre SDL). Motifs : (1) débloque **plus** de binaires ; (2) **display-free** → pas de
  dépendance SDL, portable/WASM ; (3) **oracle exact** (valeurs de chaînes / octets de blob vs Wine), là où G2b
  a un oracle dur (la tempête de messages Windows à `CreateWindow` n'est **pas** bit-reproductible) ; (4) la
  `.rsrc` est **déjà mappée**.
- **Découverte réutilisable** : les **en-têtes PE sont déjà mappés** à l'image base (`.pe_header`, loader), donc
  le runtime lit `DataDirectory[2]` (resource RVA) **depuis la mémoire** — **0 changement loader/builder**, tout
  reste dans `aret_win32.c`.
- **Fait** (`aret_win32.c`, +6 `stdcall_pops` triés) : walker de l'arbre `IMAGE_RESOURCE_DIRECTORY` en mémoire
  (`u32_rsrc_root` parse MZ→PE→PE32→DataDirectory[2] ; `u32_rsrc_entry` matche id **ou** nom UTF-16 casse-
  insensible ; `u32_rsrc_data_entry` descend type→nom→langue[0]→DATA_ENTRY). `FindResourceA`(HRSRC=ptr
  DATA_ENTRY), `LoadResource`(image_base+RVA), `LockResource`(identité), `SizeofResource`(Size), `FreeResource`
  (no-op=0). **`LoadStringA`** : RT_STRING groupées 16/bloc (bloc=id/16+1, index=id%16 ; entrée = WORD longueur
  WCHAR + WCHARs sans NUL) → narrow ANSI, tronque à cch-1. Ressource/chaîne absente → NULL/0 (**sound**, jamais
  fabriqué).
- **Oracle** : `winecorpus/user32_resources.{c,rc}` (windres compile le .rc, harness le lie) : blob RT_RCDATA
  (octets exacts `41524554341200004200`), table de chaînes sur **2 blocs** (id 277→bloc 18), ressource absente,
  id absent, **troncature** (buf 8 → "Hello, ") — **bit-identique à Wine** (les deux lisent les **mêmes octets**
  embarqués). Les en-têtes PE mappés confirmés utilisables.
- **Effet mesuré** : LoadStringA 32, FindResourceA/LoadResource/SizeofResource 26, LockResource 24, FreeResource
  25 **éliminés des 41 Win95**. *(Les binaires ne tournent pas encore : reste dialogs/MessageBox/GDI + fenêtre
  visible.)*
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, cargo test complet vert,
  winediff **59/59**, table triée.

### 2026-07-10 — [GUI][HLE-WIN32] M7 — G5a : MessageBoxA (repli display-free sound, oracle Wine-sans-écran)
- **Mesure (règle « mesurer, pas affirmer »)** : `MessageBoxA` (mur #1, **37 binaires**) est modal → bloque en
  attendant un clic ; sous Wine avec Xvfb il **hangerait**. Probe empirique : **Wine, `DISPLAY` non défini**,
  `MessageBoxA` renvoie **-1 (0xFFFFFFFF) immédiatement, sans bloquer** (MB_OK comme MB_OKCANCEL).
- **Fix (sound)** : `aret_MessageBoxA/W/ExA/ExW` → **-1** dans le tier display-free. C'est la réponse honnête
  « pas d'écran disponible » (pas un bouton **deviné**) : un programme qui ignore le résultat continue, un qui
  le teste voit le **même** échec que sous Wine headless. Un vrai dialogue (`SDL_ShowSimpleMessageBox`) le
  remplacera avec l'écran visible (G2b). +4 `stdcall_pops` triés.
- **Infra oracle** : `winediff.sh` gagne un marqueur **`NAME.nodisplay`** → lance **les deux** moteurs avec
  `env -u DISPLAY` (Wine prend son chemin sans-écran déterministe au lieu de bloquer ; les fixtures fenêtrées
  omettent le marqueur et gardent Xvfb). Réutilisable pour toute API display-sensible.
- **Oracle** : `winecorpus/user32_messagebox.c` (+`.nodisplay`) : MB_OK/MB_OKCANCEL/MB_YESNO → tous **-1**,
  **bit-identique à Wine-sans-écran**.
- **Effet mesuré** : `MessageBoxA` (37) éliminé des 41 Win95 (repli sound ; les programmes avancent au lieu
  d'aborter).
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, cargo test complet vert,
  winediff **60/60**, table triée.
- **Reste G5** : dialogs (DialogBoxParamA/CreateDialogParamA parsent DLGTEMPLATE → contrôles enfants + pompe
  modale ; EndDialog ; GetDlgItem/Set-GetDlgItemTextA) — oracle-able sous Xvfb (dlgproc EndDialog sur
  WM_INITDIALOG = déterministe). Prochain incrément.

### 2026-07-10 — [GUI][HLE-WIN32] M7 — G5b : dialogs (DLGTEMPLATE → contrôles + pompe modale), display-free
- **Fait** (`aret_win32.c`, +15 `stdcall_pops` triés) : un **dialogue = une fenêtre** (wndproc = le DLGPROC de
  l'appli) dont les **contrôles enfants** sont des fenêtres (chacune avec son `ctrl_id` + le texte du template ;
  wndproc 0 = contrôle système, texte servi nativement). Ajout du champ `ctrl_id` à `g_u32_win`.
  - **Parseur DLGTEMPLATE + DLGTEMPLATEEX** (`u32_dialog_create`) : détecte l'EX (dlgVer=1,sig=0xFFFF), lit
    style/cdit, saute menu/classe/titre (`u32_dt_szord` : 0 vide / 0xFFFF+ordinal / chaîne WCHAR), le bloc font
    si DS_SETFONT, puis chaque contrôle **DWORD-aligné** (id WORD en classic / DWORD en EX, classe, titre,
    creation-data) → crée le contrôle enfant (`u32_new_control`).
  - **`DialogBoxParamA/W`** (modal) : crée, envoie **WM_INITDIALOG** au DLGPROC via `aret_call`, **pompe modale**
    jusqu'à `EndDialog` ; un DLGPROC qui `EndDialog` pendant WM_INITDIALOG (cas scriptable/headless) rend
    aussitôt. Sinon rien à pomper headless → **abort bruyant** (jamais hang ni résultat inventé). `EndDialog`,
    `CreateDialogParamA/W` (modeless), `GetDlgItem`, `GetDlgCtrlID`, `Set`/`GetDlgItemTextA/W`,
    `Set`/`GetDlgItemInt`, `SendDlgItemMessageA/W` (contrôle système → texte via `u32_defproc_text`).
- **Oracle** : `winecorpus/user32_dialog.{c,rc}` (DIALOG classique + LTEXT/EDITTEXT/DEFPUSHBUTTON) : le DLGPROC
  lit le texte template du label, round-trip texte+entier de l'edit, `GetDlgItem`/`GetDlgCtrlID`, `EndDialog(77)`
  → **bit-identique à Wine** (sous Xvfb, se termine sans interaction). Prouve : parsing template, création des
  contrôles avec texte/id, **callback WM_INITDIALOG dans le lifté**, retour modal.
- **Effet mesuré** : DialogBoxParamA 17, CreateDialogParamA 23, EndDialog 20, GetDlgItem 33, SetDlgItemTextA 19,
  GetDlgItemTextA 12, SendDlgItemMessageA 16 **éliminés des 41 Win95**.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, cargo test complet vert,
  winediff **61/61**, table triée.

### 2026-07-10 — [GUI][HLE-WIN32] M7 — G6a : GDI (modèle objet/DC + dessin DIB bit-exact)
- **Mesure préalable (règle « mesurer, pas affirmer »)** : probe Wine → un pixel DIB 32bpp BI_RGB = **`[B,G,R,0]`**,
  `FillRect`/`SetPixel`/`GetPixel` déterministes. ⇒ dessin dans un **DIB mémoire qu'on possède** = reproductible
  **bit-à-bit** (aucun rasteriseur/police de l'hôte). C'est la stratégie oracle de la GDI (doc 72 §4.3 : hash du
  framebuffer).
- **Fait** (`aret_win32.c`, +29 `stdcall_pops` triés) : table d'objets GDI unifiée (DC/bitmap/brush/pen/font,
  handles opaques base 0x30000000). DC screen/window **sans surface** (pas d'écran → dessin no-op sound) ; la
  **DIB section** (32bpp, buffer `calloc` exposé via ppvBits) est la seule cible **vérifiée pixel**.
  - Cycle DC : `GetDC`/`GetWindowDC`/`ReleaseDC`/`CreateCompatibleDC`/`DeleteDC`/`BeginPaint`/`EndPaint`/`GdiFlush`.
  - Bitmaps : `CreateDIBSection` (32bpp ; autres profondeurs = **abort sound**), `CreateCompatibleBitmap`.
  - Objets : `CreateSolidBrush`/`CreatePen`/`GetStockObject`/`GetSysColorBrush`/`SelectObject`/`DeleteObject`.
  - Dessin **bit-exact** : `SetPixel`/`SetPixelV`/`GetPixel`/`FillRect`/`PatBlt` (PATCOPY/BLACKNESS/WHITENESS)/
    `BitBlt` (SRCCOPY ; autres ROPs abort sound). Format COLORREF↔`[B,G,R,0]`, top-down/bottom-up gérés.
  - Attributs : `SetTextColor`/`SetBkColor`/`SetBkMode`/`Get*` ; `GetSysColor` (schéma Win32 classique) ;
    `GetDeviceCaps` (métriques de classe exactes ; extents écran = desktop virtuel, testé par invariant).
- **Bug attrapé par l'oracle** (rule 10) : `GetDeviceCaps` avait **BITSPIXEL(12) et PLANES(14) inversés** →
  `bpp>=8` divergeait (0 vs 1). Corrigé (12→32, 14→1).
- **Hors périmètre (sound)** : `TextOut` (raster police ≠ Wine bit-à-bit) et `Rectangle`/`LineTo` (règles de bord
  du stylo) → **pas modélisés** ; un sous-ensemble mesuré viendra si un binaire l'exige (doc 72 §5).
- **Oracle** : `winecorpus/gdi_dib.c` : DIB 8×8, FillRect fond + sous-rect, SetPixel/SetPixelV, PatBlt
  BLACKNESS + PATCOPY → **hash du buffer `a182d45a`** + GetPixel + stock objets distincts + GetDeviceCaps par
  invariant = **bit-identique à Wine** (sous Xvfb).
- **Effet mesuré** : ReleaseDC 30, GetDeviceCaps 28, GetDC 27, GetStockObject 22, SelectObject 14, DeleteObject
  12, GetSysColor 12, BeginPaint/EndPaint 11, CreateSolidBrush 11 **éliminés des 41 Win95**.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, cargo test complet vert,
  winediff **62/62**, table triée.

### 2026-07-10 — [GUI][HLE-WIN32] M7 — G7 (1) : batch helpers fenêtre (long-tail, mesuré vs Wine)
- **Contexte (re-sweep post-G6)** : les gros blocs GUI franchis ; le front devient un **long-tail de petits
  helpers**. Batch #1 = les plus sound/oracle-able.
- **Mesuré d'abord (règle 9)** : probe Wine (Xvfb) → `GetClientRect`(WS_POPUP)=`{0,0,w,h}`,
  `AdjustWindowRect`(WS_POPUP) inchangé, `SetForegroundWindow`/`InvalidateRect`/`MessageBeep`=1,
  `CallWindowProcA`(WM_NULL→DefWindowProc)=0, `LoadCursorA`/`LoadIconA`≠NULL, `MsgWaitForMultipleObjects`
  (0,NULL,…)=258 (WAIT_TIMEOUT).
- **Fait** (`aret_win32.c`, +18 `stdcall_pops` triés) : `GetClientRect` ({0,0,w,h} — pas de non-client dans le
  modèle, exact pour borderless), `AdjustWindowRect(Ex)` (rect inchangé, no-NC), `SetFocus`/`GetFocus`/
  `SetActiveWindow`/`GetActiveWindow`/`SetForegroundWindow`/`GetForegroundWindow`/`BringWindowToTop` (focus/
  activation suivis, pas de vrai focus), `InvalidateRect`/`InvalidateRgn`/`ValidateRect`/`ValidateRgn` (no-op
  sound = rien à repeindre headless), `MessageBeep` (pas d'audio → succès), `CallWindowProcA/W` (appelle un
  wndproc lifté via `aret_call` — idiome subclass), `LoadCursorA/W`/`LoadIconA/W` (handle opaque non-null
  distinct par nom ; jamais déréférencé headless), `MsgWaitForMultipleObjects` (= logique de l'Ex).
- **Oracle** : `winecorpus/user32_helpers.c` — **bit-identique à Wine** (sous Xvfb) sur les 8 valeurs mesurées.
- **Effet mesuré** : GetClientRect 15, MsgWaitForMultipleObjects 15, SetForegroundWindow 13, CallWindowProcA 12,
  AdjustWindowRect 11, InvalidateRect 11, MessageBeep 10, LoadCursorA 23, LoadIconA 19 **éliminés des 41 Win95**.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, cargo test complet vert,
  winediff **63/63**, table triée.

### 2026-07-10 — [HLE-FILE] Win16 file API (_lopen/_lcreat/_lclose/_lread/_lwrite/_llseek) + lstrcpynA
- **Contexte** : long-tail post-GUI — les vieilles APIs fichier Win16 (`_lopen` 7, `_lclose`/`_lcreat` ~10)
  reviennent dans le corpus Win95. Pur POSIX-mappable, oracle propre (round-trip fichier).
- **Fait** (`aret_hle.c`, +9 `stdcall_pops`) : un HFILE = **fd POSIX** (modèle handle existant), donc `_lopen`
  (OF_READ/WRITE/READWRITE → O_RDONLY/WRONLY/RDWR), `_lcreat` (O_RDWR|CREAT|TRUNC, bit READONLY→mode 0444),
  `_lclose`/`_lread`/`_lwrite`/`_hread`/`_hwrite`/`_llseek` (SEEK_SET/CUR/END) mappent direct sur open/read/
  write/lseek/close, partageant `translate_path`. HFILE_ERROR=-1. + `lstrcpynA` (copie ≤ n-1 + NUL). Clés pop
  sans underscore de tête (`stdcall_pop_bytes` strip un `_` : `_lopen`→`lopen`).
- **Oracle** : `winecorpus/win16_file.c` — round-trip create/write/reopen/seek(SET & END)/read + lstrcpynA
  troncature → **bit-identique à Wine**.
- **Effet mesuré** : `_lopen`/`_lclose`/`_lcreat`/`lstrcpynA` éliminés des 41 Win95.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, cargo test complet vert,
  winediff **64/64**, table triée.

### 2026-07-10 — [GUI][HLE-WIN32] M7 — G7 (2) : sous-système menus (modèle données, display-free)
- **Mesuré d'abord (règle 9)** : probe Wine (sans écran) → `GetMenuState` renvoie les flags MF_* de l'item
  (CHECKED=0x08, GRAYED=0x01), `EnableMenuItem`/`CheckMenuItem` renvoient l'**ancien** état, `GetMenuString`
  le texte, `GetMenuState(pos,BYPOSITION)` l'item à la position.
- **Fait** (`aret_win32.c`, +19 `stdcall_pops` triés) : un menu = liste d'items (id, flags, submenu, texte),
  handles base 0x40000000. `CreateMenu`/`CreatePopupMenu`/`DestroyMenu`, `AppendMenuA/W`/`InsertMenuA`/
  `DeleteMenu`/`RemoveMenu` (MF_POPUP→submenu, MF_SEPARATOR/BITMAP/OWNERDRAW→pas de texte), `GetMenuItemCount`,
  `EnableMenuItem` (bits GRAYED|DISABLED, renvoie l'ancien), `CheckMenuItem` (bit CHECKED), `GetMenuState`
  (flags ; popup→hiword=nb items du submenu), `GetMenuStringA/W`, `GetSubMenu`, `GetMenuItemID`. Barre de
  menu fenêtre : `GetMenu`/`SetMenu` (tableau parallèle) ; `GetSystemMenu` (menu système SC_* par fenêtre,
  lazy ; bRevert reset) ; `TrackPopupMenu(Ex)`→0 (pas de sélection headless, sound).
- **Oracle** : `winecorpus/user32_menu.c` (+`.nodisplay`) — construction menu, count, states, Enable/Check
  round-trip (**statements séquencés** : l'ordre d'éval des args printf est non spécifié → forcer la mutation
  avant la lecture, sinon le test lit l'état pré-mutation), GetMenuString → **bit-identique à Wine**.
- **Effet mesuré** : EnableMenuItem 14, GetSystemMenu 12 (+ CreatePopupMenu/AppendMenu/…) éliminés des 41 Win95.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, cargo test complet vert,
  winediff **65/65**, table triée.

### 2026-07-10 — [HLE-FILE] .INI profile API (GetPrivateProfileString/Int, WritePrivateProfileString)
- **Mesuré d'abord (règle 9)** : probe Wine → valeur **trimmée** (espaces début/fin ; `"  42  "`→`42`), **une
  paire de guillemets** entourants retirée (`"spaced value"`→`spaced value`), section/clé **casse-insensible**,
  clé absente → défaut, `GetPrivateProfileInt` parse l'entier.
- **Fait** (`aret_hle.c`, +3 `stdcall_pops`) : INI adossé à un vrai fichier (`translate_path`). `ini_get`
  (sémantique de lecture Wine : trim + dé-quote + casse-insensible), `ini_rewrite` (set/insert clé ; value=NULL
  supprime la clé ; key=NULL supprime la section entière ; passe unique préservant le reste du fichier),
  `GetPrivateProfileStringA`/`GetPrivateProfileIntA`/`WritePrivateProfileStringA`. **L'oracle ne compare que
  les valeurs relues** (le layout disque est le nôtre — liberté totale de format).
- **Oracle** : `winecorpus/win_ini.c` (+`.nodisplay`) — write/read round-trip, trim, dé-quote, casse-insensible,
  défaut, **réécriture** de clé, **suppression** de clé → **bit-identique à Wine**.
- **Effet mesuré** : GetPrivateProfileStringA 7, GetPrivateProfileIntA 6 éliminés des 41 Win95.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, cargo test complet vert,
  winediff **66/66**, table triée.

### 2026-07-10 — [HLE-WIN32] Long-tail : Global/Local lock+realloc, GetObject, FindWindow, TZ, StdHandle
- **Mesuré** : `GlobalLock`(GMEM_FIXED)→le handle (=pointeur), `GlobalUnlock`→1, `GlobalReAlloc`=realloc
  (préserve), `GetObjectA`(DIB)→24 (BITMAP : w/h/planes=1/bpp=32/widthBytes=w·4).
- **Fait** (`aret_win32.c`, +12 `stdcall_pops` — dont **GlobalAlloc/Free/LocalAlloc ajoutés au pop table**,
  manquants = drift esp latent sur ces stdcall) : `GlobalLock`/`Unlock`/`ReAlloc` + jumeaux `Local*` (handle
  fixe = pointeur → Lock identité, Unlock succès, ReAlloc realloc) ; `GetObjectA/W` (BITMAP 24o / LOGBRUSH 12o) ;
  `FindWindowA/W` (par **titre** ; requête classe-seule → 0 = « pas de fenêtre », le bon « single-instance ») ;
  `GetTimeZoneInformation` (UTC : struct zérotée, Bias=0, TIME_ZONE_ID_UNKNOWN) ; `SetStdHandle` (dup2 sur le
  slot std).
- **Oracle** : `winecorpus/win_misc.c` — Global lock/realloc round-trip, GetObjectA dims, FindWindow titre →
  **bit-identique à Wine**.
- **Effet mesuré** : GetObjectA 7, GlobalLock/Unlock, FindWindowA 7, GetTimeZoneInformation 6, SetStdHandle 8
  éliminés/avancés des 41 Win95.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, cargo test complet vert,
  winediff **67/67**, table triée.

### 2026-07-10 — [HLE] Long-tail : FileTimeToSystemTime, CharNext/Prev, GetDriveType, ClientToScreen
- **Mesuré** : FileTime(2000-01-01)→dow=6 (samedi), CharNextA→+1 (à NUL reste), CharPrevA→-1,
  GetDriveTypeA("C:\\")→3 (DRIVE_FIXED), ClientToScreen(WS_POPUP @100,50) translate de (x,y).
- **Fait** (`aret_win32.c`, +7 `stdcall_pops`) : `FileTimeToSystemTime` (via `gmtime_r`, réciproque de
  SystemTimeToFileTime, date civile = Wine), `GetDriveTypeA/W`→DRIVE_FIXED (tout mappé sur le FS hôte),
  `CharNextA/W`/`CharPrevA/W` (single-byte/wide, bornés), `ClientToScreen`/`ScreenToClient` (origine client =
  origine fenêtre, no-NC → translate par (x,y)).
- **Oracle** : `winecorpus/win_timechar.c` → **bit-identique à Wine** (date, char iter, drive, coord round-trip).
- **Effet mesuré** : FileTimeToSystemTime 9, GetDriveTypeA 9, CharNextA/PrevA 5, ClientToScreen 5 éliminés.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, cargo test complet vert,
  winediff **68/68**, table triée.

### 2026-07-10 — [HLE-WIN32] Locale A-twins : CompareStringA/W, LCMapStringA, GetStringTypeA
- **Mesuré** : CompareStringA→CSTR_LESS/EQUAL/GREATER (1/2/3), NORM_IGNORECASE ok ; LCMapStringA upper/lower ;
  GetStringTypeA CT_CTYPE1 flags ('A'=0x0381, '1'=0x0284, ' '=0x0248, 'x'=0x0302 — les cœurs W existants les
  produisent déjà).
- **Fait** (`aret_win32.c`) : `GetStringTypeA` (jumeau ANSI ; **arg Locale de tête en plus** vs W), `LCMapStringA`
  (upper/lower single-byte), `CompareStringA/W` (**non implémentés avant** ; ordinal-ish + IGNORECASE, matche
  Wine pour ASCII/en-US ; collation accentuée profonde non modélisée). Pops déjà présents.
- **Oracle** : `winecorpus/win_locale.c` (+`.nodisplay`) → **bit-identique à Wine**.
- **Effet mesuré** : GetStringTypeA 9, LCMapStringA 9, CompareStringA 6 éliminés des 41 Win95.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, cargo test complet vert,
  winediff **69/69**, table triée.

### 2026-07-10 — [HLE] Long-tail (fin) : MoveFileA, CreateBitmap, cursor/popup, RegDelete
- **Fait** : `MoveFileA` (rename POSIX, `aret_hle.c`) ; `CreateBitmap` (bitmap 32bpp-backed, bpp/dims rapportés
  via GetObject — champ `bpp` ajouté à l'objet GDI, GetObjectA calcule bmWidthBytes WORD-aligné) ; `ShowCursor`
  (compteur, init 0 = Wine-avec-souris), `SetCursor`/`GetCursor` (suivi), `GetLastActivePopup`→hWnd ;
  `RegDeleteValueA/W`/`RegDeleteKeyA/W`→ERROR_FILE_NOT_FOUND(2) (cohérent hive vide read-only). +11 pops triés.
- **Oracle** : `winecorpus/win_lasttail.c` (+`.nodisplay`) — MoveFile round-trip, CreateBitmap+GetObject,
  ShowCursor (**statements séquencés** vs ordre d'éval printf), popup, RegDelete → **bit-identique à Wine**.
- **Effet mesuré** : MoveFileA 5, CreateBitmap 5, GetLastActivePopup 5, SetCursor 5, RegDeleteValueA 7,
  RegDeleteKeyA 5 éliminés des 41 Win95.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, cargo test complet vert,
  winediff **70/70**, table triée.
- **Bilan long-tail** : après ce lot, le corpus Win95 restant = **RtlUnwind/SEH** (tier EH, froid), **threads/
  process** (CreateThread/CreateProcess/GetExitCodeThread, §8.3), **FormatMessageA** (texte FROM_SYSTEM ≠ Wine),
  **ExtTextOutA** (raster police), **tokens/SID** (sécurité, à modéliser en lot), **SystemParametersInfoA**
  (env). Ce sont des chantiers dédiés ou des oracles durs — plus des « petits ».

### 2026-07-10 — [HLE-WIN32] advapi32 : modèle SID / token (structurel)
- **Mesuré** : SID = Revision(1)+SubAuthorityCount(1)+IdentifierAuthority(6)+SubAuthority[N]·4 ;
  AllocateAndInitializeSid(2 subauth)→len=16, EqualSid content-compare, subcount=2 sub0=32 sub1=544, len(1 sub)=12.
  *(LookupPrivilegeValue **hang** sous Wine — service sécurité requis — donc non oraclé.)*
- **Fait** (`aret_win32.c`, +17 `stdcall_pops`) : APIs SID **structurelles exactes** — `AllocateAndInitializeSid`/
  `InitializeSid`/`GetLengthSid`/`GetSidLengthRequired`/`IsValidSid`/`EqualSid` (compare octets)/`CopySid`/
  `GetSidSubAuthority(Count)`/`GetSidIdentifierAuthority`/`FreeSid`. Tokens (modèle mono-user non-élevé, sound,
  non oraclé) : `OpenProcessToken`/`OpenThreadToken` (handle opaque), `AdjustTokenPrivileges`→TRUE,
  `LookupPrivilegeValueA/W` (LUID fixe), `GetTokenInformation` (TokenElevation→non élevé ; classes non modélisées
  → FALSE honnête).
- **Oracle** : `winecorpus/win_sid.c` (+`.nodisplay`) — alloc/len/valid/eq/subauth round-trip → **bit-identique
  à Wine**.
- **Effet mesuré** : AllocateAndInitializeSid/EqualSid/FreeSid/OpenProcessToken/GetTokenInformation/
  AdjustTokenPrivileges/LookupPrivilegeValueA (5-7 binaires chacun) éliminés/avancés des 41 Win95.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, cargo test complet vert,
  winediff **71/71**, table triée.

### 2026-07-10 — [HLE-WIN32] Long-tail : rect math, char case, ptr/input/path stubs
- **Fait** (`aret_win32.c` +23 `stdcall_pops`, `aret_hle.c`) : **math rect** exacte — `SetRect(Empty)`/`CopyRect`/
  `IsRectEmpty`/`EqualRect`/`PtInRect`/`OffsetRect`/`InflateRect`/`IntersectRect`/`UnionRect` ; **char case** —
  `CharUpper/LowerA/W` (chaîne in-place **ou** char unique si hiword=0), `CharUpper/LowerBuffA` ; validation
  pointeur — `IsBadCodePtr`/`IsBadStringPtrA/W`→0 (natif = valide) ; input headless — `GetKeyState`/
  `GetAsyncKeyState`/`GetMessagePos`→0, `GetMessageTime`→tick, `GetWindowThreadProcessId` ; `GetShortPathNameA`
  (pas de 8.3 Linux → chemin long copié).
- **Oracle** : `winecorpus/win_rect.c` (+`.nodisplay`) — intersect/union/ptin/offset (**IntersectRect séquencé**
  avant lecture, cf. ordre d'éval printf), upper/lower, IsBadCodePtr → **bit-identique à Wine**.
- **Effet mesuré** : IntersectRect/CharUpperA/GetKeyState/IsBadCodePtr/GetShortPathNameA/GetWindowThreadProcessId
  (4-5 binaires chacun) éliminés des 41 Win95.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, cargo test complet vert,
  winediff **72/72**, table triée.

### 2026-07-10 — [GUI][HLE-WIN32] GetClassName + fonts GDI + défauts DC + IsDialogMessage
- **Fait** (`aret_win32.c`, +8 `stdcall_pops`) : **stockage du nom de classe** dans la fenêtre (champ
  `classname`, résolu à la création via `u32_class_name` — atome→registre / string A ou W) → `GetClassNameA/W`.
  Fonts GDI opaques (`CreateFontA/W`/`CreateFontIndirectA/W` → objets `GDIT_FONT`). `IsDialogMessageA/W`→0
  (nav clavier headless, sound ; **hang sous Wine** → non oraclé).
- **Bug attrapé par l'oracle (rule 10)** : `SelectObject(dc,f)` puis re-select ne round-trippait pas — un DC
  ARET n'avait **pas d'objets par défaut**, alors que Wine a SYSTEM_FONT/WHITE_BRUSH/BLACK_PEN sélectionnés.
  Fix : `u32_dc_defaults` (refactor `GetStockObject`→`u32_stock`) sélectionne les défauts à `GetDC`/
  `CreateCompatibleDC`/`BeginPaint` → round-trip = Wine. Hash GDI DIB **inchangé** `a182d45a`.
- **Oracle** : `winecorpus/win_classfont.c` — GetClassName, font non-null/distinct, **SelectObject font
  round-trip** → **bit-identique à Wine** (Xvfb).
- **Effet mesuré** : GetClassNameA/CreateFontIndirectA/IsDialogMessageA (4 binaires chacun) éliminés des 41 Win95.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, cargo test complet vert,
  winediff **73/73**, table triée.

### 2026-07-10 — [GUI][HLE-WIN32] GDI état DC (Save/Restore/MapMode/ClipBox) + stubs registre/hooks
- **Mesuré** : `SetMapMode`→ancien MM_TEXT(1) ; `SaveDC`→niveau 1 ; `RestoreDC(1)`→1, restaure la couleur texte
  (0x030201) ; `GetClipBox`→SIMPLEREGION(2), rect = bornes DIB {0,0,8,4}.
- **Fait** (`aret_win32.c`, +11 `stdcall_pops`) : **pile d'état DC** (champs `mapmode`/`savetop`/`sstk[8]` sur
  l'objet GDI) → `SaveDC`/`RestoreDC` (font/brush/pen/textcol/bkcol/bkmode/mapmode ; niveau absolu ou relatif),
  `Set`/`GetMapMode`, `GetClipBox` (surface DIB ou écran virtuel). Stubs sound : `RegOpenKeyA`→NOT_FOUND(2)
  (hive vide), `RegQueryInfoKeyA`→SUCCESS (clé vide), `SetWindowsHookExA/W` (HHOOK opaque), `CallNextHookEx`→0,
  `UnhookWindowsHookEx`→TRUE.
- **Oracle** : `winecorpus/gdi_dcstate.c` — mapmode/save/restore/textcolor/clipbox → **bit-identique à Wine**.
- **Effet mesuré** : SaveDC/RestoreDC/SetMapMode/GetClipBox/RegOpenKeyA/CallNextHookEx (3-4 binaires) éliminés.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest 271/271, cargo test complet vert,
  winediff **74/74**, table triée.

### 2026-07-11 — [GUI][HLE-WIN32][RECOMPILE] M7 — G2b : fenêtre VISIBLE via SDL2 (la 1ʳᵉ marche graphique)
- **Pourquoi** : « un transpilé doit être *utilisable* → afficher à l'écran est obligatoire ». G2b est la
  vraie marche archi de M7 : une fenêtre Win32 visible **s'affiche réellement** (Linux/macOS, WASM plus tard).
- **Fait — build (`src/builder/mod.rs`)** : `sdl2_flags()` interroge `pkg-config sdl2` (chemin i386
  `/usr/lib/i386-linux-gnu/pkgconfig` ajouté au `PKG_CONFIG_PATH`). `-DARET_HAVE_SDL` + cflags SDL injectés à la
  compile **et** `-lSDL2` au link **uniquement si** le binaire importe `CreateWindowExA/W` **et** SDL2 est
  présent (32-bit, cible native). Sinon **compile/link byte-identiques** à avant (dégradation propre : CLI,
  message-only, wasm, hôte sans SDL → couche fenêtre reste display-free). ⇒ hash transpile **inchangé**.
- **Fait — runtime (`aret_win32.c`, tout sous `#ifdef ARET_HAVE_SDL`)** : chaque **fenêtre top-level visible**
  (parent==0, pas `WS_CHILD`, pas message-only) reçoit (1) un **framebuffer client** (DIB 32bpp top-down) que
  `GetDC(hwnd)`/`BeginPaint(hwnd)` **lient** (le GDI du programme dessine dedans, comme Wine) ; (2) une vraie
  **`SDL_Window`+Renderer+Texture** (`SDL_PIXELFORMAT_RGB888` = octets DIB `[B,G,R,0]` exacts) ; (3) une **pompe
  `SDL_PollEvent`** → `WM_CLOSE`/`WM_MOUSE*`/`WM_KEY*` (VK mappé). Présentation sur `UpdateWindow`/`EndPaint`/
  `ReleaseDC`. `DestroyWindow`→destruction SDL. Câblé dans `CreateWindowEx`/`ShowWindow`/`Peek`-`GetMessage`.
- **Soundness — strictement additif** : la pompe ne synthétise **que** l'entrée réelle (close/souris/clavier) ;
  le bruit window-manager (expose/focus) **ne** devient **pas** `WM_PAINT`/`WM_ACTIVATE` (ceux-ci restent pilotés
  par le modèle d'invalidation Win32) → la séquence de messages déterministe est **inchangée**. SDL absent /
  `SDL_Init` échoue (pas d'écran) → repli display-free (no-op sound), **jamais** d'abort. `GetMessageW` ne
  s'abort plus quand une fenêtre SDL vit (source de messages réelle) : il bloque sur les events.
- **Oracle** : `winecorpus/user32_sdlwindow.c` — `WS_POPUP|WS_VISIBLE` (client==window, `GetClientRect`
  déterministe), `GetDC(hwnd)`+`SetPixel`+`GetPixel` round-trip (3 points dont coin), cycle `BeginPaint`/
  `EndPaint`+relecture → **bit-identique à Wine** (ARET SDL headless dummy/x11-Xvfb vs Wine réel sous Xvfb).
  **Preuve de l'additivité** : les fixtures GUI existantes (windowstate/dialog/gdi/…) **relient SDL et créent de
  vraies fenêtres** pendant winediff → **toujours** bit-identiques ⇒ la couche SDL n'a **rien** perturbé.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest **271/271**, cargo test complet vert
  (dont wasm_target = chemin sans SDL compile propre), winediff **74→75/75**, `regression.sh` vert (gzip/ls/cat
  recompilés byte-identiques : non-GUI = 0 flag SDL).
- **Portée honnête** : les *pixels à l'écran* sont la marche archi (non bit-comparés en portable) ; ce qu'on
  **prouve** = le modèle fenêtre visible + GDI-dans-le-DC-fenêtre + présentation, bit-identique Wine, headless.
  Reste (mesuré si un binaire l'exige) : widgets natifs (BUTTON/EDIT), GDI raster (TextOut/DrawText), WASM-GUI
  (Emscripten, 2ᵉ toolchain).

### 2026-07-11 — [GUI][HLE-WIN32] M7 — modèle de peinture `WM_PAINT` / invalidation (le contenu s'affiche vraiment)
- **Pourquoi** : après G2b (fenêtre visible), un programme qui **peint dans son handler `WM_PAINT`** ne dessinait
  **rien** — `InvalidateRect` était un no-op et **personne** ne livrait `WM_PAINT`. C'est le chaînon manquant pour
  qu'une fenêtre visible **affiche son contenu** piloté par la boucle de messages normale.
- **Fait** (`aret_win32.c`, display-free, sound, portable — **pas** `#ifdef SDL`) : région d'invalidation
  **coalescée par fenêtre** (champ `needs_paint` ; `WM_PAINT` unionne les régions de toute façon).
  `InvalidateRect`/`InvalidateRgn` (hwnd NULL = **toutes** les top-level), `ValidateRect`/`Rgn`, une fenêtre
  visible fraîche (`CreateWindow(WS_VISIBLE)`/`ShowWindow`) l'active. `WM_PAINT` **généré à la demande** (jamais
  mis en file, **priorité basse** : après la file postée et `WM_QUIT`) par `Get`/`PeekMessage` (`u32_next_paint`),
  **et** livré **synchrone** par `UpdateWindow` (comme Windows). `BeginPaint` **valide** la région (sinon boucle
  infinie). `DefWindowProc(WM_PAINT)` = peinture par défaut → **valide** (un programme qui délègue ne boucle pas) ;
  `DefWindowProc(WM_CLOSE)` = `WM_DESTROY` + destruction (le bouton X de la fenêtre SDL **ferme** vraiment). Gate
  strict : **top-level visible seulement** (`u32_win_paints` : `parent==0`) → message-only/enfants **jamais**.
- **Soundness / additivité** : `WM_PAINT` et `WM_CLOSE`/`WM_DESTROY` ne surgissent que d'événements générés ou
  d'entrée **réelle**, jamais dans l'oracle déterministe headless (les fixtures message-only sont des
  `HWND_MESSAGE` → exclues). **Découverte** : SDL sous Xvfb émet un `SDL_MOUSEMOTION` synthétique initial → un
  `WM_MOUSEMOVE` que Wine ne poste pas. C'est **intrinsèque** (l'entrée est env-dépendante, non déterministe) ⇒
  l'oracle compare le **contenu** (comptes de peinture, pixels), **pas** les itérations de boucle/events (doc 72
  §4). Le fixture n'observe donc **pas** le nombre de tours de `PeekMessage`.
- **Oracle** : `winecorpus/user32_paint.c` — handler `WM_PAINT` dessine un pixel + compte ; `UpdateWindow`
  (livraison synchrone), `InvalidateRect`+`PeekMessage` (livraison générée), no-op quand la région est vide →
  **bit-identique à Wine** (comptes 1→2, pixel `21160B`=RGB(11,22,33), headless SDL dummy / Xvfb).
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest-transpile 4/4, cargo test complet vert
  (dont wasm : modèle peinture compile sans SDL), winediff **75→76/76** (aucune régression : dialogs/windowstate/
  msgwindow intacts). Un programme GUI qui peint dans `WM_PAINT` **affiche maintenant son contenu à l'écran**.

### 2026-07-11 — [GUI][HLE-WIN32] M7 — fond `WM_ERASEBKGND` (pinceau de classe) + garde double-buffering
- **Pourquoi** : après le modèle de peinture, une vraie fenêtre affichait le **noir** au lieu de son fond voulu —
  le `hbrBackground` de la classe n'était ni stocké ni appliqué. Or l'effacement de fond via `WM_ERASEBKGND` est
  **universel** (toute fenêtre standard). Et le **double-buffering** (dessiner offscreen puis `BitBlt` vers la
  fenêtre) est **l'idiome de rendu dominant** des vraies applis.
- **Fait — `WM_ERASEBKGND`** (`aret_win32.c`) : `RegisterClassA/W` stocke `hbrBackground` (offset +28 = `wc[7]`)
  dans le registre de classes ; `CreateWindowEx A/W` le copie sur la fenêtre (`bg_brush`) via `u32_class_brush`.
  Région d'erase par fenêtre (`needs_erase`, posée au show et par `InvalidateRect(bErase=TRUE)`). `BeginPaint`
  envoie **`WM_ERASEBKGND(wParam=hdc)`** (après avoir lié le DC au framebuffer client, et **avant** de valider) ;
  `DefWindowProc(WM_ERASEBKGND)` **remplit tout le client** avec la couleur du pinceau (`u32_fill_dc_brush`,
  réutilise `gdi_brush_color`/`gdi_dc_surface`) et renvoie TRUE. `needs_erase` effacé **avant** le callback (pas
  de récursion). Pinceau NULL / pas de surface (display-free) → no-op sound.
- **Fait — double-buffering** : **aucun code** — le binding framebuffer-client (G2b) + le GDI DIB (G6) composent
  déjà : `CreateCompatibleDC`→`CreateDIBSection`→`FillRect`/`SetPixel`→`BitBlt(SRCCOPY)` vers le DC fenêtre
  atterrit dans le framebuffer, relu bit-exact. Verrouillé comme garde de régression.
- **Oracles** : `winecorpus/user32_erasebg.c` (classe avec `hbrBackground`=CreateSolidBrush ; un pixel **non
  dessiné** lu pendant/après `WM_PAINT` = **couleur du pinceau de classe** `C89603`=RGB(3,150,200)) et
  `user32_dbuffer.c` (DIB offscreen `RGB(7,8,9)`+pixel → `BitBlt` → relecture fenêtre) → **bit-identiques à Wine**
  headless.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest-transpile 4/4 (compile no-SDL propre),
  cargo test complet vert (dont wasm), winediff **76→78/78** (aucune régression). Une vraie fenêtre GUI affiche
  désormais **son fond de classe + son rendu double-buffered**, exactement comme sous Windows/Wine.

### 2026-07-11 — [GUI][HLE-WIN32] M7 — `RegisterClassExA/W` (WNDCLASSEX — la forme des applis modernes)
- **Pourquoi** : quasi toutes les applis GUI Win32 modernes enregistrent leur classe via `RegisterClassEx`
  (WNDCLASSEX), pas `RegisterClass`. Sans lui, une telle appli **abortait** dès l'enregistrement de classe.
- **Fait** (`aret_win32.c` + `stdcall_pops.rs`) : `aret_RegisterClassExA/W` parsent le WNDCLASSEX 32-bit (chaque
  champ décalé de +4 vs WNDCLASS à cause de `cbSize@0`) → **lpfnWndProc @+8 (`wc[2]`)**, **hbrBackground @+32
  (`wc[8]`)**, **lpszClassName @+40 (`wc[10]`)** ; réutilisent `u32_class_register` (donc pinceau de fond pris en
  compte) et partagent le registre A/W. Table `stdcall_pops` : `RegisterClassExA/W` = 4 (1 pointeur), insérées
  triées (`RegisterClassA` < `RegisterClassExA` < `RegisterClassExW` < `RegisterClassW`).
- **Oracle** : `winecorpus/user32_classex.c` — `RegisterClassExA` + `hbrBackground` + fenêtre visible + `WM_PAINT`
  (fond de classe lu `2D1E0F`=RGB(15,30,45), pixel dessiné) → **bit-identique à Wine** headless. Le chemin complet
  d'une appli moderne (RegisterClassEx → CreateWindowEx → peinture avec fond) fonctionne.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest-transpile 4/4, `table_is_sorted_by_name`
  vert, winediff **78→79/79**.

### 2026-07-11 — [GUI][INFRA][ORACLE] M7 G7 — 1er sweep du corpus GUI Win95 réel (mesure, pas intuition)
- **Corpus** : ISO `WIN95_09961.iso` (631 Mo) de `archive.org/download/BestOfWindows95DotCom` (cf. doc 70 §7),
  extrait via `7z` (`p7zip-full`/`libarchive-tools` installés ; **pas de module noyau iso9660** → mount échoue,
  `7z x` marche). 155 `.exe` → tri par `file` : **54 NE (Win3.1 16-bit** = hors périmètre PE), ~65 SFX/zip,
  **26 PE32 i386** (23 GUI + 3 console) = la cible réelle (mIRC 4.6, War-FTP, mailnews, cchat, finger, nslookup…).
- **Wallsweep statique** (`--mode walls`, 26 bins) — **top imports non implémentés par nb de binaires** :
  `RtlUnwind` **22**, `CreateProcessA` **18**, `CreateThread` **15**, `FormatMessageA` **13**, `ExitWindowsEx` **11**,
  `TerminateThread` **11**, puis `ExtTextOutA`/`TextOutA`/`TerminateProcess` (3), et une longue traîne (2/1) :
  `LZ*`, `Dde*`, `GetOpenFileNameA`, `GetVolumeInformationA`, `CreateDCA`, `DialogBoxIndirectParamA`, MDI…
  ⇒ **confirme la prédiction doc §5** : les murs restants = **chantiers profonds différés** (EH/`RtlUnwind`,
  threads/process) + traîne de shims. Aucune grosse victoire générale « à chaud » facile ne reste.
- **`FormatMessageA` (13) analysé, PAS retenu comme fix propre** : Wine renvoie **son propre catalogue court**
  (code 2→`File not found.\r\n`, ≠ Windows) ; matcher Wine bit-à-bit = transcrire des **centaines** de chaînes
  Wine-spécifiques par code = rustine fragile, non générale. `FROM_SYSTEM` = abort sound honnête ; seul
  `FROM_STRING` (substitution `%1..%99`, sans catalogue) serait général — à faire si un binaire l'exige.
- **Découverte runtime décisive (plus informative que les imports froids)** : les 2 plus simples (finger/nslookup,
  ~65 fn, **seul import manquant = `RtlUnwind` = froid**) **abortent au runtime** non sur RtlUnwind mais sur un
  **`indirect call to unrecovered function 0x401700`**. Cause racine tracée : 0x401700 est une fn CRT réelle dont
  l'adresse n'apparaît **que** comme **pointeur dans `.data`** (VA 0x40600c), dans une **table `_initterm`
  statique** — le code fait `push 0x406010; push 0x406008; call 0x4023d0` = `_initterm(pfbegin,pfend)` **linké en
  statique** (fn locale 0x4023d0 qui boucle sur `[begin,end)` et appelle chaque pointeur non-nul). ARET ne
  reconnaît `_initterm` **que comme import** → la table `.data` n'est pas marquée address-taken → 0x401700/0x4017d0
  non récupérées → abort sound (« refusing to guess »).
- **⇒ Prochain chantier ciblé (général, mais recovery = correctness-critique → session dédiée, batterie complète)** :
  reconnaître l'idiome **`_initterm(start,end)` statique** — un `call` précédé de `push ptrEnd; push ptrBegin`
  (deux immédiats bornant une plage de la **même section data**, contenu = pointeurs de code alignés vers `.text`)
  → marquer chaque entrée non-nulle **address-taken**. Gating strict (byte-identique sinon), régression totale
  (difftest/cpudiff/funcdiff/winediff/sweeps) obligatoire — une fausse entrée tronque une vraie fn (doc §7). Débloque
  potentiellement finger/nslookup (et tout static-CRT MSVC-like du corpus) bout-en-bout.
- **Mesuré, non deviné.** Aucun code changé cet incrément (mesure pure) ; corpus/ISO en scratchpad éphémère.

### 2026-07-11 — [RECOV] Tentative `_initterm(begin,end)` statique — **corpus-safe mais REVERTÉE** (expose un bug aval)
- **Fait (puis reverté)** : détecteur du couple `push pfend; push pfbegin; call dispatcher` → si les **deux**
  extrémités sont des pointeurs de données poussés et que **chaque mot non-nul de `[begin,end)`** pointe dans une
  section exécutable, marquer chaque entrée non-nulle address-taken (contourne la garde de prologue, comme les
  autres preuves address-taken). Idiome très spécifique → **byte-identique** sur tout le corpus existant.
- **Vérifié corpus existant INCHANGÉ** : hash transpile **`19acad982194bf07`**, difftest **271/271** — le
  heuristique **ne sur-sème pas** (sûr là où ça compte). Récupère bien 0x401700 & co pour finger (67→78 fn).
- **MAIS — reverté (principe sacré)** : une fois les entrées `_initterm` récupérées, finger/nslookup **dépassent
  le startup CRT et divergent en AVAL** : finger tourne jusqu'au bout mais **sortie vide** (Wine imprime la
  bannière) = **faux silencieux** ; nslookup **abort sur `indirect call 0x80000073`** (pointeur-fonction
  **garbage** au runtime). Transformer un **abort sound en sortie muette fausse** = **violation** du principe →
  révoqué. Le heuristique de recovery est **correct et corpus-safe**, mais **couplé** à un **bug de lift aval**
  (chemin stdio/CRT du static-CRT « Dennis J. Cox » : un init mal lifté laisse un pointeur-fonction à 0x80000073,
  ou un cast/signe le produit). ⇒ **chantier couplé** pour session dédiée : *recovery `_initterm` + fix du miscompile
  aval*, à shipper **ensemble** (recompiler finger en `-O0 -g` + gdb sur le call 0x80000073, cf. tip §7). Tant que
  l'aval n'est pas juste, finger/nslookup **restent à l'abort sound** (correct-ou-abort), jamais faux en silence.
- **Leçon (règle §2 « borner puis pivoter »)** : la recovery seule ne suffit pas — récupérer une entrée ne vaut
  que si le code qu'elle débloque lift **juste**. Le vrai mur de ces deux binaires est **en aval** de la recovery.

### 2026-07-11 — [GUI][HLE-WIN32] M7 G7 — batch window/dialog/DC + classes de contrôles prédéfinies (par la donnée)
- **Pourquoi** : le sweep runtime du corpus Win95 (26 applis) montre, après avoir écarté les **installeurs**
  (`CreateProcessA`, frontière dure = >½ du corpus), un cluster de helpers **implémentables** partagés par les
  vraies applis GUI. On les abat en batch, oracle Wine.
- **Fait** (`aret_win32.c` + `stdcall_pops.rs`, tout display-free/sound) :
  - **Classes de contrôles prédéfinies** (`u32_is_ctrl_class` : BUTTON/EDIT/STATIC/LISTBOX/COMBOBOX/SCROLLBAR/
    common-controls…) → `CreateWindowEx` crée une **fenêtre-contrôle *data-only*** (état suivi, pas de WNDPROC app,
    pas de pixels) au lieu de renvoyer 0. Enfant `WS_CHILD` : `hMenu`=**ctrl_id**. Débloque `GetDlgItem`/
    `Get-SetDlgItemText`/`CheckDlgButton` sur les contrôles créés par `CreateWindowEx` (ce que font les vrais dialogs).
  - **`CheckDlgButton`/`IsDlgButtonChecked`** (état de coche sur le contrôle enfant, round-trip).
  - **`RegisterWindowMessageA/W`** (id unique process, [0xC000..0xFFFF], même chaîne→même id, comme un atome).
  - **`MapWindowPoints`** (client↔écran via les rects fenêtre ; NULL=écran).
  - **`IsWindowUnicode`** (fenêtre créée via une API W → TRUE ; champ `unicode`).
  - **`SetTextAlign`/`GetTextAlign`** (état DC, round-trip).
  - **`BeginDeferWindowPos`/`DeferWindowPos`/`EndDeferWindowPos`** (application immédiate = état final identique
    au batch, seule l'atomicité de repaint diffère — cosmétique).
  - **`RedrawWindow`** (replié dans le modèle de peinture : RDW_INVALIDATE/ERASE/VALIDATE/UPDATENOW).
  - **`ExitWindowsEx`** → 1 (stub sound, jamais de logoff réel ; non oraclé).
  - *Écarté* : `GetWindowWord`/`SetWindowWord` — sans le suivi de `cbWndExtra`, un write hors-borne diverge de Wine
    (Wine l'ignore) → retiré plutôt que shipper un shim subtilement faux.
- **Oracle** : `winecorpus/user32_winbatch.c` — RWM (même/diff/range), MapWindowPoints (2 sens), contrôle BUTTON +
  Check/IsDlgButtonChecked, IsWindowUnicode, SetTextAlign round-trip, DeferWindowPos move → **bit-identique à Wine**.
- **Effet mesuré (re-sweep)** : les murs **ont avancé** — `CheckDlgButton` (mirc), `BeginDeferWindowPos`
  (portmessage) **disparaissent** de la tête de liste ; les applis butent désormais sur le **cluster suivant** (DDE,
  imprimante, `ExtTextOut`/text-raster, ImageList/common-controls). Aucune appli entièrement débloquée (murs
  derrière), mais la méthode *mesure→batch→re-mesure* **avance** concrètement, comme pour le corpus console.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest-transpile 4/4, `table_is_sorted` vert,
  cargo test complet, winediff **79→80/80**.

### 2026-07-11 — [GUI][HLE-WIN32][HLE-FILE] M7 G7 — batch long-tail (props fenêtre, GlobalHandle, win.ini, stubs sound)
- **Pourquoi** : après le batch dialogs, le re-sweep montre une **longue traîne** de shims (1-2 binaires chacun) ;
  pas de gros cluster restant (FormatMessageA=catalogue Wine, text-raster=pas d'oracle). On abat le sous-ensemble
  **sain + oracle-propre**.
- **Fait** (`aret_win32.c`/`aret_hle.c` + pops) :
  - **Liste de propriétés fenêtre** : `SetProp`/`GetProp`/`RemovePropA/W` (store par (hwnd,clé)→valeur, round-trip).
  - **`GlobalHandle`/`LocalHandle`** : identité (tas fixe → handle == pointeur, cohérent avec `GlobalLock`).
  - **`GetProfileStringA`/`GetProfileIntA`** : variantes win.ini des lectures private-profile (même lecteur INI,
    fichier implicite « win.ini »).
  - **`GetVolumeInformationA`** : volume ARET **invariant** (label/serial/fs), env-dépendant → non bit-comparé
    (comme `GetDiskFreeSpace`).
  - **`GetOpenFileNameA`/`GetSaveFileNameA`** → FALSE (pas de sélecteur affiché ; « annulé » sound, jamais un
    nom deviné).
  - **`WinExec`** → 2 (`ERROR_FILE_NOT_FOUND` ; même frontière dure que `CreateProcess`, ne prétend jamais lancer).
- **Oracle** : `winecorpus/user32_propmisc.c` — props round-trip, `GlobalHandle` identité, profile défauts →
  **bit-identique à Wine**.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest-transpile 4/4, `table_is_sorted` vert,
  winediff **80→81/81**.
- **Note honnête (rendements décroissants)** : la traîne restante = shims à 1-2 binaires, derrière chacun d'autres
  murs (DDE, common-controls, text-raster) ; aucune vraie appli du corpus n'est encore débloquée de bout en bout.
  La méthode avance, mais le marginal par shim est faible — cf. doc §5 (profondeur, pas largeur).

### 2026-07-11 — [GUI][HLE-WIN32] M7 G7 — common controls : sous-système **ImageList** (comctl32)
- **Pourquoi** : pivot vers un **sous-système entier** (pas la traîne) — ImageList est la **fondation données**
  des toolbars / list-view / tree-view des vraies applis GUI. Oracle propre (opérations bitmap).
- **Fait** (`aret_win32.c`, réutilise le modèle DIB GDI G6) : table d'image-lists (handle base `0x50000000`),
  images stockées en **bande verticale 32bpp** (cx large, count·cy haut). `ImageList_Create`(cx,cy,flags,cInit,
  cGrow), `ImageList_Add`/`AddMasked` (copie les tuiles cx-larges d'un HBITMAP source, croissance auto),
  `ImageList_Draw`(ILD_NORMAL/…, blit opaque dans la surface DC destination, bit-exact), `GetImageCount`,
  `GetIconSize`, `Set`/`GetBkColor`, `Destroy`. `InitCommonControls(Ex)` (no-op/TRUE).
- **Modèle 32bpp opaque (matche Wine)** : une source `BI_RGB` plate ne porte **pas** d'alpha, et une liste
  `ILC_COLOR32` n'applique **pas** de masque par couleur-clé → mesuré : Wine `AddMasked`+`ILD_TRANSPARENT` dessine
  le rouge **opaque** (pas transparent). Reproduire la sémantique masque/profondeur exacte de Wine (dépendante du
  color-depth) = terrier fragile → on **matche le comportement mesuré** (opaque) au lieu de deviner. Une divergence
  attrapée par l'oracle (fond frais → ARET transparent vs Wine opaque) → corrigé en opaque. Cf. leçon FillRect-inversé.
- **Oracle** : `winecorpus/comctl_imagelist.c` — create/iconsize/add/count, draw de 2 tuiles (rouge/vert) dans un
  DIB destination + relecture pixels, SetBkColor → **bit-identique à Wine**. (Gotcha printf ordre d'éval :
  `count` séquencé après `add`.)
- **Reste ImageList** : `ImageList_LoadImageA` (charge depuis ressource — décode bitmap, différé → 0/unimpl),
  icônes réelles avec alpha (blend), masque 1bpp pour listes <32bpp. À faire si un binaire mesuré l'exige.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest-transpile 4/4, `table_is_sorted` vert,
  winediff **81→82/82**.

### 2026-07-11 — [GUI][HLE-WIN32] M7 G7 — `LoadBitmapA/W` + `LoadImageA/W` : décodeur DIB ressource → HBITMAP
- **Pourquoi** : suite logique d'ImageList — les vraies applis chargent leurs images de toolbar/UI depuis les
  **ressources** (`LoadBitmap`/`LoadImage`) puis les passent à ImageList. Ferme le pipeline ressource→HBITMAP→
  ImageList→dessin. Oracle propre (charger un .bmp connu, relire les pixels).
- **Fait** (`aret_win32.c`, réutilise le walker de ressources G4 + le modèle DIB G6) : `u32_load_dib_resource`
  décode un **DIB empaqueté** (BITMAPINFOHEADER + palette + bits, **sans** BITMAPFILEHEADER) d'une ressource
  **RT_BITMAP** vers un HBITMAP interne 32bpp. Gère **BI_RGB 1/4/8/24/32 bpp** (palette pour ≤8bpp, bottom-up,
  stride aligné 4) ; RLE/autre compression → **abort sound**. `LoadBitmapA/W` (MAKEINTRESOURCE), `LoadImageA/W`
  (IMAGE_BITMAP depuis ressource ; icônes/curseurs/LR_LOADFROMFILE → 0 sound, pas de handle bidon).
- **Oracle** : `winecorpus/comctl_loadbitmap.{c,rc,bmp}` — .bmp 4×2 24bpp embarqué (windres), `LoadBitmapA` →
  select dans un DC mémoire → `GetObject` (taille) + `GetPixel` des 4 coins → **bit-identique à Wine**
  (`1E140A`/`3264C8`/`090807`/`FFFFFF`). `winediff.sh` : `windres -I $CORPUS` (une .rc peut référencer un .bmp
  voisin) + déjà `-lcomctl32`.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest-transpile 4/4, `table_is_sorted` vert,
  winediff **82→83/83**.

### 2026-07-11 — [HLE-FILE] M7 G7 — décompression LZ (lzexpand / SZDD) : réutilisation d'algorithme vérifié
- **Pourquoi** : après avoir écarté `FormatMessageA` (catalogue Wine, froid) et le text-raster (pas d'oracle
  pixel), **LZ** (`LZOpenFileA`/`LZCopy`/`LZClose`, 2 binaires) est le dernier mur **implémentable + oracle-propre**
  du corpus — un **vrai algorithme déterministe** (LZSS/SZDD), pas un stub. Les APIs LZ décompressent les fichiers
  d'install/données. C'est de la **réutilisation d'algorithme vérifié** (doctrine §1).
- **Fait** (`aret_hle.c`) : décompresseur **SZDD LZSS** exact (magic 8 o + mode/missing + taille 4 o, puis LZSS sur
  ring-buffer 4 Ko init 0x20, position 4078 ; control byte 8 bits LSB-first : littéral, ou back-ref 12-bit
  position + (4-bit len +3)). Un handle LZ (tag `0x4C5A0000`) porte tout le contenu décompressé ; un fichier
  non-SZDD est gardé verbatim. `LZOpenFileA` (slurp+décompresse, remplit l'OFSTRUCT), `LZRead`/`LZSeek` (sert
  depuis le buffer ; fd brut → read/lseek), `LZClose`, `LZCopy` (source LZ/fd → écrit tout vers le fd dest),
  `LZInit` (enveloppe un fd), `GetExpandedNameA` (restaure le char manquant de l'extension via l'en-tête).
- **Oracle** : `winecorpus/lz_decompress.c` — blob SZDD **embarqué** (auto-suffisant, écrit via l'API Win16
  `_lcreat`/`_lwrite`), décompressé 2 façons (`LZOpenFile`+`LZRead`, et `LZCopy` vers fichier) → 53 octets
  exacts, chaîne correcte → **bit-identique à Wine**. `winediff.sh` lie `-llz32`.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest-transpile 4/4, `table_is_sorted` vert,
  winediff **83→84/84**.

### 2026-07-11 — [RECOV][DEMO] Diagnostic finger : la recovery `_initterm` **fonctionne** ; reste 1 crash CRT aval
- **Contexte** : « est-ce une avancée générale ? » → mesurer avant de conclure. Ré-appliqué la recovery
  `_initterm` (temporairement) + `gdb` sur le C généré `-O0 -g`.
- **Résultat majeur** : avec la recovery, **finger imprime sa bannière CORRECTE** (`Finger v1.0 - Dennis j.
  Cox - Public Domain / Use finger /? for help`) = **bit-identique à Wine**. Le `main` de finger **tourne
  juste**. La recovery est donc une **vraie avancée générale** (déjà prouvée corpus-safe : difftest 271/271,
  hash inchangé) — et 0x4017d0 (entrée `_initterm` récupérée) est une **vraie fonction** (prologue propre après
  padding int3), pas un faux positif.
- **Reste UN crash aval** : après la bannière, SIGSEGV dans `sub_403a90` (fonction msvcrt géante) au **term-time
  `_initterm`** (cleanup de sortie) : `v184 = *(v146+0x18) = NULL` puis `*(NULL)`. `v146` est une structure
  **sur la pile** (`+0x00=1, +0x04=ptr, +0x08=0x45, +0x0c=ptr, puis zéros`) ; le motif `*(v186+v185+4) & 0x40`
  = un check de flag `_osfile` (lowio/`__pioinfo`). Wine ne crashe pas là → un champ que Wine a non-nul est nul
  chez ARET. **Effet observable** : le crash empêche le flush stdout (bufferisé en pipe) → sortie vide via
  `--run` (alors que la bannière est bien produite).
- **Verdict** : recovery = **générale et correcte** (fait tourner le `main`). Mais **shippée seule elle est
  pire** (finger passe d'un abort propre à un crash-sortie-vide) → **révoquée** (principe sacré). Le crash CRT
  aval est **profond** (behemoth msvcrt, centaines de locals) et sa **généralité n'est pas prouvée** — chantier
  **couplé** dédié : *recovery `_initterm` + fix du crash cleanup CRT*, à shipper ensemble.
- **Reframing utile** : finger n'est **pas** loin — son `main` marche, il est **à 1 bug CRT** de fonctionner
  (un vrai binaire Win95 qui tournerait). Le mur restant est le **cleanup CRT statique** (term `_initterm`
  touchant une structure lowio à champ nul), pas la recovery ni le `main`.

### 2026-07-11 — [RECOV][DEMO] finger (suite) : crash exit-cleanup CRT localisé, **non résolu** (borné)
- **Attaqué le crash aval** (session dédiée). Progrès réels, pas de fix convergent :
  - **Entrée = correcte** : ARET démarre à `sub_4014d0` = le **vrai `mainCRTStartup`** (prologue SEH
    `mov eax,fs:0; push ebp; push -1; push 0x405000`). L'entrée du header PE `0x4013e0` **désassemble en garbage**
    (`jmp` bogus) — forcer `--entry 0x4013e0` donne une sortie vide. Donc pas un problème de choix d'entrée.
  - **Hypothèse `VirtualAlloc` (reserve→commit) RÉFUTÉE** : sous gdb, `VirtualAlloc`/`HeapAlloc` **jamais
    appelés** ; finger n'appelle que `HeapCreate` (→ handle 1). Le fix « honorer la base au commit » n'a rien
    changé (et n'est exercé par aucun oracle) → **reverté**.
  - **Crash localisé** : SIGSEGV **déterministe** à l'exit-cleanup (term `_initterm`) dans `sub_403a90` (fonction
    msvcrt géante), `*(v146+0x18)=NULL` puis `*(NULL)`. `v146` = structure sur la pile, `+0x00=1` (le handle
    `HeapCreate`), `+0x04`/`+0x0c` = pointeurs pile auto-référents (nœud de liste), `+0x10..+0x1c=0`. Motif
    `*(v186+v185+4)&0x40` = check flag (lowio `_osfile`/heap). Zone enveloppée de `__x87rt_precall/postcall`
    (filet x87 runtime actif). Wine n'y crashe pas → un champ que Wine a non-nul est nul chez ARET.
- **Verdict** : la recovery `_initterm` **reste une avancée générale prouvée** (fait tourner le `main`, bannière
  bit-identique). Mais le crash cleanup est un bug **profond des internals de l'ancien MSVCRT** (heap-tracking /
  exit), **non caractérisé comme général** et **non convergent** en une session. Les deux (recovery + ce fix)
  restent un **chantier couplé reverté**. Borné (doc §2 « borner puis pivoter »).
- **Pour la reprise** : pistes non épuisées — tracer *qui écrit* `v146+0x18` (le write trouvé
  `*(v21+0x18)=((v1&~0x18)>>3)+0x407310` pointe une table globale `0x407310` ; vérifier si ce write est atteint
  pour la structure de `v146`), et comparer l'état à Wine au même point. Possiblement lié au filet x87 ou à la
  chaîne SEH d'exit (`fs:0`).

### 2026-07-11 — [RECOV][DEMO] finger (fin) : trace-write → **champ jamais initialisé** (lowio CRT), borné proprement
- **Angle trace-write appliqué** (watchpoint gdb sur `v146+0x18` = 0x82169a8) → **le watchpoint ne se déclenche
  JAMAIS** : le champ n'est **jamais écrit** de tout le run. Donc **init manquante**, pas corruption.
- **Valeurs concrètes au crash** (handle en cours = **1 = stdout**) : `*(0x40730c)`=32 (nb max de handles),
  `__pioinfo[0]`=**0x083b51a0 (valide)**, `v146+0x18`=**0**. La fonction `sub_403a90` = lookup lowio
  **handle→ioinfo** (`v1` vérifié < 32, puis `*(v186+v185+4)&0x40` = flag `_osfile` FDEV). Le champ `+0x18`
  devrait valoir **0x407310** (adresse de la table `__pioinfo`, constante ; le write candidat
  `*(v21+0x18)=((h&~0x18)>>3)+0x407310` → 0x407310 pour h=1) — mais cette écriture ne touche **pas** la structure
  lue (`v21`≠`v146`, deux emplacements pile distincts).
- **Diagnostic** : au **cleanup de sortie** (term `_initterm`, flush de stdout), le code lowio lit une structure
  dont le champ `__pioinfo`-ptr n'a **jamais été initialisé** — bien que `__pioinfo[0]` global soit valide (stdio a
  marché : bannière imprimée). C'est une **interaction subtile** entre le lowio du CRT statique lifté de finger et
  le modèle stdio/fichier d'ARET (ARET intercepte l'I/O par nom ; la structure de suivi que le flush-de-sortie
  parcourt n'est que partiellement peuplée).
- **Verdict (borné proprement, comme convenu)** : recovery `_initterm` = **avancée générale prouvée** (fait
  tourner le `main`). Le crash restant = **init lowio manquante**, caractérisée définitivement (champ jamais
  écrit) mais **non résolue** — fixer exactement demande de rétro-ingénierer la séquence d'init lowio du CRT
  spécifique de finger × le modèle I/O d'ARET. **Généralité incertaine** (CRT statique daté). Reverté ; couplé.
- **Régression complète PASS** (session verrouillée : difftest 271/271, funcdiff 0 div, sweeps, SMT, recompile).
- **Suite** : décision **Winelib** (router GDI/USER32/stdio vers Wine natif = bit-identique, la seule voie *sound*
  pour le GUI-texte sans substitution) — à cadrer.

### 2026-07-11 — [GUI][ORACLE] Spike **FreeType autonome** : glyphe bit-identique à Wine (débloque le texte SANS Wine)
- **Décision** : rester **autonome** (pas de dépendance runtime Wine). Question : piquer chez Wine sans en dépendre ?
- **Spike concluant** : un programme **`-m32`** liant **FreeType** (lib standard, i386 `.so` présent) rasterise le
  glyphe `'A'` (DejaVu Sans, 16px, mono/hinté) → **bitmap 11×12 STRICTEMENT identique** à celui du `TextOut` de
  Wine (mêmes réglages, `NONANTIALIASED_QUALITY`), vérifié par crop bbox + diff programmatique (`IDENTICAL: True`).
- **Pourquoi ça marche** : **Wine *utilise* FreeType** pour le rendu de police. Donc en liant FreeType
  **nous-mêmes** (statique → ELF autonome ; FreeType compile aussi en **WASM** → universalité préservée), on
  obtient le **même glyphe que Wine, bit-à-bit**, avec la **vraie police** (original, pas substitution) — et
  **winediff reste un oracle valide** (on partage le même rasterizer que la vérité-terrain Wine).
- **Ce que ça valide** (doctrine §1 « réutilisation vérifiée », au niveau *brique* pas *runtime*) : le mur du
  **texte GDI**, réputé « pas d'oracle pixel propre », est en fait **franchissable en restant autonome** — le
  point dur (le rasterizer) = FreeType, réutilisable. Reste à bâtir le **pipeline GDI texte** autour (mapping
  logical-font→face comme Wine, positionnement/advances de `TextOut`, application couleur/bkmode, table de
  substitution de polices de Wine pour choisir la MÊME face) — tout **déterministe et documenté dans la source Wine**.
- **Méthode générale émergente** : pour chaque mur profond (texte, lowio/finger, SEH), **miner la source Wine**
  (le « corrigé ») + **réutiliser ses briques autonomes** (FreeType) ou **porter sa logique** dans notre HLE.
  Binaires **autonomes + universels + bit-exacts**, sans dépendre de Wine au runtime.
- **Reste à cadrer** : static-link i386 `libfreetype.a` (autonomie totale), licences (FreeType FTL ok en static ;
  code porté de Wine = LGPL, à isoler), et le pipeline GDI-texte complet. **Aucun code produit** (spike scratchpad).

### 2026-07-12 — [GUI][HLE-WIN32] M7 G3-texte : `TextOut` FreeType **bit-identique à Wine**, autonome (1ʳᵉ marche texte)
- **Suite du spike FreeType** (2026-07-11) : le spike prouvait qu'un glyphe isolé matchait Wine ; ici on livre le
  **pipeline GDI-texte** intégré dans le HLE, vérifié bout-en-bout, binaire **autonome** (pas de runtime Wine).
- **Recette Wine minée + reproduite** (mesurée sur Wine puis répliquée octet-à-octet) :
  - **Sélection de police** : `fontconfig` (le mécanisme même de Wine sous Linux) résout le nom logique → fichier.
    Mesuré : Wine `TextOut("Arial")` == Liberation Sans == réponse fontconfig (bit-identique). (Les noms raster
    legacy « MS Sans Serif » où Wine substitue autrement = incrément suivant, table de substitution Wine.)
  - **Ligne de base** : `tmAscent = (FT_MulFix(usWinAscent, y_scale) + 32) >> 6` — Wine lit **`OS/2.usWinAscent`**
    (pas l'ascender `hhea` que renvoie `size->metrics.ascender`). Vérifié : DejaVu **et** Liberation matchent Wine
    au pixel (Liberation divergeait de 1px avec la formule hhea → corrigé par usWinAscent).
  - **Raster mono** : `FT_LOAD_RENDER|FT_LOAD_TARGET_MONO`, glyphe posé `penx+bitmap_left, baseline-bitmap_top`,
    avance `advance.x>>6` ; fond `TRANSPARENT` (pixels de fond intacts), couleur texte = `text_color` (DIB `[B,G,R,0]`).
- **Sous-ensemble prouvé exact** (le reste = **abort sound**, jamais un rendu faux silencieux) : `NONANTIALIASED_QUALITY`
  (mono), poids régulier droit, fond `TRANSPARENT`, alignement `TA_TOP|TA_LEFT`, cible DIB 32bpp, face résoluble par
  fontconfig. Antialiasing / gras-italique / fond opaque (fill du cell) / alignements / stock font (SYSTEM_FONT sans
  face) = incréments suivants, chacun vérifié vs Wine avant d'être shippé.
- **Fait** : `aret_win32.c` — `CreateFontA/W` + `CreateFontIndirectA/W` parsent le LOGFONT (height/weight/italic/
  quality/face) sur l'objet GDIT_FONT ; renderer FreeType+fontconfig (`#ifdef ARET_HAVE_FREETYPE`, cache de faces) ;
  `TextOutA/W`. `builder/mod.rs` — `freetype_flags()` (pkg-config cflags + sonames i386 `-l:libfreetype.so.6`/
  `-l:libfontconfig.so.1` liés explicitement, car pas de symlink `-dev` i386), gaté sur un import texte + `-DARET_HAVE_FREETYPE` ;
  **dégradation propre** (compile/link byte-identiques) si pas d'import texte, pas de libs, wasm, ou host sans FreeType →
  `TextOut` reste un abort sound. `stdcall_pops` : `TextOutA/W` = 20.
- **Autonomie préservée** : FreeType est **lié dans l'ELF** (comme SDL2 ; statiquement liable ensuite pour l'autonomie
  totale, compile aussi en WASM). Le binaire porte les **vrais glyphes** (police originale, pas substitution) ; **aucune
  dépendance runtime Wine** — Wine reste seulement l'**oracle** (winediff), jamais un composant du produit.
- **Oracle** : `winecorpus/gdi_textout.c` — `CreateFontA(-16, DejaVu Sans, NONANTIALIASED)` → `TextOutA` dans un DIB
  32bpp → carte ASCII des pixels + hash FNV du buffer → **bit-identique à Wine** (bbox `3 6 92 19`, `hash=79741f6c`).
- **Test intégré bout-en-bout** (`winecorpus/gui_paint_text.c`) : **une vraie appli fenêtrée** —
  `RegisterClassA` → `CreateWindowExA(WS_VISIBLE)` → boucle de messages → `WM_PAINT` dispatché au WndProc
  **lifté** → `BeginPaint`/`CreateFontA`/`SelectObject`/`TextOutA` (raster FreeType dans le framebuffer client de
  la fenêtre SDL) → `EndPaint` → relecture `GetPixel` — rend **bit-identique à Wine** (`text_pixels=186`,
  `bbox=4,7,76,18`), sous Xvfb (Wine) / SDL réel (ARET). **Toute la chaîne GUI marche** : fenêtre + messages +
  peinture + texte GDI, en ELF natif autonome, sans runtime Wine. Ajouté comme fixture de régression permanente.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest-transpile 4/4, `table_is_sorted` vert,
  winediff **84→86/86** (gdi_textout + gui_paint_text). **Additif** : les binaires sans texte ne lient pas FreeType → byte-identiques.

### 2026-07-12 — [GUI][HLE-WIN32] Texte GDI (suite) : **fond opaque + APIs de mesure** (cœur métriques partagé)
- **Objectif** : compléter le texte GDI (pas juste un sous-ensemble). 1ʳᵉ tranche après le mono transparent :
  **fond opaque** (défaut Windows) + **GetTextExtentPoint32** (les programmes mesurent pour placer leur texte).
  Une brique — le **cœur métriques** — tombe les deux (et prépare les alignements).
- **Recette Wine minée** (mesurée puis répliquée) :
  - **Métriques** : `tmAscent=(FT_MulFix(usWinAscent,ys)+32)>>6`, `tmDescent` idem avec `usWinDescent`,
    `tmHeight=asc+desc`. DejaVu -16 → 15/4/19 = **identique à `GetTextMetrics` de Wine**.
  - **Deux régimes d'avance distincts** (découverte clé) : le **rendu** avance au glyphe **mono** (`advance>>6`
    après `FT_LOAD_TARGET_MONO`), mais l'**extent / largeur du fond opaque** = somme des avances **`FT_LOAD_DEFAULT`**
    (mesuré : `'Hi!'` extent **22** mais pen mono **21** ; `default-sum` matche Wine sur 4 chaînes : 22/35/98/27).
  - **Fond opaque** : remplir le rectangle du cell `[x,y .. x+extentWidth, y+tmHeight]` avec `bkColor` **avant**
    les glyphes (mesuré : origine (10,6) → fill x[10..31]=22, y[6..24]=19). Ink dessiné par-dessus.
- **Fait** (`aret_win32.c`) : helper partagé `u32_dc_font` (résout face+taille+asc/desc, gates communs) +
  `u32_text_width` (avances default) ; `TextOut` gère `OPAQUE` (remplit le cell) ; `GetTextExtentPoint32A/W` +
  `GetTextExtentPointA/W`. `stdcall_pops` : les 4 extent = 16. `builder` : gate texte élargi aux extent/metrics.
- **Reste (abort sound)** : antialiasing, gras/italique, alignements ≠ TA_TOP|TA_LEFT, stock font, `GetTextMetrics`
  (struct complète), substitution legacy, Unicode, DrawText/ExtTextOut — incréments suivants, chacun vérifié vs Wine.
- **Oracle** : `gdi_textout.c` étendu (extent `Hello, ARET!`=98×19 + draw `OPAQUE`) → bit-identique Wine.
- **Vérifié** : hash transpile **inchangé** `19acad982194bf07`, difftest-transpile 4/4, `table_is_sorted` vert,
  winediff **86/86**.

### 2026-07-12 — [GUI][HLE-WIN32] Texte GDI (suite) : **GetTextMetrics** struct complète bit-identique Wine
- **GetTextMetricsA/W** : struct `TEXTMETRIC` entière remplie, **chaque champ** matchant Wine (hash de struct
  identique sur DejaVu-16/Arial-16/DejaVu-24). Formules minées + vérifiées sur plusieurs polices/tailles :
  - `tmHeight=asc+desc`, `tmAscent/Descent` (usWin*), `tmInternalLeading=round(MulFix(usWinAsc+usWinDesc-EM,ys))`,
    `tmExternalLeading=round(MulFix(hhea.lineGap,ys))`, `tmAveCharWidth=round(MulFix(xAvgCharWidth,xs))`,
    `tmMaxCharWidth=round(MulFix(bbox.xMax-xMin,xs))`, `tmWeight=usWeightClass`.
  - `tmDigitizedAspectX/Y=96`, `tmOverhang=0`, `tmFirst/Last/Default/Break=30/255/31/32` (constantes ANSI),
    `tmPitchAndFamily` = `VECTOR|TRUETYPE|(pitch variable ? 0x01)|famille` (famille depuis `OS/2.sFamilyClass`,
    défaut FF_SWISS comme Wine) → **0x27** vérifié, `tmItalic` depuis le LOGFONT, `tmCharSet=0`.
- **Oracle** : `gdi_textout.c` étendu (hash de la struct `TEXTMETRIC`) → bit-identique Wine.
- **Vérifié** : hash transpile **inchangé**, difftest-transpile 4/4, `table_is_sorted` vert, winediff **86/86**.
- **Reste (abort sound)** : antialiasing, gras/italique, alignements, stock font, substitution legacy, Unicode,
  DrawText/ExtTextOut.

### 2026-07-12 — [GUI][HLE-WIN32] Texte GDI (suite) : **gras / italique** (vraies faces), synthèse = abort sound
- **Gras/italique** via fontconfig : `u32_dc_font` passe `bold=(weight>=700)` et `italic` à `ft_resolve_face` →
  fontconfig choisit la face grasse/italique (la même que Wine). `tmWeight` suit `usWeightClass` de la face
  résolue, `tmItalic` suit le LOGFONT. Vérifié **bit-identique à Wine** : gras (DejaVu-Bold), italique
  (Arial→LiberationSans-Italic), gras+italique (Arial→BoldItalic).
- **Découverte** : DejaVu Sans **n'a pas** de face italique → fontconfig retombe sur la régulière, mais **Wine
  synthétise** l'italique (cisaillement oblique, matrice spécifique). Rendre la régulière droite = **faux
  silencieux**. **Garde sound** : si gras/italique demandé mais la face résolue n'a pas le style réel
  (`FT_STYLE_FLAG_BOLD/ITALIC`), **abort** (« synthesized bold/italic pending ») — la synthèse exacte
  (embolden / matrice de shear de Wine) = incrément suivant vérifié.
- **Oracle** : `gdi_textout.c` étendu (dessin gras DejaVu + italique Arial). **Vérifié** : hash transpile
  inchangé, difftest-transpile 4/4, `table_is_sorted` vert, winediff **86/86**.
- **Reste (abort sound)** : antialiasing, alignements, stock font, substitution legacy, Unicode, DrawText/
  ExtTextOut, **synthèse gras/italique**.

### 2026-07-12 — [GUI][HLE-WIN32] Texte GDI (suite) : **alignements** (SetTextAlign) bit-identiques Wine
- **`SetTextAlign`** appliqué dans `TextOut` (règles mesurées sur Wine) : horizontal **LEFT** `penx=x`, **RIGHT**
  `penx=x-width`, **CENTER** `penx=x-width/2` (trunc) ; vertical **TOP** `baseline=y+ascent`, **BASELINE**
  `baseline=y`, **BOTTOM** `baseline=y-descent`. `width` = extent (avances default). Le fill opaque suit l'origine
  alignée. `TA_UPDATECP`/`TA_RTLREADING` = **abort sound**.
- **Oracle** : `winecorpus/gdi_textalign.c` (5 modes hashés) → bit-identique Wine. winediff **86→87/87**.
- **Vérifié** : hash transpile inchangé, difftest-transpile 4/4, `table_is_sorted` vert.
- **Reste (abort sound)** : antialiasing, stock font, substitution legacy, Unicode, DrawText/ExtTextOut, synthèse
  gras/italique, TA_UPDATECP.

### 2026-07-12 — [GUI][ORACLE] Texte GDI : **antialiasing = mur dur** (mesuré non-trivial), reste abort sound
- **Mesuré** : le rendu antialiasé (`DEFAULT_QUALITY`) de Wine **ne matche AUCUN** `FT_LOAD_TARGET_*` naïf
  (NORMAL/LIGHT/NO_HINTING/FORCE_AUTOHINT) — comparé pixel-à-pixel sur 'A' 16px. Deux écarts : (1) **position**
  sous-pixel (le glyphe de Wine est 1px à gauche de tous les variants FT), (2) **couverture** différente (bords
  « plus durs » : Wine `247 32` vs FT `255 107`). C'est le **pipeline de rasterisation** de Wine (arrondi d'origine
  + hinting + cache de glyphes), pas juste des load flags.
- **Contraste avec le mono** (qui matche au bit) : le mono arrondit au pixel entier (grille 1-bit déterministe,
  identique des deux côtés) ; l'AA a de la couverture sous-pixel où le pipeline exact de Wine diverge d'un rendu FT
  direct. **Bit-exactitude AA = problème de recherche** (répliquer la config rasterizer exacte de Wine), pas un
  minage rapide.
- **Verdict (principe sacré)** : l'AA **reste un abort sound** (`quality != NONANTIALIASED` → abort) — on ne shippe
  **pas** un AA non-conforme. Borné, documenté. Les programmes qui demandent `NONANTIALIASED_QUALITY` ont un texte
  correct ; les autres s'arrêtent proprement. Reprise possible : tracer la config FreeType exacte de Wine
  (`dlls/win32u/freetype.c` : load flags par GASP, origine sous-pixel, éventuel gamma).

### 2026-07-12 — [GUI][HLE-WIN32] Texte GDI (suite) : **Unicode `TextOutW`** (codepoints réels) bit-identique Wine
- **Refactor** : le cœur texte (`u32_textout_core`, `u32_text_width`, `u32_text_extent`) prend désormais des
  **codepoints** (`uint32_t`) au lieu d'octets. `TextOutW`/`GetTextExtentPoint32W` passent le **vrai codepoint**
  UTF-16 (BMP) à `FT_Load_Char` (cmap de la police) au lieu de tronquer au low-byte. `TextOutA` inchangé (octet =
  codepoint Latin-1 == CP1252 pour 0x00-0x7F et 0xA0-0xFF ; 0x80-0x9F = suite).
- **Vérifié bit-identique Wine** : accentué latin (é è ü ñ) **et** grec (α β γ) via `TextOutW`, + `extentW`.
  ASCII non régressé. Surrogates (hors BMP) = suite.
- **Oracle** : `winecorpus/gdi_textunicode.c` (accent + grec hashés + extentW). winediff **87→88/88**.
- **Vérifié** : hash transpile inchangé, difftest-transpile 4/4, `table_is_sorted` vert.
- **Reste (abort sound)** : antialiasing (mur dur), stock font, substitution legacy, DrawText/ExtTextOut, synthèse
  gras/italique, TA_UPDATECP, TextOutA 0x80-0x9F (CP1252), surrogates.

### 2026-07-12 — [GUI][HLE-WIN32] Texte GDI (suite) : **ExtTextOutA/W** (options + rect + lpDx) bit-identique Wine
- **ExtTextOutA/W** : le cœur `u32_textout_full` gagne 3 paramètres (`dx`, `rect`, `do_opaque`, `do_clip`) ;
  `TextOut` = cas dégénéré. Recette Wine mesurée : **ETO_OPAQUE** remplit le **rect explicite** `[l,r)×[t,b)` avec
  bkColor (indépendant du fill de cell du bkmode OPAQUE) ; **lpDx** impose l'avance par glyphe ; **ETO_CLIPPED**
  restreint les pixels au rect. `options & ~(ETO_OPAQUE|ETO_CLIPPED)` (glyph-index/PDY/RTL/numeric) = **abort sound**.
- **Vérifié bit-identique Wine** : plain (==TextOut), ETO_OPAQUE (fill x[5..59] y[4..23]), lpDx=12.
- **Oracle** : `winecorpus/gdi_exttextout.c` (3 cas hashés). winediff **88→89/89**. `stdcall_pops` ExtTextOut=32.
- **Vérifié** : hash transpile inchangé, difftest-transpile 4/4, `table_is_sorted` vert.
- **Reste (abort sound)** : antialiasing (mur dur), DrawText, stock font, substitution legacy, synthèse gras/italique.

### 2026-07-12 — [GUI][HLE-WIN32] Texte GDI (suite) : **DrawTextA/W** (single-line) + murs durs identifiés
- **DrawTextA/W** single-line, bit-identique à Wine : mise en page dans le rect (règles mesurées) — horizontal
  `DT_LEFT/CENTER/RIGHT`, vertical `DT_TOP/VCENTER/BOTTOM` (VCENTER arrondit au sup `(rh-th+1)/2`), `DT_CALCRECT`
  (mesure : rect → `{l,t,l+w,t+h}`). Valeur de retour = hauteur du texte, ou offset top→bas du texte pour
  VCENTER/BOTTOM. Bâti sur le cœur métriques + `u32_textout_full` (clip au rect sauf `DT_NOCLIP`). **Abort sound** :
  multi-ligne/word-break, tabs, ellipsis, préfixe `&` sans `DT_NOPREFIX`. Vérifié 4 cas (retour+rect+pixels).
- **Oracle** : `winecorpus/gdi_drawtext.c`. winediff **89→90/90**. `stdcall_pops` DrawText=20.
- **Murs durs identifiés (mesurés, restent abort sound — non shippables bit-exact sans recherche)** :
  - **Antialiasing** (cf. entrée dédiée) : pipeline rasterizer de Wine.
  - **Stock fonts** : `GetStockObject(DEFAULT_GUI_FONT)` ≠ `CreateFont(-11,"MS Shell Dlg")` ≠ DejaVu-11 (3 hashes
    distincts) → le rendu stock de Wine ne se réduit pas à un resolve fontconfig.
  - **Substitution legacy** : à -16, Wine mappe **toutes** les faces indispo (MS Sans Serif/Shell Dlg/System/
    Courier/Tahoma/Helv/Arial) vers **le même** défaut (Liberation Sans, `cf4e610c`) ≠ réponse per-nom de
    fontconfig (MS Sans Serif→DejaVu). **Dépend de l'environnement** (fontes installées dans le prefix Wine).
  Ces trois = comme l'AA, un chantier de recherche (registre interne Wine / config rasterizer), pas un minage rapide.
- **Vérifié** : hash transpile inchangé, difftest-transpile 4/4, `table_is_sorted` vert.

### 2026-07-12 — [GUI][HLE-WIN32] **Mur dur #1 franchi : antialiasing = LCD subpixel** (bit-identique Wine)
- **Percée par le forensics Wine** (trace `WINEDEBUG=+font`) : deux révélations —
  1. `is_subpixel_rendering_enabled: subpixel rendering is enabled` → l'AA de Wine (`DEFAULT_QUALITY`) est du
     **rendu LCD subpixel (ClearType)**, PAS du grayscale. C'est pour ça qu'aucun `FT_LOAD_TARGET_*` grayscale ne
     matchait (mesuré : R≠G≠B dans le DIB de Wine).
  2. `height -16 => ppem 16` (le ppem était bon ; le « 14 » vu au début était une police positive-height).
- **Recette exacte reproduite** (glyphe 'A' → **chaque valeur RGB identique à Wine**) : `FT_Library_SetLcdFilter(
  FT_LCD_FILTER_DEFAULT)` (le filtre par défaut de Wine) + `FT_Load_Char(FT_LOAD_RENDER|FT_LOAD_TARGET_LCD)` →
  bitmap 3× large (R,G,B par pixel) ; blend par canal `dst=(fg*cov+old*(255-cov)+127)/255` **vérifié sur 3 cas
  colorés** (rouge/blanc, noir/bleu clair : R/G/B au bit près).
- **Mapping qualité→mode mesuré** : `NONANTIALIASED`→mono, `ANTIALIASED`→**grayscale** (pipeline distinct, encore
  abort sound), `DEFAULT/DRAFT/PROOF/CLEARTYPE`→**subpixel LCD**. Implémenté : le renderer branche mono/gray/LCD ;
  gray abort proprement (bords plus durs que NORMAL/LIGHT, ni la moyenne LCD — reste à cerner).
- **Vérifié bit-identique Wine** : subpixel 'A' seul, `Hello, ARET!` multi-glyphe (avances correctes), coloré,
  ClearType, descendantes `gyjpq`. Le prefix Wine frais du harness a le subpixel **activé** → l'oracle valide.
- **Oracle** : `winecorpus/gdi_textaa.c` (4 rendus subpixel hashés). winediff **90→91/91**.
- **Vérifié** : hash transpile inchangé, difftest-transpile 4/4, `table_is_sorted` vert.
- **Note env** : subpixel-on/off suit le réglage de font-smoothing du système (comme Wine) ; l'oracle le vérifie.
  Reste : **grayscale `ANTIALIASED_QUALITY`** (pipeline Wine distinct), + les 2 autres murs durs (stock/substitution).

### 2026-07-12 — [GUI][HLE-WIN32] **Murs durs #2/#3 : substitution UI-sans + stock fonts** (bit-identique Wine)
- **Débloqué par le subpixel** : le « mismatch stock font » d'avant était une **confusion mono vs subpixel**
  (les stock fonts utilisent `DEFAULT_QUALITY` = subpixel). Une fois la qualité alignée :
  `DEFAULT_GUI_FONT` == `CreateFontIndirect(son LOGFONT)` == `CreateFont("Liberation Sans")` (tous `408911be`).
- **Forensics substitution** (`GetTextFaceA` + trace) : Wine mappe **toutes** les faces UI sans indispo
  (MS Sans Serif, MS Shell Dlg, Tahoma, System, Helv, Segoe UI…) → **Liberation Sans** (le remplaçant
  métrique-compatible). fontconfig seul les route vers son défaut générique (DejaVu) → **divergence mesurée**
  (Wine `4640d291` vs ARET-avant `a06270be`) = faux silencieux pour les faces exactes qu'utilisent les vraies GUI.
- **Fait** :
  - **Table de substitution UI-sans** (`u32_face_subst`) : {MS Sans Serif, MS Shell Dlg(/2), Microsoft Sans Serif,
    Tahoma, Helv, Helvetica, System, Segoe UI, Arial} → « Liberation Sans » (comme Wine ; sérif/mono legacy gardent
    la réponse *correcte* de fontconfig — pas d'overfit au quirk « tout→sans » de ce prefix minimal). → **match Wine**.
  - **Hauteur positive→ppem** (`u32_dc_font`) : `ppem = round(height·upm/(winAsc+winDesc))` (Wine : height 16 →
    ppem 14 pour Liberation Sans ; tmHeight résultant = hauteur demandée). La négative reste `|height|`.
  - **Stock fonts** : `GetStockObject(DEFAULT_GUI_FONT)` = « MS Shell Dlg » -11, `ANSI_VAR_FONT` = « MS Sans Serif »
    12 → LOGFONT assigné, rendu subpixel **bit-identique à Wine**. `SYSTEM_FONT`/fixed/OEM (« System »/« Courier »
    bitmap legacy, rendu spécial Wine) = **pas de face → abort sound**.
- **Oracle** : `winecorpus/gdi_uifont.c` (4 faces UI-sans + 2 stock fonts hashés) → bit-identique Wine.
- **Vérifié** : hash transpile inchangé, difftest-transpile 4/4, `table_is_sorted` vert, winediff (voir chiffre).
- **Bilan des 3 murs durs** : **antialiasing** (subpixel LCD) ✅, **substitution UI-sans** ✅, **stock fonts**
  (DEFAULT_GUI_FONT/ANSI_VAR) ✅. Restes bornés (abort sound) : grayscale `ANTIALIASED_QUALITY`, sérif/mono legacy
  sur ce prefix quirk, SYSTEM_FONT bitmap, synthèse gras/italique.

### 2026-07-12 — [GUI][HLE-WIN32] Texte GDI : **DrawText multi-ligne** (word-wrap + `\n`) bit-identique Wine
- **DrawText multi-ligne** (hors `DT_SINGLELINE`) : découpe sur `\n` (sauts durs) + **word-wrap `DT_WORDBREAK`**
  (glouton : casse au dernier espace qui garde la ligne ≤ largeur du rect ; un mot seul trop long casse au
  caractère). Chaque ligne dessinée à `rc.top + i·tmHeight`, alignée (LEFT/CENTER/RIGHT) ; l'alignement vertical
  ne s'applique pas au multi-ligne (Win32). Recette Wine mesurée :
  - **Retour DRAW** = hauteur des lignes qui **démarrent dans le rect** (`top < rc.bottom`) ×`tmHeight` (les lignes
    au-delà sont comptées hors, mais dessinées clippées) ; **DT_CALCRECT** = toutes les lignes, rect → `{l, t,
    l+largeurMax, t+total}`.
  - **Espaces de fin exclus** de la largeur d'une ligne (pour CALCRECT et l'alignement centre/droite).
- **Vérifié bit-identique Wine** (5 cas : wordbreak, wb|calc, `\n` explicite, wb|center, wb|right — retour + rect +
  pixels). Oracle `winecorpus/gdi_drawtext_ml.c`.
- **Vérifié** : hash transpile inchangé, difftest-transpile 4/4, `table_is_sorted` vert, winediff (voir chiffre).
- **Note widgets** : mesuré que `WM_PRINTCLIENT` sur un STATIC headless Wine ne peint rien → l'oracle de peinture
  des widgets n'est **pas viable** headless (nécessite un cycle `WM_PAINT` sur fenêtre visible, fragile). Les
  widgets natifs = chantier à oracle fragile, reporté ; on complète le texte (oracle propre) d'abord.

### 2026-07-12 — [GUI][HLE-WIN32] Texte GDI : **soulignement / barré** (lfUnderline / lfStrikeOut) bit-identique Wine
- **lfUnderline / lfStrikeOut** : barres pleines couleur-texte couvrant l'**extent** du texte (avances default,
  pas l'avance mono du pen — diffèrent d'~1px aux petites tailles ; avec lpDx la barre suit le pen dx).
- **Positions minées via `GetOutlineTextMetrics`** (valeurs autoritaires de Wine, vérifiées sur ppem 16/24/32) :
  - soulignement : haut = `baseline - round(MulFix(post.underline_position + underline_thickness/2, ys))`
    (= `otmsUnderscorePosition` : 0/0/-1) ; épaisseur `round(MulFix(underline_thickness, ys))` min 1.
  - barré : haut = `baseline - round(MulFix(OS/2.yStrikeoutPosition, ys))` ; épaisseur idem, min 1.
- **Champs LOGFONT** : `lf_underline`/`lf_strikeout` ajoutés à l'objet FONT, parsés dans CreateFontA/W (WU6/WU7)
  et CreateFontIndirectA/W (offsets 21/22).
- **Vérifié bit-identique Wine** (4 cas : underline -16/-32, strikeout -16, both -24). Oracle `gdi_underline.c`.
- **Vérifié** : hash transpile inchangé, difftest-transpile 4/4, `table_is_sorted` vert, winediff (voir chiffre).

### 2026-07-12 — [GUI][HLE-WIN32] Texte GDI : **DrawText préfixe `&`** (souligné d'accélérateur) bit-identique Wine
- **Préfixe `&`** (sauf `DT_NOPREFIX`) : un `&` seul est retiré et marque le caractère suivant comme
  **accélérateur souligné** ; `&&` = `&` littéral ; un `&` final est ignoré. Prétraitement dans `u32_drawtext`
  (single-line ; accélérateur en multi-ligne = abort, rare).
- **Souligné d'accélérateur = trait au pen de Wine** (distinct du soulignement de police lfUnderline) : **1px**
  d'épaisseur à **`baseline + 1`**, s'étendant sur l'**extent du caractère moins un** (`LineTo` exclut son point
  final) — formules mesurées sur 3 tailles (`row=baseline+1`, `w=ext-1`).
- **Vérifié bit-identique Wine** (5 cas : `&File`, DT_NOPREFIX, `File`, `Save &As`, `A && B`). Oracle
  `gdi_drawtext_amp.c`.
- **Vérifié** : hash transpile inchangé, difftest-transpile 4/4, `table_is_sorted` vert, winediff (voir chiffre).
- **Reste texte (abort sound)** : grayscale ANTIALIASED, tabs/ellipsis DrawText, synthèse gras/italique, sérif/mono
  legacy, SYSTEM_FONT bitmap, surrogates, CP1252 0x80-0x9F.

### 2026-07-12 — [GUI][HLE-WIN32] Texte GDI : **codepage ANSI CP1252** (TextOutA 0x80-0x9F) bit-identique Wine
- **`TextOutA` (et ExtTextOut/DrawText/extent ANSI)** mappent l'octet → codepoint Unicode via **CP1252** (l'ACP
  Windows ; vérifié `GetACP()==1252`). 0x00-0x7F et 0xA0-0xFF = Latin-1 ; **0x80-0x9F = les slots CP1252**
  (€, guillemets courbes, tirets, ™, œ…) → leur vrai codepoint, pas Latin-1. Slots indéfinis (0x81/0x8D/0x8F/0x90/
  0x9D) → valeur de l'octet (comme MultiByteToWideChar). Helper `u32_ansi_cp` partagé par tous les chemins ANSI.
- **Vérifié bit-identique Wine** : `TextOutA(0x80,0x92,0x99…)` == `TextOutW(0x20AC,0x2019,0x2122…)`. Oracle
  `gdi_cp1252.c`.
- **Vérifié** : hash transpile inchangé, difftest-transpile 4/4, `table_is_sorted` vert, winediff (voir chiffre).
- **Milestone texte GDI — état** : le pipeline texte est **très complet, tout bit-identique à Wine, autonome** :
  mono/subpixel(ClearType)/opaque/aligné/Unicode/CP1252/souligné-barré ; TextOut/ExtTextOut/DrawText(mono+multi-
  ligne+`&`+CALCRECT)/GetTextExtentPoint32/GetTextMetrics ; gras-italique réels/substitution UI/stock fonts/
  hauteurs ±. **Restes bornés (abort sound, niche ou pipeline Wine distinct)** : grayscale ANTIALIASED_QUALITY,
  DrawText tabs/ellipsis, synthèse gras/italique, sérif/mono legacy (quirk prefix), SYSTEM_FONT bitmap, surrogates.

### 2026-07-12 — [GUI][HLE-WIN32] GDI vectoriel : **MoveToEx / LineTo** (Bresenham) bit-identique Wine
- **Pivot d'oracle** : widgets natifs = **pas d'oracle viable headless** (mesuré : ni `WM_PRINTCLIENT`, ni la
  peinture d'un contrôle enfant dans une fenêtre visible ne sont lisibles via `GetPixel` sous Xvfb) ; **SEH** =
  binaires de test non constructibles ici (mingw i686 ne compile pas `__try/__except`, modèle DWARF/SjLj). Le
  chantier propre-et-vérifiable restant = le **GDI vectoriel** (oracle DIB-hash, comme `gdi_dib`).
- **`MoveToEx`/`LineTo`/`GetCurrentPositionEx`** : `LineTo` = **Bresenham entier** de la position courante vers
  (x,y), **point final exclu** (sémantique GDI, cohérent avec le trait d'accélérateur), couleur du stylo
  sélectionné. Algorithme vérifié = Bresenham standard (`err=2·d_mineur−d_majeur`, seuil `>0`), tracé colonne par
  colonne exactement comme Wine. Position courante suivie sur le DC (`cur_x/cur_y`).
- **Stylo** : `gdi_pen` — solide largeur ≤1 seulement ; `PS_NULL` ne dessine rien ; styles/largeurs > 1 = **abort
  sound** (rasterisation exacte = suite). `CreatePen` stocke style/largeur.
- **Vérifié bit-identique Wine** : 6 lignes multi-octants (peu/très pentues, dx<0, verticale, horizontale, stylo
  coloré) + position courante. Oracle `gdi_lineto.c`. `stdcall_pops` : LineTo=12, MoveToEx=16, GetCurrentPositionEx=8.
- **Vérifié** : hash transpile inchangé, difftest-transpile 4/4, `table_is_sorted` vert, winediff (voir chiffre).
- **Suite GDI vectoriel** : Polyline/PolylineTo, Ellipse.

### 2026-07-12 — [GUI][HLE-WIN32] GDI vectoriel : **Rectangle** (bord stylo + remplissage pinceau) bit-identique Wine
- **`Rectangle(hdc,l,t,r,b)`** : intérieur `[l+1,r-1)×[t+1,b-1)` rempli du **pinceau** sélectionné ; contour
  `[l,r-1]×[t,b-1]` tracé au **stylo** (1px). Sémantique mesurée sur Wine (bord au pixel r-1/b-1, pas r/b).
  `gdi_brush` : couleur du pinceau, 0 si NULL/HOLLOW (pas de remplissage). Réutilise `gdi_pen`.
- **Vérifié bit-identique Wine** : rempli (vert/stylo noir), creux (NULL_BRUSH/stylo rouge), blanc/noir. Oracle
  `gdi_rectangle.c`. `stdcall_pops` : Rectangle=20.
- **Vérifié** : hash transpile inchangé, difftest-transpile 4/4, `table_is_sorted` vert, winediff (voir chiffre).

### 2026-07-12 — [GUI][HLE-WIN32] GDI vectoriel : **Polyline** bit-identique Wine ; Ellipse = mur (abort sound)
- **`Polyline(hdc, pts, count)`** : count-1 segments Bresenham connectés au stylo (chaque point final exclu →
  sommets partagés dessinés par le départ du segment suivant, point final global non dessiné). Mesuré sur Wine
  (contour ouvert : le sommet est peint par le segment suivant). N'utilise/ne met pas à jour la position courante.
- **Vérifié bit-identique Wine** : contour de boîte ouvert + zig-zag multi-octant coloré. Oracle `gdi_polyline.c`.
  `stdcall_pops` : Polyline=12.
- **Ellipse mesurée = mur** : Wine trace un vrai midpoint-ellipse (régions plate + pentue) avec **centre
  demi-entier** pour les boîtes impaires ; la rasterisation exacte = un match niveau-recherche (comme le grayscale
  AA), pas un minage rapide. **Reste abort sound** (non implémenté → stub faible). Idem Polygon/RoundRect/Arc.
- **Vérifié** : hash transpile inchangé, difftest-transpile 4/4, `table_is_sorted` vert, winediff (voir chiffre).

### 2026-07-12 — [GUI][HLE-WIN32] GDI : **BitBlt raster-ops** (ROP3 binaires S,D) bit-identique Wine
- **`BitBlt`** gère désormais les ROP3 binaires (source,destination) courants comme combinaisons booléennes
  par pixel des pixels 32bpp (les 4 octets, alpha compris — vérifié match Wine) : `SRCCOPY`(D=S), `SRCAND`(S&D),
  `SRCPAINT`(S|D), `SRCINVERT`(S^D), `NOTSRCCOPY`(~S), `SRCERASE`(S&~D), `NOTSRCERASE`(~(S|D)), `MERGEPAINT`(~S|D),
  `DSTINVERT`(~D), `BLACKNESS`(0), `WHITENESS`(~0). Les ROP à motif (lisant le pinceau) = suite → **abort sound**.
- **Vérifié bit-identique Wine** : 8 ROPs (copy/and/paint/invert/notsrc/dstinv/black/white) sur deux DIB. Oracle
  `gdi_bitblt_rop.c`.
- **Vérifié** : hash transpile inchangé, difftest-transpile 4/4, `table_is_sorted` vert, winediff (voir chiffre).

### 2026-07-12 — [GUI][HLE-WIN32] GDI : **FrameRect / InvertRect / PolylineTo** bit-identiques Wine (fin du filon vectoriel/raster propre)
- **Mesuré sur Wine avant d'implémenter** (rien de deviné) via une sonde ASCII-map : chaque primitive a sa
  sémantique exacte capturée au pixel, puis prouvée par l'oracle DIB-hash.
- **`FrameRect(hdc, RECT*, hbrush)`** : bord **1px** sur l'outline `[l,r-1]×[t,b-1]` (mêmes bornes que le contour
  de `Rectangle`) mais de la couleur du **pinceau passé en argument** (pas le sélectionné — mesuré). Pinceau NULL
  → rien. Réutilise `gdi_brush_color`/`gdi_px`/`gdi_put`.
- **`InvertRect(hdc, RECT*)`** : XOR de **chaque pixel sur les 32 bits** (octet alpha inutilisé compris — vérifié
  `0x44112233 → 0xBBEEDDCC`, cohérent avec `DSTINVERT`) sur `[l,r)×[t,b)`. Opère sur les 4 octets bruts (pas
  `gdi_put` qui force alpha=0).
- **`PolylineTo(hdc, POINT*, count)`** : **suite de `LineTo`** depuis la position courante — un segment Bresenham
  vers chaque point (**point final exclu** par segment, donc les sommets partagés sont peints par le départ du
  segment suivant), **met à jour la position courante** au dernier point (mesuré `cur=(2,8)`). Diffère de
  `Polyline` (qui part de `pts[0]` et n'utilise/ne met pas à jour la position courante).
- **Vérifié bit-identique Wine** : fixture combinée `winecorpus/gdi_framerect.c` (FrameRect vert + InvertRect sur
  fond `112233` + PolylineTo boîte ouverte, `hash=af0530e1`, `cur=3,21`). `stdcall_pops` : FrameRect=12,
  InvertRect=8, PolylineTo=12 (triés, `table_is_sorted` vert).
- **Enregistrement automatique** : le builder découvre les `aret_x(uint32_t` du runtime → aucun table de dispatch
  à toucher, juste la définition C + le pop stdcall.
- **Vérifié** : hash transpile inchangé (`19acad982194bf07`), difftest-transpile 4/4, winediff **101/101**.
- **Bilan filon GDI vectoriel/raster** : LineTo/MoveToEx, Rectangle, Polyline, BitBlt-ROPs, FrameRect, InvertRect,
  PolylineTo = tout le sous-ensemble **proprement minable** (oracle DIB-hash exact) est **fait**. Reste **abort
  sound** (match niveau-recherche, pas un minage rapide) : Ellipse/Polygon/RoundRect/Arc (midpoint-ellipse à
  centre demi-entier), stylos larges/pointillés, ROP à motif. Prochain grand chantier = **fibers** (doc 80).

### 2026-07-16 — [THREAD][HLE-WIN32] Threads coopératifs (fibers) — incrément 1 : `CreateThread` + join, bit-identique Wine
- **Nouveau chantier (doc 80, priorité 1)**. `CreateThread` devient une **coroutine réelle** (`ucontext`/
  `swapcontext`) multiplexée sur l'unique thread hôte ; on ne bascule qu'aux **points bloquants** (Wait/Sleep) ⇒
  **zéro data-race**, ordonnancement **round-robin déterministe** ⇒ oracle différentiel reproductible.
- **Feasibilité re-mesurée** (règle « mesurer, pas affirmer ») avant d'écrire le vrai code : `ucontext`
  coopératif compile et tourne en **`-m32`** (le binaire produit d'un PE 32-bit est un ELF 32-bit natif),
  probe `counter=4000` déterministe. Feu vert.
- **Architecture** (`aret_win32.c`, `#ifndef __wasm__`) : table `g_fiber[64]` (fiber 0 = main), chacun a un
  **contexte hôte** (`ucontext_t` + pile C hôte 4 Mo malloc) et une **pile machine émulée dédiée** (1 Mo malloc,
  `esp` au sommet — le modèle shared-stack devient par-fiber sans toucher le lifter). Un **scheduler** (`ucontext`
  propre) boucle : réveille les fibers dont la condition d'attente est satisfaite, choisit le prochain READY
  round-robin, `swapcontext`. Aucun READY alors qu'un fiber est BLOCKED = **deadlock → abort sound**.
- **`CreateThread`** alloue les piles, `makecontext`→ trampoline qui pose la frame `__stdcall` du thread-proc
  (`param` @ `[esp+4]`) sur la pile machine et dispatche via **`aret_call(start, esp, …)`** (le proc lifté court
  sur la pile hôte du fiber) ; au retour, code de sortie + état DONE + `uc_link`=scheduler. `CREATE_SUSPENDED`
  +`ResumeThread`, `ExitThread`, `GetExitCodeThread` (STILL_ACTIVE=259 tant que vivant).
- **`WaitForSingle/MultipleObjects` = vrai join** : bloque le fiber courant sur l'ensemble de handles jusqu'à
  satisfaction (un handle de thread signale quand son fiber est DONE), en pilotant le scheduler. **timeout 0** =
  poll → `WAIT_TIMEOUT` (jamais bloquer) ; timeout fini non nul traité comme INFINITE (dans le modèle coopératif
  un thread runnable n'a pas d'horloge : il complète → `WAIT_OBJECT_0`, ou l'ensemble deadlock → abort sound —
  jamais un faux). `waitAll`→0, `waitAny`→index du signalé. Handles non-thread gardent l'immédiat legacy
  (`WAIT_OBJECT_0`, sound en mono-thread). `CloseHandle`(thread)=succès (référence seule).
- **`last_error` par-fiber** : `g_last_error` (aret_hle.c) rendu non-`static` ; le scheduler le **sauve/restaure**
  à chaque bascule (un seul fiber court ⇒ correct). `Sleep` devient un **yield** coopératif (`aret_fiber_yield`,
  retourne 0 sans thread ⇒ `usleep` inchangé en mono-thread). `SuspendThread` d'un thread en cours → abort sound.
- **WASM** : `ucontext` inexistant ⇒ `CreateThread` = **abort sound** (Asyncify plus tard), jamais divergence
  silencieuse (règle doc 80 §3). Branche `#else` fournie.
- **Additivité prouvée** : sans `CreateThread`, le scheduler ne démarre jamais, `last_error` n'est jamais swappé,
  `Sleep` garde son `usleep` ⇒ un programme mono-thread est **byte-identique**. winediff **101→102/102**, hash
  transpile inchangé (`19acad982194bf07`).
- **Vérifié bit-identique Wine** : `winecorpus/thread_join.c` (4 threads, `SetLastError` propre puis yield puis
  `GetLastError` = **isolation par-thread prouvée à travers un yield**, somme déterministe `25800` via slots
  privés, exit-codes `1..4`, `wait=0`). `stdcall_pops` : CreateThread=24, ExitThread=4, ResumeThread=4,
  SuspendThread=4, GetExitCodeThread=8 (triés). Découverte auto du shim par le builder (`aret_x(uint32_t`).
- **Suite (doc 80 §2)** : CriticalSection réelle (`counter=4000`), Events, Mutex/Semaphore/TLS/`_beginthreadex`.

### 2026-07-16 — [THREAD][HLE-WIN32] Fibers incrément 2 : `CRITICAL_SECTION` réelle (owner + récursion), oracle `counter=4000`
- **Suite du chantier fibers** (doc 80 §2, incr. 2). Les stubs CS (no-op *sound en mono-thread*) deviennent une
  **vraie exclusion mutuelle** au-dessus du scheduler coopératif.
- **Déplacement** : les 6 fns CS quittent `aret_hle.c` (où elles étaient no-op) pour `aret_win32.c`, à côté du
  scheduler qu'elles doivent piloter (le builder les redécouvre par `aret_x(uint32_t` ; zéro changement d'en-tête).
- **Modèle** : table `g_cs[256]` **keyée par le pointeur `&cs`** (la struct RTL_CRITICAL_SECTION reste opaque,
  comme chez Wine — on ne falsifie pas ses champs), portant `owner` (index fiber+1, 0=libre) et `rec` (profondeur
  de récursion de l'owner). Table pleine → **abort sound**.
- **`EnterCriticalSection`** : libre ou déjà à moi → prends (`owner=moi`, `rec++`) ; sinon **bloque** — le fiber
  pose `wait_cs=&cs`, passe BLOCKED, rend la main au scheduler ; le scheduler (prédicat `u32_fiber_runnable`
  étendu : runnable quand `owner==0`) le réveille quand la CS se libère, il **retente** l'acquisition. ⇒ un owner
  qui **yield en tenant le lock** (Sleep/Wait) garde bien les autres dehors. `TryEnter` = même prise sans blocage
  (0 si tenu par un autre). `Leave` = `rec--`, libère (`owner=0`) à 0, **réservé à l'owner** (un Leave d'un
  non-owner ne corrompt rien). `Initialize`(+AndSpinCount/Ex) enregistrent, `Delete` compacte (pointeurs stables).
- **WASM** (`#else`) : pas de threads ⇒ CS = **no-op correct** (jamais de contention), `InitializeCritical
  SectionAndSpinCount`→1 (sinon le CRT `__fastfail`).
- **Oracle discriminant** : `winecorpus/thread_critsec.c` — 4 threads × 1000, chacun `Enter; v=counter; Sleep(0);
  counter=v+1; …; Leave`. Le **`Sleep(0)` coupe le read-modify-write sous le lock** : sans exclusion vraie, un
  autre fiber lirait le même `v` pendant le yield → incréments perdus → `<4000`. Avec la CS correcte : **`4000`
  exact**, déterministe sur Wine (threads préemptifs réels) **et** ARET (fibers). + récursion (`Enter` imbriqué,
  `TryEnter` par l'owner = TRUE) → `rec_ok=1`. `stdcall_pops` : TryEnterCriticalSection=4 (les autres déjà là).
- **Vérifié** : hash transpile inchangé (`19acad982194bf07`), `table_is_sorted` vert, winediff **102→103/103**
  (les fixtures existantes utilisant CS restent bit-identiques ⇒ additivité préservée).
- **Suite** : Events (Set/Reset/Wait), puis Mutex/Semaphore/TLS/`_beginthreadex` (doc 80 §2, incr. 3-4).

### 2026-07-16 — [THREAD][HLE-WIN32] Fibers incrément 3 : Events (manual/auto-reset) — signalisation déterministe, bit-identique Wine
- **Suite du chantier fibers** (doc 80 §2, incr. 3). Les stubs event (CreateEventA→`0x101`, Set/Reset→1, no-op)
  deviennent de **vrais objets événement** intégrés au scheduler.
- **Table** `g_event[128]` (range handle `0x71……`) : `manual` (reste signalé jusqu'à `ResetEvent`) vs auto-reset
  (**libère un seul waiter puis se réarme**), `signaled`, `name_hash` (FNV). `CreateEventA/W(attrs, bManualReset,
  bInitialState, lpName)` ; nom non-NULL → partage intra-process (retour du handle existant + `ERROR_ALREADY_EXISTS`).
  `SetEvent`=`signaled=1`, `ResetEvent`=`signaled=0` (idempotents booléens, pas un sémaphore).
- **Intégration attente** : `u32_handle_signaled` reconnaît les events (signalé = `signaled`) ; `WaitForSingle/
  MultipleObjects` bloquent tant que non satisfait. **Point clé auto-reset** : `SetEvent` peut réveiller plusieurs
  waiters (tous marqués runnable par le scheduler), mais l'attente **re-vérifie la condition après chaque réveil**
  (`do{ block; yield; }while(!wait_ok)`) ⇒ seul celui qui la trouve encore vraie **consomme** l'event (auto-reset →
  `signaled=0`), les autres se re-bloquent ⇒ **exactement un libéré par SetEvent**. Consommation à la sortie de
  l'attente (waitAll : tous les auto-events ; waitAny : le premier signalé = l'index rendu). `WaitForMultiple`
  rend `WAIT_OBJECT_0+idx`.
- **Déplacement** : `CreateEventA/W`/`SetEvent`/`ResetEvent` définis avec le scheduler (`aret_win32.c`) ; anciens
  stubs retirés. WASM (`#else`) : events = fakes sound (pas de threads). Handles non-thread/non-event = immédiat legacy.
- **Soundness** : un Wait INFINITE sur un event jamais signalé (aucun signaleur possible) = deadlock → **abort
  sound** (mieux que l'ancien faux « succès immédiat »). Les fixtures existantes ne régressent pas (winediff).
- **Oracle** : `winecorpus/thread_event.c` — (1) **gate manual-reset** : 3 workers bloquent, un thread releaser
  `SetEvent` une fois → **tous** libérés (`gate_sum=60` ; s'il n'en libérait qu'un, le join deadlockerait → abort) ;
  (2) **ping-pong auto-reset** : producteur/consommateur alternent via 2 auto-events, handoff strict `pp_sum=15`.
  Déterministe sur Wine (préemptif) **et** ARET (coopératif). `stdcall_pops` : CreateEventW=16, SetEvent=4.
- **Vérifié** : hash transpile inchangé (`19acad982194bf07`), `table_is_sorted` vert, winediff **103→104/104**.
- **Suite (doc 80 §2, incr. 4)** : Mutex/Semaphore/`WaitForMultipleObjects(FALSE)`, TLS par-fiber, `_beginthreadex`.

### 2026-07-16 — [THREAD][HLE-WIN32] Fibers incrément 4 (clôture) : Mutex + Semaphore + TLS par-fiber + `_beginthread(ex)`, bit-identique Wine
- **Dernier incrément du chantier fibers** (doc 80 §2). Complète les primitives de synchronisation.
- **Modèle d'acquisition unifié** : `u32_handle_signaled` devient `u32_handle_signaled_for(h, fiber)` (le mutex
  dépend du fiber demandeur : libre / à moi / abandonné) + `u32_handle_acquire(h, me)` (effet consommateur). Ainsi
  `WaitForSingle/MultipleObjects` gèrent thread/event/**mutex/sémaphore** uniformément, avec la **re-vérification
  après réveil** (déjà en place) ⇒ un seul waiter consomme (mutex pris, sémaphore décrémenté, event auto-reset).
- **Mutex** (`0x72……`, `g_mutex[128]`) : `owner` (fiber+1) + `rec` (récursion), waitable (acquisition dans
  `u32_handle_acquire`). `CreateMutexA/W(attrs, bInitialOwner, name)` (owner initial = créateur), `OpenMutexA/W`
  (par nom), `ReleaseMutex` (owner only, `rec--`→ libère à 0). **Owner mort sans release = abandonné → traité
  comme libre** (pas de faux deadlock ; le distinguo `WAIT_ABANDONED` reste hors-scope).
- **Semaphore** (`0x73……`, `g_sem[128]`) : `count`/`max`. Signalé si `count>0`, wait décrémente. `CreateSemaphore
  A/W(attrs, initial, max, name)`, `ReleaseSemaphore(h, n, &prev)` (ajoute `n` plafonné à `max`, écrit l'ancien
  compte, `ERROR_TOO_MANY_POSTS` si dépassement), `OpenSemaphoreA/W`.
- **TLS par-fiber** (`aret_hle.c`) : `aret_tls[64][1088]` — l'**allocation d'index** reste process-globale
  (`aret_tls_used`, `TlsAlloc` zère la colonne dans tous les fibers), mais Get/Set indexent la **ligne du fiber
  courant** via `aret_current_fiber()` (exposé par `aret_win32.c`). Un thread frais voit tous ses slots à NULL.
- **`_beginthreadex`/`_beginthread`** (msvcrt, **cdecl** → pas de stdcall_pop) : `CreateThread` factorisé en
  `u32_spawn(start, param, flags, pTid)` ; `_beginthreadex(sec, stack, start, arg, flag, tid)` = même layout,
  `_beginthread(start, stack, arg)` = variante cdecl (le trampoline `param@[esp+4]` sert les deux ABI). ⚠️ Le nom
  d'import `_beginthreadex` **perd son underscore de tête** (`sanitize_import` trim `_`) → shim `aret_beginthreadex`
  (un seul underscore), pas `aret__beginthreadex` (piège attrapé au 1er run : hit du weak stub → deadlock).
- **WASM** (`#else`) : mutex/sem = fakes sound, `_beginthread(ex)` = abort sound (pas d'ucontext).
- **Oracle** : `winecorpus/thread_mutex_sem.c` — 4 workers via **`_beginthreadex`** : (1) **mutex** protège un
  compteur avec RMW **coupé par `Sleep(0)` sous le lock** → `mcounter=2000` (exclusion réelle) ; (2) **TLS
  par-fiber** : chacun stocke `id+100`, yield, relit = le sien → `tls_ok=1` (une TLS globale serait écrasée) ;
  (3) **sémaphore** producteur/consommateur `ssum=21`. `stdcall_pops` : CreateMutexA/W=12, OpenMutexA/W=12,
  ReleaseMutex=4, CreateSemaphoreA/W=16, OpenSemaphoreA/W=12, ReleaseSemaphore=12.
- **Vérifié** : hash transpile inchangé (`19acad982194bf07`), `table_is_sorted` vert, winediff **104→105/105**.
- **⇒ Chantier fibers (doc 80 §2) COMPLET** (incréments 1-4). Prochaine orientation doc 80 : lifting comctl32
  binaire (endgame GUI) ou PGL opt-in.

### 2026-07-16 — [THREAD][HLE-WIN32] Fibers incrément 5 : timeouts finis (horloge virtuelle) — débloqué par un VRAI test binaire
- **Déclencheur = « ça marche sur de vrais binaires ? »**. Deux mesures honnêtes :
  1. **`kernel32_test.exe`** (conformance WineHQ, extraite de `winetest -x`, 3 Mo) : ARET la **transpile** (ELF
     54 Mo) mais **aborte sound AVANT le 1er test `thread.c`** (0 ligne de test imprimée) sur `indirect call to
     unrecovered function 0x446680`. Diagnostic : `0x446680` est une vraie fonction atteinte **uniquement** via la
     table `{nom, func}` de winetest en `.rdata` (`… 80664400 …` = ptr `0x00446680`, **entrelacée** string-ptr/
     code-ptr) → non récupérée par le statique. **Mur points-to orthogonal** (P3/Phase-4), **pas** un bug threads —
     mais ça prouve qu'**aucun vrai binaire MT tiers n'a encore tourné bout-en-bout** ici.
  2. **Workload composé réaliste** (pool de threads : file + mutex + sémaphore + event manual-reset + TLS) : a
     **révélé un vrai gap** — le pattern `WaitForSingleObject(sem, 50)` (**timeout fini comme sonde de vivacité**,
     ultra-courant) dead-lockait, car le modèle traitait « fini == infini ». Wine, lui, **time-out** à 50 ms et
     les workers sortent. → abort sound, mais code réaliste inexécutable.
- **Fix (cause générale, pas rustine) — horloge virtuelle déterministe** : chaque `WaitFor*(h, ms)` / `Sleep(ms)`
  **fini** pose `wake_time = g_vclock + ms` (`has_timeout`). Quand le scheduler ne trouve **aucun** fiber
  signal-runnable, au lieu d'aborter il **avance `g_vclock` à la plus proche échéance** et réveille les fibers
  échus avec `timed_out=1` (→ `WAIT_TIMEOUT`). Deadlock réel (tout bloqué, **aucune** échéance) = abort sound.
  `Sleep(ms>0)` = blocage pur sur l'horloge (`u32_sleep`, réveillé seulement par le temps) ; `Sleep(0)` = yield.
  **Déterministe** : l'horloge n'avance que par la logique du scheduler (jamais le wall-clock) ⇒ oracle
  reproductible. `WaitForSingleObject`/`WaitForMultipleObjects` passent désormais le **vrai `ms`** (plus le flag poll).
- **Additivité** : sans timeout fini, `has_timeout=0`, l'horloge n'avance jamais → INFINITE et Sleep(0) inchangés,
  programmes mono-thread inchangés (usleep réel conservé sans threads). Les 4 fixtures threads précédentes restent
  bit-identiques.
- **Oracle** : `winecorpus/thread_pool.c` — pool de 4 workers drainant une file (M=200), somme parallèle des carrés
  `total=2686700`, **avec le timeout fini de vivacité** → **bit-identique Wine** (avant le fix : deadlock→abort).
- **Vérifié** : hash transpile inchangé (`19acad982194bf07`), `table_is_sorted` vert, winediff **105→106/106**,
  sqlite/busybox smoke OK.
- **Bilan honnête** : primitives de threads (join/CS/events/mutex/sem/TLS/`_beginthreadex`/timeouts) **prouvées
  bit-identiques Wine sur 6 workloads** dont un pool réaliste. Mais **faire tourner un vrai binaire MT tiers
  bout-en-bout** reste bloqué en amont par (a) la récup de **dispatch indirect** (table de pointeurs entrelacée) et
  (b) les **API haut-niveau** (thread-pools/APC/affinity) que le conformance exerce. Prochaine valeur réelle =
  ces deux chantiers, pas plus de primitives.

### 2026-07-16 — [RECOV][DEMO] Prologue de réalignement de pile GCC (`lea ecx,[esp+4]; and esp`) → débloque le dispatch `{nom,func}` de `kernel32_test`
- **Suite directe du test « vrai binaire »** : `kernel32_test.exe` abortait sur `call [table]` indirect vers
  `0x446680` (fonction de test « thread » atteinte **uniquement** via la table `{nom, func}` de winetest en
  `.rdata`, entrelacée string-ptr/code-ptr). Diagnostic précis : le scan de données voit chaque `func` comme un
  run isolé (interrompu par le `name`-ptr voisin, non-exec) → il retombe sur `looks_like_func_start`, **qui
  rejetait le prologue** `8d 4c 24 04 83 e4 f8` = `lea ecx,[esp+4]; and esp,-8`.
- **Cause générale** : c'est le **prologue de réalignement de pile GCC/mingw sans frame-pointer** (une fonction
  qui a besoin d'un alignement 16 octets en omettant ebp garde l'esp d'origine dans ecx pour l'accès aux args).
  Toute une **classe** de fonctions mingw l'utilise. Fix : reconnaître la signature **6 octets** (spécifique →
  pas de faux positif sur du padding/données) dans `looks_like_func_start`. Byte-matching extrait en
  `known_prologue_bytes(code, allow_leaf)` (testable).
- **Effet mesuré sur le vrai binaire** : `kernel32_test.exe thread` passe de **0 → 5+ lignes `thread.c:` exécutées**
  (tests TLS-slot/OpenThread/process réels tournent). Il aborte *sound* ensuite sur `CreateProcessA` (pas de
  process enfant — hors-scope) puis **segfault** plus loin dans la surface process/remote-thread/pool (très
  au-delà des primitives) — **pas** une régression (ce chemin était injoignable avant), mais ça borne : faire
  tourner ce conformance **entièrement** demande process-création + thread-pools/APC, un autre grand chantier.
  Les lignes de test qui tournent sont **correctes** (skips/échecs attendus vu l'absence de `CreateProcessA`) ⇒
  la fonction réalignée récupérée **s'exécute juste** (le prologue de réalignement se lifte correctement).
- **Régression complète** (changement recovery = risqué) : hash transpile inchangé (`19acad982194bf07`),
  difftest **271/271**, funcdiff corpus **0 divergence**, gauntlet **19/21** (les 2 = `units.dat` environnemental),
  winediff **106/106**. Unit-tests `analysis::prologue_tests` (3, dont rejets : mauvais disp, `lea` sans `and`,
  données aléatoires). Harnais : hook `winecorpus/NAME.cflags` ajouté à `winediff.sh` (général).
- **Note honnête** : la variante `lea ecx` n'est pas reproductible avec le mingw local (il émet `push ebp; and esp`,
  déjà reconnu) → guard = unit-test byte-exact + preuve sur le vrai binaire, pas une fixture winediff.

### 2026-07-16 — [LIFT][ORACLE] SSE2 packed-integer élargi — **piloté par la donnée** (murs des vrais binaires), bit-identique Unicorn
- **Méthode : mesurer avant de coder.** `--mode walls` (carte statique) agrégé sur **6 modules de conformance
  WineHQ réels** (ucrtbase/shlwapi/msvcrt/advapi32/ole32/gdi32, extraits de `winetest -x`) → **0 appel direct non
  résolu partout** (récup solide) ; le mur dominant = **instructions non liftées**, concentrées en gdi32 (**59
  distinct / 147 sites**). Agrégat des mnémoniques : **SSE2 packed-integer** écrasant (`pextrw`×14, `packuswb`×7,
  `psrld/pslld`×11, `pmullw`, `paddb`, `punpcklbw`, `psubw`…) ; le reste = `ud2`/`in`/`out`/`sti`/`hlt` (données
  décodées-en-code, abort **correct**).
- **Implémenté** (modèle lo/hi 64-bit existant, helpers `__pi_*`) : `paddb/psubb/psubw` (add/sub octet-mot),
  `psubq` (64-bit), `pmullw` (mul-low mot), décalages de lane `pslld/psrld/psrad/psllw/psrlw/psraw` (compte
  **immédiat ET registre**, `compte>=largeur → 0`, arithmétique saturé à largeur-1), `packuswb/packssdw` (pack
  saturant, moitié basse = op0, haute = op1), `punpcklbw/punpckhbw` (interleave octets), `pextrw` (extraction de
  lane 16-bit vers GP), et **`psrldq/pslldq` généralisés** (tout compte 0-15, plus seulement 4/8/12).
- **Validation bit-exacte** : chaque op ajoutée au **corpus cpudiff** (encodages explicites, formes imm+reg) →
  `per_instruction_corpus_matches_unicorn` **passe** (4000 états aléatoires/insn, regs+flags+xmm+mémoire = Unicorn).
  Miroir Rust des helpers dans `cpudiff::helper_call`. C'est la preuve de justesse (pas juste « ça compile »).
- **Effet mesuré** : gaps de lift `gdi32_test` **147 → 19 sites** (−87 %), `msvcrt_test` **10 → 2**. Le résidu =
  données-en-code (abort correct) + MMX (`psubq mm`) + `fiadd` (filet x87). Débloque le code **vectorisé** en
  général (graphisme, boucles auto-vectorisées, string/hash SSE2), pas juste gdi32.
- **Régression complète** (changement lift = risqué) : cpudiff **vert** (bit-exact), hash transpile inchangé
  (`19acad982194bf07`), difftest **271→272/272** (un test jadis « incomplet/non modélisé » devient liftable),
  funcdiff corpus **0 divergence**, winediff **106/106**. *(Piège attrapé : un Xvfb `:99` résiduel de mes tests
  manuels faisait DIFF les 12 fixtures GUI — environnemental, pas le lift ; résolu en tuant le stray Xvfb.)*

### 2026-07-16 — [HLE-STDIO][ORACLE] CRT wide-string (16-bit) — piloté par la donnée (imports manquants des vrais binaires), bit-identique Wine
- **Méthode data-driven (axe 2)** : `--mode imports` agrégé sur 7 modules WineHQ → la famille dominante d'imports
  manquants = **wide-string** (`lstrcmpW`×7, `_wcsnicmp`×4, `lstrcmpiW`×5, `wcsrchr/wcsncmp/_wcsicmp/wcschr/wcstok/
  towlower/towupper/wsprintfW…`). Utilisée largement par les vraies applis Unicode Windows.
- **Implémenté** (code-units **16-bit**, Windows `wchar_t` ≠ hôte 32-bit → compté à la main, pas de forward host) :
  `wcsncmp/wcschr/wcsrchr/wcsncpy/wcsstr` + `_wcsicmp/_wcsnicmp` (**fold ASCII = exact en locale C**, ordinal comme
  msvcrt) + `towlower/towupper`, et kernel32 `lstrlenW/lstrcpyW/lstrcatW` (ordinaux). stdcall_pops : lstr\*W + les
  lstr\*A jusque-là **absents** (bug latent d'esp-drift, jamais testé).
- **Décision de soundness — `lstrcmpW`/`lstrcmpiW` NON implémentés** : mesuré que Wine les compare
  **linguistiquement** (`lstrcmpW("Hello","hello")=+1` : majuscule *après* minuscule via `CompareStringW`), ≠ ordinal
  (`-1`). Un ordinal serait **silencieusement faux** → laissés en **abort sound**. `_wcsicmp` (msvcrt), lui, EST
  ordinal (`_wcsicmp("ZED","abc")=+1` = Wine) → gardé. Le besoin de compare ordinal est couvert exact par
  `wcscmp`/`_wcsicmp`.
- **Pièges attrapés** : (1) `sanitize_import` retire l'underscore de tête → shims `aret_wcsicmp`/`aret_wcsnicmp`
  (pas `aret__…`) — le weak stub rendait `0` = « égal » par coïncidence, masquant le bug ; (2) `%ls` dans printf
  lit du `wchar_t` **32-bit hôte** sur une chaîne 16-bit → gap séparé (évité dans la fixture via un dump octet).
- **Vérifié bit-identique Wine** : `winecorpus/crt_widestr.c` (len/cmp/ncmp/icmp/nicmp avec **ordonnancements**,
  chr/rchr/str, towlower/upper, ncpy, lstrlenW/cpyW/catW), ASCII. Portes : hash transpile inchangé
  (`19acad982194bf07`), `table_is_sorted` vert, winediff **106→107/107**, sqlite/busybox smoke OK.

### 2026-07-16 — [HLE-STDIO][ORACLE] Formatage wide (`%ls`, `wsprintfW`, `_snwprintf`, `_vsnwprintf`) — bit-identique Wine
- **Suite du wide-string** : le gap `%ls` repéré la fois d'avant est comblé, + les formateurs wide.
- **Formateur narrow** (`aret_vformat`) : `%ls`/`%S` (chaîne **16-bit** → narrow, avec largeur/précision via un spec
  narrow reconstruit sans les length-modifiers), `%lc`/`%C` (car 16-bit). Avant, `%ls` passait à glibc qui lisait
  du `wchar_t` **32-bit** hôte sur une chaîne 16-bit → sortie fausse.
- **Formateur wide** `aret_wvformat` (analogue 16-bit) : réutilise la logique numérique éprouvée (formate en narrow
  puis élargit), seule la **source chaîne** diffère (en wide printf `%s`=wide, `%hs`/`%S`=narrow). Branché sur :
  **`wsprintfW`** (user32, max 1024), **`_snwprintf(dst,count,fmt,…)`** (sém. MS : retourne `-1` si tronqué, pas de
  NUL sur remplissage exact — **mesuré = Wine** : `_snwprintf(6,"HELLOWORLD")→-1, buf="HELLOW"`), **`_vsnwprintf`**
  (va_list = pointeur d'args).
- **Décision de soundness — `swprintf` NON modélisé** : signature **ambiguë selon le CRT** (`(buf,fmt,…)` legacy vs
  `(buf,count,fmt,…)` C99/secure que Wine utilise). Deviner mis-parse les arguments — **Wine lui-même faute**
  (`page fault, movzxw (%edi), edi=7`) quand on passe la forme legacy. → **abort sound** plutôt qu'un mis-parse
  silencieux ; `_snwprintf`/`wsprintfW`/`_vsnwprintf` (signatures non ambiguës) couvrent le besoin.
- **Piège** : `sanitize_import` → shims `aret_snwprintf`/`aret_vsnwprintf` (underscore de tête retiré).
- **Vérifié bit-identique Wine** : `winecorpus/crt_wideprintf.c` (`%ls`/`%S`/`%10ls`/`%.4ls`/`%lc`/`%C`, wsprintfW
  avec `%d`/`%08x`/`%s`/`%c`/`%%`, `_snwprintf` tronqué **et** ok, `_vsnwprintf` via va_list). Portes : hash
  transpile inchangé (`19acad982194bf07`), winediff **107→108/108**.

### 2026-07-16 — [HLE-WIN32][ORACLE] Levier dur : collation linguistique (`lstrcmpW`/`CompareStringW`) reproduite bit-à-bit, dans les règles
- **Le #1 des imports manquants transversaux** (mesuré : `lstrcmpW`×11, `lstrcmpiW`×6), que j'avais **refusé**
  d'implémenter ordinalement (silencieusement faux). Fait maintenant **proprement**.
- **Insight** : Windows compare **linguistiquement** (word-sort), et `lstrcmpW(a,b) = sign(memcmp(sortkey(a),
  sortkey(b)))` où sortkey = `LCMAP_SORTKEY`. Donc si je génère **la même sort-key**, je suis exact.
- **Forensics** : dumpé `LCMAP_SORTKEY` de Wine pour tout l'ASCII imprimable → **décodé la structure exacte** :
  `PRI(2 o/car) 01 01 CASE 01 01 00`, où PRI = poids primaire **case-folded** (chiffres `0d..`, lettres `0e..`,
  ponctuation `07/08..`), CASE = `0x12` par majuscule / `0x02` sinon avec **élagage des `0x02` de queue** (niveau
  vide si tout minuscule). Les `-` et `'` sont **ignorables au niveau primaire** (forme `ff ff` spéciale).
- **Implémentation** (`aret_win32.c`) : table `u32_pri[128]` (**poids mesurés, pas devinés**), `u32_sortkey`
  (génère la clé, retourne -1 hors sous-ensemble), `u32_collate` (`memcmp`), sur `lstrcmpW`/`lstrcmpiW` (insensible
  = niveau CASE retiré) + `CompareStringW`/`CompareStringA` (widen ASCII ; `NORM_IGNORECASE` seul flag modélisé).
  **Anciens stubs « ordinal-ish » (subtilement faux sur l'ordre de casse) supprimés.**
- **Règles respectées** : **juste sur le sous-ensemble prouvé** (bit-à-bit), **abort bruyant hors** (`-`/`'`,
  contrôles, non-ASCII → `aret_unmodelled`, jamais deviné). **Chemin rapide égalité** : deux chaînes binairement
  identiques = linguistiquement égales pour **tout** contenu → une comparaison d'égalité **n'aborte jamais**
  (vérifié : `lstrcmpW("café","café")=0`, mais `lstrcmpW("a-b","axb")` → **abort loud**).
- **Vérifié bit-identique Wine** : `winecorpus/win_collate.c` — **1444 paires** (38 chaînes × 38, ×3 fonctions),
  hash `aa8fcadd` = Wine ; spots `Hello/hello=1`, `9/10=1`, `Z/a=1`, **`~/a=-1`** (que l'ordinal ratait :
  ordinal `~`>`a`, linguistique `~`<`a`). stdcall_pops : lstrcmpW/iW=8 (CompareStringA/W=24 déjà là). Portes : hash
  transpile inchangé (`19acad982194bf07`), `table_is_sorted` vert, winediff **108→109/109**.
- **Extensible** : mesurer le niveau spécial de `-`/`'` (et Latin-1) élargirait le sous-ensemble prouvé ; pour
  l'instant abort sound suffit (le chemin rapide couvre déjà l'égalité de tout contenu).

### 2026-07-16 — [HLE-WIN32][ORACLE] Collation : niveau spécial des ignorables (`-`/`'`) — sous-ensemble prouvé élargi
- **Suite directe** de la collation linguistique : élargi le sous-ensemble prouvé aux **ponctuations ignorables au
  niveau primaire** (`-`, `'`) — fréquentes (noms de fichiers "read-me.txt", "O'Brien").
- **Forensics** : dumpé `LCMAP_SORTKEY` pour des chaînes avec `-`/`'` → **décodé le niveau spécial** : chaque
  ignorable ne contribue **rien** aux niveaux primaire/casse, mais ajoute `ff (0xff - nNonIgnorablesAvant) <poids>
  0x12` au niveau spécial (poids mesurés : `-`=0x82, `'`=0x80). Structure complète : `PRI 01 01 CASE 01 01 SPECIAL
  00`. Pattern de position **linéaire** confirmé jusqu'à n=10 (`abcdefghij-k` → `0xf5`).
- **Implémenté** : `u32_ign(c)` + branche spéciale dans `u32_sortkey` ; cap `nAvant < 0x80` (au-delà → abort sound,
  hors de toute chaîne réaliste). Message d'abort mis à jour (ne reste que non-ASCII/contrôles).
- **Vérifié bit-identique Wine** : `win_collate.c` élargi à **2500 paires** (50 chaînes dont `read-me`/`O'Brien`/
  `co-op`/`e-mail`…), hash **`63c659d1`** = Wine ; `readme/read-me=-1`, `coop/co-op=-1`, `OBrien/O'Brien=-1`. Portes :
  hash transpile inchangé, winediff (voir chiffre). ⇒ l'abort ne reste que pour le **non-ASCII** (Latin-1/Unicode).

### 2026-07-16 — [HLE-WIN32][HLE-STDIO] Cluster de petits shims généraux (piloté par la donnée)
- **Après la collation** (dernière grande famille), la mesure ne laisse qu'un **cluster de petits shims généraux**.
  Batch des exacts-vérifiables + widening sound :
  - **`MulDiv(a,b,c)`** : `round(a*b/c)`, **arrondi au plus proche, égalité vers l'extérieur** (mesuré Wine :
    `10*3/4=8`, `-10*3/4=-8`, `x/0=-1`, overflow 32-bit `-1`).
  - **`GetUserDefaultLangID`/`GetSystemDefaultLangID`/`GetSystemDefaultUILanguage`** = `0x0409` (en-US, cohérent
    avec `GetThreadLocale`/`GetUserDefaultLCID`).
  - **`wcstoul`/`wcstol`** (wide 16-bit) : copie octet-bas → `strtol/strtoul` hôte, endptr remappé en code-units
    (mesuré : `wcstoul("0xFF hi",16)=255` reste `" hi"`, `wcstol("-42abc")=−42` reste `"abc"`).
  - **Widening sound** (additifs, non oracle-testés car dépendants de l'env) : `LoadLibraryW`, `SleepEx`,
    `GetSystemDirectoryW`/`GetWindowsDirectoryW`/`GetCurrentDirectoryW`/`SetCurrentDirectoryW`.
- **stdcall_pops** : MulDiv=12, LoadLibraryW=4, +Get/SetCurrentDirectoryA/W, GetSystemDirectoryW, GetWindows
  DirectoryA/W (plusieurs A-versions **étaient absentes** — bug latent d'esp-drift, jamais testé).
- **Piège** : `aret_GetUserDefaultUILanguage` existait déjà (doublon retiré).
- **Vérifié bit-identique Wine** : `winecorpus/win_smallshims.c` (MulDiv, lang, wcstoul/wcstol). Portes : hash
  transpile inchangé (`19acad982194bf07`), `table_is_sorted` vert, winediff **109→110/110**.

### 2026-07-16 — [HLE-STDIO][DEMO][ORACLE] `sscanf` complet — bit-identique Wine (importé par sqlite/busybox)
- **La plus grosse pièce tractable restante**, et **importée par de vrais démonstrateurs** (sqlite3 **et**
  busybox l'ont en import non-couvert) — pas juste un test de conformance. Aucun scanf n'existait.
- **Implémentation** (`aret_sscanf_core`, `aret_crt.c`) : parse le format, lit l'entrée, écrit dans les pointeurs
  args de la pile machine. Couvre : entiers `%d/%i/%u/%x/%X/%o` (base auto pour `%i`) avec longueurs `h/hh/l/ll`
  (taille du store suit le modificateur), flottants `%f/%e/%g` (`%lf`→double), `%s` (délimité espace, largeur),
  `%c` (largeur), scansets `%[...]`/`%[^...]` (avec plages `a-z`), suppression `%*`, `%n`, littéraux, espace. Les
  conversions numériques défèrent à `strtoll/strtoull/strtod` (base/signe/overflow corrects) ; endptr → avance.
  Retour = nb d'items assignés, **`EOF(-1)`** si l'entrée s'épuise avant la 1ʳᵉ assignation (mesuré = Wine).
- **Vérifié bit-identique Wine** : `winecorpus/crt_sscanf.c` — 14 cas (hex/octal `0x1F`/`077`, `%lld`, `%hd/%u`,
  `%lf/%f`, `%3s`, `%c`, `%[^=]=%s`, `%*d`, `%n`, échec→0, EOF→-1, espace en tête). Cdecl (pas de stdcall_pop).
  Portes : hash transpile inchangé (`19acad982194bf07`), winediff **110→111/111**, sqlite/busybox smoke OK.

### 2026-07-16 — [HLE-CRT][HLE-STDIO][ORACLE] `swscanf` (wide, 16-bit) + `iswctype`/`isw*` — bit-identique Wine
- **Suite naturelle du `sscanf`** : la variante wide manquait. Deux chemins réels : les binaires **MSVC**
  atteignent `aret_swscanf` (analogue 16-bit de `aret_sscanf_core`, mêmes conversions) ; les binaires **mingw**
  pilotent leur **propre** scanf wide statique (`__mingw_swscanf`) qui classe les caractères via
  `iswctype(c, desc)` — non implémenté ⇒ mauvaise classification ⇒ parse échoué/garbage.
- **Fix** (`aret_crt.c`) : (1) `aret_swscanf_core` + `aret_swscanf` — mêmes conversions que sscanf mais sur
  unités 16-bit (`%ls`/`%hs`/`%lc` respectent la largeur des code units) ; (2) `w_ctype_mask` + `aret_iswctype`
  reproduisant les masques `_pctype`/`wctype` **ASCII-exacts** (alpha/digit/space/punct/xdigit/cntrl/print/…),
  plus les enveloppes `iswspace/iswdigit/iswalpha/iswalnum/iswupper/iswlower/iswpunct/iswxdigit/iswcntrl/
  iswprint/iswgraph`. Tous **cdecl** (pas de `stdcall_pop`). Wchar Windows = 16-bit → jamais de renvoi vers
  `wcs*`/`isw*` de l'hôte (32-bit).
- **Vérifié bit-identique Wine** : `winecorpus/crt_swscanf.c` — `%d/%x/%lf`, `%lld`, `%f`, `%ls`/`%hs`/`%lc`,
  `%3ls` (largeur), EOF→-1, échec→0. Portes : hash transpile inchangé (`19acad982194bf07`),
  winediff **111→112/112**.
- **Reste** : le scanset wide `%[^...]` de mingw a une quirk interne (Wine ⧣ attendu) — hors périmètre ;
  la sémantique scanset reste couverte/prouvée côté `sscanf` narrow.

### 2026-07-16 — [HLE-FILE][ORACLE] Itération de répertoire CRT `_findfirst`/`_findnext`/`_findclose` — piloté par la donnée, bit-identique Wine
- **Mesure d'abord** (règle §2) : `--mode walls` + `wallsweep.sh` sur les 21 PE du gauntlet → la famille d'imports
  manquants **dominante par largeur** est `_findfirst`/`_findnext`/`_findclose` (**10 binaires sur 21**, 1 famille).
  (Le launcher `winetest.exe` = surtout data-décodée-en-code + une famille socket + process/registry hors périmètre.
  `mul dl` non lifté = **abort sound correct**, 1 site non atteint dans sqlite — pas un bug, laissé.)
- **Forensics/mesure exacte** (jamais deviné) : sondes cross-compilées lancées **sous Wine** → `struct _finddata_t`
  = **280 o** (`attrib`@0, `time_create`@4, `time_access`@8, `time_write`@12 en `time_t` 32-bit, `size`@16 en
  `_fsize_t` 32-bit, `name`@20[260]) ; **encodage `attrib` CRT ≠ Win32** (mesuré) : fichier normal=`_A_ARCH(0x20)`,
  répertoire=`_A_SUBDIR(0x10)` **taille 0**, read-only ajoute `_A_RDONLY(0x01)` → `0x21` ; `.`/`..` **énumérés** ;
  no-match → handle `-1` **et** `errno=ENOENT(2)`.
- **Fix** (`aret_hle.c`) : `aret_findfirst`/`aret_findnext`/`aret_findclose` réutilisent la machinerie
  `aret_find_t`/`aret_ci_match`/opendir-fnmatch de `FindFirstFileA`, mais une fonction de remplissage **dédiée**
  (`aret_fill_finddata`, layout 280 o + encodage attrib mesuré) et les **conventions de retour CRT** (`_findnext`
  → 0/-1, `_findfirst` → handle/-1 avec `errno`). Temps = vrais temps hôte (corrects, non vérifiés bit-à-bit car
  env-dépendants, comme le find Win32 frère). Tous **cdecl** (pas de `stdcall_pop`). **`_rmdir` ajouté** au passage
  (rmdir POSIX, 3 binaires du wallsweep, requis par le cleanup du fixture).
- **Piège rappelé** : le runtime C est `include_str!` dans le binaire Rust → **`cargo build --release` obligatoire**
  après édition d'`aret_hle.c` (sinon shim non ramassé, stub faible gagne).
- **Vérifié bit-identique Wine** : `winecorpus/crt_findfirst.c` (7 entrées `.`/`..`/fichiers/dir/read-only avec
  attrib+size, wildcard `*.TXT` insensible casse, no-match+ENOENT). Portes : hash transpile inchangé
  (`19acad982194bf07`), winediff **112→113/113**.

### 2026-07-16 — [HLE-CRT][ORACLE] `_assert` (msvcrt) — gain de soundness + correction d'une asymétrie de l'oracle winediff
- **Piloté par la donnée** : après findfirst, l'import manquant #1 par largeur du wallsweep gauntlet = **`_assert`
  (11 binaires)**. Ce n'est **pas** un simple shim : le stub faible renvoyait 0, donc un programme **continuait après
  une assertion violée** = **faux silencieux** (viole le principe sacré). L'implémenter *est* la correction.
- **Mesuré vs Wine** (sonde) : `_assert(expr, file, line)` écrit `Assertion failed: <expr>, file <file>, line <n>\n`
  sur **stderr** puis `abort` (exit 3, stdout tronqué). Fix (`aret_hle.c`) : `aret_assert` (+`aret_wassert` wide)
  reproduisant le message exact + `abort()`. Cdecl.
- **Asymétrie d'oracle découverte et corrigée (cause générale, `src/builder/mod.rs`)** : `--run` **capturait** le
  stdout enfant via `.output()` **et y appendait le stderr** → winediff comparait stdout+stderr **mêlés** d'ARET
  contre le stdout-**seul** de Wine (dont le stderr est jeté par `2>/dev/null`). Tout fixture dont le comportement
  correct écrit sur stderr (assert, diagnostic) apparaissait faussement divergent. Fix : `--run` **hérite** le
  stderr enfant (→ fd2 d'ARET, jeté identiquement pour les deux moteurs) et ne capture/encadre que **stdout** — le
  flux que winediff compare réellement. Les sweeps (busybox/sqlite) lancent l'`app` recompilée **directement** avec
  flux séparés → **non affectés**. Chemin WASM inchangé (7/7).
- **Vérifié bit-identique Wine** : `winecorpus/crt_assert.c` (« before » sur stdout, `abort` à l'assertion → « after »
  jamais atteint). Portes : **régression unifiée PASS** (difftest 272, funcdiff 0 div, SMT 11/11, recompil. 100 %),
  hash transpile inchangé (`19acad982194bf07`), winediff **113→114/114**.

### 2026-07-16 — [RECOV][HLE-CRT][ORACLE] Profondeur : host-back de l'intrinsèque `memmove` MSVC (tables de saut entrelacées), PROUVÉ vs libc
- **Levier de profondeur, attaqué proprement (mesure d'abord).** Cible = un **vrai binaire** rapatrié d'archive.org
  (`tucows_putty.exe`, MSVC 32-bit strippé, MIT). `--mode walls` : ses **seuls** murs de lift = **8 sites**
  `jmp [reg*4+0x43XXXX]` non résolus. Forensics : ce sont deux copies de l'**intrinsèque `memmove` MSVC hand-tuné**
  (fonction `(dst,src,n)` + test de recouvrement + `rep movs` + dispatch d'alignement) dont les **tables de saut sont
  physiquement entrelacées dans le code** (l'entrée[0] d'une table lit dans les octets d'une instruction voisine) →
  **indissociable** par tout décodeur linéaire/récursif. `read_jump_table` renvoie 0/garbage → `jmp` calculé laissé
  unmodelled (abort sound). *(Mesure de contrôle : sqlite3 3.40 et nasm 2.16, MSVC modernes qui passent, n'ont
  **aucun** de ces murs — l'intrinsèque entrelacé est spécifique au vieux MSVC.)*
- **La bonne réponse = réutilisation vérifiée** (précédent libm §4.2), pas lifter ce charabia : **reconnaître** que la
  fonction *est* memmove et la brancher sur `aret_memmove`. Mais — **règle sacrée « rien de prouvé = rien de deviné »** :
  on ne devine pas « c'est memmove » sur une lecture. **PREUVE comportementale (Unicorn)** : la fonction réelle
  `0x43a620` exécutée sur **500/500** cas aléatoires (tailles 0..1000, recouvrements avant **et** arrière) produit
  **bit-pour-bit** le résultat de `memmove` libc (et `eax=dst`). C'est memmove (gère le recouvrement → **pas** memcpy),
  mesuré et non deviné.
- **Fix (mécanisme existant, zéro nouveau code de reconnaissance)** : signature **FLIRT** du préfixe 32 o de
  l'intrinsèque (jcc rel32 wildcardé) → `crt_symbol` = `memmove` → le host-backing existant lie les appels à
  `aret_memmove` et **n'émet pas** le corps. Nouveau fichier `runtime/flirt/msvc_crt.sig` (concaténé au `mingw_crt.sig`
  bundlé). **Byte-exact ⇒ zéro faux positif** : une autre version de memmove qui ne matche pas reste en abort (sound,
  jamais faux). Gardé par test unitaire `flirt::bundled_recognises_msvc_memmove`.
- **Résultat** : les deux entrées memmove de putty (`0x43a620`, `0x43b910`, octets identiques = 2 copies linker) sont
  host-backées (host-backed 18→20, murs 16→8). **Résidu honnête** : les 8→derniers gaps sont dans des **fragments de
  case-body morts** (`sub_43a690`…) que le scan address-taken a promus depuis les pointeurs des tables entrelacées ;
  **0 site d'appel** (memmove, leur seul appelant, est désormais un shim) → code mort, jamais exécuté (sound). Les
  supprimer = un fix de récupération (plus risqué) laissé en suivi.
- **Portes** : régression unifiée **PASS** (difftest 272, funcdiff 0 div, SMT 11/11, recompil. 100 %), hash transpile
  **inchangé** (`19acad982194bf07` — additif, aucun démonstrateur affecté), winediff **114/114**, flirt tests 6/6.
- **Note méthode** : levier « profondeur » réel mais **niche** (vieux MSVC). Choix 3 (borne de saut par masque) mesuré
  puis **écarté** : le `switch` à index masqué (`and idx,3`) est **déjà** résolu correctement (fixture inline-asm
  vérifiée) — `read_jump_table` s'arrête sur la 1ʳᵉ entrée non-exécutable ; pas de bug → pas de code spéculatif
  (règle « pas de changement sans bénéfice mesuré en zone correctness-critique »).

### 2026-07-16 — [ORACLE] funcdiff élargi aux fonctions à-imports (stubs symétriques) — la zone aveugle du doc rendue visible
- **Motivation mesurée (vrai binaire).** Forensics de profondeur sur `7za.exe` (7-Zip 9.20, MSVC 32-bit, archive.org) :
  le dispatch vtable C++ **se lifte** (3248/3263 fonctions) — ce n'est **pas** le mur. Le vrai mur = un **crash au
  runtime** (segfault au démarrage) dans `sub_471a83` (chaîne locale/sort-key), un **miscompile invisible à la carte
  statique**. Or `funcdiff` (l'oracle qui l'attraperait) **skippait** exactement cette classe : *toute* fonction dont
  la closure appelle un import Win32/CRT (doc 70 §7 : « les bugs profonds sont dans les fonctions skippées derrière
  imports »). Levier : **étendre l'oracle**, réutilisable pour tout binaire/fonction.
- **Mécanisme : stub d'import symétrique** (appliqué **identiquement** aux deux moteurs, donc l'import n'est jamais
  source de divergence — tout écart d'esp/registre/mémoire autour = vrai bug de lift) :
  - *Interpréteur* (`cpudiff.rs`) : un appel `Named` import pose `eax=0`, `edx=0` (colle au split de retour edx:eax
    de l'IR, donc un **tail-call `jmp [import]`** — qui saute le split — matche aussi), ne touche rien d'autre ; le
    `esp += @N` stdcall est déjà porté par l'IR. Il enregistre le slot `esp-4` où le `call` d'Unicorn pousse une
    adresse de retour, pour que le diff de pile l'exclue (comme `call_direct` pour les appels récursés).
  - *Unicorn* : le slot IAT de chaque import stdcall est repointé vers un blob `mov eax,0; mov edx,0; ret N` dans une
    page scratch → l'émulateur exécute exactement le même effet.
  - Restreint aux imports à **pop `@N` stdcall connu** (l'IR fait `esp += N`, le stub fait `ret N` → lockstep exact) ;
    cdecl/inconnus restent skippés (sound, pas de régression).
- **Debug itératif (4 sous-cas, chacun mesuré-puis-corrigé)** : (1) l'image Unicorn patchée vs l'image interpréteur
  pristine → slots IAT divergents dans le diff mémoire → donner **la même** image patchée à l'interpréteur (il résout
  par Named, jamais par le slot) ; (2) tail-call `jmp [import]` : eax non écrit → poser `regs[0]=0` ; (3) split
  edx:eax : `mov edx,0` des deux côtés ; (4) l'adresse de retour poussée par le `call` Unicorn → exclue via `ret_slots`.
- **Vérifié : 0 divergence maintenue, couverture explose.** Corpus busybox+sqlite : lift **12,5k → 16,6k** scorées
  (busybox 3567→4904, sqlite 8900→11700), **0 divergence**. `cpudiff.rs` = oracle-only (hors pipeline produit) → hash
  transpile et winediff intacts. Sur 7za : **8048 fonctions scorées, 0 divergence** (les non-SEH validées).
- **Reste (increment 2)** : la chaîne `sub_471a83` reste skippée car **SEH** (`mov fs:[0]`) — non modélisé en mode
  decompile (→ `Asm`). L'atteindre = mapper un TEB + base `fs` dans Unicorn **et** modéliser `fs:[0]` dans
  l'interpréteur. C'est le prochain incrément pour pinpointer le miscompile 7za.

### 2026-07-16 — [ORACLE] funcdiff increment 2 : fonctions SEH scorées (page-segment zéroée pour `fs:[disp]`) — et sub_471a83 PROUVÉ correct
- **Suite de l'extension imports.** Les wrappers CRT à SEH restaient skippés : Unicorn **fautait** sur `mov eax,fs:[0]`
  (base fs non configurée) alors que l'interpréteur modélise une lecture segment en `konst(0)`.
- **Fix minimal (après une impasse GDT).** Tentative 1 = descripteur GDT + base fs → **casse tout** (configurer GDTR
  invalide les sélecteurs par défaut DS/SS → `push` faute). Tentative 2 (retenue) : la base fs par défaut d'Unicorn
  est **0** (plate), donc `fs:[disp]` lit l'adresse absolue `disp` → **mapper une page zéroée à l'adresse 0** rend ces
  lectures = 0 (= l'interpréteur), sans faute ; re-zéroée par itération, **jamais comparée** (hors des plages
  image/pile du diff) ; les écritures segment (SEH `mov fs:[0],esp`) y vont — l'interpréteur les drop (Nop), donc
  ce n'est pas un signal de lift. (Un vrai null-deref ne faute plus, mais l'interpréteur n'a pas de région à 0 → skip,
  jamais de faux verdict.)
- **Vérifié : corpus **0 divergence** (16604 scorées). Sur 7za, scored **8048 → 11374** (fonctions SEH couvertes).**
- **Découverte majeure (réfute une hypothèse).** `sub_471a83` — la fonction que le segfault 7za traverse, que j'avais
  supposée porteuse d'un **miscompile de frame SEH** — est **modelable=true et scorée à 0 divergence** : son lift est
  **CORRECT**. Le crash n'est **pas** là. La chasse se redirige vers un **ancêtre** (`0x470889`/`0x471830`/`0x471c08`,
  encore unmodelables car ils appellent des **imports cdecl / appels indirects**) ou vers le **shim MBToWC** lui-même.
  C'est exactement le rôle de l'oracle : **écarter** une piste fausse par la preuve, pas la deviner.
- **Reste (increment 3)** : étendre le stub aux imports à pop non listé. **⚠️ TENTÉ — l'entrée suivante corrige le
  mécanisme exact et livre la VOIE SOUND (ajouter les `@N` prouvés), qui a débloqué +1808 fonctions scorées.**

### 2026-07-17 — [ORACLE] funcdiff : pourquoi le stub pop-0 est unsound (mécanisme corrigé) + la voie sound
> **Correction d'une entrée antérieure de ce jour.** J'avais écrit que l'élargissement du stub produisait un **faux
> positif** via un `sub esp, N` supprimé par `build.rs`. **C'est faux, mesuré :** le motif « `call [import]`
> immédiatement suivi de `sub esp,imm` » apparaît sur **0 site** dans busybox, sqlite ET 7za (`objdump` + grep). 7za
> est **push-model** (20238 `push` vs 1698 `mov [esp+k]`). Voici le vrai mécanisme.
- **Le vrai risque du stub pop-0 = faux NÉGATIF (bénir un lift faux), pas faux positif.** Pour un import **stdcall
  non listé** (pop réel `N>0`) en push-model : (a) `build.rs` n'émet **pas** `esp += N` (il ne connaît pas `N`) → l'IR
  liftée laisse esp **trop bas de N** ; (b) le stub Unicorn `ret 0` laisse esp **trop bas de N** aussi. Les deux se
  trompent **du même montant** → ils **s'accordent** → funcdiff rend **0 divergence** = verdict « correct » **sur un
  lift qui est en fait faux**. C'est **pire** qu'un faux positif : l'oracle *bénit* un miscompile (viole le principe
  sacré). Mesuré : broadening pop-0 → 7za **15828 scorées, 0 divergence** — ce 0 est précisément le **symptôme** de la
  cécité, pas une preuve de justesse. L'instinct « stubber = deviner ? » était juste : pop-0 sur un stdcall est un pari.
- **Cause racine plus profonde (trou produit, pas seulement oracle).** Cette dérive esp est un **vrai miscompile
  silencieux** du produit pour **tout code FPO** (sans frame pointer) appelant un stdcall non tabulé : les fonctions à
  frame pointer la masquent (`mov esp,ebp` à l'épilogue efface la dérive), mais le code chaud optimisé (FPO) la
  propage. C'est la **même classe** que le bug busybox `SetLastError` historique (cf. en-tête `stdcall_pops.rs`).
- **La voie SOUND = ajouter les `@N` PROUVÉS** (jamais deviner pop-0). Source = vérité terrain : la décoration `@N` des
  **import libs mingw i686** (`nm libkernel32.a | grep __imp__`), identique dans tout binaire. Mesure d'abord (règle
  §2) : `objdump` des imports de **7za** (static-CRT, 140 imports **tous stdcall** OLEAUT32/USER32/KERNEL32) croisé à
  la table → **51 fonctions à `N>0` manquantes** : `ReadFile@20`, `WriteFile@20`, `HeapAlloc@12`/`HeapFree@12`/
  `HeapReAlloc@16`/`HeapSize@12`/`HeapCreate@12`/`HeapDestroy@4`, `VirtualAlloc@16`/`VirtualFree@12`, `CreateFileW@28`,
  `FindFirstFileW@8`/`FindNextFileW@8`, `RaiseException@16`, `RtlUnwind@16`, `GetTempPath/FileName A/W`, `SearchPathA/W@24`,
  `Interlocked{In,De}crement@4`, `IsBad{Read,Write}Ptr@8`, `Get/SystemTime`/`FileTime*` … (les `@0` — GetLastError,
  GetTickCount, TlsAlloc, GetCommandLine* … — restent **omis**, rien à popper). Toutes **cdecl-safe** : un stdcall
  connu ⇒ `build.rs` émet `esp += N` **et** le stub Unicorn devient `ret N` (fidèle).
- **Bénéfice MESURÉ (correctness-critique, règle §2).** funcdiff corpus **16604 → 18412 scorées** (sqlite 11700 →
  **13508**, +1808 fonctions), **0 divergence maintenue**. Ces +1808 appelaient ReadFile/WriteFile/Heap*/… — la **zone
  aveugle** du doc 70 §7, auparavant *skippée* — désormais **prouvées correctes** avec stubs fidèles ; et le 0-divergence
  **prouve** que les `esp += N` ajoutés matchent le `ret N` d'Unicorn (donc les arités sont bonnes et les lifts sains).
  Neutre pour le modèle accumulate (le `+N` connu s'annule avec le `sub esp,N` lifté, comme le drop d'avant).
- **Portes** : régression unifiée **PASS** (difftest 272, funcdiff 0 div, SMT 11/11, recompil. 100 %), hash transpile
  **inchangé** (`19acad982194bf07` — les fixtures decompile n'importent pas de Win32), winediff **114/114**, sweeps
  sqlite (on-disk ReadFile/WriteFile) + busybox **60/60** bit-identiques, `table_is_sorted_by_name` vert.
- **7za : le crash de démarrage PERSISTE** (segfault avant la bannière) — donc les pops manquants **ne sont pas** *sa*
  cause. `sub_471a83` reste prouvé correct ; le fautif est un ancêtre encore non-modelable (import non implémenté :
  20 restent — CharPrevExA/CompareFileTime/FormatMessage*/GlobalMemoryStatus/… — ou `aesdec` AES-NI = abort sound) ou
  la surface process/EH. **Borné puis pivoté** (règle §2) : on ne s'enferme pas dans du forensics mono-7za ; le fix des
  pops vaut **pour lui-même** (soundness générale + oracle élargi), indépendamment de 7za.

### 2026-07-17 — [DEMO][LIFT] 7za : crash de démarrage LOCALISÉ au bit près (init locale/collation) — borné, pas la faute du shim/pops/SEH
- **Méthode (tip doc 70 §7 « localiser un crash natif ») :** gdb sur l'`app` recompilée (noms `sub_XXXX` présents).
  Backtrace du SIGSEGV : `aret_LCMapStringW+198` ← `sub_472126` ← `sub_471a83` ← `sub_471830` ← `sub_471c08` ←
  `sub_470889` ← `sub_46cf4c` ← `main`. C'est l'**init locale/collation du CRT/C++** (avant même la bannière usage).
- **Ce n'est PAS** : (1) le **shim** — `aret_LCMapStringW` lit correctement ses 6 args (saute le `Locale` de tête via
  `WU(1)=2e arg`, convention `esp[0]=arg1` vérifiée contre `MultiByteToWideChar`) ; (2) les **pops** — LCMapStringW=24,
  LCMapStringA=24, MultiByteToWideChar : tous corrects et tabulés ; (3) une **exception SEH/C++** — breakpoints sur
  `aret_RaiseException`/`aret_RtlUnwind`/`aret_UnhandledExceptionFilter` : **aucun** ne se déclenche avant le crash.
- **Ce que c'est** : la pile machine passée au 3ᵉ appel LCMapStringW est **corrompue en amont**. Args lus (machine-esp
  correct, via `[ebp+8]`) : call1 `flags=0x100,src=0x47cd24,cchSrc=1,dst=0` (mesure, OK) ; call2/call3
  `flags=0x230022, src=0xb000a, cchSrc=0xF000E` — **valeurs garbage** (motif de petites paires séquentielles
  0x0a/0x0b, 0x0e/0x0f, 0x22/0x23 → lecture d'une mauvaise structure/table). Au crash, `dst≠0` + `src=0xb000a` invalide
  → `src[i]` déréférence 0xb000a → SIGSEGV. `sub_472126` (frame-pointer) passe ses **propres params** `[ebp+0x8..0x24]`
  à LCMapStringW ; ils sont déjà corrompus → la faute naît **au-dessus** (sub_471a83 et ses ancêtres, tous SEH +
  imports/appels indirects que funcdiff ne modélise pas encore → 0-divergence n'y prouve rien, doc 70 §7).
- **Note soundness** : c'est un **crash** (SIGSEGV), pas une sortie fausse silencieuse → conforme au principe sacré
  (« juste, ou échec bruyant »), mais **incomplet**. Pas un abort `aret_unmodelled` propre (le lift ne *sait* pas
  qu'il produit une adresse fausse — c'est un miscompile, pas un mur de couverture).
- **Borné puis pivoté (règle §2).** Pistes pour la reprise, par ordre : (a) rendre les ancêtres modelables par funcdiff
  en gérant les **appels indirects** (le vrai chaînon manquant de l'oracle — cf. doc 80 §1.4 PGL opt-in) pour pinpointer
  la fonction qui corrompt ; (b) tracer sous gdb quelle frame écrit le garbage `0xb000a`/`0x230022` dans les params de
  sub_472126 (watchpoint) ; (c) suspecter un **`ret N` interne mal calculé** (`compute_callee_pops`) ou un appel
  indirect à callee-pop erroné sur ce chemin. **Le fix des 51 pops (entrée précédente) vaut indépendamment** — il n'a
  pas corrigé *ce* crash, mais il a élargi l'oracle de +1808 fonctions prouvées correctes, valeur générale réelle.

### 2026-07-17 — [ORACLE] funcdiff suit les appels INDIRECTS résolus (vtables/tables/pointeurs) — +13k appels validés, 0 divergence
- **Le point aveugle nommé.** Deux sessions convergeaient sur le même mur : funcdiff **skippait** toute fonction à
  appel indirect (`check_expr_calls` : `Indirect(_) => None`). Or c'est là que naissent les miscompiles C++ (dispatch
  vtable) et les corruptions (cf. l'entrée 7za). Levier choisi par l'utilisateur : **modéliser les appels indirects**.
- **Mécanisme (sound par construction).** À un appel indirect, l'interpréteur **évalue l'expression d'adresse**
  (`CallTarget::Indirect(e)`, où `e` = valeur du pointeur de fonction, cf. `lift.rs` `op_value`) → cible concrète `t` ;
  puis **réutilise `call_direct(t)`** (mêmes mécaniques : push adresse de retour sentinelle, callee `ret N`). Trois
  issues : (a) lift **correct** ⇒ `t_interp == t_unicorn` ⇒ même fonction ⇒ lockstep ; (b) lift **faux** (mauvaise
  cible calculée) ⇒ fonctions différentes ⇒ **états divergent ⇒ vrai bug attrapé** ; (c) `t` **non-fonction** (vtable
  via objet seedé aléatoire, ou pointeur garbage) ⇒ `call_direct` rend `None` ⇒ **skip l'itération**, jamais de faux
  verdict. Les cibles **basées image** (tables de saut/pointeurs en `.rdata`, pointeur chargé d'une constante)
  résolvent déterministe → **nouvelle couverture réelle**. Tail-call indirect (`jmp [x]`) exclu (discipline esp
  différente) → skip sound.
- **Trois éditions** (`cpudiff.rs`, oracle-only) : (1) `eval_or_call` case `Indirect(addr)` → `eval` + `call_direct` ;
  (2) `run_closure` skip explicite du tail-call indirect ; (3) `check_expr_calls` autorise `Indirect` (valide juste
  l'expression d'adresse, aucune cible statique ajoutée — la résolution est par-itération au runtime).
- **Mesuré : 0 divergence, couverture explose.** Corpus busybox+sqlite : lift **18412→19832** scorées (busybox
  4904→5120, sqlite 13508→**14712**), **appels suivis 7384→20449** (sqlite 4384→**17149** — +13k appels indirects
  désormais validés), **0 divergence**. ⇒ ces 13k+ cibles indirectes sont **liftées correctement** (preuve, pas
  supposition). Portes : régression unifiée **PASS**, hash transpile `19acad982194bf07` **inchangé** (oracle-only),
  difftest 272, SMT 11/11, recompil. 100 %.
- **7za avec appels indirects : toujours 0 divergence** (36916 scorées, 191054 appels). **Insight décisif** : si
  *toutes* les fonctions du chemin de crash ont un lift **fidèle** (0 div), alors le transpilé calcule **exactement**
  comme le hardware → le crash ne peut PAS venir d'un **lift modelable**. Il vient donc soit (a) d'une fonction encore
  **skippée** par funcdiff (Switch/Asm/x87/import non modélisé sur ce chemin précis), soit (b) d'un **shim HLE** qui
  renvoie autre chose que Windows (locale/codepage : GetStringTypeW/LCMapStringW-mesure/GetCPInfo/MultiByteToWideChar…)
  faisant construire une table locale garbage. **La chasse 7za se recentre sur les shims du chemin de démarrage**, pas
  sur le lift — l'oracle a fait son travail : **écarter le lift par la preuve**.

### 2026-07-17 — [ORACLE] funcdiff stubbe aussi les imports `@0` SCALAIRES — et la frontière sound (pas les retourneurs de pointeur)
- **Point aveugle mesuré.** Après les appels indirects, funcdiff restait 0-divergence sur 7za — mais parce que la
  chaîne de crash appelle **`GetACP`** (et d'autres `@0`), **absents de `stdcall_pops`** (les `@0` y sont omis, rien à
  popper), donc **non stubbés** → **toute fonction appelant un `@0` était skippée**. L'oracle ne voyait pas ces
  fonctions. Levier : stubber aussi les `@0` (pop 0 = **fait**, vérité terrain mingw `__imp__NAME@0`).
- **Mécanisme** : nouvel ensemble `is_zero_pop_import` (`stdcall_pops.rs`, oracle-only, jamais lu par `build.rs`) ;
  funcdiff l'ajoute au stub set et stubbe `ret 0`. Neutre pour le produit (un `@0` prend déjà la branche
  `prev_unknown_import` que l'ajout ne change pas).
- **DÉCOUVERTE — frontière sound (le stub `eax=0` et les pointeurs).** En incluant **tous** les `@0`, funcdiff a sorti
  **1 divergence sur 7za** : fn `0x447f00`, `stack +0x7fd4 : lifted=0 unicorn=0xf4`. **Faux positif**, isolé par
  expérience : en **excluant les `@0` retournant un POINTEUR/handle** (`GetCommandLineA/W`, `GetEnvironmentStrings(W)`,
  `GetProcessHeap`, `GetConsoleWindow`, `GetCurrentProcess`), la divergence **disparaît** (7za : 0 div). Cause : le stub
  rend `eax=0` ; quand le callee **déréférence** ce pointeur, Unicorn (qui mappe une page zéroée à l'adresse 0 pour
  `fs:[disp]`/SEH) le parcourt tandis que l'interpréteur n'a pas de région là → divergence NULL-deref **artefact du
  stub**, pas un bug de lift. Un `eax=0` **scalaire** (code page, compteur, id, errno, bool) est utilisé comme donnée
  par les deux moteurs à l'identique → sûr.
- **Décision (discipline > couverture)** : `ZERO_POP` restreint aux **14 `@0` scalaires** (`GetACP`/`GetOEMCP`/
  `GetConsole[Output]CP`/`GetLastError`/`GetCurrentProcess{Id,}`→non, `GetCurrentProcessId`/`GetCurrentThreadId`/
  `GetTickCount`/`GetVersion`/`GetLogicalDrives`/`TlsAlloc`/`AreFileApisANSI`/`DebugBreak`/`SetFileApisToOEM`).
  Raison + contre-exemple `0x447f00` gravés dans le doc-comment de `is_zero_pop_import`. *(Le même risque « stub 0 =
  faux pointeur » vaut en théorie pour un stdcall `@N` retournant un pointeur ; le corpus known-`@N` reste 0-div, donc
  non touché — mais la leçon est notée.)*
- **Mesuré : sound, couverture +.** Corpus lift **19832→20501** scorées (+669, fonctions appelant `GetLastError`/
  `GetACP`/…), **0 divergence** ; 7za **38512 scorées, 0 divergence**. Portes : régression unifiée **PASS**, hash
  transpile `19acad982194bf07` **inchangé** (oracle-only), winediff intact, `zero_pop_is_sorted` + `table_is_sorted` verts.
- **Conséquence 7za** : l'oracle a **écarté le lift par la preuve** sur tout le chemin scorable — le crash de démarrage
  n'est ni un lift modelable (0 div), ni un `@0` scalaire. Il reste dans (a) une fonction encore skippée (import à pop
  **inconnu**/cdecl, ou `@0` **pointeur** qu'on ne peut pas stubber soundement, ou Switch/Asm/x87 sur ce chemin), ou
  (b) un **shim HLE** du chemin locale (le plus probable, cf. entrée précédente : `GetACP` est le seul shim locale
  atteint avant le crash — vérifier sa valeur de retour vs Windows).

### 2026-07-17 — [ORACLE][DEMO] 7za : le blocage funcdiff précisé = blind-spot des APPELANTS d'intrinsèques host-backés (memmove)
- **Diagnostic de modelabilité de la chaîne de crash** (`main → sub_46cf4c → sub_470889 → sub_471c08 → sub_471830 →
  sub_471a83 → sub_472126 → LCMapStringW`) : `sub_471a83`/`sub_472126` = **modelable, 0 div** (prouvés corrects) ;
  `sub_470889`/`sub_471c08`/`sub_471830` = **non-modelables car ils atteignent `sub_46bab0`** ; `sub_46cf4c` = non-modelable.
- **`sub_46bab0` EST l'intrinsèque `memmove` MSVC** (bytes `558bec57568b750c8b4d108b7d088bc18bd103c63bfe76083bf80f82…`
  = signature `msvc_crt.sig` exacte ; check de recouvrement + `rep movs` + tables de saut entrelacées). **Dans le
  PRODUIT il est host-backé** → `aret_memmove` (correct, prouvé 500/500 vs libc) — pas la cause du crash.
- **Le blind-spot est un ARTEFACT de funcdiff, pas un bug produit** : funcdiff tourne en `shared_stack=false`, or
  `call_binding` (build.rs) n'applique le host-back `crt_symbol` **que si `shared_stack()`**. Donc funcdiff voit un
  `call Direct(0x46bab0)` (memmove **brut**, tables entrelacées → `Switch`/`Asm`) → récurse → skip → **skippe tous les
  appelants de memmove**. Le produit, lui, les compile correctement (via `aret_memmove`).
- **⇒ Prochain levier oracle identifié (général)** : faire reconnaître à funcdiff les intrinsèques host-backés
  (`crt_symbol` = memmove/memcpy/…) comme des **memcalls modélisés** au lieu de récurser dans le corps brut — soit en
  activant `shared_stack` dans funcdiff (gros, re-valider les 20k), soit en précalculant `{addr → crt_symbol}` et en
  modélisant l'effet mémoire à la frontière d'appel. Cela rendrait scorables les appelants de memmove/memcpy (**classe
  large**, tout binaire static-CRT) et dirait si `sub_470889`/`sub_471c08`/`sub_46cf4c` cachent le miscompile 7za, ou
  s'il faut chercher ailleurs (dépendance à de la mémoire heap non-init : `aret_HeapAlloc` honore pourtant
  HEAP_ZERO_MEMORY→calloc et lit les bons args — écarté).
- **Statut 7za** : crash **écarté** de tout lift scorable + des shims vérifiés (GetACP=1252 OK, memmove OK, HeapAlloc
  OK). Reste dans l'ombre : les appelants de memmove (blind-spot ci-dessus) et `sub_46cf4c`. **Borné** : 3 increments
  généraux livrés cette session (pops `@N`, appels indirects, `@0` scalaires) ; la résolution finale 7za attend le
  levier memmove-callers.

### 2026-07-17 — [ORACLE] funcdiff modélise les intrinsèques mémoire host-backés (memmove/memcpy) + exclut le scratch sous-esp
- **Le blind-spot du memmove résolu.** funcdiff tourne `shared_stack=false` → il ne voyait pas le host-back FLIRT et
  récursait dans le corps brut (tables entrelacées → skip) → **skippait tous les appelants de memmove/memcpy** (classe
  large : tout binaire static-CRT MSVC). Fix : précalcul `{addr → MemIntrin}` via `prog.crt_symbol` ; à la frontière
  d'appel, `call_mem_intrinsic` modélise l'effet mémoire (comme le shim `aret_memmove` du produit) au lieu de récurser.
- **Modèle sound** : cdecl, args pile `[esp]=dst/[esp+4]=src/[esp+8]=n`, retourne `dst` en `eax`. **Lecture de tous les
  octets source AVANT écriture** = résultat memmove correct pour tout recouvrement (et = memcpy sur non-recouvrement ;
  memcpy recouvrant = UB → skip). Gardes statiques (`check_expr_calls`/`fn_local_targets`/`is_closure_modelable`)
  threadées pour traiter une cible intrinsèque comme **feuille modélisée** (pas de récursion, pas de `ret N` requis).
- **DÉCOUVERTE — le scratch sous-esp (12 faux positifs, corrigés proprement).** Modéliser l'intrinsèque à la frontière
  ne réplique pas le **scratch que le corps réel écrit sous esp** (push edi/esi/ebp + dispatch) → Unicorn a des valeurs
  là où le modèle a le seed → 12 divergences 7za, **toutes le même motif** (`stack +0x7fXX : lifted=0 unicorn=0xNN`,
  juste sous esp). **Faux positifs.** Fix **principiel** (pas une rustine) : **la mémoire strictement sous l'esp final
  est morte** (un programme correct ne la lit jamais) → le diff de pile **skippe les slots sous `min(esp_interp,
  esp_unicorn)`**. Un vrai bug touche la pile **vivante** (≥ esp) et reste comparé ; le `min` garde la région vivante
  du moteur au plus haut esp si esp divergeait. Soundness-neutre (le corpus reste 0-div).
- **Mesuré** : corpus **20501 scorées, 0 divergence** (inchangé — busybox/sqlite n'ont pas l'intrinsèque host-backé) ;
  **7za 38512 → 54214 scorées** (+15 700 memmove-callers), **0 divergence**. Portes : régression unifiée **PASS**, hash
  transpile `19acad982194bf07` inchangé (oracle-only), winediff intact.
- **7za : le crash n'est TOUJOURS pas un lift scorable.** Même les memmove-callers (`sub_470889`/`sub_471c08`/
  `sub_471830`/`sub_46cf4c`) sont désormais **prouvés corrects** (0 div). ⇒ le crash est hors du lift scorable :
  fonction encore skippée (import à pop **inconnu**/cdecl, `@0` **pointeur** non-stubbable, x87, ou un autre intrinsèque
  unliftable non signaturé), ou une **dépendance environnement/heap** que l'oracle par-fonction ne peut structurellement
  pas voir (valeurs seedées ≠ layout réel). **La chasse 7za sort du périmètre funcdiff** — prochain angle : diff
  end-to-end (gdb watchpoint sur l'origine du garbage passé à sub_472126), ou audit des imports non-implémentés du
  chemin de démarrage. L'oracle a fait tout son travail : **il a prouvé que le lift n'est pas en cause**, sur ~54k
  fonctions/appels scorés de 7za.

### 2026-07-17 — [ORACLE][DEMO] Validation large de l'oracle élargi (0 div sur tout le gauntlet) + 7za borné (crash = startup CRT, hors funcdiff)
- **Sweep de validation** : funcdiff (avec les 5 incréments du jour — pops `@N`, appels indirects, `@0` scalaires,
  intrinsèques memmove, exclusion scratch sous-esp) lancé sur les **11 binaires distincts du gauntlet** (mingw C **et**
  MSVC) : `hello` 1320, `bzip2` 1302, `gzip` 1654, `grep` 1977, `sed` 1575, `m4` 2491, `units` 1320, `lua` 1527,
  `nasm` 3660, `sqlite3` 7303, `sqlite3_full` 7887 scorées — **0 divergence partout**. Les incréments sont donc **sound
  sur volume** (aucun faux positif, couverture bien plus profonde qu'avant : appels indirects + memmove-callers +
  import-callers désormais validés sur tous ces binaires réels). Pas de nouveau bug : ce sont des démonstrateurs déjà
  éprouvés (le lift est prouvé correct, plus profondément).
- **7za : chasse bornée (règle §2).** Le crash de démarrage (`aret_LCMapStringW ← sub_472126 ← … ← sub_46cf4c ← main`)
  est dans l'**init locale du CRT statique**, AVANT `main`. `sub_46cf4c` = le sas de démarrage CRT (appelle `GetVersion`,
  **`GetCommandLineA`**, `ExitProcess`, puis l'init). funcdiff a **prouvé le lift innocent** sur ~54k fonctions/appels de
  7za, y compris les memmove-callers. Le fautif est donc structurellement **hors funcdiff** : la chaîne passe par
  `GetCommandLineA` (@0 **pointeur**, non-stubbable soundement) et du code startup que l'oracle par-fonction (états
  seedés ≠ layout réel) ne peut pas valider. **Écarté** : heap non-init (`aret_HeapAlloc`→always-zero **n'a pas** changé
  le crash ; le malloc de 7za est lifté, pas host-backé), les shims vérifiés (GetACP=1252, memmove OK). **Reste** (pour
  une session dédiée) : diff instruction-par-instruction ARET vs Wine à travers le sas CRT, ou identifier ce qu'est le
  slot `call [0x48e040]` (hors IAT standard) de `sub_46cf4c`. **Pas de forensics mono-binaire infinie** ici : la valeur
  de la session est l'oracle élargi (5 incréments, validés sur 11 binaires), pas un fix 7za spéculatif.

### 2026-07-17 — [LIFT][ORACLE] Sweep de binaires INÉDITS → 1er vrai bug trouvé : `fstcw`/`_control87` lisait du garbage
- **Méthode : chasse sur du code inédit.** L'oracle élargi (5 incréments du jour) ne trouvait plus rien sur les
  démonstrateurs verts → il faut du **code jamais lifté**. Récupéré des PE 32-bit de vieux CD-ROM (archive.org, Chip
  ISO). Sweep funcdiff par binaire → **`dxfix.exe` : 6 divergences** (2 fonctions `_control87`), les autres 0-div.
- **Cause racine (vrai bug produit, silencieusement faux).** `fstcw [m]`/`fnstcw` (store du mot de contrôle x87) était
  lifté en **`Nop`** dans le chemin statique → la destination n'était **pas écrite** → un appelant qui la relit
  (`_control87`/`_controlfp`) lisait de la **pile non-initialisée = garbage**. Viole le principe sacré (faux silencieux).
- **Fix (portable, sound).** `fstcw`/`fnstcw` → **store constant `0x037F`** = le mot de contrôle x87 par défaut d'un
  process Linux/ELF (toutes exceptions masquées, précision étendue, arrondi au plus proche), donc `_control87` lit la
  **vraie** valeur par défaut. Pur constant (aucun état FPU runtime, aucun global) ⇒ portable WASM **et** sans souci de
  link (contrairement à passer par `__x87rt_stcw` qui référence des globals — tenté, cassait le link difftest
  `x87arith`/`x87div`). L'arrondi installé par `fldcw` reste tracké **statiquement** (RoundMode compile-time) pour
  `frndint`/`fist` ; seule la relecture d'un CW **custom** posé par `fldcw` reste non modélisée (rare) — plus jamais du
  garbage. **funcdiff** : seed le FPCW d'Unicorn à `0x037F` (`UC_X86_REG_FPCW`) pour qu'il matche le modèle → les
  fonctions `_control87` sont désormais **scorées et validées** (dxfix 2302→2380 scorées, **0 divergence**).
- **Portes** : régression unifiée **PASS**, difftest **272/272** (le constant n'introduit aucun global, contrairement à
  la 1ʳᵉ tentative), hash transpile `19acad982194bf07` **inchangé**, winediff **114/114**, sweeps sqlite/busybox
  bit-identiques (les démonstrateurs MSVC appellent `_control87` au startup — lisaient du garbage, lisent maintenant
  0x037F, sans changement fonctionnel), corpus funcdiff **20501, 0 div**.
- **Leçon méthodo confirmée** : l'oracle élargi **trouve de vrais bugs sur du code inédit** (ce que les démonstrateurs
  déjà verts ne montrent plus). C'est la voie pour « terminer les 32 bits » : sweeper des binaires neufs, fixer, itérer.
  *(Le CD 1997 est surtout 16-bit/DOS ; peu de 32-bit complexe. Pour la suite : des binaires plus modernes/C++ lourd.)*

### 2026-07-17 — [LIFT][RECOV] Sweep suite : `Ppview32.exe` → 2ᵉ vrai bug = idiome `push imm; …; ret` (ret-as-jump) du EH MSVC, non géré
- **Sweep des gros PE32 restants** (Ppview32 = PowerPoint Viewer MSVC, itmnm2095, uninst, wzbeta32) : seul **Ppview32
  diverge** (33273 scorées, **6 divergences** dans `sub_530bf0`/`sub_530d20`/`sub_530d26`), les autres 0-div.
- **Cause (vrai bug produit, EH).** Ces fonctions sont des helpers d'**exception MSVC** (frame SEH + itération de
  callback). Elles utilisent l'idiome **`push <addr>; … ; ret`** où le `ret` **saute** vers l'adresse poussée (une
  continuation `__finally`), PAS un retour. Ex. `sub_530bf0` : `530c49 push 0x530c6a` … `530c69 ret` → devrait sauter à
  l'épilogue `0x530c6a` (qui restaure esi/ebx/edi/ebp, démonte le frame SEH, `ret 0x10`). **ARET (et le produit) traite
  ce `ret` comme un retour de fonction** → sort en `530c69` avec `ebp=frame` non restauré, épilogue **sauté**, mauvais
  `ret N` → pile corrompue. funcdiff le voit : `reg r5(ebp) lifted=0x10007ffc (frame) unicorn=<restauré>`.
- **Statut : lead borné, pas fixé ici.** L'analyse ne reconnaît pas `push imm; ret` comme un saut (grep : aucun
  handling). Le cas Ppview32 est la forme **différée** (push continuation → bloc finally → ret), pas le `push imm; ret`
  adjacent — sa reconnaissance demande de suivre la pile à travers l'EH = c'est le **chantier SEH/EH** (doc 80 §1.3),
  pas un fix rapide. **Confirme que l'EH est la prochaine frontière pour les 32 bits.**
- **Bilan méthode** : 2 sweeps de binaires inédits → 2 vrais bugs (fstcw **fixé**, push-ret/EH **documenté**). La boucle
  « sweep → fix » marche ; les bugs restants convergent vers **l'EH MSVC** (push-ret, SEH table-driven, C++ exceptions).

### 2026-07-17 — [RECOV][LIFT] `push imm; …; ret` (ret-as-jump, continuation MSVC `__finally`) reconnu — 1re brique EH, Ppview32 fixé
- **Le bug (Ppview32, trouvé au sweep).** L'idiome de continuation `__finally` : `push <cont>; …; ret` où le `ret`
  **saute** vers l'adresse poussée (pas un retour). ARET traitait ce `ret` comme un retour → sortait tôt, `ebp` non
  restauré, épilogue SEH sauté, mauvais `ret N` → pile corrompue. funcdiff : `sub_530bf0`/`sub_530d20`/`sub_530d26`,
  `reg ebp lifted=frame vs unicorn=restauré`.
- **Fix (analyse, sound par construction).** `find_ret_jumps` (`analysis/mod.rs`) : **interprétation abstraite
  forward** par fonction — pile symbolique (chaque slot = une constante-code poussée précise, ou opaque), point-fixe ;
  un `ret` est réécrit en `jmp <imm>` **uniquement** quand `[esp]` est prouvé être la **même** constante-code poussée
  sur **tous** les chemins (exactement l'adresse que le `ret` hardware pop et vers laquelle il saute). Un vrai retour
  (`[esp]`=adresse appelant, opaque) n'est jamais converti. `stack_pointer_increment` d'iced donne l'effet esp par
  instruction ; un `call` utilise le **callee-pop** (`callee_ret_pop`, mémoïsé, scan des `ret N` du callee) ; tout
  effet non modélisé (call indirect, `mov esp,…`, écriture pile désalignée) rend les slots opaques → jamais une fausse
  constante ⇒ la passe ne peut que **rater** un saut, jamais en inventer un.
- **Trois pièges résolus** : (1) le `ret` réécrit **pop toujours** `[esp]` → build.rs émet `esp += 4` avant le `jmp`
  (sinon l'épilogue cible lit de mauvais slots) ; (2) la continuation est souvent **mal-promue en entrée de fonction**
  (address-taken du `push`) → `is_cont` accepte toute adresse-code du binaire dans un span généreux (la soundness vient
  de la preuve abstraite, pas de `is_cont`), et `insns` est **étendu** en collectant depuis la cible ; (3) `callee_pop`
  d'un helper EH qui a lui-même un push-ret : un plain `ret` (N=0) coexistant avec un `ret N` (N>0) EST un push-ret jump
  → on ignore les N=0 quand un N>0 existe (une fonction = une convention d'appel).
- **Mesuré** : **Ppview32 6 divergences → 0** (les 3 fonctions EH liftées correctement, +53 continuations incluses).
  **Aucune régression** : difftest **272/272**, hash transpile `19acad982194bf07` **inchangé**, régression unifiée
  **PASS**, winediff **114/114**, sweeps sqlite/busybox bit-identiques, gauntlet **19/21**, funcdiff corpus **0 div**,
  7za 0 div. La passe ne se déclenche sur AUCUNE fonction sans push-ret (les démonstrateurs sans EH sont intacts).
- **1re brique du chantier EH franchie.** Reste (doc 80 §1.3) : teardown de frame SEH complet (7za, 96 frames),
  dispatch `__except_handler3`, exceptions C++ (`__CxxFrameHandler`/RtlUnwind). La boucle sweep→fix continue.

### 2026-07-17 — [HLE-WIN32][EH] Chantier EH lourd, brique 1 : dispatch SEH software-raised (`RaiseException`)
- **Le manque.** La chaîne de handlers SEH (`fs:[0]`) et ses `EXCEPTION_REGISTRATION` sont **déjà** maintenues (le
  prologue lifté `push handler; push scopetable; push -1; mov fs:[0],esp` écrit dans le TEB synthétique ; les reads
  `fs:[ea]` sont de vrais loads — `ir/lift.rs` transpile-mode). Manquait le **DISPATCH** : `RaiseException` était un
  stub no-op → le programme **continuait après l'exception** (faux silencieux ; fixture : `r=1` au lieu de `r=42`).
- **Fix : `aret_RaiseException` (`aret_hle.c`).** Parcourt la chaîne `fs:[0]` (TEB[0], frames sur la pile machine =
  mémoire host réelle en shared-stack) ; pour chaque frame, appelle le handler **cdecl** `handler(EXCEPTION_RECORD*,
  EstablisherFrame, CONTEXT*, DispatcherContext)` via `aret_call` (frame cdecl posé sous l'esp machine, comme le
  trampoline qsort). Un handler qui **décline** rend `ExceptionContinueSearch(1)` → frame suivante ; un qui **catch**
  transfère non-localement (longjmp, ou le scope-jump de `__except_handler3`) et ne revient jamais. Chaîne épuisée sans
  catch ⇒ **abort bruyant** (jamais continuer en silence). WASM : reste un abort sound (pas de SEH).
- **Testabilité résolue** (le doc 80 §1.3 la notait dure). mingw i686 ne compile pas `__try/__except`, mais on installe
  le frame SEH **à la main en inline-asm** (exactement ce que MSVC émet) + un handler C qui catch via `longjmp`.
  Fixture `winecorpus/seh_raise.c` : **deux frames imbriqués** (interne décline `ContinueSearch`, externe catch) →
  teste le **parcours de chaîne**. Wine **et** ARET → `r=42`.
- **Portes** : winediff **114→115/115** (seh_raise), hash transpile `19acad982194bf07` **inchangé**, régression
  unifiée **PASS**, sweeps sqlite/busybox bit-identiques. Le stub no-op (faux silencieux) est remplacé par un dispatch
  correct — gain de soundness pour tout binaire appelant `RaiseException`.
- **Reste EH** (briques suivantes) : `__except_handler3` réel (scope-table, filtre, unwind local — nécessite un CONTEXT
  peuplé et le local-unwind), fautes **matérielles** (SIGSEGV/#DE → dispatch, natif seulement), exceptions C++
  (`_CxxThrowException`/`__CxxFrameHandler`). Le résidu 7za est **orthogonal** (init locale C++, aucune exception avant
  le crash — vérifié).

### 2026-07-17 — [HLE-WIN32][EH] Chantier EH lourd, brique 2 : local unwind (`RtlUnwind`)
- **Le manque + une croyance fausse corrigée.** `RtlUnwind` (le primitif que `__except_handler3`, via
  `__global_unwind2`, utilise pour dérouler les frames intermédiaires en exécutant leurs handlers `__finally`) était un
  **abort-sound** (froid, cf. 70 §4.5). Une croyance de session antérieure le disait **non testable vs Wine** car « il
  ne revient pas — il transfère à TargetIp ». **C'est le comportement x64.** Sur **i386** (source Wine `signal_i386.c` +
  **mesure directe**), `RtlUnwind` **IGNORE TargetIp** : il parcourt `fs:[0]` de la tête jusqu'à (**exclu**) la
  TargetFrame, appelle chaque handler intermédiaire avec le flag **`EH_UNWINDING(0x2)`**, **pop** chaque frame, puis
  **RETOURNE NORMALEMENT** (laissant `fs:[0]` = TargetFrame) ; le saut non-local vers le bloc `__except` est fait par
  l'appelant **après**. ⇒ un **shim à retour normal est le modèle fidèle**, et il est **directement testable vs Wine**.
- **Fix : `aret_RtlUnwind` (`aret_hle.c`).** stdcall @16 `RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue)`.
  Record : celui de l'appelant si fourni (Wine y OR le flag `EH_UNWINDING` en place), sinon un `STATUS_UNWIND(0xC0000027)`
  synthétisé ; `EH_EXIT_UNWIND(0x4)` ajouté si TargetFrame NULL (exit unwind = dérouler toute la chaîne). Boucle sur
  `fs:[0]` : pour chaque frame ≠ target, appelle le handler **cdecl** via `aret_call` (frame posé sous l'esp machine,
  comme le dispatcher `RaiseException`), **pop** (`teb[0]=next`), avance. Stop à (exclu) target ⇒ `fs:[0]`=target. Retourne
  `ReturnValue` (→ eax). **Bornes sound** : compteur de garde (chaîne cyclique → abort) ; target introuvable sur une
  chaîne finie (`STATUS_INVALID_UNWIND_TARGET`) → **abort bruyant** (jamais deviné). WASM : reste un abort sound.
- **Testabilité (la « muraille » tombée).** Le mental-model x64 faisait croire au blocage. La mesure i386 prouve le
  contraire → fixture `winecorpus/seh_unwind.c` : deux frames SEH installés à la main (mingw n'a pas `__try`), **appel
  DIRECT à `RtlUnwind`** (exactement ce que fait `__global_unwind2`) pour dérouler l'inner jusqu'à l'outer. Observe :
  inner appelé **1×** avec le flag unwinding, outer (cible) **jamais**, `fs:[0]`=outer. **Piège d'auteur résolu** :
  `RtlUnwind` restaure les registres non-volatils → GCC gardait une **copie périmée** du pointeur de frame en registre
  callee-saved (comparaison `fs0==outer` fausse en `-O1` alors que les valeurs brutes `%p` étaient égales) → on **stashe
  les pointeurs observés via des globals** pour un résultat stable `-O0`/`-O1`. Wine **et** ARET → `inner=1 flags=0x2
  fs0_target=1`.
- **Portes** : winediff **115→116/116** (seh_unwind), hash transpile `19acad982194bf07` **inchangé** (runtime-only,
  additif : `aret_RtlUnwind` override le stub faible), régression unifiée **PASS**, difftest **272/272**, sweeps
  sqlite/busybox bit-identiques. Gain de soundness pour tout binaire MSVC déroulant réellement une exception.
- **Reste EH** : `__except_handler3` réel (scope-table + filtre + `local_unwind2` — maintenant que `RtlUnwind` existe,
  c'est la **suite directe**, mais sa testabilité demande un **vrai binaire MSVC `__try/__except`** — le lift de
  `__except_handler3` statique-CRT — car mingw ne l'émet pas et un scope-table fait-main serait circulaire) ; fautes
  matérielles (SIGSEGV/#DE → dispatch, natif) ; C++ (`_CxxThrowException`/`__CxxFrameHandler`).

### 2026-07-17 — [ORACLE][DEMO] Sweep du corpus MSVC 1997 (CD Chip ita 7-8-97) — lift 0-div sur du code inédit
- **Source (fournie par l'utilisateur, gardée en mémoire, cf. 70 §7)** : `archive.org/download/chip-cd-ita-7-8-97/
  Chip_CD.iso` (657 Mo ; **curl `-L`** — redirect miroir). `chip-cd-ita-3-97` = BIN/CUE 742 Mo (non balayé).
  `network-32` **ne résout pas** (item dark, 0 fichier). Extraction : `7z x -r` (pas de mount, pas de root).
- **Composition mesurée** : 424 PE, dont **49 PE32** — le gros du CD est **16-bit NE** (Win3.x) ou des
  self-extractors InstallShield (confirme la note antérieure « CD 1997 surtout 16-bit »). Le 32-bit contient de
  vrais MSVC static-CRT : `itiem95`, `ARTLANT`, DomuS3D `DEMO32`/`DEMODS3D`, `slidelib`, la **démo AutoCAD LT**
  entière (`acis.dll` = noyau géométrique ACIS, `mfcans32`/`mfcuia32` MFC, `msvcrt20`), + les déjà-connus
  `Ppview32`/`dxfix`/`itmnm2095`/`wzbeta32`.
- **funcdiff (sweep par binaire)** : `ARTLANT` 20610, `DEMO32` 6559, `itiem95` 2959, `DEMODS3D` 1555, `slidelib`
  1472 scorées — **0 divergence partout** (~34k fonctions de code MSVC **inédit**). Le lift est **prouvé correct**
  sur ce nouveau matériel ; **aucun bug neuf** (contrairement à dxfix→fstcw et Ppview32→push-ret des sweeps
  précédents). Les `.dll` (`acis`/`libacge`) scorent **0** : funcdiff ne peut pas piloter une DLL isolée (pas
  d'entrée/main) → il faut le **loader multi-modules** (doc 80 §1.2, endgame GUI).
- **Verdict (règle « borner puis pivoter »)** : ces CD sont un **bon vivier de code MSVC frais** (et de MSVC-C++/MFC
  pour les briques EH C++/`__except_handler3` **futures**), mais (a) ils ne débloquent **pas seuls** la brique 3 —
  funcdiff prouve le lift **par-fonction** correct, il n'**exécute** pas le dispatch EH runtime (`__except_handler3`
  + unwind end-to-end) ; (b) le 7-8-97 n'a **pas** sorti de bug de lift. Pour la brique 3 il faut un binaire qui
  **franchit réellement** un unwind `__try/__except` (chemin de démarrage, ou app pilotable headless), ou le loader
  multi-modules pour animer les DLL MFC. Import scan **inopérant** ici (static-CRT ⇒ `__except_handler3` **interne**,
  ni importé ni en clair). Pas de forensics spéculative : la valeur du jour = brique 2 (`RtlUnwind`) + validation large.

### 2026-07-17 — [HLE-WIN32][EH] Chantier EH lourd, brique 3 : fautes matérielles (SIGSEGV→dispatch SEH)
- **Le manque.** Sur Windows un **trap CPU** (access violation, div0, …) est transformé par le noyau en **dispatch
  SEH** : parcours de `fs:[0]`, appel de chaque handler, exactement comme un `RaiseException` logiciel. ARET exécute
  les accès mémoire du programme comme de **vrais load/store hôte** → un tel trap arrive en **signal hôte** (SIGSEGV/
  SIGFPE) et **tuait le process** (aucun dispatch). Un programme qui protège un accès fautif par `__try/__except` (ou
  un frame SEH manuel) devait donc **catch et continuer** ; il crashait.
- **Fix : `aret_hw_fault` + constructor `aret_hw_fault_install` (`aret_hle.c`, natif seulement).** `sigaction(SIGSEGV/
  SIGFPE, SA_SIGINFO|SA_NODEFER)` posé à l'init (ELF constructor). À la faute : construit un `EXCEPTION_RECORD`
  (SIGSEGV→`STATUS_ACCESS_VIOLATION 0xC0000005` + `ExceptionInformation[0]`=read/write depuis `REG_ERR` du ucontext +
  `[1]`=`si_addr` ; SIGFPE→`STATUS_INTEGER_DIVIDE_BY_ZERO 0xC0000094`) puis **dispatch dans `fs:[0]`** comme
  `RaiseException`. **Découverte clé (pile scratch dédiée).** L'esp machine du **point de faute** est enfoui dans un
  registre hôte et **irrécupérable** du contexte signal — mais **inutile** : on exécute le handler sur une **pile
  scratch dédiée** (`aret_eh_stack`), et un handler qui **catch restaure l'esp depuis son propre registration record**
  (scope-jump `__except_handler3` / longjmp du fixture), piloté par la **donnée du frame**, pas par l'esp du
  dispatcher. `SA_NODEFER` garde le signal catchable **à travers le longjmp** de sortie (le longjmp d'un handler de
  signal vers un `setjmp` antérieur marche car la pile hôte mirror 1:1 la pile logique — cf. shim setjmp/longjmp).
- **Soundness (vérifiée).** Chaîne épuisée sans catch ⇒ **faute réelle non gérée** : `SIG_DFL` + re-faute → le process
  meurt du **vrai signal** (message stderr `unhandled hardware exception`, jamais avalé en silence). Prouvé : fixture
  NULL-deref **sans** handler → « before » imprimé, **« after » jamais** (Wine idem : crash). Garde de ré-entrance
  (faute dans le dispatch → abort). WASM : pas de signaux POSIX ⇒ mécanisme **exclu** (faute = trap sound).
- **Testable vs Wine.** Fixture `winecorpus/seh_hwfault.c` : frame SEH manuel (mingw n'a pas `__try`), NULL-deref dans
  le corps protégé, handler qui vérifie `ExceptionCode==0xC0000005` et sort par `longjmp`. Wine **et** ARET → `r=42
  code=0xc0000005`.
- **Portes** : winediff **116→117/117** (seh_hwfault), hash transpile `19acad982194bf07` **inchangé** (runtime-only,
  additif), régression unifiée **PASS** (difftest 272/272, funcdiff 20501 / 0 div, SMT 11/11, recompilabilité 100%),
  **sweeps sqlite (bit-identiques) + busybox 60/60** — le handler global de signal **ne perturbe pas** les
  démonstrateurs (ils ne fautent pas ; handler jamais déclenché). Les 3 fixtures SEH passent (`seh_raise`/`seh_unwind`/
  `seh_hwfault`).
- **Reste EH** : `__except_handler3` réel (scope-table — testabilité = **vrai binaire MSVC `__try/__except`**, mingw ne
  l'émet pas) ; C++ (`_CxxThrowException`/`__CxxFrameHandler`). Les 3 primitives de dispatch (software `RaiseException`,
  local unwind `RtlUnwind`, faute matérielle) sont **faites et prouvées** ; il ne reste que le handler MSVC de haut
  niveau qui les orchestre (scope-table) et les exceptions C++.
- **Note corpus (option 1, mesurée).** 6 binaires MSVC du CD 1997 lancés end-to-end sous ARET headless : **aucun** ne
  franchit un chemin EH — ils s'arrêtent (abort sound) plus tôt sur des **appels indirects non résolus** (`itiem95`/
  `DEMO32`/`slidelib` = points-to/Phase 4, le levier P3) ou un **import manquant** (`ARTLANT` : `SystemParametersInfoA`) ;
  `Ppview32`/`dxfix` tournent proprement. ⇒ le mur dominant sur ce corpus frais est le **points-to**, pas l'EH ; la
  brique 3 reste néanmoins un vrai incrément EH testable (fixture), livré ici.

### 2026-07-17 — [RECOV] Récup des pointeurs de fonction FPO isolés (cible précédée d'un terminateur) — le mur points-to mesuré
- **Le mur mesuré (option 1).** 6 MSVC du CD 1997 lancés end-to-end sous ARET headless : le mur dominant n'est PAS l'EH
  mais le **points-to** — `slidelib`/`DEMO32`/`itiem95` abortent (sound) sur un **appel indirect vers une fonction non
  récupérée** (`0x405f22`/`0x432664`/`0x100a200`). Diagnostic : chacune est une **vraie fonction FPO** (frame-pointer
  omis → pas de prologue `push ebp`), stockée comme **pointeur isolé initialisé statiquement en `.data`** (référencée
  **1×**, entourée de zéros), et **immédiatement précédée d'un terminateur propre** (`ret`/`ret N` ou padding `int3`).
- **La lacune.** Le scan de données (`analysis/mod.rs`, pass 2b) accepte un pointeur-code **isolé** (hors table ≥3)
  uniquement s'il passe `looks_like_func_start` (prologue reconnu). Une fonction FPO ouvrant sur `push imm`/`cmp [m],imm`
  échoue ce gate → jamais récupérée → l'appel indirect aborte. Motif **général** (init statique de callbacks/dispatch),
  vérifié **identique sur les 3 binaires**.
- **Fix (sound par frontière).** Helper `preceded_by_terminator(prog, global, addr)` : un pointeur-code isolé
  **address-taken** est un vrai début de fonction dès qu'une **frontière prouvée** le précède — (A) une instruction déjà
  décodée finit **exactement** à `addr` et est un terminateur (`ret`/`ret N`/`jmp`) = dernière insn de la fonction
  précédente, ou (B) l'octet avant `addr` est `int3(0xCC)` (padding inter-fonction MSVC, jamais du code fall-through
  interne). **Sound par construction** : récupérer depuis une frontière prouvée ne peut pas tronquer une fonction (rien
  ne franchit un terminateur), et un mot de données coïncidant avec une telle adresse tombe quand même sur un vrai début
  (au pire une fonction morte, liftée juste ou abort sound — jamais un miscompile). Gaté aux candidats déjà
  address-taken (pointeur de données / immédiat code), jamais un seed de balayage linéaire.
- **Mesuré (bénéfice réel).** Murs avancés : `slidelib` `0x405f22`→ récupéré (mur suivant `0x404926`), `DEMO32` l'appel
  indirect résolu (bute ensuite sur l'import `GetClassInfoA`), `itiem95` `0x100a200`→ récupéré (tombe sur `0x80000011`
  = valeur non-code, autre problème). **+89 fonctions FPO récupérées dans les démonstrateurs mêmes** (busybox
  5202→5291, funcdiff corpus **20501→20590 scorées, 0 divergence**) — la classe existe partout, désormais couverte.
- **Portes (récup = zone la plus risquée → régression complète).** Hash transpile `19acad982194bf07` **inchangé**,
  régression unifiée **PASS** (difftest 272/272, funcdiff **20590 / 0 div**, SMT 11/11, recompilabilité 100%), winediff
  **117/117**, sweeps sqlite (bit-identiques) + busybox **60/60**, **gauntlet 19/21** (inchangé). Zéro régression : le
  gate ne se déclenche que sur une frontière **prouvée**, jamais un faux positif.

### 2026-07-17 — [HLE-WIN32] `GetClassInfo(Ex)A/W` — round-trip d'une classe fenêtre (mur DEMO32)
- **Le manque (mesuré).** Après le fix récup points-to, `DEMO32.EXE` (CD Chip 1997) avançait jusqu'à
  `GetClassInfoA` **non implémenté** → abort. Le registre de classes ne gardait que `wndproc`/`hbrBackground`/nom.
- **Fix : registre de classe complet + 4 shims.** `g_u32_class` stocke désormais **tous** les champs `WNDCLASS(EX)`
  (style, cbClsExtra, cbWndExtra, hInstance, hIcon, hCursor, hbrBackground, menu, hIconSm) — capturés par les 4
  `RegisterClass(Ex)A/W` (offsets WNDCLASS 40 o / WNDCLASSEX 48 o). `GetClassInfoA/W`+`GetClassInfoExA/W`
  (`u32_get_class_info`) rendent ces champs **verbatim** → un register→query round-trip est **exact**. Retour = l'atome
  (non-nul) si trouvée, **0** sinon. La forme A **élargit** le nom (string→wide) pour la lookup partagée ; une classe
  passée en **atome** (< 0x10000) passe直. `stdcall_pops` : +`GetClassInfo{,Ex}{A,W}`@12.
- **Fidélité mesurée (piège trouvé).** `GetClassInfoEx` **n'écrit pas** `cbSize` (l'appelant le pré-remplit) : le
  fixture poisonne la struct à `0xAB` et Wine **laisse** `cbSize=0xABABABAB` → le shim ne touche pas `o[0]`. Sans ça,
  divergence (`cbSize=48` vs poison). Les handles/pointeurs (wndproc/hInstance/hIcon/hCursor/hbr) sont comparés en
  **round-trip** (adresses non déterministes), les scalaires (style/extras) en **verbatim**.
- **Portes** : winediff **117→118/118** (`user32_getclassinfo`), `user32_classex` (RegisterClassEx) **toujours vert**
  (le refactor du registre est propre), hash transpile `19acad982194bf07` **inchangé**, `table_is_sorted_by_name`
  **ok**, régression unifiée **PASS**. Débloque `DEMO32` (mur suivant `CharToOemA` — import narrow/OEM, autre famille).

### 2026-07-17 — [ABI][RECOV] Imports par ORDINAL résolus (COMCTL32 #17 = InitCommonControls) — le mur `0x80000011` d'itiem95
- **Diagnostic (gdb sur le C généré).** `itiem95.exe` (CD 1997) abortait `indirect call to unrecovered function
  0x80000011`, très tôt (chaîne `main → sub_10066e0 → sub_1003020 → sub_1003084 → aret_call(0x80000011)`). `sub_1003084`
  fait `call *0x10010c8` (slot IAT). `0x80000011 = 0x80000000|0x11` = **import par ordinal 17**. objdump confirme : le
  seul import ordinal d'itiem95 = **COMCTL32 #17** (`<none>`). COMCTL32 #17 = **InitCommonControls** (base ordinale 2,
  EAT index 15).
- **La cause (générale).** Le loader (`parse_pe_imports`) faisait `continue` sur `thunk.is_ordinal()` → l'import était
  **jamais mappé** → le slot gardait la valeur brute `0x80000011`, appelée → abort opaque. C'est une **classe** (toute
  appli liant comctl32/mfc/ws2_32 par ordinal), pas un cas isolé.
- **Fix : résolution `(dll, ordinal) → nom`.** Nouveau module `src/ir/ordinal_imports.rs` : table COMCTL32 (126 entrées)
  **extraite verbatim** de l'export table du `comctl32.dll` **que Wine exécute** — notre oracle. Wine matche la
  numérotation d'ordinaux de Microsoft **par conception** (sinon les apps par-ordinal casseraient sous Wine) ⇒ le
  mapping est **correct par construction** vs l'oracle (aucune supposition ; ordinaux ABI-stables). Le loader résout
  l'ordinal en nom → le routage par-nom (shim) reprend. Le shim `aret_InitCommonControls` **existait déjà** (no-op) : il
  ne manquait QUE la résolution d'ordinal. Inconnu (dll/ordinal) ⇒ non résolu (abort sound, jamais deviné).
- **Testabilité.** Unit tests Rust (`comctl32 #17 → InitCommonControls`, dll/ordinal inconnu → None, table triée).
  **Bit-exact vs Wine** : le harness winediff gagne le support `NAME.def` (dlltool → import lib liée **en premier**, pour
  forcer un import par ordinal que `-lcomctl32` fournirait sinon par nom) ; fixture `winecorpus/comctl32_ordinal.{c,def}`
  importe InitCommonControls **par ordinal 17** et appelle → Wine **et** ARET = `ok`.
- **Mesuré.** `itiem95` : `0x80000011` **disparu** → avance au mur suivant `DialogBoxIndirectParamA` (import nommé, autre
  famille). **Portes** : winediff **118→119/119** (`comctl32_ordinal`), hash transpile `19acad982194bf07` **inchangé**
  (loader-only, inerte pour les binaires à imports **nommés** = tous les démonstrateurs), régression unifiée **PASS**,
  tests ordinal_imports **3/3**.

### 2026-07-17 — [HLE-WIN32] `CharToOem`/`OemToChar` (ANSI CP1252 ↔ OEM CP437) — mur DEMO32, tables extraites de Wine
- **Le mur (mesuré).** Après `GetClassInfoA`, `DEMO32.EXE` (CD 1997) butait sur `CharToOemA` non implémenté.
- **La subtilité : best-fit, pas strict.** `CharToOemA` convertit ACP(1252)→OEMCP(437) en **best-fit** : un caractère
  CP1252 sans forme CP437 exacte prend la plus proche (U+201A `‚` → `,`, U+2019 `’` → `'`), sans équivalent → `?`, et
  un caractère présent dans les deux mappe direct (`é` 0xE9 → 0x82). Une table stricte (codec Python) **ne matche pas**.
- **Fix : tables extraites verbatim de Wine (l'oracle).** Un programme jetable a lancé `CharToOemA`/`OemToCharA` de Wine
  sur **les 256 valeurs d'octet** → 2 tables de 256 (`u32_ansi_to_oem`/`u32_oem_to_ansi`) hardcodées → **bit-identique
  Wine par construction** (ACP=1252/OEMCP=437 confirmés). Shims `CharToOem{,Buff}A`/`OemToChar{,Buff}A` (formes A
  NUL-terminées copient le NUL ; formes Buff = longueur explicite). `stdcall_pops` : +CharToOemA@8/CharToOemBuffA@12/
  OemToCharA@8/OemToCharBuffA@12.
- **Mesuré (fort levier).** `DEMO32` traverse désormais **tout son démarrage** (récup points-to + GetClassInfo + ordinal
  InitCommonControls + CharToOem) et atteint sa **boucle de messages** (`GetMessageW: empty queue` — repli headless
  attendu, pas un bug). Le binaire s'initialise entièrement. **Portes** : winediff **119→120/120** (`win32_charoem`,
  signature round-trip 256 octets `sig=67a9e5e5` = Wine), hash transpile `19acad982194bf07` **inchangé**,
  `table_is_sorted_by_name` ok, régression unifiée **PASS**.

### 2026-07-17 — [RECOV] `preceded_by_terminator` étendu au `ret`(0xC3) — slidelib `0x404926`, + wzbeta32 = C++ EH (borné)
- **Suite du sweep end-to-end.** Après les fixes du jour, re-run des MSVC du CD 1997 : `slidelib`→`0x404926`,
  `wzbeta32`→`0x400000`, `ARTLANT`→`SystemParametersInfoA`, `itiem95`→`DialogBoxIndirectParamA`.
- **wzbeta32 `0x400000` = C++ EH (diagnostiqué, borné).** gdb : `sub_404b88` (chaîne startup). Sa disasm = machinerie
  **exception C++ MSVC** : walk `fs:0`, `cmp eax, 0x56433230` ("VC20" magic), struct de registration, dispatch tail
  `jmp *[edx+0x14]`. C'est `__CxxFrameHandler`/EH C++ — **même chantier que la brique EH restante** (`__except_handler3`/
  C++), pas un bug isolé. `0x400000` (image base) = champ EH non peuplé. **Borné** (chantier EH lourd).
- **slidelib `0x404926` = même motif FPO, gap de la brique récup.** Disasm : `0x404926` précédé d'un `ret`(0xC3) à
  `0x404925`, référencé **1×** par un pointeur `.data` isolé (`0x40b168`, entouré de zéros) — exactement le motif de
  `0x405f22`. Mais `preceded_by_terminator` le ratait : la fonction *précédente* n'est pas récupérée → son `ret` absent
  de `global` → (A) échoue ; et (B) ne testait que `int3`(0xCC), pas `ret`(0xC3).
- **Fix (extension sound de (B)).** (B) accepte désormais l'octet-avant ∈ {`0xCC` int3, `0xC3` ret} — tous deux
  **1 octet** ⇒ `addr` est une vraie frontière. **Piège évité** : tentative initiale de *décoder frais* à `addr-k` →
  cassait `0x405f22` (x86 non auto-synchronisant : `decode_at(addr-2)` trouvait un faux `adc [eax],al` de 2 o finissant
  à `addr` **avant** d'atteindre le vrai `ret 0x10` de 3 o à `addr-3`, et comme non-terminateur → rejet). Donc (A) reste
  **`global`-only** (décode linéaire autoritaire), (B) = test d'octet direct.
- **Mesuré.** `slidelib` 103→**111 fonctions**, franchit `0x405f22` **et** `0x404926` (ne bute plus sur un appel
  indirect). **Portes (récup = zone la plus risquée → tout passé)** : hash transpile `19acad982194bf07` **inchangé**,
  régression unifiée **PASS** (difftest 272/272, funcdiff **20558 / 0 div**, SMT 11/11, recompilabilité 100%), winediff
  **120/120**, sweeps sqlite + busybox **60/60** bit-identiques, **gauntlet 19/21** (inchangé). Zéro régression malgré
  l'octet `0xC3` (plus permissif) — la preuve address-taken + frontière tient.

### 2026-07-17 — [HLE-WIN32] `SystemParametersInfo(A/W)` — mur ARTLANT, actions GET vérifiées + SET stateful
- **Le mur.** `ARTLANT.EXE` (CD 1997) butait sur `SystemParametersInfoA` non implémenté. gdb→ le diag d'abort a
  révélé l'action précise : **`0x11` = `SPI_SETSCREENSAVEACTIVE`** (désactivation de l'écran de veille pendant la démo).
- **Fix.** Actions GET display-indépendantes à valeur déterministe (vérifiées vs Wine headless) : `SPI_GETBEEP`=1,
  `GETBORDER`=1, `GETSCREENSAVEACTIVE`=1(défaut), `GETDRAGFULLWINDOWS`=0, `GETWHEELSCROLLLINES`=3. `SPI_GETWORKAREA` =
  invariant écran 1024×768 (comme GetSystemMetrics). **`SCREENSAVEACTIVE` rendu STATEFUL** (global défaut 1) : SET stocke
  `uiParam`, GET relit — **vérifié vs Wine** (`SET(0);GET→0 ; SET(1);GET→1`). Un SET no-op aurait divergé d'un GET
  ultérieur. Actions non modélisées ⇒ **abort sound** (retourner FALSE sans écrire pvParam = un appelant qui ignore le
  retour lit de la mémoire non-init valide sous Wine = faux silencieux). `stdcall_pops` : +SystemParametersInfoA/W@16.
- **Piège testabilité (résolu).** Le harness winediff lance Wine sur un Xvfb **1280×1024** ; `SPI_GETWORKAREA` y rend
  {0,0,1280,1024} alors qu'ARET rend son invariant {0,0,1024,768} → DIFF. C'est **par conception** (valeurs écran =
  invariant, hors oracle bit-exact, doc 72 §4.5) → le fixture ne teste **que** les valeurs display-indépendantes + le
  round-trip stateful (auto-relatif). L'impl de WORKAREA reste correcte (invariant), juste non bit-testable headless.
- **Mesuré.** `ARTLANT` avance au **rendu de texte GDI** (stock font sans face — chantier M7 graphisme). **Portes** :
  winediff **120→121/121** (`user32_spi`), hash transpile `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok,
  régression unifiée **PASS**.

### 2026-07-17 — [HLE-WIN32] `DialogBox/CreateDialogIndirectParamA/W` — dialogue depuis template mémoire (mur itiem95)
- **Le mur.** `itiem95.exe` (CD 1997), après les fixes du jour, butait sur `DialogBoxIndirectParamA` non implémenté.
  Diffère de `DialogBoxParamA` (déjà modélisé) **uniquement** par la source du DLGTEMPLATE : un **pointeur direct** en
  mémoire (host memory en shared-stack) au lieu d'un id de ressource.
- **Fix (factorisation).** Core modal (`u32_dialog_modal`) et modeless (`u32_dialog_modeless`) extraits — création +
  WM_INITDIALOG + pompe jusqu'à EndDialog, identiques quelle que soit la source du template. `DialogBoxParam*` passe
  `u32_dlg_template(id)`, `DialogBoxIndirectParam*` passe `(uint8_t*)WU(1)` direct ; idem CreateDialog*/Indirect.
  `stdcall_pops` : +DialogBoxIndirectParamA/W@20, CreateDialogIndirectParamA/W@20.
- **Testabilité (piège template résolu).** 1re fixture avec un contrôle → **Wine rejetait** le template (`result=-1`)
  car un `DLGITEMTEMPLATE` doit être **DWORD-aligné** (mon struct `#pragma pack(1)` ne l'était pas — ARET, plus laxiste,
  le lançait → divergence révélatrice). Simplifié à un template **zéro-contrôle** (header + menu/class 0 + titre) dont le
  DLGPROC `EndDialog` sur WM_INITDIALOG → Wine **et** ARET = `init param=77 / result=4242`.
- **Mesuré.** `itiem95` avance : `DialogBoxIndirectParamA` **résolu**, crée le dialogue → nouveau « mur » = **modal
  headless** (`DLGPROC did not EndDialog and no events`) — la limite **honnête** (un dialogue modal attend une entrée
  utilisateur absente headless), pas un bug. **Portes** : winediff **121→122/122** (`user32_dlgindirect`), hash transpile
  `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok, régression unifiée **PASS**. `user32_dialog` (ressource)
  toujours vert = la factorisation est propre.

### 2026-07-17 — [GUI] ARTLANT stock-font : SYSTEM_FONT = **bitmap** Wine (mesuré, borné — pas un quick win TrueType)
- **Hypothèse testée.** ARTLANT bute sur `TextOut`(SYSTEM_FONT) = abort sound « stock font (no face name) ». La DC
  sélectionne SYSTEM_FONT (13) par défaut, qui n'avait pas de face. Wine `GetTextFace`(SYSTEM_FONT) = **'Liberation
  Sans'**, LOGFONT {'System', height 16, weight 700}, tm {h16 asc13 desc3} → hypothèse : rendable comme les autres sans
  stocks via FreeType (bold → Liberation Sans Bold).
- **Mesure décisive (réfute l'hypothèse).** Fixture DIB-hash SYSTEM_FONT, Wine vs ARET (face='System'→Liberation Sans
  Bold, même fichier `LiberationSans-Bold.ttf` que Wine, ppem de height 16) : **vertical identique** (tm h16/asc13/desc3/
  weight700) mais **largeurs différentes** — Wine extent 'System Fn' = **60px** (ave 7), ARET **72px** (ave 9). Même
  fichier + même ppem ⇒ mêmes avances… sauf que Wine ne rend PAS Liberation Sans : il rend le **vrai bitmap System.fon**
  (compact), `GetTextFace` ne reporte 'Liberation Sans' que comme *plus proche match*. Le commentaire d'origine (« bitmap,
  abort soundly ») était **correct**.
- **Verdict (borné, soundness préservée).** Rendre SYSTEM_FONT comme Liberation Sans Bold produirait un texte **plus
  large que Wine** = faux silencieux → **révoqué** (règle « pas de changement sans bénéfice mesuré », « jamais faux
  présenté comme correct »). SYSTEM_FONT reste **abort sound**. Le mur ARTLANT est donc du vrai **M7 bitmap-font**
  (embarquer/répliquer le System.fon de Wine au pixel), pas un resolve TrueType — session dédiée. Commentaire du code mis
  à jour avec la mesure (60 vs 72px) pour éviter de re-tenter. Aucune régression (hash inchangé, gdi_textout MATCH).

### 2026-07-17 — [HLE-WIN32][LIFT] TEB StackBase/StackLimit = **vraies bornes** de la pile machine (bug général, sweep Win95)
- **Trouvé par le sweep sur du code neuf.** Nouveau corpus téléchargé (ISO **BestOfWindows95DotCom** WIN95_09964,
  565 Mo → 37 PE32 shareware Win95, compilateurs variés). Run end-to-end : `gifcon32.exe` → **faute matérielle**
  `0xc0000005 at 0x7ffefff8` (captée par le dispatch fautes du jour). gdb → `sub_401000` (1re fonction, sas CRT).
- **Cause racine (générale).** `sub_401000` fait `mov %fs:0x4,%edx` (lit **StackBase** du TEB) puis `mov -0x8(%edx),%eax`
  (déréférence `[StackBase-8]`) — idiome CRT MSVC (stack-cookie / bornes de pile). ARET posait `fs:[4]=0x7FFF0000`
  (placeholder **bidon**) → `[0x7FFF0000-8]=0x7FFEFFF8` = **mémoire non mappée** → segfault. La pile machine réelle est
  `aret_stack[]` (BSS), à une tout autre adresse. **Tout binaire lisant `fs:[4]`/`fs:[8]` et déréférençant** était
  touché (faux silencieux évité seulement parce que ça faultait).
- **Fix.** L'entrée émise (`aret_main.c`, connaît `aret_stack`) publie les **vraies bornes** au TEB avant de lancer le
  programme : `__aret_set_stack_bounds(top = aret_stack+taille, bottom = aret_stack)` → `aret_teb_init` pose
  `fs:[4]=StackBase` (haut) / `fs:[8]=StackLimit` (bas) réels (placeholder gardé en repli si pas d'entrée, ex. test
  unitaire — jamais déréférencé là). `[StackBase-8]` tombe alors dans `aret_stack` (valide). Ordre sûr : `main` publie
  avant tout code lifté ; `aret_teb_init` est paresseux (1er accès fs, dans le programme).
- **Mesuré.** `gifcon32` franchit la faute → avance au mur suivant (`0x408574`, gap récup). **Portes** : hash transpile
  `19acad982194bf07` **inchangé** (le hash couvre les fonctions liftées, pas `aret_main`), régression unifiée **PASS**,
  sweeps sqlite/busybox bit-identiques, gauntlet 19/21, winediff **122/122** — les démonstrateurs lisent aussi `fs:[4]`
  au sas CRT et tournent avec les vraies bornes (plus correct que le placeholder). **Bug général corrigé** (soundness :
  `fs:[4]` déréférençable = valeurs valides au lieu d'un crash).

### 2026-07-17 — [HLE-STDIO][CRT] `_controlfp`/`_controlfp_s` — mot de contrôle FP msvcrt stateful (sweep Win95)
- **Sweep Win95 (suite du StackBase).** Après le fix StackBase, re-run large des PE32 : `v.exe` et `mpegplayer.exe`
  butaient sur `_controlfp` non implémenté (import msvcrt commun).
- **Fix.** `aret_controlfp(new, mask)` (aret_crt.c, cdecl) : mot de contrôle FP msvcrt en encodage plateforme-indépendant,
  **défaut 0x0008001f** (mesuré Wine : toutes exceptions masquées, précision 53-bit, arrondi au plus proche). **Stateful** :
  set = `(cur & ~mask)|(new & mask)`, retourne le nouveau ; query (mask 0) retourne le courant. `_controlfp_s(&cur,new,mask)`
  = forme sécurisée (écrit via arg0, retourne 0). **Vérifié bit-identique Wine** : query `0008001f`, setRC `0008021f`,
  restore `0009001f`, setPC `000a001f`. (Limite : l'arrondi FP réel reste le défaut hôte ; un programme qui SET un arrondi
  non-défaut et s'y fie = même limite x87 bornée que fldcw custom — rare.)
- **Piège fixture.** mingw **inline** `_controlfp_s` avec sa propre validation stricte (retourne 22, ne set pas) ≠ le
  msvcrt de Wine (retourne 0, set) → `_controlfp_s` ne passe jamais par le shim en mingw → retiré du fixture (quirk
  toolchain, pas mon code). Le shim reste pour les vrais imports `_controlfp_s`.
- **Mesuré.** `v.exe` franchit `_controlfp` → tourne (endpoint). **Portes** : winediff **122→123/123** (`crt_controlfp`),
  hash transpile `19acad982194bf07` **inchangé** (runtime-only), régression unifiée **PASS**.

### 2026-07-18 — [STRATÉGIE][HLE-WIN32] Stratégie « zéro-abort 32-bit » (mesurée) + tête de liste : UnhandledExceptionFilter
- **Stratégie posée (doc 70 §5.0).** But : plus aucun abort sur le vrai logiciel compilé 32-bit (pas silencé — couvert),
  le résidu abort restant l'obfusqué/fait-main (§9). **Anti-années** : jamais de fix à l'intuition, toujours en tête
  d'une liste MESURÉE. Leviers : (0) `wallsweep` mesure+classe → (tête shim-main fort levier) → (2) mécanismes de classe
  (EH C++, bitmap-font, VB runtime) → (1) **lifting DLL** (ReactOS user32/gdi32/comctl32/VB40032 → code lifté prouvé,
  autonome — effondre la traîne + runtimes tiers d'un coup, doc 80 §1.2) → (3) mop-up. Re-mesurer après chaque vague.
- **Mesure (Levier 0, corpus Win95 37 PE32).** Instructions non-liftées = **bruit** (`outs`/`into`/`daa` = I/O port
  privilégié + data-en-code → abort correct ; lift complet). Tête imports : **UnhandledExceptionFilter 31/37**,
  **CreateProcessA 27**, **FormatMessageA 11**, puis traîne cohérente GDI mapping-mode / DDE / imprimante (2-3 chacun).
- **Fix (tête #1).** `SetUnhandledExceptionFilter` était un stub `return 0` → rendu **stateful** (retourne le filtre
  précédent, comme Wine). `UnhandledExceptionFilter(ExceptionInfo)` implémenté : lance le filtre top-level installé via
  `aret_call` (retourne sa disposition), sinon `EXCEPTION_EXECUTE_HANDLER(1)` (le CRT termine, pas de debugger). Gardé
  `winecorpus/win32_unhandledfilter.c` (chaîne stateful relative, env-indépendante — la 1re valeur diffère par
  conception : Wine a un filtre défaut, ARET part de 0). Wine **et** ARET → `p1_is_fa=1 p2_is_fb=1`.
- **Portes** : winediff **123→124/124**, hash transpile `19acad982194bf07` **inchangé** (runtime-only), régression
  unifiée **PASS**. Prochaines têtes mesurées : `CreateProcessA` (échec sound, 27 binaires — non bit-testable, Wine
  réussit), `FormatMessageA` (11, testable), puis la famille GDI mapping-mode.

### 2026-07-18 — [HLE-WIN32] `CreateProcessA/W` = échec sound (tête mesurée #2, 27/37)
- **Tête de liste mesurée #2.** `CreateProcessA` importé par 27/37 du corpus Win95, absent → abort. Lancer un `.exe`
  enfant Windows n'a **pas de modèle natif fidèle** (pas de Windows pour l'exécuter) → **échec sound** (doctrine 70
  §4.5/§8.3), jamais simulé, jamais abort : retourne `0` (FALSE) + `g_last_error=ERROR_FILE_NOT_FOUND(2)`. Un appelant
  teste le BOOL, voit l'échec, prend son chemin d'erreur → les 27 binaires **continuent** au lieu d'aborter.
- **Testabilité.** Wine tente réellement le lancement ; pour un chemin dont le répertoire n'existe pas, les deux
  échouent identiquement (FALSE). Fixture `winecorpus/win32_createprocess.c` (chemin `Z:\…\nope.exe`) n'imprime que le
  BOOL (code d'erreur + succès d'un vrai lancement = dépendants chemin/env, hors check bit-exact). Wine **et** ARET →
  `r=0`. `stdcall_pops` : +CreateProcessW@40.
- **Portes** : winediff **124→125/125**, hash transpile `19acad982194bf07` **inchangé** (runtime-only), régression
  unifiée **PASS**. Reste tête : `FormatMessageA` (11, table de messages système), puis famille GDI mapping-mode.

### 2026-07-18 — [HLE-WIN32] `FormatMessageA/W` (FROM_SYSTEM) — tête mesurée #3, table de messages extraite de Wine
- **Tête #3** (11/37). `FormatMessage(FROM_SYSTEM)` traduit un code d'erreur en texte. Strings **extraites verbatim de
  Wine** (l'oracle) → table `u32_sys_msg` (22 codes courants : 0/1/2/3/5/6/8/13/14/32/33/38/50/87/112/122/183/206/234/
  259/1223), chacune finissant `.\r\n`. `FORMAT_MESSAGE_ALLOCATE_BUFFER` → `LocalAlloc` (malloc, LocalFree=free) + écrit
  le pointeur via `buffer`. `FORMAT_MESSAGE_FROM_STRING`/`FROM_HMODULE`/inserts **et** un code hors table ⇒ **abort
  sound** (jamais une string vide/fausse ; la table grandit par la donnée). W = même table, élargie.
- **Testabilité.** Fixture `winecorpus/win32_formatmessage.c` : 12 codes (message + longueur + octets exacts incl. CRLF)
  + le chemin ALLOCATE_BUFFER. Wine **et** ARET **bit-identiques**.
- **Portes** : winediff **125→126/126**, hash transpile `19acad982194bf07` **inchangé** (runtime-only),
  `table_is_sorted_by_name` ok, régression unifiée **PASS**. **Tête mesurée épuisée** (UnhandledExceptionFilter 31 /
  CreateProcess 27 / FormatMessage 11 faits) → prochaine vague : **re-mesurer** (le levier change) puis la famille GDI
  mapping-mode (`SetViewportOrgEx`/`Scale*`/…) — candidate au **lifting DLL** (Levier 1).

### 2026-07-18 — [HLE-WIN32] `GetWindow`/`GetTopWindow` — nav hiérarchie fenêtres (plateau, plus fort levier après re-mesure)
- **Re-mesure post-tête** (Levier 0) : la tête (UnhandledExceptionFilter/CreateProcess/FormatMessage) a **disparu** de la
  liste — la distribution est devenue **plate** (plateau 3-5 binaires : familles fenêtre-nav / thread / imprimante / GDI
  mapping-mode / menu). C'est le domaine du **Levier 1 (lifting DLL)**. Passe shim-main sur le plus fort levier du
  plateau : `GetWindow`(4)+`GetTopWindow`(4).
- **Fix.** `GetWindow(hwnd, cmd)` navigue la hiérarchie via le registre de fenêtres : enfants/fratrie partagent un parent
  et sont ordonnés par **création** (= index `g_u32_win`), ce qui **matche le Z-order enfant par défaut de Wine**
  (**mesuré** : c1,c2,c3 sous a → GW_CHILD=c1, GW_HWNDNEXT(c1)=c2, GW_HWNDLAST=c3, GW_HWNDFIRST(c3)=c1). GW_OWNER : 0 si
  WS_CHILD, sinon le parent/owner stocké. `GetTopWindow(hwnd)` = premier enfant (GW_CHILD) ; NULL/desktop → premier
  top-level. `stdcall_pops` : +GetWindow@8, GetTopWindow@4.
- **Portes** : winediff **126→127/127** (`user32_getwindow`, `gwchild=1 next=2 last=3 first=1 topwin=1` = Wine), hash
  transpile `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok, régression unifiée **PASS**.

### 2026-07-18 — [LOADER] Levier 1 (lifting DLL) incrément 1 : parser l'**Export Directory** d'un PE DLL
- **Bascule stratégique actée.** La tête mesurée est épuisée (UnhandledExceptionFilter/CreateProcess/FormatMessage) et
  la re-mesure montre un **plateau plat** (fenêtre-nav/thread/imprimante/GDI mapping-mode/menu). Le plateau EST le domaine
  du **Levier 1 (lifting DLL, doc 80 §1.2)** — pas un shim de plus à la main. On **arrête la passe shim** et on démarre le
  Levier 1 **par petits incréments committables** (pas big-bang). Incrément 1 = brique isolée, testable sur une vraie DLL.
- **Ce que c'est.** Pour lifter une DLL (comctl32/user32/…) et exposer sa surface d'API, le loader multi-modules doit
  savoir **quelle fonction liftée** chaque export (nom/ordinal) désigne. Première brique : lire l'**Export Directory**.
  `parse_pe_exports(data) -> Vec<PeExport>` (`src/loader/mod.rs`, via le crate `object` `ExportTable::exports()`) :
  chaque export → `{ordinal (base incluse), name: Option, target}`. `target` = `Address(image_base+RVA)` (= l'adresse du
  `sub_<va>` lifté) pour une cible **locale**, ou `ForwardByName(dll,name)`/`ForwardByOrdinal(dll,ord)` pour un **forward**
  (enregistré verbatim, résolu plus tard par le loader — **jamais deviné**, sinon abort sound). Les trous de l'EAT
  (adresse 0, gaps d'une plage d'ordinaux creuse) sont **sautés** (RVA 0 = en-tête DOS, jamais un export valide).
- **Testabilité (double : forgé déterministe + VRAIES DLL système).** (1) Unit test `parse_pe_exports_reads_the_export_directory` :
  PE32 DLL **forgé octet par octet** (ordinal base 5 ; ord5 "Alpha"→local, ord6 sans nom→ordinal-only, ord7→trou EAT
  sauté, ord8 "Gamma"→forward "OTHER.Delta") — vérité terrain **que je contrôle exactement** → couvre base d'ordinaux,
  named local, ordinal-only, saut de trou, forward-by-name. + `parse_pe_exports_empty_on_non_pe`. (2) **Mesuré vs objdump
  sur les VRAIES DLL de Wine** (règle « mesurer, pas affirmer » ; ce sont les cibles réelles du Levier 1) : les PE32
  builtin de Wine (`/usr/lib/i386-linux-gnu/wine/i386-windows/`) parsés **bit-exact vs `i686-w64-mingw32-objdump -p`** —
  **comctl32 191 (160 addr + 31 fwd), user32 780, gdi32 496 (481 addr + 15 fwd), comdlg32 28** ; forwards résolus
  correctement (`comctl32→kernelbase.StrChrA`, `gdi32→win32u.NtGdiDd…`), `InitCommonControls` ressort **ord=17** (= la
  table `ordinal_imports`). Verrouillé en test committé `parse_pe_exports_matches_wine_comctl32` (gated sur la présence
  du builtin Wine, comme winediff ; assertions **robustes aux versions** = invariants ABI-stables, pas des compteurs
  figés). (Un vrai DLL mingw `Add/Sub/gCounter` a aussi servi de premier cross-check jetable.)
- **Portes (additif, hors chemin actif — `parse_pe_exports` pas encore appelée hors test).** Build OK (warning dead_code
  attendu, levé à l'incrément 2), unit tests bin **64/64**, hash transpile `19acad982194bf07` **inchangé**, régression
  unifiée **PASS** (difftest 272/272, funcdiff 20558 / 0 div, SMT 11/11, recompilabilité 100 %), winediff **127/127**.
- **Suite (incrément 2).** Loader multi-modules minimal : charger app + 1 DLL dans un espace, résoudre les imports de
  l'app vers les exports de la DLL (nom → `sub_<va>` lifté), le compilateur hôte les linke. Puis incrément 3 = lifter
  comctl32 (pur user-mode) sur le HLE user32/gdi32 existant.

### 2026-07-18 — [LOADER] Levier 1 incrément 2, brique 2.1 : imports gardant le **DLL source**
- **Le besoin.** Pour la résolution inter-modules (Incrément 2), quand `app.exe` importe `InitCommonControls`, il faut
  savoir qu'il vient **de comctl32** — pour lier le slot IAT vers l'**export lifté** de comctl32, pas vers un shim HLE.
  Or `Program::imports` (slot IAT → nom résolu) **perd le module** (le nom de DLL n'était utilisé que transitoirement
  pour résoudre les ordinaux). Brique isolée et testable : parser les imports **en gardant le DLL source**.
- **Fix.** `parse_pe_imports_detailed(data) -> BTreeMap<u64, PeImport{dll, name, ordinal}>` (`src/loader/mod.rs`),
  parallèle à `parse_pe_imports` : même parcours de l'import table (crate `object`), mais chaque slot IAT garde
  `{dll (nom du module source), name: Option, ordinal: Option}`. Stocké sur `Program::pe_imports`, câblé dans `load`.
  **Purement additif** : l'ancien `imports` (slot→nom de shim) est inchangé → le pipeline actuel ne bouge pas.
- **Testabilité (vraie DLL Wine).** `parse_pe_imports_detailed_keeps_source_dll` (gated sur le builtin Wine) : la vraie
  `comctl32.dll` importe **`gdi32.dll!BitBlt`** et **`advapi32.dll!RegCloseKey`** (vérifié vs `objdump -p`) → le parseur
  les retrouve avec le bon DLL source (assertions ABI-stables, case-insensitive, pas de compteurs figés). Chaque import
  porte un DLL non-vide + un nom ou un ordinal.
- **Portes** : build OK, bin unit tests **66/66**, hash transpile `19acad982194bf07` **inchangé**, régression unifiée
  **PASS**. **Suite** : brique 2.2 = le **résolveur** (imports app + tables d'exports des DLL chargées → slot IAT app →
  VA de l'export lifté), puis 2.3 = fusion d'espace d'adressage + alimentation du pipeline.

### 2026-07-18 — [LOADER] Levier 1 incrément 2, brique 2.2 : **résolveur inter-modules** (imports app → exports DLL liftés)
- **Le cœur de l'Incrément 2.** `resolve_module_imports(app_imports, modules) -> BTreeMap<slot_IAT, VA_export>` : pour
  chaque slot IAT importé par l'app, si l'import vient d'un DLL chargé (`LoadedModule{name, exports}`, brique 2.1 + Inc.1)
  et que ce DLL exporte le nom/ordinal demandé vers une adresse **locale**, lie le slot à la **VA de l'export** (= le
  `sub_<va>` lifté). Match DLL **insensible casse/`.dll`** (`norm_dll`), préférence au **nom** (stable) sinon l'ordinal.
  **Non résolu (→ shim HLE / abort sound, jamais deviné)** : DLL non chargé, symbole inconnu, export **forwardé** (pointe
  vers un autre module — chaîne à résoudre plus tard).
- **Testabilité (synthétique + cross-module RÉEL).** (1) `resolve_module_imports_binds_by_name_and_ordinal` : module
  synthétique (Alpha@0x2000/ord5, ord6@0x2100, Gamma→forward) × imports variés → lie par nom (casse/ext-insensible) et
  par ordinal, **laisse non résolu** forward / DLL non chargé / nom inconnu. (2) **`resolve_module_imports_cross_module_wine`**
  (gated Wine) : la vraie **comctl32** importe `gdi32.BitBlt` → résolue contre la table d'exports de la vraie **gdi32**,
  le slot est lié à la **VA d'export BitBlt réelle de gdi32** (`addr_by_name`), et **chaque** cible résolue est une
  adresse d'export gdi32 réelle (jamais inventée). La chaîne Inc.1→2.1→2.2 est ainsi prouvée bout-en-bout sur DLL système.
- **Portes** : build OK, bin unit tests **68/68**, hash transpile `19acad982194bf07` **inchangé** (fonction standalone,
  non câblée au pipeline). **Suite** : brique 2.3 = fusion d'espace d'adressage (app + DLL liftés dans un binaire) +
  alimentation du pipeline (le slot IAT résolu émet un appel vers le `sub_<va>` lifté au lieu du shim) — la 1ʳᵉ marche
  end-to-end du lifting DLL.

### 2026-07-18 — [LOADER] Levier 1 incrément 2, brique 2.3a : **rebaser multi-modules** (appliquer les base-relocs)
- **Le besoin.** user32/gdi32/comctl32 préfèrent **tous** la base `0x10000000` → charger ≥2 DLL dans un espace fusionné
  force à **rebaser** toutes sauf une. Rebaser = appliquer les **base relocations** (`.reloc`) : chaque site tient une
  adresse absolue 32-bit à décaler de `delta = new_base - old_base`. Le loader captait déjà les **sites** (`base_relocs`) ;
  il manquait de les **appliquer**. Sous-brique isolée, testable.
- **Fix.** `apply_base_relocations(sections, reloc_sites, delta) -> Result<usize>` (`src/loader/mod.rs`) : patche chaque
  site (u32 LE += delta) en place, rend le nombre de sites patchés. **Sound** : un site hors de toute section chargée =
  **erreur bruyante** (jamais sauté — un reloc non appliqué laisserait un pointeur absolu **périmé** = faux silencieux).
  `delta==0` = no-op (module à sa base préférée). 32-bit only (tout site = `HIGHLOW` 4 o, le seul type qu'un `-m32`
  émet ; un DIR64 64-bit demanderait le type par-site).
- **Testabilité (synthétique + vraie DLL).** (1) `apply_base_relocations_shifts_absolute_pointers` : section avec un
  pointeur absolu `0x10002000` + filler → rebase +0x10000000 → `0x20002000`, filler intact ; delta 0 = no-op ; site
  hors-section = **Err**. (2) `apply_base_relocations_covers_real_gdi32` (gated Wine) : rebase de la **vraie gdi32** →
  **tous** ses sites `.reloc` tombent dans une section chargée et sont patchés (`n == sites.len()`) — la primitive gère
  le layout `.reloc` réel d'une DLL système.
- **Portes** : build OK, bin unit tests **70/70**, hash transpile `19acad982194bf07` **inchangé** (fonction standalone).
  **Suite (2.3, reste)** : fusionner app + DLL rebasés dans un `Program` unique (sections + exports/symboles décalés du
  delta), seeder les exports liftés, router chaque slot IAT résolu (map 2.2) vers le `sub_<va>` lifté — la 1ʳᵉ marche
  end-to-end. Garde dure : mono-module **byte-identique** (hash `19acad982194bf07`).

### 2026-07-18 — [LOADER] Levier 1 incrément 2, brique 2.3b : **fusion multi-modules** (app + DLL rebasés dans un `Program`)
- **Le besoin.** Après le rebaser (2.3a), fondre plusieurs modules dans **un seul espace d'adressage** : placer chaque
  DLL à une base libre, rebaser, replier ses sections + symboles, et exposer ses **exports comme fonctions à récupérer**.
- **Fix.** `Program` gagne `image_base` (base préférée PE, via `relative_address_base`) et `exports` (sa propre Export
  Directory, vide pour un exe) — calculés au load, **additifs** (non consommés par le pipeline actuel). `merge_modules(
  primary, dlls: Vec<(nom, Program)>) -> Vec<LoadedModule>` : chaque DLL est placée à une base **64K-alignée au-dessus de
  tout ce qui est déjà mappé** (la préférence commune 0x10000000 de user32/gdi32/comctl32 ne collisionne donc jamais),
  rebasée via `apply_base_relocations`, ses sections/symboles décalés du delta et repliés dans `primary` ; ses **exports
  locaux nommés deviennent des symboles-fonction** à leur VA rebasée (points d'entrée de récupération). **Sound** : une
  section rebasée qui **chevaucherait** l'image existante = erreur bruyante (jamais silencieuse).
- **Testabilité (2 vraies DLL Wine).** `merge_modules_rebases_and_folds_exports` (gated) : fusionne la vraie **comctl32**
  (primary) + **gdi32** (toutes deux préférant 0x10000000) → gdi32 rebasée au-dessus, **sans chevauchement** ; son export
  **BitBlt** atterrit comme **symbole-fonction** à la VA rebasée, **dans** une section fusionnée ; aucune paire de sections
  ne se chevauche (vérifié sur tout l'espace fusionné).
- **Portes** : build OK, bin unit tests **71/71**, hash transpile `19acad982194bf07` **inchangé**, régression unifiée
  **PASS** (le champ ajouté au load est inerte pour tous les binaires réels). **Suite (2.3c, dernière)** : router chaque
  slot IAT résolu (map 2.2) → le `call [slot]` de l'app émet un appel vers le `sub_<va>` lifté du DLL au lieu du shim HLE
  — la 1ʳᵉ marche end-to-end du lifting DLL. Zone porteuse (analysis/emit) → garde dure hash + régression complète.

### 2026-07-18 — [LOADER] Levier 1 incrément 2, brique 2.3c : **`load_with_modules`** — assemblage + routage (capstone loader)
- **Le capstone côté loader.** `load_with_modules(primary_data, dlls: &[(nom, bytes)]) -> Program` compose toute la
  chaîne : charge l'app + chaque DLL, les fusionne dans un espace rebasé (`merge_modules`), résout les imports de l'app
  contre les exports des DLL (`resolve_module_imports`), et **route** chaque slot IAT résolu → écrit la **VA d'export**
  dans les octets du slot + **retire le slot de `imports`**. Résultat : l'émission dispatchera le `call [slot]` de l'app
  vers le `sub_<export_va>` lifté (fonction interne récupérée) au lieu d'un shim HLE. Imports **non** satisfaits par un
  DLL chargé → restent shim. **Sound** : slot routé hors de toute section = erreur bruyante (jamais un pointeur IAT périmé).
- **Testabilité (2 vraies DLL Wine, end-to-end loader).** `load_with_modules_routes_resolved_imports` (gated) : comctl32
  comme « app » + gdi32 → l'import `gdi32.BitBlt` est **routé** (retiré de `imports`, slot patché vers une VA qui est un
  **symbole-fonction récupéré** `BitBlt`), tandis que `advapi32.RegCloseKey` (DLL non chargée) **reste shim**. Toute la
  chaîne Inc.1→2.1→2.2→2.3a→2.3b→2.3c prouvée sur DLL système.
- **Portes** : build OK, bin unit tests **72/72**, hash transpile `19acad982194bf07` **inchangé** (fonction standalone,
  non câblée à la CLI par défaut → aucun binaire existant ne change).
- **⇒ Toute la couche LOADER du lifting DLL multi-modules est FAITE et prouvée sur vraies DLL Wine.** **Reste (la vraie
  1ʳᵉ marche end-to-end, chantier ciblé à part)** : (1) flag CLI (`--with-dll nom=chemin`) appelant `load_with_modules` ;
  (2) **vérifier que l'emit dispatche réellement** le `call [slot]` patché vers le `sub_<va>` lifté (dispatch indirect
  content-based dans le modèle shared-stack) via un **run end-to-end vs Wine** (une app minimale important d'un DLL
  minimal, les deux liftés). C'est la zone emit porteuse → garde dure hash + régression + fixture runnable.

### 2026-07-18 — [LOADER][LIFT][DEMO] Levier 1 : **1ʳᵉ marche end-to-end du lifting DLL** — une app appelle du code DLL lifté, bit-identique à Wine
- **Le jalon.** Une app Windows qui **importe d'un DLL** est liftée **avec ce DLL** → les appels d'import dispatchent
  vers le **code DLL lifté** (pas un shim HLE), sortie **bit-identique à Wine**. C'est la thèse du Levier 1 prouvée en
  exécution, pas juste en structure.
- **Le câblage (minimal, zone emit non touchée).** Flag CLI **`--with-dll nom=chemin`** (`src/main.rs`, répétable) →
  bascule sur `load_with_modules` (briques Inc.1→2.3c). **Rien d'autre à changer dans l'emit** : le mécanisme de dispatch
  indirect existant suffit. `load_with_modules` écrit la **VA d'export** dans le slot IAT (au lieu de la laisser à un
  shim) et le retire de `imports` ; à l'exécution `call [slot]` lit cette VA et `aret_call` la trouve dans la table de
  dispatch (l'export lifté est une fonction interne récupérée `sub_<va>`) → appelle le vrai code. `__aret_patch_iat`
  n'écrase pas le slot (il n'itère que `imports`, d'où le slot est retiré). L'ABI colle (l'export lifté suit l'ABI
  `sub_` interne, esp threadé). **Défaut inchangé** : sans `--with-dll`, `Program::load` — zéro impact.
- **Preuve mesurée (fixture + contrôle).** `bench/winecorpus/dll_lifting.{c,dll.c}` : app importe `lift_add`/`lift_mul`/
  `lift_poly` (boucle+branche) d'un DLL compagnon. **Avec `--with-dll`** → `add=42 mul=42 poly=55`, **SOUND**, =Wine.
  **Contrôle sans `--with-dll`** → `dll_add`/`dll_mul` = imports non implémentés (stubs → `0`), INCOMPLETE : la sortie
  `42` **ne peut venir que** du code DLL lifté (aucun shim `aret_lift_*` n'existe). Harness winediff étendu : convention
  `NAME.dll.c` (DLL compagnon construit + lié + lifté via `--with-dll` ; Wine le charge normalement depuis $TMP ;
  `*.dll.c` exclus du loop standalone). **winediff 127→128/128** (`dll_lifting`, bit-identique Wine).
- **Portes** : build OK, bin unit tests **72/72**, hash transpile `19acad982194bf07` **inchangé** (flag guardé), régression
  unifiée **PASS**, winediff **128/128**. **⇒ Le Levier 1 est prouvé end-to-end sur un DLL réel.** Reste pour le vrai
  monde : lifter comctl32 (pur user-mode) sur le HLE user32/gdi32 (incrément 3) — replier aussi les **imports du DLL**
  (mydll→msvcrt) dans `merge_modules` (aujourd'hui la fixture est self-contained ; comctl32 importe gdi32/user32) ;
  puis routage `win32k` pour user32/gdi32.

### 2026-07-18 — [LOADER] Levier 1 : `merge_modules` replie aussi les **imports du DLL** (vers les vraies DLL)
- **Le manque (vers comctl32).** La fixture `dll_lifting` est **self-contained** (le DLL n'importe rien), mais une
  **vraie** DLL importe d'autres modules (comctl32 → gdi32/user32/kernel32/advapi32, gdi32 → ntdll/win32u/ucrtbase…).
  `merge_modules` repliait sections + symboles + exports, mais **pas les imports du DLL** → après fusion, les appels du
  DLL vers ses propres imports étaient **perdus** (ni shim, ni routés).
- **Fix.** `merge_modules` replie maintenant `dll.imports` **et** `dll.pe_imports` (clés **décalées** du delta de rebase)
  dans le primary (sans clobber). ⇒ un import du DLL reste **shim-bound**, ou est **routé** aussi si ce module cible est
  lui-même chargé (`resolve_module_imports` voit les `pe_imports` repliés → chaînes inter-DLL supportées).
- **Testabilité.** `merge_modules_rebases_and_folds_exports` étendu : fusionner gdi32 dans comctl32 → `primary.imports`
  gagne **exactement** le nombre d'imports de gdi32 (`imports_before + gdi_imports`, aucune collision de VA). Fixture
  `dll_lifting` **toujours verte** (`add=42 mul=42 poly=55`, SOUND — self-contained, repli = no-op).
- **Portes** : bin unit tests **72/72**, hash transpile `19acad982194bf07` **inchangé**, fixture end-to-end OK. Prochaine
  étape réelle : tenter de lifter **comctl32** (pur user-mode) sur le HLE user32/gdi32 — mesurer ce qui lift / abort sound.

### 2026-07-18 — [LOADER][STRATÉGIE] Levier 1 : **carte mesurée** du lifting de comctl32/gdi32/user32 → le mur = win32k (NtGdi*/NtUser*)
- **Levier 0 appliqué à la frontière DLL** (`--mode walls` + `--with-dll`, sur les vraies DLL Wine). But : borner par la
  donnée ce qui sépare ARET de **faire tourner de vraies DLL GUI**.
- **comctl32 seule.** Récupère **2577 fonctions (2489 liftées, 96 %+)** — le lift structurel est quasi complet. Gaps
  d'instructions = **bruit** (ud2/`in`/`push es` = data-en-code/privilégié → abort correct). Vrai mur = **144 imports**
  non implémentés (gdi32/user32/kernel32 dans lesquels comctl32 appelle) — c.-à-d. le **socle HLE**, pas comctl32.
- **comctl32 + gdi32 + user32 liftées ensemble** (`--with-dll gdi32 --with-dll user32`) : **7839 fonctions récupérées,
  7150 liftées**. Le tail d'API user-mode nommé **s'effondre** (routé vers le code lifté) → il reste **356 imports**, qui
  se scindent **NETTEMENT en deux** :
  - **250 syscalls `Nt*`** = **le mur win32k** : **117 `NtGdi*`** (BitBlt/AlphaBlend/BeginPath/CombineRgn…) + **131
    `NtUser*`** (BeginPaint/CallHwnd*/CheckMenuItem/ClipCursor…) + 2 ntdll. C'est la **frontière noyau du NT moderne** :
    gdi32/user32 user-mode liftent, mais leur fond appelle les stubs syscall `win32u`. **Exactement la prédiction doc 80
    §1.2** (« le fond win32k à router vers le HLE, FLIRT chirurgical »).
  - **106 non-`Nt*`** = kernel32/CRT **ordinaire** (Atoms `GlobalAddAtom*`, locale `GetLocaleInfoW`/`GetDateFormatW`,
    version-info, IME `Imm*`, classification de char `IsCharAlpha*`) — de simples shims HLE, même famille que d'habitude.
- **La carte stratégique (borne le reste du Levier 1 vers les DLL réelles).** Le lifting du **code user-mode marche**
  (mécanisme prouvé end-to-end + 7150 fns liftées). Faire **tourner** comctl32 réelle demande deux chantiers **bornés et
  mesurés** : (1) **router les ~250 `NtGdi*`/`NtUser*` vers le HLE existant** (qui rend déjà des DIB, BitBlt, modèle
  fenêtre/paint bit-identiques à Wine — donc `NtGdiBitBlt`→notre blit DIB, `NtUserBeginPaint`→notre paint, etc. ; « FLIRT
  chirurgical » de la frontière win32k) ; (2) les **106 shims kernel32/CRT ordinaires** (data-driven, faciles). Aucun
  n'est de la recherche : c'est de la **couverture bornée mesurée**. Reste aussi ~17 unresolved-direct + `jl 0x100afcd4`
  ×16 (gaps de récup mineurs, à regarder). *Note : les DLL Win9x monolithiques faisaient le dessin en user-mode (pas de
  split `NtGdi*`) — les éviterait, mais proprio ; ReactOS/Wine miment le split NT.*
- **Portes** : mesure seule (aucun code changé). Prochaine brique concrète : router une 1ʳᵉ famille `NtGdi*`/`NtUser*`
  vers le HLE (ex. `NtGdiBitBlt`→`aret` blit) et **mesurer** qu'une vraie fonction comctl32 traverse — piloté par un vrai
  chemin, une famille à la fois, garde winediff.

### 2026-07-18 — [LOADER][STRATÉGIE] Levier 1 : la bonne config = **lifter comctl32 SEULE** (pas gdi32/user32) → zéro win32k
- **Correction de trajectoire (mesurée).** Lifter gdi32/user32 était une **fausse piste** : notre HLE gdi32/user32
  **marche déjà** (winediff le prouve) ; les lifter les *remplace* par du code lifté qui rebute alors sur le plancher
  win32k (`NtGdi*`/`NtUser*`). La **bonne** config Levier 1 : lifter **comctl32 seule** (le tail qu'on n'a pas), ses
  imports gdi32/user32/kernel32 se liant aux **shims HLE existants** → **aucun win32k**.
- **Mesuré (fixture ImageList vs Wine).** App `ImageList_Create/Add/GetImageCount/Destroy` + `CreateBitmap`, lift
  `--with-dll comctl32.dll` seul. Wine : `il=1 / before=0 idx=0 after=1`. ARET : comctl32 lift, mais le chemin ImageList
  bute sur de **vrais gaps HLE ordinaires** — `CreateDIBSection` (on ne modélise que **32bpp BI_RGB**, ImageList veut
  24bpp) et `CreatePatternBrush` (manquant). Pas un mur win32k : de la **couverture HLE bornée**.
- **⇒ Le reste du Levier 1 (faire tourner de vraies comctl32) = couverture HLE data-driven** : les 144 shims
  gdi32/user32/kernel32 manquants + étendre l'existant (CreateDIBSection multi-bpp), **une famille à la fois vs Wine**
  (même méthode que toute la couche HLE). Borné, mesuré, pas de recherche, pas de win32k. À piloter par un vrai chemin
  (ImageList, puis toolbar/listview…) quand on poursuit l'incrément 3.

### 2026-07-18 — [HLE-WIN32] Famille **Atom** complète (kernel32/user32) — batch mesuré du socle Levier 1
- **Batch du socle** (les 144 imports mesurés de comctl32 sont surtout du socle borné → on les couvre par **familles
  complètes vérifiées vs Wine**, pas une par une). 1ʳᵉ famille : les **atoms** (interning chaîne↔ATOM), sous-système
  autonome, très courant (pas que comctl32), zéro dépendance écran/env → **bit-testable**.
- **14 shims** (`aret_win32.c`) : `{Global,}AddAtom{A,W}`, `{Global,}FindAtom{A,W}`, `{Global,}DeleteAtom`,
  `{Global,}GetAtomName{A,W}`. Deux tables **séparées** (locale vs globale — une locale est invisible à la globale,
  mesuré), string atoms **casse-insensibles** gardant la 1ʳᵉ casse, numérotés depuis 0xC000, **refcountés** (Add++ /
  Delete--, libéré à 0). Atomes entiers (`MAKEINTATOM`, ptr < 0x10000) : valeur passe-through, nom `#N`. `stdcall_pops` :
  +14 `@N`.
- **Tout MESURÉ vs Wine (aucune supposition), y compris les quirks** : `DeleteAtom` retourne **0 en succès** (!),
  find-miss → `ERROR_FILE_NOT_FOUND(2)`, bad atom → `ERROR_INVALID_HANDLE(6)`, buffer trop petit → `ERROR_MORE_DATA(234)`
  **et** retourne le **count copié pour la table LOCALE mais 0 pour la GLOBALE** (quirk réel local≠global). Piège de
  testabilité : la **base absolue de la table globale dépend de l'env** (Wine pré-seed ~3 atoms → base 0xC003 ≠ 0xC000) —
  l'ATOM est un **handle opaque** par spec → la fixture teste l'**absolu sur la locale** (0xC000, déterministe) et le
  **relatif** sur la globale (round-trip, séparation, casse).
- **Portes** : winediff **128→129/129** (`win32_atom`, bit-identique Wine — 9 lignes, tous les codes d'erreur + quirks),
  hash transpile `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok, régression unifiée **PASS**. Prochaines
  familles socle (même méthode) : char-classification (`IsCharAlpha*`…), locale-getters, version-info.

### 2026-07-18 — [LOADER][HLE-WIN32][DEMO] Levier 1 : **1ère vraie fonctionnalité comctl32 (ImageList) tourne bit-identique à Wine**
- **Le jalon Phase B.** Une vraie API comctl32 (**ImageList**), **liftée depuis la vraie comctl32.dll de Wine**
  (`--with-dll`), tourne sur le HLE gdi32 d'ARET, **bit-identique à Wine, exit 0**. Pas un DLL jouet : du **vrai code
  comctl32** (2577 fonctions liftées) qui appelle notre gdi32 HLE. Piloté par un vrai chemin (méthode : lifter → buter →
  combler le mur mesuré → relancer).
- **Murs comblés (mesurés vs Wine, primitives gdi32 pures)** : `GetDIBits` (query = décrit le bitmap, ret=height, comp=3
  BITFIELDS ; copy 32bpp = copie scanline honorant l'orientation biHeight vs source top-down/bottom-up — **mesuré** :
  bottom-up→copie directe), `CreatePatternBrush` (tient le bitmap de motif), `StretchDIBits` (blit DIB 32bpp SRCCOPY 1:1
  whole-image → surface DC ; stretch/sub-rect/autre ROP/bpp = **abort sound**). `stdcall_pops` : +GetDIBits@28,
  CreatePatternBrush@4, StretchDIBits@52. NB : la **logique** ImageList (count/index/iconsize) vient du **code comctl32
  lifté** — on ne fournit que les primitives gdi32, comctl32 fait le reste (c'est tout l'intérêt du lifting).
- **Fixture** `winecorpus/comctl32_imagelist.{c,withdll}` : convention **`.withdll`** ajoutée au harness (une ligne =
  un DLL système à lifter depuis les PE builtin de Wine ; gated sur leur présence). Sortie ARET =
  `create=1/count0=0/add_idx=0 count1=1/iconsize=16x16/remove=1 count2=1` = **Wine**, exit 0.
- **Portes** : winediff **129→130/130** (`comctl32_imagelist`), hash transpile `19acad982194bf07` **inchangé**,
  `table_is_sorted_by_name` ok, régression unifiée **PASS**. **Suite Phase B** : un vrai widget (Progress/Toolbar) — même
  boucle, comblera les familles socle suivantes (char-class, régions GDI, DIB multi-bpp) mesurées vs Wine.

### 2026-07-18 — [LOADER][STRATÉGIE] Levier 1 : la profondeur suivante = **enregistrement de classe des contrôles comctl32** (mesuré, borné)
- **Phase B, prochain palier tenté : un vrai widget (Progress bar).** Bien plus profond qu'ImageList : les messages `PBM_*`
  doivent dispatcher vers le **WNDPROC comctl32 lifté** via notre modèle fenêtre/message. Cible Wine (sous Xvfb) :
  `parent=1 pb=1 pos=50 lo=0 hi=100`.
- **Mur mesuré : la classe de contrôle n'est pas enregistrée.** ARET : `parent=1` (notre fenêtre HLE marche) mais **`pb=0`**
  — `CreateWindowExA("msctls_progress32")` retourne 0 **proprement** (aucun abort runtime), car la classe n'est pas dans
  le registre. **Diagnostic isolé** : `InitCommonControlsEx` **EST routé** vers le comctl32 lifté (slot IAT `0x40e254`
  retiré des imports, le stub HLE `aret_InitCommonControlsEx` n'est **jamais** appelé), il **tourne sans abort**, mais son
  chemin d'enregistrement (RegisterClassW routé vers le HLE) n'aboutit pas à une classe **retrouvable** par
  `CreateWindowExA`. (Ce n'est donc **pas** le stub, ni un DllMain non appelé — c'est le chemin de registration lifté qui
  diverge.)
- **La donnée stratégique.** ImageList (API **stateless** : count/index/iconsize calculés par le code lifté, sur nos
  primitives gdi32) **marche** (bit-identique Wine). Les **contrôles stateful** (fenêtre + WNDPROC lifté) exigent en plus
  que la **machinerie d'enregistrement de classe** de comctl32 fonctionne bout-en-bout à travers le HLE
  (`RegisterClassW`, possiblement noms de classe **par atome** — qu'on vient d'implémenter — et/ou un early-return dans une
  fonction partial-asm). **Prochaine brique = déboguer ce chemin** (recompiler `-O0 -g`, gdb le comctl32 lifté depuis
  `InitCommonControlsEx` → voir où la registration diverge du comportement Wine). Borné, mesuré, une session dédiée.
- **Portes** : exploration mesurée, **aucun code changé**, aucune fixture ajoutée (la progress bar n'est pas verte —
  honnête). Le jalon ImageList (contrôle stateless réel bit-identique Wine, winediff 130/130) reste la preuve acquise.

### 2026-07-18 — [LOADER][BUILD] Levier 1 : **DllMain des DLL liftés exécuté au démarrage** — comctl32 enregistre ses classes de contrôle
- **Le mur (débogué au tour précédent).** Un contrôle comctl32 stateful (progress bar) : `CreateWindowEx("msctls_progress32")`
  retournait 0 car la classe n'était pas enregistrée. Isolé : dans ce comctl32, les classes sont enregistrées dans le
  **DllMain (DLL_PROCESS_ATTACH)**, qu'on **n'appelait pas** en liftant. C'est un gap **général** : tout DLL stateful
  initialise dans DllMain (classes, globals, TLS).
- **Fix (brique générale, 3 couches).** (1) `Program::dll_inits: Vec<(entry_va, hinstance)>` — les initialiseurs des DLL
  fusionnés. (2) `merge_modules` calcule l'**entrée rebasée** de chaque DLL (`_DllMainCRTStartup`), la **seed comme
  fonction** (récupérée), et remonte `(entry, base)` ; `load_with_modules` les pose dans `prog.dll_inits`. (3) le builder
  émet, dans `aret_main.c` **avant l'entrée de l'app**, un appel `DllMain(hinstDLL=base, DLL_PROCESS_ATTACH=1, 0)` par DLL
  (frame stdcall sur la pile machine partagée, retourne avant le frame de l'app). **Défaut inchangé** : sans DLL fusionné,
  `dll_inits` vide → `aret_main.c` byte-identique (hash couvre les fonctions liftées de toute façon).
- **Mesuré : ça marche exactement comme Wine.** Le DllMain lifté de comctl32 tourne au démarrage et enregistre **toutes**
  ses classes de contrôle via `RegisterClassW` routé vers le HLE (`msctls_progress32` wndproc lifté `4ad5f0`, trackbar,
  toolbar, statusbar, listview, tab, …). `CreateWindowEx(PROGRESS_CLASS)` → **`pb=1`** (avant : 0). +shim
  `DisableThreadLibraryCalls` (no-op sound, appelé par le DllMain).
- **Fixture** `winecorpus/comctl32_class_reg.{c,withdll}` (display-free) : **`preinit_progress=1`** (la classe existe
  **avant** `InitCommonControlsEx` = le DllMain a tourné au chargement, comme Wine) + progress/trackbar/toolbar/status=1,
  bogus=0 — **bit-identique à Wine** (l'atome GetClassInfo étant opaque, on compare enregistré-ou-non).
- **Portes** : winediff **130→131/131**, hash transpile `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok,
  régression unifiée **PASS** ; `comctl32_imagelist`/`dll_lifting` **toujours verts** (ils liftent avec DllMain actif).
  **Reste (couche suivante)** : le **round-trip des messages** du contrôle (`PBM_SETPOS`/`GETPOS` → `pos=50`) — la fenêtre
  est créée mais l'état du contrôle (extra-bytes `cbWndExtra` / struct alloué en WM_(NC)CREATE) n'est pas encore backé.

### 2026-07-18 — [HLE-WIN32] Création de fenêtre : **WM_NCCREATE/WM_CREATE dispatchés + extra-bytes `cbWndExtra`** (état des contrôles)
- **La couche suivante du contrôle stateful.** Après le DllMain (classes enregistrées), la progress bar créait sa fenêtre
  (`pb=1`) mais son état ne round-trippait pas (`PBM_SETPOS`/`GETPOS`) : un contrôle **alloue son état en `WM_(NC)CREATE`**
  et le range dans les **extra-bytes `cbWndExtra`** via `SetWindowLong(hwnd, 0, infoPtr)`. Notre modèle ne faisait ni
  l'un ni l'autre.
- **Fix (général, correct Windows).** (1) `CreateWindowEx(A/W)` envoie désormais **WM_NCCREATE puis WM_CREATE** au WNDPROC
  (via `u32_create_dispatch`, avec une `CREATESTRUCTA` malloc'd guest-accessible) ; WM_NCCREATE→FALSE ou WM_CREATE→-1
  **échoue la création** (destroy + retour 0), comme Windows. (2) Chaque fenêtre a `cbWndExtra` octets (`extra[64]`, len
  du champ classe) ; `Set`/`GetWindowLong` à offset **≥0** lisent/écrivent 4 octets dedans (l'idiome de stockage d'état
  de contrôle). (3) `DefWindowProc(WM_NCCREATE)`→**TRUE**, `(WM_CREATE)`→0 (sinon le dispatch échouait sur les fenêtres
  à DefWindowProc).
- **Vérifié bit-identique Wine (headless, message-only).** `winecorpus/user32_wmcreate` : WNDPROC met des flags en
  WM_NCCREATE/WM_CREATE + stocke via `SetWindowLong(0/4)` → `ncc=1 created=1 win=1 e0=1234 e4=5678` = Wine. **Zéro
  régression** : les fixtures GUI existantes reçoivent maintenant WM_CREATE (comme Wine) et restent toutes vertes.
- **Portes** : winediff **131→132/132** (`user32_wmcreate`), hash transpile `19acad982194bf07` **inchangé**, régression
  unifiée **PASS**. **Reste (mur suivant isolé) pour la progress bar** : son WM_CREATE lifté bute sur
  **`ResolveDelayLoadedAPI`** → comctl32 utilise des **imports delay-loaded** (résolution paresseuse ; le stub appelle
  `ResolveDelayLoadedAPI`, non implémenté → pointeur 0 → appel indirect vers 0). Prochaine brique : supporter le
  delay-load (table `IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT` + `ResolveDelayLoadedAPI`).

### 2026-07-18 — [HLE-WIN32][STRATÉGIE] Progress bar : mur isolé = **delay-load uxtheme** (comctl32 theming), frontière documentée
- **La couche suivante mesurée.** Après DllMain + WM_CREATE + extra-bytes, le WM_CREATE lifté de la progress bar appelle
  **`ResolveDelayLoadedAPI`** → parse du descripteur : c'est **`uxtheme.dll.OpenThemeData`**. Le comctl32 de Wine
  **delay-load uxtheme** (styles visuels) via un delay-load **manuel** (la delay-import directory PE standard est **vide**,
  objdump — c'est un descripteur en .data résolu à runtime).
- **Pourquoi c'est un vrai chantier (pas un quick win).** Résoudre proprement un import **à runtime** se heurte au modèle
  de dispatch **statique** d'ARET (`aret_call` = table VA→fn figée à l'émission) : il faudrait (1) un **résolveur runtime**
  (nom → VA synthétique + fallback dans `aret_call`), **et** (2) le **pop stdcall** de l'appel indirect delay-résolu
  (fallback dans `__aret_callee_pop`, sinon l'esp du comctl32 lifté dérive de N par appel). Bounded mais réel (3 points de
  contact dans le code généré + une table de shims theme).
- **Décision d'intégrité (sound).** `aret_ResolveDelayLoadedAPI` **abort dur nommé** — `aret_unmodelled("delay-load
  uxtheme.dll.OpenThemeData ...")` — au lieu du crash générique « indirect call to 0 » (le slot non résolu). La frontière
  est **bruyante et nommée**, jamais un faux silencieux. `stdcall_pops` : +ResolveDelayLoadedAPI@24.
- **La donnée stratégique.** Les contrôles comctl32 qui **thément** (progress bar, trackbar, …) butent sur ce delay-load
  uxtheme ; ceux dont on n'exerce que la **logique/état sans theming** (ImageList) tournent. Le brick **résolveur runtime
  delay-load** (+ shims uxtheme « pas de thème » → fallback classic) est la prochaine étape ciblée, avec les 3 exigences
  ci-dessus désormais **précisément identifiées**. Portes : winediff **132/132** (aucune régression — aucune fixture
  existante ne delay-load), hash transpile `19acad982194bf07` **inchangé**.

### 2026-07-18 — [LOADER][HLE-WIN32][DEMO] Levier 1 : **une vraie progress bar comctl32 fonctionne bit-identique à Wine** (résolveur delay-load)
- **Le jalon : un contrôle comctl32 STATEFUL complet.** `pb=1 / pos=50 lo=0 hi=100 / pos2=85` (avec `PBM_DELTAPOS`) =
  **Wine, exactement**. Toute la pile bouclée : DllMain enregistre la classe → `CreateWindowEx` crée la fenêtre
  (WM_NCCREATE/WM_CREATE allouent l'état dans `cbWndExtra`) → les messages `PBM_*` **dispatchent vers le WNDPROC comctl32
  lifté** → son état range/pos round-trippe. Le contrôle **exécute son vrai code lifté**, on ne fournit que la plateforme.
- **La brique qui débloque : résolveur delay-load runtime** (les 3 exigences cadrées, réalisées). (1) Table de shims
  connus `k_delay` (uxtheme « pas de thème » : `OpenThemeData`→NULL, `IsThemeActive`/`IsAppThemed`→FALSE, …) → un contrôle
  retombe en **rendu classic**, sa logique intacte. (2) `ResolveDelayLoadedAPI` parse l'`IMAGE_DELAYLOAD_DESCRIPTOR`,
  assigne une **VA synthétique** (`0x7EDA….`) à l'API résolue, patche le slot delay-IAT, la retourne. (3) le dispatch
  **statique** défère : `aret_call` (miss) → `aret_delay_dispatch` (appelle le shim), `__aret_callee_pop` → `aret_delay_pop`
  (le **pop stdcall** de l'appel indirect delay-résolu, sinon l'esp du comctl32 lifté dérivait). API non modélisée →
  **abort dur nommé** (jamais un 0 deviné). `stdcall_pops` : +ResolveDelayLoadedAPI@24.
- **Inerte hors delay-load.** Sans DLL lifté / delay-load, `g_delay_res` est vide → `aret_delay_dispatch`/`aret_delay_pop`
  rendent 0 → `aret_call`/`__aret_callee_pop` **identiques** à avant. Hash couvre les fonctions liftées de toute façon.
- **Fixture** `winecorpus/comctl32_progress.{c,withdll}` (display-free, message-only) : range/pos + DELTAPOS,
  **bit-identique Wine**. **Portes** : winediff **132→133/133**, hash transpile `19acad982194bf07` **inchangé**,
  `table_is_sorted_by_name` ok, régression unifiée **PASS** (le changement `__aret_callee_pop` touche tous les appels
  indirects stdcall — vérifié : `aret_delay_pop`=0 hors delay-load).
- **⇒ Le Levier 1 est prouvé sur un contrôle comctl32 stateful réel end-to-end.** Chemins couverts : ImageList (API
  stateless) **et** progress bar (contrôle stateful, messages, état, theming). Les autres contrôles (trackbar, toolbar,
  listview…) suivent la **même** machinerie — au fil des workloads réels, data-driven.

### 2026-07-18 — [STRATÉGIE] Re-mesure Levier 0 post-lifting-DLL (corpus Win95, 37 PE32) → tête = **GDI mapping-mode**
- **Étape 0 du plan ordonné** (§5.0) : re-lancer `--mode walls` sur les 37 vrais PE32 i386 du corpus Win95, agréger les
  causes d'abort par **#binaires** (largeur). Ré-ancre sur la donnée après le travail Levier 1 (comctl32).
- **Instructions non-liftées = BRUIT confirmé** (`int3`/`arpl`/`les`/`aaa`/`aas`/`in`/`insb`/`bound`/`into`/`popad` = data
  décodée-en-code + privilégié/BCD obsolète → abort correct). **Le lift par-instruction est complet.**
- **Imports manquants — distribution PLATE** (plateau, la tête ayant été traitée : UnhandledExceptionFilter/CreateProcess/
  FormatMessage). Tête : `TerminateThread` (5 binaires), puis traîne à 3. **MAIS une famille COHÉRENTE domine nettement** :
  le **GDI mapping-mode** (transformation coordonnées logique→device) — `SetViewportOrgEx`/`SetViewportExtEx`/
  `SetWindowExtEx`/`ScaleViewportExtEx`/`ScaleWindowExtEx`/`OffsetViewportOrgEx`/`DPtoLP`/`LPtoDP`/`GetViewportOrgEx`
  (+`PtVisible`/`RectVisible`/`Escape` = clipping/escape voisins), **~12 fonctions, 3 binaires chacune**. Autres familles :
  imprimante (`OpenPrinterA`/`ClosePrinter`/`DocumentPropertiesA`), menu (`ModifyMenuA`/`SetMenuItemBitmaps`), thread
  (`TerminateThread`/`SetThreadPriority`/`OpenProcess`), divers (`WinHelpA`/`WaitForInputIdle`/`TabbedTextOutA`/`GrayStringA`).
- **Conclusion (la donnée redirige le plan).** La **prochaine cible mesurée = la famille GDI mapping-mode** (la plus large
  ET la plus cohérente), **pas** les familles kernel32/CRT génériques supposées à l'étape 1. ⚠️ **Zone correctness-critique**
  (le mode de mapping transforme les coordonnées de **tout** le dessin ultérieur → chaque primitive GDI doit appliquer la
  transforme viewport/window) → chantier ciblé, vérifié **DIB-hash vs Wine**, pas un batch de shims triviaux. C'est le
  vrai « step 1/2 » de la vague, dicté par la mesure.

### 2026-07-18 — [HLE-WIN32][GUI] Famille **GDI mapping-mode** (transforme logique→device) — tête mesurée du plateau Win95
- **La tête mesurée** (re-mesure Levier 0) : la famille GDI mapping-mode (~12 fn, 3 binaires chacune). Implémentée à la
  main, **recette lue/mesurée sur Wine** (doctrine « Wine = livre de recettes »).
- **Recette (mesurée bit-exact vs Wine).** `device = (logical − winOrg) × vpExt / winExt + vpOrg`, arrondi **MulDiv**
  (au plus proche, égalités **loin de zéro** — mesuré `/3` : 1→0, 2→1, 5→2, −8→−3). Inverse pour DPtoLP.
- **Livré.** État par-DC (`vp_ox/oy`, `win_ox/oy`, `vp_ex/ey`, `win_ex/ey`, défaut **identité**) + `gdi_muldiv`/`dc_l2d`/
  `dc_d2l`. Shims : `SetMapMode` (MM_TEXT + MM_ANISOTROPIC ; métrique/isotropique → abort sound), `Set/Get/Offset/Scale`
  `{Viewport,Window}{Org,Ext}Ex`, `DPtoLP`/`LPtoDP`. Transforme appliquée dans les primitives **vectorielles**
  (SetPixel/V/GetPixel, LineTo, Polyline, Rectangle) → dessin **pixel-identique Wine** sous mapping. `stdcall_pops` : +14 `@N`.
- **Soundness (clé).** Défaut = **identité** → dessin non-transformé **inchangé** (zéro régression). Les primitives non
  encore transformées (FillRect/PatBlt/BitBlt/TextOut/FrameRect/InvertRect/PolylineTo/StretchDIBits/DrawText) sont
  **gardées** (`GDI_MAP_GUARD`) : sous mapping non-identité → **abort sound** (jamais un dessin faux silencieux) ;
  transformées data-driven quand un binaire réel les y utilise.
- **Fixture** `winecorpus/gdi_mapmode` : round-trip LPtoDP/DPtoLP + ScaleViewportExtEx + dessin (LineTo/Rectangle/SetPixel)
  sous MM_ANISOTROPIC → **coordonnées ET DIB-hash bit-identiques Wine** (`lptodp=(17,7)`, `dibhash=a76efa65`).
- **Portes** : winediff **133→134/134**, hash transpile `19acad982194bf07` **inchangé** (identité par défaut),
  `table_is_sorted_by_name` ok, régression unifiée **PASS**. ⚠️ Piège corrigé : le macro `GDI_MAP_GUARD` doit être défini
  **avant** la 1re primitive gardée (StretchDIBits) — sinon lien échoue (macro vu comme fonction).

### 2026-07-19 — [HLE-WIN32][GUI] Famille **menu (traîne)** : `ModifyMenuA/W` + `SetMenuItemBitmaps` + `GetMenuCheckMarkDimensions`
- **Seconde moitié du step 2** du plan ordonné (§5.0) : après la tête mesurée (GDI mapping-mode), les familles socle du
  plateau Win95. Le **menu** (`ModifyMenuA`/`SetMenuItemBitmaps`, mesuré sur le corpus) étend un sous-système qu'on
  possède déjà (`g_u32_menu`), display-free, entièrement vérifiable vs Wine headless.
- **Recette (mesurée bit-exact vs Wine, prefix propre)** : `ModifyMenu(hMnu, uPos, uFlags, uIDNew, lpNew)` **remplace
  l'item en place** — l'item trouvé par `uPos` (MF_BYCOMMAND/MF_BYPOSITION) reçoit de **nouveaux** flags (moins le bit de
  lookup), id/submenu et texte = exactement `u32_menu_setitem` au slot trouvé ; item absent → **FALSE**. Mesuré :
  by-command r1=1, by-position r2=1, absent r3=0 ; `GetMenuState(200)=1` (MF_GRAYED conservé) ; variante W convertit le
  texte large. `SetMenuItemBitmaps(hMenu, uPos, uFlags, hbmpU, hbmpC)` : effet **display-only** (pas de getter headless)
  → on stocke les 2 handles (champs `bmp_unchecked`/`bmp_checked` ajoutés à l'item) et retourne **TRUE** sur item trouvé /
  **FALSE** sinon (mesuré ok=1/miss=0). `GetMenuCheckMarkDimensions()` = `MAKELONG(SM_CXMENUCHECK, SM_CYMENUCHECK)` =
  **13×13** = `0x000d000d` (mesuré) ; +`GetSystemMetrics` **SM_CXMENUCHECK(71)/SM_CYMENUCHECK(72)=13**.
- **Soundness.** ModifyMenu réutilise la machinerie de find/setitem existante (aucun nouveau chemin deviné). Les 3
  fonctions sont **stateful/déterministes** ou pures ; aucun n'a de repli silencieux. `stdcall_pops` : +`ModifyMenuA/W@20`,
  +`SetMenuItemBitmaps@20`, +`GetMenuCheckMarkDimensions@0`.
- **Fixture** `winecorpus/user32_menu2.{c,nodisplay}` : cxcheck/cycheck + checkdim + ModifyMenu (by-cmd/by-pos/absent/W) +
  round-trip GetMenuString/GetMenuItemID/GetMenuState + SetMenuItemBitmaps → **bit-identique à Wine** (`checkdim=000d000d`,
  `s0=[Gamma](id=200) s1=[Delta](id=201) st200=1 cnt=2`, `modifyW=1 s0w=[Wide](id=300)`, `bitmaps ok=1 miss=0`).
- **Portes** : winediff **134→135** (`user32_menu2` ok ; le seul rouge = `gdi_uifont`, **environnemental** fontconfig i386,
  orthogonal), hash transpile `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok, régression unifiée **PASS**.

### 2026-07-19 — [HLE-WIN32][THREAD] Famille **thread (traîne)** : `SetThreadPriority`/`GetThreadPriority` + `OpenProcess` + `TerminateThread`
- **Suite du step 2** du plan ordonné (§5.0) : après menu, la famille **thread** (tête mesurée du plateau Win95 —
  `TerminateThread` dans 5 binaires). Chaque fonction mesurée bit-exact vs Wine (prefix propre), puis modélisée
  **soundement sur le modèle fiber coopératif** (doc 80 §2).
- **`SetThreadPriority`/`GetThreadPriority`** : la priorité est un **hint d'ordonnancement**. Notre scheduler est
  **round-robin déterministe** (un programme qui *dépendrait* de la priorité pour ordonner ses threads serait racy sous
  vrai Windows aussi) ⇒ on ne fait que **round-tripper** la valeur (champ `priority` par-fiber, défaut 0 = NORMAL), on ne
  réordonne pas. Résout le pseudo-handle `GetCurrentThread()` (-2) → fiber courant (`u32_thread_resolve`). Mesuré :
  `default=0`, `Set→1`, `Get→1` (ABOVE_NORMAL round-trip). Handle invalide → `THREAD_PRIORITY_ERROR_RETURN`.
- **`OpenProcess`** : le seul process qui existe = le nôtre (`CreateProcess` = échec sound). Own pid → handle (pseudo
  `0xFFFFFFFF`), autre pid → 0 + `ERROR_INVALID_PARAMETER(87)`. Mesuré `self=1 bogus=0 err=87`. (`OpenProcess@12` était
  déjà dans `stdcall_pops` mais sans shim → abort ; désormais implémenté.)
- **`TerminateThread(h, code)`** : termine de force. Modèle coopératif : le fiber cible est marqué **FST_DONE** avec le
  code, **plus jamais ordonnancé** (ses piles fuient — exactement comme Windows fuit les ressources d'un thread terminé,
  le danger documenté) ; les waiters le voient signalé, `GetExitCodeThread`→code. Se terminer soi-même dégénère en
  ExitThread / exit process (main). Si la victime tenait un lock, aucun fiber ne peut plus l'acquérir → **le détecteur de
  deadlock du scheduler abort bruyamment** (un vrai programme Windows hangerait pareil). Mesuré : `ret=1 ran=1 exit=55
  wait=0` (le worker a atteint `g_ran=1` puis parké dans `Sleep`, **jamais** `g_ran=2`).
- **Fixture** `winecorpus/win32_thread_tail.{c,nodisplay}` : priorité + OpenProcess own/bogus + TerminateThread d'un worker
  parké + GetExitCodeThread + WaitForSingleObject → **bit-identique à Wine** (`prio default=0 set=1 after=1`,
  `openproc self=1 bogus=0 bogus_err=87`, `terminate ret=1 ran=1 exit=55 wait=0`).
- **stdcall_pops** : +`GetThreadPriority@4`, +`SetThreadPriority@8`, +`TerminateThread@8` (`OpenProcess@12` déjà présent).
- **Portes** : winediff **135→136** (`win32_thread_tail` ok ; seul rouge = `gdi_uifont`, environnemental), hash transpile
  `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok, régression unifiée **PASS**.

### 2026-07-19 — [HLE-WIN32] Famille **divers non-display** : `WaitForInputIdle` (harnais) + `WinHelpA/W` (hors-harnais)
- **Suite du step 2** (§5.0) : les membres **non-display** de la famille « divers » du plateau Win95. Les deux autres
  membres mesurés (`TabbedTextOutA`/`GrayStringA`) sont du **rendu texte GDI** (sous-famille FreeType, séparée).
- **`WaitForInputIdle`** : valide seulement pour un **process GUI enfant** qu'on a créé ; on ne crée jamais de vrai
  process (`CreateProcess` = échec sound), donc tout handle ici est le nôtre ou invalide → **WAIT_FAILED (0xFFFFFFFF) +
  ERROR_INVALID_HANDLE(6)**, jamais un faux « idle » réussi. Mesuré vs Wine `self=0xFFFFFFFF bogus=0xFFFFFFFF err=6`.
  Gardé **dans le harnais** `winecorpus/win32_misc_tail.{c,nodisplay}` (déterministe) — **bit-identique à Wine**.
- **`WinHelpA/W`** : `HELP_QUIT`(2) ferme la fenêtre d'aide (absente) → **TRUE** ; toute autre commande nécessite un
  viewer (winhlp32) inexistant headless → **FALSE**. Mesuré vs Wine `ctx=0 quit=1 contents=0 quitW=1`. ARET rend
  immédiatement (aucun spawn). **Piège mesuré (⇒ hors harnais)** : sous Wine, `WinHelp` **spawn un winhlp32 enfant** qui
  **traîne sur le pipe stdout** → toute capture par pipe/`$(...)` (dont `winediff.sh`) **hangerait**. Donc `WinHelp`
  n'entre **pas** dans le corpus winediff ; son shim est vérifié par **mesure directe hors-harnais** (Wine → fichier, pas
  pipe, + cleanup du child), bit-identique. Sound : ARET ne simule jamais qu'une aide a été affichée.
- **stdcall_pops** : +`WaitForInputIdle@8`, +`WinHelpA@16`, +`WinHelpW@16`.
- **Portes** : winediff **136→137** (`win32_misc_tail` ok ; seul rouge = `gdi_uifont`, environnemental), hash transpile
  `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok, régression unifiée **PASS**.
- **⇒ Non-display du plateau = épuisé.** Reste de la famille « divers » = **rendu texte GDI** : `TabbedTextOutA`/
  `GetTabbedTextExtentA` (extent mesuré : no-tab = extent texte simple ; tab expanse au tab-stop, ex. `(75,17)`),
  `GrayStringA` — sous-famille **FreeType** (gate `-DARET_HAVE_FREETYPE`, DIB-hash vs Wine), prochain incrément ciblé.

### 2026-07-19 — [HLE-WIN32][GUI] Sous-famille **texte tabulé GDI** : `TabbedTextOutA/W` + `GetTabbedTextExtentA/W` (FreeType, DIB-hash vs Wine)
- **Dernier morceau valeur du step 2** (§5.0) : étend la famille texte **FreeType** prouvée (G3-text) avec l'expansion des
  tabulations. Rendu **pixel-identique à Wine** (même rasterizer FreeType) + valeurs d'extent/retour bit-exactes.
- **Recette du tab-stop MESURÉE bit-exact vs Wine** (grille de sondes, pas devinée) : largeur de segment = avances par
  défaut (`GetTextExtentPoint32`) ; le crayon saute au **prochain tab-stop STRICTEMENT supérieur** à sa position ;
  stops : **>1 positions** → absolu `org+lpTabPos[j]` (depuis `nTabOrigin`) ; **≤1 ou au-delà** du tableau → multiples de
  `defWidth`, où `defWidth = lpTabPos[0]` (exactement 1 stop) sinon **`8*tmAveCharWidth`** (défaut Wine mesuré) ;
  retour = `MAKELONG(largeurTotale, tmHeight)`. **Piège de mesure attrapé** : `n2_ABCD` sortait 140 vs 139 prédit — cause
  = `GetTextExtentPoint32("D")=12` (D plus large que A/B/C=11), pas un +1 mystère → algo confirmé. Boundary exact
  (segment == stop) → avance au **suivant** (strictement supérieur, mesuré `uni11_A_t`=22).
- **Livré.** `u32_tabbed_core` (partage `u32_textout_core`/`u32_text_width`/`u32_dc_font`, `tmAveCharWidth =
  round(MulFix(OS/2.xAvgCharWidth, x_scale))`) + `u32_next_tab`. Shims A/W pour draw et extent. **Négatifs**
  (tab-stops right-align, rares) et largeur uniforme ≤0 → **abort sound** (jamais deviné). Builder : `GetTabbedTextExtentA/W`
  ajoutés au gate `-DARET_HAVE_FREETYPE` (TabbedTextOut y était déjà). `stdcall_pops` : +`TabbedTextOutA/W@32`,
  +`GetTabbedTextExtentA/W@20`.
- **Fixture** `winecorpus/gdi_tabbedtext.c` (DIB 32bpp mono + hash, display comme les autres GDI) : extents uniforme/
  tableau/au-delà/défaut + 2 draws → **bit-identique Wine** : `ext uni=0013004b arr=0013008c def=0013004b`,
  `tto r1=0013008a r2=0013006d`, **`dibhash=ca64e7d7`** (rendu pixel-exact), `bbox=2 5 138 23`.
- **Portes** : winediff **137→138** (`gdi_tabbedtext` ok ; seul rouge = `gdi_uifont`, environnemental), hash transpile
  `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok, régression unifiée **PASS**.
- **⇒ Famille « divers » du plateau close** (non-display + texte tabulé). Reste hors-plateau : `GrayStringA` (callback de
  dessin custom — complexe, sur demande) et l'imprimante (`OpenPrinterA`… — dépend d'un spooler → probablement échec sound).

### 2026-07-19 — [HLE-WIN32] Famille **imprimante** (winspool) : Enum/GetDefault/Open/ClosePrinter — état « zéro imprimante » déterministe
- **Dernière famille du plateau Win95** (§5.0), faite **sans download** (Internet Archive globalement hors-ligne — page
  « Temporarily Offline » sur tous ses endpoints — donc la re-mesure du corpus est bloquée ; pivot vers un incrément
  auto-suffisant). Pas du **print** : ce sont les **entry points d'énumération/ouverture** qu'un programme appelle pour
  découvrir les imprimantes ; headless il n'y en a **aucune** (pas de spooler/CUPS) — état **déterministe** → vérifiable
  bit-exact comme `WaitForInputIdle`.
- **Recette mesurée vs Wine headless** : `EnumPrintersA/W` (niveaux 1/2/4) → TRUE, `needed=0`, `count=0` (vide) ;
  `GetDefaultPrinterA/W` → FALSE + `ERROR_FILE_NOT_FOUND(2)`, `pcchBuffer` **non touché** ; `OpenPrinterA/W(NULL)` →
  ouvre le **serveur d'impression local** (handle valide, table `U32_PRINTER_BASE`), `OpenPrinter(nom)` → FALSE +
  `ERROR_INVALID_PRINTER_NAME(1801)` (aucune imprimante n'existe) ; `ClosePrinter(nôtre)` → TRUE, `(bogus)` → FALSE +
  `ERROR_INVALID_HANDLE(6)`. **Sound** : jamais une fausse imprimante ; le programme prend son chemin « pas d'imprimante ».
  `DocumentPropertiesA` laissé en **abort sound** (nécessite un handle d'imprimante réelle qu'on n'émet jamais headless → inatteignable).
- **stdcall_pops** : +`OpenPrinterA/W@12`, +`ClosePrinter@4`, +`EnumPrintersA/W@28`, +`GetDefaultPrinterA/W@8`.
- **Fixture** `winecorpus/win32_printer_tail.{c,nodisplay}` → **bit-identique à Wine** : `enum2/enum1 ret=1 needed=0
  count=0`, `getdefault ret=0 err=2 cch=256`, `open_server ret=1 handle=1 close=1`, `open_bogus err=1801`, `close_bogus err=6`.
- **Portes** : winediff **138→139** (`win32_printer_tail` ok ; seul rouge = `gdi_uifont`, environnemental), hash transpile
  `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok, régression unifiée **PASS**.
- **⇒ Plateau Win95 entièrement couvert** (menu, thread, divers, texte tabulé, imprimante). Prochaine vague = re-mesure
  (dès qu'IA revient) OU step 3 (vrai binaire GUI comctl32) / step 4 (MFC/VB + EH C++) — nécessitent un binaire du corpus.

### 2026-07-19 — [STRATÉGIE] Orientation enregistrée : **rétro-cible Windows moderne** (vieux binaire → tourne sur Win11)
- **Réflexion utilisateur** : ARET fait PE32 → ELF/WASM (autre OS) ; ajouter une **cible PE Windows récent** vaudrait-il le
  coup ? Verdict d'ingénierie honnête écrit dans **doc 80 §1.6** (+ pointeurs doc 70 §8.4bis / table doc-80 / ligne priorité).
- **L'archi rend ça presque gratuit pour une moitié, cher pour l'autre** — le **lifter (x86→C) est OS-agnostique** ; seuls le
  **backend** (émettre un PE, mingw/MSVC compile déjà) et le **HLE** (forwarder au vrai Win32 pour l'existant, embarquer la
  réimplé des API retirées) changent. Deux cas **disjoints** :
  - **Cas 1 — vieux 32-bit → PE moderne** : faisabilité HAUTE, utilité MOYENNE (WoW64 les fait déjà tourner ; le gain =
    **bundling autonome** + ressusciter `WinHelp`/DirectDraw retirés).
  - **Cas 2 — vieux 16-bit NE → PE 64-bit** : utilité **UNIQUE** (Win64 ne peut PAS exécuter du 16-bit ; le **volume** du
    vieux logiciel est là — Chip CD majoritairement NE), mais **nouveau frontend lifter** (segmentation/real-mode/Win16/NE),
    jalon dédié, prérequis partagé avec la Phase 8 (multi-arch).
- **Conformité totale** : autonomie **redéfinie** (« zéro dépendance au runtime *supprimé* », qu'ARET embarque) ; **bonus
  oracle** = la cible EST Windows → vrai Win32 = vérité terrain. Décision : documenter maintenant, exécuter plus tard
  (backend PE = petit incrément prouvant le concept ; frontend 16-bit = grand prix planifié). **Aucun code** dans cette
  entrée — orientation seulement.

### 2026-07-19 — [STRATÉGIE] Re-mesure Levier 0 post-plateau (IA revenu) : **29 PE32 Win95** → nouvelle tête = Registry / DDE / OLE-init / misc
- **Étape « reprendre mesure »** (après avoir comblé le plateau : menu/thread/divers/texte-tabulé/imprimante). Internet
  Archive **revenu** → re-téléchargé `BestOfWindows95DotCom/WIN95_09964.iso` (593 Mo), extrait les `apps/*.zip` (2 niveaux),
  classé : **29 PE32 i386** (+ **34 NE 16-bit** — data qui appuie l'orientation rétro-cible 16-bit, doc 80 §1.6). `wallsweep.sh`
  sur les 29 (ISO supprimée après capture, pool gardé en scratchpad).
- **Instructions non-liftées = BRUIT** confirmé (outsd/insb/popad/in/hlt/arpl/les/aaa/daa/bound = data-en-code + privilégié/
  BCD → abort correct). **Le lift est complet.**
- **Nouvelle tête d'imports (par #binaires /29), l'ancien plateau a disparu** (menu/thread/mapping-mode traités) :
  | #bins | famille | imports |
  |---|---|---|
  | 15–17 | **Registry** | `RegCreateKeyA` 17 (+ Reg* : Open/Query/Set/Close/Delete attendus) |
  | 15–17 | **DDE param** | `UnpackDDElParam` 17 / `PackDDElParam` 15 (bit-packing LPARAM↔atom, facile) |
  | 15–16 | **OLE init** | `OleInitialize` 16 / `OleUninitialize` 15 / `CoCreateInstance` 16 |
  | 15–16 | **misc k32/u32** | `IsDBCSLeadByte` 16, `OpenFile` 16, `wvsprintfA` 16, `GetLogicalDrives` 15, `VerInstallFileA` 15 |
  | 5–7 | **GUI profond** | palette (Create/Realize/Select/GetSystemPaletteEntries), capture (Get/Set/Release), clip (Intersect/Exclude/RectVisible), scroll (Get/SetScrollPos/Info, ScrollWindow), cursor/icon, SetROP2, SetStretchBltMode, WindowFromPoint, CreateDIBitmap, CreateRectRgn, GetLocaleInfoA, DrawMenuBar, DefFrameProcA, GetDCEx |
- **Conclusion (la donnée redirige la vague)** : prochaine cible mesurée = **Registry** (tête large, cohérente, stateful
  déterministe → vérifiable vs Wine comme le reste), + le **couple DDE `Pack/UnpackDDElParam`** (trivial, très large) et
  **`OleInitialize`/`OleUninitialize`** (S_OK, très large). Le cluster **GUI profond** (palette/capture/clip/scroll) = la
  couche suivante (aligne aussi avec le step 3 « vrai binaire GUI »). `CoCreateInstance` = dur (vrais objets COM) → plus tard.

### 2026-07-19 — [HLE-WIN32] Quick-wins de la nouvelle tête : **DDE param** (`Pack/Unpack/FreeDDElParam`) + **OLE init** (`OleInitialize/Uninitialize`)
- **Deux familles très larges** de la re-mesure (15–17/29 binaires), emballées avant le Registry.
- **OLE init** : `OleInitialize` initialise COM en interne → partage le compteur de profondeur `aret_co_init_depth`
  existant → **S_OK** au 1er init du thread, **S_FALSE(1)** imbriqué. Mesuré vs Wine : `OleInitialize=0` puis
  `CoInitialize=1` (le nested). `OleUninitialize` déroule comme `CoUninitialize`.
- **DDE param** : les 4 messages `WM_DDE_ADVISE/ACK/DATA/POKE` (0x3E2/4/5/7) ne tiennent pas leurs 2 valeurs dans un LPARAM
  → `PackDDElParam` **alloue** un holder (malloc, -m32 → pointeur 32-bit) et rend un handle ; `UnpackDDElParam` le relit ;
  `FreeDDElParam` le libère. Tout autre message = `MAKELONG(lo,hi)` / `LOWORD`/`HIWORD`. Mesuré vs Wine : round-trip
  **exact** (le handle brut est un pointeur non-déterministe → **non comparé** ; seuls le round-trip + le MAKELONG le sont).
- **Pops (correction de fond, pas seulement additif)** : `CoInitialize`/`CoInitializeEx`/`CoTaskMem*`/`OleInitialize` étaient
  **absents** de `stdcall_pops` → ne marchaient que via le repli « drop du `sub esp` compensateur » (modèle accumulate-args
  **uniquement**). Le corpus Win95 est **push-model** MSVC → le `@N` **doit** être tabulé (sinon esp dérive). Ajoutés :
  `CoInitialize@4`, `CoInitializeEx@8`, `CoTaskMemAlloc@4`, `CoTaskMemFree@4`, `CoTaskMemRealloc@8`, `OleInitialize@4`,
  `PackDDElParam@12`, `UnpackDDElParam@16`, `FreeDDElParam@8` (Un/OleUninitialize = @0, omis).
- **Fixture** `winecorpus/win32_dde_ole.{c,nodisplay}` → **bit-identique Wine** : `ole=0 co=1`, `ack unpack=1 lo=1234
  hi=abcd`, `data lo=1234 hi=abcd`, `user pack=abcd1234 lo=1234 hi=abcd free=1`.
- **Portes** : winediff **139→140**, hash transpile `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok,
  régression unifiée **PASS**. Prochain : le **Registry** (tête à 17 binaires).

### 2026-07-19 — [HLE-WIN32] **Registry en mémoire** (advapi32) — la tête mesurée (RegCreateKey 17/29), round-trip bit-identique Wine
- **La tête de la re-mesure.** Les stubs registre existants **échouaient toujours** (`RegCreateKeyEx`→ACCESS_DENIED,
  `RegQueryValueEx`→NOT_FOUND) — sound mais un programme qui **écrit puis relit** ses réglages n'obtenait rien → cassait
  le pattern dominant (17 binaires). Remplacés par un **vrai registre en mémoire** (arbre de clés + valeurs, bornés).
- **Principe (sound).** Le registre démarre **VIDE**. Une valeur écrite ce run se relit **exactement** (round-trip) ; une
  valeur **jamais écrite** (clé système, réglage d'un installeur / d'un run précédent) reste honnêtement
  **ERROR_FILE_NOT_FOUND**, jamais devinée → le programme prend son chemin par défaut. Roots prédéfinis
  (HKCR/HKCU/HKLM/HKU/HKPD/HKCC/HKDD) ; handle = `U32_REG_BASE|idx` (les clés vivent dans l'arbre, `RegCloseKey`=no-op).
- **Livré (A complet + W par conversion de nom, data verbatim)** : `RegCreateKeyEx`/`RegCreateKey`, `RegOpenKeyEx`/
  `RegOpenKey`, `RegSetValueEx`, `RegQueryValueEx` (size-query data=NULL → *cb=len ; buffer trop petit → MORE_DATA(234) +
  taille requise ; absent → 2), `RegCloseKey`/`RegFlushKey`, `RegDeleteValue`, `RegDeleteKey` (récursif, refuse un root),
  `RegEnumValue`, `RegEnumKeyEx`/`RegEnumKey`, `RegQueryInfoKey` (compte sous-clés/valeurs + longueurs max). Disposition
  `REG_CREATED_NEW_KEY(1)` / `REG_OPENED_EXISTING_KEY(2)` selon que la clé finale a été créée. Piège corrigé : `u32_w2n`
  utilisé par les shims W **avant** sa définition → **forward-declaration** ajoutée (sinon décl. implicite → conflit static).
- **stdcall_pops** : +`RegCreateKeyA@12`, `RegCreateKeyExW@36`, `RegCreateKeyW@12`, `RegEnumKeyExA@32`, `RegEnumValueA@32`,
  `RegFlushKey@4`, `RegOpenKeyExW@20`, `RegOpenKeyW@12`, `RegQueryValueExW@24`, `RegSetValueExW@24` (les A de base y étaient).
- **Fixture** `winecorpus/win32_registry.{c,nodisplay}` (self-cleaning : delete la clé au début → disposition stable, et à
  la fin) → **bit-identique à Wine** : `create rc=0 disp=1`, `qnum type=4 cb=4 val=cafe1234`, `qstr type=1 cb=6 [hello]`,
  `qmiss rc=2 | qsmall rc=234 cb=6`, `enum0 [num] type=4 nlen=3`, `info subkeys=0 values=2`, `reopen disp=2`,
  `open_missing rc=2`, `delval/delkey rc=0`.
- **Portes** : winediff **140/141** (141 fixtures, seul rouge = `gdi_uifont` environnemental), hash transpile
  `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok, régression unifiée **PASS** (le remplacement des stubs
  registre ne casse aucun binaire du corpus — plink/PuTTY qui probaient le registre prennent toujours leur défaut, la clé
  n'existant pas dans un registre vide).
- **Correctif harnais (honnêteté) — `win32_printer_tail` ne LINKAIT pas** : la ligne de link de `winediff.sh` n'incluait
  pas `-lwinspool` (mingw auto-linke advapi32 → le registre passait, mais **pas** winspool → `EnumPrintersA` undefined).
  Mon commit imprimante antérieur annonçait « winediff 139 » d'après le test **ciblé** (que je linkais `-lwinspool` à la
  main), **sans** avoir lancé le harnais complet → la fixture ne buildait pas sous le harnais. Corrigé : `-lwinspool`
  ajouté à `winediff.sh` (comme -lcomctl32/-lversion). `win32_printer_tail` **build et passe** désormais (140/141).

### 2026-07-19 — [HLE-WIN32] Batch **misc k32/u32** : `IsDBCSLeadByte` + `wvsprintfA/W` + `OpenFile` + `GetLogicalDrives`
- Suite de la vague mesurée (chaque ~16/29 binaires). Mesuré vs Wine, implémenté selon la fidélité atteignable.
- **`IsDBCSLeadByte`** : l'ACP modélisée = CP1252 (single-byte) → **toujours 0** (mesuré). (`IsDBCSLeadByteEx` existait déjà.)
- **`wvsprintfA/W`** (le trou mesuré — forme **va_list** de `wsprintfA/W`) : l'arglist est un **pointeur** vers les args
  packés → `aret_vformat`/`aret_wvformat` lit directement dessus. Bit-identique Wine (`n=-7 s=hi x=00ab u=42 c=Q`).
- **`OpenFile`** (API fichier Win16-legacy) : HFILE=fd, même modèle que `_lopen` (translate_path). OF_READ/WRITE/READWRITE/
  OF_CREATE(crée+tronque)/OF_DELETE(unlink)/OF_EXIST(open-test), OFSTRUCT rempli (nErrCode, szPathName). **Non fixturé
  (honnêteté) : le OpenFile de Wine 9.0 est peu fiable ici** — `OF_CREATE` **échoue et ne crée pas** (`nErr=13`), `OF_EXIST`
  est flaky ; matcher ce bug serait **unsound** (l'OpenFile d'ARET est **correct** per contrat Win32 / équivalence POSIX à
  `_lopen` prouvé). Shim gardé, vérifié par cette équivalence, pas par l'oracle Wine cassé.
- **`GetLogicalDrives`** : expose **C:** (bit 2) — le disque système conventionnel. (Wine expose aussi **Z:** = racine Unix,
  artefact Wine env-dépendant → on modélise le minimum portable ; la fixture teste le **bit C: dérivé**, pas le masque brut.)
- **stdcall_pops** : +`IsDBCSLeadByte@4`, `OpenFile@12`, `wvsprintfA@12`, `wvsprintfW@12` (`GetLogicalDrives`=@0 déjà listé).
- **Fixture** `winecorpus/win32_misc_k32.{c,nodisplay}` (DBCS + wvsprintf + driveC) → **bit-identique Wine**.
- **Portes** : winediff **141→142** (fixtures ; seul rouge = `gdi_uifont` env), hash transpile `19acad982194bf07`
  **inchangé**, `table_is_sorted_by_name` ok, régression unifiée **PASS**. **Reste de la vague** : `VerInstallFileA`
  (install-file, complexe) et le cluster **GUI profond** (palette/capture/clip/scroll — converge avec le step 3).

### 2026-07-19 — [HLE-WIN32][GUI] Cluster GUI profond (1/n) : **capture souris** + **barres de défilement** (état pur)
- Première coupe du cluster « GUI profond » de la re-mesure (5-7 binaires chacun). Choisi les APIs d'**état
  window-manager sans implication de dessin** (les APIs à correction-dessin — ROP2/clip/palette — attendront un vrai
  binaire GUI, step 3). État pur → déterministe, entièrement vérifiable vs Wine.
- **Capture** (`SetCapture`/`GetCapture`/`ReleaseCapture`) : une fenêtre détient la capture (global `g_u32_capture`).
  SetCapture rend le précédent, GetCapture le courant, ReleaseCapture clear (TRUE). Mesuré : `prev=0 cur=h rel=1 after=0`.
- **Scroll** (`Set/GetScrollRange`, `Set/GetScrollPos`, `Set/GetScrollInfo`) : état `{min,max,page,pos}` **par-fenêtre,
  par-barre** (SB_HORZ/VERT/CTL, ajouté à `g_u32_win`). `SetScrollPos` clampe à `[min,max]` et rend le pos précédent ;
  `SetScrollRange` re-clampe le pos ; `SetScrollInfo` applique fMask et clampe le pos à **`[nMin, nMax-nPage+1]`** avec une
  page (mesuré : 95→**81** pour min0/max100/page20) ; `nTrackPos`=pos courant (pas de drag headless). SCROLLINFO offsets
  mesurés (nMin@8/nMax@12/nPage@16/nPos@20/nTrackPos@24).
- **stdcall_pops** : +`SetCapture@4`, `Set/GetScrollRange@20/16`, `Set/GetScrollPos@16/8`, `Set/GetScrollInfo@16/12`
  (`Get/ReleaseCapture`=@0). Piège : la création de fenêtre remet à zéro le nouveau champ `scroll` (slot réutilisé).
- **Fixture** `winecorpus/win32_capture_scroll.c` (display, comme les autres GUI) → **bit-identique Wine** (capture
  round-trip + range/pos/clamp + SCROLLINFO page-clamp 95→81).
- **Portes** : winediff **142→143** (fixtures ; seul rouge = `gdi_uifont` env), hash transpile `19acad982194bf07`
  **inchangé**, `table_is_sorted_by_name` ok, régression unifiée **PASS**. **Reste du cluster GUI** : palette (32bpp =
  no-op sound), clip (`IntersectClipRect`/`RectVisible` — correction-dessin), `SetROP2`/`SetStretchBltMode` (correction-
  dessin, à gater), `WindowFromPoint`, `CreateDIBitmap`/`CreateRectRgn` — au fil du step 3 (vrai binaire GUI).

### 2026-07-19 — [DEMO][GUI] Step 3 : **un vrai binaire GUI Win32 shippé (`FishTank.exe`) atteint sa message loop sous ARET**
- **Step 3 du plan** (§5.0) : lancer un vrai binaire GUI du corpus pour que la donnée dicte les shims. Corpus 29 PE32 :
  trié par DLL importées → écarté **VB** (Octobre = VB40032, Wine ne l'a pas → pas d'oracle), **jeux** (GlidePath = WinG+
  MFC30), **OpenGL** (NBODY20). Retenu **`FishTank.exe`** (aquarium Win95) = **pur Win32** (kernel32/user32/gdi32/comdlg32/
  comctl32), que **Wine exécute** (message loop, tué au timeout).
- **Résultat.** FishTank **transpile** (1380 fn, **0 unresolved direct call**, 3 partial-asm = data-en-code), et à
  l'exécution **atteint sa message loop comme Wine**. Les instructions non-liftées sont du **bruit data-en-code** (out/in/
  arpl/popad/aam/into/salc = privilégié/segment/BCD). Sur son **chemin de démarrage réel**, il n'appelait que **3 imports
  non implémentés** (les 25 autres du listing statique sont derrière l'interaction — drag-drop/menu/impression — non
  atteints headless).
- **Les 3 imports du chemin réel, implémentés + mesurés vs Wine** : `GetProcessVersion(0)`=**0x00040000** (subsystem Win4.0,
  ère Win95), `SetMessageQueue`=**1** (no-op Win16 obsolète), `GetCursorPos`=**1** (remplit le POINT à l'invariant écran
  (0,0) — la vraie position souris est env-dépendante → seul le retour est comparé, jamais un POINT non-écrit = pas de faux
  silencieux). Avant : stubs faibles « warn + return 0 » (GetCursorPos laissait le POINT non écrit — faux silencieux
  potentiel). Après : **FishTank n'a plus AUCUN import non implémenté sur son chemin d'exécution**.
- **stdcall_pops** : +`GetCursorPos@4`, `GetProcessVersion@4`, `SetMessageQueue@4`.
- **Fixture** `winecorpus/win32_procver_cursor.{c,nodisplay}` → **bit-identique Wine** (`procver=00040000`, `setmsgqueue=1`,
  `getcursorpos ret=1`).
- **Portes** : winediff **143→144**, hash transpile `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok,
  régression unifiée **PASS**. **⇒ Step 3 démontré** : la machinerie contrôles + le chemin de démarrage tiennent sur du
  **réel shippé**. Reste (piloté par interaction, au fil des besoins) : le tail statique de FishTank (CreateDIBitmap, clip
  `IntersectClipRect`/`RectVisible`, DrawIcon/DrawFocusRect, drag-drop `DragQueryFileA`, `LoadMenuA`/accélérateurs, GrayString).

### 2026-07-19 — [HLE-WIN32][GUI] Vers « pas display-free » : `DrawFocusRect` (1ʳᵉ primitive de peinture de contrôle) + découverte des **couleurs système**
- **Objectif** : commencer à sortir du display-free pour les contrôles. Enquête FishTank (écran virtuel Xvfb) : son UI est
  un **dialogue à contrôles natifs** (BUTTON/EDIT/COMBOBOX, user32) → peinture display-free → écran noir. Le doc 70 §7
  documente désormais l'**écran virtuel comme oracle GUI** et la **stratégie widgets** (comctl32 par lifting+GDI ; base
  user32 par HLE-paint ou win32k — le lifting DLL **ne** les couvre **pas** gratis).
- **Découverte bloquante mesurée** : les primitives de contrôle (DrawEdge, boutons, fonds) tirent leurs couleurs de
  `GetSysColor`, or **ARET rend le classique Win95** (`3DFACE=c0c0c0`, `BTNSHADOW=808080`) tandis que **Wine 9.0 rend un
  thème moderne clair** (`3DFACE=f5f5f5`, `BTNSHADOW=a6a6a6`, `3DDKSHADOW=6a6a6a`, `3DLIGHT=e3e3e3`, captions orange
  `fa9632`). Ce sont les **défauts hardcodés de Wine 9.0** (version-stables, pas per-run) → même classe env-dépendante que
  `gdi_uifont`. ⇒ toute peinture de contrôle à base de couleurs système ne sera bit-exacte vs **ce** Wine qu'en **alignant
  `u32_syscolor` sur les valeurs mesurées de Wine** (décision doctrine « Wine=oracle », à trancher — vs garder le look
  classique authentique). **Prochaine étape** : aligner GetSysColor + `DrawEdge`/`DrawFrameControl`, DIB-hash vs Wine.
- **Livré (theme-independent, donc faisable NOW)** : **`DrawFocusRect`** — contour pointillé 1px **XOR-inversé** (dot si
  `(x+y)` impair, parité en coords absolues → pavage sans couture ; passe unique, pas de coin XOR deux fois). Aucune couleur
  système → bit-exact quel que soit le thème. Sur la liste d'imports de FishTank. `stdcall_pops` : +`DrawFocusRect@8`.
- **Fixture** `winecorpus/gdi_focusrect.c` (DIB 32bpp + hash, 2 rects à offsets impairs pour tester la parité) →
  **bit-identique Wine** (`x3=3f3f3f`=c0c0c0^ffffff, `focus_dibhash=4802145c`).
- **Portes** : winediff **144→145**, hash transpile `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok,
  régression unifiée **PASS**.

### 2026-07-19 — [HLE-WIN32][GUI] `DrawEdge` (le biseau 3D des contrôles) + **décision couleurs = classique Win95 authentique**
- **Décision (prise, modal utilisateur ayant échoué 2×)** : garder les **couleurs système classiques Win95**
  (`3DFACE=c0c0c0`, `BTNSHADOW=808080`, +ajout `3DDKSHADOW=000000`/`3DLIGHT=e0e0e0`) — le look pour lequel ces apps ont
  été conçues. Le thème moderne clair de Wine 9.0 est un **artefact env** (même classe que `gdi_uifont`) ; on ne l'aligne
  pas. **Réversible** (l'utilisateur peut préférer Wine-match plus tard).
- **Conséquence de vérification (clé)** : les couleurs absolues diffèrent (ARET classique vs Wine moderne) → **pas de
  DIB-hash** vs Wine pour la peinture de contrôle. On vérifie la **STRUCTURE, index par index, theme-independent** : pour
  chaque pixel de bord, la fixture teste `pixel == GetSysColor(INDEX attendu)`. ARET (classique) et Wine (moderne)
  impriment alors **les mêmes 1** (chacun contre sa propre palette) → layout + mapping d'index **prouvés bit-à-bit** sans
  dépendre de la palette. Nouvelle technique d'oracle, réutilisable pour tous les contrôles.
- **Livré : `DrawEdge`** (le biseau 3D de tout bouton/group-box/bord d'edit). Deux anneaux ; un pixel prend la couleur
  « bas-droite » s'il est sur la colonne droite/ligne basse de son anneau, sinon « haut-gauche » (mesuré vs Wine : les
  lignes sombres bas/droite gagnent aux coins TR/BL). Indices mesurés : RAISED outer `3DLIGHT/3DDKSHADOW` inner
  `BTNHIGHLIGHT/BTNSHADOW` ; SUNKEN inverse. `BF_MIDDLE` remplit l'intérieur en `3DFACE`. Sous-ensemble **EDGE_RAISED/
  EDGE_SUNKEN + BF_RECT** (mesurés) ; autres combos edge/flags → **abort sound**. `stdcall_pops` : +`DrawEdge@16`.
- **Fixture** `winecorpus/gdi_drawedge.c` (structural, index-based) → **bit-identique Wine** (raised/sunken outer+inner+
  corners + BF_MIDDLE, tous `=1`).
- **Portes** : winediff **145→146**, hash transpile `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok,
  régression unifiée **PASS**. **Fondation posée** : DrawFocusRect + DrawEdge = les primitives que tout contrôle utilise.
  Suite : `DrawFrameControl` (bouton/checkbox complet), puis câbler au WM_PAINT des contrôles → composer à l'écran (SDL).

### 2026-07-19 — [HLE-WIN32][GUI] `DrawFrameControl` (bouton poussoir) — bâti sur DrawEdge (BF_SOFT ajouté)
- Suite des primitives de peinture de contrôle. **Découpe DrawEdge en helper** `u32_drawedge(bm, rect, edge, flags)`
  (partagé par `DrawEdge` et `DrawFrameControl`) + ajout **`BF_SOFT`** : sur un bord RAISED, échange les indices du côté
  clair (`3DLIGHT`↔`BTNHIGHLIGHT`) — le biseau « soft » du bouton (mesuré).
- **`DrawFrameControl(DFC_BUTTON, DFCS_BUTTONPUSH)` normal = bouton poussoir** = `DrawEdge(EDGE_RAISED, BF_SOFT|BF_RECT|
  BF_MIDDLE)` (mesuré vs Wine : outer `BTNHIGHLIGHT/3DDKSHADOW`, inner `3DLIGHT/BTNSHADOW`, face `3DFACE`). Pushed +
  check/radio + caption/menu/scroll → **abort sound** (chacun un suivant mesuré, jamais un cadre faux). `stdcall_pops` :
  +`DrawFrameControl@16`.
- **Fixture** `winecorpus/gdi_framecontrol.c` (structural, index-based) → **bit-identique Wine** (`oTL/oBR/iTL/iBR/face` tous `=1`).
- **Portes** : winediff **146→147**, hash transpile `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok,
  régression unifiée **PASS**. **Primitives de contrôle** : DrawFocusRect + DrawEdge + DrawFrameControl(push). Suite :
  câbler au WM_PAINT des contrôles BUTTON (peindre le cadre + le texte) → dialogue visible via SDL.

### 2026-07-19 — [HLE-WIN32][GUI] **1er contrôle natif qui PEINT** : le BUTTON (cadre + texte) — sortie du display-free
- **Le vrai franchissement du display-free** : jusqu'ici les contrôles prédéfinis étaient des fenêtres logiques
  **sans wndproc** (data-only, aucune peinture). Désormais un **BUTTON peint son apparence**.
- **Architecture** : `SendMessage` à un contrôle (wndproc==0) route vers un **proc de contrôle intégré** `u32_control_proc`
  (avant : retournait 0). Le BUTTON y gère `WM_SETFONT` (stocke le font, champ `ctrl_font`), `WM_GETFONT`, et
  **`WM_PRINTCLIENT`** (peint dans le DC fourni). `u32_button_paint` = `DrawFrameControl` (cadre soft-raised + face) +
  **caption centrée** (`u32_drawtext` DT_CENTER|DT_VCENTER|DT_SINGLELINE, `COLOR_BTNTEXT`, transparent, dans le font du
  contrôle). Mesuré vs Wine (`WM_PRINTCLIENT`). Builder : `CreateFontA/W`/`CreateFontIndirectA/W` ajoutés au gate
  `-DARET_HAVE_FREETYPE` (une appli qui crée des fonts peint du texte via ses contrôles même sans `DrawText` direct).
- **Vérif** : le cadre **structurel** (index-based, theme-independent) ; le texte dépend du font résolu (caveat gdi_uifont)
  → on assert seulement que des pixels de caption existent (`caption_drawn=1`). `stdcall_pops` : aucun nouveau (WM_* sont
  des messages). Piège corrigé : `WM_PRINTCLIENT` = **0x0318** (pas 0x0317 = WM_PRINT) ; forward-decl `u32_ansi_cp`/
  `u32_drawtext` (utilisés avant leur définition) ; `ctrl_font` remis à 0 à la création (réutilisation de slot).
- **Fixture** `winecorpus/user32_button_paint.c` (crée un BUTTON, WM_SETFONT, WM_PRINTCLIENT dans un DIB) →
  **bit-identique Wine** (`frame oTL=1 oBR=1 iTL=1 face=1 | caption_drawn=1`).
- **Portes** : winediff **147→148**, hash transpile `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok,
  régression unifiée **PASS**. **⇒ 1ᵉʳ contrôle natif réellement peint.** Suite : les autres contrôles (STATIC/EDIT/
  group-box) même machinerie, puis rendre le **dialogue visible** (fenêtre SDL) et composer les enfants → FishTank affiché.

### 2026-07-19 — [HLE-WIN32][GUI] Unités de dialogue → pixels : `MapDialogRect` + **base-units par-dialogue calculées façon Wine** (bit-exact)
- **Objectif / fondation « dialogue visible »** : les positions des contrôles d'un dialogue sont en **unités de dialogue**
  (grille calée sur la police : 1 DU horizontale = ¼ largeur-moyenne-caractère, 1 DU verticale = ⅛ hauteur-caractère), donc
  la géométrie **pixel dépend de la police résolue**. C'est le maillon manquant pour composer un dialogue à l'écran.
- **Fausse alerte « seulement visuel » levée** : la conversion **EST bit-vérifiable**. On reproduit **exactement et en
  autonomie** l'algorithme de Wine (`GdiGetCharDimensions`, lu dans sa source — doctrine §1 « Wine = livre de recettes ») :
  sélectionner la police du dialogue dans un DC scratch, puis `du_x = (extent(alphabet 52 lettres).cx / 26 + 1) / 2`,
  `du_y = tmHeight`. Police depuis le point-size du template comme Wine : `lfHeight = -MulDiv(pt, LOGPIXELSY=96, 72)`. On
  **réutilise nos métriques FreeType existantes** (`u32_dc_font`/`u32_text_width`, déjà bit-identiques Wine cf. gdi_textout)
  → mêmes base-units que Wine. **Pas de FreeType ⇒ abort sound** (jamais un facteur d'échelle deviné).
- **Preuve bit-exacte** : fixture nommant **« DejaVu Sans »** (police que **les deux moteurs résolvent identiquement**, comme
  gdi_textout) → même TTF → mêmes métriques → mêmes unités. Le trace `WINEDEBUG=+dialog` de Wine imprime lui-même
  `DIALOG_CreateIndirect units = 7,13` = **exactement** les `base 7 13` d'ARET ; `MapDialogRect({2,3,160,100})` = `{4,5,280,163}`
  des deux côtés (MulDiv + arrondi GDI identiques). Le caveat résiduel = **le seul et même que gdi_uifont** (une police type
  « MS Sans Serif » se substitue de façon env-dépendante) — pas une nouvelle source de flou.
- **Livré** : champ `du_x/du_y` par-fenêtre ; `u32_dlg_base_units` (calcul+stockage à la création du dialogue, sous
  `ARET_HAVE_FREETYPE`) ; `MapDialogRect` (MulDiv façon Wine, arrondi `gdi_muldiv`). `u32_dialog_create` capture désormais
  point-size/weight/italic/typeface (au lieu de les jeter). Builder : `MapDialogRect` ajouté au gate `-DARET_HAVE_FREETYPE`.
  `stdcall_pops` : +`MapDialogRect@8`.
- **Fixture** `winecorpus/user32_dlgunits.c` (template DS_SETFONT en mémoire, DejaVu Sans 8pt, modeless, MapDialogRect) →
  **bit-identique Wine** (`base 7 13`, `rect 4 5 280 163`). ⚠️ fenêtrée : Wine crée un vrai window (Xvfb requis) ; ARET headless.
- **Portes** : winediff **148→149 fixtures** (nouvelle passe ; le seul rouge = `gdi_uifont`, env fontconfig i386), hash transpile
  `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok. **⇒ Fondation posée** pour la géométrie des contrôles :
  suite = parser x/y/cx/cy + classname des contrôles dans le template → placer/dimensionner chaque enfant via ces base-units →
  composer leur peinture dans le framebuffer du dialogue → dialogue FishTank visible.

### 2026-07-19 — [HLE-WIN32][GUI] **Géométrie des contrôles de dialogue** (x/y/cx/cy → pixels + classname), bit-exact vs Wine
- **Incrément 2 vers « dialogue visible »** (sur la fondation base-units) : `u32_dialog_create` parsait les contrôles mais
  **jetait leur géométrie et leur classe** (tous à 0,0,0,0, classname vide). Désormais il extrait `x,y,cx,cy` (unités de
  dialogue) + la **classe** (atome prédéfini `0xFFFF`+ordinal → "Button"/"Edit"/"Static"/"ListBox"/"ScrollBar"/"ComboBox", ou
  nom-chaîne) de chaque contrôle, **et** la taille client du dialogue lui-même. Conversion unités→pixels via les base-units du
  dialogue (`gdi_muldiv` = MulDiv arrondi GDI). `u32_new_control` élargi (x,y,w,h + classname stockés). La classe pilotera le
  proc de peinture intégré (`u32_control_proc` : BUTTON peint déjà).
- **Base-units en best-effort** : la peinture de texte abortait dur (`u32_dc_font`) si une police ne résolvait pas — correct
  pour `TextOut`, mais un dialogue **display-free qui ne lit jamais sa géométrie** ne doit pas en dépendre. Ajout d'un flag
  `g_dc_font_quiet` : le calcul des base-units tolère une police non résolue (géométrie non mise à l'échelle, **pas d'abort**).
  Builder : les **créateurs de dialogue** (`DialogBoxParam`/`CreateDialogParam`/…Indirect…) ajoutés au gate FreeType (un
  dialogue DS_SETFONT place ses contrôles via ces métriques).
- **Vérif bit-exacte independent-du-WM** : lire chaque contrôle **relatif au client du dialogue** — `GetWindowRect` puis
  `MapWindowPoints(NULL, hDlg, …)` (ARET modélise l'origine client = position fenêtre ; Wine soustrait son cadre non-client) →
  la position écran du dialogue **s'annule** des deux côtés → comparaison bit-exacte indépendante du window manager.
- **Fixture** `winecorpus/user32_dlgcontrols.c` (dialogue DejaVu Sans, BUTTON + EDIT à positions DLU connues, template
  construit octet-par-octet avec alignement DWORD) → **bit-identique Wine** : `dlg client w=350 h=195`, `ctl 100 Button
  x=18 y=33 w=88 h=23`, `ctl 101 Edit x=18 y=65 w=105 h=20`.
- **Portes** : winediff **149→150 fixtures** (les 4 fixtures dialogue vertes ; seul rouge = `gdi_uifont` env), hash transpile
  `19acad982194bf07` **inchangé** (le refactor `u32_dc_font` est comportement-identique quand `g_dc_font_quiet=0`),
  `table_is_sorted_by_name` ok. **⇒ Géométrie + classes des contrôles prêtes.** Suite (incrément 3) : composer la peinture de
  chaque enfant (BUTTON fait ; +STATIC/EDIT) dans le framebuffer du dialogue à son offset → DIB-hash structurel → puis fenêtre
  SDL dimensionnée → dialogue FishTank visible (oracle Xvfb).

### 2026-07-19 — [HLE-WIN32][GUI][DEMO] **Dialogue à contrôles natifs VISIBLE** : compositing des enfants → fenêtre SDL (sortie du display-free dialogue)
- **Incrément 3 = la marche visible** (sur base-units + géométrie) : un dialogue Win32 à contrôles natifs **s'affiche réellement**
  comme ELF autonome (SDL, sans Wine). `u32_dialog_composite(di)` : remplit le framebuffer client avec **COLOR_3DFACE** (couleur
  d'effacement du dialogue, mesurée vs Wine) puis peint chaque **enfant visible à son offset** (chaque BUTTON via
  `u32_button_paint` dans un DIB temporaire, blitté à `(x,y)`). Câblé dans `sdl_window_show` (+ recompose en fin de
  `u32_dialog_create` pour le cas WS_VISIBLE où l'auto-show précède la création des contrôles). Font du dialogue **persistée** et
  appliquée aux contrôles (Windows envoie WM_SETFONT(font dialogue) à chaque contrôle à l'init — reproduit) → **les captions se
  peignent** ("OK"/"Cancel"/… centrées, FreeType).
- **Fenêtre créée à la bonne taille** : base-units calculées **avant** `u32_window_create` (refactor out-params) → le dialogue
  visible reçoit d'emblée une SDL_Window à sa taille pixel. Builder : les **créateurs de dialogue** ajoutés au gate **SDL**
  (`CreateWindowExA/W` seul ne suffisait pas — un dialogue n'appelle pas CreateWindowEx directement) ; `u32_dialog_composite`
  gardé `#ifdef ARET_HAVE_SDL` (utilise le framebuffer client).
- **Vérif = qualitative (écran virtuel), ASSUMÉE** : Wine n'offre **aucune API pour capturer son propre dialogue composé** dans
  un DIB (`WM_PRINT PRF_CHILDREN` **ne peint pas** les enfants — mesuré : 0 pixel enfant) → pas de DIB-hash possible pour la
  composition. Preuve : **capture Xvfb** de l'app ARET vs Wine (doc 70 §7) → **layout identique** (dialogue + 3 boutons aux mêmes
  positions/tailles), ARET en couleurs Win95 classiques (décision assumée §7), Wine en thème moderne. Les **briques** sont, elles,
  bit-exactes (base-units, géométrie, peinture d'un bouton) → la composition est **correcte par composition**.
- **Fixture non-crash** `winecorpus/user32_dlgpaint.c` (dialogue visible + bouton → composite → `done`, bit-identique Wine) : garde
  déterministe contre un crash/abort du chemin composite (les pixels composés, eux, ne sont pas diffables vs Wine).
- **Portes** : winediff **150→151 fixtures** (`user32_dlgpaint` verte ; seul rouge = `gdi_uifont` env), hash transpile
  `19acad982194bf07` **inchangé**, difftest 272/272, `table_is_sorted_by_name` ok. **⇒ Un dialogue à contrôles natifs s'affiche
  en ELF natif** — le display-free du dialogue est franchi. C'est exactement ce qui manquait à FishTank (dialogue noir). Reste :
  peinture STATIC/EDIT/checkbox/radio (même machinerie `u32_control_proc`+composite), repaint sur invalidation, focus/interaction.

### 2026-07-19 — [HLE-WIN32][GUI] Contrôles **STATIC + EDIT** peints (labels + champs texte) — bit-exact WM_PRINTCLIENT
- Suite des contrôles natifs (après BUTTON). **STATIC** (`u32_static_paint`) : remplit **COLOR_3DFACE** (fond dialogue, mesuré vs
  Wine) + texte **gauche/haut** en **COLOR_WINDOWTEXT**, transparent, font du contrôle. **EDIT** (`u32_edit_paint`) : client
  **COLOR_WINDOW** (blanc) + texte gauche/vcentré ; la **bordure 3D creusée est non-cliente** (mesuré : WM_PRINTCLIENT ne peint
  que le client blanc+texte, le `WS_EX_CLIENTEDGE` est hors-client) → ajoutée par le **composite** (`EDGE_SUNKEN` autour du rect,
  texte inséré de 2px). Helpers factorisés : `u32_ctrl_text`/`u32_ctrl_fill`/`u32_ctrl_paintable`/`u32_control_paint_full`.
- `u32_control_proc` **dispatch par classe** (button/static/edit) pour WM_SETFONT/GETFONT/**PRINTCLIENT** ; le composite du
  dialogue peint tout contrôle `u32_ctrl_paintable` via `u32_control_paint_full` (client + bordure éventuelle).
- **Vérif bit-exact (structurel, index-based, theme-independent)** : `winecorpus/user32_static_paint.c` (`bg=1`[3DFACE]
  `magenta=0` `text_drawn=1`) et `user32_edit_paint.c` (`bg=1`[COLOR_WINDOW] `magenta=0` `text_drawn=1`), **bit-identiques Wine**.
  Le fond (index de couleur) est prouvé exact des deux côtés (ARET classique / Wine moderne impriment le même 1) ; le texte est
  font-dépendant → seule son existence est assertée (caveat gdi_uifont).
- **Rendu** : un **formulaire complet** (3 labels + 3 champs EDIT avec valeurs + bouton, type calculateur de volume d'aquarium)
  s'affiche en ELF natif, **layout identique à Wine** (capture Xvfb ; ARET couleurs Win95 classiques, Wine thème moderne).
- **Portes** : winediff **151→153 fixtures** (static/edit paint vertes ; seul rouge = `gdi_uifont` env), hash transpile
  `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok. **Reste contrôles** : checkbox/radio (styles BUTTON — cadre +
  glyphe coché), repaint sur invalidation, focus + interaction (clic → `WM_COMMAND` au DLGPROC).

### 2026-07-19 — [HLE-WIN32][GUI] **Case à cocher** (checkbox) : glyphe `DrawFrameControl` bit-exact + peinture du contrôle
- **Découverte de méthode (importante)** : Wine **ne peint RIEN** via `WM_PRINTCLIENT` pour un checkbox/radio (ni aucun BUTTON
  non-push) — seul **BS_PUSHBUTTON** se peint ainsi (mesuré). ⇒ on **ne peut pas** vérifier bit-exact un checkbox *entier* (pas
  de référence capturable, comme le composite du dialogue). **Mais** la primitive `DrawFrameControl(DFC_BUTTON, DFCS_BUTTONCHECK)`
  **est** capturable → on la vérifie bit-exact, et la peinture du contrôle s'appuie dessus (composite = qualitatif Xvfb).
- **Glyphe 13×13 mesuré pixel-exact** : la case = `DrawEdge(EDGE_SUNKEN, BF_RECT)` (bord creusé, indices déjà bit-exacts) +
  intérieur **COLOR_WINDOW** + (si `DFCS_CHECKED`) la **coche Marlett** = 21 pixels **COLOR_WINDOWTEXT** au motif mesuré exact
  (hardcodé). `aret_DrawFrameControl` étendu (case 13×13 ; radio = courbe → abort-sound comme Ellipse ; autres tailles → abort).
- **Peinture du contrôle** : `u32_check_paint` (fond 3DFACE + glyphe gauche-vcentré selon `check_state` + label à droite).
  Dispatch BUTTON par sous-style (`u32_btn_is_push`/`u32_btn_is_check` sur les 4 bits BS_*) dans `u32_control_paint_full` **et**
  `u32_control_proc` (WM_PRINTCLIENT : seul le push peint, comme Wine). Re-composition sur **`UpdateWindow`** → l'état coché posé
  en `WM_INITDIALOG` (CheckDlgButton) s'affiche (avant : composite one-shot à la création, cases toujours vides).
- **Vérif** : `winecorpus/gdi_framecontrol_check.c` (structurel index-based) → **bit-identique Wine** (`oTL/iTL/iBR/oBR/field=1`,
  `tick=0` non coché / `tick=1` coché). Rendu : dialogue d'options (4 cases dont 2 cochées + label + bouton) **identique à Wine**
  (capture Xvfb, coches Marlett incluses).
- **Portes** : winediff **153→154 fixtures** (gdi_framecontrol_check verte ; seul rouge = `gdi_uifont` env), hash transpile
  `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok. **Reste contrôles** : radio (cercle, niveau-recherche), group
  box (DrawEdge ETCHED — faisable), combo/list, repaint général sur invalidation, interaction (clic → WM_COMMAND).

### 2026-07-19 — [HLE-WIN32][GUI] **Group box** (cadre étiqueté) : `DrawEdge EDGE_ETCHED` bit-exact + peinture du contrôle
- Dernière primitive de peinture statique courante. **`DrawEdge(EDGE_ETCHED, BF_RECT)`** ajouté à `u32_drawedge` (indices
  **mesurés** : outer TL=BTNSHADOW/BR=BTNHIGHLIGHT, inner TL=BTNHIGHLIGHT/BR=BTNSHADOW = la gravure) — avant, ETCHED
  warn+no-draw ; désormais **bit-exact**. `u32_group_paint` : fond 3DFACE + cadre gravé dont le **haut traverse la ligne du
  label**, label en haut-gauche dessiné **OPAQUE** (fond 3DFACE) pour **couper la bordure** (le cadre étiqueté classique).
  Dispatch BUTTON : +`u32_btn_is_group` (BS_GROUPBOX=7) dans `u32_control_paint_full`.
- **Vérif** : `winecorpus/gdi_drawedge_etched.c` (structurel index-based) → **bit-identique Wine** (`oTL/iTL/oBR/iBR=1`). Rendu :
  dialogue « Settings » avec group box « Display » (cadre + label) contenant 2 checkboxes + label + champ EDIT + bouton →
  **identique à Wine** (capture Xvfb), indiscernable en layout.
- **Portes** : winediff **154→155 fixtures** (gdi_drawedge_etched verte ; seul rouge = `gdi_uifont` env), hash transpile
  `19acad982194bf07` **inchangé**, `table_is_sorted_by_name` ok. **⇒ Jeu de contrôles statiques courants COMPLET** (bouton,
  label, champ, case, group box). **Reste** : radio (cercle, niveau-recherche), combo/list (complexe), **interaction** (clic →
  hit-test → WM_COMMAND, focus, saisie clavier) = prochain grand chantier.

### 2026-07-19 — [HLE-WIN32][GUI] **Radio button** : glyphe 13×13 fixe (bitmap mesuré) bit-exact
- Le radio semblait « niveau-recherche » (cercle) mais c'est un **glyphe 13×13 FIXE** → mesurable et **hardcodable bit-exact**
  (comme la coche), pas un tracé de courbe paramétrique. `u32_draw_radio_glyph` encode le bitmap exact (cercle biseauté
  S/K/H/L + point central WINDOWTEXT si coché) mesuré depuis `DrawFrameControl(DFCS_BUTTONRADIO)`. `aret_DrawFrameControl`
  étendu (radio 13×13) ; `u32_radio_paint` (fond 3DFACE + glyphe + label) ; dispatch `u32_btn_is_radio` (BS_*RADIOBUTTON 4/9).
- **Vérif** : `winecorpus/gdi_framecontrol_radio.c` (structurel) → **bit-identique Wine** (anneau sombre/clair + point coché).
- **Portes** : winediff **155→156 fixtures**, hash `19acad982194bf07` **inchangé**, table triée. **⇒ TOUS les contrôles-boutons
  peints** (push/checkbox/radio/groupbox) + label/champ. Reste peinture : combo/list. Puis **interaction**.

### 2026-07-19 — [HLE-WIN32][GUI] **Interaction dialogue** : clic → hit-test → toggle + `WM_COMMAND` (BN_CLICKED)
- Le dialogue devient **cliquable/utilisable** (plus seulement affiché). `u32_ctrl_click(esp,i)` : applique la sémantique
  **auto** Win32 (BS_AUTOCHECKBOX toggle, BS_AUTO3STATE cycle 0→1→2, BS_AUTORADIOBUTTON sélectionne + décoche les frères du
  dialogue), **re-compose** l'écran, puis notifie le parent par **`WM_COMMAND(MAKEWPARAM(id, BN_CLICKED), hCtrl)`**.
  `u32_control_proc` gère **BM_GETCHECK/BM_SETCHECK/BM_CLICK**. Chemin souris réel : un `WM_LBUTTONUP` livré à un dialogue (pompe
  SDL ou PostMessage) déclenche `u32_dialog_hittest_click` (hit-test des contrôles enfants → clic du bouton sous le curseur) —
  nos contrôles ne sont pas des fenêtres d'entrée séparées, donc le dialogue fait le hit-test que l'entrée par-fenêtre de Wine
  ferait. **Modal ET modeless** couverts (la boucle modale draine SDL + attend l'entrée au lieu d'aborter quand une vraie
  fenêtre existe).
- **Découverte de vérification** : Wine **n'offre aucun oracle headless déterministe** pour le clic — `WM_LBUTTONUP` synthétique
  et **`BM_CLICK`** ne font **rien** sous Wine sans état de capture souris réel / pompe (mesuré). Donc le clic est vérifié
  **qualitativement** (vrai clic **xdotool** sous Xvfb → la case se coche à l'écran, **21 px = la coche Marlett**, +WM_COMMAND
  déterministe côté ARET conforme au spec Win32) et non par winediff. La partie **déterministe vérifiable** = `BM_SETCHECK/
  BM_GETCHECK` round-trip → `winecorpus/user32_bmsetcheck.c` **bit-identique Wine** (`init=0 set1=1 set0=0`).
- **Portes** : winediff **156→157 fixtures** (bmsetcheck verte ; seul rouge `gdi_uifont` env), hash `19acad982194bf07`
  **inchangé**, table triée. **⇒ Dialogue interactif** (clic → toggle + WM_COMMAND). Reste : focus + saisie clavier EDIT,
  repaint général, combo/list, comctl32 peints.

### 2026-07-19 — [HLE-WIN32][GUI] **Focus + saisie clavier EDIT** + fix `SetWindowText/GetWindowText` sur contrôle
- **Saisie clavier** : `SDL_StartTextInput` activé ; la pompe SDL route **`SDL_TEXTINPUT`** vers l'EDIT **focalisé**
  (`u32_edit_key_text`, sous-ensemble ASCII imprimable, sound) et **Backspace** → `u32_edit_key_back`, avec re-composition à
  chaque frappe. **Focus** : `g_u32_focus` posé par défaut sur le 1ᵉʳ EDIT du dialogue (l'app peut `SetFocus`), et sur clic
  (`u32_dialog_hittest_click` focalise l'EDIT/bouton cliqué). Vérif **qualitative** (xdotool tape « Alice » → s'affiche dans le
  champ **et** `GetDlgItemTextA` le rend).
- **Bug général corrigé** : `SetWindowText/GetWindowText/GetWindowTextLength` renvoyaient **0 sur un contrôle** (pas de WNDPROC
  app) → un EDIT créé par `CreateWindowEx` ne stockait/rendait pas son texte (Wine, lui, le fait via DefWindowProc). Fix : repli
  sur `u32_defproc_text` (lit/écrit le titre) quand pas de wndproc + **re-composition** du dialogue (chemin `SetDlgItemText` →
  l'écran se met à jour). Trouvé en testant le focus.
- **Vérif déterministe** : `winecorpus/user32_ctrl_focus_text.c` (SetWindowText/GetWindowText/GetWindowTextLength sur EDIT +
  SetFocus/GetFocus round-trip) → **bit-identique Wine** (`text=[hi] n=2 len=2 focus_eq=1`). La frappe elle-même = pas d'oracle
  headless Wine (qualitatif).
- **Portes** : winediff **157→158 fixtures** (ctrl_focus_text verte ; seul rouge `gdi_uifont` env), hash `19acad982194bf07`
  **inchangé**, table triée. **⇒ EDIT saisissable, focus fonctionnel.** Reste : repaint général, combo/list, comctl32 peints.

### 2026-07-19 — [HLE-WIN32][GUI] **Repaint général** : les API de changement d'état re-composent le dialogue
- Complète l'interaction : une API standard qui **change un contrôle** met à jour l'écran **immédiatement** (comme Windows
  invalide+repeint). `u32_ctrl_recomposite` ajouté à `CheckDlgButton`, `SetDlgItemTextA/W`, `SetDlgItemInt` (en plus de
  `SetWindowText`/`BM_SETCHECK`/clic/frappe déjà couverts + `UpdateWindow`). Un dialogue reflète donc en direct : cases cochées,
  textes de champs/labels, valeurs numériques — sans attendre un `UpdateWindow` explicite.
- Purement **display** (re-composition du framebuffer, no-op sans SDL / hors dialogue) → **aucun changement de sortie
  déterministe**. Portes : winediff **158/158 fixtures inchangé** (seul rouge `gdi_uifont` env), hash `19acad982194bf07`
  **inchangé**. Vérifié par les démos interactives (toggle case, saisie champ = même mécanisme). **Reste** : combo/list, comctl32 peints.

### 2026-07-19 — [HLE-WIN32][GUI] **LISTBOX + COMBOBOX** : modèle d'items (bit-exact) + peinture
- **Modèle d'items** (liste de chaînes sur le tas, champs `items/item_count/item_cap/cur_sel` par-fenêtre) partagé LB/CB :
  `LB_ADDSTRING/INSERTSTRING/DELETESTRING/RESETCONTENT/SETCURSEL/GETCURSEL/GETTEXT/GETTEXTLEN/GETCOUNT` et les jumeaux **CB_***
  (mêmes opérations, opcodes différents). `cur_sel` init à **-1** (LB_ERR). **Bit-exact vs Wine** :
  `winecorpus/user32_listbox.c` (`LB count=4 cursel=2 text2=[Banana] len1=7`, afterdel `[Apricot]`, `CB … [Green]`).
- **Peinture** : LISTBOX (`u32_listbox_paint`) = fond COLOR_WINDOW + items + **ligne sélectionnée COLOR_HIGHLIGHT/HIGHLIGHTTEXT**
  (index-based ; bord creusé au composite). COMBOBOX fermé (`u32_combobox_paint`) = champ blanc (texte de la sélection) + **bouton
  flèche déroulante** (DrawEdge RAISED + triangle) ; zone sous le champ = fond dialogue. `u32_paint_text` généralisé (texte
  arbitraire, pas que le titre). Vérif **qualitative** (capture Xvfb : liste 4 items « Angelfish » surligné + combo « Freshwater »
  = layout identique Wine ; surlignage bleu classique vs bleu moderne, même index). Piège corrigé au passage : `u32_window_create`
  ne zérotait pas `is_dialog/du_x/du_y/dlg_font/items` (slot réutilisé → état fantôme) — désormais initialisés.
- **Portes** : winediff **158→159 fixtures** (user32_listbox verte ; seul rouge `gdi_uifont` env), hash `19acad982194bf07`
  **inchangé**, table triée. **⇒ Tous les contrôles de dialogue courants peints** (bouton/label/champ/case/radio/group/list/combo).
  Reste GUI : contrôles **comctl32** peints à l'écran (progress bar liftée → GDI→framebuffer).

### 2026-07-19 — [HLE-WIN32][GUI][LOADER] Contrôles **comctl32 peints** : démontré, mais **gated sur le socle Levier 1** (mesuré)
- **But** : faire peindre à l'écran les contrôles comctl32 (progress bar…) — leur *logique* est déjà liftée+prouvée (doc 70 §5.0).
- **Mesure décisive** : une **progress bar comctl32 liftée PEINT bien** via `WM_PRINTCLIENT` sous ARET (style **classique** : fond
  `000080`=COLOR_HIGHLIGHT + `c0c0c0`=3DFACE, dessiné par le code comctl32 lifté sur notre GDI HLE — preuve que le chemin
  lift→GDI→pixels marche). **MAIS** le binaire est **INCOMPLETE** : **115 imports non implémentés** (`CharLowerBuffW`/
  `CharUpperBuffW`/… = les **~106 shims socle** kernel32/CRT du Levier 1, doc 70 §5.0). ⇒ faire peindre les comctl32 **en vrai**
  (composite écran) = **finir le socle DLL-lifting** (106 shims) + alignement thème (classique vs Wine natif themed) + **intégrer
  au composite** (appeler le WNDPROC lifté avec un `esp` valide pendant la composition — non threadé aujourd'hui). C'est un
  **chantier Levier 1 dédié** (multi-sessions), **pas** un incrément de contrôle user32.
- **Conclusion** : le **chantier GUI *user32* (dialogues + contrôles standard) est COMPLET** (bouton/checkbox/radio/groupbox/
  static/edit/listbox/combobox peints + interaction clic/focus/clavier + repaint). Les **contrôles comctl32 peints** relèvent du
  **Levier 1** (lifting DLL) et restent l'item avancé suivant, borné et mesuré (socle 106 shims).

### 2026-07-19 — [HLE-WIN32][LOADER] **Socle comctl32 (Levier 1) — batch 1** (mesuré `--mode walls`, 115→102)
- **Levier 0 appliqué à comctl32** : `--mode walls` sur `comctl32.dll` lifté (+ progress/trackbar/updown) → liste **mesurée**
  des **115 imports socle** manquants (`CharLowerBuffW`, `CompareFileTime`, `IsChild`, clipboard, caret, regions, Script*, IMM*,
  monitors…). On les traite **par la donnée**, du plus simple/large au plus lourd.
- **Batch 1 livré** (simples, larges, vérifiables bit-exact) : `CharLowerBuffW`/`CharUpperBuffW` (fold ASCII), `CompareFileTime`
  (−1/0/1 sur FILETIME 64-bit), `GetDoubleClickTime` (500, mesuré Wine), `IsChild` (chaîne parent WS_CHILD), `GetObjectType`
  (GDIT_*→OBJ_*), `GetBkMode`, `StrCmpIW`/`StrCmpNIW` (fold ASCII ordinal, comme notre wcsicmp prouvé), `MonitorFrom*`/
  `GetMonitorInfoA/W` (moniteur primaire = invariant écran 1024×768), `GetDpiForWindow` (96). Gardé
  `winecorpus/win32_socle_comctl1.c` → **bit-identique Wine** (`cft -1 1 0`, `ischild 1 0`, `objtype brush=2 pen=1`, upper/lower,
  `bkmode 1`, `dclick 500`).
- **Mesure après batch** : comctl32 socle **115 → 102** imports manquants. **Portes** : winediff **159→160 fixtures**, hash
  `19acad982194bf07` **inchangé**, table triée. Suite : batches 2+ (clipboard, caret, regions GDI, blit, clip, Uniscribe/IMM =
  no-op sound), puis intégration composite (esp threadé) → contrôles comctl32 peints.

### 2026-07-19 — [HLE-WIN32][LOADER] **Socle comctl32 batch 2** : clipboard + caret + IMM/no-op (mesuré 102→82)
- Batch 2 : **clipboard en mémoire** (table format→handle : Open/Close/Empty/Set/Get/IsClipboardFormatAvailable — round-trip
  sound, format absent = NULL comme un presse-papier vide), **caret** (position stateful Set/GetCaretPos ; Create/Destroy/Show/
  Hide = no-op display), **IMM** (`ImmGetContext=0`/`ImmReleaseContext=1`/composition = pas d'IME = état correct sur setup
  non-CJK), **divers no-op sound** (`NotifyWinEvent`/`TrackMouseEvent`/`KillSystemTimer`/`GetLayout`=LTR). Gardé
  `winecorpus/win32_socle_comctl2.c` (clipboard round-trip `[Hello]` + caret `7 9`) → **bit-identique Wine**.
- **Mesure** : comctl32 socle **102 → 82** imports manquants. **Portes** : winediff **160→161**, hash `19acad982194bf07`
  **inchangé**, table triée. Suite : batch 3 (régions GDI, clip, blit, char-width, window-nav), puis Uniscribe/deep = abort/no-op
  sound, puis intégration composite (esp) → progress bar peinte à l'écran.

### 2026-07-19 — [HLE-WIN32][LOADER] **Socle comctl32 batch 3** : régions GDI + object/rect/nav (mesuré 82→71)
- **Régions GDI** (modèle **rect englobant** + flag `rgn_complex`) : `CreateRectRgn(Indirect)`, `CreateRoundRectRgn`/
  `CreatePolygonRgn` (bbox, complex), `SetRectRgn`, `GetRgnBox`, **`CombineRgn`** (AND = rect exact ; OR = rect si l'un contient
  l'autre sinon **COMPLEXREGION** avec bbox, **mesuré vs Wine** ; COPY = src1 ; DIFF/XOR = bbox complex). Le **type**
  NULL/SIMPLE/COMPLEX retourné est correct. Sous-ensemble rectangulaire **exact** ; non-rect = bbox (superset pour invalidation
  grossière, documenté, jamais une donnée fausse).
- **Divers** : `GetCurrentObject` (objet sélectionné du DC), `SetPolyFillMode` (no-op, Polygon=abort), `SubtractRect`
  (sémantique Windows : rect si le résultat en est un, sinon src1), `GetNextDlgTabItem`/`GetNextDlgGroupItem` (contrôle
  suivant/précédent en ordre z, wrap, skip non-tabstop). Gardé `winecorpus/win32_socle_comctl3.c` → **bit-identique Wine**
  (régions AND/OR/COPY + types, SubtractRect).
- **Mesure** : comctl32 socle **82 → 71**. Portes : hash `19acad982194bf07` **inchangé**, table triée. (Reste : clip appliqué,
  blit, char-widths FreeType, icônes, locale, Uniscribe=abort/deep, puis intégration composite esp → progress bar à l'écran.)

### 2026-07-21 — [HLE-WIN32][LOADER] **Socle comctl32 batch 4** : nav fenêtres + ressources + no-ops (mesuré 71→50)
- **Navigation fenêtres** : `SetParent` (reparente, retourne l'ancien parent, `GetParent` reflète le déplacement),
  `EnumChildWindows` (rappel du callback par enfant via frame `aret_call`), `ChildWindowFromPoint`/`WindowFromPoint`
  (hit-test point→enfant), `GetNextDlgTabItem`-adjacents déjà couverts. `GetDCEx`→`GetDC`, `SetSystemTimer`→`SetTimer`.
- **Ressources** : `LoadStringW` (copie RT_STRING large), `FindResourceW`→`FindResourceA`, `InternalGetWindowText`.
- **Constantes/no-ops sound** : `IsValidLocale=1`, `GetNearestColor`=couleur telle quelle, `SelectPalette`=0,
  `GdiGetCodePage=1252`, `DragDetect=0`, `ShowScrollBar=1`, `ScrollWindow(Ex)`, `GetClassLongW=0`, `GetKeyNameTextW`
  (buffer vidé, 0), `MapVirtualKeyW=0`, `GetTextCharsetInfo=0`. 21 shims, 21 entrées `stdcall_pops` (triées).
- **Mesure** : comctl32 socle **71 → 50** imports manquants. Gardé `winecorpus/win32_socle_comctl4.c` (EnumChildWindows +
  SetParent + ChildWindowFromPoint) → **bit-identique Wine** (`enum=2`, `setparent old_is_p1=1 newparent_is_p2=1`,
  `childfrompt_is_c2=1`). **Portes** : winediff **162→163**, hash `19acad982194bf07` **inchangé**, table triée. Reste 50 :
  clip appliqué (batch 5), blit/pen/char-widths FreeType (6), icônes/curseur (7), Uniscribe `Script*`/locale/divers (8).

### 2026-07-21 — [HLE-WIN32][LOADER] **Socle comctl32 batch 5** : clip DC + régions fenêtre + FillRgn/FrameRgn (mesuré 50→39)
- **Clip DC** (bbox rect + flag complex + flag « set ») : `IntersectClipRect` (rect ∩ rect = **SIMPLE**), `ExcludeClipRect`
  (bande = rect, coupe intérieure = **COMPLEX**, couverture totale = **NULL**), `SelectClipRgn(NULL)` = **SIMPLE** + retire le
  clip, `SelectClipRgn(hrgn)`/`ExtSelectClipRgn` (AND/OR/COPY), `GetClipRgn` (0 = aucun / 1 = box copiée), `RectVisible`
  (intersection ; sans clip = toujours visible). **Types NULL/SIMPLE/COMPLEX mesurés bit-exact vs Wine** (probe : `isect=2`,
  `exclude=3`, `selectnull=2`, `getclip 0/1`).
- **Régions fenêtre** : `SetWindowRgn`/`GetWindowRgn` (round-trip bbox + type, ERROR=0 si aucune).
- **FillRgn/FrameRgn** : peignent la **région rectangulaire exactement** (couleur pinceau dans le DIB) ; une région **complexe
  aborte loud** (`aret_unimpl`) plutôt que peindre son bbox (jamais un pixel faux). `ExtCreateRegion` (RGNDATA, transform NULL :
  bbox `rcBound`, complex si >1 rect ; transform ≠ NULL = abort).
- **Mesure** : comctl32 socle **50 → 39**. Gardé `winecorpus/win32_socle_comctl5.c` (clip 9 lignes + FillRgn/FrameRgn GetPixel +
  window-rgn) → **bit-identique Wine**. **Portes** : winediff **163→164**, hash `19acad982194bf07` **inchangé**, table triée.
  Reste 39 : blit/pen/char-widths FreeType (batch 6), icônes/curseur (7), Uniscribe `Script*`/locale/divers (8).

### 2026-07-21 — [HLE-WIN32][LOADER] **Socle comctl32 batch 6a** : métriques par caractère FreeType (mesuré 39→34)
- Partagent **le chemin de police DC exact** de `GetTextExtentPoint32` (déjà bit-identique à Wine) → les avances concordent
  par construction. `GetCharWidthW`/`A` (avance par glyphe), `GetCharABCWidthsW` (A = bearing gauche, B = boîte noire, C =
  avance-A-B, `A+B+C = avance`), `GetTextExtentExPointW`/`A` (extent plein + `lpnFit` = nb qui tient dans `maxExtent` + tableau
  `dx` cumulatif ; `SIZE` = extent de **toute** la chaîne, mesuré vs Wine), `GdiGetCharDimensions` (helper interne largeur
  moyenne, alphabet 52 lettres — réutilisé par le calcul base-units DLU). `GetCharWidthInfo` (ntgdi interne : pas de bearings
  spéciaux dans notre modèle → zéros, `TRUE`).
- **Mesure** : comctl32 socle **39 → 34**. Gardé `winecorpus/win32_socle_comctl6.c` (DejaVu Sans : `cw`, `abc`, `extex` avec
  fit) → **bit-identique Wine** (`cw 11 11 11 12`, `abc[B]=1,9,1`, `extex fit=4 cx=45 dx=11,22,33,45`, `fit20 fit=1 cx=45`).
  Même mise en garde résolution de police que les autres fixtures texte / `gdi_uifont`. **Portes** : winediff **164→165**, hash
  `19acad982194bf07` **inchangé**, table triée. Reste 34 : pens/blit/DIB (batch 7), icônes/curseur (8), Uniscribe/locale/divers (9).

### 2026-07-21 — [HLE-WIN32][LOADER] **Socle comctl32 batch 7** : StretchBlt + SetDIBits + GdiAlphaBlend + pens (mesuré 34→27)
- Réutilisent le **modèle DIB 32bpp éprouvé** (`gdi_px`/`gdi_put`/`gdi_rop_apply`). Sémantiques **mesurées bit-exact vs Wine**
  (probe GetPixel) : `StretchBlt` **plus-proche-voisin** (`src = s0 + i*sw/dw` ; extents négatifs/miroir = abort sound),
  `SetDIBits` **bottom-up** (ligne image `y = H-1-scan`, BI_RGB 24/32bpp ; compressé = abort), `GdiAlphaBlend`
  `out = (src*ca + dst*(255-ca))/255` (alpha constant ; `AC_SRC_ALPHA` = premultiplié par-pixel). Pens : `CreatePenIndirect`
  (LOGPEN), `ExtCreatePen` (style+largeur, couleur du LOGBRUSH). `GetDIBColorTable`/`SetDIBColorTable` = 0 (pas de palette en
  32bpp, sound).
- **Mesure** : comctl32 socle **34 → 27**. Gardé `winecorpus/win32_socle_comctl7.c` (StretchBlt 2×2→4×4, SetDIBits bottom-up,
  GdiAlphaBlend const 128) → **bit-identique Wine** (`stretch (0,0)=0000FF (1,1)=0000FF`, `setdib (0,0)=CC0000 (0,1)=0000AA`,
  `alpha const128 = 000080`). **Portes** : winediff **165→166**, hash `19acad982194bf07` **inchangé**, table triée. Reste 27 :
  icônes/curseur (batch 8), Uniscribe `Script*`/locale/divers (batch 9).

### 2026-07-21 — [HLE-WIN32][LOADER] **Socle comctl32 batch 8** : icônes + curseurs + DrawState (mesuré 27→18)
- Les icônes ressource (`LoadIcon`) restent des **jetons opaques** sans forme rasterisée. `CreateIconIndirect` garde les bitmaps
  couleur/masque de l'appelant dans une table → `GetIconInfo` **round-trip**, en reproduisant la règle Wine : sur une **ICÔNE**,
  le hotspot rapporté = **centre** du bitmap (`cx/2,cy/2`), pas celui stocké (seul un CURSEUR le garde), et retourne des **copies
  fraîches** des bitmaps (dims correctes). `CopyImage` **deep-copy** un bitmap (handle distinct, dims/bpp préservés, redimensionné
  si taille donnée) ; `CopyIcon` = icône distincte ; `DestroyIcon`/`DestroyCursor` libèrent.
- **Dessin** (`DrawIcon`/`DrawIconEx`/`DrawStateW`) : blit du bitmap couleur si présent, sinon **no-op sound** (un jeton opaque n'a
  pas de pixels). `DrawStateW` peint les types tractables (texte/bitmap/icône) via les primitives GDI ; `DST_COMPLEX` (callback) =
  abort. Pas d'oracle headless pour le dessin → vérifié qualitativement (comme le reste de la peinture écran).
- **Mesure** : comctl32 socle **27 → 18**. Gardé `winecorpus/win32_socle_comctl8.c` (CreateIconIndirect→GetIconInfo hotspot
  centré + dims, CopyImage distinct+resize, CopyIcon) → **bit-identique Wine** (`hotx=8 hoty=8`, `color 16x16`,
  `copy 4x4 bpp=32`, `copy8 8x8`, `copyicon distinct=1`). **Portes** : winediff **166→167**, hash `19acad982194bf07`
  **inchangé**, table triée. Reste 18 : Uniscribe `Script*` (10, abort/failure sound → fallback appelant), locale/menu/divers (8).

### 2026-07-21 — [HLE-WIN32][LOADER] **Socle comctl32 batch 9 (final)** : locale/date + polygones + heap + Uniscribe (18→**0**)
- **Dernier lot** du socle comctl32. `GetDateFormatW` : parseur de **picture explicite** (`yyyy`/`yy`/`MM`/`M`/`dd`/`d` +
  littéraux + `'quote'`) — champs numériques **bit-exact vs Wine** (`yyyy-MM-dd`→`2024-03-07`, `yy/M/d`→`24/3/7`) ; noms de
  mois/jours en anglais best-effort (mise en garde résolution locale, comme `gdi_uifont`). `GetLocaleInfoW` : défauts en-US
  (dates/AM-PM/noms). `LocalSize` = `malloc_usable_size` (sound). `LoadMenuA`/`PlayEnhMetaFile` = 0 (sound). `PolyPolyline`
  = **contour pur** via la primitive Bresenham déjà DIB-hash-exacte (**vérifié** `pphash=6c583cc5` vs Wine). ⚠️ **`Polygon`
  = ABORT sound** (jamais un remplissage deviné) : un remplissage pair-impair naïf a été **MESURÉ divergent** de Wine
  (DIB `fd1adec5` vs `70a8e185`) → contraire au principe sacré et à la ligne « Polygon = abort » du GDI vectoriel ; reste
  abort tant que le remplissage n'est pas reproduit bit-exact. **CRT** : `floor` (`MATH1`, canal x87),
  `__stdio_common_vsprintf` (délègue à `aret_vformat`).
- **Uniscribe** `Script*` (9) : moteur de shaping **hors périmètre** → chaque appel retourne un **échec sound** (`E_FAIL` /
  `NULL` / `pssa=NULL`) pour qu'un comctl32 lifté prenne son chemin **texte GDI non-USP** (rendu bit-exact par ARET). Pas
  d'oracle standalone (Wine implémente Uniscribe → succès) : vérifié uniquement *in situ* via l'intégration comctl32 lifté.
- **Mesure** : comctl32 socle **18 → 0** — le socle Levier-1 est **complet** (les 71 imports mesurés au départ sont tous
  couverts). Restent 2 *unresolved direct calls* (gaps de **récupération de code** dans le comctl32 lifté — catégorie distincte
  du socle HLE). Gardé `winecorpus/win32_socle_comctl9.c` (GetDateFormatW numérique + LocalSize) → **bit-identique Wine**.
  **Portes** : winediff **167→168**, hash `19acad982194bf07` **inchangé**, table triée. **Suite** : intégration composite (esp
  threadé) → peindre un contrôle comctl32 lifté (progress bar) à l'écran, exerçant floor/vsprintf/Script*/blit in situ.

### 2026-07-21 — [HLE-WIN32] **Durcissement socle : 5 no-op/constantes « plausibles » → comportement RÉEL mesuré** (audit utilisateur)
- Suite de l'auto-audit (après le fix `Polygon`) : l'utilisateur a demandé de ne **plus se contenter** de no-op/constantes non
  prouvés. Chacun **mesuré vs Wine** puis implémenté bit-exact (fixture `winecorpus/win32_socle_harden.c`) :
  - **`IsValidLocale`** ne renvoie plus **toujours 1** : valide **ssi** l'id de langue primaire ∈ plage assignée MS-LCID
    `[0x01,0x92]` (mesuré : `0x09`/`0x0C`/`0x50`/`0x91`/`0x92` valides ; `0xA0`/`0xFF`/`0x350` invalides), `0x0400`/`0x0800`
    (USER/SYSTEM_DEFAULT) rejetés comme Wine. Bord hors-scope documenté : locales custom `0x0C00`, trous non assignés dans la plage.
  - **`MapVirtualKey(A/W)`** : **vraie table de scan-codes clavier US** (set-1) — VK↔VSC + VK→CHAR (mesuré `A→0x1E`, `RET→0x1C`,
    `SHIFT→0x2A`, `F1→0x3B`, `A→'A'`, `0x1E→'A'`).
  - **`ShowScrollBar`** : bascule réellement le bit de style **WS_HSCROLL/WS_VSCROLL** (mesuré via `GetWindowLong(GWL_STYLE)`).
  - **`GetClassLong(A/W)`** : renvoie les **vrais champs** de la classe enregistrée (fenêtre→registre de classes ;
    style/wndproc/cbCls/cbWnd/hbr/hicon/hcursor/hmod/menu/hIconSm). Octets class-extra (index positif) = **abort sound** (pas un 0 devin).
  - **`ScrollWindow(Ex)`** : calcule la **bande d'invalidation exposée** exacte (`prcUpdate` rempli — mesuré sur un enfant où
    client==window : `dx10→0,0,10,80`, `dy-15→0,65,120,80`, L-shape→bbox) + déplace le framebuffer client (scroll réel,
    qualitatif SDL) + invalide. Retour SIMPLEREGION(2)/TRUE comme Wine.
- **Restant honnête** (documenté, pas maquillé) : les **noms** de mois/jours de `GetDateFormatW`/`GetLocaleInfoW` et les défauts
  `GetLocaleInfoW` restent **en-US best-effort** (dépendance locale = même mise en garde environnementale que `gdi_uifont` ; les
  champs **numériques** sont bit-exact). `PlayEnhMetaFile`=0 (rejoue EMF = à implémenter si un binaire l'exige). `Polygon` reste
  **abort sound** (remplissage non reproduit bit-exact). **Portes** : winediff **168→169**, hash `19acad982194bf07` **inchangé**,
  difftest 272/272, table triée, socle comctl32 **toujours 0**. **Suite** : intégration in situ (contrôle comctl32 lifté à l'écran).

### 2026-07-24 — [LIFT][RECOV] **Bug MISCOMPILE : appel indirect d'import → mauvais shim (reset held par bloc)**
- **Découvert en poussant l'intégration GUI** (faire peindre un contrôle comctl32 lifté à l'écran) : un vrai binaire fenêtré
  (`mv_A`, fenêtre user32 pure) **crashait** (`0xC0000005`) là où Wine tourne. Diagnostic au débogueur (`-O0 -g` + gdb, recette
  doc 70 §7) : `chunk_0.c` store vers `v183` invalide ; `v183` dérivé d'un appel dont **la boucle de messages appelait
  `aret_GetModuleHandleA` au lieu de `aret_PeekMessageA`** (`aret_PeekMessageA` apparaissait **0 fois**).
- **Cause racine** (`src/ir/build.rs`, passe de nommage des appels indirects d'imports) : la map `held` (registre→import, pour
  `mov reg,[IAT]; call reg`) était threadée **linéairement à travers tous les blocs en ordre de STOCKAGE** (≠ ordre
  d'exécution/dataflow), **sans reset aux frontières de bloc ni fusion aux jointures**. Un mapping périmé `reg→importA` d'un bloc
  antérieur **fuyait** dans un bloc ultérieur qui chargeait `importB` dans ce registre → `call reg` mal résolu.
- **Fix** : **reset de `held` par bloc** — scan avant en ligne droite, valable **seulement dans un bloc** (où il est fiable). Un
  appel d'import tenu-en-registre qui **franchit une frontière de bloc** reste **indirect** : au runtime il dispatche via le
  **jeton-auto du slot IAT** (bon shim) — sound, juste pas nommé statiquement. **Portes** : difftest **272/272**, hash
  `19acad982194bf07` **inchangé** (chirurgical — les fonctions de référence n'ont pas le motif), funcdiff **0 divergence**
  (20558 scorées), winediff **168/169** (seul `gdi_uifont` env). Vraie correction de justesse **générale** (tout
  `mov reg,[IAT]; call reg` inter-bloc), sortie par l'intégration. *(Fausse piste écartée : un « fix de pop » ajoutant les slots
  IAT à `__aret_callee_pop` **double-poppait** les `call [iat]` (déjà poppés par la passe per-insn) et cassait `gdi_drawtext` —
  reverté ; le reset par-bloc seul suffit, le retaddr des indirects s'équilibre déjà.)*

### 2026-07-24 — [GUI][HLE-WIN32] **Pipeline visuel prouvé : fenêtre Win32 peinte à l'écran X (+ fond par défaut)**
- **Environnement de capture débloqué** (le redémarrage conteneur avait cassé l'affichage SDL) : (1) **renderer SDL software**
  obligatoire — le défaut `opengl` se crée mais **ne s'affiche pas sur Xvfb** (pas de GPU/GLX) ⇒ `SDL_RENDER_DRIVER=software` ;
  (2) **capturer la RACINE** (`import -window root`) — une fenêtre SDL sans window-manager ne se capture pas individuellement
  (fond noir). Diagnostiqué par un test SDL natif minimal : `renderer name: opengl` → noir ; software → la racine montre les
  pixels. ⇒ **une fenêtre Win32 d'un PE transpilé en ELF natif peint réellement à l'écran X** (fenêtre bleue `custom WM_PAINT`
  vérifiée `srgb(20,110,210)`).
- **Fix `DefWindowProc(WM_PAINT)` = efface le fond de classe** (`u32_erase_window_client`) : le vrai DefWindowProc fait
  BeginPaint/EndPaint → efface (WM_ERASEBKGND) ; le nôtre ne faisait que valider → une fenêtre **sans handler de peinture**
  affichait un client **noir**. Corrigé → elle montre son fond (COLOR_BTNFACE = gris 192, vérifié à l'écran). Runtime-only (hash
  inchangé), winediff **168/169** (les fixtures à WM_PAINT custom ne passent pas par ce chemin ⇒ pas de régression).
- **RESTE pour la progress bar** : un **contrôle enfant** comctl32 ne **compose pas** ses pixels dans le framebuffer du parent
  (ce chemin existe pour les **dialogues** via `u32_dialog_composite`, pas pour un parent+enfant simple). Prochain morceau :
  router le DC de l'enfant vers le framebuffer de l'**ancêtre** avec offset de viewport + clip, puis présenter l'ancêtre.

### 2026-07-24 — [HANDOFF] **Note pour le successeur (si compression de contexte)**
- **Reprise** : relis le **doc 70 EN ENTIER** et le **doc 80 EN ENTIER** ; relis les **dernières entrées du doc 71** + les
  **derniers commits** ; **énumère toutes les règles de travail** (principe sacré §0, doctrine §1, méthode §2, doc 80 §3 — elles
  sont et resteront **incontournables**) ; **fais le point**, puis **poursuis**.
- **État** : socle comctl32 **complet** (71→0) + durcissement ; **bug 2 (miscompile indirect-import) corrigé** ; pipeline visuel
  **prouvé** (fenêtre peinte à l'écran, fond par défaut). **Portes vertes** : difftest 272/272, hash `19acad982194bf07`, funcdiff
  0 div, winediff 168/169 (`gdi_uifont` env). Tout est **poussé** sur `claude/zen-hamilton-6pi1k4`.
- **Suite ordonnée (demandée par l'utilisateur)** : (1) **composite enfant** (progress bar comctl32 à l'écran — DC enfant→
  framebuffer ancêtre, offset+clip, vérifié vs Wine) ; (2) **FishTank.exe** (dialogue à contrôles natifs — le composite de
  dialogue existe déjà, donc mesurer d'abord ce qui s'affiche avant de coder). ⚠️ **Toujours committer AVANT les vérifs
  longues** (winediff/funcdiff) — le conteneur est éphémère (leçon vécue : un durcissement non-commité perdu au reset).
  ⚠️ Capture GUI = **renderer software + capture racine** ; échantillonner un pixel **dans** la fenêtre (elle est à `100,100`).

### 2026-07-24 — [GUI][DEMO] **FishTank.exe (MFC 1996) : dialogue natif peint à l'écran ; écart = frontière MFC (mesuré)**
- **Test visuel du vrai binaire shippé** (aquarium-calculateur Win95, `refetch/ft/`) : transpilé en ELF natif, lancé sous Xvfb
  (renderer **software**, capture **racine**), comparé à la référence Wine (`ft_wine.png`). **L'ELF ARET affiche le dialogue
  COMPLET, structurellement identique à Wine** : tous les group boxes (`You Provide:`/`Tank Geometry:`/`Calculation Results:`),
  labels + accélérateurs soulignés, 4 boutons (Exit/Calculate/Help/About), cadres combos (flèche)/radios/edits — au pixel — via
  `u32_dialog_composite`. **Log d'exécution VIDE** : 0 abort, 0 import manquant touché au démarrage, **24 contrôles enfants créés**,
  `WM_INITDIALOG` dispatché (`CreateDialogIndirectParamA` modeless).
- **Seul écart = le CONTENU/ÉTAT des contrôles** (combos vides vs `Tropical Freshwater`/`Custom...`, radios sans point, edits sans
  `0`). **Cause MESURÉE** (pas devinée, via trace `getenv`-gatée + rebuild) : après `WM_INITDIALOG`, l'appli fait **0 `GetDlgItem`,
  0 `SendMessage`** → son `OnInitDialog` (les `AddString`/`SetCheck`/DDX qui peuplent) **ne s'exécute pas**. FishTank est **MFC**
  (`CDialog`) : cette machinerie vit derrière les message maps + mapping `CWnd`↔`HWND`, dépendant du **runtime C++/`__CxxFrameHandler`**.
  ⇒ **frontière MFC** (doc 70 §5.0 étape 4, gated EH C++), **pas** le composite enfant ni le dispatch HLE des contrôles.
- **Fausse piste écartée par la mesure** : 1ʳᵉ hypothèse « `SendDlgItemMessage` ne route pas `CB_*`/`BM_*` vers le contrôle
  système » (réelle incohérence : `SendMessage` les gère via `u32_control_proc`, `SendDlgItemMessage` défère à `u32_defproc_text`
  qui ne connaît que `WM_*TEXT`). Un correctif de dispatch unifié a été écrit **puis révoqué** : la trace prouve que FishTank
  **n'appelle jamais** ces chemins au démarrage → aucun bénéfice mesuré (règle §2 : pas de changement sans bénéfice mesuré). À
  reprendre **avec une fixture dédiée** (dialogue non-MFC peuplant un combo via `SendDlgItemMessage`, round-trip vs Wine) si on
  veut fermer l'incohérence proprement.
- ⚠️ **Piège outillage attrapé** : `runtime/aret_hle/aret_win32.c` est **embarqué dans le binaire `aret` par `include_str!`** →
  tout changement du runtime C **n'a d'effet qu'après `cargo build --release`**. Deux mesures initiales faites sur binaire périmé
  (conclusions invalidées, refaites). **Toujours `cargo build --release` avant de re-transpiler quand on touche le runtime C.**
- **Portes** : inchangées (le test est en lecture seule, aucun code committé ; tree = origin). **Suite** : composite enfant
  (progress bar comctl32) OU fixture-preuve du fix dispatch OU ouvrir l'EH C++ (MFC) — au choix utilisateur.

### 2026-07-24 — [GUI][HLE-WIN32] **Composite parent→enfant généralisé : un contrôle comctl32 LIFTÉ peint à l'écran**
- **But (suite ordonnée, étape #1)** : faire peindre à l'écran un **contrôle enfant comctl32 lifté** (progress bar), pas seulement
  les contrôles système d'un dialogue. Le composite existant (`u32_dialog_composite`) ne peignait QUE les classes système
  peintes bit-exact (button/edit/…) et QUE pour un `is_dialog` ; un contrôle avec son **propre WNDPROC** (comctl32 lifté) et une
  **fenêtre simple** (non-dialogue) étaient laissés en fond.
- **Méthode §2** : (a) **fixture visuelle minimale** `pbwin` (fenêtre simple + progress bar enfant `WS_CHILD|WS_VISIBLE` à 60 %,
  `--with-dll comctl32`) ; (b) **baseline mesurée** : la fenêtre peint son fond gris `COLOR_BTNFACE` (28600 px) mais **la progress
  bar est absente** (0 px), vs Wine qui montre le bar bleu à 60 % ; (c) implémenté ; (d) **re-mesuré** : bar peinte (2310 px
  `srgb(0,0,128)` navy ≈ 60 % de 200×24 — le rendu **classique authentique** de la comctl32 liftée, chemin uxtheme→classique).
- **Implémentation** (généralisation, `aret_win32.c`) : un contrôle enfant reçoit **son propre framebuffer client**
  (`u32_ensure_child_bmp`) ; le composite **pilote son `WM_PAINT`** (esp threadé — le contrôle Begin/EndPaint dedans, comme une
  top-level) puis **blitte** son framebuffer à l'offset enfant avec clip (`u32_blit_clip`). `u32_present_toplevel` unifie la
  présentation (dialogue = refill 3DFACE + enfants ; fenêtre simple = enfants **par-dessus** sa propre peinture), câblé dans
  `UpdateWindow`/`EndPaint`/`ReleaseDC`/`ShowWindow`. Un contrôle **système** (sans WNDPROC) garde son chemin bit-exact
  (`u32_control_paint_full`). Framebuffers enfants **libérés à la destruction** (`u32_free_child_bmp`, DestroyWindow + dialog).
  `esp==0` (chemin sans pile machine, ex. input SDL) saute le pilotage d'un contrôle lifté ce tour (sound : peint au prochain
  présent porteur d'esp). **Vérité de composition** : l'état du contrôle est déjà bit-identique Wine (fixture headless
  `comctl32_progress`, `pos=60`) ; le lift du contrôle est prouvé (cpudiff/funcdiff) ; le composite ajoute **le rendu à l'écran** à
  l'offset/taille mesurés — vérifié qualitativement (Xvfb), même caveat que le dialog composite (Wine n'expose pas ses enfants
  composités en DIB). L'écart avec Wine = **thème** (classique segmenté vs Wine thémé plat car Wine charge uxtheme), pas géométrie.
- **CRT** : `_ismbblead`/`_ismbbtrail` = 0 (locale mono-octet, comme `IsDBCSLeadByte`) — touchés par la peinture de la progress bar.
- **Portes** : difftest **272/272**, hash transpile `19acad982194bf07` **inchangé**, winediff **168/169** (1 rouge = `gdi_uifont`,
  environnemental fontconfig i386 = le rouge attendu de référence ; les fixtures GUI/paint restent vertes). Committé + poussé.
- ⚠️ **Bug pré-existant orthogonal noté** (tâche suivi) : une boucle `PeekMessage` en spin serré (500000×, sans `GetMessage`
  bloquant) crashe par corruption (surface dans `pump_timers`→`mono_ns`). **Reproduit avec le composite entièrement neutralisé**
  ⇒ indépendant du compositing. Les vraies applis GUI utilisent `GetMessage` (bloquant), qui marche. À investiguer (pop/esp du
  chemin PeekMessage).

### 2026-07-24 — [HLE-WIN32] **Dispatch unifié des contrôles système (SendMessage ET SendDlgItemMessage) — prouvé par fixture**
- **Étape #2** (reprise du correctif écarté au diagnostic FishTank, cette fois **avec bénéfice mesuré**). Incohérence réelle : un
  contrôle prédéfini **sans WNDPROC applicatif** doit répondre à la fois à ses **messages de classe** (`CB_ADDSTRING`/`CB_SETCURSEL`,
  `BM_SETCHECK`, `WM_SETFONT`…) **et** aux **messages texte** communs (`WM_SETTEXT`/`WM_GETTEXT`), quel que soit le point d'entrée.
  Avant : `SendMessage` routait **seulement** vers `u32_control_proc` (classe, pas texte) et `SendDlgItemMessage` **seulement** vers
  `u32_defproc_text` (texte, pas classe) — chacun manquait ce que l'autre avait → un dialogue peuplant un combo via
  `SendDlgItemMessage(CB_ADDSTRING)` ou cochant via `BM_SETCHECK` les **perdait en silence**.
- **Fix** : `u32_sys_control_msg` (classe **puis** texte, largeur ANSI/UTF-16 correcte par point d'entrée), branché sur les 4 entrées
  (`SendMessage A/W`, `SendDlgItemMessage A/W`). **Méthode §2** : fixture `winecorpus/user32_dlgitemmsg` (parent + enfants
  COMBOBOX/BUTTON/EDIT peuplés via `SendDlgItemMessage` **et** `SendMessage(GetDlgItem)`). **Baseline mesurée** ARET =
  `count=0 sel=0 text= chk=0 edit=` (cassé) ; Wine = `count=3 sel=1 text=Bravo chk=1 edit=hi`. **Après fix** : ARET =
  `count=3 sel=1 text=Bravo chk=1 edit=hi` **bit-identique Wine**. (Le combo nécessite un parent display-backé → fixture sous le
  Xvfb partagé du harnais, comme les autres fixtures fenêtrées ; round-trip de messages **déterministe**, pas de pixels.)
- **Portes** : difftest **272/272**, hash transpile `19acad982194bf07` **inchangé**, winediff **+1 fixture** (le rouge reste
  `gdi_uifont` environnemental). Committé + poussé.

### 2026-07-24 — [GUI][HLE-WIN32] **Frontière MFC (#3) : premier blocage diagnostiqué = hook WH_CBT non déclenché**
- **Diagnostic (avant d'implémenter)** de pourquoi l'`OnInitDialog` de FishTank (MFC) ne s'exécute pas (mesuré : 0 `GetDlgItem`,
  0 `SendMessage` après `WM_INITDIALOG`). **Cause pinée** : `SetWindowsHookExA/W` est un **stub** (rend un handle opaque, le hook
  **ne se déclenche jamais**). MFC attache ses objets C++ `CWnd` aux `HWND` **et les subclasse** via un hook **`WH_CBT`**
  (`HCBT_CREATEWND`) posé au démarrage ; sans déclenchement, `AfxDlgProc`/`AfxWndProc` ne trouvent pas le `CWnd` → le message map
  ne route pas `WM_INITDIALOG` vers `CDialog::OnInitDialog`. **Bonne nouvelle** : le subclassing `SetWindowLong(GWL_WNDPROC=-4)`
  **marche déjà** (échange le wndproc) — il ne manque que le déclenchement du hook.
- **Plan #3 (bricks fixturables isolément, comme les bricks SEH)** : **A** = `WH_CBT`/`HCBT_CREATEWND` déclenché pendant
  `CreateWindowEx` (avant `WM_NCCREATE`), via le hook proc lifté (esp) → MFC subclasse + attache le `CWnd` (le déblocage probable
  de FishTank) ; **B** = EH C++ (`_CxxThrowException`/`__CxxFrameHandler`) pour les TRY/CATCH de MFC ; **C** = `__except_handler3`
  réel (scope-table SEH). Mesurer après chaque brique (souvent A suffit à faire tomber le mur suivant). Chantier **multi-sessions**.

### 2026-07-24 — [GUI][HLE-WIN32] **MFC brick A : hook WH_CBT déclenché → l'`OnInitDialog` de FishTank s'exécute**
- **Brick A du chantier MFC.** `SetWindowsHookEx` n'était qu'un stub → le hook **`WH_CBT`** que MFC pose pour attacher/subclasser
  ses `CWnd` ne se déclenchait jamais → `AfxDlgProc` ne routait pas `WM_INITDIALOG` vers `CDialog::OnInitDialog`. **Implémenté** :
  le hook proc est stocké par idHook ; **`WH_CBT` est délivré** — `HCBT_CREATEWND` (avec `CBT_CREATEWND`+`CREATESTRUCTA` guest)
  est déclenché pour chaque fenêtre créée via `CreateWindowEx` **et** pour la fenêtre de dialogue (`u32_dialog_create`), **avant**
  `WM_NCCREATE`/`WM_INITDIALOG`. Le subclassing `GWL_WNDPROC` marchait déjà → le filtre MFC re-route le wndproc et attache le `CWnd`.
- **Fix composite associé (mesuré via FishTank)** : une classe prédéfinie connue (button/edit/combo…) est composée par **son
  apparence système même quand elle est SUBCLASSÉE** (MFC `DDX_Control`). Piloter le `WM_PAINT` du subclass ne peignait rien (une
  **boîte noire**) car le « proc original » sauvé de nos contrôles système est un stub non-peignant. Seul un contrôle vraiment
  non-standard (comctl32 lifté) se peint via `WM_PAINT`. La boîte noire sur le combo « Tank Size » subclassé a disparu.
- **Vérif** : fixture `winecorpus/user32_cbthook` (installe un hook `WH_CBT`, subclasse la fenêtre au `HCBT_CREATEWND`, le proc
  subclassé reçoit les messages) = `hooked=1 sub_msgs_positive=1 win=1` **bit-identique Wine**. **Démontré sur FishTank.exe (MFC)** :
  `OnInitDialog` **s'exécute** — le combo Water Type affiche **« Tropical Freshwater »** et les edits Length/Width/Height affichent
  **« 0 »** (étaient vides), aucun artefact noir. **Portes** : winediff **170/171** (`user32_cbthook` vert, seul rouge `gdi_uifont`),
  difftest 272/272, hash `19acad982194bf07` inchangé. Committé + poussé.
- **Reste FishTank (raffinements mesurés, suite)** : les **points des radios** (Rectangular / in.,gal.,lb.) et le texte **« Custom… »**
  du combo Tank Size ne s'affichent pas encore (MFC les pose par un chemin — DDX/CheckRadioButton — pas encore reflété). Bricks B
  (EH C++) / C (`__except_handler3`) mesurés au besoin ensuite.

### 2026-07-24 — [HLE-WIN32] **CallWindowProc(0) exécute le proc système du contrôle (chaînage de subclass) + diag radios FishTank**
- **Fix (prouvé)** : un subclasser (MFC `AfxWndProc`) sauve le wndproc **original** d'un contrôle prédéfini et y **chaîne** via
  `CallWindowProc` les messages qu'il ne traite pas. Pour nos contrôles système ce proc original est **0** (leur comportement vit
  dans `u32_control_proc`) → `CallWindowProc(0, …)` doit **émuler le proc système** : messages de classe (`BM_SETCHECK`/`CB_*`/
  `WM_SETFONT`) puis `DefWindowProc`. Avant, il rendait 0 et **jetait** le message → un contrôle MFC-subclassé perdait son état
  coché/liste. Gardé `winecorpus/user32_subclass` (subclasse une checkbox, chaîne `BM_SETCHECK` via `CallWindowProc`, relit
  `BM_GETCHECK`) : `saw=1 check=1` **bit-identique Wine** (baseline = `check=0`).
- **Diagnostic FishTank (#17, mesuré)** : les points des radios ne s'affichent **pas** car — trace — les radios ont **`wp=0`
  (non subclassés)** et **`check=0`** : MFC **n'envoie jamais** `BM_SETCHECK` aux radios. Le combo Water Type se peuple (un
  `AddString` explicite de `OnInitDialog` tourne), mais le chemin **`DoDataExchange`/`UpdateData(FALSE)`** (qui pose l'état des
  radios + la sélection Tank Size) **ne s'exécute pas complètement** — probablement **gated sur l'EH C++ (brick B)** ou un mur API
  MFC interne à `DoDataExchange`. Ce n'est **ni** un défaut de peinture, **ni** de subclass/dispatch. **Suite** : tracer jusqu'où
  `OnInitDialog`/`DoDataExchange` va sous ARET (ce qu'il appelle avant de s'arrêter) → décide entre brick B (EH C++) et une API manquante.

### 2026-07-24 — [HLE-WIN32] **DefWindowProc répond aux messages de classe d'un contrôle prédéfini (combo Tank Size FishTank)**
- **Suite #17.** Trace : le combo « Tank Size » de FishTank (subclassé par MFC `DDX_Control`) recevait bien **39 `CB_ADDSTRING`
  + `CB_SETCURSEL`** — mais routés via **`DefWindowProc`** (`dwp=40, cwp=0`), pas `CallWindowProc`. Cause : nos contrôles ont
  `wndproc==0` → MFC sauve un proc original **NULL** → `CWnd::Default()` chaîne les messages non traités vers **`::DefWindowProc`**
  (et non `CallWindowProc`). Notre `DefWindowProc` ne connaissait pas `CB_*`/`BM_*` → items perdus (combo vide).
- **Fix** : `DefWindowProc(A/W)` route d'abord par **`u32_control_proc`** (qui rend 0 pour une fenêtre non-contrôle → fenêtres
  ordinaires inchangées) — le « proc système » implicite de nos contrôles tient lieu du vrai proc contrôle qu'un super non-NULL
  serait sous Windows. **Mesuré sur FishTank** : le combo Tank Size affiche maintenant **« Custom… »** (comme Wine). ⚠️ **Pas
  bit-exact-fixturable en isolation** (le `DefWindowProc` de Wine ne gère **pas** `CB_*` — un vrai contrôle n'a jamais de super
  NULL) → vérifié sur le **vrai binaire MFC** + régression. Fixtures contrôle existantes (`user32_subclass`/`_dlgitemmsg`) toujours vertes.
- **Bilan FishTank** : combos (Water Type « Tropical Freshwater », Tank Size « Custom… ») + edits (« 0 ») **peuplés = Wine**. **Seul
  reste** : les **points des radios** (Rectangular / in.,gal.,lb.) — MFC n'envoie **jamais** `BM_SETCHECK` (0 mesuré) → le chemin
  `DoDataExchange`/`DDX_Radio` ne s'exécute pas (brick B / EH C++ ou mur API interne). Tâche #17 (radios) reste ouverte.

### 2026-07-24 — [HLE-WIN32] **Diagnostic précis des points de radio FishTank : DDX_Radio gate sur WS_TABSTOP=0**
- **Suite #17** (le combo Tank Size est réglé ; reste les points des radios). **Trace définitive** : `DoDataExchange`/`DDX_Radio`
  **s'exécute bien** (35 appels `GetWindow(GW_HWNDNEXT)` = la marche de groupe de DDX_Radio, tracés). Mais nos radios de dialogue
  sont des `BS_AUTORADIOBUTTON` avec **`WS_TABSTOP=0`** (styles `0x50000009` / `0x50020009` avec `WS_GROUP`). Or `DDX_Radio`
  **conditionne l'envoi de `BM_SETCHECK` sur `WS_TABSTOP`** → chaque radio est sauté → **0 `BM_SETCHECK`** → aucun point (mesuré).
  Également trouvé : **`WM_GETDLGCODE` (0x0087) non géré** (DDX/PrepareCtrl l'interroge) → tâche dédiée.
- **Prochaine MESURE (pas une supposition, règle §0)** : une fixture dialogue (DLGTEMPLATE mémoire ou .rc) avec un groupe de radios
  **sans `WS_TABSTOP`** → sous Wine, `GetWindowLong(GWL_STYLE)` rapporte-t-il `WS_TABSTOP` (le gestionnaire de dialogue l'ajoute-t-il ?)
  et la marche façon DDX_Radio coche-t-elle le radio ? Si Wine diffère d'ARET sur ce bit → corriger notre création de contrôle de
  dialogue pour matcher ; si identique → l'état « coché » de Wine vient d'un autre chemin (à investiguer). **Ne pas deviner-ajouter
  `WS_TABSTOP`.** Le reste du dialogue FishTank (combos + edits) = **peuplé comme Wine**.

### 2026-07-24 — [HLE-WIN32] **Radios FishTank : hypothèse WS_TABSTOP RÉFUTÉE par la mesure**
- **Mesure décisive** (fixture `winecorpus/user32_dlgradio` : groupe de radios **sans `WS_TABSTOP`** + marche façon DDX_Radio) :
  **bit-identique Wine vs ARET** — `tabstop r0=0`, la marche gated-WS_TABSTOP ne coche **rien** dans **les deux**. ⇒ l'hypothèse
  « les points de radio FishTank sont gated sur WS_TABSTOP » est **RÉFUTÉE** : le gestionnaire de dialogue de Wine n'ajoute pas
  `WS_TABSTOP` et une marche gated-WS_TABSTOP ne coche rien sous Wine non plus. (Règle §2 : mesurer, ne pas affirmer — hypothèse tombée.)
- **Où on en est** : le `DDX_Radio` réel de FishTank **s'exécute** (35 `GetWindow(GW_HWNDNEXT)`), envoie **0 `BM_SETCHECK`** sous ARET,
  alors que les radios sont cochés sous Wine — **même code MFC lifté** dans les deux. La divergence vient donc d'une **lecture HLE qui
  diffère au milieu de `DoDataExchange`** (candidats : ordre/appartenance des frères de `GetWindow`, une valeur `GetWindowLong`,
  `PrepareCtrl`/`GetDlgItem`, ou une valeur membre lue) qui fait sauter le `BM_SETCHECK` au code lifté. **Prochain** : trace niveau
  débogueur (`-O0 -g` gdb) du `DoDataExchange` lifté pour trouver la lecture HLE exacte qui diverge — **lourd** (MFC strippé, ~1380 fn).
  Le gros du dialogue (combos + edits) matche déjà Wine ; les points de radio sont le résidu profond.

### 2026-07-24 — [HLE-WIN32] **✅ #17 CLÔTURÉ : `WM_GETDLGCODE` = la cause générale des points de radio FishTank**
- **Cause racine trouvée par trace** (tous les messages aux fenêtres radio) : chaque radio reçoit **`WM_GETDLGCODE` (0x0087)** —
  **non géré** (rendait 0) — et **jamais `BM_SETCHECK`**. Le code de groupe-radio de MFC (`DDX_Radio`/`PrepareCtrl`) interroge
  `SendMessage(WM_GETDLGCODE) & DLGC_RADIOBUTTON` pour **confirmer qu'un contrôle est un radio** avant de le sélectionner ; recevant
  0, il classait chaque radio comme « pas un radio » → sauté → **0 `BM_SETCHECK`**. (Les hypothèses WS_TABSTOP puis DefWindowProc
  étaient des **fausses pistes réfutées par la mesure** — §2.)
- **Fix GÉNÉRAL (pas de rustine par binaire, §0)** : `u32_control_proc` répond à `WM_GETDLGCODE` avec la valeur par classe **mesurée
  bit-à-bit vs Wine** (fixture `user32_getdlgcode`) : radio=`0x2040` (BUTTON|RADIOBUTTON), defpush=`0x2010`, push=`0x2020`,
  checkbox=`0x2000`, groupbox/static=`0x100`, edit=`0x89`, combo/listbox=`0x81`. Vaut pour **tout contrôle, tout dialogue** (la
  classification de contrôle que le gestionnaire de dialogue + `IsDialogMessage` + MFC utilisent).
- **Résultat FishTank.exe (MFC)** : les radios **Tank Geometry (Rectangular) + Units (in.,gal.,lb.)** affichent leur **point de
  sélection = Wine**. **⇒ Le dialogue MFC est COMPLET** : combos (Water Type / Tank Size) + edits (0) + radios, tous conformes à
  Wine. Reste cosmétique hors #17 : surbrillance bleue du combo fermé focalisé (#18, focus-rendering, différé).
- **Portes** : difftest **272/272**, hash `19acad982194bf07` **inchangé**, winediff (+`user32_getdlgcode` +`user32_dlgradio`).
  Fixtures : `user32_getdlgcode` (8 classes bit-identiques Wine), `user32_dlgradio` (locke le comportement radio-sans-WS_TABSTOP).
  **#17 clos par une cause générale, vérifiée, testée, documentée.**

### 2026-07-24 — [HANDOFF] **Note pour le successeur (si compression de contexte)**
- **Reprise** : relis le **doc 70 EN ENTIER** et le **doc 80 EN ENTIER** ; relis les **dernières entrées du doc 71** + les
  **derniers commits** ; **énumère toutes les règles de travail** (principe sacré §0, doctrine §1, méthode §2, doc 80 §3 — elles
  sont et resteront **incontournables**) ; **fais le point**, puis **reprends le travail**.
- **En cours** : **tâche « focus » (gestion + rendu), dans les règles**, par incréments : (1) `SetFocus` envoie
  `WM_KILLFOCUS`/`WM_SETFOCUS` (ordre/wParam mesurés vs Wine, fixture) ; (2) `IsDialogMessage` (Tab/Shift-Tab via
  `GetNextDlgTabItem`, flèches via `GetNextDlgGroupItem`, Entrée=bouton défaut, Échap=IDCANCEL) ; (3) **rendu de focus** (#18 :
  pointillé `DrawFocusRect` auto + surbrillance bleue du combo fermé focalisé). Chaque incrément : fixture vs Wine → vert → commit → doc.
  **Puis enchaîner le plan** (brick B EH C++ = multiplicateur MFC ; résidus bornés #13 esp-drift, #15/#16).
- **État** : #17 **clos** (radios FishTank via `WM_GETDLGCODE` général, dialogue MFC complet). Portes vertes : difftest **272/272**,
  hash `19acad982194bf07`, winediff **173/174** (seul rouge `gdi_uifont` env). Tout poussé sur `claude/zen-hamilton-6pi1k4`.
  ⚠️ **`aret_win32.c` embarqué par `include_str!` → rebuild `cargo` après tout changement du runtime C.** ⚠️ Capture GUI = renderer
  software + capture racine, Xvfb `:99` (le relancer s'il meurt). ⚠️ Committer AVANT les vérifs longues (conteneur éphémère).

### 2026-07-25 — [GUI][HLE-WIN32] **Focus management (SetFocus messages + IsDialogMessage) + focus rendering — chantier « focus » clos dans les règles**
- **Objectif utilisateur** : « terminer focus dans les règles puis enchaîner le plan ». Le chantier couvre **la gestion**
  (qui a le focus, changement de focus, messages) **et le rendu** (état visuel du contrôle focalisé). Livré par incréments,
  chacun fixture→vert→commit→doc.
- **Incrément 1 — `SetFocus` notifie comme Wine** (`u32_set_focus`) : `WM_KILLFOCUS` au perdant (wParam = gagnant) **puis**
  `WM_SETFOCUS` au gagnant (wParam = perdant), `g_u32_focus` mis à jour **avant** (GetFocus correct dans les handlers),
  no-op si inchangé, retourne l'ancien focus. Gardé `winecorpus/user32_focusmsg` (ordre + wParam **bit-identique Wine**).
- **Incrément 2 — `IsDialogMessage`** (était un stub → 0) : navigation clavier de dialogue. Tab → `GetNextDlgTabItem` (Shift
  indisponible headless ⇒ avant, documenté), flèches → `GetNextDlgGroupItem` (groupe), Entrée → `WM_COMMAND` du bouton défaut
  (sinon IDOK), Échap → `WM_COMMAND(IDCANCEL)`. Le focus bouge via `u32_set_focus` (donc KILL/SET firent). Renvoie TRUE pour
  ces touches (les autres messages → FALSE, l'appelant les dispatche). Gardé `winecorpus/user32_isdlgmsg` (tab c1→c2, wrap
  c3→c1, enter/esc ret=1 **bit-identique Wine**).
- **Incrément 3 — rendu de focus (#18)**, **deux comportements généraux mesurés vs Wine** :
  1. **Focus initial de dialogue.** Après `WM_INITDIALOG`, si le DLGPROC retourne **TRUE**, le gestionnaire de dialogue donne
     le focus au **premier contrôle tab-stop** (`GetNextDlgTabItem(hDlg, NULL, FALSE)`) et fire `WM_SETFOCUS` ; s'il retourne
     **FALSE**, le DLGPROC a posé le focus lui-même → on le laisse. `u32_dialog_default_focus`, câblé dans les cœurs modal
     **et** modeless, **gated sur la valeur de retour** de `WM_INITDIALOG`. **Remplace** l'ancienne devinette au moment de la
     création (« premier EDIT » — ni le bon contrôle ni le bon moment).
  2. **Un changement de focus repeint les contrôles affectés** (`u32_set_focus` recompose la dialog de l'ancien et du nouveau
     contrôle) → leur rendu d'état de focus se met à jour, comme Windows repeint au changement de focus. ⇒ un
     **`CBS_DROPDOWNLIST` focalisé** montre sa sélection **surlignée** (`COLOR_HIGHLIGHT` + `COLOR_HIGHLIGHTTEXT`) ; non
     focalisé = `COLOR_WINDOW`. Vaut aussi pour la **navigation Tab** (le surlignage suit le focus).
- **Preuve bit-exact du général** : `winecorpus/user32_dlgfocus` (nouvelle) — dialogue à 3 contrôles (STATIC **non**-tabstop,
  puis BUTTON tab-stop id 100, puis EDIT tab-stop id 101) rapporte `GetDlgCtrlID(GetFocus())` après création :
  `initret=TRUE → focus_id=100` (premier tab-stop, static sauté), `initret=FALSE → focus_id=101` (l'EDIT que le proc a
  focalisé), **bit-identique Wine**.
- **Mesuré bout-en-bout sur FishTank.exe (MFC)** : le combo **Water Type** montre désormais sa sélection « Tropical Freshwater »
  sur la **couleur de surbrillance** = l'état focalisé de Wine (le combo est le premier tab-stop → focus initial). L'écart de
  **teinte** (ARET `#000080` COLOR_HIGHLIGHT **classique** vs Wine `#3096FA` **thémé uxtheme**) = **exactement le caveat
  classique-vs-uxtheme** déjà documenté pour tout contrôle composité (progress bar, checkbox, radio) — le **comportement**
  correspond, la teinte suit le thème chargé. Aucun focus-rect pointillé n'était requis (Wine focalise le combo, pas un bouton).
- **Portes** : difftest **272/272**, hash transpile `19acad982194bf07` **inchangé** (changement HLE-only), winediff (+`user32_focusmsg`
  +`user32_isdlgmsg` +`user32_dlgfocus`, seul rouge `gdi_uifont` environnemental). Committé + poussé sur `claude/zen-hamilton-6pi1k4`.
- **⇒ Chantier « focus » COMPLET** (gestion fonctionnelle + rendu). **Suite = le plan** : brick B EH C++
  (`_CxxThrowException`/`__CxxFrameHandler`, #15 — le multiplicateur MFC), puis résidus bornés (#13 esp-drift PeekMessage, #16
  `__except_handler3`).

### 2026-07-25 — [GUI][HLE-WIN32] **#13 (crash spin PeekMessage) : NON reproductible sur le build actuel — borné**
- **Reprise du plan après le chantier focus.** Brick B (EH C++, #15) mesuré **bloqué faute de driver** : FishTank n'importe
  aucun `_CxxThrowException`/`__CxxFrameHandler` (dialogue complet **sans** EH C++, cause #17 = `WM_GETDLGCODE`), mingw n'émet
  pas le modèle EH MSVC (pas de fixture possible), et les seuls candidats présents (`GlidePath.exe` jeu MFC+WinG,
  `winetest.exe`) abortent sur un **mur points-to orthogonal** (`call [ordinal non résolu]`) **avant** d'atteindre l'EH C++.
  Règle P2 : pas de forensics sur un mécanisme sans binaire qui échoue réellement dessus → brick B en attente.
- **Pivot sur #13 (échec dur reproduit = soundness, borné, autonome).** **Reproduction fidèle** du crash « spin PeekMessage » du
  2026-07-24 : fenêtre simple, puis fenêtre **montrée** (chemin SDL/`sdl_pump` actif), puis **enfant progress bar comctl32**
  (`--with-dll comctl32`, exactement `pbwin`), avec dispatch complet (`Translate`/`Dispatch`), **de 500 000 à 5 000 000
  itérations**, **avec** display (Xvfb :99, renderer software), **sans** display, et driver `dummy`. **Aucune configuration ne
  plante** (`exit=0`, `got=0/1`). ⇒ Le crash décrit **ne se reproduit plus**.
- **Verdict (mesuré, pas supposé)** : soit **corrigé incidemment** par le remaniement composite/pompe depuis le 2026-07-24
  (le chemin `PeekMessage`→`u32_pump_timers` a changé), soit **artefact environnemental** (le journal du 2026-07-24 notait
  lui-même un **Xvfb mort** donnant des faux résultats + « clock_gettime part en garbage » = symptôme de pile corrompue OU de
  display dégradé, pas un bug esp isolé prouvé). **#13 borné : rien à corriger à l'aveugle** (pas de repro = pas de cause
  prouvée à fixer, règle §0.4). À **rouvrir seulement** avec un repro concret et déterministe.
- **Portes** : inchangées (investigation lecture seule, aucun code touché). **Suite** : re-mesurer (Levier 0) le paysage des
  murs sur le corpus présent pour prioriser le prochain fix **général par la donnée** (le mur points-to/ordinaux qui bloque les
  drivers EH C++ est un candidat — à confirmer par la mesure, pas l'intuition).

### 2026-07-25 — [GUI][HLE-WIN32] **Famille palette GDI (comportement truecolor sound, bit-exact vs Wine) — piloté par la donnée**
- **Levier 0 re-mesuré** (`wallsweep.sh` sur le corpus Win95, **29 PE32**) après le chantier focus : instructions = **bruit**
  confirmé (`outsb`/`insb`/`out`/`in`/`hlt`/`arpl`/`bound`/`aam`/`daa`/`sldt` = privilégié/16-bit/**data-décodée-en-code** →
  abort correct ; `push`/`pop`/`mov` « unmodelled » co-occurrents = régions data mal-alignées). **Imports (le vrai signal)** :
  tête = `CoCreateInstance` 16/29 + `VerInstallFileA` 15/29 (**lourds**, vrais objets COM / install versionné → « plus tard ») ;
  **première famille bornée et testable = palette** (`CreatePalette`/`RealizePalette`/`GetSystemPaletteEntries`/… 5-7 binaires),
  doc-sanctionnée « 32bpp = no-op sound » (§5.0 2bis).
- **Implémenté (mesuré bit-pour-bit vs Wine, `winecorpus/gdi_palette`)** : sur notre cible **truecolor (32bpp)** une palette ne
  fait **aucun remapping de couleur**, mais le **modèle d'objet + les requêtes** doivent matcher Windows pour qu'une appli
  palette **tourne** au lieu d'aborter à `CreatePalette`. `CreatePalette` (stocke les entrées, nouvel objet `GDIT_PALETTE`) /
  `GetPaletteEntries` (round-trip ; count 0 = requête du total) / `SetPaletteEntries` / `GetNearestPaletteIndex` (plus proche
  euclidien) / `ResizePalette` (→1, entries ré-alloués+zéro) / `RealizePalette` (**→0**, truecolor : 0 entrée remappée) /
  `UnrealizeObject` (→1) / `SelectPalette` (rend la précédente, ou un **DEFAULT_PALETTE non-null** paresseux) /
  `GetSystemPaletteUse` (→1 SYSPAL_STATIC) / `SetSystemPaletteUse` / **`GetSystemPaletteEntries`** (→**0** mais **remplit** le
  buffer avec les **20 couleurs statiques réservées** — indices 0-9 et 246-255, **mesurés** ; reste à 0) + `GetObject(hpal,WORD)`
  = count d'entrées + `GetStockObject(DEFAULT_PALETTE=15)`. `GetNearestColor` reste **identité** (truecolor). Entrées libérées à
  `DeleteObject`.
- **`stdcall_pops` : +12 entrées palette** (triées, binary-search — sinon dérive esp = miscompile silencieux). Auto-couvertes par
  le scan `aret_X(uint32_t` du builder.
- **Effet mesuré** : coverage `GlidePath.exe` (jeu MFC) **31→34** (les 3 fonctions palette importées passent couvertes).
- **Portes** : difftest **272/272**, hash transpile `19acad982194bf07` **inchangé**, winediff **+`gdi_palette`** (bit-identique
  Wine ; seul rouge `gdi_uifont` env). Committé + poussé. **Suite plan** : re-mesurer / prochaine famille bornée (cluster GDI
  SetROP2/SetStretchBltMode, MDI, ou palette-restants) — toujours par la donnée. Brick B (EH C++) reste **bloqué faute de driver**
  (mur points-to sur GlidePath/winetest à lever d'abord).

### 2026-07-25 — [EH][INFRA] **Chantier EH C++ / MFC — Phase 0 : chaîne fixture MSVC-C++ prouvée + diagnostic v1/v3 (driver réel wzbeta32)**
- **Cap fixé par l'utilisateur** : objectifs prioritaires = **exceptions C++/MFC** (le multiplicateur) + mur d'appels indirects.
  « On n'a pas tout sous la main » n'est pas un blocage : on **obtient** ce qu'il faut. (Recadrage : arrêter les victoires
  faciles, aller au bout — ingénierie sérieuse.)
- **Correction factuelle (vérifiée journal 17/07, confirmée par Perplexity)** : le **mur points-to est déjà largement tombé**
  (slidelib/itiem95/DEMO32/ARTLANT franchissent leurs appels indirects → limites GUI/headless, pas couverture). La Phase
  « points-to » se réduit aux **résiduels mesurés**, pas un chantier neuf.
- **Mon « brick B impossible à fixturer sans MSVC » était FAUX.** `clang` 18.1.3 **cible l'ABI MSVC** (`--target=i686-pc-windows-msvc`)
  et émet le vrai EH C++ : `__CxxThrowException@8`, `___CxxFrameHandler3`. Avec `lld-link` + une **import-lib msvcrt générée depuis
  la vraie msvcrt de Wine** (`bench/eh/gen_msvcrt_lib.py`) + un pont décoration/type_info (`bench/eh/eh_support.c`), on **bâtit un
  PE C++ throw/catch qui tourne sous Wine** (`bench/eh/throw_catch.cpp` → **Wine `r=49`**). ⇒ **la même PE = oracle bit-exact**
  (Wine) vs ARET. Chaîne : `bench/eh/build.sh`.
- **Driver réel diagnostiqué (wzbeta32 = install WinZip beta 32, fourni par l'utilisateur)** : `WZ32.DLL` = **66 régions C++ EH**,
  `WZSEPE32.EXE` = 2, tous **`__CxxFrameHandler` v1 (magic `0x19930520`)** — **PAS** le v3 (`…522`) qu'émet clang. ⇒ **mise en garde
  Perplexity fondée** : implémentation **dual-version keyée par le magic** (v1 réel + v3 fixtures clang). « VC20 » (`0x56433230`)
  est un marqueur CRT distinct, **pas** le magic EH (le diag de juillet les avait conflés). `SETUP.EXE` = SEH pur (`_XcptFilter`,
  bon driver brick C).
- **Mur reproduit + trou de soundness trouvé** : sous ARET, `_CxxThrowException`/`__CxxFrameHandler3` non implémentés → le stub
  d'import **retourne** au lieu d'aborter → le `throw` tombe à travers, le `catch` lit du garbage (`r=4198623`) et le programme
  **sort 0** = **faux silencieux (§0.1) existant aujourd'hui**. Brick B le corrige (et devra faire ARET = Wine = `r=49`).
- **Portes** : aucun code runtime touché (Phase 0 = outillage `bench/eh/` + docs). difftest/hash inchangés. **Suite** : Phase 1
  brick C (`__except_handler3`, driver SETUP.EXE) → Phase 2 brick B (`__CxxFrameHandler` v1 + v3). Fixture clang = oracle de dev,
  binaires WinZip v1 = validation réelle.

### 2026-07-25 — [EH] **Phase 1 (brick C) — mur reproduit + design de `_except_handler3` mappé (le cœur dur identifié)**
- **Outillage fixture finalisé** (`bench/eh/`, sur import-libs mingw + `/safeseh:no` + lib générée pour `__CxxFrameHandler3` v3) :
  `throw_catch.cpp` (C++) → Wine `r=49` ; `seh_except.c` (`__try/__except/__finally` via `RaiseException` logiciel) → Wine
  `a=42 b=1 c=3 d=5 fin=110`. **Deux oracles valides.**
- **Mur brick C reproduit** : sous ARET, `_except_handler3` non implémenté → `RaiseException` dispatche, appelle le handler du
  frame = `_except_handler3` (absent) → l'exception n'est pas attrapée → `a=999 b=1 c=1 d=1` (faux-silencieux, comme `_CxxThrowException`).
- **Design mappé (lecture du C généré + de la PE)** : la fonction `__try` liftée est du **C structuré** ; le prologue pose la
  registration SEH inline (`[frame+4]`=handler `_except_handler3`, `[+8]`=scopetable, `[+0xc]`=trylevel, `fs:[0]`=frame). **La
  scope-table** (`{EnclosingLevel, FilterFunc, HandlerFunc}[]`) pointe des VAs que l'analyse ARET **récupère comme fonctions
  liftées séparées** (address-taken) : ex. `[0] filter=sub_40120e handler=sub_40109c`. ⇒ `_except_handler3` peut **appeler le
  filtre** (`aret_call(filter, ebp=frame+16)`) et lire son verdict (1 EXECUTE / 0 SEARCH / -1 CONTINUE).
- **Le cœur dur = le transfert non-local vers le handler.** Sur EXECUTE_HANDLER, `_except_handler3` doit dérouler (RtlUnwind +
  `__finally` via `_local_unwind2`), poser `trylevel=enclosing`, puis **sauter au bloc `__except`** — or ce bloc vit dans la
  fonction établisseuse (bloquée dans la pile C au site `RaiseException`). Options : (a) **setjmp injecté par le lifter** à
  l'établissement du frame SEH (`mov fs:[0],esp`) + `longjmp` depuis `_except_handler3` vers la continuation ; (b) exécuter le
  **funclet handler récupéré** puis propager son retour comme celui de la fonction. (a) est le modèle MSVC fidèle
  (`_JumpToContinuation`). ⇒ brick C = **changement lifter (setjmp au SEH-establish) + shim HLE `_except_handler3`** (scope-walk
  + filtre + unwind + jump). Chantier lourd **correctness-critique** (touche le lifter → toutes les portes). **Prochaine étape**
  ciblée, à concevoir proprement avant d'écrire (pas de demi-mesure).
- **Portes** : inchangées (Phase 1 = fixtures + repro, zéro code runtime/lifter modifié).

### 2026-07-25 — [EH] **Brick C — cœur dur disséqué : setjmp au SEH-establish + threading ebp des funclets (2 sous-problèmes)**
- **Poursuite de la conception** (avant d'écrire du code lifter correctness-critique). Le mécanisme setjmp/longjmp d'ARET est
  réutilisable (`aret_jmpbuf_for`/`aret_longjmp_do`, keyé par adresse, expansé inline). Le flux visé : le lifter injecte
  `setjmp(key=frame)` au SEH-establish ; `aret__except_handler3` fait scope-walk → filtre → unwind → `aret_longjmp_do(frame,
  lvl+1)` ; au retour du setjmp l'établisseur appelle le funclet handler et retourne sa valeur.
- **Sous-problème 1 — injection setjmp.** Impossible de faire retourner une fonction C bloquée (l'établisseur, coincé au site
  `RaiseException`) sans un setjmp qu'elle a posé. ⇒ le lifter doit émettre, au `mov fs:[0],esp` dont le handler poussé =
  `_except_handler3`, un `setjmp` + un épilogue de dispatch. **Gaté** sur ce motif ⇒ code non-SEH **byte-identique** (hash
  inchangé, régression verte).
- **Sous-problème 2 — threading ebp des funclets (le vrai nœud).** Mesuré sur le C généré : le filtre `sub_40120e` et le
  handler `sub_40109c` accèdent aux **locaux du parent** via un **registre-param threadé** (leur dernier param = l'ebp de
  l'établisseur), pas via un ebp machine. Or `aret_call(va,esp,a,c,d,b)` = eax/ecx/edx/ebx, **sans slot ebp**. Donc
  `aret__except_handler3` doit appeler ces funclets en plaçant l'**ebp de l'établisseur** (= `frame+16`) dans le bon slot
  registre-param — ce qui dépend du modèle exact de threading ebp d'ARET (per-fonction). C'est le point délicat qui rend brick C
  **lourd** (doc 80 §1.3 : « chantier lourd »).
- **État** : runway complet + validé (Phase 0/1), design **complet** jusqu'aux 2 sous-problèmes précis. Implémentation =
  **prochain travail focalisé** (lifter setjmp-injection gaté + `_except_handler3` avec threading ebp correct), à faire
  proprement avec la fixture `seh_except` (Wine `a=42 b=1 c=3 d=5 fin=110`) + **toutes les portes** comme oracle — pas de
  demi-mesure sur du code lifter correctness-critique. Zéro code runtime/lifter modifié à ce stade.

### 2026-07-25 — [EH] **Brick C composant 1 LIVRÉ (handler HLE) + composant 2 conçu (injection emit setjmp)**
- **Sous-problème 2 (threading ebp) RÉSOLU par la mesure** : `internal_call_args()` (build.rs) montre que le **6e arg
  d'`aret_call` (`b`) EST l'ebp** (registre-param callee-saved threadé), et le C généré confirme `establisher_ebp = frame+16`
  (le handler lifté restaure `fs:[0]` depuis `[ebp-0x10]=frame[0]`). ⇒ appeler un funclet filtre/handler avec le bon ebp =
  `aret_call(va, scratch, 0,0,0, frame+16)`. Le nœud craint était en fait propre.
- **Composant 1 (LIVRÉ, committé, portes vertes)** : `aret_except_handler3` (HLE) — scope-walk {EnclosingLevel, FilterFunc,
  HandlerFunc}[trylevel], appel du filtre, EXECUTE_HANDLER → global_unwind (RtlUnwind) + local_unwind (`__finally`) +
  `aret_longjmp_do(frame, lvl+1)` ; passe d'unwind = `__finally`. + `aret_seh_run(frame, level)` (exécute le funclet handler au
  retour du setjmp). **Additif** : `_except_handler3` couvert 100 % (fixture seh), hash `19acad982194bf07` **inchangé**,
  difftest 272/272. (Piège respecté : `sanitize_import` retire l'underscore de tête → shim `aret_except_handler3`.)
- **Composant 2 (conçu, à écrire) = injection lifter du setjmp au SEH-establish.** Approche **la moins risquée = niveau
  émission** (structured.rs `body_line`) : quand on rend le Store `fs:[0] = esp` **et** que le programme importe
  `_except_handler3` (gate programme → **tout le reste byte-identique, portes intactes**), appender
  `{ uint32_t _sj = aret_seh_setjmp(<esp>); if (_sj) return aret_seh_run(g_seh_frame, _sj-1); }`. **Macro dédiée**
  `aret_seh_setjmp(f)=setjmp(*aret_jmpbuf_for(f))` (keyé par `frame` **direct**, pas via `ARET_SJ_KEY` qui lit `[esp]`).
  **Subtilité setjmp/locals résolue** : après longjmp les locaux C non-volatile sont indéterminés → passer `frame` via un
  global `g_seh_frame` (posé par `_except_handler3` avant le longjmp) et le `level` via la valeur de longjmp (`_sj`), pas via un
  local. (Global = limite ré-entrance/fibers connue, à durcir plus tard.) Reste : détecter le motif fs:[0]-store-de-esp dans
  `body_line` (match Expr) + le gate `seh_active()` + build/test fixture `seh_except` (doit rendre `a=42 b=1 c=3 d=5 fin=110`
  = Wine) + régression complète. **Correctness-critique (emit) → à écrire soigneusement, pas en fin de marathon.**
- **État** : runway complet + composant 1 livré/vert/committé + composant 2 spécifié au détail. Prochaine étape focalisée.

### 2026-07-25 — [EH][LIFT] **Brick C composant 2 : injection setjmp au SEH-establish (machinerie complète, gatée, sound) — reste l'ABI funclet**
- **Machinerie d'injection écrite et fonctionnelle** (gatée sur l'import `_except_handler3` → tout le reste byte-identique) :
  `emit::set_seh_active(uses_seh(prog))` ; **lift.rs** émet, à chaque store `fs:[0]=reg` (segment fs, disp 0), un marqueur
  `__aret_seh_establish(value)` **avant** le store ; **structured.rs** rend le marqueur en setjmp **gardé par un check runtime**
  qui distingue establish de restore **sans dataflow** : `newframe->prev == fs:[0] courant` (vrai seulement à l'establish, le
  prologue vient de lier prev=ancienne tête). Macro dédiée `aret_seh_setjmp(frame)` keyée **directement** par le frame ;
  `g_seh_frame` porte le frame à travers le longjmp (les locaux C sont indéterminés après longjmp), le level via la valeur de
  longjmp. **Vérifié** : `mov fs:0x0,eax` (clang, pas esp) détecté ; le guard filtre bien les restores ; `_except_handler3`
  atteint et fait le scope-walk. Portes **vertes** : hash `19acad982194bf07` inchangé, difftest 272/272, sqlite3/busybox/winetest
  n'importent pas `_except_handler3` (byte-identiques), sqlite3 tourne (`4`/`64120494`/`42`).
- **Reste = l'ABI précise `_except_handler3`↔funclet** (la dernière pièce, mesurée) : (a) le **filtre** lit
  `GetExceptionInformation` via `[ebp-0x14]` = un pointeur EXCEPTION_POINTERS que `_except_handler3` doit **peupler** avant
  l'appel (sinon double-deref de garbage → faute) ; (b) l'**offset ebp** de référence des funclets (le handler `sub_40109c` fait
  `mov esp,[ebp-0x18]; add ebp,0xc` ⇒ appelé avec ebp=frame+16, vérifié `[frame+16-0x18]=[frame-8]`=esp sauvé ✓ ; le filtre
  utilise `[ebp+0]` comme base — à re-mesurer). **Interim sound** : `aret_except_handler3` fait un **abort bruyant**
  (`aret_unmodelled`, message clair) plutôt que de mal-exécuter un filtre → un programme `__try/__except` **aborte proprement**
  (mesuré sur la fixture seh), jamais boucle ni faux-silencieux. **Prochaine étape** : mesurer l'ABI exacte (peupler le slot
  exception-info + fixer l'ebp filtre) → retirer l'abort → fixture `seh_except` doit rendre `a=42 b=1 c=3 d=5 fin=110` = Wine.

### 2026-07-25 — [HANDOFF] **Consigne de reprise (à suivre DÈS la prise de travail, si compression)**
- **Rituel obligatoire avant tout** : relis le **doc 70 EN ENTIER** et le **doc 80 EN ENTIER** ; relis les **dernières entrées
  du doc 71** + les **derniers commits** ; **énumère toutes les règles de travail** (principe sacré §0, doctrine §1, méthode §2,
  doc 80 §3 — elles sont et resteront **incontournables**). Puis reprends.
- **Tâche en cours (demande utilisateur)** : **terminer brick C puis brick B** (EH C++/MFC), en planifiant bien, travail propre
  et dans les règles. brick C = ~90 % (handler HLE + injection setjmp gatée committés, portes vertes) ; reste l'**ABI funclet**
  (peupler le slot `EXCEPTION_POINTERS` à `[frame-4]` + ebp funclet=frame+16). Oracle : `bench/eh/seh_except.c` doit rendre
  `a=42 b=1 c=3 d=5 fin=110` = Wine. Puis brick B (`__CxxFrameHandler` v1+v3) réutilise le même transfert non-local.
- ⚠️ Outillage : `aret_win32.c`/`aret_hle.c` embarqués par `include_str!` → `cargo build --release` avant re-transpiler. Fixtures
  EH via `bench/eh/build.sh` (clang ABI MSVC). Driver réel = WinZip (`bench/eh` diag, `WZ32.DLL` v1).

### 2026-07-25 — [EH][LIFT] **✅ BRICK C COMPLET : `_except_handler3` (SEH `__try/__except/__finally`) bit-identique Wine**
- **Fonctionnel end-to-end** : la fixture `bench/eh/seh_except.c` rend `a=42 b=1 c=3 d=5 fin=110` = **exactement Wine** (catch
  simple, `__finally` normal + exceptionnel, filtre CONTINUE_SEARCH → catch externe). Harnais `bench/ehdiff.sh` (nouveau) :
  `seh_except` **ok**.
- **Les 2 dernières pièces de l'ABI, mesurées** : (1) **slot GetExceptionInformation** — un filtre lit `PEXCEPTION_POINTERS`
  à `[establisher_ebp-0x14]=[EstablisherFrame-4]` ; `_except_handler3` l'y **publie** ({ExceptionRecord, ContextRecord}) avant
  d'appeler les filtres (tout est 32-bit, recompile `-m32`). (2) **ebp funclet = EstablisherFrame+16** (confirmé : le handler
  fait `mov esp,[ebp-0x18]`=esp sauvé `[frame-8]` puis `add ebp,0xc`→ebp réel). (3) **guard d'injection durci** : le check
  establish/restore `newframe->prev == fs:[0]` **déréférençait** `newframe` — au restore vers le terminateur `0xffffffff` ça
  fautait ; on exclut `0xffffffff`/`0` avant de déréférencer.
- **Bilan machinerie** (composants 1+2, tous committés) : handler HLE scope-table (filtre/unwind/`__finally`) + injection setjmp
  gatée sur l'import `_except_handler3` (tout le reste **byte-identique**) + guard runtime establish-vs-restore (sans dataflow) +
  transfert non-local par longjmp vers l'établisseur (`g_seh_frame`/valeur de longjmp portent frame/level à travers le longjmp).
- **Portes** : difftest **272/272**, hash `19acad982194bf07` **inchangé**, winediff **177/178** (seul `gdi_uifont`), sqlite3/
  busybox/winetest byte-identiques et fonctionnels (n'importent pas `_except_handler3`), `ehdiff` `seh_except` ok. Committé + poussé.
- **Suite = brick B** (`__CxxFrameHandler` v1+v3, `_CxxThrowException`) : réutilise **tout** ce transfert non-local (setjmp
  injecté au SEH-establish, longjmp, funclet call). Reproduction en main : `bench/eh/throw_catch.cpp` DIFF (`r=49` Wine vs
  `r=4198623` ARET) — `_CxxThrowException` tombe à travers, à implémenter.

### 2026-07-25 — [EH] **Brick B (C++ EH) — reproduit + structure analysée ; plan (réutilise le transfert de brick C)**
- **Reproduction** : `bench/eh/throw_catch.cpp` DIFF (`r=49` Wine vs `r=4198623` ARET) — `_CxxThrowException` tombe à travers.
- **Structure mesurée (disasm `tc.exe`)** : `mainCRTStartup` installe un frame SEH dont le handler `[frame+4]=0x4010e0` est
  le thunk **`__CxxFrameHandler3`** (donc **l'injection setjmp de brick C fire déjà** au `mov fs:0x0,eax`). `[frame+8]`=state
  (-1 puis 0). `throw E{42}` = construit l'objet puis `_CxxThrowException(&obj, &ThrowInfo=0x402184)`. Le **catch funclet**
  (0x40104c) est atteint par le transfert du handler, fait `add ebp,0xc`, exécute le catch, puis **CONTINUE** l'exécution
  (0x401054, le throw suivant) — **pas de return** : c'est la différence avec `__except` (qui retournait la valeur de fonction).
- **Plan brick B** (méthode §2, réutilise le transfert non-local de brick C) :
  1. **`aret__CxxThrowException(pobj, pThrowInfo)`** : construit l'EXCEPTION_RECORD C++ (code `0xE06D7363`, params
     `[magic=0x19930520/22, pobj, pThrowInfo]`) et **dispatche via la chaîne fs:[0]** (réutilise le walk de `RaiseException`).
  2. **`aret__CxxFrameHandler3` (+ v1 `__CxxFrameHandler`)** : lire le **FuncInfo** (passé via edx par le thunk du frame) +
     le **ThrowInfo** (param exception) ; parcourir le **TryBlockMap** pour le `state` courant ; pour chaque catch, **matcher
     le type** (ThrowInfo→CatchableTypeArray→CatchableType→TypeDescriptor, compare les noms manglés) ; sur match → unwind +
     appeler le **catch funclet** (copie l'objet dans le param catch, exécute le corps, rend l'**adresse de continuation**) →
     transfert (longjmp établisseur + reprise à la continuation). **v1 vs v3** : layout FuncInfo différent (v3 ajoute EHFlags/
     pESTypeList), keyé par le magic.
  3. Structures MSVC à parser : `FuncInfo{magic, maxState, pUnwindMap, nTryBlocks, pTryBlockMap, ...}`, `TryBlockMapEntry
     {tryLow, tryHigh, catchHigh, nCatches, pHandlerArray}`, `HandlerType{adjectives, pType, dispCatchObj, addressOfHandler}`,
     `ThrowInfo{attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray}`, `CatchableType{properties, pType, ...}`,
     `TypeDescriptor{pVFTable, spare, name[]}`.
  - **Nuance vs brick C** : la continuation de catch n'est pas un simple return — le catch funclet rend une **adresse de
     continuation** dans l'établisseur. Modéliser via le même setjmp/longjmp mais reprendre à la continuation (à concevoir :
     soit le funclet récupéré inclut la continuation, soit transfert explicite). Chantier substantiel mais borné, driver réel
     = WinZip `WZ32.DLL` (v1, 66 régions).

### 2026-07-25 — [EH][LIFT] **✅ BRICK B — C++ EH base (`_CxxThrowException` + `__CxxFrameHandler3`) bit-identique Wine**
- **Fonctionnel end-to-end** : `bench/eh/throw_catch.cpp` rend `r=49` = **exactement Wine** (2 try/catch : classe par référence
  `catch(E&)` + type fondamental `catch(int)`, dans une même fonction, avec reprise de continuation entre les deux). Harnais
  `bench/ehdiff.sh` : **3/3** (`seh_except` brick C + `throw_catch` brick B + `throw_dtor` oracle d'abort).
- **Trois murs résolus (mesurés, pas devinés)** :
  1. **ABI du call HLE→funclet** — la boucle de dispatch de `aret_CxxThrowException` appelait `aret_CxxFrameHandler3(hesp)` mais
     le shim lit ses args cdecl à `[esp+0]` ; les dispatchers d'`aret_call` passent `esp+4` (saut du slot d'adresse-retour). Le
     `hesp` (args posés à `cf[1..4]`) donnait un `recp` garbage → **SIGSEGV** → le handler de faute matérielle rappelait le thunk
     `0x4010e0` via `aret_call` (non récupéré) → « indirect call to unrecovered ». Fix : `aret_CxxFrameHandler3(hesp + 4)`. *(La
     lecture de l'octet 0xB8 du thunk marche : le `.text` de l'image EST mappé à son VA — le résumé antérieur « guest code non
     mappé » était **faux**, mesuré `byte@handler=b8`.)*
  2. **Injection setjmp non déclenchée** — le gate `uses_seh` ne voyait que `_except_handler3` ; un binaire C++ pur importe
     `__CxxFrameHandler3`. Élargi à `starts_with("aret_CxxFrameHandler")` → le setjmp est injecté au SEH-establish C++ aussi
     (tout le reste **byte-identique**, hash inchangé). Sans lui, le longjmp du handler tombait dans un jmp_buf non-initialisé.
  3. **Continuation de catch (le vrai nœud)** — un catch funclet MSVC exécute le catch puis **retourne (eax) l'adresse de
     continuation** dans l'établisseur (où l'exécution reprend après le try/catch) ; ce n'est PAS un return de fonction (≠ brick C
     SEH). Modèle : `aret_seh_run` branche sur `g_seh_is_cxx` — côté C++ il appelle le funclet (→ VA de continuation) puis
     `aret_call(continuation, …, ebp)` pour reprendre. Un throw imbriqué dans la continuation re-longjmp vers le **même** setjmp
     (boucle naturelle, la pile hôte ne croît pas). **ebp funclet = `EstablisherFrame + 0xc`** (= `&frame->ebp` de Wine
     `call_catch_block`, mesuré/vérifié — ≠ `+16` du SEH).
- **Récupération SOUND des entrées EH** (`src/analysis/mod.rs::cxx_eh_entries`) — les funclets (atteints seulement par le
  dispatch EH) et surtout les **continuations** (matérialisées seulement en `mov eax,imm32` dans un funclet) ne sont vues ni par
  descente récursive, ni par le scan de prologues/pointeurs, alors que le programme y transfère **prouvablement**. On parse les
  **tables EH du binaire lui-même** : thunk `mov eax,&FuncInfo; jmp __CxxFrameHandler[3]` (scan `B8 … E9/FF25→IAT handler`) →
  `FuncInfo{magic,maxState,pUnwindMap,nTryBlocks,pTryBlockMap}` → `TryBlockMapEntry` (20 o) → `HandlerType` (16 o,
  `addressOfHandler`) = funclet ; puis décode le funclet (instruction-aware) pour le `mov eax,<code-imm>` final = la continuation.
  Rien de deviné (chaque entrée est prouvée par la métadonnée / un `mov eax,codeaddr;…;ret` que le programme exécute), général
  (tout binaire ABI-MSVC C++). Effet mesuré : `throw_catch` **4 → 6 fonctions** (les 2 continuations récupérées).
- **Soundness — destructeurs d'unwind NON modélisés = abort bruyant** (pas de faux silencieux). Le modèle mono-passe longjmpe
  droit au catch ; il ne lance pas les destructeurs C++ que le vrai unwind exécuterait. Garde : `aret_cxx_unwind_has_dtor` scanne
  l'`UnwindMap` des états unwindés (à l'entrée du catch **et** à la propagation à travers un frame non-catchant) ; toute action
  non-nulle ⇒ `aret_unmodelled("destructor during … unwind not modelled")`. Oracle `bench/eh/throw_dtor.cpp` (+ `.abort`) :
  Wine imprime `dtor`+`r=42`, **ARET aborte** (jamais `r=42` seul). *(Piège mesuré : un destructeur à effet foldable — `cleaned++`
  — est **constant-foldé** par clang en `inc eax`, sans funclet ni UnwindMap : d'où le destructeur à `printf` non-foldable.)*
- **Portes** : difftest **272/272**, hash transpile `19acad982194bf07` **inchangé** (changements C++ EH gatés sur l'import ;
  `cxx_eh_entries` retourne vide sans import `CxxFrameHandler` → binaires non-EH byte-identiques), ehdiff **3/3**, winediff (à
  confirmer, orthogonal). Committé + poussé.
- **Reste brick B** : (a) **destructeurs d'unwind** (lancer les funclets `UnwindMap`, lever la garde) — fixture `throw_dtor` prête ;
  (b) **rethrow** / catch imbriqués multi-frames ; (c) validation sur le **driver réel** WinZip `WZ32.DLL` (v1, magic `0x19930520`,
  `__CxxFrameHandler` — mêmes offsets FuncInfo, le dispatch route déjà par le thunk 0xB8 indépendamment de la version) ; (d) MFC
  (le driver FishTank). La base (throw/catch simple, types fondamentaux + classes par réf/valeur, multi-try + continuations) est
  **complète et bit-identique Wine**.

### 2026-07-25 — [EH] **Brick B — destructeurs d'unwind C++ exécutés (local unwind) bit-identique Wine**
- **Increment** : la garde d'abort « destructor during catch unwind not modelled » est **levée** — les destructeurs locaux
  s'exécutent réellement pendant l'unwind. `aret_cxx_local_unwind(fi, framep, from, to)` reproduit `cxx_local_unwind` de Wine :
  parcourt l'`UnwindMap` de l'état courant vers l'état cible (chaîne `toState`), avance `frame->state` **avant** chaque action
  (un throw dans un destructeur reprend l'unwind au bon point), et appelle chaque destructeur non-nul via `aret_seh_funclet`
  (ebp = `frame+0xc`). Appelé à l'entrée du catch (`state`→`tryLow`), puis `frame->state := tryHigh+1` (comme Wine).
- **Oracle** : `bench/eh/throw_dtor.cpp` (destructeur à `printf` non-foldable) — Wine imprime `dtor` puis `r=42` ; **ARET
  identique** (le destructeur s'exécute, dans l'ordre). Passe désormais du statut « oracle d'abort » à **égalité stricte**
  (`.abort` retiré). ehdiff **3/3** en égalité.
- **Garde restante (sound)** : `aret_cxx_unwind_has_dtor` conservée sur le **chemin de propagation** (frame traversée sans
  catch) — un destructeur dans un frame **intermédiaire** multi-frames n'est pas encore lancé ⇒ abort bruyant (jamais sauté en
  silence). Fixture multi-frames à ajouter avec cet incrément.
- **Portes** : hash transpile `19acad982194bf07` **inchangé** (changement 100 % dans le chemin HLE C++ EH, gaté), ehdiff **3/3**,
  difftest/winediff (confirmés inchangés — les binaires n'exercent pas le chemin `__CxxFrameHandler`). Committé + poussé.

### 2026-07-25 — [EH] **Brick B — unwind à deux passes multi-frames (throw dans un callee, catch dans le caller) bit-identique Wine**
- **Increment** : le vrai motif réel (MFC/WinZip : `throw` profond, `catch` superficiel, nettoyage entre les deux). Passe le
  modèle mono-passe (search+transfer) à un vrai **deux-passes** façon Wine :
  - **Phase 1 (search)** : `aret_CxxThrowException` marche `fs:[0]` sans effet de bord ; un handler qui ne catch pas retourne 1.
  - **Phase 2 (unwind)** : à la frame qui catch, `aret_cxx_global_unwind(esp, framep)` re-marche `fs:[0]` de la tête jusqu'à
    (exclu) la frame catchante, appelle chaque handler intermédiaire avec `EH_UNWINDING` (dispatch `aret_cxx_call_handler`,
    0xB8-aware : thunk C++ → `aret_CxxFrameHandler3`, sinon `aret_call`), pop chaque frame — **innermost d'abord**. Sur la passe
    d'unwind, `aret_CxxFrameHandler3` lance les destructeurs locaux de la frame (`aret_cxx_local_unwind(state,-1)`). Puis unwind
    partiel de la frame catchante (→`tryLow`) + transfert.
- **Récupération des funclets destructeurs** (`analysis::parse_cxx_func_info`) : le bug était le `return` anticipé sur
  `nTryBlocks==0` — une frame peut n'avoir **qu'une UnwindMap** (un local à destructeur, sans catch — ex. `inner()`). On parse
  désormais l'**UnwindMap** (`{maxState, pUnwindMap}` → entrées `{toState, action}`) et on récupère chaque `action` non-nulle
  (funclet destructeur) comme entrée de fonction. Sans ça, `aret_call(dtor_funclet)` abortait « unrecovered ».
- **Oracle** : `bench/eh/throw_across.cpp` (`inner()` `noinline` avec `Guard` à `printf`, throw ; `catch` dans `mainCRTStartup`)
  — Wine `inner-dtor`+`r=42`, **ARET identique**. ehdiff **4/4** (seh_except, throw_catch, throw_dtor, throw_across).
- **Portes** : hash transpile `19acad982194bf07` **inchangé**, ehdiff **4/4**, difftest/winediff (confirmés inchangés). Committé + poussé.
- **Reste brick B** : rethrow (`throw;` nu), catch-by-value avec copy-ctor non trivial, driver réel WinZip `WZ32.DLL` (v1), MFC.

### 2026-07-25 — [EH] **Brick B — bornage : rethrow & driver réel = CRT statiquement liée (prochain jalon identifié)**
- **Rethrow non fixturable** : `throw;` nu re-lève l'exception **courante** (MSVC : `_CxxThrowException(NULL,NULL)` lit l'état
  par-thread de la CRT). Le harnais minimal (`mainCRTStartup`, **sans init CRT**) ne l'initialise pas → **Wine lui-même faute**
  (page fault) sur la fixture. Donc rethrow n'est pas testable ainsi (l'oracle crashe) ; il faut la vraie CRT / le driver réel.
- **Driver réel = découverte architecturale majeure** : `WZSEPE32.EXE` (auto-extracteur WinZip, 203 Ko) **utilise bien le C++ EH
  v1** (magic `0x19930520`, 2 régions mesurées) **MAIS la CRT est liée statiquement** — `__CxxFrameHandler`/`_CxxThrowException`
  sont **dans le `.text`**, PAS des imports (173 imports = kernel32/user32/gdi32 GUI, zéro runtime EH). Conséquence : le modèle
  HLE gaté-sur-import de brick B **ne s'engage pas** sur ces binaires — le throw/dispatch est du **code lifté interne**, pas routé
  vers le HLE. C'est cohérent avec doc 80 §1.3 (« driver réel = multi-sessions »).
- **Prochain jalon brick B (précisé)** : **reconnaissance FLIRT de la CRT EH statiquement liée** (`_CxxThrowException` /
  `__CxxFrameHandler[3]` internes) → routage vers le HLE, exactement la doctrine memmove/libm (§4.4/§4.2 : reconnaître par
  signature **prouvée** vs Unicorn, emballer « correct ou abort »). C'est un chantier distinct (signatures version-spécifiques +
  ABI d'appel interne + gate élargi au-delà de l'import). La base fixturée (runtime EH **importé**) est, elle, **complète et
  bit-identique Wine** (throw/catch, destructeurs, multi-frames). Binaires réels disponibles : scratchpad `wz/` (WZ32.DLL v1 66
  régions, WZSEPE32.EXE v1 2 régions).

### 2026-07-25 — [EH] **Brick B — liaison du paramètre catch rendue générale & sound (taille réelle + ajustement de base)**
- **Faille de soundness trouvée à l'auto-revue** (avant de bâtir plus loin) : la liaison du param catch **par valeur** copiait
  **seulement le premier mot** (`*slot = *(uint32_t*)pObject`). Correct pour les fondamentaux et POD 1-mot (toutes les fixtures
  passaient), mais pour une **classe multi-mots attrapée par valeur** → seuls les 4 premiers octets copiés, le reste = garbage =
  **faux présenté comme correct** (viole §0). De même, un catch d'une **classe de base** avec ajustement de pointeur (`PMD.mdisp`)
  n'était pas appliqué.
- **Fix général & sound** : `aret_cxx_catchable_match` retourne désormais la **CatchableType** appariée (qui porte
  `PMD{mdisp,pdisp,vdisp}`, `sizeOrOffset`, `copyFunction`). Liaison : `src = pObject + mdisp` (ajustement vers le sous-objet de
  base) ; par référence → pointeur ajusté ; par valeur trivadmissible → `memcpy(slot, src, size)` (taille réelle) ; **copy-ctor
  non trivial** (`copyFunction != 0`) ou **base virtuelle** (`pdisp != -1`) → **abort bruyant** (jamais copié faux).
- **Oracle** : `bench/eh/throw_byval.cpp` (`struct E{int a;int b;}` attrapée **par valeur**, 8 o) — Wine `r=49`, ARET **identique**
  (l'ancien code aurait donné `a`+garbage). ehdiff **5/5**.
- **Portes** : hash transpile `19acad982194bf07` **inchangé** (gaté), ehdiff **5/5**, difftest/winediff (confirmés). Committé + poussé.

### 2026-07-25 — [EH] **Jalon suivant : reconnaissance CRT-EH statiquement liée — grounding + plan (comprendre avant d'implémenter, §2)**
- **But** : router le `_CxxThrowException` **interne** (CRT liée statiquement, cas des vrais binaires 1990s) vers le HLE. Le
  dispatch des handlers réutilise déjà la détection **structurelle** du thunk `0xB8` (indépendante de l'import), donc **la seule
  pièce manquante = reconnaître `_CxxThrowException` interne** et le brancher sur `aret_CxxThrowException` (doctrine memmove/libm :
  `crt_symbol` → shim, reconnaissance **prouvée**, emballage « correct ou abort »).
- **Grounding mesuré (WZ32.DLL, imgbase 0x20000000, 8 réfs `0xE06D7363` en `.text`)** : à `0x20024e..` le code fait
  `cmp [esi],0x19930520` / `cmp [eax],0xe06d7363` = le **handler** qui *lit/compare* les codes. Donc **`__CxxFrameHandler` ET
  `_CxxThrowException` référencent tous deux les constantes** → « contient `0xE06D7363` » **ne suffit pas** à distinguer. Signature
  structurelle requise : le *thrower* **construit** un EXCEPTION_RECORD (`mov/push 0xe06d7363`, `push 0x19930520`, params
  `[magic,pobj,pThrowInfo]`) puis **appelle `RaiseException`** ; le *handler* **compare** `[reg]==0xe06d7363`.
- **Blocage de testabilité (honnête, §2 « mesurer pas affirmer »)** : pas d'oracle exécutable sous la main — le **MSVC CRT statique
  est proprio** (pas de `libcmt`), et `WZ32.DLL` est une **DLL** (pas lançable seule ; WZSEPE32.EXE, lui, n'a **aucun** `0xE06D7363`
  en `.text` = throw C++ quasi absent). ⇒ le vrai 1er pas propre = **fabriquer une fixture reproductible** : le toolchain
  `bench/eh/build.sh` en variante « CRT statique » (fournir `_CxxThrowException`/`__CxxFrameHandler` **localement** au lieu de les
  importer de msvcrt), pour que ARET doive les reconnaître structurellement et router — vérifiable vs Wine comme les autres. C'est
  le prochain incrément focalisé (chantier multi-sessions, doc 80 §1.3).
- **Ne PAS rusher** le code de reconnaissance correctness-critique sans cet oracle (règle §2). Base fixturée (runtime importé) =
  **complète et bit-identique Wine** (5/5), indépendante de ce jalon.

### 2026-07-25 — [EH] **CRT-EH statique — côté thrower FAIT via `RaiseException(0xE06D7363)` (insight clé : pas de reconnaissance fragile)**
- **Insight majeur qui dé-risque le jalon** : un `throw` C++ — **même avec CRT liée statiquement** — passe **toujours** par le
  `RaiseException` **importé** (kernel32, un wrapper syscall que la CRT ne peut pas inliner ; vérifié : `WZ32.DLL` importe
  `RaiseException`). Donc **inutile de reconnaître le `_CxxThrowException` interne** (dont la signature structurelle est ambiguë —
  le thrower *construit* et le handler *compare* les mêmes constantes `0xE06D7363`/`0x19930520`). On intercepte au niveau
  `RaiseException` : quand `code == 0xE06D7363`, `aret_RaiseException` extrait `{magic, pObject, pThrowInfo}` des params et lance le
  **même dispatch C++ deux-passes** que le shim `_CxxThrowException` importé (helper partagé `aret_cxx_dispatch`, 0xB8-aware).
- **Un seul chemin couvre les deux cas** (runtime EH importé **et** CRT statique) — aucune rustine, aucune constante par-binaire.
- **Oracle reproductible** `bench/eh/throw_static.cpp` : `_CxxThrowException` défini **localement** (dans le `.text`, override du
  symbole d'import — lld-link prend l'objet, ne tire pas le membre d'archive) et appelant `RaiseException` — exactement la CRT
  statique réelle. Wine `r=49`, **ARET identique**. `_CxxThrowException` **absent des imports** (= local, confirmé). ehdiff **6/6**.
- **Portes** : hash transpile `19acad982194bf07` **inchangé** ; `aret_RaiseException` ne fait qu'ajouter une branche sur le code
  C++ (les autres RaiseException — SEH — inchangés) ; difftest/winediff (confirmés).
- **Reste pour le driver réel COMPLET** : le **handler** aussi statiquement lié (gate `uses_seh` + parse `cxx_eh_entries` détectent
  aujourd'hui le thunk par la cible d'import ; il faut les faire détecter le thunk `mov eax,&FuncInfo(magic valide); jmp <interne>`
  par la **magie du FuncInfo** plutôt que par l'import) → puis faire tourner un vrai binaire bout-en-bout (les autres murs).

### 2026-07-25 — [EH][ANALYSIS] **CRT-EH statique — côté handler : détection du thunk par la MAGIE FuncInfo (import-indépendant)**
- **`cxx_eh_entries` généralisé** : le thunk handler `mov eax,&FuncInfo; jmp <__CxxFrameHandler>` est désormais reconnu
  **structurellement** — opérande `B8` pointant sur un FuncInfo (1er dword = magie EH `0x19930520/21/22`) **+** jmp qui suit
  (`E9` interne = CRT statique, `FF25` import = CRT dynamique). Plus de dépendance à l'import du handler ⇒ les funclets/continuations/
  destructeurs se récupèrent **aussi quand `__CxxFrameHandler` est lié statiquement** (vrais binaires 1990s). Rétro-compatible :
  fixtures (handler importé) toujours **6/6** (le thunk pointe toujours un FuncInfo valide).
- **Validé sur données réelles** : `WZ32.DLL` (CRT statique) → **58 thunks EH détectés** (≈ « 66 régions » du README) ; le DLL
  transpile (973 fn, 961 liftées) sans explosion. Non-EH : le scan est gaté sur magie+jmp ⇒ zéro faux positif (hash inchangé,
  difftest/winediff confirmés).
- **⇒ RECONNAISSANCE CRT-EH statique COMPLÈTE (thrower + handler)** : côté thrower via `RaiseException(0xE06D7363)`, côté handler
  via la détection par magie. Le HLE route déjà le thunk `0xB8` vers `aret_CxxFrameHandler3` (bypass du handler interne).
- **Reste pour le driver réel bout-en-bout** (chantier multi-sessions, non fixturable) : (a) gate `uses_seh` élargi au cas
  **entièrement** statique (aujourd'hui il voit l'import `__CxxFrameHandler*` ; pour une CRT 100 % statique, le faire déclencher
  sur la présence de thunks EH) — non validable sans exécuter un vrai binaire ; (b) faire tourner un vrai binaire jusqu'à l'EH
  (tous les autres murs : imports GUI, appels indirects…). Base fixturée = **complète et bit-identique Wine** (6/6).

### 2026-07-25 — [RECOV] **Murs d'appels indirects — MESURE sur binaires réels : la récupération les résout déjà ; le 1er mur = imports HLE**
- **58 vs 66 (WZ32.DLL) élucidé** : les « 66 » du README = occurrences brutes de la constante `0x19930520`. Ventilation mesurée :
  **58 vrais FuncInfo `.rdata` référencés par un thunk (tous détectés)** + **6 en `.text`** = opérande `cmp [reg],0x19930520` *dans*
  `__CxxFrameHandler` (pas des régions) + 1 `.data` + 1 `.rdata` **orphelin** (magie coïncidente, `nTryBlocks=0xffffffff` garbage, **0
  référence**). ⇒ détection EH **complète** (58/58 actifs), pas incomplète.
- **Mesure des murs d'appels indirects (§2, donnée d'abord)** sur les binaires réels disponibles :
  - `WZ32.DLL` (973 fn) : **0 appel direct non résolu** ; murs = 7 imports HLE + 1 vraie jump-table + qq instructions décodées-en-code.
  - `WZSEPE32.EXE` (auto-extracteur, 336 fn liftées) : **0 appel direct non résolu, 0 mur d'appel indirect statique** ; sous ARET il
    **tourne** (exit 0). Murs = **12 imports HLE** (GUI/shell : `CreateDCA`, `DragQueryFileA`, `EnumWindows`, `GetWindowWord`…) + 1
    `outsb` (donnée-en-code, benin).
- **Conclusion honnête & mesurée** : sur ces vrais binaires, **la récupération de fonctions d'ARET (§4.4 : scan address-taken data =
  vtables, jump-tables, callbacks) résout déjà les appels indirects** — le « mur points-to » est largement tombé (cohérent avec la
  note Perplexity). Le **1er mur réel = les imports HLE** (GUI/shell). La séquence donnée-pilotée pour aller plus loin sur un vrai
  binaire = **fermer ces imports** (shims généraux, oracle Wine) → il tourne plus loin → **alors** re-mesurer un éventuel mur
  d'appel indirect **profond** (dispatch vtable C++/MFC), qui n'apparaît que sur un vrai binaire MFC (driver, non dispo ici).

### 2026-07-25 — [EH][RECOV] **✅ Brick B VALIDÉ sur un vrai driver MFC — WinMerge 2.14.0 (MFC90, CRT statique, 869 régions EH)**
- **Driver réel fourni** (utilisateur) : `WinMergeU.exe` (2,3 Mo, **i386 32-bit**, **MFC90 lié statiquement**, **869 magies EH v3
  `0x19930522`**, importe `RaiseException`). Exactement la cible de brick B (CRT statique + vtables MFC).
- **✅ Reconnaissance EH statique PROUVÉE sur données réelles** : ma détection par magie FuncInfo trouve **868/869 thunks handler**
  (le 1 restant = magie coïncidente en donnée, comme l'orphelin WZ32). ⇒ la reconnaissance CRT-EH statique (handler par magie +
  thrower par `RaiseException`) **marche sur un vrai binaire MFC90**, pas seulement sur fixtures.
- **Récupération** : **9761 fonctions** (9573 liftées), **appels indirects résolus** (le mur points-to ne se manifeste pas ; 4
  « appels directs non résolus » = adresses garbage de données-en-code). ⇒ confirme : sur un vrai MFC, la récup gère les indirects.
- **1er mur runtime mesuré (§2)** : au **démarrage CRT**, `_encode_pointer` (import HLE, cookie de sécurité MSVC) puis **`pushfd`**
  (instruction non liftée) → abort sound. Murs statiques : **148 imports HLE** (COM/shell/GUI) + qq instructions décodées-en-code.
- **Prochain incrément concret & général = lifter `pushfd`/`popfd`** : reconstruire EFLAGS depuis les drapeaux suivis
  individuellement (CF bit0, PF bit2, AF bit4, ZF bit6, SF bit7, DF bit10, OF bit11 + bits réservés/IF) et redistribuer au pop.
  **Correctness-critique** (positions de bits + bits non-suivis) → à implémenter **avec validation cpudiff vs Unicorn** (pas à
  l'arrache). C'est le blocage du démarrage CRT de tout binaire MSVC statique. Ensuite : les 148 imports HLE (data-driven, oracle
  Wine) pour faire tourner WinMerge plus loin.

### 2026-07-25 — [LIFT] **Spec `pushfd`/`popfd` : subtilité CPUID → shadow EFLAGS (ne PAS modéliser naïvement)**
- **Blocage mesuré** : le démarrage CRT MSVC de WinMerge abort sur `pushfd` (non lifté). ARET suit les drapeaux
  **individuellement** (`read_flag`/`set_flag` ; CF b0, PF b2, AF b4, ZF b6, SF b7, DF b10, OF b11) — il n'a pas de registre EFLAGS.
- **Piège identifié (avant d'écrire)** : la CRT MSVC détecte CPUID par l'idiome `pushfd; pop eax; mov ecx,eax; xor eax,0x200000;
  push eax; popfd; pushfd; pop eax; xor eax,ecx` = **bascule le bit 21 (ID)** et teste s'il tient. Un modèle naïf (pushfd =
  assembler seulement les drapeaux suivis, bit21=0) pousserait **bit21=0 les deux fois** → l'idiome conclut « CPUID absent » →
  chemin CRT legacy/faux. **Faux silencieux potentiel.**
- **Modèle SOUND requis** : un **registre shadow `eflags_other`** portant les bits **non suivis** (init `0x202` = IF+réservé b1),
  préservé au travers du couple push/pop : `pushfd = push(eflags_other | assemble(drapeaux suivis))` ; `popfd = { set drapeaux
  suivis depuis la valeur ; eflags_other = valeur & ~MASK_suivis }`. Ainsi le bit 21 basculé par l'idiome **survit** au round-trip →
  CPUID correctement détecté. **À valider cpudiff vs Unicorn** (positions de bits + valeur des bits non suivis dans le seed
  Unicorn) — correctness-critique, à faire en focalisé, pas à l'arrache. C'est le prochain incrément lifter concret.

### 2026-07-25 — [LIFT] **✅ `pushfd`/`popfd` liftés (modèle shadow-EFLAGS) — bit-exact Unicorn, idiome CPUID inclus ; débloque le démarrage CRT de WinMerge**
- **Implémenté** (`ir/lift.rs`) : `pushfd` réassemble EFLAGS = `(shadow & ~0xCD7) | 0x2 | drapeaux_suivis@positions` ; `popfd`
  redistribue la valeur poppée dans les 7 drapeaux **et** le **shadow** (`RegId(121)`, pseudo-registre entier comme `fsw`, init 0 =
  seed cpudiff/Unicorn). Le shadow porte les bits non-suivis (IF, **ID bit21**…) au travers du couple push/pop.
- **Validé bit-exact vs Unicorn** (cpudiff, l'oracle) : `pushfd`/`popfd` ajoutés au `seq_pool` → `sequence_random_matches_unicorn`
  (4000 compositions random, regs+drapeaux+**page scratch** = la valeur EFLAGS poussée) **OK**, et `sequence_corpus` **OK** avec 2
  séquences curatées ajoutées : (a) `add;pushfd;sub;popfd` (round-trip des drapeaux), (b) **l'idiome CPUID du CRT MSVC**
  `pushfd;pop eax;xor eax,0x200000;push;popfd;pushfd;pop ecx` — ecx doit porter le bit21 basculé, ce qui ne marche **que** grâce au
  shadow. Le piège identifié en amont est donc couvert et **prouvé**.
- **Effet mesuré** : WinMerge **franchit le mur `pushfd`** du démarrage CRT (avance ensuite sur `_encode_pointer`/`_decode_pointer`/
  `__dllonexit` = imports faible-stubbés). Hash transpile `19acad982194bf07` **inchangé** (les 4 fixtures difftest_transpile
  n'utilisent pas pushfd → byte-identiques). Portes difftest/funcdiff/winediff : à confirmer.
- **Reste WinMerge** : lot d'imports HLE (CRT sécurité `_encode/_decode_pointer` = passthrough sound ; `__dllonexit` ; puis les
  148 GUI/COM) pour tourner plus loin.

### 2026-07-25 — [RECOV][HLE] **WinMerge : traverse tout le démarrage CRT → bute sur la frontière `mfc90u.dll` (884 imports par ordinal)**
- **Après pushfd/popfd + shims CRT** (`_encode_pointer`/`_decode_pointer` passthrough sound, `__dllonexit`→atexit), WinMerge
  **traverse tout l'init CRT** (plus aucun import CRT non implémenté). Nouveau mur, bien plus profond : `indirect call to unrecovered
  0x80000471` = un **import par ordinal non résolu** (`0x80000000 | ord 0x471`).
- **Source mesurée** : l'ordinal vient de **`mfc90u.dll`** — WinMerge lie la CRT **statiquement** (d'où mes 869 magies EH) mais lie
  **MFC dynamiquement** : **884 fonctions MFC importées par ordinal**. Résoudre = besoin de la **table d'export de `mfc90u.dll`**
  (ordinal→fonction), puis lifter le DLL (`--with-dll`, doc 80 §1.2 « endgame M7-GUI ») ou shimmer MFC.
- **Blocage honnête** : `mfc90u.dll` **n'est pas dispo** (ni dans le zip WinMerge, ni sur le système — c'est le redist MFC90). Sans
  lui, l'exécution de WinMerge s'arrête à cette frontière. ⇒ pour faire **tourner** WinMerge (et donc voir l'EH C++ **se déclencher**,
  pas seulement être reconnu), il faut **le redist MFC90** (`mfc90u.dll` + `msvcr90.dll`/`msvcp90.dll` éventuels).
- **Ce que WinMerge a déjà prouvé** (sans tourner jusqu'au bout) : reconnaissance EH statique **868/869** sur MFC90 réel, récup
  **9573 fn** (indirects résolus), et il a piloté deux vrais incréments généraux : **pushfd/popfd** (validé Unicorn) + shims CRT.

### 2026-07-25 — [RECOV] **MFC90 lifting (endgame M7-GUI) — pièces prouvées, la frontière ordinaux TOMBE ; reste l'échelle du blob 4-modules**
- **`mfc90u.dll` obtenu** : extrait de `vcredist_x86.exe` (VC++ 2008 SP1, fourni par l'utilisateur) → `vc_red.cab` → `nosxs_mfc90u.dll`
  (3,78 Mo, i386, table d'export OK) + `msvcr90.dll`/`msvcp90.dll`. **Non committés** (binaires MS propriétaires ; doctrine licence,
  doc 80 §3.5 — gardés en scratchpad pour validation locale).
- **✅ Résolution des ordinaux MFC : FONCTIONNE** — `WinMergeU.exe --with-dll mfc90u.dll=…` : ARET *« lifted 1 DLL module »* et les
  **imports de WinMerge passent de la forme ordinal à 893 imports nommés**. La frontière `0x80000471` (mfc90u ordinal) **tombe** avec
  le DLL — c'est exactement le mécanisme BYO-DLL (doc 80 §1.2).
- **✅ `mfc90u.dll` se lifte seul** — `aret mfc90u.dll --mode transpile` **émet** (200+ fichiers/chunks à t+50s) : ARET **encaisse un
  DLL MFC de 3,7 Mo**. Donc ni la résolution ni le lifting d'un gros DLL ne bloquent.
- **Reste = l'ÉCHELLE du blob 4-modules** : `WinMerge + mfc90u + msvcr90 + msvcp90` (~20k fonctions, ~15 Mo de code) — le transpile
  produit un out-dir **vide** (exit 0, seul le *note* imprimé). Les pièces marchent séparément mais la **fusion multi-modules à cette
  échelle** ne sort rien → à diagnostiquer (OOM ? early-return du chemin multi-modules géant ?). C'est le cœur du chantier **endgame
  M7-GUI**, multi-sessions.
- **⇒ La voie est prouvée ouverte** : redist MFC90 en main, ordinaux résolus, MFC liftable. Le dernier verrou est l'ingénierie
  d'échelle du pipeline multi-modules sur un vrai blob GUI complet (à mener en session focalisée).

### 2026-07-25 — [EMIT] **✅ Verrou d'échelle multi-modules LEVÉ (chunk par octets) — WinMerge+MFC90 compile en ELF natif et TOURNE jusqu'à l'init MFC**
- **Cause racine mesurée** : `emit_split` chunkait par **nombre de fonctions** (`chunk_size`/chunk). Les fonctions MFC sont si
  grosses qu'un chunk devenait une TU de **43 Mo / 462k lignes** (`chunk_49.c`) que **gcc -O0 n'arrive pas à compiler** (bloque ;
  189/199 `.o` faits, 2 `cc` qui tournent sans fin). Ce n'était **ni OOM ni analyse** (13 Go libres, émission OK) — purement la
  **compilation C** d'une TU géante.
- **Fix général & correctness-neutre** : cap par **octets** (`MAX_CHUNK_BYTES = 4 Mo`) en plus du cap par compte. Plus gros chunk
  **43 Mo → 4,6 Mo**. Une fonction seule > cap reste seule dans son chunk (inévitable ; ici max ~1,5 Mo, bien en-dessous). Petits
  binaires = un seul chunk (octets ≪ cap) ⇒ **hash `19acad982194bf07` inchangé**.
- **✅ Résultat mesuré** : `WinMergeU.exe --with-dll mfc90u.dll` **transpile + compile les 228 objets + linke un ELF natif de 141 Mo**
  en ~40 s (avant : illimité). Le binaire **tourne** : passe le démarrage CRT **et l'init MFC** (mfc90u lifté appelé), puis abort sur
  `indirect call to unrecovered 0x1111c718` = un pointeur **calculé/garbage** provenant probablement des **272 imports COM/GUI non
  implémentés** (weak-stub → pointeur bidon appelé). ⇒ **le pipeline endgame M7-GUI fonctionne bout-en-bout** ; le mur restant =
  la **surface COM/OLE + GUI** (HLE), pas l'échelle.
- **Reste** : (a) les imports COM/OLE/GUI (CoGetClassObject… 272) — data-driven, oracle Wine ; (b) le cas 4-modules (+msvcr90/p90)
  qui émettait 0 fichier = bug distinct à diagnostiquer (le 2-modules, lui, marche). Portes du fix chunking : à confirmer.

### 2026-07-25 — [EH] **Brick B SE DÉCLENCHE sur une vraie exception MFC (WinMerge) — backtrace : throw pendant l'init statique CRT**
- **Backtrace gdb du binaire natif WinMerge+mfc90u** (au moment de l'abort) :
  `main → sub_4ac1c2 → aret_initterm → aret_call → sub_4bb4b0 → sub_4ad95c → sub_4aab9e → aret_call → sub_6ac27c → sub_6acd6a →
  sub_6a20fd → aret_CxxThrowException → aret_cxx_dispatch → aret_cxx_call_handler → aret_call(0x1111c718) → abort`.
- **⇒ Le C++ EH de brick B S'EXÉCUTE sur un vrai binaire MFC** : pendant `_initterm` (constructeurs globaux CRT), un ctor global de
  **mfc90u lifté** (`sub_6a20fd`, dans la plage rebasée mfc90u >0x642000) **throw** → mon `aret_CxxThrowException`/`aret_cxx_dispatch`
  parcourt `fs:[0]`. Validation réelle de la machinerie EH, au-delà des fixtures.
- **Mur** : `aret_cxx_call_handler` appelle un handler `0x1111c718` — **au-dessus de tous les modules mappés** (0x400000–0x9c2000) ⇒
  **pointeur garbage** (mappé mais `!=0xB8`, donc `aret_call` → abort ; pas un thunk C++). Donc une frame de la chaîne `fs:[0]` porte
  un handler bidon.
- **Contexte mesuré** : WinMerge n'a **pas** `__except_handler3/4` ni `__security_cookie` en chaîne (CRT statique, noms strippés) mais
  **a** `__CxxFrameHandler3`, `_XcptFilter`, `_purecall` ; mfc90u utilise `_CxxThrowException`. Frames GS-protégées (VC9) probables.
- **Question ouverte (à trancher d'abord)** : le throw est-il **réel** (MFC throw+catch en interne pendant l'init) ou **spurious**
  (un miscompile dans le ctor lifté prend une branche throw à tort) ? Si réel → la chaîne `fs:[0]` a une frame dont le handler
  n'est pas récupéré/mal posé (setjmp inter-modules, `__except_handler4`, cookie GS). Si spurious → bug de lift en amont. **Prochain
  pas §2** : déterminer réel vs spurious (idéalement comparer à WinMerge sous Wine, mais GUI+display requis), puis corriger. C'est un
  sous-chantier EH-sur-vrai-MFC focalisé.

### 2026-07-25 — [EH][RECOV] **Trace décisive : le mur WinMerge = corruption d'état machine amont (esp/frame garbage), PAS un bug EH**
- **Trace `fs:[0]` au throw** (aret_cxx_dispatch, gaté `TEH`) : `throw pobj=1111c6fc pthrow=880ea0 fs0=1111c6fc` puis
  `frame=1111c6fc prev=8ae1e0 handler=1111c718 lead=00`. ⇒ **`fs:[0] == pobj == 0x1111c6fc`** et `handler == pobj+0x1c`.
  `0x1111c6fc` est **au-dessus de tous les modules** (0x400000–0x9c2000) = **adresse garbage**. `prev=0x8ae1e0` pointe, lui, dans
  mfc90u rebasé (frame réelle).
- **Conclusion (mesurée) : brick B fait le bon travail** — il dispatche un throw dont les entrées (objet, chaîne `fs:[0]`) sont
  **déjà garbage**. La frame SEH et l'objet lancé sont à une adresse corrompue ⇒ **corruption d'état machine EN AMONT** (un `esp`/
  frame parti en vrille pendant l'init MFC), pas un défaut du C++ EH. Le garbage handler `0x1111c718` est un **symptôme**.
- **Source probable, mesurée** : le blob complet = **39833 fonctions, 2956 partial-asm (non liftées → abort), 85 appels directs non
  résolus**. Une de ces fonctions non-liftées/non-résolues, appelée pendant l'init MFC, **corrompt `esp`/la pile** (retour sans
  maintenir esp, ou état bidon) → tout en aval (frame SEH, objet) atterrit en garbage.
- **⇒ Le vrai dernier verrou du endgame M7-GUI = la COMPLÉTUDE de lift/récup sur le blob MFC massif** (réduire les 2956 partial-asm
  + 85 appels non résolus qui corrompent l'état), pas l'EH ni l'échelle. Chantier de debug ciblé sur binaire 40k-fonctions
  (multi-sessions) : isoler LA fonction fautive (bisection : quel appel juste avant l'init corrompt esp). Trace retirée (diagnostic).

### 2026-07-25 — [EH] **Précision : l'objet lancé est un heap valide ; c'est `fs:[0]` qui est corrompu (pointe l'objet, pas une frame SEH)**
- `aret_malloc` = host `malloc` ⇒ `0x1111c6fc` est très probablement une **allocation heap valide** (l'objet lancé est **correct**).
- **Bug précis** : `fs:[0]` (teb[0], tête de la chaîne SEH) a été **mis = l'adresse de l'objet lancé** au lieu d'une frame de
  registration SEH (sur la pile). D'où `frame=pobj`, `handler=pobj+0x1c` (garbage), abort. La chaîne SEH est **corrompue** durant
  l'init MFC — un `mov fs:[0],reg` lifté qui a stocké la mauvaise valeur, OU une écriture parasite dans le TEB par une fonction
  mal-liftée (parmi les 2956 partial-asm / 85 appels non résolus).
- **Prochain pas §2 (session focalisée)** : instrumenter les écritures `fs:[0]` (l'injection SEH-establish) pour capturer **quand**
  teb[0] devient l'objet, remonter à la fonction fautive. C'est un bug de complétude/justesse de lift sur le blob 40k-fonctions —
  le dernier verrou du endgame M7-GUI. Ni EH, ni échelle : **complétude de récup/lift sur MFC massif**.

### 2026-07-25 — [EH] **Affinage : `fs:[0]` = l'objet lancé, tout aggloméré au SOMMET de la pile → chaîne SEH obsolète/parasite**
- **Trace bornes de pile + min-esp** (aret_call `g_aret_min_esp`, diag retiré) : `fs0=1111c6fc esp=1111c6f4 StackBase=1111c800
  StackLimit=1101c800 min_esp_seen=1111c714 used_at_throw=268 min_used=236`. ⇒ l'objet, la « frame » SEH et `esp` sont **tous
  dans les 268 octets sous le sommet de pile**, et **`fs:[0]` pointe l'objet lancé** (frame=pobj, handler=pobj+0x1c). (min-esp ne
  voit que les appels **indirects** — signal partiel — mais confirme l'agglomération au sommet.)
- **Mécanisme (précisé)** : `fs:[0]` (teb[0], tête de chaîne SEH) est un **pointeur obsolète/parasite** — soit un SEH-establish
  antérieur (`mov fs:[0],&reg`) **non défait au retour** (fs:[0] reste sur un slot de pile ensuite **réutilisé** pour l'objet
  d'exception), soit une **écriture parasite** dans le TEB par une fonction mal-liftée. Ni EH, ni échelle, ni appels indirects :
  **justesse de lift qui corrompt la tête de chaîne SEH** dans le blob MFC 40k-fn.
- **Prochain pas §2 focalisé (session dédiée)** : instrumenter **toutes** les écritures `fs:[0]` (rendu du Store fs dans
  structured.rs) + un garde d'intégrité sur teb[0] à chaque `aret_call`, pour capturer **la** fonction qui laisse/pose le
  `fs:[0]` obsolète. Cycle ~4 min ×N — chasse ciblée, à mener frais. Le pipeline endgame M7-GUI (scale/EH/ordinaux) est, lui,
  **fonctionnel** ; ce dernier verrou est un bug de justesse isolable.

### 2026-07-26 — [EH][ABI] **✅ VERROU fs:[0] de WinMerge RÉSOLU — `_EH_prolog3_GS` non reconnu (relocalisation esp non propagée) ; fix général, WinMerge franchit l'init MFC**
- **Chasse robuste (watchpoint matériel gdb, pas N rebuilds HLE)** — la « solution robuste et plus intelligente » demandée. Trois passes décisives sur le binaire natif WinMerge+mfc90u :
  1. **Watchpoint sur `teb[0]`** (toutes les écritures fs:[0] + `info symbol $pc`) : la **dernière** écriture avant le throw est
     `sub_8674c7+806` posant `fs:[0]=0x1111c6fc` (un slot **de pile**, pas un heap — mon affinage précédent « objet heap » était faux :
     0x1111c6fc ∈ [StackLimit 0x1101c800, StackBase 0x1111c800]).
  2. **Args cdecl le long de la chaîne** (binaire **32-bit** ⇒ args sur la pile, `uint64` = **8 o** chacun) : `sub_8674c7` est appelé
     **une seule fois**, par `sub_6acd6a`, avec `__esp=0x1111c704`, `v2(handler)=0x0086f42d` (un **vrai** thunk `__CxxFrameHandler3`).
     Il bâtit le nœud SEH à `esp-8=0x1111c6fc`, `node[+4]=handler=0x86f42d` — **bien formé à l'établissement**.
  3. **Watchpoint sur `node+4` (0x1111c700)** — 3 écritures : `#2 =0x86f42d` par `sub_8674c7+345` (établissement, **correct**) ;
     `#3 =0x1111c718` par **`sub_6a20fd+190`** (le ctor qui throw) qui **écrase** le handler avec un pointeur de pile *avant* de throw ;
     puis le throw lit ce handler corrompu → `aret_call(0x1111c718)` → abort. **Décisif.**
- **Cause racine exacte** (désassemblé à la vraie base rebase **0x650000**, calculée via le Security Cookie du Load Config) :
  `sub_8674c7` = **`_EH_prolog3_GS`** (helper CRT MSVC standard, /GS, C++ EH), qui **relocalise esp** : `push eax(handler); push fs:[0];
  lea eax,[esp+0xc]; sub esp,[esp+0xc]; …; mov ebp,eax; …; lea eax,[ebp-0xc]; mov fs:[0],eax; ret` — installe le nœud SEH à `esp-8` et
  laisse esp/ebp relocalisés. ARET **possède déjà** la machinerie (`frame_setup_helper_body` **inline** `_EH_prolog`/`_chkstk` pour
  propager la relocalisation esp que le modèle `__esp`-par-valeur ne peut pas transporter), **mais** son détecteur ne reconnaissait que
  `lea ebp,[esp+K]` **direct** — il **ratait** la forme `_EH_prolog3` (`lea eax,[esp+K]; mov ebp,eax` via un **temp**). Non inliné, la
  relocalisation esp ne se propageait pas au caller `sub_6acd6a` → ses frames en aval (dont `sub_6a20fd`) **chevauchaient** le nœud SEH →
  `node+4` (handler) écrasé → `fs:[0]` pointant un nœud corrompu → le throw (réel, MFC) trouve un handler garbage → abort. **Une classe
  entière** : tout binaire MSVC C++ à CRT statique (/GS) utilise `_EH_prolog3(_catch)_GS`.
- **Fix (général, doctrine-pure, `src/ir/build.rs`)** : `frame_setup_helper_body` suit désormais aussi la forme `lea R,[esp+K] (K>0); … ;
  mov ebp,R` (temp → ebp) en plus du `lea ebp,[esp+K]` direct. Réutilise **tout** l'inliner existant (`inline_frame_helper`). **Sûr par
  construction** : inliner un helper **branch-free terminé par `ret`** = exécuter son corps exact dans la SSA du caller (sémantique
  préservée) ; le gate `saw_rewrite` ne fait que **limiter** quels helpers on inline (pas de bloat des leaf-calls), et l'inliner **refuse**
  (fallback call normal) si un insn du corps ne lifte pas (pas d'`Asm` opaque). Le drop des temps clobbés évite tout faux-match.
- **✅ Résultat mesuré** : WinMerge+mfc90u **franchit le mur `0x1111c718`** — l'init statique MFC (ctors globaux, `_CxxThrowException`)
  passe, `fs:[0]` reste sain. Il avance dans du **code neuf** (`main→sub_864ff5→sub_864eda→sub_85fd18`) et bute sur un **mur de
  récupération distinct** : `jne short 0x0085fd5a` **non lifté** (cible non récupérée = data-en-code / partial-asm, la surface « 2956
  partial-asm » du blob, orthogonale à l'EH). **Borner puis pivoter** : c'est le prochain mur, plus profond.
- **Portes** : difftest **272/272**, transpile-diff **4/4** hash **`19acad982194bf07` inchangé** (fixtures sans `_EH_prolog3` ⇒
  byte-identique), funcdiff/winediff : à confirmer (en cours). Le fix est **gaté** (n'affecte que les call-sites matchant le nouvel
  idiome) ⇒ nul effet sur les binaires sans `_EH_prolog3`.

### 2026-07-26 — [RECOV][EH] **✅ Mur suivant WinMerge : les continuations de catch ne doivent PAS tronquer l'établisseur — récup, WinMerge avance dans l'init MFC profonde**
- **Mur mesuré** (après le fix `_EH_prolog3`) : `jne short 0x85fd5a` / `je short 0x85fd56` **non liftés** (`aret_unmodelled`) dans `sub_85fd18`,
  atteints en flot **normal** (init MFC, sans throw). Un `Jcc` tombe en `Asm` quand sa cible n'est pas un **leader de bloc** de la
  fonction (`build.rs` : `idx.get(succ)==None`).
- **Cause racine (la mienne, brick B)** : à `0x85fdb8` le binaire a `mov eax,0x85fd56; ret` = un **funclet de catch** qui retourne sa
  **continuation** `0x85fd56` (le code après le try/catch). `cxx_funclet_continuation` enregistrait cette continuation comme **entrée de
  fonction** → elle devient une **frontière** (`boundary`) qui **tronque** `sub_85fd18` à `0x85fd56` → `fd56`/`fd5a` (aussi cibles de `je`/
  `jne` internes, la continuation étant un point de reprise **dans le corps** de l'établisseur) sortent de la fonction → `Jcc` non résolus →
  abort. `sub_85fd56` apparaissait même comme fonction séparée (preuve du split).
- **Tension** : le runtime **résume** la continuation via `aret_call(cont)` (`aret_seh_run`, `g_seh_is_cxx`) ⇒ elle **doit** rester une
  fonction appelable ; mais elle est **aussi** un joint du flot normal de l'établisseur ⇒ elle ne doit **pas** tronquer celui-ci.
- **Fix (général, sound, `src/analysis/mod.rs`)** — sur le modèle du hot/cold split (`.cold` exclu de la `boundary`) : `cxx_eh_entries`
  retourne `(funclets, continuations)` séparément ; `analyze` **garde** les continuations dans la **liste de fonctions** (`func_entries` →
  bâties pour l'`aret_call` de reprise EH) **mais les exclut de la frontière de troncature** → l'établisseur **absorbe** la queue post-try
  comme **ses propres blocs** (ses `je`/`jne` internes résolvent). La queue partagée est **dupliquée** entre établisseur et fonction-
  continuation : **sound** (code identique). Les **funclets** (atteints seulement par le dispatch EH) restent des frontières réelles.
- **Byte-neutre hors C++ EH** : un binaire sans tables EH ⇒ `cxx_conts` **vide** ⇒ `boundary == func_entries` (comportement inchangé).
- **✅ Résultat** : `sub_85fd18` récupéré **entier**, les `je`/`jne` résolvent, WinMerge **franchit** ce mur et avance **bien plus loin**
  dans l'init MFC (`main→sub_4ac1c2→sub_4ad951→sub_6baf17→sub_6f0286→sub_6f0312→sub_6d9a17`), 5 imports HLE exercés en plus.
- **Portes toutes vertes** : difftest **272/272**, transpile hash **`19acad982194bf07` inchangé**, **ehdiff 6/6** (la reprise EH marche
  toujours — continuations toujours bâties), funcdiff **0 divergence** (20558, identique), winediff **177/178** (baseline).
- **Mur suivant (borné, à pivoter)** : `int3` dans `sub_6d9a17`, **juste après** `aret_CxxThrowException` (noreturn) → le `int3` est le
  marqueur *unreachable* du compilo. On l'atteint parce que le **dispatch du throw retourne** (au lieu de longjmp vers un catch ou d'aborter
  « unhandled ») : `aret_cxx_dispatch` retourne 0 (un handler a rendu 0) sur ce **vrai throw MFC imbriqué**. Prochain sous-chantier : brick B
  dispatch sur throw réel (pourquoi le catch n'est pas trouvé / handler rend 0). C'est un vrai throw pendant l'init MFC.

### 2026-07-26 — [EH] **Mur `int3` précisé : le throw MFC réel remonte au frame `_except_handler4` du CRT (statique), dont le worker lifté rend 0 → dispatch s'arrête**
- **Trace dispatch** (gdb, `aret_cxx_call_handler`) : le throw de `sub_6d9a17` fait **un seul** appel de handler : `handler=0x4ac555 lead=0x8b`
  (≠ `0xB8` ⇒ chemin `aret_call`, pas un thunk C++). Ce handler **rend 0** ⇒ `aret_cxx_dispatch` s'arrête (`==0 → return 0`) ⇒
  `aret_CxxThrowException` retourne ⇒ chute dans le `int3` *unreachable* qui suit le `_CxxThrowException` noreturn.
- **Identité de `0x4ac555`** (désassemblé) : `mov edi,edi; push ebp; mov ebp,esp; push [ebp+14]; push [ebp+10]; push [ebp+0c]; push
  [ebp+08]; push 0x4ac1cc; push 0x51e7ec (__security_cookie); call 0x4accf6; add esp,18; pop ebp; ret` = **`_except_handler4`**
  (SEH /GS du CRT statique), qui appelle `_except_handler4_common`. C'est le **frame SEH top-level du CRT** (autour de `main`/
  `_XcptFilter`) — la tête `fs:[0]` au moment du throw.
- **Lecture** : le throw MFC pendant l'init **remonte jusqu'au handler top-level du CRT** (aucun catch C++ intermédiaire ne l'attrape
  dans notre exécution headless). Deux sous-problèmes distincts : (1) notre `_except_handler4_common` **lifté** rend 0 au lieu de la
  bonne disposition (ContinueSearch=1) ⇒ le dispatch devrait soit continuer/épuiser → abort « unhandled C++ exception » **propre**, soit
  le throw devrait être attrapé plus tôt ; (2) c'est vraisemblablement une **exception réellement non gérée** (MFC échoue en init
  headless — surface GUI/ressources absente), donc même corrigé le programme ne dépasserait pas sans cette surface. **Sous-chantier
  brick-B-sur-vrai-MFC** (dispatch à travers un `_except_handler4` lifté du CRT statique) — multi-session, comme anticipé (doc 70 §5.0.4).
  **Borné ici** : les deux murs tractables de la session (relocalisation `_EH_prolog3` + split de continuation) sont corrigés et vérifiés.

### 2026-07-26 — [EH] **✅ Throw MFC = `CUserException` non gérée (limite headless) ; dispatch C++ corrigé (ne s'arrête que sur un catch), abort « unhandled » propre et typé**
- **Type lancé décodé** (ThrowInfo `0x880fa8` → CatchableTypeArray → TypeDescriptor) : **`CUserException`** (`.PAVCUserException@@`,
  dérive de CSimpleException→CException→CObject). MFC lance `CUserException` via `AfxThrowUserException` pour **abandonner une
  opération après avoir notifié l'utilisateur** (chemin `AfxMessageBox`→abort). En headless (MessageBox HLE rend -1, 272 imports
  COM/OLE/GUI absents), l'init MFC **échoue et throw**. `sub_6d9a17` (le throw) ≈ `AfxThrowUserException`.
- **Chaîne `fs:[0]` au throw = 1 seule frame** (gdb) : le handler top-level **`_except_handler4`** du CRT statique de WinMerge
  (`0x4ac555` : stub hotpatch `mov edi,edi` → push args+cookie+scopetable → `_except_handler4_common`), `prev=0xffffffff`. **Aucun
  catch C++ intermédiaire** ⇒ exception **réellement non gérée** dans notre exécution (= la limite headless-GUI, pas un bug de lift).
- **Fix général & sound (`runtime/aret_hle/aret_hle.c`)** : `aret_cxx_dispatch` **ne s'arrête plus** quand un handler *retourne* 0.
  Rationale : dans la recherche phase-1 d'un throw C++, **seul un catch transfère le contrôle (longjmp) et ne revient jamais** ; tout
  handler qui **retourne** (quelle que soit sa disposition) n'a pas attrapé → on **continue** de marcher la chaîne. L'ancien
  `if(handler==0) return 0` faisait qu'un `_except_handler4` lifté du CRT (rendant 0) **stoppait** la recherche → `aret_CxxThrowException`
  retournait → chute dans le `int3` *unreachable* du `_CxxThrowException` noreturn. Désormais : chaîne épuisée → **abort « unhandled C++
  exception » propre**, **typé** (`aret_cxx_thrown_name` décode le nom depuis la ThrowInfo). Message WinMerge : `unhandled C++ exception
  (type .PAVCUserException@@, ThrowInfo 0x00880fa8)` au lieu de `int3`. **Général** (tout binaire C++ EH), **sound** (un catch longjmpe
  toujours ; les fixtures ne rendent jamais 0 par un handler).
- **Portes** : **ehdiff 6/6** (catches transfèrent toujours), difftest **272/272**, transpile hash **`19acad982194bf07` inchangé** (change
  runtime, pas lift), winediff : à confirmer.
- **⇒ Bilan WinMerge (session)** : 3 murs francs — (1) corruption `fs:[0]` = `_EH_prolog3_GS` non reconnu **[corrigé]**, (2) split
  d'établisseur par une continuation de catch **[corrigé]**, (3) throw `CUserException` non géré = **limite headless-MFC** (abort propre
  désormais). WinMerge traverse tout le démarrage CRT + une **grande partie de l'init statique MFC** avant l'échec headless. Aller
  au-delà = la surface **COM/OLE/GUI/ressources** (endgame M7-GUI, multi-session). Les briques EH (B/C) sont prouvées sur du **vrai MFC**.

### 2026-07-26 — [EH][ORACLE] **CORRECTION mesurée : WinMerge tourne SOUS WINE (fenêtre ouverte) → la `CUserException` n'est PAS « non gérée par design », c'est un manque HLE en amont**
- **Oracle décisif** : `wine WinMergeU.exe` (wine-9.0, Xvfb :99, avec `mfc90u/msvcr90/msvcp90.dll` posés à côté de l'exe) **ouvre la
  fenêtre principale de WinMerge** (menus File/Edit/View/Tools/Plugins/Window/Help + toolbar + status bar « Ready » — capture d'écran).
  L'init MFC **réussit** sous un vrai Win32. ⇒ la `CUserException` qu'ARET lance en headless **n'est pas** un « unhandled by design »
  ni une barrière headless dure : c'est **une valeur HLE incorrecte en amont** (une API qu'ARET rend en échec là où Wine réussit) qui
  fait **abandonner MFC**. **Rectifie** ma formulation précédente (« limite headless-MFC ») : le chemin d'init EST complétable.
- **Ce qui reste vrai** : `_except_handler4` **n'est pas** le bloqueur (l'exception ne devrait pas être lancée du tout) ; les fixes de la
  session (`_EH_prolog3`, continuation, dispatch) restent justes ; l'abort « unhandled » typé reste le bon filet.
- **Ce qui change** : le prochain pas WinMerge est **traçable et data-driven** — identifier LA (les) API HLE dont le retour fait
  diverger MFC vers `AfxThrowUserException` (comparer l'exécution ARET vs Wine autour de l'init). C'est **tractable**, pas un mur dur.
  Confirme aussi un **oracle GUI bout-en-bout** utilisable (Wine+Xvfb+capture), et que WinMerge est un **bon driver** (pas une impasse).
- Détail méthode + plan d'industrialisation : nouveau **doc 81**.

### 2026-07-26 — [HLE][I5] **✅ Cause de la `CUserException` WinMerge trouvée & corrigée : 4 imports HLE manquants — WinMerge franchit l'init et avance (nouveau mur = hang MFC)**
- **Diagnostic sans traceur** (les impressions `unimplemented import` d'ARET ont suffi — I5 data-driven) : avant le throw, **5** imports
  touchés, dont le 5ᵉ (`_except_handler4_common`) est une **conséquence** (worker SEH appelé *pendant* le dispatch). Les **4 causes** :
  `_malloc_crt` (allocateur CRT — le stub faible rendait **NULL** ⇒ 1ʳᵉ alloc MFC échoue), `RegisterClipboardFormatW`, `memcpy_s`,
  `PathFindExtensionW`.
- **Oracle décisif** : `wine WinMergeU.exe` (+ redist) **ouvre la fenêtre** ⇒ ce ne sont pas des murs durs, juste des retours HLE
  incorrects. **4 shims implémentés, vérifiés bit-identiques Wine** (`bench/winecorpus/crt_secure_path.c` : `memcpy_s` Annex-K
  `0/ERANGE=34+dest zéroée/EINVAL=22`, `PathFindExtensionA/W` sémantique shlwapi Wine verbatim `. après reset '\\'/' '`,
  `RegisterClipboardFormatA/W` contrat d'atome `[0xC000,0xFFFF]` stable/unique — réutilise l'allocateur de `RegisterWindowMessage`).
  `_malloc_crt` family (`malloc/calloc/realloc/free _crt`) = host malloc.
- **✅ Effet mesuré** : la `CUserException` **disparaît**, WinMerge **franchit l'init** et **tourne** (18 s+ sans abort) — avance dans
  l'init statique MFC bien plus loin (`main→sub_864ff5→sub_864eda→sub_864caf→aret_initterm→sub_8800ed→sub_6abb49→sub_6ac573→sub_6ac51f`).
- **Nouveau mur (borné, à pivoter) : HANG** — backtrace **identique** à t=6/10/14 s ⇒ **boucle infinie** dans un ctor global mfc90u
  (`sub_6ac51f`/`sub_6ac573`, ce dernier établit un frame SEH `_EH_prolog3`). Spin-wait sur une condition qui ne change jamais en
  headless, **possible interaction** avec `_except_handler4_common` non implémenté (rend 0). Prochain incrément I5/I4.
- **Piège d'infra confirmé** : le runtime est `include_str!`'d dans le binaire aret (`builder/mod.rs`) ⇒ **`cargo build` obligatoire**
  après toute édition de `runtime/aret_hle/*.c` (sinon l'ancien runtime est ré-embarqué — j'ai perdu un cycle là-dessus).
- **Portes** : difftest **272/272**, transpile hash **`19acad982194bf07` inchangé** (runtime-only), fixture **bit-identique Wine**
  (manuel + CRLF-normalisé), winediff : à confirmer (en cours).

### 2026-07-26 — [LIFT][ABI] **Mur suivant WinMerge classifié : registre callee-saved (`esi`=`this`) entrant non threadé → store NULL → SIGSEGV réarmé en boucle (le « hang »)**
- **Ce n'est pas un hang, c'est une boucle de SIGSEGV** (gdb) : à `sub_6ac51f+243`, `movl $0x658a7c,(%eax)` avec **`eax` invalide**.
  Dans le C : `*(uint32_t*)(v7) = 0x658a7c` où **`v7` est un local `=0` jamais assigné** ⇒ store à NULL. `0x658a7c` (rebasé) =
  `0x789e8a7c` = un **pointeur de vtable** ⇒ `sub_6ac51f` est un **constructeur**.
- **Désassemblage réel** : `mov edi,edi; push ebp; mov ebp,esp; push [ebp+8]; lea ecx,[esi+0xc]; movl [esi],vtable; call ctor; and
  [esi+4],0; movb [esi+8],0; mov eax,esi; ret 4`. ⇒ **`this` arrive dans `ESI`** (registre **callee-saved**), lu **sans être sauvé
  d'abord** = une valeur d'ENTRÉE. C'est une convention **d'aide optimisée MSVC/MFC** (façon `__fastcall`, objet dans un registre
  non-volatil), pas un thiscall standard (`this` en `ecx`).
- **Cause racine (lift-ABI, générale)** : ARET thread `esp` (par valeur) et `ebp` (reg-param callee-saved) à travers les appels, **mais
  pas `esi`/`edi`/`ebx`** — en code standard ceux-ci sont save/restore (locaux), ils ne portent jamais de donnée **du** caller. Ici
  `sub_6ac51f` lit `esi` **entrant** comme donnée ⇒ ARET le voit à 0 ⇒ `this=0` ⇒ store NULL ⇒ SIGSEGV. Le **handler de faute**
  (`aret_hw_fault`) route vers SEH, ne trouve pas de vrai handler qui reprend, et **réexécute l'instruction fautive** → **boucle de
  faute infinie** (silencieuse = le « hang » observé 18 s+ sans sortie).
- **Deux items pour la suite** (chantier focalisé dédié, PAS en fin de longue session — changement de modèle cœur, risque de régression
  élevé) : **(1) [LIFT-ABI] threader les registres callee-saved (`esi`/`edi`/`ebx`) utilisés en live-in** (lus avant écriture/sauvegarde)
  comme `ebp` — étendre le jeu de reg-params + `aret_call`. Général (tout helper MSVC optimisé à objet-en-registre). Portes complètes
  obligatoires (difftest/cpudiff/funcdiff/winediff — touche le cœur). **(2) [SOUNDNESS] une faute matérielle sans handler qui reprend
  doit aborter BRUYAMMENT**, pas réarmer la même instruction à l'infini (un hang silencieux est un mauvais mode d'échec ; détecter la
  re-faute au même PC → abort). Item (2) est un filet indépendant et moins risqué.
- **⇒ Bilan WinMerge (session)** : de « abort `CUserException` en init » à « **tourne à travers une grande partie des ctors globaux
  MFC** » puis bute sur ce **bug de lift-ABI** (esi entrant). Les 3 fixes EH + les 4 shims HLE ont chacun fait avancer le driver, tous
  vérifiés vs Wine. Le prochain verrou est **lift-correctness** (le vrai « reste » du blob MFC 40k-fn), pas EH/imports.

### 2026-07-26 — [SOUNDNESS] **✅ I7 : une faute matérielle non résolue n'est plus un hang silencieux mais un abort BRUYANT (garde anti-boucle de reprise)**
- **Mécanisme du « hang » (précisé)** : store à NULL (bug I6 `esi=this=0`) → SIGSEGV → `aret_hw_fault` parcourt `fs:[0]` → trouve le frame
  `_except_handler4` du CRT → notre `_except_handler4_common` **non implémenté** (stub faible → **rend 0 = ExceptionContinueExecution**) →
  le dispatcher **réexécute l'instruction fautive** → refaute → **boucle infinie**. Le garde de ré-entrance (`depth>8`) ne se déclenchait
  pas car `depth--` à chaque reprise. Résultat = **hang silencieux** (18 s+ sans sortie) — le pire mode d'échec (§0 : échouer bruyamment).
- **Fix (`aret_hw_fault`)** : garde anti-no-progrès **keyé sur l'adresse fautive `si_addr`** (toujours présente dans `siginfo` ; `REG_EIP`
  nécessite `_GNU_SOURCE`, **non défini** ici — 1ʳᵉ tentative PC-based était un no-op, corrigée). Sentinelle `-1` = « pas encore de faute »
  (gère le NULL-deref `si_addr==0`). Même adresse **16×** de suite ⇒ **abort bruyant** avec code+adresse. Une vraie reprise qui **corrige**
  la cause avance (adresse suivante différente) ⇒ compteur remis à 0, jamais de faux positif ; seule une vraie boucle le déclenche.
- **✅ Vérifié** : WinMerge affiche désormais `aret: hardware fault 0xc0000005 at (nil) keeps re-faulting without progress
  (ExceptionContinueExecution loop) — aborting instead of hanging` — **loud + diagnostique** (pointe le NULL = le bug I6), au lieu de
  boucler. `seh_hwfault` (faute **attrapée** → `r=42`) **inchangé** (une faute gérée avance, ne boucle pas). Portes : difftest **272/272**,
  ehdiff **6/6**, transpile hash **inchangé** (runtime-only).
- ⇒ **I7 fait** (filet soundness indépendant). Le bug de fond reste **I6** (threader `esi/edi/ebx` callee-saved live-in) — chantier cœur
  dédié, désormais **signalé bruyamment** au lieu de hanguer.

### 2026-07-26 — [LIFT][ABI] **✅ I6 : threader les registres callee-saved `esi/edi/ebx` (live-in) — fixe le `this`-en-registre des helpers MSVC ; WinMerge dépasse le store-NULL**
- **Cause (rappel)** : certains helpers optimisés MSVC/MFC reçoivent un argument dans un **registre callee-saved** (`sub_6ac51f` : `this`
  dans **`esi`**, `mov [esi],vtable` sans sauvegarde). ARET ne threadait que `esp` + `eax/ecx/edx` + `ebp` ⇒ `esi` entrant lu à **0** ⇒
  store NULL. Le vrai reste du blob MFC = **lift-correctness ABI**, pas EH/imports.
- **Fix (modèle cœur, en synchronisation)** : la liste FIXE de reg-params passe de `[eax,ecx,edx,ebp]` à `[eax,ecx,edx,ebp,**esi,edi,ebx**]`
  (RegId `[0,1,2,5,6,7,3]`), threadés à **chaque appel** direct **et** indirect. Sites synchronisés : `ssa/mod.rs` (liste), `ir/build.rs`
  (`internal_call_args`/`internal_tailcall_args`), `emit/structured.rs` (signature forward 8 args), `emit/mod.rs` (auto via `reg_params`),
  `builder/mod.rs` (`emit_dispatch` : typedef `aret_fn`, décls `sub_`, adaptateurs host/IAT, corps `aret_call`), `aret_hle.h` +
  **17 sites** `aret_call` runtime (+`,0,0,0`), `emit/llvm.rs` (backend expérimental, aligné). **9-arg** `aret_call(va, esp, a,c,d,b, si,di,bx)`.
- **Sûr par construction (strictement additif)** : le code standard save/restore ses `esi/edi/ebx` ⇒ la valeur entrante threadée est **morte**
  (poussée/restaurée symétriquement) ⇒ comportement inchangé ; le caller garde son propre `esi` à travers l'appel (callee-saved, sa valeur
  SSA n'est pas réassignée). Seuls les helpers qui **lisent** le registre entrant en profitent.
- **Portes toutes vertes** : difftest **272/272**, **cpudiff 6/0**, **funcdiff 0 divergence** (20558, identique), transpile hash
  **`19acad982194bf07` INCHANGÉ** (les fixtures n'utilisent pas le pattern ⇒ comportement byte-identique ; le hash est comportemental),
  winediff (en cours, 0 régression). ⇒ **correctness-neutre sur tout le code testé**, additif.
- **✅ Effet WinMerge (preuve positive)** : le **store-NULL disparaît** (`sub_6ac51f` reçoit le vrai `this` en `esi`) ; plus de boucle de
  faute. WinMerge **avance** et bute sur une **faute différente/plus profonde** : `unhandled hardware exception 0xc0000005 at 0xe` — un
  **abort BRUYANT** (sound, via I7/le filet faute-non-gérée), pas un hang. Mur suivant = un accès `[ptr+0xe]` sur pointeur bas (bug de lift
  distinct, plus loin dans l'init MFC).
- **Note testabilité** : pas de fixture minimale (le pattern `this`-en-`esi` est spécifique MSVC, non émis par mingw ; l'inline-asm ne
  lifte pas proprement). Vérif = régression complète (aucun dommage) + WinMerge bout-en-bout (le bug disparaît = preuve positive).

### 2026-07-26 — [LIFT] **Mur WinMerge suivant (post-I6) : faute `0xc0000005 at 0xe` — champ SEH d'objet = 0xe (mop-up lift-correctness, multi-session)**
- **Trace gdb** : faute à `sub_7924d5` (deep MFC init, `main→sub_864ff5→…→aret_initterm→sub_87f251→sub_79195d→sub_7924d5`), instruction
  `mov (%eax),%ebx` avec **eax=0xe**. Le désassemblage amont montre le pattern du **check d'établissement SEH injecté** :
  `frame = *(obj+0x30); if (frame != -1 && frame != 0 && *frame == fs:[0]) { setjmp }` — mais `frame = *(obj+0x30) = 0xe` (garbage) passe
  le garde `!= -1 && != 0` et `*frame = *(0xe)` **faute**. La faute est un **abort BRUYANT** (I7/le filet faute-non-gérée), pas un hang.
- **Nature** : le champ **+0x30 d'un objet** (état thread/module MFC portant la frame SEH par-thread) vaut **0xe** au lieu de -1/frame
  valide. Objet lisible mais champ garbage ⇒ **bug de lift-correctness** distinct (pas le `this`-en-esi d'I6, déjà corrigé ; pas EH ni
  imports). Une écriture liftée a posé 0xe là où le programme réel écrit -1/une frame. Racine = dig individuel dans le blob 40k-fn.
- **Note soundness** : durcir le garde (valider que `frame` est un pointeur pile plausible avant deref) éviterait la faute ICI mais
  **masquerait** le bug (fs:[0]=0xe garbage relu plus tard) — contraire au §0. La faute bruyante actuelle est **correcte** (pointe le bug).
- **⇒ Inflexion stratégique (mesurée cette session)** : chaque fix (corruption fs:[0], continuation, dispatch, 4 imports, **I6 esi**, I7)
  fait **avancer** WinMerge et **révèle le bug de lift suivant**. C'est exactement le **mop-up lift-correctness** du blob MFC 40k-fn
  (doc 81) — **multi-session**, chaque bug = un dig (build→run→gdb ~10 min). L'**accélérateur documenté = le traceur I1** (doc 81 §I1) :
  voir la séquence d'instructions produisant la valeur garbage en **1 run** au lieu de digger manuellement. **Prochain pas recommandé =
  bâtir I1** (multiplicateur pour tout le mop-up), plutôt que digger le `0xe` isolément.
- **Acquis session** : WinMerge est passé de « abort dès l'init » à « traverse tout le CRT + une longue série de ctors globaux MFC »
  (7 incréments vérifiés, dont 2 changements de modèle cœur I6+I7). Portes toutes vertes. Le reste = mop-up (I1 pour l'accélérer).

### 2026-07-26 — [INFRA] **✅ I1 : traceur d'exécution (ring buffer, `ARET_TRACE=1`) — l'accélérateur du mop-up lift-correctness**
- **Quoi** : un **ring buffer en mémoire** (`aret_trace_buf[65536]`) qui enregistre **à chaque entrée de fonction** son état complet
  (`va, esp` + les 7 registres threadés `eax,ecx,edx,ebp,esi,edi,ebx` — gratuit, ce sont les params). **Dump uniquement sur un chemin de
  crash** (`aret_unmodelled`, faute matérielle non gérée, boucle de reprise I7) → les ~400 dernières entrées = la **chaîne d'appels + l'état
  registre** menant au crash. Zéro I/O sur le chemin chaud.
- **Comment (codegen gaté)** : ARET transpile vers du C, il n'interprète pas ⇒ le traceur est **émis par le backend** (`structured.rs`
  préfixe chaque corps d'`aret_trace_push(va, esp, regs…)`), **seulement** si `ARET_TRACE` est dans l'environnement
  (`builder/mod.rs` → `emit::set_trace`, flag thread-local comme `shared_stack`). Runtime : `aret_trace_push`/`aret_trace_dump`
  (`aret_hle.c`), déclarés dans `aret_hle.h`.
- **Off par défaut = byte-identique** : sans `ARET_TRACE`, **aucun** `aret_trace_push` émis ⇒ transpile hash **`19acad982194bf07`
  INCHANGÉ**, difftest **272/272**, ehdiff **6/6** (les hooks dans `aret_hw_fault` no-op si `head==0`). Purement additif.
- **✅ Vérifié** : fixture crash minimale (`inner→mid→main`, deref `0xe`) avec `ARET_TRACE=1` → dump la **chaîne d'appels avec l'état
  registre** de chaque frame (ex. `sub_40151c eax=0x3 esi=… ebp=…`). Exactement l'outil pour remonter une valeur garbage à sa source en
  **1 run** au lieu d'un dig gdb par bug.
- **Usage** : `ARET_TRACE=1 aret <exe> --mode transpile --out-dir OUT --run` → au crash, la trace s'imprime. Mono-fiber pour l'instant
  (buffer par-fiber = extension multi-thread). ⇒ Prochain : l'appliquer au mur `0xe` de WinMerge (voir quelle fonction pose
  `obj+0x30 = 0xe`), puis dérouler le mop-up MFC accéléré.

### 2026-07-26 — [ABI][LIFT] **Mur `0xe` de WinMerge : CAUSE RACINE trouvée (callee-pop d'un import `__stdcall` appelé indirectement, cross-block) — 1er fix REVERTÉ (régressait le lifting DLL), fix propre borné**
- **Diagnostic (gdb first-hand, sans traceur — le C lifté + 4 runs gdb ciblés ont suffi)** : la faute `mov (%eax),%ebx eax=0xe` à
  `sub_7924d5+27459` (mfc90u lifté) était le **check d'établissement SEH injecté** `frame = *[esp+0x30]; if (frame!=-1 && frame!=0 &&
  *frame==fs:[0]) setjmp` lisant un **local de frame** `[esp+0x30]` qui **aliasait un slot de pile obsolète** contenant `0xe`. **fs:[0]
  était sain** (`0x11b045ec`, pas 0xe) — l'hypothèse « fs:[0] corrompu » écartée par la mesure.
- **Chaîne de cause (décisive, prouvée)** : une watchpoint matérielle sur l'adresse exacte (`0x11b0458c`, déterministe) a capté le writer =
  un **`push 0xe`** (arg d'appel) à `sub_7924d5+12317`. Le lifté : `sub_7924d5` cache les couleurs système dans une boucle
  `push idx; call v39; store this+off` (~14×) où **`v39 = *(0x651950)`** = le **slot IAT de `GetSysColor`** (`__stdcall` @4). Le pattern est
  **register-indirect CROSS-block** (le load de `v39` est dans un bloc, les `call v39` dans la boucle d'autres blocs). `__aret_callee_pop`
  rend **0** pour la VA du slot ⇒ esp dérive **-4/appel** ⇒ le local SEH `[esp+0x30]` finit par pointer un vieux `push 0xe` ⇒ `*frame=*(0xe)`.
- **Cause racine (GÉNÉRALE)** : un import `__stdcall` appelé **register-indirect à travers les blocs** (`mov reg,[iat]` dans un bloc ; `call
  reg` dans un autre) n'est ni nommé ni poppé : la passe de nommage (`name_calls`/`held`, `build.rs` ~650) **remet `held` à zéro par bloc**
  (le threading en ordre-de-stockage laissait fuiter un mapping périmé), et le filet runtime `__aret_callee_pop` ne connaissait **pas** les
  slots d'import → pop 0 → dérive. Le cas **in-block** est déjà couvert (pop statique `stdcall_pop_for_regcall`) ; le cas **`call [abs]`
  direct** aussi (par nom). Seul le **register-indirect cross-block** driftait (le vrai bug de WinMerge).
- **1er fix tenté, puis REVERTÉ** (`40137b7`) : ajouter les slots d'import stdcall à `__aret_callee_pop` (table runtime). **Régression
  mesurée** : `bench/winecorpus/comctl32_imagelist` (lifting DLL comctl32) **cassait** (`indirect call to unrecovered 0x50441c`) — parce que
  `callee_pop_adjust` applique **déjà** le filet runtime à **tout** appel indirect, et le pop statique in-block **aussi** ⇒ **DOUBLE POP** sur
  les appels in-block de comctl32. Retirer le pop statique (2ᵉ essai) a inversé le problème : les slots d'import **fusionnés** de comctl32
  (résolus par `merge_modules`, absents de `prog.imports` tels quels) rendaient 0 ⇒ **sous-pop** ⇒ faute `0xc`. ⇒ **interaction complexe avec
  le chemin lifting-DLL** (multi-modules) : approche trop large. **Revert du code** (retour à `688bee0`), comctl32_imagelist **re-vert**
  (6 lignes correctes). Portes du 1er fix (avant découverte de la régression) : difftest 272/272, hash inchangé, cpudiff 5/0, funcdiff 0 div —
  **mais winediff a révélé la régression comctl32** (d'où l'importance de la porte winediff pour un changement touchant le dispatch d'import).
- **⇒ Leçon** : un changement du callee-pop **interagit avec 3 mécanismes existants** (pop statique in-block `stdcall_pop_for_regcall`, filet
  runtime `callee_pop_adjust`→`__aret_callee_pop`, pop par nom `call [abs]`) **et** avec la résolution d'imports **multi-modules** du lifting
  DLL. Le filet runtime `has_callee_pops`/`callee_pop_adjust` est **déjà** appliqué à tout indirect → y ajouter les imports **double-poppe**
  l'in-block. **winediff (fixtures DLL-lifting) est la porte qui l'attrape** — la lancer AVANT de conclure sur un changement d'ABI/import.
- **Fix propre borné (prochain incrément, à vérifier winediff COMPLET inclus)** : ne PAS toucher le filet runtime ni le lifting DLL. Étendre la
  seule passe de nommage `held` au **cross-block SÛR** : pré-scanner la fonction pour les registres **import-invariants** (assignés
  **exactement** depuis un unique slot d'import, jamais réécrits ailleurs — cas de `v39`), seeder `held` de ces invariants à l'entrée de chaque
  bloc → le pop statique in-block existant couvre alors le cross-block **sans** double-pop ni impact multi-modules. Sûr par construction
  (invariant = pas de mapping périmé possible). Chaque essai **doit** passer comctl32_imagelist **et** winediff complet **et** WinMerge.
- **Acquis** : la cause du mur `0xe` est **prouvée** (register-indirect stdcall-import cross-block), le fix est **cadré et borné**, et la
  branche est **revenue à un état correct** (aucune régression). Le mur `0xe` reste ouvert mais parfaitement caractérisé.

### 2026-07-26 — [ABI][LIFT] **✅ Mur `0xe` RÉSOLU proprement : dataflow MUST des registres porteurs d'import (cross-block) — WinMerge atteint l'init GUI de MFC**
- **Le bon endroit** (≠ 1ʳᵉ tentative, revertée) : ne PAS toucher au filet runtime (`__aret_callee_pop`) ni au lifting-DLL — corriger la
  **seule passe trop faible**, celle qui suit les registres porteurs d'un pointeur d'import (`held`, `build.rs`). Elle était **remise à zéro
  par bloc** : un `mov reg,[iat]` dans un bloc et un `call reg` dans un autre ⇒ appel **ni nommé ni poppé** ⇒ dérive esp de `@N`/appel.
- **Fix : `block_entry_imports()`** = **dataflow MUST avant** sur le CFG, qui calcule la carte `registre → import` **prouvée** en entrée de
  chaque bloc :
  - **meet = INTERSECTION** sur les prédécesseurs → un mapping ne survit que là où **tous** les chemins s'accordent sur le **même** import.
    C'est ce qui rend le cross-block **sound** et exclut précisément le piège documenté (`PeekMessageA` nommé `GetModuleHandleA`) qui avait
    fait abandonner le threading « en ordre de stockage ».
  - **transfert** = la fonction déjà utilisée en intra-bloc (`update_import_regs`) : tue le mapping sur toute autre écriture, sur les
    **clobbers ecx/edx** que le lifter émet à **chaque** appel (`lift.rs` : `Set{reg, Undef}`), et sur un `Asm` opaque. **Exhaustif** :
    `Set` est la **seule** écriture de registre pré-SSA (vérifié sur l'enum `Stmt`) ⇒ aucune écriture ne peut échapper au kill.
  - **init optimiste** (`None` = pas encore calculé, ne contribue à aucun meet) ⇒ un mapping établi **avant** une boucle **survit au
    back-edge** (exactement la forme de WinMerge : load hors boucle, `call reg` dans la boucle). Une init pessimiste (vide) aurait donné le
    plus petit point fixe et **raté** ce cas.
  - **racine ancrée par ADRESSE** (`func.entry`), pas par « sans prédécesseur » : quand le bloc d'entrée **est lui-même un en-tête de
    boucle** (forme réelle, cf. split pre-header SSA §4.1) il a un prédécesseur, et le semer depuis ce seul back-edge serait **faux**. La
    racine est épinglée à la carte **vide** (rien de supposé sur les registres venant de l'appelant) — et intersecter vide avec le back-edge
    reste vide, donc c'est aussi **exact**.
  - **terminaison** : une carte ne fait que **rétrécir** une fois calculée (domaine fini registres × imports) ; borne défensive `4n+16`
    dont le repli (tout-vide) **est exactement l'ancien comportement**. Le nommage ne tourne **qu'après** convergence (jamais sur un état
    intermédiaire).
- **Pourquoi pas de double-pop (le piège de la 1ʳᵉ tentative)** : le filet runtime `callee_pop_adjust` s'applique déjà à tout appel indirect,
  mais `__aret_callee_pop` **ignore** les slots d'import ⇒ rend **0**. Le pop statique in-block (`stdcall_pop_for_regcall`) ajoute `@N`.
  Total = `@N` **exactement**, pour l'in-block **comme** pour le cross-block désormais couvert. Aucun changement de la table runtime ⇒ **zéro
  impact sur la résolution d'imports multi-modules** du lifting-DLL (la cause de la régression précédente).
- **✅ Portes TOUTES vertes (confirmées sur le binaire final)** : **winediff 178/179, 0 FAIL** (= la référence exacte ; seul non-pass =
  `gdi_uifont` **environnemental**) — **la porte qui avait attrapé la régression précédente** ; **`comctl32_imagelist` MATCH** (les 6 lignes,
  testé en direct) ; difftest **272/272** ; transpile hash **`19acad982194bf07` INCHANGÉ** ; **cpudiff 5/0** ; **funcdiff 20558 scored /
  10808 opt, 0 divergence** (identique à la référence). ⇒ **correctness-neutre** sur tout le décompile/lift **et** sur l'axe OS-API.
- **✅ Effet WinMerge (preuve positive)** : le mur `0xe` **disparaît**. WinMerge franchit toute la série de ctors globaux MFC **et atteint
  l'init GUI de MFC** — il appelle maintenant `wcscat_s` et `SystemParametersInfoA` — puis **abort proprement (sound)** sur
  `SystemParametersInfoA: unmodelled action 0x29` (« refusing to guess », §0). ⇒ le driver est passé du **lift-correctness** à la **surface
  GUI/HLE** (= le chantier **I5** du doc 81, data-driven : combler l'action SPI + `wcscat_s`, chacun vérifié vs Wine).
- **Note testabilité** : pas de fixture minimale committée — le pattern (`mov reg,[iat]` puis `call reg` **dans un autre bloc**, sur un
  stdcall) est produit par MSVC optimisé, pas par mingw i686 (qui émet des `call [iat]` directs). Vérif = **portes complètes** (0 dommage,
  dont winediff qui exerce le lifting-DLL) **+** WinMerge bout-en-bout (le mur disparaît ; la cause `GetSysColor@4` avait été prouvée par
  watchpoint matérielle).

### 2026-07-26 — [HLE-WIN32][I5] **✅ `SPI_GETNONCLIENTMETRICS` + `wcscat_s` — les 2 API de l'init GUI MFC, bit-identiques Wine (mesurées, pas déduites)**
- **Contexte** : après le fix du mur `0xe` (entrée précédente), WinMerge atteint l'**init GUI de MFC** et bute sur ces deux API. Traitement
  **I5 data-driven** : mesurer sous Wine → reproduire à l'octet près → fixture de garde.
- **`SPI_GETNONCLIENTMETRICS` (action `0x29`)** — remplit `NONCLIENTMETRICS` (métriques non-client + les **5 polices shell** que tout
  framework lit au démarrage). **Trois pièges que la mesure a attrapés** et qu'une implémentation « raisonnable » aurait tous ratés :
  1. **A et W n'ont PAS le même layout** (`LOGFONTA` 60 o vs `LOGFONTW` 92 o) — or `aret_SystemParametersInfoW` **renvoyait simplement vers
     la version A** ⇒ il aurait écrit aux **mauvais offsets** (faux silencieux). Les deux chemins sont désormais séparés (`u32_spi(esp, wide)`).
  2. C'est le **champ `cbSize`** du **caller** qui sélectionne le layout, **pas `uiParam`** (mesuré : `uiParam=0` marche quand même). La
     taille **pré-Vista** (340/500) doit laisser `iPaddedBorderWidth` **INTACT** ; une taille inconnue rend **FALSE sans rien écrire**.
  3. Les valeurs **ne se déduisent pas** de notre `GetSystemMetrics` : Wine rend `SM_CYCAPTION`=**26** vs `iCaptionHeight`=**25**,
     `SM_CYMENU`=**19** vs `iMenuHeight`=**18**, `SM_CYSMCAPTION`=**18** vs `iSmCaptionHeight`=**17**. Les dériver = divergence silencieuse.
     Valeurs retenues (mesurées) : border 1, scroll 17/17, caption 18/**25**, smcaption 17/17, menu 18/18, padded 0 ; polices toutes
     **Tahoma**, `lfWeight`=400, `lfCharSet`=1, `lfHeight`=**-13** (caption) / **-11** (les 4 autres).
  - **La fixture compare TOUS les octets bruts** sur un tampon pré-rempli d'un **motif poison** — c'est ce qui a attrapé le dernier détail,
    invisible autrement : dans le chemin **A**, Wine n'écrit le nom de police **que jusqu'au NUL** et **laisse le reste du tableau tel que
    l'appelant l'avait**, en ne forçant que le **DERNIER** élément à 0 ; le chemin **W**, lui, **zéro-remplit** toute la queue. (Mécanique :
    A convertit le nom W→ANSI et n'écrit que la longueur convertie.) Les deux formes sont reproduites ⇒ **aucun memset global** du tampon.
- **`wcscat_s`** — les **7 cas mesurés** : `destsz==0` ⇒ EINVAL(22) destination **INTACTE** (≠ vidée) ; `src==NULL` ⇒ EINVAL(22) + `dest[0]=0` ;
  dest non terminée dans `destsz` ⇒ ERANGE(34) + `dest[0]=0` ; débordement ⇒ ERANGE(34) + `dest[0]=0` **mais les éléments déjà copiés restent
  écrasés** (effet réel visible, reproduit au lieu d'être idéalisé) ; ajustement exact (NUL sur le dernier élément) ⇒ **succès**.
- **Portes** : `winecorpus/user32_ncm.c` **+** `crt_wcscat_s.c` **bit-identiques Wine** (`ok` tous deux en winediff) ; **`user32_spi` toujours
  OK** (le plus exposé au refactor A/W) ; difftest **272/272** ; transpile hash **`19acad982194bf07` inchangé** (runtime-only) ; **winediff
  178/179 → 180/181** (les 2 nouvelles fixtures passent, seul rouge = `gdi_uifont` **environnemental**).
- **✅ Effet WinMerge** : franchit les deux murs et **avance dans l'énumération de polices** — nouveau mur = **`EnumFontFamiliesW`**
  (import non implémenté ; son stub faible rend 0, ce qui mène ensuite à un gap de lift statique `je short 0x00867436`). ⇒ prochain
  incrément I5 : `EnumFontFamilies(Ex)W` (API à **callback** — rappelle du code lifté avec `LOGFONT`/`TEXTMETRIC`, plus substantielle).
- **Note d'infra (piège rencontré)** : un `pkill -9 -f '\.exe'` antérieur avait tué `wineboot` en cours d'initialisation et **corrompu le
  prefix Wine** (`drive_c` absent ⇒ `wine: could not load kernel32.dll`). Réparation : `rm -rf ~/.wine && wineboot -i`. ⚠️ Ne jamais
  `pkill` large pendant qu'un oracle Wine tourne. (Corollaire déjà connu : ne pas toucher Xvfb pendant winediff.)

### 2026-07-26 — [HLE-WIN32][GUI][I5] **✅ `EnumFontFamilies(A/W)` — énumération de polices à CALLBACK (contrat bit-identique Wine ; liste = environnementale, assumée)**
- **Mur** : après les métriques non-client, WinMerge appelle `EnumFontFamiliesW` (ce que fait tout sélecteur de police). Le stub faible
  rendait 0 **sans appeler le callback** ⇒ « aucune police » ⇒ le programme partait ensuite dans un chemin qui butait sur un gap de lift.
- **Ce qui est EXACT vs ce qui est ENVIRONNEMENTAL** (la distinction structurante ici, doc 70 §4.5 / 72 §4.5) : la **liste** des familles et
  leurs métriques dépendent des polices installées (**399 familles** ici — et c'est **tout aussi environnemental sous Wine**) ⇒ **jamais
  bit-comparées**. Le **contrat**, lui, est déterministe et **reproduit exactement** :
  - un callback qui rend **0 ARRÊTE** l'énumération immédiatement **et la fonction retourne ce 0** (piège réel : ce n'est pas un compte) ;
  - une famille inexistante ⇒ **zéro callback** et retour **1** ;
  - `lpszFamily == NULL` ⇒ tout énumérer, retour **1** ; A et W se comportent pareil.
- **Données réelles, pas inventées** : la liste vient de **fontconfig** (la source que Wine utilise sous Linux), dédupliquée et **triée**
  (l'ordre de fontconfig n'est pas déterministe) ; les métriques de chaque face sont calculées par les **mêmes formules déjà vérifiées
  bit-exactes** que `GetTextMetrics` — `u32_fill_textmetric` a été **refactorisée** en `u32_tm_from_face(face, ascent, descent, …)` pour être
  réutilisée telle quelle. Une famille dont le fichier ne charge pas / sans table OS/2 est **sautée** plutôt que rapportée avec des métriques
  inventées.
- **Piège mesuré (aurait été un faux silencieux)** : `lfPitchAndFamily` ≠ `tmPitchAndFamily` — **pour toutes les polices** (mesuré : lf
  `0x22` vs tm `0x27`). Ils partagent le **nibble de famille FF_\*** mais les bits bas diffèrent : le **LOGFONT** porte la *demande de pas*
  (`VARIABLE_PITCH`=2 / `FIXED_PITCH`=1), le **TEXTMETRIC** porte les drapeaux `TMPF_*` (fixed-pitch/vector/truetype). Ma 1ʳᵉ version copiait
  l'un dans l'autre ⇒ les deux auraient été égaux, **divergence invisible sans la mesure**. Corrigé : `lf.pf = (tm.pf & 0xf0) | (fixe ? 1 : 2)`.
- **`@N` : vérité terrain, pas déduction** — `EnumFontFamilies` **manquait** à `stdcall_pops` (or c'est exactement la classe de bug qui
  faisait dériver esp, cf. entrée précédente). Décorations lues dans l'**import-lib mingw** (`nm libgdi32.a`) : `EnumFontFamiliesA/W@16`,
  `EnumFontFamiliesExA/W@20` — les 4 ajoutées (table triée, test `table_is_sorted` vert).
- **Gate FreeType élargi** (`builder/mod.rs`) : `EnumFontFamilies(Ex)A/W` déclenche désormais `-DARET_HAVE_FREETYPE` (l'énumération lit
  fontconfig et mesure avec FreeType, même socle que le texte). Sans lui l'implé abortait *sound* — c'est ce qui a produit `calls=0` au
  1ᵉʳ essai, symptôme diagnostiqué et corrigé.
- **Callback dans le lifté** : même mécanique que `u32_call_wndproc` (frame stdcall posée sous esp, `aret_call`). Les deux structures
  (`LOGFONT`+`TEXTMETRIC`) sont placées **entre l'esp de l'appelant et la frame du callback**, pour que la pile du callback (qui descend
  sous la frame) ne puisse pas les écraser.
- **Portes** : `winecorpus/gdi_enumfonts.c` **identique à Wine** (contrat + invariants en **booléens**, jamais en compteurs — un compte
  serait le nombre de polices installées, donc machine-dépendant) ; difftest **272/272** ; `table_is_sorted` OK ; transpile hash
  **`19acad982194bf07` inchangé** ; **winediff 180/181 → 181/182** (`ok gdi_enumfonts`, seul rouge = `gdi_uifont` environnemental).

### 2026-07-26 — [HLE-CRT][I5] **✅ `_wcsicoll`/`wcscoll` — « collate » ne veut PAS dire linguistique : mesuré ORDINAL en locale C (+ trou `setlocale` documenté)**
- **Mur** : après l'énumération de polices, WinMerge appelle `_wcsicoll`.
- **Le piège évité (le cœur de l'incrément)** : le nom dit *collate*, donc le réflexe est de brancher sur la machinerie de **sort-keys
  linguistiques** déjà en place (`lstrcmpiW`/`CompareStringW`, §4.5). **La mesure dit l'inverse** : en locale **« C »** — celle où est un
  programme **avant** tout `setlocale` — msvcrt collationne **ORDINALEMENT**. `_wcsicoll` ≡ `_wcsicmp` et `wcscoll` ≡ `wcscmp`, **13/13 cas
  identiques**, et **différents de `lstrcmpiW`** précisément sur les cas discriminants : `readme` vs `read-me` (**+1** ordinal contre **-1**
  linguistique), `~` vs `a` (**+1** contre **-1**), `O'Brien` vs `OBrien` (**-1** contre **+1**). Brancher sur le linguistique aurait donc
  produit l'ordre **inverse** sur exactement les cas pour lesquels cette machinerie existe — un faux silencieux.
- **Implémentation** : `aret_wcsicoll` = `aret_wcsicmp`, `aret_wcscoll` = `aret_wcscmp` (deux lignes, mais **prouvées**).
- **Fixture** `winecorpus/crt_wcscoll.c` : chaque ligne imprime le résultat de la fonction **à côté** de la réponse ordinale **et** de la
  réponse linguistique (`lstrcmpiW`), sur les paires **choisies pour discriminer** — donc toute régression qui rebrancherait sur le
  linguistique saute aux yeux. Signe seulement (la magnitude d'un compare CRT n'est pas spécifiée). **Identique à Wine**.
- **⚠️ Trou de soundness trouvé au passage (documenté, PAS corrigé)** : `aret_setlocale` rend **`"C"` pour n'importe quelle locale demandée**
  au lieu de **NULL** (échec) sur celles qu'on ne modélise pas. La valeur implémentée ici est donc **juste pour la locale C**, mais rien ne
  garantit qu'on y est : un programme qui sélectionne une vraie locale puis collationne diverge **en silence**. Consigné en **70 §P1bis**
  avec le fix propre (accepter `NULL`/`"C"`/`"POSIX"`, abort sound sinon) et la précaution : `""` est courant ⇒ **mesurer le corpus d'abord**
  (porte winediff complète) avant de trancher. Ne pas corriger à la volée en fin d'incrément.
- **Portes** : fixture **identique Wine** ; difftest **272/272** ; transpile hash **`19acad982194bf07` inchangé** (runtime-only) ; winediff
  complet + WinMerge en cours.

### 2026-07-26 — [LIFT] **✅ `jcc <autre_fonction>` = TAIL CALL CONDITIONNEL (au lieu d'un abort) — additif par construction**
- **Mur** (le premier de WinMerge qui ne soit **plus** un import : la remontée d'API est terminée, il ne reste que du lift) :
  `sub_867400` s'ouvre sur `je short 0x00867436`, et `0x867436` est **lui-même une fonction récupérée** (`sub_867436` est émise juste après).
  La cible n'étant pas un bloc de la fonction courante, `idx.get()` rend `None` ⇒ **tout le `jcc` dégradait en `Stmt::Asm`** ⇒ abort.
- **Le modèle existait déjà pour le cas inconditionnel** : un `jmp` qui sort de la fonction vers une adresse exécutable est lifté en
  **tail call** (`return f(args)`). Un `jcc` qui sort, c'est la même chose **sous condition** — idiome MSVC classique pour partager une
  queue commune/froide.
- **Fix** (`ir/build.rs`) : quand l'arête **prise** sort vers une adresse exécutable et que la **chute** reste interne, on branche vers un
  **bloc synthétique** ajouté après les blocs réels, contenant exactement le `Stmt::Return(tail_call(...))` du cas inconditionnel. Les
  indices sont réservés au moment du branchement (`order.len() + n`), les blocs synthétiques sont appendus **avant** le calcul des
  prédécesseurs (donc le CFG est cohérent), et ils **ne sont pas dans `idx`** (bâti sur les adresses des blocs réels) ⇒ rien d'autre ne peut
  les cibler par accident.
- **ADDITIF PAR CONSTRUCTION (la propriété qui rend le changement sûr)** : ce bras ne capture **que** des cas qui tombaient juste en dessous
  dans l'`Asm`/abort. Autrement dit il ne peut **que** transformer un abort en code modélisé — **aucun programme qui marche aujourd'hui ne
  change de comportement**. Confirmé par la mesure : **transpile hash `19acad982194bf07` INCHANGÉ**.
- **Portes toutes vertes** : difftest **272/272**, hash **inchangé**, **cpudiff 5/0**, **funcdiff 20558 scored / 0 divergence**, **winediff 182/183** (seul rouge = `gdi_uifont` environnemental ; les 6 fixtures comctl32 du lifting-DLL passent — celles qui avaient attrapé la tentative callee-pop revertée).
- **✅ Effet WinMerge (preuve positive, backtrace gdb)** : `sub_867400` **appelle réellement** `sub_867436` (frames #3→#2) — le `je` est
  désormais un vrai tail call conditionnel. WinMerge **avance dans `sub_867436`** et bute sur un **mur NOUVEAU et indépendant** : le garde de
  la **pile x87 runtime** (`__x87rt_ldi`→`__x87rt_at`→**`ud2`** = le `__builtin_trap` documenté §4.2 sur under/overflow).
- **⚠️ Observation soundness à traiter (prochain incrément candidat)** : ce garde `ud2` est **loud** (le process meurt) mais **muet** — aucun
  message, et la sortie stdio bufferisée est **perdue** (d'où un run « sans aucune sortie », trompeur : ce n'est pas une absence de
  progrès). Conforme au §0 sur le fond (pas de faux silencieux), mais **non diagnostique** : il devrait imprimer *quoi* a débordé (op,
  profondeur) sur stderr **avant** de trapper, comme `aret_unmodelled`. Petit, sans risque, gros gain de diagnostic.
- **Piège d'infra rencontré** : `/tmp` **plein** (plusieurs répertoires temporaires de 1,3 Go laissés par des runs de fixtures + les
  `wmg_out*`) ⇒ un winediff entier a rendu **104 FAIL « PE build: »/« dlltool: »/« windres: »** — **échecs de BUILD, pas de comportement**.
  Toujours vérifier la *nature* d'un FAIL avant de conclure à une régression : ici `df -h` suffisait. Nettoyage puis re-run.

### 2026-07-26 — [X87][SOUNDNESS] **✅ Le garde de pile x87 runtime était SOUND mais MUET — il diagnostique désormais avant d'aborter**

- **Le constat** (relevé en butant dessus avec WinMerge) : `__x87rt_at`/`__x87rt_psh` gardent la pile FPU modélisée par un
  `__builtin_trap()` **nu**. Le contrat §0 est respecté sur le fond — une pile incohérente **trappe** au lieu de lire un slot
  périmé — mais l'échec est **non diagnostique** de deux façons, dont la seconde est un vrai piège :
  1. **Aucun message** : rien ne dit *quoi* a débordé (quelle op, quelle profondeur, quel index).
  2. **La sortie du programme est PERDUE** : le trap tue le process avec `stdout` encore **bufferisé**. Un run qui avait en
     fait progressé très loin **paraît n'avoir rien produit du tout** — ce qui envoie chercher un échec précoce fantôme.
     C'est exactement ce qui m'est arrivé sur WinMerge (« aucune sortie » ⇒ fausse piste).
- **Preuve avant/après** (harnais `x87t` : `printf` puis underflow, stdout dans un **pipe** = pleinement bufferisé) :
  - **ancien** (`__builtin_trap` nu) → **AUCUNE sortie**, exit **132** (SIGILL). Le `printf` du programme est perdu.
  - **nouveau** → `PROGRAM OUTPUT BEFORE THE FAULT` **préservé**, puis
    `ARET: x87 runtime stack UNDERFLOW in st(i) access: requested st(0) at depth 0 (slot -1)` + l'explication, exit **134**.
- **Le fix** : `aret_x87_stack_error(op, i, depth)` (runtime HLE) — **`fflush(stdout)` D'ABORD** (préserver la preuve de
  jusqu'où on est allé), puis nommer op/index/profondeur (UNDERFLOW vs OVERFLOW distingués), puis `aret_trace_dump()`
  (la trace I1 si `ARET_TRACE=1`), puis `abort()`. **L'abort ne bouge pas** : seule la valeur diagnostique est ajoutée.
- **Deux points techniques qui rendent le changement sûr** :
  - **`__attribute__((noreturn))` obligatoire** : `__builtin_trap` l'est. Sans ça, le compilateur considère l'accès **hors
    bornes** placé après l'appel comme atteignable — on aurait remplacé un chemin terminé par un chemin UB. La sémantique
    reste **exactement** l'ancienne.
  - **Zéro nouvelle dépendance de lien** : vérifié **avant** d'écrire le code — `__x87rt_s`/`__x87rt_p` sont **déjà définis
    dans `aret_hle.c`**, donc tout programme atteignant ce garde lie déjà le runtime HLE. (Le commentaire de `__ix_diverr`
    rappelle que `__builtin_trap` était choisi pour n'avoir **aucune** dépendance de bibliothèque — la question méritait
    d'être tranchée par la mesure, pas supposée.)
- **Portes** : difftest **272/272**, transpile **4/4** hash **`19acad982194bf07` inchangé**, **winediff 182/183** (seul rouge
  `gdi_uifont`, environnemental) — la porte qui compte ici, puisque c'est un changement de **runtime** touchant tout programme.
- **Portée** : ne débloque rien en soi (l'abort reste un abort) — c'est un **investissement de diagnostic** sur le mur x87
  courant de WinMerge (`sub_867436`) et sur tous les futurs. Le §0 exige un arrêt **bruyant** ; un arrêt bruyant qui ne dit
  pas pourquoi respecte la lettre et rate l'intention.

### 2026-07-26 — [X87][LIFT] **✅ « la pile x87 est vide aux appels » était une HYPOTHÈSE, pas une preuve — le mur x87 de WinMerge tombe**

- **Trouvé grâce au diagnostic de l'incrément 8** (le garde muet ne l'aurait pas permis) : `UNDERFLOW, st(0) demandé à
  profondeur 0` — la pile FPU modélisée est **vide** et quelque chose lit son sommet.
- **Chaîne** : `sub_791ebc` → `sub_867400` → `sub_867436`. Identification du callee par son corps :
  `fld st(0)`, `fstp [esp+0x18]`, `fistp qword [esp+0x10]`, `fild qword`, comparaison, retour **edx:eax 64 bits** —
  c'est **`_ftol2`**, la conversion *float → `__int64`* de MSVC, **dont l'argument arrive dans `st(0)`, poussé par
  l'appelant** (`sub_867400` en est le dispatcher : teste le flag SSE2 en `0x8ba200`, ses **deux** branches lisent la
  pile x87 runtime). C'est ce que génère **tout cast `(__int64)` d'un flottant** en code MSVC.
- **CAUSE RACINE** : `sub_791ebc` calcule sa valeur en **x87 STATIQUE** (`__x87_ild32`/`__x87_add`/`__x87_div` = valeurs
  SSA, locales C) — **0 appel `__x87rt_*`** — puis appelle `_ftol2` qui, lui, a bailé et tourne sur le **filet runtime**.
  La valeur reste dans une locale C de l'appelant ; la pile runtime est vide. **Les deux mécanismes x87 ne communiquent
  pas dans le sens ARGUMENT** (le sens RETOUR, lui, a bien son pont : `__aret_x87_ret`/`__x87rt_pushret`).
- **L'hypothèse fautive, en toutes lettres dans le code** : le commentaire de la passe de profondeur disait *« the x87 ABI
  keeps the stack otherwise empty across calls, so this single push is the only adjustment a call needs »*. Vrai pour du
  code compilé normal, **faux** pour les helpers CRT qui prennent leur argument dans `st(0)`. Une hypothèse non vérifiée
  au cœur d'une zone correctness-critique = exactement ce que le **§0.4** interdit.
- **Fix (3 lignes de logique)** : **vérifier l'hypothèse au lieu de la supposer** — un `call`/`call` indirect atteint avec
  `sp > 0` (pile modélisée **non vide**) ⇒ **bail** de toute la fonction vers le filet runtime. Alors appelant **et**
  appelé partagent **une seule** pile et s'accordent **par construction**. **Conservateur et sound** : dans du vrai code
  compilé la pile x87 *est* vide aux appels, donc `sp > 0` ici est le cas rare du helper, pas le chemin courant.
- **Effet mesuré** : `sub_791ebc` bascule de **statique (0 op)** à **runtime (121 `__x87rt_`)** ; WinMerge **franchit
  entièrement le mur x87** et avance jusqu'au **chargement de polices GDI**, puis bute sur un mur **différent et plus
  profond** : `mov [0x8b5200], ss` (store de registre de segment, non lifté ⇒ abort correct).
- **Portes** : difftest **272/272**, transpile 4/4 **hash `19acad982194bf07` inchangé** (aucune fixture ne fait de cast
  `(__int64)` flottant), cpudiff, funcdiff, winediff.
- **Leçon réutilisable** : chercher dans le code les **commentaires qui affirment une invariante d'ABI** — ce sont des
  hypothèses non vérifiées en puissance. Celle-ci était écrite noir sur blanc depuis le début et tenait tant qu'aucun
  binaire ne convertissait un flottant en `__int64`.

### 2026-07-26 — [ABI][LIFT] **Mur WinMerge suivant CLASSIFIÉ : échec du cookie /GS dans `sub_791ebc` = DÉRIVE ESP (+ un oracle gratuit découvert)**

- **Symptôme** : après la chute du mur x87, WinMerge atteint le chargement de polices GDI puis abort sur
  `mov [0x8b5200], ss` — **site unique** (les milliers d'`outs`/`ins`/`push cs` sont le bruit connu de données
  décodées en code). La fonction contenante `sub_864955` capture **tous** les registres + **les six registres de
  segment** + EFLAGS vers un bloc global, gardée par un test sur `*0x8ad018` : c'est
  **`__security_check_cookie`/`__report_gsfailure`** (MSVC /GS). L'instruction non liftée n'est donc **pas le
  problème** — c'est le **chemin d'échec** qu'on n'aurait pas dû atteindre.
- **⭐ DÉCOUVERTE STRUCTURELLE RÉUTILISABLE — le cookie /GS lifté EST un contrôle d'invariance d'`esp`.** Dans le C
  généré, le prologue de `sub_791ebc` fait `[v22+0x470] = cookie ^ v22` et l'épilogue relit `[v609+0x470] ^ v609`.
  Le test ne passe donc **que si `v609 == v22`**, c'est-à-dire **si esp à l'épilogue == esp au prologue**.
  ⇒ **Tout binaire MSVC /GS embarque un détecteur de dérive esp gratuit**, placé par le compilateur à chaque
  épilogue protégé. C'est exactement la famille de bugs la plus coûteuse d'ARET (famille esp-drift : cksum, 7za,
  mur `0xe`), et on vient de découvrir qu'on dispose d'un **oracle par-fonction** pour elle, sans rien instrumenter.
  **À exploiter** : un binaire /GS qui atteint `__report_gsfailure` **prouve** une dérive esp dans la fonction
  appelante — meilleur signal que n'importe quel sweep statique.
- **Le modèle /GS est CORRECT** (mesuré, pas supposé) : `__security_check_cookie` est appelé **5 fois** dans ce run,
  **4 passent**. Seule `sub_791ebc` échoue. Ce n'est donc pas un défaut de modélisation du cookie mais une **vraie
  dérive esp** localisée.
- **Statut / honnêteté** : la dérive est dans du code **nouvellement atteignable** (avant l'incrément 9 on abortait
  plus tôt, dans `_ftol2`). **Non testé** : savoir si elle préexistait ou non — c'était inatteignable. Les portes
  disent qu'il n'y a pas de régression (funcdiff **20558 scorées / 0 divergence**, qui couvre précisément la
  justesse de lift/esp ; cpudiff 6/6 ; winediff 182/183), et l'incrément 9 ne touche **que** le choix du mécanisme
  x87, pas la modélisation d'esp — mais c'est un raisonnement, pas une mesure.
- **Prochaine étape cadrée** (session dédiée, méthode déjà éprouvée sur le mur `0xe`) : recompiler le C généré en
  **`-O0 -g`** (tip §7) **ou** poser une **watchpoint matérielle** sur le slot du cookie `[v22+0x470]`, remonter à
  l'écriture/au call qui décale esp. `sub_791ebc` est une grosse fonction MFC ; le décalage attendu est un multiple
  de 4 (pop manquant ou en trop). ⚠️ Piège rencontré : un `break sub_XXX` gdb tombe **après** le prologue hôte, donc
  `$esp+4` n'est **pas** l'argument `__esp` modélisé — les valeurs lues ainsi (`0x1`, `0x5`) sont du bruit.

### 2026-07-26 — [INFRA][ABI] **I1 braqué sur le mur /GS : `ARET_TRACE_DUMP=N`, et la dérive ramenée à EXACTEMENT 4 octets**

- **Réponse à « les techniques du 81 aident-elles ? » : oui, I1 est précisément l'outil.** Le traceur enregistre
  `aret_trace_push(va, **esp**, eax, ecx, edx, ebp, esi, edi, ebx)` à **chaque entrée de fonction** — donc pour une
  dérive esp il suffit de comparer l'esp d'entrée d'une fonction à celui de son épilogue.
- **Un affûtage a été nécessaire (et il est réutilisable)** : le dump était plafonné à **400** lignes alors que le ring
  en garde **65536**. `sub_791ebc` fait ~600 appels, donc **son entrée tombait hors fenêtre** — l'information existait,
  seul l'affichage la cachait. Ajout de **`ARET_TRACE_DUMP=N`** (0 = tout le ring). C'est exactement le « outils qui
  fabriquent les outils » du 81 §0 : le diagnostic était bloqué par une constante, pas par un manque de donnée.
- **Mesure obtenue en un run** : `sub_791ebc` entre à `esp=0x120ce5fc` ⇒ prologue `v22 = ((esp−4) & ~7) −4 −4 −4 −0x478`
  = **0x120ce174**. La dernière entrée de trace donne l'épilogue : `sub_864955 esp=0x120ce16c` ⇒ **v609 = 0x120ce170**.
  ⇒ **DÉRIVE = 4 OCTETS EXACTEMENT** (esp final **plus bas** de 4 : un push non dépilé / un pop manquant).
  ⚠️ **Erreur d'arithmétique corrigée en cours de route** : une première lecture donnait `0x1004` (4100) — je lisais
  l'esp d'un **run précédent** dont la base de pile différait. La trace doit être lue **dans le run où l'on calcule**.
- **`_ftol2` MIS HORS DE CAUSE** (les incréments 7 et 9 touchent cet appel, donc il fallait le vérifier) : `0x867400`
  et `0x867436` sont **absents de `aret_poptab`** = pop 0 = cdecl, ce qui est **correct** pour `_ftol2` (argument en
  `st(0)`, aucun argument pile). Le tail call conditionnel et la bascule x87 ne créent donc pas cette dérive.
- **Reste à faire** (session dédiée) : identifier lequel des ~597 appels de `sub_791ebc` perd 4 octets. Le traceur ne
  journalise que les **entrées**, pas les retours ⇒ **amélioration I1 suivante, ciblée et évidente** : journaliser aussi
  l'esp **au retour** de chaque appel (ou le couple entrée/sortie), ce qui rendrait la dérive **directement localisable**
  au lieu de demander une bissection. C'est la suite naturelle du chantier I1.
- **Portes** (changement runtime confiné à `aret_trace_dump`, chemin crash uniquement) : difftest, hash transpile, winediff.

### 2026-07-26 — [ABI][SOUNDNESS] **`__aret_callee_pop` : hypothèse posée puis ⚠️ CORRIGÉE par la mesure — ce n'était pas un trou**

- **Hypothèse initiale (publiée, puis corrigée)** : en traquant la dérive esp de 4 o de `sub_791ebc`, j'ai instrumenté les
  ratés de la table de callee-pop → **1095 ratés / run, 60 VAs distinctes, dont 55 « non récupérées »** — et j'en ai conclu
  trop vite à un trou de soundness général (0 deviné sur adresse inconnue, §0.4).
- **⚠️ MESURE DE CONTRÔLE — L'HYPOTHÈSE EST FAUSSE** : ces VAs sont des **slots IAT** (`aret_iat.c` en déclare **677** dans
  la plage `0x651xxx` de WinMerge). Or le **§4.3 documente que le design REPOSE** sur `__aret_callee_pop` rendant **0 sur un
  slot d'import** — c'est exactement ce qui **évite le double-pop** (le pop statique in-block fournit `@N` **une seule
  fois**). **0 y est donc voulu et correct.** Les 5 VAs restantes sont de vraies fonctions récupérées : absentes de la table
  = **cdecl**, 0 correct également. ⇒ **zéro instance nuisible mesurée.**
- **Ce qui subsiste (théorique, aucun cas observé)** : « absent de la table » ne distingue pas « récupérée et cdecl » (0
  **prouvé**) d'une adresse d'un **troisième type** — ni fonction récupérée, ni slot IAT. **À ne pas traiter
  spéculativement** : zone à haut risque (un 1ᵉʳ fix callee-pop a été reverté pour double-pop sur le lifting-DLL) et **aucun
  binaire ne l'exige**. Rouvrir seulement si une mesure exhibe une VA de ce troisième type. Cf. 70 §P1ter (réécrit).
- **Leçon (la vraie valeur de l'entrée)** : j'ai publié une cause à partir d'un **compteur** (« 55 non récupérées ») sans
  qualifier la **nature** des adresses comptées. Le compteur était juste, l'interprétation fausse. C'est le **même piège**
  que le winediff à « 104 FAIL » qui étaient des échecs de *build* : **toujours qualifier la nature d'un signal avant d'en
  tirer une cause** — et à plus forte raison avant de toucher une zone à haut risque.
- **Bénéfice net malgré tout** : deux pistes **éliminées proprement** pour la dérive de `sub_791ebc` — (a) l'appel virtuel
  suspecté est dans une branche **jamais exécutée** (`sub_6ae472` n'apparaît **pas une seule fois** dans la trace) ; (b) les
  ratés de pop sont **voulus**. La dérive est donc ailleurs, et le périmètre de recherche est réduit d'autant.
- **Piste restante, cadrée** : la chaîne esp exécutée donne `v555 = v514 + v515 + v531` et `v555 = 0x120ce160` (mesuré) ;
  il reste à remonter `v514` (statiquement, sans rebuild — la dérive est **figée dans le C**).

### 2026-08-01 — [LIFT-DLL][INFRA] **Pourquoi lifter shlwapi n'a RIEN débloqué : une DLL builtin Wine peut n'être qu'un RELAIS (règle mesurable avant de lifter)**

- **Symptôme** : le mur de WinMerge était `PathAddBackslashW`, exporté par **shlwapi**, disponible en builtin Wine. Levier 1
  appliqué (`--with-dll shlwapi.dll=…`) ⇒ transpile OK (695 Mo), **et le mur ne bouge pas** : toujours
  `ARET: unimplemented import CALLED: PathAddBackslashW`. Contradiction avec le résultat shell32 (qui, lui, avait effacé
  7 API d'un coup). Tant que ce n'était pas expliqué, « lifter plus de DLL » ne pouvait pas être recommandé comme stratégie.
- **Cause racine, PROUVÉE au désassemblage** (pas déduite) : le PE `WinMergeU.exe` **n'importe pas** `PathAddBackslashW` ;
  `mfc90u.dll` non plus ; **aucun** binaire livré avec WinMerge ne l'importe. C'est la **shell32 liftée** qui l'appelle —
  et l'export de shlwapi n'est **pas une implémentation** :
  ```
  10006120 <___wine_spec_imp_PathAddBackslashW>:
      8b ff  mov %edi,%edi ; 55 push %ebp ; 8b ec mov %esp,%ebp ; 5d pop %ebp
      ff 25 ec e9 03 10    jmp *0x1003e9ec        ; slot IAT → kernelbase.PathAddBackslashW
  ```
  Wine's shlwapi **importe** `PathAddBackslashA/W` **de kernelbase** (bloc d'import mesuré) et **réexporte** un thunk. Le
  loader multi-modules a donc parfaitement fait son travail : l'appel a bien été routé vers du **code lifté** — un thunk qui
  saute dans l'IAT d'un module **non chargé** ⇒ repli `aret_hle_shim_lookup` ⇒ pas de shim ⇒ `aret_unimpl`. **Le mur n'est
  pas tombé, il a reculé d'un module.** Le nom du symbole le disait : `__wine_spec_imp_`.
- **Règle générale qui en sort (mesurable en une commande, AVANT de payer un lift)** :
  `objdump -t <dll> | grep -c __wine_spec_imp_` vs le nombre d'exports nommés. Mesure sur les builtins Wine i386 :

  | DLL | exports nommés | thunks de réexport | verdict |
  |---|---|---|---|
  | comctl32 | 126 | **0** | implémente → lifter (prouvé : progress bar stateful) |
  | ole32 | 301 | **0** | implémente → lifter |
  | comdlg32 | 28 | **0** | implémente → lifter |
  | oleaut32 | 418 | 3 | implémente → lifter |
  | shell32 | 362 | 4 | implémente → lifter (prouvé : 7 API effacées) |
  | kernelbase | 1402 | 2 | **la vraie couche d'implémentation** |
  | advapi32 | 582 | **196 (34 %)** | relais partiel vers kernelbase |
  | **shlwapi** | 362 | **198 (55 %)** | **RELAIS — lifter ne rend rien** |
  | version | 16 | **12 (75 %)** | relais |
- **Où finit la chaîne** (mesuré) : `kernelbase` n'importe **que ntdll**, 417 fonctions = **131 `Nt*` (vrais syscalls = le
  mur, jumeau du mur win32k côté noyau)** + **212 `Rtl*`** (utilitaires user-mode, liftables) + 74 divers (CRT/`Ldr*`/`Tp*`).
  Donc la chaîne user-mode est **finie et énumérée** ; elle ne se dilue pas à l'infini, elle bute sur 131 syscalls NT.
- **Contre-mesure au compteur statique** (le 70 §5.0 avertissait déjà de ne pas arbitrer dessus) : `--mode imports` sur le
  programme fusionné, **0,086 s** par configuration, donne la trajectoire — 146 → 287 (mfc90u) → 410 (shell32) → 565
  (shlwapi) → 731 (kernelbase) imports non couverts. Le compteur **monte** à chaque lift parce qu'une DLL apporte ses
  propres imports ; il ne dit donc pas si le lift sert. **Ce qui le dit, c'est le ratio de thunks ci-dessus** — et lui se
  mesure sans rien construire.
- **Conséquence pour WinMerge** : la famille `Path*`/`Str*` réclamée est de la **manipulation de chaînes pure**
  (déterministe, sans état, oracle Wine trivial) ⇒ elle relève du **shim HLE** (méthode I5), pas du lifting. Lifter
  kernelbase pour l'obtenir échangerait 5 fonctions de chaîne contre 131 syscalls NT.
- **Vérifié** : désassemblage de l'export shlwapi, tables d'import/export `objdump -p/-t` des 9 builtins, `--mode imports`
  sur les 5 configurations, et le run WinMerge qui abort toujours sur le même nom.

### 2026-08-01 — [INFRA][SSA] **⭐ Le C généré n'était PAS déterministe — trouvé par le cache d'objets, cause = un `HashMap` itéré dans le placement des φ**

- **Comment c'est sorti** : en mesurant le cache d'objets (I9) sur WinMerge, la passe *warm* n'a réutilisé que **42 objets
  sur 255**. Attendu : ~255 (rien n'avait changé entre les deux runs — même binaire, même commande, même compilateur).
  Le cache ne mentait pas : les **sources différaient**. Comparaison des deux `--out-dir` :
  **212 des 254 `.c` générés diffèrent**, et l'ELF final diffère. Les **8 fichiers fixes** (`aret_hle.c`, `aret_crt.c`,
  `aret_win32.c`, `aret_dispatch.c`, `aret_iat.c`, `aret_stubs.c`, `aret_main.c`, `aret_layout.c`) sont, eux, identiques —
  donc le non-déterminisme est **dans le lifting**, pas dans l'émission de la couche fixe.
- **Cause racine** (`src/ssa/mod.rs`, `to_ssa`) : `defsites: HashMap<Location, Vec<usize>>` est **itéré**
  (`for (var, sites) in &defsites`) pour placer les φ. Le `HashMap` de Rust est **seedé aléatoirement par processus** ⇒
  l'ordre d'insertion des φ dans chaque bloc change à chaque run ⇒ le compteur de renommage distribue d'autres `ValueId`
  ⇒ tout le C change, et la copy-propagation ne folde pas les mêmes copies (mesuré : `v30 = v21;` d'un côté, **quatre**
  affectations de l'autre). Ce n'était **pas** un simple renommage cosmétique.
- **Ce que ça n'est pas** : un bug de justesse. Les deux numérotations sont du SSA valide et les deux programmes se
  comportent pareil — c'est exactement pourquoi **aucune porte ne l'avait vu** : le hash de `difftest_transpile` est
  **comportemental** (il hache la *sortie du programme*), donc il reste `19acad982194bf07` quelle que soit la numérotation.
- **Ce que ça est quand même** : « même entrée ⇒ même sortie » est une propriété dont le projet a besoin. Sans elle
  aucune porte **au niveau octet** ne peut exister, un diff de C généré entre deux commits est illisible, et un binaire
  livré n'est pas reproductible. Le 81 §0.2 exige déjà « déterminisme intact » pour l'instrumentation ; ici c'était le
  pipeline lui-même.
- **Fix** : `defsites` passe en **`IndexMap`** (déjà une dépendance, « Ordered maps for deterministic output » dit le
  `Cargo.toml`). L'ordre d'insertion — blocs dans l'ordre, statements dans l'ordre — est déterministe par construction,
  et coûte zéro. Audit des autres itérations de `Hash*` dans `ssa`/`opt` fait au passage : `undef` est itéré mais son
  résultat (`fp80`) est **trié+dédupliqué** ⇒ insensible à l'ordre ; `range` (`opt/frame.rs`) est un `BTreeMap` ; `safe`
  n'est jamais itéré. **`defsites` était le seul.**
- **Vérifié** : `sqlite3.exe` transpilé deux fois → **22/22 `.c` identiques et `app` bit-identique** ; et à la plus grande
  échelle disponible, **WinMerge + 3 DLL → 254/254 `.c` identiques et l'ELF de 172 Mo bit-identique** (avant le fix : les
  mêmes fichiers différaient). Portes : difftest **272/272**, hash **`19acad982194bf07` inchangé** (avec **et** sans
  cache — la porte est passée les deux fois pour que la preuve ne dépende pas du cache).
- **Leçon** : un outil construit pour une raison (aller plus vite) a servi d'**oracle** pour une propriété qu'aucune
  porte ne testait. Un cache adressé par contenu est, gratuitement, un **détecteur de non-déterminisme** — le taux de
  réutilisation attendu est une mesure, et l'écart à cette mesure est un bug.

### 2026-08-01 — [INFRA] **I9 — cache d'objets adressé par contenu : la boucle de dev cesse de repayer la même compilation**

- **Problème mesuré** (WinMerge + mfc90u + shell32 + shlwapi) : `--mode imports` 0,086 s · `--mode walls` 116 s ·
  **compilation des 254 `.c` générés (316 Mo) = 141 s** sur 4 cœurs. Or la boucle I5 réelle est *éditer un shim HLE →
  rebuild → relancer*, et sur cette édition **tous** les objets du code applicatif lifté sont bit-identiques au build
  précédent (le C lifté ne dépend pas des sources du runtime). Même gaspillage sur les **194 fixtures winediff**, qui
  recompilent chacune les mêmes `aret_hle.c`/`aret_crt.c`/`aret_win32.c` (~1,7 s de CPU par fixture).
- **Le point délicat = la soundness, pas la vitesse.** Un cache qui sert un objet périmé **est** le faux silencieux que
  le §0 interdit. Donc la clé n'est **pas** approximative : le premier build écrit sa liste de dépendances **`-MD`**, et
  toute réutilisation **re-hache chaque fichier listé** — headers générés **et** headers système — avant de servir
  l'objet. Header modifié, header système modifié, fichier supprimé : chacun échoue la vérification et retombe sur une
  compilation. Le cache ne peut échouer que **fermé** (travail en trop), jamais **ouvert** (mauvais octets).
- **Détails qui comptent** : les chemins internes à l'`--out-dir` sont stockés **relatifs** ⇒ un objet construit dans un
  répertoire est réutilisable depuis un autre (le cas qui compte : chaque expérience utilise un `--out-dir` neuf).
  **SHA-256 implémenté sur place** (aucune dépendance de hash dans le projet) et **prouvé sur les vecteurs FIPS 180-4** —
  un hash 64 bits serait une vraie façon de servir le mauvais objet. La liste `-MD` est écrite à côté de l'objet dans
  l'out-dir (noms uniques ⇒ deux threads rayon, ou deux fixtures winediff parallèles, ne peuvent pas s'écraser).
- **Réglages** : `ARET_NO_OBJCACHE=1` (off) · `ARET_OBJCACHE=<dir>` (défaut `$XDG_CACHE_HOME/aret/obj`) ·
  `ARET_OBJCACHE_MAX_MB` (défaut 4096, éviction LRU en fin de build). Une ligne `note: N object(s) compiled, M reused`.
- **Effet mesuré (après le fix de déterminisme — l'ancienne mesure ne mesurait que le bug)** :
  | | froid | chaud | objets réutilisés |
  |---|---|---|---|
  | **WinMerge + 3 DLL** (255 objets, 316 Mo de C) | **4 min 35** | **1 min 58** | **254 / 255** |
  | **winediff complet** (194 fixtures) | **6 min 25** | **3 min 56** | — (CPU total 10 min 22 → 3 min 08) |
  Le seul objet non réutilisé de WinMerge est `aret_layout.S` (l'assembleur n'est pas caché, un fichier minuscule).
  Verdict winediff **identique** (193/194) et les **194 lignes de fixtures byte-identiques** entre les deux runs.
- **Vérifié** : test dédié qui mesure les **deux sens** contre un vrai compilateur — un *warm lookup* sert des octets
  **identiques**, et un header modifié **rate**, avec la preuve que l'objet périmé aurait été **différent** ; plus le
  retour à l'ancien header qui re-touche l'entrée d'origine. + KAT SHA-256 + parsing des continuations `-MD`. 4/4.

### 2026-08-01 — [HLE-WIN32][I5] **Famille shlwapi `Path*` : 4 vagues, ~52 shims, et le passage du « mur par mur » au « par famille »**

- **Cible/symptôme** : après le levier-1 mesuré (entrée précédente), WinMerge enchaîne les murs `Path*` :
  `PathAddBackslashW` → `GetUserNameW` → `PathAppendW` → `PathFileExistsW`. Traités d'abord un par un, puis —
  **sur remarque de l'utilisateur, et conformément au 70 §5.0 Levier 0 qu'on n'appliquait pas** — en **vagues**.
- **Vague 1 — racine** (`win32_pathroot.c`, 25 chemins × 8 fonctions × A/W) : `PathIsUNC`/`IsRoot`/`IsRelative`/
  `SkipRoot`/`AddBackslash`/`RemoveBackslash`/`StripPath`/`RemoveFileSpec`. **4 réponses contredisent l'implémentation
  évidente** : (a) `/` **n'est pas** un séparateur pour cette famille (`PathIsUNC("//srv/sh")`=FAUX) alors qu'il l'est
  pour `PathFindFileName` **dans le même fichier** — incohérence réelle, ne pas « harmoniser » ; (b)
  `PathIsRoot("\\srv\sh")`=VRAI mais `PathSkipRoot` de la même chaîne = NULL ; (c) **aucun cas spécial `\\?\`** — il
  tombe de la règle « sauter deux composants » (`\\?\C:\x` → composants `?` et `C:` → racine à 7 ;
  `\\?\UNC\srv\sh\f` → `?` et `UNC` → 8). J'avais écrit une branche `\\?\` à la main ; **la 2ᵉ ligne l'a tuée** ;
  (d) `PathAddBackslash` sur 259 caractères **écrit quand même** (260 + NUL) : il déborde un tampon `MAX_PATH` au
  lieu de refuser.
- **Vague 2 — combinaison** (`win32_pathcombine.c`, 38 lignes) : `PathCanonicalize`/`PathCombine`/`PathAppend`, **un
  seul incrément parce que c'est une seule implémentation** (Append défère à Combine qui canonicalise). Mesures
  non déductibles : `"C:\a\."` **garde** son point final alors que `"\.\"` au milieu disparaît ; `"C:\a\...\b"` →
  `"C:.\b"` ; `"C:"` → `"C:\"` (le backslash est ajouté **à la fin**) ; `"C:a\..\b"` → `"\b"` (un lecteur **sans**
  séparateur n'est pas une racine : il est **perdu**) alors que `"C:\..\a"` → `"C:\a"` ; grimper dans un nom de
  serveur UNC est **refusé** (`"\\srv\.."` → `"\\srv"`, seul le séparateur final tombe). ⚠️ **Deux points mesurés
  plutôt que supposés, et j'allais me tromper sur les deux** : (1) j'allais **aborter** sur un `PathCombine` au-delà
  de `MAX_PATH` comme « non modélisé » — la mesure dit NULL + destination mise à `""` ; (2) un 2ᵉ argument UNC ne
  prend **pas** le chemin « racine du 1ᵉʳ + queue ».
- **⭐ La fixture a attrapé une divergence invisible à la chaîne** : ma 1ʳᵉ canonicalisation travaillait **en place**
  dans le tampon de l'appelant ; comme l'algorithme écrit puis recule, elle laissait des octets périmés **après le
  NUL**. Wine n'y touche pas (son entrée A convertit via un tampon large et n'écrit que le résultat). Deux lignes
  divergeaient sur **uniquement ces octets**. Construire en scratch puis copier corrige, et rend gratuitement sûr un
  `dst == src` aliasé. **3ᵉ fois** que le couple *tampon empoisonné + dump brut* trouve ce que la chaîne visible cache.
- **Vague 3 — extensions/composants** (`win32_pathparts.c`, 11 fonctions **prises en bloc**, vertes du 1ᵉʳ coup) :
  tout pivote sur `PathFindExtension`, dont la moitié surprenante est qu'un **espace** réinitialise le candidat —
  d'où `"x.exe arg1 arg2"` **sans extension** et un `PathAddExtension` qui appende à la ligne de commande entière.
  Un point **initial** compte (`".hidden"` a déjà une extension) et un point **final** aussi (`"file."`).
  `PathFindNextComponent` saute la **suite** de séparateurs (`"\\srv"` → 2, pas 1) ; `PathGetArgs` suit les
  guillemets (`"\"a b\" c"` → 6) ; `PathIsUNCServerShare` veut **exactement un** séparateur et se moque de ce qui
  suit ; `PathIsSameRoot` ignore la **casse** et ne compare que la racine. `PathStripToRoot` = la boucle de Wine
  (retirer le file-spec jusqu'à être une racine) → **réutilise** les fonctions de la vague 1, donc les trois
  s'accordent **par construction** au lieu de trois transcriptions séparées.
- **Vague 4 — filesystem** (`win32_pathexists.c`) : `PathFileExists`/`PathIsDirectory`, placées dans `aret_hle.c`
  près de `translate_path`/`aret_attr_named` pour partager **une seule** réponse à « que nomme ce chemin ».
  **3 réponses qu'un simple wrapper `stat()` rate** : `PathIsDirectory` rend **`FILE_ATTRIBUTE_DIRECTORY` (0x10)**,
  pas 1 (un `== TRUE` prend la mauvaise branche en silence) ; un **joker** est `ERROR_INVALID_NAME` (123), pas
  « absent » ; le chemin **vide** est `ERROR_PATH_NOT_FOUND` (3) là où `stat("")` donne ENOENT (2), et un chemin
  **NULL** rend FAUX **sans toucher** au last-error. La fixture **crée** ce qu'elle interroge puis le supprime et
  ré-interroge, donc les réponses ne peuvent venir ni du contenu de l'hôte ni d'un cache.
- **Méthode, deux fois** : grille **élargie en cours de dérivation** plutôt que raisonner par-dessus un trou — une
  ligne `\\srv\` a tranché `PathIsUNCServerShare`, une paire ne différant que par la casse a tranché `PathIsSameRoot`.
- **Non livré volontairement** (et c'est le point de l'incrément, pas un trou) : `PathIsUNCServer` (Wine se
  contredit entre A et W) et `PathCommonPrefix`/`PathIsPrefix` (11 paires laissent la règle ambiguë) — **abort**
  plutôt qu'une règle devinée. Tous deux **tranchés ensuite par l'oracle Windows** (entrée suivante).
- **Vérifié** : 5 fixtures bit-identiques Wine, audit stdcall PASS (`@N` depuis les import-libs mingw, table 905),
  hash `19acad982194bf07` inchangé. **Effet WinMerge** : 4 murs franchis dans la session.

### 2026-08-01 — [HLE][SOUNDNESS] **`GetUserNameA/W` — et l'abort a payé dès le premier run**

- **Fix** : le nom vient de la **même source que Wine** (le compte Unix de l'hôte), donc les deux moteurs lisent une
  seule vérité et la fixture **compare le nom** au lieu de le sauter comme environnemental. Rien n'est inventé : si
  l'hôte ne fournit aucun nom, on **aborte** au lieu de répondre un substitut plausible.
- **⭐ Et c'est exactement ce qui a servi** : ma 1ʳᵉ version lisait `$USER`, puis `$LOGNAME`, puis `getlogin()` — ce
  qui marche dans un shell interactif et **échoue dans le processus fils de winediff** (pas d'environnement, pas de
  terminal). La fixture est revenue en **abort nommant la cause exacte**, au lieu d'une divergence silencieuse à
  traquer. La correction est l'**ORDRE** des sources, pas seulement l'ensemble : la base **passwd d'abord**, comme Wine.
- **Contrat de taille, mesuré sur tampon empoisonné** : `*pcb` = taille requise **NUL compris**, en succès **comme**
  en échec ; un tampon trop court d'un seul caractère échoue avec `ERROR_INSUFFICIENT_BUFFER` et laisse le tampon
  **totalement intact** (aucun nom tronqué écrit — seul le dump brut le prouve) ; demander exactement la taille
  rapportée réussit. Octets pour A, caractères pour W.
- **Chaque sonde remet le last-error à zéro d'abord** : ce n'est pas de la prudence gratuite, c'est le piège qui avait
  produit une conclusion publiée fausse sur la famille SPI. Conçu pour ne pas se reproduire plutôt que redécouvert.

### 2026-08-01 — [INFRA][ORACLE] **⭐ UN VRAI ORACLE WINDOWS (GitHub Actions) — la circularité du 70 §1 n'est plus un argument, elle est mesurée**

- **Origine** : idée de l'utilisateur (« et départager via GitHub Actions ? »). Toutes les portes comparent ARET à
  **Wine** ; le 70 §1 enregistre depuis toujours la faiblesse honnête *« si Wine est à la fois l'oracle et
  l'implémentation, on vérifie Wine contre Wine »*. Un runner `windows-latest` **casse le cercle** : c'est le Win32
  contre lequel les binaires d'origine ont été construits.
- **Livré** : `.github/workflows/windows-oracle.yml` (MSVC **32 bits** via `vcvars32` — donc l'ABI, la largeur de
  `wchar_t` et les layouts sont ceux qu'ARET vise) + `bench/winoracle/` (sondes + `wine_hashes.sh` + README).
- **Choix de conception, délibérés** :
  - **Ce n'est PAS une porte.** Elle produit des **mesures** (log + artefact) qu'une session lit puis encode. Une
    divergence Windows/Wine est un **constat à instruire**, pas un rouge à faire taire — et une porte qui rougit
    pour des raisons que personne ne doit corriger par réflexe est **pire qu'aucune porte**.
  - Les sondes vivent dans `bench/winoracle/`, **pas** dans `winecorpus/`, précisément parce que winediff compare à
    Wine et qu'ici **Wine est le suspect**.
  - Comparaison du corpus **en deux temps** : une **empreinte** `nom statut sha256` par fixture (le runner) que
    `wine_hashes.sh` reproduit **localement sous Wine** — les fixtures dont l'empreinte diffère **SONT** le constat ;
    le détail complet n'est imprimé que pour celles-là. Les critères d'éligibilité sont **dupliqués à l'identique**
    des deux côtés (sinon on diffe deux ensembles différents) et les **skips sont rapportés** : un ensemble qui
    rétrécit en silence ressemblerait à un ensemble de problèmes qui rétrécit.
- **⭐ Constat n°0, avant même de mesurer quoi que ce soit : le dépôt était INCLONABLE sous Windows.** Le checkout
  meurt sur `error: invalid path ':eoy'` — un fichier **vide** nommé comme une coquille de redirection shell
  (`2>:eoy`), commité par accident. NTFS réserve le `:` (flux alternatifs), donc git **abandonne tout le checkout**,
  pas seulement cette entrée. Passé inaperçu parce que **toutes** les portes tournent sous Linux. *(Le retirer
  demande `git rm ':(literal):eoy'` — un `':eoy'` nu est interprété comme de la magie de pathspec et ne matche rien.)*
- **Constat n°1 — deux divergences sur les QUATRE premières fixtures**, sur du comportement **déjà livré** et
  **déjà vert** :
  - `PathAddExtension(chemin, NULL)` : Windows appende `.exe` et rend VRAI ; Wine rend FAUX sans rien changer.
    MSDN documente le NULL comme signifiant `.exe` ⇒ **bug de Wine**, qu'ARET reproduit parce que Wine était le seul
    oracle au moment de la vague 3.
  - `PathFileExists("f.txt\")` : Windows pose `ERROR_DIRECTORY` (267), Wine `ERROR_PATH_NOT_FOUND` (3). Le booléen
    est **identique** des deux côtés ; seul le code d'erreur diverge. ⚠️ Le mapping `ENOTDIR→3` est écrit à **trois
    endroits** et alimente aussi `GetFileAttributes(Ex)A/W` : **portée potentiellement plus large, NON mesurée** —
    hypothèse à trancher par la sonde, pas à affirmer.
  - **Gravité honnête** : aucune des deux n'est de la classe que le §0 vise en premier (donnée fausse présentée comme
    juste). La 1ʳᵉ rend un **échec** là où un succès était dû ⇒ casse **visiblement**. La 2ᵉ ne change qu'un code
    d'erreur secondaire. **Ce qui compte n'est pas leur gravité, c'est le TAUX** : 2 sur 4 fixtures. Les suivantes
    peuvent tomber dans une classe qui, elle, produit du faux silencieux (taille de structure, longueur retournée,
    ordre de tri).
- **Constat n°2 — les deux questions laissées ouvertes par la vague 3 sont tranchées** :
  - `PathIsUNCServer` : Windows répond **A ≡ W** et donne raison au **W de Wine**. Le A de Wine rend FAUX pour
    **toutes** les entrées ⇒ **bug confirmé**. Nos deux entrées implémentent la règle Windows (« préfixe `\\` et
    aucun autre séparateur »). **Conséquence de porte** : notre A **diverge volontairement de Wine**, donc seule la
    colonne **W** est comparable en winediff ; l'en-tête de la fixture le dit explicitement pour que personne ne
    « corrige » plus tard en réalignant sur Wine — c'est précisément le piège qu'un oracle Wine-seul tend.
  - `PathCommonPrefix` : il fallait des paires où les deux lectures candidates prédisent des nombres **différents** ;
    l'ancienne grille n'avait que des paires où elles **coïncidaient**, d'où onze lignes qui ne prouvaient rien.
    Règle Windows : chaînes identiques → **longueur entière** (`C:\a`/`C:\a` → 4) ; sinon index du dernier
    séparateur commun, **exclu** (`C:\a\b`/`C:\a\c` → 4) ; **sauf** le séparateur de la racine de lecteur, conservé
    (`C:\aa\b`/`C:\ab\b` → 3) ; et un UNC n'a **pas** cette exception (`\\s\h`/`\\s\i` → 3). `PathIsPrefix` en découle.
  - **Résultat NÉGATIF utile** : gatées sur **exactement les mêmes paires** que le runner, ces deux fonctions
    reviennent bit-identiques sous Wine ⇒ **Wine et Windows s'accordent** dessus. Sur toute la famille, la seule
    divergence est `PathIsUNCServerA`. L'oracle Wine se trompe **rarement** — et on sait maintenant **où**.
- **Piège d'infra à connaître** : le workflow déclenche sur `paths:`, donc le commit qui a *réellement* débloqué le
  checkout (retrait de `:eoy`) n'a **rien relancé** ; il a fallu toucher une sonde. Et `workflow_dispatch` via l'API
  répond **403** avec le jeton de session — le push reste le déclencheur.
- **Vérifié** : run vert, 5 fichiers d'artefact, verdicts encodés, winediff `win32_pathparts` toujours bit-identique.

### 2026-08-01 — [HLE-COM][I5] **1ʳᵉ interface COM (`CoGetMalloc`/`IMalloc`) — et le « mécanisme de vtable » n'existait pas**

- **Mur** : après la famille `Path*`, WinMerge bute sur `ole32.CoGetMalloc` en delay-load. C'est le premier appel qui
  rend une **VTABLE** que le programme appelle ensuite — le « mécanisme de vtable COM » que le 70 §5.0 cadrait comme
  un chantier.
- **Il n'y en avait pas.** Le résolveur delay-load distribue déjà des **VA synthétiques** (`DELAY_VA_BASE`) que
  `aret_call` redispatche vers le HLE ; **une vtable, c'est neuf de ces VA**. Zéro machinerie nouvelle. Le chantier
  annoncé était le problème du delay-load sous un autre visage.
- **⚠️ La règle « thunk » du matin était INCOMPLÈTE, et elle m'aurait fait payer un lift inutile.** Elle comptait les
  `__wine_spec_imp_` et classait `ole32` à **0 thunk = implémente**. Faux : `ole32` a **133 forwarders PE** sur 301
  exports, et `ole32.CoGetMalloc` en est un, droit vers `combase`. **Thunks et forwarders sont deux mécanismes
  distincts de réexport** ; la métrique n'en voyait qu'un. Table corrigée, deux colonnes :

  | DLL | exports | thunks | forwarders | verdict |
  |---|---|---|---|---|
  | comctl32 | 126 | 0 | 31 | implémente |
  | comdlg32 | 28 | 0 | 0 | implémente |
  | oleaut32 | 418 | 3 | 0 | implémente |
  | shell32 | 362 | 4 | 36 | implémente |
  | kernelbase | 1402 | 2 | 92 | implémente |
  | combase | 345 | 0 | 0 | implémente (le vrai COM) |
  | **ole32** | 301 | **0** | **133** | **RELAIS (44 %)** |
  | shlwapi | 362 | 198 | 217 | RELAIS |
  | advapi32 | 582 | 196 | 30 | RELAIS |
  | version | 16 | 12 | 2 | RELAIS |

  ⇒ **la commande de contrôle devient** : `objdump -t X.dll | grep -c __wine_spec_imp_` **ET**
  `objdump -p X.dll | grep -c 'Forwarder RVA'`, rapportés aux exports nommés.
- **Décision mesurée** : `combase` traîne rpcrt4 (30), ucrtbase (22), kernel32 (55) **et une dépendance circulaire
  vers ole32**. Contre ça, `IMalloc` = **9 méthodes** dont 3 déjà écrites (`CoTaskMemAlloc/Realloc/Free`). Écrit à la
  main, sans hésitation.
- **⭐ Trois lignes mesurées contredisent le contrat COM documenté** — la raison même de sonder plutôt que raisonner :
  (a) `QueryInterface` **n'AddRef PAS** (trois QI réussis laissent le compteur à 1) — j'avais écrit l'AddRef **avant**
  de mesurer, et j'ai ajouté une ligne à la sonde **parce que les lignes existantes ne le discriminaient pas** ;
  (b) `QueryInterface` d'une interface non supportée rend `E_NOINTERFACE` et **ne touche pas** au paramètre de sortie
  (COM exige de le mettre à NULL) ; (c) le compteur de références est **réel** (AddRef→2, Release→1) sur un singleton
  jamais détruit. Résultat positif utile : un bloc `CoTaskMemAlloc` est **connu d'IMalloc** et libérable par lui —
  **un seul allocateur**, pas deux.
- **`GetSize`/`DidAlloc` : table latérale, pas en-tête de taille.** Deux raisons qui comptent toutes les deux : un
  en-tête changerait le pointeur rendu par `CoTaskMemAlloc` (un programme qui le mélange avec `free()` corromprait),
  et `DidAlloc` doit répondre pour un pointeur qu'on n'a **pas** alloué — y lire un en-tête, c'est déréférencer un
  pointeur étranger, donc pouvoir fauter. La table ne lit **que sa propre mémoire** : un argument hostile obtient un
  « non » correct, jamais un crash. ⚠️ La suppression **réinsère la chaîne de sondage derrière le trou**, sinon une
  recherche ultérieure saute une entrée vivante et rend « pas à moi » pour de la mémoire à nous.
- **Piège d'infra** : deux `struct` **anonymes** déclarées séparément sont des **types distincts** en C — nommer la
  structure. Et trois chaînes de build avaient divergé (winediff, `wine_hashes.sh`, le workflow Windows) : seule ma
  commande ad-hoc liait `uuid`. **Trois chaînes qui doivent s'accorder, c'est trois occasions de comparer autre chose.**
- **Vérifié** : `winecorpus/win32_comalloc.c` bit-identique Wine, audit stdcall PASS, hash inchangé.

### 2026-08-01 — [HLE-COM] **Mur suivant NOMMÉ : `CoCreateInstance` = MLang — et pourquoi l'échec « défini » aurait été faux**

- L'abort disait `unimplemented import CALLED: CoCreateInstance` : **vrai et inutile**, puisque *quelle classe* est
  toute la question. Rendu **diagnostique** (même leçon que le garde x87 : « bruyant » ≠ « diagnostique ») :
  `CoCreateInstance class {CLSID} as {IID} (ctx N)`.
- **Mesuré** : WinMerge demande `{275C23E2-3747-11D0-9FEA-00AA003F8646}` = **CLSID_MultiLanguage**, en
  `{275C23E1-…}` = **IID_IMultiLanguage**, ctx 1 (INPROC_SERVER). ⇒ une classe **précise et bornée** (MLang, détection
  et conversion de jeux de caractères), **pas** l'ouverture du registre COM complet.
- **⚠️ Le piège évité** : renvoyer `REGDB_E_CLASSNOTREG` est un **échec défini**, donc apparemment éligible au canal
  `aret_partial`. **Ce n'est pas sound ici** : sous Wine la classe **est** enregistrée et l'appel réussit ; répondre
  « non enregistrée » pousserait le programme sur un chemin d'erreur **qu'il ne prend jamais** sur un vrai système —
  une exécution différente présentée comme normale, soit exactement ce que le §0 interdit. **Habiller un faux
  silencieux d'un HRESULT légitime ne le rend pas légitime.** Donc : abort, mais nommé.
- **Prochain incrément, cadré et mesuré** : `mlang.dll` (builtin Wine) = **14 exports, 0 thunk, 0 forwarder** et
  n'importe que gdi32 (9) / kernel32 (31) / ntdll (1) / ucrtbase (17) — le **candidat idéal du Levier 1** sous la
  règle corrigée. Il exporte `DllGetClassObject`. Chemin : `CoCreateInstance` reconnaît le CLSID → appelle le
  `DllGetClassObject` **lifté** → `IClassFactory::CreateInstance` **à travers la vtable liftée** (via `aret_call`,
  comme le WNDPROC comctl32) → `Release`. Les deux étapes appellent du code **lifté**, mécanisme déjà prouvé.

### 2026-08-01 — [HLE-COM][LIFT-DLL][I5] **⭐ ACTIVATION COM RÉELLE : `CoCreateInstance` sert une classe depuis une DLL LIFTÉE — et sans table de CLSID**

- **Ce qui tourne** : `CoCreateInstance(CLSID_CMultiLanguage, …, IID_IMultiLanguage, …)` rend un **vrai objet**,
  servi de bout en bout par du **code lifté** de `mlang.dll` (builtin Wine, `--with-dll`). La chaîne est :
  `CoCreateInstance` → **`DllGetClassObject` lifté** → **`IClassFactory::CreateInstance` à travers la vtable du
  module lifté** → `Release` de la fabrique. Puis le programme appelle l'objet : `QueryInterface`,
  `GetNumberOfCodePageInfo`, `GetCodePageInfo`, `ConvertStringToUnicode` — **quatre méthodes de plus à travers la
  vtable liftée**. Gardé par `winecorpus/ole_mlang.c` (+ `.withdll`), **bit-identique Wine**.
- **⭐ Le point de conception : il n'y a AUCUNE table de CLSID, et il ne doit pas y en avoir.** Coder « ce CLSID →
  ce module » serait une **rustine par binaire** (§0.3) qui périme dès que le module change. On fait donc ce que
  fait un chargeur COM in-proc **moins le registre** : demander à **chaque** module lifté qui exporte
  `DllGetClassObject` s'il sert ce CLSID. Un module qui ne le sert pas répond `CLASS_E_CLASSNOTAVAILABLE` —
  **une vraie réponse de vrai code**, pas une supposition de notre part. Le mécanisme est donc **général** : toute
  DLL COM in-proc liftée à l'avenir marche sans une ligne de plus.
- **La brique qui manquait, et elle est petite** : le lifting DLL liait jusqu'ici ce que l'app importe
  **statiquement** (le loader écrit la VA d'export dans le slot IAT). L'activation COM va dans l'**autre sens** —
  elle atteint un point d'entrée qui n'apparaît dans **aucune** table d'imports. D'où
  `Program::dll_exports` (loader) → table `aret_lifted_exports` générée dans `aret_dispatch.c` →
  `aret_lifted_export()` / `aret_lifted_export_iter()` dans le HLE. **Vide, donc sans effet, pour un exe seul.**
  C'est aussi la brique qui rendra `GetProcAddress` (§P1quater) implémentable sur du code lifté.
- **Choix mesuré : publier TOUS les exports nommés, sans filtrer sur « fonction récupérée ».** Une VA non
  récupérée tombe dans l'abort **nommé** d'`aret_call` (« indirect call to unrecovered function 0x… »), ce qui en
  dit plus qu'un lookup qui rapporterait « absent » — l'échec reste bruyant, il devient juste diagnostique.
- **CLSCTX honoré** : seuls les contextes **in-proc** (`INPROC_SERVER|INPROC_HANDLER`) sont tentés. Un
  `LOCAL_SERVER` demande un autre processus, qui est un **échec sound** ailleurs dans le HLE ; le dégrader
  silencieusement en in-proc serait une exécution différente présentée comme normale.
- **La fabrique est relâchée** : elle a sa propre durée de vie et l'appelant n'apprend jamais son existence.
- **L'abort résiduel dit maintenant combien de modules ont été interrogés** — « 0 module lifté offrait
  `DllGetClassObject` » et « 3 l'offraient et aucun ne l'a servie » sont deux problèmes différents, et l'ancien
  message ne les distinguait pas.
- **Vérifié** : `ole_mlang` bit-identique Wine, hash `19acad982194bf07` inchangé, difftest 272/272, audit stdcall
  PASS, cargo test complet vert.

### 2026-08-01 — [HLE-WIN32][ORACLE] **`TranslateCharsetInfo` — une table embarquée n'est légitime que balayée EXHAUSTIVEMENT**

- **Mur** : le premier import que mlang **lifté** appelle est `gdi32.TranslateCharsetInfo` (il décrit un code page).
  Le reste de sa liste statique — `GetTextCharset`, `GetFontUnicodeRanges`, `GetLocaleInfoA`, `LocaleNameToLCID`,
  `EnumSystemLocalesEx`, `EnumResourceNamesW` — a été **mesuré d'abord** (Levier 0) : aucun n'est atteint sur ce
  chemin, donc rien n'a été écrit spéculativement.
- **Grille de mesure, exhaustive et non échantillonnée** : les **256** valeurs de charset, les **32** bits de
  `fsCsb[0]`, **46** code pages. C'est ce qui rend l'embarquement de la table **légitime** au sens du 70 §7 : la
  donnée est version-dépendante mais **déterministe**, donc la fixture couvre **chaque case** et un changement de
  Wine vire au **rouge** au lieu de pourrir en silence. Une grille échantillonnée aurait donné la même
  implémentation avec aucune de ses garanties.
- **Ce que la mesure contredit** (et qui aurait été deviné faux) :
  - `fsUsb[4]` revient **tout à zéro** ; seul `fsCsb[0]` porte l'information.
  - Pour `TCI_SRCFONTSIG` la source est un **pointeur** vers `fsCsb`, le **bit le plus bas gagne** (bits 0+1 →
    l'entrée du bit 0) et `fsCsb[1]` est **totalement ignoré**.
  - Un bit **sans entrée** (9-15, 22-25, 27-30) → FAUX, et **tout** refus laisse la `CHARSETINFO` de l'appelant
    **strictement intacte** — prouvé au tampon empoisonné, invisible autrement — et **ne touche pas** au last-error.
  - `DEFAULT_CHARSET` (1) est **refusé** alors que `ANSI_CHARSET` (0) est accepté.
- **⚠️ Piège d'oracle attrapé en cours de route (à retenir)** : ma 1ʳᵉ sonde faisait
  `printf(…, TranslateCharsetInfo(…), ci.ciCharset, …)`. **L'ordre d'évaluation des arguments de `printf` n'est pas
  spécifié** : gcc a lu la structure **avant** l'appel, et j'ai lu « Wine rend VRAI sans rien écrire » — une
  conclusion **fausse**, du même genre que le last-error qui fuyait dans la famille SPI. Règle : *lire la valeur
  APRÈS l'appel, dans son propre énoncé*. Encodé dans la fixture et dans son en-tête.
- **Trois cases mises en file pour l'oracle Windows** plutôt que tranchées par Wine seul (`bench/winoracle/
  win32_charsetdisputed.c`) : `fsUsb` tout-à-zéro, la ligne **254 ↔ 65001** (UTF-8, pas un charset Windows
  documenté), et `TCI_SRCLOCALE` que Wine rend FAUX avec un FIXME dans sa source. Notre valeur est un **échec
  défini**, jamais un succès fabriqué — mais c'est exactement le profil de `PathIsUNCServer`.
- **Infra de l'oracle, deux corrections** : (a) l'étape « sondes » du workflow compile désormais **tout `.c` de
  `bench/winoracle`** — ajouter une sonde est *ajouter un fichier*, plus éditer le YAML, et une sonde cassée ne
  masque plus les autres ; (b) `wine_hashes.sh` **ignore le code de sortie**, comme le runner le faisait déjà :
  `crt_assert` **meurt exprès** (c'est sa preuve), et le traiter en échec écartait la seule fixture dont le contrat
  est de mourir — donc les deux côtés étaient éligibles sur des règles différentes, ce que le README interdit.
- **Vérifié** : `winecorpus/gdi_charsetinfo.c` bit-identique Wine, `@N` (`TranslateCharsetInfo@12`,
  `GetTextCharset@4`) pris de la **vérité terrain** (import-libs mingw), audit stdcall PASS.

### 2026-08-01 — [LIFT-DLL][HLE-WIN32][I5] **Trois murs de WinMerge en une passe — et le deuxième a coûté un argument de ligne de commande**

- **Progression réelle du driver**, dans l'ordre où les murs sont tombés :
  `CoCreateInstance` (activation COM, entrée précédente) → **`SHGetSpecialFolderLocation`** → **`StrSpnW`** →
  **`GetThreadDesktop`** → mur suivant à mesurer. Tous de la **couverture d'API**, aucun de lift-correctness.
- **⭐ `SHGetSpecialFolderLocation` a coûté `--with-dll shell32.dll=…`, rien d'autre.** Le 70 §5.0 mesurait déjà
  shell32 « implémente » (362 exports, 4 thunks, 36 forwarders) et prédisait que le lifter effacerait
  `SHGetSpecialFolderLocation`/`SHGetMalloc`/`SHGetPathFromIDListW`/… d'un coup. **La prédiction était juste** : zéro
  shim écrit. C'est le Levier 1 rendant ce qu'il promet, et c'est la meilleure justification qu'on ait produite pour
  **mesurer avant de coder** — la mesure de juillet a payé six jours plus tard sans travail supplémentaire.
- **Famille shlwapi `Str*`, vague 1 (22 shims : recherche/balayage)** — shimmée et **pas** liftée, parce que shlwapi
  est un **relais** (198 thunks + 217 forwarders / 362). Ces fonctions **ressemblent** à `<string.h>` et n'en sont
  pas ; les quatre contrastes mesurés qui font qu'une implémentation « évidente » diverge en silence :
  aiguille **vide** ⇒ NULL (là où `strstr` rend la botte de foin) ; `StrCSpn(s,"")` = longueur **entière** quand
  `StrSpn(s,"")` = 0 ; le `n` des variantes comptées borne **le départ** du motif, pas sa fin (**balayé n=0..22**
  plutôt que raisonné) ; NULL toléré partout. **Et une asymétrie franche** : `StrChrW(s,0)` rend le **terminateur**
  quand `StrChrA`, `StrChrIW` et `StrRChrW` rendent NULL — une fonction sur quatre en désaccord avec ses sœurs.
  Reproduite verbatim (Wine est la porte), **mise en file pour l'oracle Windows**, et l'en-tête de la fixture
  interdit explicitement de « corriger vers la cohérence » sans mesure Windows. `StrChr`/`StrChrN` sont **écrites à
  la main** et non générées par macro **précisément** parce qu'une macro aurait imposé une seule réponse aux deux
  largeurs — le sur-partage aurait effacé le fait mesuré.
- **Famille window-station/desktop (`GetThreadDesktop`, `GetProcessWindowStation`, `GetUserObjectInformationA/W`)** :
  deux singletons distincts, nommés comme Windows les nomme (`Default` sur `WinSta0`). `GetThreadDesktop` réussit
  pour un tid que **ce processus possède réellement** (le thread principal ou un fiber vivant, `0x1000+i` que
  `CreateThread` rend) et répond NULL + 87 sinon — jamais un handle pour un thread qui n'existe pas.
  **Ce que seul le tampon empoisonné révèle** : `UOI_FLAGS` remplit 12 octets mais **n'écrit que le troisième
  dword** — `fInherit` et `fReserved` gardent le poison. Un shim qui aurait `memset` la structure passerait
  n'importe quel test lisant `dwFlags` seul. Trois échecs **distincts** aussi : tampon trop court (err 122, taille
  rendue), index non supporté (err 87, taille **0**), mauvais handle (err **6**, taille 0).
  **⚠️ Mis en file pour l'oracle Windows** : sur le chemin **A**, le succès rend la taille **étroite** (8) et
  l'échec la taille **large** (16). Une taille requise qui dépend de la **réussite** n'est pas un contrat qu'on
  dessine — c'est ce que fait un wrapper A qui délègue au W et ne convertit qu'en sortie.
- **Vérifié** : `str_search` et `user32_desktop` bit-identiques Wine du **premier coup**, winediff complet
  **202/203** après la vague `Str*` (seul rouge `gdi_uifont`, environnemental), difftest 272/272, hash
  `19acad982194bf07` inchangé, audit stdcall PASS (table 909 → 937, `@N` depuis les import-libs mingw).
- **Reste cartographié de la famille `Str*`** (53 exports, 4 vagues) : copie/concat/trim (`StrCpy*`/`StrCat*`/
  `StrNCat*`/`StrDup*`/`StrTrim*`), comparaison (`StrCmp*` + `StrCmpLogicalW` + `StrIsIntlEqual*`), conversion
  entière (`StrToInt*`/`StrToInt64Ex*`), formatage (`StrFormatByteSize*`/`StrFormatKBSize*`/`StrFromTimeInterval*`),
  et les `StrRetTo*` (structure `STRRET` du shell).

### 2026-08-01 — [HLE-WIN32] **Famille shlwapi `Str*`, vague 2 (copie/concat/trim/dup) — et le piège d'ordre d'évaluation a frappé DEUX fois dans la même journée**

- **Livré** (14 shims) : `StrCatBuffA/W`, `StrNCatA/W`, `StrTrimA/W`, `StrDupA/W`, `StrCatW`, `StrCpyW`,
  `StrCpyNW`, `StrCatChainW`. Grille **balayée** (les compteurs de 0 à 8/12) plutôt qu'échantillonnée, parce que
  toute cette famille tient sur une convention : **le compteur est la taille de la destination ENTIÈRE, NUL
  compris**, et se tromper d'un cran est un débordement d'un caractère.
- **Ce que seul le tampon empoisonné montre** : `StrCpyNW(dst, src, 0)` n'écrit **rien du tout** — pas même le NUL
  (n=1 n'écrit que le NUL) ; `StrCatBuff` avec un cch qui ne couvre que l'existant n'ajoute rien et **laisse la
  chaîne intacte** ; et `StrCatChainW` écrit à `ichAt` **littéralement**, sans chercher la fin — les indices 1 à 4
  gardent le poison quand on écrit à l'indice 5. Un test qui relit une *chaîne* validerait les trois à tort.
- **Autres faits mesurés, non déductibles** : `StrDup(NULL)` rend une chaîne **vide valide**, pas NULL (idem
  `StrDup("")`) ; le bloc doit venir du tas que **`LocalFree` (= `free()` ici)** accepte, sinon chaque `StrDup`
  fuit ou corrompt ; `StrTrim` rogne **les deux bouts** en place et rend FAUX quand rien n'a bougé — un ensemble de
  caractères vide ou NULL ne change rien et rend **FAUX**, pas VRAI.
- **Non modélisé, volontairement** : une **source NULL** à `StrNCat`/`StrCpyN` — Wine **faute** (mesuré), c'est donc
  un bug d'appelant et pas un contrat ; inventer « ne fait rien » serait être plus gentil que Windows, ce qui est
  une divergence comme une autre. `StrCpyNX*` reste dehors : export **non documenté**, absent de l'import-lib mingw,
  donc non liable par une fixture sans `.def`.
- **⚠️ LE PIÈGE D'ORDRE D'ÉVALUATION A FRAPPÉ DEUX FOIS AUJOURD'HUI, sous deux visages.** Le matin :
  `printf(…, TranslateCharsetInfo(…), ci.ciCharset, …)` lisait la structure **avant** l'appel. Le soir :
  `printf(…, strcmp(p,"dup me")==0, LocalFree(p)==NULL)` **libérait `p` avant de le comparer** — un
  **use-after-free** que Wine survit (les octets sont encore là) et qu'un autre allocateur ne survit pas. La
  fixture est sortie **rouge sur une seule ligne**, et la cause n'était pas le shim mais la sonde. **La leçon
  générale est plus forte que « lire après l'appel »** : *dans un `printf`, deux arguments qui touchent le même
  objet sont un bug latent, quel que soit le sens de la dépendance* (lire-après-écrire, ou lire-après-libérer).
  Écrit dans l'en-tête de la fixture pour que la prochaine session ne le redécouvre pas une troisième fois.
  **Et le rouge a été qualifié avant d'accuser** (règle 70 §7) : c'était la sonde, pas l'implémentation.
- **Vérifié** : `str_copy` bit-identique Wine, difftest 272/272, hash `19acad982194bf07` inchangé, audit stdcall
  PASS (table 949, `@N` depuis les import-libs mingw).
- **Effet driver** : WinMerge franchit `GetThreadDesktop` (4ᵉ mur de la session) et demande `swprintf_s` — famille
  CRT `*_s` wide, prochain incrément. À noter : `swprintf` **tout court** reste **non modélisé** (signature
  ambiguë, 70 §4.5), mais `swprintf_s` est **sans ambiguïté** (`buf, count, fmt, …`) — donc implémentable, et le
  refus de deviner sur l'un n'empêche pas de servir l'autre.

### 2026-08-01 — [HLE-CRT][ORACLE] **`swprintf_s`/`vswprintf_s` livrées, `sprintf_s`/`vsprintf_s` REFUSÉES — et c'est `objdump` qui a sauvé la mesure**

- **Mur** : après `GetThreadDesktop`, WinMerge demande `swprintf_s`. Note d'entrée : `swprintf` **tout court** reste
  **non modélisé** (signature ambiguë selon le CRT, 70 §4.5) — mais `swprintf_s` n'a **qu'une** forme
  (`buf, count, fmt, …`). *Refuser de deviner sur l'une n'empêche pas de servir l'autre* : ce sont deux questions
  différentes, pas un principe de prudence global.
- **⚠️ La règle du 70 §7 a payé immédiatement, et deux fois.** Ma 1ʳᵉ sonde compilait sans `.def` : `objdump -p`
  montre que **rien n'était importé** — mingw fournit ses propres corps pour plusieurs `*_s`, donc je mesurais
  **mingw des deux côtés** et j'allais encoder un contrat qui n'est pas celui du CRT. 2ᵉ tentative avec un `.def` :
  `objdump` montre que **seules les versions étroites** se sont liées (mingw garde les larges). Ce n'est qu'à la
  3ᵉ, par la route `<wchar.h>` + `MINGW_HAS_SECURE_API`, que `swprintf_s` (ordinal **1106**) et `vswprintf_s`
  (**1137**) apparaissent réellement dans la table d'imports. **Vérifier l'import n'est pas une formalité : c'est
  la différence entre mesurer le CRT et mesurer son propre compilateur.**
- **Contrat mesuré** (capacité **balayée** 0..8 sur un résultat de 5 caractères, tampon empoisonné) : capacité 0 ⇒
  −1 et tampon **totalement intact** ; capacité trop petite ⇒ −1 et **zéro-remplissage d'exactement `capacité`
  unités** (ni `dst[0]` seul, ni tout le tableau) ; ça rentre ⇒ le texte, son NUL, et le retour est la **longueur**.
  Un shim qui n'écrirait que le terminateur en échec satisferait tout appelant qui relit une chaîne.
- **⭐ Et les jumelles ÉTROITES sont refusées, délibérément.** Forcées vers msvcrt par un `.def`, `sprintf_s`/
  `vsprintf_s` de Wine font **autre chose des deux côtés** : elles laissent une **sortie partielle** en échec au
  lieu de zéro-remplir, et à l'ajustement **exact** elles rendent un **succès** (5) en écrivant cinq caractères et
  **aucun terminateur** — que la version large, elle, refuse (il lui faut 6). **Deux fonctions d'une même famille
  en désaccord sur l'emplacement du NUL, c'est la signature d'un dérapage d'implémentation, pas d'un contrat** —
  même profil que `PathIsUNCServerA` et que `StrChrW` sur le terminateur. Donc **abort bruyant** plutôt que
  reproduire ça depuis un seul oracle, et la question part au runner Windows
  (`bench/winoracle/crt_sprintfs_disputed.c`). Si Windows se comporte comme la version large, le code large sert
  les deux ; s'il rend vraiment une longueur sans terminer, il vaut mieux le savoir **avant** qu'un programme en
  dépende.
- **Vérifié** : `winecorpus/crt_swprintf_s.c` bit-identique Wine, difftest 272/272, hash `19acad982194bf07`
  inchangé, audit stdcall PASS (ces fonctions sont **cdecl** — pas de `@N`).

### 2026-08-01 — [EH][I4][ORACLE] **`_except_handler4_common` : l'encodage v4 PROUVÉ dans les deux sens — et une route bon marché mesurée avant d'écrire le handler**

- **Mur** : après `swprintf_s`, WinMerge réclame `_except_handler4_common`. C'est le chantier **I4** du doc 81, que
  le document externe mettait en **priorité 1** et qu'on a **refusé de construire spéculativement**. La mesure le
  réclame maintenant — pour la **deuxième** fois (déjà atteint le 2026-07-26). La priorisation par la donnée a tenu :
  il arrive quand un binaire l'exige, pas quand un document l'annonce.
- **Méthode imposée par le §I4 : instrument-first.** Harnais conservé dans `bench/eh/eh4_probe.{c,def}` —
  **sonde de mesure, ni fixture ni porte** — pour que la prochaine session ne le reconstruise pas.
- **⭐ Fait PROUVÉ (dans les deux sens, une exécution chacun)** : le champ `scopetable` de la registration record est
  **XOR-encodé avec `*cookie`**.
  - stocké **en clair** ⇒ la faute tombe à `stored ^ cookie` (`edi = &tbl ^ cookie`, adresse `0x12748644`) ;
  - stocké **XOR'é** ⇒ `edi = 0x0040d040` = **`&tbl` exactement**.
  Une seule direction n'aurait pas tranché ; une coïncidence doit survivre aux **deux**. L'observable a été choisi
  pour que *aucune* des deux réponses ne soit muette (les deux plantent, mais à des adresses qui **disent laquelle**).
- **Confirmé aussi, lu sur l'oracle et non supposé** : la signature `(ULONG *cookie, void (*check_cookie)(void),
  EXCEPTION_RECORD*, FRAME*, CONTEXT*, void**)` — les symboles de Wine impriment les noms **et les valeurs** des
  paramètres dans la backtrace ; et le layout `{prev, handler, scopetable, trylevel, _ebp, xpointers}` est accepté,
  cohérent avec le `funclet ebp = EstablisherFrame + 16` de la brique C.
- **Explicitement NON deviné** (chacun demande sa propre expérience, et c'est écrit dans l'en-tête de la sonde) :
  `trylevel` est-il encodé lui aussi ; que valent `gs_cookie_offset`/`eh_cookie_offset` **= -2** (sentinelle MSVC
  « absent », ou vrai offset ?) ; où commence le tableau de `ScopeRecord` après l'en-tête de quatre `int` ; le
  protocole du callback `check_cookie`. Le run XOR est parti dans un appel à `0xfffffffe`, ce qui est **compatible**
  avec un niveau mal décodé mais n'en est **pas une preuve** — donc rien n'est encodé sur cette base.
- **⭐ Et avant d'écrire le handler à la main, la route bon marché a été MESURÉE** (règle des deux commandes,
  70 §5.0) : `msvcr90.dll`, qui expédie **à côté de WinMerge**, fait **2900 exports, 0 thunk, 0 forwarder**,
  n'importe **que KERNEL32**, et exporte `_except_handler4_common` **directement**. C'est le profil exact de
  `mlang`/`shell32` — donc le Levier 1 s'applique, et un `--with-dll` peut servir cette fonction comme il a servi
  `SHGetSpecialFolderLocation`. **Réserve honnête** : le handler lifté devra marcher **notre** `fs:[0]` synthétique
  (modélisé, donc a priori OK) **et** provoquer le transfert non-local, dont le mécanisme (setjmp injecté au
  SEH-establish) est **gaté sur l'import `_except_handler3`/`__CxxFrameHandler*`** — un import
  `_except_handler4_common` ne réveille pas forcément ce gate. Ce n'est donc pas gratuit, mais c'est **une mesure**
  et pas un chantier : à tenter avant de coder.

### 2026-08-01 — [LIFT-DLL][EH] **La route bon marché du I4 est MESURÉE et REJETÉE — et la règle du Levier 1 gagne sa troisième correction**

- **Essai** : lifter `msvcr90.dll` (2900 exports, 0 thunk, 0 forwarder, n'importe que KERNEL32) pour servir
  `_except_handler4_common` comme lifter shell32 a servi `SHGetSpecialFolderLocation`.
- **Le routage marche** : la fonction **disparaît** de la liste des imports non implémentés et le lift passe de
  43686 à **46524 fonctions**. Techniquement, l'appel atteint bien du code lifté.
- **Mais le programme meurt en `exit(255)`, proprement, SANS AUCUNE SORTIE** — ni stdout ni stderr, et gdb confirme
  une sortie **normale**, pas une faute. Or la version sans `msvcr90` imprimait déjà son premier message (le
  `partially modelled` du chargement de police) bien plus loin dans l'init MFC. ⇒ il meurt **plus tôt qu'avant**,
  donc dans le démarrage.
- **Hypothèse, affichée comme telle** : une DLL **CRT n'est pas une bibliothèque feuille**. Lifter shell32 ou mlang
  est **additif** — l'app leur demandait déjà des services que le HLE ne rendait pas. Lifter le CRT **substitue
  tout un sous-système** que le HLE implémente déjà bien (démarrage, tas, stdio, locale) par du code lifté, d'un
  seul coup, sans que rien n'ait été vérifié de cet ensemble. Non prouvé dans le détail (on ne sait pas *quelle*
  étape du démarrage échoue) ; ce qui est **mesuré**, c'est le routage réussi et la mort anticipée.
- **⭐ Troisième correction de la règle du Levier 1 en deux jours**, et la plus utile :
  1. compter les `__wine_spec_imp_` (thunks) — **incomplet** ;
  2. compter **aussi** les forwarders PE — corrigé le 2026-08-01 (ole32 : 133 forwarders) ;
  3. **et même « la DLL contient du vrai code » ne dit pas que la lifter soit un bon échange.** La métrique répond
     à « y a-t-il une implémentation dedans ? », pas à « remplacer notre HLE par elle est-il un progrès ? ».
     Le discriminant est **feuille vs sous-système** : additif ⇒ oui ; substitution d'une couche déjà couverte et
     déjà vérifiée ⇒ non.
- **Idée cadrée qui rendrait la route réelle** (pour plus tard, pas engagée) : le loader route **tous** les imports
  de l'app que la DLL liftée exporte. Une option « lifter cette DLL mais ne router que **ces noms-là** » rendrait le
  Levier 1 utilisable **chirurgicalement** — on prendrait `_except_handler4_common` de msvcr90 en laissant le reste
  du CRT au HLE. Petit changement dans `resolve_module_imports` (un filtre de noms), gros gain de portée.
- **Conclusion pour I4** : la brique se **code à la main**, avec l'encodage XOR **déjà prouvé** et les quatre
  questions ouvertes listées dans `bench/eh/eh4_probe.c`. Le détour a coûté une mesure et a rapporté une règle.

### 2026-08-01 — [EH][I4] **✅ `_except_handler4_common` IMPLÉMENTÉ — trois différences avec la brique C, toutes mesurées, dont un piège qui aurait été invisible**

- **Les quatre questions ouvertes sont fermées**, chacune par une expérience à observable **non muet** (les deux
  réponses possibles produisent une sortie différente, aucune n'est un silence) :
  1. **`trylevel` est stocké EN CLAIR**, pas encodé — prouvé dans les deux sens avec un cookie de 4 et une table de
     8 niveaux, de sorte que la réponse « clair » (2) **et** la réponse « encodé » (6) soient toutes deux des
     indices valides avec leur propre filtre. Aucun des deux cas ne peut planter ⇒ le résultat est lisible, pas déduit.
  2. **`gs_cookie_offset`/`eh_cookie_offset` = -2 signifie « absent »**, et — mesuré — **Wine n'appelle JAMAIS
     `check_cookie`**, même avec un cookie GS délibérément faux.
  3. **Disposition** : en-tête de **quatre `int`** puis les enregistrements `{EnclosingLevel, FilterFunc,
     HandlerFunc}` de 12 octets, parcourus depuis `trylevel` via `EnclosingLevel`.
  4. **`EXCEPTION_POINTERS` va en `[frame-4]` et l'ebp du filtre est `frame+16`** — donc **identiques à la brique C**
     alors que la frame v4 possède un champ `xpointers` à +20 que Wine laisse **intact**. Le champ à l'air évident
     est le mauvais ; seule la mesure le dit.
- **⭐ LE PIÈGE : le terminateur de chaîne est `-2`, pas `-1`.** Réutiliser le `-1` de v3 lirait un enregistrement
  **avant** le tableau sur **chaque** frame sans `__try` actif — et le troisième `int` de l'en-tête vaut justement
  `-2`, donc il serait lu comme **adresse de filtre** et **appelé**. C'est exactement l'appel sauvage vers
  `0xfffffffe` que la sonde a produit avant que le terminateur soit mesuré, et j'avais d'abord noté ce plantage
  comme « compatible avec un `trylevel` mal décodé, mais pas une preuve » — ce qui était la bonne prudence :
  l'explication réelle était ailleurs. **Un tel bug se serait présenté comme une erreur de lifting**, pas comme une
  erreur de handler.
- **Implémentation** : `_except_handler3` et `_except_handler4_common` partagent désormais **un seul corps**
  (`aret_seh_dispatch` + `aret_seh_dispatch_search`) paramétré par *(base des enregistrements, terminateur)*. Écrire
  deux fois la marche, l'unwind local/global et le transfert aurait été deux fois la surface de bug pour zéro
  différence sémantique — les seules différences réelles sont les positions d'arguments, le XOR + l'en-tête, et le
  terminateur.
- **⚠️ Un gate qu'il fallait ouvrir, et qui aurait échoué EN SILENCE** : l'injection du `setjmp` au SEH-establish est
  conditionnée aux imports (`uses_seh`), et un binaire MSVC /GS moderne n'importe **que** `_except_handler4_common`.
  Sans l'ajouter à cette liste, le handler aurait fonctionné parfaitement et le transfert n'aurait **jamais eu lieu**
  — un mode d'échec bien pire qu'un abort.
- **Sondes conservées, mais SORTIES du répertoire gardé** (`bench/eh/probes/`, README dédié) : `ehdiff.sh` compile
  tout `bench/eh/*.c` par la chaîne clang/MSVC, et ces harnais demandent mingw + un `.def`. Laissés en place ils
  faisaient virer la porte au rouge **pour une raison que personne ne doit corriger** — précisément ce que le 70 §7
  interdit (« une porte instable est pire qu'une porte lente »). Attrapé par la porte elle-même, en une exécution.
- **Vérifié** : `winecorpus/seh_handler4.{c,def}` bit-identique Wine **du premier coup** (marche des filtres, passe
  d'unwind avec `__finally`, terminateur), **ehdiff 6/6** (le refactor ne régresse pas la brique C), difftest
  272/272, hash `19acad982194bf07` inchangé.
- **Périmètre honnête** : la fixture couvre les chemins **sans transfert**. `EXECUTE_HANDLER` (unwind global +
  longjmp) est **partagé mot pour mot** avec `_except_handler3` et gardé par `ehdiff` ; il n'est pas testable sur une
  frame fabriquée à la main, qui n'a pas fait de vrai `mov fs:[0]` et n'a donc pas de `setjmp` où revenir.

### 2026-08-01 — [HLE-CRT][EH] **`_XcptFilter` — pourquoi il fallait balayer TROIS états de la table `signal()` pour en dériver un seul**

- **Mur** : après `_except_handler4_common`, WinMerge appelle `_XcptFilter(code, EXCEPTION_POINTERS*)` — le filtre
  top-level du CRT, l'endroit où une exception structurée Win32 rencontre le monde `signal()` du C.
- **⭐ Le point de méthode, et il est réutilisable** : la grille balaie **trois états** de la table (rien d'installé /
  handlers installés / `SIG_IGN`), et ce n'est **pas** de la minutie gratuite. **Délivrer un signal REMET sa
  disposition à `SIG_DFL`** (règle one-shot ANSI) ⇒ avec des handlers installés, seul le **premier** code de chaque
  groupe se déclenche. Un balayage à un seul état aurait montré une correspondance quasi vide, et une implémentation
  bâtie dessus aurait été fausse pour **tous les codes sauf un par groupe**. C'est la passe **`SIG_IGN`**, qui ne
  consomme rien, qui révèle les vrais groupes. *Quand une API lit un état global qu'elle MODIFIE, une grille à un
  seul état mesure la consommation, pas le contrat.*
- **Deux résultats mesurés qui contredisent les noms** : `STATUS_INTEGER_DIVIDE_BY_ZERO` n'est **pas** mappé sur
  SIGFPE (ni `INTEGER_OVERFLOW`, ni `ARRAY_BOUNDS_EXCEEDED`, ni `STACK_OVERFLOW`) — **seuls les sept statuts
  flottants** le sont ; et sans rien d'installé la réponse est **CONTINUE_SEARCH (0)**, pas EXECUTE_HANDLER : le CRT
  **n'avale pas** l'exception. Ce second point est aussi ce qui nous garde sound — une exception que personne ne
  gère continue jusqu'à notre chemin « non gérée », bruyant, au lieu d'être absorbée en silence ici.
- **⚠️ Et la fixture a attrapé une case que j'avais modélisée de travers** : `_XcptFilter(code, **NULL**)` rend **0**
  *même* quand la disposition est `SIG_IGN` (qui rendrait -1). Ma 1ʳᵉ version consultait la table d'abord ⇒ rouge sur
  **une ligne**. Correction : le pointeur est testé **avant**. Et plutôt que de patcher sur un seul point, la grille a
  été **élargie** — le cas NULL est désormais balayé dans les **trois** dispositions, ce qui sépare « testé d'abord »
  de « coïncidence ». Un seul état ne l'aurait pas fait.
- **Vérifié** : `winecorpus/crt_xcptfilter.{c,def}` bit-identique Wine, **ehdiff 6/6**, difftest 272/272, hash
  `19acad982194bf07` inchangé.

### 2026-08-01 — [I5][EH][LIFT] **Le driver CHANGE DE RÉGIME : après sept murs d'API, le suivant est une faute matérielle — et la brique v4 tourne pour de vrai**

- **Sept murs franchis dans la session**, tous de **couverture d'API** :
  `CoCreateInstance` → `SHGetSpecialFolderLocation` → `StrSpnW` → `GetThreadDesktop` → `swprintf_s` →
  `_except_handler4_common` → `_XcptFilter`. Le **huitième n'en est plus un** : `unhandled hardware exception
  0xc0000005 at 0x10`. ⇒ **le blocage rebascule de la surface d'API vers la lift-correctness**, pour la 1ʳᵉ fois
  depuis le 2026-07-26.
- **⭐ Ce que la trace I1 montre, et c'est une bonne nouvelle avant d'être un mur** : la faute est **dispatchée par
  la machinerie SEH**, sur la pile scratch dédiée de `aret_hw_fault` (`esp=0x13269d..`, distinct de la pile
  machine `0x13d63...`, registres à 0), avec des handlers **liftés qui s'exécutent** (`sub_4ac1cc` récurrent,
  `ecx=0xc8593fa7` — profil d'un `__security_check_cookie` / thunk `_except_handler4`). ⇒ **la brique v4 livrée
  aujourd'hui n'est pas seulement bit-identique sur une fixture : elle est EXERCÉE par un vrai binaire MFC.** Ce
  qui termine le run, c'est la **chaîne épuisée** — aucune frame ne rattrape —, pas un défaut du handler.
- **Chaîne d'appel avant la faute** (trace, du plus ancien au plus récent) : init MFC → `sub_864955` →
  `sub_6faeaf` → `sub_42e14e` → `sub_42d86f` → **`sub_42eca8`**, puis bascule sur la pile de faute. Le mur est
  donc dans/après `sub_42eca8`.
- **Piste, affichée comme HYPOTHÈSE et non comme cause** : un accès à `NULL+0x10` a la forme classique d'un
  `this->membre` avec `this` nul, ou d'un **appel de vtable sur un pointeur d'interface COM nul**. Le précédent
  `0xe` (2026-07-26) avait cette même signature et s'est révélé être une **dérive esp** (import stdcall appelé
  register-indirect cross-block), pas ce que la forme suggérait — donc **ne pas conclure sur la forme**. Le
  cookie /GS reste le détecteur de dérive esp gratuit (70 §7) et `ecx=0xc8593fa7` est cohérent avec ce chemin :
  à instruire, pas à affirmer.
- **Honnêteté sur l'antériorité** : ce code est **nouvellement atteignable**, donc « le bug est-il préexistant ? »
  n'est **pas testé**. Les portes (difftest, ehdiff, winediff, hash) disent qu'il n'y a pas de régression, ce qui
  est un **raisonnement**, pas une mesure directe sur ce chemin.
- **Cadrage pour la suite** : session dédiée, méthode déjà éprouvée sur le mur `0xe` puis sur le mur /GS —
  traceur I1 (fait, ci-dessus) → **instrumentation directe du C généré par numéro de ligne** (l'outil qui a
  tranché le /GS, cf. 81 2026-07-26) → watchpoint matérielle sur l'adresse fautive. Ne PAS repartir de gdb sur
  `$esp` (l'`__esp` modélisé n'est pas celui de l'hôte).

### 2026-08-01 — [I5][LIFT][DIAG] **Le mur `0x10` DIAGNOSTIQUÉ (et ce n'est PAS le lift) — une globale MFC que personne n'initialise**

- **Point de départ** : `unhandled hardware exception 0xc0000005 at 0x10`, opaque.
- **Chaîne de diagnostic, du moins cher au plus cher** — et chaque étape a réduit l'espace avant la suivante :
  1. **Traceur I1** → la fonction fautive est `sub_42eca8`, entrée avec **`ecx = 0`**.
  2. **Lecture du C généré** de `sub_42eca8` → sa 1ʳᵉ instruction est `v5 = *(uint32_t*)(ecx + 0x10)` : une méthode
     **thiscall** qui déréférence `this->+0x10`. Donc `this` est nul, et `0x10` **est** l'offset du champ — la
     valeur du message d'abort n'était pas une adresse mais un **déplacement**.
  3. **Lecture du site d'appel** (`sub_42e14e`) → `ecx` vient de `*(uint32_t*)(esi + 0xc)` avec `esi = 0x51efd0`,
     une **globale**. ⇒ **le lift est FIDÈLE** : il charge et passe exactement ce que le flux d'instructions dit.
     C'est le contenu de la globale qui est faux, pas la traduction.
  4. **Watchpoint matérielle** sur `0x51efdc` (= `0x51efd0+0xc`), posée juste après `__aret_map_memory` →
     **elle ne se déclenche JAMAIS** avant la faute. Personne n'écrit ce champ, du démarrage jusqu'au crash.
  5. **Recherche du writer dans tout le C généré** → **aucune** fonction récupérée ne matérialise `0x51efd0+0xc`
     pour y écrire. Cohérent avec (4), par un chemin indépendant.
- **Énoncé du mur, désormais précis et actionnable** : *la globale `[0x51efd0+0xc]` doit contenir un pointeur
  d'objet et vaut 0 ; `sub_42e14e` la charge comme `this` et appelle `sub_42eca8`, qui déréférence `this->+0x10`.
  Reste à trouver quelle initialisation MFC devrait la peupler et pourquoi elle ne tourne pas.* On est passé de
  « faute à 0x10 » à une question de **flot de données sur une globale nommée** — celle-là se tranche.
- **⭐ Bénéfice collatéral qui vaut l'entrée à lui seul** : l'appelant `sub_44bf51` est une fonction **/GS** dont le
  prologue pose une frame SEH **v4** (XOR du cookie `0x51e7ec`, handler `0x4b5684`) — et `0x4b5684` est
  **exactement** l'un des handlers que la trace montrait s'exécuter sur la pile de faute. ⇒ **la brique
  `_except_handler4_common` livrée aujourd'hui n'est pas seulement verte sur une fixture : elle est exercée par du
  vrai code MFC, sur le chemin réel**, et c'est elle qui dispatche cette faute.
- **Méthode à retenir** : l'ordre traceur → C généré → site d'appel → watchpoint a coûté **quatre mesures** et a
  éliminé le lift comme suspect **avant** d'ouvrir gdb sérieusement. Le réflexe inverse (gdb d'abord) échoue ici,
  parce que l'`__esp` modélisé n'est pas celui de l'hôte (70 §7) — mais la watchpoint sur une **adresse guest
  identity-mappée** marche parfaitement, elle.

### 2026-08-01 — [DIAG][CORRECTION] **⚠️ L'entrée précédente désigne la MAUVAISE globale — `0x51ef18+0xc`, pas `0x51efd0+0xc`**

- **Erreur à corriger avant qu'elle coûte une session** : l'entrée « le mur `0x10` n'est pas le lift » identifie la
  globale fautive comme `0x51efd0+0xc`. **C'est faux.** Le site d'appel dans `sub_42e14e` lit `[esi + 0xc]`, et la
  trace donne pour cette frame `ecx = 0x51efd0` **mais `esi = 0x51ef18`**. J'ai lu `ecx` là où il fallait lire `esi`.
  La globale nulle est **`0x51ef18 + 0xc` = `0x51ef24`**.
- **Ce que l'erreur a coûté**, et c'est instructif : une watchpoint matérielle posée sur `0x51efdc` qui « ne se
  déclenche jamais » — un résultat **vrai mais sans rapport**, que j'ai interprété comme une preuve. Puis une
  recherche statique de l'écrivain de `0x51efd0+0xc`, tout aussi hors sujet, dont le résultat négatif **confirmait**
  la fausse piste. **Deux mesures concordantes sur le mauvais objet ressemblent exactement à deux mesures
  concordantes sur le bon.**
- **Règle à retenir, et elle manquait** : dans une trace de registres, *vérifier de quel registre vient réellement
  l'opérande avant de croiser avec la trace*. La signature liftée est
  `sub_X(__esp, eax, ecx, edx, ebp, esi, edi, ebx)` — la position **6** est `esi`. Le C généré nomme les paramètres
  `v34`, `v21`… : il faut **remonter la signature**, pas deviner par proximité. C'est la version « registres » du
  piège déjà connu sur les compteurs (« qualifier la NATURE avant d'en tirer une cause », 70 §7).
- **Ce qui reste vrai de l'entrée précédente** : la fonction fautive est bien `sub_42eca8`, elle déréférence bien
  `this->+0x10`, `0x10` est bien un **offset de champ** et non une adresse, et **le lift est fidèle** — le site
  d'appel charge et passe exactement ce que le flux d'instructions dit. Seule l'identité de la globale change.
- **État mesuré de la bonne cible** : `0x51ef18` est référencée **172 fois** dans le `.text` d'origine (globale
  très utilisée), et **aucun** accès direct à `0x51ef24` n'existe — l'écriture passe donc par un `this`, comme pour
  l'autre. Watchpoint sur `0x51ef24` en cours ; c'est elle qui tranchera.

### 2026-08-01 — [DIAG][CORRECTION-2] **La correction précédente était FAUSSE — et le vrai enseignement est que j'ai raisonné deux fois sur des POSITIONS au lieu de mesurer**

- **Rétablissement** : la globale est bien **`0x51efd0 + 0xc`**, comme l'entrée initiale le disait. La
  « correction » qui désignait `0x51ef18` est **erronée** et est annulée.
- **Ce qui tranche, et c'est vérifiable par n'importe qui** : la ligne de définition est
  `sub_42e14e(uint64_t __esp, uint64_t v256, uint64_t v33, uint64_t v37, uint64_t v20, uint64_t v17, uint64_t v19,
  uint64_t v15)` et la convention d'émission est `sub_X(__esp, eax, ecx, edx, ebp, esi, edi, ebx)` ⇒ **`v33` est
  `ecx`**. Or le corps fait `v34 = (v33 & 0xffffffff)` puis `v49 = *(uint32_t*)(v34 + 0xc)`. Donc la base est
  `ecx = 0x51efd0`.
- **Pourquoi je me suis trompé la 2ᵉ fois** : j'ai vu `v34` apparaître dans le **slot esi** d'un appel sortant
  (`sub_42d86f(..., v21, v34, v19, v15)`) et j'en ai conclu que `v34` **était** esi. Faux : c'est la valeur que
  l'appelant **place** dans l'esi de l'appelé — un simple mouvement de registre. *Un paramètre se lit dans la
  DÉFINITION, jamais dans un site d'appel sortant.*
- **⭐ Le vrai enseignement, et il vaut plus que le bug** : les deux fois, j'ai déduit un registre d'une **position**
  (dans une trace, puis dans un appel) au lieu de le **mesurer**. Et les deux fois, des mesures ultérieures sont
  venues **confirmer** la fausse piste — parce qu'elles portaient sur le mauvais objet et qu'un objet non concerné
  n'est, lui non plus, jamais écrit. **Une mesure ne valide pas l'hypothèse qui a choisi sa cible.** C'est la
  version « diagnostic » du piège des 104 FAIL et des compteurs : *qualifier la nature de ce qu'on mesure avant
  d'en tirer une cause* (70 §7) — ici, qualifier **quel objet** on mesure.
- **Faits VÉRIFIÉS par mesure directe** (pas par lecture de code), à la faute :
  - `[0x51efd0+0xc] = 0` **et** `[0x51ef18+0xc] = 0` (les deux, donc la mémoire seule ne discrimine pas — c'est
    précisément ce qui a rendu l'erreur confortable) ;
  - les **deux** objets sont **construits** : vtables `0x4cab90` et `0x4d0b94` réellement écrites — donc le
    problème n'est **pas** un constructeur global qui ne tourne pas ;
  - watchpoints sur `0x51efdc` **et** sur `0x51ef24` : **aucune des deux ne se déclenche** avant la faute.
- **Énoncé net du mur** : `0x51efd0` est un objet global **construit**, dont le membre `+0xc` est un pointeur
  **initialisé paresseusement** et resté nul ; `sub_42e14e` le charge et le passe comme `this` à `sub_42eca8`, qui
  déréférence `this->+0x10`. **Reste à trouver ce qui doit le remplir** — et, puisque ce n'est pas le constructeur,
  c'est un chemin d'initialisation paresseuse (un « get-or-create ») qu'on n'emprunte jamais.

### 2026-08-01 — [DIAG][BORNÉ→PIVOT] **Mur `0x10` : diagnostic FERME et borné — l'objet est construit, un membre lazy reste nul, et le vrai levier est un outil, pas ce binaire**

- **Fait décisif, mesuré des DEUX côtés** (winedbg `--gdb` en remote gdb, au même point `0x42e14e`) :
  - **sous Wine** : `[0x51efd0+0xc] = 0xed63f0` — un **pointeur tas**, l'objet lazy existe ;
  - **sous ARET** : `[0x51efd0+0xc] = 0` — il n'existe pas.
  L'objet **conteneur** `0x51efd0`, lui, est **construit des deux côtés** (vtable `0x4cab90` écrite, observé sous ARET
  par watchpoint). ⇒ Ce n'est **ni un bug de lift** (le site d'appel charge fidèlement le champ), **ni un
  constructeur global manquant**. C'est un **membre à initialisation paresseuse** (`get-or-create` via `new`) dont
  le chemin de création **ne s'exécute pas sous ARET**.
- **Pourquoi la localisation exacte est bloquée ICI** (limite d'outillage, pas de méthode) : trouver l'écrivain
  demande une **watchpoint données côté Wine**, et dans ce bac à sable — pas de desktop réel — les trois voies
  échouent : le **stub gdb** de `winedbg --gdb` **ignore les watchpoints matérielles** ; **winedbg natif** se bloque
  sur le pilotage par pipe ; et la recherche **statique** de l'écrivain est ambiguë (le setter reçoit `this` en
  paramètre thiscall, sans immédiat `0x51efd0` à tracer, et « store `eax` à `+0xc` après un `call` » a **118**
  occurrences — le motif de tout constructeur). Aucune de ces trois n'est un cul-de-sac de fond, seulement de
  moyens.
- **⭐ Le vrai levier est GÉNÉRAL, et il est déjà nommé (doc 81 §4)** : un **diff d'exécution ARET↔Wine** — logguer
  les appels d'imports (nom + args + retour) des deux côtés et **diffe** — donnerait la **première divergence** d'un
  gros binaire GUI **directement**, au lieu de la remonter à la main mur par mur. Cette classe de murs (« un retour
  HLE incorrect en amont fait abandonner MFC », I5) ne se traite pas efficacement un objet nul à la fois : elle se
  traite avec l'outil qui **pointe la divergence amont**. C'est le prochain incrément à fort levier, à opposer à la
  forensics mono-binaire qui, elle, est **bornée et documentée** ici (§2 « borner puis pivoter »).
- **Ce qui reste ACQUIS de ce fil**, indépendamment de WinMerge : la brique `_except_handler4_common` est **exercée
  par du vrai code MFC** sur le chemin de faute (handler `0x4b5684` = frame v4 de `sub_44bf51`), et le pipeline de
  diagnostic (traceur I1 → C généré → winedbg remote gdb) est validé de bout en bout — winedbg **fonctionne** en
  remote gdb pour lire la mémoire à un breakpoint, ce qui n'était pas établi avant cette session.

### 2026-08-01 — [I11][DIAG] **Le diff d'exécution TRANCHE : le mur MFC n'est PAS un mauvais retour d'API OS — nos constantes GDI diffèrent, et le reste est plus profond**

- **Outil** : `bench/relaydiff.py` (doc 81 §I11) confronte la trace relay d'ARET (`ARET_RELAY=1`, build+run) à celle
  de Wine (`WINEDEBUG=+relay,+loaddll`). Détail de construction et règles d'alignement : 81 §I11.
- **Constat principal sur WinMerge, après filtrage du CRT host-backé des deux côtés** : ARET et Wine **tracent
  ensemble** à la frontière OS/Win32 jusqu'au crash d'ARET. **Aucune** bifurcation de flot réelle, **aucune** API
  créatrice qui rende un handle d'un côté et 0 de l'autre. ⇒ **l'hypothèse « un retour HLE incorrect en amont fait
  abandonner MFC » est ÉCARTÉE pour la surface OS** — c'était pourtant la thèse portée depuis I5. Un outil général
  a réfuté une hypothèse que la forensics mono-objet ne pouvait ni confirmer ni infirmer.
- **Ce que l'outil fait REMONTER, en revanche** (42 divergences de valeur, toutes bénignes pour l'instant mais
  réelles) : nos **`GetSysColor`** rendent les couleurs **Win95 classiques** (`c0c0c0`, `808080`) là où Wine rend
  son **thème moderne** (`f5f5f5`, `a6a6a6`) — 18 occurrences ; **`GetSystemMetrics`** diverge sur 4 indices
  (scrollbars 16 vs 17) ; et surtout **`GetSystemMetrics(0x44/0x45)` (SM_C{X,Y}MENUSIZE) rend 0 sous ARET** contre 4
  sous Wine — une **métrique non modélisée** (retour 0), la plus susceptible des trois de casser un calcul de
  layout en aval (division/indexation par une taille nulle). **Piste à instruire, pas cause prouvée.**
- **Filtres d'alignement, documentés comme ne pouvant masquer une divergence** : le CRT purement computationnel
  (ctype/string/mem/alloc/collate) est **host-backé** par ARET (libc native, non relayée) donc n'apparaît que côté
  Wine ; le jeter des deux côtés compare le **même périmètre** (la frontière HLE), pas msvcrt. Après ce filtrage :
  ARET **940** appels OS avant crash, Wine **142 631** (l'app complète). L'asymétrie résiduelle est le crash
  d'ARET, pas un défaut de l'outil.
- **Valeur de méthode** : c'est le premier outil du projet qui **réfute une hypothèse de diagnostic à l'échelle du
  binaire entier** au lieu de la remonter à la main. Le prochain pas cadré n'est plus « trouver qui écrit
  `[0x51efd0+0xc]` » (frontière OS écartée) mais **soit** modéliser les métriques manquantes (`SM_CXMENUSIZE`…) et
  re-mesurer, **soit** étendre le relay au **CRT non-computationnel** (les retours msvcrt qui portent de l'état :
  `_setmbcp`, `setlocale`, `_get_osplatform`…) pour voir si la divergence est là.

### 2026-08-01 — [I11][DIAG] **Mur `0x10` encore rétréci : TLS écarté, la création NE S'EXÉCUTE PAS sous ARET**

- Le contexte relay juste avant la faute montrait `TlsGetValue(0)` entre Enter/LeaveCriticalSection avec `0x51ef18`
  en argument — le motif **AFX_MODULE_STATE de MFC** (état de module en slot TLS). `TlsGetValue` étant dans
  `RELAY_EXCLUDE`, le diff ne l'avait **jamais comparé** : candidat naturel.
- **⭐ TLS ÉCARTÉ par mesure directe** : la trace ARET montre `TlsAlloc→0`, `TlsSetValue(0, 1442b4a0)`, puis **tous**
  les `TlsGetValue(0)` rendent **`1442b4a0`** (le pointeur d'état de module MFC). L'état est **correctement stocké
  et relu** — pas un échec TLS.
- **Fait robuste, mesuré côté ARET** (watchpoints matérielles fiables sur notre ELF) : la watchpoint sur `0x51efdc`
  (= `[0x51efd0+0xc]`) **ne se déclenche jamais** avant la faute ⇒ **le store de création ne s'exécute jamais sous
  ARET**, alors que sous Wine le champ vaut `0xed63f0`. La création est un `new`+store **gardé par une condition
  fausse sous ARET** — et cette condition est en **C++ lifté**, pas à la frontière OS (le diff l'a établi) ni TLS.
- **Pinpoint du writer bloqué par l'outillage Wine** (stub gdb ignore les hw-watchpoints ; sw-watchpoints et
  winedbg natif se bloquent) — **limite d'outillage documentée, pas de méthode**.
- **Prochain incrément cadré, tool-based** : étendre le relay au **store de l'allocateur** (`operator new`/`malloc`
  **avec site d'appel**, des deux côtés) pour repérer la création présente sous Wine et absente sous ARET.

### 2026-08-02 — [I11][DIAG][CORRECTION] **Mur `0x10` RECADRÉ : l'objet n'est PAS « paresseux gardé par une condition fausse » — c'est un GLOBAL `_initterm` construit inconditionnellement, et la divergence est CONFINÉE à UN constructeur, entre deux instructions connues**

- **Graphe d'objets réconcilié (fin de la confusion `0x51ef18` vs `0x51efd0`)** — les deux adresses des vieilles
  entrées « correction » désignaient **le même objet à deux niveaux** :
  - **`0x51ef18`** = objet **externe** (global), vtable `0x4d0b94`, constructeur **`0x44aabd`** ;
  - **`0x51efd0`** = `0x51ef18 + 0xb8` = **sous-objet membre**, vtable `0x4cab90`, constructeur **`0x42e884`**
    (le caller `0x44aafd` fait `lea 0xb8(%esi),%ecx` avant `call 0x42e884` → `this = externe+0xb8`) ;
  - **`0x51efdc`** = `0x51efd0 + 0xc` = **le champ**, écrit **une seule fois**, à l'instruction **`0x42e8fa`**
    (`mov %eax,0xc(%esi)`) après `operator new(0x18)` (`0x42e8d8` → thunk mfc90u `0x4aab98`) et le
    constructeur du sous-objet alloué (`call 0x470022`).
- **⭐ CORRECTION de fond des entrées des 2026-08-01** : le champ **n'est PAS** « à initialisation paresseuse,
  chemin de création gardé par une condition fausse ». Le writer `0x44aabd` est appelé depuis **`0x4bb39a`**
  (`mov $0x51ef18,%ecx ; call 0x44aabd ; push $dtor ; call 0x4ac427`=atexit) — motif **exact** d'une entrée de la
  **table de constructeurs globaux `_initterm`**. L'objet est donc **construit inconditionnellement au démarrage
  CRT**, comme sous Wine. **Il n'y a aucune « condition fausse » à trouver.**
- **Divergence CONFINÉE, et bornée par le flot de contrôle** : sous ARET la vtable `0x4cab90` **est** écrite en
  `0x51efd0` (mesuré, watchpoint) ⇒ `_initterm` **a** exécuté `0x44aabd`→`0x42e884` jusqu'à **`0x42e89c`**. Le
  champ n'est jamais écrit ⇒ `0x42e884` n'atteint **jamais** `0x42e8fa`. Or les **deux** bras du test
  `new==0` (`je 0x42e8f4`) **convergent** sur `0x42e8fa` (l'un stocke le pointeur, l'autre stocke 0) : le store
  est donc inévitable **sauf si un appel intermédiaire ne rend pas la main normalement**. Suspects, tous entre
  `0x42e89c` et `0x42e8fa` : `call *0x4bd65c` ×2 (import mfc90u, ~50 sites), `call 0x402132` ×2 (ctor membre
  local), `operator new` (`0x4aab98`, import mfc90u), `call 0x470022` (ctor du sous-objet). ⇒ **le mur est un
  appel dans du code mfc90u LIFTÉ (ou un ctor membre) qui, sous ARET, déroute au lieu de retourner** — pas une
  garde applicative, pas la frontière OS (le diff relay l'avait déjà écarté), pas TLS.
- **Oracle Windows réel (GitHub Actions) — l'infra MARCHE, la mesure a été bornée** : le workflow
  `windows-watchpoint.yml` télécharge WinMerge 2.14.0 (sha256 vérifié `cb886017…`), installe VC90, localise `cdb`
  — **les 4 premières étapes vertes** (le correctif `curl -L` + `vcredist2008` du run #1 a tenu). Mais l'étape de
  watch a **timeouté à 8 min sans déclencher** : le log montre **tout** le stack MFC/GUI chargé (MFC90ENU,
  COMCTL32, ole32, OLEAUT32, UxTheme, MSCTF…) puis un **idle** — WinMerge **sans argument** entre dans sa boucle
  de messages sans jamais exercer le writer. **Deux causes, toutes deux comprises** : (1) `ba w4` posée au
  **break initial du loader** ntdll est **non fiable** (les registres de debug sont réinitialisés à la création
  des threads) ; (2) headless-sans-argument, l'objet global est pourtant construit **au CRT-init** (avant la
  boucle) — donc la voie robuste est un **breakpoint LOGICIEL sur l'instruction writer exacte** `bp 0x42e8fa`
  (pas d'ASLR ⇒ adresse runtime = adresse statique), qui se déclenche **en quelques secondes** au démarrage. À
  retenir : *un point d'arrêt matériel posé trop tôt ne watch rien ; sur cible sans ASLR, préférer `bp` sur
  l'adresse exacte du writer, prouvée statiquement.*
- **Prochaine mesure, tool-based et en bac à sable (pas Wine)** : build ARET de WinMerge avec **`ARET_TRACE=1`**
  (traceur I1, ring-buffer vidé au crash) → la **queue de trace** dira **lequel** des appels intermédiaires de
  `0x42e884` fut le dernier entré avant la faute `0x42e14e`, donc **quel** appel mfc90u lifté déroute. Mesure en
  cours.

### 2026-08-02 — [I5][HLE-WIN32][DIAG] **Le mur WinMerge a BOUGÉ (mesuré, pas supposé) : le champ `0x51efdc` est franchi — nouveau mur = `SHGetSpecialFolderLocation` ; famille CSIDL/PIDL shell32 implémentée et vérifiée bit-identique Wine**

- **Mesure décisive (build `ARET_TRACE=1` de WinMerge+mfc90u, exécuté headless)** : l'ELF tourne **46 252 entrées de
  fonctions** avant l'`aret_unimpl`, et l'abort n'est **plus** le champ nul `[0x51efd0+0xc]` — c'est
  **`SHGetSpecialFolderLocation`**. Contrôles dans la trace : `sub_42e884` (le ctor du sous-objet, cf. entrée
  précédente) **a tourné** (compté 1), et l'ancien consommateur `sub_42e14e` **n'a jamais tourné** (compté 0) ⇒
  **le mur `0x10` est derrière nous** dans ce build. Il n'a **pas** été « corrigé » par un fix ciblé : les
  incréments HLE de la session (SEH v4, Str*/charset/desktop, etc.) ont déplacé le flot, et la **re-mesure** l'a
  montré — exactement pourquoi la doctrine dit *mesurer, pas affirmer*. La forensics statique du `0x51efdc` reste
  utile (elle a **corrigé le dossier** : global `_initterm`, pas garde paresseuse) mais le binaire ne l'atteint plus.
- **`SHGetSpecialFolderLocation` vient de mfc90u LIFTÉ** (absent du désassemblage de WinMergeU.exe) : MFC résout un
  dossier spécial au démarrage. WinMergeU.exe importe la famille : `SHGetSpecialFolderLocation`,
  `SHGetPathFromIDListW`, `SHGetMalloc`, `SHBrowseForFolderW`, `SHGetDesktopFolder`, `SHGetValueW` ; mfc90u ajoute
  `SHGetSpecialFolderPathW`/`SHGetFolderPath{W,A}`.
- **Famille CSIDL/PIDL modélisée d'un bloc, SOUND (`aret_win32.c`)** :
  - `SHGetSpecialFolderLocation(hwnd,csidl,&pidl)` → un **PIDL synthétique** : bloc `CoTaskMem`-tracké
    `[magic 'APIL'][chemin Windows\0]` ; `SHGetPathFromIDList{W,A}(pidl,path)` le **décode**. Un PIDL étranger
    (impossible dans notre monde : les énumérateurs de namespace shell ne sont pas modélisés) → **FALSE défini**,
    jamais un chemin faux.
  - `SHGetSpecialFolderPath{W,A}` et `SHGetFolderPath{W,A}` → le chemin **directement**.
  - `SHGetMalloc(&pMalloc)` → **le MÊME singleton IMalloc** que `CoGetMalloc` (refactor `u32_get_imalloc`) ⇒ le
    PIDL se libère via `IMalloc::Free`/`CoTaskMemFree` (le `u32_com_track` partagé). C'est l'idiome exact de MFC.
  - `csidl_to_winpath` mappe **34 CSIDL** vers un profil utilisateur standard (`C:\users\aret\AppData\Roaming`…),
    masque les `CSIDL_FLAG_*`, crée le dossier natif (mkdir -p sous le préfixe, via `translate_path` exposé).
    **CSIDL inconnu → échec DÉFINI** (`E_INVALIDARG`/`FALSE`), jamais un chemin deviné (principe sacré §0).
  - **Choix de soundness** : la *valeur* exacte d'un chemin de dossier spécial est **environnementale** (nom
    d'utilisateur, layout OS) et **n'est pas** une propriété de justesse — n'importe quel dossier valide et
    inscriptible du bon type est correct. Le fixture ne compare donc **pas** les octets du chemin (ils diffèrent
    légitimement d'avec Wine) mais le **contrat** : codes de succès, chemin non vide, PIDL qui round-trippe, accord
    entre les deux familles d'API, et l'échec défini du CSIDL inconnu — **tout bit-identique Wine**.
- **Vérif & portes** : `winecorpus/shell_folders.c` = **bit-identique Wine** (`ok    shell_folders`, winediff 1/1) ;
  `-lshell32` ajouté à la ligne de link partagée de `winediff.sh` (shell32 était absent) ; **stdcall_audit PASS**
  (8 nouveaux `@N` : `SHGetSpecialFolderLocation@12`, `SHGetPathFromIDList{W,A}@8`, `SHGetMalloc@4`,
  `SHGetSpecialFolderPath{W,A}@16`, `SHGetFolderPath{W,A}@20` ; 0 manquant) ; **hash transpile inchangé**
  `19acad982194bf07` (additif, zéro impact comportemental). `#include <stddef.h>` ajouté à `aret_hle.h` (le
  `size_t` de `translate_path` exposé).

- **⭐ Advance CONFIRMÉ par re-build (le fix a bougé le driver)** : WinMerge re-transpilé **avec** la famille
  CSIDL franchit `SHGetSpecialFolderLocation` et bute désormais **plus loin**, sur un `aret_unmodelled` **nommé** :
  `CoCreateInstance` de la classe **`{275C23E2-3747-11D0-9FEA-00AA003F8646}`** (= **CLSID_CMultiLanguage**, mlang)
  en **`{275C23E1-…}`** (= **IMultiLanguage**) — *« 0 lifted module offered DllGetClassObject »* parce que WinMerge
  est lifté avec **mfc90u seul**, pas mlang. **Prochain mur = activation COM de mlang** : soit `--with-dll
  mlang.dll` (l'infra COM in-proc existe, §COM), soit un objet `IMultiLanguage` en HLE. Mur propre, borné, nommé.
- **Oracle Windows réel (run #3, `bp 0x42e8fa` logiciel)** : **succès en 46 s** — le breakpoint logiciel sur
  l'instruction writer exacte se déclenche au CRT-init comme prévu, confirmant la leçon du run #2 (le `ba w4` posé
  au break loader ne tenait pas). L'infra oracle-Windows est désormais **fiable** pour un writer à adresse fixe.

### 2026-08-02 — [I5][HLE-CRT][HLE-WIN32][LARGEUR] **Cluster CRT/console/handle piloté par la LARGEUR mesurée (wallsweep) — 11 shims, bit-identiques Wine**

- **Méthode = mesurer la largeur d'abord** : `bench/wallsweep.sh bench/gauntlet/bins` (21 binaires CLI, 19 avec murs)
  classe les murs d'imports par **#binaires bloqués**. Tête : `__p___argv` (9), `popen/pclose` (8), `DuplicateHandle`/
  `GetHandleInformation`/`dup2`/`_get/_setmaxstdio` (7), `SetConsoleTextAttribute`/`raise` (6), `_fstat64`/`_wunlink` (4).
  **Enseignement stratégique** : `mlang` (le mur WinMerge) n'apparaît **nulle part** ⇒ il est *étroit* (spécifique
  GUI/MFC) ; la vraie largeur est un **cluster CRT/console/handle** dont chaque shim débloque 4-9 binaires.
- **Implémenté (sound, general)** : `_dup`/`_dup2` (⚠️ msvcrt `_dup2` rend **0** au succès, pas le fd POSIX),
  `_getmaxstdio`/`_setmaxstdio` (512 par défaut), `_fstat64`/`_stat64` (nouveau `struct _stat64` 56 o, times 64-bit),
  `_wunlink`, `raise` (via la **même table de dispositions** que `signal`/`_XcptFilter` : SIG_IGN avale, handler
  installé one-shot via `aret_call`, SIG_DFL termine bruyamment), `__p___argv`/`__p___argc` (+ data-imports
  `__argv`/`__argc` → `&aret_real_argv`/`&aret_real_argc`), `GetHandleInformation`/`SetHandleInformation`,
  `DuplicateHandle` (handles = fds ⇒ `dup`, `DUPLICATE_CLOSE_SOURCE` gérée), `SetConsoleTextAttribute`.
- **⭐ Soundness `SetConsoleTextAttribute`** : le retour n'est **pas** un TRUE aveugle. Une API console **échoue
  (FALSE)** sur un handle **redirigé** (non-console) — exactement le cas de stdout sous `winediff`. Le retour est
  donc `isatty(fd)` : TRUE sur un vrai terminal, FALSE sous pipe/fichier ⇒ fidèle à Windows/Wine dans les deux cas
  (la couleur reste un no-op hors-bande sur les octets). Un TRUE constant aurait été **faux** (et aurait cassé
  l'oracle).
- **Vérif & portes** : `winecorpus/crt_console_cluster.c` = **bit-identique Wine** (sortie *significative* : chaque
  contrat vérifié, valeurs env-spécifiques réduites à des booléens) ; `stdcall_audit` **PASS** (seul `@N` ajouté :
  `GetHandleInformation@8` — `DuplicateHandle@28`/`SetHandleInformation@12`/`SetConsoleTextAttribute@8` existaient
  déjà) ; **hash transpile inchangé** `19acad982194bf07`. Débloque une large part des 19 binaires du gauntlet en une
  passe — le rendement de la **priorisation par la largeur** (levier 0).
- **Note stratégique (échange avec l'utilisateur)** : la vraie accélération « en gros » = **générer depuis les
  sources ouvertes, compilé DANS le binaire** (autonome), pas lier Winelib au runtime (dépendance Wine = refusé) :
  win32metadata → tuyauterie (`@N`/signatures) ; sources Wine (C portable des DLL user-mode) → comportement, avec un
  plancher `ntdll`/win32u fini (~131 syscalls NT) porté une fois. À analyser au prochain incrément.
