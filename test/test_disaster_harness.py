#!/usr/bin/env python3
import hashlib
import json
import os
import struct
import tempfile
import unittest
from datetime import date, datetime, time, timedelta
from decimal import Decimal
from pathlib import Path

from scripts.corruption_scenarios import (
    FIL_PAGE_INDEX,
    FIL_PAGE_SDI,
    FIL_PAGE_TYPE_ABS,
    FIL_PAGE_OFFSET_ABS,
    FIL_PAGE_SPACE_ID_ABS,
    FIL_PAGE_LSN_ABS,
    run_corruption_scenario,
)
from scripts.disaster_harness.comparator import compare, compare_three_way
from scripts.disaster_harness.experiment import (
    CommandResult,
    ExperimentSpec,
    MutationSpec,
    NativeSpec,
    OfflineSpec,
    run_experiment,
    run_native_level,
)
from scripts.disaster_harness.ground_truth import (
    build_ground_truth,
    canonical_value,
    schema_fingerprint,
)
from scripts.disaster_harness.manifest import (
    SafetyError,
    build_manifest,
    canonical_json,
    file_entry,
    init_workspace,
    require_workspace,
    seal_run,
    sha256_file,
    validate_output_dir,
    validate_source,
    verify_input_clone,
    verify_seal,
    verify_source_immutable,
    write_manifest,
)


PAGE_SIZE = 16384


def make_page(page_no, page_type, space_id=7, lsn=0x0102030405060708):
    page = bytearray(PAGE_SIZE)
    struct.pack_into(">I", page, FIL_PAGE_OFFSET_ABS, page_no)
    struct.pack_into(">Q", page, FIL_PAGE_LSN_ABS, lsn)
    struct.pack_into(">H", page, FIL_PAGE_TYPE_ABS, page_type)
    struct.pack_into(">I", page, FIL_PAGE_SPACE_ID_ABS, space_id)
    for index in range(64, PAGE_SIZE - 4):
        page[index] = (page_no + index) & 0xFF
    return bytes(page)


def make_tablespace(include_sdi=False):
    pages = [make_page(0, FIL_PAGE_INDEX)]
    pages.extend(make_page(index, FIL_PAGE_SDI if include_sdi and index == 1 else FIL_PAGE_INDEX)
                 for index in range(1, 4))
    return b"".join(pages)


