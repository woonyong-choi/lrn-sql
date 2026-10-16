"""#137: 공개 SQL API의 동일 입력 정확도와 실제 실행 비용을 측정한다."""

from __future__ import annotations

import argparse
import hashlib
import json
import logging
import math
import os
from pathlib import Path
import platform
import random
import select
import statistics
import subprocess
import sys
import time
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "bench/sqlite_workload.json"
BUILD = ROOT / "build/sqlite-performance"
LOGGER = logging.getLogger(__name__)
SOURCES = [
    "bench/sqlite_driver.c",
    "src/sql/database.c",
    "src/sql/sql_statement.c",
    "src/sql/sql_eval.c",
    "src/sql/lexer.c",
    "src/sql/result.c",
    "src/storage/value.c",
    "src/storage/catalog.c",
    "src/storage/catalog_store.c",
    "src/storage/heap_store.c",
    "src/storage/pk_tree.c",
    "src/storage/text_tree.c",
    "src/storage/page_cache.c",
    "src/storage/checksum.c",
    "src/storage/page_file.c",
    "src/storage/record.c",
    "src/storage/record_page.c",
    "src/storage/overflow.c",
]


class Worker:
    # cost: time O(p), heap O(p), stack O(1), io 1
    # vars: p = 자식 프로세스 초기화 비용과 메모리
    # basis: estimate
    def __init__(self, engine: str, path: Path) -> None:
        self.engine = engine
        self.path = path
        self.process = subprocess.Popen(
            [str(BUILD / "driver"), engine, str(path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.pending = bytearray()

    # cost: time O(p), heap O(1), stack O(1), io 2
    # vars: p = 자식 프로세스 종료 대기 시간
    # basis: estimate
    def close(self) -> None:
        if self.process.stdin is not None:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        if self.process.returncode != 0:
            error = self.process.stderr.read() if self.process.stderr else b""
            raise RuntimeError(
                f"{self.engine} driver exit {self.process.returncode}: {error!r}"
            )

    # cost: time O(q + b), heap O(b), stack O(1), io O(b)
    # vars: q = 엔진 SQL 실행 비용, b = 결과 프로토콜 바이트 수
    # basis: estimate
    def execute(self, sql: str, timeout_seconds: float) -> dict[str, Any]:
        if self.process.stdin is None:
            raise RuntimeError("missing driver stdin")
        deadline = time.monotonic() + timeout_seconds
        self.process.stdin.write(sql.encode() + b"\n")
        self.process.stdin.flush()
        fields = [int(value) for value in self._readline(deadline).split()]
        if len(fields) != 7:
            raise RuntimeError(f"invalid response header: {fields}")
        keys = (
            "error",
            "ns",
            "row_count",
            "columns",
            "affected",
            "rss_bytes",
            "disk_bytes",
        )
        result = dict(zip(keys, fields))
        if result["rss_bytes"] > 2 << 30:
            self.process.kill()
            raise RuntimeError(f"{self.engine} observed RSS exceeds 2 GiB")
        error_line = self._readline(deadline).decode()
        result["message"] = bytes.fromhex(error_line[1:-1]).decode()  # type: ignore[assignment]
        result["rows"] = [self._readline(deadline).decode() for _ in range(fields[2])]  # type: ignore[assignment]
        return result

    # cost: time O(b), heap O(b), stack O(1), io O(b)
    # vars: b = 한 결과 행의 프로토콜 바이트 수
    # basis: estimate
    def _readline(self, deadline: float) -> bytes:
        if self.process.stdout is None:
            raise RuntimeError("missing driver stdout")
        while b"\n" not in self.pending:
            remaining = deadline - time.monotonic()
            ready, _, _ = select.select(
                [self.process.stdout], [], [], max(remaining, 0)
            )
            if not ready:
                self.process.kill()
                raise TimeoutError(f"{self.engine} statement timeout")
            chunk = os.read(self.process.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError(f"{self.engine} driver closed output")
            self.pending.extend(chunk)
        line, _, rest = self.pending.partition(b"\n")
        self.pending = bytearray(rest)
        return bytes(line)


def _compare(results: dict[str, dict[str, Any]], sql: str) -> None:
    mini, sqlite = results["mini"], results["sqlite"]
    if mini["error"] or sqlite["error"]:
        raise RuntimeError(
            f"unexpected SQL failure: {sql}: {mini['message']} / {sqlite['message']}"
        )
    for field in ("row_count", "columns", "affected", "rows"):
        left, right = mini[field], sqlite[field]
        if field == "rows" and "ORDER BY" not in sql:
            left, right = sorted(left), sorted(right)
        if left != right:
            raise RuntimeError(
                f"oracle mismatch ({field}): {sql}: {str(mini[field])[:500]} / {str(sqlite[field])[:500]}"
            )


# cost: time O(q + b), heap O(b), stack O(1), io O(b)
# vars: q = 두 엔진의 실행 비용, b = 전체 결과 프로토콜 바이트 수
# basis: estimate
def _execute_pair(workers: list[Worker], sql: str, timeout: float) -> dict[str, Any]:
    results = {worker.engine: worker.execute(sql, timeout) for worker in workers}
    _compare(results, sql)
    return results


def _compact(result: dict[str, Any]) -> dict[str, Any]:
    digest = hashlib.sha256("\n".join(sorted(result["rows"])).encode()).hexdigest()
    return {
        **{key: value for key, value in result.items() if key != "rows"},
        "rows_sha256": digest,
    }


def _insert_sql(index: int, seed: int) -> str:
    bucket = (index * 17 + seed) % 97
    note = "NULL" if index % 11 == 0 else f"'note-{index % 101}'"
    payload = "'" + "x" * 9000 + "'" if index % 1000 == 0 else "'payload-가나다'"
    return f"INSERT INTO items VALUES ({index}, {bucket}, {index % 997}.5, 'user-{index}', {note}, {payload}, X'00ff10')"


# cost: time O(n * q), heap O(n), stack O(1), io O(n * p)
# vars: n = 삽입 행 수, q = 문장 실행 비용, p = 문장당 접근 페이지와 프로토콜 비용
# basis: estimate
def _load(workers: list[Worker], size: int, config: dict[str, Any]) -> dict[str, Any]:
    started = time.monotonic()
    samples: dict[str, list[int]] = {worker.engine: [] for worker in workers}
    last: dict[str, Any] = {}
    completed = 0
    for index in range(1, size + 1):
        if time.monotonic() - started >= config["load_timeout_seconds"]:
            break
        last = _execute_pair(
            workers,
            _insert_sql(index, config["seed"]),
            config["statement_timeout_seconds"],
        )
        for engine, result in last.items():
            samples[engine].append(result["ns"])
        completed = index
        if index % 1000 == 0:
            LOGGER.info(
                "loaded %d/%d rows in %.1fs", index, size, time.monotonic() - started
            )
    return {
        "requested_rows": size,
        "completed_rows": completed,
        "status": "complete" if completed == size else "load_timeout",
        "elapsed_seconds": time.monotonic() - started,
        "latencies_ns": samples,
        "last": {engine: _compact(result) for engine, result in last.items()},
    }


def _query_cases(count: int, seed: int) -> list[tuple[str, str]]:
    randomizer = random.Random(seed)
    return [
        (
            "point",
            f"SELECT id, name FROM items WHERE id = {randomizer.randint(1, count)}",
        ),
        ("missing", f"SELECT id FROM items WHERE id = {count + 100}"),
        ("range", f"SELECT id, score FROM items WHERE id >= {max(1, count - 99)}"),
        ("bucket", "SELECT id, name FROM items WHERE bucket = 3"),
        ("null", "SELECT id, note FROM items WHERE note IS NULL"),
        ("full_scan", "SELECT * FROM items"),
    ]


# cost: time O(k * q + b), heap O(b), stack O(1), io O(k * p + b)
# vars: k = 조회 반복 수, q = SQL 실행 비용, b = 반환 바이트, p = 접근 페이지 수
# basis: estimate
def _read_queries(
    workers: list[Worker], count: int, config: dict[str, Any]
) -> list[dict[str, Any]]:
    cases = _query_cases(count, config["seed"])
    timeout = config["statement_timeout_seconds"]
    for _ in range(config["warmups"]):
        for _, sql in cases:
            _execute_pair(workers, sql, timeout)
    observations = []
    for repetition in range(config["query_repetitions"]):
        cases = _query_cases(count, config["seed"] + repetition)
        for name, sql in cases:
            results = _execute_pair(workers, sql, timeout)
            observations.append(
                {
                    "name": name,
                    "sql": sql,
                    "repetition": repetition,
                    "results": {
                        engine: _compact(result) for engine, result in results.items()
                    },
                }
            )
    return observations


# cost: time O(k * q + b), heap O(b), stack O(1), io O(k * p + b)
# vars: k = 기능 구문 수, q = SQL 실행 비용, b = 반환 바이트, p = 접근 페이지 수
# basis: estimate
def _complex_queries(
    workers: list[Worker], config: dict[str, Any]
) -> list[dict[str, Any]]:
    observations = []
    for name, sql in config["complex_queries"].items():
        results = {
            worker.engine: worker.execute(sql, config["statement_timeout_seconds"])
            for worker in workers
        }
        if results["sqlite"]["error"]:
            raise RuntimeError(f"invalid SQLite reference query: {sql}")
        if results["mini"]["error"] not in (0, 1, 2):
            raise RuntimeError(
                f"unexpected lrn-sql failure: {sql}: {results['mini']['message']}"
            )
        status = "unsupported" if results["mini"]["error"] else "supported"
        if status == "supported":
            _compare(results, sql)
        observations.append(
            {
                "name": name,
                "sql": sql,
                "status": status,
                "results": {
                    engine: _compact(result) for engine, result in results.items()
                },
            }
        )
    return observations


# cost: time O(k * q + b), heap O(b), stack O(1), io O(k * p + b)
# vars: k = 변경 및 검증 문장 수, q = 엔진 실행 비용, b = 전체 결과, p = 접근 페이지
# basis: estimate
def _mutations(
    workers: list[Worker], count: int, timeout: float
) -> list[dict[str, Any]]:
    statements = [
        f"UPDATE items SET score = 9001.5, note = 'changed' WHERE id = {count // 2}",
        "UPDATE items SET note = 'batch' WHERE bucket = 3",
        "SELECT * FROM items",
        "DELETE FROM items WHERE id = 1",
        "DELETE FROM items WHERE bucket = 7",
        "SELECT * FROM items",
        "@reopen",
        "SELECT * FROM items",
        "SELECT * FROM groups",
    ]
    observations = []
    for sql in statements:
        results = _execute_pair(workers, sql, timeout)
        observations.append(
            {
                "sql": sql,
                "results": {
                    engine: _compact(result) for engine, result in results.items()
                },
            }
        )
    return observations


MIXED_KINDS = (("point", 60), ("range", 10), ("update", 20), ("insert", 7), ("delete", 3))


def _mixed_sql(kind: str, count: int, step: int, randomizer: random.Random) -> str:
    key = randomizer.randint(1, count)
    if kind == "point":
        return f"SELECT id, name, note FROM items WHERE id = {key}"
    if kind == "range":
        return f"SELECT id, score FROM items WHERE id >= {key} AND id < {key + 50}"
    if kind == "update":
        return f"UPDATE items SET score = {step}.25, note = 'mixed-{step}' WHERE id = {key}"
    if kind == "insert":
        return _insert_sql(count + 1000 + step, step)
    return f"DELETE FROM items WHERE id = {key}"


# cost: time O(k * q), heap O(k), stack O(1), io O(k * p)
# vars: k = 혼합 문장 수, q = 문장 실행 비용, p = 문장당 접근 페이지와 fsync
# basis: estimate
def _mixed(workers: list[Worker], count: int, config: dict[str, Any]) -> dict[str, Any]:
    """조회·범위·갱신·삽입·삭제를 정해진 비율로 섞어 자동 commit으로 실행하고 매 문장 결과를 대조한다."""
    randomizer = random.Random(config["seed"] + count)
    kinds = [kind for kind, weight in MIXED_KINDS for _ in range(weight)]
    samples: dict[str, dict[str, list[int]]] = {
        worker.engine: {kind: [] for kind, _ in MIXED_KINDS} for worker in workers
    }
    errors = {worker.engine: 0 for worker in workers}
    started = time.monotonic()
    for step in range(config["mixed_operations"]):
        kind = randomizer.choice(kinds)
        results = _execute_pair(
            workers, _mixed_sql(kind, count, step, randomizer), config["statement_timeout_seconds"]
        )
        for engine, result in results.items():
            samples[engine][kind].append(result["ns"])
            errors[engine] += result["error"] != 0
    final = _execute_pair(workers, "SELECT * FROM items", config["statement_timeout_seconds"])
    return {
        "operations": config["mixed_operations"],
        "elapsed_seconds": time.monotonic() - started,
        "latencies_ns": samples,
        "errors": errors,
        "final": {engine: _compact(result) for engine, result in final.items()},
    }


def _scalar(result: dict[str, Any]) -> int:
    return int(result["rows"][0].strip(";").lstrip("I"))


# cost: time O(t * (r + q)), heap O(1), stack O(1), io O(t * f)
# vars: t = 강제 종료 횟수, r = 다시 열기 비용, q = 검증 문장 비용, f = 파일 크기
# basis: estimate
def _recovery(engine: str, path: Path, config: dict[str, Any]) -> list[dict[str, Any]]:
    """진행 중인 변경 문장 도중 프로세스를 SIGKILL로 끝내고, 다시 열어 기본 키 조회 하나에 답할 때까지의 시간과 원자성을 확인한다."""
    timeout = config["statement_timeout_seconds"]
    randomizer = random.Random(config["seed"])
    trials = []
    for trial in range(config["recovery_trials"]):
        worker = Worker(engine, path)
        before = _scalar(worker.execute("SELECT COUNT(*) FROM items", timeout))
        marked = _scalar(worker.execute("SELECT COUNT(*) FROM items WHERE bucket < 48", timeout))
        worker.execute("UPDATE items SET note = NULL WHERE note = 'crash'", timeout)
        assert worker.process.stdin is not None
        worker.process.stdin.write(b"UPDATE items SET note = 'crash' WHERE bucket < 48\n")
        worker.process.stdin.flush()
        time.sleep(randomizer.uniform(0, 0.02))
        worker.process.kill()
        worker.process.wait()
        size = path.stat().st_size
        began = time.monotonic_ns()
        reopened = Worker(engine, path)
        reopened.execute("SELECT label FROM groups WHERE id = 0", timeout)
        reopen_ns = time.monotonic_ns() - began
        after = _scalar(reopened.execute("SELECT COUNT(*) FROM items", timeout))
        crashed = _scalar(reopened.execute("SELECT COUNT(*) FROM items WHERE note = 'crash'", timeout))
        reopened.close()
        if after != before or crashed not in (0, marked):
            raise RuntimeError(f"{engine} recovery broke atomicity: {before}->{after}, {crashed}/{marked}")
        trials.append(
            {"trial": trial, "reopen_ns": reopen_ns, "file_bytes": size, "applied": crashed == marked}
        )
    return trials


# cost: time O(n * q + b), heap O(n + b), stack O(1), io O(n * p + b)
# vars: n = 문장 수, q = 엔진 실행 비용, b = 결과 바이트, p = 문장당 접근 페이지
# basis: estimate
def _run_case(size: int, repetition: int, config: dict[str, Any]) -> dict[str, Any]:
    names = ["mini", "sqlite"] if repetition % 2 == 0 else ["sqlite", "mini"]
    workers = [
        Worker(name, BUILD / f"{name}-{size}-{repetition}-{config['run_id']}.db")
        for name in names
    ]
    timeout = config["statement_timeout_seconds"]
    result: dict[str, Any] = {
        "requested_rows": size,
        "repetition": repetition,
        "engine_order": names,
    }
    try:
        sqlite_worker = next(worker for worker in workers if worker.engine == "sqlite")
        result["sqlite_settings"] = {
            name: sqlite_worker.execute(f"PRAGMA {name}", timeout)
            for name in (
                "page_size",
                "journal_mode",
                "synchronous",
                "fullfsync",
                "cache_size",
                "mmap_size",
                "temp_store",
            )
        }
        _execute_pair(
            workers,
            "CREATE TABLE items (id INTEGER PRIMARY KEY, bucket INTEGER, score REAL, name TEXT, note TEXT, payload TEXT, binary BLOB)",
            timeout,
        )
        _execute_pair(
            workers, "CREATE TABLE groups (id INTEGER PRIMARY KEY, label TEXT)", timeout
        )
        for index in range(97):
            _execute_pair(
                workers,
                f"INSERT INTO groups VALUES ({index}, 'group-{index}')",
                timeout,
            )
        result["load"] = _load(workers, size, config)
        count = result["load"]["completed_rows"]
        if count == 0:
            raise RuntimeError("no rows loaded")
        result["queries"] = _read_queries(workers, count, config)
        result["complex"] = _complex_queries(workers, config)
        result["mutations"] = _mutations(workers, count, timeout)
        result["mixed"] = _mixed(workers, count, config)
    finally:
        for worker in workers:
            worker.close()
    result["recovery"] = {worker.engine: _recovery(worker.engine, worker.path, config) for worker in workers}
    result["status"] = "verified"
    return result


# cost: time O(c), heap O(c), stack O(1), io 1
# vars: c = 소스 컴파일 비용과 컴파일러 메모리
# basis: estimate
def _build(config: dict[str, Any]) -> None:
    BUILD.mkdir(parents=True, exist_ok=True)
    command = [
        "clang",
        *config["build_flags"],
        "-Iinclude",
        *SOURCES,
        "-lsqlite3",
        "-lm",
        "-lpthread",
        "-o",
        str(BUILD / "driver"),
    ]
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)


# cost: time O(b), heap O(b), stack O(1), io 1
# vars: b = 측정 원자료 바이트 수
# basis: estimate
def _save(output: Path, measurement: dict[str, Any]) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(measurement, ensure_ascii=False, indent=2) + "\n")


def _stats(values: list[int]) -> dict[str, float]:
    ordered = sorted(values)
    return {
        **{
            f"p{percentile}_ms": ordered[
                max(0, math.ceil(len(ordered) * percentile / 100) - 1)
            ]
            / 1e6
            for percentile in (50, 95, 99)
        },
        "qps": len(values) * 1e9 / sum(values),
    }


def _summarize(runs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    summary = []
    for run in runs:
        entry: dict[str, Any] = {
            "requested_rows": run["requested_rows"],
            "completed_rows": run["load"]["completed_rows"],
            "repetition": run["repetition"],
            "load_status": run["load"]["status"],
        }
        for engine in ("mini", "sqlite"):
            entry[engine] = {"insert": _stats(run["load"]["latencies_ns"][engine])}
            for name in {query["name"] for query in run["queries"]}:
                times = [
                    query["results"][engine]["ns"]
                    for query in run["queries"]
                    if query["name"] == name
                ]
                entry[engine][name] = _stats(times)
            results = [
                query["results"][engine] for query in run["queries"] + run["mutations"]
            ]
            entry[engine]["peak_rss_bytes"] = max(
                result["rss_bytes"] for result in results
            )
            entry[engine]["final_disk_bytes"] = results[-1]["disk_bytes"]
            entry[engine]["mixed"] = {
                kind: _stats(times) for kind, times in run["mixed"]["latencies_ns"][engine].items() if times
            }
            entry[engine]["reopen_after_kill"] = _stats([trial["reopen_ns"] for trial in run["recovery"][engine]])
        summary.append(entry)
    return summary


# cost: time O(p), heap O(b), stack O(1), io 4
# vars: p = 네 환경 명령 실행 비용, b = 출력 바이트 수
# basis: estimate
def _environment() -> dict[str, str]:
    commands = {
        "head": ["git", "rev-parse", "HEAD"],
        "compiler": ["clang", "--version"],
        "sqlite": [str(BUILD / "driver"), "--version"],
    }
    return {
        "platform": platform.platform(),
        "python": sys.version,
        "cpu_memory": _cpu_memory(),
        **{
            name: subprocess.check_output(command, cwd=ROOT, text=True).strip()
            for name, command in commands.items()
        },
    }


# cost: time O(b), heap O(b), stack O(1), io 1
# vars: b = 장비 정보 출력 바이트 수
# basis: estimate
def _cpu_memory() -> str:
    """CPU 이름과 메모리 바이트 수. macOS는 sysctl, Linux는 /proc에서 읽는다."""
    if sys.platform == "darwin":
        command = ["sysctl", "-n", "machdep.cpu.brand_string", "hw.memsize"]
        return subprocess.check_output(command, text=True).strip()
    cpu = next((line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                if line.startswith("model name")), "unknown")
    memory_kb = next((int(line.split()[1]) for line in Path("/proc/meminfo").read_text().splitlines()
                      if line.startswith("MemTotal:")), 0)
    return f"{cpu}\n{memory_kb * 1024}"


def _self_check() -> None:
    known = {"error": 0, "row_count": 1, "columns": 1, "affected": 0, "rows": ["I1;"]}
    _compare({"mini": known, "sqlite": dict(known)}, "positive control")
    try:
        _compare(
            {"mini": known, "sqlite": {**known, "rows": ["I2;"]}}, "negative control"
        )
    except RuntimeError:
        return
    raise RuntimeError("oracle failed to reject altered result")


def _verify_run(run: dict[str, Any]) -> int:
    count = run["load"]["completed_rows"]
    buckets = sum((index * 17 + 137) % 97 == 3 for index in range(1, count + 1))
    expected = {
        "point": 1,
        "missing": 0,
        "range": min(count, 100),
        "bucket": buckets,
        "null": count // 11,
        "full_scan": count,
    }
    for engine in ("mini", "sqlite"):
        if len(run["load"]["latencies_ns"][engine]) != count:
            raise RuntimeError("missing insert latency observations")
    for query in run["queries"]:
        for result in query["results"].values():
            if result["row_count"] != expected[query["name"]]:
                raise RuntimeError(f"independent row-count mismatch: {query['sql']}")
    for query in run["queries"] + run["mutations"]:
        mini, sqlite = query["results"]["mini"], query["results"]["sqlite"]
        if mini["error"] or sqlite["error"]:
            raise RuntimeError(f"unexpected persisted error: {query['sql']}")
        for field in ("rows_sha256", "row_count", "affected", "columns"):
            if mini[field] != sqlite[field]:
                raise RuntimeError(f"persisted oracle mismatch: {query['sql']}")
    for query in run["complex"]:
        mini, sqlite = query["results"]["mini"], query["results"]["sqlite"]
        if sqlite["error"] or (query["status"] == "supported") != (mini["error"] == 0):
            raise RuntimeError(f"unexpected feature status: {query['sql']}")
        if query["status"] == "supported" and mini["rows_sha256"] != sqlite["rows_sha256"]:
            raise RuntimeError(f"persisted oracle mismatch: {query['sql']}")
    if run["mixed"]["final"]["mini"]["rows_sha256"] != run["mixed"]["final"]["sqlite"]["rows_sha256"]:
        raise RuntimeError("mixed workload final state mismatch")
    return 2 * len(run["queries"])


def _aggregate_size(runs: list[dict[str, Any]], engine: str) -> dict[str, Any]:
    inserts = [sum(run["load"]["latencies_ns"][engine]) / 1e9 for run in runs]
    metrics: dict[str, Any] = {
        "insert_seconds": inserts,
        "insert_seconds_median": statistics.median(inserts),
        "insert_qps_median": statistics.median(
            run["load"]["completed_rows"] / seconds
            for run, seconds in zip(runs, inserts)
        ),
    }
    for name in sorted({query["name"] for run in runs for query in run["queries"]}):
        values = [
            query["results"][engine]["ns"]
            for run in runs
            for query in run["queries"]
            if query["name"] == name
        ]
        metrics[name] = {"samples": len(values), **_stats(values)}
    observations = [
        query["results"][engine]
        for run in runs
        for query in run["queries"] + run["mutations"]
    ]
    metrics["max_rss_bytes"] = max(result["rss_bytes"] for result in observations)
    metrics["final_disk_bytes"] = runs[-1]["mutations"][-1]["results"][engine][
        "disk_bytes"
    ]
    for kind, _ in MIXED_KINDS:
        values = [ns for run in runs for ns in run["mixed"]["latencies_ns"][engine][kind]]
        metrics[f"mixed_{kind}"] = {"samples": len(values), **_stats(values)}
    mixed_seconds = [sum(sum(v) for v in run["mixed"]["latencies_ns"][engine].values()) / 1e9 for run in runs]
    metrics["mixed_qps_median"] = statistics.median(
        run["mixed"]["operations"] / seconds for run, seconds in zip(runs, mixed_seconds)
    )
    metrics["mixed_errors"] = sum(run["mixed"]["errors"][engine] for run in runs)
    trials = [trial for run in runs for trial in run["recovery"][engine]]
    metrics["reopen_after_kill"] = {
        "samples": len(trials),
        "applied": sum(trial["applied"] for trial in trials),
        "max_file_bytes": max(trial["file_bytes"] for trial in trials),
        **_stats([trial["reopen_ns"] for trial in trials]),
    }
    return metrics


# cost: time O(b + n log n), heap O(b + n), stack O(1), io 1
# vars: b = 기록 바이트, n = 원자료 시간 표본 수
# basis: estimate
def _analyze(output: Path) -> dict[str, Any]:
    measurement = json.loads(output.read_text())
    runs = measurement["runs"]
    config = measurement["config"]
    expected = {
        (size, repetition)
        for size in config["sizes"]
        for repetition in range(config["repetitions"])
    }
    actual = {(run["requested_rows"], run["repetition"]) for run in runs}
    if actual != expected or len(runs) != len(expected):
        raise RuntimeError("missing or duplicate workload runs")
    if "failure" in measurement:
        raise RuntimeError("measurement contains a failed run")
    analysis: dict[str, Any] = {
        "source_head": measurement["environment"]["head"],
        "runs": len(runs),
        "independent_row_count_checks": sum(_verify_run(run) for run in runs),
        "paired_inserts": sum(run["load"]["completed_rows"] for run in runs),
        "verified_selects": sum(len(run["queries"]) for run in runs),
        "unsupported_features": sorted(
            {query["name"] for run in runs for query in run["complex"] if query["status"] == "unsupported"}
        ),
        "capacity_attempts": [
            {
                "requested": run["requested_rows"],
                "completed": run["load"]["completed_rows"],
                "elapsed_seconds": run["load"]["elapsed_seconds"],
            }
            for run in runs
            if run["load"]["status"] != "complete"
        ],
        "sizes": {},
    }
    for size in config["sizes"]:
        complete = [
            run
            for run in runs
            if run["requested_rows"] == size and run["load"]["status"] == "complete"
        ]
        if len(complete) != config["repetitions"]:
            continue
        analysis["sizes"][str(size)] = {
            engine: _aggregate_size(complete, engine) for engine in ("mini", "sqlite")
        }
    return analysis


# cost: time O(n * q + b), heap O(n + b), stack O(1), io O(n * p + b)
# vars: n = 측정 문장 수, q = 실행 비용, b = 원결과 바이트, p = 문장당 페이지와 프로토콜 비용
# basis: estimate
def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=ROOT / "build/bench/sqlite_workload.json")
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--analyze", action="store_true")
    parser.add_argument("--sizes", type=int, nargs="+")
    parser.add_argument("--load-timeout-seconds", type=float)
    args = parser.parse_args()
    if args.analyze:
        print(
            json.dumps(
                _analyze(args.output), ensure_ascii=False, sort_keys=True, indent=2
            )
        )
        return 0
    config = json.loads(CONFIG.read_text())
    if args.sizes is not None:
        if any(size <= 0 for size in args.sizes):
            parser.error("--sizes requires positive row counts")
        config["sizes"] = args.sizes
    if args.load_timeout_seconds is not None:
        if args.load_timeout_seconds <= 0:
            parser.error("--load-timeout-seconds requires a positive value")
        config["load_timeout_seconds"] = args.load_timeout_seconds
    config["run_id"] = time.time_ns()
    _self_check()
    _build(config)
    if args.build_only:
        print(json.dumps(_environment(), ensure_ascii=False))
        return 0
    if args.smoke:
        config.update(sizes=[30], repetitions=1, query_repetitions=1, mixed_operations=50, recovery_trials=2)
    measurement: dict[str, Any] = {
        "config": config,
        "environment": _environment(),
        "runs": [],
    }
    for size in config["sizes"]:
        for repetition in range(config["repetitions"]):
            LOGGER.info("starting %d rows, repetition %d", size, repetition + 1)
            try:
                measurement["runs"].append(_run_case(size, repetition, config))
            except (RuntimeError, TimeoutError, OSError) as error:
                measurement["failure"] = {
                    "size": size,
                    "repetition": repetition,
                    "error": str(error),
                }
                _save(args.output, measurement)
                raise
            measurement["summary"] = _summarize(measurement["runs"])
            _save(args.output, measurement)
            print(
                json.dumps(measurement["summary"][-1], ensure_ascii=False), flush=True
            )
    return 0


if __name__ == "__main__":
    logging.basicConfig(
        level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s"
    )
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        LOGGER.error("measurement failed: %s", error)
        raise SystemExit(1) from error
