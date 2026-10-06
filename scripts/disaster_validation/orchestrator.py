"""Declarative, fail-closed disaster-validation orchestration."""

from __future__ import annotations

import json
import os
import re
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from itertools import product
from pathlib import Path
from typing import Any, Iterable, Sequence

from scripts.disaster_harness.manifest import (
    SafetyError,
    build_manifest,
    canonical_json,
    file_entry,
    init_workspace,
    seal_run,
    verify_seal,
    write_manifest,
)

from .adapters import AdapterContext, AdapterResult, ValidationAdapter, adapter_for
from .matrix import MatrixItem, matrix_item

ORCHESTRATION_VERSION = 1
_RESULT_VERSION = 1
_RUN_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
_FINAL_CASE_STATUSES = frozenset({
    "PASS", "FAIL", "UNSUPPORTED", "FAILED_SETUP", "NOT_COMPUTABLE", "ABORTED", "BLOCKED",
})
_RETRYABLE_STATUSES = frozenset({"FAIL", "UNSUPPORTED", "FAILED_SETUP", "ABORTED", "BLOCKED"})


class OrchestrationError(RuntimeError):
    """Raised when an orchestration request cannot preserve evidence invariants."""


@dataclass(frozen=True)
class OrchestrationConfig:
    run_id: str
    evidence_root: Path
    versions: tuple[str, ...]
    filesystems: tuple[str, ...]
    scenarios: tuple[str, ...]
    dataset_profiles: tuple[str, ...]
    page_sizes: tuple[int, ...] = (16384,)
    row_formats: tuple[str, ...] = ("DYNAMIC",)
    dry_run: bool = True
    resume: bool = False
    retry_case_ids: tuple[str, ...] = ()
    allow_real_media: bool = False
    real_media_action: str | None = None


@dataclass(frozen=True)
class CaseRequest:
    case_id: str
    matrix: MatrixItem


@dataclass(frozen=True)
class CaseResult:
    case_id: str
    matrix: MatrixItem
    status: str
    reason: str
    adapter_name: str | None
    adapter_version: int | None
    execution_class: str | None
    case_root: Path | None = None
    manifest_sha256: str | None = None
    lifecycle_state: str | None = None
    comparison: dict[str, Any] | None = None
    strict_pass: bool = False
    comparable: bool = False
    export_eligible: bool = False
    resumed: bool = False
    evidence_sealed: bool = False

    def document(self) -> dict[str, Any]:
        comparison = self.comparison or {}
        return {
            "case_id": self.case_id,
            "mysql_version": self.matrix.version,
            "filesystem": self.matrix.filesystem,
            "scenario": self.matrix.scenario,
            "dataset_profile": self.matrix.dataset_profile,
            "page_size": self.matrix.page_size,
            "row_format": self.matrix.row_format,
            "matrix_status": self.matrix.status,
            "matrix_reason": self.matrix.reason,
            "status": self.status,
            "reason": self.reason,
            "adapter": {
                "name": self.adapter_name,
                "version": self.adapter_version,
                "execution_class": self.execution_class,
            } if self.adapter_name else None,
            "case_root": str(self.case_root) if self.case_root else None,
            "manifest_sha256": self.manifest_sha256,
            "lifecycle_state": self.lifecycle_state,
            "comparison": comparison or None,
            "strict_pass": self.strict_pass,
            "comparable": self.comparable,
            "export_eligible": self.export_eligible,
            "resumed": self.resumed,
            "evidence_sealed": self.evidence_sealed,
        }


@dataclass(frozen=True)
class AggregateResult:
    run_id: str
    root: Path
    status: str
    entries: tuple[CaseResult, ...]
    manifest_sha256: str
    dry_run: bool

    def document(self) -> dict[str, Any]:
        return {
            "orchestration_version": ORCHESTRATION_VERSION,
            "run_id": self.run_id,
            "status": self.status,
            "dry_run": self.dry_run,
            "manifest_sha256": self.manifest_sha256,
            "entries": [entry.document() for entry in self.entries],
        }


def case_identity(item: MatrixItem) -> str:
    """Return the stable identity for every matrix axis."""
    values = (
        item.version.replace(".", "_"), item.filesystem, item.scenario,
        item.dataset_profile, str(item.page_size), item.row_format.lower(),
    )
    return "-".join(_safe_component(value) for value in values)


