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
        # Candidate screenshot buffer (Top-N)
        self._candidate_buffer = CandidateBuffer(
            max_size=config.max_candidate_frames,
            min_interval_ms=config.candidate_min_interval_ms,
            save_screenshot=self._save_screenshot,
            capture_dir=capture_dir,
        )
        # Event deduplicator
        self._deduplicator = EventDeduplicator(
            dedup_window_sec=config.dedup_window_sec,
        )

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
        session is updated. The frame is also submitted to the
        CandidateBuffer and the detection to the EventDeduplicator.

        Returns the session (new or updated), or None if ignored.
        """
        if cls not in self.config.detect_classes:
            return None
        if confidence < self.config.confidence_threshold:
            return None

        session = self._active_sessions.get(cls)
        if session is None:
            session = self._create_session(cls, confidence, timestamp_ms, frame)
        else:
            session = self._update_session(session, confidence, timestamp_ms)

        # Submit frame to CandidateBuffer (start screenshot is saved
        # separately in _create_session and does not occupy buffer slots)
        self._candidate_buffer.try_add(
            frame, confidence, timestamp_ms, cls, session.session_id,
        )

        # Submit detection to EventDeduplicator
        self._deduplicator.on_detection(cls, confidence, timestamp_ms)

        return session

    def check_timeouts(self, current_time_ms: int) -> List[ActivitySession]:
        """End sessions that have exceeded the inactivity timeout.

        Also checks the EventDeduplicator for window expiry.

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

        # Check EventDeduplicator timeout
        self._deduplicator.check_timeout(current_time_ms)

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

        # Get candidate frames from buffer (sorted by confidence descending)
        candidates = self._candidate_buffer.get_candidates_descending()
        candidate_screenshots = [c.filename for c in candidates]

        # Collect detected_classes and per-class stats from CandidateBuffer
        # and EventDeduplicator
        dedup_event = self._deduplicator.get_active_event()
        if dedup_event is not None:
            detected_classes = list(dedup_event.detected_classes)
            primary_class = dedup_event.primary_class
            class_max_confidences = dict(dedup_event.class_max_confidences)
            class_detection_counts = dict(dedup_event.class_detection_counts)
        else:
            detected_classes = [session.detected_class]
            primary_class = session.detected_class
            class_max_confidences = {session.detected_class: session.max_confidence}
            class_detection_counts = {session.detected_class: session.detection_count}

        # Clear the candidate buffer for the next session
        self._candidate_buffer.clear()

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
            # New fields for edge-cloud pipeline
            "candidate_screenshots": candidate_screenshots,
            "detected_classes": detected_classes,
            "primary_class": primary_class,
            "class_max_confidences": class_max_confidences,
            "class_detection_counts": class_detection_counts,
            "verification_status": "pending",
            "pipeline_stage": "edge_detected",
        }
        self._save_metadata(metadata)


# ---------------------------------------------------------------------------
# CandidateBuffer — Top-N candidate screenshot buffer
# ---------------------------------------------------------------------------


@dataclass
class CandidateFrame:
    """A single candidate frame in the buffer."""

    filename: str
    confidence: float
    timestamp_ms: int
    detected_class: str


