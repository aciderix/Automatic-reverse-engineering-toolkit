#!/usr/bin/env bash
# Construit le zip distribuable du plugin ARET-MMU.
#
# - Rafraîchit la copie du skill depuis la source canonique (source unique de vérité :
#   .claude/skills/aret-mmu/SKILL.md), pour qu'il n'y ait jamais de dérive.
# - Zippe le dossier plugin AVEC .claude-plugin/plugin.json à la racine (ce qui satisfait
#   la validation Claude Code « must contain a .claude-plugin/plugin.json file »).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$here/../.." && pwd)"
canonical_skill="$repo_root/.claude/skills/aret-mmu/SKILL.md"

if [ ! -f "$canonical_skill" ]; then
  echo "ERREUR : skill canonique introuvable : $canonical_skill" >&2
  exit 1
fi

mkdir -p "$here/skills/aret-mmu"
cp -f "$canonical_skill" "$here/skills/aret-mmu/SKILL.md"
echo "skill rafraîchi depuis la source canonique."

mkdir -p "$here/dist"
out="$here/dist/aret-mmu-plugin.zip"
rm -f "$out"

# Zippe le contenu du dossier plugin (sans dist/, sans pack.sh) — racine = ce dossier.
( cd "$here" && zip -rq "$out" \
    .claude-plugin hooks skills README.md \
    -x 'dist/*' )

echo "zip construit : $out"
unzip -l "$out" | sed 's/^/  /'
