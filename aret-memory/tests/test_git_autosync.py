from __future__ import annotations

import json
import subprocess
from pathlib import Path

from core.repository import MemoryStore
from ops.git_memory import current_branch, status, sync_memory_only


def git(repository: Path, *args: str) -> str:
    completed = subprocess.run(["git", "-C", str(repository), *args], check=True, text=True, capture_output=True)
    return completed.stdout.strip()


def make_repository(tmp_path: Path) -> Path:
    repository = tmp_path / "repo"
    repository.mkdir()
    git(repository, "init")
    git(repository, "config", "user.email", "aret@example.invalid")
    git(repository, "config", "user.name", "ARET Test")
    (repository / "README.md").write_text("base\n", encoding="utf-8")
    git(repository, "add", "README.md")
    git(repository, "commit", "-m", "base")
    return repository


def write_policy(memory_dir: Path, auto_commit: bool = True) -> None:
    (memory_dir / "sync_policy.json").write_text(
        json.dumps({"auto_commit": auto_commit, "auto_push": False, "remote": "origin", "branch": ""}) + "\n",
        encoding="utf-8",
    )


def test_autosync_commits_only_memory_store(tmp_path: Path) -> None:
    repository = make_repository(tmp_path)
    memory_dir = repository / "aret-memory" / ".aret-memory"
    store = MemoryStore(memory_dir, write_enabled=True)
    write_policy(memory_dir)

    store.register_component("CORE", "Core", "", "test")

    changed = git(repository, "show", "--name-only", "--format=")
    assert "aret-memory/.aret-memory/aret_memory.sqlite" in changed
    assert "README.md" not in changed
    assert store.last_sync_status["committed"] is True
    assert store.last_sync_status["pushed"] is False


def test_nested_memory_path_is_relativized_against_git_root(tmp_path: Path) -> None:
    repository = make_repository(tmp_path)
    memory_dir = repository / "aret-memory" / ".aret-memory"
    store = MemoryStore(memory_dir, write_enabled=True)
    (memory_dir / "note.txt").write_text("mémoire\n", encoding="utf-8")

    report = status(repository / "aret-memory", None)

    assert report["repository"] == str(repository.resolve())
    assert report["memory_dir"] == str(memory_dir.resolve())
    assert report["safe_to_commit_memory_only"] is True
    assert {item["path"] for item in report["memory_changes"]} >= {"aret-memory/.aret-memory/note.txt"}


def test_autosync_refuses_when_changes_exist_outside_memory_store(tmp_path: Path) -> None:
    repository = make_repository(tmp_path)
    memory_dir = repository / "aret-memory" / ".aret-memory"
    store = MemoryStore(memory_dir, write_enabled=True)
    write_policy(memory_dir)
    (repository / "outside.txt").write_text("ne pas committer\n", encoding="utf-8")

    store.register_component("CORE", "Core", "", "test")

    assert store.last_sync_status["refused"] is True
    assert "hors du Memory Store" in store.last_sync_status["reason"]
    assert "outside.txt" in git(repository, "status", "--porcelain")


def make_repository_with_remote(tmp_path: Path) -> tuple[Path, Path]:
    remote = tmp_path / "remote.git"
    subprocess.run(["git", "init", "--bare", str(remote)], check=True, text=True, capture_output=True)
    repository = make_repository(tmp_path)
    git(repository, "branch", "-M", "work")
    git(repository, "remote", "add", "origin", str(remote))
    git(repository, "push", "-u", "origin", "work")
    return repository, remote


def test_sync_memory_only_commits_scope_and_pushes_current_branch(tmp_path: Path) -> None:
    """Friction #1 : persistance de fin de tour bornée au Memory Store, push branche courante."""
    repository, remote = make_repository_with_remote(tmp_path)
    memory_dir = repository / "aret-memory" / ".aret-memory"
    store = MemoryStore(memory_dir, write_enabled=True)  # aucune policy => aucun autosync par mutation
    store.register_component("CORE", "Core", "", "test")
    # Un changement de CODE en cours ne doit JAMAIS être committé par la persistance mémoire.
    (repository / "code_wip.txt").write_text("travail en cours\n", encoding="utf-8")

    result = sync_memory_only(repository, str(memory_dir), "STOP")

    assert result["committed"] is True
    assert result["pushed"] is True
    assert result["branch"] == "work"
    changed = git(repository, "show", "--name-only", "--format=")
    assert "aret-memory/.aret-memory/aret_memory.sqlite" in changed
    assert "code_wip.txt" not in changed
    assert "code_wip.txt" in git(repository, "status", "--porcelain")  # laissé intact
    # Le remote a bien reçu le commit mémoire sur la branche courante.
    assert git(remote, "log", "--oneline", "work")


def test_sync_memory_only_is_non_fatal_without_memory_changes(tmp_path: Path) -> None:
    repository, _remote = make_repository_with_remote(tmp_path)
    memory_dir = repository / "aret-memory" / ".aret-memory"
    MemoryStore(memory_dir, write_enabled=True)  # crée la base puis... on committe une première fois
    sync_memory_only(repository, str(memory_dir), "STOP")
    # Deuxième passe sans changement mémoire : aucun commit vide, aucune erreur.
    result = sync_memory_only(repository, str(memory_dir), "STOP")
    assert result["committed"] is False
    assert "error" not in result
    assert current_branch(repository) == "work"


def test_sync_memory_only_detached_head_skips_push(tmp_path: Path) -> None:
    repository, _remote = make_repository_with_remote(tmp_path)
    memory_dir = repository / "aret-memory" / ".aret-memory"
    store = MemoryStore(memory_dir, write_enabled=True)
    store.register_component("CORE", "Core", "", "test")
    git(repository, "checkout", "--detach", "HEAD")
    assert current_branch(repository) is None
    result = sync_memory_only(repository, str(memory_dir), "STOP")
    assert result["committed"] is True
    assert result["pushed"] is False
    assert "push_skipped" in result
