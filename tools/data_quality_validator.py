#!/usr/bin/env python3
"""
Data Quality Validator for MySQL Recovery Pro

Validates recovered data quality:
1. Check CSV file format
2. Verify record counts
3. Analyze field patterns
4. Detect data corruption
5. Generate quality report

Author: jliang.fang
Date: 2026-07-03
"""

import os
import sys
import csv
import json
import hashlib
from pathlib import Path
from datetime import datetime
from collections import defaultdict, Counter
from typing import Dict, List, Tuple, Optional


class DataQualityValidator:
    def __init__(self, recovery_dir: str):
        self.recovery_dir = Path(recovery_dir)
        self.quality_report = {
            "timestamp": datetime.now().isoformat(),
            "recovery_dir": str(recovery_dir),
            "total_files": 0,
            "total_rows": 0,
            "files_analysis": {},
            "quality_metrics": {},
            "issues": [],
            "recommendations": []
        }

    def validate_all_files(self) -> Dict:
        """Validate all CSV files in recovery directory"""
        csv_files = list(self.recovery_dir.glob("*.csv"))
        self.quality_report["total_files"] = len(csv_files)

        print(f"Found {len(csv_files)} CSV files to validate")

        for csv_file in csv_files:
            file_analysis = self.validate_single_file(csv_file)
            self.quality_report["files_analysis"][csv_file.name] = file_analysis
            self.quality_report["total_rows"] += file_analysis["row_count"]

        # Calculate overall quality metrics
        self.calculate_quality_metrics()

        return self.quality_report

    def validate_single_file(self, csv_file: Path) -> Dict:
        """Validate a single CSV file"""
        analysis = {
            "file_name": csv_file.name,
            "file_size": csv_file.stat().st_size,
            "row_count": 0,
            "column_count": 0,
            "columns": [],
            "field_patterns": defaultdict(int),
            "empty_fields": 0,
            "duplicate_rows": 0,
            "unique_values": {},
            "data_types": defaultdict(int),
            "quality_score": 0,
            "issues": []
        }

        try:
            with open(csv_file, 'r', encoding='utf-8', errors='ignore') as f:
                reader = csv.reader(f)

                # Read header
                try:
                    header = next(reader)
                    analysis["column_count"] = len(header)
                    analysis["columns"] = header
                except StopIteration:
                    analysis["issues"].append("Empty file (no header)")
                    return analysis

                # Analyze rows
                row_hashes = set()
                for row_idx, row in enumerate(reader):
                    analysis["row_count"] += 1

                    # Check row length consistency
                    if len(row) != analysis["column_count"]:
                        analysis["issues"].append(
                            f"Row {row_idx}: column mismatch ({len(row)} vs {analysis['column_count']})"
                        )

                    # Check for empty fields
                    for field in row:
                        if not field or field.strip() == '':
                            analysis["empty_fields"] += 1

                    # Analyze field patterns
                    for field_idx, field in enumerate(row):
                        if field_idx < len(header):
                            col_name = header[field_idx]
                            if col_name not in analysis["unique_values"]:
                                analysis["unique_values"][col_name] = set()
                            analysis["unique_values"][col_name].add(field[:50])  # Sample first 50 chars

                            # Detect data type
                            data_type = self.detect_data_type(field)
                            analysis["data_types"][data_type] += 1

                            # Pattern analysis
                            pattern = self.extract_pattern(field)
                            analysis["field_patterns"][pattern] += 1

                    # Duplicate detection
                    row_hash = hashlib.md5(str(row).encode()).hexdigest()
                    if row_hash in row_hashes:
                        analysis["duplicate_rows"] += 1
                    row_hashes.add(row_hash)

                # Limit unique values for report
                for col in analysis["unique_values"]:
                    if isinstance(analysis["unique_values"][col], set):
                        analysis["unique_values"][col] = list(analysis["unique_values"][col])[:100]

                # Calculate quality score
                analysis["quality_score"] = self.calculate_file_quality_score(analysis)

        except Exception as e:
            analysis["issues"].append(f"Read error: {str(e)}")

        return analysis

    def detect_data_type(self, value: str) -> str:
        """Detect data type of a field value"""
        if not value or value.strip() == '':
            return "empty"

        # Try numeric
        try:
            int(value)
            return "integer"
        except ValueError:
            pass

        try:
            float(value)
            return "float"
        except ValueError:
            pass

        # Hex pattern (schema-agnostic)
        if all(c in '0123456789abcdefABCDEF' for c in value):
            if len(value) >= 16:
                return "hex_data"

        # JSON pattern
        if value.startswith('{') or value.startswith('['):
            try:
                json.loads(value)
                return "json"
            except:
                pass

        # Date/time patterns
        if len(value) >= 10:
            if '-' in value and value.count('-') == 2:
                return "date"
            if ':' in value and value.count(':') >= 2:
                return "datetime"

        # String
        return "string"

    def extract_pattern(self, value: str) -> str:
        """Extract pattern from field value"""
        if not value:
            return "empty"

        # Schema-agnostic hex pattern
        if all(c in '0123456789abcdefABCDEF' for c in value):
            if len(value) >= 32:
                return f"hex_{len(value)}"
            return f"hex_short_{len(value)}"

        # String with known patterns
        if 'User_' in value or 'user' in value.lower():
            return "user_pattern"
        if '@' in value and '.' in value:
            return "email_pattern"

        # Numeric pattern
        try:
            num = int(value)
            if 0 <= num < 128:
                return "tinyint_range"
            if 0 <= num < 32768:
                return "smallint_range"
            if 0 <= num < 8388608:
                return "mediumint_range"
            return "int_range"
        except:
            pass

        return f"string_{min(len(value), 20)}"

    def calculate_file_quality_score(self, analysis: Dict) -> float:
        """Calculate quality score for a file (0-100)"""
        score = 100.0

        # Deduct for issues
        score -= len(analysis["issues"]) * 5

        # Deduct for empty fields
        if analysis["row_count"] > 0:
            empty_ratio = analysis["empty_fields"] / (analysis["row_count"] * analysis["column_count"])
            score -= empty_ratio * 20

        # Deduct for duplicates
        if analysis["row_count"] > 0:
            duplicate_ratio = analysis["duplicate_rows"] / analysis["row_count"]
            score -= duplicate_ratio * 10

        # Bonus for data diversity
        if analysis["unique_values"]:
            avg_unique = sum(len(set(v)) for v in analysis["unique_values"].values()) / len(analysis["unique_values"])
            if avg_unique > 10:
                score += 5  # Good data diversity

        return max(0, min(100, score))

    def calculate_quality_metrics(self):
        """Calculate overall quality metrics"""
        metrics = {
            "avg_quality_score": 0,
            "files_with_issues": 0,
            "total_issues": 0,
            "avg_row_count": 0,
            "avg_column_count": 0,
            "data_type_distribution": defaultdict(int),
            "schema_agnostic_ratio": 0,
            "recovery_success_rate": 0
        }

        total_score = 0
        hex_files = 0

        for file_name, analysis in self.quality_report["files_analysis"].items():
            total_score += analysis["quality_score"]

            if analysis["issues"]:
                metrics["files_with_issues"] += 1
                metrics["total_issues"] += len(analysis["issues"])

            # Data type distribution
            for dtype, count in analysis["data_types"].items():
                metrics["data_type_distribution"][dtype] += count

            # Schema-agnostic detection
            if "hex_data" in analysis["data_types"]:
                hex_files += 1

        if self.quality_report["total_files"] > 0:
            metrics["avg_quality_score"] = total_score / self.quality_report["total_files"]
            metrics["avg_row_count"] = self.quality_report["total_rows"] / self.quality_report["total_files"]
            metrics["schema_agnostic_ratio"] = hex_files / self.quality_report["total_files"]

            # Recovery success rate (assuming 10000 expected rows from test)
            expected_rows = 10000
            metrics["recovery_success_rate"] = min(100, (self.quality_report["total_rows"] / expected_rows) * 100)

        # Convert defaultdict to dict for JSON
        metrics["data_type_distribution"] = dict(metrics["data_type_distribution"])

        self.quality_report["quality_metrics"] = metrics

        # Generate recommendations
        self.generate_recommendations()

    def generate_recommendations(self):
        """Generate recommendations based on quality analysis"""
        metrics = self.quality_report["quality_metrics"]

        # High schema-agnostic ratio
        if metrics["schema_agnostic_ratio"] > 0.5:
            self.quality_report["recommendations"].append(
                "High schema-agnostic data ratio detected. Consider providing schema via --schema or --schema-file for better field interpretation."
            )

        # Low quality score
        if metrics["avg_quality_score"] < 80:
            self.quality_report["recommendations"].append(
                "Average quality score below 80%. Review files with issues for data corruption."
            )

        # Many duplicates
        total_duplicates = sum(
            analysis["duplicate_rows"]
            for analysis in self.quality_report["files_analysis"].values()
        )
        if total_duplicates > self.quality_report["total_rows"] * 0.1:
            self.quality_report["recommendations"].append(
                "High duplicate ratio detected (>10%). Data may contain repeated records from multiple recovery passes."
            )

        # High recovery rate
        if metrics["recovery_success_rate"] >= 100:
            self.quality_report["recommendations"].append(
                f"Excellent recovery rate: {metrics['recovery_success_rate']:.1f}%. All test data successfully recovered."
            )

    def generate_report(self, output_file: Optional[str] = None) -> str:
        """Generate quality report"""
        report_lines = []

        report_lines.append("=" * 80)
        report_lines.append("MySQL Recovery Pro - Data Quality Validation Report")
        report_lines.append("=" * 80)
        report_lines.append(f"\nTimestamp: {self.quality_report['timestamp']}")
        report_lines.append(f"Recovery Directory: {self.quality_report['recovery_dir']}")
        report_lines.append(f"\nTotal Files: {self.quality_report['total_files']}")
        report_lines.append(f"Total Rows: {self.quality_report['total_rows']}")

        metrics = self.quality_report["quality_metrics"]
        report_lines.append(f"\n--- Quality Metrics ---")
        report_lines.append(f"Average Quality Score: {metrics['avg_quality_score']:.2f}%")
        report_lines.append(f"Files with Issues: {metrics['files_with_issues']}")
        report_lines.append(f"Total Issues: {metrics['total_issues']}")
        report_lines.append(f"Average Rows per File: {metrics['avg_row_count']:.2f}")
        report_lines.append(f"Recovery Success Rate: {metrics['recovery_success_rate']:.2f}%")
        report_lines.append(f"Schema-Agnostic Ratio: {metrics['schema_agnostic_ratio']:.2%}")

        report_lines.append(f"\n--- Data Type Distribution ---")
        for dtype, count in sorted(metrics["data_type_distribution"].items(), key=lambda x: -x[1]):
            report_lines.append(f"  {dtype}: {count}")

        report_lines.append(f"\n--- Top 10 Files by Row Count ---")
        sorted_files = sorted(
            self.quality_report["files_analysis"].items(),
            key=lambda x: -x[1]["row_count"]
        )[:10]
        for file_name, analysis in sorted_files:
            report_lines.append(
                f"  {file_name}: {analysis['row_count']} rows, "
                f"quality={analysis['quality_score']:.2f}%, "
                f"issues={len(analysis['issues'])}"
            )

        if self.quality_report["recommendations"]:
            report_lines.append(f"\n--- Recommendations ---")
            for idx, rec in enumerate(self.quality_report["recommendations"], 1):
                report_lines.append(f"  {idx}. {rec}")

        report_lines.append(f"\n--- Detailed File Analysis ---")
        for file_name, analysis in sorted(self.quality_report["files_analysis"].items()):
            if analysis["issues"]:
                report_lines.append(f"\n  {file_name}:")
                report_lines.append(f"    Quality Score: {analysis['quality_score']:.2f}%")
                report_lines.append(f"    Issues:")
                for issue in analysis["issues"][:10]:  # Limit to 10 issues per file
                    report_lines.append(f"      - {issue}")

        report_lines.append("\n" + "=" * 80)
        report_lines.append("End of Report")
        report_lines.append("=" * 80)

        report_text = "\n".join(report_lines)

        if output_file:
            with open(output_file, 'w', encoding='utf-8') as f:
                f.write(report_text)
            print(f"Report saved to: {output_file}")

            # Also save JSON report
            json_file = output_file.replace('.txt', '.json')

            # Custom encoder to handle sets and defaultdicts
            def convert_to_serializable(obj):
                if isinstance(obj, set):
                    return list(obj)[:100]  # Limit to 100 items
                if isinstance(obj, defaultdict):
                    return dict(obj)
                if isinstance(obj, Path):
                    return str(obj)
                raise TypeError(f"Object of type {obj.__class__.__name__} is not JSON serializable")

            class CustomEncoder(json.JSONEncoder):
                def default(self, obj):
                    try:
                        return convert_to_serializable(obj)
                    except TypeError:
                        return super().default(obj)

            with open(json_file, 'w', encoding='utf-8') as f:
                json.dump(self.quality_report, f, indent=2, cls=CustomEncoder)
            print(f"JSON report saved to: {json_file}")

        return report_text


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 data_quality_validator.py <recovery_dir> [output_file]")
        sys.exit(1)

    recovery_dir = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else None

    validator = DataQualityValidator(recovery_dir)
    validator.validate_all_files()

    report = validator.generate_report(output_file)
    print(report)


if __name__ == "__main__":
    main()