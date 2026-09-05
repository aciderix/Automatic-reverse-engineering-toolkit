# MCP ARET-MMU — démarrage, robustesse, récupération

## Comment le harness se connecte
Le harness Claude Code lit le `.mcp.json` du dépôt et **spawn lui-même** le serveur en
**transport stdio** (`mcp_stdio.sh`), puis attend le handshake MCP (~30 s). Les serveurs
distants (github, Supabase…) sont des endpoints HTTP déjà debout ; aret-memory, lui, est
**local** et doit être spawné — c'est la seule différence, et la source de toute la
fragilité au démarrage.

## Pourquoi stdio et pas HTTP
Sous stdio, le marqueur de vivacité `runtime/mcp_ready` n'est écrit que par le serveur
**spawné par le harness** : `mcp_ready` frais ⟺ harness réellement connecté. La barrière
de reprise s'appuie là-dessus pour ne **jamais** hard-bloquer quand le canal est mort.
Un serveur **HTTP** persistant casserait cette équivalence (serveur up mais harness
déconnecté = fausse vivacité) et re-créerait le deadlock vécu. → stdio est le bon choix.

## Les scripts
- **`mcp_stdio.sh`** — commande de `.mcp.json`. Auto-réparante : chemin chaud (venv +
  paquet `mcp` présents, vérif **fichier** instantanée) = exec direct ; chemin froid =
  `bootstrap_venv.sh` (uv ~5 s) puis exec ; sinon **abort bruyant** (jamais de repli muet).
- **`bootstrap_venv.sh`** — construit le venv (idempotent, `flock`, uv puis repli pip),
  pré-compile les `.pyc` après une vraie (re)construction.
- **`mcp_doctor.sh`** — diagnostic + récupération manuelle (voir ci-dessous).

## Si le MCP est down
1. **Ce n'est pas bloquant.** Serveur down → `mcp_ready` périmé → la barrière passe en
   **mode soft** (contexte de reprise injecté, mais aucun refus d'outil). On peut
   travailler ; le harness **retente** la connexion tout seul.
2. **Diagnostiquer** : `bash aret-memory/scripts/mcp_doctor.sh status`.
3. **Réparer le venv** : `bash aret-memory/scripts/mcp_doctor.sh warm`.
4. **Forcer la levée de barrière** (si jamais coincé) :
   `bash aret-memory/scripts/mcp_doctor.sh barrier-off` (puis `barrier-on` pour ré-armer).
5. **Reconnexion** : au besoin, redémarrer/désarchiver la session — le clone contient déjà
   la bonne config, le wrapper garantit le cold-start.

## Cause connue de coupure au démarrage
Au boot du conteneur (clone + provisioning + `session_start` simultanés = tempête CPU),
le spawn + `import mcp` peut dépasser les 30 s du harness → CONNECT_TIMEOUT. Atténuations
en place : vérif chaude par fichier (plus de double `import mcp`), `.pyc` pré-compilés.
Résiduel : sous starvation extrême, le premier connect peut encore échouer — mais c'est
**non bloquant** et **auto-réparé** par le retry du harness. Règle : ne pas lancer de build
lourd pendant une (re)connexion MCP.