def normalize_config(config: OrchestrationConfig) -> OrchestrationConfig:
    if not _RUN_ID.fullmatch(config.run_id):
        raise OrchestrationError("run ID must contain only letters, digits, dot, underscore, or dash")
    evidence_root = Path(config.evidence_root).resolve()
    if not evidence_root.is_dir() or evidence_root.is_symlink():
        raise OrchestrationError("evidence root must be an existing non-symlink directory")
    if config.resume and config.retry_case_ids:
        raise OrchestrationError("resume and retry cannot be requested together")
    values = {
        "versions": config.versions,
        "filesystems": config.filesystems,
        "scenarios": config.scenarios,
        "dataset_profiles": config.dataset_profiles,
        "page_sizes": config.page_sizes,
        "row_formats": config.row_formats,
    }
    normalized: dict[str, tuple[Any, ...]] = {}
    for name, axis in values.items():
        if not axis:
            raise OrchestrationError(f"{name} must not be empty")
        if len(axis) != len(set(axis)):
            raise OrchestrationError(f"{name} contains duplicate selectors")
        normalized[name] = tuple(sorted(axis))
    retry_case_ids = tuple(sorted(config.retry_case_ids))
    if len(retry_case_ids) != len(set(retry_case_ids)):
        raise OrchestrationError("retry_case_ids contains duplicates")
    if any(not _RUN_ID.fullmatch(case_id) for case_id in retry_case_ids):
        raise OrchestrationError("retry case IDs must be stable case identities")
    if config.allow_real_media and config.real_media_action != "mysql80-ext4-drop":
        raise OrchestrationError("real-media execution requires real_media_action=mysql80-ext4-drop")
    if config.real_media_action and config.real_media_action != "mysql80-ext4-drop":
        raise OrchestrationError("unknown real-media action")
    return OrchestrationConfig(
        run_id=config.run_id,
        evidence_root=evidence_root,
        versions=normalized["versions"],
        filesystems=normalized["filesystems"],
        scenarios=normalized["scenarios"],
        dataset_profiles=normalized["dataset_profiles"],
        page_sizes=normalized["page_sizes"],
        row_formats=normalized["row_formats"],
        dry_run=config.dry_run,
        resume=config.resume,
        retry_case_ids=retry_case_ids,
        allow_real_media=config.allow_real_media,
        real_media_action=config.real_media_action,
    )


def expand_requests(config: OrchestrationConfig) -> tuple[CaseRequest, ...]:
    config = normalize_config(config)
    requests = tuple(
        CaseRequest(
            case_identity(item),
            item,
        )
        for version, filesystem, scenario, dataset_profile, page_size, row_format in product(
            config.versions, config.filesystems, config.scenarios, config.dataset_profiles,
            config.page_sizes, config.row_formats,
        )
        for item in (matrix_item(version, filesystem, scenario, dataset_profile, page_size, row_format),)
    )
    identities = [request.case_id for request in requests]
    if len(identities) != len(set(identities)):
        raise OrchestrationError("selected matrix cases have duplicate identities")
    return requests


