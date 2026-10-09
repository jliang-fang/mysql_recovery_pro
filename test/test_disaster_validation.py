#!/usr/bin/env python3
import json
import tempfile
import unittest
from dataclasses import asdict
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest.mock import patch

from scripts.disaster_validation.device_safety import (
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
from scripts.disaster_validation.mysql80_ext4_drop_smoke import (
    CommandResult,
    Mysql80Ext4DropSmoke,
    SmokeConfig,
    SmokeRunError,
)
from scripts.disaster_validation.matrix import CAPABILITIES, expand_matrix, matrix_item
from scripts.disaster_validation.lifecycle import RunLifecycleError, ValidationRun
from scripts.disaster_validation.adapters import (
    AdapterContext,
    default_adapters,
    adapter_for,
)
from scripts.disaster_validation.csv_to_canonical import _parse_csv_cell
from scripts.disaster_validation.fresh_import_verify import (
    _decode_tsv_cell,
    build_ddl,
    sql_literal,
)
from scripts.disaster_validation import fixtures as validation_fixtures
from scripts.disaster_validation.stage1_qualification import Stage1QualificationRunner
from scripts.disaster_validation.phase_a_offline_orchestrator import (
    REQUIRED_CONTRACT_KEYS,
    PhaseAOrchestrator,
    build_schema_argument,
    parse_recovered_sql,
    sql_rows_to_canonical_dataset,
)
from scripts.disaster_harness.ground_truth import build_ground_truth, canonical_value
from scripts.disaster_harness.native_export import connection_collation
from scripts.disaster_harness.manifest import sha256_bytes


IDENTITY = DeviceIdentity(
    path=ALLOWED_DEVICE,
    major_minor="259:3",
    size_bytes=214748364800,
    serial="test-serial",
    model="test-model",
    wwn="test-wwn",
    filesystem_uuid="test-uuid",
    filesystem_type="ext4",
)


def preflight_report():
    report = {
        "status": "PRECHECKED",
        "device": asdict(IDENTITY),
    }
    report["preflight_sha256"] = sha256_bytes(b"preflight")
    return report


class MatrixTests(unittest.TestCase):
    def test_declares_all_supported_version_profiles(self):
        self.assertEqual(set(CAPABILITIES), {"5.6", "5.7", "8.0", "8.4", "9.0"})
        self.assertEqual(CAPABILITIES["5.6"].connection_collation, "utf8mb4_unicode_ci")
        self.assertEqual(CAPABILITIES["8.0"].connection_collation, "utf8mb4_0900_ai_ci")
        self.assertFalse(CAPABILITIES["5.6"].supports_json)
        self.assertTrue(CAPABILITIES["5.7"].supports_generated_columns)

    def test_rejects_invalid_capability_combinations(self):
        cases = [
            (("10.0", "ext4", "baseline", "smoke"), "unknown_mysql_version"),
            (("8.0", "btrfs", "baseline", "smoke"), "unknown_filesystem"),
            (("8.0", "ext4", "overwrite", "smoke"), "unknown_scenario"),
            (("8.0", "ext4", "baseline", "10m"), "unknown_dataset_profile"),
            (("5.6", "ext4", "baseline", "smoke", 65536), "unsupported_page_size"),
            (("8.0", "ext4", "baseline", "smoke", 16384, "COMPACT"), "unsupported_row_format"),
        ]
        for arguments, reason in cases:
            with self.subTest(reason=reason):
                item = matrix_item(*arguments)
                self.assertEqual(item.status, "UNSUPPORTED")
                self.assertEqual(item.reason, reason)

    def test_expansion_preserves_unsupported_items(self):
        items = expand_matrix(["5.6", "8.0"], ["ext4"], ["baseline"], ["smoke"], 65536)
        self.assertEqual(len(items), 2)
        self.assertEqual(items[0].status, "UNSUPPORTED")
        self.assertEqual(items[0].reason, "unsupported_page_size")
        self.assertEqual(items[1].status, "PLANNED")

    def test_native_export_collation_uses_version_capability(self):
        self.assertEqual(connection_collation("5.6.51-log"), "utf8mb4_unicode_ci")
        self.assertEqual(connection_collation("5.7"), "utf8mb4_unicode_ci")
        self.assertEqual(connection_collation("8.0.44"), "utf8mb4_0900_ai_ci")
        with self.assertRaisesRegex(ValueError, "unsupported MySQL version"):
            connection_collation("10.0")

    def test_phase_a_schema_preserves_nullability_and_charset(self):
        schema = {
            "primary_key": ["id"],
            "columns": [
                {"name": "id", "type": "BIGINT", "nullable": False, "charset": None},
                {"name": "signed_value", "type": "INT", "nullable": False, "charset": None},
                {"name": "nullable_text", "type": "VARCHAR(128)", "nullable": True, "charset": "utf8mb4"},
                {"name": "blob_value", "type": "BLOB", "nullable": True, "charset": None},
            ],
        }

        self.assertEqual(
            build_schema_argument(schema),
            "id:BIGINT:pk:notnull,signed_value:INT:notnull,"
            "nullable_text:VARCHAR(128):utf8mb4,blob_value:BLOB",
        )

    def test_csv_adapter_matches_null_and_binary_export_encoding(self):
        nullable_text = {"type": "VARCHAR(128)", "nullable": True}
        blob = {"type": "BLOB", "nullable": True}

        self.assertIsNone(_parse_csv_cell("", nullable_text))
        self.assertEqual(_parse_csv_cell("0100", blob), b"\x01\x00")
        self.assertEqual(_parse_csv_cell("not-hex", blob), b"not-hex")
    def test_fixture_schema_file_uses_cli_object_shape(self):
        spec = validation_fixtures.build_suite("numeric", "8.0", row_format="DYNAMIC")
        document = spec.schema_json()
        self.assertEqual(document["table_name"], "t_numeric")
        self.assertIn("columns", document)
        self.assertTrue(document["indexes"][0]["is_primary"])
        self.assertEqual(document["columns"][0]["type"], 9)
        self.assertNotIn("mysql_type_id", document["columns"][0])

    def test_stage1_defaults_to_separately_verifiable_suites(self):
        runner = Stage1QualificationRunner("stage1-test", Path("/tmp"), Path("/tmp/stage1-mount"))
        self.assertEqual(runner.suite_names,
                         ("numeric", "temporal", "text", "structured", "composite"))
        self.assertEqual(runner.rows, 100000)

    def test_rejects_any_device_other_than_authorized_target(self):
        with self.assertRaisesRegex(DeviceSafetyError, "only permits"):
            read_device_identity("/dev/null")

    def test_preflight_collects_only_facts_when_isolation_is_proven(self):
        with tempfile.TemporaryDirectory() as temp:
            evidence_root = Path(temp)
            with patch("scripts.disaster_validation.device_safety.read_device_identity", return_value=IDENTITY), \
                 patch("scripts.disaster_validation.device_safety._mounts_for", return_value=[]), \
                 patch("scripts.disaster_validation.device_safety._holders", return_value=[]), \
                 patch("scripts.disaster_validation.device_safety._storage_dependencies", return_value={}), \
                 patch("scripts.disaster_validation.device_safety._optional_json_command", return_value={"filesystems": []}):
                report = preflight(ALLOWED_DEVICE, evidence_root)

        self.assertEqual(report["status"], "PRECHECKED")
        self.assertEqual(report["device"], asdict(IDENTITY))
        self.assertEqual(len(report["preflight_sha256"]), 64)
        self.assertFalse(report["device_read_only"])

    def test_preflight_rejects_evidence_on_test_device(self):
        with tempfile.TemporaryDirectory() as temp:
            evidence_root = Path(temp)
            with patch("scripts.disaster_validation.device_safety.read_device_identity", return_value=IDENTITY), \
                 patch("scripts.disaster_validation.device_safety._mounts_for", return_value=[]), \
                 patch("scripts.disaster_validation.device_safety._optional_json_command", return_value={
                     "filesystems": [{"source": ALLOWED_DEVICE}]
                 }):
                with self.assertRaisesRegex(DeviceSafetyError, "evidence root is located"):
                    preflight(ALLOWED_DEVICE, evidence_root)

    def test_controlled_mount_preflight_requires_exact_ext4_rw_mount(self):
        with tempfile.TemporaryDirectory() as temp:
            evidence_root = Path(temp) / "evidence"
            mount_point = Path(temp) / "mounted-media"
            evidence_root.mkdir()
            mount_point.mkdir()
            mount = {
                "source": ALLOWED_DEVICE,
                "target": str(mount_point),
                "fstype": "ext4",
                "options": "rw,relatime",
            }
            with patch("scripts.disaster_validation.device_safety.read_device_identity", return_value=IDENTITY), \
                 patch("scripts.disaster_validation.device_safety._mounts_for", return_value=[mount]), \
                 patch("scripts.disaster_validation.device_safety._holders", return_value=[]), \
                 patch("scripts.disaster_validation.device_safety._storage_dependencies", return_value={}), \
                 patch("scripts.disaster_validation.device_safety._optional_json_command", return_value={"filesystems": []}):
                report = preflight_controlled_mount(ALLOWED_DEVICE, evidence_root, mount_point)
                self.assertEqual(report["status"], "PRECHECKED")
                with self.assertRaisesRegex(DeviceSafetyError, "test device is mounted"):
                    preflight(ALLOWED_DEVICE, evidence_root)

                for invalid_mount, message in (
                    ({**mount, "target": str(Path(temp) / "wrong")}, "target"),
                    ({**mount, "fstype": "xfs"}, "ext4"),
                    ({**mount, "options": "ro,relatime"}, "read-write"),
                ):
                    with patch("scripts.disaster_validation.device_safety._mounts_for", return_value=[invalid_mount]):
                        with self.assertRaisesRegex(DeviceSafetyError, message):
                            preflight_controlled_mount(ALLOWED_DEVICE, evidence_root, mount_point)
                with patch("scripts.disaster_validation.device_safety._mounts_for", return_value=[mount, mount]):
                    with self.assertRaisesRegex(DeviceSafetyError, "exactly one"):
                        preflight_controlled_mount(ALLOWED_DEVICE, evidence_root, mount_point)

    def test_approval_token_binds_action_run_preflight_and_identity(self):
        report = preflight_report()
        token = issue_approval_token(report, "smoke-001", "format", nonce="a" * 32)
        validate_approval_token(token, IDENTITY, "smoke-001", "format", report["preflight_sha256"])
        setrw_token = issue_approval_token(report, "smoke-001", "setrw", nonce="d" * 32)
        validate_approval_token(setrw_token, IDENTITY, "smoke-001", "setrw", report["preflight_sha256"])

        with self.assertRaisesRegex(DeviceSafetyError, "action or run ID"):
            validate_approval_token(token, IDENTITY, "smoke-001", "mount", report["preflight_sha256"])
        with self.assertRaisesRegex(DeviceSafetyError, "bind to this preflight"):
            validate_approval_token(token, IDENTITY, "smoke-001", "format", "b" * 64)
        drifted = DeviceIdentity(**{**asdict(IDENTITY), "serial": "different"})
        with self.assertRaisesRegex(DeviceSafetyError, "identity drifted"):
            validate_approval_token(token, drifted, "smoke-001", "format", report["preflight_sha256"])

    def test_approval_token_expiry_and_non_overwrite(self):
        report = preflight_report()
        now = datetime.now(timezone.utc)
        expired = ApprovalToken(
            token_version=1,
            run_id="smoke-001",
            action="format",
            device=IDENTITY,
            preflight_sha256=report["preflight_sha256"],
            issued_at=(now - timedelta(minutes=2)).isoformat(),
            expires_at=(now - timedelta(minutes=1)).isoformat(),
            nonce="b" * 32,
        )
        with self.assertRaisesRegex(DeviceSafetyError, "expired"):
            validate_approval_token(expired, IDENTITY, "smoke-001", "format", report["preflight_sha256"], now)

        token = issue_approval_token(report, "smoke-001", "format", nonce="c" * 32)
        with tempfile.TemporaryDirectory() as temp:
            first = write_approval_token(Path(temp), token)
            self.assertTrue(first.is_file())
            with self.assertRaisesRegex(DeviceSafetyError, "refusing to overwrite"):
                write_approval_token(Path(temp), token)


class Mysql80Ext4DropSmokeTests(unittest.TestCase):
    def _report(self):
        return preflight_report()

    def _truth_capture(self, _socket, _database, _table, _version, output):
        truth = {
            "status": "SEALED_READY",
            "schema": {"primary_key": ["id"]},
            "dataset_sha256": "a" * 64,
        }
        output.write_text("{}\n", encoding="utf-8")
        return truth

    def test_raw_recovery_requires_verified_read_only_state(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary = root / "recovery"
            binary.touch()
            smoke = Mysql80Ext4DropSmoke(
                SmokeConfig(root, "smoke-001", root / "mount", binary),
                identity_reader=lambda _device: IDENTITY,
            )
            smoke.run = type("Run", (), {"root": root})()
            with self.assertRaisesRegex(SmokeRunError, "verified kernel read-only"):
                smoke._run_raw_recovery()

    def test_smoke_uses_controlled_drop_preflight_and_read_only_gate(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            evidence_root = root / "evidence"
            mount_point = root / "mount"
            evidence_root.mkdir()
            mount_point.mkdir()
            binary = root / "recovery"
            binary.touch()
            commands = []
            generic_stages = []
            controlled_stages = []

            def generic_preflight(_device, _evidence_root):
                generic_stages.append("generic")
                return self._report()

            def controlled_preflight(_device, _evidence_root, expected_mount):
                self.assertEqual(expected_mount, mount_point)
                controlled_stages.append("controlled")
                return self._report()

            def runner(argv, _timeout, _input):
                commands.append(argv)
                if argv[:4] == ["podman", "image", "inspect", "--format"]:
                    return CommandResult(argv, 0, CAPABILITIES["8.0"].image_digest + "\n", "")
                if argv[:2] == ["blockdev", "--getro"]:
                    return CommandResult(argv, 0, "1\n", "")
                return CommandResult(argv, 0, "", "")

            smoke = Mysql80Ext4DropSmoke(
                SmokeConfig(evidence_root, "smoke-001", mount_point, binary),
                runner=runner,
                preflight_fn=generic_preflight,
                controlled_mount_preflight_fn=controlled_preflight,
                identity_reader=lambda _device: IDENTITY,
                truth_capture=self._truth_capture,
            )
            result = smoke.run_smoke()

            self.assertEqual(result["status"], "NOT_COMPUTABLE")
            self.assertEqual(len(controlled_stages), 2)
            self.assertGreaterEqual(len(generic_stages), 5)
            unmount = commands.index(["umount", str(mount_point)])
            setro = commands.index(["blockdev", "--setro", ALLOWED_DEVICE])
            getro = commands.index(["blockdev", "--getro", ALLOWED_DEVICE])
            recovery = next(index for index, argv in enumerate(commands) if "--disk-carving" in argv)
            self.assertLess(unmount, setro)
            self.assertLess(setro, getro)
            self.assertLess(getro, recovery)

    def test_smoke_stops_before_recovery_when_read_only_verification_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            evidence_root = root / "evidence"
            mount_point = root / "mount"
            evidence_root.mkdir()
            mount_point.mkdir()
            binary = root / "recovery"
            binary.touch()
            commands = []

            def runner(argv, _timeout, _input):
                commands.append(argv)
                if argv[:4] == ["podman", "image", "inspect", "--format"]:
                    return CommandResult(argv, 0, CAPABILITIES["8.0"].image_digest + "\n", "")
                if argv[:2] == ["blockdev", "--getro"]:
                    return CommandResult(argv, 0, "0\n", "")
                return CommandResult(argv, 0, "", "")

            smoke = Mysql80Ext4DropSmoke(
                SmokeConfig(evidence_root, "smoke-001", mount_point, binary),
                runner=runner,
                preflight_fn=lambda _device, _evidence_root: self._report(),
                controlled_mount_preflight_fn=lambda _device, _evidence_root, _mount: self._report(),
                identity_reader=lambda _device: IDENTITY,
                truth_capture=self._truth_capture,
            )
            with self.assertRaisesRegex(SmokeRunError, "did not enter kernel read-only"):
                smoke.run_smoke()
            self.assertFalse(any("--disk-carving" in argv for argv in commands))

    def test_lifecycle_rejects_skips_and_seals_reported_run(self):
        matrix = matrix_item("8.0", "ext4", "baseline", "smoke")
        with tempfile.TemporaryDirectory() as temp:
            run = ValidationRun.create(Path(temp), "smoke-001", matrix)
            self.assertEqual(run.state, "PLANNED")
            with self.assertRaisesRegex(RunLifecycleError, "invalid run state transition"):
                run.transition("PERSISTED")
            for state in ("PRECHECKED", "MEDIA_PREPARED", "DATASET_CREATED", "TRUTH_SEALED",
                          "PERSISTED", "SCENARIO_APPLIED", "RECOVERY_ATTEMPTED",
                          "IMPORT_VALIDATED", "REPORTED"):
                run.transition(state)
            digest = run.seal({"result": "PASS"})
            self.assertEqual(len(digest), 64)
            self.assertEqual(run.state, "SEALED")
            loaded = ValidationRun.load(run.root)
            self.assertEqual(loaded.state, "SEALED")
            self.assertEqual(loaded.events[-1].state, "SEALED")
            with self.assertRaisesRegex(RunLifecycleError, "terminal state"):
                loaded.transition("ABORTED")

    def test_lifecycle_rejects_invalid_matrix_and_reused_run_directory(self):
        invalid = matrix_item("5.6", "ext4", "baseline", "smoke", 65536)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with self.assertRaisesRegex(RunLifecycleError, "matrix status"):
                ValidationRun.create(root, "bad", invalid)
            matrix = matrix_item("8.0", "ext4", "baseline", "smoke")
            ValidationRun.create(root, "smoke-001", matrix)


class PhaseABaselineContractTests(unittest.TestCase):
    """Exercise Phase A contract gating and dual-format comparison with a fake runner."""

    _SCHEMA = {
        "primary_key": ["id"],
        "columns": [
            {"name": "id", "type": "BIGINT", "nullable": False, "charset": None},
            {"name": "nullable_text", "type": "VARCHAR(128)", "nullable": True,
             "charset": "utf8mb4"},
            {"name": "blob_value", "type": "BLOB", "nullable": True, "charset": None},
        ],
    }
    _ROWS = (
        {"id": 1, "nullable_text": "alpha", "blob_value": bytes.fromhex("0a0b")},
        {"id": 2, "nullable_text": None, "blob_value": None},
    )

    @classmethod
    def truth_doc(cls):
        return build_ground_truth("db", "t", cls._SCHEMA, list(cls._ROWS), "8.0")

    @staticmethod
    def _contract(**overrides):
        contract = {
            "format_version": 2, "table_name": "t", "success": True,
            "classification": "structured", "structured_rows": 2,
            "evidence_fragments": 0, "schema_source": "argument",
            "schema_trust_level": "trusted", "damage_level": "none",
            "recovery_method": "index", "ddl_variation_detected": False,
            "export_success": True, "export_rejection_reason": "",
            "total_pages_scanned": 1, "skipped_pages": 0, "damaged_pages": 0,
            "checksum_mismatches": 0, "header_damage_pages": 0,
            "importable": False, "reason": "", "source_ibd": "t.ibd",
            "compression_status": "none", "logical_page_size": 16384,
            "physical_page_size": 16384,
            "compression_classification_basis": "none",
            "decoder_validated": True, "compression_rejection_reason": "",
        }
        contract.update(overrides)
        return contract

    def _runner(self, tmp, *, csv_text=None, sql_text=None, contract_overrides=None,
                missing_contract=(), skip_artifacts=()):
        """Return a fake runner plus the captured ibd path inside the run dir."""
        overrides = contract_overrides or {}
        captured = {}
        commands = []

        def runner(argv, _timeout, input_text=""):
            commands.append(list(argv))
            if argv[:2] == ["podman", "run"]:
                mount = argv[argv.index("-v") + 1]
                datadir = Path(mount.split(":", 1)[0])
                table_dir = datadir / "phase_a_db"
                table_dir.mkdir(parents=True, exist_ok=True)
                (table_dir / "t_smoke.ibd").write_bytes(b"tablespace")
                return CommandResult(argv, 0, "container-id", "")
            if argv[:2] == ["podman", "exec"]:
                if "mysqladmin" in argv:
                    return CommandResult(argv, 0, "mysqld is alive", "")
                sql = input_text or argv[-1]
                if "FLUSH TABLES" in sql or "UNLOCK TABLES" in sql:
                    return CommandResult(argv, 0, "", "")
                return self._mysql_response(sql)
            if str(argv[0]) == str(tmp / "recovery"):
                def flag(name):
                    prefix = f"--{name}="
                    return next(
                        a.split("=", 1)[1] for a in argv if a.startswith(prefix))
                captured["ibd"] = Path(flag("ibd"))
                out_dir = Path(flag("output"))
                fmt = flag("format")
                result_json = Path(flag("result-json"))
                if fmt not in missing_contract:
                    result_json.write_text(
                        json.dumps(self._contract(**overrides)) + "\n", encoding="utf-8")
                if fmt == "csv" and "csv" not in skip_artifacts:
                    (out_dir / "recovered.csv").write_text(
                        csv_text if csv_text is not None else self._csv_text(),
                        encoding="utf-8")
                if fmt == "sql" and "sql" not in skip_artifacts:
                    (out_dir / "recovered.sql").write_text(
                        sql_text if sql_text is not None else self._sql_text(),
                        encoding="utf-8")
                return CommandResult(argv, 0, "", "")
            return CommandResult(argv, 0, "", "")

        return runner, captured

    def _mysql_response(self, sql):
        if "information_schema.COLUMNS" in sql:
            lines = [
                "COLUMN_NAME\tORDINAL_POSITION\tCOLUMN_TYPE\tIS_NULLABLE\t"
                "CHARACTER_MAXIMUM_LENGTH\tNUMERIC_PRECISION\tNUMERIC_SCALE\t"
                "CHARACTER_SET_NAME\tCOLLATION_NAME",
                "id\t1\tbigint\tNO\tNULL\t19\t0\tNULL\tNULL",
                "nullable_text\t2\tvarchar(128)\tYES\t128\tNULL\tNULL\tutf8mb4\tutf8mb4_bin",
                "blob_value\t3\tblob\tYES\tNULL\tNULL\tNULL\tNULL\tNULL",
            ]
            return CommandResult([], 0, "\n".join(lines) + "\n", "")
        if "KEY_COLUMN_USAGE" in sql:
            return CommandResult([], 0, "COLUMN_NAME\nid\n", "")
        if "CONCAT" in sql and "ORDER BY" in sql:
            lines = [
                "id\tnullable_text\tblob_value",
                "1\talpha\t0x0A0B",
                "2\tNULL\tNULL",
            ]
            return CommandResult([], 0, "\n".join(lines) + "\n", "")
        return CommandResult([], 0, "", "")

    def _csv_text(self):
        return "id,nullable_text,blob_value\n1,alpha,0a0b\n2,,\n"

    def _sql_text(self):
        return (
            "INSERT INTO `recovered_table` (`id`, `nullable_text`, `blob_value`) "
            "VALUES\n"
            "INSERT INTO `recovered_table` (`id`, `nullable_text`, `blob_value`) "
            "VALUES ('1','alpha','0a0b');\n"
            "INSERT INTO `recovered_table` (`id`, `nullable_text`, `blob_value`) "
            "VALUES ('2',NULL,NULL);\n"
        )

    def _execute(self, tmp, runner):
        (tmp / "recovery").write_bytes(b"binary")
        item = matrix_item("8.0", "ext4", "baseline", "smoke")
        orchestrator = PhaseAOrchestrator(
            run_root=tmp, item=item, binary=tmp / "recovery",
            run_id="case", runner=runner)
        return orchestrator.execute()

    def test_sql_rows_retyped_to_canonical_dataset(self):
        tmp = Path(tempfile.mkdtemp())
        parsed = parse_recovered_sql(self._write(tmp / "r.sql", self._sql_text()))
        self.assertEqual(len(parsed), 2)
        doc = sql_rows_to_canonical_dataset(parsed, "db", "t", self._SCHEMA, "8.0")
        self.assertEqual(doc["status"], "SEALED_READY")
        self.assertEqual(doc["row_count"], 2)
        truth = self.truth_doc()
        self.assertEqual(
            [r["row_hash"] for r in doc["rows"]],
            [r["row_hash"] for r in truth["rows"]])

    def _write(self, path: Path, text: str) -> Path:
        path.write_text(text, encoding="utf-8")
        return path

    def test_strict_typed_pass_reaches_import_validated_and_seal(self):
        with tempfile.TemporaryDirectory() as temp:
            tmp = Path(temp)
            runner, _captured = self._runner(tmp)
            result = self._execute(tmp, runner)
            self.assertEqual(result["status"], "PASS")

            run = ValidationRun.load(tmp / "case")
            states = [event.state for event in run.events]
            self.assertIn("RECOVERY_ATTEMPTED", states)
            self.assertIn("IMPORT_VALIDATED", states)
            self.assertLess(states.index("RECOVERY_ATTEMPTED"),
                            states.index("IMPORT_VALIDATED"))
            self.assertLess(states.index("IMPORT_VALIDATED"),
                            states.index("REPORTED"))
            self.assertEqual(states[-1], "SEALED")
            self.assertIn("truth_vs_csv", result["comparison"])
            self.assertIn("truth_vs_sql", result["comparison"])
            self.assertIn("csv_vs_sql", result["comparison"])
            for entry in result["comparison"].values():
                self.assertEqual(entry["status"], "COMPARABLE")
                self.assertTrue(entry["strict_pass"])

    def test_missing_result_contract_blocks_pass_and_seals_not_computable(self):
        with tempfile.TemporaryDirectory() as temp:
            tmp = Path(temp)
            runner, _ = self._runner(tmp, missing_contract=("csv",))
            result = self._execute(tmp, runner)
            self.assertEqual(result["status"], "NOT_COMPUTABLE")
            self.assertIn("csv_contract_rejected", result["reason"])
            run = ValidationRun.load(tmp / "case")
            self.assertEqual(run.state, "SEALED")

    def test_rejected_export_contract_is_not_computable(self):
        with tempfile.TemporaryDirectory() as temp:
            tmp = Path(temp)
            runner, _ = self._runner(
                tmp,
                contract_overrides={
                    "export_success": False,
                    "export_rejection_reason": "compression_uncertain",
                })
            result = self._execute(tmp, runner)
            self.assertEqual(result["status"], "NOT_COMPUTABLE")
            self.assertIn("csv_contract_rejected", result["reason"])
            self.assertIn("sql_contract_rejected", result["reason"])

    def test_sql_csv_disagreement_is_not_pass(self):
        with tempfile.TemporaryDirectory() as temp:
            tmp = Path(temp)
            wrong_sql = self._sql_text().replace("'1','alpha'", "'1','beta'")
            runner, _ = self._runner(tmp, sql_text=wrong_sql)
            result = self._execute(tmp, runner)
            self.assertEqual(result["status"], "NOT_COMPUTABLE")
            self.assertIn("sql_strict_compare_failed", result["reason"])
            self.assertIn("csv_sql_disagreement", result["reason"])

    def test_missing_csv_artifact_blocks_pass(self):
        with tempfile.TemporaryDirectory() as temp:
            tmp = Path(temp)
            runner, _ = self._runner(tmp, skip_artifacts=("csv",))
            result = self._execute(tmp, runner)
            self.assertEqual(result["status"], "NOT_COMPUTABLE")
            self.assertIn("csv_artifact_missing", result["reason"])

    def test_malformed_contract_blocks_pass(self):
        with tempfile.TemporaryDirectory() as temp:
            tmp = Path(temp)
            (tmp / "recovery").write_bytes(b"binary")

            def runner(argv, _timeout, input_text=""):
                if argv[:2] == ["podman", "run"]:
                    mount = argv[argv.index("-v") + 1]
                    datadir = Path(mount.split(":", 1)[0])
                    table_dir = datadir / "phase_a_db"
                    table_dir.mkdir(parents=True, exist_ok=True)
                    (table_dir / "t_smoke.ibd").write_bytes(b"tablespace")
                    return CommandResult(argv, 0, "container-id", "")
                if argv[:2] == ["podman", "exec"]:
                    if "mysqladmin" in argv:
                        return CommandResult(argv, 0, "mysqld is alive", "")
                    sql = input_text or argv[-1]
                    if "FLUSH TABLES" in sql or "UNLOCK TABLES" in sql:
                        return CommandResult(argv, 0, "", "")
                    return self._mysql_response(sql)
                if str(argv[0]) == str(tmp / "recovery"):
                    def flag(name):
                        prefix = f"--{name}="
                        return next(a.split("=", 1)[1] for a in argv
                                    if a.startswith(prefix))
                    result_json = Path(flag("result-json"))
                    result_json.write_text('{"format_version":1}\n', encoding="utf-8")
                    return CommandResult(argv, 0, "", "")
                return CommandResult(argv, 0, "", "")

            result = self._execute(tmp, runner)
            self.assertEqual(result["status"], "NOT_COMPUTABLE")
            self.assertIn("csv_contract_rejected", result["reason"])
            self.assertIn("sql_contract_rejected", result["reason"])

    def test_baseline_path_touches_only_orchestrator_owned_inputs(self):
        with tempfile.TemporaryDirectory() as temp:
            tmp = Path(temp)
            runner, captured = self._runner(tmp)
            result = self._execute(tmp, runner)
            self.assertEqual(result["status"], "PASS")
            self.assertEqual(
                captured["ibd"],
                (tmp / "case" / "persistent-input" / "t_smoke.ibd").resolve())


class DefaultAdapterSelectionTests(unittest.TestCase):
    def test_default_adapters_no_longer_fail_setup_when_podman_available(self):
        item = matrix_item("8.0", "ext4", "baseline", "smoke")
        context = AdapterContext(case_id="case", evidence_root=Path(tempfile.mkdtemp()))

        with patch("scripts.disaster_validation.adapters._podman_available",
                   return_value=False):
            adapters = default_adapters()
            baseline = adapter_for(item, adapters)
            result = baseline.run(item, context)
            self.assertEqual(result.status, "FAILED_SETUP")
            self.assertEqual(result.reason, "offline_experiment_factory_not_configured")

        with patch("scripts.disaster_validation.adapters._podman_available",
                   return_value=True):
            adapters = default_adapters()
            baseline = adapter_for(item, adapters)
            self.assertIsNotNone(baseline.baseline_runner)


class FreshImportLiteralTests(unittest.TestCase):
    """The clean-server import renderer must emit literals MySQL accepts.

    Two failures motivated these cases: TIME cells were rendered as bare
    microsecond integers (ERROR 1292), and SET cells silently collapsed to
    NULL because the canonical document keys them under "values" rather
    "value".
    """

    def test_duration_us_renders_as_time_literal(self):
        self.assertEqual(sql_literal({"type": "duration_us", "value": 27030000000}),
                         "'7:30:30'")
        self.assertEqual(sql_literal({"type": "duration_us", "value": -27030000000}),
                         "'-7:30:30'")
        self.assertEqual(sql_literal({"type": "duration_us", "value": 1234567}),
                         "'0:00:01.234567'")
        self.assertEqual(sql_literal({"type": "duration_us", "value": 0}), "'0:00:00'")

    def test_negative_timedelta_beyond_one_day_is_not_strftime(self):
        # str(timedelta) yields "-2 days, 13:00:00" for this value, which MySQL
        # rejects; the renderer must format from the microsecond total.
        self.assertEqual(sql_literal(timedelta(days=-2, hours=13)), "'-35:00:00'")

    def test_set_canonical_joins_members(self):
        self.assertEqual(
            sql_literal({"type": "set", "values": ["read", "执行"]}, "utf8mb4"),
            "'read,执行'")
        self.assertEqual(sql_literal({"type": "set", "values": []}), "''")
        self.assertEqual(sql_literal({"type": "null"}), "NULL")

    def test_control_characters_become_hex_convert(self):
        literal = sql_literal({"type": "string", "value": "tab\there\nnl"})
        self.assertEqual(literal,
                         "CONVERT(0x74616209686572650a6e6c USING utf8mb4)")

    def test_latin1_high_bytes_become_hex_convert(self):
        literal = sql_literal({"type": "string", "value": "café"}, "latin1")
        self.assertEqual(literal, "CONVERT(0x636166e9 USING latin1)")

    def test_read_back_time_cells_are_canonicalised_exactly(self):
        # canonical_value goes through float seconds; reading TIME(6) back must
        # not lose the microsecond the sealed truth holds (530 of the 200k
        # cells in the Stage 1 fixture are not fixed points of that float path).
        column = {"name": "c_time6", "type": "time", "nullable": True}
        for microseconds in (33334701141, 259159183, 0, 999999, -33334701141):
            sign = "-" if microseconds < 0 else ""
            rest = abs(microseconds)
            frac = rest % 1_000_000
            hours, rem = divmod(rest // 1_000_000, 3600)
            minutes, seconds = divmod(rem, 60)
            text = (f"{sign}{hours}:{minutes:02d}:{seconds:02d}.{frac:06d}")
            cell = _decode_tsv_cell(text, column)
            canonical = canonical_value(cell, "time")
            self.assertEqual(canonical,
                             {"type": "duration_us", "value": microseconds})

    def test_ddl_orders_charset_before_not_null(self):
        ddl = build_ddl({
            "schema": {
                "columns": [{"name": "id_b", "type": "varchar(32)",
                             "charset": "utf8mb4", "collation": "utf8mb4_0900_ai_ci",
                             "nullable": False}],
                "primary_key": ["id_b"],
            }
        })
        self.assertIn("varchar(32) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci NOT NULL", ddl)
        self.assertNotIn("NOT NULL CHARACTER SET", ddl)

    def test_ddl_preserves_4byte_enum_members(self):
        ddl = build_ddl({
            "schema": {
                "columns": [{"name": "c_enum", "type": "enum('emoji?')",
                             "enum_values": ["emoji🙂"], "nullable": True}],
                "primary_key": [],
            }
        })
        self.assertIn("enum('emoji🙂')", ddl)


if __name__ == "__main__":
    unittest.main()
