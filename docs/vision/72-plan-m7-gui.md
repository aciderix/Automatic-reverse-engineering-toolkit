# 72 — Plan M7 : GUI (USER32 / GDI / ressources / dialogs), portable et sound

> Plan de chantier dédié pour la **milestone M7** (roadmap doc 70 §6). Déclenché
> **par la donnée** : le dégrossissement corpus (41 exe Win95, doc 71) a montré
> qu'après les murs lift/shim, **le seul gros bloc restant = la GUI** (top imports
> ~100 % USER32-A / GDI / ressources / dialogs). Ce doc fixe l'architecture,
> l'ordre des incréments, et — le plus dur — la **stratégie d'oracle**.

## 0. Principe & garanties (inchangés)

- **Correct ou abort bruyant, jamais faux en silence.** Un message non modélisé,
  une API GDI non gérée → `aret_unimpl`/abort, pas un no-op qui ment.
- **Portable d'abord.** La couche fenêtre/dessin s'adosse à **SDL2** (Linux, macOS,
  **et WASM via Emscripten**) — **pas** X11 (qui clouerait au sol Linux et tuerait
  le WASM). Une fenêtre *message-only* (déjà faite) reste **sans dépendance
  graphique**. SDL2 n'est tiré **que** quand une fenêtre *visible* est créée.
