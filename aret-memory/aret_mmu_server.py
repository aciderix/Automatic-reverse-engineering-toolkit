"""Serveur MCP ARET-MMU : façade métier déterministe devant SQLite."""

from __future__ import annotations

import argparse
import os
import threading
import time
from typing import Any

from mcp.server import MCPServer

from pathlib import Path

from core.repository import AretError, MemoryStore
from evidence.adapters.oracles import run_oracle
from evidence.adapters.pipelines import pipeline_catalog, register_asset, run_pipeline, toolchain_status
from hooks.resume_guard import touch_mcp_ready, validate_recap
from ops.git_memory import GitMemoryError, automatic_sync

SERVER_INSTRUCTIONS = """ARET-MMU fournit une mémoire durable déterministe et une façade de pipelines ARET à liste fermée. Utilisez FIND uniquement pour découvrir des candidats, puis READ ou READ_BATCH sur les adresses explicitement sélectionnées pour récupérer le contenu canonique. Ne déduisez jamais une preuve d’un score de recherche. PROVEN exige une preuve PASS admissible. Consultez d’abord aret_get_pipeline_catalog et utilisez aret_run_pipeline en dry_run ; les pipelines génératifs, réseau et sensibles exigent leurs confirmations explicites. Aucun shell, URL ou push Git arbitraire n’est exposé.

OUTILS ARET PAR LE MCP (obligatoire). Les oracles et pipelines ARET (winediff, cpudiff, funcdiff, difftest, wallsweep, sweeps…) s'exécutent via aret_run_oracle / aret_run_pipeline, JAMAIS par un équivalent shell direct pour une décision durable. Raison : un résultat passé par le MCP est enregistré en SQLite — canonique, adressable (ARET://pipeline/…, ARET://proof/…), horodaté et il SURVIT À LA COMPACTION du contexte ; une sortie shell brute est éphémère et non-canonique (elle disparaît au prochain résumé de contexte). Le shell reste un laboratoire pour explorer/compiler/diagnostiquer, mais sa sortie n'est ni un fait canonique ni une preuve.

INDUSTRIALISER OU NON. Tout outil réutilisable, ou qui contribue de façon récurrente à une décision, une validation, une preuve, un corpus, un asset ou une mesure de priorisation, DOIT être ajouté au catalogue MCP (liste fermée, paramètres bornés, politique, artefact adressable, tests) avant d'être une capacité officielle. À l'inverse, un script ponctuel — spécifique à une seule reproduction, sans effet durable sur les décisions — reste un prototype local et n'a pas à entrer dans le MCP.

FRONT & HANDOFF TOUJOURS À JOUR (obligatoire). Le Front est le pointeur de travail vivant : dès qu'il change (subsystem, brique, mur courant, prochaine action, champ de handoff), mettez-le à jour via aret_update_front — garder l'avancement fidèle est primordial. aret_update_front renvoie `handoff_status` : si `stale=true`, votre changement a PÉRIMÉ le handoff et vous DEVEZ ré-exécuter aret_prepare_handoff. Vérifiez la continuité à tout moment via aret_get_resume_status (`degraded=true` ⇒ la prochaine reprise serait dégradée ; chaque manque nomme l'outil qui le répare). En fin de session ou avant compaction, un handoff frais et non périmé est la condition d'une reprise NORMALE."""

store = MemoryStore()
mcp = MCPServer(
    "ARET-MMU",
    title="ARET Memory Management Unit",
    description="Mémoire SQLite structurée, adressable et probatoire pour ARET.",
    instructions=SERVER_INSTRUCTIONS,
    version="0.1.0",
)


def _configured_repository() -> Path:
    """Résout une unique racine ARET dérivée du Store configuré."""
    default = store.memory_dir.parents[1] if store.memory_dir.parent.name == "aret-memory" else Path.cwd()
    return default.resolve()


def _configured_repository_path(repository_path: str | None) -> Path:
    """Refuse toute racine de travail différente du dépôt ARET configuré."""
    configured = _configured_repository()
    candidate = Path(repository_path).expanduser().resolve() if repository_path else configured
    if candidate != configured:
        raise AretError("repository_path doit désigner le dépôt ARET configuré")
    return configured


