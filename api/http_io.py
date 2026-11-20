"""Bounded, deadline-aware socket input for the standard HTTP parser."""

import io
import socket
import time


class HeadersTooLarge(ValueError):
    pass


class DeadlineSocketReader(io.RawIOBase):
    def __init__(self, connection: socket.socket, deadline: float, max_header_bytes: int):
        super().__init__()
        self._connection = connection
        self._deadline = deadline
        self._max_header_bytes = max_header_bytes
        self._header_prefix = bytearray()
        self._headers_complete = False

    def readable(self):
        return True

    def readinto(self, buffer):
        remaining = self._deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Request input deadline expired")
        self._connection.settimeout(remaining)
        count = self._connection.recv_into(buffer)
        if not self._headers_complete and count:
            self._header_prefix.extend(memoryview(buffer)[:count])
            end = self._header_prefix.find(b"\r\n\r\n")
            header_size = end + 4 if end >= 0 else len(self._header_prefix)
            if header_size > self._max_header_bytes:
                raise HeadersTooLarge("HTTP headers exceed configured limit")
            if end >= 0:
                self._headers_complete = True
                self._header_prefix.clear()
        return count
