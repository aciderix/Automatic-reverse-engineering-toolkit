# 80 — Orientations architecturales : dépasser les plafonds (threads, DLL, SEH, indirects, x87)

> Document de conception. Récapitule cinq orientations proposées (origine : échange
> avec Gemini, 2026-07-12), leur **verdict d'ingénierie honnête** (faisabilité ×
> utilité × conformité), l'architecture retenue pour le prochain chantier
> (**threads coopératifs / fibers**), et — le plus important — l'**analyse de
> conformité au principe sacré, à la doctrine (§1) et aux règles** (doc 70 §0/§1/§2).
>
> **Statut** : orientations validées comme *compatibles* avec la philosophie ARET ;
> ordre de priorité fixé ; le chantier **fibers est COMPLET** (incréments 1-4, 2026-07-16).

## 0. Rappel du cadre (ce contre quoi on juge)

- **Principe sacré** (70 §0) : (1) jamais de faux présenté comme correct — juste ou
  **abort bruyant** ; (2) non-sûr ⇒ `Asm`/abort ; (3) **jamais de rustine par
  binaire**, cause générale ; (4) **rien de prouvé = rien de deviné**.
- **Doctrine — réutilisation vérifiée** (70 §1) : une brique réutilisée n'est jamais
  une boîte noire de confiance ; ARET la **vérifie et emballe** dans « correct ou
  abort », mêmes oracles que du code maison.
- **Contraintes dures** (utilisateur) : binaires **autonomes** (zéro dépendance
  runtime), **universels** (ELF **et** WASM), déterministes (protège les oracles).

**Le fil rouge des cinq idées** : passer de « *deviner et mimer* » (HLE manuel,
heuristiques statiques) à « *traduire factuellement* » (lifter les DLLs, tracer avec
Unicorn). **Ce glissement RÉDUIT le devinement** (règle 4) au lieu de l'augmenter —
donc il est *plus* aligné, pas moins, **tant que chaque brique reste emballée dans
« correct ou abort »**. C'est la condition unique et non négociable.

---

## 1. Les cinq orientations — verdict honnête

### 1.1 Fibers (multithreading coopératif) — ✅ RETENU, priorité 1
`CreateThread` = une **coroutine** (fiber) multiplexée sur un seul thread hôte ; on
ne bascule qu'aux **points bloquants** (Sleep/Wait/GetMessage). Détail archi : §2.
- **Faisabilité** : ✅ mesurée. `ucontext`/`swapcontext` marche en `-m32` ; mingw
  bâtit des binaires multi-thread et Wine les exécute (`4 threads×1000 sous section
  critique → counter=4000`, **déterministe**). Oracle **propre**.
- **Utilité** : haute (classe entière des programmes MT).
- **Conformité** : ✅ **totale**. Déterministe ⇒ oracle différentiel valide ⇒ « correct
  ou abort » tenable. Pas de data-race, pas de barrière mémoire (un seul fiber court).
  Limite préemption (un thread CPU-bound qui ne yield jamais starve les autres) =
  **hang, pas faux** ⇒ détection → **abort sound**. Autonomie : `ucontext` = libc
  natif, lié statiquement.
- **Nuance honnête** : `ucontext` **n'existe pas en WASM** → natif d'abord ; WASM-MT
  via **Asyncify** (transform build-time) plus tard. **Règle** : le build WASM sans
  Asyncify doit **aborter** sur `CreateThread`, jamais diverger en silence.

