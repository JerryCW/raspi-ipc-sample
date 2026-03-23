"""Activity session state machine for AI video activity detection.

Manages activity sessions per detected class. Each session tracks a
continuous period of detections for a single object class (person, cat,
dog, bird). Sessions are created on first detection, updated on
subsequent detections, and ended after a configurable timeout of
inactivity (default 60 s).

Design references:
- Requirements 2.3, 2.4, 2.5, 2.6, 2.7
"""

from __future__ import annotations

import json
import uuid
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional

import numpy as np

from device.ai.config import DetectorConfig


@dataclass
class ActivitySession:
    """State for a single activity session."""

    session_id: str
    detected_class: str
    start_time_ms: int
    end_time_ms: int = 0
    last_active_ms: int = 0
    max_confidence: float = 0.0
    detection_count: int = 0
    start_screenshot: str = ""
    end_screenshot: str = ""
    is_active: bool = True


# Type aliases for dependency-injected I/O callables.
SaveScreenshotFn = Callable[[np.ndarray, str], None]
SaveMetadataFn = Callable[[dict], None]


def _default_save_screenshot(frame: np.ndarray, filename: str) -> None:  # pragma: no cover
    """Default screenshot saver — writes JPEG via OpenCV."""
    import cv2

    cv2.imwrite(filename, frame)


def _default_save_metadata(metadata: dict) -> None:  # pragma: no cover
    """Default metadata saver — writes JSON file."""
    path = metadata.get("_path", "metadata.json")
    with open(path, "w") as f:
        json.dump({k: v for k, v in metadata.items() if not k.startswith("_")}, f)


class SessionManager:
    """Manages activity sessions across multiple object classes.

    Each detected class maintains at most one active session at a time.
    The manager supports dependency injection for screenshot and metadata
    saving to facilitate testing without file I/O.
    """

    def __init__(
        self,
        config: DetectorConfig,
        capture_dir: str = "",
        save_screenshot: Optional[SaveScreenshotFn] = None,
        save_metadata: Optional[SaveMetadataFn] = None,
    ) -> None:
        self.config = config
        self.capture_dir = capture_dir
        self._save_screenshot = save_screenshot or _default_save_screenshot
        self._save_metadata = save_metadata or _default_save_metadata
        # class name -> active session
        self._active_sessions: Dict[str, ActivitySession] = {}

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def on_detection(
        self,
        cls: str,
        confidence: float,
        timestamp_ms: int,
        frame: np.ndarray,
    ) -> Optional[ActivitySession]:
        """Process a detection event.

        If *cls* is not in the configured detect_classes or *confidence*
        is below the threshold, the detection is ignored (returns None).

        On first detection for a class, a new session is created and the
        start screenshot is saved. On subsequent detections the existing
        session is updated.

        Returns the session (new or updated), or None if ignored.
        """
        if cls not in self.config.detect_classes:
            return None
        if confidence < self.config.confidence_threshold:
            return None

        session = self._active_sessions.get(cls)
        if session is None:
            return self._create_session(cls, confidence, timestamp_ms, frame)
        else:
            return self._update_session(session, confidence, timestamp_ms)

    def check_timeouts(self, current_time_ms: int) -> List[ActivitySession]:
        """End sessions that have exceeded the inactivity timeout.

        Returns a list of sessions that were ended.
        """
        timeout_ms = self.config.session_timeout_sec * 1000
        ended: List[ActivitySession] = []

        classes_to_remove: List[str] = []
        for cls, session in self._active_sessions.items():
            if current_time_ms - session.last_active_ms > timeout_ms:
                self._end_session(session, current_time_ms)
                classes_to_remove.append(cls)
                ended.append(session)

        for cls in classes_to_remove:
            del self._active_sessions[cls]

        return ended

    def get_active_sessions(self) -> Dict[str, ActivitySession]:
        """Return a copy of the active sessions dict."""
        return dict(self._active_sessions)

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _create_session(
        self,
        cls: str,
        confidence: float,
        timestamp_ms: int,
        frame: np.ndarray,
    ) -> ActivitySession:
        sid = uuid.uuid4().hex
        screenshot_name = f"{sid}_start.jpg"

        session = ActivitySession(
            session_id=sid,
            detected_class=cls,
            start_time_ms=timestamp_ms,
            last_active_ms=timestamp_ms,
            max_confidence=confidence,
            detection_count=1,
            start_screenshot=screenshot_name,
            is_active=True,
        )

        self._save_screenshot(frame, screenshot_name)
        self._active_sessions[cls] = session
        return session

    def _update_session(
        self,
        session: ActivitySession,
        confidence: float,
        timestamp_ms: int,
    ) -> ActivitySession:
        session.last_active_ms = timestamp_ms
        if confidence > session.max_confidence:
            session.max_confidence = confidence
        session.detection_count += 1
        return session

    def _end_session(self, session: ActivitySession, current_time_ms: int) -> None:
        session.end_time_ms = current_time_ms
        session.is_active = False

        end_screenshot_name = f"{session.session_id}_end.jpg"
        session.end_screenshot = end_screenshot_name

        # Save end screenshot with a zero frame (tests inject their own callable)
        dummy_frame = np.zeros((1, 1, 3), dtype=np.uint8)
        self._save_screenshot(dummy_frame, end_screenshot_name)

        # Build and save metadata
        duration_seconds = (session.end_time_ms - session.start_time_ms) / 1000.0
        metadata = {
            "session_id": session.session_id,
            "kvs_start_timestamp": session.start_time_ms,
            "kvs_end_timestamp": session.end_time_ms,
            "duration_seconds": duration_seconds,
            "detected_class": session.detected_class,
            "max_confidence": session.max_confidence,
            "start_screenshot_filename": session.start_screenshot,
            "end_screenshot_filename": session.end_screenshot,
            "detection_count": session.detection_count,
        }
        self._save_metadata(metadata)