def _call(operation: str, **kwargs: Any) -> dict[str, Any]:
    """Convertit toute erreur métier en résultat structuré, sans ambiguïté."""
    try:
        return {"ok": True, "operation": operation, "result": getattr(store, operation)(**kwargs)}
    except AretError as exc:
        return {"ok": False, "operation": operation, "error": {"code": type(exc).__name__, "message": str(exc)}}
    except Exception as exc:  # Défense de transport : ne jamais faire tomber le serveur sur une requête invalide.
        return {"ok": False, "operation": operation, "error": {"code": "INTERNAL_ERROR", "message": str(exc)}}


@mcp.tool()
def aret_boot() -> dict[str, Any]:
    """Retourne la doctrine minimale, les bornes de pagination et l’état opérationnel du Memory Store."""
    return _call("boot")


@mcp.tool()
def aret_get_front() -> dict[str, Any]:
    """Retourne l’Active Front minimal, sans charger le journal ou les connaissances froides."""
    return _call("get_front")


@mcp.tool()
def aret_restore() -> dict[str, Any]:
    """Retourne le contexte chaud restaurable : doctrine, versions et Active Front, sans historique massif."""
    return _call("restore")


@mcp.tool()
def aret_get_resume_brief(journal_limit: int = 8, rule_limit: int = 20, audit_limit: int = 12) -> dict[str, Any]:
    """Retourne le paquet de reprise : Front, règles, dernières entrées 71 et audit ; Git reste séparé en lecture seule."""
    return _call("get_resume_brief", journal_limit=journal_limit, rule_limit=rule_limit, audit_limit=audit_limit)


@mcp.tool()
def aret_get_resume_protocol(journal_limit: int = 8, batch_size: int = 20) -> dict[str, Any]:
    """Compatibilité : retourne les pointeurs documentaires, sans imposer leur relecture après reprise.

    Choix entre les vues de reprise : aret_boot = état opérationnel + doctrine ; aret_get_front =
    Active Front minimal ; aret_get_resume_brief = Front + règles + dernières entrées 71 + audit ;
    aret_get_resume_status = verdict COMPACT (une session fraîche reprendrait-elle normalement ?)."""
    return _call("get_resume_protocol", journal_limit=journal_limit, batch_size=batch_size)


@mcp.tool()
def aret_get_resume_status() -> dict[str, Any]:
    """Verdict COMPACT de reprise (lecture seule) : `degraded` + `missing` (chaque manque nomme l'outil qui le répare) + `warnings` de provenance.

    À utiliser pour vérifier la continuité SANS rejouer le hook de démarrage : si `degraded=True`,
    la barrière serait dégradée au prochain SessionStart et `missing` dit exactement quoi préparer
    (le plus souvent aret_prepare_handoff). `warnings` signale un Front possiblement semé par un
    bootstrap et jamais validé."""
    return _call("resume_status")


@mcp.tool()
def aret_acknowledge_resume(
    working_rules: str,
    current_state: str,
    capabilities: str,
    git_state: str,
    risks_and_limits: str,
    next_action: str,
    resume_contract_hash: str,
) -> dict[str, Any]:
    """Valide le récapitulatif rituel requis après SessionStart ou PostCompact avant toute poursuite ARET."""
    try:
        recap = validate_recap({
            "working_rules": working_rules,
            "current_state": current_state,
            "capabilities": capabilities,
            "git_state": git_state,
            "risks_and_limits": risks_and_limits,
            "next_action": next_action,
        })
        if len(resume_contract_hash) != 64 or any(char not in "0123456789abcdef" for char in resume_contract_hash):
            raise ValueError("resume_contract_hash doit être un SHA-256 hexadécimal de 64 caractères")
        return {
            "ok": True,
            "operation": "acknowledge_resume",
            "result": {
                "acknowledged": True,
                "sections": list(recap),
                "resume_contract_hash": resume_contract_hash,
                "notice": "Le récapitulatif est formellement complet. Le hook PostToolUse ne lève la barrière que si son hash correspond au dossier injecté.",
            },
        }
    except ValueError as exc:
        return {"ok": False, "operation": "acknowledge_resume", "error": {"code": "AretError", "message": str(exc)}}


