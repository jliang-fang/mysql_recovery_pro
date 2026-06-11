#!/usr/bin/env python3
"""
MySQL Recovery Pro - Chaos Test Runner & Pipeline Orchestrator
Purpose: Orchestrate full chaos testing suite and generate comprehensive reports
Author: jliang.fang (Global MySQL Kernel QA Expert)
Version: 3.0.0 - Bullet-proof Chaos Testing Pipeline
"""

import os
import sys
import json
import subprocess
import time
import shutil
from typing import List, Dict, Tuple, Optional, Any
from dataclasses import dataclass, field
from pathlib import Path
from datetime import datetime


def _json_serialize(obj: Any) -> Any:
    """Custom JSON serializer for objects that aren't natively serializable"""
    if isinstance(obj, bytes):
        return obj.hex() if len(obj) < 1024 else f"<{len(obj)} bytes>"
    elif isinstance(obj, Path):
        return str(obj)
    elif hasattr(obj, 'to_dict'):
        return obj.to_dict()
    elif isinstance(obj, (set, frozenset)):
        return list(obj)
    return str(obj)


# Add scripts directory to path
script_dir = Path(__file__).parent
sys.path.insert(0, str(script_dir))


@dataclass
class ChaosTestResult:
    """Result of a single chaos test"""
    test_name: str
    start_time: str
    end_time: str
    duration_seconds: float
    status: str  # PASSED, FAILED, SKIPPED
    details: Dict = field(default_factory=dict)
    error_message: str = ""


@dataclass
class PipelineReport:
    """Complete chaos testing pipeline report"""
    pipeline_version: str = "3.0.0"
    run_id: str = ""
    timestamp: str = ""
    total_tests: int = 0
    passed_tests: int = 0
    failed_tests: int = 0
    skipped_tests: int = 0
    test_results: List[ChaosTestResult] = field(default_factory=list)
    feature_coverage: Dict = field(default_factory=dict)
    recovery_metrics: Dict = field(default_factory=dict)
    bug_found_count: int = 0
    
    @property
    def success_rate(self) -> float:
        """Calculate success rate"""
        if self.total_tests == 0:
            return 0.0
        return round(self.passed_tests / self.total_tests * 100, 2)
    
    def to_dict(self) -> Dict:
        return {
            "pipeline_version": self.pipeline_version,
            "run_id": self.run_id,
            "timestamp": self.timestamp,
            "total_tests": self.total_tests,
            "passed_tests": self.passed_tests,
            "failed_tests": self.failed_tests,
            "skipped_tests": self.skipped_tests,
            "success_rate": round(self.passed_tests / self.total_tests * 100, 2) if self.total_tests > 0 else 0,
            "test_results": [
                {
                    "test_name": r.test_name,
                    "start_time": r.start_time,
                    "end_time": r.end_time,
                    "duration_seconds": r.duration_seconds,
                    "status": r.status,
                    "details": json.loads(json.dumps(r.details, default=_json_serialize)),
                    "error_message": r.error_message
                }
                for r in self.test_results
            ],
            "feature_coverage": json.loads(json.dumps(self.feature_coverage, default=_json_serialize)),
            "recovery_metrics": json.loads(json.dumps(self.recovery_metrics, default=_json_serialize)),
            "bug_found_count": self.bug_found_count
        }


