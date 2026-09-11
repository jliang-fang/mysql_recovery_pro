#!/usr/bin/env python3
"""Three-way comparison for sealed typed recovery results."""

from collections import defaultdict
from typing import Any, Dict, Optional

from .manifest import canonical_json


def _index(document: Optional[Dict[str, Any]]):
    rows = document.get("rows", []) if document else []
    grouped = defaultdict(list)
    for row in rows:
        grouped[canonical_json(row["pk"])].append(row)
    duplicates = [
        {"pk": grouped[key][0]["pk"], "count": len(grouped[key])}
        for key in sorted(grouped)
        if len(grouped[key]) > 1
    ]
    return grouped, duplicates


def _prerequisites(document: Optional[Dict[str, Any]]) -> list:
    if not document:
        return ["missing_result"]
    required = ["database", "table", "schema_fingerprint", "schema", "rows"]
    missing = [field for field in required if field not in document]
    if document.get("status") not in (None, "SEALED_READY", "COMPLETE"):
        missing.append("result_not_comparable")
    if document.get("duplicate_primary_keys"):
        missing.append("duplicate_primary_keys")
    if not document.get("schema", {}).get("primary_key"):
        missing.append("missing_primary_key")
    return sorted(set(missing))


def _identity_prerequisites(expected: Optional[Dict[str, Any]],
                            actual: Optional[Dict[str, Any]],
                            require_input_identity: bool = False) -> list:
    if not expected or not actual:
        return []
    prerequisites = []
    for field in ("database", "table"):
        if expected.get(field) != actual.get(field):
            prerequisites.append(f"{field}_mismatch")
    expected_version = expected.get("mysql_version", "")
    actual_version = actual.get("mysql_version", "")
    if expected_version and actual_version and expected_version != actual_version:
        prerequisites.append("mysql_version_mismatch")
    expected_input = expected.get("input_identity")
    actual_input = actual.get("input_identity")
    if require_input_identity and (not expected_input or not actual_input):
        prerequisites.append("missing_input_identity")
    if expected_input and actual_input:
        if expected_input.get("tablespace_sha256") != actual_input.get("tablespace_sha256"):
            prerequisites.append("native_offline_input_mismatch")
        if expected_input.get("mutation_fingerprint") != actual_input.get("mutation_fingerprint"):
            prerequisites.append("native_offline_mutation_mismatch")
        if expected_input.get("mutation_scope") != actual_input.get("mutation_scope"):
            prerequisites.append("native_offline_scope_mismatch")
    return prerequisites


def compare(expected: Optional[Dict[str, Any]], actual: Optional[Dict[str, Any]],
            label: str, require_input_identity: bool = False) -> Dict[str, Any]:
    prerequisites = _prerequisites(expected) + _prerequisites(actual)
    expected_groups, expected_duplicates = _index(expected)
    actual_groups, actual_duplicates = _index(actual)
    if expected_duplicates:
        prerequisites.append("duplicate_expected_rows")
    if actual_duplicates:
        prerequisites.append("duplicate_actual_rows")
    prerequisites.extend(_identity_prerequisites(
        expected, actual, require_input_identity=require_input_identity))

    expected_rows = {key: rows[0] for key, rows in expected_groups.items()}
    actual_rows = {key: rows[0] for key, rows in actual_groups.items()}
    expected_keys = set(expected_rows)
    actual_keys = set(actual_rows)
    common = expected_keys & actual_keys
    field_mismatches = []
    for key in sorted(common):
        expected_row = expected_rows[key]
        actual_row = actual_rows[key]
        if expected_row.get("row_hash") != actual_row.get("row_hash"):
            expected_values = expected_row.get("values", {})
            actual_values = actual_row.get("values", {})
            names = sorted(set(expected_values) | set(actual_values))
            fields = [name for name in names if expected_values.get(name) != actual_values.get(name)]
            field_mismatches.append({"pk": expected_row["pk"], "fields": fields})
    correct = len(common) - len(field_mismatches)
    false_negatives = expected_keys - actual_keys
    false_positives = actual_keys - expected_keys
    if expected and expected.get("schema_fingerprint") != (actual or {}).get("schema_fingerprint"):
        prerequisites.append("schema_mismatch")
    prerequisites = sorted(set(prerequisites))
    comparable = not prerequisites
    strict_pass = comparable and not false_negatives and not false_positives and not field_mismatches
    result = {
        "label": label,
        "status": "COMPARABLE" if comparable else "NOT_COMPUTABLE",
        "prerequisites": prerequisites,
        "expected_rows": sum(len(rows) for rows in expected_groups.values()),
        "actual_rows": sum(len(rows) for rows in actual_groups.values()),
        "correct_rows": correct if comparable else None,
        "false_negatives": [expected_rows[key]["pk"] for key in sorted(false_negatives)],
        "false_positives": [actual_rows[key]["pk"] for key in sorted(false_positives)],
        "duplicate_expected": expected_duplicates,
        "duplicate_actual": actual_duplicates,
        "field_mismatches": field_mismatches,
        "excluded_evidence_levels": (actual or {}).get("excluded_evidence_levels", []),
        "strict_pass": strict_pass,
    }
    if comparable:
        result["precision"] = correct / len(actual_rows) if actual_rows else 1.0 if not expected_rows else 0.0
        result["recall"] = correct / len(expected_rows) if expected_rows else 1.0
    else:
        result["precision"] = None
        result["recall"] = None
    return result


def compare_three_way(ground_truth: Dict[str, Any], native: Optional[Dict[str, Any]],
                      offline: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    gt_native = compare(ground_truth, native, "ground_truth_vs_native")
    gt_offline = compare(ground_truth, offline, "ground_truth_vs_offline")
    native_offline = compare(native, offline, "native_vs_offline", require_input_identity=True)
    comparisons = (gt_native, gt_offline, native_offline)
    return {
        "comparison_version": 2,
        "status": "COMPARABLE" if all(item["status"] == "COMPARABLE" for item in comparisons)
        else "NOT_COMPUTABLE",
        "strict_pass": all(item["strict_pass"] for item in comparisons),
        "comparisons": {
            "ground_truth_vs_native": gt_native,
            "ground_truth_vs_offline": gt_offline,
            "native_vs_offline": native_offline,
        },
    }


def write_comparison(path, report: Dict[str, Any]):
    from pathlib import Path
    path = Path(path)
    if path.exists():
        raise FileExistsError(f"refusing to overwrite comparison: {path}")
    path.write_text(canonical_json(report) + "\n", encoding="utf-8")
    return path
