#!/usr/bin/env python3
import json
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from typing import Any

from scripts.disaster_harness.experiment import (
    CommandResult,
    ExperimentSpec,
    NativeSpec,
    OfflineSpec,
)
from scripts.disaster_harness.ground_truth import build_ground_truth
from scripts.disaster_harness.manifest import canonical_json, init_workspace, sha256_file
from scripts.disaster_validation.adapters import (
    AdapterCapabilities,
    AdapterContext,
    AdapterResult,
    OfflineExperimentAdapter,
)
from scripts.disaster_validation.matrix import matrix_item
from scripts.disaster_validation.orchestrator import (
    DisasterOrchestrator,
    OrchestrationConfig,
    OrchestrationError,
    case_identity,
    expand_requests,
    normalize_config,
)
from scripts.phase_b_disaster_orchestrator import apply_overrides, build_parser, load_config


def sealed_case(root: Path) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    manifest = root / "manifest.json"
    manifest.write_text('{"manifest_version":1}\n', encoding="utf-8")
    (root / "manifest.seal").write_text(sha256_file(manifest) + "\n", encoding="utf-8")
    return root


class RecordingAdapter:
    execution_class = "disposable"

    def __init__(self, name="offline-experiment", scenarios=frozenset({"baseline"}),
                 execution_class="disposable", result_factory=None):
        self.capabilities = AdapterCapabilities(
            name=name,
            version=1,
            execution_class=execution_class,
            versions=frozenset({"5.6", "5.7", "8.0", "8.4", "9.0"}),
            filesystems=frozenset({"ext4", "xfs"}),
            scenarios=scenarios,
            dataset_profiles=frozenset({"smoke", "100k", "1m"}),
            page_sizes=frozenset({4096, 8192, 16384, 32768, 65536}),
            row_formats=frozenset({"COMPACT", "REDUNDANT", "DYNAMIC", "COMPRESSED"}),
        )
        self.calls: list[AdapterContext] = []
        self._result_factory = result_factory

    def run(self, item, context):
        self.calls.append(context)
        if self._result_factory is not None:
            return self._result_factory(item, context)
        return AdapterResult("NOT_COMPUTABLE", "no_factory")


def base_config(evidence_root: Path, **overrides) -> OrchestrationConfig:
    config = OrchestrationConfig(
        run_id="phase-b",
        evidence_root=evidence_root,
        versions=("8.0",),
        filesystems=("ext4",),
        scenarios=("baseline",),
        dataset_profiles=("smoke",),
    )
    return replace(config, **overrides)


