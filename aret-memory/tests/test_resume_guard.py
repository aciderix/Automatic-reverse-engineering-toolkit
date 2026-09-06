from __future__ import annotations

import os
import time
from pathlib import Path

from core.repository import MemoryStore
from hooks.resume_guard import (
    MCP_LIVENESS_MAX_AGE_S,
    acknowledge,
    arm,
    barrier_off_sentinel,
    build_attempt_diagnostic,
    decision,
    mcp_ready_marker,
    state_path,
    stop_feedback,
    touch_mcp_ready,
    validate_recap,
)


RESUME_HASH = "a" * 64
PLAYBOOK_PADDING = (
    " Cette entrée contractuelle s’applique à chaque incrément, doit être vérifiée par une preuve "
    "reproductible et interdit toute valeur devinée, tout succès silencieux ou toute conclusion non auditée."
)

PLAYBOOK = (
    ("PLAYBOOK_FOUNDATION", "Principe sound", "Juste ou arrêt bruyant, jamais de valeur inventée ni de faux succès silencieux."),
    ("PLAYBOOK_METHOD", "Méthode", "Mesurer, reproduire, fixture minimale, implémenter, vérifier puis documenter."),
    ("PLAYBOOK_ARCHITECTURE", "Architecture", "Wine est une source et un oracle, jamais une dépendance runtime ; le modèle shared-stack reste sound."),
    ("PLAYBOOK_GATES", "Portes", "Les oracles appropriés et les hashes requis valident chaque changement pertinent."),
    ("PLAYBOOK_TOOLING", "Outils", "Trace, relay, wallsweep et pipelines fermés servent à mesurer avant toute modification."),
)


def _populated_store(memory_dir: Path) -> MemoryStore:
    store = MemoryStore(memory_dir, write_enabled=True)
    store.register_component("CORE", "Noyau", "Fixture de reprise", "test")
    store.register_brick("RESUME-01", "Brique de reprise", "ACTIVE", "CORE", "Fixture", "test", "M1", "x86-pe32", 1)
    addresses: list[str] = []
    for domain, title, content in PLAYBOOK:
        tags = ["CORE_PLAYBOOK", domain]
        if domain == "PLAYBOOK_ARCHITECTURE":
            tags.append("PLAYBOOK_SHARED_STACK")
        record = store.append_knowledge(
            knowledge_type="ARCHITECTURE" if domain == "PLAYBOOK_ARCHITECTURE" else "RULE",
            status="ACTIVE", title=title, content=content + PLAYBOOK_PADDING, component_id="CORE", function_id=None,
            brick_id=None, tags=tags, proof_ids=[], supersedes_id=None, actor="test",
        )
        addresses.append(record["address"])
    store.update_front({
        "subsystem": "Fixture", "brick": "RESUME-01", "current_wall": "Validation de reprise",
        "last_action": "Playbook V5 tagué.", "next_action": "Confirmer le rituel de reprise.",
        "relevant_1_address": addresses[0],
    }, "test")
    store.prepare_handoff(
        work_summary="Le playbook stable est prêt et le contexte métier actif est explicitement préparé.",
        verified_results="Les connaissances de fixture et le Front ont été écrits dans SQLite avec audit.",
        open_risks="Aucun risque bloquant connu ; toute divergence doit interrompre la reprise.",
        deferred_items="Aucun élément différé ne doit masquer une action de reprise critique.",
        next_action="Produire le récapitulatif puis confirmer le rituel avec le dossier injecté.",
        technical_checkpoint_state="NONE",
        relevant_addresses=addresses[:2], actor="test",
    )
    return store


