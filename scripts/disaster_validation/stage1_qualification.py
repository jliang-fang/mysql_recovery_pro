#!/usr/bin/env python3
"""Stage 1: Real-media qualification test on /dev/nvme0n2 with structured schema-aware verification.

Unlike the simple smoke test, this driver:
1. Provisions 100,000 deterministic rows using the validated fixture generator.
2. Captures full ground truth and extracts the clustered index ID.
3. Generates the parser-domain schema JSON.
4. Drops the table, unmounts, and sets the device to read-only.
5. Invokes mysql_recovery_pro with --schema-file AND --target-index-ids for strict structured recovery.
6. Runs canonical comparisons against ground truth and performs fresh-instance import validation.
"""

from __future__ import annotations

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
    ApprovalToken,
    DeviceIdentity,
    DeviceSafetyError,
    issue_approval_token,
    preflight,
    preflight_controlled_mount,
    read_device_identity,
    validate_approval_token,
    write_approval_token,
)
from scripts.disaster_validation.lifecycle import RunLifecycleError, ValidationRun
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


class Stage1QualificationRunner:
    def __init__(self, run_id: str, evidence_root: Path, mount_point: Path,
                 rows: int = 100000, page_size: int = 16384):
        self.run_id = run_id
        self.evidence_root = Path(evidence_root).resolve()
        self.mount_point = Path(mount_point).resolve()
        self.device_path = ALLOWED_DEVICE
        self.rows = rows
        self.page_size = page_size
        self.matrix = matrix_item("8.0", "ext4", "drop", "100k", page_size, "DYNAMIC")
        self.run: ValidationRun | None = None
        self.container_name = f"dbrec-stage1-{run_id}"
        self.network_name = f"dbrec-stage1-net-{run_id}"
        self.database = "stage1_db"
        self.recovery_binary = Path("/app/mysql_recovery_pro/build/mysql_recovery_pro")

    @property
    def root(self) -> Path:
        assert self.run is not None
        return self.run.root

    def _authorize_and_execute(self, action: str, argv: list[str],
                               controlled_mount: bool = False, timeout: int = 180) -> None:
        if controlled_mount:
            report = preflight_controlled_mount(self.device_path, self.evidence_root, self.mount_point)
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
        print(f"=== Starting Stage 1 Qualification Run on {self.device_path} (Run ID: {self.run_id}) ===")
        self.run = ValidationRun.create(self.evidence_root, self.run_id, self.matrix)

        # 0. Ensure device is RW for initialization
        print("[SETUP] Setting device RW...")
        run(["blockdev", "--setrw", self.device_path], timeout=30)

        # 1. Format ext4
        init_preflight = preflight(self.device_path, self.evidence_root)
        (self.root / "environment" / "preflight_initial.json").write_text(
            canonical_json(init_preflight) + "\n", encoding="utf-8")
        self.run.transition("PRECHECKED", {"device": self.device_path})

        self._authorize_and_execute("format", ["mkfs.ext4", "-F", "-q", self.device_path], timeout=180)
        subprocess.run(["udevadm", "settle"], check=False)

        # 2. Mount
        self.mount_point.mkdir(parents=True, exist_ok=True)
        self._authorize_and_execute("mount", ["mount", "-t", "ext4", "-o", "rw", self.device_path, str(self.mount_point)], timeout=60)
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

        run_argv = [
            "podman", "run", "-d", "--name", self.container_name, "--network", self.network_name,
            "-v", f"{data_dir}:/var/lib/mysql:Z", "-v", f"{socket_dir}:/var/run/mysqld:Z",
            "-e", "MYSQL_ALLOW_EMPTY_PASSWORD=yes", CAPABILITIES["8.0"].image,
            "--innodb-file-per-table=1", f"--innodb-page-size={self.page_size}",
            "--innodb_fast_shutdown=0", "--innodb_flush_log_at_trx_commit=1", "--sync_binlog=1",
        ]
        res = run(run_argv, timeout=180)
        if not res.ok:
            raise RuntimeError(f"Failed to start MySQL container: {res.stderr}")

        client = MysqlClient(self.container_name, database="")
        print("[SETUP] Waiting for MySQL server readiness...")
        client.wait_ready(timeout=240)

        # 4. Create and populate table using deterministic fixtures
        print(f"[DATASET] Creating database and inserting {self.rows} deterministic rows...")
        client.execute(f"CREATE DATABASE IF NOT EXISTS `{self.database}`;")
        client.database = self.database
        spec = fx.build_suite("numeric", "8.0", row_format="DYNAMIC")
        client.execute(spec.ddl(self.database))

        batch_size = 500
        for first, last in fx.row_count_batches(self.rows, batch_size=batch_size):
            client.execute_batch(spec.insert_sql(self.database, first, last), timeout=1800)

        actual_count = client.row_count(self.database, spec.name)
        print(f"[DATASET] Successfully inserted and verified {actual_count} rows.")
        if actual_count != self.rows:
            raise RuntimeError(f"Expected {self.rows} rows, got {actual_count}")
        self.run.transition("DATASET_CREATED", {"table": spec.name, "rows": actual_count})

        # 5. Capture ground truth and discover index ID
        print("[TRUTH] Capturing canonical ground truth and schema...")
        from scripts.disaster_harness.native_export import export_table
        sock_file = socket_dir / "mysqld.sock" if (socket_dir / "mysqld.sock").exists() else socket_dir / "mysql.sock"
        truth_path = self.root / "truth" / "ground_truth.json"
        truth_doc = export_table(str(sock_file), self.database, spec.name, truth_path, "root", "", "8.0")
        self.run.transition("TRUTH_SEALED", {"truth_sha256": truth_doc.get("dataset_sha256")})

        schema_file = self.root / "persistent-input" / "schema.json"
        fx.write_schema_file(spec, schema_file)

        # 6. Force persistence and discover clustered index id from disk
        print("[PERSIST] Forcing tablespace persistence barriers...")
        client.execute("SET GLOBAL innodb_flush_log_at_trx_commit=1;")
        client.execute("FLUSH TABLES;")
        client.execute(f"FLUSH TABLES `{self.database}`.`{spec.name}` FOR EXPORT;")
        ibd_on_disk = data_dir / self.database / f"{spec.name}.ibd"
        shutil.copy2(ibd_on_disk, self.root / "persistent-input" / f"{spec.name}.ibd")
        client.execute("UNLOCK TABLES;")
        run(["podman", "exec", self.container_name, "sync"], timeout=60)
        run(["sync"], timeout=60)

        import struct
        index_counts: dict[int, int] = {}
        with open(self.root / "persistent-input" / f"{spec.name}.ibd", "rb") as handle:
            total_pages = handle.seek(0, 2) // self.page_size
            for p in range(total_pages):
                handle.seek(p * self.page_size + 24)
                if struct.unpack(">H", handle.read(2))[0] == 0x45BF:
                    handle.seek(p * self.page_size + 66)
                    iid = struct.unpack(">Q", handle.read(8))[0]
                    index_counts[iid] = index_counts.get(iid, 0) + 1
        clustered_index_id = max(index_counts.items(), key=lambda kv: kv[1])[0]
        print(f"[PERSIST] Discovered clustered index ID: {clustered_index_id}")
        self.run.transition("PERSISTED", {"clustered_index_id": clustered_index_id})

        # 7. Apply disaster: DROP TABLE
        print("[DISASTER] Executing DROP TABLE...")
        drop_sql = f"DROP TABLE `{self.database}`.`{spec.name}`;"
        self._authorize_and_execute("drop", ["podman", "exec", self.container_name, "mysql", "-uroot", "-e", drop_sql], controlled_mount=True, timeout=60)
        self.run.transition("SCENARIO_APPLIED", {"scenario": "drop"})

        # 8. Stop MySQL, unmount, and set physical device READ-ONLY
        print("[TEARDOWN] Stopping container and unmounting device...")
        run(["podman", "stop", "-t", "60", self.container_name], timeout=90)
        run(["podman", "rm", "-f", self.container_name], timeout=60)
        run(["podman", "network", "rm", "-f", self.network_name], timeout=60)
        self._authorize_and_execute("unmount", ["umount", str(self.mount_point)], timeout=60)

        print("[LOCK] Setting /dev/nvme0n2 kernel READ-ONLY mode...")
        run(["blockdev", "--setro", self.device_path], timeout=30)
        ro_check = run(["blockdev", "--getro", self.device_path], timeout=30).stdout.strip()
        if ro_check != "1":
            raise RuntimeError("Failed to set /dev/nvme0n2 to read-only mode!")

        # 9. Execute schema-aware raw recovery on /dev/nvme0n2
        print(f"[RECOVERY] Running schema-aware disk carving on {self.device_path}...")
        rec_out = self.root / "recovery-output" / "drop"
        rec_out.mkdir(parents=True, exist_ok=True)
        rec_json = rec_out / "result.json"

        from scripts.disaster_validation.phase_a_offline_orchestrator import build_schema_argument
        schema_arg = build_schema_argument(truth_doc["schema"])

        carve_argv = [
            str(self.recovery_binary), "--disk-carving",
            "--device-path", self.device_path,
            "--filesystem", "ext4",
            "--page-size", str(self.page_size),
            "--schema", schema_arg,
            "--target-index-ids", str(clustered_index_id),
            "--output", str(rec_out),
            "--result-json", str(rec_json),
        ]
        carve_res = run(carve_argv, timeout=3600)
        (self.root / "logs" / "carve_stdout.log").write_text(carve_res.stdout, encoding="utf-8")
        (self.root / "logs" / "carve_stderr.log").write_text(carve_res.stderr, encoding="utf-8")
        self.run.transition("RECOVERY_ATTEMPTED", {"returncode": carve_res.returncode})

        # 10. Verify recovered artifacts and compare strictly against truth
        print("[VERIFY] Verifying recovered result contract and canonical data...")
        contract = json.loads(rec_json.read_text(encoding="utf-8"))
        print(f"[VERIFY] Result contract: classification={contract.get('classification')}, structured_rows={contract.get('structured_rows')}, export_success={contract.get('export_success')}")

        csv_candidates = sorted(rec_out.glob("*.csv"))
        if not csv_candidates:
            raise RuntimeError("No recovered CSV produced!")

        from scripts.disaster_validation.csv_to_canonical import csv_to_canonical_dataset
        recovered_dataset = csv_to_canonical_dataset(
            csv_path=csv_candidates[0], database=self.database, table=spec.name,
            schema=truth_doc["schema"], mysql_version="8.0"
        )
        comp_res = compare(truth_doc, recovered_dataset, f"{self.run_id}_raw_drop")
        (self.root / "reports" / "comparison.json").write_text(json.dumps(comp_res, indent=2), encoding="utf-8")
        print(f"[VERIFY] Strict comparison: status={comp_res.get('status')}, correct={comp_res.get('correct_rows')}/{comp_res.get('expected_rows')}, strict_pass={comp_res.get('strict_pass')}")

        # 11. Fresh import verification
        print("[IMPORT] Running clean-instance import verification...")
        from scripts.disaster_validation.fresh_import_verify import verify_case
        import_res = verify_case(self.root, "8.0")
        (self.root / "reports" / "fresh_import_result.json").write_text(json.dumps(import_res, indent=2), encoding="utf-8")
        print(f"[IMPORT] Fresh import status: {import_res.get('status')}")

        overall_pass = comp_res.get("strict_pass") and (import_res.get("status") == "PASS")
        final_verdict = "PASS" if overall_pass else "FAIL"

        if overall_pass:
            self.run.transition("IMPORT_VALIDATED", {"status": "PASS"})
        self.run.transition("REPORTED", {"verdict": final_verdict})
        seal = self.run.seal({"verdict": final_verdict, "comparison": comp_res, "import": import_res})

        print(f"=== Stage 1 Qualification Complete! Final Verdict: {final_verdict} (Seal: {seal}) ===")
        return {
            "run_id": self.run_id,
            "verdict": final_verdict,
            "comparison": comp_res,
            "fresh_import": import_res,
            "contract": contract,
            "seal": seal,
        }


if __name__ == "__main__":
    runner = Stage1QualificationRunner(
        run_id=f"stage1-nvme80-100k-{int(time.time())}",
        evidence_root=Path("/app/mysql_recovery_artifacts/raw_nvme_qualification"),
        mount_point=Path("/mnt/dbrec-nvme-stage1"),
        rows=100000,
    )
    result = runner.execute()
    print(json.dumps(result, indent=2))