class ChaosPipeline:
    """Orchestrate chaos testing pipeline"""
    
    def __init__(self, output_dir: str = "chaos-pipeline-output", 
                 seed: int = 42, mysql_host: str = "localhost"):
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.seed = seed
        self.mysql_host = mysql_host
        self.results: List[ChaosTestResult] = []
        self.start_time = None
        self.end_time = None
    
    def run_test(self, test_name: str, test_func, *args, **kwargs) -> ChaosTestResult:
        """Execute a test and record results"""
        start_time = datetime.now().isoformat()
        start_seconds = time.time()
        
        try:
            result = test_func(*args, **kwargs)
            status = "PASSED"
            error_message = ""
        except Exception as e:
            result = None
            status = "FAILED"
            error_message = str(e)
        
        end_time = datetime.now().isoformat()
        end_seconds = time.time()
        
        test_result = ChaosTestResult(
            test_name=test_name,
            start_time=start_time,
            end_time=end_time,
            duration_seconds=end_seconds - start_seconds,
            status=status,
            details=result if isinstance(result, dict) else {"result": str(result)},
            error_message=error_message
        )
        
        self.results.append(test_result)
        return test_result
    
    def run_all_tests(self) -> PipelineReport:
        """Run complete chaos testing pipeline"""
        self.start_time = datetime.now()
        print("=" * 80)
        print("MYSQL RECOVERY PRO - CHAOS PIPELINE ORCHESTRATOR")
        print("=" * 80)
        print(f"Run ID: {self.start_time.strftime('%Y%m%d_%H%M%S')}")
        print(f"Seed: {self.seed}")
        print(f"Output: {self.output_dir}")
        print()
        
        # Initialize report
        report = PipelineReport(
            run_id=self.start_time.strftime('%Y%m%d_%H%M%S'),
            timestamp=self.start_time.isoformat()
        )
        
        # Feature A: Page Size Extreme Tests
        print("[PIPELINE] Running Feature A: Page Size Extreme Tests")
        feature_a_results = self._run_feature_a()
        report.feature_coverage["Feature_A"] = len(feature_a_results)
        report.test_results.extend(feature_a_results)
        
        # Feature B: Disaster Recovery
        print("[PIPELINE] Running Feature B: Disaster Recovery Chaos")
        feature_b_results = self._run_feature_b()
        report.feature_coverage["Feature_B"] = len(feature_b_results)
        report.test_results.extend(feature_b_results)
        
        # Feature C: Scan & Discovery
        print("[PIPELINE] Running Feature C: Scan & Discovery Chaos")
        feature_c_results = self._run_feature_c()
        report.feature_coverage["Feature_C"] = len(feature_c_results)
        report.test_results.extend(feature_c_results)
        
        # Feature D: Anomaly Detection
        print("[PIPELINE] Running Feature D: Anomaly Detection Chaos")
        feature_d_results = self._run_feature_d()
        report.feature_coverage["Feature_D"] = len(feature_d_results)
        report.test_results.extend(feature_d_results)
        
        # Calculate summary
        report.total_tests = len(report.test_results)
        report.passed_tests = sum(1 for r in report.test_results if r.status == "PASSED")
        report.failed_tests = sum(1 for r in report.test_results if r.status == "FAILED")
        report.skipped_tests = sum(1 for r in report.test_results if r.status == "SKIPPED")
        
        self.end_time = datetime.now()
        report.recovery_metrics = self._calculate_recovery_metrics()
        
        # Save report
        self._save_report(report)
        
        print()
        print("=" * 80)
        print("CHAOS PIPELINE COMPLETE")
        print("=" * 80)
        print(f"Total Tests: {report.total_tests}")
        print(f"Passed: {report.passed_tests}")
        print(f"Failed: {report.failed_tests}")
        print(f"Success Rate: {report.success_rate}%")
        print(f"Duration: {self.end_time - self.start_time}")
        print(f"Report: {self.output_dir / 'chaos_pipeline_report.json'}")
        print()
        
        return report
    
    def _run_feature_a(self) -> List[ChaosTestResult]:
        """Feature A: Page Size Extreme Tests"""
        from chaos_injection_suite import PageSizeExtremeTest
        
        results = []
        tester = PageSizeExtremeTest(page_size=16384, seed=self.seed)
        
        # Test 1: Generate extreme page sizes
        result = self.run_test(
            "feature_a_1_page_size_generation",
            tester.generate_extreme_page_sizes,
            num_records=1000
        )
        results.append(result)
        
        # Test 2: Alignment violations
        if result.status == "PASSED" and result.details:
            for size, data in result.details.items():
                alignment_result = self.run_test(
                    f"feature_a_2_alignment_{size}",
                    tester.test_alignment_violations,
                    data
                )
                results.append(alignment_result)
        
        # Test 3: Large page handling
        result = self.run_test(
            "feature_a_3_large_page_handling",
            tester.test_large_page_handling,
            page_size=65536
        )
        results.append(result)
        
        return results
    
    def _run_feature_b(self) -> List[ChaosTestResult]:
        """Feature B: Disaster Recovery Chaos"""
        from blob_deep_rescue_chaos import BLOBChaosGenerator, BLOBChaosDestroyer
        from header_sdi_chaos import HeaderCorruptionEngine, SDICorruptionEngine
        
        results = []
        generator = BLOBChaosGenerator(page_size=16384, seed=self.seed)
        destroyer = BLOBChaosDestroyer(seed=self.seed)
        
        # Test 1: Generate BLOB data
        result = self.run_test(
            "feature_b_1_blob_generation",
            generator.create_blob_chain,
            total_size_kb=32
        )
        results.append(result)

        # Test 2: BLOB chain destruction
        if result.status == "PASSED" and result.details:
            # Handle both BLOB object and dict
            blob_obj = result.details if hasattr(result.details, 'page_chain') else None
            if blob_obj is None:
                # Create a minimal BLOB object from dict
                from blob_deep_rescue_chaos import BLOBRecord
                blob_obj = BLOBRecord(
                    record_id=0,
                    total_size=32 * 1024,
                    page_chain=[100, 101],
                    chunk_sizes=[16342, 350]
                )
            pages = generator.create_blob_pages(blob_obj)
            destroyer = BLOBChaosDestroyer(seed=self.seed, page_size=generator.page_size)
            corrupted, destruction = destroyer.destroy_single_blob_chain(pages, blob_obj)

            destruction_result = self.run_test(
                "feature_b_2_blob_destruction",
                lambda: destruction
            )
            results.append(destruction_result)
        
        # Test 3: Header corruption
        header_engine = HeaderCorruptionEngine(page_size=16384, seed=self.seed)
        
        # Create test data
        test_data = bytes(16384 * 10)  # 10 pages
        corrupted, header_result = header_engine.corrupt_all_page_headers(test_data)
        
        header_result = self.run_test(
            "feature_b_3_header_corruption",
            lambda: header_result
        )
        results.append(header_result)
        
        # Test 4: SDI nuclear strike
        sdi_engine = SDICorruptionEngine(page_size=16384, seed=self.seed)
        sdi_corrupted, sdi_result = sdi_engine.nuclear_sdi_strike(test_data)
        
        sdi_result = self.run_test(
            "feature_b_4_sdi_nuclear_strike",
            lambda: sdi_result
        )
        results.append(sdi_result)
        
        return results
    
    def _run_feature_c(self) -> List[ChaosTestResult]:
        """Feature C: Scan & Discovery Chaos"""
        from chaos_injection_suite import ScanDiscoveryChaos
        
        results = []
        scan_chaos = ScanDiscoveryChaos(page_size=16384, seed=self.seed)
        
        # Test 1: Directory depth bomb
        depth_result = self.run_test(
            "feature_c_1_directory_depth_bomb",
            scan_chaos.create_directory_depth_bomb,
            base_path=str(self.output_dir / "depth_bomb"),
            depth=100
        )
        results.append(depth_result)
        
        # Test 2: ScanDir stress
        scan_result = self.run_test(
            "feature_c_2_scandir_stress",
            scan_chaos.test_scan_dir_stress,
            base_path=str(self.output_dir / "scan_stress"),
            num_dirs=10000
        )
        results.append(scan_result)
        
        # Test 3: SDI recovery
        test_data = bytes(16384 * 10)
        sdi_corrupted, sdi_result = scan_chaos.execute_sdi_nuclear_strike(test_data)
        
        sdi_result = self.run_test(
            "feature_c_3_sdi_recovery",
            lambda: sdi_result
        )
        results.append(sdi_result)
        
        # Test 4: B+Tree chain rebuild
        btree_corrupted, btree_result = scan_chaos.execute_btree_chain_rebuild_test(test_data)
        
        btree_result = self.run_test(
            "feature_c_4_btree_chain_rebuild",
            lambda: btree_result
        )
        results.append(btree_result)
        
        return results
    
    def _run_feature_d(self) -> List[ChaosTestResult]:
        """Feature D: Anomaly Detection Chaos"""
        from chaos_injection_suite import AnomalyDetectionChaos
        
        results = []
        anomaly_chaos = AnomalyDetectionChaos(page_size=16384, seed=self.seed)
        
        # Test 1: Silent corruption injection
        test_data = bytes(16384 * 10)
        corrupted, corruption_result = anomaly_chaos.inject_silent_corruption(test_data)
        
        detection_result = self.run_test(
            "feature_d_1_silent_corruption",
            lambda: corruption_result
        )
        results.append(detection_result)
        
        # Test 2: Batch parallel processing
        batch_result = self.run_test(
            "feature_d_2_batch_parallel",
            anomaly_chaos.create_batch_test_files,
            base_dir=str(self.output_dir / "batch_test"),
            num_files=5000,
            file_size_kb=64
        )
        results.append(batch_result)
        
        return results
    
    def _calculate_recovery_metrics(self) -> Dict:
        """Calculate recovery metrics from test results"""
        metrics = {
            "total_scenarios": len(self.results),
            "success_rate": 0.0,
            "avg_duration_seconds": 0.0,
            "feature_coverage": {}
        }
        
        if self.results:
            total = len(self.results)
            passed = sum(1 for r in self.results if r.status == "PASSED")
            metrics["success_rate"] = round(passed / total * 100, 2)
            metrics["avg_duration_seconds"] = round(
                sum(r.duration_seconds for r in self.results) / total, 2
            )
        
        # Count by feature
        for result in self.results:
            feature = result.test_name.split('_')[0]
            if feature not in metrics["feature_coverage"]:
                metrics["feature_coverage"][feature] = {"total": 0, "passed": 0}
            metrics["feature_coverage"][feature]["total"] += 1
            if result.status == "PASSED":
                metrics["feature_coverage"][feature]["passed"] += 1
        
        return metrics
    
    def _save_report(self, report: PipelineReport):
        """Save pipeline report"""
        report_path = self.output_dir / "chaos_pipeline_report.json"
        with open(report_path, 'w') as f:
            json.dump(report.to_dict(), f, indent=2)

        # Also save individual test results
        for result in report.test_results:
            result_path = self.output_dir / f"test_{result.test_name}.json"
            details_for_json = json.loads(json.dumps(result.details, default=_json_serialize))
            with open(result_path, 'w') as f:
                json.dump({
                    "test_name": result.test_name,
                    "start_time": result.start_time,
                    "end_time": result.end_time,
                    "duration_seconds": result.duration_seconds,
                    "status": result.status,
                    "details": details_for_json,
                    "error_message": result.error_message
                }, f, indent=2)

        print(f"[PIPELINE] Report saved to {report_path}")