def _recap(contract_hash: str = RESUME_HASH) -> dict[str, str]:
    return {
        "working_rules": "Le playbook injecté impose le correct ou arrêt bruyant, la preuve avant affirmation et la méthode mesurer puis vérifier.",
        "current_state": "Le handoff injecté fixe le Front, l’état actif, les résultats vérifiés, les risques et le prochain incrément.",
        "capabilities": "Les outils MCP, oracles, assets et pipelines fermés sont disponibles ; un dry run précède toute action générative, réseau ou sensible.",
        "git_state": "La branche active, les derniers commits et l’état de l’arbre ont été examinés dans le contexte de reprise automatiquement injecté.",
        "risks_and_limits": "Une recherche ne prouve rien, aucun SQL ou shell arbitraire n’est permis, auto_push reste faux et le document 91 est non applicable.",
        "next_action": "Je vais confirmer le point de départ avec le Front et exécuter ensuite une action ciblée conforme aux limites et priorités injectées.",
        "resume_contract_hash": contract_hash,
    }


def test_resume_context_requires_a_complete_sqlite_dossier_without_document_reread(tmp_path: Path) -> None:
    context = _populated_store(tmp_path / "memory").get_resume_context()
    dossier = context["resume_dossier"]

    assert dossier["ready"] is True
    assert dossier["playbook"]["domains"] == [
        "PLAYBOOK_FOUNDATION", "PLAYBOOK_METHOD", "PLAYBOOK_ARCHITECTURE", "PLAYBOOK_GATES", "PLAYBOOK_TOOLING",
    ]
    assert dossier["handoff"]["next_action"].startswith("Produire le récapitulatif")
    assert "document source ne doit être relu" in context["notice"]
    assert "rules" not in context


def test_resume_guard_blocks_until_a_complete_ritual_recap_is_acknowledged(tmp_path: Path) -> None:
    memory_dir = tmp_path / "memory"
    session = {"session_id": "test-resume"}
    armed = arm(memory_dir, session, reason="SessionStart", resume_contract_hash=RESUME_HASH)
    assert armed["status"] == "awaiting_recap"
    touch_mcp_ready(memory_dir)  # serveur MCP joignable ⇒ la porte de sortie existe ⇒ hard-block

    blocked = decision(memory_dir, {**session, "tool_name": "Bash", "tool_input": {"command": "true"}})
    assert blocked is not None
    assert blocked["hookSpecificOutput"]["permissionDecision"] == "deny"
    assert "ne relisez pas les documents source" in blocked["hookSpecificOutput"]["additionalContext"]
    assert RESUME_HASH in blocked["hookSpecificOutput"]["additionalContext"]
    assert decision(memory_dir, {**session, "tool_name": "mcp__aret-memory__aret_acknowledge_resume", "tool_input": _recap()}) is None

    rejected = acknowledge(memory_dir, {
        **session,
        "tool_name": "mcp__aret-memory__aret_acknowledge_resume",
        "tool_input": _recap("b" * 64),
        "tool_response": {"structuredContent": {"ok": True}},
    })
    assert rejected is not None
    assert rejected["acknowledged_at"] is None

    completed = acknowledge(memory_dir, {
        **session,
        "tool_name": "mcp__aret-memory__aret_acknowledge_resume",
        "tool_input": _recap(),
        "tool_response": {"structuredContent": {"ok": True}},
    })
    assert completed is not None
    assert completed["acknowledged_at"] is not None
    assert completed["recap"]["next_action"] == _recap()["next_action"]
    assert decision(memory_dir, {**session, "tool_name": "Edit", "tool_input": {}}) is None


def test_resume_guard_keeps_barrier_for_an_incomplete_recap_and_stop_is_single_shot(tmp_path: Path) -> None:
    memory_dir = tmp_path / "memory"
    session = {"session_id": "test-stop"}
    arm(memory_dir, session, reason="PostCompact", resume_contract_hash=RESUME_HASH)
    incomplete = {**_recap(), "next_action": "trop court"}
    try:
        validate_recap(incomplete)
    except ValueError as exc:
        assert "prochaine action proposée" in str(exc)
    else:
        raise AssertionError("Le récapitulatif incomplet doit être refusé")

    feedback = stop_feedback(memory_dir, session)
    assert feedback is not None
    assert "ne relisez pas les documents source" in feedback["hookSpecificOutput"]["additionalContext"]
    assert stop_feedback(memory_dir, {**session, "stop_hook_active": True}) is None


