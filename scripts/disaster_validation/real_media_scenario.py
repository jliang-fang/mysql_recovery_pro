#!/usr/bin/env python3
"""Phase 4: Generalized Real-Media Lifecycle Driver on /dev/nvme0n2.

Supports:
- Scenarios: baseline, drop, truncate, rm-rf
- Filesystems: ext4, xfs
- MySQL Versions: 5.6, 5.7, 8.0, 8.4, 9.0
- Deterministic multi-table fixtures with schema-file and target-index-ids
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import secrets
import shutil
import subprocess
import sys
import time
from dataclasses import asdict
from pathlib import Path
from typing import Any

sys.path.insert(0, "/app/mysql_recovery_pro")

from scripts.disaster_harness.comparator import compare
from scripts.disaster_harness.manifest import canonical_json, sha256_file
from scripts.disaster_validation import fixtures as fx
from scripts.disaster_validation.device_safety import (
    ALLOWED_DEVICE,
    issue_approval_token,
    preflight,
    preflight_controlled_mount,
    read_device_identity,
    validate_approval_token,
    write_approval_token,
)
from scripts.disaster_validation.lifecycle import ValidationRun
from scripts.disaster_validation.matrix import CAPABILITIES, matrix_item
from scripts.disaster_validation.mysql_driver import MysqlClient, run


def _write_new_json(path: Path, document: dict[str, Any]) -> None:
    path = Path(path)
    if path.exists():
        raise RuntimeError(f"refusing to overwrite evidence: {path}")
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    with os.fdopen(os.open(path, flags, 0o600), "w", encoding="utf-8") as stream:
        stream.write(canonical_json(document) + "\n")


class RealMediaScenarioRunner:
    def __init__(self, run_id: str, evidence_root: Path, mount_point: Path,
                 version: str = "8.0", filesystem: str = "ext4", scenario: str = "truncate",
                 rows: int = 100000, page_size: int = 16384,
                 suite_names: tuple[str, ...] = ("numeric", "temporal", "text", "structured", "composite")):
        self.run_id = run_id
        self.evidence_root = Path(evidence_root).resolve()
        self.mount_point = Path(mount_point).resolve()
        self.device_path = ALLOWED_DEVICE
        self.version = version
        self.filesystem = filesystem.lower()
        self.scenario = scenario.lower()
        self.rows = rows
        self.page_size = page_size
        self.suite_names = suite_names

        # 5.6 row_format / json adjustments
        row_format = "DYNAMIC" if version != "5.6" else "COMPACT"
        if version == "5.6":
            # 5.6 does not support JSON column
            self.suite_names = tuple(s for s in self.suite_names if s != "structured")

        self.matrix = matrix_item(version, filesystem, scenario, f"{rows//1000}k", page_size, row_format)
        self.run: ValidationRun | None = None
        self.container_name = f"dbrec-real-{run_id}"
        self.network_name = f"dbrec-real-net-{run_id}"
        self.database = "stage_db"
        self.recovery_binary = Path("/app/mysql_recovery_pro/build/mysql_recovery_pro")

    @property
    def root(self) -> Path:
        assert self.run is not None
        return self.run.root

    def _authorize_and_execute(self, action: str, argv: list[str],
                               controlled_mount: bool = False, timeout: int = 180) -> None:
        if controlled_mount:
            report = preflight_controlled_mount(self.device_path, self.evidence_root, self.mount_point,
                                                allowed_filesystems=(self.filesystem,))
        else:
            report = preflight(self.device_path, self.evidence_root)
        token = issue_approval_token(report, self.run_id, action, nonce=secrets.token_hex(16))
        token_path = write_approval_token(self.root, token)

        current_identity = read_device_identity(self.device_path)
        validate_approval_token(token, current_identity, self.run_id, action, report["preflight_sha256"])

        ledger = self.root / "approval_tokens" / f"consumed-{token.nonce}.json"
        _write_new_json(ledger, {
            "token": asdict(token),
            "token_path": str(token_path.relative_to(self.root)),
            "argv": argv,
            "action": action,
            "time": time.time(),
        })

        print(f"[ACTION] Executing {action}: {' '.join(argv)}")
        cmd_res = run(argv, timeout=timeout)
        if not cmd_res.ok:
            raise RuntimeError(f"Action {action} failed: {cmd_res.stderr}\nSTDOUT: {cmd_res.stdout}")

    def execute(self) -> dict[str, Any]:
        print(f"=== Starting Real Media Run ({self.version}/{self.filesystem}/{self.scenario}) on {self.device_path} (Run ID: {self.run_id}) ===")
        self.run = ValidationRun.create(self.evidence_root, self.run_id, self.matrix)

        ro_audit: list[dict[str, Any]] = []

        def record_ro(label: str) -> str:
            result = run(["blockdev", "--getro", self.device_path], timeout=30)
            value = result.stdout.strip()
            ro_audit.append({
                "label": label,
                "checked_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "returncode": result.returncode,
                "value": value,
            })
            if result.returncode != 0 or value not in {"0", "1"}:
                raise RuntimeError(f"Unable to verify device read-only state at {label}: {result.stderr}")
            return value

        # 0. Set device RW
        print("[SETUP] Setting device RW...")
        record_ro("before_setrw")
        self._authorize_and_execute("setrw", ["blockdev", "--setrw", self.device_path], timeout=30)
        if record_ro("after_setrw") != "0":
            raise RuntimeError("Device did not become read-write before initialization")

        # 1. Format filesystem
        init_preflight = preflight(self.device_path, self.evidence_root)
        device_identity = init_preflight["device"]
        preflight_sha256 = init_preflight["preflight_sha256"]
        (self.root / "environment" / "preflight_initial.json").write_text(
            canonical_json(init_preflight) + "\n", encoding="utf-8")
        self.run.transition("PRECHECKED", {"device": self.device_path})

        if self.filesystem == "ext4":
            self._authorize_and_execute("format", ["mkfs.ext4", "-F", "-q", self.device_path], timeout=180)
        elif self.filesystem == "xfs":
            # Wipe existing signatures first to avoid "Device or resource busy"
            self._authorize_and_execute("wipefs", ["wipefs", "-a", self.device_path], timeout=60)
            subprocess.run(["udevadm", "settle"], check=False)
            time.sleep(1)
            self._authorize_and_execute("format", ["mkfs.xfs", "-f", "-q", self.device_path], timeout=180)
        else:
            raise ValueError(f"Unsupported filesystem: {self.filesystem}")
        subprocess.run(["udevadm", "settle"], check=False)

        # 2. Mount
        self.mount_point.mkdir(parents=True, exist_ok=True)
        self._authorize_and_execute("mount", ["mount", "-t", self.filesystem, "-o", "rw", self.device_path, str(self.mount_point)], timeout=60)
        self.run.transition("MEDIA_PREPARED", {"mount_point": str(self.mount_point)})

        # 3. Start MySQL container
        data_dir = self.mount_point / "mysql-data"
        socket_dir = Path(f"/tmp/dbrec-sock-{self.run_id}")
        data_dir.mkdir(mode=0o777, exist_ok=True)
        socket_dir.mkdir(mode=0o777, exist_ok=True)
        os.chmod(data_dir, 0o777)
        os.chmod(socket_dir, 0o777)

        run(["podman", "rm", "-f", self.container_name], timeout=60)
        run(["podman", "network", "rm", "-f", self.network_name], timeout=60)
        run(["podman", "network", "create", self.network_name], timeout=60)

        img = CAPABILITIES[self.version].image
        run_argv = [
            "podman", "run", "-d", "--name", self.container_name, "--network", self.network_name,
            "-v", f"{data_dir}:/var/lib/mysql:Z", "-v", f"{socket_dir}:/var/run/mysqld:Z",
            "-e", "MYSQL_ALLOW_EMPTY_PASSWORD=yes", img,
            "--innodb-file-per-table=1", f"--innodb-page-size={self.page_size}",
            "--innodb_fast_shutdown=0", "--innodb_flush_log_at_trx_commit=1", "--sync_binlog=1",
        ]
        if self.version.startswith("5."):
            run_argv += [
                "--innodb-file-format=Barracuda", "--innodb-file-format-max=Barracuda",
                "--innodb-large-prefix=ON", "--explicit_defaults_for_timestamp=ON",
            ]
        res = run(run_argv, timeout=180)
        if not res.ok:
            raise RuntimeError(f"Failed to start MySQL container: {res.stderr}")

        client = MysqlClient(self.container_name, database="")
        print("[SETUP] Waiting for MySQL server readiness...")
        client.wait_ready(timeout=240)

        # 4. Populate tables
        print(f"[DATASET] Creating {len(self.suite_names)} suites with {self.rows} rows each...")
        client.execute(f"CREATE DATABASE IF NOT EXISTS `{self.database}`;")
        client.database = self.database
        specs = [fx.build_suite(name, self.version, row_format=self.matrix.row_format) for name in self.suite_names]
        for spec in specs:
            client.execute(spec.ddl(self.database))

        batch_size = 500
        for spec in specs:
            for first, last in fx.row_count_batches(self.rows, batch_size=batch_size):
                client.execute_batch(spec.insert_sql(self.database, first, last), timeout=1800)
            actual_count = client.row_count(self.database, spec.name)
            print(f"[DATASET] {spec.name}: inserted and verified {actual_count} rows.")
            if actual_count != self.rows:
                raise RuntimeError(f"Expected {self.rows} rows for {spec.name}, got {actual_count}")
        self.run.transition("DATASET_CREATED", {
            "tables": [spec.name for spec in specs], "rows_per_table": self.rows,
            "total_rows": self.rows * len(specs),
        })

        # 5. Capture ground truth and schema
        print("[TRUTH] Capturing canonical ground truth and per-table schema...")
        from scripts.disaster_harness.native_export import export_table
        sock_file = socket_dir / "mysqld.sock" if (socket_dir / "mysqld.sock").exists() else socket_dir / "mysql.sock"
        table_inputs: dict[str, dict[str, Any]] = {}
        for spec in specs:
            truth_dir = self.root / "truth" / spec.name
            truth_dir.mkdir(parents=True, exist_ok=True)
            truth_path = truth_dir / "ground_truth.json"
            truth_doc = export_table(str(sock_file), self.database, spec.name, truth_path, "root", "", self.version)
            schema_dir = self.root / "persistent-input" / spec.name
            schema_dir.mkdir(parents=True, exist_ok=True)
            schema_file = schema_dir / "schema.json"
            fx.write_schema_file(spec, schema_file)
            table_inputs[spec.name] = {
                "spec": spec, "truth_path": truth_path, "truth": truth_doc,
                "schema_file": schema_file,
            }
        self.run.transition("TRUTH_SEALED", {
            "tables": {name: entry["truth"].get("dataset_sha256") for name, entry in table_inputs.items()},
        })

        # 6. Flush tables and preserve .ibd for index-id extraction
        print("[PERSIST] Forcing tablespace persistence barriers...")
        client.execute("SET GLOBAL innodb_flush_log_at_trx_commit=1;")
        client.execute("FLUSH TABLES;")
        import struct
        for name, entry in table_inputs.items():
            spec = entry["spec"]
            client.execute(f"FLUSH TABLES `{self.database}`.`{spec.name}` FOR EXPORT;")
            ibd_on_disk = data_dir / self.database / f"{spec.name}.ibd"
            ibd_copy = self.root / "persistent-input" / spec.name / f"{spec.name}.ibd"
            shutil.copy2(ibd_on_disk, ibd_copy)
            client.execute("UNLOCK TABLES;")
            index_counts: dict[int, int] = {}
            with open(ibd_copy, "rb") as handle:
                total_pages = handle.seek(0, 2) // self.page_size
                for page in range(total_pages):
                    handle.seek(page * self.page_size + 24)
                    if struct.unpack(">H", handle.read(2))[0] == 0x45BF:
                        handle.seek(page * self.page_size + 66)
                        index_id = struct.unpack(">Q", handle.read(8))[0]
                        index_counts[index_id] = index_counts.get(index_id, 0) + 1
            if not index_counts:
                raise RuntimeError(f"No index pages found for {spec.name}")
            entry["clustered_index_id"] = max(index_counts.items(), key=lambda item: item[1])[0]
            entry["ibd_copy"] = ibd_copy
            print(f"[PERSIST] {spec.name}: clustered index ID {entry['clustered_index_id']}")
        run(["podman", "exec", self.container_name, "sync"], timeout=60)
        run(["sync"], timeout=60)
        self.run.transition("PERSISTED", {
            "clustered_index_ids": {name: entry["clustered_index_id"] for name, entry in table_inputs.items()},
        })

        # 7. Apply scenario
        if self.scenario == "drop":
            print("[DISASTER] Executing DROP TABLE...")
            sql = "DROP TABLE " + ", ".join(f"`{self.database}`.`{entry['spec'].name}`" for entry in table_inputs.values()) + ";"
            self._authorize_and_execute("drop", ["podman", "exec", self.container_name, "mysql", "-uroot", "-e", sql],
                                        controlled_mount=True, timeout=60)
        elif self.scenario == "truncate":
            print("[DISASTER] Executing TRUNCATE TABLE...")
            for entry in table_inputs.values():
                sql = f"TRUNCATE TABLE `{self.database}`.`{entry['spec'].name}`;"
                self._authorize_and_execute("truncate", ["podman", "exec", self.container_name, "mysql", "-uroot", "-e", sql],
                                            controlled_mount=True, timeout=60)
        elif self.scenario == "rm-rf":
            print("[DISASTER] Executing rm -rf on tablespace files...")
            run(["podman", "stop", "-t", "60", self.container_name], timeout=90)
            for entry in table_inputs.values():
                ibd_path = data_dir / self.database / f"{entry['spec'].name}.ibd"
                self._authorize_and_execute("rm-rf", ["rm", "-f", str(ibd_path)], controlled_mount=True, timeout=60)
        elif self.scenario == "baseline":
            print("[SCENARIO] Baseline scenario - no disaster applied.")
        else:
            raise ValueError(f"Unsupported scenario: {self.scenario}")

        self.run.transition("SCENARIO_APPLIED", {"scenario": self.scenario, "tables": sorted(table_inputs)})

        # 8. Unmount and set READ-ONLY
        print("[TEARDOWN] Stopping container and unmounting device...")
        run(["podman", "stop", "-t", "60", self.container_name], timeout=90)
        run(["podman", "rm", "-f", self.container_name], timeout=60)
        run(["podman", "network", "rm", "-f", self.network_name], timeout=60)
        self._authorize_and_execute("unmount", ["umount", str(self.mount_point)],
                                    controlled_mount=True, timeout=60)

        print("[LOCK] Setting /dev/nvme0n2 kernel READ-ONLY mode...")
        ro_result = run(["blockdev", "--setro", self.device_path], timeout=30)
        if not ro_result.ok or record_ro("after_setro") != "1":
            raise RuntimeError("Failed to set /dev/nvme0n2 to read-only mode!")

        # 9. Disk carving recovery
        print(f"[RECOVERY] Running schema-aware disk carving on {self.device_path}...")
        recovery_started = datetime.datetime.now(datetime.timezone.utc).isoformat()
        table_recovery: dict[str, dict[str, Any]] = {}
        for name, entry in table_inputs.items():
            rec_out = self.root / "recovery-output" / name
            rec_out.mkdir(parents=True, exist_ok=True)
            rec_json = rec_out / "result.json"
            schema_file = entry["schema_file"]
            schema_sha256 = sha256_file(schema_file)
            carve_argv = [
                str(self.recovery_binary), "--disk-carving",
                "--device-path", self.device_path,
                "--filesystem", self.filesystem,
                "--page-size", str(self.page_size),
                "--schema-file", str(schema_file),
                "--target-index-ids", str(entry["clustered_index_id"]),
            ]
            # Baseline leaves every tablespace allocated, so the scan must cover
            # allocated blocks rather than only freed ones.
            if self.scenario == "baseline":
                carve_argv.append("--scan-allocated")
            carve_argv += [
                "--output", str(rec_out),
                "--result-json", str(rec_json),
            ]
            carve_res = run(carve_argv, timeout=3600)
            stdout_path = self.root / "logs" / f"carve_{name}_stdout.log"
            stderr_path = self.root / "logs" / f"carve_{name}_stderr.log"
            stdout_path.write_text(carve_res.stdout, encoding="utf-8")
            stderr_path.write_text(carve_res.stderr, encoding="utf-8")
            table_recovery[name] = {
                "argv": carve_argv,
                "returncode": carve_res.returncode,
                "schema": {"path": str(schema_file), "sha256": schema_sha256},
                "result_path": rec_json,
                "result_json_sha256": sha256_file(rec_json) if rec_json.is_file() else "MISSING",
                "stdout_sha256": sha256_file(stdout_path),
                "stderr_sha256": sha256_file(stderr_path),
            }
        recovery_finished = datetime.datetime.now(datetime.timezone.utc).isoformat()
        record_ro("after_recovery")

        invocation = {
            "record_version": 2,
            "binary": {
                "path": str(self.recovery_binary.resolve()),
                "sha256": sha256_file(self.recovery_binary),
            },
            "tables": {
                name: {
                    "argv": result["argv"], "schema": result["schema"],
                    "truth_sha256": sha256_file(entry["truth_path"]),
                    "tablespace_sha256": sha256_file(entry["ibd_copy"]),
                    "clustered_index_id": entry["clustered_index_id"],
                    "returncode": result["returncode"],
                    "result_json_sha256": result["result_json_sha256"],
                    "stdout_sha256": result["stdout_sha256"],
                    "stderr_sha256": result["stderr_sha256"],
                } for name, (entry, result) in
                ((key, (table_inputs[key], table_recovery[key])) for key in table_inputs)
            },
            "device_path": self.device_path,
            "device_identity": device_identity,
            "preflight_sha256": preflight_sha256,
            "recovery_started_at": recovery_started,
            "recovery_finished_at": recovery_finished,
            "read_only_audit": ro_audit,
        }
        _write_new_json(self.root / "reports" / "recovery_invocation.json", invocation)
        self.run.transition("RECOVERY_ATTEMPTED", {
            "returncodes": {name: result["returncode"] for name, result in table_recovery.items()},
            "schema_sha256": {name: result["schema"]["sha256"] for name, result in table_recovery.items()},
        })

        # 10. Strict comparison
        print("[VERIFY] Performing per-table strict canonical comparisons...")
        from scripts.disaster_validation.csv_to_canonical import csv_to_canonical_dataset
        comparisons: dict[str, dict[str, Any]] = {}
        contracts: dict[str, dict[str, Any]] = {}
        for name, entry in table_inputs.items():
            result = table_recovery[name]
            rec_json = result["result_path"]
            if not rec_json.is_file():
                raise RuntimeError(f"{name}: missing recovery result contract")
            contract = json.loads(rec_json.read_text(encoding="utf-8"))
            contracts[name] = contract
            csv_candidates = sorted(rec_json.parent.glob("*.csv"))
            if not csv_candidates:
                raise RuntimeError(f"{name}: no recovered CSV produced")
            recovered_dataset = csv_to_canonical_dataset(
                csv_path=csv_candidates[0], database=self.database, table=entry["spec"].name,
                schema=entry["truth"]["schema"],
                mysql_version=entry["truth"].get("mysql_version") or "")
            comp_res = compare(entry["truth"], recovered_dataset, f"{self.run_id}_{name}_{self.scenario}")
            comparisons[name] = comp_res
            (self.root / "reports" / f"comparison_{name}.json").write_text(
                json.dumps(comp_res, indent=2), encoding="utf-8")
            print(f"[VERIFY] {name}: correct={comp_res.get('correct_rows')}/"
                  f"{comp_res.get('expected_rows')}, strict_pass={comp_res.get('strict_pass')}")

        # 11. Fresh import
        print("[IMPORT] Running clean-instance import verification...")
        from scripts.disaster_validation.fresh_import_verify import verify_case
        import_res = verify_case(self.root, self.version)
        (self.root / "reports" / "fresh_import_result.json").write_text(
            json.dumps(import_res, indent=2), encoding="utf-8")
        print(f"[IMPORT] Fresh import status: {import_res.get('status')}")

        overall_pass = (all(res.get("strict_pass") is True for res in comparisons.values())
                        and import_res.get("status") == "PASS"
                        and all(contract.get("classification") == "structured_recovery"
                                and contract.get("export_success") is True
                                for contract in contracts.values()))
        final_verdict = "PASS" if overall_pass else "FAIL"

        if overall_pass:
            self.run.transition("IMPORT_VALIDATED", {"status": "PASS"})
        self.run.transition("REPORTED", {"verdict": final_verdict})
        seal = self.run.seal({"verdict": final_verdict, "comparisons": comparisons,
                              "import": import_res, "contracts": contracts,
                              "invocation": invocation})

        print(f"=== Real Media Run Complete! Verdict: {final_verdict} (Seal: {seal}) ===")
        return {
            "run_id": self.run_id,
            "verdict": final_verdict,
            "comparisons": comparisons,
            "fresh_import": import_res,
            "contracts": contracts,
            "seal": seal,
        }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Real media scenario runner")
    parser.add_argument("--version", default="8.0")
    parser.add_argument("--filesystem", default="ext4")
    parser.add_argument("--scenario", default="truncate")
    parser.add_argument("--rows", type=int, default=100000)
    args = parser.parse_args()

    runner = RealMediaScenarioRunner(
        run_id=f"real-{args.version.replace('.', '')}-{args.filesystem}-{args.scenario}-{args.rows//1000}k-{int(time.time())}",
        evidence_root=Path("/app/mysql_recovery_artifacts/raw_nvme_qualification"),
        mount_point=Path("/mnt/dbrec-nvme-stage1"),
        version=args.version,
        filesystem=args.filesystem,
        scenario=args.scenario,
        rows=args.rows,
    )
    result = runner.execute()
    print(json.dumps(result, indent=2))
