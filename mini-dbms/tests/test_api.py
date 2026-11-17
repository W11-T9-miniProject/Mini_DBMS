import concurrent.futures
import http.client
import json
import pathlib
import os
import select
import signal
import subprocess
import tempfile
import time
import unittest

BIN = pathlib.Path(__file__).resolve().parents[1] / 'mini_dbms_server'


class ApiTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.db = pathlib.Path(self.tmp.name)
        (self.db / 'people.schema').write_text('id INTEGER\nname TEXT\nage INTEGER\n')
        self.start_server()

    def start_server(self):
        self.server = subprocess.Popen([str(BIN), str(self.db), '0', '4', '16'],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(self.stop_server)
        self.assertTrue(select.select([self.server.stdout], [], [], 5)[0], 'server did not start')
        line = self.server.stdout.readline()
        self.assertTrue(line.startswith('Listening on 127.0.0.1:'), line)
        self.port = int(line.strip().rsplit(':', 1)[1])

    def stop_server(self):
        if self.server.poll() is None:
            os.kill(self.server.pid, signal.SIGTERM)
            deadline = time.monotonic() + 10
            while self.server.poll() is None and time.monotonic() < deadline:
                time.sleep(0.02)
            if self.server.poll() is None:
                self.server.kill()
                self.server.wait()
                self.fail("server did not stop within 10 seconds: " + self.server.stderr.read())
        self.server.stdout.close()
        self.server.stderr.close()

    def request(self, sql):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=10)
        connection.request('POST', '/query', json.dumps({'sql': sql}),
                           {'Content-Type': 'application/json'})
        response = connection.getresponse()
        result = response.status, json.loads(response.read())
        connection.close()
        return result

    def test_insert_select_and_rebuild(self):
        self.assertEqual(self.request("INSERT INTO people (name, age) VALUES ('Mina', 24);")[0], 200)
        self.assertEqual(self.request('SELECT name FROM people WHERE id = 1;'),
                         (200, {'ok': True, 'output': 'Mina\n'}))
        self.stop_server()
        self.start_server()
        self.assertEqual(self.request('SELECT * FROM people;')[1]['output'], '1,Mina,24\n')

    def test_concurrent_inserts_have_unique_ids_and_no_missing_rows(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            results = list(pool.map(lambda i: self.request(
                f"INSERT INTO people (name, age) VALUES ('user{i}', {i});"), range(80)))
        self.assertTrue(all(status == 200 for status, _ in results), results)
        status, result = self.request('SELECT * FROM people;')
        self.assertEqual(status, 200)
        rows = [row.split(',') for row in result['output'].splitlines()]
        self.assertEqual(sorted(int(row[0]) for row in rows), list(range(1, 81)))
        self.assertEqual({row[1] for row in rows}, {f'user{i}' for i in range(80)})

    def test_bad_sql_and_multiple_statements_do_not_write(self):
        for sql in ['DELETE FROM people;', '', "INSERT INTO people VALUES (1, 'bad', 1); SELECT * FROM people;",
                    "INSERT INTO people VALUES (1, 'bad', 1); trailing"]:
            self.assertEqual(self.request(sql)[0], 400)
        self.assertEqual(self.request('SELECT * FROM people;')[1]['output'], '')

    def raw_request(self, raw):
        import socket
        with socket.create_connection(('127.0.0.1', self.port), timeout=6) as connection:
            connection.sendall(raw)
            connection.shutdown(socket.SHUT_WR)
            data = b''
            while True:
                part = connection.recv(65536)
                if not part:
                    return data
                data += part

    def test_malformed_http_and_json_are_rejected(self):
        cases = [
            (b'GET /query HTTP/1.1\r\n\r\n', 405),
            (b'POST /wrong HTTP/1.1\r\n\r\n', 404),
            (b'POST /query HTTP/1.1\r\nContent-Type: text/plain\r\n\r\n', 415),
            (b'POST /query HTTP/1.1\r\nContent-Length: -1\r\n\r\n', 400),
            (b'POST /query HTTP/1.1\r\nContent-Length: 999999\r\n\r\n', 413),
            (b'POST /query HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n', 400),
            (b'POST /query HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx', 400),
        ]
        for request, status in cases:
            with self.subTest(request=request):
                response = self.raw_request(request)
                self.assertEqual(int(response.split(b' ', 2)[1]), status)
        for body in [b'{}', b'{"sql":123}', b'{"sql":"x","sql":"y"}',
                     b'{"sql":"\\u0000"}', b'{"sql":"\\ud800"}', b'{"sql":"x"}garbage', b'{"sql":"\xff"}']:
            request = (b'POST /query HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: ' +
                       str(len(body)).encode() + b'\r\n\r\n' + body)
            self.assertEqual(int(self.raw_request(request).split(b' ', 2)[1]), 400)

    def test_partial_socket_reads_and_unicode_and_quoted_semicolon(self):
        import socket
        sql = "INSERT INTO people (name, age) VALUES ('미나;😀', 24);"
        body = json.dumps({'sql': sql}).encode()
        header = (b'POST /query HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: ' +
                  str(len(body)).encode() + b'\r\n\r\n')
        with socket.create_connection(('127.0.0.1', self.port), timeout=6) as connection:
            for chunk in [header[:12], header[12:], body[:8], body[8:]]:
                connection.sendall(chunk)
                time.sleep(.01)
            response = http.client.HTTPResponse(connection)
            response.begin()
            self.assertEqual(response.status, 200, response.read())
        self.assertEqual(self.request('SELECT name FROM people WHERE id = 1;')[1]['output'], '미나;😀\n')

    def test_simultaneous_reads_and_writes_and_cli_exclusion(self):
        query_file = self.db / 'query.sql'
        query_file.write_text('SELECT * FROM people;')
        cli = subprocess.run([str(BIN.with_name('mini_dbms')), str(self.db), str(query_file)],
                             capture_output=True, text=True)
        self.assertNotEqual(cli.returncode, 0)
        self.assertIn('locked', cli.stderr)
        def task(i):
            sql = (f"INSERT INTO people (name, age) VALUES ('user{i}', {i});" if i % 2 == 0
                   else 'SELECT * FROM people;')
            return self.request(sql)
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            results = list(pool.map(task, range(60)))
        self.assertTrue(all(status == 200 for status, _ in results))
        rows = self.request('SELECT id FROM people;')[1]['output'].splitlines()
        self.assertEqual(sorted(map(int, rows)), list(range(1, 31)))

    def test_bounded_queue_overload_and_shutdown_with_slow_clients(self):
        import socket
        connections = []
        try:
            for _ in range(30):
                connection = socket.create_connection(('127.0.0.1', self.port), timeout=2)
                connection.sendall(b'POST /query HTTP/1.1\r\n')
                connections.append(connection)
            available, _, _ = select.select(connections, [], [], 2)
            responses = [connection.recv(4096) for connection in available]
            self.assertTrue(any(b'503' in response.split(b'\r\n')[0] for response in responses))
            started = time.monotonic()
            self.stop_server()
            self.assertLess(time.monotonic() - started, 7)
        finally:
            for connection in connections:
                connection.close()


    def test_oversized_query_result_is_bounded_and_server_stays_usable(self):
        self.assertEqual(self.request("INSERT INTO people (name, age) VALUES ('" + 'a' * 40000 + "', 24);")[0], 200)
        sql = 'SELECT ' + ','.join(['name'] * 128) + ' FROM people;'
        self.assertEqual(self.request(sql)[0], 413)
        self.assertEqual(self.request('SELECT age FROM people WHERE id = 1;'),
                         (200, {'ok': True, 'output': '24\n'}))
