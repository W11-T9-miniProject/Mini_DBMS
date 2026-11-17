from concurrent.futures import ThreadPoolExecutor
import http.client
from pathlib import Path
import socket
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from api.native import NativeEngine
from api.pool import BoundedWorkerPool
from api.server import ApiHandler, ApiServer, Settings
from api_support import ServerProcess, raw_request, request


def wait_for(predicate, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.005)
    raise AssertionError("Condition did not become true before deadline")


class PoolTests(unittest.TestCase):
    def test_fixed_workers_queue_full_reuse_and_drain(self):
        release = threading.Event()
        seen = []
        guard = threading.Lock()
        def process(task):
            release.wait(5)
            with guard:
                seen.append((task, threading.current_thread().name))
        pool = BoundedWorkerPool(2, 1, process)
        try:
            self.assertTrue(pool.try_submit(1))
            wait_for(lambda: pool.stats()["active"] == 1)
            self.assertTrue(pool.try_submit(2))
            wait_for(lambda: pool.stats()["active"] == 2)
            self.assertTrue(pool.try_submit(3))
            self.assertFalse(pool.try_submit(4))
            self.assertEqual(pool.stats()["queued"], 1)
        finally:
            release.set()
            pool.close()
        self.assertEqual(sorted(task for task, _name in seen), [1, 2, 3])
        self.assertLessEqual(len({name for _task, name in seen}), 2)
        self.assertEqual(pool.stats()["peak_active"], 2)
        self.assertEqual(pool.stats()["completed"], 3)
        self.assertFalse(pool.try_submit(5))
        self.assertFalse(any(thread.is_alive() for thread in pool._threads))

    def test_worker_exception_does_not_destroy_pool(self):
        completed = threading.Event()
        def process(task):
            if task == "bad":
                raise RuntimeError("injected worker failure")
            completed.set()
        pool = BoundedWorkerPool(1, 2, process)
        try:
            with self.assertLogs(level="ERROR"):
                self.assertTrue(pool.try_submit("bad"))
                self.assertTrue(pool.try_submit("good"))
                self.assertTrue(completed.wait(5))
        finally:
            pool.close()
        self.assertEqual(pool.stats()["completed"], 2)


class RealHttpTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="minidb-http-")
        self.path = Path(self.temporary.name) / "database"
        self.server = None

    def tearDown(self):
        if self.server:
            self.server.close()
        self.temporary.cleanup()

    def start(self, *options, token=None):
        self.server = ServerProcess(self.path, *options, token=token)
        return self.server.port

    def query(self, sql, **kwargs):
        return request(self.server.port, document={"sql": sql}, **kwargs)

    def test_http_insert_select_index_explain_and_restart(self):
        self.start()
        self.assertEqual(request(self.server.port, "GET", "/health")[0], 200)
        status, inserted, _ = self.query("INSERT INTO users(name,age) VALUES('홍길동',25),('Bob',30)")
        self.assertEqual(status, 200)
        self.assertEqual((inserted["affected_rows"], inserted["first_insert_id"]), (2, 1))
        status, selected, headers = self.query("SELECT name,id FROM users WHERE id=1")
        self.assertEqual((status, selected["rows"]), (200, [["홍길동", 1]]))
        self.assertEqual(selected["plan"], "INDEX LOOKUP")
        self.assertTrue(headers["X-Worker-Name"].startswith("db-worker-"))
        self.assertEqual(self.query("SELECT * FROM users WHERE age=30")[1]["plan"], "TABLE SCAN")
        self.assertEqual(self.query("SELECT * FROM users WHERE id BETWEEN 1 AND 2")[1]["plan"], "INDEX RANGE SCAN")
        self.assertTrue(self.query("EXPLAIN SELECT * FROM users WHERE id=1")[1]["explain"])
        self.server.close()
        self.server = None
        self.start()
        self.assertEqual(self.query("SELECT name FROM users WHERE id=1")[1]["rows"], [["홍길동"]])
        self.assertEqual(self.query("INSERT INTO users(name,age) VALUES('Next',1)")[1]["first_insert_id"], 3)

    def test_parallel_http_insert_and_read_no_lost_rows(self):
        self.start("--workers", "4", "--queue-size", "64")
        def insert(number):
            status, result, _headers = self.query(f"INSERT INTO users(name,age) VALUES('http_{number}',{number})")
            self.assertEqual(status, 200)
            row_id = result["first_insert_id"]
            status, found, _headers = self.query(f"SELECT name FROM users WHERE id={row_id}")
            self.assertEqual((status, found["rows"]), (200, [[f"http_{number}"]]))
            return row_id, result["request"]["worker"]
        with ThreadPoolExecutor(max_workers=16) as clients:
            results = list(clients.map(insert, range(120)))
        self.assertEqual(sorted(row_id for row_id, _worker in results), list(range(1, 121)))
        self.assertLessEqual(len({worker for _row_id, worker in results}), 4)
        self.assertGreater(len({worker for _row_id, worker in results}), 1)
        status, stats, _ = request(self.server.port, "GET", "/stats")
        self.assertEqual((status, stats["engine"]["row_count"]), (200, 120))
        self.assertLessEqual(stats["pool"]["peak_active"], 4)
        self.assertGreater(stats["pool"]["peak_active"], 1)
        self.server.close()
        self.server = None
        with NativeEngine(self.path) as engine:
            self.assertEqual(engine.stats()["row_count"], 120)

    def test_sql_and_json_error_mapping(self):
        self.start()
        status, response, _ = self.query("SELECT * FROM unknown")
        self.assertEqual((status, response["error"]["code"]), (422, "SQL_ERROR"))
        for body in (b"{", b"[]", b'{"sql":12}', b'{"sql":""}', b'{"sql":"SELECT * FROM users","extra":1}',
                     b'{"sql":"a","sql":"b"}', b'{"sql":NaN}', b'{"sql":"SELECT\\u0000"}',
                     b'{"sql":"\\ud800"}', b'{"sql":"\xff"}', b"[" * 2000):
            with self.subTest(body=body[:40]):
                self.assertEqual(request(self.server.port, body=body)[0], 400)
        self.assertEqual(request(self.server.port, "GET", "/unknown")[0], 404)
        self.assertEqual(request(self.server.port, "GET", "/query")[0], 405)
        self.assertEqual(request(self.server.port, document={"sql": "SELECT * FROM users"},
                                 headers={"Content-Type": "text/plain"})[0], 415)

    def test_request_framing_and_limits(self):
        self.start("--max-body-bytes", "1000")
        prefix = b"POST /query HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
        requests = [
            (prefix + b"\r\n", 411),
            (prefix + b"Content-Length: -1\r\n\r\n", 400),
            (prefix + b"Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}", 400),
            (prefix + b"Transfer-Encoding: chunked\r\nContent-Length: 2\r\n\r\n{}", 400),
            (prefix + b"Content-Length: 1001\r\n\r\n", 413),
            (prefix + b"Content-Length: 20\r\nExpect: 100-continue\r\n\r\n", 417),
            (b"GET /" + b"x" * 5000 + b" HTTP/1.1\r\n\r\n", 414),
            (prefix + b"X-Padding: " + b"x" * 17000 + b"\r\n\r\n", 431),
        ]
        for data, expected in requests:
            with self.subTest(status=expected, size=len(data)):
                self.assertEqual(raw_request(self.server.port, data)[0], expected)
        self.assertEqual(raw_request(self.server.port, prefix + b"Content-Length: 10\r\n\r\n{}",
                                     shutdown_write=True)[0], 400)

    def test_result_row_limit_preserves_data(self):
        self.start("--max-rows", "2")
        self.assertEqual(self.query("INSERT INTO users(name,age) VALUES('a',1),('b',2),('c',3)")[0], 200)
        status, error, _ = self.query("SELECT * FROM users")
        self.assertEqual((status, error["error"]["code"]), (413, "RESULT_TOO_LARGE"))
        self.assertEqual(self.query("SELECT * FROM users LIMIT 2")[1]["row_count"], 2)
        self.assertEqual(request(self.server.port, "GET", "/stats")[1]["engine"]["row_count"], 3)

    def test_response_byte_limit(self):
        self.start("--max-response-bytes", "1024")
        values = ",".join(f"('{('n' * 60)}',{i})" for i in range(30))
        self.assertEqual(self.query("INSERT INTO users(name,age) VALUES " + values)[0], 200)
        self.assertEqual(self.query("SELECT * FROM users")[0], 413)
        self.assertEqual(self.query("SELECT * FROM users LIMIT 1")[0], 200)

    def test_optional_token(self):
        self.start(token="test-secret")
        self.assertEqual(request(self.server.port, "GET", "/health")[0], 200)
        self.assertEqual(self.query("SELECT * FROM users")[0], 401)
        self.assertEqual(self.query("SELECT * FROM users", headers={"Authorization": "Bearer wrong"})[0], 401)
        self.assertEqual(self.query("SELECT * FROM users", headers={"Authorization": "Bearer test-secret"})[0], 200)
        self.assertEqual(request(self.server.port, "GET", "/stats")[0], 401)

    def test_deadline_releases_slow_client_worker(self):
        self.start("--workers", "1", "--request-timeout", "0.2")
        with socket.create_connection(("127.0.0.1", self.server.port), timeout=3) as connection:
            connection.sendall(b"POST /query HTTP/1.1\r\nHost: localhost\r\n")
            import http.client
            response = http.client.HTTPResponse(connection)
            response.begin()
            self.assertEqual(response.status, 408)
            response.read()
        self.assertEqual(request(self.server.port, "GET", "/health")[0], 200)

    def test_absolute_deadline_is_not_reset_by_trickled_bytes(self):
        self.start("--workers", "1", "--request-timeout", "0.2")
        with socket.create_connection(("127.0.0.1", self.server.port), timeout=3) as connection:
            connection.sendall(b"POST /query HTTP/1.1\r\nHost: localhost\r\nX-Slow: ")
            stop = threading.Event()
            def trickle():
                while not stop.wait(0.03):
                    try:
                        connection.sendall(b"x")
                    except OSError:
                        return
            sender = threading.Thread(target=trickle)
            sender.start()
            try:
                response = http.client.HTTPResponse(connection)
                response.begin()
                self.assertEqual(response.status, 408)
                response.read()
            finally:
                stop.set()
                sender.join(2)
        self.assertEqual(request(self.server.port, "GET", "/health")[0], 200)


