"""Capability-declared adapters for generalized disaster validation."""

from __future__ import annotations

import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Protocol

from scripts.disaster_harness.experiment import ExperimentSpec, run_experiment

from .matrix import MatrixItem
from .mysql80_ext4_drop_smoke import Mysql80Ext4DropSmoke


@dataclass(frozen=True)
class AdapterCapabilities:
    name: str
    version: int
    execution_class: str
    versions: frozenset[str]
    filesystems: frozenset[str]
    scenarios: frozenset[str]
    dataset_profiles: frozenset[str]
    page_sizes: frozenset[int]
    row_formats: frozenset[str]

    def supports(self, item: MatrixItem) -> bool:
        return (
            item.status == "PLANNED"
            and item.version in self.versions
            and item.filesystem in self.filesystems
            and item.scenario in self.scenarios
            and item.dataset_profile in self.dataset_profiles
            and item.page_size in self.page_sizes
            and item.row_format in self.row_formats
        )


@dataclass(frozen=True)
class AdapterContext:
    case_id: str
    evidence_root: Path
    dry_run: bool = False


@dataclass(frozen=True)
class AdapterResult:
    status: str
    reason: str = ""
    case_root: Path | None = None
    manifest_sha256: str | None = None
    lifecycle_state: str | None = None
    comparison: dict[str, Any] | None = None
    export_eligible: bool = False


class ValidationAdapter(Protocol):
    capabilities: AdapterCapabilities

    def run(self, item: MatrixItem, context: AdapterContext) -> AdapterResult:
        ...


ExperimentFactory = Callable[[MatrixItem, AdapterContext], ExperimentSpec]
SmokeFactory = Callable[[MatrixItem, AdapterContext], Mysql80Ext4DropSmoke]
BaselineRunner = Callable[[MatrixItem, AdapterContext], dict[str, Any]]


def _phase_a_baseline_runner(binary: Path | None = None,
                             runner: Any = None) -> BaselineRunner:
    """Return a baseline runner that dispatches through PhaseAOrchestrator.

    The returned callable runs the full sealed Phase A lifecycle inside the
    orchestrator-provided case workspace and returns its result document.
    """
    from .phase_a_offline_orchestrator import (
        PhaseAOrchestrator,
        RECOVERY_BINARY,
        _subprocess_runner,
    )

    resolved_binary = binary if binary is not None else RECOVERY_BINARY
    resolved_runner = runner if runner is not None else _subprocess_runner

    def run(item: MatrixItem, context: AdapterContext) -> dict[str, Any]:
        orchestrator = PhaseAOrchestrator(
            run_root=context.evidence_root,
            item=item,
            binary=resolved_binary,
            run_id=context.case_id,
            runner=resolved_runner,
        )
        return orchestrator.execute()

    return run


class OfflineExperimentAdapter:
    """Runs only factory-supplied disposable ExperimentSpec instances."""

    capabilities = AdapterCapabilities(
        name="offline-experiment",
        version=1,
        execution_class="disposable",
        versions=frozenset({"5.6", "5.7", "8.0", "8.4", "9.0"}),
        filesystems=frozenset({"ext4", "xfs"}),
        scenarios=frozenset({"baseline"}),
        dataset_profiles=frozenset({"smoke", "100k", "1m"}),
        page_sizes=frozenset({4096, 8192, 16384, 32768, 65536}),
        row_formats=frozenset({"COMPACT", "REDUNDANT", "DYNAMIC", "COMPRESSED"}),
    )

    def __init__(self, experiment_factory: ExperimentFactory | None = None,
                 runner: Callable[..., Any] | None = None,
                 baseline_runner: BaselineRunner | None = None) -> None:
        self.experiment_factory = experiment_factory
        self.runner = runner
        self.baseline_runner = baseline_runner

    def run(self, item: MatrixItem, context: AdapterContext) -> AdapterResult:
        # Prefer the audited Phase A baseline path when a baseline runner is
        # configured; fall back to the raw ExperimentSpec seam for legacy tests.
        if self.baseline_runner is not None:
            return self._run_baseline(item, context)
        if self.experiment_factory is None:
            return AdapterResult("FAILED_SETUP", "offline_experiment_factory_not_configured")
        spec = self.experiment_factory(item, context)
        result = run_experiment(spec) if self.runner is None else run_experiment(spec, self.runner)
        comparison = result.get("comparison")
        strict_pass = bool(comparison) and all(
            entry.get("strict_pass") is True
            for entry in comparison.values()
            if entry.get("status") == "COMPARABLE"
        ) and any(entry.get("status") == "COMPARABLE" for entry in comparison.values())
        return AdapterResult(
            "PASS" if strict_pass else "FAIL",
            "" if strict_pass else "strict_typed_comparison_failed",
            Path(result["experiment_dir"]),
            result.get("manifest_sha256"),
            "SEALED",
            comparison,
            strict_pass,
        )

    def _run_baseline(self, item: MatrixItem, context: AdapterContext) -> AdapterResult:
        try:
            result = self.baseline_runner(item, context)
        except Exception as error:
            return AdapterResult("ABORTED", f"baseline_error:{type(error).__name__}:{error}")
        status = str(result.get("status", "NOT_COMPUTABLE")).upper()
        comparison = result.get("comparison")
        strict_pass = bool(comparison) and all(
            entry.get("strict_pass") is True
            for entry in comparison.values()
            if isinstance(entry, dict) and entry.get("status") == "COMPARABLE"
        ) and any(
            isinstance(entry, dict) and entry.get("status") == "COMPARABLE"
            for entry in comparison.values()
        )
        case_root = result.get("case_root")
        if case_root is None:
            case_root = context.evidence_root / context.case_id
        return AdapterResult(
            status="PASS" if (status == "PASS" and strict_pass) else (
                "NOT_COMPUTABLE" if status in ("NOT_COMPUTABLE", "PASS") else status),
            reason=result.get("reason", ""),
            case_root=Path(case_root),
            manifest_sha256=result.get("manifest_sha256"),
            lifecycle_state="SEALED",
            comparison=comparison,
            export_eligible=strict_pass,
        )