class CandidateBuffer:
    """Top-N candidate screenshot buffer for a session.

    Maintains the highest-confidence frames captured during a session,
    subject to a minimum time interval constraint to ensure temporal
    diversity.

    Design references:
    - Requirements 3.2, 3.3, 3.4, 3.5, 3.6
    """

    def __init__(
        self,
        max_size: int = 5,
        min_interval_ms: int = 500,
        save_screenshot: Optional[SaveScreenshotFn] = None,
        capture_dir: str = "",
    ) -> None:
        self._max_size = max_size
        self._min_interval_ms = min_interval_ms
        self._save_screenshot = save_screenshot or _default_save_screenshot
        self._capture_dir = capture_dir
        self._buffer: List[CandidateFrame] = []
        self._next_index: int = 0  # monotonic counter for unique filenames

    def try_add(
        self,
        frame: np.ndarray,
        confidence: float,
        timestamp_ms: int,
        detected_class: str,
        session_id: str,
    ) -> bool:
        """Try to add a frame to the candidate buffer.

        Returns True if the frame was added, False otherwise.

        Logic:
        1. Reject if timestamp is within min_interval_ms of any existing frame.
        2. If buffer is not full, add directly.
        3. If buffer is full and confidence > lowest in buffer, replace lowest
           and delete the old JPEG file.
        4. Otherwise reject.
        """
        # 1. Minimum interval check
        for existing in self._buffer:
            if abs(timestamp_ms - existing.timestamp_ms) < self._min_interval_ms:
                return False

        # 2. Buffer not full — add directly
        if len(self._buffer) < self._max_size:
            filename = f"{session_id}_candidate_{self._next_index}.jpg"
            self._next_index += 1
            self._save_screenshot(frame, filename)
            self._buffer.append(
                CandidateFrame(
                    filename=filename,
                    confidence=confidence,
                    timestamp_ms=timestamp_ms,
                    detected_class=detected_class,
                )
            )
            return True

        # 3. Buffer full — find lowest confidence frame
        min_idx = 0
        for i in range(1, len(self._buffer)):
            if self._buffer[i].confidence < self._buffer[min_idx].confidence:
                min_idx = i

        if confidence > self._buffer[min_idx].confidence:
            old_frame = self._buffer[min_idx]
            # Delete old JPEG file
            import os

            old_path = os.path.join(self._capture_dir, old_frame.filename)
            try:
                os.remove(old_path)
            except OSError:
                pass

            # Replace with new frame
            filename = f"{session_id}_candidate_{self._next_index}.jpg"
            self._next_index += 1
            self._save_screenshot(frame, filename)
            self._buffer[min_idx] = CandidateFrame(
                filename=filename,
                confidence=confidence,
                timestamp_ms=timestamp_ms,
                detected_class=detected_class,
            )
            return True

        # 4. Reject — confidence not high enough
        return False

    def get_candidates_descending(self) -> List[CandidateFrame]:
        """Return candidate frames sorted by confidence descending."""
        return sorted(self._buffer, key=lambda c: c.confidence, reverse=True)

    def clear(self) -> List[str]:
        """Clear the buffer and return all candidate filenames."""
        filenames = [c.filename for c in self._buffer]
        self._buffer.clear()
        self._next_index = 0
        return filenames


# ---------------------------------------------------------------------------
# EventDeduplicator — merge detections within a time window
# ---------------------------------------------------------------------------


@dataclass
class DedupEvent:
    """A deduplicated merged event spanning multiple detection classes.

    Design references:
    - Requirements 2.1, 2.2, 2.3, 2.4, 2.5, 2.6
    """

    event_id: str
    start_time_ms: int
    last_active_ms: int
    detected_classes: list[str]
    primary_class: str
    class_max_confidences: dict[str, float]
    class_detection_counts: dict[str, int]
    total_detection_count: int


class EventDeduplicator:
    """Merges YOLO detections within a configurable time window into a single event.

    Within the dedup window, detections of different classes (person, bird,
    dog, etc.) are merged into one ``DedupEvent``. The window is extended
    each time a new detection arrives. When no detection arrives for longer
    than ``dedup_window_sec``, the event is considered finished.

    Design references:
    - Requirements 2.1, 2.2, 2.3, 2.4, 2.5, 2.6
    """

    def __init__(self, dedup_window_sec: int = 30) -> None:
        self._dedup_window_ms = dedup_window_sec * 1000
        self._active_event: Optional[DedupEvent] = None

    def on_detection(self, cls: str, confidence: float, timestamp_ms: int) -> DedupEvent:
        """Process a single detection and return the current merged event.

        If no active event exists, a new one is created. Otherwise the
        existing event is updated with the new detection data.
        """
        if self._active_event is None:
            self._active_event = DedupEvent(
                event_id=uuid.uuid4().hex,
                start_time_ms=timestamp_ms,
                last_active_ms=timestamp_ms,
                detected_classes=[cls],
                primary_class=cls,
                class_max_confidences={cls: confidence},
                class_detection_counts={cls: 1},
                total_detection_count=1,
            )
            return self._active_event

        evt = self._active_event
        evt.last_active_ms = timestamp_ms
        evt.total_detection_count += 1

        # Update per-class tracking
        if cls in evt.class_detection_counts:
            evt.class_detection_counts[cls] += 1
            if confidence > evt.class_max_confidences[cls]:
                evt.class_max_confidences[cls] = confidence
        else:
            evt.detected_classes.append(cls)
            evt.class_max_confidences[cls] = confidence
            evt.class_detection_counts[cls] = 1

        # Recompute primary_class — class with highest max confidence
        evt.primary_class = max(evt.class_max_confidences, key=evt.class_max_confidences.get)  # type: ignore[arg-type]

        return evt

    def check_timeout(self, current_time_ms: int) -> Optional[DedupEvent]:
        """Check whether the dedup window has expired.

        If the active event's ``last_active_ms`` is more than
        ``dedup_window_ms`` before *current_time_ms*, the event is
        considered finished: it is removed from the active slot and
        returned. Otherwise returns ``None``.
        """
        if self._active_event is None:
            return None

        if current_time_ms - self._active_event.last_active_ms > self._dedup_window_ms:
            finished = self._active_event
            self._active_event = None
            return finished

        return None

    def get_active_event(self) -> Optional[DedupEvent]:
        """Return the currently active merged event, or ``None``."""
        return self._active_event
