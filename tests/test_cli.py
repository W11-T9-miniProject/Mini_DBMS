import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

INSERT = "INSERT INTO play_history (event_code,user_id,song_id,genre) VALUES ('PLAY1',71,305,'rock');"


class CliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mini-dbms-cli-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.db = self.root / "history.db"
        self.binary = str(Path(os.environ.get("MINI_DBMS_CLI", "build/mini-dbms")).resolve())

    def run_sql(self, text, ok=True):
        sql = self.root / "input.sql"
        sql.write_bytes(text if isinstance(text, bytes) else text.encode())
        result = subprocess.run([self.binary, "--db", str(self.db), str(sql)],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0 if ok else 1, result.stderr)
        return result, [json.loads(line) for line in result.stdout.splitlines()]

    def test_insert_select_and_next_process(self):
        _, output = self.run_sql(INSERT + "\nSELECT * FROM play_history WHERE id=1;")
        row = next(item for item in output if item["type"] == "row")
        self.assertEqual(row["record"], {"id": 1, "event_code": "PLAY1", "user_id": 71,
                                        "song_id": 305, "genre": "rock"})
        self.assertEqual(output[-1]["inserted"], 1)
        _, output = self.run_sql(INSERT + "SELECT * FROM play_history;")
        self.assertEqual([item["record"]["id"] for item in output if item["type"] == "row"], [1, 2])
        self.assertEqual(output[-1]["selected"], 2)

    def test_case_comments_and_multiline(self):
        _, output = self.run_sql("-- initial comment ;\n;\n" + INSERT.lower() + "\n-- query\nSeLeCt *\nFrOm PLAY_HISTORY\nWhErE user_id = 71")
        self.assertEqual(output[-1]["statements"], 2)
        self.assertEqual(output[-1]["selected"], 1)
        self.assertEqual(next(x for x in output if x["type"] == "row")["record"]["event_code"], "play1")

    def test_semicolon_quote_and_json_escaping(self):
        code = 'P;--\'"\\1'
        sql_code = code.replace("'", "''")
        _, output = self.run_sql(INSERT.replace("PLAY1", sql_code) + "SELECT * FROM play_history;")
        self.assertEqual(next(x for x in output if x["type"] == "row")["record"]["event_code"], code)

    def test_error_is_fail_fast_but_not_a_transaction(self):
        result, output = self.run_sql(INSERT + "\nSELECT name FROM play_history;\n" + INSERT, ok=False)
        self.assertIn("line 2", result.stderr)
        self.assertFalse(any(x.get("status") == "ok" for x in output))
        _, output = self.run_sql("SELECT * FROM play_history;")
        self.assertEqual(output[-1]["selected"], 1)

    def test_semantic_error_does_not_append(self):
        self.run_sql(INSERT)
        before = self.db.read_bytes()
        result, _ = self.run_sql(INSERT.replace("'rock'", "''"), ok=False)
        self.assertIn("invalid argument", result.stderr)
        self.assertEqual(self.db.read_bytes(), before)

    def test_unsupported_and_nul(self):
        self.run_sql(INSERT)
        before = self.db.read_bytes()
        for text in ("DROP TABLE play_history;", "SELECT * FROM play_history WHERE id=1 OR id=2;",
                     "SELECT * FROM missing;", b"SELECT\x00 * FROM play_history;"):
            self.run_sql(text, ok=False)
            self.assertEqual(self.db.read_bytes(), before)

    def test_missing_or_oversized_source_and_usage(self):
        missing = self.root / "missing.sql"
        result = subprocess.run([self.binary, "--db", str(self.db), str(missing)], capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 1)
        self.assertFalse(self.db.exists())
        huge = self.root / "huge.sql"
        with huge.open("wb") as stream:
            stream.truncate(256 * 1024 * 1024 + 1)
        result = subprocess.run([self.binary, "--db", str(self.db), str(huge)], capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 1)
        self.assertFalse(self.db.exists())
        result = subprocess.run([self.binary], capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 2)

    def test_corrupt_db_is_preserved(self):
        self.db.write_bytes(b"not a database")
        self.run_sql("SELECT * FROM play_history;", ok=False)
        self.assertEqual(self.db.read_bytes(), b"not a database")

    def test_empty_result_and_comment_only_file(self):
        _, output = self.run_sql("-- no SQL\n")
        self.assertEqual(output[-1]["statements"], 0)
        _, output = self.run_sql("SELECT * FROM play_history WHERE id=999;")
        self.assertFalse(any(x["type"] == "row" for x in output))
        self.assertEqual(output[-1]["selected"], 0)

    def test_genres_preserved_and_exactly_filtered(self):
        template = "INSERT INTO play_history (event_code,user_id,song_id,genre) VALUES ('P',71,305,'{}');"
        genres = ["rock", "lock", "Rock", "k-pop", "r&b/soul", "alternative rock",
                  "jazz", "electronic/dance", "hip-hop/rap", "classical", "X" * 31]
        self.run_sql("\n".join(template.format(genre) for genre in genres))
        _, output = self.run_sql("SELECT * FROM play_history;")
        rows = [item["record"] for item in output if item["type"] == "row"]
        self.assertEqual([row["genre"] for row in rows], genres)
        self.assertTrue(all("played_at" not in row for row in rows))
        _, output = self.run_sql("SELECT * FROM play_history WHERE genre='rock';")
        self.assertEqual(output[-1]["selected"], 1)
        self.assertEqual(next(x for x in output if x["type"] == "row")["record"]["id"], 1)
        _, output = self.run_sql("SELECT * FROM play_history WHERE genre='missing';")
        self.assertEqual(output[-1]["selected"], 0)
        before = self.db.read_bytes()
        for invalid in ("", "X" * 32, "bad\ngenre", "록"):
            self.run_sql(template.format(invalid), ok=False)
            self.assertEqual(self.db.read_bytes(), before)
        self.run_sql("SELECT * FROM play_history WHERE played_at='rock';", ok=False)
        self.assertEqual(self.db.read_bytes(), before)

    def test_old_file_version_is_preserved(self):
        header = bytearray(32)
        header[:8] = b"MJDBMS1\0"
        header[8:12] = (1).to_bytes(4, "little")
        header[12:16] = (80).to_bytes(4, "little")
        checksum = 2166136261
        for byte in header[:28]:
            checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
        header[28:32] = checksum.to_bytes(4, "little")
        self.db.write_bytes(header)
        self.run_sql("SELECT * FROM play_history;", ok=False)
        self.assertEqual(self.db.read_bytes(), header)

    def test_index_access_path_after_reopen(self):
        self.run_sql(INSERT)
        for condition, path, scanned, matched in (("id=1", "bplus_tree", 1, 1),
                                                  ("id=999", "bplus_tree", 0, 0),
                                                  ("genre='rock'", "full_scan", 1, 1)):
            _, output = self.run_sql("SELECT * FROM play_history WHERE " + condition + ";")
            result = next(x for x in output if x["type"] == "select")
            self.assertEqual((result["access_path"], result["scanned"], result["matched"]), (path, scanned, matched))


if __name__ == "__main__":
    unittest.main(verbosity=2)
