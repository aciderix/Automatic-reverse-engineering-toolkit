"""Hook SessionStart : restitue le contrat minimal de reprise, sans mutation."""

from __future__ import annotations

from typing import Any

from common import run
from evidence.adapters.pipelines import pipeline_catalog


def handler(store: Any, payload: dict[str, Any]) -> dict[str, Any]:
    brief = store.get_resume_brief()
    restored = brief.pop("restore")
    catalog = pipeline_catalog()
    recent_runs = store.get_pipeline_runs(limit=8)["runs"]
    pipeline_summary = {
        policy: [item["name"] for item in items]
        for policy, items in catalog["policies"].items()
    }
    return {
        **restored,
        "rules": brief["rules"],
        "latest_document_71_entries": brief["latest_document_71_entries"],
        "recent_audit": brief["recent_audit"],
        "resume_notice": brief["notice"],
        "pipeline_catalog": pipeline_summary,
        "recent_pipeline_runs": recent_runs,
        "pipeline_contract": catalog["contract"],
        "instructions": [
            "Utiliser FIND uniquement pour découvrir des candidats.",
            "Utiliser READ ou READ_BATCH pour récupérer les objets canoniques explicitement adressés.",
            "Ne jamais traiter un score de recherche comme une preuve.",
            "Consulter le catalogue pipeline avant une action ; lancer aret_run_pipeline avec dry_run=true avant toute exécution générative, réseau ou sensible.",
        ],
    }


if __name__ == "__main__":
    run("SessionStart", handler)
