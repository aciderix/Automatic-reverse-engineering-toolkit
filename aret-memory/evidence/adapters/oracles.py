"""Adaptateurs déterministes des oracles ARET vers l’Evidence Store.

Les scripts sont choisis dans une liste fermée. Aucune commande arbitraire ni secret de
signature n’est accepté depuis un client MCP.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any
from uuid import uuid4

PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from core.repository import AretError, MemoryStore
from evidence.capture import create_receipt


@dataclass(frozen=True)
class OracleSpec:
    name: str
    kind: str
    script: str | None
    dependencies: tuple[str, ...]
    timeout_seconds: int
    accepts_fixture: bool = False
    requires_aret_binary: bool = False
    command: tuple[str, ...] | None = None


ORACLES: dict[str, OracleSpec] = {
    "difftest": OracleSpec("difftest", "DIFFTEST", "bench/difftest.sh", ("bash", "gcc"), 1800, requires_aret_binary=True),
    "transpilediff": OracleSpec("transpilediff", "TRANSPILEDIFF", "bench/difftest_transpile.sh", ("bash", "gcc"), 1800, requires_aret_binary=True),
    "stdcall_audit": OracleSpec("stdcall_audit", "STDCALL_AUDIT", "bench/stdcall_audit.sh", ("bash", "python3", "i686-w64-mingw32-nm"), 1800),
    "winediff": OracleSpec("winediff", "WINEDIFF", "bench/winediff.sh", ("bash", "wine", "i686-w64-mingw32-gcc"), 3600, True, True),
    "winehash": OracleSpec("winehash", "WINEHASH", "bench/winoracle/wine_hashes.sh", ("bash", "wine", "i686-w64-mingw32-gcc"), 3600, requires_aret_binary=False),
    "ehdiff": OracleSpec("ehdiff", "EHDIFF", "bench/ehdiff.sh", ("bash", "clang", "lld-link", "llvm-dlltool", "wine"), 3600, requires_aret_binary=True),
    "gnuehdiff": OracleSpec("gnuehdiff", "GNUEHDIFF", "bench/gnuehdiff.sh", ("bash", "i686-w64-mingw32-g++", "wine"), 3600, requires_aret_binary=True),
    "funcdiff": OracleSpec("funcdiff", "FUNCDIFF", "bench/funcdiff.sh", ("bash", "cargo"), 1800),
    "cpudiff": OracleSpec("cpudiff", "CPUDIFF", "src/cpudiff.rs", ("cargo",), 3600, command=("cargo", "test", "--release", "--features", "unpack", "cpudiff")),
}


def utc_now() -> str:
    return datetime.now(UTC).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def repository_revision(repository: Path) -> str:
    result = subprocess.run(["git", "-C", str(repository), "rev-parse", "HEAD"], text=True, capture_output=True, check=False)
    return result.stdout.strip() if result.returncode == 0 else "UNKNOWN"


def _repository_file(repository: Path, relative_path: str, label: str) -> Path:
    """Résout un fichier d’oracle et refuse toute sortie du dépôt configuré."""
    root = repository.resolve()
    candidate = (root / relative_path).resolve()
    if candidate != root and root not in candidate.parents:
        raise AretError(f"{label} résolu hors du dépôt ARET configuré")
    return candidate


def required_tools(spec: OracleSpec, repository: Path) -> list[str]:
    missing = [tool for tool in spec.dependencies if shutil.which(tool) is None]
    if spec.requires_aret_binary and not _repository_file(repository, "target/release/aret", "Binaire ARET").is_file():
        missing.append("target/release/aret")
    if spec.script and not _repository_file(repository, spec.script, "Script d’oracle").is_file():
        missing.append(spec.script)
    return missing


def normalise_result(spec: OracleSpec, exit_code: int | None, stdout: str, stderr: str, missing: list[str], timed_out: bool) -> str:
    if missing:
        return "SKIPPED"
    if timed_out:
        return "ERROR"
    output = f"{stdout}\n{stderr}"
    # Un script peut signaler quelques fixtures SKIP tout en échouant globalement.
    # Le code de sortie non nul reste alors un FAIL observable ; il ne doit jamais
    # être masqué par une ligne SKIP partielle dans la sortie.
    if exit_code is not None and exit_code != 0:
        return "FAIL"
    if spec.name == "difftest":
        match = re.search(r"differential equivalence:\s*(\d+)\s*/\s*(\d+)\s+functions", output)
        if exit_code == 0 and match and int(match.group(1)) == int(match.group(2)) and int(match.group(2)) > 0:
            return "PASS"
    elif spec.name == "transpilediff":
        match = re.search(r"transpile-pipeline equivalence:\s*(\d+)\s*/\s*(\d+)\s+opt-levels", output)
        if exit_code == 0 and match and int(match.group(1)) == int(match.group(2)) and int(match.group(2)) > 0:
            return "PASS"
    elif spec.name == "stdcall_audit" and exit_code == 0 and re.search(r"stdcall-pop audit:\s*PASS", output):
        return "PASS"
    elif spec.name == "winediff":
        match = re.search(r"OS-API \(Wine\) equivalence:\s*(\d+)\s*/\s*(\d+)\s+programs", output)
        if exit_code == 0 and match and int(match.group(1)) == int(match.group(2)) and int(match.group(2)) > 0:
            return "PASS"
    elif spec.name == "ehdiff":
        match = re.search(r"MSVC EH differential:\s*(\d+)\s*/\s*(\d+)\s+fixtures", output)
        if exit_code == 0 and match and int(match.group(1)) == int(match.group(2)) and int(match.group(2)) > 0:
            return "PASS"
    elif spec.name == "gnuehdiff":
        match = re.search(r"GNU/Itanium C\+\+ EH differential:\s*(\d+)\s*/\s*(\d+)\s+fixtures", output)
        if exit_code == 0 and match and int(match.group(1)) == int(match.group(2)) and int(match.group(2)) > 0:
            return "PASS"
    elif spec.name == "funcdiff" and exit_code == 0 and re.search(r"funcdiff corpus gate:\s*PASS", output):
        return "PASS"
    elif spec.name == "cpudiff" and exit_code == 0 and re.search(r"test result:\s*ok", output):
        return "PASS"
    elif spec.name == "winehash" and exit_code == 0 and re.search(r"\bOK\s+[0-9a-f]{64}\b", output):
        # Cette sortie est une mesure Wine à comparer au runner Windows, pas un gate de conformité.
        return "UNKNOWN"
    if re.search(r"^SKIP(?:PED)?\b", output, flags=re.MULTILINE):
        return "SKIPPED"
    return "ERROR"


def safe_fixture(value: str | None) -> str | None:
    if value is None:
        return None
    candidate = value.strip()
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,100}", candidate):
        raise AretError("Nom de fixture invalide")
    return candidate


def _execute_command(
    command: list[str], cwd: Path, environment: dict[str, str], limit: int, stream: bool,
) -> tuple[str, str, int | None, bool]:
    """Run the oracle subprocess and return (stdout, stderr, exit_code, timed_out).

    stream=False (the signing path, run_oracle): exactly the previous
    subprocess.run(capture_output=True) behaviour — captured, silent.
    stream=True (the CI measure path): stdout is ALSO echoed live to our stderr as it
    arrives (per-fixture progress in the CI log); the captured stdout/stderr are
    byte-identical to the silent path either way. A timeout kills the process and sets
    timed_out, and exit_code stays None on timeout — like the non-stream path."""
    if not stream:
        try:
            completed = subprocess.run(command, cwd=cwd, env=environment, text=True, capture_output=True, timeout=limit, check=False)
            return completed.stdout, completed.stderr, completed.returncode, False
        except subprocess.TimeoutExpired as exc:
            out = exc.stdout if isinstance(exc.stdout, str) else (exc.stdout or b"").decode("utf-8", errors="replace")
            err = exc.stderr if isinstance(exc.stderr, str) else (exc.stderr or b"").decode("utf-8", errors="replace")
            return out, err, None, True
    proc = subprocess.Popen(command, cwd=cwd, env=environment, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out_buf: list[str] = []
    err_buf: list[str] = []

    def _pump(stream_obj: Any, buf: list[str], echo: bool) -> None:
        for line in iter(stream_obj.readline, ""):
            buf.append(line)
            if echo:
                sys.stderr.write(line)
                sys.stderr.flush()
        stream_obj.close()

    t_out = threading.Thread(target=_pump, args=(proc.stdout, out_buf, True), daemon=True)
    t_err = threading.Thread(target=_pump, args=(proc.stderr, err_buf, False), daemon=True)
    t_out.start()
    t_err.start()
    timed_out = False
    try:
        proc.wait(timeout=limit)
    except subprocess.TimeoutExpired:
        timed_out = True
        proc.kill()
        proc.wait()
    t_out.join()
    t_err.join()
    exit_code = None if timed_out else proc.returncode
    return "".join(out_buf), "".join(err_buf), exit_code, timed_out


def build_measurement(
    repository: Path,
    oracle_name: str,
    fixture: str | None = None,
    timeout_seconds: int | None = None,
    stream: bool = False,
) -> tuple[OracleSpec, dict[str, Any], Path, list[str], bool]:
    """Exécute un oracle de la liste fermée et construit l'artefact de mesure canonique
    `aret-oracle-artifact/v1`, SANS aucun accès à l'Evidence Store ni au secret de
    signature.

    C'est la SEULE source de vérité de l'exécution et du verdict (via normalise_result).
    Partagée par run_oracle (poste local : signe et enregistre) et measure_oracle (CI :
    publie seulement la mesure, re-vérifiée et re-signée LOCALEMENT ensuite — le secret
    ne quitte jamais le poste)."""
    name = oracle_name.strip().lower()
    if name not in ORACLES:
        raise AretError("Oracle inconnu : choisir parmi " + ", ".join(sorted(ORACLES)))
    spec = ORACLES[name]
    fixture = safe_fixture(fixture)
    if fixture and not spec.accepts_fixture:
        raise AretError(f"L’oracle {name} n’accepte pas de sélection de fixture")
    limit = timeout_seconds if timeout_seconds is not None else spec.timeout_seconds
    if not isinstance(limit, int) or limit < 1 or limit > spec.timeout_seconds:
        raise AretError(f"timeout_seconds doit être compris entre 1 et {spec.timeout_seconds} pour {name}")
    repository = repository.expanduser().resolve()
    if not repository.is_dir():
        raise AretError("Dépôt ARET introuvable")
    missing = required_tools(spec, repository)
    script_path = _repository_file(repository, str(spec.script), "Script d’oracle") if spec.script else None
    aret_binary = _repository_file(repository, "target/release/aret", "Binaire ARET")
    command = list(spec.command) if spec.command else ["bash", str(script_path)]
    if fixture:
        command.append(fixture)
    started = utc_now()
    started_monotonic = time.monotonic()
    stdout = ""
    stderr = ""
    exit_code: int | None = None
    timed_out = False
    if not missing:
        environment = {"PATH": os.environ.get("PATH", ""), "LC_ALL": "C", "TZ": "UTC", "ARET": str(aret_binary)}
        # Forward the content-addressed object cache dir when the caller set one. It is
        # the 60min->10min lever for winediff (the HLE is compiled once and reused across
        # fixtures). Optimization only: the cache is keyed by content, so a stale entry is
        # a miss — never a wrong result — and determinism is preserved. Kept to an explicit
        # allowlist so the oracle subprocess env stays otherwise hermetic.
        objcache = os.environ.get("ARET_OBJCACHE")
        if objcache:
            environment["ARET_OBJCACHE"] = objcache
        stdout, stderr, exit_code, timed_out = _execute_command(command, repository, environment, limit, stream)
    finished = utc_now()
    result = normalise_result(spec, exit_code, stdout, stderr, missing, timed_out)
    command_text = " ".join(json.dumps(part) if re.search(r"\s", part) else part for part in command)
    environment_summary = {
        "adapter": "aret-mmu-oracles/1",
        "oracle": spec.name,
        "repository": str(repository),
        "repository_revision": repository_revision(repository),
        "script": spec.script or "<commande-cargo-fermée>",
        "fixture": fixture or "",
        "timeout_seconds": limit,
        "missing_dependencies": missing,
        "timed_out": timed_out,
        "duration_seconds": round(time.monotonic() - started_monotonic, 3),
    }
    artifact = {
        "format": "aret-oracle-artifact/v1",
        "oracle": spec.name,
        "kind": spec.kind,
        "command": command_text,
        "result": result,
        "exit_code": exit_code,
        "started_at": started,
        "finished_at": finished,
        "environment": environment_summary,
        "stdout": stdout,
        "stderr": stderr,
    }
    return spec, artifact, aret_binary, missing, timed_out


def measure_oracle(
    repository: Path,
    oracle_name: str,
    fixture: str | None = None,
    timeout_seconds: int | None = None,
) -> dict[str, Any]:
    """Exécute un oracle et renvoie l'artefact de mesure `aret-oracle-artifact/v1`, SANS
    Evidence Store ni secret. Destiné à la CI : la mesure est publiée telle quelle, puis
    re-vérifiée (re-parse du stdout) et signée LOCALEMENT par l'importateur de preuve."""
    _spec, artifact, _binary, _missing, _timed_out = build_measurement(repository, oracle_name, fixture, timeout_seconds, stream=True)
    return artifact


