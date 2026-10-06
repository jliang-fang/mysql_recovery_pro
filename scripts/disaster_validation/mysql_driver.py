#!/usr/bin/env python3
"""Shared MySQL/podman execution helpers for the validation harnesses.

Every phase used to carry its own copy of these helpers, so a retry policy or a
diagnostic added in one path silently did not exist in the others.  They live
here now, with the behaviours the phases actually depend on:

* SQL goes in over **stdin**, never as an argv element.  A 500-row batch of the
  wide fixtures is far past ``ARG_MAX``; ``podman exec ... -e "<sql>"`` fails
  with E2BIG on exactly the tables the validation most needs to exercise.
* Read-only statements retry transient socket loss; the caller-visible error is
  the last attempt.
* Non-idempotent statements are never retried blindly — the caller proves what
  landed first, otherwise a dropped client connection duplicates rows.
"""

from __future__ import annotations

import json
import os
import subprocess
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional, Sequence

TRANSIENT_MARKERS = (
    "Can't connect to local MySQL server",
    "Lost connection to MySQL server",
    "Server shutdown in progress",
    "system error: 104",
    "Error in result packet",
    "empty reply from server",
    "MySQL server has gone away",
)


@dataclass
class CommandResult:
    """One completed (or timed-out) command."""

    argv: list
    returncode: Optional[int]
    stdout: str
    stderr: str
    duration_s: float = 0.0

    @property
    def ok(self) -> bool:
        return self.returncode == 0


def run(argv: Sequence[str], timeout: float = 300, input_text: str = "",
        cwd: Optional[Path] = None) -> CommandResult:
    """Run a command, turning every transport failure into a result object."""
    started = time.time()
    try:
        completed = subprocess.run(list(argv), input=input_text, capture_output=True,
                                   text=True, timeout=timeout, check=False,
                                   cwd=str(cwd) if cwd else None)
        return CommandResult(list(argv), completed.returncode, completed.stdout,
                             completed.stderr, time.time() - started)
    except subprocess.TimeoutExpired as error:
        return CommandResult(list(argv), None, _text(error.stdout), _text(error.stderr),
                             time.time() - started)
    except OSError as error:
        return CommandResult(list(argv), None, "", str(error), time.time() - started)


def _text(value) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    return str(value)


def is_transient(stderr: str) -> bool:
    return any(marker in (stderr or "") for marker in TRANSIENT_MARKERS)


def require(argv: Sequence[str], timeout: float = 300, input_text: str = "") -> CommandResult:
    result = run(argv, timeout, input_text)
    if not result.ok:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(map(str, argv))}\n"
            f"stdout={result.stdout[-800:]}\nstderr={result.stderr[-800:]}")
    return result


class MysqlUnavailable(RuntimeError):
    """The server never became usable, so the case is an environment failure."""