def test_resume_guard_scopes_distinct_sessions_and_fallback_identities(tmp_path: Path) -> None:
    memory_dir = tmp_path / "memory"
    sessions = (
        {"session_id": "session-a"},
        {"session_id": "session-b"},
        {"cwd": "/workspace/a"},
        {"cwd": "/workspace/b"},
    )
    states = [arm(memory_dir, session, reason="SessionStart", resume_contract_hash=RESUME_HASH) for session in sessions]
    assert all(state["status"] == "awaiting_recap" for state in states)
    touch_mcp_ready(memory_dir)  # serveur joignable ⇒ hard-block effectif

    # Les quatre évènements utilisent des clés distinctes : aucun acquittement ne peut traverser leur scope.
    state_files = {state_path(memory_dir, session) for session in sessions}
    assert len(state_files) == len(sessions)
    assert all(decision(memory_dir, {**session, "tool_name": "Bash"}) is not None for session in sessions)


def test_degraded_barrier_is_soft_and_never_hard_blocks(tmp_path: Path) -> None:
    """Reproduction du deadlock : mémoire DÉGRADÉE (non prête) armée en mode soft.

    La barrière est bien ARMÉE (fail-loud : état présent + nudge Stop actif), mais
    PreToolUse ne doit JAMAIS refuser une action — sinon, sans acquittement MCP
    atteignable, la session est verrouillée (vécu en live)."""
    memory_dir = tmp_path / "memory"
    session = {"session_id": "test-degraded"}
    armed = arm(memory_dir, session, reason="SessionStart", resume_contract_hash=RESUME_HASH, ready=False)
    assert armed["mode"] == "soft"
    assert armed["status"] == "awaiting_recap"  # fail-loud : la barrière EST armée

    # Aucune action n'est bloquée en mode dégradé, y compris sans acquittement.
    assert decision(memory_dir, {**session, "tool_name": "Bash", "tool_input": {"command": "true"}}) is None
    assert decision(memory_dir, {**session, "tool_name": "Edit", "tool_input": {}}) is None

    # Mais le nudge Stop reste actif une passe : on avertit fort sans jamais verrouiller.
    feedback = stop_feedback(memory_dir, session)
    assert feedback is not None
    assert "ne relisez pas les documents source" in feedback["hookSpecificOutput"]["additionalContext"]
    assert stop_feedback(memory_dir, {**session, "stop_hook_active": True}) is None


def test_ready_barrier_is_hard_and_blocks_until_acknowledged(tmp_path: Path) -> None:
    """Dossier prêt ⇒ mode dur : le blocage rituel reste imposé (acquittement possible)."""
    memory_dir = tmp_path / "memory"
    session = {"session_id": "test-ready"}
    armed = arm(memory_dir, session, reason="SessionStart", resume_contract_hash=RESUME_HASH, ready=True)
    assert armed["mode"] == "hard"
    touch_mcp_ready(memory_dir)  # serveur joignable ⇒ hard-block effectif

    blocked = decision(memory_dir, {**session, "tool_name": "Bash", "tool_input": {"command": "true"}})
    assert blocked is not None
    assert blocked["hookSpecificOutput"]["permissionDecision"] == "deny"

    acknowledge(memory_dir, {
        **session,
        "tool_name": "mcp__aret-memory__aret_acknowledge_resume",
        "tool_input": _recap(),
        "tool_response": {"structuredContent": {"ok": True}},
    })
    assert decision(memory_dir, {**session, "tool_name": "Edit", "tool_input": {}}) is None


