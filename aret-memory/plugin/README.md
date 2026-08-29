# Plugin Claude Code — ARET-MMU

Empaquette l'activation d'ARET-MMU (serveur MCP `aret-memory` + barrière de reprise +
synchronisation Git) en **un plugin Claude Code**. Enabler le plugin remplace les étapes
manuelles 2-3 d'`aret-memory/integration/INSTALL.md` (copier les hooks, fusionner
`.mcp.json` et `settings.json`).

## Ce que le plugin apporte

- Déclare le **serveur MCP** `aret-memory` (via `${CLAUDE_PROJECT_DIR}/aret-memory/scripts/launch_aret_mcp.sh`).
- Branche les **6 hooks** (SessionStart, PreCompact, PostCompact, PreToolUse, PostToolUse,
  Stop) sur les scripts canoniques in-repo — pas de copie, ils se sourcent via `dirname $0`.
- Fournit le **skill `aret-mmu`** (discipline FIND→READ, Front, PROVEN).

## Prérequis / hypothèse

Comme INSTALL.md, le plugin **suppose le paquet Python vendored** sous
`<racine-du-projet>/aret-memory/`. Le serveur et la mémoire SQLite y vivent : le plugin
**câble** l'activation, il ne porte pas le code Python ni la base.

## Caveats honnêtes (à lire avant d'enabler)

1. **La mémoire est spécifique au dépôt.** La valeur d'ARET-MMU est la base SQLite
   accumulée (`aret-memory/.aret-memory/`), versionnée dans CE dépôt. Un plugin déployé
   ailleurs démarre avec une **base vide** : utile pour la machinerie, pas pour transporter
   la mémoire.
2. **Ne pas double-brancher.** Ce dépôt câble déjà les hooks via `.claude/settings.json`.
   Enabler le plugin ICI ferait **tirer les hooks deux fois**. Le plugin est destiné à un
   **autre** projet qui vendore `aret-memory/` sans câblage manuel — pas à ce dépôt.
3. **Réseau au premier démarrage.** `launch_aret_mcp.sh` fait un `pip install -e` au
   premier lancement (env cloud : Network access = `Trusted`). Les hooks, eux, n'ont pas
   besoin du réseau.
4. **Écriture.** `ARET_WRITE_ENABLED=true` par défaut (pour un usage fonctionnel) ;
   passer à `false` pour un montage lecture seule.

## Enabler

Depuis un projet qui vendore `aret-memory/` : ajouter ce dossier comme plugin local
(`/plugin` dans Claude Code, ou pointer un marketplace vers ce chemin), puis approuver le
serveur `aret-memory` (`/mcp`). À chaque démarrage/compaction, les hooks injectent le
Resume Dossier et arment la barrière ; produire le récapitulatif des six volets puis
appeler `aret_acknowledge_resume`.

## Construire le zip distribuable

```bash
aret-memory/plugin/pack.sh
```
Rafraîchit la copie du skill depuis la source canonique (`.claude/skills/aret-mmu/SKILL.md`)
puis produit `aret-memory/plugin/dist/aret-mmu-plugin.zip` (racine = ce dossier, contenant
`.claude-plugin/plugin.json`). C'est le zip qui satisfait la validation Claude Code
(« must contain a .claude-plugin/plugin.json file »).