class DisasterOrchestrator:
    def __init__(self, adapters: Sequence[ValidationAdapter]) -> None:
        self.adapters = tuple(adapters)
        names = [(adapter.capabilities.name, adapter.capabilities.version) for adapter in self.adapters]
        if len(names) != len(set(names)):
            raise OrchestrationError("adapter registry contains duplicate name/version entries")

    def run(self, requested: OrchestrationConfig) -> AggregateResult:
        config = normalize_config(requested)
        requests = expand_requests(config)
        self._validate_retry_selection(config, requests)
        if config.retry_case_ids:
            requests = tuple(request for request in requests if request.case_id in config.retry_case_ids)
        root = self._create_aggregate_root(config)
        entries = tuple(self._run_case(request, config, len(requests)) for request in requests)
        status = self._aggregate_status(entries, config.dry_run)
        report_path = root / "aggregate.json"
        _write_new_json(report_path, {
            "orchestration_version": ORCHESTRATION_VERSION,
            "run_id": config.run_id,
            "status": status,
            "dry_run": config.dry_run,
            "created_at": datetime.now(timezone.utc).isoformat(),
            "entries": [entry.document() for entry in entries],
        })
        manifest_sha256 = self._seal_aggregate(root, config, entries, status)
        return AggregateResult(config.run_id, root, status, entries, manifest_sha256, config.dry_run)

    def _run_case(self, request: CaseRequest, config: OrchestrationConfig,
                  selection_size: int) -> CaseResult:
        item = request.matrix
        if item.status != "PLANNED":
            return CaseResult(request.case_id, item, "UNSUPPORTED", item.reason,
                              None, None, None)
        adapter = adapter_for(item, self.adapters, allow_real_media=config.allow_real_media)
        if adapter is None:
            return CaseResult(request.case_id, item, "UNSUPPORTED", "no_audited_adapter",
                              None, None, None)
        capabilities = adapter.capabilities
        if capabilities.execution_class == "real-media":
            policy_reason = self._real_media_policy(config, selection_size)
            if policy_reason:
                return CaseResult(request.case_id, item, "BLOCKED", policy_reason,
                                  capabilities.name, capabilities.version, capabilities.execution_class)
        if config.resume:
            resumed = self._load_resumable_case(request, config, capabilities)
            if resumed is not None:
                return resumed
        if config.dry_run:
            return CaseResult(request.case_id, item, "PLANNED", "dry_run",
                              capabilities.name, capabilities.version, capabilities.execution_class)
        workspace = self._case_workspace(config, request.case_id)
        try:
            init_workspace(workspace, "disaster-validation-case")
        except SafetyError as error:
            raise OrchestrationError(str(error)) from error
        try:
            adapter_result = adapter.run(item, AdapterContext(request.case_id, workspace))
        except Exception as error:
            adapter_result = AdapterResult("ABORTED", f"adapter_error:{type(error).__name__}:{error}")
        result = self._normalize_adapter_result(request, capabilities, adapter_result, workspace)
        _write_new_json(workspace / "orchestration_result.json", {
            "result_version": _RESULT_VERSION,
            "case_id": request.case_id,
            "matrix": asdict(item),
            "result": result.document(),
        })
        return result

    def _normalize_adapter_result(self, request: CaseRequest, capabilities: Any,
                                  result: AdapterResult, workspace: Path) -> CaseResult:
        status = result.status.upper()
        if status not in _FINAL_CASE_STATUSES:
            status = "ABORTED"
            reason = f"invalid_adapter_status:{result.status}"
        else:
            reason = result.reason
        case_root = Path(result.case_root).resolve() if result.case_root else Path(workspace).resolve()
        evidence_sealed = bool(case_root and verify_seal(case_root))
        manifest_sha256 = result.manifest_sha256
        if evidence_sealed and case_root is not None:
            observed = (case_root / "manifest.seal").read_text(encoding="utf-8").strip()
            if manifest_sha256 and manifest_sha256 != observed:
                evidence_sealed = False
                reason = "adapter_manifest_digest_mismatch"
            manifest_sha256 = observed
        strict_pass, comparable = _strict_typed_pass(result.comparison)
        if status == "PASS":
            if not strict_pass or not comparable:
                status, reason = "NOT_COMPUTABLE", "pass_requires_strict_typed_comparison"
            elif not evidence_sealed:
                status, reason = "ABORTED", "pass_requires_sealed_case_evidence"
        if status == "NOT_COMPUTABLE":
            strict_pass = False
        export_eligible = bool(result.export_eligible and strict_pass and comparable and evidence_sealed)
        return CaseResult(
            request.case_id, request.matrix, status, reason,
            capabilities.name, capabilities.version, capabilities.execution_class,
            case_root, manifest_sha256, result.lifecycle_state,
            result.comparison, strict_pass, comparable, export_eligible,
            evidence_sealed=evidence_sealed,
        )

    def _load_resumable_case(self, request: CaseRequest, config: OrchestrationConfig,
                             capabilities: Any) -> CaseResult | None:
        workspace = self._case_workspace(config, request.case_id)
        if not workspace.exists():
            return None
        result_path = workspace / "orchestration_result.json"
        if not result_path.is_file():
            raise OrchestrationError(f"cannot resume case without result metadata: {workspace}")
        try:
            document = json.loads(result_path.read_text(encoding="utf-8"))
            if document["result_version"] != _RESULT_VERSION or document["case_id"] != request.case_id:
                raise OrchestrationError(f"resume metadata does not match case: {workspace}")
            if MatrixItem(**document["matrix"]) != request.matrix:
                raise OrchestrationError(f"resume matrix does not match case: {workspace}")
            result = document["result"]
        except (KeyError, TypeError, json.JSONDecodeError) as error:
            raise OrchestrationError(f"invalid resume metadata: {workspace}") from error
        case_root = Path(result["case_root"]).resolve() if result.get("case_root") else None
        if not case_root or not verify_seal(case_root):
            raise OrchestrationError(f"cannot resume terminal non-sealed case: {workspace}")
        if result.get("adapter", {}).get("name") != capabilities.name:
            raise OrchestrationError(f"resume adapter does not match case: {workspace}")
        return CaseResult(
            request.case_id, request.matrix, str(result["status"]), str(result.get("reason", "")),
            capabilities.name, capabilities.version, capabilities.execution_class,
            case_root, str(result.get("manifest_sha256") or "") or None,
            result.get("lifecycle_state"), result.get("comparison"),
            bool(result.get("strict_pass")), bool(result.get("comparable")),
            bool(result.get("export_eligible")), resumed=True, evidence_sealed=True,
        )

    def _validate_retry_selection(self, config: OrchestrationConfig,
                                  requests: tuple[CaseRequest, ...]) -> None:
        if not config.retry_case_ids:
            return
        selected = {request.case_id for request in requests}
        unknown = set(config.retry_case_ids) - selected
        if unknown:
            raise OrchestrationError(f"retry cases are not selected: {', '.join(sorted(unknown))}")
        for case_id in config.retry_case_ids:
            previous = self._latest_case_result(config.evidence_root, config.run_id, case_id)
            if previous is None or previous.get("status") not in _RETRYABLE_STATUSES:
                raise OrchestrationError(f"case is not eligible for retry: {case_id}")

    def _latest_case_result(self, evidence_root: Path, run_id: str,
                            case_id: str) -> dict[str, Any] | None:
        matches: list[tuple[float, dict[str, Any]]] = []
        prefix = f"{run_id}-aggregate"
        for aggregate in evidence_root.glob(f"{prefix}*/aggregate.json"):
            try:
                document = json.loads(aggregate.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                continue
            for entry in document.get("entries", []):
                if entry.get("case_id") == case_id:
                    matches.append((aggregate.stat().st_mtime, entry))
        return max(matches, key=lambda item: item[0])[1] if matches else None

    def _case_workspace(self, config: OrchestrationConfig, case_id: str) -> Path:
        suffix = ""
        if config.retry_case_ids:
            retry = 1
            while (config.evidence_root / f"{config.run_id}-case-{case_id}-retry-{retry}").exists():
                retry += 1
            suffix = f"-retry-{retry}"
        return config.evidence_root / f"{config.run_id}-case-{case_id}{suffix}"

    def _create_aggregate_root(self, config: OrchestrationConfig) -> Path:
        try:
            return init_workspace(self._free_aggregate_root(config),
                                  "disaster-validation-orchestration")
        except SafetyError as error:
            raise OrchestrationError(str(error)) from error

    @staticmethod
    def _free_aggregate_root(config: OrchestrationConfig) -> Path:
        evidence_root = config.evidence_root
        if config.resume or config.retry_case_ids:
            mode = "resume" if config.resume else "retry"
            index = 1
            while (evidence_root / f"{config.run_id}-aggregate-{mode}-{index}").exists():
                index += 1
            return evidence_root / f"{config.run_id}-aggregate-{mode}-{index}"
        candidate = evidence_root / f"{config.run_id}-aggregate"
        if not candidate.exists():
            return candidate
        index = 2
        while (evidence_root / f"{config.run_id}-aggregate-{index}").exists():
            index += 1
        return evidence_root / f"{config.run_id}-aggregate-{index}"

    @staticmethod
    def _real_media_policy(config: OrchestrationConfig, selection_size: int) -> str:
        if not config.allow_real_media or config.real_media_action != "mysql80-ext4-drop":
            return "real_media_authorization_required"
        if selection_size != 1:
            return "real_media_requires_single_exact_case"
        return ""

    @staticmethod
    def _aggregate_status(entries: tuple[CaseResult, ...], dry_run: bool) -> str:
        if dry_run:
            return "PLANNED"
        return "PASS" if entries and all(entry.status == "PASS" for entry in entries) else "COMPLETE_NONPASS"

    @staticmethod
    def _seal_aggregate(root: Path, config: OrchestrationConfig,
                        entries: tuple[CaseResult, ...], status: str) -> str:
        files = [
            file_entry(path, "artifact", root)
            for path in root.rglob("*")
            if path.is_file() and path.name not in {"manifest.json", "manifest.seal"}
        ]
        manifest = build_manifest(root, files, {
            "orchestration_version": ORCHESTRATION_VERSION,
            "run_id": config.run_id,
            "status": status,
            "dry_run": config.dry_run,
            "case_count": len(entries),
            "all_cases_terminal": not config.dry_run and all(
                entry.status in _FINAL_CASE_STATUSES for entry in entries
            ),
            "entries": [entry.document() for entry in entries],
        })
        try:
            write_manifest(root, manifest)
            return seal_run(root)
        except SafetyError as error:
            raise OrchestrationError(str(error)) from error


def _strict_typed_pass(comparison: dict[str, Any] | None) -> tuple[bool, bool]:
    if not comparison:
        return False, False
    if comparison.get("status") == "COMPARABLE":
        return comparison.get("strict_pass") is True, True
    comparable = [
        entry for entry in comparison.values()
        if isinstance(entry, dict) and entry.get("status") == "COMPARABLE"
    ]
    return bool(comparable) and all(entry.get("strict_pass") is True for entry in comparable), bool(comparable)


def _safe_component(value: str) -> str:
    component = re.sub(r"[^A-Za-z0-9._-]+", "-", str(value)).strip("-.")
    if not component:
        raise OrchestrationError("matrix axis has no safe case identity component")
    return component


def _write_new_json(path: Path, document: dict[str, Any]) -> None:
    if path.exists():
        raise OrchestrationError(f"refusing to overwrite evidence: {path}")
    encoded = (canonical_json(document) + "\n").encode("utf-8")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    with os.fdopen(os.open(path, flags, 0o600), "wb") as stream:
        stream.write(encoded)
