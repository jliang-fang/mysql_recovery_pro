#!/usr/bin/env python3
"""Restricted MySQL 8.0/ext4/DROP TABLE real-media smoke validation.

This module deliberately supports one matrix item.  It preserves evidence and
media state on failures, and treats raw-carving output as forensic evidence
rather than a verified row-recovery result.
"""

from __future__ import annotations

import argparse
import json
import os
import secrets
import subprocess
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Iterable, Optional

from scripts.disaster_harness.ground_truth import write_ground_truth
from scripts.disaster_harness.manifest import canonical_json, sha256_file

from .device_safety import (
    ALLOWED_DEVICE,
    ApprovalToken,
    DeviceIdentity,
    DeviceSafetyError,
    issue_approval_token,
    preflight,
    preflight_controlled_mount,
    read_device_identity,
    validate_approval_token,
    write_approval_token,
)
from .lifecycle import RunLifecycleError, ValidationRun
from .matrix import CAPABILITIES, MatrixItem, matrix_item


SMOKE_STATES = (
    "NEW", "PREFLIGHTED", "FORMAT_APPROVED", "FORMATTED", "MOUNT_APPROVED",
    "MOUNTED", "MYSQL_READY", "TRUTH_SEALED", "DROP_APPROVED", "DROPPED",
    "UNMOUNTED", "DEVICE_READ_ONLY", "RAW_RECOVERY", "FINALIZED", "FAILED",
)
_TERMINAL_STATES = frozenset({"FINALIZED", "FAILED"})
_ALLOWED_TRANSITIONS = {
    "NEW": frozenset({"PREFLIGHTED", "FAILED"}),
    "PREFLIGHTED": frozenset({"FORMAT_APPROVED", "FAILED"}),
    "FORMAT_APPROVED": frozenset({"FORMATTED", "FAILED"}),
    "FORMATTED": frozenset({"MOUNT_APPROVED", "FAILED"}),
    "MOUNT_APPROVED": frozenset({"MOUNTED", "FAILED"}),
    "MOUNTED": frozenset({"MYSQL_READY", "FAILED"}),
    "MYSQL_READY": frozenset({"TRUTH_SEALED", "FAILED"}),
    "TRUTH_SEALED": frozenset({"DROP_APPROVED", "FAILED"}),
    "DROP_APPROVED": frozenset({"DROPPED", "FAILED"}),
    "DROPPED": frozenset({"UNMOUNTED", "FAILED"}),
    "UNMOUNTED": frozenset({"DEVICE_READ_ONLY", "FAILED"}),
    "DEVICE_READ_ONLY": frozenset({"RAW_RECOVERY", "FAILED"}),
    "RAW_RECOVERY": frozenset({"FINALIZED", "FAILED"}),
}


class SmokeRunError(RuntimeError):
    """Raised when the restricted smoke workflow cannot safely advance."""


@dataclass(frozen=True)
class CommandResult:
    argv: list[str]
    returncode: int
    stdout: str
    stderr: str


CommandRunner = Callable[[list[str], int, str], CommandResult]
Preflight = Callable[[str, Path], dict[str, Any]]
ControlledMountPreflight = Callable[[str, Path, Path], dict[str, Any]]
IdentityReader = Callable[[str], DeviceIdentity]
TruthCapture = Callable[[Path, str, str, str, Path], dict[str, Any]]


@dataclass(frozen=True)
class SmokeConfig:
    evidence_root: Path
    run_id: str
    mount_point: Path
    recovery_binary: Path
    device_path: str = ALLOWED_DEVICE
    image: str = CAPABILITIES["8.0"].image
    image_digest: str = CAPABILITIES["8.0"].image_digest
    page_size: int = 16384
    row_format: str = "DYNAMIC"
    database: str = "disaster_smoke"
    table: str = "drop_smoke"