@mcp.tool()
def aret_find(
    component_id: str | None = None,
    function_id: str | None = None,
    brick_id: str | None = None,
    knowledge_type: str | None = None,
    status: str | None = None,
    tag: str | None = None,
    text: str | None = None,
    created_after: str | None = None,
    created_before: str | None = None,
    limit: int = 20,
) -> dict[str, Any]:
    """Découvre des candidats par filtres structurés ou FTS5 ; ne retourne jamais le contenu intégral comme preuve."""
    return _call(
        "find", component_id=component_id, function_id=function_id, brick_id=brick_id,
        knowledge_type=knowledge_type, status=status, tag=tag, text=text,
        created_after=created_after, created_before=created_before, limit=limit,
    )


@mcp.tool()
def aret_read(address: str) -> dict[str, Any]:
    """Récupère exactement la ressource dont l’adresse ARET est connue, avec son hash et ses métadonnées."""
    return _call("read", address=address)


@mcp.tool()
def aret_read_batch(addresses: list[str], max_items: int = 20, max_bytes: int = 65536) -> dict[str, Any]:
    """Récupère plusieurs pages adressées dans un seul appel, en refusant tout dépassement des bornes demandées."""
    return _call("read_batch", addresses=addresses, max_items=max_items, max_bytes=max_bytes)


@mcp.tool()
def aret_get_forensics(
    component_id: str | None = None, function_id: str | None = None, status: str | None = None, limit: int = 20
) -> dict[str, Any]:
    """Découvre les forensics liés à un composant ou une fonction, sans confondre découverte et lecture exacte."""
    return _call("get_forensics", component_id=component_id, function_id=function_id, status=status, limit=limit)


@mcp.tool()
def aret_get_proofs(knowledge_id: str) -> dict[str, Any]:
    """Retourne les métadonnées des preuves liées à une connaissance, sans charger leurs artefacts lourds."""
    return _call("get_proofs", knowledge_id=knowledge_id)


@mcp.tool()
def aret_get_related(
    entity_id: str, relation_type: str | None = None, direction: str = "both", include_inactive: bool = False
) -> dict[str, Any]:
    """Traverse les relations actives par défaut ; l’historique remplacé exige include_inactive=true."""
    return _call(
        "get_related", entity_id=entity_id, relation_type=relation_type, direction=direction,
        include_inactive=include_inactive,
    )


@mcp.tool()
def aret_get_roadmap(
    milestone: str | None = None, component_id: str | None = None, target_platform: str | None = None,
    include_done: bool = False, max_items: int = 50,
) -> dict[str, Any]:
    """Retourne une vue roadmap compacte des briques, états, bloqueurs, décisions et preuves actifs."""
    return _call(
        "get_roadmap", milestone=milestone, component_id=component_id, target_platform=target_platform,
        include_done=include_done, max_items=max_items,
    )


@mcp.tool()
def aret_read_artifact(proof_id: str, max_bytes: int = 65536) -> dict[str, Any]:
    """Lit explicitement et de manière bornée l’artefact d’une preuve après vérification de son hash."""
    return _call("read_artifact", proof_id=proof_id, max_bytes=max_bytes)


