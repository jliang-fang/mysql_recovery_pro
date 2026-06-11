#!/usr/bin/env python3
"""
Chaos Test Pipeline Configuration
Purpose: Run all stages of the chaos testing pipeline
Author: jliang.fang (Global MySQL Kernel QA Expert)
"""

import argparse
import sys
import os
from pathlib import Path
from typing import List, Dict, Optional
import subprocess


class ChaosPipeline:
    """Manages the complete chaos testing pipeline"""
    
    def __init__(self, project_root: str = ".", verbose: bool = False):
        self.project_root = Path(project_root)
        self.scripts_dir = self.project_root / "scripts"
        self.docker_dir = self.project_root / "docker"
        self.verbose = verbose
        
        # Configuration
        self.versions = ["5.0", "5.5", "5.6", "5.7", "8.0", "8.4", "9.0"]
        self.record_count = 100000
        self.max_retries = 5
        
        # Directories
        self.golden_hashes_dir = self.project_root / "golden-hashes"
        self.chaos_data_dir = self.project_root / "chaos-test-volumes"
        self.recovery_output = self.project_root / "recovery-output"
        self.diagnosis_dir = self.project_root / "diagnosis"
        
        # Ensure directories exist
        for d in [self.golden_hashes_dir, self.chaos_data_dir, 
                  self.recovery_output, self.diagnosis_dir]:
            d.mkdir(parents=True, exist_ok=True)
    
    def run_stage(self, name: str, command: List[str], shell: bool = False) -> bool:
        """Run a pipeline stage"""
        print(f"\n{'='*70}")
        print(f"STAGE: {name}")
        print(f"{'='*70}")
        
        try:
            if self.verbose:
                print(f"Command: {' '.join(command)}")
            
            result = subprocess.run(
                command,
                shell=shell,
                capture_output=not self.verbose,
                text=True,
                timeout=600
            )
            
            if result.returncode == 0:
                print(f"✓ {name} completed successfully")
                return True
            else:
                print(f"✗ {name} failed")
                if self.verbose and result.stderr:
                    print(result.stderr)
                return False
                
        except subprocess.TimeoutExpired:
            print(f"✗ {name} timed out")
            return False
        except Exception as e:
            print(f"✗ {name} error: {e}")
            return False
    
    def stage_setup_docker(self) -> bool:
        """Setup Docker environment"""
        print("\nSKIP Docker setup (run manually for MySQL versions)")
        return True
    
    def stage_data_injection(self, version: str, host: str = "localhost", 
                             port: int = 3306) -> bool:
        """Inject data for a specific version"""
        python = sys.executable
        cmd = [
            python, str(self.scripts_dir / "ultra_data_injector.py"),
            "--host", host,
            "--port", str(port),
            "--user", "root",
            "--password", "root123",
            "--database", "testdb",
            "--version", version,
            "--num-records", str(self.record_count),
            "--vector",
            "--gis",
            "--hash",
            "--hash-output", str(self.golden_hashes_dir)
        ]
        return self.run_stage(f"Data Injection (MySQL {version})", cmd)
    
    def stage_all_injections(self, host: str = "localhost", port: int = 3306) -> bool:
        """Inject data for all versions"""
        success = True
        for version in self.versions:
            if not self.stage_data_injection(version, host, port):
                success = False
        return success
    
    def stage_build_engine(self) -> bool:
        """Build recovery engine with ASAN"""
        os.chdir(str(self.project_root))
        
        build_dir = self.project_root / "build"
        build_dir.mkdir(exist_ok=True)
        
        print("\nBuilding recovery engine with ASAN/UBSAN...")
        
        # CMake configure
        cmake_cmd = [
            "cmake", str(self.project_root),
            "-DCMAKE_BUILD_TYPE=Debug",
            "-DENABLE_ASAN=ON",
            "-DENABLE_UBSAN=ON",
            "-DENABLE_LSAN=ON",
            f"-B{build_dir}"
        ]
        
        if not self.run_stage("CMake Configure", cmake_cmd):
            return False
        
        # Build
        build_cmd = ["cmake", "--build", str(build_dir), "-j4"]
        return self.run_stage("Build Recovery Engine", build_cmd)
    
    def stage_generate_hashes(self, version: str, host: str = "localhost",
                              port: int = 3306) -> bool:
        """Generate golden hashes for a version"""
        python = sys.executable
        cmd = [
            python, str(self.scripts_dir / "golden_hash_base.py"),
            "--host", host,
            "--port", str(port),
            "--user", "root",
            "--password", "root123",
            "--database", "testdb",
            "--version", version,
            "--output-dir", str(self.golden_hashes_dir)
        ]
        return self.run_stage(f"Golden Hash (MySQL {version})", cmd)
    
    def stage_all_hashes(self, host: str = "localhost", port: int = 3306) -> bool:
        """Generate hashes for all versions"""
        success = True
        for version in self.versions:
            if not self.stage_generate_hashes(version, host, port):
                success = False
        return success
    
    def stage_chaos_simulation(self, input_file: str, output_file: str,
                               scenario: str) -> bool:
        """Apply chaos corruption"""
        python = sys.executable
        cmd = [
            python, str(self.scripts_dir / "advanced_chaos_monkey.py"),
            "--input", input_file,
            "--output", output_file,
            "--scenario", scenario,
            "--verbose"
        ]
        return self.run_stage(f"Chaos: {scenario}", cmd)
    
    def stage_self_healing(self, input_file: str, expected_hash: str = None) -> bool:
        """Run self-healing loop"""
        python = sys.executable
        cmd = [
            python, str(self.scripts_dir / "self_healing_loop.py"),
            "--input", input_file,
            "--max-retries", str(self.max_retries),
            "--project-root", str(self.project_root),
            "--build-dir", str(self.project_root / "build"),
            "--output-dir", str(self.recovery_output),
            "--report", str(self.diagnosis_dir / "diagnosis.json")
        ]
        
        if expected_hash:
            cmd.extend(["--expected-hash", expected_hash])
        
        return self.run_stage("Self-Healing Loop", cmd)
    
    def stage_final_report(self) -> bool:
        """Generate final report"""
        python = sys.executable
        cmd = [
            python, str(self.scripts_dir / "generate_final_report.py"),
            "--output-dir", str(self.recovery_output),
            "--golden-hashes", str(self.golden_hashes_dir),
            "--diagnosis-dir", str(self.diagnosis_dir),
            "--output-file", "final_chaos_report.json",
            "--versions", *self.versions,
            "--verbose"
        ]
        return self.run_stage("Final Report Generation", cmd)