class Mysql80Ext4DropSmoke:
    """One-shot workflow with durable state and action-specific token records."""

    def __init__(self, config: SmokeConfig, runner: CommandRunner = None,
                 preflight_fn: Preflight = preflight,
                 controlled_mount_preflight_fn: ControlledMountPreflight = preflight_controlled_mount,
                 identity_reader: IdentityReader = read_device_identity,
                 truth_capture: TruthCapture = None) -> None:
        self.config = config
        self.runner = runner or _subprocess_runner
        self.preflight_fn = preflight_fn
        self.controlled_mount_preflight_fn = controlled_mount_preflight_fn
        self.identity_reader = identity_reader
        self.truth_capture = truth_capture or _capture_truth_from_socket
        self.matrix = matrix_item("8.0", "ext4", "drop", "smoke",
                                  config.page_size, config.row_format)
        self.run: Optional[ValidationRun] = None
        self.state = "NEW"
        self.events: list[dict[str, Any]] = []
        self.raw_recovery_read_only = False
        self.container_name = f"dbrec-smoke-{config.run_id}"
        self.network_name = f"dbrec-smoke-net-{config.run_id}"

    @property
    def root(self) -> Path:
        if self.run is None:
            raise SmokeRunError("run workspace has not been created")
        return self.run.root

    @property
    def smoke_state_path(self) -> Path:
        return self.root / "smoke_state.json"

    def run_smoke(self) -> dict[str, Any]:
        self._validate_config()
        try:
            self.run = ValidationRun.create(self.config.evidence_root, self.config.run_id, self.matrix)
            self._record("NEW", {"matrix": asdict(self.matrix), "device": self.config.device_path})
            initial = self._snapshot_preflight("initial")
            self._transition("PREFLIGHTED", {"preflight_sha256": initial["preflight_sha256"]})
            self.run.transition("PRECHECKED", {"preflight_sha256": initial["preflight_sha256"]})

            format_argv = ["mkfs.ext4", "-F", self.config.device_path]
            self._authorize_action("format", format_argv, initial)
            self._transition("FORMAT_APPROVED", {"argv": format_argv})
            self._execute_required(format_argv, 120, "format")
            # Wait for udev to process the new filesystem UUID before taking the post-format snapshot.
            subprocess.run(["udevadm", "settle"], check=False)
            self._transition("FORMATTED", {"argv": format_argv})

            post_format = self._snapshot_preflight("post-format")
            mount_argv = ["mount", "-t", "ext4", "-o", "rw", self.config.device_path,
                          str(self.config.mount_point)]
            self._authorize_action("mount", mount_argv, post_format)
            self._transition("MOUNT_APPROVED", {"argv": mount_argv})
            self._execute_required(mount_argv, 60, "mount")
            self._transition("MOUNTED", {"argv": mount_argv})
            self.run.transition("MEDIA_PREPARED", {"mount_point": str(self.config.mount_point)})

            data_dir, socket_dir = self._prepare_mysql()
            self._transition("MYSQL_READY", {
                "container": self.container_name,
                "network": self.network_name,
                "data_dir": str(data_dir),
            })
            self.run.transition("DATASET_CREATED", {"container": self.container_name})

            truth_path = self.root / "truth" / "ground_truth.json"
            # In official MySQL 8.0 images the socket is named mysqld.sock or mysql.sock
            sock = socket_dir / "mysqld.sock" if (socket_dir / "mysqld.sock").exists() else socket_dir / "mysql.sock"
            truth = self.truth_capture(sock, self.config.database,
                                       self.config.table, "8.0", truth_path)
            if truth.get("status") != "SEALED_READY" or not truth.get("schema", {}).get("primary_key"):
                raise SmokeRunError("captured ground truth is not independently comparable")
            self._record_artifact("ground_truth", truth_path)
            self._transition("TRUTH_SEALED", {
                "ground_truth": str(truth_path.relative_to(self.root)),
                "dataset_sha256": truth.get("dataset_sha256", ""),
            })
            self.run.transition("TRUTH_SEALED", {"ground_truth": str(truth_path.relative_to(self.root))})

            self._force_persistence_barriers()
            drop_preflight = self._snapshot_preflight("pre-drop", controlled_mount=True)
            drop_sql = f"DROP TABLE `{self.config.database}`.`{self.config.table}`"
            drop_argv = self._mysql_argv(drop_sql)
            self._authorize_action("drop", drop_argv, drop_preflight, controlled_mount=True)
            self._transition("DROP_APPROVED", {"argv": drop_argv, "sql": drop_sql})
            self._execute_required(drop_argv, 60, "drop")
            self._transition("DROPPED", {"argv": drop_argv})
            self.run.transition("PERSISTED", {"barriers": "recorded"})
            self.run.transition("SCENARIO_APPLIED", {"scenario": "drop"})

            self._stop_mysql()
            unmount_argv = ["umount", str(self.config.mount_point)]
            self._execute_required(unmount_argv, 60, "unmount")
            self._transition("UNMOUNTED", {"argv": unmount_argv})

            post_unmount = self._snapshot_preflight("post-unmount")
            set_read_only = self._execute_required(
                ["blockdev", "--setro", self.config.device_path], 60, "device read-only set"
            )
            read_only_state = self._execute_required(
                ["blockdev", "--getro", self.config.device_path], 60, "device read-only verify"
            )
            if read_only_state.stdout.strip() != "1":
                raise SmokeRunError("device did not enter kernel read-only mode")
            self.raw_recovery_read_only = True
            self._transition("DEVICE_READ_ONLY", {
                "post_unmount_preflight_sha256": post_unmount["preflight_sha256"],
                "set_read_only_argv": set_read_only.argv,
                "verify_read_only_argv": read_only_state.argv,
                "verified_read_only": True,
            })

            raw = self._run_raw_recovery()
            self._transition("RAW_RECOVERY", raw)
            self.run.transition("RECOVERY_ATTEMPTED", {"returncode": raw["returncode"]})

            report = self._write_final_report(truth, raw)
            self._transition("FINALIZED", {"report": str(report.relative_to(self.root))})
            self.run.transition("REPORTED", {"report": str(report.relative_to(self.root))})
            seal = self.run.seal({"final_status": "NOT_COMPUTABLE", "raw_classification": "forensic_evidence"})
            return {"status": "NOT_COMPUTABLE", "run_root": str(self.root),
                    "report": str(report), "seal": seal, "raw": raw}
        except Exception as error:  # preserve evidence and target-media state for inspection
            self._fail(error)
            if isinstance(error, SmokeRunError):
                raise
            raise SmokeRunError(str(error)) from error

    def _validate_config(self) -> None:
        if self.matrix.status != "PLANNED" or self.matrix != matrix_item(
                "8.0", "ext4", "drop", "smoke", self.config.page_size, self.config.row_format):
            raise SmokeRunError("only the MySQL 8.0/ext4/DROP/smoke matrix item is permitted")
        if os.path.realpath(self.config.device_path) != ALLOWED_DEVICE:
            raise SmokeRunError(f"only {ALLOWED_DEVICE} is permitted")
        if self.config.image != CAPABILITIES["8.0"].image or self.config.image_digest != CAPABILITIES["8.0"].image_digest:
            raise SmokeRunError("MySQL image must match the recorded 8.0 digest")
        evidence_root = Path(self.config.evidence_root).resolve()
        mount_point = Path(self.config.mount_point).resolve()
        if not evidence_root.is_dir() or evidence_root.is_symlink():
            raise SmokeRunError("evidence root must be an existing non-symlink directory")
        if mount_point.is_symlink() or evidence_root == mount_point or evidence_root in mount_point.parents:
            raise SmokeRunError("mount point must not overlap the evidence root")
        if mount_point.exists() and not mount_point.is_dir():
            raise SmokeRunError("mount point must be a directory")
        if not Path(self.config.recovery_binary).is_file() or Path(self.config.recovery_binary).is_symlink():
            raise SmokeRunError("recovery binary must be an existing non-symlink file")

    def _snapshot_preflight(self, stage: str, controlled_mount: bool = False) -> dict[str, Any]:
        if controlled_mount:
            report = self.controlled_mount_preflight_fn(
                self.config.device_path, self.config.evidence_root, self.config.mount_point
            )
        else:
            report = self.preflight_fn(self.config.device_path, self.config.evidence_root)
        if report.get("status") != "PRECHECKED":
            raise SmokeRunError(f"{stage} preflight did not succeed")
        identity = DeviceIdentity(**report["device"])
        if identity.path != ALLOWED_DEVICE or identity != self.identity_reader(self.config.device_path):
            raise SmokeRunError(f"{stage} preflight identity drifted")
        digest = report.get("preflight_sha256", "")
        if not isinstance(digest, str) or len(digest) != 64:
            raise SmokeRunError(f"{stage} preflight has no valid digest")
        directory = self.root / "environment" / f"preflight-{stage}"
        directory.mkdir(mode=0o700)
        path = directory / "preflight.json"
        path.write_text(canonical_json(report) + "\n", encoding="utf-8")
        self._record_artifact(f"preflight_{stage}", path)
        return report

    def _authorize_action(self, action: str, argv: list[str], report: dict[str, Any],
                          controlled_mount: bool = False) -> None:
        token = issue_approval_token(report, self.config.run_id, action, nonce=secrets.token_hex(16))
        token_path = write_approval_token(self.root, token)
        recheck = self._snapshot_preflight(f"{action}-recheck", controlled_mount=controlled_mount)
        original_identity = DeviceIdentity(**report["device"])
        rechecked_identity = DeviceIdentity(**recheck["device"])
        current_identity = self.identity_reader(self.config.device_path)
        if original_identity != rechecked_identity or current_identity != original_identity:
            raise SmokeRunError(f"{action} preflight identity changed before execution")
        if _preflight_fingerprint(report) != _preflight_fingerprint(recheck):
            raise SmokeRunError(f"{action} preflight safety facts changed before execution")
        try:
            validate_approval_token(token, current_identity, self.config.run_id, action,
                                    report["preflight_sha256"])
        except DeviceSafetyError as error:
            raise SmokeRunError(f"{action} approval rejected: {error}") from error
        self._consume_token(token, token_path, argv, recheck["preflight_sha256"])

    def _consume_token(self, token: ApprovalToken, token_path: Path, argv: list[str],
                       recheck_digest: str) -> None:
        ledger = self.root / "approval_tokens" / f"consumed-{token.nonce}.json"
        if ledger.exists():
            raise SmokeRunError("approval token was already consumed")
        record = {
            "token": asdict(token),
            "token_path": str(token_path.relative_to(self.root)),
            "workspace": str(self.root),
            "argv": argv,
            "recheck_preflight_sha256": recheck_digest,
            "consumed_at": datetime.now(timezone.utc).isoformat(),
            "consumed": True,
        }
        _write_new_json(ledger, record)
        self._record_artifact(f"consumed_token_{token.action}", ledger)

    def _prepare_mysql(self) -> tuple[Path, Path]:
        inspect = self._execute_required(
            ["podman", "image", "inspect", "--format", "{{.Digest}}", self.config.image],
            60, "image inspect")
        if inspect.stdout.strip() != self.config.image_digest:
            raise SmokeRunError("local MySQL image does not match the recorded digest")
        self._execute_required(["podman", "network", "create", self.network_name], 60, "network create")
        data_dir = self.config.mount_point / "mysql-data"
        # Linux AF_UNIX socket paths are limited to 108 bytes; use /tmp/dbrec-<run_id> for the socket directory
        socket_dir = Path(f"/tmp/dbrec-sock-{self.config.run_id}")
        data_dir.mkdir(mode=0o700)
        socket_dir.mkdir(mode=0o777, exist_ok=True)
        # Ensure world-read/write for podman/mysql root user
        os.chmod(socket_dir, 0o777)
        run_argv = [
            "podman", "run", "-d", "--name", self.container_name, "--network", self.network_name,
            "-v", f"{data_dir}:/var/lib/mysql:Z", "-v", f"{socket_dir}:/var/run/mysqld:Z",
            "-e", "MYSQL_ALLOW_EMPTY_PASSWORD=yes", self.config.image,
        ]
        self._execute_required(run_argv, 180, "mysql start")
        # Poll mysqladmin ping with retries to account for MySQL container startup & initialization time.
        readiness_cmd = ["podman", "exec", self.container_name, "mysqladmin", "ping", "-uroot"]
        last_result = None
        for attempt in range(60):
            last_result = self.runner(readiness_cmd, 10, "")
            if last_result.returncode == 0:
                break
            import time
            time.sleep(1)
        self._record_command("mysql readiness", last_result)
        if last_result.returncode != 0:
            raise SmokeRunError(f"mysql readiness failed with exit status {last_result.returncode}")
        self._execute_required(self._mysql_argv(
            f"CREATE DATABASE `{self.config.database}`; "
            f"CREATE TABLE `{self.config.database}`.`{self.config.table}` "
            "(id INT NOT NULL PRIMARY KEY, payload VARBINARY(32) NOT NULL, amount DECIMAL(10,2) NOT NULL) "
            "ENGINE=InnoDB ROW_FORMAT=DYNAMIC; "
            f"INSERT INTO `{self.config.database}`.`{self.config.table}` VALUES "
            "(1, X'000102', 1.25), (2, X'FEFF', 2.50), (3, X'', 3.75)"),
            60, "dataset create")
        return data_dir, socket_dir

    def _force_persistence_barriers(self) -> None:
        commands = [
            self._mysql_argv("FLUSH NO_WRITE_TO_BINLOG TABLES"),
            self._mysql_argv("SET GLOBAL innodb_flush_log_at_trx_commit=1"),
            ["podman", "exec", self.container_name, "sync"],
            ["sync"],
        ]
        for argv in commands:
            self._execute_required(argv, 60, "persistence barrier")

    def _stop_mysql(self) -> None:
        self._execute_required(["podman", "stop", self.container_name], 90, "mysql stop")
        self._execute_required(["podman", "rm", self.container_name], 60, "container remove")
        self._execute_required(["podman", "network", "rm", self.network_name], 60, "network remove")

    def _run_raw_recovery(self) -> dict[str, Any]:
        if not self.raw_recovery_read_only:
            raise SmokeRunError("raw recovery requires verified kernel read-only device state")
        output_dir = self.root / "recovery-output"
        output_dir.mkdir(exist_ok=True)
        result_path = output_dir / "result_contract.json"
        argv = [
            str(self.config.recovery_binary), "--disk-carving", "--device-path", self.config.device_path,
            "--filesystem", "ext4", "--page-size", str(self.config.page_size),
            "--output", str(output_dir), "--result-json", str(result_path),
        ]
        result = self.runner(argv, 3600, "")
        execution_path = self.root / "logs" / "raw_recovery_execution.json"
        _write_new_json(execution_path, _result_document(result))
        self._record_artifact("raw_recovery_execution", execution_path)
        if result_path.is_file():
            self._record_artifact("raw_result_contract", result_path)
        return {
            "argv": argv,
            "returncode": result.returncode,
            "result_contract": str(result_path.relative_to(self.root)) if result_path.is_file() else None,
            "classification": "forensic_evidence",
            "status": "NOT_COMPUTABLE",
            "reason": "raw recovery has no independently canonical typed rows for strict comparison",
        }

    def _write_final_report(self, truth: dict[str, Any], raw: dict[str, Any]) -> Path:
        report = {
            "report_version": 1,
            "run_id": self.config.run_id,
            "matrix": asdict(self.matrix),
            "native_typed_result": {
                "status": truth["status"],
                "ground_truth": "truth/ground_truth.json",
                "dataset_sha256": truth.get("dataset_sha256"),
            },
            "raw_forensic_result": raw,
            "final_status": "NOT_COMPUTABLE",
            "reason": "raw scanner output is forensic evidence, not a canonical typed recovery result",
            "lifecycle_state": "FINALIZED",
        }
        path = self.root / "reports" / "final_report.json"
        _write_new_json(path, report)
        self._record_artifact("final_report", path)
        return path

    def _mysql_argv(self, sql: str) -> list[str]:
        return ["podman", "exec", self.container_name, "mysql", "-uroot", "-e", sql]

    def _execute_required(self, argv: list[str], timeout: int, label: str) -> CommandResult:
        result = self.runner(argv, timeout, "")
        self._record_command(label, result)
        if result.returncode != 0:
            raise SmokeRunError(f"{label} failed with exit status {result.returncode}")
        return result

    def _record_command(self, label: str, result: CommandResult) -> None:
        logs = self.root / "logs"
        path = logs / f"{len(list(logs.glob('[0-9][0-9][0-9]-*.json'))):03d}-{label.replace(' ', '_')}.json"
        _write_new_json(path, {"label": label, **_result_document(result)})
        self._record_artifact("command", path)

    def _transition(self, state: str, details: dict[str, Any]) -> None:
        if self.state in _TERMINAL_STATES or state not in _ALLOWED_TRANSITIONS.get(self.state, frozenset()):
            raise SmokeRunError(f"invalid smoke state transition: {self.state} -> {state}")
        self.state = state
        self._record(state, details)

    def _record(self, state: str, details: dict[str, Any]) -> None:
        self.events.append({"state": state, "recorded_at": datetime.now(timezone.utc).isoformat(),
                            "details": details})
        _write_replace_json(self.smoke_state_path, {
            "smoke_state_version": 1,
            "run_id": self.config.run_id,
            "state": state,
            "events": self.events,
        })

    def _record_artifact(self, role: str, path: Path) -> None:
        path = Path(path)
        if not path.is_file():
            raise SmokeRunError(f"required evidence artifact is missing: {path}")
        ledger = self.root / "manifests" / "artifacts.jsonl"
        with ledger.open("a", encoding="utf-8") as stream:
            stream.write(canonical_json({"role": role, "path": str(path.relative_to(self.root)),
                                         "sha256": sha256_file(path), "size": path.stat().st_size}) + "\n")

    def _fail(self, error: Exception) -> None:
        if self.run is None or self.state in _TERMINAL_STATES:
            return
        try:
            self._transition("FAILED", {"error": f"{type(error).__name__}: {error}"})
            if self.run.state not in {"ABORTED", "SEALED", "REJECTED", "NOT_COMPUTABLE", "FAILED_SETUP"}:
                self.run.transition("ABORTED", {"error": f"{type(error).__name__}: {error}"})
        except (SmokeRunError, RunLifecycleError):
            pass


