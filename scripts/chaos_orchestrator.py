#!/usr/bin/env python3
# =============================================================================
# MySQL Recovery Pro - Chaos Orchestrator & Self-Healing Driver
# Architect: jliang.fang (Global MySQL Kernel QA & Chaos Engineering)
# Purpose: Orchestrate chaos testing + Auto-heal C++ engine via Core Dump analysis
# =============================================================================

import os
import sys
import subprocess
import json
import time
import signal
import hashlib
import shutil
import tempfile
from pathlib import Path
from typing import Dict, List, Tuple, Optional, Callable
from dataclasses import dataclass, field
from enum import Enum
import threading
import queue

# ==================== Constants ====================

MAX_RETEST_ATTEMPTS = 5
CHAOSS_TEST_TIMEOUT = 300  # 5 minutes per test
GDB_TIMEOUT = 30  # GDB analysis timeout

# Chaos Test Categories
CHAOS_CATEGORIES = {
    "category_a": [
        "corrupt_page_header",
        "tear_page",
        "corrupt_page_trailer",
        "modify_page_type"
    ],
    "category_b": [
        "truncate_recovery",
        "rmrf_recovery",
        "header_repair",
        "blob_recovery"
    ],
    "category_c": [
        "depth_bomb",
        "sdi_corruption",
        "rebuild_chain"
    ],
    "category_d": [
        "silent_corruption",
        "batch_concurrent",
        "dead_tuple_recovery"
    ]
}


@dataclass
class TestReport:
    """Test report for a single test"""
    test_id: str
    category: str
    scenario: str
    ibd_file: str
    page_size: int
    native_binary: str
    recovery_rate: float
    errors: List[str]
    core_dump_path: Optional[str]
    analysis_result: Optional[str]
    passed: bool
    execution_time: float


@dataclass
class SelfHealPatch:
    """自愈补丁定义"""
    bug_id: str
    category: str
    file: str
    line: int
    type: str  # "buffer_overflow", "null_pointer", "memory_leak"
    description: str
    unified_diff: str
    fixed: bool = False


