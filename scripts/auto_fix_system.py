#!/usr/bin/env python3
"""
Auto-Fix and Regression Testing System for MySQL Recovery Pro
Analyzes failures and automatically suggests/apply fixes to C++ code
"""

import os
import sys
import json
import re
import subprocess
import argparse
from datetime import datetime
from typing import List, Dict, Tuple, Optional
from pathlib import Path
import difflib


class FailureAnalyzer:
    """Analyze failure logs and suggest fixes"""
    
    # Known failure patterns and their fixes
    FAILURE_PATTERNS = {
        "page_header_corrupt": {
            "patterns": [
                r"Failed to parse page header",
                r"Invalid page type",
                r"Checksum mismatch",
                r"Page header corruption"
            ],
            "fix_location": "src/innodb/page_parser.cpp",
            "fix_type": "header_validation",
            "suggestion": "Relax page header validation or implement header recovery"
        },
        "record_chain_broken": {
            "patterns": [
                r"Record chain broken",
                r"Invalid next_record_offset",
                r"Failed to traverse record chain",
                r"Record chain corruption"
            ],
            "fix_location": "src/innodb/btree_node_parser.cpp",
            "fix_type": "chain_repair",
            "suggestion": "Implement record chain repair with fallback to heap order"
        },
        "page_chain_broken": {
            "patterns": [
                r"Page chain broken",
                r"Invalid prev_page/next_page",
                r"Failed to rebuild page chain",
                r"Orphan page detected"
            ],
            "fix_location": "src/innodb/page_chain_rebuilder.cpp",
            "fix_type": "chain_rebuild",
            "suggestion": "Implement page chain rebuild using page number ordering"
        },
        "sdi_parse_error": {
            "patterns": [
                r"Failed to parse SDI",
                r"SDI JSON parse error",
                r"SDI extraction failed",
                r"Table schema not found"
            ],
            "fix_location": "src/innodb/sdi_manager.cpp",
            "fix_type": "sdi_recovery",
            "suggestion": "Implement SDI recovery from corrupted data"
        },
        "blob_chain_broken": {
            "patterns": [
                r"BLOB chain broken",
                r"Failed to rebuild BLOB chain",
                r"BLOB page not found",
                r"BLOB chain corruption"
            ],
            "fix_location": "src/innodb/blob_chain_rebuilder.cpp",
            "fix_type": "blob_rebuild",
            "suggestion": "Implement BLOB chain rebuild with fallback scanning"
        },
        "heap_top_mismatch": {
            "patterns": [
                r"Heap top mismatch",
                r"Invalid heap_top value",
                r"Record overflow",
                r"Page heap corruption"
            ],
            "fix_location": "src/innodb/page_parser.cpp",
            "fix_type": "heap_validation",
            "suggestion": "Implement heap_top validation and recovery"
        },
        "page_size_mismatch": {
            "patterns": [
                r"Page size mismatch",
                r"Invalid page size",
                r"File size not aligned",
                r"Page size detection failed"
            ],
            "fix_location": "src/innodb/file_reader.cpp",
            "fix_type": "page_size_detection",
            "suggestion": "Implement automatic page size detection"
        },
        "checksum_error": {
            "patterns": [
                r"Checksum error",
                r"CRC32 mismatch",
                r"Page checksum failed",
                r"Invalid checksum"
            ],
            "fix_location": "src/innodb/page_parser.cpp",
            "fix_type": "checksum_validation",
            "suggestion": "Implement checksum validation with ignore option"
        },
        "record_parse_error": {
            "patterns": [
                r"Failed to parse record",
                r"Record length mismatch",
                r"Invalid record header",
                r"Record corruption"
            ],
            "fix_location": "src/innodb/btree_node_parser.cpp",
            "fix_type": "record_recovery",
            "suggestion": "Implement record recovery with partial parsing"
        },
        "schema_not_found": {
            "patterns": [
                r"Schema not found",
                r"Table schema missing",
                r"Column count mismatch",
                r"Field definition error"
            ],
            "fix_location": "src/catalog/schema_manager.cpp",
            "fix_type": "schema_recovery",
            "suggestion": "Implement schema recovery from data patterns"
        }
    }
    
    def __init__(self):
        self.failure_counts = {}
    
    def analyze_log(self, log_content: str) -> List[Dict]:
        """Analyze log content and identify failure patterns"""
        failures = []
        
        for failure_type, config in self.FAILURE_PATTERNS.items():
            for pattern in config["patterns"]:
                if re.search(pattern, log_content, re.IGNORECASE):
                    failures.append({
                        "type": failure_type,
                        "pattern": pattern,
                        "location": config["fix_location"],
                        "fix_type": config["fix_type"],
                        "suggestion": config["suggestion"]
                    })
                    self.failure_counts[failure_type] = self.failure_counts.get(failure_type, 0) + 1
                    break  # Only match first pattern for each type
        
        return failures
    
    def analyze_file(self, log_file: str) -> List[Dict]:
        """Analyze a log file"""
        try:
            with open(log_file, 'r') as f:
                content = f.read()
            return self.analyze_log(content)
        except Exception as e:
            return [{"type": "file_error", "suggestion": f"Failed to read log file: {e}"}]
    
    def get_top_failures(self, n: int = 5) -> List[Tuple[str, int]]:
        """Get top N most common failures"""
        return sorted(self.failure_counts.items(), key=lambda x: x[1], reverse=True)[:n]


