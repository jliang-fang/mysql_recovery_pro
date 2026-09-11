import unittest
import tempfile
import json
import os
import sys

# Add scripts directory to sys.path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "scripts")))

from phase6_generate_report import _matrix_outcome, _matrix_entries, check_build, generate_report


class Phase6ReportTest(unittest.TestCase):
    def test_matrix_outcome_strict_pass(self):
        entry = {
            "status": "COMPARABLE",
            "strict_pass": True,
            "export_eligible": True,
            "trust": "COMPLETE_VERIFIED",
        }
        outcome, details = _matrix_outcome(entry)
        self.assertEqual(outcome, "PASS")
        self.assertTrue(details)

    def test_matrix_outcome_not_computable(self):
        entry = {
            "status": "NOT_COMPUTABLE",
            "strict_pass": False,
            "export_eligible": False,
            "trust": "EVIDENCE_ONLY",
            "classification": "FORENSIC_EVIDENCE",
        }
        outcome, details = _matrix_outcome(entry)
        self.assertEqual(outcome, "FAIL")
        self.assertIn("NOT_COMPUTABLE", details)
        self.assertIn("FORENSIC_EVIDENCE", details)
        self.assertIn("trust=EVIDENCE_ONLY", details)
        self.assertIn("strict_pass=false", details)
        self.assertIn("structured export blocked", details)

    def test_matrix_outcome_strict_pass_false(self):
        entry = {
            "status": "COMPARABLE",
            "strict_pass": False,
            "export_eligible": True,
        }
        outcome, details = _matrix_outcome(entry)
        self.assertEqual(outcome, "FAIL")
        self.assertIn("strict_pass=false", details)

    def test_matrix_outcome_export_blocked(self):
        entry = {
            "status": "COMPARABLE",
            "strict_pass": True,
            "export_eligible": False,
        }
        outcome, details = _matrix_outcome(entry)
        self.assertEqual(outcome, "FAIL")
        self.assertIn("structured export blocked", details)

    def test_matrix_entries_list_and_dict(self):
        raw_list = [{"scenario": "A"}]
        self.assertEqual(_matrix_entries(raw_list), raw_list)

        dict_comp = {"comparisons": [{"scenario": "B"}]}
        self.assertEqual(_matrix_entries(dict_comp), [{"scenario": "B"}])

        dict_res = {"results": [{"scenario": "C"}]}
        self.assertEqual(_matrix_entries(dict_res), [{"scenario": "C"}])

        self.assertEqual(_matrix_entries({}), [])
        self.assertEqual(_matrix_entries(None), [])

    def test_generate_report_renders_and_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            build_dir = os.path.join(tmpdir, "build")
            os.mkdir(build_dir)
            os.mkdir(os.path.join(build_dir, "tests"))
            with open(os.path.join(build_dir, "libdbrecovery_innodb.a"), "wb") as f:
                f.write(b"!<arch>\n")

            matrix_path = os.path.join(tmpdir, "matrix.json")
            with open(matrix_path, "w") as f:
                json.dump({"comparisons": [
                    {"status": "NOT_COMPUTABLE", "strict_pass": False,
                     "export_eligible": False, "trust": "EVIDENCE_ONLY",
                     "classification": "FORENSIC_EVIDENCE", "scenario": "level2_page"},
                ]}, f)

            output = os.path.join(tmpdir, "report.html")
            status = generate_report(build_dir, [], matrix_path, output)
            self.assertEqual(status, "FAIL")
            html = open(output).read()
            self.assertIn("NOT_COMPUTABLE", html)
            self.assertIn("EVIDENCE_ONLY", html)
            with open(output.replace(".html", ".json")) as f:
                json_report = json.load(f)
            self.assertEqual(json_report["overall_status"], "FAIL")

    def test_generate_report_strict_pass_is_pass(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            build_dir = os.path.join(tmpdir, "build")
            os.makedirs(os.path.join(build_dir, "tests"))
            for rel in ("libdbrecovery_innodb.a", "mysql_recovery_pro",
                        "tests/test_p0_regression", "tests/test_p1_salvage_fuzz",
                        "tests/test_p1_layout_fuzz", "tests/test_p1_type_classifier_fuzz"):
                with open(os.path.join(build_dir, rel), "wb") as f:
                    f.write(b"!<arch>\n")
            matrix_path = os.path.join(tmpdir, "matrix.json")
            with open(matrix_path, "w") as f:
                json.dump({"results": [
                    {"status": "COMPARABLE", "strict_pass": True,
                     "export_eligible": True, "scenario": "level1"},
                ]}, f)
            output = os.path.join(tmpdir, "report.html")
            suites = [{"suite": "test_p0_regression",
                       "tests": [{"name": "test_p0_regression", "passed": True, "details": "ok"}]}]
            self.assertEqual(generate_report(build_dir, suites, matrix_path, output), "PASS")

    def test_missing_build_artifact_fails_overall(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            build_dir = os.path.join(tmpdir, "build")
            os.makedirs(os.path.join(build_dir, "tests"))
            with open(os.path.join(build_dir, "libdbrecovery_innodb.a"), "wb") as f:
                f.write(b"!<arch>\n")
            output = os.path.join(tmpdir, "report.html")
            self.assertEqual(generate_report(build_dir, [], None, output), "FAIL")

    def test_missing_matrix_results_fails_overall(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            build_dir = os.path.join(tmpdir, "build")
            os.makedirs(os.path.join(build_dir, "tests"))
            for rel in ("libdbrecovery_innodb.a", "mysql_recovery_pro",
                        "tests/test_p0_regression", "tests/test_p1_salvage_fuzz",
                        "tests/test_p1_layout_fuzz", "tests/test_p1_type_classifier_fuzz"):
                with open(os.path.join(build_dir, rel), "wb") as f:
                    f.write(b"!<arch>\n")
            output = os.path.join(tmpdir, "report.html")
            self.assertEqual(generate_report(build_dir, [], None, output), "FAIL")
            with open(output) as f:
                html = f.read()
            self.assertIn("NOT VERIFIED", html)
            self.assertIn("No test suites executed", html)

    def test_check_build_requires_static_lib(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            # Empty build dir should report missing
            results = check_build(tmpdir)
            static_lib = next(r for r in results if "libdbrecovery_innodb.a" in r["name"])
            self.assertFalse(static_lib["passed"])

            # Create dummy libdbrecovery_innodb.a
            lib_path = os.path.join(tmpdir, "libdbrecovery_innodb.a")
            with open(lib_path, "wb") as f:
                f.write(b"!<arch>\n")

            results2 = check_build(tmpdir)
            static_lib2 = next(r for r in results2 if "libdbrecovery_innodb.a" in r["name"])
            self.assertTrue(static_lib2["passed"])


if __name__ == "__main__":
    unittest.main()
