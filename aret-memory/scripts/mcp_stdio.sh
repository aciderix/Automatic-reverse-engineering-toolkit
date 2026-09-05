#!/usr/bin/env bash
# Commande stdio de .mcp.json pour le serveur MCP ARET-MMU — AUTO-RÉPARANTE.
#
# Pourquoi : en transport stdio, c'est le harness qui exécute cette commande au
# démarrage de session et attend le handshake MCP (délai ~30 s). Si .mcp.json
# pointait directement vers `.venv/bin/python`, un conteneur FROID (où `.venv`
# n'existe pas encore, il est gitignoré) verrait le spawn échouer -> serveur jamais
# connecté -> barrière de reprise sans issue. C'est la même course perdue qu'en HTTP,
# déplacée sur `.venv`.
#
# Ce wrapper la SUPPRIME : il garantit que `mcp` est importable (bootstrap si besoin)
# PUIS exec le serveur. Le bootstrap se fait donc À L'INTÉRIEUR de la fenêtre d'attente
# du harness, séquentiellement — plus de course. Chemin chaud (venv déjà prêt) = exec
# immédiat.
#
# Contrats :
#   - stdout est RÉSERVÉ au flux MCP stdio : toute journalisation va sur stderr, et les
#     sondes `import mcp` écrivent dans /dev/null.
#   - exec final (pas de sous-shell) : le serveur hérite proprement des fd stdio.
#   - idempotent, sans effet de bord ; bootstrap sérialisé par flock (dans bootstrap_venv.sh).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MMU_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROJECT_DIR="${CLAUDE_PROJECT_DIR:-$(cd "${MMU_DIR}/.." && pwd)}"
VENV_PY="${MMU_DIR}/.venv/bin/python"
SERVER="${MMU_DIR}/aret_mmu_server.py"
MEMORY_DIR="${ARET_MMU_MEMORY_DIR:-${MMU_DIR}/.aret-memory}"

export PYTHONPATH="${MMU_DIR}${PYTHONPATH:+:${PYTHONPATH}}"
export CLAUDE_PROJECT_DIR="${PROJECT_DIR}"
export ARET_WRITE_ENABLED="${ARET_WRITE_ENABLED:-true}"

usable() { [ -x "$1" ] && "$1" -c 'import mcp' >/dev/null 2>&1; }

# --- Chemin CHAUD : venv déjà utilisable -> exec direct (instantané). --------
if usable "${VENV_PY}"; then
  exec "${VENV_PY}" "${SERVER}" --memory-dir "${MEMORY_DIR}" --write-enabled
fi

# --- Chemin FROID : bootstrap borné (uv ~1 s / pip ~17 s), logs sur stderr. --
echo "ARET-MMU mcp_stdio: venv non prêt, bootstrap..." >&2
"${SCRIPT_DIR}/bootstrap_venv.sh" 1>&2 || true
if usable "${VENV_PY}"; then
  exec "${VENV_PY}" "${SERVER}" --memory-dir "${MEMORY_DIR}" --write-enabled
fi

# --- Dernier recours : python système (si l'environnement fournit `mcp`). -----
echo "ARET-MMU mcp_stdio: repli sur python3 système" >&2
exec python3 "${SERVER}" --memory-dir "${MEMORY_DIR}" --write-enabled
