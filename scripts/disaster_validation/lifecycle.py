"""Non-destructive state tracking for auditable validation runs."""

from __future__ import annotations

import json
import re
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from scripts.disaster_harness.manifest import (
    SafetyError,
    build_manifest,
    canonical_json,
    file_entry,
    init_workspace,
    seal_run,
    write_manifest,
)

from .matrix import MatrixItem

RUN_STATE_VERSION = 1
_RUN_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
TERMINAL_STATES = frozenset({"REJECTED", "NOT_COMPUTABLE", "FAILED_SETUP", "ABORTED", "SEALED"})
_ALLOWED_TRANSITIONS = {
    "PLANNED": frozenset({"PRECHECKED", "REJECTED", "FAILED_SETUP", "ABORTED"}),
    "PRECHECKED": frozenset({"MEDIA_PREPARED", "REJECTED", "FAILED_SETUP", "ABORTED"}),
    "MEDIA_PREPARED": frozenset({"DATASET_CREATED", "REJECTED", "FAILED_SETUP", "ABORTED"}),
    "DATASET_CREATED": frozenset({"TRUTH_SEALED", "REJECTED", "FAILED_SETUP", "ABORTED"}),
    "TRUTH_SEALED": frozenset({"PERSISTED", "REJECTED", "FAILED_SETUP", "ABORTED"}),
    "PERSISTED": frozenset({"SCENARIO_APPLIED", "REJECTED", "FAILED_SETUP", "ABORTED"}),
    "SCENARIO_APPLIED": frozenset({"RECOVERY_ATTEMPTED", "REJECTED", "NOT_COMPUTABLE", "ABORTED"}),
    "RECOVERY_ATTEMPTED": frozenset({"IMPORT_VALIDATED", "REPORTED", "REJECTED", "NOT_COMPUTABLE", "ABORTED"}),
    "IMPORT_VALIDATED": frozenset({"REPORTED", "REJECTED", "NOT_COMPUTABLE", "ABORTED"}),
    "REPORTED": frozenset({"SEALED", "ABORTED"}),
}


class RunLifecycleError(RuntimeError):
    """Raised when a validation run cannot advance safely."""


@dataclass(frozen=True)
class RunEvent:
    state: str
    recorded_at: str
    details: dict[str, Any]


class ValidationRun:
    """A non-overwriting external-evidence validation run."""

    def __init__(self, root: Path, run_id: str, matrix: MatrixItem, state: str,
                 events: list[RunEvent]) -> None:
        self.root = root
        self.run_id = run_id
        self.matrix = matrix
        self.state = state
        self.events = events

    @property
    def state_path(self) -> Path:
        return self.root / "run_state.json"

    @property
    def manifest_path(self) -> Path:
        return self.root / "manifest.json"

    @classmethod
    def create(cls, evidence_root: Path, run_id: str, matrix: MatrixItem) -> "ValidationRun":
        if not _RUN_ID.fullmatch(run_id):
            raise RunLifecycleError("run ID must contain only letters, digits, dot, underscore, or dash")
        evidence_root = Path(evidence_root).resolve()
        if not evidence_root.is_dir() or evidence_root.is_symlink():
            raise RunLifecycleError(f"evidence root must be an existing non-symlink directory: {evidence_root}")
        if matrix.status != "PLANNED":
            raise RunLifecycleError(f"cannot create a run for matrix status: {matrix.status}")
        root = evidence_root / run_id
        try:
            init_workspace(root, "disaster-validation")
        except SafetyError as error:
            raise RunLifecycleError(str(error)) from error
        run = cls(root, run_id, matrix, "PLANNED", [])
        run._append("PLANNED", {})
        for name in ("environment", "truth", "persistent-input", "disaster-input",
                     "recovery-output", "import-validation", "logs", "reports", "manifests"):
            (root / name).mkdir(mode=0o700)
        return run

    @classmethod
    def load(cls, root: Path) -> "ValidationRun":
        root = Path(root).resolve()
        state_path = root / "run_state.json"
        if not state_path.is_file():
            raise RunLifecycleError(f"run state is missing: {state_path}")
        try:
            document = json.loads(state_path.read_text(encoding="utf-8"))
            if document["run_state_version"] != RUN_STATE_VERSION:
                raise RunLifecycleError("unsupported run state version")
            matrix = MatrixItem(**document["matrix"])
            events = [RunEvent(**event) for event in document["events"]]
            return cls(root, document["run_id"], matrix, document["state"], events)
        except (KeyError, TypeError, json.JSONDecodeError) as error:
            raise RunLifecycleError(f"invalid run state: {state_path}") from error

    def transition(self, state: str, details: dict[str, Any] | None = None) -> None:
        if self.state in TERMINAL_STATES:
            raise RunLifecycleError(f"cannot transition terminal state: {self.state}")
        if state not in _ALLOWED_TRANSITIONS.get(self.state, frozenset()):
            raise RunLifecycleError(f"invalid run state transition: {self.state} -> {state}")
        self._append(state, details or {})
        self.state = state

    def seal(self, extra: dict[str, Any] | None = None) -> str:
        if self.state == "REPORTED":
            self._append("SEALED", {})
            self.state = "SEALED"
        elif self.state != "SEALED" or self.manifest_path.exists():
            raise RunLifecycleError("only a reported run can be sealed")
        files = [
            file_entry(path, "artifact", self.root)
            for path in self.root.rglob("*")
            if path.is_file() and path.name not in {"manifest.json", "manifest.seal"}
        ]
        manifest = build_manifest(self.root, files, {
            "run_id": self.run_id,
            "matrix": asdict(self.matrix),
            "state": self.state,
            "events": [asdict(event) for event in self.events],
            **(extra or {}),
        })
        try:
            write_manifest(self.root, manifest)
            return seal_run(self.root)
        except SafetyError as error:
            raise RunLifecycleError(str(error)) from error

    def _append(self, state: str, details: dict[str, Any]) -> None:
        event = RunEvent(state, datetime.now(timezone.utc).isoformat(), details)
        events = [*self.events, event]
        document = {
            "run_state_version": RUN_STATE_VERSION,
            "run_id": self.run_id,
            "matrix": asdict(self.matrix),
            "state": state,
            "events": [asdict(item) for item in events],
        }
        temporary = self.state_path.with_suffix(".json.tmp")
        if temporary.exists():
            raise RunLifecycleError(f"temporary state path already exists: {temporary}")
        temporary.write_text(canonical_json(document) + "\n", encoding="utf-8")
        temporary.replace(self.state_path)
        self.events = events
