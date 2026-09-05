#!/usr/bin/env bash
# Lanceur du serveur MCP ARET-MMU en HTTP LOCAL PERSISTANT.
#
# Pourquoi : en transport stdio, le harness relance un sous-processus serveur à CHAQUE
# (re)connexion et attend son handshake ; sous forte charge du conteneur (build cargo,
# sweeps winediff) ce spawn + import Python dépasse le délai de connexion MCP (30 s) →
# coupure. Un serveur HTTP DÉJÀ debout supprime ce coût : le harness fait juste un
# connect TCP + requête HTTP sur un process persistant qui détient déjà SQLite (l'état
# survit aux reconnexions). Mesuré : prêt à servir en ~1 s ; handshake MCP réel OK.
#
# Contrats :
#   - IDEMPOTENT : si le port répond déjà, ne démarre PAS un 2e serveur (deux
#     instances se disputeraient SQLite → "database is locked").
#   - Détaché (nohup + disown) : survit à la fin du hook qui le lance.
#   - Journalisation → fichier (jamais stdout, qui n'est pas le flux MCP ici).
#   - Retourne vite : attend (borné) que le port réponde puis rend la main.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MMU_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROJECT_DIR="${CLAUDE_PROJECT_DIR:-$(cd "${MMU_DIR}/.." && pwd)}"
VENV_DIR="${MMU_DIR}/.venv"
HOST="${ARET_MMU_HTTP_HOST:-127.0.0.1}"
PORT="${ARET_MMU_HTTP_PORT:-8765}"
MEMORY_DIR="${ARET_MMU_MEMORY_DIR:-${MMU_DIR}/.aret-memory}"
RUNTIME_DIR="${MEMORY_DIR}/runtime"
LOG="${RUNTIME_DIR}/mcp_http.log"
LOCK="${RUNTIME_DIR}/mcp_http.lock"
mkdir -p "${RUNTIME_DIR}"

url="http://${HOST}:${PORT}/mcp"
up() { curl -s -o /dev/null -m 1 "${url}" 2>/dev/null; }

# Déjà en écoute -> rien à faire (idempotent).
if up; then echo "ARET-MMU HTTP déjà en écoute sur ${HOST}:${PORT}" >&2; exit 0; fi

# Environnement Python (chemin chaud immédiat, cold-start uv ~5 s ; non fatal).
"${SCRIPT_DIR}/bootstrap_venv.sh" || true
ARET_PYTHON="${VENV_DIR}/bin/python"; [ -x "${ARET_PYTHON}" ] || ARET_PYTHON="python3"
export PYTHONPATH="${MMU_DIR}${PYTHONPATH:+:${PYTHONPATH}}"
export CLAUDE_PROJECT_DIR="${PROJECT_DIR}"
export ARET_WRITE_ENABLED="${ARET_WRITE_ENABLED:-true}"

# Sérialise le démarrage (deux hooks pourraient lancer deux serveurs concurremment).
exec 8>"${LOCK}" || true
if command -v flock >/dev/null 2>&1; then flock -w 20 8 || true; fi
if up; then exit 0; fi   # un autre lancement a pu réussir sous le verrou

# setsid : démarre le serveur dans une NOUVELLE SESSION (nouveau groupe de process),
# détaché du groupe du hook SessionStart. Sinon, quand le harness nettoie l'arbre de
# process du hook, le serveur backgroundé est tué avec lui (mesuré : il servait un
# POST 200 OK puis mourait). nohup couvre SIGHUP ; setsid couvre le kill de groupe.
setsid_cmd=""; command -v setsid >/dev/null 2>&1 && setsid_cmd="setsid"
${setsid_cmd} nohup "${ARET_PYTHON}" "${MMU_DIR}/aret_mmu_server.py" \
      --streamable-http --host "${HOST}" --port "${PORT}" \
      --memory-dir "${MEMORY_DIR}" --write-enabled \
      >>"${LOG}" 2>&1 &
disown || true

# Attend (borné) que le serveur écoute, pour que le harness le trouve prêt. Court par
# défaut (compatible budget hook 10 s) ; le serveur détaché continue de toute façon.
end=$((SECONDS + ${ARET_MMU_HTTP_WAIT:-8}))
until up || [ "${SECONDS}" -ge "${end}" ]; do sleep 0.3; done
if up; then echo "ARET-MMU HTTP prêt sur ${HOST}:${PORT}" >&2; else
  echo "ARET-MMU HTTP: le serveur n'a pas répondu à temps (voir ${LOG})" >&2; fi
