"""Garde déterministe de reprise ARET-MMU.

Le contexte de reprise est injecté depuis SQLite au démarrage et après compaction.
Le garde n'impose pas une relecture des documents ingérés : il exige uniquement
un récapitulatif structuré de ce contexte avant toute action de poursuite.
Son état est local et éphémère, sous `.aret-memory/runtime/`, jamais dans
SQLite ni Git.
"""

from __future__ import annotations

import hashlib
import json
import os
import sys
import time
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))


RITUAL_FIELDS: tuple[tuple[str, str, int], ...] = (
    ("working_rules", "règles de travail incontournables", 80),
    ("current_state", "état courant, Front et objectifs", 60),
    ("capabilities", "outils MCP, analyse, industrialisation et pipelines", 80),
    ("git_state", "branche, commits et état Git", 40),
    ("risks_and_limits", "limites, preuves et garde-fous", 60),
    ("next_action", "prochaine action proposée", 30),
)


_TRUTHY = {"1", "true", "yes", "on"}

# Fenêtre de fraîcheur du marqueur de vivacité du serveur MCP. Au-delà, on considère
# le canal d'acquittement (aret_acknowledge_resume) injoignable et la barrière NE
# hard-bloque PAS — anti-deadlock. Généreux pour éviter un faux « serveur mort »
# sous charge ; le serveur rafraîchit le marqueur par heartbeat (~20 s).
MCP_LIVENESS_MAX_AGE_S = 90.0


def _runtime_root(memory_dir: Path) -> Path:
    return memory_dir / "runtime"


def barrier_off_sentinel(memory_dir: Path | None) -> Path | None:
    """Fichier sentinelle du kill-switch, à la RACINE de runtime/ pour être trouvable."""
    if memory_dir is None:
        return None
    return _runtime_root(memory_dir) / "BARRIER_OFF"


def barrier_disabled(memory_dir: Path | None = None) -> bool:
    """Kill-switch d'exploitation : DEUX voies de sortie, toujours disponibles.

    Une barrière ne doit jamais pouvoir s'armer sans issue. Même en mode dur, si
    l'acquittement MCP est inatteignable (serveur non connecté), on doit pouvoir
    lever la garde sans éditer de code :
      1. `ARET_MMU_BARRIER_OFF=1` — mais l'env d'un hook est FIGÉ au démarrage de la
         session : inatteignable en cours de route (leçon du deadlock vécu).
      2. `runtime/BARRIER_OFF` — un fichier que l'opérateur OU l'agent peut créer
         À TOUT MOMENT ; c'est la vraie voie de sortie en cours de session.
    Vaut pour PreToolUse comme pour Stop.
    """
    if os.environ.get("ARET_MMU_BARRIER_OFF", "").strip().lower() in _TRUTHY:
        return True
    sentinel = barrier_off_sentinel(memory_dir)
    if sentinel is not None:
        try:
            if sentinel.exists():
                return True
        except OSError:
            pass
    return False


def mcp_ready_marker(memory_dir: Path) -> Path:
    return _runtime_root(memory_dir) / "mcp_ready"


def touch_mcp_ready(memory_dir: Path) -> None:
    """Écrit/rafraîchit le marqueur de vivacité — appelé par le serveur MCP (au
    démarrage puis en heartbeat). Sa présence FRAÎCHE prouve que la porte de sortie
    (aret_acknowledge_resume) est réellement atteignable."""
    root = _runtime_root(memory_dir)
    root.mkdir(parents=True, exist_ok=True)
    tmp = root / "mcp_ready.tmp"
    tmp.write_text(utc_now() + "\n", encoding="utf-8")
    tmp.replace(root / "mcp_ready")


def mcp_channel_alive(memory_dir: Path, now: float | None = None) -> bool:
    """Le canal d'acquittement (serveur MCP) est-il PROUVÉ vivant ?

    Marqueur `runtime/mcp_ready` absent ou périmé (> MCP_LIVENESS_MAX_AGE_S) ⇒ on ne
    peut PAS garantir que aret_acknowledge_resume est atteignable ⇒ la barrière ne
    doit pas hard-bloquer (sinon deadlock, exactement le cas vécu)."""
    marker = mcp_ready_marker(memory_dir)
    try:
        age = (time.time() if now is None else now) - marker.stat().st_mtime
    except OSError:
        return False
    return age <= MCP_LIVENESS_MAX_AGE_S


