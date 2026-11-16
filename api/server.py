"""HTTP transport: accept -> bounded queue -> fixed worker -> shared C engine."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, HTTPServer
import hmac
import io
import json
import logging
import math
import os
from pathlib import Path
import re
import signal
import socket
import threading
import time

from .http_io import DeadlineSocketReader, HeadersTooLarge
from .native import NativeEngine, NativeError
from .pool import BoundedWorkerPool


@dataclass(frozen=True)
class Settings:
    workers: int = 4
    queue_size: int = 64
    request_timeout: float = 5.0
    send_timeout: float = 2.0
    max_body_bytes: int = 65536
    max_rows: int = 1000
    max_response_bytes: int = 1048576
    token: str | None = None
    log_requests: bool = False

    def __post_init__(self):
        for name, minimum, maximum in (("workers", 1, 64), ("queue_size", 1, 4096),
                ("max_body_bytes", 1, 4 * 1024 * 1024), ("max_rows", 1, 10000),
                ("max_response_bytes", 1024, 16 * 1024 * 1024)):
            value = getattr(self, name)
            if type(value) is not int or not minimum <= value <= maximum:
                raise ValueError(f"{name} must be in [{minimum}, {maximum}]")
        for name in ("request_timeout", "send_timeout"):
            value = getattr(self, name)
            if not math.isfinite(value) or not 0.05 <= value <= 120:
                raise ValueError(f"{name} must be finite and in [0.05, 120]")


@dataclass(frozen=True)
class Connection:
    socket: socket.socket
    address: tuple
    accepted_at: float


def problem(code: str, message: str) -> dict:
    return {"error": {"code": code, "message": message}}


def send_connection_error(connection: socket.socket, status: int, code: str, message: str) -> None:
    body = json.dumps(problem(code, message), separators=(",", ":")).encode("utf-8")
    reason = {408: "Request Timeout", 503: "Service Unavailable"}[status]
    headers = (f"HTTP/1.1 {status} {reason}\r\nContent-Type: application/json\r\n"
               f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n").encode("ascii")
    try:
        connection.settimeout(0.05)
        connection.sendall(headers + body)
        # Closing with unread request bytes can send RST and erase the HTTP
        # rejection on Windows. Half-close output, then discard a bounded
        # amount of input while the client receives the error and closes.
        connection.shutdown(socket.SHUT_WR)
        drain_deadline = time.monotonic() + 0.05
        discarded = 0
        while discarded < 65536:
            remaining = drain_deadline - time.monotonic()
            if remaining <= 0:
                break
            connection.settimeout(remaining)
            chunk = connection.recv(min(4096, 65536 - discarded))
            if not chunk:
                break
            discarded += len(chunk)
    except OSError:
        pass  # A peer that cannot read the rejection is simply disconnected.


class ApiServer(HTTPServer):
    allow_reuse_address = True
    request_queue_size = 128  # OS listen backlog, separate from the worker queue.

    def __init__(self, address: tuple[str, int], engine: NativeEngine, settings: Settings):
        self.engine = engine
        self.settings = settings
        self.stop_event = threading.Event()
        self._closed = False
        super().__init__(address, ApiHandler)
        self.timeout = 0.1
        try:
            self.pool = BoundedWorkerPool(settings.workers, settings.queue_size, self._serve_connection)
        except Exception:
            super().server_close()
            raise

    def process_request(self, request, client_address):
        # Acceptor does not read HTTP or wait for a free worker.
        connection = Connection(request, client_address, time.monotonic())
        if not self.pool.try_submit(connection):
            send_connection_error(request, 503, "SERVER_BUSY", "Worker queue is full; SQL was not executed")
            self.shutdown_request(request)

    def _serve_connection(self, connection: Connection):
        try:
            if time.monotonic() >= connection.accepted_at + self.settings.request_timeout:
                send_connection_error(connection.socket, 408, "REQUEST_TIMEOUT", "Request expired in queue; SQL was not executed")
            else:
                ApiHandler(connection.socket, connection.address, self, accepted_at=connection.accepted_at)
        except (OSError, ConnectionError):
            pass
        finally:
            self.shutdown_request(connection.socket)

    def serve_until_stopped(self):
        # Unlike shutdown() from the serve_forever thread, setting this event
        # cannot deadlock. The owner closes the server after this loop exits.
        while not self.stop_event.is_set():
            self.handle_request()

    def request_stop(self):
        self.stop_event.set()

    def server_close(self):
        if self._closed:
            return
        self._closed = True
        super().server_close()
        if hasattr(self, "pool"):
            self.pool.close()  # Drain admitted work and join before engine.close().


def unique_json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def reject_json_constant(value):
    raise ValueError(f"Nonstandard JSON constant: {value}")


class ApiHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "MiniDB/1.0"
    sys_version = ""
    disable_nagle_algorithm = True

    def __init__(self, request, client_address, server, *, accepted_at):
        self.accepted_at = accepted_at
        self.queue_wait_ms = (time.monotonic() - accepted_at) * 1000
        super().__init__(request, client_address, server)

    def setup(self):
        super().setup()
        self.rfile.close()
        self.rfile = io.BufferedReader(DeadlineSocketReader(
            self.connection, self.accepted_at + self.server.settings.request_timeout, 16384))

    def handle(self):
        # Exactly one request per TCP connection; idle keep-alive clients cannot
        # monopolize pool workers. No chunked transfer or pipelining is offered.
        self.close_connection = True
        self.response_started = False
        self.sql_started = False
        self.requestline, self.request_version, self.command = "", "HTTP/1.1", None
        try:
            self.raw_requestline = self.rfile.readline(4097)
            if not self.raw_requestline:
                return
            if len(self.raw_requestline) > 4096:
                self.send_problem(414, "URI_TOO_LONG", "Request line exceeds 4096 bytes")
                return
            if self.parse_request():
                self._dispatch()
        except HeadersTooLarge as error:
            self.send_problem(431, "HEADERS_TOO_LARGE", str(error))
        except TimeoutError:
            if not self.response_started and not self.sql_started:
                self.send_problem(408, "REQUEST_TIMEOUT", "Request input deadline expired; SQL was not executed")
            # A send timeout after SQL execution has an unknown outcome to the
            # client. Never claim that its INSERT was not executed.
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass
        except Exception:
            logging.exception("Unexpected HTTP request failure")
            if not self.response_started:
                self.send_problem(500, "INTERNAL_ERROR", "Unexpected server failure")
        finally:
            self.close_connection = True

    def handle_expect_100(self):
        self.send_problem(417, "EXPECTATION_FAILED", "Expect/100-continue is not supported")
        return False

    def send_error(self, code, message=None, explain=None):
        self.send_problem(code, "HTTP_ERROR", str(message or "Malformed HTTP request"))

    def version_string(self):
        return self.server_version

    def log_message(self, format, *args):
        if self.server.settings.log_requests:
            logging.info("%s %s", self.client_address[0], format % args)

    def send_json(self, status, payload, *, extra_headers=None):
        body = json.dumps(payload, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8")
        if len(body) > self.server.settings.max_response_bytes:
            status = 413
            body = json.dumps(problem("RESULT_TOO_LARGE", "Response exceeds byte limit; use SQL LIMIT")).encode("utf-8")
        # Send has a separate timeout. It does not hold engine or queue locks.
        self.connection.settimeout(self.server.settings.send_timeout)
        if self.request_version == "HTTP/0.9":
            self.request_version = "HTTP/1.1"
        self.response_started = True
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Worker-Name", threading.current_thread().name)
        for key, value in (extra_headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def send_problem(self, status, code, message, *, extra_headers=None):
        self.send_json(status, problem(code, message), extra_headers=extra_headers)

    def _authorized(self):
        token = self.server.settings.token
        if token is None:
            return True
        values = self.headers.get_all("Authorization", [])
        actual = values[0] if len(values) == 1 else ""
        if hmac.compare_digest(actual.encode("utf-8"), f"Bearer {token}".encode("utf-8")):
            return True
        self.send_problem(401, "UNAUTHORIZED", "A valid bearer token is required",
                          extra_headers={"WWW-Authenticate": "Bearer"})
        return False

    def _dispatch(self):
        if self.request_version not in ("HTTP/1.0", "HTTP/1.1"):
            self.send_problem(505, "HTTP_VERSION", "Only HTTP/1.0 and HTTP/1.1 are supported")
            return
        routes = {"/health": "GET", "/stats": "GET", "/query": "POST"}
        if self.path not in routes:
            self.send_problem(404, "NOT_FOUND", "Unknown endpoint")
            return
        if self.command != routes[self.path]:
            self.send_problem(405, "METHOD_NOT_ALLOWED", "Unsupported method for this endpoint",
                              extra_headers={"Allow": routes[self.path]})
            return
        if self.path == "/health":
            self.send_json(200, {"status": "ok"})
        elif self._authorized():
            if self.path == "/stats":
                self.send_json(200, {"engine": self.server.engine.stats(), "pool": self.server.pool.stats()})
            else:
                self._execute_query()

    def _read_sql(self):
        if self.headers.get_all("Transfer-Encoding"):
            self.send_problem(400, "TRANSFER_ENCODING", "Chunked/transfer-encoded requests are not supported")
            return None
        lengths = self.headers.get_all("Content-Length", [])
        if not lengths:
            self.send_problem(411, "LENGTH_REQUIRED", "Content-Length is required")
            return None
        if len(lengths) != 1 or not re.fullmatch(r"[0-9]{1,10}", lengths[0]):
            self.send_problem(400, "INVALID_LENGTH", "Expected one nonnegative Content-Length")
            return None
        length = int(lengths[0])
        if length > self.server.settings.max_body_bytes:
            self.send_problem(413, "BODY_TOO_LARGE", "Request body exceeds configured byte limit")
            return None
        content_types = self.headers.get_all("Content-Type", [])
        if len(content_types) != 1 or content_types[0].split(";", 1)[0].strip().lower() != "application/json":
            self.send_problem(415, "CONTENT_TYPE", "Content-Type must be application/json (UTF-8)")
            return None
        body = self.rfile.read(length)
        if len(body) != length:
            self.send_problem(400, "INCOMPLETE_BODY", "Request body ended before Content-Length")
            return None
        try:
            document = json.loads(body.decode("utf-8"), object_pairs_hook=unique_json_object,
                                  parse_constant=reject_json_constant)
            if type(document) is not dict or set(document) != {"sql"}:
                raise ValueError("Expected exactly one JSON field: sql")
            sql = document["sql"]
            if not isinstance(sql, str) or not sql.strip() or "\0" in sql:
                raise ValueError("sql must be a nonempty string without NUL bytes")
            sql.encode("utf-8", errors="strict")
            return sql
        except (ValueError, UnicodeError, RecursionError) as error:
            self.send_problem(400, "INVALID_REQUEST", str(error))
            return None

    def _execute_query(self):
        sql = self._read_sql()
        if sql is None:
            return
        if time.monotonic() >= self.accepted_at + self.server.settings.request_timeout:
            self.send_problem(408, "REQUEST_TIMEOUT", "Request input deadline expired; SQL was not executed")
            return
        start = time.perf_counter()
        self.sql_started = True
        try:
            result = self.server.engine.execute(sql, max_rows=self.server.settings.max_rows)
        except NativeError as error:
            status = {"SQL_ERROR": 422, "INVALID_SQL": 400, "RESULT_TOO_LARGE": 413,
                      "ENGINE_CLOSED": 503, "IO_ERROR": 503, "BUSY": 503}.get(error.code, 500)
            self.send_problem(status, error.code, str(error))
            return
        result["request"] = {"worker": threading.current_thread().name,
                             "queue_wait_ms": self.queue_wait_ms,
                             "execution_ms": (time.perf_counter() - start) * 1000}
        self.send_json(200, result)


def parse_arguments(argv=None):
    parser = argparse.ArgumentParser(description="Python HTTP API + shared C mini DBMS")
    parser.add_argument("--db", default="data")
    parser.add_argument("--library", type=Path)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    for name in ("workers", "queue_size", "max_body_bytes", "max_rows", "max_response_bytes"):
        parser.add_argument("--" + name.replace("_", "-"), type=int, default=getattr(Settings(), name))
    for name in ("request_timeout", "send_timeout"):
        parser.add_argument("--" + name.replace("_", "-"), type=float, default=getattr(Settings(), name))
    parser.add_argument("--no-sync", action="store_true")
    parser.add_argument("--log-requests", action="store_true")
    args = parser.parse_args(argv)
    if not 0 <= args.port <= 65535:
        parser.error("port must be in [0, 65535]; 0 chooses an available port")
    try:
        settings = Settings(**{name: getattr(args, name) for name in Settings.__dataclass_fields__
                              if name != "token"}, token=os.environ.get("MINIDB_API_TOKEN") or None)
    except ValueError as error:
        parser.error(str(error))
    return args, settings


def main(argv=None):
    args, settings = parse_arguments(argv)
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    try:
        with NativeEngine(args.db, args.library, sync_writes=not args.no_sync) as engine:
            with ApiServer((args.host, args.port), engine, settings) as server:
                def stop(_signum, _frame):
                    server.request_stop()
                signal.signal(signal.SIGINT, stop)
                signal.signal(signal.SIGTERM, stop)
                print(json.dumps({"event": "ready", "host": server.server_address[0],
                                  "port": server.server_port, "workers": settings.workers}), flush=True)
                server.serve_until_stopped()
    except (NativeError, OSError, AttributeError) as error:
        logging.error("Server could not start: %s", error)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