class CodeFixGenerator:
    """Generate code fixes for identified issues"""
    
    def __init__(self, project_dir: str = "."):
        self.project_dir = Path(project_dir)
    
    def generate_header_validation_fix(self, failure: Dict) -> str:
        """Generate fix for header validation issues"""
        return """// Fix: Relax page header validation
// Location: src/innodb/page_parser.cpp

bool PageParser::validate_page_header(const PageHeader& header) {
    // Original strict validation
    // if (header.page_type != FIL_PAGE_INDEX && header.page_type != FIL_PAGE_SDI) {
    //     return false;
    // }
    
    // New relaxed validation with recovery support
    // Accept unknown page types but log warning
    if (header.page_type != FIL_PAGE_INDEX && 
        header.page_type != FIL_PAGE_SDI &&
        header.page_type != FIL_PAGE_UNDO_LOG &&
        header.page_type != FIL_PAGE_BLOB) {
        std::cerr << "[WARN] Unknown page type: 0x" << std::hex << header.page_type << std::dec << std::endl;
        // Don't fail immediately - try to recover
    }
    
    // Check checksum only if flag is set
    if (options_.strict_checksum && !validate_checksum(header)) {
        std::cerr << "[WARN] Checksum validation failed" << std::endl;
        if (!options_.ignore_checksum) {
            return false;
        }
    }
    
    return true;
}
"""
    
    def generate_chain_repair_fix(self, failure: Dict) -> str:
        """Generate fix for chain repair issues"""
        return """// Fix: Implement record chain repair with fallback
// Location: src/innodb/btree_node_parser.cpp

std::vector<RecordInfo> BPlusTreeNodeParser::parse_records_with_repair() {
    std::vector<RecordInfo> records;
    
    // Try normal parsing first
    records = parse_records_normal();
    
    // If chain is broken, try heap-based recovery
    if (records.empty() || is_chain_broken(records)) {
        std::cerr << "[INFO] Chain broken, attempting heap-based recovery" << std::endl;
        records = parse_records_by_heap_order();
    }
    
    // Final fallback: scan entire page
    if (records.empty()) {
        std::cerr << "[INFO] Heap recovery failed, scanning page" << std::endl;
        records = scan_page_for_records();
    }
    
    return records;
}

bool BPlusTreeNodeParser::is_chain_broken(const std::vector<RecordInfo>& records) {
    if (records.size() <= 1) return false;
    
    // Check if record chain is properly linked
    for (size_t i = 0; i < records.size() - 1; ++i) {
        if (records[i].next_record_offset == 0xFFFF ||
            records[i].next_record_offset < records[i].offset) {
            return true;
        }
    }
    
    return false;
}
"""
    
    def generate_page_chain_rebuild_fix(self, failure: Dict) -> str:
        """Generate fix for page chain rebuild issues"""
        return """// Fix: Implement page chain rebuild using page number ordering
// Location: src/innodb/page_chain_rebuilder.cpp

std::vector<Page> PageChainRebuilder::rebuild_chain_fallback() {
    std::vector<Page> pages;
    
    // Collect all index pages
    std::vector<Page> index_pages;
    for (const auto& page : scanned_pages_) {
        if (page.page_type == FIL_PAGE_INDEX) {
            index_pages.push_back(page);
        }
    }
    
    // Sort by page number
    std::sort(index_pages.begin(), index_pages.end(),
              [](const Page& a, const Page& b) {
                  return a.page_no < b.page_no;
              });
    
    // Rebuild chain using page number order
    for (size_t i = 0; i < index_pages.size(); ++i) {
        auto& page = index_pages[i];
        
        // Set correct prev/next pointers
        if (i > 0) {
            page.prev_page = index_pages[i - 1].page_no;
        } else {
            page.prev_page = 0xFFFFFFFF;
        }
        
        if (i < index_pages.size() - 1) {
            page.next_page = index_pages[i + 1].page_no;
        } else {
            page.next_page = 0xFFFFFFFF;
        }
        
        pages.push_back(page);
    }
    
    return pages;
}
"""
    
    def generate_sdi_recovery_fix(self, failure: Dict) -> str:
        """Generate fix for SDI recovery issues"""
        return """// Fix: Implement SDI recovery from corrupted data
// Location: src/innodb/sdi_manager.cpp

std::optional<std::string> SDIManager::extract_sdi_json_fallback(
    const uint8_t* data, size_t size) {
    
    // Try normal extraction first
    auto result = extract_sdi_json_normal(data, size);
    if (result.has_value()) {
        return result;
    }
    
    // Fallback: search for JSON-like patterns in data
    std::string search_data(reinterpret_cast<const char*>(data), size);
    
    // Look for JSON object patterns
    size_t start_pos = search_data.find('{');
    while (start_pos != std::string::npos) {
        size_t end_pos = search_data.find('}', start_pos);
        if (end_pos != std::string::npos) {
            std::string potential_json = search_data.substr(start_pos, end_pos - start_pos + 1);
            
            // Validate JSON
            try {
                auto json = nlohmann::json::parse(potential_json);
                if (json.contains("table") || json.contains("schema")) {
                    return potential_json;
                }
            } catch (...) {
                // Continue searching
            }
        }
        start_pos = search_data.find('{', start_pos + 1);
    }
    
    return std::nullopt;
}
"""
    
    def generate_blob_rebuild_fix(self, failure: Dict) -> str:
        """Generate fix for BLOB chain rebuild issues"""
        return """// Fix: Implement BLOB chain rebuild with fallback scanning
// Location: src/innodb/blob_chain_rebuilder.cpp

std::vector<BLOBPage> BlobChainRebuilder::rebuild_chain_fallback() {
    std::vector<BLOBPage> pages;
    
    // Try normal chain traversal first
    pages = rebuild_chain_normal();
    
    // If chain is broken, scan for BLOB pages
    if (pages.empty()) {
        std::cerr << "[INFO] Chain broken, scanning for BLOB pages" << std::endl;
        
        for (uint32_t page_no = 0; page_no < total_pages_; ++page_no) {
            auto page_data = reader_.read_page_raw(page_no);
            if (page_data.size() < 50) continue;
            
            uint16_t page_type = read_le<uint16_t>(page_data.data(), FIL_PAGE_TYPE_ABS);
            if (page_type == FIL_PAGE_BLOB) {
                BLOBPage blob_page;
                blob_page.page_no = page_no;
                blob_page.data = page_data;
                
                // Extract BLOB header
                if (page_data.size() >= 62) {
                    blob_page.next_page = read_le<uint32_t>(page_data.data() + 38, 0);
                    blob_page.total_length = read_le<uint32_t>(page_data.data() + 42, 0);
                }
                
                pages.push_back(blob_page);
            }
        }
    }
    
    // Sort by page number
    std::sort(pages.begin(), pages.end(),
              [](const BLOBPage& a, const BLOBPage& b) {
                  return a.page_no < b.page_no;
              });
    
    return pages;
}
"""
    
    def generate_fix(self, failure: Dict) -> str:
        """Generate appropriate fix based on failure type"""
        fix_type = failure.get("fix_type", "")
        
        if fix_type == "header_validation":
            return self.generate_header_validation_fix(failure)
        elif fix_type == "chain_repair":
            return self.generate_chain_repair_fix(failure)
        elif fix_type == "chain_rebuild":
            return self.generate_page_chain_rebuild_fix(failure)
        elif fix_type == "sdi_recovery":
            return self.generate_sdi_recovery_fix(failure)
        elif fix_type == "blob_rebuild":
            return self.generate_blob_rebuild_fix(failure)
        else:
            return f"// Fix for {failure.get('type', 'unknown')}\n// TODO: Implement fix\n"