### 1.2 Lifting de DLL binaire (le « sablier / BYO-DLL ») — ✅ endgame M7-GUI
Prendre le **PE binaire** d'une DLL (comctl32 ReactOS, …) et le passer au **lifter
ARET** comme n'importe quel `.exe` ; exposer ses **exports** ; le loader multi-modules
résout `app.exe` ↔ `dll.c` liftée ; le compilateur hôte les linke. La DLL liftée finit
par appeler le HLE (`aret_CreateWindowExA`, `aret_DrawText`…) pour le rendu.
- **Faisabilité** : ⚠️ moyenne. **Nouveau dev lifter** : parser l'**Export Directory**
  + **loader multi-modules** (résolution inter-modules). **Mur win32k** : user32/gdi32
  descendent aux syscalls noyau → à router vers le HLE (« surgical FLIRT ») ; mais
  **comctl32 est user-mode** (dessine via user32/gdi32 que j'ai) → **c'est LA cible**.
- **Utilité** : haute — logique widgets « gratuite », sans deviner.
- **Conformité** : ✅ **c'est la forme la plus pure de la doctrine §1**. Le comctl32
  lifté **EST** la vraie logique (lift validé par **cpudiff/funcdiff**) posée sur mon
  gdi32 (**winediff**) → **correct par composition** ; deux oracles que j'ai déjà, sans
  observer la peinture de Wine (ce qui **règle l'oracle widget** bloqué en HLE manuel).
  Code lifté non gérable (asm manuel, syscall) ⇒ abort sound. Autonomie : lifté → C →
  compilé dans l'ELF/WASM, **zéro runtime** (mieux que Winelib).
- **Contrainte externe (pas de soundness)** : **licence**. Win95 = proprio
  non-redistribuable ; ReactOS binaire = **GPL** (contamine la sortie) ; WineLib = LGPL.
  Décision consciente à prendre.
- **Statut** : **✅ RÉALISÉ ET DÉMONTRÉ (2026-07-18)**. Loader multi-modules complet (`load_with_modules` : Export
  Directory + imports-avec-DLL-source + résolveur inter-modules + rebaser + fusion + routage IAT), flag CLI `--with-dll`,
  invocation du **DllMain** des DLL liftés, **résolveur delay-load runtime**. **De vrais contrôles comctl32 tournent
  bit-identiques à Wine**, liftés de la vraie comctl32.dll sur le HLE gdi32 : ImageList (stateless) **et une progress bar
  stateful complète** (classes via DllMain, WM_CREATE→`cbWndExtra`, messages→WNDPROC lifté, theming uxtheme→classic).
  Détail : doc 70 §5.0 (Levier 1) + doc 71 (entrées 2026-07-18). Reste : les autres contrôles = **même machinerie**
  (data-driven) ; MFC/VB par le même lifting ; win32k `NtGdi*`/`NtUser*` seulement si on lifte gdi32/user32 eux-mêmes.

### 1.3 SEH / RtlUnwind natif dans le HLE — ✅ compatible, gros chantier
Implémenter le SEH **entièrement dans le runtime** (ne PAS lier l'EH C++ de l'hôte).
- **Faisabilité** : ⚠️ moyenne, **sous-estimée par le framing « setjmp »**. Le vrai SEH
  MSVC est **table-driven** (`__except_handler3` + scope tables), pas setjmp : il faut
  reproduire le **dispatch** (parcourir `fs:[0]`, appeler le handler, lire la
  disposition, sauter au continuation address). Fautes **matérielles** (NULL-deref,
  div0) : catchables natif via `SIGSEGV`+longjmp mais **pas en WASM** (pas de signaux) ;
  exceptions **logicielles** (`RaiseException`, `throw`) OK partout. **Testabilité ici :
  mingw i686 ne compile pas `__try/__except`** → binaires MSVC requis.
- **Utilité** : haute (C++/Delphi, logiciels Win32 2000s).
- **Conformité** : ✅ in-HLE + abort sound sur ce qu'on ne modélise pas. WASM : SEH
  matériel indisponible ⇒ **abort sound** (jamais divergence silencieuse).
- **Statut** : **engagé — 3 primitives de dispatch faites et prouvées vs Wine** (2026-07-17, doc 70 §5 P3.5-P3.8) :
  `push imm;…;ret` (ret-as-jump `__finally`), **software `RaiseException`** (parcours `fs:[0]`), **local unwind
  `RtlUnwind`** (i386 : retour normal, TargetIp ignoré — la « muraille de testabilité » était une erreur de
  mental-model x64), **fautes matérielles** (SIGSEGV/SIGFPE→dispatch, pile scratch dédiée car l'esp du point de faute
  est irrécupérable mais inutile — le handler restaure l'esp depuis son registration record). Chaque brique = fixture
  SEH inline-asm (mingw n'a pas `__try`) bit-identique Wine. **Reste** : `__except_handler3` réel (scope-table, testable
  seulement avec un **vrai binaire MSVC `__try`**) + exceptions C++ (`__CxxFrameHandler`). La testabilité globale n'était
  donc PAS un blocage : elle l'était par **brique**, chacune fixturable isolément.

### 1.4 Profile-Guided Lifting (Unicorn comme guide) — ✅ **si opt-in**
Tracer les cibles d'appels indirects (`call eax`, `jmp [edx]`) via Unicorn, générer un
`switch(eax)` des cibles observées ; **`default:` → `aret_unmodelled()`**.
- **Faisabilité** : ✅ haute (infra Unicorn **déjà là**, cf. cpudiff).
- **Utilité** : moyenne (vtables COM/C++ massives ; le résidu que le statique n'atteint
  pas — le bug NASM `-f obj` cité est **déjà résolu statiquement**).
- **Conformité** : ✅ **sound par construction** (observé → switch, non-observé → abort).
  **MAIS** rend la sortie **dépendante du profil** (abort sur chemin non profilé) →
  moins « natif ». **Règle** : **mode opt-in**, **jamais** activé sur les démonstrateurs
  100%-statiques (Lua/sqlite/nasm/busybox) — ne pas diluer la garantie statique.
- **Statut** : escape-hatch optionnel pour binaires durs. Additif, faible risque.

### 1.5 SoftFloat (floatx80) pour x87 universel — ⚠️ à moitié juste
- **Le vrai problème existe** : le filet x87 runtime utilise `long double` = 80-bit sur
  x86 mais 64/128-bit sur ARM/WASM → **divergence cross-plateforme**. SoftFloat
  **`floatx80`** donnerait des **ops de base** (add/mul/div/sqrt) **80-bit déterministes
  partout**. Réel et utile pour l'objectif ARM/WASM.
- **La faille** : SoftFloat **n'a pas** `fsin/fcos/ftan` — les transcendantales **ne
  sont pas dans IEEE-754**. Le `fsin` x87 = **approximation polynomiale Intel** (à
  répliquer en microcode, pas SoftFloat). Le titre « sin(x) même bit partout » est
  **faux**.
- **Conformité** : ✅ améliore la soundness (ops de base) ; ne résout pas les
  transcendantales.
- **Statut** : basse priorité, pour l'ère ARM/WASM. Ne pas croire au titre.

### 1.6 Rétro-cible **Windows moderne** (vieux binaire → tourne sur Windows 11) — ✅ compatible, deux cas très distincts
**Idée (utilisateur, 2026-07-19)** : aujourd'hui ARET fait PE32 → **ELF/WASM** (autre OS). Ajouter une **cible PE Windows
récent** : prendre un **vieux** binaire Windows et le rendre **fonctionnel sur Windows 11**. L'archi rend ça **presque
gratuit pour une moitié, très cher pour l'autre** — il faut séparer nettement.

**Ce qui est déjà là (le lifter est OS-agnostique).** `frontend (PE→IR)` → `lifter (x86→C, indépendant de l'OS cible)` →
`HLE (réimplémente Win32)` → `backend (C→binaire)`. Pour cibler Windows, **deux maillons** seulement changent : (a) le
**backend émet un PE** (mingw/MSVC compile déjà du C → PE) ; (b) le **HLE** *forwarde vers le vrai Win32* pour toute API qui
**existe encore** sur Win11 (plus fidèle que l'ému POSIX), et **garde/embarque** sa réimplémentation pour les API
**supprimées/changées**.

- **Cas 1 — vieux 32-bit → Windows moderne : ⚠️ faisabilité HAUTE, utilité MOYENNE.** La plupart des .exe 32-bit tournent
  déjà via **WoW64**, donc le gain n'est pas « les faire tourner » mais : **autonomie/bundling** (un seul PE, zéro « VB6
  runtime / MFC42.dll / MSVCRT manquant » — douleur d'archivage réelle) et **ressusciter les API retirées** (`WinHelp`/`.hlp`
  absent depuis Vista, vieux DirectDraw, composants dépréciés) qu'ARET réimplémente et embarque. Petit incrément (surtout le
  backend PE), bénéfice **ciblé** (les apps qui *cassent*).
- **Cas 2 — vieux 16-bit (NE) → Windows 64-bit : ✅ faisabilité MOYENNE-BASSE, utilité UNIQUE.** Windows 64-bit **ne peut PAS**
  exécuter du 16-bit (NTVDM retiré) → aujourd'hui il faut un émulateur (winevdm/otvdm). **Le volume de vieux logiciel est
  LÀ** (mesure Chip CD : ~424 PE dont 49 PE32, **le reste = NE 16-bit**). ARET lifterait le 16-bit → **PE 64-bit natif** —
  ce que *rien* ne fait nativement. Mais **nouveau frontend lifter** : mémoire **segmentée**, real/protected-mode 16-bit,
  surface **Win16** (USER/GDI/KERNEL 16), loader **NE**. Chantier **multi-sessions**, du même ordre que le 64-bit (Phase 8).
- **Conformité** : ✅ **totale**. Le glissement « traduire factuellement » est intact. Nuance sur l'**autonomie** : cibler
  Windows *dépend* du Win32 de Win11 — mais c'est **le but** (tourner *sur* Windows), donc utiliser ses API est **correct**,
  pas un compromis. L'autonomie se **redéfinit** : « **zéro dépendance au runtime supprimé** » (sous-système 16-bit, runtime
  VB6, winhlp32), qu'ARET compile dedans. **Bonus oracle** : la cible EST Windows → le **vrai Win32 = vérité terrain**
  (winediff devient natif, encore plus fidèle). Non-modélisé ⇒ abort sound, comme toujours.
- **Statut** : orientation **enregistrée** (non engagée). Ordre de valeur : (1) **backend PE** (cas 1) = petit incrément
  prouvant le concept + bundling/WinHelp immédiat ; (2) **frontend 16-bit** (cas 2) = **le grand prix**, jalon dédié
  planifié (comme le 64-bit). Prérequis partagé avec Phase 8 (élargir le lifter au-delà du i386 32-bit).

---

## 2. Architecture retenue — threads coopératifs (fibers)

**Principe** : un seul fiber court à la fois ; bascule **uniquement** aux points
bloquants → zéro data-race, ordonnancement **round-robin déterministe** → oracle
reproductible bit-à-bit.

**Table de fibers** `g_fiber[N]` (fiber 0 = thread principal) :
```
ucontext_t ctx;         // contexte C hôte (registres + SP hôte)
void*      host_stack;   // pile C hôte (malloc)
uint32_t   mstack_base;  // pile machine émulée dédiée (malloc 32-bit, esp au sommet)
uint32_t   teb, last_error;   // état HLE PAR-FIBER (aujourd'hui global)
int        state;        // READY / RUNNING / BLOCKED / DONE
uint32_t   wait_on;      // handle attendu si BLOCKED
```
**Par-fiber** (aujourd'hui global) : `last_error`, `TEB`. `esp` déjà passé par valeur
(pile machine par fiber) ; `ebp` threadé en registre-param (per-appel). Chirurgical.

**`CreateThread(start, param)`** : alloue pile machine + pile hôte, `makecontext` →
trampoline `aret_call(start, esp, param)` puis DONE + signale le handle. READY (ne
court pas encore — coopératif). Retourne un handle attendable.

**Scheduler** `u32_yield()` : `swapcontext` → prochain READY (round-robin) → lui.
Aucun READY alors que le courant est BLOCKED = **deadlock → abort sound**.

**Primitives bloquantes → yield** (remplacent les no-op actuels) : `Sleep`, `WaitFor
Single/MultipleObjects` (join inclus), `GetMessage` ; `EnterCriticalSection` (owner +
récursion + file d'attente), `SetEvent/ResetEvent`, Mutex, Semaphore.

**Plan incrémental (règle §2, piloté par fixture)** :
1. ✅ **FAIT (2026-07-16)** — Infra fibers + `CreateThread` (+`CREATE_SUSPENDED`/
   `ResumeThread`/`ExitThread`/`GetExitCodeThread`) + `WaitForSingle/MultipleObjects`
   (join) + `Sleep`=yield + `last_error` par-fiber. Fixture `thread_join.c` : 4 threads,
   somme déterministe `25800` + isolation `last_error` à travers un yield, bit-identique
   Wine. winediff 102/102, hash transpile inchangé. Détail : doc 70 §4.7, doc 71.
2. ✅ **FAIT (2026-07-16)** — `CriticalSection` réelle (table keyée par `&cs` : owner
   fiber + récursion ; Enter bloque un acquéreur d'un autre fiber tant que l'owner ne
   Leave pas ; Try/Delete/Init*). Oracle `thread_critsec.c` : **`counter=4000`** avec le
   read-modify-write **coupé par un yield sous le lock** (discriminant réel — un lock
   no-op perdrait des incréments), + récursion. Bit-identique Wine, winediff 103/103.
3. ✅ **FAIT (2026-07-16)** — Events (`CreateEventA/W`, manual-reset/auto-reset,
   `SetEvent`/`ResetEvent`) intégrés à `WaitForSingle/MultipleObjects` : bloque tant que
   non signalé, re-vérifie après chaque réveil (auto-reset = un seul waiter consomme).
   Oracle `thread_event.c` : gate manual release-all (`60`) + ping-pong auto-reset (`15`),
   bit-identique Wine, winediff 104/104.
4. ✅ **FAIT (2026-07-16)** — Mutex (récursif, waitable, abandon=libre), Semaphore
   (compteur borné), TLS **par-fiber** (`aret_tls[fiber][slot]`, valeurs par-fiber),
   `_beginthreadex`/`_beginthread` (factorisés sur `CreateThread`). Modèle d'acquisition
   unifié (event auto-reset/mutex/sem, re-check après réveil ⇒ un seul consomme). Oracle
   `thread_mutex_sem.c` (mutex `2000`, TLS `ok`, sémaphore `21`), bit-identique Wine,
   winediff 105/105.

5. ✅ **FAIT (2026-07-16)** — **Timeouts finis** via **horloge virtuelle déterministe**
   (débloqué par un vrai workload : `WaitForSingleObject(h, 50)` comme sonde de vivacité
   dead-lockait avec le modèle « fini==infini »). `WaitFor*`/`Sleep` finis posent une
   échéance ; quand rien n'est signal-runnable, le scheduler avance l'horloge à la plus
   proche et réveille les timed-out (`WAIT_TIMEOUT`). Déterministe (horloge ≠ wall-clock).
   Oracle `thread_pool.c` (pool réaliste : file+mutex+sémaphore+event+TLS+timeout, `2686700`).

**⇒ Chantier fibers (incréments 1-5) COMPLET.** Reste hors-scope (abort sound) : préemption
d'un thread CPU-bound (hang→abort), WAIT_ABANDONED distinct, `SuspendThread` courant, WASM (Asyncify).

**Test « vrai binaire » (honnête, 2026-07-16)** : `kernel32_test.exe` (conformance WineHQ, 3 Mo)
transpile mais **aborte sound avant le sous-test `thread`** — sur un `call [table]` indirect non
résolu (table `{nom, func}` de winetest, entrelacée en `.rdata`, non récupérée par le statique).
**Mur points-to orthogonal** (§1.4 / doc 70 P3), pas un bug threads. Les primitives sont prouvées
sur workloads composés (pool réaliste inclus) ; faire tourner un vrai binaire MT tiers bout-en-bout
demande d'abord la récup du dispatch indirect **et** les API haut-niveau (thread-pools/APC/affinity).

Chaque incrément : fixture minimale → oracle Wine → régression complète → commit + doc.

---

## 3. Conformité philosophie & règles — verdict global

**Oui, ces orientations entrent dans notre philosophie** — et plusieurs (lifting,
PGL) sont l'expression *la plus pure* de « réutilisation vérifiée / rien de deviné ».
Le glissement « traduire factuellement » **réduit** le devinement. La condition unique :
**chaque brique reste emballée dans « correct ou abort »**.

**Règles à graver (extensions, pas révisions, du 70 §0/§1/§2)** :
1. **Emballage obligatoire** : toute brique réutilisée/liftée/profilée reste « correct
   ou abort », vérifiée aux mêmes oracles. Non négociable.
2. **PGL = opt-in** : jamais sur les démonstrateurs 100%-statiques ; ne dilue pas la
   garantie statique.
3. **Parité WASM sound** : là où un mécanisme n'existe pas en WASM (`ucontext`, SEH
   matériel), le build WASM **aborte proprement**, jamais ne diverge en silence. Le
   split natif/WASM est **documenté**.
4. **Déterminisme préservé** : fibers coopératifs, replay PGL — protège l'oracle.
5. **Licence = décision externe consciente** (GPL/proprio/LGPL), pas un problème de
   soundness mais à trancher avant d'embarquer du code lifté redistribué.

**Priorité** : (1) **fibers** ✅ **COMPLET** (incr. 1-4) → (2) **lifting comctl32** ✅ **DÉMONTRÉ** (contrôle stateful réel bit-identique Wine, 2026-07-18) → (3) PGL
opt-in → (4) SEH in-HLE → (5) SoftFloat floatx80 [ère ARM/WASM].

**Orientation transverse enregistrée (2026-07-19, §1.6)** : **rétro-cible Windows moderne**. Bon marché = **backend PE**
(cas 1, vieux 32-bit → PE autonome : bundling + API retirées type WinHelp). Grand prix = **frontend 16-bit NE→Win16** (cas
2, seul moyen natif de faire tourner du 16-bit sur Windows 64-bit ; le volume du vieux logiciel est là), jalon dédié,
prérequis lifter partagé avec la Phase 8 (multi-arch/64-bit).