class MysqlClient:
    """One container's MySQL client, with retrying reads and safe writes."""

    def __init__(self, container: str, *, database: str = "", binary: str = "mysql",
                 as_root: bool = True, default_timeout: float = 600,
                 charset: str = "utf8mb4") -> None:
        self.container = container
        self.database = database
        self.binary = binary
        self.as_root = as_root
        self.default_timeout = default_timeout
        self.charset = charset

    # -- process plumbing ---------------------------------------------------
    def _client_argv(self, extra: Sequence[str]) -> list:
        """Base `mysql` client invocation.

        ``--default-character-set`` is always explicit.  The client otherwise
        inherits ``latin1`` from the server build, and every byte >= 0x80 is then
        reinterpreted as a latin1 character and re-encoded to utf8mb4 on the way
        into the table — roughly doubling the stored size and turning a
        correctly-sized value into ERROR 1406 "Data too long".  Declaring the
        connection charset keeps byte budgets exact in both directions.
        """
        argv = ["podman", "exec", "-i", self.container, self.binary]
        if self.as_root:
            argv.append("-uroot")
        if self.charset:
            argv.append(f"--default-character-set={self.charset}")
        argv.extend(extra)
        return argv

    def _argv(self, extra: Sequence[str], database: str = "") -> list:
        argv = self._client_argv(extra)
        target = database or self.database
        if target:
            argv.append(target)
        return argv

    def diagnostics(self) -> dict:
        """Why the server side is unusable, so a SETUP_ERROR is actionable."""
        inspect = run(["podman", "inspect", "-f",
                       "{{.State.Status}} exitcode={{.State.ExitCode}} "
                       "oomkilled={{.State.OOMKilled}}", self.container], 60)
        log = run(["podman", "logs", "--tail", "40", self.container], 60)
        ping = run(["podman", "exec", self.container, "mysqladmin", "ping", "-uroot"], 30)
        return {
            "container_state": inspect.stdout.strip() or inspect.stderr.strip(),
            "mysqladmin_ping": ping.stdout.strip() or ping.stderr.strip(),
            "container_log_tail": (log.stdout or log.stderr)[-2000:],
        }

    def _fail(self, sql: str, result: CommandResult) -> RuntimeError:
        preview = sql if len(sql) <= 400 else sql[:400] + " ... [truncated]"
        return RuntimeError(
            f"mysql failed: {preview}\n{result.stderr[-600:]}\n"
            f"container diagnostics: {json.dumps(self.diagnostics())[:1200]}")

    # -- statements ---------------------------------------------------------
    def execute(self, sql: str, timeout: float = 0, retries: int = 4) -> CommandResult:
        """Run one idempotent statement, retrying transient transport loss.

        The statement is always fed over stdin: a 500-row INSERT against the wide
        fixtures is well past ARG_MAX, and ``podman exec ... -e "<sql>"`` fails
        with E2BIG on exactly the tables the validation most needs to exercise.
        """
        timeout = timeout or self.default_timeout
        last: Optional[CommandResult] = None
        for attempt in range(max(1, retries)):
            last = run(self._client_argv([])
                       + ([self.database] if self.database else []),
                       timeout, sql)
            if last.ok:
                return last
            if not is_transient(last.stderr) or attempt == retries - 1:
                break
            time.sleep(2 * (attempt + 1))
        assert last is not None
        raise self._fail(sql, last)

    def execute_batch(self, sql: str, timeout: float = 1800) -> CommandResult:
        """Feed a large SQL stream over stdin (no ARG_MAX limit)."""
        result = run(self._client_argv([])
                     + ([self.database] if self.database else []),
                     timeout, sql)
        if result.returncode != 0 and result.returncode is not None:
            raise self._fail(sql, result)
        if result.returncode is None:
            raise RuntimeError(f"mysql batch timed out after {timeout}s: {sql[:200]}")
        return result

    def query(self, sql: str, timeout: float = 0, retries: int = 4,
              database: str = "") -> str:
        """Run a read-only statement and return raw rows.

        ``-N -B`` suppresses the header, so callers must NOT drop the first line.
        """
        timeout = timeout or self.default_timeout
        last: Optional[CommandResult] = None
        for attempt in range(max(1, retries)):
            last = run(self._argv(["-N", "-B", "-e", sql], database), timeout)
            if last.ok:
                return last.stdout
            if not is_transient(last.stderr) or attempt == retries - 1:
                break
            time.sleep(2 * (attempt + 1))
        assert last is not None
        raise self._fail(sql, last)

    def query_rows(self, sql: str, timeout: float = 0) -> list[list[str]]:
        return [line.split("\t") for line in self.query(sql, timeout).splitlines()
                if line.strip()]

    def scalar(self, sql: str, timeout: float = 0) -> str:
        rows = self.query_rows(sql, timeout)
        return rows[0][0] if rows else ""

    def wait_ready(self, timeout: float = 300, interval: float = 1.0) -> None:
        """Wait until the *real* server is accepting connections.

        The MySQL entrypoint boots a **temporary** server to initialize the data
        directory, then shuts it down and starts the real one.  Both answer
        ``mysqladmin ping``, and ``podman logs`` lags behind reality, so neither
        a ping nor a log marker reliably distinguishes them — the failing
        symptom is an intermittent "socket not found" on the first statement
        after an apparently successful start.

        The two servers do differ in one queryable way: the entrypoint starts
        the temporary one with ``--skip-networking``.  Waiting for
        ``@@skip_networking`` to be OFF identifies the real server exactly, with
        no dependence on log timing.
        """
        deadline = time.time() + timeout
        last_reason = "no attempt made"
        while time.time() < deadline:
            probe = run(["podman", "exec", self.container, "mysqladmin", "ping", "-uroot"],
                        15)
            if probe.returncode != 0:
                last_reason = "ping failed"
                time.sleep(interval)
                continue
            networked = run(["podman", "exec", self.container, "mysql", "-uroot", "-N", "-B",
                             "-e", "SELECT @@skip_networking;"], 30)
            value = networked.stdout.strip()
            if networked.returncode == 0 and value in ("0", "OFF"):
                return
            last_reason = f"temporary server still active (skip_networking={value!r})"
            time.sleep(interval)
        raise MysqlUnavailable(
            f"MySQL container {self.container} never became ready in {timeout}s "
            f"({last_reason}): {json.dumps(self.diagnostics())[:600]}")

    # -- table introspection ------------------------------------------------
    def column_metadata(self, database: str, table: str) -> list[dict]:
        """Authoritative column metadata for canonical ground truth."""
        sql = (
            "SELECT COLUMN_NAME, ORDINAL_POSITION, COLUMN_TYPE, IS_NULLABLE, "
            "CHARACTER_MAXIMUM_LENGTH, NUMERIC_PRECISION, NUMERIC_SCALE, "
            "CHARACTER_SET_NAME, COLLATION_NAME, EXTRA "
            "FROM information_schema.COLUMNS "
            f"WHERE TABLE_SCHEMA='{database}' AND TABLE_NAME='{table}' "
            "ORDER BY ORDINAL_POSITION;")
        columns = []
        for row in self.query_rows(sql):
            if len(row) < 10:
                continue
            columns.append({
                "name": row[0],
                "ordinal": int(row[1]) - 1,
                "type": row[2],
                "nullable": row[3] == "YES",
                "length": int(row[4]) if row[4] != "NULL" else None,
                "precision": int(row[5]) if row[5] != "NULL" else None,
                "scale": int(row[6]) if row[6] != "NULL" else None,
                "charset": row[7] if row[7] != "NULL" else None,
                "collation": row[8] if row[8] != "NULL" else None,
                "extra": row[9] if row[9] != "NULL" else "",
            })
        return columns

    def primary_key(self, database: str, table: str) -> list[str]:
        sql = ("SELECT COLUMN_NAME FROM information_schema.KEY_COLUMN_USAGE "
               f"WHERE TABLE_SCHEMA='{database}' AND TABLE_NAME='{table}' "
               "AND CONSTRAINT_NAME='PRIMARY' ORDER BY ORDINAL_POSITION;")
        return [row[0] for row in self.query_rows(sql) if row and row[0]]

    def row_count(self, database: str, table: str) -> int:
        value = self.scalar(f"SELECT COUNT(*) FROM `{database}`.`{table}`;")
        return int(value) if value.isdigit() else 0

    def table_present(self, database: str, table: str) -> bool:
        value = self.scalar(
            "SELECT COUNT(*) FROM information_schema.TABLES "
            f"WHERE TABLE_SCHEMA='{database}' AND TABLE_NAME='{table}';")
        return value not in ("", "0")


