import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class StorageProcessTests(unittest.TestCase):
    def test_separate_processes_preserve_rows_and_ids(self):
        binary = os.environ.get("MINI_DBMS_DEMO", "build/storage-demo")
        with tempfile.TemporaryDirectory(prefix="mini-dbms-process-") as directory:
            path = Path(directory) / "play_history.db"
            def run(mode):
                return subprocess.run([binary, mode, str(path)], check=True,
                                      capture_output=True, text=True, timeout=15).stdout
            self.assertIn("SAVED id=1 offset=32", run("write"))
            self.assertIn("1\tPLAY000001\t71\t305\trock", run("read"))
            self.assertIn("SAVED id=2 offset=124", run("write"))
            rows = run("read").strip().splitlines()
            self.assertEqual(len(rows), 3)
            self.assertTrue(rows[2].startswith("2\tPLAY000001\t"))
            self.assertEqual(path.stat().st_size, 32 + 2 * 92)


if __name__ == "__main__":
    unittest.main(verbosity=2)