def _is_schema_load_tool(payload: dict[str, Any]) -> bool:
    """ToolSearch charge le SCHÉMA d'un outil différé — dont la porte de sortie
    (aret_acknowledge_resume) elle-même quand les ~44 outils MCP sont « deferred ».
    Le bloquer rendait l'acquittement impossible (chicken-and-egg vécu). Charger un
    schéma n'est pas agir : les VRAIS appels d'outils restent refusés."""
    return str(payload.get("tool_name", "")) == "ToolSearch"


def utc_now() -> str:
    return datetime.now(UTC).isoformat(timespec="seconds").replace("+00:00", "Z")


def session_identity(payload: dict[str, Any]) -> str | None:
    """Retourne une identité de session explicite, ou None lorsqu’aucun scope n’est fourni."""
    for field in ("session_id", "transcript_path", "cwd"):
        value = str(payload.get(field) or "").strip()
        if value:
            return f"{field}:{value}"
    return None


def session_key(payload: dict[str, Any]) -> str:
    raw = session_identity(payload) or "unscoped"
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()[:24]


def runtime_dir(memory_dir: Path) -> Path:
    path = memory_dir / "runtime" / "resume_guard"
    path.mkdir(parents=True, exist_ok=True)
    return path


def state_path(memory_dir: Path, payload: dict[str, Any]) -> Path:
    return runtime_dir(memory_dir) / f"{session_key(payload)}.json"


def load_state(memory_dir: Path, payload: dict[str, Any]) -> dict[str, Any] | None:
    path = state_path(memory_dir, payload)
    if not path.is_file():
        return None
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(value, dict) or value.get("version") != 3:
        return None
    return value