- **Mono-thread** (comme tout le runtime aujourd'hui). Le multithread GUI est un
  sur-chantier ultérieur.
- **Oracle = Wine**, sur le **comportement** (textes, séquences de messages,
  valeurs de contrôles, résultats de ressources), **pas les pixels** au début.
  SDL headless (`SDL_VIDEODRIVER=dummy`) → tests sans écran réel.

## 1. Ce qui existe déjà (socle)

- **Sous-système fenêtre message-only (W)** — `aret_win32.c`, doc 71 : registre de
  classes, table de fenêtres, file de messages mono-thread, timers, **dispatch
  WNDPROC dans le lifté via `aret_call`** (frame stdcall sous esp, réentrant). 15
  fns W (`RegisterClassW`/`CreateWindowExW`/`Get/Peek/Dispatch/Post/SendMessageW`/…).
- **Section `.rsrc` du PE déjà chargée** en mémoire (Memory Layout Mapper) → les
  ressources sont **présentes**, il « suffit » de les indexer.
- **`aret_call`** : mécanisme de callback vers le code lifté (validé).

## 2. Architecture cible (couches)

```
  Programme lifté
      │  RegisterClassA / CreateWindowExA(WS_VISIBLE) / DialogBoxParamA / GDI…
      ▼
  ┌─────────────────────────────────────────────────────────────┐
  │  Couche USER32/GDI ARET (aret_gui.c, nouveau)                │
  │  • registre de classes A+W  • table de fenêtres              │
  │  • file de messages + pompe  • timers                        │
  │  • dispatch WNDPROC (aret_call)                              │
  │  • fenêtre VISIBLE ─────────────► SDL_Window/Renderer        │
  │  • GDI (HDC) ──► framebuffer mémoire ──► SDL_Texture/blit    │
  │  • ressources (.rsrc) : Find/Load/SizeofResource, LoadString │
  │  • dialogs : parse template ressource → contrôles            │
  └─────────────────────────────────────────────────────────────┘
      │ (headless en test : SDL_VIDEODRIVER=dummy)
      ▼  SDL2 (Linux/macOS/WASM) — la seule dépendance plateforme
```

**Gating portabilité** : la couche est compilée à part (`aret_gui.c`) ; le lien
SDL2 n'est requis que si le binaire crée une fenêtre visible. Les cibles CLI et
message-only **ne dépendent pas** de SDL2 (ni du WASM-wasi actuel). *(Le WASM GUI
via Emscripten est une **2ᵉ cible** distincte de notre wasm32-wasi — à cadrer au
moment de M7-WASM, non bloquant pour Linux/macOS.)*

## 3. Incréments ordonnés (chacun = un artefact + un oracle)

| # | Incrément | Débloque (corpus) | Oracle |
|---|-----------|-------------------|--------|
| **G1** | **Jumeaux A** du modèle fenêtre/classe/message (RegisterClassA, CreateWindowExA, DefWindowProcA, Get/Peek/Dispatch/Translate/Post/SendMessageA, GetMessageA) — ANSI, **toujours message-only** | RegisterClassA 26, SendMessageA 32, DefWindowProcA 24, DispatchMessageA 24, PeekMessageA 22, CreateWindowExA 20… | fixture message-only ANSI vs Wine (comme `user32_msgwindow.c` en A) |
| **G2a** ✅ | **Modèle fenêtre étendu (display-free)** : GetWindowRect/SetWindowPos/MoveWindow/ShowWindow/UpdateWindow/EnableWindow/Get-SetWindowLong ; Get-SetWindowText via WM_SETTEXT/GETTEXT ; GetSystemMetrics/GetDesktopWindow/IsWindow(Visible/Enabled)/GetParent | GetWindowRect 35, SetWindowPos 34, ShowWindow 21, GetSystemMetrics 17, GetDesktopWindow 21 | `user32_windowstate_a.c` : géométrie/état/texte round-trip, métriques par invariant — headless vs Wine ✅ |
| **G2b** ✅ | **Fenêtre visible via SDL2** : CreateWindowEx(WS_VISIBLE)→SDL_CreateWindow+Renderer+Texture ; **framebuffer client** (DIB 32bpp) lié par GetDC(hwnd)/BeginPaint (le GDI y dessine), présenté sur UpdateWindow/EndPaint/ReleaseDC ; pompe SDL_PollEvent → WM_CLOSE/WM_MOUSE*/WM_KEY* ; **modèle de peinture `WM_PAINT`/invalidation** (InvalidateRect/ValidateRect, `needs_paint` par fenêtre, WM_PAINT généré à la demande par Get/PeekMessage + livré synchrone par UpdateWindow, BeginPaint valide, DefWindowProc(WM_PAINT/WM_CLOSE)) → **le handler WM_PAINT du programme dessine = le contenu s'affiche** ; `-DARET_HAVE_SDL` via pkg-config i386, gaté sur import CreateWindowEx*, **dégradation display-free propre** si absent (compile/link byte-identiques). **Additif** : seul l'input réel devient message → séquence déterministe inchangée ; l'entrée (souris/clavier) est env-dépendante ⇒ hors oracle bit-exact | visible windows | `user32_sdlwindow.c` (round-trip pixel) + `user32_paint.c` (handler WM_PAINT dessine, comptes+pixel), **bit-identiques Wine** headless ✅ ; **+ toutes les fixtures GUI relient SDL sans perturbation** ✅ |
| **G3** | **Texte fenêtre + widgets simples** : SetWindowTextA/GetWindowTextA, WM_SETTEXT ; contrôles STATIC/BUTTON/EDIT (classes prédéfinies) ; GetDlgItem/SetDlgItemText/GetDlgItemText | SetWindowTextA 34, GetDlgItem 33, SetDlgItemTextA 19, EnableWindow 19 | round-trip texte + état contrôles vs Wine |
| **G4** ✅ (partiel) | **Ressources** : FindResourceA/LoadResource/SizeofResource/LockResource/FreeResource (walker `IMAGE_RESOURCE_DIRECTORY` en mémoire ; en-têtes PE déjà mappés → 0 changement loader), **LoadStringA**. *Reste : LoadIconA/LoadCursorA (handles opaques), W-variants.* | LoadStringA 32, FindResourceA/LoadResource/SizeofResource 26, LockResource 24, FreeResource 25 | `user32_resources.{c,rc}` : blob RCDATA + table de chaînes multi-blocs + troncature vs Wine (octets/valeurs exacts) ✅ |
| **G5** ✅ | **Dialogs + MessageBox** : **MessageBoxA/W = -1 display-free** (repli sound = Wine-sans-écran) ; **DialogBoxParamA/W + CreateDialogParamA/W** parsent le **DLGTEMPLATE(EX)** → contrôles enfants + **pompe modale** (WM_INITDIALOG via aret_call) ; **EndDialog**, **GetDlgItem/GetDlgCtrlID**, **Set-GetDlgItemText A/W**, **Set-GetDlgItemInt**, **SendDlgItemMessageA/W** | MessageBoxA 37, GetDlgItem 33, CreateDialogParamA 23, EndDialog 20, SetDlgItemTextA 19, DialogBoxParamA 17, SendDlgItemMessageA 16 | `user32_messagebox` (`.nodisplay`) ✅ + `user32_dialog.{c,rc}` (DLGTEMPLATE + WM_INITDIALOG + EndDialog, texte/int round-trip) vs Wine sous Xvfb ✅ |
| **G6** ✅ (partiel) | **GDI de base** : DC/objet (GetDC/ReleaseDC/CreateCompatibleDC/BeginPaint/EndPaint/SelectObject/DeleteObject/GetStockObject/GetSysColor/GetDeviceCaps) + **dessin DIB bit-exact** (CreateDIBSection 32bpp, SetPixel/GetPixel/FillRect/PatBlt/BitBlt SRCCOPY, CreateSolidBrush/Pen). *Reste : TextOut/Rectangle/LineTo (raster ≠ Wine → abort sound), <32bpp, blit→SDL.* | ReleaseDC 30, GetDeviceCaps 28, GetDC 27, GetStockObject 22, SelectObject 14, BeginPaint/EndPaint 11 | `gdi_dib.c` : dessin dans un DIB mémoire → **hash du buffer** (`a182d45a`) + GetPixel + caps invariant vs Wine ✅ |
| **G7** | **Élargir au réel** : re-sweep corpus, attaquer les messages/API restants par la donnée. Boucle mesure→fix→re-mesure jusqu'à faire **tourner une vraie appli Win95** bout-en-bout. **Source corpus GUI** : `https://archive.org/download/BestOfWindows95DotCom` (4 ISO Win95 shareware, reachable via proxy — cf. doc 70 §7) → extraire `.exe` → `--mode walls`/`wallsweep.sh`. | le reste | une appli du corpus tourne vs Wine (contenu) |

**Tier EH (parallèle, quand une appli GUI en a besoin)** : `RtlUnwind`/SEH (froid
aujourd'hui, doc 71) + C++ exceptions. À traiter ici, **pas** avant — control-flow
non-local + chaîne `fs:[0]` + handlers, correctness-critique.

## 4. Stratégie d'oracle (le point dur de la GUI)

1. **Contenu, pas pixels** (G1–G5) : on compare textes, séquences de messages,
   valeurs de contrôles, résultats `LoadString`/ressources — **déterministes** et
   identiques Wine↔ARET. Les fixtures **impriment** ces faits sur stdout →
   `winediff` bit-à-bit inchangé.
2. **Headless** : `SDL_VIDEODRIVER=dummy` (et `WINEDEBUG=-all`, déjà en place) →
   pas d'écran, tests en CI. Une vraie fenêtre n'est **pas** requise pour vérifier
   le modèle fenêtre/message/ressource/dialogue.
3. **Framebuffer pour GDI** (G6) : on dessine dans un buffer mémoire et on compare
   son **hash** à celui de Wine (même DC offscreen) — le rendu, sans l'écran.
4. **Auto-réponse** aux API bloquantes (MessageBox, DialogBox) : une variable
   d'env de test fait renvoyer le bouton par défaut, pour un flot déterministe.
5. **Invariant** quand la valeur dépend de l'environnement (métriques écran) :
   tester des relations stables, pas des valeurs brutes (cf. `GetDiskFreeSpaceA`).

## 5. Risques / questions ouvertes

- **SDL2 dans l'environnement de build** : ✅ **RÉSOLU** (2026-07-10). `libsdl2-dev:i386`
  **2.30.0** installé (le proxy autorise les dépôts Ubuntu ; archi i386 activée) ; un
  binaire **`-m32`** lie SDL2 et tourne **headless** (`SDL_VIDEODRIVER=dummy`,
  `driver=dummy`). Conteneur **éphémère** → réinstallé au besoin par le hook
  `.claude/hooks/session-start.sh` (idempotent, comme z3). La couche GUI détectera
  SDL2 via `pkg-config` (chemin i386) et **dégradera proprement** si absent (CLI /
  message-only / headless-contenu n'en dépendent pas).
- **Impédance Win32↔SDL** : la sémantique fenêtre/message (HWND, WNDPROC, WM_*,
  boucle modale) reste **du code maison** au-dessus des primitives SDL — SDL ne
  donne « que » la fenêtre OS + les entrées (la vraie partie plateforme).
- **WASM GUI** = Emscripten (2ᵉ toolchain, ≠ wasm32-wasi actuel) — à cadrer à
  M7-WASM, non bloquant pour Linux/macOS.
- **GDI est vaste** : on s'arrête au sous-ensemble **mesuré** (TextOut/Rectangle/
  DC/stock objects) ; le reste = abort sound jusqu'à ce qu'un binaire l'exige.

## 6. Ordre d'exécution

**G1 fait** (jumeaux A). **G2a fait** (modèle fenêtre étendu **display-free** :
géométrie/état/texte — GetWindowRect/SetWindowPos/MoveWindow/ShowWindow/Enable/
Get-SetWindowLong/Get-SetWindowText via WM_*TEXT*/GetSystemMetrics/desktop, oracle
`user32_windowstate_a.c` bit-identique Wine, 58/58). G2a scinde G2 en deux : le
**modèle** (fait, portable, sans SDL) puis **G2b** = la vraie marche archi (fenêtre
SDL visible : `CreateWindowEx(WS_VISIBLE)`→`SDL_CreateWindow`, pompe messages ↔
`SDL_PollEvent`, lien SDL2 via pkg-config `-DARET_HAVE_SDL`, dégradation propre si
absent). **G4 fait (partiel)** — **priorisé par la donnée AVANT G2b** : le re-sweep
post-G2a montre que les plus gros murs sont **display-free** (ressources/LoadString/
MessageBox), oracle **exact**, alors que G2b (fenêtre SDL) a un oracle dur (tempête
de messages `CreateWindow` non bit-reproductible). Ressources `.rsrc` indexées
(walker mémoire, en-têtes PE déjà mappés) + LoadStringA, `user32_resources` 59/59.
Le reste par la donnée (re-sweep après chaque incrément). Ordre suivi : **G4
(ressources/LoadString) ✅ → G5 (dialogs/MessageBox, display-free) ✅ → G6 (GDI DIB) ✅
→ G2b (fenêtre SDL visible) ✅** — les tranches display-free (oracle exact) d'abord,
puis **G2b livré** (2026-07-11) : « un transpilé doit afficher à l'écran pour être
utilisable ». Fenêtre visible réelle + framebuffer client + présentation + pompe
SDL_PollEvent, **strictement additif** (les oracles déterministes restent
bit-identiques, les fixtures GUI relient SDL sans perturbation). Reste : widgets
natifs (BUTTON/EDIT), GDI raster (TextOut/DrawText), G7 (re-sweep appli réelle),
WASM-GUI (Emscripten). Chaque incrément : fixture minimale →
implément → **oracle Wine** → régression complète → commit + doc (70 §4.5 / §5, 71
daté). Aucune régression tolérée.
