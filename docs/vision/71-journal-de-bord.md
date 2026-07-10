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
- **funcdiff** (Unicorn, fonction) : **closure** (suit les appels directs récupérés,
  discipline call/ret exacte, retaddr sentinelle non-mappée, frames OFF) + **opt-diff**
  (post-opt SSA vs pré-opt : DCE ne supprime jamais un Store, opt ne touche pas le
  CFG). memcpy/rep-stos modélisés ; adresses masquées 32-bit. **`0 divergence`
  ≠ pas de bug** : dit *où il n'est pas* (bugs profonds derrière imports/skips).
- **Portes** : difftest (décompile O0→O3, **271/271**), transpile-diff (produit, **4/4**,
  hash **`19acad982194bf07`**), winediff (Wine, **49/49**), sweeps (sqlite/busybox/
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

<!-- NOUVELLES ENTRÉES ICI (garder l'ordre chronologique, plus récent en bas) -->
