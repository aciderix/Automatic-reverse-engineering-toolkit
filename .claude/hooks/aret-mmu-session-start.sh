#!/usr/bin/env bash
# SessionStart ARET-MMU : injecte uniquement le contexte chaud canonique.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/aret-mmu-env.sh"
# Pré-chauffe le venv du serveur MCP en arrière-plan (best-effort, non bloquant) :
# une session neuve a ainsi son venv prêt avant le premier appel d'outil mémoire,
# sans retarder l'injection du contexte. En transport stdio, le wrapper mcp_stdio.sh
# (commande de .mcp.json, spawné par le harness) re-bootstrappe de toute façon À
# L'INTÉRIEUR de la fenêtre d'attente du handshake : ce pré-chauffe n'est qu'une
# optimisation du chemin chaud, jamais un prérequis.
"${aret_mmu_dir}/scripts/bootstrap_venv.sh" >/dev/null 2>&1 &
# NOTE : plus de lanceur HTTP ici. Le transport est stdio (harness-spawné) ; démarrer
# un serveur HTTP en parallèle ouvrirait la MÊME SQLite (-> "database is locked") et
# écrirait un marqueur de vivacité `mcp_ready` TROMPEUR (serveur up mais canal harness
# non connecté), ce qui re-armait la barrière en hard-block. Voir KN-0088.
aret_mmu_exec session_start.py