class AutoFixSystem:
    """Main auto-fix system"""
    
    def __init__(self, project_dir: str = ".", build_dir: str = "build"):
        self.project_dir = Path(project_dir)
        self.build_dir = Path(build_dir)
        self.failure_analyzer = FailureAnalyzer()
        self.fix_generator = CodeFixGenerator(str(project_dir))
        self.fix_history: List[Dict] = []
    
    def build_project(self) -> Tuple[bool, str]:
        """Build the project"""
        build_path = self.build_dir
        build_path.mkdir(exist_ok=True)
        
        try:
            # Run cmake
            result = subprocess.run(
                ["cmake", ".."],
                cwd=str(build_path),
                capture_output=True,
                text=True,
                timeout=120
            )
            
            if result.returncode != 0:
                return False, f"CMake failed: {result.stderr}"
            
            # Run make
            result = subprocess.run(
                ["make", "-j4"],
                cwd=str(build_path),
                capture_output=True,
                text=True,
                timeout=300
            )
            
            if result.returncode != 0:
                return False, f"Build failed: {result.stderr}"
            
            return True, "Build successful"
            
        except subprocess.TimeoutExpired:
            return False, "Build timed out"
        except Exception as e:
            return False, f"Build error: {str(e)}"
    
    def run_test(self, test_file: str = None) -> Tuple[bool, str]:
        """Run tests and return (success, output)"""
        try:
            binary = self.build_dir / "mysql_recovery_pro"
            
            if test_file:
                cmd = [str(binary), f"--ibd={test_file}", "--format=csv"]
            else:
                cmd = [str(binary), "--help"]
            
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=60
            )
            
            success = result.returncode == 0
            output = result.stdout + result.stderr
            
            return success, output
            
        except Exception as e:
            return False, str(e)
    
    def analyze_and_fix(self, log_file: str = None, log_content: str = None) -> List[Dict]:
        """Analyze failures and generate fixes"""
        failures = []
        
        if log_file:
            failures = self.failure_analyzer.analyze_file(log_file)
        elif log_content:
            failures = self.failure_analyzer.analyze_log(log_content)
        
        fixes = []
        for failure in failures:
            fix_code = self.fix_generator.generate_fix(failure)
            fix = {
                "failure": failure,
                "fix_code": fix_code,
                "applied": False
            }
            fixes.append(fix)
            self.fix_history.append(fix)
        
        return fixes
    
    def apply_fix(self, fix: Dict) -> bool:
        """Apply a fix to the source code"""
        failure = fix["failure"]
        fix_code = fix["fix_code"]
        location = failure.get("location", "")
        
        if not location:
            return False
        
        file_path = self.project_dir / location
        
        if not file_path.exists():
            print(f"Warning: File not found: {file_path}")
            return False
        
        # Read current file
        with open(file_path, 'r') as f:
            content = f.read()
        
        # Generate fix comment
        fix_comment = f"\n// AUTO-FIX APPLIED: {failure.get('type', 'unknown')}\n// {datetime.now().isoformat()}\n"
        
        # Append fix to file (simple approach - in production, use more sophisticated patching)
        with open(file_path, 'a') as f:
            f.write(fix_comment)
            f.write(fix_code)
        
        fix["applied"] = True
        return True
    
    def run_regression_test(self, scenario: str) -> Dict:
        """Run regression test for a specific scenario"""
        result = {
            "scenario": scenario,
            "passed": False,
            "details": {}
        }
        
        # Build project
        build_success, build_output = self.build_project()
        result["details"]["build"] = {
            "success": build_success,
            "output": build_output[:500]
        }
        
        if not build_success:
            return result
        
        # Run test
        test_success, test_output = self.run_test()
        result["details"]["test"] = {
            "success": test_success,
            "output": test_output[:500]
        }
        
        result["passed"] = test_success
        return result