def test_env_kill_switch_releases_even_a_hard_barrier(tmp_path, monkeypatch) -> None:
    """Voie de sortie d'exploitation toujours disponible : `ARET_MMU_BARRIER_OFF`.

    Même en mode dur non acquitté (ex. serveur MCP non connecté), l'opérateur peut
    lever PreToolUse et Stop sans éditer de code."""
    memory_dir = tmp_path / "memory"
    session = {"session_id": "test-killswitch"}
    arm(memory_dir, session, reason="SessionStart", resume_contract_hash=RESUME_HASH, ready=True)
    touch_mcp_ready(memory_dir)  # serveur joignable ⇒ hard-block (que le kill-switch doit lever)

    monkeypatch.delenv("ARET_MMU_BARRIER_OFF", raising=False)
    assert decision(memory_dir, {**session, "tool_name": "Bash"}) is not None  # dur : bloque

    monkeypatch.setenv("ARET_MMU_BARRIER_OFF", "1")
    assert decision(memory_dir, {**session, "tool_name": "Bash"}) is None       # levé
    assert stop_feedback(memory_dir, session) is None

    monkeypatch.setenv("ARET_MMU_BARRIER_OFF", "0")
    assert decision(memory_dir, {**session, "tool_name": "Bash"}) is not None    # rétabli


def test_resume_status_reports_ready_dossier(tmp_path: Path) -> None:
    """Friction #6 : verdict compact de reprise en lecture seule, sans rejouer le hook."""
    store = _populated_store(tmp_path / "memory")
    status = store.resume_status()
    assert status["ready"] is True
    assert status["degraded"] is False
    assert status["missing"] == []
    assert status["front_brick"] == "RESUME-01"


def test_resume_status_missing_names_the_repair_tool(tmp_path: Path) -> None:
    store = MemoryStore(tmp_path / "memory", write_enabled=True)
    store.register_component("CORE", "Core", "", "test")
    store.register_brick("SEED-01", "Brique", "ACTIVE", "CORE", "", "test", "M1", "x86-pe32", 1)
    store.update_front({"brick": "SEED-01"}, "test")
    status = store.resume_status()
    assert status["degraded"] is True
    assert status["missing"], "un dossier sans handoff doit lister des manques"
    assert any(item["fix_with"].startswith("aret_prepare_handoff") for item in status["missing"])


def test_resume_status_flags_bootstrap_provenance(tmp_path: Path) -> None:
    """Friction #7 : un Front dont la brique active a été semée par un bootstrap est signalé."""
    store = _populated_store(tmp_path / "memory")
    store.register_brick("BOOT-01", "Semée", "ACTIVE", "CORE", "", "aret-mmu-graph-bootstrap", "M1", "x86-pe32", 1)
    store.update_front({"brick": "BOOT-01"}, "test")
    warnings = store.resume_status()["warnings"]
    assert any("PROVENANCE" in warning and "BOOT-01" in warning for warning in warnings)


def test_resume_source_preserves_acknowledgement_without_a_new_ritual(tmp_path: Path) -> None:
    """Friction #10 : SessionStart se re-déclenche (source=resume) à chaque tour web.

    Une reprise `resume` d'une session DÉJÀ acquittée ne doit PAS re-bloquer l'agent
    vivant. Seule une vraie perte de contexte (PostCompact/clear/startup) re-force le rituel."""
    memory_dir = tmp_path / "memory"
    session = {"session_id": "test-resume-preserve"}
    arm(memory_dir, session, reason="startup", resume_contract_hash=RESUME_HASH)
    touch_mcp_ready(memory_dir)  # serveur joignable tout le long (PostCompact final doit hard-bloquer)
    acknowledge(memory_dir, {
        **session,
        "tool_name": "mcp__aret-memory__aret_acknowledge_resume",
        "tool_input": _recap(),
        "tool_response": {"structuredContent": {"ok": True}},
    })
    assert decision(memory_dir, {**session, "tool_name": "Bash"}) is None

    # Tour suivant : SessionStart re-arme en source=resume, avec une empreinte MÊME différente
    # (le Front a pu muter). L'acquittement doit être PRÉSERVÉ, pas réinitialisé.
    preserved = arm(memory_dir, session, reason="resume", resume_contract_hash="c" * 64)
    assert preserved["status"] == "acknowledged"
    assert preserved["acknowledged_at"] is not None
    assert preserved["resume_contract_hash"] == "c" * 64
    assert decision(memory_dir, {**session, "tool_name": "Bash"}) is None  # PAS re-bloqué

    # Mais une vraie compaction re-force le rituel (acknowledged_at remis à None).
    rearmed = arm(memory_dir, session, reason="PostCompact", resume_contract_hash="c" * 64)
    assert rearmed["status"] == "awaiting_recap"
    assert rearmed["acknowledged_at"] is None
    assert decision(memory_dir, {**session, "tool_name": "Bash"}) is not None


