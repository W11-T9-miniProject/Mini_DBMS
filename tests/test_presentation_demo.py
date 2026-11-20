import contextlib
import hashlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("presentation", Path(__file__).resolve().parents[1] / "tools/demo_presentation.py")
presentation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(presentation)


class PresentationTests(unittest.TestCase):
    def test_prepare_repeat_demo_and_source_preservation(self):
        with tempfile.TemporaryDirectory(prefix="mini-dbms-presentation-test-") as temp, contextlib.redirect_stdout(io.StringIO()):
            directory = Path(temp)
            presentation.prepare(directory, 1000)
            database = directory / "source.db"
            before = hashlib.sha256(database.read_bytes()).digest()
            presentation.prepare(directory, 1000)
            for _ in range(2):
                result = presentation.run_demo(directory, auto=True)
                self.assertEqual(result["base_records"], 1000)
                self.assertEqual(result["indexed_scanned"], 1)
                self.assertEqual(result["linear_scanned"], 1001)
                self.assertEqual(result["concurrent_inserted"], 24)
                self.assertEqual(result["verified"], 24)
                self.assertEqual(hashlib.sha256(database.read_bytes()).digest(), before)
            interactive = subprocess.run([sys.executable, str(presentation.ROOT / "tools/demo_presentation.py"),
                                          "run", "--directory", str(directory)], input="\n\n\n",
                                         capture_output=True, text=True, timeout=30)
            self.assertEqual(interactive.returncode, 0, interactive.stderr)
            self.assertIn("③ 요청 24개", interactive.stdout)
            self.assertEqual(hashlib.sha256(database.read_bytes()).digest(), before)

    def test_existing_unrecognized_file_is_preserved(self):
        with tempfile.TemporaryDirectory(prefix="mini-dbms-presentation-preserve-") as temp:
            directory = Path(temp)
            (directory / "source.db").write_bytes(b"existing data")
            with self.assertRaises(RuntimeError):
                presentation.prepare(directory, 1000)
            self.assertEqual((directory / "source.db").read_bytes(), b"existing data")


if __name__ == "__main__":
    unittest.main(verbosity=2)
