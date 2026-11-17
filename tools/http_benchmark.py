"""End-to-end, real-process HTTP load validation. Python standard library only."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import math
from pathlib import Path
import platform
import shutil
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from api.native import NativeEngine
from tests.api_support import ServerProcess, request


def percentile(values, percent):
    values = sorted(values)
    return values[max(0, math.ceil(len(values) * percent / 100) - 1)]


def seed_database(path: Path, rows: int):
    with NativeEngine(path) as engine:
        for first in range(1, rows + 1, 1000):
            values = ",".join(f"('seed_{number}',{number % 100})"
                              for number in range(first, min(first + 1000, rows + 1)))
            engine.execute("INSERT INTO users(name,age) VALUES " + values)


def run_case(root: Path, seed_rows: int, requests: int, clients: int, workload: str,
             workers: int, queue_size: int, repeat: int):
    case = root / f"{workload}-clients{clients}-repeat{repeat}"
    database = case / "database"
    database.mkdir(parents=True)
    shutil.copyfile(root / "seed" / "users.db", database / "users.db")
    with ServerProcess(database, "--workers", str(workers), "--queue-size", str(queue_size)) as server:
        # Warm native and HTTP paths before timing client-visible operations.
        status, result, _ = request(server.port, document={"sql": "SELECT * FROM users WHERE id=1"})
        if status != 200 or result["rows"][0][0] != 1:
            raise RuntimeError("HTTP warmup failed")

        def execute(number):
            writing = workload == "overload" or (workload == "mixed" and number % 10 == 9)
            expected_id = (number * 2654435761 + 17) % seed_rows + 1
            sql = (f"INSERT INTO users(name,age) VALUES('http_{number}',{number % 100})" if writing
                   else f"SELECT id,name FROM users WHERE id={expected_id}")
            start = time.perf_counter()
            status, body, headers = request(server.port, document={"sql": sql}, timeout=15)
            elapsed = (time.perf_counter() - start) * 1000
            if status == 503 and body.get("error", {}).get("code") == "SERVER_BUSY":
                return {"status": status, "latency_ms": elapsed}
            if status != 200:
                raise RuntimeError(f"Unexpected HTTP error: {status} {body}")
            if writing:
                if body["affected_rows"] != 1 or body["first_insert_id"] <= seed_rows:
                    raise RuntimeError("Invalid INSERT response")
            elif body["rows"] != [[expected_id, f"seed_{expected_id}"]] or body["plan"] != "INDEX LOOKUP":
                raise RuntimeError("HTTP SELECT returned incorrect data or plan")
            return {"status": status, "latency_ms": elapsed, "request": number,
                    "insert_id": body["first_insert_id"] if writing else None,
                    "worker": headers["X-Worker-Name"],
                    "execution_ms": body["request"]["execution_ms"],
                    "queue_wait_ms": body["request"]["queue_wait_ms"]}

        start = time.perf_counter()
        with ThreadPoolExecutor(max_workers=clients) as pool:
            outcomes = list(pool.map(execute, range(requests)))
        elapsed = time.perf_counter() - start
        status, stats, _ = request(server.port, "GET", "/stats")
        if status != 200 or stats["pool"]["peak_active"] > workers:
            raise RuntimeError("Pool worker bound violated")
    successes = [item for item in outcomes if item["status"] == 200]
    rejected = [item for item in outcomes if item["status"] == 503]
    if workload != "overload" and rejected:
        raise RuntimeError("Unexpected rejection in a non-overload case")
    inserted = {item["insert_id"]: item["request"] for item in successes if item["insert_id"] is not None}
    if len(inserted) != sum(item["insert_id"] is not None for item in successes):
        raise RuntimeError("Duplicate INSERT ID")
    if sorted(inserted) != list(range(seed_rows + 1, seed_rows + len(inserted) + 1)):
        raise RuntimeError("Missing INSERT ID")
    with NativeEngine(database) as engine:
        if engine.stats()["row_count"] != seed_rows + len(inserted):
            raise RuntimeError("Restart row count differs from acknowledged writes")
        found = engine.execute(f"SELECT id,name,age FROM users WHERE id>{seed_rows}", max_rows=requests)["rows"]
        if len(found) != len(inserted):
            raise RuntimeError("Unexpected persisted INSERT count")
        for row_id, name, age in found:
            number = inserted[row_id]
            if name != f"http_{number}" or age != number % 100:
                raise RuntimeError("Persisted INSERT content differs")
    latencies = [item["latency_ms"] for item in outcomes]
    success_latencies = [item["latency_ms"] for item in successes]
    return {"workload": workload, "clients": clients, "workers": workers, "queue_size": queue_size,
            "repeat": repeat, "requests": requests, "successes": len(successes),
            "rejected_503": len(rejected), "unexpected_errors": 0, "inserted_rows": len(inserted),
            "elapsed_seconds": elapsed, "successes_per_second": len(successes) / elapsed,
            "p50_ms": percentile(latencies, 50), "p95_ms": percentile(latencies, 95),
            "p99_ms": percentile(latencies, 99),
            "success_p95_ms": percentile(success_latencies, 95) if success_latencies else None,
            "execution_p95_ms": percentile([item["execution_ms"] for item in successes], 95) if successes else None,
            "queue_wait_p95_ms": percentile([item["queue_wait_ms"] for item in successes], 95) if successes else None,
            "worker_names": sorted({item["worker"] for item in successes}),
            "peak_active": stats["pool"]["peak_active"], "restart_verified": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="new directory; parent must exist")
    parser.add_argument("--rows", type=int, default=10000)
    parser.add_argument("--requests", type=int, default=500)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 1 <= args.rows <= 1000000 or not 1 <= args.requests <= 100000 or not 1 <= args.repeats <= 20:
        parser.error("rows/requests/repeats exceed benchmark bounds")
    args.directory.mkdir()  # Existing data is never overwritten.
    seed_database(args.directory / "seed", args.rows)
    report = {"environment": {"platform": platform.platform(), "python": platform.python_version()},
              "seed_rows": args.rows, "sync_writes": True, "closed_loop": True,
              "connection_per_request": True, "cases": []}
    def save_progress():
        if args.output:
            args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for repeat in range(1, args.repeats + 1):
        for workload in ("point", "mixed"):
            for clients in (1, 8, 32):
                result = run_case(args.directory, args.rows, args.requests, clients, workload, 4, 64, repeat)
                report["cases"].append(result)
                save_progress()
                print(f"PASS {workload} clients={clients} repeat={repeat} "
                      f"successes/s={result['successes_per_second']:.1f} p95={result['p95_ms']:.2f}ms", file=sys.stderr)
        result = run_case(args.directory, args.rows, args.requests, 64, "overload", 2, 2, repeat)
        if not result["rejected_503"]:
            raise RuntimeError("Overload case did not exercise admission rejection")
        report["cases"].append(result)
        save_progress()
        print(f"PASS overload repeat={repeat}: {result['successes']} completed, {result['rejected_503']} rejected", file=sys.stderr)
    report["correctness"] = "PASS"
    encoded = json.dumps(report, ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(encoded + "\n", encoding="utf-8")
    else:
        print(encoded)


if __name__ == "__main__":
    main()
