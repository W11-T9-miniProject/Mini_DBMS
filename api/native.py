"""Typed, lifetime-safe ctypes boundary. SQL execution stays in the C engine."""

from __future__ import annotations

import ctypes as ct
from contextlib import contextmanager
from pathlib import Path
import sys
import threading
from typing import Iterator


class NativeError(RuntimeError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


class Row(ct.Structure):
    _fields_ = [("id", ct.c_uint64), ("age", ct.c_int32),
                ("name", ct.c_char * 64), ("email", ct.c_char * 128)]


class Result(ct.Structure):
    _fields_ = [("columns", ct.c_char_p * 4), ("column_count", ct.c_size_t),
                ("affected_rows", ct.c_uint64), ("examined_rows", ct.c_uint64),
                ("first_insert_id", ct.c_uint64), ("plan", ct.c_int), ("explain", ct.c_bool),
                ("lock_wait_ms", ct.c_double), ("lock_hold_ms", ct.c_double)]


class Error(ct.Structure):
    _fields_ = [("code", ct.c_int), ("message", ct.c_char * 256)]


class Stats(ct.Structure):
    _fields_ = [("row_count", ct.c_uint64), ("file_bytes", ct.c_uint64),
                ("index_height", ct.c_size_t), ("index_nodes", ct.c_size_t),
                ("index_memory_bytes", ct.c_size_t)]


RowVisitor = ct.CFUNCTYPE(ct.c_bool, ct.POINTER(Row), ct.POINTER(Result), ct.c_void_p)


def default_library_path() -> Path:
    name = "minidb.dll" if sys.platform == "win32" else (
        "libminidb.dylib" if sys.platform == "darwin" else "libminidb.so")
    return Path(__file__).resolve().parents[1] / "build" / name


def load_library(path: Path) -> ct.CDLL:
    # CDLL releases the GIL while native code executes. Do not use PyDLL.
    library = ct.CDLL(str(path.resolve()))
    library.db_abi_version.argtypes = []
    library.db_abi_version.restype = ct.c_uint32
    library.db_abi_sizeof.argtypes = [ct.c_uint]
    library.db_abi_sizeof.restype = ct.c_size_t
    if library.db_abi_version() != 1:
        raise NativeError("ABI_MISMATCH", "Rebuild the C library: unsupported ABI version")
    for kind, structure in enumerate((Row, Result, Error, Stats), start=1):
        if library.db_abi_sizeof(kind) != ct.sizeof(structure):
            raise NativeError("ABI_MISMATCH", f"C/Python structure size differs: {structure.__name__}")
    library.db_open.argtypes = [ct.c_char_p, ct.c_bool, ct.POINTER(Error)]
    library.db_open.restype = ct.c_void_p
    library.db_close.argtypes = [ct.c_void_p]
    library.db_close.restype = None
    library.db_execute.argtypes = [ct.c_void_p, ct.c_char_p, RowVisitor, ct.c_void_p,
                                  ct.POINTER(Result), ct.POINTER(Error)]
    library.db_execute.restype = ct.c_int
    library.db_get_stats.argtypes = [ct.c_void_p, ct.POINTER(Stats), ct.POINTER(Error)]
    library.db_get_stats.restype = ct.c_int
    for name in ("db_status_name", "db_plan_name"):
        function = getattr(library, name)
        function.argtypes = [ct.c_int]
        function.restype = ct.c_char_p
    return library


class NativeEngine:
    def __init__(self, directory: str | Path, library: str | Path | None = None,
                 *, sync_writes: bool = True):
        if "\0" in str(directory):
            raise ValueError("Database path cannot contain NUL")
        self._library = load_library(Path(library) if library else default_library_path())
        self._lifetime = threading.Condition()
        self._active_calls = 0
        self._closing = False
        error = Error()
        self._handle = self._library.db_open(str(directory).encode("utf-8"), sync_writes, ct.byref(error))
        if not self._handle:
            self._raise_native(error)

    def _raise_native(self, error: Error) -> None:
        code = self._library.db_status_name(error.code).decode("ascii")
        raise NativeError(code, error.message.decode("utf-8", errors="replace"))

    @contextmanager
    def _borrow_handle(self) -> Iterator[int]:
        # This condition protects lifetime only; it is not held across C calls.
        with self._lifetime:
            if self._closing or not self._handle:
                raise NativeError("ENGINE_CLOSED", "Engine is shutting down")
            self._active_calls += 1
            handle = self._handle
        try:
            yield handle
        finally:
            with self._lifetime:
                self._active_calls -= 1
                self._lifetime.notify_all()

    def execute(self, sql: str, *, max_rows: int = 1000) -> dict:
        if not isinstance(sql, str) or not sql.strip() or "\0" in sql:
            raise NativeError("INVALID_SQL", "sql must be a nonempty string without NUL bytes")
        if max_rows < 0:
            raise ValueError("max_rows must be nonnegative")
        encoded_sql = sql.encode("utf-8", errors="strict")
        rows: list[list] = []
        callback_errors: list[Exception] = []
        limit_exceeded = False

        @RowVisitor
        def collect(row_pointer, result_pointer, _context):
            nonlocal limit_exceeded
            try:
                if len(rows) >= max_rows:
                    limit_exceeded = True
                    return False
                row, result = row_pointer.contents, result_pointer.contents
                if result.column_count > 4:
                    raise ValueError("Invalid native column count")
                values = {"id": row.id, "age": row.age,
                          "name": row.name.decode("utf-8"), "email": row.email.decode("utf-8")}
                rows.append([values[result.columns[i].decode("ascii")] for i in range(result.column_count)])
                return True
            except Exception as error:
                # Never let a Python exception escape a C callback.
                callback_errors.append(error)
                return False

        result, error = Result(), Error()
        with self._borrow_handle() as handle:
            status = self._library.db_execute(handle, encoded_sql, collect, None, ct.byref(result), ct.byref(error))
        if callback_errors:
            raise NativeError("RESULT_ENCODING_ERROR", str(callback_errors[0]))
        if limit_exceeded:
            raise NativeError("RESULT_TOO_LARGE", f"Result exceeds {max_rows} rows; use SQL LIMIT")
        if status != 0:
            self._raise_native(error)
        return {
            "columns": [result.columns[i].decode("ascii") for i in range(result.column_count)],
            "rows": rows, "row_count": len(rows), "affected_rows": result.affected_rows,
            "first_insert_id": result.first_insert_id if result.plan == 0 else None,
            "plan": self._library.db_plan_name(result.plan).decode("ascii"),
            "explain": result.explain, "examined_rows": result.examined_rows,
            "lock_wait_ms": result.lock_wait_ms, "lock_hold_ms": result.lock_hold_ms,
        }

    def stats(self) -> dict:
        stats, error = Stats(), Error()
        with self._borrow_handle() as handle:
            status = self._library.db_get_stats(handle, ct.byref(stats), ct.byref(error))
        if status != 0:
            self._raise_native(error)
        return {name: getattr(stats, name) for name, _type in Stats._fields_}

    def close(self) -> None:
        with self._lifetime:
            self._closing = True
            while self._active_calls:
                self._lifetime.wait()
            handle, self._handle = self._handle, None
        if handle:
            self._library.db_close(handle)

    def __enter__(self):
        return self

    def __exit__(self, *_exception):
        self.close()