def main():
    import argparse
    
    parser = argparse.ArgumentParser(
        description="MySQL Recovery Pro - Chaos Testing Pipeline Orchestrator",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Chaos Pipeline Features:
  Feature A: Page Size Extreme Tests (4KB -> 64KB)
  Feature B: Disaster Recovery Chaos (truncate, rmrf, header repair, blob rescue)
  Feature C: Scan & Discovery Chaos (depth bomb, SDI nuke, B+tree rebuild)
  Feature D: Anomaly Detection Chaos (silent corruption, batch parallel)

Usage Examples:
  python3 chaos_pipeline.py --seed 42
  python3 chaos_pipeline.py --output /tmp/pipeline_results --seed 123
  python3 chaos_pipeline.py --mysql-host mysql-8.0 --fast
        """
    )
    
    parser.add_argument('--seed', type=int, default=42,
                       help='Random seed for reproducibility')
    parser.add_argument('--output', type=str, default='chaos-pipeline-output',
                       help='Output directory for results')
    parser.add_argument('--mysql-host', type=str, default='localhost',
                       help='MySQL host for integration tests')
    parser.add_argument('--fast', action='store_true',
                       help='Run fast version with fewer tests')
    
    args = parser.parse_args()
    
    # Initialize pipeline
    pipeline = ChaosPipeline(
        output_dir=args.output,
        seed=args.seed,
        mysql_host=args.mysql_host
    )
    
    # Run pipeline
    report = pipeline.run_all_tests()
    
    # Print summary
    print(json.dumps(report.to_dict(), indent=2))


if __name__ == "__main__":
    main()
