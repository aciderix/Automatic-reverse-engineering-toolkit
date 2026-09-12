#!/usr/bin/env python3
"""Report ARET oracle measurements to the GitHub run summary, and gate the job.

Reads every aret-oracle-artifact/v1 JSON in the directory given as argv[1], writes a
compact verdict table to GITHUB_STEP_SUMMARY, and decides the exit code:

  * FAIL or ERROR in any measurement  -> exit 1 (job RED: a signal to go qualify it,
    per §0.5 — environmental vs real — never a trigger to auto-"fix").
  * only PASS / SKIPPED / UNKNOWN      -> exit 0 (SKIPPED = a toolchain piece genuinely
    absent; UNKNOWN = a pure measurement like winehash; neither is a failure).
  * no measurement file at all         -> exit 1 (the measure step did not even produce
    one — that is itself a failure to surface, never a silent green).

The raw JSON is uploaded separately as an artifact; this only summarises and gates.
It is deliberately dependency-free (stdlib only) so it runs in the toolchain image.
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

# Results that must turn the job red. PASS/SKIPPED/UNKNOWN do not.
GATING = {"FAIL", "ERROR"}


def load_measurements(directory: Path) -> list[dict]:
    items: list[dict] = []
    for path in sorted(directory.glob("*.json")):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            items.append({"oracle": path.stem, "result": "ERROR", "_error": f"unreadable measurement: {exc}"})
            continue
        items.append(data)
    return items


def summarise(items: list[dict]) -> str:
    lines = ["## ARET oracles (Linux, CI)", "", "| oracle | result | exit | duration (s) | revision |", "|---|---|---|---|---|"]
    for it in items:
        env = it.get("environment", {}) if isinstance(it.get("environment"), dict) else {}
        rev = str(env.get("repository_revision", ""))[:12]
        lines.append(
            f"| {it.get('oracle', '?')} | **{it.get('result', '?')}** | "
            f"{it.get('exit_code', '')} | {env.get('duration_seconds', '')} | `{rev}` |"
        )
    return "\n".join(lines) + "\n"


def main() -> int:
    directory = Path(sys.argv[1] if len(sys.argv) > 1 else "ci-out")
    items = load_measurements(directory)

    summary = summarise(items) if items else "## ARET oracles (Linux, CI)\n\n**No measurement file produced** — the measure step failed before writing one.\n"
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8") as handle:
            handle.write(summary)
    sys.stdout.write(summary)

    if not items:
        print("::error::no oracle measurement produced", file=sys.stderr)
        return 1

    gating = [it for it in items if str(it.get("result", "")).upper() in GATING]
    for it in gating:
        print(f"::error::oracle {it.get('oracle', '?')} -> {it.get('result')}", file=sys.stderr)
    return 1 if gating else 0


if __name__ == "__main__":
    raise SystemExit(main())
