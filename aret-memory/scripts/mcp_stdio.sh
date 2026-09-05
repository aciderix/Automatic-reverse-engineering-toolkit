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

# venv_ready : verif RAPIDE par FICHIER (aucun interpréteur Python lancé). Le chemin
# chaud est le cas courant et le plus sensible au timeout de boot : lancer un
# `python -c 'import mcp'` juste pour VERIFIER coûte ~1 s, puis le serveur ré-importe
# `mcp` (~1 s) = DOUBLE import sous la tempête de boot. On teste donc l'existence du
# paquet sur disque (instantané) ; le vrai import, c'est le serveur qui le fait. Si
# `mcp` etait present mais casse, le serveur echoue bruyamment et le harness retente.
venv_ready() {
  [ -x "${VENV_PY}" ] || return 1
  local d
  for d in "${MMU_DIR}"/.venv/lib/python*/site-packages/mcp; do
    [ -d "$d" ] && return 0
  done
  return 1
}
# importable : verif COMPLETE (vrai import), reservee a la DECISION du chemin froid.
importable() { [ -x "${VENV_PY}" ] && "${VENV_PY}" -c 'import mcp' >/dev/null 2>&1; }

# --- Chemin CHAUD : venv présent -> exec direct (aucun spawn Python de contrôle). --
# "$@" : forward d'eventuels arguments passes par .mcp.json (aucun aujourd'hui,
# mais on ne les perd pas si un jour la commande en recoit).
if venv_ready; then
  exec "${VENV_PY}" "${SERVER}" --memory-dir "${MEMORY_DIR}" --write-enabled "$@"
fi

# --- Chemin FROID : bootstrap borné (uv ~5 s / pip ~17 s), logs sur stderr. --
# On NE masque PAS l'echec du bootstrap (pas de `|| true`) : le `&&` en fait une
# condition, donc `set -e` ne tue pas le script, mais un echec ne fait PAS croire a
# une reussite. Le repli `python3` systeme a ete RETIRE : mesure faite, le python
# systeme n'a pas `mcp` -> il ne ferait que demarrer un serveur qui plante aussitot
# a l'import, masquant la vraie cause (anti-§0). En cas d'echec : abort BRUYANT.
echo "ARET-MMU mcp_stdio: venv non prêt, bootstrap..." >&2
if "${SCRIPT_DIR}/bootstrap_venv.sh" 1>&2 && importable; then
  exec "${VENV_PY}" "${SERVER}" --memory-dir "${MEMORY_DIR}" --write-enabled "$@"
fi

echo "ARET-MMU mcp_stdio: ECHEC — module 'mcp' indisponible apres bootstrap (venv non" \
     "construit ; reseau/proxy PyPI injoignable ?). Abandon, PAS de repli silencieux." >&2
exit 1