@mcp.tool()
def aret_append_knowledge(
    knowledge_type: str,
    title: str,
    content: str,
    status: str | None = None,
    component_id: str | None = None,
    function_id: str | None = None,
    brick_id: str | None = None,
    tags: list[str] | str | None = None,
    proof_ids: list[str] | None = None,
    supersedes_id: str | None = None,
    effective_at: str | None = None,
    document_source: dict[str, Any] | None = None,
    actor: str = "mcp-agent",
) -> dict[str, Any]:
    """Ajoute une connaissance append-only et auditée, avec provenance documentaire optionnelle et contrôlée.

    knowledge_type ∈ {RULE, ARCHITECTURE, DECISION, FORENSIC, OBSERVATION, HYPOTHESIS, STATE,
    MEASUREMENT, DISCOVERY} (insensible à la casse).
    document_source, si fourni, EXIGE les sept clés : repository, revision, path, start_line,
    end_line, section, hash — `path` relatif au dépôt (ni `/` initial ni `..`), 1≤start_line≤end_line.
    Ne pas fournir un hash inventé : omettre document_source si l'empreinte de section n'est pas connue
    (les références peuvent alors vivre dans `content`)."""
    return _call(
        "append_knowledge", knowledge_type=knowledge_type, status=status, title=title, content=content,
        component_id=component_id, function_id=function_id, brick_id=brick_id, tags=tags,
        proof_ids=proof_ids, supersedes_id=supersedes_id, actor=actor,
        effective_at=effective_at, document_source=document_source,
    )


@mcp.tool()
def aret_update_front(updates: dict[str, str], actor: str = "mcp-agent") -> dict[str, Any]:
    """Met à jour une partie bornée de l'Active Front et inscrit un audit event.

    OBLIGATION : le Front DOIT refléter le travail réel à tout instant — dès que le subsystem,
    la brique, le mur courant, la prochaine action ou un champ de handoff change, mettez-le à jour
    ici. Le résultat contient `handoff_status` : si `stale=true`, votre changement a PÉRIMÉ le
    handoff et vous DEVEZ ré-exécuter aret_prepare_handoff (sinon la prochaine reprise sera
    dégradée). `changed_keys` liste les clés de reprise effectivement modifiées."""
    return _call("update_front", updates=updates, actor=actor)


@mcp.tool()
def aret_replace_front(updates: dict[str, str], actor: str = "mcp-agent") -> dict[str, Any]:
    """Remplace entièrement l’Active Front après validation des clés et inscrit l’état précédent dans l’audit."""
    return _call("replace_front", updates=updates, actor=actor)


@mcp.tool()
def aret_prepare_handoff(
    work_summary: str,
    verified_results: str,
    open_risks: str,
    deferred_items: str,
    next_action: str,
    technical_checkpoint_state: str,
    technical_target: str = "",
    technical_change: str = "",
    execution_state: str = "",
    last_validation: str = "",
    immediate_actions: str = "",
    relevant_addresses: list[str] | None = None,
    actor: str = "mcp-agent",
) -> dict[str, Any]:
    """Prépare atomiquement le handoff et le checkpoint technique ; c'est l'outil unique qui répare une reprise dégradée.

    BORNES (octets UTF-8, pas caractères : un accent = 2 octets — viser large) :
    - work_summary, verified_results, open_risks, deferred_items, next_action : 1000 octets chacun.
    - Dossier de reprise ASSEMBLÉ (playbook + handoff + adresses) : 12500 octets MAX, 2000 MIN.
      Si dépassement, RACCOURCIR ces cinq champs (le playbook stable est incompressible).
    technical_checkpoint_state est un ENUM, pas du texte : "NONE" ou "ACTIVE".
    - "NONE" : les cinq champs technical_* DOIVENT rester vides (aucun geste technique inventé).
    - "ACTIVE" : les cinq champs technical_* sont REQUIS et bornés — technical_target≤120,
      technical_change≤160, execution_state≤130, last_validation≤160, immediate_actions≤180 octets.
    relevant_addresses : uniquement des adresses de CONNAISSANCE (ARET://knowledge/...) — une adresse
    de brique est refusée (le Front, lui, accepte une brique dans relevant_N_address).
    Un seul appel peuple TOUS les champs dérivés du dossier (handoff_front_hash, cutoffs V1.3,
    handoff_prepared_at) : c'est donc lui qui corrige les manques listés par aret_get_resume_status.

    OBLIGATION : ré-exécutez cet outil dès que le handoff est PÉRIMÉ (aret_update_front renvoie
    `handoff_status.stale=true`, ou aret_get_resume_status signale « périmé ») — garder l'avancement
    à jour est primordial pour une reprise fidèle.
    RÉPONSE : en cas de succès, le dossier complet (`ready=true`) est rendu. En cas d'échec de
    bornes, TOUTES les violations sont rendues d'un coup (compte d'octets réel vs borne par champ) :
    corrigez-les en une seule fois. Si le dossier assemblé n'est pas prêt (p.ex. dépassement de
    budget), un diagnostic COMPACT est rendu (`written=true`, `errors`, `overflow_bytes`,
    `field_bytes`) SANS ré-écho du playbook — raccourcissez les champs visés puis rappelez l'outil."""
    return _call(
        "prepare_handoff",
        work_summary=work_summary,
        verified_results=verified_results,
        open_risks=open_risks,
        deferred_items=deferred_items,
        next_action=next_action,
        technical_checkpoint_state=technical_checkpoint_state,
        technical_target=technical_target,
        technical_change=technical_change,
        execution_state=execution_state,
        last_validation=last_validation,
        immediate_actions=immediate_actions,
        relevant_addresses=relevant_addresses,
        actor=actor,
    )


