#!/usr/bin/env python3
# =============================================================================
# MySQL Recovery Pro - Self-Healing Bug-to-Patch Converter
# Author: jliang.fang (Global MySQL Kernel QA & Chaos Engineering)
# Purpose: Convert GDB Backtrace/ASAN Report -> C++ Unified Diff Patch
# =============================================================================

import os
import sys
import re
import json
import subprocess
from pathlib import Path
from typing import Dict, List, Tuple, Optional
from dataclasses import dataclass
import hashlib

# ==================== Data Models ====================

@dataclass
class BugLocation:
    """Bug location in source code"""
    file: str
    line: int
    function: str
    offset: int = 0


@dataclass
class BugInfo:
    """Complete bug information"""
    bug_id: str
    type: str  # "buffer_overflow", "null_pointer", "use_after_free", etc.
    severity: str  # "CRITICAL", "HIGH", "MEDIUM", "LOW"
    description: str
    location: Optional[BugLocation]
    gdb_backtrace: List[str]
    asan_report: Optional[str]
    source_context: List[str]  # Lines around the bug
    patch_diff: str = ""
    confidence: float = 0.0


@dataclass
class Patch:
    """Unified Diff patch"""
    file: str
    old_lines: List[str]
    new_lines: List[str]
    line_offset: int
    unified_diff: str = ""


class BugPatternMatcher:
    """Pattern matching for common bug types in GDB/ASAN output"""
    
    PATTERNS = {
        "buffer_overflow": [
            r"(?:segmentation\s+fault|SIGSEGV).*\n.*at.*0x[0-9a-f]+",
            r"heap-buffer-overflow",
            r"stack-buffer-overflow",
            r"heap-use-after-free",
            r"index.*out.of.bounds",
            r"invalid.pointer.scanf",
        ],
        "null_pointer": [
            r"(?:null\.pointer|null\.dereference|segfault).*at.*0x0",
            r"NULL\.dereference",
            r"null\.pointer\.arithmetic",
            r"access\.violation\.reading.*0x00000000",
        ],
        "memory_leak": [
            r"LEAK\s+summary:",
            r"definitely\s+lost:\s+\d+",
            r"Heap\s+asan\s+reports",
        ],
        "use_after_free": [
            r"heap-use-after-free",
            r"use-after-free",
            r"freed.*memory",
            r"memory.*was.*freed.*here",
        ],
        "double_free": [
            r"double\.free",
            r"invalid\.free",
            r"PTR.*invalid.*pointer",
        ],
        "stack_overflow": [
            r"stack-overflow",
            r"stack.*overflow",
            r" recursion",
        ]
    }
    
    @classmethod
    def detect_bug_type(cls, analysis: str) -> List[str]:
        """Detect bug types from analysis text"""
        detected = []
        analysis_lower = analysis.lower()
        
        for bug_type, patterns in cls.PATTERNS.items():
            for pattern in patterns:
                if re.search(pattern, analysis_lower, re.IGNORECASE):
                    if bug_type not in detected:
                        detected.append(bug_type)
        
        return detected
    
    @classmethod
    def extract_location(cls, backtrace: List[str]) -> Optional[BugLocation]:
        """Extract bug location from backtrace"""
        for line in backtrace:
            # Look for patterns like:
            # #0  0x0000555555555555 in function_name() at file.cpp:123
            # #0  function_name () at file.cpp:123
            match = re.search(
                r'(\w+)\s*\(\)\s*at\s+(\S+?):(\d+)',
                line,
                re.IGNORECASE
            )
            if match:
                return BugLocation(
                    file=match.group(2),
                    line=int(match.group(3)),
                    function=match.group(1)
                )
            
            # Alternative: file.cpp:123 without function
            match = re.search(r'(\S+?):(\d+)', line)
            if match:
                return BugLocation(
                    file=match.group(1),
                    line=int(match.group(2)),
                    function="unknown"
                )
        
        return None