def _write_state(memory_dir: Path, payload: dict[str, Any], state: dict[str, Any]) -> None:
    path = state_path(memory_dir, payload)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(state, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")
    temporary.replace(path)


def arm(memory_dir: Path, payload: dict[str, Any], reason: str, resume_contract_hash: str, ready: bool = True) -> dict[str, Any]:
    """Arme une confirmation liée à l’empreinte du dossier réellement injecté.

    `ready` distingue deux modes, et c'est la leçon du deadlock vécu :
      - mode "hard" (dossier prêt) : le PreToolUse refuse toute action jusqu'à
        l'acquittement rituel — l'acquittement est alors sémantiquement possible.
      - mode "soft" (dossier DÉGRADÉ / non prêt) : la barrière reste ARMÉE et le
        contexte bruyant est injecté (fail-loud préservé), mais le PreToolUse NE
        BLOQUE PAS dur. Sur une mémoire cassée, imposer un rituel rigide sans
        voie de sortie fiable = deadlock. On avertit fort, on ne verrouille pas.
    """
    if len(resume_contract_hash) != 64 or any(char not in "0123456789abcdef" for char in resume_contract_hash):
        raise ValueError("Empreinte Resume Dossier invalide : hash SHA-256 hexadécimal requis")
    # Reprise "resume" d'une session DÉJÀ acquittée : ce n'est PAS une perte de
    # contexte. En session web/async, SessionStart se re-déclenche (source=resume)
    # à CHAQUE tour ; ré-armer remettrait acknowledged_at=None et re-bloquerait
    # l'agent vivant à chaque échange. On préserve donc l'acquittement (on rafraîchit
    # seulement l'empreinte/le mode). Seule une vraie perte de contexte —
    # PostCompact, clear, startup, ou un armement explicite — re-force le rituel.
    if reason == "resume":
        existing = load_state(memory_dir, payload)
        if isinstance(existing, dict) and existing.get("status") == "acknowledged":
            existing["resume_contract_hash"] = resume_contract_hash
            existing["reason"] = reason
            existing["mode"] = "hard" if ready else "soft"
            _write_state(memory_dir, payload, existing)
            return existing
    armed_at = utc_now()
    state = {
        "version": 3,
        "armed_at": armed_at,
        "reason": reason,
        "status": "awaiting_recap",
        "mode": "hard" if ready else "soft",
        "required_fields": [field for field, _, _ in RITUAL_FIELDS],
        "resume_contract_hash": resume_contract_hash,
        "acknowledged_at": None,
        "recap": None,
    }
    _write_state(memory_dir, payload, state)
    return state


def recap_from_input(payload: dict[str, Any]) -> dict[str, Any] | None:
    tool_input = payload.get("tool_input")
    if not isinstance(tool_input, dict):
        return None
    return tool_input


def validate_recap(recap: dict[str, Any]) -> dict[str, str]:
    """Valide une attestation de reprise sans prétendre juger sa sémantique.

    La validité factuelle du récapitulatif relève de l'agent ; le contrôle
    déterministe garantit les six volets du rituel et des contenus non triviaux.
    """
    normalized: dict[str, str] = {}
    failures: list[str] = []
    for field, label, minimum in RITUAL_FIELDS:
        value = recap.get(field)
        text = value.strip() if isinstance(value, str) else ""
        if len(text) < minimum:
            failures.append(f"{label} ({minimum} caractères minimum)")
        else:
            normalized[field] = text[:4000]
    if failures:
        raise ValueError("Récapitulatif de reprise incomplet : " + "; ".join(failures))
    return normalized


def is_resume_acknowledgement(payload: dict[str, Any]) -> bool:
    return str(payload.get("tool_name", "")).endswith("__aret_acknowledge_resume")


def _tool_succeeded(payload: dict[str, Any]) -> bool:
    response = payload.get("tool_response")
    if response is None:
        return True
    if isinstance(response, dict):
        if response.get("is_error") is True or response.get("isError") is True:
            return False
        structured = response.get("structured_content") or response.get("structuredContent")
        if isinstance(structured, dict) and structured.get("ok") is False:
            return False
    if isinstance(response, str) and '"ok":false' in response.replace(" ", "").lower():
        return False
    return True


def acknowledge(memory_dir: Path, payload: dict[str, Any]) -> dict[str, Any] | None:
    """Lève la garde seulement après une attestation MCP complète et réussie."""
    state = load_state(memory_dir, payload)
    if session_identity(payload) is None:
        return state
    if state is None or not is_resume_acknowledgement(payload) or not _tool_succeeded(payload):
        return state
    recap = recap_from_input(payload)
    if recap is None:
        return state
    try:
        normalized = validate_recap(recap)
    except ValueError:
        return state
    if recap.get("resume_contract_hash") != state.get("resume_contract_hash"):
        return state
    state["status"] = "acknowledged"
    state["acknowledged_at"] = utc_now()
    state["recap"] = normalized
    _write_state(memory_dir, payload, state)
    return state


def ritual_prompt(resume_contract_hash: str) -> str:
    fields = "; ".join(label for _, label, _ in RITUAL_FIELDS)
    return (
        "Le contexte de reprise a déjà été injecté depuis SQLite canonique : ne relisez pas les documents source. "
        "Avant toute action de poursuite, produisez un récapitulatif fidèle couvrant : " + fields + ". "
        "Puis appelez aret_acknowledge_resume avec les six champs correspondants et "
        f"resume_contract_hash={resume_contract_hash}. "
        "SOIS BREF : 1 phrase courte par champ, total < ~900 octets. Un recap trop long peut voir ses 6 champs "
        "FUSIONNER dans le premier au niveau de la sérialisation d'appel d'outil (le serveur ne reçoit alors qu'un "
        "seul champ et refuse) ; si l'appel échoue ainsi, RACCOURCIS le recap et réessaie — ne change pas le hash."
    )


def _recap_field_names() -> list[str]:
    return [field for field, _, _ in RITUAL_FIELDS]


def build_attempt_diagnostic(tool_input: Any) -> dict[str, Any] | None:
    """Photographie ce que le hook a RÉELLEMENT reçu d'un appel d'acquittement.

    C'est la donnée-clé de débogage : PreToolUse voit le `tool_input` AVANT le serveur.
    Si les 6 champs de recap ne sont pas tous présents, l'appel a fusionné en transit
    (limite de sérialisation d'appel d'outil du harness) — fait constaté, non deviné.
    """
    if not isinstance(tool_input, dict):
        return None
    lengths = {k: len(v) for k, v in tool_input.items() if isinstance(v, str)}
    total_bytes = sum(len(v.encode("utf-8")) for v in tool_input.values() if isinstance(v, str))
    recap_fields = _recap_field_names()
    present_recap = [field for field in recap_fields if field in tool_input]
    missing = [field for field in (recap_fields + ["resume_contract_hash"]) if field not in tool_input]
    return {
        "at": utc_now(),
        "present_lengths": lengths,
        "missing_fields": missing,
        "total_bytes": total_bytes,
        "recap_fields_received": len(present_recap),
        "collapsed": len(present_recap) < len(recap_fields),
    }


def _record_ack_attempt(memory_dir: Path, payload: dict[str, Any], state: dict[str, Any] | None) -> None:
    """Persiste la photographie de la dernière tentative d'acquittement pour l'afficher au prochain blocage."""
    if state is None:
        return
    diagnostic = build_attempt_diagnostic(payload.get("tool_input"))
    if diagnostic is None:
        return
    state["last_ack_attempt"] = diagnostic
    try:
        _write_state(memory_dir, payload, state)
    except OSError:
        pass


def attempt_diagnostic_text(state: dict[str, Any] | None) -> str:
    """Rend, en clair, ce que le système a observé de la dernière tentative d'acquittement.

    Toutes les valeurs proviennent du `tool_input` réellement reçu — aucune n'est devinée.
    """
    diagnostic = (state or {}).get("last_ack_attempt")
    if not isinstance(diagnostic, dict):
        return ""
    present = ", ".join(f"{k}({v}c)" for k, v in sorted(diagnostic.get("present_lengths", {}).items())) or "aucun"
    missing = ", ".join(diagnostic.get("missing_fields", [])) or "aucun"
    lines = [
        " DIAGNOSTIC — données OBSERVÉES par le hook sur ta dernière tentative d'acquittement (non devinées) :",
        f" - champs réellement reçus : {present}",
        f" - champs MANQUANTS à l'arrivée : {missing}",
        f" - total reçu : {diagnostic.get('total_bytes')} octets ; champs de recap reçus : {diagnostic.get('recap_fields_received')}/{len(RITUAL_FIELDS)}",
    ]
    if diagnostic.get("collapsed"):
        lines.append(
            " => COLLAPSE CONFIRMÉ : les champs ont fusionné dans le premier AVANT d'atteindre le serveur "
            "(limite de sérialisation d'appel d'outil du harness ; serveur + transport MCP hors de cause). "
            "PARADE : raccourcis fortement le recap (< ~900 octets, 1 phrase courte/champ) puis réémets."
        )
    else:
        lines.append(
            " => Les champs sont bien arrivés : si la barrière tient encore, vérifie que resume_contract_hash "
            "correspond EXACTEMENT au dossier injecté et que chaque champ atteint sa longueur minimale."
        )
    return "\n" + "\n".join(lines)


def system_facts_text(memory_dir: Path, payload: dict[str, Any], state: dict[str, Any] | None) -> str:
    """État système OBSERVÉ au moment du blocage (faits mesurés, aucune supposition)."""
    facts = [f" - outil bloqué : {payload.get('tool_name', '?')}"]
    marker = mcp_ready_marker(memory_dir)
    try:
        age = int(time.time() - marker.stat().st_mtime)
        facts.append(
            f" - canal MCP (aret_acknowledge_resume) : marqueur vivant à {age}s (seuil {int(MCP_LIVENESS_MAX_AGE_S)}s) "
            "=> l'acquittement EST atteignable ; un blocage persistant vient donc du CONTENU de ton appel, pas du serveur"
        )
    except OSError:
        facts.append(" - canal MCP : marqueur de vivacité ABSENT (serveur jamais connecté)")
    if isinstance(state, dict):
        facts.append(f" - barrière : reason={state.get('reason')} mode={state.get('mode')} armée={state.get('armed_at')}")
    return "\n ÉTAT SYSTÈME OBSERVÉ :\n" + "\n".join(facts)


def diagnostic_suffix(memory_dir: Path, payload: dict[str, Any], state: dict[str, Any] | None) -> str:
    """Tout le diagnostic non-deviné à joindre à un message de blocage."""
    return system_facts_text(memory_dir, payload, state) + attempt_diagnostic_text(state)


def decision(memory_dir: Path, payload: dict[str, Any]) -> dict[str, Any] | None:
    if barrier_disabled(memory_dir):
        return None
    state = load_state(memory_dir, payload)
    if state is None:
        return None
    if state.get("mode") == "soft":
        # Reprise DÉGRADÉE : le contexte bruyant a déjà été injecté à l'armement
        # (SessionStart/PostCompact) et le nudge Stop reste actif ; on n'impose
        # PAS de blocage dur sans voie de sortie fiable — c'est ce qui a deadlocké.
        return None
    if state.get("acknowledged_at"):
        return None
    if is_resume_acknowledgement(payload):
        # On laisse toujours passer l'acquittement, MAIS on photographie ce que le hook a
        # reçu : si les champs ont fusionné en transit, le prochain blocage l'affichera.
        _record_ack_attempt(memory_dir, payload, state)
        return None
    # Charger le schéma de la porte de sortie ne doit JAMAIS être bloqué par la
    # barrière elle-même (sinon on ne peut pas appeler aret_acknowledge_resume).
    if _is_schema_load_tool(payload):
        return None
    # SONDE DE DISPONIBILITÉ (correctif n°1) : ne hard-bloquer QUE si la porte de
    # sortie (serveur MCP → aret_acknowledge_resume) est PROUVÉE joignable. Serveur
    # non connecté ⇒ aucun blocage dur : la barrière ne s'arme jamais sans issue.
    if not mcp_channel_alive(memory_dir):
        return None
    if session_identity(payload) is None:
        return {
            "hookSpecificOutput": {
                "hookEventName": "PreToolUse",
                "permissionDecision": "deny",
                "permissionDecisionReason": "BARRIÈRE DE REPRISE ARET-MMU : identité de session absente ; reprise refusée fail-closed.",
            }
        }
    return {
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "deny",
            "permissionDecisionReason": "BARRIÈRE DE REPRISE ARET-MMU : le récapitulatif rituel doit être confirmé avant toute action de poursuite.",
            "additionalContext": ritual_prompt(str(state.get("resume_contract_hash", ""))) + diagnostic_suffix(memory_dir, payload, state),
        }
    }


def stop_feedback(memory_dir: Path, payload: dict[str, Any]) -> dict[str, Any] | None:
    """Force une unique continuation lorsque l'agent tente de conclure sans récapitulatif.

    Actif en mode dur ET en mode soft (dégradé) : le nudge est informatif et
    borné à une seule passe (stop_hook_active) — il avertit sans jamais bloquer.
    Le kill-switch d'exploitation le désarme aussi.
    """
    if barrier_disabled(memory_dir):
        return None
    state = load_state(memory_dir, payload)
    if state is None or state.get("acknowledged_at") or payload.get("stop_hook_active") is True:
        return None
    return {
        "hookSpecificOutput": {
            "hookEventName": "Stop",
            "additionalContext": "BARRIÈRE DE REPRISE ARET-MMU ACTIVE : ne concluez pas et ne poursuivez pas encore. " + ritual_prompt(str(state.get("resume_contract_hash", ""))) + diagnostic_suffix(memory_dir, payload, state),
        }
    }


def memory_dir_from_env() -> Path:
    configured = os.environ.get("ARET_MEMORY_DIR") or ".aret-memory"
    return Path(configured).expanduser().resolve()
