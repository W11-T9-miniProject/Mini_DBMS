from concurrent.futures import ThreadPoolExecutor
import http.client
import json
import os
from pathlib import Path
import re
import select
import socket
import subprocess
import tempfile
import time
import unittest

INSERT = "INSERT INTO play_history (event_code,user_id,song_id,genre) VALUES ('P',71,108,'rock');"


class Server:
    def __init__(self, db, workers=4, queue=64):
        self.closed = False
        binary = os.environ.get("MINI_DBMS_SERVER", "build/mini-dbms-server")
        self.proc = subprocess.Popen([binary, "--db", str(db), "--port", "0", "--workers", str(workers), "--queue", str(queue)],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if not select.select([self.proc.stdout], [], [], 10)[0]:
            self.proc.kill()
            raise AssertionError("server startup timeout")
        line = self.proc.stdout.readline()
        match = re.search(r"LISTENING 127\.0\.0\.1:(\d+)", line)
        if not match:
            self.proc.kill()
            raise AssertionError(line + self.proc.communicate()[1])
        self.port = int(match[1])

    def close(self):
        if self.closed:
            return
        self.closed = True
        self.proc.terminate()
        try:
            out, err = self.proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise AssertionError("shutdown timeout")
        if self.proc.returncode != 0 or "Sanitizer" in err or "runtime error:" in err:
            raise AssertionError((self.proc.returncode, out, err))

    def request(self, sql=None, path="/sql", method="POST", content_type="text/plain"):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        try:
            conn.request(method, path, body=sql, headers={"Content-Type": content_type})
            response = conn.getresponse()
            return response.status, json.loads(response.read())
        finally:
            conn.close()

    def raw(self, data):
        with socket.create_connection(("127.0.0.1", self.port), timeout=10) as sock:
            sock.sendall(data)
            response = bytearray()
            while True:
                try:
                    chunk = sock.recv(65536)
                except ConnectionResetError:
                    break
                if not chunk:
                    break
                response.extend(chunk)
            return bytes(response)


class HttpTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mini-dbms-http-")
        self.db = Path(self.temp.name) / "history.db"
        self.server = Server(self.db)

    def tearDown(self):
        try:
            self.server.close()
        finally:
            self.temp.cleanup()

    def test_basic_index_scan_and_missing(self):
        status, health = self.server.request(path="/health", method="GET")
        self.assertEqual((status, health["workers"]), (200, 4))
        status, result = self.server.request(INSERT)
        self.assertEqual((status, result["record"]["id"]), (200, 1))
        status, result = self.server.request("SELECT * FROM play_history WHERE id=1;")
        self.assertEqual((status, result["matched"], result["scanned"], result["access_path"]), (200, 1, 1, "bplus_tree"))
        self.assertEqual(result["rows"][0]["genre"], "rock")
        _, result = self.server.request("SELECT * FROM play_history WHERE genre='rock';")
        self.assertEqual(result["access_path"], "full_scan")
        _, result = self.server.request("SELECT * FROM play_history WHERE id=999;")
        self.assertEqual((result["rows"], result["matched"], result["scanned"]), ([], 0, 0))

    def test_reject_before_execution_and_invalid_values(self):
        for sql in ("", "-- comment", "SELECT name FROM play_history;", INSERT+INSERT, INSERT+"bad", "SELECT\x00 * FROM play_history;"):
            status, _ = self.server.request(sql)
            self.assertEqual(status, 400, sql)
        self.assertEqual(self.server.request(INSERT.replace(",71,", ",0,"))[0], 422)
        self.assertEqual(self.server.request(INSERT.replace("'rock'", "''"))[0], 422)
        self.assertEqual(self.server.request("SELECT * FROM play_history;")[1]["matched"], 0)

    def test_routes_types_and_body_limits(self):
        self.assertEqual(self.server.request("x", path="/unknown")[0], 404)
        self.assertEqual(self.server.request(path="/sql", method="GET")[0], 405)
        self.assertEqual(self.server.request(INSERT, content_type="application/json")[0], 415)
        self.assertEqual(self.server.request(" "*16385)[0], 413)

    def test_http_framing(self):
        cases = [(b"POST /sql HTTP/1.1\r\nHost: x\r\n\r\n", 411),
                 (b"POST /sql HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx", 400),
                 (b"POST /sql HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400),
                 (b"POST /sql HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n", 400),
                 (b"POST /sql HTTP/1.1\r\nHost: x\r\nX-Long: "+b"a"*9000+b"\r\n\r\n", 413)]
        for request, expected in cases:
            self.assertIn(f" {expected} ".encode(), self.server.raw(request))
        self.assertEqual(self.server.request("SELECT * FROM play_history;")[1]["matched"], 0)

    def test_parallel_insert_select_and_worker_reuse(self):
        def write(number):
            status, result = self.server.request(INSERT.replace("'P'", f"'P{number}'"))
            self.assertEqual(status, 200)
            read_status, read = self.server.request(f"SELECT * FROM play_history WHERE id={result['record']['id']};")
            self.assertEqual((read_status, read["rows"][0]["event_code"]), (200, f"P{number}"))
            return result["record"]["id"], result["worker_id"]
        with ThreadPoolExecutor(max_workers=16) as executor:
            results = list(executor.map(write, range(120)))
        self.assertEqual(sorted(x[0] for x in results), list(range(1, 121)))
        self.assertGreater(len({x[1] for x in results}), 1)
        self.assertLessEqual(len({x[1] for x in results}), 4)
        _, all_rows = self.server.request("SELECT * FROM play_history;")
        self.assertEqual(all_rows["matched"], 120)

    def test_restart_after_ack_and_aborted_request(self):
        self.assertEqual(self.server.request(INSERT)[0], 200)
        self.server.proc.kill()
        _, err = self.server.proc.communicate(timeout=5)
        self.assertEqual(self.server.proc.returncode, -9)
        self.assertNotIn("Sanitizer", err)
        self.server.closed = True
        self.server = Server(self.db)
        self.assertEqual(self.server.request("SELECT * FROM play_history WHERE id=1;")[1]["rows"][0]["user_id"], 71)
        with socket.create_connection(("127.0.0.1", self.server.port)) as sock:
            sock.sendall(b"POST /sql HTTP/1.1\r\nHost: x\r\nContent-Type: text/plain\r\nContent-Length: 200\r\n\r\nINSERT")
        self.assertEqual(self.server.request("SELECT * FROM play_history;")[1]["matched"], 1)

    def test_fragmented_request_and_json_escape(self):
        sql = INSERT.replace("'P'", "'P''\"\\1'").encode()
        header = f"POST /sql HTTP/1.1\r\nHost: x\r\nContent-Type: text/plain\r\nContent-Length: {len(sql)}\r\n\r\n".encode()
        with socket.create_connection(("127.0.0.1", self.server.port), timeout=10) as sock:
            sock.sendall(header[:10]); sock.sendall(header[10:]+sql[:15]); sock.sendall(sql[15:])
            data = bytearray()
            while chunk := sock.recv(65536):
                data.extend(chunk)
        self.assertIn(b" 200 ", data)
        self.assertEqual(self.server.request("SELECT * FROM play_history;")[1]["rows"][0]["event_code"], 'P\'"\\1')

    def test_extra_bytes_after_fragmented_body_are_rejected(self):
        body = INSERT.encode()
        headers = f"POST /sql HTTP/1.1\r\nHost: x\r\nContent-Type: text/plain\r\nContent-Length: {len(body)}\r\n\r\n".encode()
        with socket.create_connection(("127.0.0.1", self.server.port), timeout=10) as sock:
            sock.sendall(headers)
            time.sleep(.05)
            sock.sendall(body+b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
            reply = bytearray()
            while True:
                try:
                    chunk = sock.recv(65536)
                except ConnectionResetError:
                    break
                if not chunk:
                    break
                reply.extend(chunk)
        self.assertIn(b" 400 ", reply)
        self.assertEqual(self.server.request("SELECT * FROM play_history;")[1]["matched"], 0)

    def test_queue_saturation_and_recovery(self):
        self.server.close(); self.server = Server(self.db, workers=1, queue=1)
        a = socket.create_connection(("127.0.0.1", self.server.port), timeout=10)
        a.sendall(b"GET /health HTTP/1.1\r\n")
        time.sleep(.08)
        b = socket.create_connection(("127.0.0.1", self.server.port), timeout=10)
        b.sendall(b"GET /health HTTP/1.1\r\n")
        try:
            time.sleep(.08)
            reply = self.server.raw(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
            self.assertIn(b" 503 ", reply)
        finally:
            a.close(); b.close()
        time.sleep(.05)
        self.assertEqual(self.server.request(path="/health", method="GET")[0], 200)

    def test_result_limit_is_explicit_and_index_still_works(self):
        with ThreadPoolExecutor(max_workers=16) as executor:
            statuses = list(executor.map(lambda _: self.server.request(INSERT)[0], range(1001)))
        self.assertTrue(all(status == 200 for status in statuses))
        status, result = self.server.request("SELECT * FROM play_history;")
        self.assertEqual((status, result["status"]), (413, "error"))
        self.assertNotIn("rows", result)
        self.assertEqual(self.server.request("SELECT * FROM play_history WHERE id=1001;")[1]["matched"], 1)

    def test_shutdown_closes_pending_and_bounds_partial_request(self):
        self.server.close(); self.server = Server(self.db, workers=1, queue=2)
        a = socket.create_connection(("127.0.0.1", self.server.port), timeout=10)
        b = socket.create_connection(("127.0.0.1", self.server.port), timeout=10)
        try:
            a.sendall(b"GET /health HTTP/1.1\r\n")
            b.sendall(b"GET /health HTTP/1.1\r\n")
            start = time.monotonic(); self.server.close()
            self.assertLess(time.monotonic()-start, 8)
        finally:
            a.close(); b.close()
        self.server = Server(self.db)
        self.assertEqual(self.server.request(path="/health", method="GET")[0], 200)


if __name__ == "__main__":
    unittest.main(verbosity=2)
