#!/usr/bin/env python3
"""Unit tests for the shared deterministic fixture module."""

import json
import unittest

from scripts.disaster_validation import fixtures as fx


class DeterminismTests(unittest.TestCase):
    def _row_values(self, spec, database: str, first: int, last: int) -> list[str]:
        statement = spec.insert_sql(database, first, last)
        payload = statement.split("VALUES ", 1)[1].rstrip(";")
        return self._split_rows(payload)

    @staticmethod
    def _split_rows(payload: str) -> list[str]:
        """Split a VALUES list on top-level '),' boundaries."""
        rows, depth, current = [], 0, ""
        quote = False
        index = 0
        while index < len(payload):
            char = payload[index]
            if quote:
                if char == "\\":
                    current += payload[index:index + 2]
                    index += 2
                    continue
                if char == "'":
                    if payload[index + 1:index + 2] == "'":
                        current += "''"
                        index += 2
                        continue
                    quote = False
            elif char == "'":
                quote = True
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            elif char == "," and depth == 0:
                rows.append(current.strip())
                current = ""
                index += 1
                continue
            current += char
            index += 1
        if current.strip():
            rows.append(current.strip())
        return rows

    def test_a_row_is_identical_wherever_the_batch_starts(self):
        """Row identity must not depend on the generation window."""
        spec = fx.build_suite("numeric", "8.0")
        window_a = self._row_values(spec, "db", 0, 249)
        window_b = self._row_values(spec, "db", 250, 499)
        whole = self._row_values(spec, "db", 0, 499)
        self.assertEqual(whole, window_a + window_b)

    def test_row_zero_and_one_are_reproducible_across_calls(self):
        spec = fx.build_suite("text", "8.0")
        self.assertEqual(spec.insert_sql("db", 0, 3), spec.insert_sql("db", 0, 3))

    def test_columns_are_not_constant_across_rows(self):
        spec = fx.build_suite("numeric", "8.0")
        rows = self._row_values(spec, "db", 100, 120)
        self.assertGreater(len(set(rows)), 15)


class DdlTests(unittest.TestCase):
    def test_every_suite_renders_ddl_and_schema_json_without_pk(self):
        for name in fx.suite_names():
            spec = fx.build_suite(name, "8.0")
            self.assertTrue(spec.ddl("db").startswith(f"CREATE TABLE `db`.`{spec.name}`"))
            document = spec.schema_json()
            self.assertEqual(document["table_name"], spec.name)
            self.assertTrue(document["columns"])

    def test_schema_json_marks_key_columns_not_null_and_ordered_first(self):
        spec = fx.build_suite("composite", "8.0")
        document = spec.schema_json()
        positions = {entry["name"]: entry["physical_pos"] for entry in document["columns"]}
        self.assertEqual(positions["id_a"], 0)
        self.assertEqual(positions["id_b"], 1)
        by_name = {entry["name"]: entry for entry in document["columns"]}
        self.assertFalse(by_name["id_a"]["is_nullable"])
        self.assertFalse(by_name["id_b"]["is_nullable"])
        self.assertEqual(document["indexes"][0]["columns"], ["id_a", "id_b"])

    def test_schema_json_uses_parser_domain_type_ids(self):
        spec = fx.build_suite("text", "8.0")
        by_name = {entry["name"]: entry for entry in spec.schema_json()["columns"]}
        self.assertEqual(by_name["c_blob"]["type"], fx.PARSER_BLOB)
        self.assertEqual(by_name["c_binary"]["type"], fx.PARSER_STRING)
        # Binary columns are identified by collation 63, not by the type id.
        self.assertEqual(by_name["c_varbinary"]["collation_id"], fx.COLLATION_BINARY)
        self.assertEqual(by_name["c_varchar"]["collation_id"], fx.COLLATION_UTF8MB4)

    def test_decimal_and_temporal_precision_reach_the_schema_document(self):
        spec = fx.build_suite("numeric", "8.0")
        by_name = {entry["name"]: entry for entry in spec.schema_json()["columns"]}
        self.assertEqual(by_name["c_dec_max"]["numeric_precision"], 65)
        self.assertEqual(by_name["c_dec_max"]["numeric_scale"], 30)
        temporal = fx.build_suite("temporal", "8.0")
        temporal_by_name = {e["name"]: e for e in temporal.schema_json()["columns"]}
        self.assertEqual(temporal_by_name["c_datetime6"]["datetime_precision"], 6)
        self.assertEqual(temporal_by_name["c_time6"]["datetime_precision"], 6)

    def test_enum_and_set_members_are_base64_encoded_in_the_schema_document(self):
        spec = fx.build_suite("structured", "5.7")
        by_name = {entry["name"]: entry for entry in spec.schema_json()["columns"]}
        members = [json.loads(json.dumps(e))["name"] for e in by_name["c_enum"]["elements"]]
        self.assertEqual(len(members), 4)
        self.assertIn("数据库", [__import__("base64").b64decode(m).decode() for m in members])
        self.assertEqual(len(by_name["c_set"]["elements"]), 4)