@mcp.tool()
def aret_rebuild_front(actor: str = "mcp-front-rebuild") -> dict[str, Any]:
    """Reconstitue les pointeurs dérivés du Front depuis les connaissances canoniques, sans supprimer les clés manuelles."""
    return _call("rebuild_front", actor=actor)


@mcp.tool()
def aret_record_proof(
    kind: str,
    command: str,
    result: str,
    exit_code: int | None = None,
    stdout_ref: str = "",
    stderr_ref: str = "",
    artifact_path: str = "",
    artifact_hash: str = "",
    environment: dict[str, Any] | None = None,
    started_at: str | None = None,
    finished_at: str | None = None,
    receipt_hmac: str = "",
    actor: str = "trusted-oracle",
) -> dict[str, Any]:
    """Enregistre une preuve. Elle n’est admissible qu’avec un reçu HMAC valide produit par un adaptateur de confiance."""
    return _call(
        "record_proof", kind=kind, command=command, result=result, exit_code=exit_code,
        stdout_ref=stdout_ref, stderr_ref=stderr_ref, artifact_path=artifact_path, artifact_hash=artifact_hash,
        environment=environment, started_at=started_at, finished_at=finished_at, receipt_hmac=receipt_hmac, actor=actor,
    )


@mcp.tool()
def aret_add_relation(from_id: str, relation_type: str, to_id: str, actor: str = "mcp-agent") -> dict[str, Any]:
    """Ajoute une relation immutable, explicitement typée et auditée entre deux objets existants."""
    return _call("add_relation", from_id=from_id, relation_type=relation_type, to_id=to_id, actor=actor)


@mcp.tool()
def aret_supersede_relation(
    relation_id: str, from_id: str, relation_type: str, to_id: str, actor: str = "mcp-agent"
) -> dict[str, Any]:
    """Crée une relation de remplacement et enregistre la supersession de l’ancienne dans l’audit append-only."""
    return _call(
        "supersede_relation", relation_id=relation_id, from_id=from_id,
        relation_type=relation_type, to_id=to_id, actor=actor,
    )


@mcp.tool()
def aret_register_component(component_id: str, title: str, description: str = "", actor: str = "mcp-agent") -> dict[str, Any]:
    """Crée un composant stable et adressable avant de lui associer connaissances, fonctions ou briques."""
    return _call("register_component", component_id=component_id, title=title, description=description, actor=actor)


@mcp.tool()
def aret_register_function(
    component_id: str, module: str, symbol: str, calling_convention: str = "", actor: str = "mcp-agent"
) -> dict[str, Any]:
    """Crée un symbole ou une fonction adressable, explicitement rattaché à un composant existant."""
    return _call(
        "register_function", component_id=component_id, module=module, symbol=symbol,
        calling_convention=calling_convention, actor=actor,
    )