def test_resume_source_does_not_preserve_when_never_acknowledged(tmp_path: Path) -> None:
    """`resume` ne doit rien préserver si le rituel n'a jamais été acquitté : blocage maintenu."""
    memory_dir = tmp_path / "memory"
    session = {"session_id": "test-resume-fresh"}
    state = arm(memory_dir, session, reason="resume", resume_contract_hash=RESUME_HASH)
    assert state["status"] == "awaiting_recap"
    touch_mcp_ready(memory_dir)  # serveur joignable ⇒ hard-block effectif
    assert decision(memory_dir, {**session, "tool_name": "Bash"}) is not None


def test_resume_guard_without_any_identity_is_never_acknowledged_or_released(tmp_path: Path) -> None:
    memory_dir = tmp_path / "memory"
    unscoped: dict[str, str] = {}
    arm(memory_dir, unscoped, reason="SessionStart", resume_contract_hash=RESUME_HASH)
    touch_mcp_ready(memory_dir)  # serveur joignable ⇒ le fail-closed sans identité s'applique

    completed = acknowledge(memory_dir, {
        "tool_name": "mcp__aret-memory__aret_acknowledge_resume",
        "tool_input": _recap(),
        "tool_response": {"structuredContent": {"ok": True}},
    })
    assert completed is not None
    assert completed["acknowledged_at"] is None
    assert completed["status"] == "awaiting_recap"

    blocked = decision(memory_dir, {"tool_name": "Bash", "tool_input": {}})
    assert blocked is not None
    assert blocked["hookSpecificOutput"]["permissionDecision"] == "deny"
    assert "identité de session absente" in blocked["hookSpecificOutput"]["permissionDecisionReason"]


def test_barrier_is_soft_when_the_mcp_channel_is_unreachable(tmp_path: Path) -> None:
    """Correctif n°1 (anti-deadlock) : la barrière ne hard-bloque JAMAIS tant que la
    porte de sortie (serveur MCP -> aret_acknowledge_resume) n'est pas prouvée vivante.
    C'est le coeur du deadlock vécu : dossier prêt (mode dur) + serveur non connecté."""
    memory_dir = tmp_path / "memory"
    session = {"session_id": "test-unreachable"}
    armed = arm(memory_dir, session, reason="SessionStart", resume_contract_hash=RESUME_HASH, ready=True)
    assert armed["mode"] == "hard"

    # Aucun marqueur de vivacité (serveur jamais connecté) => PAS de blocage dur.
    assert decision(memory_dir, {**session, "tool_name": "Bash"}) is None

    # Marqueur PÉRIMÉ (serveur mort en cours de route) => toujours pas de blocage dur.
    touch_mcp_ready(memory_dir)
    marker = mcp_ready_marker(memory_dir)
    old = time.time() - (MCP_LIVENESS_MAX_AGE_S + 30)
    os.utime(marker, (old, old))
    assert decision(memory_dir, {**session, "tool_name": "Bash"}) is None

    # Marqueur FRAIS (serveur vivant) => la barrière hard-bloque comme prévu.
    touch_mcp_ready(memory_dir)
    assert decision(memory_dir, {**session, "tool_name": "Bash"}) is not None


