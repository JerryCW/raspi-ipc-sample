"""IPC client for communicating with the C++ FrameExporter.

Connects via Unix Domain Socket to receive frame notifications,
and maps POSIX shared memory to read raw frame data.
"""

from __future__ import annotations

import json
import logging
import mmap
import os
import socket
import time
from dataclasses import asdict, dataclass
from typing import Optional

import numpy as np

logger = logging.getLogger(__name__)

HEADER_SIZE = 64  # SharedMemoryHeader is 64 bytes


@dataclass
class FrameNotification:
    """Mirrors the C++ FrameNotification struct sent over Unix Socket."""

    shm_name: str
    buffer_index: int
    offset: int
    size: int
    width: int
    height: int
    pixel_format: str
    timestamp_ms: int
    sequence: int


def serialize_notification(notif: FrameNotification) -> str:
    """Serialize a FrameNotification to a JSON string terminated by newline."""
    return json.dumps(asdict(notif)) + "\n"


def parse_notification(json_str: str) -> FrameNotification:
    """Parse a JSON string into a FrameNotification."""
    data = json.loads(json_str.strip())
    return FrameNotification(**data)



class IPCClient:
    """Client that connects to the C++ FrameExporter via Unix Socket and shared memory."""

    def __init__(self) -> None:
        self._sock: Optional[socket.socket] = None
        self._sock_file = None  # buffered file wrapper for readline
        self._mmap: Optional[mmap.mmap] = None
        self._shm_fd: Optional[int] = None

    # ------------------------------------------------------------------
    # Connection
    # ------------------------------------------------------------------

    def connect(
        self,
        socket_path: str,
        max_retries: int = 10,
        initial_delay: float = 1.0,
        max_delay: float = 30.0,
    ) -> None:
        """Connect to the Unix Domain Socket with exponential backoff.

        Args:
            socket_path: Path to the Unix Domain Socket.
            max_retries: Maximum number of connection attempts.
            initial_delay: Initial delay in seconds before the first retry.
            max_delay: Maximum delay between retries (cap for exponential backoff).

        Raises:
            ConnectionError: If all retry attempts are exhausted.
        """
        delay = initial_delay
        for attempt in range(max_retries):
            try:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.connect(socket_path)
                self._sock = sock
                self._sock_file = sock.makefile("r", encoding="utf-8")
                logger.info("Connected to FrameExporter at %s (attempt %d)", socket_path, attempt + 1)
                return
            except OSError as exc:
                logger.warning(
                    "Connection attempt %d/%d to %s failed: %s",
                    attempt + 1,
                    max_retries,
                    socket_path,
                    exc,
                )
                if attempt < max_retries - 1:
                    time.sleep(delay)
                    delay = min(delay * 2, max_delay)

        raise ConnectionError(
            f"Failed to connect to {socket_path} after {max_retries} attempts"
        )

    # ------------------------------------------------------------------
    # Shared memory
    # ------------------------------------------------------------------

    def map_shared_memory(self, shm_name: str, shm_size: int) -> None:
        """Memory-map the POSIX shared memory region at /dev/shm.

        Args:
            shm_name: POSIX shared memory name (e.g. ``/smart_camera_frames``).
                       The leading ``/`` is stripped to form the filename under ``/dev/shm``.
            shm_size: Total size of the shared memory region in bytes.

        Raises:
            FileNotFoundError: If the shared memory file does not exist.
            OSError: If mmap fails.
        """
        # /dev/shm/<name_without_leading_slash>
        shm_path = "/dev/shm/" + shm_name.lstrip("/")
        fd = os.open(shm_path, os.O_RDONLY)
        try:
            self._mmap = mmap.mmap(fd, shm_size, access=mmap.ACCESS_READ)
        finally:
            # mmap keeps its own reference; we can close the fd
            os.close(fd)
        logger.info("Mapped shared memory %s (%d bytes)", shm_name, shm_size)

    # ------------------------------------------------------------------
    # Frame reading
    # ------------------------------------------------------------------

    def read_frame(self, notification: FrameNotification) -> np.ndarray:
        """Read raw frame bytes from shared memory based on a notification.

        Returns a 1-D ``numpy.uint8`` array of length ``notification.size``.

        Args:
            notification: The frame notification describing where the data lives.

        Raises:
            RuntimeError: If shared memory is not mapped.
        """
        if self._mmap is None:
            raise RuntimeError("Shared memory not mapped. Call map_shared_memory() first.")

        self._mmap.seek(notification.offset)
        raw = self._mmap.read(notification.size)
        return np.frombuffer(raw, dtype=np.uint8).copy()

    # ------------------------------------------------------------------
    # Notification receiving
    # ------------------------------------------------------------------

    def receive_notification(self) -> FrameNotification:
        """Read one JSON-line notification from the socket.

        Blocks until a complete line is available.

        Raises:
            ConnectionError: If the socket is closed or not connected.
        """
        if self._sock_file is None:
            raise ConnectionError("Not connected. Call connect() first.")

        line = self._sock_file.readline()
        if not line:
            raise ConnectionError("Socket closed by remote end")

        return parse_notification(line)

    # ------------------------------------------------------------------
    # Cleanup
    # ------------------------------------------------------------------

    def close(self) -> None:
        """Release all resources (socket and shared memory)."""
        if self._sock_file is not None:
            try:
                self._sock_file.close()
            except OSError:
                pass
            self._sock_file = None

        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

        if self._mmap is not None:
            try:
                self._mmap.close()
            except OSError:
                pass
            self._mmap = None

        logger.info("IPC client closed")