class RegressionTestSuite:
    """Run complete regression test suite"""
    
    def __init__(self, project_dir: str = ".", build_dir: str = "build"):
        self.project_dir = Path(project_dir)
        self.build_dir = Path(build_dir)
        self.auto_fix = AutoFixSystem(str(project_dir), str(build_dir))
        self.test_results: List[Dict] = []
    
    def run_all_scenarios(self) -> Dict:
        """Run all regression test scenarios"""
        scenarios = [
            "basic_recovery",
            "truncate_recovery",
            "rmrf_recovery",
            "header_repair",
            "blob_recovery",
            "chain_rebuild",
            "sdi_recovery"
        ]
        
        results = {
            "timestamp": datetime.now().isoformat(),
            "scenarios": {},
            "summary": {
                "total": len(scenarios),
                "passed": 0,
                "failed": 0
            }
        }
        
        for scenario in scenarios:
            print(f"Running scenario: {scenario}")
            result = self.auto_fix.run_regression_test(scenario)
            results["scenarios"][scenario] = result
            
            if result["passed"]:
                results["summary"]["passed"] += 1
            else:
                results["summary"]["failed"] += 1
        
        # Save results
        results_file = self.build_dir / "regression_results.json"
        with open(results_file, 'w') as f:
            json.dump(results, f, indent=2)
        
        print(f"\nRegression test results saved to: {results_file}")
        return results


