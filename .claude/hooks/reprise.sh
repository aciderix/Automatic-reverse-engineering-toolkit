#!/bin/bash
# SessionStart[compact] hook — RITUEL DE REPRISE APRÈS COMPRESSION.
#
# Pourquoi ce hook existe : la consigne permanente de reprise ("relis 70/80/81,
# les dernières entrées du 71 et les derniers commits, énumère les règles, puis
# fais le point") était jusqu'ici transmise de main en main par l'agent sortant
# à l'agent entrant. Une chaîne humaine de ce type casse au premier maillon qui
# oublie. Le stdout d'un hook SessionStart est injecté dans le contexte de
# l'agent : la consigne devient donc structurelle, plus déclarative.
#
# Le principe sacré §0 est EXTRAIT EN DIRECT du doc 70 plutôt que recopié ici :
# une copie divergerait silencieusement de la source, ce qui est exactement le
# type de faux-positif que le principe interdit.
#
# Contrainte : rapide et sans effet de bord (pas de build) — il s'ajoute au
# session-start.sh existant, qui lui continue de tourner sur toutes les sources.
set -uo pipefail

cd "${CLAUDE_PROJECT_DIR:-.}" 2>/dev/null || exit 0
D=docs/vision

cat <<'BANNER'
================================================================================
  ARET — REPRISE APRÈS COMPRESSION (consigne permanente de l'utilisateur)
================================================================================
Tu viens d'être compacté. AVANT toute action, exécute ce rituel :

  1. Relis EN ENTIER  docs/vision/70-reference-etat-methode-reste.md
  2. Relis EN ENTIER  docs/vision/80-orientations-architecturales.md
                 et  docs/vision/81-industrialisation.md
  3. Relis les DERNIÈRES ENTRÉES de docs/vision/71-journal-de-bord.md
  4. Relis les DERNIERS COMMITS (rappelés ci-dessous)
  5. ÉNUMÈRE toutes les règles de travail — principe sacré §0, doctrine §1,
     méthode §2 (doc 70) et §3 (doc 80). Elles sont et resteront TOUJOURS
     incontournables.
  6. Fais le point, puis poursuis le travail en cours.

Cette consigne est permanente : elle vaut pour chaque compression, sans qu'on
ait à te la redonner.
BANNER

echo
echo "--- Principe sacré (extrait en direct de 70 §0, source de vérité) ---"
awk '/^## 0\. Objectif & principe sacré/{f=1} f&&/^## 1\./{exit} f' \
    "$D/70-reference-etat-methode-reste.md" 2>/dev/null

echo
echo "--- 10 derniers commits ---"
git log --oneline -10 2>/dev/null

echo
echo "--- Dernières entrées du journal 71 (titres) ---"
grep -n '^### ' "$D/71-journal-de-bord.md" 2>/dev/null | tail -8

echo
echo "--- État de l'arbre ---"
if [ -n "$(git status --porcelain 2>/dev/null)" ]; then
  echo "ATTENTION : travail non commité en cours —"
  git status -s 2>/dev/null | head -20
else
  echo "propre (rien à commiter)"
fi
echo "branche : $(git rev-parse --abbrev-ref HEAD 2>/dev/null)"
echo "================================================================================"
exit 0
