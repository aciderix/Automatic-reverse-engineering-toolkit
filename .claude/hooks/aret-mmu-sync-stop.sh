#!/usr/bin/env bash
# Stop / PreCompact ARET-MMU : persiste le Memory Store (commit+push borné à .aret-memory/).
# Non-fatal : n'interrompt jamais la fin de tour. Désarmable par ARET_MMU_SYNC_OFF=1.
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/aret-mmu-env.sh"
aret_mmu_exec sync_stop.py
