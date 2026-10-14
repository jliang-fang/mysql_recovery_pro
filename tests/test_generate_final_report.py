import json
import tempfile
import unittest
from pathlib import Path

from scripts.disaster_validation import generate_final_report as report


class GenerateFinalReportTest(unittest.TestCase):
    def test_report_helpers_exist_and_accept_empty_input(self):
        # Editing these builders previously dropped a `def` line and left its
        # body attached to the previous function, which only surfaced when the
        # report was generated at the end of a batch.
        for name in ("_build_defects", "_build_fail_closed", "_build_validation_gaps",
                     "_conclusion_defects", "collect_corruption", "collect_phase_a",
                     "collect_phase_b"):
            self.assertTrue(callable(getattr(report, name, None)), name)
        self.assertEqual(report._build_defects([], [], []), [])
        self.assertEqual(report._build_fail_closed([], [], []), [])
        self.assertIsInstance(report._build_validation_gaps([], [], []), list)
        self.assertEqual(report._conclusion_defects([], [], []), "none")

    def test_collect_raw_media_classifies_attempted_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "raw_nvme_qualification"
            case = root / "stage1-nvme80-100k-1"
            (case / "reports").mkdir(parents=True)
            (case / "recovery-output" / "drop").mkdir(parents=True)
            (case / "run_state.json").write_text(json.dumps({
                "state": "SEALED", "matrix": {"version": "8.0", "filesystem": "ext4",
                "scenario": "drop", "dataset_profile": "100k", "page_size": 16384}
            }), encoding="utf-8")
            (case / "manifest.json").write_text(json.dumps({
                "state": "SEALED", "verdict": "FAIL",
                "matrix": {"version": "8.0", "filesystem": "ext4", "scenario": "drop",
                           "dataset_profile": "100k", "page_size": 16384},
                "contract": {"classification": "structured_recovery", "structured_rows": 99941,
                             "export_success": True, "importable": False,
                             "schema_source": "EXTERNAL"},
                "comparison": {"expected_rows": 100000, "actual_rows": 99941,
                               "correct_rows": 99941, "field_mismatches": [],
                               "false_negatives": [1], "strict_pass": False},
                "import": {"status": "FAIL"}
            }), encoding="utf-8")
            cases = report.collect_raw_media(root)
            self.assertEqual(len(cases), 1)
            self.assertEqual(cases[0]["status"], "ATTEMPTED_FAILED")
            self.assertEqual(cases[0]["verdict"], "FAIL")
            self.assertEqual(cases[0]["comparison"]["actual_rows"], 99941)
            self.assertIn("fresh_import_failed", cases[0]["failure_reasons"])

    def test_collect_raw_media_empty_is_unexecuted(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(report.collect_raw_media(Path(directory)), [])

    def test_corruption_cases_feed_defects_and_fail_closed(self):
        failing = {
            "case": "corruption-57-ext4-baseline-checksum_body_flip-1",
            "version": "5.7", "filesystem": "ext4", "scenario": "baseline",
            "mutation": "checksum_body_flip", "verdict": "PRODUCT_FAIL",
            "reasons": ["1 fabricated rows"], "runs": {},
            "minimal_repro": "python3 -m scripts.disaster_validation.corruption_matrix",
            "input_sha256": "a", "output_sha256": "b",
        }
        defects = report._build_defects([], [], [failing])
        self.assertEqual([d["id"] for d in defects], ["C01"])
        self.assertIn("checksum_body_flip", defects[0]["title"])

        rejected = {
            "case": "corruption-57-ext4-baseline-tde_candidate-1",
            "verdict": "PASS_EVIDENCE_ONLY", "reasons": ["rejected"],
            "runs": {"tde_candidate": {
                "classification": "expected_reject", "structured_rows": 0,
                "evidence_fragments": 0, "export_success": False,
                "importable": False, "contract_complete": True,
                "reason": "tde_candidate_UNVALIDATED"}},
        }
        fail_closed = report._build_fail_closed([], [], [rejected])
        self.assertEqual(len(fail_closed), 1)
        self.assertEqual(fail_closed[0]["phase"], "CORRUPTION")
        self.assertFalse(fail_closed[0]["importable"])

    def test_collect_corruption_reads_case_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            case = root / "corruption-84-ext4-baseline-tde_candidate-1"
            (case / "reports").mkdir(parents=True)
            (case / "manifests").mkdir()
            (case / "reports" / "corruption.json").write_text(json.dumps({
                "version": "8.4", "filesystem": "ext4", "scenario": "baseline",
                "mutation": "tde_candidate", "status": "PASS_EVIDENCE_ONLY",
                "verdict_reasons": ["rejected"],
                "verification": {"runs": {"tde_candidate": {
                    "returncode": 1, "contract_complete": True,
                    "classification": "expected_reject", "structured_rows": 0,
                    "export_success": False, "importable": False,
                    "reason": "tde_candidate_UNVALIDATED", "status": "EVIDENCE_ONLY"}}},
            }), encoding="utf-8")
            (case / "manifests" / "mutation.json").write_text(
                json.dumps({"source_sha256": "a",
                            "mutation": {"target_sha256": "b"}}), encoding="utf-8")

            cases = report.collect_corruption(root)
            self.assertEqual(len(cases), 1)
            self.assertEqual(cases[0]["verdict"], "PASS_EVIDENCE_ONLY")
            self.assertEqual(cases[0]["output_sha256"], "b")
            self.assertIn("corruption_matrix", cases[0]["minimal_repro"])

    def test_collect_raw_media_interrupted_run_is_setup_error(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "raw_nvme_qualification"
            case = root / "real-57-xfs-rm-rf-100k-interrupted"
            (case / "environment").mkdir(parents=True)
            (case / "run_state.json").write_text(json.dumps({
                "run_id": case.name, "state": "MEDIA_PREPARED",
                "matrix": {"version": "5.7", "filesystem": "xfs", "scenario": "rm-rf",
                           "dataset_profile": "100k", "page_size": 16384, "row_format": "DYNAMIC",
                           "status": "PLANNED"},
                "events": [{"state": "PLANNED"}, {"state": "PRECHECKED"}, {"state": "MEDIA_PREPARED"}]
            }), encoding="utf-8")
            cases = report.collect_raw_media(root)
            self.assertEqual(len(cases), 1)
            self.assertEqual(cases[0]["verdict"], "SETUP_ERROR")
            self.assertEqual(cases[0]["lifecycle_state"], "MEDIA_PREPARED")

    def test_collect_raw_media_sealed_pass_case(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "raw_nvme_qualification"
            case = root / "real-57-xfs-truncate-100k-test"
            (case / "reports").mkdir(parents=True)
            (case / "recovery-output" / "t1").mkdir(parents=True)
            (case / "run_state.json").write_text(json.dumps({
                "state": "SEALED", "matrix": {"version": "5.7", "filesystem": "xfs",
                                              "scenario": "truncate", "dataset_profile": "100k",
                                              "page_size": 16384, "row_format": "DYNAMIC",
                                              "status": "PLANNED"}
            }), encoding="utf-8")
            (case / "reports" / "comparison_t1.json").write_text(json.dumps({
                "expected_rows": 100000, "actual_rows": 100000, "correct_rows": 100000,
                "strict_pass": True, "field_mismatches": [], "false_positives": [], "false_negatives": []
            }), encoding="utf-8")
            (case / "recovery-output" / "t1" / "result.json").write_text(json.dumps({
                "classification": "structured_recovery", "structured_rows": 100000,
                "export_success": True, "importable": False, "details": {"device_path": "/dev/nvme0n2"}
            }), encoding="utf-8")
            (case / "reports" / "fresh_import_result.json").write_text(json.dumps({
                "status": "PASS"
            }), encoding="utf-8")
            (case / "manifest.json").write_text(json.dumps({
                "state": "SEALED", "verdict": "PASS",
                "matrix": {"version": "5.7", "filesystem": "xfs", "scenario": "truncate",
                           "dataset_profile": "100k", "page_size": 16384, "row_format": "DYNAMIC",
                           "status": "PLANNED"}
            }), encoding="utf-8")
            cases = report.collect_raw_media(root)
            self.assertEqual(len(cases), 1)
            self.assertEqual(cases[0]["verdict"], "PASS")
            self.assertEqual(cases[0]["status"], "ATTEMPTED_PASS")
            self.assertTrue(cases[0]["comparison"]["strict_pass"])


if __name__ == "__main__":
    unittest.main()
