from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import tempfile
import unittest

from api.native import NativeEngine, NativeError, default_library_path


class NativeBridgeTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="minidb-한글-")
        self.path = Path(self.directory.name) / "database"

    def tearDown(self):
        self.directory.cleanup()

    def test_existing_c_engine_projection_unicode_and_restart(self):
        with NativeEngine(self.path) as engine:
            inserted = engine.execute("INSERT INTO users(name,age) VALUES('홍길동',25),('O''Brien; Kim',30)")
            self.assertEqual((inserted["affected_rows"], inserted["first_insert_id"]), (2, 1))
            selected = engine.execute("SELECT name,id FROM users WHERE id=1")
            self.assertEqual(selected["columns"], ["name", "id"])
            self.assertEqual(selected["rows"], [["홍길동", 1]])
            self.assertEqual(selected["plan"], "INDEX LOOKUP")
            self.assertEqual(selected["examined_rows"], 1)
            self.assertEqual(engine.stats()["row_count"], 2)
        with NativeEngine(self.path) as engine:
            self.assertEqual(engine.execute("SELECT name FROM users WHERE id=2")["rows"], [["O'Brien; Kim"]])
            self.assertEqual(engine.execute("INSERT INTO users(name,age) VALUES('next',1)")["first_insert_id"], 3)

    def test_result_limit_aborts_select_without_poisoning_engine(self):
        with NativeEngine(self.path) as engine:
            engine.execute("INSERT INTO users(name,age) VALUES('a',1),('b',2)")
            with self.assertRaises(NativeError) as error:
                engine.execute("SELECT * FROM users", max_rows=1)
            self.assertEqual(error.exception.code, "RESULT_TOO_LARGE")
            self.assertEqual(engine.execute("SELECT * FROM users LIMIT 1", max_rows=1)["row_count"], 1)
            self.assertEqual(engine.stats()["row_count"], 2)

    def test_bad_sql_cannot_partially_insert(self):
        with NativeEngine(self.path) as engine:
            with self.assertRaises(NativeError) as error:
                engine.execute("INSERT INTO users(name,age) VALUES('a',1),('b','x')")
            self.assertEqual(error.exception.code, "SQL_ERROR")
            self.assertEqual(engine.stats()["row_count"], 0)
            for sql in ("", " \t ", "SELECT * FROM users\0; DROP", 123):
                with self.assertRaises(NativeError):
                    engine.execute(sql)

    def test_exclusive_process_lock_and_close_idempotency(self):
        engine = NativeEngine(self.path)
        try:
            with self.assertRaises(NativeError) as error:
                NativeEngine(self.path)
            self.assertEqual(error.exception.code, "BUSY")
        finally:
            engine.close()
            engine.close()
        with self.assertRaises(NativeError):
            engine.stats()

    def test_parallel_calls_use_one_native_handle(self):
        with NativeEngine(self.path) as engine:
            def insert(number):
                result = engine.execute(f"INSERT INTO users(name,age) VALUES('thread_{number}',{number})")
                row = engine.execute(f"SELECT name FROM users WHERE id={result['first_insert_id']}")
                self.assertEqual(row["rows"], [[f"thread_{number}"]])
                return result["first_insert_id"]
            with ThreadPoolExecutor(max_workers=8) as workers:
                ids = list(workers.map(insert, range(80)))
            self.assertEqual(sorted(ids), list(range(1, 81)))
        with NativeEngine(self.path) as engine:
            self.assertEqual(engine.stats()["row_count"], 80)

    def test_abi_sizes_checked_on_real_library(self):
        with NativeEngine(self.path, default_library_path()) as engine:
            self.assertEqual(engine._library.db_abi_version(), 1)
            self.assertEqual(engine._library.db_abi_sizeof(999), 0)


if __name__ == "__main__":
    unittest.main()
