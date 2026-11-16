"""A fixed set of workers and a bounded waiting queue, independent of HTTP."""

from __future__ import annotations

import logging
import queue
import threading
from typing import Callable


class BoundedWorkerPool:
    def __init__(self, workers: int, queue_size: int, process: Callable):
        if workers < 1 or queue_size < 1:
            raise ValueError("workers and queue_size must be positive")
        self._queue = queue.Queue(maxsize=queue_size)
        self._process = process
        self._state = threading.Lock()
        self._closing = False
        self._active = self._peak_active = self._completed = self._rejected = 0
        self._threads = [threading.Thread(target=self._worker_loop,
                        name=f"db-worker-{i + 1}", daemon=False) for i in range(workers)]
        started = []
        try:
            for thread in self._threads:
                thread.start()
                started.append(thread)
        except Exception:
            for _thread in started:
                self._queue.put(None)
            for thread in started:
                thread.join()
            raise

    def try_submit(self, task) -> bool:
        # Admission and close are atomic with respect to one another.
        with self._state:
            if self._closing:
                self._rejected += 1
                return False
            try:
                self._queue.put_nowait(task)
                return True
            except queue.Full:
                self._rejected += 1
                return False

    def _worker_loop(self) -> None:
        while True:
            task = self._queue.get()
            try:
                if task is None:
                    return
                with self._state:
                    self._active += 1
                    self._peak_active = max(self._peak_active, self._active)
                try:
                    self._process(task)
                except Exception:
                    logging.exception("Worker failed while processing a connection")
                finally:
                    with self._state:
                        self._active -= 1
                        self._completed += 1
            finally:
                self._queue.task_done()

    def stats(self) -> dict:
        with self._state:
            return {"workers": len(self._threads), "queue_capacity": self._queue.maxsize,
                    "queued": self._queue.qsize(), "active": self._active,
                    "peak_active": self._peak_active, "completed": self._completed,
                    "rejected": self._rejected, "closing": self._closing}

    def close(self) -> None:
        # The server owner calls close once, after stopping admission.
        with self._state:
            if self._closing:
                return
            self._closing = True
        self._queue.join()
        for _thread in self._threads:
            self._queue.put(None)
        for thread in self._threads:
            thread.join()
