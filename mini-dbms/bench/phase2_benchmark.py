#!/usr/bin/env python3
"""Compare a rebuilt in-memory ID index with a linear non-ID scan.

The benchmark creates all data in a temporary directory and removes it on exit.
It intentionally leaves no million-row artifact in the repository.
"""

import argparse
import pathlib
import subprocess
import tempfile
import time


ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = ROOT / "mini_dbms"


def run(db: pathlib.Path, sql: str) -> tuple[float, str]:
    query = db / "query.sql"
    query.write_text(sql)
    started = time.perf_counter()
    result = subprocess.run([str(BIN), str(db), str(query)], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, check=False)
    elapsed = time.perf_counter() - started
    if result.returncode:
        raise RuntimeError(result.stderr)
    return elapsed, result.stdout


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rows", type=int, default=1_000_000)
    parser.add_argument("--lookups", type=int, default=1000)
    parser.add_argument("--report", type=pathlib.Path,
                        help="optional path for the final text report")
    args = parser.parse_args()
    if args.rows < 1_000_000:
        parser.error("--rows must be at least 1,000,000 for the Phase 2 requirement")
    if args.lookups < 2:
        parser.error("--lookups must be at least 2")
    if not BIN.exists():
        parser.error("build first with: make")

    with tempfile.TemporaryDirectory(prefix="mini-dbms-phase2-") as temp:
        db = pathlib.Path(temp)
        (db / "people.schema").write_text("id INTEGER\nname TEXT\nage INTEGER\n")
        inserts = db / "inserts.sql"
        started = time.perf_counter()
        with inserts.open("w") as output:
            for row in range(args.rows):
                output.write(f"INSERT INTO people (name, age) VALUES ('user{row}', {row});\n")
        generated_seconds = time.perf_counter() - started
        started = time.perf_counter()
        result = subprocess.run([str(BIN), str(db), str(inserts)], stdout=subprocess.DEVNULL,
                                stderr=subprocess.PIPE, text=True, check=False)
        load_seconds = time.perf_counter() - started
        if result.returncode:
            raise RuntimeError(result.stderr)

        target = args.rows // 2
        rebuild_plus_first, first_output = run(db, f"SELECT id FROM people WHERE id = {target};")
        if first_output != f"{target}\n":
            raise RuntimeError("ID lookup returned an unexpected result")
        id_batch = "".join(
            f"SELECT id FROM people WHERE id = {(target + i) % args.rows};"
            for i in range(args.lookups)
        )
        id_batch_seconds, id_output = run(db, id_batch)
        expected_id_output = "".join(
            f"{(target + i) % args.rows}\n" for i in range(args.lookups)
        )
        if id_output != expected_id_output:
            raise RuntimeError("repeated ID lookups returned unexpected results")
        steady_id = max(0.0, id_batch_seconds - rebuild_plus_first) / (args.lookups - 1)
        linear_seconds, linear_output = run(db, f"SELECT age FROM people WHERE age = {target};")
        if linear_output != f"{target}\n":
            raise RuntimeError("linear lookup returned an unexpected result")

        report = "\n".join([
            f"rows: {args.rows:,}",
            f"SQL generation: {generated_seconds:.3f}s",
            f"insert/load: {load_seconds:.3f}s",
            f"index rebuild + first ID lookup: {rebuild_plus_first:.3f}s",
            f"steady ID lookup estimate: {steady_id * 1_000_000:.1f}us",
            f"linear age lookup: {linear_seconds:.3f}s",
        ])
        print(report)
        if args.report:
            args.report.write_text(report + "\n")


if __name__ == "__main__":
    main()