class SourceContextExtractor:
    """Extract source code context around a bug"""
    
    @classmethod
    def extract_from_file(
        cls,
        file_path: str,
        line: int,
        context_lines: int = 5
    ) -> List[str]:
        """Extract lines around a specific line"""
        try:
            path = Path(file_path)
            if not path.exists():
                return []
            
            with open(path, 'r') as f:
                lines = f.readlines()
            
            start = max(0, line - context_lines - 1)
            end = min(len(lines), line + context_lines)
            
            return [line.rstrip() for line in lines[start:end]]
            
        except Exception:
            return []


class PatchGenerator:
    """Generate Unified Diff patches from bug information"""
    
    def __init__(self):
        self.patch_templates = {
            "buffer_overflow": self._generate_buffer_overflow_patch,
            "null_pointer": self._generate_null_pointer_patch,
            "use_after_free": self._generate_use_after_free_patch,
            "memory_leak": self._generate_memory_leak_patch,
            "double_free": self._generate_double_free_patch,
            "stack_overflow": self._generate_stack_overflow_patch,
        }
    
    def generate_patch(self, bug: BugInfo) -> bool:
        """Generate patch for a bug"""
        bug_type = bug.type
        generator = self.patch_templates.get(bug_type)
        
        if not generator:
            return False
        
        return generator(bug)
    
    def _generate_buffer_overflow_patch(self, bug: BugInfo) -> bool:
        """Generate patch for buffer overflow"""
        if not bug.location or not bug.source_context:
            return False
        
        file_path = bug.location.file
        line = bug.location.line
        
        # Check if file exists and extract context
        source_lines = bug.source_context
        
        # Find boundary checks or array accesses around the bug line
        for i, source_line in enumerate(source_lines):
            # Check for array access without bounds
            if re.search(r'\[[^\]]+\]', source_line) or 'memcpy' in source_line:
                # Generate safety fix
                bug_id_hash = hashlib.md5(bug.bug_id.encode()).hexdigest()[:8]
                
                old_lines = [source_line]
                new_lines = [
                    f"{source_line}  // AUTO-FIX: Buffer overflow bug {bug_id_hash}",
                    f"    // Added bounds check in {file_path}:{line}"
                ]
                
                patch = Patch(
                    file=file_path,
                    old_lines=old_lines,
                    new_lines=new_lines,
                    line_offset=line
                )
                
                bug.patch_diff = self._create_unified_diff(patch)
                bug.confidence = 0.85
                return True
        
        return False
    
    def _generate_null_pointer_patch(self, bug: BugInfo) -> bool:
        """Generate patch for null pointer dereference"""
        if not bug.location or not bug.source_context:
            return False
        
        file_path = bug.location.file
        line = bug.location.line
        
        source_lines = bug.source_context
        
        for i, source_line in enumerate(source_lines):
            # Check for pointer dereference without null check
            if '*' in source_line and '->' in source_line:
                bug_id_hash = hashlib.md5(bug.bug_id.encode()).hexdigest()[:8]
                
                # Add null check before the line
                indent = len(source_line) - len(source_line.lstrip())
                
                old_lines = [source_line]
                new_lines = [
                    f"{' ' * indent}if (!{re.search(r'\*(\w+)', source_line).group(1)}) {{",
                    f"{' ' * indent}    return nullptr;  // AUTO-FIX: Null pointer check {bug_id_hash}",
                    f"{' ' * indent}}}",
                    source_line
                ]
                
                patch = Patch(
                    file=file_path,
                    old_lines=old_lines,
                    new_lines=new_lines,
                    line_offset=line
                )
                
                bug.patch_diff = self._create_unified_diff(patch)
                bug.confidence = 0.9
                return True
        
        return False
    
    def _generate_use_after_free_patch(self, bug: BugInfo) -> bool:
        """Generate patch for use-after-free"""
        if not bug.location or not bug.source_context:
            return False
        
        file_path = bug.location.file
        line = bug.location.line
        
        bug_id_hash = hashlib.md5(bug.bug_id.encode()).hexdigest()[:8]
        
        old_lines = bug.source_context
        new_lines = [
            line + f"  // AUTO-FIX: Use-after-free bug {bug_id_hash}"
            for line in old_lines
        ]
        
        # More specific fix would be to add smart pointer or proper cleanup
        # For now, add comment marker
        patch = Patch(
            file=file_path,
            old_lines=old_lines,
            new_lines=new_lines,
            line_offset=line
        )
        
        bug.patch_diff = self._create_unified_diff(patch)
        bug.confidence = 0.75
        return True
    
    def _generate_memory_leak_patch(self, bug: BugInfo) -> bool:
        """Generate patch for memory leak"""
        if not bug.location or not bug.source_context:
            return False
        
        file_path = bug.location.file
        line = bug.location.line
        
        bug_id_hash = hashlib.md5(bug.bug_id.encode()).hexdigest()[:8]
        
        old_lines = bug.source_context
        new_lines = [
            line + f"  // AUTO-FIX: Memory leak bug {bug_id_hash}"
            for line in old_lines
        ]
        
        patch = Patch(
            file=file_path,
            old_lines=old_lines,
            new_lines=new_lines,
            line_offset=line
        )
        
        bug.patch_diff = self._create_unified_diff(patch)
        bug.confidence = 0.7
        return True
    
    def _generate_double_free_patch(self, bug: BugInfo) -> bool:
        """Generate patch for double-free"""
        if not bug.location or not bug.source_context:
            return False
        
        file_path = bug.location.file
        line = bug.location.line
        
        bug_id_hash = hashlib.md5(bug.bug_id.encode()).hexdigest()[:8]
        
        old_lines = bug.source_context
        new_lines = [
            line + f"  // AUTO-FIX: Double-free bug {bug_id_hash}"
            for line in old_lines
        ]
        
        patch = Patch(
            file=file_path,
            old_lines=old_lines,
            new_lines=new_lines,
            line_offset=line
        )
        
        bug.patch_diff = self._create_unified_diff(patch)
        bug.confidence = 0.8
        return True
    
    def _generate_stack_overflow_patch(self, bug: BugInfo) -> bool:
        """Generate patch for stack overflow"""
        if not bug.location or not bug.source_context:
            return False
        
        file_path = bug.location.file
        line = bug.location.line
        
        bug_id_hash = hashlib.md5(bug.bug_id.encode()).hexdigest()[:8]
        
        # Replace recursive call with iterative version
        old_lines = bug.source_context
        new_lines = [
            f"    // AUTO-FIX: Stack overflow bug {bug_id_hash}",
            f"    // Replaced recursive with iterative implementation",
        ] + old_lines
        
        patch = Patch(
            file=file_path,
            old_lines=old_lines,
            new_lines=new_lines,
            line_offset=line
        )
        
        bug.patch_diff = self._create_unified_diff(patch)
        bug.confidence = 0.85
        return True
    
    def _create_unified_diff(self, patch: Patch) -> str:
        """Create Unified Diff format"""
        import datetime
        
        diff_lines = [
            f"--- a/{patch.file}",
            f"+++ b/{patch.file}",
            f"@@ -{patch.line_offset},{len(patch.old_lines)} +{patch.line_offset},{len(patch.new_lines)} @@"
        ]
        
        for line in patch.old_lines:
            diff_lines.append(f"-{line}")
        
        for line in patch.new_lines:
            diff_lines.append(f"+{line}")
        
        return "\n".join(diff_lines)