def test_toolsearch_schema_load_is_never_blocked(tmp_path: Path) -> None:
    """Correctif n°3 : charger le schéma d'un outil différé (ToolSearch) — dont la
    porte de sortie elle-même — ne doit jamais être bloqué, sinon chicken-and-egg."""
    memory_dir = tmp_path / "memory"
    session = {"session_id": "test-toolsearch"}
    arm(memory_dir, session, reason="SessionStart", resume_contract_hash=RESUME_HASH, ready=True)
    touch_mcp_ready(memory_dir)
    # Un vrai appel d'outil reste bloqué…
    assert decision(memory_dir, {**session, "tool_name": "Bash"}) is not None
    # …mais ToolSearch (chargement de schéma) passe toujours.
    assert decision(memory_dir, {**session, "tool_name": "ToolSearch"}) is None


def test_build_attempt_diagnostic_detects_a_collapsed_call() -> None:
    """Un appel fusionné (un seul champ reçu) est constaté, pas deviné."""
    collapsed = build_attempt_diagnostic({"working_rules": "x" * 1800})
    assert collapsed is not None
    assert collapsed["collapsed"] is True
    assert collapsed["recap_fields_received"] == 1
    assert "current_state" in collapsed["missing_fields"]
    assert "resume_contract_hash" in collapsed["missing_fields"]

    whole = build_attempt_diagnostic({**_recap()})
    assert whole is not None
    assert whole["collapsed"] is False
    assert whole["recap_fields_received"] == len(("wr", "cs", "cap", "git", "risk", "next"))  # 6
    assert whole["missing_fields"] == []


def test_block_message_surfaces_the_observed_collapse(tmp_path: Path) -> None:
    """Le message de blocage AFFICHE ce que le hook a réellement reçu de l'acquittement.

    Objectif : rendre le blocage auto-explicatif avec des données mesurées (non devinées),
    sans jamais lever la barrière — pour déboguer la cause exacte au moment où elle survient."""
    memory_dir = tmp_path / "memory"
    session = {"session_id": "test-diag"}
    arm(memory_dir, session, reason="SessionStart", resume_contract_hash=RESUME_HASH, ready=True)
    touch_mcp_ready(memory_dir)  # canal MCP prouvé vivant

    # L'agent tente un acquittement FUSIONNÉ : seul working_rules arrive (tout collé dedans).
    collapsed_input = {"working_rules": "x" * 1800}
    assert decision(memory_dir, {
        **session, "tool_name": "mcp__aret-memory__aret_acknowledge_resume", "tool_input": collapsed_input,
    }) is None

    # Le blocage SUIVANT expose le collapse observé + l'état système, en données mesurées.
    blocked = decision(memory_dir, {**session, "tool_name": "Bash", "tool_input": {"command": "true"}})
    assert blocked is not None
    context = blocked["hookSpecificOutput"]["additionalContext"]
    assert "COLLAPSE CONFIRMÉ" in context
    assert "working_rules(1800c)" in context          # longueur réellement reçue
    assert "current_state" in context                  # listé comme manquant
    assert "l'acquittement EST atteignable" in context  # canal MCP vivant, donc cause = contenu
    # La barrière n'est PAS levée pour autant : elle reste dure.
    assert blocked["hookSpecificOutput"]["permissionDecision"] == "deny"