class ContainerGuard:
    """Best-effort container teardown that never masks the real failure."""

    def __init__(self, name: str) -> None:
        self.name = name

    def __enter__(self) -> "ContainerGuard":
        return self

    def __exit__(self, *_exc) -> bool:
        try:
            run(["podman", "rm", "-f", self.name], 120)
        except Exception:
            pass
        return False


def start_mysql(run_id: str, image: str, data_dir: Optional[Path],
                server_args: Sequence[str] = (), *, ready_timeout: float = 240,
                container_prefix: str = "dbrec") -> str:
    """Start a disposable MySQL container backed by ``data_dir`` and wait for it.

    A ``None`` data dir means the container keeps its own anonymous volume; the
    filesystem scenarios always pass one so the datadir is an explicit artifact.
    """
    container = f"{container_prefix}-{run_id}"
    run(["podman", "rm", "-f", container], 60)
    argv = ["podman", "run", "-d", "--name", container]
    if data_dir is not None:
        data_dir.mkdir(parents=True, exist_ok=True)
        os.chmod(data_dir, 0o777)
        argv += ["-v", f"{data_dir}:/var/lib/mysql:Z"]
    argv += ["-e", "MYSQL_ALLOW_EMPTY_PASSWORD=yes", image, *server_args]
    result = run(argv, 300)
    if not result.ok:
        raise RuntimeError(f"container start failed: {result.stderr[-600:]}")
    client = MysqlClient(container)
    client.wait_ready(ready_timeout)
    return container


