"""Suivi durable des exécutions d'oracles asynchrones (friction timeout MCP 60s).

L'async est nécessaire parce que winediff/difftest/cpudiff dépassent le timeout transport
MCP de 60s : le serveur lance l'oracle en fond et enregistre ICI un suivi RUNNING→DONE/ERROR
que aret_get_oracle_run interroge. Ces tests couvrent le mécanisme durable (table oracle_run) ;
le lancement du thread côté serveur est une fine glue au-dessus."""
from __future__ import annotations

import pytest

from core.repository import AretError, MemoryStore


def store(tmp_path) -> MemoryStore:
    return MemoryStore(tmp_path / ".aret-memory", write_enabled=True)


def test_migration_creates_oracle_run_table(tmp_path) -> None:
    s = store(tmp_path)
    with s._read_connection() as conn:
        row = conn.execute(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='oracle_run'"
        ).fetchone()
    assert row is not None


def test_start_then_get_is_running(tmp_path) -> None:
    s = store(tmp_path)
    run = s.start_oracle_run(oracle="winediff", knowledge_id="KN-0001", promote=True, actor="t")
    assert run["status"] == "RUNNING"
    assert run["id"].startswith("OR-")
    got = s.get_oracle_run(run["id"])
    assert got["status"] == "RUNNING"
    assert got["oracle"] == "winediff"
    assert got["knowledge_id"] == "KN-0001"
    assert got["promote"] is True
    assert got["proof_id"] is None
    assert got["finished_at"] is None


def test_complete_done_records_result_and_proof(tmp_path) -> None:
    s = store(tmp_path)
    run = s.start_oracle_run(oracle="difftest", knowledge_id=None, promote=False, actor="t")
    s.complete_oracle_run(run["id"], status="DONE", result="PASS", proof_id="P-0042", exit_code=0, actor="t")
    got = s.get_oracle_run(run["id"])
    assert got["status"] == "DONE"
    assert got["result"] == "PASS"
    assert got["proof_id"] == "P-0042"
    assert got["exit_code"] == 0
    assert got["finished_at"] is not None


def test_complete_error_records_message(tmp_path) -> None:
    s = store(tmp_path)
    run = s.start_oracle_run(oracle="cpudiff", knowledge_id=None, promote=False, actor="t")
    s.complete_oracle_run(run["id"], status="ERROR", error="boom" * 1000, actor="t")
    got = s.get_oracle_run(run["id"])
    assert got["status"] == "ERROR"
    assert got["error"] is not None and len(got["error"]) <= 2000  # tronqué


def test_complete_is_idempotent_no_overwrite(tmp_path) -> None:
    s = store(tmp_path)
    run = s.start_oracle_run(oracle="funcdiff", knowledge_id=None, promote=False, actor="t")
    s.complete_oracle_run(run["id"], status="DONE", result="PASS", proof_id="P-0001", actor="t")
    # Une seconde clôture (ex. course entre threads) ne réécrit PAS un run déjà clos.
    s.complete_oracle_run(run["id"], status="ERROR", error="late", actor="t")
    got = s.get_oracle_run(run["id"])
    assert got["status"] == "DONE"
    assert got["result"] == "PASS"
    assert got["error"] is None


def test_get_unknown_run_raises(tmp_path) -> None:
    s = store(tmp_path)
    with pytest.raises(AretError):
        s.get_oracle_run("OR-9999")


def test_get_invalid_id_raises(tmp_path) -> None:
    s = store(tmp_path)
    with pytest.raises(AretError):
        s.get_oracle_run("not-an-id")


def test_start_invalid_oracle_name_raises(tmp_path) -> None:
    s = store(tmp_path)
    with pytest.raises(AretError):
        s.start_oracle_run(oracle="BAD NAME!", knowledge_id=None, promote=False, actor="t")


def test_complete_unknown_run_raises(tmp_path) -> None:
    s = store(tmp_path)
    with pytest.raises(AretError):
        s.complete_oracle_run("OR-0001", status="DONE", result="PASS", actor="t")


def test_complete_rejects_bad_status(tmp_path) -> None:
    s = store(tmp_path)
    run = s.start_oracle_run(oracle="winediff", knowledge_id=None, promote=False, actor="t")
    with pytest.raises(AretError):
        s.complete_oracle_run(run["id"], status="RUNNING", actor="t")