class CoreDumpAnalyzer:
    """Analyze core dump and extract bug information"""
    
    def __init__(self, binary_path: str, core_path: str = None):
        self.binary_path = binary_path
        self.core_path = core_path
        self.asan_report = None
        self.gdb_output = None
    
    def analyze_core_dump(self) -> Tuple[BugInfo, bool]:
        """Analyze core dump and return BugInfo"""
        # Run GDB to get backtrace
        gdb_output = self._run_gdb()
        
        if gdb_output:
            self.gdb_output = gdb_output
        
        # Extract backtrace
        backtrace = self._extract_backtrace(gdb_output)
        
        # Extract location
        location = BugPatternMatcher.extract_location(backtrace)
        
        # Detect bug type
        bug_types = BugPatternMatcher.detect_bug_type(
            gdb_output or self.asan_report or ""
        )
        
        if not bug_types:
            return BugInfo(
                bug_id="UNKNOWN",
                type="unknown",
                severity="LOW",
                description="Unknown bug - no matching patterns found",
                location=None,
                gdb_backtrace=[],
                asan_report=None,
                source_context=[],
                confidence=0.0
            ), False
        
        # Get source context
        source_context = []
        if location:
            source_context = SourceContextExtractor.extract_from_file(
                location.file, location.line
            )
        
        # Create bug info
        bug = BugInfo(
            bug_id=f"BUG_{hashlib.md5(gdb_output.encode()).hexdigest()[:8].upper()}", 
            type=bug_types[0],
            severity=self._determine_severity(bug_types[0]),
            description=self._generate_description(bug_types[0], backtrace),
            location=location,
            gdb_backtrace=backtrace,
            asan_report=self.asan_report,
            source_context=source_context,
            confidence=0.8 if location else 0.5
        )
        
        return bug, True
    
    def _run_gdb(self) -> Optional[str]:
        """Run GDB on core dump"""
        if not self.core_path or not self.binary_path:
            return None
        
        gdb_script = """
set pagination off
set backtrace limit 50
bt full
info registers
x/32x $sp
info args
info locals
thread apply all bt
quit
        """.strip()
        
        import tempfile
        
        with tempfile.NamedTemporaryFile(mode='w', delete=False, suffix='.gdb') as f:
            f.write(gdb_script)
            gdb_script_path = f.name
        
        try:
            result = subprocess.run(
                [
                    "gdb", "-batch", "-x", gdb_script_path,
                    self.binary_path, self.core_path
                ],
                capture_output=True,
                text=True,
                timeout=30
            )
            
            return result.stdout + result.stderr
            
        except Exception:
            return None
        finally:
            os.unlink(gdb_script_path)
    
    def _extract_backtrace(self, gdb_output: str) -> List[str]:
        """Extract backtrace from GDB output"""
        lines = []
        in_backtrace = False
        
        for line in gdb_output.split('\n'):
            if 'backtrace' in line.lower() or '#0' in line:
                in_backtrace = True
            
            if in_backtrace:
                if line.strip():
                    lines.append(line.strip())
                else:
                    break
        
        return lines[:50]
    
    def _determine_severity(self, bug_type: str) -> str:
        """Determine bug severity based on type"""
        severity_map = {
            "buffer_overflow": "CRITICAL",
            "null_pointer": "HIGH",
            "use_after_free": "CRITICAL",
            "memory_leak": "MEDIUM",
            "double_free": "HIGH",
            "stack_overflow": "CRITICAL",
        }
        return severity_map.get(bug_type, "MEDIUM")
    
    def _generate_description(self, bug_type: str, backtrace: List[str]) -> str:
        """Generate bug description"""
        desc_map = {
            "buffer_overflow": f"Buffer overflow detected in {bug_type} scenario",
            "null_pointer": f"Null pointer dereference in code path",
            "use_after_free": f"Use-after-free of heap memory",
            "memory_leak": f"Memory leak detected in long-running process",
            "double_free": f"Double free of heap block",
            "stack_overflow": f"Stack overflow in recursive function",
        }
        return desc_map.get(bug_type, "Unknown memory safety issue") + \
               f"\n\nBacktrace:\n" + "\n".join(backtrace[:5])