def test_block_message_surfaces_a_wellformed_but_unaccepted_attempt(tmp_path: Path) -> None:
    """Champs bien arrivés mais barrière tenue ⇒ le message oriente vers le hash / les minimums."""
    memory_dir = tmp_path / "memory"
    session = {"session_id": "test-diag-ok"}
    arm(memory_dir, session, reason="SessionStart", resume_contract_hash=RESUME_HASH, ready=True)
    touch_mcp_ready(memory_dir)
    assert decision(memory_dir, {
        **session, "tool_name": "mcp__aret-memory__aret_acknowledge_resume", "tool_input": _recap("d" * 64),
    }) is None
    blocked = decision(memory_dir, {**session, "tool_name": "Bash", "tool_input": {"command": "true"}})
    context = blocked["hookSpecificOutput"]["additionalContext"]
    assert "Les champs sont bien arrivés" in context
    assert "resume_contract_hash correspond EXACTEMENT" in context


def test_file_sentinel_kill_switch_releases_a_hard_barrier(tmp_path: Path) -> None:
    """Correctif n°2 : voie de sortie atteignable EN COURS de session — un fichier
    sentinelle runtime/BARRIER_OFF (l'env var, elle, est figée au démarrage du hook)."""
    memory_dir = tmp_path / "memory"
    session = {"session_id": "test-sentinel"}
    arm(memory_dir, session, reason="SessionStart", resume_contract_hash=RESUME_HASH, ready=True)
    touch_mcp_ready(memory_dir)
    assert decision(memory_dir, {**session, "tool_name": "Bash"}) is not None  # dur

    sentinel = barrier_off_sentinel(memory_dir)
    sentinel.parent.mkdir(parents=True, exist_ok=True)
    sentinel.write_text("off\n", encoding="utf-8")
    assert decision(memory_dir, {**session, "tool_name": "Bash"}) is None  # levé (PreToolUse)
    assert stop_feedback(memory_dir, session) is None                      # levé (Stop)

    sentinel.unlink()
    assert decision(memory_dir, {**session, "tool_name": "Bash"}) is not None  # rétabli


def test_decollapse_reconstructs_collapsed_fields_and_is_idempotent() -> None:
    """Le bug de serialisation du client fusionne les champs dans le premier ; on les reconstruit."""
    from hooks.resume_guard import decollapse
    close = "</" + "parameter>"
    op = "<" + "parameter name="
    blob = ("WR" + close + "\n" + op + '"current_state">CS' + close + "\n"
            + op + '"resume_contract_hash">' + "a" * 64 + close + "\n" + "</" + "invoke>")
    got = decollapse({"working_rules": blob, "current_state": "", "resume_contract_hash": ""})
    assert got["working_rules"] == "WR"
    assert got["current_state"] == "CS"
    assert got["resume_contract_hash"] == "a" * 64
    clean = {"working_rules": "x", "current_state": "y"}
    assert decollapse(clean) == clean          # idempotent sur un appel non fusionne
    assert decollapse("not a dict") == "not a dict"


def test_readonly_tools_pass_the_barrier_but_actions_stay_blocked(tmp_path: Path) -> None:
    """Carve-out lecture : investiguer pendant la barriere est permis ; les actions restent bloquees."""
    memory_dir = tmp_path / "memory"
    session = {"session_id": "ro"}
    arm(memory_dir, session, reason="startup", resume_contract_hash=RESUME_HASH, ready=True)
    touch_mcp_ready(memory_dir)
    for ro in ("Read", "Grep", "Glob", "LS", "mcp__aret-memory__aret_get_front",
               "mcp__aret-memory__aret_find", "mcp__aret-memory__aret_read_artifact"):
        assert decision(memory_dir, {**session, "tool_name": ro, "tool_input": {}}) is None, ro
    blocked = decision(memory_dir, {**session, "tool_name": "Bash", "tool_input": {"command": "true"}})
    assert blocked is not None and blocked["hookSpecificOutput"]["permissionDecision"] == "deny"
    wr = decision(memory_dir, {**session, "tool_name": "mcp__aret-memory__aret_append_knowledge", "tool_input": {}})
    assert wr is not None and wr["hookSpecificOutput"]["permissionDecision"] == "deny"
