#!/usr/bin/env python3
"""Disposable native/offline disaster-recovery experiment orchestration.

This module only operates on disposable copies. It records execution evidence;
it does not promote page counts, process exit codes, or evidence fragments to
row-level recovery results.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import stat
import subprocess
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Sequence

from .comparator import compare_three_way
from .ground_truth import load_ground_truth
from .manifest import (
    SafetyError,
    build_manifest,
    canonical_json,
    file_entry,
    init_workspace,
    require_workspace,
    seal_run,
    sha256_file,
    validate_source,
    verify_source_immutable,
    write_manifest,
)


@dataclass(frozen=True)
class CommandResult:
    argv: List[str]
    returncode: Optional[int]
    stdout: str
    stderr: str
    elapsed_seconds: float
    timed_out: bool = False
    spawn_error: Optional[str] = None

    @property
    def status(self) -> str:
        if self.spawn_error:
            return "SPAWN_FAILED"
        if self.timed_out:
            return "TIMEOUT"
        return "EXITED_0" if self.returncode == 0 else "EXITED_NONZERO"

    def as_dict(self) -> dict:
        return {
            "argv": self.argv,
            "returncode": self.returncode,
            "stdout": self.stdout,
            "stderr": self.stderr,
            "elapsed_seconds": self.elapsed_seconds,
            "timed_out": self.timed_out,
            "spawn_error": self.spawn_error,
            "status": self.status,
        }


@dataclass(frozen=True)
class NativeSpec:
    image: str
    datadir_mount: str = "/var/lib/mysql"
    socket_path: str = "/var/run/mysqld/mysqld.sock"
    root_password: str = "root"
    database: str = ""
    table: str = ""
    export_sql: str = ""
    container_engine: str = "podman"
    startup_timeout: float = 120.0
    query_timeout: float = 60.0
    shutdown_timeout: float = 30.0
    extra_mysqld_args: tuple[str, ...] = ()
    result_container_path: str = "/var/lib/dbrec-export/native_result.json"
    # Typed native export runs inside the container as python3 -m disaster_harness.native_export.
    # The harness package and a pure-python DB-API package are bind-mounted read-only.
    python_bin: str = "python3"
    harness_host_path: Optional[Path] = None
    harness_container_path: str = "/opt/dbrec-harness"
    site_packages_host_path: Optional[Path] = None
    site_packages_container_path: str = "/opt/dbrec-libs"


@dataclass(frozen=True)
class OfflineSpec:
    binary: Path
    ibd_arg: str
    format: str = "csv"
    extra_args: tuple[str, ...] = ()
    timeout: float = 300.0


@dataclass(frozen=True)
class MutationSpec:
    """A declared mutation applied only inside a disposable datadir clone."""
    relative_path: Path
    role: str
    scope: str
    source: Optional[Path] = None
    action: str = "replace"


@dataclass(frozen=True)
class ExperimentSpec:
    workspace: Path
    pristine_datadir: Path
    damaged_input: Optional[Path]
    damaged_relative_path: Optional[Path]
    ground_truth: Path
    native: NativeSpec
    offline: OfflineSpec
    levels: tuple[int, ...] = tuple(range(7))
    source_hash: Optional[str] = None
    mutations: tuple[MutationSpec, ...] = ()
    offline_input: Optional[Path] = None


Runner = Callable[..., CommandResult]


def run_command(argv: Sequence[str], timeout: float, input_text: str = "") -> CommandResult:
    started = time.monotonic()
    try:
        completed = subprocess.run(
            list(argv),
            input=input_text,
            text=True,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
        return CommandResult(
            argv=list(argv),
            returncode=completed.returncode,
            stdout=completed.stdout,
            stderr=completed.stderr,
            elapsed_seconds=time.monotonic() - started,
        )
    except subprocess.TimeoutExpired as error:
        return CommandResult(
            argv=list(argv),
            returncode=None,
            stdout=error.stdout or "",
            stderr=error.stderr or "",
            elapsed_seconds=time.monotonic() - started,
            timed_out=True,
        )
    except OSError as error:
        return CommandResult(
            argv=list(argv),
            returncode=None,
            stdout="",
            stderr="",
            elapsed_seconds=time.monotonic() - started,
            spawn_error=str(error),
        )


def _validate_tree(root: Path, label: str) -> Path:
    root = Path(root)
    if root.is_symlink() or not root.is_dir():
        raise SafetyError(f"{label} must be a regular directory: {root}")
    for path in root.rglob("*"):
        if path.is_symlink():
            raise SafetyError(f"{label} contains symlink: {path}")
        mode = path.lstat().st_mode
        if stat.S_ISBLK(mode) or stat.S_ISCHR(mode):
            raise SafetyError(f"{label} contains device: {path}")
        if not (stat.S_ISREG(mode) or stat.S_ISDIR(mode)):
            raise SafetyError(f"{label} contains unsupported entry: {path}")
    return root.resolve()


def _clone_tree(source: Path, target: Path) -> None:
    if target.exists() or target.is_symlink():
        raise SafetyError(f"refusing to overwrite clone: {target}")
    shutil.copytree(source, target, symlinks=False)


def _mutation_target(datadir: Path, relative_path: Path) -> Path:
    if relative_path.is_absolute() or not relative_path.parts or any(part in ("", ".", "..")
                                                               for part in relative_path.parts):
        raise SafetyError(f"invalid mutation relative path: {relative_path}")
    root = datadir.resolve()
    target = (root / relative_path).resolve()
    try:
        target.relative_to(root)
    except ValueError as error:
        raise SafetyError("mutation path escapes cloned datadir") from error
    if target.is_symlink() or not target.exists() or not target.is_file():
        raise SafetyError(f"mutation target must be an existing regular file: {target}")
    return target


def _apply_mutations(datadir: Path, mutations: Sequence[MutationSpec]) -> list[dict]:
    if not mutations:
        raise SafetyError("at least one declared mutation is required")
    seen = set()
    records = []
    for mutation in mutations:
        if mutation.role not in {"tablespace", "redo", "server_dictionary", "table_sdi"}:
            raise SafetyError(f"unsupported mutation role: {mutation.role}")
        if mutation.scope not in {"level1", "level2", "level3_table_sdi", "level3_server_dictionary"}:
            raise SafetyError(f"unsupported mutation scope: {mutation.scope}")
        target = _mutation_target(datadir, Path(mutation.relative_path))
        key = str(target)
        if key in seen:
            raise SafetyError(f"duplicate mutation target: {target}")
        seen.add(key)
        before = sha256_file(target)
        if mutation.action == "replace":
            if mutation.source is None:
                raise SafetyError("replacement mutation requires a source file")
            source = validate_source(mutation.source)
            shutil.copyfile(source, target)
            expected = sha256_file(source)
        elif mutation.action == "remove":
            if mutation.source is not None:
                raise SafetyError("removal mutation must not include a source file")
            target.unlink()
            expected = None
        else:
            raise SafetyError(f"unsupported mutation action: {mutation.action}")
        after = sha256_file(target) if target.exists() else None
        if after != expected:
            raise SafetyError(f"mutation verification failed: {target}")
        records.append({
            "path": str(Path(mutation.relative_path)),
            "role": mutation.role,
            "scope": mutation.scope,
            "action": mutation.action,
            "before_sha256": before,
            "after_sha256": after,
            "source_sha256": expected,
        })
    return records


def _legacy_mutation(spec: ExperimentSpec) -> MutationSpec:
    if spec.damaged_input is None or spec.damaged_relative_path is None:
        raise SafetyError("legacy mutation requires damaged input and relative path")
    return MutationSpec(
        source=spec.damaged_input,
        relative_path=spec.damaged_relative_path,
        role="tablespace",
        scope="level2",
    )


def _mysql_command(spec: NativeSpec, level: int, datadir: Path, run_dir: Path, name: str) -> List[str]:
    command = [
        spec.container_engine, "run", "-d", "--name", name,
        "-v", f"{datadir.resolve()}:{spec.datadir_mount}:Z",
        "-v", f"{run_dir.resolve()}:/var/lib/dbrec-export:Z",
        "--security-opt", "label=disable",
        "--ulimit", "core=0",
    ]
    if spec.harness_host_path is not None:
        command.extend(["-v", f"{Path(spec.harness_host_path).resolve()}:{spec.harness_container_path}:ro"])
    if spec.site_packages_host_path is not None:
        command.extend(["-v", f"{Path(spec.site_packages_host_path).resolve()}:{spec.site_packages_container_path}:ro"])
    command.extend([
        "-e",
        f"PYTHONPATH={spec.harness_container_path}"
        + (f":{spec.site_packages_container_path}" if spec.site_packages_host_path else ""),
        spec.image,
        "mysqld",
        f"--innodb-force-recovery={level}",
        f"--socket={spec.socket_path}",
        "--skip-networking",
        "--skip-log-bin",
    ])
    command.extend(spec.extra_mysqld_args)
    return command


def _typed_export_command(spec: NativeSpec, name: str) -> List[str]:
    return [
        spec.container_engine, "exec", "-i", name,
        spec.python_bin, "-m", "disaster_harness.native_export",
        "--socket", spec.socket_path,
        "--database", spec.database,
        "--table", spec.table,
        "--output", spec.result_container_path,
        "--user", "root",
        "--password-stdin",
    ]


def _mysql_ping_command(spec: NativeSpec, name: str) -> List[str]:
    return [
        spec.container_engine, "exec", "-i", name, "mysqladmin",
        "--protocol=socket", f"--socket={spec.socket_path}",
        "-uroot", "-p", "ping",
    ]


def _password_input(password: str) -> str:
    return password + "\n"


def _native_result_host_path(spec: NativeSpec, run_dir: Path) -> Path:
    mount_root = Path("/var/lib/dbrec-export")
    container_path = Path(spec.result_container_path)
    try:
        relative = container_path.relative_to(mount_root)
    except ValueError as error:
        raise SafetyError("native result path must be inside the export mount") from error
    if not relative.parts or any(part in ("", ".", "..") for part in relative.parts):
        raise SafetyError("native result path must name a file inside the export mount")
    return run_dir / relative


def _write_json(path: Path, value: Any) -> None:
    if path.exists():
        raise SafetyError(f"refusing to overwrite artifact: {path}")
    path.write_text(canonical_json(value) + "\n", encoding="utf-8")


def _native_result_document(level: int, command: CommandResult, query: CommandResult,
                            shutdown: CommandResult, output: Optional[dict]) -> dict:
    if output is None:
        return {
            "status": "NOT_COMPUTABLE",
            "reason": "native structured export missing",
            "force_recovery": level,
            "excluded_evidence_levels": ["native_export"],
        }
    if output.get("status") not in (None, "SEALED_READY", "COMPLETE"):
        output = dict(output)
        output.setdefault("excluded_evidence_levels", []).append("native_export")
    output["execution"] = {
        "force_recovery": level,
        "startup": command.as_dict(),
        "query": query.as_dict(),
        "shutdown": shutdown.as_dict(),
    }
    return output


def run_native_level(spec: NativeSpec, level: int, datadir: Path, run_dir: Path,
                     runner: Runner = run_command) -> dict:
    name = f"dbrec-native-{os.getpid()}-{level}-{uuid.uuid4().hex[:12]}"
    startup = runner(_mysql_command(spec, level, datadir, run_dir, name), spec.startup_timeout)
    query = CommandResult([], None, "", "not executed", 0.0, spawn_error="startup not ready")
    shutdown = CommandResult([], None, "", "not executed", 0.0, spawn_error="startup not ready")
    result_document = None
    ping = None
    try:
        if startup.status == "EXITED_0":
            deadline = time.monotonic() + spec.startup_timeout
            while time.monotonic() < deadline:
                ping = runner(
                    _mysql_ping_command(spec, name),
                    min(5.0, max(1.0, deadline - time.monotonic())),
                    _password_input(spec.root_password),
                )
                if ping.status == "EXITED_0":
                    break
                time.sleep(0.25)
            if ping is None or ping.status != "EXITED_0":
                query = CommandResult([], None, "", "mysql did not become ready", 0.0,
                                      spawn_error="startup not ready")
            else:
                query = runner(
                    _typed_export_command(spec, name),
                    spec.query_timeout,
                    _password_input(spec.root_password),
                )
                candidate = _native_result_host_path(spec, run_dir)
                if candidate.is_file():
                    result_document = _load_typed_result(candidate, "native_export")
            logs = runner([spec.container_engine, "logs", name], spec.shutdown_timeout)
            (run_dir / "mysqld.log").write_text(
                f"# stdout\n{logs.stdout}\n# stderr\n{logs.stderr}\n", encoding="utf-8")
    finally:
        if startup.status == "EXITED_0":
            shutdown = runner([spec.container_engine, "stop", "-t", str(int(spec.shutdown_timeout)), name],
                              spec.shutdown_timeout)
            runner([spec.container_engine, "rm", "-f", name], spec.shutdown_timeout)
    _write_json(run_dir / "native_execution.json", {
        "force_recovery": level,
        "startup": startup.as_dict(),
        "readiness": ping.as_dict() if ping else None,
        "query": query.as_dict(),
        "shutdown": shutdown.as_dict(),
    })
    return _native_result_document(level, startup, query, shutdown, result_document)


def _load_typed_result(path: Path, evidence_level: str) -> dict:
    try:
        result = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return {
            "status": "NOT_COMPUTABLE",
            "reason": f"invalid structured result: {type(error).__name__}",
            "excluded_evidence_levels": [evidence_level],
        }
    required = {"database", "table", "schema", "schema_fingerprint", "rows"}
    if not isinstance(result, dict) or not required.issubset(result) or not isinstance(result["rows"], list):
        return {
            "status": "NOT_COMPUTABLE",
            "reason": "result is not a typed canonical dataset",
            "excluded_evidence_levels": [evidence_level],
            "result_artifact": str(path),
        }
    return result


def _offline_not_computable(run_dir: Path, reason: str) -> dict:
    result = {
        "status": "NOT_COMPUTABLE",
        "reason": reason,
        "excluded_evidence_levels": ["offline_export"],
    }
    _write_json(run_dir / "offline_execution.json", {
        "status": "NOT_EXECUTED",
        "reason": reason,
    })
    return result


def run_offline(spec: OfflineSpec, offline_input: Optional[Path], run_dir: Path,
                runner: Runner = run_command) -> dict:
    if offline_input is None:
        return _offline_not_computable(run_dir, "no offline comparison input declared")
    output_dir = run_dir / "output"
    output_dir.mkdir()
    expected_result = output_dir / f"{Path(offline_input).stem}_result.json"
    argv = [str(spec.binary), f"--{spec.ibd_arg}", str(offline_input),
            "--output", str(output_dir), "--format", spec.format]
    argv.extend(spec.extra_args)
    command = runner(argv, spec.timeout)
    _write_json(run_dir / "offline_execution.json", command.as_dict())
    result_paths = sorted(output_dir.glob("*_result.json"))
    result_path = expected_result if expected_result.is_file() else (
        result_paths[0] if len(result_paths) == 1 else None)
    if result_path is None:
        return {
            "status": "NOT_COMPUTABLE",
            "reason": "offline structured result missing",
            "excluded_evidence_levels": ["offline_export"],
        }
    result = _load_typed_result(result_path, "offline_export")
    result["execution"] = command.as_dict()
    result["result_artifact"] = str(result_path.relative_to(run_dir))
    return result


def run_experiment(spec: ExperimentSpec, runner: Runner = run_command) -> dict:
    workspace = require_workspace(spec.workspace)
    pristine = _validate_tree(spec.pristine_datadir, "pristine datadir")
    damaged = validate_source(spec.damaged_input) if spec.damaged_input is not None else None
    if spec.source_hash and damaged is None:
        raise SafetyError("sealed source hash requires a damaged input")
    ground_truth = load_ground_truth(spec.ground_truth)
    damaged_hash = sha256_file(damaged) if damaged is not None else None
    if spec.source_hash and spec.source_hash != damaged_hash:
        raise SafetyError("damaged input does not match the sealed source hash")
    offline_input = (validate_source(spec.offline_input)
                     if spec.offline_input is not None else None)
    if offline_input is None and damaged is not None and damaged.suffix.lower() == ".ibd":
        offline_input = damaged
    offline_input_hash = sha256_file(offline_input) if offline_input is not None else None
    if not spec.levels or any(level not in range(7) for level in spec.levels):
        raise ValueError("native levels must be a non-empty subset of 0..6")
    binary = validate_source(spec.offline.binary)
    if spec.mutations:
        mutations = spec.mutations
    elif damaged is not None and spec.damaged_relative_path is not None:
        mutations = (_legacy_mutation(spec),)
    else:
        raise SafetyError("mutation plan is required when no legacy damaged input is declared")
    for mutation in mutations:
        if mutation.source is not None:
            validate_source(mutation.source)
    if offline_input is not None and offline_input.suffix.lower() != ".ibd":
        raise SafetyError("offline comparison input must be an .ibd tablespace")

    experiment_dir = workspace / "experiment"
    if experiment_dir.exists():
        raise SafetyError(f"refusing to overwrite experiment: {experiment_dir}")
    experiment_dir.mkdir()
    _write_json(experiment_dir / "experiment_spec.json", {
        "levels": list(spec.levels),
        "offline_input": str(offline_input) if offline_input else None,
        "offline_input_sha256": offline_input_hash,
        "pristine_datadir": str(pristine),
        "ground_truth": str(spec.ground_truth),
        "native_image": spec.native.image,
        "offline_binary_sha256": sha256_file(binary),
        "mutations": [{
            "relative_path": str(mutation.relative_path),
            "role": mutation.role,
            "scope": mutation.scope,
            "action": mutation.action,
            "source_sha256": sha256_file(mutation.source) if mutation.source else None,
        } for mutation in mutations],
    })

    native_results = {}
    for level in spec.levels:
        level_dir = experiment_dir / f"native_{level}"
        level_dir.mkdir()
        datadir = level_dir / "datadir"
        _clone_tree(pristine, datadir)
        mutation_records = _apply_mutations(datadir, mutations)
        _write_json(level_dir / "input.json", {
            "mutations": mutation_records,
            "offline_input": str(offline_input) if offline_input else None,
            "offline_input_sha256": offline_input_hash,
            "source_unchanged": (verify_source_immutable(damaged, damaged_hash)
                                  if damaged is not None else None),
        })
        native_results[str(level)] = run_native_level(spec.native, level, datadir, level_dir, runner)

    offline_dir = experiment_dir / "offline"
    offline_dir.mkdir()
    offline_result = run_offline(spec.offline, offline_input, offline_dir, runner)
    comparison = {
        str(level): compare_three_way(ground_truth, native_results[str(level)], offline_result)
        for level in spec.levels
    }
    _write_json(experiment_dir / "comparison.json", comparison)
    files = [file_entry(path, "artifact", experiment_dir) for path in experiment_dir.rglob("*")
             if path.is_file() and path.name not in {"manifest.json", "manifest.seal"}]
    manifest = build_manifest(experiment_dir, files, {
        "status": "SEALED_READY" if all(item["status"] == "NOT_COMPUTABLE" or
                                          item["status"] == "COMPARABLE"
                                          for item in comparison.values()) else "INCOMPLETE",
        "offline_input_sha256": offline_input_hash,
        "source_unchanged_sha256": (sha256_file(damaged) if damaged is not None else None),
        "mutation_source_unchanged": all(
            mutation.source is None or verify_source_immutable(mutation.source, sha256_file(mutation.source))
            for mutation in mutations
        ),
        "offline_binary_sha256": sha256_file(binary),
    })
    write_manifest(experiment_dir, manifest)
    seal = seal_run(experiment_dir)
    return {"experiment_dir": str(experiment_dir), "manifest_sha256": seal,
            "comparison": comparison}


def _parse_levels(value: str) -> tuple[int, ...]:
    levels = tuple(sorted({int(item) for item in value.split(",") if item.strip()}))
    if not levels or any(level not in range(7) for level in levels):
        raise argparse.ArgumentTypeError("levels must be a comma-separated subset of 0..6")
    return levels


def main() -> None:
    parser = argparse.ArgumentParser(description="Run sealed disposable native/offline recovery experiments")
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--pristine-datadir", type=Path, required=True)
    parser.add_argument("--damaged-input", type=Path)
    parser.add_argument("--damaged-relative-path", type=Path)
    parser.add_argument("--offline-input", type=Path)
    parser.add_argument("--ground-truth", type=Path, required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--offline-binary", type=Path, required=True)
    parser.add_argument("--offline-ibd-arg", default="ibd")
    parser.add_argument("--database", default="")
    parser.add_argument("--table", default="")
    parser.add_argument("--export-sql", required=True)
    parser.add_argument("--levels", type=_parse_levels, default=tuple(range(7)))
    args = parser.parse_args()
    result = run_experiment(ExperimentSpec(
        workspace=args.workspace,
        pristine_datadir=args.pristine_datadir,
        damaged_input=args.damaged_input,
        damaged_relative_path=args.damaged_relative_path,
        offline_input=args.offline_input,
        ground_truth=args.ground_truth,
        native=NativeSpec(image=args.image, database=args.database, table=args.table,
                           export_sql=args.export_sql),
        offline=OfflineSpec(binary=args.offline_binary, ibd_arg=args.offline_ibd_arg),
        levels=args.levels,
    ))
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
