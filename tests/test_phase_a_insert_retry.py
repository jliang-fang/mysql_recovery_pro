import tempfile
import unittest
from pathlib import Path

from scripts.disaster_validation.phase_a_offline_orchestrator import (
    CommandResult,
    PhaseAOrchestrator,
)
from scripts.disaster_validation.matrix import matrix_item


def _sql_of(argv):
    return argv[-1]


class _ScriptedRunner:
    """Runner that answers INSERT/SELECT by inspecting the statement."""

    def __init__(self, insert_outcomes, landed_ids):
        self.insert_outcomes = list(insert_outcomes)
        self.landed_ids = set(landed_ids)
        self.insert_statements = []

    def __call__(self, argv, timeout, input_text=""):
        sql = _sql_of(argv)
        if sql.startswith("INSERT INTO"):
            self.insert_statements.append(sql)
            outcome = (self.insert_outcomes.pop(0) if self.insert_outcomes
                       else ("ok", ""))
            if outcome[0] == "ok":
                return CommandResult(argv, 0, "", "")
            return CommandResult(argv, 1, "", outcome[1])
        if sql.startswith("SELECT id FROM"):
            rows = "\n".join(str(i) for i in sorted(self.landed_ids))
            return CommandResult(argv, 0, rows + ("\n" if rows else ""), "")
        raise AssertionError(f"unexpected statement: {sql}")


def _orchestrator(runner):
    with tempfile.TemporaryDirectory() as directory:
        item = matrix_item("8.4", "ext4", "baseline", "smoke", 16384, "DYNAMIC")
        # __init__ only records metadata, so the temporary directory can go away.
        return PhaseAOrchestrator(Path(directory), item, runner=runner)


class InsertRetryTest(unittest.TestCase):
    def test_transient_failure_reinserts_only_missing_ids(self):
        # 500-row batch: the server applied ids 1..3 before the client lost the
        # connection. A blind retry would violate the primary key; the batch
        # must be narrowed to the ids that are actually missing.
        runner = _ScriptedRunner(
            insert_outcomes=[
                ("fail", "ERROR 2002 (HY000): Can't connect to local MySQL server"),
                ("ok", ""),
            ],
            landed_ids=[1, 2, 3],
        )
        orchestrator = _orchestrator(runner)
        orchestrator._insert_rows(500, has_json=False)

        self.assertEqual(len(runner.insert_statements), 2)
        first, second = runner.insert_statements
        self.assertIn("(500, -500", first)
        self.assertIn("(1, -1,", first)
        # Only the three landed ids were dropped from the retry.
        self.assertNotIn("(1, -1,", second)
        self.assertNotIn("(2, -2,", second)
        self.assertNotIn("(3, -3,", second)
        self.assertIn("(4, -4,", second)
        self.assertEqual(second.count("),(") + 1, 497)

    def test_non_transient_failure_is_not_retried(self):
        runner = _ScriptedRunner(
            insert_outcomes=[("fail", "ERROR 1064 (42000): You have an error in your SQL syntax")],
            landed_ids=[],
        )
        orchestrator = _orchestrator(runner)
        with self.assertRaises(RuntimeError) as caught:
            orchestrator._insert_rows(10, has_json=False)
        self.assertIn("ERROR 1064", str(caught.exception))
        self.assertEqual(len(runner.insert_statements), 1)

    def test_batch_landed_completely_after_a_lost_response(self):
        runner = _ScriptedRunner(
            insert_outcomes=[("fail", "Lost connection to MySQL server during query")],
            landed_ids=range(1, 501),
        )
        orchestrator = _orchestrator(runner)
        orchestrator._insert_rows(500, has_json=False)
        # Everything is already present, so no second INSERT is issued.
        self.assertEqual(len(runner.insert_statements), 1)


if __name__ == "__main__":
    unittest.main()
