#!/usr/bin/env python3
"""Phase B: generalized disaster-scenario driver (DROP / TRUNCATE / rm -rf).

Drives real destruction on isolated, disposable media images instead of on a
raw host block device, so every scenario is repeatable and non-destructive to
the host.  Each case owns its own image and its own MySQL data directory; no
state is reused between cases.

Media model
-----------
1. A sparse image file is created under `<evidence_root>/media/`.
2. The image is formatted ext4 or XFS and loop-mounted.
3. MySQL runs in a container with its datadir bound to the mounted image.
4. Data is written and forcibly persisted (flush tables / sync / checkpoint).
5. Ground truth is captured and sealed.
6. The destructive scenario is applied.
7. MySQL is stopped, the image is unmounted.
8. Recovery runs against the **image file** (read-only), never the host disk.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import time
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Iterable, Optional

from scripts.disaster_validation.dataset_values import hex_literal
from scripts.disaster_validation.lifecycle import RunLifecycleError, ValidationRun
from scripts.disaster_validation.matrix import CAPABILITIES, MatrixItem, matrix_item

RECOVERY_BINARY = Path("/app/mysql_recovery_pro/build/mysql_recovery_pro")
SCENARIOS = ("baseline", "drop", "truncate", "rm-rf")
FILESYSTEMS = ("ext4", "xfs")


class ScenarioError(RuntimeError):
    """Raised when a scenario case cannot proceed safely."""


@dataclass(frozen=True)
class CommandResult:
    argv: list
    returncode: Optional[int]
    stdout: str
    stderr: str


def _run(argv, timeout: float = 300, input_text: str = "") -> CommandResult:
    try:
        completed = subprocess.run(list(argv), input=input_text, capture_output=True,
                                   text=True, timeout=timeout, check=False)
        return CommandResult(list(argv), completed.returncode, completed.stdout, completed.stderr)
    except subprocess.TimeoutExpired as error:
        return CommandResult(list(argv), None, error.stdout or "", error.stderr or "")
    except OSError as error:
        return CommandResult(list(argv), None, "", str(error))


def _require(argv, timeout: float = 300, input_text: str = "") -> CommandResult:
    result = _run(argv, timeout, input_text)
    if result.returncode != 0:
        raise ScenarioError(
            f"command failed ({result.returncode}): {' '.join(map(str, argv))}\n"
            f"stdout={result.stdout[-800:]}\nstderr={result.stderr[-800:]}")
    return result


def _mysql(container: str, sql: str, timeout: float = 300) -> CommandResult:
    """Run SQL through podman exec, retrying transient exec/socket hiccups."""
    return _mysql_exec(container, ["mysql", "-uroot"], sql, timeout)


def _mysql_exec(container: str, argv_tail: list, sql: str,
                timeout: float = 300) -> CommandResult:
    """Run one `podman exec` client invocation, retrying transient failures.

    Under sustained write load on a loop-mounted filesystem `podman exec` can
    occasionally fail with "Can't connect to local MySQL server through socket"
    even though mysqld is healthy; the same statement succeeds on retry.
    """
    last = None
    for attempt in range(5):
        last = _run(["podman", "exec", container, *argv_tail, "-e", sql], timeout)
        if last.returncode == 0:
            return last
        if not _is_transient_mysql_error(last.stderr or ""):
            return last
        time.sleep(2 * (attempt + 1))
    return last


_TRANSIENT_MYSQL_ERRORS = (
    "Can't connect to local MySQL server",
    "Lost connection to MySQL server",
    "Server shutdown in progress",
    "system error: 104",
    "Error in result packet",
    "empty reply from server",
)


def _is_transient_mysql_error(stderr: str) -> bool:
    return any(marker in stderr for marker in _TRANSIENT_MYSQL_ERRORS)


def _container_diagnostics(container: str) -> dict:
    """Capture why the server side is unusable, so SETUP_ERROR is actionable."""
    inspect = _run(["podman", "inspect", "-f",
                    "{{.State.Status}} exitcode={{.State.ExitCode}} "
                    "oomkilled={{.State.OOMKilled}}", container], 60)
    log = _run(["podman", "logs", "--tail", "40", container], 60)
    ping = _run(["podman", "exec", container, "mysqladmin", "ping", "-uroot"], 30)
    return {
        "container_state": inspect.stdout.strip() or inspect.stderr.strip(),
        "mysqladmin_ping": ping.stdout.strip() or ping.stderr.strip(),
        "container_log_tail": (log.stdout or log.stderr)[-2000:],
    }


def _mysql_require(container: str, sql: str, timeout: float = 300) -> CommandResult:
    result = _mysql(container, sql, timeout)
    if result.returncode != 0:
        raise ScenarioError(
            f"mysql failed: {sql}\n{result.stderr[-500:]}\n"
            f"container diagnostics: {json.dumps(_container_diagnostics(container))}")
    return result


def _mysql_out(container: str, sql: str, timeout: float = 300) -> str:
    """Run SQL and return raw rows.

    `-N` suppresses the column header, so callers must NOT drop a first line.

    This shares the retrying path with `_mysql`: verification queries run right
    after a write, so a transient socket hiccup must not turn a healthy case
    into a setup failure.
    """
    result = _mysql_exec(container, ["mysql", "-uroot", "-N", "-B"], sql, timeout)
    if result.returncode != 0:
        raise ScenarioError(
            f"mysql failed: {sql}\n{result.stderr[-500:]}\n"
            f"container diagnostics: {json.dumps(_container_diagnostics(container))}")
    return result.stdout


# --------------------------------------------------------------------------
# media
# --------------------------------------------------------------------------

class Media:
    """One disposable filesystem image plus its loop mount."""

    def __init__(self, path: Path, filesystem: str, size_mb: int, mount_point: Path) -> None:
        self.path = path
        self.filesystem = filesystem
        self.size_mb = size_mb
        self.mount_point = mount_point
        self.loop: Optional[str] = None
        self.mounted = False

    def create(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        if self.path.exists():
            raise ScenarioError(f"refusing to reuse an existing image: {self.path}")
        with open(self.path, "wb") as handle:
            handle.truncate(self.size_mb * 1024 * 1024)
        self.loop = _require(["losetup", "-f", "--show", str(self.path)], 60).stdout.strip()
        if not self.loop:
            raise ScenarioError("losetup did not return a device")
        # A recycled loop device can still carry the kernel's read-only flag.
        _run(["blockdev", "--setrw", self.loop], 60)
        read_only = _run(["blockdev", "--getro", self.loop], 60).stdout.strip()
        if read_only == "1":
            raise ScenarioError(f"loop device {self.loop} is still write-protected")
        if self.filesystem == "ext4":
            _require(["mkfs.ext4", "-q", "-F", self.loop], 300)
        elif self.filesystem == "xfs":
            _require(["mkfs.xfs", "-q", "-f", self.loop], 300)
        else:
            raise ScenarioError(f"unsupported filesystem: {self.filesystem}")
        self.mount_point.mkdir(parents=True, exist_ok=True)
        _require(["mount", self.loop, str(self.mount_point)], 120)
        self.mounted = True
        os.chmod(self.mount_point, 0o777)

    def unmount(self) -> None:
        if self.mounted:
            _run(["umount", str(self.mount_point)], 120)
            self.mounted = False
        if self.loop:
            _run(["losetup", "-d", self.loop], 60)
            self.loop = None

    def sha256(self) -> Optional[str]:
        if not self.path.is_file():
            return None
        import hashlib
        digest = hashlib.sha256()
        with open(self.path, "rb") as handle:
            for block in iter(lambda: handle.read(1 << 20), b""):
                digest.update(block)
        return digest.hexdigest()


# --------------------------------------------------------------------------
# case driver
# --------------------------------------------------------------------------

@dataclass(frozen=True)
class ScenarioConfig:
    evidence_root: Path
    run_id: str
    item: MatrixItem
    rows: int = 20000
    image_mb: int = 2048
    recovery_binary: Path = RECOVERY_BINARY
    keep_media: bool = True
    # The in-process comparison proves row/field equality, but the acceptance
    # criteria are import-based: a PASS must survive a clean-server round trip.
    fresh_import: bool = True


class ScenarioDriver:
    def __init__(self, config: ScenarioConfig,
                 runner: Callable[..., CommandResult] = _run) -> None:
        self.config = config
        self.runner = runner
        self.item = config.item
        self.capability = CAPABILITIES[self.item.version]
        self.container = f"phase-b-{config.run_id}"
        self.database = "phase_b_db"
        self.table = "t_disaster"
        self.run: Optional[ValidationRun] = None
        self.media: Optional[Media] = None
        self.record: dict[str, Any] = {}

    # -- helpers ----------------------------------------------------------
    @property
    def run_dir(self) -> Path:
        return self.run.root

    def log(self, name: str, result: CommandResult) -> None:
        path = self.run_dir / "logs" / f"{name}.json"
        if path.exists():
            path = self.run_dir / "logs" / f"{name}-{int(time.time()*1000)}.json"
        path.write_text(json.dumps({
            "argv": result.argv, "returncode": result.returncode,
            "stdout_tail": result.stdout[-4000:], "stderr_tail": result.stderr[-4000:],
        }, indent=1), encoding="utf-8")

    # -- lifecycle --------------------------------------------------------
    def execute(self) -> dict[str, Any]:
        self.run = ValidationRun.create(self.config.evidence_root, self.config.run_id, self.item)
        self.run.transition("PRECHECKED", {
            "version": self.item.version,
            "image": self.capability.image,
            "scenario": self.item.scenario,
            "filesystem": self.item.filesystem,
            "rows": self.config.rows,
        })

        try:
            self._start_mysql()
            self.run.transition("MEDIA_PREPARED", {"image": str(self.media.path),
                                                   "filesystem": self.item.filesystem,
                                                   "mount_point": str(self.media.mount_point)})
            self._populate()
            self.run.transition("DATASET_CREATED", {"row_count": self.config.rows,
                                                    "database": self.database,
                                                    "table": self.table})
            truth = self._seal_truth()
            self.run.transition("TRUTH_SEALED", {"row_count": truth.get("row_count"),
                                                 "dataset_sha256": truth.get("dataset_sha256")})
            self._persist()
            self._snapshot_before()
            self.run.transition("PERSISTED", self.record.get("before", {}))

            self._apply_scenario()
            self.run.transition("SCENARIO_APPLIED", {"scenario": self.item.scenario})
            self._stop_mysql()

            recovery = self._recover()
            self.run.transition("RECOVERY_ATTEMPTED", {
                "returncodes": {k: v.get("returncode")
                                for k, v in (recovery.get("runs") or {}).items()}})

            verification = self._verify(recovery)
            blockers = verification.get("blockers", [])
            import_interrupted = None
            if not blockers and self.config.fresh_import:
                fresh = self._fresh_import()
                verification["fresh_import"] = fresh
                if fresh.get("status") == "SETUP_ERROR":
                    # The round trip was cut off by the environment; that is an
                    # interruption to record, not a verdict on the recovered rows.
                    import_interrupted = (fresh.get("reason") or
                                          "clean-instance import was interrupted")
                elif fresh.get("status") != "PASS":
                    blockers.append("fresh_import_failed")
            elif self.config.fresh_import:
                # Preserve why the round trip was skipped rather than silently
                # reporting no import evidence.
                verification["fresh_import"] = {
                    "status": "SKIPPED", "reason": "strict comparison already failed"}

            self.record.update({
                "scenario": self.item.scenario,
                "filesystem": self.item.filesystem,
                "version": self.item.version,
                "image": self.capability.image,
                "rows_written": truth.get("row_count"),
                "media": self.record.get("before", {}),
                "media_after": self.record.get("after", {}),
                "recovery": recovery,
                "verification": verification,
                "status": ("FAIL" if blockers
                           else ("IMPORT_INTERRUPTED" if import_interrupted else "PASS")),
                "import_interrupted": import_interrupted,
            })
            self.run.transition("REPORTED", {"blockers": blockers})
        except Exception as error:  # noqa: BLE001 - recorded as a failed case
            self.record.update({
                "scenario": self.item.scenario, "status": "FAILED_SETUP",
                "error": f"{type(error).__name__}: {error}",
            })
            try:
                self._stop_mysql()
            except Exception:
                pass
            try:
                self.run.transition("FAILED_SETUP", {"error": self.record["error"]})
            except RunLifecycleError:
                pass
        finally:
            self._teardown_media()

        (self.run_dir / "reports" / "scenario.json").write_text(
            json.dumps(self.record, indent=1, ensure_ascii=False), encoding="utf-8")
        if self.run.state == "REPORTED":
            self.run.seal({"scenario": self.record})
        return self.record

    def _fresh_import(self) -> dict:
        """Re-import every CSV artifact on a clean server of this MySQL version.

        A failure inside the round-trip harness is an environment interruption:
        it is reported as such instead of aborting the case before the recovery
        results can be sealed.
        """
        from scripts.disaster_validation.fresh_import_verify import verify_case
        try:
            return verify_case(self.run_dir, self.item.version)
        except Exception as error:  # noqa: BLE001 - classified, not swallowed
            return {"case": self.run_dir.name, "version": self.item.version,
                    "status": "SETUP_ERROR",
                    "reason": f"{type(error).__name__}: {error}"}

    def _teardown_media(self) -> None:
        if self.media is None:
            return
        try:
            self.media.unmount()
        except Exception:
            pass

    # -- mysql ------------------------------------------------------------
    def _start_mysql(self) -> None:
        self.media = Media(self.config.evidence_root / "media" / f"{self.config.run_id}.img",
                           self.item.filesystem, self.config.image_mb,
                           Path(f"/mnt/dbrec-{self.config.run_id}"))
        self.media.create()
        suffix = "xfs" if self.item.filesystem == "xfs" else "ext4"
        _require(["podman", "volume", "create", f"{self.container}-vol"], 60)
        data_dir = self.media.mount_point / "mysql"
        data_dir.mkdir(mode=0o777, exist_ok=True)
        os.chmod(data_dir, 0o777)

        _run(["podman", "rm", "-f", self.container], 60)
        argv = [
            "podman", "run", "-d", "--name", self.container,
            "--volume", f"{data_dir}:/var/lib/mysql:Z",
            "--privileged",
            "-e", "MYSQL_ALLOW_EMPTY_PASSWORD=yes",
            self.capability.image,
            "--innodb-file-per-table=1",
            f"--innodb-page-size={self.item.page_size}",
            "--innodb_fast_shutdown=0",
            "--innodb_flush_log_at_trx_commit=1",
            "--sync_binlog=1",
            # MySQL 5.6 defaults to Antelope, which downgrades DYNAMIC/COMPRESSED
            # to COMPACT without an error; pin Barracuda to test what is asked.
            *(["--innodb-file-format=Barracuda",
               "--innodb-file-format-max=Barracuda",
               "--innodb-large-prefix=ON"]
              if self.item.version.startswith("5.") else []),
        ]
        result = self.runner(argv, 300, "")
        self.log("mysql_start", result)
        if result.returncode != 0:
            raise ScenarioError(f"container start failed: {result.stderr[-400:]}")

        for _ in range(120):
            if _run(["podman", "exec", self.container, "mysqladmin", "ping", "-uroot"], 15).returncode == 0:
                break
            time.sleep(1)
        else:
            raise ScenarioError("mysql never became ready")
        self.record["mount_fstype"] = suffix

    def _populate(self) -> None:
        _mysql_require(self.container, f"CREATE DATABASE IF NOT EXISTS `{self.database}`;")
        row_fmt = f"ROW_FORMAT={self.item.row_format}"
        if self.item.row_format == "COMPRESSED":
            # KEY_BLOCK_SIZE must stay within innodb_page_size (and keep a 2:1
            # ratio), otherwise MySQL rejects the table outright.
            key_block = max(1, min(8, self.item.page_size // 2048))
            row_fmt += f" KEY_BLOCK_SIZE={key_block}"
        col_json = "`payload` JSON," if self.capability.supports_json else ""
        _mysql_require(self.container,
                       f"CREATE TABLE `{self.database}`.`{self.table}` ("
                       "`id` BIGINT NOT NULL, `signed_value` INT NOT NULL,"
                       "`decimal_value` DECIMAL(12,4) NOT NULL, `nullable_text` VARCHAR(128),"
                       f"{col_json}`created_at` DATETIME NOT NULL, `blob_value` BLOB,"
                       f"PRIMARY KEY (`id`)) ENGINE=InnoDB {row_fmt};")
        # secondary index + a large-ish payload so records spill across pages
        _mysql_require(self.container,
                       f"CREATE INDEX `idx_text` ON `{self.database}`.`{self.table}` (`nullable_text`);")

        batch = 500
        for start in range(1, self.config.rows + 1, batch):
            end = min(start + batch, self.config.rows + 1)
            values = []
            for i in range(start, end):
                txt = f"'sample_text_{i}'" if i % 10 != 0 else "NULL"
                js = f"'{{\"k\": {i}}}'," if self.capability.supports_json else ""
                # Shared literal builder: an odd-length X'...' is rejected by
                # MySQL with ERROR 1064 (the 04x form crossed into 5 digits at
                # id 65536, killing every 100k/1m profile at row 65537).
                values.append(f"({i}, -{i}, {i}.5000, {txt}, {js} '2026-05-01 12:00:00', {hex_literal(i)})")
            self._insert_batch(start, end - 1, ",".join(values))


    def _insert_batch(self, first_id: int, last_id: int, values: str) -> None:
        """Insert one id range, retrying only when the server confirms it landed.

        `podman exec` under sustained writes can drop the client connection after
        the statement has already been applied. Retrying blindly would duplicate
        rows, so each retry first checks how many rows of the range exist.
        """
        sql = (f"INSERT INTO `{self.database}`.`{self.table}` "
               f"VALUES {values};")
        expected = last_id - first_id + 1
        last_error = ""
        for _ in range(6):
            result = _mysql(self.container, sql, 900)
            if result.returncode == 0:
                return
            last_error = result.stderr.strip()
            count = _mysql_out(self.container,
                               f"SELECT COUNT(*) FROM `{self.database}`.`{self.table}` "
                               f"WHERE id BETWEEN {first_id} AND {last_id};", 120).strip()
            if count.isdigit() and int(count) == expected:
                return
            time.sleep(2)
        raise ScenarioError(f"insert batch {first_id}..{last_id} failed: {last_error[-500:]}")

    def _seal_truth(self) -> dict[str, Any]:
        from scripts.disaster_harness.ground_truth import build_ground_truth, write_ground_truth

        cols_sql = (
            "SELECT COLUMN_NAME, ORDINAL_POSITION, COLUMN_TYPE, IS_NULLABLE, "
            "CHARACTER_MAXIMUM_LENGTH, NUMERIC_PRECISION, NUMERIC_SCALE, "
            "CHARACTER_SET_NAME, COLLATION_NAME FROM information_schema.COLUMNS "
            f"WHERE TABLE_SCHEMA='{self.database}' AND TABLE_NAME='{self.table}' "
            "ORDER BY ORDINAL_POSITION;")
        columns = []
        for line in _mysql_out(self.container, cols_sql).splitlines():
            parts = line.split("\t")
            if len(parts) >= 9:
                columns.append({
                    "name": parts[0], "ordinal": int(parts[1]) - 1, "type": parts[2],
                    "nullable": parts[3] == "YES",
                    "length": int(parts[4]) if parts[4] != "NULL" else None,
                    "precision": int(parts[5]) if parts[5] != "NULL" else None,
                    "scale": int(parts[6]) if parts[6] != "NULL" else None,
                    "charset": parts[7] if parts[7] != "NULL" else None,
                    "collation": parts[8] if parts[8] != "NULL" else None})
        pk_sql = ("SELECT COLUMN_NAME FROM information_schema.KEY_COLUMN_USAGE "
                  f"WHERE TABLE_SCHEMA='{self.database}' AND TABLE_NAME='{self.table}' "
                  "AND CONSTRAINT_NAME='PRIMARY' ORDER BY ORDINAL_POSITION;")
        pk = [l.strip() for l in _mysql_out(self.container, pk_sql).splitlines() if l.strip()]
        schema = {"columns": columns, "primary_key": pk}

        binary_types = {"binary", "varbinary", "blob", "tinyblob", "mediumblob", "longblob"}
        from scripts.disaster_validation.csv_to_canonical import _parse_csv_cell
        select = []
        for column in columns:
            base = column["type"].lower().split("(", 1)[0].strip()
            select.append(f"CONCAT('0x', HEX(`{column['name']}`)) AS `{column['name']}`"
                          if base in binary_types else f"`{column['name']}`")
        tsv = _mysql_out(self.container,
                         f"SELECT {', '.join(select)} FROM `{self.database}`.`{self.table}` "
                         f"ORDER BY `id`;", timeout=600)
        rows = []
        for line in tsv.splitlines():
            if not line.strip():
                continue
            parts = line.split("\t")
            row = {}
            for idx, column in enumerate(columns):
                value = parts[idx] if idx < len(parts) else "NULL"
                row[column["name"]] = None if value == "NULL" else _parse_csv_cell(value, column)
            rows.append(row)
        truth = build_ground_truth(self.database, self.table, schema, rows, self.item.version)
        write_ground_truth(self.run_dir / "truth" / "ground_truth.json", truth)
        return truth

    def _persist(self) -> None:
        for sql in ("SET GLOBAL innodb_flush_log_at_trx_commit=1;", "FLUSH TABLES;"):
            _mysql(self.container, sql, 300)
        _mysql_require(self.container,
                       f"FLUSH TABLES `{self.database}`.`{self.table}` FOR EXPORT;", 300)
        _mysql(self.container, "UNLOCK TABLES;", 120)
        _run(["podman", "exec", self.container, "sync"], 120)
        _run(["sync"], 120)

    def _snapshot_before(self) -> None:
        data_dir = self.media.mount_point / "mysql"
        ibd = data_dir / self.database / f"{self.table}.ibd"
        info = {
            "data_dir": str(data_dir),
            "tablespace": str(ibd),
            "tablespace_bytes": ibd.stat().st_size if ibd.is_file() else None,
            "data_dir_bytes": sum(f.stat().st_size for f in data_dir.rglob("*") if f.is_file()),
            "frm_present": (data_dir / self.database / f"{self.table}.frm").is_file(),
            "image_bytes": self.media.path.stat().st_size,
        }
        import hashlib
        if ibd.is_file():
            digest = hashlib.sha256()
            with open(ibd, "rb") as handle:
                for block in iter(lambda: handle.read(1 << 20), b""):
                    digest.update(block)
            info["tablespace_sha256"] = digest.hexdigest()
        self.record["before"] = info
        shutil.copy2(ibd, self.run_dir / "persistent-input" / f"{self.table}.ibd") if ibd.is_file() else None
        frm = data_dir / self.database / f"{self.table}.frm"
        if frm.is_file():
            shutil.copy2(frm, self.run_dir / "persistent-input" / f"{self.table}.frm")

    # -- destruction ------------------------------------------------------
    def _apply_scenario(self) -> None:
        scenario = self.item.scenario
        data_dir = self.media.mount_point / "mysql"
        if scenario == "baseline":
            _mysql_require(self.container, "FLUSH TABLES;")
        elif scenario == "drop":
            _mysql_require(self.container,
                           f"DROP TABLE `{self.database}`.`{self.table}`;")
        elif scenario == "truncate":
            _mysql_require(self.container,
                           f"TRUNCATE TABLE `{self.database}`.`{self.table}`;")
        elif scenario == "rm-rf":
            _run(["podman", "stop", "-t", "120", self.container], 300)
            target = data_dir / self.database
            shutil.rmtree(target)
            self.record["rm_target"] = str(target)
        else:
            raise ScenarioError(f"unsupported scenario: {scenario}")

        after = {}
        ibd = data_dir / self.database / f"{self.table}.ibd"
        after["tablespace_present"] = ibd.is_file()
        after["tablespace_bytes"] = ibd.stat().st_size if ibd.is_file() else None
        after["db_dir_present"] = (data_dir / self.database).is_dir()
        self.record["after"] = after

    def _stop_mysql(self) -> None:
        _run(["podman", "stop", "-t", "120", self.container], 300)
        _run(["podman", "rm", self.container], 120)
        if self.media and self.media.mounted:
            _run(["umount", str(self.media.mount_point)], 120)
            self.media.mounted = False
        if self.media and self.media.loop:
            _run(["losetup", "-d", self.media.loop], 60)
            self.media.loop = None

    # -- recovery ---------------------------------------------------------
    # -- recovery + verification ------------------------------------------------
    def _write_schema_json(self, truth: dict[str, Any]) -> Path:
        """Write the parser-domain schema JSON the DROP path requires."""
        # MySQL server type -> InnoDB parser-domain type id.
        mapping = {
            "tinyint": 2, "smallint": 3, "mediumint": 10, "int": 4,
            "integer": 4, "bigint": 9, "float": 5, "double": 6,
            "decimal": 21, "numeric": 21, "varchar": 16, "char": 29,
            "text": 27, "tinytext": 24, "mediumtext": 25, "longtext": 26,
            "blob": 27, "tinyblob": 24, "mediumblob": 25, "longblob": 26,
            "binary": 29, "varbinary": 16, "date": 11, "time": 20,
            "datetime": 19, "timestamp": 18, "year": 14, "json": 31,
            "enum": 22, "set": 23, "bit": 17, "geometry": 30,
        }
        columns = []
        pk = set(truth["schema"].get("primary_key") or [])
        for column in truth["schema"]["columns"]:
            base = str(column["type"]).lower().split("(", 1)[0].strip()
            type_id = mapping.get(base, 0)
            entry = {
                "name": column["name"],
                "mysql_type_id": type_id,
                "is_nullable": bool(column.get("nullable", True)) and column["name"] not in pk,
                "is_signed": True,
                "column_length": int(column.get("length") or 0) or 255,
                "ordinal": int(column.get("ordinal", 0)),
                "physical_pos": int(column.get("ordinal", 0)),
                # Numeric/time columns carry no charset in MySQL; only the
                # actual character and binary columns may set one.
                "charset_id": 45,
                "collation_id": 45,
            }
            if base in ("blob", "binary", "varbinary", "json", "text",
                        "tinyblob", "mediumblob", "longblob", "tinytext",
                        "mediumtext", "longtext"):
                entry["charset_id"] = entry["collation_id"] = 63
            if base in ("decimal", "numeric"):
                entry["numeric_precision"] = int(column.get("precision") or 0)
                entry["numeric_scale"] = int(column.get("scale") or 0)
            if base in ("datetime", "timestamp", "time"):
                entry["datetime_precision"] = 0
            columns.append(entry)
        path = self.run_dir / "persistent-input" / "schema.json"
        path.write_text(json.dumps(columns, indent=1), encoding="utf-8")
        return path

    def _discover_index_id(self) -> Optional[int]:
        """Return the clustered index id from the preserved pre-disaster .ibd."""
        candidates = sorted((self.run_dir / "persistent-input").glob("*.ibd"))
        if not candidates:
            raise ScenarioError("no preserved tablespace to derive the index id from")
        import struct
        page_size = self.item.page_size
        counts: dict[int, int] = {}
        with open(candidates[0], "rb") as handle:
            total = candidates[0].stat().st_size // page_size
            for page in range(total):
                handle.seek(page * page_size + 24)
                page_type = struct.unpack(">H", handle.read(2))[0]
                if page_type != 0x45BF:
                    continue
                handle.seek(page * page_size + 66)
                index_id = struct.unpack(">Q", handle.read(8))[0]
                counts[index_id] = counts.get(index_id, 0) + 1
        if not counts:
            raise ScenarioError("no clustered index pages in the preserved tablespace")
        # The clustered index owns the row data, so it has the most pages.
        best = max(counts.items(), key=lambda entry: entry[1])[0]
        self.record["index_ids"] = counts
        self.record["clustered_index_id"] = best
        return best

    @staticmethod
    def _parse_carve_csv(path: Path) -> list[Path]:
        return sorted(path.parent.glob("*.csv")) if path.parent.is_dir() else []

    def _run_one(self, label: str, argv: list[str], out_dir: Path) -> dict[str, Any]:
        contract = out_dir / "result.json"
        if contract.exists():
            contract.unlink()
        result = self.runner(argv, 3600, "")
        self.log(f"recovery_{label}", result)
        payload: dict[str, Any] = {
            "argv": argv,
            "returncode": result.returncode,
            "stdout_tail": result.stdout[-4000:],
            "stderr_tail": result.stderr[-4000:],
            "artifacts": sorted(p.name for p in out_dir.iterdir() if p.is_file()),
        }
        from scripts.disaster_validation.phase_a_offline_orchestrator import (
            REQUIRED_CONTRACT_KEYS, _load_result_contract)
        if contract.is_file():
            try:
                document = _load_result_contract(contract)
                payload["contract"] = document
                payload["contract_complete"] = True
            except RuntimeError as error:
                payload["contract_error"] = str(error)
                payload["contract_complete"] = False
        else:
            payload["contract_complete"] = False
            payload["contract_error"] = "result contract missing"
        return payload

    def _recover(self) -> dict[str, Any]:
        if self.media is None:
            raise ScenarioError("media is unavailable")
        image = self.media.path
        truth_path = self.run_dir / "truth" / "ground_truth.json"
        truth = json.loads(truth_path.read_text(encoding="utf-8"))
        from scripts.disaster_validation.phase_a_offline_orchestrator import build_schema_argument
        schema_arg = build_schema_argument(truth["schema"])
        schema_file = self._write_schema_json(truth)
        index_id = self._discover_index_id()

        runs: dict[str, Any] = {}

        # 1. filesystem-aware carving with the sealed schema
        carve_out = self.run_dir / "recovery-output" / self.item.scenario
        carve_out.mkdir(parents=True, exist_ok=True)
        carve_argv = [
            str(self.config.recovery_binary), "--disk-carving",
            "--device-path", str(image),
            "--filesystem", self.item.filesystem,
            "--page-size", str(self.item.page_size),
            "--schema", schema_arg,
            "--target-index-ids", str(index_id),
        ]
        if self.item.scenario == "baseline":
            # The baseline tablespace is still allocated: scan allocated blocks.
            carve_argv.append("--scan-allocated")
        carve_argv += ["--output", str(carve_out),
                       "--result-json", str(carve_out / "result.json")]
        runs["disk_carving"] = self._run_one("disk_carving", carve_argv, carve_out)

        # 2. DROP TABLE dedicated path on the same image
        if self.item.scenario == "drop":
            drop_out = self.run_dir / "recovery-output" / "drop-table"
            drop_out.mkdir(parents=True, exist_ok=True)
            drop_argv = [
                str(self.config.recovery_binary), "--drop-table-recovery",
                "--device-path", str(image),
                "--page-size", str(self.item.page_size),
                "--schema-file", str(schema_file),
                "--target-index-ids", str(index_id),
                "--output", str(drop_out),
                "--result-json", str(drop_out / "result.json"),
            ]
            runs["drop_table"] = self._run_one("drop_table", drop_argv, drop_out)

        return {
            "image": str(image),
            "schema_argument": schema_arg,
            "schema_file": str(schema_file),
            "clustered_index_id": index_id,
            "truth": str(truth_path),
            "runs": runs,
        }

    def _verify(self, recovery: dict[str, Any]) -> dict[str, Any]:
        """Gate every run on its contract and compare structured rows to truth."""
        import sys
        sys.path.insert(0, "/app/mysql_recovery_pro")
        from pathlib import Path as _Path
        from scripts.disaster_validation.csv_to_canonical import csv_to_canonical_dataset
        from scripts.disaster_harness.comparator import compare

        truth = json.loads(Path(recovery["truth"]).read_text(encoding="utf-8"))
        comparison: dict[str, Any] = {}
        blockers: list[str] = []

        for label, payload in recovery.get("runs", {}).items():
            entry: dict[str, Any] = {
                "returncode": payload.get("returncode"),
                "contract_complete": payload.get("contract_complete", False),
                "contract_error": payload.get("contract_error"),
            }
            if not payload.get("contract_complete"):
                entry["status"] = "FAILED_SETUP"
                entry["reason"] = payload.get("contract_error", "missing contract")
                blockers.append(f"{label}_contract_rejected")
                comparison[label] = entry
                continue

            contract = payload["contract"]
            entry.update({
                "classification": contract.get("classification"),
                "structured_rows": contract.get("structured_rows"),
                "evidence_fragments": contract.get("evidence_fragments"),
                "export_success": contract.get("export_success"),
                "importable": contract.get("importable"),
                "recovery_method": contract.get("recovery_method"),
            })

            out_dir = Path(payload["argv"][payload["argv"].index("--output") + 1])
            csvs = sorted(out_dir.glob("*.csv"))
            if contract.get("classification") != "structured_recovery":
                entry["status"] = "EVIDENCE_ONLY"
                entry["reason"] = contract.get("reason")
                comparison[label] = entry
                continue
            if not csvs:
                # A contract that claims rows while producing no artifact is a
                # tool defect, not an environment interruption.
                entry["status"] = "FAIL"
                entry["reason"] = "contract claims structured rows but no CSV exists"
                blockers.append(f"{label}_contract_artifact_mismatch")
                comparison[label] = entry
                continue

            # Undecodable output is a failed comparison, never a setup error:
            # one malformed cell must not abort the whole matrix.
            try:
                recovered = csv_to_canonical_dataset(
                    csv_path=csvs[0], database=truth["database"], table=truth["table"],
                    schema=truth["schema"], mysql_version=self.item.version)
            except Exception as error:
                # A CSV the standard reader cannot parse is a product defect in
                # the artifact, never an environment interruption: it must be
                # scored as a failure with the concrete decoder error.
                entry["status"] = "FAIL"
                entry["reason"] = f"csv_parse_failed:{type(error).__name__}:{error}"
                blockers.append(f"{label}_csv_parse_failed")
                comparison[label] = entry
                continue
            (out_dir / "recovered_typed.json").write_text(
                json.dumps(recovered, sort_keys=True, ensure_ascii=False), encoding="utf-8")
            result = compare(truth, recovered, f"{self.config.run_id}_{label}")
            entry["comparison"] = {
                "expected_rows": result["expected_rows"],
                "actual_rows": result["actual_rows"],
                "correct_rows": result["correct_rows"],
                "field_mismatches": len(result.get("field_mismatches", [])),
                "false_positives": len(result.get("false_positives", [])),
                "false_negatives": len(result.get("false_negatives", [])),
                "status": result["status"],
                "strict_pass": result["strict_pass"],
            }
            if not result.get("strict_pass"):
                blockers.append(f"{label}_strict_compare_failed")
                entry["status"] = "FAIL"
            else:
                entry["status"] = "PASS"
            comparison[label] = entry

        if blockers:
            blockers.append("csv_strict_compare_failed")
        return {"runs": comparison, "blockers": sorted(set(blockers))}


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="Phase B generalized disaster scenarios on isolated media images")
    parser.add_argument("--versions", nargs="+", default=["8.0"])
    parser.add_argument("--filesystems", nargs="+", default=["ext4"])
    parser.add_argument("--scenarios", nargs="+", default=["drop"])
    parser.add_argument("--row-formats", nargs="+", default=["DYNAMIC"])
    parser.add_argument("--page-sizes", nargs="+", type=int, default=[16384])
    parser.add_argument("--rows", type=int, default=20000)
    parser.add_argument("--image-mb", type=int, default=2048)
    parser.add_argument("--evidence-root", type=Path, default=Path("/app/mysql_recovery_artifacts/phase_b"))
    parser.add_argument("--fresh-import", dest="fresh_import", action="store_true",
                        default=True,
                        help="verify every structured case by re-importing it into a "
                             "clean instance (default: on)")
    parser.add_argument("--no-fresh-import", dest="fresh_import", action="store_false",
                        help="skip the clean-instance round trip (debugging only)")
    args = parser.parse_args()

    args.evidence_root.mkdir(parents=True, exist_ok=True)
    (args.evidence_root / "media").mkdir(exist_ok=True)

    results = []
    for version in args.versions:
        for filesystem in args.filesystems:
            for scenario in args.scenarios:
                for row_format in args.row_formats:
                    for page_size in args.page_sizes:
                        item = matrix_item(version, filesystem, scenario, "smoke",
                                           page_size, row_format)
                        if item.status != "PLANNED":
                            print(f"SKIP {version}/{filesystem}/{scenario}/{row_format}: {item.reason}")
                            continue
                        run_id = (f"phase-b-{version.replace('.', '')}-{filesystem}-{scenario}-"
                                  f"{row_format.lower()}-p{page_size}-{int(time.time())}")
                        print(f"\n=== {run_id} ===")
                        driver = ScenarioDriver(ScenarioConfig(
                            evidence_root=args.evidence_root, run_id=run_id, item=item,
                            rows=args.rows, image_mb=args.image_mb,
                            fresh_import=args.fresh_import))
                        try:
                            record = driver.execute()
                        except Exception as error:  # noqa: BLE001
                            record = {"status": "ABORTED", "error": str(error), "run_id": run_id}
                        print(f"    -> {record.get('status')}")
                        results.append({"run_id": run_id, **record})

    summary = args.evidence_root / "phase_b_summary.json"
    summary.write_text(json.dumps(results, indent=1, ensure_ascii=False), encoding="utf-8")
    print(f"\nwrote {summary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())