def server_args_for(version: str, page_size: int, *, file_per_table: bool = True,
                    durable: bool = True) -> list[str]:
    """InnoDB server arguments that make a version test what the matrix claims.

    MySQL 5.6 defaults to the Antelope file format, which silently stores
    ROW_FORMAT=DYNAMIC/COMPRESSED as COMPACT.  Barracuda must be pinned or the
    ``DYNAMIC`` and ``COMPRESSED`` matrix cells would be measuring COMPACT.
    """
    argv: list[str] = []
    if file_per_table:
        argv.append("--innodb-file-per-table=1")
    argv.append(f"--innodb-page-size={page_size}")
    if durable:
        argv += ["--innodb_fast_shutdown=0", "--innodb_flush_log_at_trx_commit=1",
                 "--sync_binlog=1"]
    if version.startswith("5."):
        argv += ["--innodb-file-format=Barracuda", "--innodb-file-format-max=Barracuda",
                 "--innodb-large-prefix=ON"]
        # MySQL 5.6/5.7 default to explicit_defaults_for_timestamp=OFF, under
        # which only one TIMESTAMP column may carry the implicit
        # DEFAULT CURRENT_TIMESTAMP and a second one is rejected with
        # "Invalid default value" (ERROR 1067).  MySQL 8.0 turned this ON by
        # default.  Pinning it ON aligns the 5.x servers with 8.0 so the same
        # temporal fixture is valid on every version under test.
        argv.append("--explicit_defaults_for_timestamp=ON")
    return argv


def parallel_map(func: Callable, items: Sequence, workers: int = 1) -> list:
    """Run ``func`` over ``items`` with a bounded number of threads.

    Used for hashing large images and capturing per-case metadata; recovery
    itself stays sequential so two cases never contend for the same loop device.
    """
    if workers <= 1 or len(items) <= 1:
        return [func(item) for item in items]
    results: list = [None] * len(items)
    errors: list = [None] * len(items)

    def worker(index: int, item) -> None:
        try:
            results[index] = func(item)
        except Exception as error:  # noqa: BLE001 - surfaced by the caller
            errors[index] = error

    threads = []
    for index, item in enumerate(items):
        thread = threading.Thread(target=worker, args=(index, item), daemon=True)
        thread.start()
        threads.append(thread)
        if len(threads) >= workers:
            threads.pop(0).join()
    for thread in threads:
        thread.join()
    for error in errors:
        if error is not None:
            raise error
    return results
