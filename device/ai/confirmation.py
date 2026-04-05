"""Detection confirmation window using a sliding deque.

Filters single-frame false positives by requiring a class to appear
in at least M of the last N frames before confirming the detection.

Requirements: 1.1, 1.2, 1.3, 1.4, 2.1, 2.2, 2.3, 2.4
"""

from __future__ import annotations

import logging
from collections import deque

logger = logging.getLogger(__name__)


class DetectionConfirmationWindow:
    """Sliding-window confirmation filter.

    Maintains the last *N* frames of detected class sets and only
    confirms a class when it appears in at least *M* of those frames.
    """

    def __init__(self, window_size: int = 3, min_count: int = 2) -> None:
        """Initialise the confirmation window.

        Args:
            window_size: Sliding window size N (clamped to [1, 10]).
            min_count: Minimum confirmation count M (clamped to [1, window_size]).

        If *min_count* exceeds *window_size* after clamping, a WARNING
        is logged and *min_count* is clamped to *window_size*.
        """
        # Clamp window_size to [1, 10]
        clamped_ws = max(1, min(window_size, 10))
        if clamped_ws != window_size:
            logger.warning(
                "window_size=%d out of range [1, 10], clamped to %d",
                window_size,
                clamped_ws,
            )

        # Clamp min_count: must be >= 1 and <= clamped window_size
        clamped_mc = max(1, min_count)
        if clamped_mc > clamped_ws:
            logger.warning(
                "min_count=%d exceeds window_size=%d, clamped to %d",
                min_count,
                clamped_ws,
                clamped_ws,
            )
            clamped_mc = clamped_ws

        self._window_size = clamped_ws
        self._min_count = clamped_mc
        self._window: deque[set[str]] = deque(maxlen=self._window_size)

    # -- public API --------------------------------------------------

    def push_frame(self, detected_classes: set[str]) -> set[str]:
        """Push one frame's detections and return confirmed classes.

        Args:
            detected_classes: Class names detected in the current frame.

        Returns:
            The subset of classes appearing in >= *min_count* frames
            within the current window.
        """
        self._window.append(detected_classes)

        # Count occurrences of every class present in the window
        counts: dict[str, int] = {}
        for frame_set in self._window:
            for cls in frame_set:
                counts[cls] = counts.get(cls, 0) + 1

        return {cls for cls, cnt in counts.items() if cnt >= self._min_count}

    def get_count(self, cls: str) -> int:
        """Return the number of frames in the window containing *cls*."""
        return sum(1 for frame_set in self._window if cls in frame_set)

    # -- read-only properties ----------------------------------------

    @property
    def window_size(self) -> int:
        """Sliding window size N."""
        return self._window_size

    @property
    def min_count(self) -> int:
        """Minimum confirmation count M."""
        return self._min_count