class VersionGatingTests(unittest.TestCase):
    def test_json_suite_is_unavailable_before_5_7(self):
        ok, reason = fx.suite_supports("structured", "5.6")
        self.assertFalse(ok)
        self.assertIn("5.7", reason)
        with self.assertRaises(KeyError):
            fx.build_suite("structured", "5.6")

    def test_every_suite_available_on_every_supported_version_except_json(self):
        for version in ("5.6", "5.7", "8.0", "8.4", "9.0"):
            for name in fx.suite_names():
                ok, _ = fx.suite_supports(name, version)
                expected = not (name == "structured" and version == "5.6")
                self.assertEqual(ok, expected, f"{name} on {version}")

    def test_unknown_suite_is_rejected(self):
        with self.assertRaises(KeyError):
            fx.build_suite("does_not_exist", "8.0")


class ValueCoverageTests(unittest.TestCase):
    def test_null_rows_are_always_present_in_nullable_columns(self):
        spec = fx.build_suite("numeric", "8.0")
        statement = spec.insert_sql("db", 0, 40)
        self.assertIn("NULL", statement)

    def test_special_characters_are_escaped_for_sql(self):
        spec = fx.build_suite("text", "8.0")
        statement = spec.insert_sql("db", 2, 2)
        self.assertIn("\\n", statement)
        self.assertIn("\\t", statement)
        self.assertIn("''", statement)

    def test_binary_literals_are_even_length_hex(self):
        spec = fx.build_suite("text", "8.0")
        for row in (0, 1, 3, 9):
            statement = spec.insert_sql("db", row, row)
            for token in statement.split("X'")[1:]:
                literal = token.split("'", 1)[0]
                self.assertEqual(len(literal) % 2, 0, f"row {row} literal {literal!r}")

    def test_empty_and_max_boundary_values_are_pinned(self):
        spec = fx.build_suite("text", "8.0")
        blob = spec.column("c_blob")
        self.assertEqual(blob.literal(0), "X''")
        self.assertGreater(len(blob.literal(1)), 4000)

    def test_row_count_batches_cover_every_row_exactly_once(self):
        ranges = list(fx.row_count_batches(1201, batch_size=500))
        self.assertEqual(ranges, [(0, 499), (500, 999), (1000, 1200)])
        covered = [row for first, last in ranges for row in range(first, last + 1)]
        self.assertEqual(covered, list(range(1201)))

    def test_row_count_batches_handles_zero_and_small(self):
        self.assertEqual(list(fx.row_count_batches(0)), [])
        self.assertEqual(list(fx.row_count_batches(1)), [(0, 0)])


class NoKeyTableTests(unittest.TestCase):
    def test_nokey_table_is_marked_not_comparable_and_has_no_primary_key(self):
        spec = fx.build_suite("nokey", "8.0")
        self.assertFalse(spec.comparable)
        self.assertEqual(spec.primary_key, ())
        self.assertNotIn("PRIMARY KEY", spec.ddl("db"))

    def test_schema_file_write_refuses_to_overwrite(self):
        import tempfile
        from pathlib import Path
        spec = fx.build_suite("numeric", "8.0")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "schema.json"
            fx.write_schema_file(spec, path)
            self.assertTrue(path.is_file())
            with self.assertRaises(FileExistsError):
                fx.write_schema_file(spec, path)


if __name__ == "__main__":
    unittest.main()