class ManifestTests(unittest.TestCase):
    def test_workspace_marker_and_canonical_seal(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            workspace = init_workspace(root / "workspace", "unit-test")
            self.assertEqual(workspace, require_workspace(workspace))
            source_dir = root / "source"
            source_dir.mkdir()
            source = source_dir / "input.bin"
            source.write_bytes(b"evidence")
            self.assertEqual(source, validate_source(source))
            output_dir = validate_output_dir(root / "output", source)
            output_dir.mkdir()
            artifact = output_dir / "artifact.bin"
            artifact.write_bytes(b"artifact")
            entry = file_entry(artifact, "artifact", output_dir)
            manifest = build_manifest(workspace, [entry], {"purpose": "unit-test"})
            write_manifest(workspace, manifest)
            digest = seal_run(workspace)
            self.assertEqual(digest, hashlib.sha256((workspace / "manifest.json").read_bytes()).hexdigest())
            self.assertTrue(verify_seal(workspace))
            (workspace / "manifest.json").write_text("{}\n", encoding="utf-8")
            self.assertFalse(verify_seal(workspace))

    def test_workspace_and_input_safety(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            workspace = init_workspace(root / "workspace", "unit-test")
            with self.assertRaises(SafetyError):
                init_workspace(workspace, "again")
            source_dir = root / "source"
            source_dir.mkdir()
            source = source_dir / "input.bin"
            source.write_bytes(b"source")
            self.assertTrue(verify_source_immutable(source, sha256_file(source)))
            clone = root / "clone.bin"
            clone.write_bytes(source.read_bytes())
            self.assertTrue(verify_input_clone(clone, sha256_file(source)))
            with self.assertRaises(SafetyError):
                validate_output_dir(source_dir / "nested", source)
            symlink = root / "source-link"
            try:
                symlink.symlink_to(source)
            except OSError as error:
                self.skipTest(f"symlinks unavailable: {error}")
            with self.assertRaises(SafetyError):
                validate_source(symlink)

    def test_canonical_json_is_stable(self):
        self.assertEqual(canonical_json({"b": 2, "a": 1}), '{"a":1,"b":2}')

    def test_connector_export_requires_utf8mb4(self):
        source = Path(__file__).parents[1] / "scripts" / "disaster_harness" / "native_export.py"
        self.assertIn('charset="utf8mb4"', source.read_text(encoding="utf-8"))


class GroundTruthTests(unittest.TestCase):
    def setUp(self):
        self.schema = {
            "columns": [
                {"name": "id", "ordinal": 0, "type": "int", "nullable": False},
                {"name": "empty_text", "ordinal": 1, "type": "varchar(20)", "nullable": True},
                {"name": "amount", "ordinal": 2, "type": "decimal(10,2)", "nullable": False},
                {"name": "payload", "ordinal": 3, "type": "blob", "nullable": True},
                {"name": "when_seen", "ordinal": 4, "type": "datetime", "nullable": True},
                {"name": "doc", "ordinal": 5, "type": "json", "nullable": True},
            ],
            "primary_key": ["id"],
        }

    def test_typed_values_preserve_distinctions(self):
        self.assertEqual(canonical_value(None), {"type": "null"})
        self.assertEqual(canonical_value(""), {"type": "string", "value": ""})
        self.assertEqual(canonical_value(b"\x00"), {"type": "binary", "encoding": "base64", "value": "AA=="})
        self.assertEqual(canonical_value(Decimal("12.3400")), {"type": "decimal", "value": "12.34"})
        self.assertEqual(canonical_value(date(2026, 9, 7)), {"type": "date", "value": "2026-09-07"})
        self.assertEqual(canonical_value(datetime(2026, 9, 7, 1, 2, 3)), {"type": "datetime", "value": "2026-09-07 01:02:03"})
        self.assertEqual(canonical_value(time(1, 2, 3)), {"type": "time", "value": "01:02:03"})
        self.assertEqual(canonical_value(timedelta(seconds=2)), {"type": "duration_us", "value": 2000000})
        self.assertEqual(canonical_value({"z": 2, "a": [True, None]}, "json"),
                         {"type": "json", "value": {"a": [True, None], "z": 2}})

    def test_ground_truth_is_comparable_and_deterministic(self):
        rows = [{
            "id": 1,
            "empty_text": "",
            "amount": Decimal("12.3400"),
            "payload": b"\x00\xff",
            "when_seen": datetime(2026, 9, 7, 1, 2, 3),
            "doc": {"b": 2, "a": 1},
        }]
        first = build_ground_truth("db", "table", self.schema, rows, "9.0")
        second = build_ground_truth("db", "table", self.schema, rows, "9.0")
        self.assertEqual(first, second)
        self.assertEqual(first["status"], "SEALED_READY")
        self.assertEqual(first["row_count"], 1)
        self.assertEqual(first["schema_fingerprint"], schema_fingerprint(first["schema"]))
        self.assertNotEqual(first["rows"][0]["values"]["empty_text"], first["rows"][0]["values"]["payload"])

    def test_duplicate_and_missing_primary_key_are_not_comparable(self):
        row = {"id": 1, "empty_text": "", "amount": Decimal("1"), "payload": None,
               "when_seen": None, "doc": None}
        duplicate = build_ground_truth("db", "table", self.schema, [row, row])
        self.assertEqual(duplicate["status"], "NOT_COMPARABLE")
        no_pk = build_ground_truth("db", "table", {"columns": self.schema["columns"], "primary_key": []}, [row])
        self.assertEqual(no_pk["status"], "NOT_COMPARABLE")


class ComparatorTests(unittest.TestCase):
    def setUp(self):
        schema = {"columns": [{"name": "id", "type": "int", "nullable": False},
                              {"name": "name", "type": "varchar(20)", "nullable": False}],
                  "primary_key": ["id"]}
        self.expected = build_ground_truth("db", "t", schema, [{"id": 1, "name": "one"},
                                                                  {"id": 2, "name": "two"}])

    def test_exact_match_and_set_differences(self):
        actual = build_ground_truth("db", "t", self.expected["schema"], [{"id": 1, "name": "one"},
                                                                            {"id": 3, "name": "three"}])
        result = compare(self.expected, actual, "test")
        self.assertEqual(result["status"], "COMPARABLE")
        self.assertEqual(result["correct_rows"], 1)
        self.assertEqual(result["false_negatives"], [[{"type": "int", "value": 2}]])
        self.assertEqual(result["false_positives"], [[{"type": "int", "value": 3}]])
        self.assertEqual(result["precision"], 0.5)
        self.assertEqual(result["recall"], 0.5)

    def test_field_mismatch_and_missing_result_fail_closed(self):
        actual = build_ground_truth("db", "t", self.expected["schema"], [{"id": 1, "name": "changed"},
                                                                            {"id": 2, "name": "two"}])
        result = compare(self.expected, actual, "test")
        self.assertEqual(result["field_mismatches"][0]["fields"], ["name"])
        self.assertEqual(result["correct_rows"], 1)
        self.assertFalse(result["strict_pass"])
        missing = compare(self.expected, None, "missing")
        self.assertEqual(missing["status"], "NOT_COMPUTABLE")
        self.assertIsNone(missing["precision"])
        self.assertFalse(missing["strict_pass"])

    def test_duplicate_primary_keys_are_not_collapsed_or_computable(self):
        duplicate = build_ground_truth(
            "db", "t", self.expected["schema"],
            [{"id": 1, "name": "one"}, {"id": 1, "name": "different"}],
        )
        result = compare(self.expected, duplicate, "duplicate")
        self.assertEqual(result["status"], "NOT_COMPUTABLE")
        self.assertIsNone(result["precision"])
        self.assertEqual(result["actual_rows"], 2)
        self.assertEqual(result["duplicate_actual"], [{
            "pk": [{"type": "int", "value": 1}], "count": 2,
        }])
        self.assertFalse(result["strict_pass"])

    def test_identity_and_version_mismatches_are_not_computable(self):
        other_table = build_ground_truth("db", "other", self.expected["schema"],
                                         [{"id": 1, "name": "one"}, {"id": 2, "name": "two"}], "9.0.1")
        expected = build_ground_truth("db", "t", self.expected["schema"],
                                      [{"id": 1, "name": "one"}, {"id": 2, "name": "two"}], "9.0.2")
        result = compare(expected, other_table, "identity")
        self.assertEqual(result["status"], "NOT_COMPUTABLE")
        self.assertEqual(result["prerequisites"], ["mysql_version_mismatch", "table_mismatch"])
        self.assertFalse(result["strict_pass"])

    def test_exact_match_is_strict_pass(self):
        result = compare(self.expected, self.expected, "exact")
        self.assertEqual(result["status"], "COMPARABLE")
        self.assertTrue(result["strict_pass"])

    def test_three_way_reports_incomplete_member(self):
        report = compare_three_way(self.expected, self.expected, None)
        self.assertEqual(report["status"], "NOT_COMPUTABLE")
        self.assertEqual(report["comparisons"]["ground_truth_vs_native"]["status"], "COMPARABLE")


class CorruptionTests(unittest.TestCase):
    def test_big_endian_damage_ranges_and_seed_reproducibility(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "input.ibd"
            source.write_bytes(make_tablespace())
            first = run_corruption_scenario(str(source), str(root / "first.ibd"), "level2_data_page",
                                            seed=11, page_size=PAGE_SIZE, damage_bytes=32, page_numbers=[1])
            second = run_corruption_scenario(str(source), str(root / "second.ibd"), "level2_data_page",
                                             seed=11, page_size=PAGE_SIZE, damage_bytes=32, page_numbers=[1])
            self.assertEqual(first["output_sha256"], second["output_sha256"])
            self.assertEqual(first["input_sha256"], sha256_file(source))
            self.assertEqual(first["source_unchanged_sha256"], sha256_file(source))
            self.assertEqual(first["ranges"][0]["start"], PAGE_SIZE + 38)
            self.assertEqual(first["ranges"][0]["length"], 32)
            damaged = (root / "first.ibd").read_bytes()
            self.assertEqual(first["ranges"][0]["after_sha256"], hashlib.sha256(damaged[PAGE_SIZE + 38:PAGE_SIZE + 70]).hexdigest())

    def test_level_one_requires_explicit_redo_role(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "ib_logfile0"
            source.write_bytes(b"redo" * 32)
            with self.assertRaises(ValueError):
                run_corruption_scenario(str(source), str(root / "missing.bin"),
                                        "level1_redo_missing")
            with self.assertRaises(ValueError):
                run_corruption_scenario(str(source), str(root / "wrong.bin"),
                                        "level1_redo_missing", input_role="tablespace")

    def test_level_one_records_redo_role_without_page_inference(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "input.ibd"
            source.write_bytes(b"redo" * 32)
            manifest = root / "damage.json"
            result = run_corruption_scenario(
                str(source), str(root / "damaged.redo"), "level1_redo_checksum",
                input_role="redo", manifest_path=str(manifest))
            self.assertEqual(result["input_role"], "redo")
            self.assertIsNone(result["page_size"])
            self.assertEqual(json.loads(manifest.read_text())["input_role"], "redo")
            self.assertEqual(result["source_unchanged_sha256"], sha256_file(source))

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "input.ibd"
            source.write_bytes(make_tablespace())
            with self.assertRaises(ValueError):
                run_corruption_scenario(str(source), str(root / "damaged.ibd"), "level3_sdi_damage",
                                        page_size=PAGE_SIZE)


class ExperimentTests(unittest.TestCase):
    def _document(self):
        schema = {
            "columns": [{"name": "id", "type": "int", "nullable": False}],
            "primary_key": ["id"],
        }
        return build_ground_truth("db", "t", schema, [{"id": 1}], "9.0")

    def test_run_native_level_missing_structured_result_is_not_computable(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            run_dir = root / "native_0"
            run_dir.mkdir()
            datadir = root / "datadir"
            datadir.mkdir()
            spec = NativeSpec(image="mysql: test", startup_timeout=0.01)
            calls = []

            def runner(argv, timeout, input_text=""):
                calls.append((list(argv), timeout, input_text))
                if argv[1] == "exec" and "mysqladmin" in argv:
                    return CommandResult(list(argv), 0, "mysqld is alive", "", 0.0)
                return CommandResult(list(argv), 0, "", "", 0.0)

            result = run_native_level(spec, 0, datadir, run_dir, runner)

            self.assertEqual(result["status"], "NOT_COMPUTABLE")
            self.assertEqual(result["excluded_evidence_levels"], ["native_export"])
            self.assertTrue((run_dir / "native_execution.json").is_file())
            self.assertTrue(any(item[2] == "" for item in calls))

    def test_run_experiment_rejects_non_tablespace_target(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            workspace = init_workspace(root / "workspace", "target validation")
            pristine = root / "pristine"
            (pristine / "db").mkdir(parents=True)
            (pristine / "db" / "table.ibd").write_bytes(b"pristine")
            damaged = root / "damaged.ibd"
            damaged.write_bytes(b"damaged")
            binary = root / "offline"
            binary.write_bytes(b"binary")
            ground_truth = root / "ground_truth.json"
            ground_truth.write_text(canonical_json(self._document()) + "\n", encoding="utf-8")

            with self.assertRaises(SafetyError):
                run_experiment(ExperimentSpec(
                    workspace=workspace,
                    pristine_datadir=pristine,
                    damaged_input=damaged,
                    damaged_relative_path=Path("db/table"),
                    ground_truth=ground_truth,
                    native=NativeSpec(image="mysql:test"),
                    offline=OfflineSpec(binary=binary, ibd_arg="ibd"),
                    levels=(0,),
                ), lambda argv, timeout, input_text="": CommandResult(list(argv), 0, "", "", 0.0))

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            workspace = init_workspace(root / "workspace", "fake experiment")
            pristine = root / "pristine"
            (pristine / "db").mkdir(parents=True)
            (pristine / "db" / "table.ibd").write_bytes(b"pristine")
            damaged = root / "damaged.ibd"
            damaged.write_bytes(b"damaged")
            binary = root / "offline"
            binary.write_bytes(b"binary")
            document = self._document()
            ground_truth = root / "ground_truth.json"
            ground_truth.write_text(canonical_json(document) + "\n", encoding="utf-8")
            calls = []
            datadirs = {}

            def runner(argv, timeout, input_text=""):
                calls.append((list(argv), timeout, input_text))
                if len(argv) > 1 and argv[1] == "run":
                    name = argv[argv.index("--name") + 1]
                    mount = argv[argv.index("-v") + 1]
                    datadirs[name] = Path(mount.split(":", 1)[0])
                    return CommandResult(list(argv), 0, "container-id", "", 0.0)
                if len(argv) > 1 and argv[1] == "exec" and "mysqladmin" in argv:
                    return CommandResult(list(argv), 0, "mysqld is alive", "", 0.0)
                if len(argv) > 1 and argv[1] == "exec":
                    name = argv[3] if argv[2] == "-i" else argv[2]
                    datadir = datadirs[name]
                    (datadir.parent / "native_result.json").write_text(
                        canonical_json(document) + "\n", encoding="utf-8")
                    return CommandResult(list(argv), 0, "", "", 0.0)
                if len(argv) > 1 and argv[1] == "stop":
                    return CommandResult(list(argv), 0, "", "", 0.0)
                if len(argv) > 1 and argv[1] in {"logs", "rm"}:
                    return CommandResult(list(argv), 0, "", "", 0.0)
                output_dir = Path(argv[argv.index("--output") + 1])
                result_path = output_dir / "damaged_result.json"
                result_path.write_text(canonical_json(document) + "\n", encoding="utf-8")
                return CommandResult(list(argv), 0, "", "", 0.0)

            result = run_experiment(ExperimentSpec(
                workspace=workspace,
                pristine_datadir=pristine,
                damaged_input=damaged,
                damaged_relative_path=Path("db/table.ibd"),
                ground_truth=ground_truth,
                native=NativeSpec(image="mysql:test", export_sql="SELECT 1"),
                offline=OfflineSpec(binary=binary, ibd_arg="ibd"),
                levels=(0, 2, 6),
                source_hash=sha256_file(damaged),
            ), runner)

            self.assertTrue((Path(result["experiment_dir"]) / "manifest.seal").is_file())
            self.assertTrue(verify_seal(Path(result["experiment_dir"])))
            self.assertTrue(all(report["status"] == "COMPARABLE"
                                for report in result["comparison"].values()))
            self.assertEqual(sha256_file(damaged), sha256_file(damaged))
            clone_hashes = []
            for level in (0, 2, 6):
                clone = workspace / "experiment" / f"native_{level}" / "datadir" / "db" / "table.ibd"
                self.assertEqual(clone.read_bytes(), b"damaged")
                clone_hashes.append(str(clone.parent.parent))
            self.assertEqual(len(set(clone_hashes)), 3)
            self.assertEqual(sum(1 for call in calls if len(call[0]) > 1 and call[0][1] == "run"), 3)
            self.assertEqual(sum(1 for call in calls if len(call[0]) > 1 and call[0][1] == "stop"), 3)

    def test_run_experiment_uses_explicit_offline_input(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            workspace = init_workspace(root / "workspace", "independent offline input")
            pristine = root / "pristine"
            (pristine / "db").mkdir(parents=True)
            (pristine / "db" / "table.ibd").write_bytes(b"pristine")
            damaged = root / "damaged.ibd"
            damaged.write_bytes(b"damaged")
            offline_input = root / "offline.ibd"
            offline_input.write_bytes(b"offline")
            binary = root / "offline-tool"
            binary.write_bytes(b"binary")
            ground_truth = root / "ground_truth.json"
            document = self._document()
            ground_truth.write_text(canonical_json(document) + "\n", encoding="utf-8")
            offline_argv = []

            def runner(argv, timeout, input_text=""):
                if len(argv) > 1 and argv[1] == "run":
                    return CommandResult(list(argv), 0, "container-id", "", 0.0)
                if len(argv) > 1 and argv[1] == "exec" and "mysqladmin" in argv:
                    return CommandResult(list(argv), 0, "alive", "", 0.0)
                if len(argv) > 1 and argv[1] == "exec":
                    run_dir = Path(argv[argv.index("-v") + 1].split(":", 1)[0]) if "-v" in argv else None
                    if run_dir:
                        (run_dir / "native_result.json").write_text(
                            canonical_json(document) + "\n", encoding="utf-8")
                    return CommandResult(list(argv), 0, "", "", 0.0)
                if len(argv) > 1 and argv[1] in {"stop", "logs", "rm"}:
                    return CommandResult(list(argv), 0, "", "", 0.0)
                offline_argv.extend(argv)
                output_dir = Path(argv[argv.index("--output") + 1])
                (output_dir / "offline_result.json").write_text(
                    canonical_json(document) + "\n", encoding="utf-8")
                return CommandResult(list(argv), 0, "", "", 0.0)

            result = run_experiment(ExperimentSpec(
                workspace=workspace,
                pristine_datadir=pristine,
                damaged_input=damaged,
                damaged_relative_path=Path("db/table.ibd"),
                offline_input=offline_input,
                ground_truth=ground_truth,
                native=NativeSpec(image="mysql:test"),
                offline=OfflineSpec(binary=binary, ibd_arg="ibd"),
                levels=(0,),
            ), runner)

            self.assertEqual(offline_argv[offline_argv.index("--ibd") + 1], str(offline_input))
            self.assertEqual(json.loads((Path(result["experiment_dir"]) / "experiment_spec.json").read_text())["offline_input_sha256"], sha256_file(offline_input))

    def test_run_experiment_without_offline_input_is_not_computable(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            workspace = init_workspace(root / "workspace", "native only")
            pristine = root / "pristine"
            (pristine / "#innodb_redo").mkdir(parents=True)
            (pristine / "#innodb_redo" / "redo.log").write_bytes(b"redo")
            replacement = root / "damaged.redo"
            replacement.write_bytes(b"damaged")
            binary = root / "offline-tool"
            binary.write_bytes(b"binary")
            ground_truth = root / "ground_truth.json"
            ground_truth.write_text(canonical_json(self._document()) + "\n", encoding="utf-8")
            offline_calls = []

            def runner(argv, timeout, input_text=""):
                if len(argv) > 1 and argv[1] == "run":
                    return CommandResult(list(argv), 0, "container-id", "", 0.0)
                if len(argv) > 1 and argv[1] == "exec" and "mysqladmin" in argv:
                    return CommandResult(list(argv), 0, "alive", "", 0.0)
                if len(argv) > 1 and argv[1] == "exec":
                    return CommandResult(list(argv), 0, "", "", 0.0)
                if len(argv) > 1 and argv[1] in {"stop", "logs", "rm"}:
                    return CommandResult(list(argv), 0, "", "", 0.0)
                offline_calls.append(argv)
                return CommandResult(list(argv), 0, "", "", 0.0)

            result = run_experiment(ExperimentSpec(
                workspace=workspace,
                pristine_datadir=pristine,
                damaged_input=None,
                damaged_relative_path=None,
                ground_truth=ground_truth,
                native=NativeSpec(image="mysql:test"),
                offline=OfflineSpec(binary=binary, ibd_arg="ibd"),
                levels=(0,),
                mutations=(MutationSpec(Path("#innodb_redo/redo.log"), "redo", "level1", replacement),),
            ), runner)

            offline = json.loads((Path(result["experiment_dir"]) / "offline" / "offline_execution.json").read_text())
            self.assertEqual(offline["status"], "NOT_EXECUTED")
            self.assertEqual(offline_calls, [])
            self.assertEqual(result["comparison"]["0"]["status"], "NOT_COMPUTABLE")

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            workspace = init_workspace(root / "workspace", "mutation plan")
            pristine = root / "pristine"
            (pristine / "db").mkdir(parents=True)
            (pristine / "db" / "table.ibd").write_bytes(b"pristine")
            (pristine / "#innodb_redo").mkdir()
            (pristine / "#innodb_redo" / "redo.log").write_bytes(b"redo-pristine")
            damaged = root / "damaged.ibd"
            damaged.write_bytes(b"damaged")
            damaged_redo = root / "damaged.redo"
            damaged_redo.write_bytes(b"damaged-redo")
            binary = root / "offline"
            binary.write_bytes(b"binary")
            document = self._document()
            ground_truth = root / "ground_truth.json"
            ground_truth.write_text(canonical_json(document) + "\n", encoding="utf-8")
            datadirs = {}

            def runner(argv, timeout, input_text=""):
                if len(argv) > 1 and argv[1] == "run":
                    name = argv[argv.index("--name") + 1]
                    mount = argv[argv.index("-v") + 1]
                    datadirs[name] = Path(mount.split(":", 1)[0])
                    return CommandResult(list(argv), 0, "container-id", "", 0.0)
                if len(argv) > 1 and argv[1] == "exec" and "mysqladmin" in argv:
                    return CommandResult(list(argv), 0, "mysqld is alive", "", 0.0)
                if len(argv) > 1 and argv[1] == "exec":
                    name = argv[3] if argv[2] == "-i" else argv[2]
                    (datadirs[name].parent / "native_result.json").write_text(
                        canonical_json(document) + "\n", encoding="utf-8")
                    return CommandResult(list(argv), 0, "", "", 0.0)
                if len(argv) > 1 and argv[1] in {"stop", "logs", "rm"}:
                    return CommandResult(list(argv), 0, "", "", 0.0)
                output_dir = Path(argv[argv.index("--output") + 1])
                (output_dir / "damaged_result.json").write_text(canonical_json(document) + "\n", encoding="utf-8")
                return CommandResult(list(argv), 0, "", "", 0.0)

            result = run_experiment(ExperimentSpec(
                workspace=workspace,
                pristine_datadir=pristine,
                damaged_input=damaged,
                damaged_relative_path=Path("db/table.ibd"),
                ground_truth=ground_truth,
                native=NativeSpec(image="mysql:test", export_sql="SELECT 1"),
                offline=OfflineSpec(binary=binary, ibd_arg="ibd"),
                levels=(0,),
                mutations=(
                    MutationSpec(Path("db/table.ibd"), "tablespace", "level2", damaged),
                    MutationSpec(Path("#innodb_redo/redo.log"), "redo", "level1", damaged_redo),
                ),
            ), runner)
            level_dir = Path(result["experiment_dir"]) / "native_0"
            input_record = json.loads((level_dir / "input.json").read_text())
            self.assertEqual(len(input_record["mutations"]), 2)
            self.assertEqual((level_dir / "datadir" / "#innodb_redo" / "redo.log").read_bytes(), b"damaged-redo")
            self.assertTrue(verify_seal(Path(result["experiment_dir"])))