class RealMediaDropAdapter:
    """Explicit wrapper for the one audited real-media smoke workflow."""

    capabilities = AdapterCapabilities(
        name="mysql80-ext4-drop-smoke",
        version=1,
        execution_class="real-media",
        versions=frozenset({"8.0"}),
        filesystems=frozenset({"ext4"}),
        scenarios=frozenset({"drop"}),
        dataset_profiles=frozenset({"smoke"}),
        page_sizes=frozenset({16384}),
        row_formats=frozenset({"DYNAMIC"}),
    )

    def __init__(self, smoke_factory: SmokeFactory | None = None) -> None:
        self.smoke_factory = smoke_factory

    def run(self, item: MatrixItem, context: AdapterContext) -> AdapterResult:
        if self.smoke_factory is None:
            return AdapterResult("FAILED_SETUP", "real_media_smoke_factory_not_configured")
        smoke = self.smoke_factory(item, context)
        result = smoke.run_smoke()
        return AdapterResult(
            status=result.get("status", "NOT_COMPUTABLE"),
            reason="raw_forensic_evidence",
            case_root=Path(result["run_root"]),
            manifest_sha256=result.get("seal"),
            lifecycle_state="SEALED",
            comparison=None,
            export_eligible=False,
        )


def _phase_b_scenario_runner(runner: Any = None) -> Callable[[MatrixItem, AdapterContext], dict[str, Any]]:
    """Return a callable dispatching Phase B disposable filesystem disaster scenarios."""
    from .phase_b_general import ScenarioConfig, ScenarioDriver, _run

    resolved_runner = runner if runner is not None else _run

    def run(item: MatrixItem, context: AdapterContext) -> dict[str, Any]:
        rows = 20000 if item.dataset_profile == "smoke" else (
            100000 if item.dataset_profile == "100k" else 1000000)
        config = ScenarioConfig(
            evidence_root=context.evidence_root.parent,
            run_id=context.case_id,
            item=item,
            rows=rows,
            image_mb=2048,
            fresh_import=True,
        )
        driver = ScenarioDriver(config, runner=resolved_runner)
        return driver.execute()

    return run


class PhaseBDisasterAdapter:
    """Dispatches filesystem disaster scenarios on isolated media images."""

    capabilities = AdapterCapabilities(
        name="phase-b-filesystem-disaster",
        version=1,
        execution_class="disposable",
        versions=frozenset({"5.6", "5.7", "8.0", "8.4", "9.0"}),
        filesystems=frozenset({"ext4", "xfs"}),
        scenarios=frozenset({"drop", "truncate", "rm-rf"}),
        dataset_profiles=frozenset({"smoke", "100k", "1m"}),
        page_sizes=frozenset({4096, 8192, 16384, 32768, 65536}),
        row_formats=frozenset({"DYNAMIC"}),
    )

    def __init__(self, runner: Callable[[MatrixItem, AdapterContext], dict[str, Any]] | None = None) -> None:
        self.runner = runner

    def run(self, item: MatrixItem, context: AdapterContext) -> AdapterResult:
        if self.runner is None:
            return AdapterResult("FAILED_SETUP", "phase_b_runner_not_configured")
        try:
            record = self.runner(item, context)
        except Exception as error:
            return AdapterResult("ABORTED", f"phase_b_error:{type(error).__name__}:{error}")
        status = str(record.get("status", "NOT_COMPUTABLE")).upper()
        verification = record.get("verification", {})
        runs = verification.get("runs", {})
        comparisons = {}
        for label, run in runs.items():
            if isinstance(run, dict) and "comparison" in run:
                comparisons[label] = run["comparison"]
        strict_pass = bool(comparisons) and all(
            entry.get("strict_pass") is True for entry in comparisons.values()
        )
        case_root = context.evidence_root.parent / context.case_id
        return AdapterResult(
            status="PASS" if (status == "PASS" and strict_pass) else status,
            reason=";".join(verification.get("blockers", [])) if verification.get("blockers") else record.get("error", ""),
            case_root=case_root,
            manifest_sha256=record.get("seal"),
            lifecycle_state="SEALED",
            comparison=comparisons,
            export_eligible=strict_pass,
        )