class ConfigValidationTests(unittest.TestCase):
    def test_normalization_sorts_and_rejects_duplicates(self):
        with tempfile.TemporaryDirectory() as temp:
            config = base_config(Path(temp), versions=("8.0", "5.6"))
            normalized = normalize_config(config)
            self.assertEqual(normalized.versions, ("5.6", "8.0"))
            with self.assertRaisesRegex(OrchestrationError, "duplicate"):
                normalize_config(base_config(Path(temp), scenarios=("baseline", "baseline")))
            with self.assertRaisesRegex(OrchestrationError, "must not be empty"):
                normalize_config(base_config(Path(temp), dataset_profiles=()))
            with self.assertRaisesRegex(OrchestrationError, "resume and retry"):
                normalize_config(base_config(Path(temp), resume=True, retry_case_ids=("x",)))

    def test_resume_and_retry_case_ids_must_be_stable_identities(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(OrchestrationError, "stable case identities"):
                normalize_config(base_config(Path(temp), retry_case_ids=("bad case",)))

    def test_config_loader_rejects_unknown_and_missing_fields(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            good = root / "good.json"
            good.write_text(json.dumps({
                "orchestration_version": 1,
                "run_id": "phase-b",
                "evidence_root": str(root),
                "versions": ["8.0"],
                "filesystems": ["ext4"],
                "scenarios": ["baseline"],
                "dataset_profiles": ["smoke"],
            }), encoding="utf-8")
            config = load_config(good)
            self.assertEqual(config.versions, ("8.0",))
            self.assertEqual(config.page_sizes, (16384,))

            bad = root / "bad.json"
            bad.write_text(json.dumps({"orchestration_version": 1, "run_id": "x", "unexpected": 1}),
                           encoding="utf-8")
            with self.assertRaisesRegex(OrchestrationError, "unknown configuration fields"):
                load_config(bad)
            missing = root / "missing.json"
            missing.write_text(json.dumps({"orchestration_version": 1}), encoding="utf-8")
            with self.assertRaisesRegex(OrchestrationError, "missing configuration fields"):
                load_config(missing)

    def test_cli_defaults_to_dry_run_and_requires_real_media_action(self):
        with tempfile.TemporaryDirectory() as temp:
            config = base_config(Path(temp))
            args = build_parser().parse_args(["--config", str(Path(temp) / "c.json")])
            self.assertTrue(apply_overrides(config, args).dry_run)
            execute = build_parser().parse_args(["--config", "c.json", "--execute"])
            self.assertFalse(apply_overrides(config, execute).dry_run)
            with self.assertRaises(SystemExit):
                build_parser().parse_args(["--config", "c.json", "--real-media-action", "unknown"])


class ExpansionTests(unittest.TestCase):
    def test_case_identity_covers_all_axes_and_is_deterministic(self):
        first = case_identity(matrix_item("8.0", "ext4", "baseline", "smoke", 16384, "DYNAMIC"))
        second = case_identity(matrix_item("8.0", "ext4", "baseline", "smoke", 16384, "DYNAMIC"))
        other = case_identity(matrix_item("8.0", "ext4", "baseline", "smoke", 8192, "DYNAMIC"))
        self.assertEqual(first, second)
        self.assertNotEqual(first, other)

    def test_duplicate_selection_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            config = base_config(root, versions=("8.0", "8.0"), page_sizes=(16384, 16384))
            with self.assertRaisesRegex(OrchestrationError, "duplicate"):
                expand_requests(config)

    def test_expansion_records_unsupported_matrix_items(self):
        with tempfile.TemporaryDirectory() as temp:
            requests = expand_requests(
                base_config(Path(temp), versions=("5.6",), page_sizes=(65536,)))
            self.assertEqual(len(requests), 1)
            self.assertEqual(requests[0].matrix.status, "UNSUPPORTED")
            self.assertEqual(requests[0].matrix.reason, "unsupported_page_size")


class DispatchTests(unittest.TestCase):
    def test_dry_run_never_dispatches_and_seals_plan(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            adapter = RecordingAdapter()
            result = DisasterOrchestrator((adapter,)).run(base_config(root))
            self.assertEqual(adapter.calls, [])
            self.assertEqual(result.status, "PLANNED")
            self.assertEqual(result.entries[0].status, "PLANNED")
            self.assertTrue((result.root / "manifest.seal").is_file())
            entry = result.document()["entries"][0]
            self.assertIn("mysql_version", entry)
            self.assertEqual(entry["export_eligible"], False)

    def test_unsupported_case_records_reason_without_dispatch(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            adapter = RecordingAdapter(scenarios=frozenset({"drop"}))
            result = DisasterOrchestrator((adapter,)).run(base_config(root, dry_run=False))
            self.assertEqual(adapter.calls, [])
            self.assertEqual(result.entries[0].status, "UNSUPPORTED")
            self.assertEqual(result.entries[0].reason, "no_audited_adapter")
            self.assertEqual(result.status, "COMPLETE_NONPASS")

    def test_adapter_cannot_match_a_nearby_scenario(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            adapter = RecordingAdapter(scenarios=frozenset({"drop"}))
            result = DisasterOrchestrator((adapter,)).run(
                base_config(root, scenarios=("truncate",), dry_run=False))
            self.assertEqual(adapter.calls, [])
            self.assertEqual(result.entries[0].status, "UNSUPPORTED")

    def test_strict_comparison_is_the_only_pass(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)

            def factory(_item, context):
                case_root = sealed_case(context.evidence_root)
                return AdapterResult(
                    "PASS", "", case_root, sha256_file(case_root / "manifest.json"), "SEALED",
                    {"0": {"status": "COMPARABLE", "strict_pass": True}}, True)

            adapter = RecordingAdapter(result_factory=factory)
            result = DisasterOrchestrator((adapter,)).run(base_config(root, dry_run=False))
            entry = result.entries[0]
            self.assertEqual(entry.status, "PASS")
            self.assertTrue(entry.strict_pass)
            self.assertTrue(entry.comparable)
            self.assertTrue(entry.export_eligible)
            self.assertEqual(result.status, "PASS")

    def test_raw_forensic_output_cannot_become_pass(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)

            def factory(_item, context):
                case_root = sealed_case(context.evidence_root)
                return AdapterResult("PASS", "claim", case_root, None, "SEALED", None, False)

            result = DisasterOrchestrator((RecordingAdapter(result_factory=factory),)).run(
                base_config(root, dry_run=False))
            self.assertEqual(result.entries[0].status, "NOT_COMPUTABLE")
            self.assertEqual(result.entries[0].reason, "pass_requires_strict_typed_comparison")

    def test_pass_requires_sealed_case_evidence(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)

            def factory(_item, context):
                unsealed = context.evidence_root / "unsealed"
                unsealed.mkdir()
                return AdapterResult("PASS", "", unsealed, None, "SEALED",
                                     {"0": {"status": "COMPARABLE", "strict_pass": True}}, True)

            result = DisasterOrchestrator((RecordingAdapter(result_factory=factory),)).run(
                base_config(root, dry_run=False))
            self.assertEqual(result.entries[0].status, "ABORTED")
            self.assertEqual(result.entries[0].reason, "pass_requires_sealed_case_evidence")

    def test_case_workspaces_are_immutable(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            orchestrator = DisasterOrchestrator((RecordingAdapter(),))
            first = orchestrator.run(base_config(root, dry_run=False))
            self.assertEqual(first.entries[0].status, "NOT_COMPUTABLE")
            with self.assertRaisesRegex(OrchestrationError, "non-empty workspace"):
                orchestrator.run(base_config(root, dry_run=False))


class ResumeRetryTests(unittest.TestCase):
    def _previous_pass(self, evidence_root: Path):
        def factory(_item, context):
            case_root = sealed_case(context.evidence_root)
            return AdapterResult("PASS", "", case_root, sha256_file(case_root / "manifest.json"),
                                 "SEALED", {"0": {"status": "COMPARABLE", "strict_pass": True}}, True)
        adapter = RecordingAdapter(result_factory=factory)
        return DisasterOrchestrator((adapter,)).run(
            base_config(evidence_root, run_id="phase-b", dry_run=False)), adapter

    def test_resume_reuses_only_sealed_case_without_redispatch(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            previous, _ = self._previous_pass(root)
            resumed_adapter = RecordingAdapter()
            resumed = DisasterOrchestrator((resumed_adapter,)).run(
                base_config(root, resume=True))
            self.assertEqual(resumed_adapter.calls, [])
            self.assertTrue(resumed.entries[0].resumed)
            self.assertEqual(resumed.entries[0].status, previous.entries[0].status)
            self.assertNotEqual(resumed.root, previous.root)

    def test_resume_rejects_terminal_unsealed_case(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            first = DisasterOrchestrator((RecordingAdapter(),)).run(base_config(root, dry_run=False))
            self.assertEqual(first.entries[0].status, "NOT_COMPUTABLE")
            with self.assertRaisesRegex(OrchestrationError, "non-sealed|without result metadata"):
                DisasterOrchestrator((RecordingAdapter(),)).run(base_config(root, resume=True))

    def test_retry_requires_prior_eligible_failure_and_new_workspace(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            case_id = case_identity(matrix_item("8.0", "ext4", "baseline", "smoke"))
            failing = RecordingAdapter(
                result_factory=lambda _item, _context: AdapterResult("FAILED_SETUP", "setup_failed"))
            first = DisasterOrchestrator((failing,)).run(base_config(root, dry_run=False))
            self.assertEqual(first.entries[0].status, "FAILED_SETUP")
            retry = DisasterOrchestrator((RecordingAdapter(),)).run(
                base_config(root, retry_case_ids=(case_id,), dry_run=False))
            self.assertEqual(len(retry.entries), 1)
            self.assertEqual(retry.entries[0].matrix.status, "PLANNED")
            self.assertIn("-retry-", retry.entries[0].case_root.name)

    def test_retry_rejects_unknown_case(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with self.assertRaisesRegex(OrchestrationError, "not selected"):
                DisasterOrchestrator((RecordingAdapter(),)).run(
                    base_config(root, retry_case_ids=("missing-case",), dry_run=False))


class OfflineAdapterContractTests(unittest.TestCase):
    def _bridge(self, case_root: Path):
        document = build_ground_truth(
            "db", "t", {"columns": [{"name": "id", "type": "int", "nullable": False}],
                        "primary_key": ["id"]}, [{"id": 1}], "9.0")

        def bridge(argv, timeout, input_text=""):
            if len(argv) > 1 and argv[1] == "run":
                name = argv[argv.index("--name") + 1]
                mount = argv[argv.index("-v") + 1]
                self.datadirs[name] = Path(mount.split(":", 1)[0])
                return CommandResult(list(argv), 0, "container-id", "", 0.0)
            if len(argv) > 1 and argv[1] == "exec" and "mysqladmin" in argv:
                return CommandResult(list(argv), 0, "mysqld is alive", "", 0.0)
            if len(argv) > 1 and argv[1] == "exec":
                name = argv[3] if argv[2] == "-i" else argv[2]
                (self.datadirs[name].parent / "native_result.json").write_text(
                    canonical_json(document) + "\n", encoding="utf-8")
                return CommandResult(list(argv), 0, "", "", 0.0)
            if len(argv) > 1 and argv[1] in {"stop", "logs", "rm"}:
                return CommandResult(list(argv), 0, "", "", 0.0)
            output_dir = Path(argv[argv.index("--output") + 1])
            (output_dir / "damaged_result.json").write_text(
                canonical_json(document) + "\n", encoding="utf-8")
            return CommandResult(list(argv), 0, "", "", 0.0)

        return bridge

    def test_offline_adapter_returns_strict_typed_pass_with_sealed_evidence(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            datadir = root / "datadir"
            (datadir / "db").mkdir(parents=True)
            (datadir / "db" / "table.ibd").write_bytes(b"pristine")
            binary = root / "offline"
            binary.write_bytes(b"binary")
            damaged = root / "damaged.ibd"
            damaged.write_bytes(b"damaged")
            self.datadirs = {}
            bridge = self._bridge(root)

            def factory(item, context):
                return ExperimentSpec(
                    workspace=context.evidence_root,
                    pristine_datadir=datadir,
                    damaged_input=damaged,
                    damaged_relative_path=Path("db/table.ibd"),
                    ground_truth=self._ground_truth_file(root),
                    native=NativeSpec(image="mysql:test", export_sql="SELECT 1"),
                    offline=OfflineSpec(binary=binary, ibd_arg="ibd"),
                    levels=(0,),
                    source_hash=sha256_file(damaged),
                )

            adapter = OfflineExperimentAdapter(experiment_factory=factory, runner=bridge)
            context = AdapterContext(case_id="case", evidence_root=root / "case")
            init_workspace(context.evidence_root, "disaster-validation-case")
            result = adapter.run(matrix_item("8.0", "ext4", "baseline", "smoke"), context)
            self.assertEqual(result.status, "PASS")
            self.assertEqual(result.lifecycle_state, "SEALED")
            self.assertTrue(result.export_eligible)
            self.assertTrue(sha256_file(result.case_root / "manifest.json"))

    def _ground_truth_file(self, root: Path) -> Path:
        path = root / "ground_truth.json"
        if not path.exists():
            path.write_text(canonical_json(build_ground_truth(
                "db", "t", {"columns": [{"name": "id", "type": "int", "nullable": False}],
                            "primary_key": ["id"]}, [{"id": 1}], "9.0")) + "\n",
                encoding="utf-8")
        return path


class RealMediaGateTests(unittest.TestCase):
    def _real_adapter(self, dispatched: list):
        class RealAdapter(RecordingAdapter):
            def run(self, item, context):
                dispatched.append(item)
                return self._result_factory(item, context) if self._result_factory else super().run(item, context)

        return RealAdapter(
            name="mysql80-ext4-drop-smoke",
            scenarios=frozenset({"drop"}),
            execution_class="real-media",
            result_factory=lambda _item, context: AdapterResult(
                "NOT_COMPUTABLE", "raw_forensic_evidence", sealed_case(context.evidence_root),
                None, "SEALED", None, False),
        )

    def test_real_media_requires_both_flags(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            dispatched: list = []
            adapter = self._real_adapter(dispatched)
            config = base_config(root, scenarios=("drop",), dry_run=False)
            blocked = DisasterOrchestrator((adapter,)).run(config)
            self.assertEqual(dispatched, [])
            self.assertEqual(blocked.entries[0].status, "BLOCKED")
            self.assertEqual(blocked.entries[0].reason, "real_media_authorization_required")

            allowed = DisasterOrchestrator((adapter,)).run(
                replace(config, allow_real_media=True, real_media_action="mysql80-ext4-drop"))
            self.assertEqual(len(dispatched), 1)
            self.assertEqual(allowed.entries[0].status, "NOT_COMPUTABLE")
            self.assertFalse(allowed.entries[0].export_eligible)

    def test_real_media_rejects_broad_selection(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            dispatched: list = []
            adapter = self._real_adapter(dispatched)
            config = base_config(root, scenarios=("baseline", "drop"), dry_run=False,
                                 allow_real_media=True, real_media_action="mysql80-ext4-drop")
            result = DisasterOrchestrator((adapter,)).run(config)
            self.assertEqual(dispatched, [])
            real_entry = next(e for e in result.entries if e.matrix.scenario == "drop")
            self.assertEqual(real_entry.status, "BLOCKED")
            self.assertEqual(real_entry.reason, "real_media_requires_single_exact_case")

    def test_real_media_never_dispatched_in_dry_run(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            dispatched: list = []
            adapter = self._real_adapter(dispatched)
            result = DisasterOrchestrator((adapter,)).run(
                base_config(root, scenarios=("drop",), allow_real_media=True,
                            real_media_action="mysql80-ext4-drop"))
            self.assertEqual(dispatched, [])
            self.assertEqual(result.entries[0].status, "PLANNED")


if __name__ == "__main__":
    unittest.main()