@mcp.tool()
def aret_register_brick(
    brick_id: str, title: str, state: str = "PLANNED", component_id: str | None = None,
    description: str = "", milestone: str | None = None, target_platform: str | None = None,
    priority: int = 3, actor: str = "mcp-agent"
) -> dict[str, Any]:
    """Crée une brique mesurable avec son état, jalon, cible et priorité de roadmap contrôlés.

    state ∈ {PLANNED, ACTIVE, BLOCKED, DONE, OBSOLETE}. La clé `brick` du Front doit toujours
    référencer une brique ACTIVE ; il ne devrait exister qu'UN front de travail ACTIVE à la fois."""
    return _call(
        "register_brick", brick_id=brick_id, title=title, state=state, component_id=component_id,
        description=description, milestone=milestone, target_platform=target_platform, priority=priority, actor=actor,
    )


@mcp.tool()
def aret_update_brick(
    brick_id: str, state: str | None = None, milestone: str | None = None,
    target_platform: str | None = None, priority: int | None = None, actor: str = "mcp-agent"
) -> dict[str, Any]:
    """Met à jour l’état et le classement d’une brique, avec audit et protection du Front actif.

    state ∈ {PLANNED, ACTIVE, BLOCKED, DONE, OBSOLETE}. Faire passer hors ACTIVE une brique encore
    référencée par le Front est refusé : réaligner d'abord le Front (aret_update_front)."""
    return _call(
        "update_brick", brick_id=brick_id, state=state, milestone=milestone,
        target_platform=target_platform, priority=priority, actor=actor,
    )


@mcp.tool()
def aret_attach_proof(knowledge_id: str, proof_id: str, promote: bool = False, actor: str = "mcp-agent") -> dict[str, Any]:
    """Lie une preuve existante à une connaissance et ne promeut que si le proof est PASS admissible."""
    return _call("attach_proof", knowledge_id=knowledge_id, proof_id=proof_id, promote=promote, actor=actor)


@mcp.tool()
def aret_invalidate_proof(proof_id: str, reason: str, actor: str = "mcp-auditor") -> dict[str, Any]:
    """Invalide une preuve et rétrograde transactionnellement les `PROVEN` non justifiés restants."""
    return _call("invalidate_proof", proof_id=proof_id, reason=reason, actor=actor)


@mcp.tool()
def aret_run_oracle(
    oracle: str, knowledge_id: str | None = None, promote: bool = False, fixture: str | None = None,
    timeout_seconds: int | None = None, repository_path: str | None = None, actor: str = "mcp-oracle-adapter"
) -> dict[str, Any]:
    """Exécute difftest, winehash, winediff ou funcdiff via une liste fermée, puis enregistre son artefact et sa preuve.

    PASSER PAR ICI plutôt que par le shell : le verdict et l'artefact deviennent canoniques,
    adressables (ARET://proof/…) et survivent à la compaction ; une sortie shell brute est éphémère."""
    try:
        repository = _configured_repository_path(repository_path)
        return {"ok": True, "operation": "run_oracle", "result": run_oracle(
            store, repository, oracle, knowledge_id, promote, fixture, timeout_seconds, actor,
        )}
    except AretError as exc:
        return {"ok": False, "operation": "run_oracle", "error": {"code": type(exc).__name__, "message": str(exc)}}
    except Exception as exc:
        return {"ok": False, "operation": "run_oracle", "error": {"code": "INTERNAL_ERROR", "message": str(exc)}}


@mcp.tool()
def aret_get_pipeline_catalog() -> dict[str, Any]:
    """Retourne les pipelines ARET nommés, leurs politiques, prérequis et contrats sans lancer de processus."""
    return {"ok": True, "operation": "get_pipeline_catalog", "result": pipeline_catalog()}


@mcp.tool()
def aret_get_toolchain_status(repository_path: str | None = None) -> dict[str, Any]:
    """Diagnostique les prérequis Wine, MinGW, Rust, Unicorn, Clang, Z3 et le binaire ARET sans mutation."""
    try:
        repository = _configured_repository_path(repository_path)
        return {"ok": True, "operation": "get_toolchain_status", "result": toolchain_status(repository)}
    except AretError as exc:
        return {"ok": False, "operation": "get_toolchain_status", "error": {"code": type(exc).__name__, "message": str(exc)}}
    except Exception as exc:
        return {"ok": False, "operation": "get_toolchain_status", "error": {"code": "INTERNAL_ERROR", "message": str(exc)}}


