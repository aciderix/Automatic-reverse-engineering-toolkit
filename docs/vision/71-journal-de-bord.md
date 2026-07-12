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

<!-- NOUVELLES ENTRÉES ICI (garder l'ordre chronologique, plus récent en bas) -->