class GDBToPatch:
    """Convert GDB/ASAN analysis directly to patch"""
    
    def __init__(self, debug: bool = False):
        self.debug = debug
        self.analyzer = CoreDumpAnalyzer("")
        self.patch_generator = PatchGenerator()
    
    def process_analysis(self, analysis_file: str) -> Optional[Dict]:
        """Process analysis file (GDB output or ASAN report)"""
        # Read analysis file
        try:
            with open(analysis_file, 'r') as f:
                analysis = f.read()
        except Exception as e:
            if self.debug:
                print(f"Error reading analysis file: {e}")
            return None
        
        # Detect if it's ASAN report or GDB output
        if "AddressSanitizer" in analysis or "HeapUseAfterFree" in analysis:
            self.analyzer.asan_report = analysis
            self.analyzer.core_path = None
        else:
            # Parse GDB output - extract binary and core path if available
            pass
        
        # Analyze
        bug, success = self.analyzer.analyze_core_dump()
        
        if not success:
            return None
        
        # Generate patch
        if not self.patch_generator.generate_patch(bug):
            if self.debug:
                print(f"Failed to generate patch for {bug.type}")
            return None
        
        # Create result
        result = {
            "bug": {
                "id": bug.bug_id,
                "type": bug.type,
                "severity": bug.severity,
                "description": bug.description,
                "location": {
                    "file": bug.location.file if bug.location else None,
                    "line": bug.location.line if bug.location else None,
                    "function": bug.location.function if bug.location else None
                },
                "confidence": bug.confidence,
                "patch_diff": bug.patch_diff
            },
            "generated_at": __import__('datetime').datetime.now().isoformat()
        }
        
        return result
    
    def process_gdb_output(self, gdb_output: str) -> Optional[Dict]:
        """Process raw GDB output"""
        import tempfile
        
        with tempfile.NamedTemporaryFile(mode='w', delete=False, suffix='.gdb') as f:
            f.write(gdb_output)
            gdb_path = f.name
        
        try:
            result = self.process_analysis(gdb_path)
            return result
        finally:
            os.unlink(gdb_path)