def _preflight_fingerprint(report: dict[str, Any]) -> str:
    stable = {key: value for key, value in report.items()
              if key not in {"checked_at", "preflight_sha256"}}
    # Podman's graphRootUsed counter fluctuates naturally between checks;
    # mask it to avoid false-positive safety drift detections.
    if isinstance(stable.get("storage_dependencies"), dict):
        deps = dict(stable["storage_dependencies"])
        if isinstance(deps.get("podman_store"), dict):
            pstore = dict(deps["podman_store"])
            pstore.pop("graphRootUsed", None)
            deps["podman_store"] = pstore
        stable["storage_dependencies"] = deps
    return canonical_json(stable)


def _result_document(result: CommandResult) -> dict[str, Any]:
    return {"argv": result.argv, "returncode": result.returncode,
            "stdout": result.stdout, "stderr": result.stderr}


def _write_new_json(path: Path, document: dict[str, Any]) -> None:
    path = Path(path)
    if path.exists():
        raise SmokeRunError(f"refusing to overwrite evidence: {path}")
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    with os.fdopen(os.open(path, flags, 0o600), "w", encoding="utf-8") as stream:
        stream.write(canonical_json(document) + "\n")


def _write_replace_json(path: Path, document: dict[str, Any]) -> None:
    path = Path(path)
    temporary = path.with_suffix(path.suffix + ".tmp")
    if temporary.exists():
        raise SmokeRunError(f"temporary state path already exists: {temporary}")
    _write_new_json(temporary, document)
    temporary.replace(path)