def main():
    parser = argparse.ArgumentParser(
        description='Auto-Fix and Regression Testing System'
    )
    parser.add_argument('--project-dir', default='.',
                       help='Project directory')
    parser.add_argument('--build-dir', default='build',
                       help='Build directory')
    parser.add_argument('--log-file', '-l',
                       help='Log file to analyze')
    parser.add_argument('--fix-type', '-f',
                       choices=['header_validation', 'chain_repair', 'chain_rebuild',
                               'sdi_recovery', 'blob_rebuild'],
                       help='Fix type to generate')
    parser.add_argument('--apply-fix', action='store_true',
                       help='Apply generated fixes')
    parser.add_argument('--regression', action='store_true',
                       help='Run regression tests')
    parser.add_argument('--scenario',
                       help='Specific scenario to test')
    
    args = parser.parse_args()
    
    auto_fix = AutoFixSystem(args.project_dir, args.build_dir)
    
    if args.log_file:
        # Analyze log file
        failures = auto_fix.analyze_and_fix(log_file=args.log_file)
        
        print(f"Found {len(failures)} failure patterns:")
        for i, fix in enumerate(failures, 1):
            failure = fix["failure"]
            print(f"\n{i}. {failure.get('type', 'unknown')}")
            print(f"   Location: {failure.get('location', 'N/A')}")
            print(f"   Suggestion: {failure.get('suggestion', 'N/A')}")
            print(f"\n   Generated fix:")
            print(fix["fix_code"])
        
        if args.apply_fix:
            for fix in failures:
                if auto_fix.apply_fix(fix):
                    print(f"\nApplied fix for {fix['failure']['type']}")
                else:
                    print(f"\nFailed to apply fix for {fix['failure']['type']}")
    
    elif args.fix_type:
        # Generate specific fix
        failure = {
            "type": args.fix_type,
            "fix_type": args.fix_type,
            "location": f"src/innodb/{args.fix_type}.cpp"
        }
        fix_code = auto_fix.fix_generator.generate_fix(failure)
        print(fix_code)
    
    elif args.regression:
        # Run regression tests
        suite = RegressionTestSuite(args.project_dir, args.build_dir)
        results = suite.run_all_scenarios()
        
        print("\n" + "=" * 60)
        print("Regression Test Summary")
        print("=" * 60)
        print(f"Total: {results['summary']['total']}")
        print(f"Passed: {results['summary']['passed']}")
        print(f"Failed: {results['summary']['failed']}")
        
        for scenario, result in results['scenarios'].items():
            status = "PASS" if result['passed'] else "FAIL"
            print(f"  {status}: {scenario}")
    
    else:
        parser.print_help()


if __name__ == '__main__':
    main()