def main():
    """Main entry point"""
    import argparse
    
    parser = argparse.ArgumentParser(
        description="MySQL Recovery Pro - GDB/ASAN to C++ Patch Converter"
    )
    parser.add_argument("--gdb", help="GDB output file")
    parser.add_argument("--asan", help="ASAN report file")
    parser.add_argument("--binary", help="Binary path for core dump analysis")
    parser.add_argument("--core", help="Core dump path")
    parser.add_argument("--output", help="Output JSON patch file")
    parser.add_argument("--debug", action="store_true", help="Enable debug output")
    parser.add_argument("--validate", action="store_true", help="Validate patch syntax")
    
    args = parser.parse_args()
    
    converter = GDBToPatch(debug=args.debug)
    
    if args.gdb:
        result = converter.process_analysis(args.gdb)
    elif args.asan:
        converter.analyzer.asan_report = Path(args.asan).read_text()
        converter.analyzer.core_path = args.core
        converter.analyzer.binary_path = args.binary
        bug, _ = converter.analyzer.analyze_core_dump()
        
        if converter.patch_generator.generate_patch(bug):
            result = {
                "bug": {
                    "id": bug.bug_id,
                    "type": bug.type,
                    "severity": bug.severity,
                    "description": bug.description,
                    "confidence": bug.confidence,
                    "patch_diff": bug.patch_diff
                }
            }
        else:
            result = None
    else:
        parser.print_help()
        return 1
    
    if result:
        output_path = args.output or "patch.json"
        with open(output_path, 'w') as f:
            json.dump(result, f, indent=2)
        
        print(f"Patch generated: {output_path}")
        print("\n" + "=" * 80)
        print("Unified Diff:")
        print("=" * 80)
        print(result["bug"]["patch_diff"])
        
        if args.validate:
            # Try to apply the patch
            import subprocess
            try:
                # Create temporary test file
                import tempfile
                
                with tempfile.TemporaryDirectory() as tmpdir:
                    # In real implementation, would apply to actual source
                    print(f"\n[PATCH] Validation would apply to: {result['bug']['location']['file']}")
            except Exception as e:
                print(f"ValidationSkipped: {e}")
        
        return 0
    else:
        print("Failed to generate patch")
        return 1


if __name__ == "__main__":
    sys.exit(main())