@mcp.tool()
def aret_run_pipeline(
    pipeline: str, parameters: dict[str, Any] | None = None, dry_run: bool = True,
    confirm_apply: bool = False, confirm_network: bool = False, confirm_sensitive: bool = False,
    timeout_seconds: int | None = None, repository_path: str | None = None, actor: str = "mcp-pipeline-adapter",
) -> dict[str, Any]:
    """Exécute un pipeline ARET de liste fermée ; dry_run est la valeur sûre par défaut et aucune commande libre n’est admise."""
    try:
        repository = _configured_repository_path(repository_path)
        result = run_pipeline(
            store, repository, pipeline, parameters, dry_run=dry_run, confirm_apply=confirm_apply,
            confirm_network=confirm_network, confirm_sensitive=confirm_sensitive,
            timeout_seconds=timeout_seconds, actor=actor,
        )
        return {"ok": True, "operation": "run_pipeline", "result": result}
    except AretError as exc:
        return {"ok": False, "operation": "run_pipeline", "error": {"code": type(exc).__name__, "message": str(exc)}}
    except Exception as exc:
        return {"ok": False, "operation": "run_pipeline", "error": {"code": "INTERNAL_ERROR", "message": str(exc)}}


@mcp.tool()
def aret_get_pipeline_runs(pipeline: str | None = None, limit: int = 12) -> dict[str, Any]:
    """Retourne les derniers résultats de pipelines et leurs adresses, sans charger les artefacts lourds."""
    return _call("get_pipeline_runs", pipeline_name=pipeline, limit=limit)


@mcp.tool()
def aret_read_pipeline_artifact(pipeline_run_id: str, max_bytes: int = 65536) -> dict[str, Any]:
    """Lit l’artefact hashé d’une exécution de pipeline explicitement adressée."""
    return _call("read_pipeline_artifact", pipeline_run_id=pipeline_run_id, max_bytes=max_bytes)


@mcp.tool()
def aret_get_assets(kind: str | None = None, limit: int = 20) -> dict[str, Any]:
    """Retourne les corpus, binaires et snapshots enregistrés avec leurs hashes et provenances, sans charger leurs octets."""
    return _call("get_assets", kind=kind, limit=limit)


@mcp.tool()
def aret_register_asset(source_path: str, kind: str, confirm_import: bool = False, actor: str = "mcp-asset-import") -> dict[str, Any]:
    """Copie et enregistre un asset local ARET autorisé après confirmation explicite, hash et provenance contrôlée."""
    try:
        default_repository = store.memory_dir.parents[1] if store.memory_dir.parent.name == "aret-memory" else Path.cwd()
        return {"ok": True, "operation": "register_asset", "result": register_asset(
            store, default_repository, source_path, kind, confirm_import=confirm_import, actor=actor,
        )}
    except AretError as exc:
        return {"ok": False, "operation": "register_asset", "error": {"code": type(exc).__name__, "message": str(exc)}}
    except Exception as exc:
        return {"ok": False, "operation": "register_asset", "error": {"code": "INTERNAL_ERROR", "message": str(exc)}}


@mcp.tool()
def aret_sync_memory(operation: str = "MCP_SYNC") -> dict[str, Any]:
    """Déclenche une synchronisation post-mutation strictement limitée au `.aret-memory/` courant et à sa politique JSON locale (sync_policy.json : auto_commit/auto_push, opt-in).

    Distinct de la persistance AUTOMATIQUE de fin de tour : les hooks Stop/PreCompact
    (aret-mmu-sync-stop.sh) commitent+poussent déjà le Memory Store sur la branche courante à
    chaque tour. Cet appel reste utile pour forcer une synchronisation immédiate selon la policy."""
    try:
        result = automatic_sync(store.memory_dir.parent, str(store.memory_dir), operation)
        store.last_sync_status = result
        return {"ok": True, "operation": "sync_memory", "result": result}
    except GitMemoryError as exc:
        return {"ok": False, "operation": "sync_memory", "error": {"code": type(exc).__name__, "message": str(exc)}}