def run_oracle(
    store: MemoryStore,
    repository: Path,
    oracle_name: str,
    knowledge_id: str | None = None,
    promote: bool = False,
    fixture: str | None = None,
    timeout_seconds: int | None = None,
    actor: str = "aret-oracle-adapter",
) -> dict[str, Any]:
    store._require_write()
    spec, artifact, _aret_binary, missing, timed_out = build_measurement(repository, oracle_name, fixture, timeout_seconds)
    result = artifact["result"]
    exit_code = artifact["exit_code"]
    command_text = artifact["command"]
    environment_summary = artifact["environment"]
    artifact_rel = f"oracles/{spec.name}/{datetime.now(UTC).strftime('%Y%m%dT%H%M%SZ')}_{uuid4().hex[:12]}.json"
    artifact_path = store.artifacts_dir / artifact_rel
    artifact_path.parent.mkdir(parents=True, exist_ok=True)
    artifact_path.write_text(json.dumps(artifact, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    artifact_hash = hashlib.sha256(artifact_path.read_bytes()).hexdigest()
    receipt_payload = {
        "kind": spec.kind,
        "command": command_text,
        "result": result,
        "exit_code": exit_code,
        "artifact_path": artifact_rel,
        "artifact_hash": artifact_hash,
        "environment": environment_summary,
        "started_at": artifact["started_at"],
        "finished_at": artifact["finished_at"],
    }
    secret = store.proof_hmac_secret
    receipt = create_receipt(receipt_payload, secret) if secret else {"payload_hash": "", "receipt_hmac": ""}
    proof = store.record_proof(
        **receipt_payload,
        stdout_ref=artifact_rel,
        stderr_ref=artifact_rel,
        receipt_hmac=receipt["receipt_hmac"],
        actor=actor,
    )
    attachment = None
    if knowledge_id:
        attachment = store.attach_proof(knowledge_id, proof["id"], actor, promote=promote)
    elif promote:
        raise AretError("promotion demandée sans knowledge_id")
    return {
        "proof": proof,
        "artifact": {"path": artifact_rel, "sha256": artifact_hash},
        "execution": {"oracle": spec.name, "result": result, "exit_code": exit_code, "missing_dependencies": missing, "timed_out": timed_out},
        "attachment": attachment,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Exécuter un oracle ARET et enregistrer sa preuve")
    parser.add_argument("oracle", choices=sorted(ORACLES))
    parser.add_argument("--repository", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--memory-dir", type=Path, default=Path(__file__).resolve().parents[2] / ".aret-memory")
    parser.add_argument("--knowledge-id")
    parser.add_argument("--promote", action="store_true")
    parser.add_argument("--fixture")
    parser.add_argument("--timeout-seconds", type=int)
    parser.add_argument("--write-enabled", action="store_true")
    parser.add_argument("--measure-only", action="store_true",
                        help="N'exécute que la mesure et émet l'artefact aret-oracle-artifact/v1 ; "
                             "aucun Evidence Store, aucun secret (mode CI).")
    parser.add_argument("--output", type=Path, help="Fichier de sortie pour --measure-only (défaut : stdout).")
    args = parser.parse_args()
    # Mode CI : produire la mesure canonique, jamais signer ici. Le secret reste local ;
    # la preuve admissible est frappée sur le poste par l'importateur, qui re-vérifie
    # le verdict à partir du stdout de cette mesure.
    if args.measure_only:
        artifact = measure_oracle(args.repository, args.oracle, args.fixture, args.timeout_seconds)
        text = json.dumps(artifact, ensure_ascii=False, indent=2) + "\n"
        if args.output:
            args.output.write_text(text, encoding="utf-8")
        else:
            sys.stdout.write(text)
        sys.stderr.write(f"{args.oracle} {artifact['result']} exit={artifact['exit_code']}\n")
        return
    if not args.write_enabled:
        raise SystemExit("--write-enabled est requis pour enregistrer une preuve")
    os.environ["ARET_MEMORY_DIR"] = str(args.memory_dir)
    os.environ["ARET_WRITE_ENABLED"] = "true"
    try:
        result = run_oracle(MemoryStore(), args.repository, args.oracle, args.knowledge_id, args.promote, args.fixture, args.timeout_seconds)
        print(json.dumps({"ok": True, "result": result}, ensure_ascii=False, indent=2))
    except AretError as exc:
        print(json.dumps({"ok": False, "error": {"code": type(exc).__name__, "message": str(exc)}}, ensure_ascii=False, indent=2))
        raise SystemExit(2) from exc


if __name__ == "__main__":
    main()