def run_full_pipeline(pipeline: ChaosPipeline, quick: bool = False) -> bool:
    """Run the complete chaos testing pipeline"""
    
    print("="*70)
    print("CHaos Testing Pipeline - Full Run")
    print("="*70)
    
    success = True
    
    if not quick:
        # Docker setup (manual for production use)
        print("\nNote: Setup Docker environment manually with:")
        print("  cd docker && docker-compose -f docker-compose.multiversions.yml up -d")
    
    # Stage 1: Inject data for all versions
    print("\n" + "-"*70)
    print("STAGE 1: Data Injection")
    print("-"*70)
    success &= pipeline.stage_all_injections()
    
    # Stage 2: Build recovery engine
    print("\n" + "-"*70)
    print("STAGE 2: Build Recovery Engine")
    print("-"*70)
    success &= pipeline.stage_build_engine()
    
    # Stage 3: Generate golden hashes
    print("\n" + "-"*70)
    print("STAGE 3: Generate Golden Hashes")
    print("-"*70)
    success &= pipeline.stage_all_hashes()
    
    # Stage 4: Chaos simulation (single corruption scenario for demo)
    print("\n" + "-"*70)
    print("STAGE 4: Chaos Simulation")
    print("-"*70)
    
    # Create test .ibd file first (simplified - use existing or generate)
    test_file = pipeline.recovery_output / "test.ibd"
    if not test_file.exists():
        print("Creating test .ibd file...")
        test_file.write_bytes(b"\x00" * 16384)
    
    success &= pipeline.stage_chaos_simulation(
        str(test_file),
        str(test_file).replace(".ibd", "_corrupted.ibd"),
        "torn_page"
    )
    
    # Stage 5: Recovery with self-healing
    print("\n" + "-"*70)
    print("STAGE 5: Recovery & Self-Healing")
    print("-"*70)
    
    # Get corrupted file path
    corrupted_file = test_file.parent / "test_corrupted.ibd"
    if corrupted_file.exists():
        success &= pipeline.stage_self_healing(str(corrupted_file))
    else:
        print("Skipping self-healing (no corrupted file found)")
    
    # Stage 6: Final report
    print("\n" + "-"*70)
    print("STAGE 6: Final Report")
    print("-"*70)
    success &= pipeline.stage_final_report()
    
    return success


def main():
    parser = argparse.ArgumentParser(
        description='Chaos Testing Pipeline - MySQL Recovery Pro'
    )
    parser.add_argument('--quick', action='store_true',
                       help='Skip Docker setup (use local MySQL)')
    parser.add_argument('--project-root', default='.',
                       help='Project root directory')
    parser.add_argument('--verbose', '-v', action='store_true',
                       help='Verbose output')
    parser.add_argument('--stage', choices=[
        'data-injection', 'build', 'chaos', 'healing', 'report', 'all'
    ], default='all', help='Only run specific stage')
    
    args = parser.parse_args()
    
    pipeline = ChaosPipeline(
        project_root=args.project_root,
        verbose=args.verbose
    )
    
    if args.stage == 'all':
        success = run_full_pipeline(pipeline, quick=args.quick)
    elif args.stage == 'data-injection':
        success = pipeline.stage_all_injections()
    elif args.stage == 'build':
        success = pipeline.stage_build_engine()
    elif args.stage == 'chaos':
        success = pipeline.stage_chaos_simulation(
            str(pipeline.recovery_output / "test.ibd"),
            str(pipeline.recovery_output / "corrupted.ibd"),
            "torn_page"
        )
    elif args.stage == 'healing':
        corrupted = pipeline.recovery_output / "corrupted.ibd"
        if corrupted.exists():
            success = pipeline.stage_self_healing(str(corrupted))
        else:
            print("No corrupted file found. Run chaos stage first.")
            success = False
    elif args.stage == 'report':
        success = pipeline.stage_final_report()
    
    if success:
        print("\n" + "="*70)
        print("Pipeline completed successfully!")
        print("="*70)
        return 0
    else:
        print("\n" + "="*70)
        print("Pipeline completed with errors.")
        print("="*70)
        return 1


if __name__ == '__main__':
    sys.exit(main())