def _corruption_scenario_runner(runner: Any = None) -> Callable[[MatrixItem, AdapterContext], dict[str, Any]]:
    """Return a callable dispatching bounded physical corruption matrix runs."""
    from .corruption_matrix import CorruptionScenarioDriver
    from .phase_b_general import ScenarioConfig, _run

    resolved_runner = runner if runner is not None else _run

    def run(item: MatrixItem, context: AdapterContext) -> dict[str, Any]:
        mutation_map = {
            "corruption-l1": "checksum_body_flip",
            "corruption-l2": "page_type_flip",
            "corruption-l3": "record_chain_break",
            "corruption-l4": "tail_truncate_partial_page",
            "tde-unvalidated": "tde_candidate",
        }
        mutation = mutation_map.get(item.scenario, "checksum_body_flip")
        rows = 20000 if item.dataset_profile == "smoke" else (
            100000 if item.dataset_profile == "100k" else 1000000)
        config = ScenarioConfig(
            evidence_root=context.evidence_root.parent,
            run_id=context.case_id,
            item=item,
            rows=rows,
            image_mb=2048,
            fresh_import=False,
        )
        driver = CorruptionScenarioDriver(config, mutation=mutation, runner=resolved_runner)
        return driver.execute()

    return run


class CorruptionMatrixAdapter:
    """Dispatches bounded physical mutations and verifies fail-closed boundaries."""

    capabilities = AdapterCapabilities(
        name="corruption-fail-closed",
        version=1,
        execution_class="disposable",
        versions=frozenset({"5.6", "5.7", "8.0", "8.4", "9.0"}),
        filesystems=frozenset({"ext4", "xfs"}),
        scenarios=frozenset({"corruption-l1", "corruption-l2", "corruption-l3", "corruption-l4", "tde-unvalidated"}),
        dataset_profiles=frozenset({"smoke", "100k", "1m"}),
        page_sizes=frozenset({4096, 8192, 16384, 32768, 65536}),
        row_formats=frozenset({"DYNAMIC", "COMPACT", "REDUNDANT", "COMPRESSED"}),
    )

    def __init__(self, runner: Callable[[MatrixItem, AdapterContext], dict[str, Any]] | None = None) -> None:
        self.runner = runner

    def run(self, item: MatrixItem, context: AdapterContext) -> AdapterResult:
        if self.runner is None:
            return AdapterResult("FAILED_SETUP", "corruption_runner_not_configured")
        try:
            record = self.runner(item, context)
        except Exception as error:
            return AdapterResult("ABORTED", f"corruption_error:{type(error).__name__}:{error}")
        status = str(record.get("status", "NOT_COMPUTABLE")).upper()
        case_root = context.evidence_root.parent / context.case_id
        is_pass = status in ("PASS_BOUNDED_STRUCTURED", "PASS_EVIDENCE_ONLY", "PASS")
        return AdapterResult(
            status="PASS" if is_pass else ("FAIL" if status == "PRODUCT_FAIL" else status),
            reason=";".join(record.get("verdict_reasons", [])) or record.get("error", ""),
            case_root=case_root,
            manifest_sha256=record.get("seal"),
            lifecycle_state="SEALED",
            comparison=record.get("verification", {}).get("runs"),
            export_eligible=False,
        )


def _podman_available() -> bool:
    return shutil.which("podman") is not None


def default_adapters() -> tuple[ValidationAdapter, ...]:
    """Return the production adapter set.

    The baseline, Phase B disaster, and corruption adapters are wired to audited
    runners when podman is available; otherwise they stay registered but will
    fail closed with FAILED_SETUP so missing prerequisites can never produce PASS.
    """
    if _podman_available():
        baseline = OfflineExperimentAdapter(
            baseline_runner=_phase_a_baseline_runner())
        disaster = PhaseBDisasterAdapter(
            runner=_phase_b_scenario_runner())
        corruption = CorruptionMatrixAdapter(
            runner=_corruption_scenario_runner())
    else:
        baseline = OfflineExperimentAdapter()
        disaster = PhaseBDisasterAdapter()
        corruption = CorruptionMatrixAdapter()
    return (baseline, disaster, corruption, RealMediaDropAdapter())


def adapter_for(item: MatrixItem, adapters: tuple[ValidationAdapter, ...],
                allow_real_media: bool = False) -> ValidationAdapter | None:
    matches = [adapter for adapter in adapters if adapter.capabilities.supports(item)]
    if len(matches) > 1:
        # Resolve real-media vs disposable: if allow_real_media is false, exclude real-media
        if not allow_real_media:
            matches = [a for a in matches if a.capabilities.execution_class != "real-media"]
        else:
            matches = [a for a in matches if a.capabilities.execution_class == "real-media"]
    if len(matches) > 1:
        raise ValueError(f"multiple adapters declare support for matrix case: {item}")
    return matches[0] if matches else None