class ServerLifecycleTests(unittest.TestCase):
    def test_send_timeout_does_not_claim_insert_was_not_executed(self):
        with tempfile.TemporaryDirectory() as directory:
            with NativeEngine(Path(directory) / "database") as engine:
                server = ApiServer(("127.0.0.1", 0), engine, Settings())
                acceptor = threading.Thread(target=server.serve_until_stopped)
                original = ApiHandler.send_json
                def fail_send(handler, status, payload, **options):
                    if handler.path == "/query" and status == 200:
                        handler.response_started = True
                        raise TimeoutError("Injected response-send timeout")
                    return original(handler, status, payload, **options)
                acceptor.start()
                try:
                    with patch.object(ApiHandler, "send_json", fail_send):
                        with self.assertRaises(http.client.RemoteDisconnected):
                            request(server.server_port, document={"sql": "INSERT INTO users(name,age) VALUES('saved',1)"})
                    self.assertEqual(engine.stats()["row_count"], 1)
                    self.assertEqual(engine.execute("SELECT name FROM users WHERE id=1")["rows"], [["saved"]])
                finally:
                    server.request_stop()
                    acceptor.join(3)
                    server.server_close()

    def test_queue_saturation_and_graceful_drain(self):
        class BlockingEngine:
            def __init__(self):
                self.started = threading.Event()
                self.release = threading.Event()
                self.calls = 0
            def execute(self, sql, **_options):
                self.calls += 1
                self.started.set()
                self.release.wait(5)
                return {"rows": [], "affected_rows": 0}
        engine = BlockingEngine()
        server = ApiServer(("127.0.0.1", 0), engine, Settings(workers=1, queue_size=1))
        acceptor = threading.Thread(target=server.serve_until_stopped)
        acceptor.start()
        with ThreadPoolExecutor(max_workers=2) as clients:
            first = clients.submit(request, server.server_port, document={"sql": "first"})
            try:
                self.assertTrue(engine.started.wait(3))
                second = clients.submit(request, server.server_port, document={"sql": "second"})
                wait_for(lambda: server.pool.stats()["queued"] == 1)
                status, error, _ = request(server.server_port, document={"sql": "rejected"})
                self.assertEqual((status, error["error"]["code"]), (503, "SERVER_BUSY"))
                self.assertEqual(engine.calls, 1)
                self.assertEqual(server.pool.stats()["rejected"], 1)
                server.request_stop()
                acceptor.join(3)
                self.assertFalse(acceptor.is_alive())
            finally:
                engine.release.set()
                server.request_stop()
                acceptor.join(3)
                server.server_close()
            self.assertEqual(first.result(3)[0], 200)
            self.assertEqual(second.result(3)[0], 200)
        self.assertEqual(engine.calls, 2)
        self.assertFalse(any(worker.is_alive() for worker in server.pool._threads))

    def test_expired_queued_request_never_calls_engine(self):
        class SlowEngine:
            def __init__(self):
                self.started = threading.Event()
                self.release = threading.Event()
                self.calls = 0
            def execute(self, *_args, **_kwargs):
                self.calls += 1
                self.started.set()
                self.release.wait(3)
                return {"rows": []}
        engine = SlowEngine()
        server = ApiServer(("127.0.0.1", 0), engine, Settings(workers=1, queue_size=1, request_timeout=0.1))
        acceptor = threading.Thread(target=server.serve_until_stopped)
        acceptor.start()
        try:
            with ThreadPoolExecutor(max_workers=2) as clients:
                first = clients.submit(request, server.server_port, document={"sql": "first"})
                self.assertTrue(engine.started.wait(3))
                second = clients.submit(request, server.server_port, document={"sql": "expires"})
                wait_for(lambda: server.pool.stats()["queued"] == 1)
                time.sleep(0.15)
                engine.release.set()
                self.assertEqual(first.result(3)[0], 200)
                self.assertEqual(second.result(3)[0], 408)
            self.assertEqual(engine.calls, 1)
        finally:
            engine.release.set()
            server.request_stop()
            acceptor.join(3)
            server.server_close()


if __name__ == "__main__":
    unittest.main()