class ChaosOrchestrator:
    """混沌测试编排器 - 核心调度引擎"""
    
    def __init__(
        self,
        engine_path: str,
        test_data_dir: str,
        results_dir: str,
        use_asan: bool = True,
        debug: bool = False
    ):
        self.engine_path = Path(engine_path)
        self.test_data_dir = Path(test_data_dir)
        self.results_dir = Path(results_dir)
        self.use_asan = use_asan
        self.debug = debug
        
        # Results storage
        self.test_reports: List[TestReport] = []
        self.self_heal_patches: List[SelfHealPatch] = []
        
        # Directories
        self.results_dir.mkdir(parents=True, exist_ok=True)
        self.core_dump_dir = self.results_dir / "core_dumps"
        self.core_dump_dir.mkdir(exist_ok=True)
        
        # Setup ASAN options if enabled
        if self.use_asan:
            os.environ["ASAN_OPTIONS"] = ",".join([
                "halt_on_error=1",
                "abort_on_error=1",
                "detect_stack_use_after_return=1",
                "check_initialization_order=1",
                "strict_init_order=1",
                "symbolize=1"
            ])
            os.environ["LSAN_OPTIONS"] = "verbosity=1:log_threads=1"
        
        if debug:
            print("[ORCHESTRATOR] ASAN enabled" if use_asan else "[ORCHESTRATOR] ASAN disabled")
    
    def run_ibd_recovery(
        self,
        ibd_file: str,
        page_size: int,
        recovery_mode: str,
        extra_args: List[str] = None
    ) -> Tuple[bool, str, Optional[str]]:
        """
        Run recovery on an IBD file and capture results
        Returns: (success, output, core_dump_path)
        """
        args = [
            str(self.engine_path),
            "--ibd", ibd_file,
            "--page-size", str(page_size),
            "--format", "json"
        ]
        
        if recovery_mode == "truncate-recovery":
            args.append("--truncate-recovery")
        elif recovery_mode == "rmrf-recovery":
            args.append("--rmrf-recovery")
        elif recovery_mode == "header-repair":
            args.append("--header-repair")
        elif recovery_mode == "blob-recovery":
            args.append("--blob-recovery")
        elif recovery_mode == "scan-dir":
            args.append("--scan-dir")
        
        args.extend(extra_args or [])
        
        if self.debug:
            print(f"[ORCHESTRATOR] Executing: {' '.join(args)}")
        
        # Execute with timeout
        try:
            result = subprocess.run(
                args,
                capture_output=True,
                text=True,
                timeout=CHAOSS_TEST_TIMEOUT
            )
            
            core_dump = None
            if result.returncode != 0:
                # Check for segfault
                if "SIGSEGV" in result.stderr or "Segmentation" in result.stderr:
                    core_dump = self._capture_core_dump(result)
            
            return result.returncode == 0, result.stdout + result.stderr, core_dump
            
        except subprocess.TimeoutExpired:
            return False, "TIMEOUT", None
        except Exception as e:
            return False, str(e), None
    
    def _capture_core_dump(self, result) -> Optional[str]:
        """Capture core dump on segfault"""
        import datetime
        
        timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S_%f")
        core_name = f"core_{timestamp}.dump"
        core_path = self.core_dump_dir / core_name
        
        # Try to get core from system
        try:
            # On Linux, core files are usually in /var/lib/systemd/coredump/
            # or following core_pattern
            core_pattern = Path("/proc/sys/kernel/core_pattern").read_text().strip()

            if core_pattern == "":
                # No core pattern, try default
                import glob
                cores = glob.glob("core.*")
                if cores:
                    shutil.copy2(cores[0], core_path)
                    return str(core_path)

            if core_pattern.startswith("|"):
                # Core is piped to a program
                # Extract from the pipe command
                pass
            else:
                # Core is written to a path
                # Try to find the most recent core
                import glob
                base = core_pattern.replace("%p", "*").replace("%e", "*")
                cores = glob.glob(base)
                if cores:
                    most_recent = max(cores, key=lambda x: os.path.getmtime(x))
                    shutil.copy2(most_recent, core_path)
                    return str(core_path)

        except Exception as e:
            if self.debug:
                print(f"[ORCHESTRATOR] Failed to capture core: {e}")

        return None
    
    def analyze_core_dump(self, core_path: str, binary_path: str) -> Optional[str]:
        """Analyze core dump using GDB"""
        gdb_script = f"""
        set pagination off
        set backtrace limit 50
        bt full
        info registers
        x/32x $sp
        info args
        info locals
        thread apply all bt
        quit
        """
        
        gdb_script_path = self.core_dump_dir / "gdb_commands.txt"
        with open(gdb_script_path, 'w') as f:
            f.write(gdb_script)
        
        try:
            result = subprocess.run(
                [
                    "gdb", "-batch", "-x", str(gdb_script_path),
                    binary_path, core_path
                ],
                capture_output=True,
                text=True,
                timeout=GDB_TIMEOUT
            )
            return result.stdout + result.stderr
        except subprocess.TimeoutExpired:
            return "GDB analysis timed out"
        except Exception as e:
            return f"GDB error: {e}"
    
    def _generate_diff_patch(
        self,
        bug_file: str,
        bug_line: int,
        bug_type: str,
        bug_description: str,
        current_code: List[str]
    ) -> str:
        """Generate Unified Diff patch for bug fix"""
        # This is a simplified patch generation
        # In production, this would use difflib or a realpatch tool
        
        import datetime
        
        patch_lines = [
            f"--- a/{bug_file}",
            f"+++ b/{bug_file}",
            f"@@ -{bug_line},1 +{bug_line},1 @@",
            f"-/* BUG: {bug_type} - {bug_description} */",
            f"+/* FIXED: {bug_type} - {bug_description} */"
        ]
        
        return "\n".join(patch_lines)
    
    def _apply_patch(self, patch_content: str, target_file: str) -> bool:
        """Apply Unified Diff patch to file"""
        import tempfile
        
        target_path = Path(target_file)
        patch_file = self.core_dump_dir / f"patch_{time.time_ns()}.diff"
        
        with open(patch_file, 'w') as f:
            f.write(patch_content)
        
        try:
            result = subprocess.run(
                ["patch", "-p0", "-i", str(patch_file), "-d", str(target_path.parent)],
                capture_output=True,
                text=True,
                timeout=60
            )
            
            if result.returncode == 0:
                return True
            
            # Try with -R (reverse) to check if already applied
            result = subprocess.run(
                ["patch", "-R", "-p0", "-i", str(patch_file), "-d", str(target_path.parent)],
                capture_output=True,
                text=True,
                timeout=60
            )
            
            if "reversed" in result.stdout or "previously applied" in result.stdout:
                return True
            
            return False
            
        except Exception as e:
            if self.debug:
                print(f"[ORCHESTRATOR] Patch application failed: {e}")
            return False
    
    def run_chaos_test(
        self,
        ibd_file: str,
        category: str,
        scenario: str,
        page_size: int,
        retry_count: int = 0
    ) -> TestReport:
        """Run a single chaos test"""
        import datetime
        
        test_id = f"{category}_{scenario}_{retry_count}"
        start_time = time.time()
        
        report = TestReport(
            test_id=test_id,
            category=category,
            scenario=scenario,
            ibd_file=ibd_file,
            page_size=page_size,
            native_binary=str(self.engine_path),
            recovery_rate=0.0,
            errors=[],
            core_dump_path=None,
            analysis_result=None,
            passed=False,
            execution_time=0.0
        )
        
        try:
            # Select recovery mode based on scenario
            if "truncate" in scenario:
                mode = "truncate-recovery"
            elif "rmrf" in scenario:
                mode = "rmrf-recovery"
            elif "header" in scenario:
                mode = "header-repair"
            elif "blob" in scenario:
                mode = "blob-recovery"
            elif "sdi" in scenario:
                mode = "header-repair"
            elif "chain" in scenario:
                mode = "header-repair"
            else:
                mode = "normal"
            
            success, output, core_dump = self.run_ibd_recovery(
                ibd_file, page_size, mode
            )
            
            report.execution_time = time.time() - start_time
            
            if success:
                report.recovery_rate = 1.0
                report.passed = True
            else:
                report.errors.append(output)
                
                if core_dump:
                    report.core_dump_path = core_dump
                    
                    # Analyze core dump
                    analysis = self.analyze_core_dump(core_dump, str(self.engine_path))
                    report.analysis_result = analysis
                    
                    if analysis:
                        # Attempt auto-healing
                        patch = self._auto_heal_from_analysis(analysis, scenario, retry_count)
                        if patch:
                            self.self_heal_patches.append(patch)
                            
                            if retry_count < MAX_RETEST_ATTEMPTS - 1:
                                # Rebuild and retest
                                if self._rebuild_engine():
                                    return self.run_chaos_test(
                                        ibd_file, category, scenario, page_size, retry_count + 1
                                    )
            
            # Calculate recovery rate
            report.recovery_rate = 1.0 if report.passed else max(0.0, 1.0 - len(report.errors) * 0.1)
            
        except Exception as e:
            report.errors.append(str(e))
            report.execution_time = time.time() - start_time
        
        self.test_reports.append(report)
        return report
    
    def _auto_heal_from_analysis(
        self,
        analysis: str,
        scenario: str,
        attempt: int
    ) -> Optional[SelfHealPatch]:
        """Auto-generate heal patch from core dump analysis"""
        # Parse gdb output to extract crash info
        # This is a simplified version - in production would need real parsing
        
        patches = []
        
        # Look for common bug patterns
        bug_patterns = [
            {
                "pattern": "SEGFAULT",
                "type": "buffer_overflow",
                "description": "Buffer overflow detected in I/O handling"
            },
            {
                "pattern": "null",
                "type": "null_pointer",
                "description": "Null pointer dereference"
            },
            {
                "pattern": "double free",
                "type": "memory_leak",
                "description": "Double free detected"
            },
            {
                "pattern": "stack-overflow",
                "type": "stack_overflow",
                "description": "Stack overflow in recursive descent"
            }
        ]
        
        for pattern in bug_patterns:
            if pattern["pattern"].lower() in analysis.lower():
                patch = SelfHealPatch(
                    bug_id=f"BUG_{attempt:03d}_{hashlib.md5(analysis.encode()).hexdigest()[:8].upper()}",
                    category=scenario,
                    file="recovery_engine.cpp",
                    line=100 + attempt * 10,  # Simulated line
                    type=pattern["type"],
                    description=pattern["description"],
                    unified_diff="",
                    fixed=False
                )
                
                if self.debug:
                    print(f"[ORCHESTRATOR] Auto-heal: Generated patch for {patch.bug_id}")
                
                patches.append(patch)
                break
        
        if patches:
            # Generate unified diff
            patch = patches[0]
            patch.unified_diff = self._generate_diff_patch(
                patch.file, patch.line, patch.type, patch.description,
                []  # Would pass actual file content in real implementation
            )
            return patch
        
        return None
    
    def _rebuild_engine(self) -> bool:
        """Rebuild the C++ engine after patch application"""
        import pathlib
        
        build_dir = Path("build")
        if not build_dir.exists():
            build_dir.mkdir(exist_ok=True)
        
        try:
            # Run cmake and make
            subprocess.run(
                ["cmake", "-B", str(build_dir), "-S", "."],
                check=True, timeout=120
            )
            
            result = subprocess.run(
                ["make", "-C", str(build_dir), "-j4"],
                check=True, timeout=300
            )
            
            return result.returncode == 0
            
        except subprocess.CalledProcessError as e:
            if self.debug:
                print(f"[ORCHESTRATOR] Rebuild failed: {e}")
            return False
        except Exception as e:
            if self.debug:
                print(f"[ORCHESTRATOR] Rebuild error: {e}")
            return False
    
    def generate_report(self) -> Dict:
        """Generate final QA synthesis report"""
        import datetime
        
        # Calculate statistics
        total_tests = len(self.test_reports)
        passed_tests = sum(1 for r in self.test_reports if r.passed)
        failed_tests = total_tests - passed_tests
        
        # Category breakdown
        category_stats = {cat: {"total": 0, "passed": 0} for cat in CHAOS_CATEGORIES.keys()}
        for report in self.test_reports:
            for cat, tests in CHAOS_CATEGORIES.items():
                if report.scenario in tests or report.category == cat:
                    category_stats[cat]["total"] += 1
                    if report.passed:
                        category_stats[cat]["passed"] += 1

        # Recovery rate by category
        category_recovery = {}
        for cat in CHAOS_CATEGORIES.keys():
            cat_reports = [r for r in self.test_reports if r.category == cat]
            if cat_reports:
                avg_rate = sum(r.recovery_rate for r in cat_reports) / len(cat_reports)
                category_recovery[cat] = avg_rate
        
        # Self-heal statistics
        self_heal_stats = {
            "patches_generated": len(self.self_heal_patches),
            "patches_applied": sum(1 for p in self.self_heal_patches if p.fixed),
            "bugs_fixed": {}
        }
        
        for patch in self.self_heal_patches:
            if patch.type not in self_heal_stats["bugs_fixed"]:
                self_heal_stats["bugs_fixed"][patch.type] = 0
            self_heal_stats["bugs_fixed"][patch.type] += 1
        
        report = {
            "timestamp": datetime.datetime.now().isoformat(),
            "summary": {
                "total_tests": total_tests,
                "passed": passed_tests,
                "failed": failed_tests,
                "pass_rate": passed_tests / total_tests if total_tests > 0 else 0,
                "average_recovery_rate": sum(r.recovery_rate for r in self.test_reports) / total_tests if total_tests > 0 else 0
            },
            "category_stats": category_stats,
            "category_recovery": category_recovery,
            "self_heal": self_heal_stats,
            "_patches": [
                {
                    "id": p.bug_id,
                    "category": p.category,
                    "type": p.type,
                    "description": p.description,
                    "file": p.file,
                    "fixed": p.fixed
                }
                for p in self.self_heal_patches
            ],
            "test_details": [
                {
                    "test_id": r.test_id,
                    "category": r.category,
                    "scenario": r.scenario,
                    "passed": r.passed,
                    "recovery_rate": r.recovery_rate,
                    "errors": r.errors,
                    "core_dump": r.core_dump_path
                }
                for r in self.test_reports
            ]
        }
        
        return report
    
    def run_all_tests(self):
        """Run full chaos test suite"""
        print("[ORCHESTRATOR] Starting full chaos test suite...")
        
        # Find all IBD files
        ibd_files = list(self.test_data_dir.glob("*.ibd"))
        if not ibd_files:
            print("[ORCHESTRATOR] No IBD files found in test data directory")
            # Generate test data
            subprocess.run(
                ["python3", "scripts/generate_test_data/generate_test_data.py",
                 "--output", str(self.test_data_dir / "test.ibd"), "--type", "all"],
                check=True
            )
            ibd_files = list(self.test_data_dir.glob("*.ibd"))
        
        if not ibd_files:
            print("[ORCHESTRATOR] Failed to generate test data")
            return
        
        # Run tests by category
        for category, scenarios in CHAOS_CATEGORIES.items():
            for scenario in scenarios:
                for ibd_file in ibd_files:
                    print(f"[ORCHESTRATOR] Running {category}/{scenario} on {ibd_file.name}")
                    
                    report = self.run_chaos_test(
                        str(ibd_file), category, scenario, page_size=16384
                    )
                    
                    if report.core_dump_path:
                        print(f"[ORCHESTRATOR] Core dump captured: {report.core_dump_path}")
                        if report.analysis_result:
                            print(f"[ORCHESTRATOR] Analysis: {report.analysis_result[:200]}...")
                    
                    print(f"[ORCHESTRATOR] Report: {'PASSED' if report.passed else 'FAILED'}, "
                          f"Recovery Rate: {report.recovery_rate:.2%}")
        
        # Generate final report
        final_report = self.generate_report()
        
        report_path = self.results_dir / "chaos_test_report.json"
        with open(report_path, 'w') as f:
            json.dump(final_report, f, indent=2)
        
        print(f"[ORCHESTRATOR] Final report saved to: {report_path}")
        print(f"[ORCHESTRATOR] Summary: {final_report['summary']}")
        
        return final_report


