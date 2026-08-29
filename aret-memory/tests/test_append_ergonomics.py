"""Robustesse MCP Phase 2 : confort d'API sur append_knowledge (frictions vécues)."""
from __future__ import annotations

from pathlib import Path

import pytest

from core.repository import AretError, MemoryStore


def _store(tmp_path: Path) -> MemoryStore:
    return MemoryStore(tmp_path / "m", write_enabled=True)


def _kw() -> dict:
    return dict(component_id=None, function_id=None, brick_id=None, proof_ids=[], supersedes_id=None, actor="test")


def test_tags_accept_a_comma_or_space_string_and_do_not_iterate_characters(tmp_path: Path) -> None:
    rec = _store(tmp_path).append_knowledge(
        knowledge_type="OBSERVATION", status=None, title="t" * 10, content="c" * 40,
        tags="alpha, beta gamma", **_kw(),
    )
    assert rec["tags"] == ["ALPHA", "BETA", "GAMMA"]


def test_tags_list_is_still_accepted_unchanged(tmp_path: Path) -> None:
    rec = _store(tmp_path).append_knowledge(
        knowledge_type="OBSERVATION", status=None, title="t" * 10, content="c" * 40,
        tags=["x", "y"], **_kw(),
    )
    assert rec["tags"] == ["X", "Y"]


def test_invalid_initial_status_error_lists_the_allowed_values(tmp_path: Path) -> None:
    with pytest.raises(AretError) as excinfo:
        _store(tmp_path).append_knowledge(
            knowledge_type="OBSERVATION", status="MEASURED", title="t" * 10, content="c" * 40,
            tags=None, **_kw(),
        )
    message = str(excinfo.value)
    assert "Valeurs admises" in message
    assert "ACTIVE" in message and "PROVEN" in message
