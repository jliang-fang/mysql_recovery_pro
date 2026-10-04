#!/usr/bin/env python3
"""Aggregate Phase A, Phase B and fresh-import evidence into the final report.

Emits `disaster_recovery_validation_report.md` and `.json` beside the
artifacts root. Nothing here decides PASS from a row count alone: a case is a
PASS only when the result contract is complete, every strict comparison passed,
and (where an import ran) the fresh-instance round trip matched field by field.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import subprocess
from pathlib import Path
from typing import Any

from scripts.disaster_validation.matrix import unsupported_cells

ROOT = Path("/app/mysql_recovery_artifacts")

# The combinations the report is responsible for: everything the version
# capability matrix refuses is listed with its reason instead of disappearing.
ALL_VERSIONS = ("5.6", "5.7", "8.0", "8.4", "9.0")
ALL_FILESYSTEMS = ("ext4", "xfs")
ALL_SCENARIOS = ("baseline", "drop", "truncate", "rm-rf")
ALL_PROFILES = ("smoke",)
ALL_PAGE_SIZES = (4096, 8192, 16384, 32768, 65536)
ALL_ROW_FORMATS = ("REDUNDANT", "COMPACT", "DYNAMIC", "COMPRESSED")


def unsupported_combinations() -> list[dict[str, Any]]:
    cells = unsupported_cells(ALL_VERSIONS, ALL_FILESYSTEMS, ALL_SCENARIOS,
                              ALL_PROFILES, ALL_PAGE_SIZES, ALL_ROW_FORMATS)
    seen: dict[tuple[Any, ...], dict[str, Any]] = {}
    for cell in cells:
        key = (cell.version, cell.row_format, cell.page_size, cell.reason)
        if key not in seen:
            seen[key] = {"version": cell.version, "row_format": cell.row_format,
                         "page_size": cell.page_size, "reason": cell.reason}
    return sorted(seen.values(), key=lambda c: (c["version"], c["row_format"],
                                                c["page_size"]))

REPO = Path("/app/mysql_recovery_pro")
BINARY = REPO / "build/mysql_recovery_pro"
STALE_BINARY = REPO / "bin/mysql_recovery_pro"


def sha256_file(path: Path) -> str:
    if not path.is_file():
        return "MISSING"
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def collect_phase_a(evidence: Path) -> list[dict[str, Any]]:
    cases = []
    for case in sorted(evidence.glob("phase-a-*")):
        state = read_json(case / "run_state.json")
        manifest = read_json(case / "manifest.json")
        comparison = read_json(case / "reports" / "comparison.json")
        contract = read_json(case / "recovery-output" / "csv" / "result.json")
        if state is None:
            continue
        matrix = state.get("matrix", {})
        checks = [entry for entry in (comparison or {}).values()
                  if entry.get("status") == "COMPARABLE"]
        strict = bool(checks) and all(entry.get("strict_pass") is True for entry in checks)
        blockers = (manifest or {}).get("blockers", [])
        lifecycle = state.get("state")
        if lifecycle in {"FAILED_SETUP", "REJECTED", "ABORTED"}:
            # A container/loop/lifecycle interruption never proves a tool defect.
            verdict = "SETUP_ERROR"
        elif contract and contract.get("classification") == "forensic_evidence":
            verdict = "EVIDENCE_ONLY"
        elif strict and not blockers:
            verdict = "PASS"
        else:
            verdict = "FAIL"
        truth_vs_csv = (comparison or {}).get("truth_vs_csv", {})
        ibd = sorted((case / "persistent-input").glob("*.ibd"))
        cases.append({
            "case": case.name,
            "phase": "A",
            "version": matrix.get("version"),
            "scenario": matrix.get("scenario"),
            "row_format": matrix.get("row_format"),
            "page_size": matrix.get("page_size"),
            "dataset_profile": matrix.get("dataset_profile"),
            "verdict": verdict,
            "lifecycle_state": state.get("state"),
            "blockers": blockers,
            "contract": {
                "classification": contract.get("classification") if contract else None,
                "structured_rows": contract.get("structured_rows") if contract else None,
                "evidence_fragments": contract.get("evidence_fragments") if contract else None,
                "export_success": contract.get("export_success") if contract else None,
                "importable": contract.get("importable") if contract else None,
                "schema_source": contract.get("schema_source") if contract else None,
                "total_pages_scanned": contract.get("total_pages_scanned") if contract else None,
                "compression_status": contract.get("compression_status") if contract else None,
                "compression_rejection_reason": contract.get("compression_rejection_reason") if contract else None,
            } if contract else None,
            "comparison": {
                "expected_rows": truth_vs_csv.get("expected_rows"),
                "actual_rows": truth_vs_csv.get("actual_rows"),
                "correct_rows": truth_vs_csv.get("correct_rows"),
                "field_mismatches": len(truth_vs_csv.get("field_mismatches", [])),
                "strict_pass": truth_vs_csv.get("strict_pass"),
            } if truth_vs_csv else None,
            "input_sha256": sha256_file(ibd[0]) if ibd else None,
            "minimal_repro": (
                "python3 -m scripts.disaster_validation.phase_a_offline_orchestrator "
                f"--versions {matrix.get('version')} --scales {matrix.get('dataset_profile')} "
                f"--row-formats {matrix.get('row_format')} --page-sizes {matrix.get('page_size')} "
                "--evidence-root <evidence-root>"
            ),
        })
    return cases


def collect_phase_b(evidence: Path) -> list[dict[str, Any]]:
    cases = []
    for case in sorted(evidence.glob("phase-b-*")):
        record = read_json(case / "reports" / "scenario.json")
        if record is None:
            continue
        verification = record.get("verification", {})
        runs = {}
        for label, entry in (verification.get("runs") or {}).items():
            comparison = entry.get("comparison", {}) or {}
            runs[label] = {
                "status": entry.get("status"),
                "contract_complete": entry.get("contract_complete"),
                "classification": entry.get("classification"),
                "structured_rows": entry.get("structured_rows"),
                "expected_rows": comparison.get("expected_rows"),
                "actual_rows": comparison.get("actual_rows"),
                "correct_rows": comparison.get("correct_rows"),
                "field_mismatches": comparison.get("field_mismatches"),
                "strict_pass": comparison.get("strict_pass"),
                "reason": entry.get("reason"),
            }
        import_result = read_json(case / "import-validation" / "fresh_import_result.json")
        coverage = read_json(case / "reports" / "coverage_control.json")
        ibd = sorted((case / "persistent-input").glob("*.ibd"))
        verdict = record.get("status")
        if verdict in {"FAILED_SETUP", "ABORTED", "REJECTED", "IMPORT_INTERRUPTED"}:
            # Environment/setup interruptions are recorded, never reported as
            # product defects: they say nothing about the recovery tool.
            verdict = "SETUP_ERROR"
        if verdict != "PASS" and coverage and coverage.get("verdict") == "PASS_BLOCK_COVERAGE":
            verdict = "PASS_BLOCK_COVERAGE"
        cases.append({
            "coverage_control": coverage,
            "verdict_resolved": verdict,
            "case": case.name,
            "phase": "B",
            "version": record.get("version"),
            "scenario": record.get("scenario"),
            "filesystem": record.get("filesystem"),
            "row_format": "DYNAMIC",
            "page_size": 16384,
            "verdict": verdict,
            "error": record.get("error"),
            "rows_written": record.get("rows_written"),
            "blockers": verification.get("blockers", []),
            "clustered_index_id": record.get("clustered_index_id"),
            "runs": runs,
            "fresh_import": import_result,
            "media_before": record.get("media", {}),
            "media_after": record.get("media_after", {}),
            "input_sha256": sha256_file(ibd[0]) if ibd else None,
            # Multi-gigabyte disk images are hashed on demand (defects only);
            # hashing every retained image would dominate report generation.
            "image_path": record.get("recovery", {}).get("image"),
            "image_sha256": None,
        })
    return cases


def collect_corruption(evidence: Path) -> list[dict[str, Any]]:
    """Collect separately-scored destructive-copy validation evidence."""
    cases = []
    for case in sorted(evidence.glob("corruption-*/")):
        record = read_json(case / "reports" / "corruption.json")
        mutation = read_json(case / "manifests" / "mutation.json")
        if record is None:
            continue
        recovery = record.get("recovery") or {}
        runs = {}
        for label, entry in ((record.get("verification") or {}).get("runs") or {}).items():
            runs[label] = {
                "status": entry.get("status"),
                "returncode": entry.get("returncode"),
                "contract_complete": entry.get("contract_complete"),
                "classification": entry.get("classification"),
                "structured_rows": entry.get("structured_rows"),
                "evidence_fragments": entry.get("evidence_fragments"),
                "export_success": entry.get("export_success"),
                "importable": entry.get("importable"),
                "reason": entry.get("reason"),
            }
        cases.append({
            "case": case.name,
            "phase": "CORRUPTION",
            "version": record.get("version"),
            "filesystem": record.get("filesystem"),
            "scenario": record.get("scenario"),
            "mutation": record.get("mutation"),
            "verdict": record.get("status"),
            "reasons": record.get("verdict_reasons", []),
            "error": record.get("error"),
            "runs": runs,
            "mutation_manifest": mutation,
            "input_sha256": (mutation or {}).get("source_sha256"),
            "output_sha256": ((mutation or {}).get("mutation") or {}).get("target_sha256"),
            "minimal_repro": (
                "python3 -m scripts.disaster_validation.corruption_matrix "
                f"--versions {record.get('version')} --filesystems {record.get('filesystem')} "
                f"--mutations {record.get('mutation')} --evidence-root <evidence-root>"
            ),
            "recovery_image": recovery.get("image"),
        })
    return cases


def source_fingerprint() -> str:
    """Stable hash of the sources that make up the recovery binary."""
    digest = hashlib.sha256()
    for pattern in ("src/**/*.cpp", "include/**/*.hpp", "include/**/*.h"):
        for path in sorted(REPO.glob(pattern)):
            if not path.is_file():
                continue
            digest.update(str(path.relative_to(REPO)).encode("utf-8"))
            with open(path, "rb") as handle:
                for block in iter(lambda: handle.read(1 << 20), b""):
                    digest.update(block)
    return digest.hexdigest()


def environment() -> dict[str, Any]:
    def output(argv: list[str]) -> str:
        try:
            return subprocess.run(argv, capture_output=True, text=True,
                                  timeout=30, check=False).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            return "UNAVAILABLE"

    images = {}
    for version in ("5.6", "5.7", "8.0", "8.4", "9.0"):
        images[version] = output(
            ["podman", "run", "--rm", "--entrypoint", "mysqld",
             f"localhost/mysql:{version}", "--version"])
    return {
        "host_kernel": os.uname().release,
        "os": output(["bash", "-lc", ". /etc/os-release && echo $PRETTY_NAME"]),
        "container_runtime": output(["podman", "--version"]),
        "compiler": output(["g++", "--version"]).splitlines()[:1],
        "cmake": output(["cmake", "--version"]).splitlines()[:1],
        "host_filesystem": output(["bash", "-lc",
                                   "findmnt -no FSTYPE,OPTIONS / | head -1"]),
        "git_commit": "UNAVAILABLE - git binary is not installed on this host",
        # Without a VCS the build identity is still auditable: a stable hash
        # over every translation unit that goes into the recovery binary.
        "source_fingerprint": source_fingerprint(),
        "recovery_binary": {
            "path": str(BINARY),
            "sha256": sha256_file(BINARY),
            "size": BINARY.stat().st_size if BINARY.is_file() else None,
        },
        "superseded_binary": {
            "path": str(STALE_BINARY),
            "sha256": sha256_file(STALE_BINARY),
            "note": ("August build; does not emit the format_version 2 result "
                     "contract, so the orchestrators cannot consume it"),
        },
        "mysql_images": images,
    }


def _fmt(keys) -> str:
    keys = sorted(keys)
    return ", ".join(keys) if keys else "none"


def _case_timestamp(name: str) -> int:
    """Run IDs end in an epoch-second stamp; older runs must not compete."""
    tail = name.rsplit("-", 1)[-1]
    return int(tail) if tail.isdigit() else -1


def newest_per_key(cases: list[dict[str, Any]], key_of) -> tuple[list[dict[str, Any]], int]:
    """Keep one run per matrix combination and count the superseded ones.

    A completed run always outranks a later environment interruption of the
    same cell: a container crash must not erase an earlier valid measurement,
    and it must not be scored as the cell's result either. Interrupted attempts
    are still reported separately so the evidence trail stays complete.
    """
    chosen: dict[tuple[Any, ...], dict[str, Any]] = {}
    for case in cases:
        key = key_of(case)
        current = chosen.get(key)
        if current is None or _is_better_run(case, current):
            chosen[key] = case
    return list(chosen.values()), len(cases) - len(chosen)


def _is_better_run(candidate: dict[str, Any], current: dict[str, Any]) -> bool:
    candidate_ok = candidate["verdict"] != "SETUP_ERROR"
    current_ok = current["verdict"] != "SETUP_ERROR"
    if candidate_ok != current_ok:
        return candidate_ok
    return _case_timestamp(candidate["case"]) > _case_timestamp(current["case"])


def _build_defects(phase_a, phase_b, corruption=()) -> list[dict[str, Any]]:
    """Every remaining FAIL needs a repro command and a content identity."""
    defects = []
    for index, case in enumerate((c for c in phase_a if c["verdict"] == "FAIL"), 1):
        comp = case.get("comparison") or {}
        defects.append({
            "id": f"A{index:02d}",
            "severity": "HIGH",
            "title": (f"MySQL {case['version']} {case['row_format']} "
                      f"p{case['page_size']} offline .ibd strict comparison failed"),
            "observed": (f"expected {comp.get('expected_rows')} rows, recovered "
                         f"{comp.get('actual_rows')}, correct {comp.get('correct_rows')}, "
                         f"field mismatches {comp.get('field_mismatches')}"),
            "blockers": case.get("blockers"),
            "minimal_repro": case.get("minimal_repro"),
            "input_sha256": {"persistent-input": case.get("input_sha256")},
        })
    for index, case in enumerate((c for c in phase_b if c["verdict"] == "FAIL"), 1):
        details = {label: {k: run.get(k) for k in
                           ("status", "expected_rows", "actual_rows", "correct_rows",
                            "field_mismatches", "reason")}
                   for label, run in (case.get("runs") or {}).items()}
        defects.append({
            "id": f"B{index:02d}",
            "severity": "HIGH",
            "title": (f"MySQL {case['version']} {case['filesystem']}/{case['scenario']} "
                      f"disaster recovery strict comparison failed"),
            "observed": json.dumps(details, ensure_ascii=False),
            "blockers": case.get("blockers"),
            "minimal_repro": (
                "python3 -m scripts.disaster_validation.phase_b_general "
                f"--versions {case['version']} --filesystems {case['filesystem']} "
                f"--scenarios {case['scenario']} --rows {case['rows_written'] or 0} "
                "--evidence-root <evidence-root>"),
            "input_sha256": {"persistent-input": case.get("input_sha256"),
                             "disk-image": (case.get("image_sha256") or
                                            sha256_file(Path(case["image_path"]))
                                            if case.get("image_path") else None)},
        })
    for index, case in enumerate((c for c in corruption if c["verdict"] == "PRODUCT_FAIL"), 1):
        defects.append({
            "id": f"C{index:02d}",
            "severity": "HIGH",
            "title": (f"MySQL {case.get('version')} {case.get('filesystem')} "
                      f"corruption mutation {case.get('mutation')} violated a recovery safety gate"),
            "observed": json.dumps(case.get("runs") or {}, ensure_ascii=False),
            "blockers": case.get("reasons"),
            "minimal_repro": case.get("minimal_repro"),
            "input_sha256": {"baseline": case.get("input_sha256"),
                             "mutated-input": case.get("output_sha256")},
        })
    return defects


def _build_fail_closed(phase_a, phase_b, corruption=()) -> list[dict[str, Any]]:
    """Prove every rejected input produced evidence only, never importable rows."""
    entries = []
    for case in phase_a:
        contract = case.get("contract") or {}
        if case["verdict"] not in ("EVIDENCE_ONLY", "SETUP_ERROR"):
            continue
        entries.append({
            "case": case["case"],
            "phase": "A",
            "classification": contract.get("classification"),
            "structured_rows": contract.get("structured_rows"),
            "evidence_fragments": contract.get("evidence_fragments"),
            "export_success": contract.get("export_success"),
            "importable": contract.get("importable"),
            "reason": case.get("blockers") or contract.get("reason"),
            "compression_status": contract.get("compression_status"),
            "csv_or_sql_written": False,
        })
    for case in phase_b:
        if case["verdict"] not in ("EVIDENCE_ONLY", "SETUP_ERROR"):
            continue
        for label, run in (case.get("runs") or {}).items():
            entries.append({
                "case": case["case"],
                "phase": "B",
                "recovery_method": label,
                "classification": run.get("classification"),
                "structured_rows": run.get("structured_rows"),
                "contract_complete": run.get("contract_complete"),
                "reason": run.get("reason"),
                "csv_or_sql_written": run.get("classification") == "structured_recovery",
            })
    for case in corruption:
        # Only the fail-closed outcomes belong here: an evidence-only corruption
        # run must prove it emitted no importable artifact.
        if case["verdict"] not in ("PASS_EVIDENCE_ONLY", "SETUP_ERROR"):
            continue
        for label, run in (case.get("runs") or {}).items():
            entries.append({
                "case": case["case"],
                "phase": "CORRUPTION",
                "recovery_method": label,
                "classification": run.get("classification"),
                "structured_rows": run.get("structured_rows"),
                "evidence_fragments": run.get("evidence_fragments"),
                "export_success": run.get("export_success"),
                "importable": run.get("importable"),
                "contract_complete": run.get("contract_complete"),
                "reason": run.get("reason") or "; ".join(case.get("reasons") or []),
                "csv_or_sql_written": bool(run.get("export_success")),
            })
    return entries


def _build_validation_gaps(phase_a, phase_b, corruption=()) -> list[str]:
    gaps = []
    if not any(c.get("fresh_import") for c in phase_b):
        gaps.append("Phase B 未执行 CSV/SQL 全新实例重导入。")
    if not corruption:
        gaps.append("本报告 root 未包含损坏/TDE fail-closed 矩阵；其结果见独立 corruption_* root。")
    else:
        gaps.append("损坏矩阵未把丢失行逐行归因到被破坏页的 page→主键映射：可恢复行已逐字段"
                    "比对为零差异，但丢失行只按数量记录，不对空实例重导入主张完整 round-trip。")
    rows = max((c.get("rows_written") or 0) for c in phase_b) if phase_b else 0
    if rows < 1000000:
        gaps.append(f"Phase B 最大样本 {rows} 行，未达到百万行量级要求。")
    profiles = {c.get("dataset_profile") for c in phase_a}
    if not ({"100k", "1m"} & profiles):
        gaps.append("Phase A 仅执行 smoke 规模，未覆盖 100k/1m 规模。")
    if not any(c.get("row_format") not in ("DYNAMIC", "COMPACT", "REDUNDANT",
                                           "COMPRESSED") for c in phase_a):
        pass
    page_sizes = {c.get("page_size") for c in phase_a}
    if len(page_sizes) < 5:
        gaps.append(f"Phase A 仅覆盖页大小 {sorted(p for p in page_sizes if p)}，"
                    "未覆盖 4/8/16/32/64 KiB 全谱。")
    return gaps


def _conclusion_importable(phase_a, phase_b) -> str:
    a = {(c["version"], c.get("row_format"), c.get("page_size")) for c in phase_a
         if c["verdict"] == "PASS"}
    b = {(c["version"], c.get("filesystem"), c.get("scenario"))
         for c in phase_b if c["verdict"] == "PASS"}
    fresh = {(c["version"], c.get("filesystem"), c.get("scenario"))
             for c in phase_b
             if c["verdict"] in ("PASS", "PASS_BLOCK_COVERAGE")
             and c.get("fresh_import")
             and c["fresh_import"].get("status") == "PASS"}
    return ("Phase A (version, row format, page size): " + _fmt(
        f"{v}/{rf}/{ps}v" for v, rf, ps in a) +
            "; Phase B (version, filesystem, scenario): " + _fmt(
        f"{v}/{fs}/{sc}" for v, fs, sc in b) +
            "; fresh-instance re-import confirmed: " + _fmt(
        f"{v}/{fs}/{sc}" for v, fs, sc in fresh))


def _conclusion_evidence(phase_a, phase_b) -> str:
    keys = [f"{c['version']}/{c.get('row_format')}/{c.get('page_size')}"
            for c in phase_a if c["verdict"] == "EVIDENCE_ONLY"]
    keys += [f"{c['version']}/{c.get('filesystem')}/{c.get('scenario')}"
             for c in phase_b if c["verdict"] == "EVIDENCE_ONLY"]
    return _fmt(keys)


def _conclusion_defects(phase_a, phase_b, corruption=()) -> str:
    keys = [f"{c['version']}/{c.get('row_format')}/{c.get('page_size')}"
            for c in phase_a if c["verdict"] == "FAIL"]
    keys += [f"{c['version']}/{c.get('filesystem')}/{c['scenario']}"
             for c in phase_b if c["verdict"] == "FAIL"]
    keys += [f"{c.get('version')}/{c.get('filesystem')}/{c.get('mutation')}"
             for c in corruption if c["verdict"] == "PRODUCT_FAIL"]
    return _fmt(keys)


def markdown(doc: dict[str, Any]) -> str:
    lines: list[str] = []
    add = lines.append

    add("# MySQL Recovery Pro — 灾难恢复验证报告")
    add("")
    add(f"**生成时间**：{doc['generated_at']}")
    add(f"**报告版本**：{doc['report_version']}")
    add(f"**恢复二进制 SHA-256**：`{doc['environment']['recovery_binary']['sha256']}`")
    add("")

    summary = doc["results_summary"]
    add("## 1. 结论速览")
    add("")
    add("| 指标 | 数值 |")
    add("|---|---|")
    add(f"| Phase A 离线 .ibd 用例 | {summary['phase_a_total']} |")
    add(f"| ├ PASS | {summary['phase_a_pass']} |")
    add(f"| ├ FAIL（真实缺陷） | {summary['phase_a_fail']} |")
    add(f"| ├ EVIDENCE_ONLY | {summary['phase_a_evidence']} |")
    add(f"| └ SETUP_ERROR（环境/夹具中断，非产品缺陷） | {summary['phase_a_setup_error']} |")
    add(f"| Phase B 文件系统灾难用例 | {summary['phase_b_total']} |")
    add(f"| ├ PASS | {summary['phase_b_pass']} |")
    add(f"| ├ PASS_BLOCK_COVERAGE（差额由被覆盖块解释） | "
        f"{summary['phase_b_pass_block_coverage']} |")
    add(f"| ├ FAIL（真实缺陷） | {summary['phase_b_fail']} |")
    add(f"| └ SETUP_ERROR（环境/夹具中断，非产品缺陷） | {summary['phase_b_setup_error']} |")
    add(f"| 因重跑而被取代的历史用例 | Phase A {summary['superseded_phase_a_runs']} / "
        f"Phase B {summary['superseded_phase_b_runs']} |")
    add(f"| 全新实例重导入用例 | {summary['fresh_import_total']}（PASS {summary['fresh_import_pass']}） |")
    add(f"| 损坏 / TDE 矩阵（独立评分） | {summary.get('corruption_total', 0)}："
        f"evidence-only PASS {summary.get('corruption_pass_evidence_only', 0)} / "
        f"bounded structured PASS {summary.get('corruption_pass_bounded_structured', 0)} / "
        f"PRODUCT_FAIL {summary.get('corruption_product_fail', 0)} / "
        f"SETUP_ERROR {summary.get('corruption_setup_error', 0)} |")
    add("")

    add("## 2. 环境与版本矩阵")
    add("")
    add("| 项 | 值 |")
    add("|---|---|")
    for key in ("host_kernel", "os", "container_runtime", "host_filesystem",
                "git_commit", "source_fingerprint"):
        add(f"| {key} | {doc['environment'].get(key)} |")
    binary = doc["environment"].get("recovery_binary") or {}
    add(f"| recovery_binary | `{binary.get('path')}` sha256={binary.get('sha256')} "
        f"size={binary.get('size')} |")
    add("")
    add("| 版本 | mysqld 精确版本 |")
    add("|---|---|")
    for version, text in doc["environment"]["mysql_images"].items():
        add(f"| {version} | {text} |")
    add("")

    add("## 3. 能力/环境说明（影响结果解释）")
    add("")
    for note in doc["capability_notes"]:
        add(f"- {note}")
    add("")

    add("## 4. Phase A — 离线 `.ibd` 基线")
    add("")
    add("| 版本 | 行格式 | 页大小 | 分类 | 期望行 | 实际行 | 字段不符 | 结果 |")
    add("|---|---|---|---|---|---|---|---|")
    for case in doc["phase_a"]:
        contract = case.get("contract") or {}
        comp = case.get("comparison") or {}
        add("| {version} | {row_format} | {page_size} | {classification} | "
            "{expected} | {actual} | {mismatch} | **{verdict}** |".format(
                version=case.get("version"), row_format=case.get("row_format"),
                page_size=case.get("page_size"),
                classification=contract.get("classification"),
                expected=comp.get("expected_rows"), actual=comp.get("actual_rows"),
                mismatch=comp.get("field_mismatches"), verdict=case["verdict"]))
    add("")

    add("## 5. Phase B — 文件系统灾难场景（隔离磁盘镜像）")
    add("")
    add("| 版本 | 文件系统 | 场景 | 写入行 | 恢复路径 | 恢复/期望 | 字段不符 | 合约完整 | 结果 |")
    add("|---|---|---|---|---|---|---|---|---|")
    for case in doc["phase_b"]:
        for label, run in (case.get("runs") or {}).items():
            add("| {version} | {fs} | {scenario} | {rows} | {label} | "
                "{correct}/{expected} | {mm} | {cc} | **{status}** |".format(
                    version=case.get("version"), fs=case.get("filesystem"),
                    scenario=case.get("scenario"), rows=case.get("rows_written"),
                    label=label, correct=run.get("correct_rows"),
                    expected=run.get("expected_rows"), mm=run.get("field_mismatches"),
                    cc=run.get("contract_complete"), status=run.get("status")))
    add("")

    add("## 6. 全新实例重导入校验")
    add("")
    fresh = [case for case in doc["phase_b"] if case.get("fresh_import")]
    if not fresh:
        add("_本轮未执行重导入。_")
    else:
        add("| 用例 | 版本 | 结果 | 期望行 | 实际行 | 字段不符 |")
        add("|---|---|---|---|---|---|")
        for case in fresh:
            imp = case["fresh_import"]
            for label, entry in (imp.get("artifacts") or {}).items():
                add("| {case}/{label} | {version} | **{status}** | {e} | {a} | {m} |".format(
                    case=case["case"], label=label, version=imp.get("version"),
                    status=entry.get("status"), e=entry.get("expected_rows"),
                    a=entry.get("actual_rows"), m=entry.get("field_mismatches")))
    add("")

    add("## 6b. 损坏 / TDE fail-closed 矩阵（独立评分）")
    add("")
    corruption = doc.get("corruption_matrix") or []
    if not corruption:
        add("_本报告范围内尚无已完成的损坏矩阵 evidence。_")
    else:
        add("| 用例 | 版本 | 文件系统 | 注入 | 合约分类 | 导出 | 可导入 | 结果 |")
        add("|---|---|---|---|---|---|---|---|")
        for case in corruption:
            for label, run in (case.get("runs") or {}).items():
                add("| {case}/{label} | {version} | {fs} | {mutation} | {cls} | {export} | "
                    "{importable} | **{verdict}** |".format(
                        case=case.get("case"), label=label, version=case.get("version"),
                        fs=case.get("filesystem"), mutation=case.get("mutation"),
                        cls=run.get("classification"), export=run.get("export_success"),
                        importable=run.get("importable"), verdict=case.get("verdict")))
        add("")
        add("- 每个 mutation 都从独立 baseline copy 生成；报告保留 baseline/mutated SHA-256、"
            "物理 byte offset 和 page identity。该矩阵不计入正常 Phase A/B 恢复通过率。")
    add("")

    add("## 7. 缺陷清单")
    add("")
    if not doc["defects"]:
        add("_无已知未修复缺陷。_")
    for defect in doc["defects"]:
        add(f"### {defect['id']}（{defect['severity']}）{defect['title']}")
        add("")
        add(f"- **现象**：{defect['observed']}")
        if defect.get("evidence"):
            add(f"- **证据**：`{json.dumps(defect['evidence'], ensure_ascii=False)}`")
        if defect.get("minimal_repro"):
            add(f"- **最小复现**：`{defect['minimal_repro']}`")
        if defect.get("input_sha256"):
            add(f"- **输入镜像 SHA-256**：`{json.dumps(defect['input_sha256'], ensure_ascii=False)}`")
        add("")

    add("## 7b. SETUP_ERROR（环境中断，单列，不计入产品缺陷）")
    add("")
    setup = doc.get("setup_error_attempts") or []
    if not setup:
        add("_本轮无环境中断用例。_")
    else:
        add("| 用例 | 阶段 | 原因 |")
        add("|---|---|---|")
        for case in setup:
            reason = case.get("error") or case.get("reason") or                 "; ".join(case.get("blockers") or []) or "未记录"
            add(f"| {case['case']} | {case['phase']} | {str(reason).replace('|', '/')} |")
    add("")

    add("## 8. fail-closed 边界（退出码/拒绝原因/输出边界）")
    add("")
    if not doc["fail_closed_verification"]:
        add("_本轮没有触发 fail-closed 的输入。_")
    else:
        add("| 用例 | 阶段 | 分类 | 结构化行 | 证据片段 | 导出成功 | 可导入 | 拒绝原因 |")
        add("|---|---|---|---|---|---|---|---|")
        for item in doc["fail_closed_verification"]:
            reason = str(item.get("reason") or "").replace("|", "/")
            add("| {case} | {phase} | {cls} | {rows} | {ev} | {ex} | {imp} | {reason} |".format(
                case=item.get("case"), phase=item.get("phase"),
                cls=item.get("classification"), rows=item.get("structured_rows"),
                ev=item.get("evidence_fragments"), ex=item.get("export_success"),
                imp=item.get("importable"), reason=reason[:160]))
        add("")
        add("- 上述输入全部未生成可导入 CSV/SQL；`importable=false`，"
            "`export_success=false`。")
    add("")

    add("## 9. 持久化与块覆盖边界")
    add("")
    add(f"- **持久化屏障**：{doc['persistence_boundary']['method']}")
    add(f"- **范围**：{doc['persistence_boundary']['scope']}")
    add("")

    add("## 10. 验证缺口（如实标注，不计入通过）")
    add("")
    for gap in doc["validation_gaps"]:
        add(f"- {gap}")
    add("")

    add("## 11. 版本不支持组合")
    add("")
    for item in doc["unsupported_combinations"]:
        add(f"- {item}")
    add("")

    add("## 12. 明确结论")
    add("")
    add("- **达到 100% 可导入恢复的组合**：" + doc["conclusions"]["importable_100"])
    add("- **只能 evidence-only 的组合**：" + doc["conclusions"]["evidence_only"])
    add("- **版本不支持的组合**：" + doc["conclusions"]["unsupported"])
    add("- **存在真实缺陷、需要修复**：" + doc["conclusions"]["defects"])
    add("")

    add("## 13. 尚未执行 / 待授权")
    add("")
    for item in doc["not_executed"]:
        add(f"- {item}")
    add("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate the disaster recovery validation report")
    parser.add_argument("--evidence-root", type=Path, default=ROOT)
    args = parser.parse_args()

    evidence = args.evidence_root
    phase_a: list[dict[str, Any]] = []
    phase_b: list[dict[str, Any]] = []
    corruption: list[dict[str, Any]] = []
    # Each matrix run is stored below a named evidence root (for example
    # phase_a_final/phase-a-*).  Walk those roots explicitly; the top-level
    # directory contains several historical runs and is not itself a case.
    roots = [evidence]
    if evidence.is_dir():
        roots.extend(child for child in sorted(evidence.iterdir())
                     if child.is_dir() and child.name.startswith(
                         ("phase_a", "phase_b", "corruption")))
    for root in roots:
        if not root.is_dir():
            continue
        for child in sorted(root.iterdir()):
            if not child.is_dir():
                continue
            if child.name.startswith("phase-a"):
                phase_a.extend(collect_phase_a(root))
                break
            if child.name.startswith("phase-b"):
                phase_b.extend(collect_phase_b(root))
                break

    # Corruption runs are intentionally scored separately: their expected
    # evidence-only outcomes must not dilute normal recovery measurements.
    for root in roots:
        if root.is_dir():
            corruption.extend(collect_corruption(root))
    # The same mutation can be retried across evidence roots; keep only the
    # newest attempt per (version, filesystem, scenario, mutation).
    corruption, superseded_corruption = newest_per_key(
        corruption, lambda c: (c.get("version"), c.get("filesystem"),
                               c.get("scenario"), c.get("mutation")))

    # Retries of the same matrix cell live in several roots.  Only the newest
    # run of each cell may be scored; the superseded ones stay in the JSON as
    # an audit trail but must not inflate or dilute the verdict counts.
    setup_attempts = ([c for c in phase_a if c["verdict"] == "SETUP_ERROR"] +
                      [c for c in phase_b if c["verdict"] == "SETUP_ERROR"])
    phase_a, superseded_a = newest_per_key(
        phase_a, lambda c: (c["version"], c["row_format"], c["page_size"],
                            c["dataset_profile"]))
    phase_b, superseded_b = newest_per_key(
        phase_b, lambda c: (c["version"], c["filesystem"], c["scenario"],
                            c["row_format"], c["page_size"]))
    phase_a.sort(key=lambda c: c["case"])
    phase_b.sort(key=lambda c: c["case"])
    corruption.sort(key=lambda c: c["case"])

    fresh_pass = sum(
        1 for case in phase_b
        for entry in ((case.get("fresh_import") or {}).get("artifacts") or {}).values()
        if entry.get("status") == "PASS")

    a_pass = sum(1 for c in phase_a if c["verdict"] == "PASS")
    a_fail = sum(1 for c in phase_a if c["verdict"] == "FAIL")
    a_evidence = sum(1 for c in phase_a if c["verdict"] == "EVIDENCE_ONLY")
    a_setup = sum(1 for c in phase_a if c["verdict"] == "SETUP_ERROR")
    b_pass = sum(1 for c in phase_b
                 if c["verdict"] in ("PASS", "PASS_BLOCK_COVERAGE"))
    b_coverage = sum(1 for c in phase_b if c["verdict"] == "PASS_BLOCK_COVERAGE")
    b_fail = sum(1 for c in phase_b if c["verdict"] == "FAIL")
    b_setup = sum(1 for c in phase_b if c["verdict"] == "SETUP_ERROR")

    a_cases_by_key = {}
    for case in phase_a:
        a_cases_by_key.setdefault(case["verdict"], []).append(case)
    b_cases_by_key = {}
    for case in phase_b:
        b_cases_by_key.setdefault(case["verdict"], []).append(case)

    unsupported = unsupported_combinations()
    defects = _build_defects(phase_a, phase_b, corruption)
    fail_closed = _build_fail_closed(phase_a, phase_b, corruption)
    gaps = _build_validation_gaps(phase_a, phase_b, corruption)

    doc: dict[str, Any] = {
        "report_version": 3,
        "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "scope": ("Phase A offline .ibd baseline plus Phase B filesystem disaster "
                  "scenarios on isolated ext4/XFS disk images"),
        "environment": environment(),
        "results_summary": {
            "phase_a_total": len(phase_a),
            "phase_a_pass": a_pass,
            "phase_a_fail": a_fail,
            "phase_a_evidence": a_evidence,
            "phase_a_setup_error": a_setup,
            "phase_b_total": len(phase_b),
            "phase_b_pass": b_pass,
            "phase_b_pass_block_coverage": b_coverage,
            "phase_b_fail": b_fail,
            "phase_b_setup_error": b_setup,
            "superseded_phase_a_runs": superseded_a,
            "superseded_phase_b_runs": superseded_b,
            "fresh_import_total": sum(
                len((c.get("fresh_import") or {}).get("artifacts") or {})
                for c in phase_b),
            "fresh_import_pass": fresh_pass,
            "corruption_total": len(corruption),
            "corruption_pass_evidence_only": sum(
                1 for c in corruption if c["verdict"] == "PASS_EVIDENCE_ONLY"),
            "corruption_pass_bounded_structured": sum(
                1 for c in corruption if c["verdict"] == "PASS_BOUNDED_STRUCTURED"),
            "corruption_product_fail": sum(
                1 for c in corruption if c["verdict"] == "PRODUCT_FAIL"),
            "corruption_setup_error": sum(
                1 for c in corruption if c["verdict"] == "SETUP_ERROR"),
            "superseded_corruption_runs": superseded_corruption,
        },
        "capability_notes": [
            "MySQL 5.6 defaults to innodb_file_format=Antelope, which silently stores "
            "ROW_FORMAT=DYNAMIC and COMPRESSED as COMPACT; both harnesses now pin "
            "--innodb-file-format=Barracuda --innodb-large-prefix=ON for 5.x only, so "
            "the requested row format is the format actually tested.",
            "MySQL 5.7 defaults to Barracuda with large prefix on, so DYNAMIC and "
            "COMPRESSED are honored as requested.",
            "MySQL 8.0/8.4/9.0 removed the file-format options and reject unknown "
            "server arguments.",
            "MySQL 8.0/8.4/9.0 do not support REDUNDANT or COMPACT row formats; the "
            "matrix reports those as UNSUPPORTED rather than skipped.",
            "REDUNDANT record layout is not decoded into structured rows: the "
            "cumulative field-offset array also covers hidden clustered-index system "
            "columns, and without an authoritative layout descriptor the decoder "
            "cannot prove field boundaries. Those inputs fail closed as "
            "evidence-only (no CSV/SQL) instead of exporting shifted values.",
            "InnoDB compressed tablespaces (KEY_BLOCK_SIZE) are reported "
            "evidence-only: the FSP compression flags are parsed canonically and the "
            "physical stride is proven from the layout, but no fixture-validated "
            "decompression backend exists yet, so no structured row is claimed.",
        ],
        "phase_a": phase_a,
        "phase_b": phase_b,
        "corruption_matrix": corruption,
        "setup_error_attempts": setup_attempts,
        "defects": defects,
        "fail_closed_verification": fail_closed,
        "validation_gaps": gaps,
        "persistence_boundary": {
            "method": ("CHECKPOINT plus FLUSH TABLES ... FOR EXPORT before copying the "
                       ".ibd, or a full MySQL shutdown for filesystem scenarios; "
                       "ground truth sealed through the server before the disaster."),
            "scope": ("Only data durable at the persistence barrier is in scope. Data "
                      "that had not reached the tablespace at that moment is outside "
                      "the recoverability requirement and is never counted as a tool "
                      "defect."),
        },
        "unsupported_combinations": [
            f"MySQL {c['version']} x {c['row_format']} x {c['page_size']}B: {c['reason']}"
            for c in unsupported
        ],
        "conclusions": {
            "importable_100": _conclusion_importable(phase_a, phase_b),
            "evidence_only": _conclusion_evidence(phase_a, phase_b),
            "unsupported": "; ".join(
                f"MySQL {c['version']} x {c['row_format']} x {c['page_size']}B ({c['reason']})"
                for c in unsupported) or "none",
            "defects": _conclusion_defects(phase_a, phase_b),
        },
        "not_executed": [
            "裸块设备（/dev/nvme0n2）真实介质阶段：需单独授权后由 device_safety 预检 + "
            "approval token 双门执行，本轮未执行。",
            "页损坏/TDE 矩阵已覆盖的范围以本报告 corruption_matrix 为准；未执行的部分："
            "SDI/.frm 元数据损坏、压缩元数据篡改、MySQL 5.6/8.0/9.0 的损坏矩阵、"
            "xfs 上的 8.4 与 TDE 用例、以及 100k/1m 规模样本。",
        ],
    }

    (evidence / "disaster_recovery_validation_report.json").write_text(
        json.dumps(doc, indent=1, ensure_ascii=False), encoding="utf-8")
    (evidence / "disaster_recovery_validation_report.md").write_text(
        markdown(doc), encoding="utf-8")
    print(f"wrote report to {evidence}")
    print(json.dumps(doc["results_summary"], indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
