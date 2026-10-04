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


if __name__ == "__main__":
    unittest.main()
