#!/usr/bin/env bash
# mcp_doctor.sh — diagnostic et RÉCUPÉRATION MANUELLE du MCP ARET-MMU.
#
# Le transport est stdio : c'est le harness qui spawn le serveur ; on ne peut donc pas
# "démarrer le serveur" à sa place. Ce que cet outil garantit, c'est qu'on n'est JAMAIS
# coincé : il SIGNALE l'état précis et offre des réparations sûres. Rappel de conception :
# quand le serveur est down, la barrière de reprise est NON-BLOQUANTE (marqueur mcp_ready
# périmé -> mode soft), donc cet outil reste appelable même "MCP down".
#
# Usage :
#   mcp_doctor.sh [status]     # défaut : rapport de santé complet + verdict
#   mcp_doctor.sh warm         # (re)construit/répare le venv puis pré-compile les .pyc
#   mcp_doctor.sh barrier-off  # LÈVE la barrière de reprise (crée runtime/BARRIER_OFF)
#   mcp_doctor.sh barrier-on   # ré-arme la barrière (retire runtime/BARRIER_OFF)
#   mcp_doctor.sh clean-locks  # retire un verrou de bootstrap resté en place
#
# Tout va sur stdout ici (ce n'est PAS le flux MCP : script opérateur, pas la commande
# .mcp.json).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MMU_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
VENV_PY="${MMU_DIR}/.venv/bin/python"
SERVER="${MMU_DIR}/aret_mmu_server.py"
MEMORY_DIR="${ARET_MMU_MEMORY_DIR:-${MMU_DIR}/.aret-memory}"
RUNTIME_DIR="${MEMORY_DIR}/runtime"
READY="${RUNTIME_DIR}/mcp_ready"
BARRIER_OFF="${RUNTIME_DIR}/BARRIER_OFF"
LIVENESS_MAX_AGE=90   # doit rester aligné sur resume_guard.MCP_LIVENESS_MAX_AGE_S

ok()   { printf '  \033[32m✓\033[0m %s\n' "$*"; }
warn() { printf '  \033[33m!\033[0m %s\n' "$*"; }
bad()  { printf '  \033[31m✗\033[0m %s\n' "$*"; }

venv_pkg_present() { for d in "${MMU_DIR}"/.venv/lib/python*/site-packages/mcp; do [ -d "$d" ] && return 0; done; return 1; }

cmd_status() {
  echo "ARET-MMU MCP — diagnostic ($(date '+%H:%M:%S'))"
  echo "== Environnement Python =="
  if [ -x "${VENV_PY}" ]; then ok "venv présent : ${VENV_PY}"; else bad "venv ABSENT -> lancer: mcp_doctor.sh warm"; fi
  if venv_pkg_present; then ok "paquet 'mcp' présent sur disque"; else bad "paquet 'mcp' ABSENT -> lancer: mcp_doctor.sh warm"; fi

  echo "== Serveur (handshake stdio réel, borné 20 s) =="
  local t0 t1 dur out
  t0=$(date +%s.%N)
  out=$(printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"doctor","version":"0"}}}' \
        | timeout 20 "${SCRIPT_DIR}/mcp_stdio.sh" 2>/dev/null | head -c 40)
  t1=$(date +%s.%N); dur=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')
  if printf '%s' "$out" | grep -q '"result"'; then
    ok "handshake OK en ${dur}s"
    awk -v d="$dur" 'BEGIN{ if (d+0 > 20) exit 1 }' || warn "handshake LENT (>20s) : sous forte charge, le harness (timeout 30s) peut échouer -> éviter les builds pendant une reconnexion"
  else
    bad "handshake ÉCHOUÉ en ${dur}s (voir: ${SCRIPT_DIR}/mcp_stdio.sh)"
  fi

  echo "== Barrière de reprise (marqueur de vivacité) =="
  if [ -f "${BARRIER_OFF}" ]; then warn "BARRIER_OFF présent : barrière LEVÉE (coupe-circuit actif) -> mcp_doctor.sh barrier-on pour ré-armer"; fi
  if [ -f "${READY}" ]; then
    local age; age=$(awk -v m="$(stat -c %Y "${READY}" 2>/dev/null || echo 0)" 'BEGIN{srand(); print systime()-m}')
    if [ "${age}" -le "${LIVENESS_MAX_AGE}" ]; then
      ok "mcp_ready frais (${age}s) : canal jugé VIVANT -> barrière en mode dur (rituel requis) — normal si connecté"
    else
      ok "mcp_ready périmé (${age}s > ${LIVENESS_MAX_AGE}s) : canal jugé MORT -> barrière NON-BLOQUANTE (aucun deadlock possible)"
    fi
  else
    ok "pas de mcp_ready : aucun serveur n'a tourné -> barrière NON-BLOQUANTE"
  fi

  echo "== Verdict =="
  echo "  - MCP down = normal et SANS blocage : la barrière passe en soft, tu peux travailler ;"
  echo "    le harness retente la connexion tout seul (constaté). Sinon, redémarre la session."
  echo "  - Coincé malgré tout ? -> mcp_doctor.sh barrier-off (lève la barrière immédiatement)."
}

cmd_warm() {
  echo "Reconstruction/réparation du venv + pré-compilation des .pyc..."
  "${SCRIPT_DIR}/bootstrap_venv.sh" || { echo "bootstrap ÉCHOUÉ (réseau/proxy PyPI ?)"; return 1; }
  if [ -x "${VENV_PY}" ]; then
    "${VENV_PY}" -m compileall -q "${MMU_DIR}"/*.py "${MMU_DIR}/core" "${MMU_DIR}/evidence" "${MMU_DIR}/hooks" "${MMU_DIR}/ops" >/dev/null 2>&1 || true
    for d in "${MMU_DIR}"/.venv/lib/python*/site-packages/mcp; do "${VENV_PY}" -m compileall -q "$d" >/dev/null 2>&1 || true; done
  fi
  echo "OK."
}

cmd_barrier_off() { mkdir -p "${RUNTIME_DIR}"; date -u +%FT%TZ > "${BARRIER_OFF}"; echo "Barrière LEVÉE (créé ${BARRIER_OFF}). Ré-armer : mcp_doctor.sh barrier-on"; }
cmd_barrier_on()  { rm -f "${BARRIER_OFF}" && echo "Barrière RÉ-ARMÉE (retiré ${BARRIER_OFF})."; }
cmd_clean_locks() { rm -f "${MMU_DIR}/.venv.bootstrap.lock" && echo "Verrou de bootstrap retiré."; }

case "${1:-status}" in
  status)      cmd_status ;;
  warm)        cmd_warm ;;
  barrier-off) cmd_barrier_off ;;
  barrier-on)  cmd_barrier_on ;;
  clean-locks) cmd_clean_locks ;;
  *) echo "Usage: mcp_doctor.sh [status|warm|barrier-off|barrier-on|clean-locks]"; exit 2 ;;
esac