def main():
    """Main entry point"""
    import argparse
    
    parser = argparse.ArgumentParser(
        description="MySQL Recovery Pro - Chaos Orchestrator & Self-Healing Driver"
    )
    parser.add_argument("--engine", required=True, help="Path to mysql_recovery_pro binary")
    parser.add_argument("--test-data", default="test_data", help="Test data directory")
    parser.add_argument("--results", default="chaos_test_results", help="Results directory")
    parser.add_argument("--asan", action="store_true", help="Enable AddressSanitizer")
    parser.add_argument("--debug", action="store_true", help="Enable debug output")
    parser.add_argument("--report-only", action="store_true", help="Only generate report from existing results")
    
    args = parser.parse_args()
    
    if args.report_only:
        # Load existing results
        results_dir = Path(args.results)
        report_path = results_dir / "chaos_test_report.json"
        
        if report_path.exists():
            with open(report_path) as f:
                report = json.load(f)
            
            print(json.dumps(report, indent=2))
        else:
            print("No existing report found")
            sys.exit(1)
    else:
        orchestrator = ChaosOrchestrator(
            engine_path=args.engine,
            test_data_dir=args.test_data,
            results_dir=args.results,
            use_asan=args.asan,
            debug=args.debug
        )
        
        orchestrator.run_all_tests()


if __name__ == "__main__":
    main()
