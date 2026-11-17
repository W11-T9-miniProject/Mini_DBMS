"""Studio delivery is tested over HTTP; SQL still runs through the real C DLL."""

import http.client
from pathlib import Path
import tempfile
import unittest

from api_support import ServerProcess, request


class StudioHttpTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="minidb-studio-")
        self.path = Path(self.temporary.name) / "database"
        self.server = ServerProcess(self.path)

    def tearDown(self):
        self.server.close()
        self.temporary.cleanup()

    def asset(self, path, method="GET"):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.port, timeout=5)
        try:
            connection.request(method, path)
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def test_studio_assets_and_security_headers(self):
        expected = {"/": "text/html", "/index.html": "text/html",
                    "/assets/studio.css": "text/css", "/assets/studio.js": "text/javascript",
                    "/favicon.svg": "image/svg+xml"}
        for path, content_type in expected.items():
            with self.subTest(path=path):
                status, headers, body = self.asset(path)
                self.assertEqual(status, 200)
                self.assertTrue(headers["Content-Type"].startswith(content_type))
                self.assertEqual(int(headers["Content-Length"]), len(body))
                self.assertEqual(headers["X-Content-Type-Options"], "nosniff")
                self.assertIn("script-src 'self'", headers["Content-Security-Policy"])
                self.assertIn("frame-ancestors 'none'", headers["Content-Security-Policy"])
        body = self.asset("/")[2].decode("utf-8")
        self.assertIn('id="sql-editor"', body)
        self.assertIn("SQL 워크스페이스", body)
        self.assertNotIn("https://", body)  # No external scripts/fonts/services.

    def test_static_routes_cannot_read_workspace_files(self):
        for path in ("/../README.md", "/assets/../api/server.py", "/assets/%2e%2e/api/native.py",
                     "/api/server.py", "/users.db", "/assets/missing.js"):
            with self.subTest(path=path):
                self.assertEqual(self.asset(path)[0], 404)
        self.assertEqual(self.asset("/", "POST")[0], 405)
        status, headers, body = self.asset("/", "HEAD")
        self.assertEqual(status, 200)
        self.assertGreater(int(headers["Content-Length"]), 0)
        self.assertEqual(body, b"")

    def test_page_load_is_read_only_and_query_endpoint_is_real(self):
        self.asset("/")
        self.asset("/assets/studio.js")
        self.assertEqual(request(self.server.port, "GET", "/stats")[1]["engine"]["row_count"], 0)
        sql = "INSERT INTO users(name,age,email) VALUES('O''Brien <script>',25,'a@example.com')"
        self.assertEqual(request(self.server.port, document={"sql": sql})[0], 200)
        result = request(self.server.port, document={"sql": "SELECT * FROM users WHERE id=1"})[1]
        self.assertEqual(result["rows"][0][1], "O'Brien <script>")
        self.assertEqual(result["plan"], "INDEX LOOKUP")
        self.assertEqual(result["examined_rows"], 1)

    def test_studio_can_open_before_entering_token(self):
        self.server.close()
        self.server = ServerProcess(self.path, token="studio-test-token")
        self.assertEqual(self.asset("/")[0], 200)
        self.assertEqual(self.asset("/assets/studio.js")[0], 200)
        self.assertEqual(request(self.server.port, "GET", "/stats")[0], 401)
        self.assertEqual(request(self.server.port, "GET", "/stats",
                                 headers={"Authorization": "Bearer studio-test-token"})[0], 200)

    def test_studio_assets_do_not_use_api_result_byte_limit(self):
        self.server.close()
        self.server = ServerProcess(self.path, "--max-response-bytes", "1024")
        status, _headers, body = self.asset("/")
        self.assertEqual(status, 200)
        self.assertGreater(len(body), 1024)


if __name__ == "__main__":
    unittest.main()
