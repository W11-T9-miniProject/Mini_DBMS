"""Real-process HTTP test client shared by functional tests and load tool."""

from __future__ import annotations

import http.client
import json
import os
from pathlib import Path
import queue
import socket
import subprocess
import sys
import threading

ROOT = Path(__file__).resolve().parents[1]


def request(port, method="POST", path="/query", document=None, *, body=None, headers=None, timeout=10):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    if body is None and document is not None:
        body = json.dumps(document, ensure_ascii=False).encode("utf-8")
    outgoing_headers = {"Content-Type": "application/json", **(headers or {})}
    try:
        connection.request(method, path, body=body, headers=outgoing_headers)
        response = connection.getresponse()
        raw = response.read()
        return response.status, json.loads(raw.decode("utf-8")), dict(response.getheaders())
    finally:
        connection.close()


def raw_request(port, data: bytes, *, shutdown_write=False):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as connection:
        connection.sendall(data)
        if shutdown_write:
            connection.shutdown(socket.SHUT_WR)
        response = http.client.HTTPResponse(connection)
        response.begin()
        return response.status, json.loads(response.read().decode("utf-8"))


class ServerProcess:
    def __init__(self, directory: Path, *options, token=None):
        self.log = (directory.parent / f"{directory.name}-server.log").open("wb")
        environment = os.environ.copy()
        environment.pop("MINIDB_API_TOKEN", None)
        if token:
            environment["MINIDB_API_TOKEN"] = token
        command = [sys.executable, "-m", "api.server", "--db", str(directory), "--port", "0", *options]
        flags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
        self.process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE,
                                        stderr=self.log, env=environment, creationflags=flags)
        ready = queue.Queue()
        def read_ready():
            try:
                ready.put(self.process.stdout.readline())
            except Exception as error:
                ready.put(error)
        self.reader = threading.Thread(target=read_ready, daemon=True)
        self.reader.start()
        try:
            line = ready.get(timeout=15)
            if not isinstance(line, bytes) or not line:
                raise RuntimeError("API exited before ready; inspect its server log")
            message = json.loads(line)
            if message.get("event") != "ready":
                raise RuntimeError(f"Unexpected ready message: {message}")
            self.port = message["port"]
        except Exception:
            self.close()
            raise

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.reader.join(timeout=2)
        self.process.stdout.close()
        self.log.close()

    def __enter__(self):
        return self

    def __exit__(self, *_exception):
        self.close()