@mcp.tool()
def aret_rebuild_index(actor: str = "aret-mmu-maintenance") -> dict[str, Any]:
    """Reconstruit l’index FTS5 entièrement à partir des tables canoniques et journalise l’opération."""
    return _call("rebuild_index", actor=actor)


@mcp.tool()
def aret_export_reference_91(output_name: str | None = None) -> dict[str, Any]:
    """Produit une synthèse dérivée compatible avec l’ancien numéro 91 ; aucun Markdown 91 n’est attendu ni importé."""
    return _call("export_reference_91", output_name=output_name)


@mcp.tool()
def aret_export_roadmap(
    milestone: str | None = None, component_id: str | None = None, target_platform: str | None = None,
    include_done: bool = False, max_items: int = 50, output_name: str | None = None,
) -> dict[str, Any]:
    """Génère un Markdown de roadmap dérivé de SQLite, avec hash logique et sans fichier source parallèle."""
    return _call(
        "export_roadmap", milestone=milestone, component_id=component_id, target_platform=target_platform,
        include_done=include_done, max_items=max_items, output_name=output_name,
    )


@mcp.tool()
def aret_export(export_format: str = "json", output_name: str | None = None) -> dict[str, Any]:
    """Produit une vue JSON, Markdown, HTML ou un bundle ZIP v3 dérivé de l’état canonique."""
    return _call("export", export_format=export_format, output_name=output_name)


@mcp.tool()
def aret_export_bundle(output_name: str | None = None) -> dict[str, Any]:
    """Crée un Memory Bundle ZIP v3 avec manifest, migrations hashées et inventaire d’artefacts vérifiés."""
    return _call("export_bundle", output_name=output_name)


@mcp.tool()
def aret_import_bundle(bundle_path: str, actor: str = "mcp-bundle-import") -> dict[str, Any]:
    """Importe un bundle vérifié uniquement dans un Store vide ; toute fusion implicite est refusée."""
    return _call("import_bundle", bundle_path=bundle_path, actor=actor)


def main() -> None:
    """Lance le transport explicitement demandé, stdio restant le mode de connecteur local."""
    global store
    parser = argparse.ArgumentParser(description="Serveur MCP ARET-MMU")
    parser.add_argument("--memory-dir", help="Répertoire .aret-memory à utiliser")
    parser.add_argument("--write-enabled", action="store_true", help="Active les mutations contrôlées pour cette exécution")
    parser.add_argument("--streamable-http", action="store_true", help="Expose le serveur sur HTTP au lieu de stdio")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    if args.memory_dir:
        os.environ["ARET_MEMORY_DIR"] = args.memory_dir
    if args.write_enabled:
        os.environ["ARET_WRITE_ENABLED"] = "true"
    store = MemoryStore()
    _start_liveness_heartbeat(store.memory_dir)
    if args.streamable_http:
        mcp.run("streamable-http", host=args.host, port=args.port, streamable_http_path="/mcp")
    else:
        mcp.run("stdio")


def _start_liveness_heartbeat(memory_dir: Path) -> None:
    """Marqueur de vivacité pour la barrière de reprise (hooks/resume_guard).

    Écrit `runtime/mcp_ready` au démarrage puis le rafraîchit toutes les ~20 s via un
    thread démon. Sa présence FRAÎCHE prouve à la barrière PreToolUse que la porte de
    sortie (aret_acknowledge_resume) est réellement atteignable — sans quoi la barrière
    ne hard-bloque pas (anti-deadlock). Best-effort : jamais fatal, n'écrit rien sur
    stdout (réservé au flux MCP stdio)."""
    def _beat() -> None:
        while True:
            try:
                touch_mcp_ready(memory_dir)
            except Exception:  # noqa: BLE001 — le heartbeat ne doit jamais tuer le serveur
                pass
            time.sleep(20)

    try:
        touch_mcp_ready(memory_dir)
    except Exception:  # noqa: BLE001
        pass
    threading.Thread(target=_beat, name="aret-mmu-liveness", daemon=True).start()


if __name__ == "__main__":
    main()
