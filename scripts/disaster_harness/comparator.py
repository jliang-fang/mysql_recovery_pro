#!/usr/bin/env python3
"""Three-way comparison for sealed typed recovery results."""

from collections import Counter
from typing import Any, Dict, Iterable, Optional

from .manifest import canonical_json, sha256_bytes


def _index(document: Optional[Dict[str, Any]]):
    rows = document.get("rows", []) if document else []
    keys = [canonical_json(row["pk"]) for row in rows]
    duplicates = sorted(key for key, count in Counter(keys).items() if count > 1)
    return {key: row for key, row in zip(keys, rows)}, duplicates


def _prerequisites(document: Optional[Dict[str, Any]]) -> list:
    if not document:
        return ["missing_result"]
    required = ["schema_fingerprint", "schema", "rows"]
    missing = [field for field in required if field not in document]
    if document.get("status") not in (None, "SEALED_READY", "COMPLETE"):
        missing.append("result_not_comparable")
    if document.get("duplicate_primary_keys"):
        missing.append("duplicate_primary_keys")
    if not document.get("schema", {}).get("primary_key"):
        missing.append("missing_primary_key")
    return sorted(set(missing))


def compare(expected: Dict[str, Any], actual: Dict[str, Any], label: str) -> Dict[str, Any]:
    prerequisites = _prerequisites(expected) + _prerequisites(actual)
    expected_rows, expected_duplicates = _index(expected)
    actual_rows, actual_duplicates = _index(actual)
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
    if expected.get("schema_fingerprint") != (actual or {}).get("schema_fingerprint"):
        prerequisites.append("schema_mismatch")
    prerequisites = sorted(set(prerequisites))
    result = {
        "label": label,
        "status": "COMPARABLE" if not prerequisites else "NOT_COMPUTABLE",
        "prerequisites": prerequisites,
        "expected_rows": len(expected_rows),
        "actual_rows": len(actual_rows),
        "correct_rows": correct if not prerequisites else None,
        "false_negatives": [expected_rows[key]["pk"] for key in sorted(false_negatives)],
        "false_positives": [actual_rows[key]["pk"] for key in sorted(false_positives)],
        "duplicate_expected": expected_duplicates,
        "duplicate_actual": actual_duplicates,
        "field_mismatches": field_mismatches,
        "excluded_evidence_levels": (actual or {}).get("excluded_evidence_levels", []),
    }
    if not prerequisites:
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
    native_offline = compare(native, offline, "native_vs_offline")
    return {
        "comparison_version": 1,
        "status": "COMPARABLE" if all(item["status"] == "COMPARABLE" for item in
                                      (gt_native, gt_offline, native_offline)) else "NOT_COMPUTABLE",
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
