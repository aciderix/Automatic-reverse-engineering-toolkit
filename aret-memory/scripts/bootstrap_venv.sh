#!/usr/bin/env bash
# Bootstrap idempotent et RAPIDE de l'environnement Python du serveur MCP ARET-MMU.
#
# Pourquoi : dans un conteneur Claude Code éphémère (web/cloud), le venv du projet
# n'existe pas encore au premier démarrage. Un `pip install` complet de `mcp` et de
# ses dépendances compilées prend ~17 s à froid, ce qui DÉPASSE le délai de connexion
# du client MCP : le serveur `aret-memory` n'apparaît alors jamais connecté.
#
# Ce script rend le bootstrap déterministe et court :
#   - chemin CHAUD : si le venv importe déjà `mcp`, il ne fait RIEN (sortie immédiate) ;
#   - chemin FROID : il privilégie `uv` (~1 s) et retombe sur `python -m venv` + `pip`.
#
# Contrats :
#   - toute journalisation va sur stderr (stdout reste réservé au flux MCP stdio) ;
#   - idempotent : rejouable sans effet de bord ;
#   - le paquet ARET-MMU lui-même est importé par PYTHONPATH (pas d'install éditable),
#     donc SEUL `mcp` doit être présent dans le venv.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MMU_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
VENV_DIR="${MMU_DIR}/.venv"
PY="${VENV_DIR}/bin/python"

# Contrainte de version alignée sur aret-memory/pyproject.toml (dépendance `mcp`).
MCP_SPEC="${ARET_MMU_MCP_SPEC:-mcp>=2.0,<3.0}"

# Chronométrage : chaque log porte le temps écoulé depuis le début du bootstrap, pour
# MESURER (et non deviner) où passent les secondes d'un cold-start lent — le timeout de
# connexion MCP (30 s) se joue ici. `ARET_MMU_BOOTSTRAP_QUIET=1` réduit au strict minimum.
_BOOT_T0="$(date +%s.%N 2>/dev/null || echo 0)"
_elapsed() { awk -v a="$_BOOT_T0" -v b="$(date +%s.%N 2>/dev/null || echo 0)" 'BEGIN{printf "%.2f", b-a}'; }
log() { printf 'ARET-MMU bootstrap [+%ss]: %s\n' "$(_elapsed)" "$*" >&2; }

usable() { [ -x "$PY" ] && "$PY" -c 'import mcp' >/dev/null 2>&1; }

# --- Chemin CHAUD : rien à faire. -------------------------------------------
if usable; then
  exit 0
fi

# --- Sérialisation anti-concurrence (cause d'une déconnexion MCP au cold-start).
# Sur un conteneur neuf le venv n'existe pas et DEUX bootstraps se lancent en même
# temps : le hook SessionStart en arrière-plan ET le launcher MCP en synchrone.
# Sans verrou, ils exécutent `uv venv`/`pip install` concurremment sur le MÊME
# dossier -> venv à moitié construit -> le serveur échoue `import mcp` -> le client
# voit "déconnecté" jusqu'à une reprise propre. `flock` fait attendre le second, qui
# retrouve alors le venv prêt et sort. Best-effort : si flock manque, on continue
# (le pire cas redevient l'ancien comportement, jamais pire).
LOCK="${MMU_DIR}/.venv.bootstrap.lock"
if command -v flock >/dev/null 2>&1; then
  exec 9>"$LOCK"
  if flock -w 120 9; then
    # Sous le verrou : l'autre bootstrap a pu terminer entre-temps.
    if usable; then
      log "venv déjà prêt (construit par un bootstrap concurrent)."
      exit 0
    fi
  else
    log "verrou de bootstrap non acquis (120 s) — poursuite best-effort."
  fi
fi

# --- Chemin FROID : privilégier uv (rapide ~3-5 s), sinon repli venv+pip. -----
# MESURE (2026-08-29) : avec uv, cold-start ≈ uv venv 0,1 s + install mcp ~2,9 s +
# import 1,8 s ≈ 5 s — bien sous le timeout MCP de 30 s. Le venv n'est PAS le goulot
# quand uv est présent. Le risque de timeout vient du REPLI pip ci-dessous (~17 s) sur
# un conteneur SANS uv : dans ce cas, provisionner uv dans le setup de l'environnement.
if command -v uv >/dev/null 2>&1; then
  log "chemin rapide : création du venv via uv ($(uv --version 2>/dev/null || echo uv))…"
  if uv venv "$VENV_DIR" >&2 2>&1; then
    UV_NO_PROGRESS=1 uv pip install --python "$PY" "$MCP_SPEC" >&2 2>&1 || true
  fi
fi

if ! usable; then
  log "AVERTISSEMENT : uv indisponible ou en échec — REPLI pip LENT (~17 s), risque de dépasser le timeout MCP 30 s ; provisionnez uv dans l'environnement pour l'éviter."
  "${PYTHON_BOOTSTRAP:-python3}" -m venv "$VENV_DIR" >&2 2>&1 || true
  if [ -x "$PY" ]; then
    "$PY" -m pip install --disable-pip-version-check "$MCP_SPEC" >&2 2>&1 || true
  fi
fi

if usable; then
  # Pré-compilation .pyc : on ne passe ici qu'APRÈS une vraie (re)construction (le
  # chemin chaud a court-circuité en tête). Compiler mcp + le paquet ARET-MMU évite
  # que le TOUT PREMIER spawn — celui du boot, sous forte charge — ait à compiler à
  # l'import. Best-effort, borné, jamais fatal ; logs sur stderr.
  "$PY" -m compileall -q "$MMU_DIR"/*.py "$MMU_DIR/core" "$MMU_DIR/evidence" "$MMU_DIR/hooks" "$MMU_DIR/ops" >&2 2>&1 || true
  for _d in "$VENV_DIR"/lib/python*/site-packages/mcp; do "$PY" -m compileall -q "$_d" >&2 2>&1 || true; done
  log "environnement prêt."
  exit 0
fi

log "ÉCHEC : le module 'mcp' reste introuvable après bootstrap (réseau/proxy ?)."
exit 1
