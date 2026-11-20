import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class BenchmarkTests(unittest.TestCase):
    def test_concurrent_path_creation_is_preserved(self):
        binary = os.environ.get("MINI_DBMS_BENCHMARK_RACE", "build/benchmark-race")
        with tempfile.TemporaryDirectory(prefix="mini-dbms-bench-race-") as directory:
            sql = Path(directory) / "load.sql"
            db = Path(directory) / "history.db"
            subprocess.run(["python3", "tools/generate_history.py", "--count", "1", "--output", str(sql)], check=True)
            result = subprocess.run([binary, str(sql), str(db), "1"], capture_output=True, text=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(db.read_bytes(), b"")

    def test_generation_load_rebuild_and_equal_result_comparison(self):
        binary = os.environ.get("MINI_DBMS_BENCHMARK", "build/benchmark")
        with tempfile.TemporaryDirectory(prefix="mini-dbms-benchmark-") as directory:
            sql = Path(directory) / "load.sql"
            db = Path(directory) / "history.db"
            subprocess.run(["python3", "tools/generate_history.py", "--count", "1000", "--output", str(sql)], check=True)
            result = subprocess.run([binary, str(sql), str(db), "3"], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual(report["records"], 1000)
            self.assertEqual(report["file_bytes"], 32 + 1000 * 92)
            self.assertEqual(report["samples"], 3)
            self.assertEqual(len(report["queries"]), 3)
            for query in report["queries"]:
                self.assertEqual(query["indexed_scanned"], 1)
                self.assertEqual(query["linear_scanned"], 1000)
                self.assertEqual(query["matched"], 1)
                self.assertGreater(query["index_nodes"], 1)
            before = db.read_bytes()
            repeated = subprocess.run([binary, str(sql), str(db), "3"], capture_output=True, timeout=30)
            self.assertNotEqual(repeated.returncode, 0)
            self.assertEqual(db.read_bytes(), before)


if __name__ == "__main__":
    unittest.main(verbosity=2)
