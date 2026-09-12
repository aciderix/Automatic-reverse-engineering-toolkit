#!/usr/bin/env python3
"""Aggregate every oracle MEASUREMENT of a run into one state file + summary.

argv[1] = directory holding the per-job measurement JSONs (aret-oracle-artifact/v1,
one per oracle, downloaded from the oracle-* artifacts). argv[2] = output results.json.

Produces, for a session to read in ONE place:
  * results.json — {generated_at, oracles:[{oracle,result,ratio,exit_code,duration_s,revision}],
    summary:{pass,fail,skipped,error,unknown,total}} — canonical, machine-readable state.
  * a compact markdown table appended to GITHUB_STEP_SUMMARY.

Gates like oracle_report.py: FAIL/ERROR anywhere (or no measurement at all) exits non-zero
so the aggregate job reflects the truth; PASS/SKIPPED/UNKNOWN stay green. Deliberately
dependency-free (stdlib) so it runs on a bare runner without the toolchain image.
"""
from __future__ import annotations

import json
import os
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

GATING = {"FAIL", "ERROR"}
# "N/M programs|functions|fixtures|opt-levels" — the ratio each oracle prints.
RATIO = re.compile(r"(\d+)\s*/\s*(\d+)\s+(?:programs|functions|fixtures|opt-levels)")


def load(directory: Path) -> list[dict]:
    items = []
    for path in sorted(directory.rglob("*.json")):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            items.append({"oracle": path.stem, "result": "ERROR", "_error": str(exc)})
            continue
        if isinstance(data, dict) and data.get("format") == "aret-oracle-artifact/v1":
            items.append(data)
    return items


def ratio_of(item: dict) -> str:
    m = RATIO.search(str(item.get("stdout", "")))
    return f"{m.group(1)}/{m.group(2)}" if m else ""


def main() -> int:
    directory = Path(sys.argv[1] if len(sys.argv) > 1 else "ci-measurements")
    out_path = Path(sys.argv[2] if len(sys.argv) > 2 else "results.json")
    items = sorted(load(directory), key=lambda d: str(d.get("oracle", "")))

    rows, counts = [], {"PASS": 0, "FAIL": 0, "SKIPPED": 0, "ERROR": 0, "UNKNOWN": 0}
    revision = ""
    for it in items:
        env = it.get("environment", {}) if isinstance(it.get("environment"), dict) else {}
        revision = revision or str(env.get("repository_revision", ""))
        result = str(it.get("result", "ERROR")).upper()
        counts[result] = counts.get(result, 0) + 1
        rows.append({
            "oracle": it.get("oracle", "?"), "result": result, "ratio": ratio_of(it),
            "exit_code": it.get("exit_code"), "duration_s": env.get("duration_seconds"),
            "revision": str(env.get("repository_revision", ""))[:12],
        })

    results = {
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "repository_revision": revision, "oracles": rows,
        "summary": {**{k.lower(): v for k, v in counts.items()}, "total": len(items)},
    }
    out_path.write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    lines = ["## ARET oracles — run aggregate", "",
             f"revision `{revision[:12]}` · " + " · ".join(f"{k.lower()}={v}" for k, v in counts.items() if v) + f" · total={len(items)}",
             "", "| oracle | result | ratio | exit | duration (s) |", "|---|---|---|---|---|"]
    for r in rows:
        lines.append(f"| {r['oracle']} | **{r['result']}** | {r['ratio']} | {r['exit_code']} | {r['duration_s']} |")
    summary = "\n".join(lines) + "\n"
    if (sp := os.environ.get("GITHUB_STEP_SUMMARY")):
        with open(sp, "a", encoding="utf-8") as fh:
            fh.write(summary)
    sys.stdout.write(summary)

    if not items:
        print("::error::no oracle measurements to aggregate", file=sys.stderr)
        return 1
    gating = [r for r in rows if r["result"] in GATING]
    for r in gating:
        print(f"::error::oracle {r['oracle']} -> {r['result']}", file=sys.stderr)
    return 1 if gating else 0


if __name__ == "__main__":
    raise SystemExit(main())
