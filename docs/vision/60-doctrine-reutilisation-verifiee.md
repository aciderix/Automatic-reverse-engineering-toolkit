# Doctrine ARET — réutilisation vérifiée (rester soi-même sans passer des siècles binaire par binaire)

> ⚠️ **ABSORBÉ dans [`70-reference-etat-methode-reste.md`](70-reference-etat-methode-reste.md) §1** (2026-07-05).
> Le 70 est la référence unique. Ce document est conservé pour l'historique ; son
> contenu (doctrine, fait établi Wine-natif, arbitrage, limite dure) vit désormais
> dans le 70.
>
> **Boussole stratégique.** Répond à une question de fond : peut-on couvrir
> *n'importe quel* programme sans travail manuel sans fin, tout en gardant la
> philosophie ARET (safety, rigueur) ? **Oui — à condition de comprendre où vit
> réellement l'identité d'ARET.**

## 1. L'identité d'ARET n'est PAS « tout écrire à la main »

« Fait maison from-scratch » est un **choix de mise en œuvre**, pas une valeur.
Les vraies valeurs, non négociables :

1. **Sound** — jamais un résultat faux présenté comme correct. Juste, ou **arrêt
   bruyant**. Jamais de silence trompeur.
2. **Vérifié** — on **prouve** que la sortie = l'original (oracles différentiels :
   Unicorn pour le CPU, Wine pour l'OS, natif pour le bout-en-bout, Z3 pour
   l'équivalence).
3. **Natif** — la sortie (ELF/WASM) tourne **directement**, sans émulateur CPU.
4. **Général** — jamais de rustine par binaire ; chaque correctif profite à une
   classe entière.

**Conséquence clé : ces valeurs vivent dans la COUCHE DE VÉRIFICATION et la
DISCIPLINE DE FRONTIÈRE, pas dans l'origine du code.** Elles sont donc
**transférables** sur des briques réutilisées.

## 2. La règle d'or de la réutilisation

> Une brique réutilisée n'est **jamais** une boîte noire de confiance. C'est un
> **composant qu'ARET vérifie et emballe dans son contrat « correct ou abort »**,
> avec les mêmes oracles que du code maison.

Réutiliser **sert** même la valeur #4 (général) : couvrir tout un axe d'un coup,
c'est l'anti-« siècle binaire par binaire ».

## 3. Stratégie par axe

| Axe | Brique réutilisable | Oracle de vérification | Philosophie préservée ? |
|-----|---------------------|------------------------|--------------------------|
| **Instructions CPU** | spec de sémantique (SLEIGH/XED) → **notre** backend natif | **Unicorn** (indépendant de la source) | ✅ **Totale.** Couvre l'ISA finie d'un coup ; oracle indépendant. |
| **Comptage x87** (profondeur de pile) | **filet runtime** (pile FPU modélisée à l'exécution, correcte par construction) | différentiel bout-en-bout | ✅ **Totale.** Natif (juste code moins joli), jamais faux. |
| **APIs Windows** | **Winelib** (implés natives de Wine) *ou* shims auto-générés depuis win32metadata | **Wine** (winediff) | ⚠️ **Presque** — voir §5. |
| **Récupération de fonctions** | analyse type Ghidra/angr | différentiel de sortie | ✅ si vérifié en sortie. |

## 4. Fait établi (vérifié, pas asserté)

**Wine n'est PAS un émulateur.** Mesuré dans cet environnement (2026-07-03) :
- `file` sur l'implémentation kernel/ntdll de Wine → **`ELF 32-bit LSB shared
  object`** = code Linux **natif**.
- `strace` d'un PE sous Wine → charge ces `.so` natifs, **aucun émulateur CPU**
  (pas de qemu/box86) ; le code i386 s'exécute **directement sur le processeur**.
- Un `GetFileAttributesA` renvoie le bon résultat via l'implé native de Wine.

Donc « réutiliser Wine pour les APIs » = **lier du natif**, cohérent avec la
valeur #3.

**Preuve de concept end-to-end (2026-07-03, vérifiée)** : un programme C appelant
`GetVersionExA`/`AreFileApisANSI`/`GetFileAttributesA`/`GetLastError`, compilé par
**`winegcc -m32`**, produit un **`ELF 32-bit LSB shared object` natif** qui tourne
et sort `ver=6.2 ansi=1 attr_ok=1 missing=1 lasterr=2` — **zéro émulateur CPU**.
Notable : `lasterr=2` (`ERROR_FILE_NOT_FOUND`) = **exactement** la valeur qu'on
avait codée à la main dans le fix `SetLastError` d'ARET → l'implé native de Wine
**confirme** le comportement de notre shim. La couverture API « d'un coup, native »
est donc **démontrée**, pas supposée.

*Intégration* : brancher Winelib dans le pipeline ARET (router les imports de la
sortie transpilée vers Winelib au lieu de `aret_hle`) est une **étape une fois**,
pas par binaire. Le toolchain (`wine32-tools` + `libwine-dev:i386`) demande un env
propre ; dans ce conteneur il a fallu extraire les `.deb` à la main (deps cassées),
mais le mécanisme est prouvé.

## 5. Le seul vrai arbitrage : l'indépendance de la preuve (axe APIs)

Aujourd'hui l'oracle des APIs **est** Wine (winediff). Si on utilise **aussi**
Wine comme implémentation (Winelib), on « vérifie Wine contre Wine » →
contrôle **circulaire**, moins indépendant. Deux positions honnêtes :
- **Assumer Wine comme vérité terrain pratique** : ses trous sont **connus et
  testés par le monde entier**, jamais des faux silencieux.
- **Voie médiane** : auto-générer la **tuyauterie** des shims (ABI, `@N` stdcall)
  depuis win32metadata, mais garder le **comportement vérifié** contre un oracle
  indépendant. Moins « tout d'un coup », indépendance totale.

## 6. La limite dure (honnêteté, pas paresse)

Le trio **« tout binaire + 100 % fonctionnel + 100 % natif pur »** est
**prouvé impossible** en toute généralité (indécidabilité, famille du problème de
l'arrêt). Ce que la réutilisation vérifiée atteint réellement :

- **Vrai logiciel compilé** (sqlite, Lua, jeux…) → **pleinement fonctionnel**.
- Garantie exacte : **« fonctionnel, OU arrêt qui dit où — jamais faux en
  silence »**. Le résidu théoriquement impossible (obfusqué/fait main) **se
  signale**, il ne ment pas.

## 7. Finalité (inchangée)

**Binaire Windows → binaire Linux natif / WASM, exécutant la vraie logique,
directement, avec la même soundness et la même vérification.** La réutilisation
ne change **ni le but ni les garanties** — seulement la **vitesse** pour y
arriver, en s'appuyant sur des briques matures **et vérifiées**.

> **En une phrase.** ARET = une **couche de vérification + soundness** au-dessus
> de briques (maison ou réutilisées) qu'il **prouve** correctes ou **rejette
> bruyamment**. Rester soi-même, sans réécrire l'univers.
