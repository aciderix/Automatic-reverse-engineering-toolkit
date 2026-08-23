#!/usr/bin/env python3
"""Hook Stop / PreCompact : persiste le Memory Store (commit+push borné) en fin de tour.

Pourquoi : le conteneur Claude Code est éphémère. Une mutation mémoire non poussée
disparaît au reset, et la session suivante re-clone le DERNIER état committé — donc
un état périmé, en silence. Ce hook ferme cette faille : à chaque frontière de tour,
il commite le SEUL dossier `.aret-memory/` et pousse la branche courante. Il est
non-fatal (ne bloque jamais la fin de tour) et désarmable par `ARET_MMU_SYNC_OFF=1`.
"""
from __future__ import annotations

import json
import os
from pathlib import Path

from common import input_payload
from ops.git_memory import sync_memory_only

_TRUTHY = {"1", "true", "yes", "on"}


def main() -> None:
    payload = input_payload()
    if os.environ.get("ARET_MMU_SYNC_OFF", "").strip().lower() in _TRUTHY:
        print(json.dumps({"ok": True, "hook": "sync", "skipped": "ARET_MMU_SYNC_OFF"}, ensure_ascii=False))
        return
    memory_dir = Path(str(payload.get("memory_dir") or os.environ.get("ARET_MEMORY_DIR") or ".aret-memory"))
    operation = str(payload.get("hook_event_name") or payload.get("reason") or "STOP")
    do_push = os.environ.get("ARET_MMU_SYNC_PUSH", "true").strip().lower() in _TRUTHY
    try:
        result = sync_memory_only(memory_dir.parent, str(memory_dir), operation, do_push=do_push)
        print(json.dumps({"ok": True, "hook": "sync", "result": result}, ensure_ascii=False))
    except Exception as exc:  # pragma: no cover - une fin de tour ne doit jamais échouer
        print(json.dumps({"ok": False, "hook": "sync", "error": str(exc)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
