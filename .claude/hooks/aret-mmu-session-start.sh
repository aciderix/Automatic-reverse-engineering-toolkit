#!/usr/bin/env bash
# SessionStart ARET-MMU : injecte uniquement le contexte chaud canonique.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/aret-mmu-env.sh"
# Pré-chauffe le venv du serveur MCP en arrière-plan (best-effort, non bloquant) :
# une session neuve a ainsi son venv prêt avant le premier appel d'outil mémoire,
# sans retarder l'injection du contexte. Le lanceur MCP re-bootstrappe au besoin.
"${aret_mmu_dir}/scripts/bootstrap_venv.sh" >/dev/null 2>&1 &
aret_mmu_exec session_start.py