def _subprocess_runner(argv: list[str], timeout: int, input_text: str = "") -> CommandResult:
    completed = subprocess.run(argv, input=input_text, text=True, capture_output=True, timeout=timeout,
                               check=False)
    return CommandResult(list(argv), completed.returncode, completed.stdout, completed.stderr)


def _capture_truth_from_socket(socket_path: Path, database: str, table: str, mysql_version: str,
                               output: Path) -> dict[str, Any]:
    from scripts.disaster_harness.native_export import export_table
    return export_table(str(socket_path), database, table, output, "root", "", mysql_version)


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Restricted MySQL 8.0/ext4/DROP smoke validation")
    parser.add_argument("--evidence-root", required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--mount-point", required=True)
    parser.add_argument("--recovery-binary", required=True)
    parser.add_argument("--execute", action="store_true",
                        help="perform the authorized workflow; omitted mode is non-mutating preflight only")
    args = parser.parse_args(argv)
    config = SmokeConfig(Path(args.evidence_root), args.run_id, Path(args.mount_point),
                         Path(args.recovery_binary))
    if not args.execute:
        try:
            report = preflight(config.device_path, config.evidence_root)
        except DeviceSafetyError as error:
            print(f"preflight_rejected: {error}")
            return 2
        print(canonical_json({"status": "DRY_RUN_PRECHECKED", "preflight_sha256": report["preflight_sha256"]}))
        return 0
    try:
        result = Mysql80Ext4DropSmoke(config).run_smoke()
    except SmokeRunError as error:
        print(f"smoke_failed: {error}")
        return 2
    print(canonical_json(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
