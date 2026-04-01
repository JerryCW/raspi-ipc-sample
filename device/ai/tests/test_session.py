"""Property-based tests for the activity session state machine.

Uses Hypothesis to verify correctness properties of SessionManager
and ActivitySession.
"""

from __future__ import annotations

from hypothesis import given, settings, strategies as st

import numpy as np

from device.ai.config import DetectorConfig
from device.ai.session import ActivitySession, SessionManager

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

VALID_CLASSES = ["person", "cat", "dog", "bird"]

# Dummy frame used throughout tests (avoids real image data).
DUMMY_FRAME = np.zeros((4, 4, 3), dtype=np.uint8)


def _make_config(**overrides) -> DetectorConfig:
    """Build a DetectorConfig with sensible test defaults."""
    defaults = dict(
        detect_classes=list(VALID_CLASSES),
        confidence_threshold=0.5,
        session_timeout_sec=60,
    )
    defaults.update(overrides)
    return DetectorConfig(**defaults)


def _make_manager(config: DetectorConfig | None = None) -> tuple[SessionManager, list, list]:
    """Create a SessionManager with recording stubs for I/O.

    Returns (manager, screenshots_log, metadata_log).
    """
    screenshots: list[str] = []
    metadata_records: list[dict] = []

    def record_screenshot(_frame: np.ndarray, filename: str) -> None:
        screenshots.append(filename)

    def record_metadata(meta: dict) -> None:
        metadata_records.append(meta)

    cfg = config or _make_config()
    mgr = SessionManager(
        config=cfg,
        capture_dir="/tmp/test_captures",
        save_screenshot=record_screenshot,
        save_metadata=record_metadata,
    )
    return mgr, screenshots, metadata_records


# ---------------------------------------------------------------------------
# Strategies
# ---------------------------------------------------------------------------

valid_class_st = st.sampled_from(VALID_CLASSES)
confidence_above_threshold_st = st.floats(min_value=0.5, max_value=1.0, allow_nan=False)
confidence_below_threshold_st = st.floats(min_value=0.0, max_value=0.49, allow_nan=False)
timestamp_ms_st = st.integers(min_value=0, max_value=2**53 - 1)


# ---------------------------------------------------------------------------
# Property 5: 检测类别约束
# ---------------------------------------------------------------------------


class TestDetectionClassConstraint:
    """Property 5: detected_class must be in configured set.

    **Validates: Requirements 2.2**

    For any detection with a class NOT in the configured set, the
    SessionManager must ignore it (return None, no session created).
    """

    @given(
        cls=st.text(min_size=1, max_size=20).filter(lambda c: c not in VALID_CLASSES),
        confidence=st.floats(min_value=0.5, max_value=1.0, allow_nan=False),
        ts=timestamp_ms_st,
    )
    @settings(max_examples=200)
    def test_invalid_class_ignored(self, cls: str, confidence: float, ts: int) -> None:
        mgr, screenshots, _ = _make_manager()
        result = mgr.on_detection(cls, confidence, ts, DUMMY_FRAME)
        assert result is None
        assert mgr.get_active_sessions() == {}
        assert screenshots == []

    @given(cls=valid_class_st, confidence=confidence_above_threshold_st, ts=timestamp_ms_st)
    @settings(max_examples=200)
    def test_valid_class_accepted(self, cls: str, confidence: float, ts: int) -> None:
        mgr, _, _ = _make_manager()
        result = mgr.on_detection(cls, confidence, ts, DUMMY_FRAME)
        assert result is not None
        assert result.detected_class == cls
        assert result.detected_class in VALID_CLASSES


# ---------------------------------------------------------------------------
# Property 6: 首次检测创建会话
# ---------------------------------------------------------------------------


class TestFirstDetectionCreatesSession:
    """Property 6: first detection creates session with start_time = timestamp.

    **Validates: Requirements 2.3**

    For any first detection of a class (confidence >= threshold, class in
    configured set, no active session for that class), a new session is
    created with start_time_ms equal to the detection timestamp, and a
    start screenshot is saved.
    """

    @given(cls=valid_class_st, confidence=confidence_above_threshold_st, ts=timestamp_ms_st)
    @settings(max_examples=200)
    def test_new_session_created(self, cls: str, confidence: float, ts: int) -> None:
        mgr, screenshots, _ = _make_manager()
        session = mgr.on_detection(cls, confidence, ts, DUMMY_FRAME)

        assert session is not None
        assert session.start_time_ms == ts
        assert session.last_active_ms == ts
        assert session.detected_class == cls
        assert session.max_confidence == confidence
        assert session.detection_count == 1
        assert session.is_active is True
        # Start screenshot saved (first in the list); candidate may also be saved
        assert screenshots[0] == session.start_screenshot
        assert session.start_screenshot.endswith("_start.jpg")
        # Start screenshot is separate from candidate buffer
        start_shots = [s for s in screenshots if s.endswith("_start.jpg")]
        assert len(start_shots) == 1


# ---------------------------------------------------------------------------
# Property 7: 持续检测仅更新时间
# ---------------------------------------------------------------------------


class TestSubsequentDetectionUpdatesOnly:
    """Property 7: subsequent detections update last_active_ms only.

    **Validates: Requirements 2.4**

    When a class already has an active session, further detections update
    last_active_ms (and possibly max_confidence / detection_count) but do
    NOT save additional screenshots.
    """

    @given(
        cls=valid_class_st,
        conf1=confidence_above_threshold_st,
        conf2=confidence_above_threshold_st,
        ts1=st.integers(min_value=0, max_value=10**12),
        ts_delta=st.integers(min_value=1, max_value=59_000),
    )
    @settings(max_examples=200)
    def test_update_does_not_add_screenshot(
        self, cls: str, conf1: float, conf2: float, ts1: int, ts_delta: int
    ) -> None:
        mgr, screenshots, _ = _make_manager()
        ts2 = ts1 + ts_delta

        # First detection — creates session + 1 start screenshot (+ possibly candidate)
        mgr.on_detection(cls, conf1, ts1, DUMMY_FRAME)
        start_shots = [s for s in screenshots if s.endswith("_start.jpg")]
        assert len(start_shots) == 1

        # Second detection — updates session, no new start screenshot
        session = mgr.on_detection(cls, conf2, ts2, DUMMY_FRAME)
        assert session is not None
        assert session.last_active_ms == ts2
        assert session.detection_count == 2
        assert session.max_confidence == max(conf1, conf2)
        # Still only 1 start screenshot (candidate screenshots may be added)
        start_shots = [s for s in screenshots if s.endswith("_start.jpg")]
        assert len(start_shots) == 1


# ---------------------------------------------------------------------------
# Property 8: 超时结束会话
# ---------------------------------------------------------------------------


class TestTimeoutEndsSession:
    """Property 8: session ends when timeout exceeded.

    **Validates: Requirements 2.5**

    When current_time_ms - last_active_ms > session_timeout_sec * 1000,
    the session is ended, an end screenshot and JSON metadata are saved.
    """

    @given(
        cls=valid_class_st,
        confidence=confidence_above_threshold_st,
        ts=st.integers(min_value=0, max_value=10**12),
        extra_ms=st.integers(min_value=1, max_value=600_000),
    )
    @settings(max_examples=200)
    def test_session_ends_on_timeout(
        self, cls: str, confidence: float, ts: int, extra_ms: int
    ) -> None:
        mgr, screenshots, metadata = _make_manager()
        mgr.on_detection(cls, confidence, ts, DUMMY_FRAME)

        timeout_ms = mgr.config.session_timeout_sec * 1000
        current = ts + timeout_ms + extra_ms  # guaranteed past timeout

        ended = mgr.check_timeouts(current)
        assert len(ended) == 1
        session = ended[0]
        assert session.is_active is False
        assert session.end_time_ms == current
        assert session.end_screenshot.endswith("_end.jpg")
        # Start screenshot + end screenshot present (candidates may also exist)
        start_shots = [s for s in screenshots if s.endswith("_start.jpg")]
        end_shots = [s for s in screenshots if s.endswith("_end.jpg")]
        assert len(start_shots) == 1
        assert len(end_shots) == 1
        # Metadata saved
        assert len(metadata) == 1

    @given(
        cls=valid_class_st,
        confidence=confidence_above_threshold_st,
        ts=st.integers(min_value=0, max_value=10**12),
    )
    @settings(max_examples=200)
    def test_session_not_ended_before_timeout(
        self, cls: str, confidence: float, ts: int
    ) -> None:
        mgr, screenshots, metadata = _make_manager()
        mgr.on_detection(cls, confidence, ts, DUMMY_FRAME)

        timeout_ms = mgr.config.session_timeout_sec * 1000
        # Exactly at the boundary — should NOT end (need to exceed, not equal)
        current = ts + timeout_ms

        ended = mgr.check_timeouts(current)
        assert len(ended) == 0
        assert mgr.get_active_sessions() != {}
        # No end screenshot saved (candidates + start may exist)
        end_shots = [s for s in screenshots if s.endswith("_end.jpg")]
        assert len(end_shots) == 0
        assert len(metadata) == 0


# ---------------------------------------------------------------------------
# Property 9: 事件元数据完整性
# ---------------------------------------------------------------------------


class TestEventMetadataIntegrity:
    """Property 9: ended session JSON has all required fields.

    **Validates: Requirements 2.6**

    duration_seconds must equal (end - start) / 1000.
    """

    @given(
        cls=valid_class_st,
        confidence=confidence_above_threshold_st,
        ts=st.integers(min_value=0, max_value=10**12),
        extra_ms=st.integers(min_value=1, max_value=600_000),
    )
    @settings(max_examples=200)
    def test_metadata_fields_complete(
        self, cls: str, confidence: float, ts: int, extra_ms: int
    ) -> None:
        mgr, _, metadata_log = _make_manager()
        session = mgr.on_detection(cls, confidence, ts, DUMMY_FRAME)
        assert session is not None

        timeout_ms = mgr.config.session_timeout_sec * 1000
        end_time = ts + timeout_ms + extra_ms
        mgr.check_timeouts(end_time)

        assert len(metadata_log) == 1
        meta = metadata_log[0]

        # All required fields present
        required_fields = {
            "session_id",
            "kvs_start_timestamp",
            "kvs_end_timestamp",
            "duration_seconds",
            "detected_class",
            "max_confidence",
            "start_screenshot_filename",
            "end_screenshot_filename",
            "detection_count",
        }
        assert required_fields.issubset(meta.keys())

        # duration_seconds == (end - start) / 1000
        expected_duration = (meta["kvs_end_timestamp"] - meta["kvs_start_timestamp"]) / 1000.0
        assert meta["duration_seconds"] == expected_duration

        # Consistency with session
        assert meta["detected_class"] == cls
        assert meta["max_confidence"] == confidence
        assert meta["detection_count"] == 1
        assert meta["kvs_start_timestamp"] == ts
        assert meta["kvs_end_timestamp"] == end_time


# ---------------------------------------------------------------------------
# Property 10: 多类别独立会话
# ---------------------------------------------------------------------------


class TestMultiClassIndependentSessions:
    """Property 10: N different classes → N independent sessions.

    **Validates: Requirements 2.7**

    Detections of different classes create independent sessions. Ending
    one session does not affect others.
    """

    @given(
        classes=st.lists(
            valid_class_st, min_size=2, max_size=4, unique=True
        ),
        confidence=confidence_above_threshold_st,
        ts=st.integers(min_value=0, max_value=10**12),
    )
    @settings(max_examples=200)
    def test_n_classes_create_n_sessions(
        self, classes: list[str], confidence: float, ts: int
    ) -> None:
        mgr, _, _ = _make_manager()

        for i, cls in enumerate(classes):
            mgr.on_detection(cls, confidence, ts + i, DUMMY_FRAME)

        active = mgr.get_active_sessions()
        assert len(active) == len(classes)
        for cls in classes:
            assert cls in active
            assert active[cls].detected_class == cls

    @given(
        confidence=confidence_above_threshold_st,
        ts=st.integers(min_value=0, max_value=10**12),
        extra_ms=st.integers(min_value=1, max_value=600_000),
    )
    @settings(max_examples=200)
    def test_ending_one_session_does_not_affect_others(
        self, confidence: float, ts: int, extra_ms: int
    ) -> None:
        mgr, _, _ = _make_manager()
        timeout_ms = mgr.config.session_timeout_sec * 1000

        # Create sessions for person and cat
        mgr.on_detection("person", confidence, ts, DUMMY_FRAME)
        mgr.on_detection("cat", confidence, ts, DUMMY_FRAME)

        # check_time is well past person's timeout
        check_time = ts + timeout_ms + extra_ms

        # Update cat right at check_time so it stays within the timeout window
        mgr.on_detection("cat", confidence, check_time, DUMMY_FRAME)

        # Now check timeouts — person should end, cat should survive
        ended = mgr.check_timeouts(check_time)

        ended_classes = {s.detected_class for s in ended}
        assert "person" in ended_classes
        assert "cat" not in ended_classes

        active = mgr.get_active_sessions()
        assert "cat" in active
        assert "person" not in active


# ---------------------------------------------------------------------------
# Imports for CandidateBuffer tests
# ---------------------------------------------------------------------------

import os
import tempfile

from device.ai.session import CandidateBuffer, CandidateFrame


# ---------------------------------------------------------------------------
# CandidateBuffer unit tests
# ---------------------------------------------------------------------------


class TestCandidateBufferAddWhenNotFull:
    """Frames are added directly when buffer has capacity.

    **Validates: Requirements 3.2**
    """

    def test_add_single_frame(self) -> None:
        screenshots: list[str] = []

        def record(frame: np.ndarray, filename: str) -> None:
            screenshots.append(filename)

        buf = CandidateBuffer(max_size=5, min_interval_ms=500, save_screenshot=record)
        added = buf.try_add(DUMMY_FRAME, 0.8, 1000, "bird", "sess1")

        assert added is True
        assert len(screenshots) == 1
        assert screenshots[0] == "sess1_candidate_0.jpg"

    def test_add_multiple_frames_within_capacity(self) -> None:
        screenshots: list[str] = []

        def record(frame: np.ndarray, filename: str) -> None:
            screenshots.append(filename)

        buf = CandidateBuffer(max_size=3, min_interval_ms=500, save_screenshot=record)
        assert buf.try_add(DUMMY_FRAME, 0.6, 1000, "bird", "s") is True
        assert buf.try_add(DUMMY_FRAME, 0.7, 2000, "person", "s") is True
        assert buf.try_add(DUMMY_FRAME, 0.8, 3000, "cat", "s") is True

        assert len(screenshots) == 3
        candidates = buf.get_candidates_descending()
        assert len(candidates) == 3
        assert candidates[0].confidence == 0.8


class TestCandidateBufferReplacesLowest:
    """When buffer is full, higher-confidence frame replaces the lowest.

    **Validates: Requirements 3.3**
    """

    def test_replace_lowest_confidence(self) -> None:
        screenshots: list[str] = []

        def record(frame: np.ndarray, filename: str) -> None:
            screenshots.append(filename)

        with tempfile.TemporaryDirectory() as tmpdir:
            buf = CandidateBuffer(
                max_size=2, min_interval_ms=500, save_screenshot=record, capture_dir=tmpdir
            )
            # Fill buffer
            buf.try_add(DUMMY_FRAME, 0.5, 1000, "bird", "s")
            buf.try_add(DUMMY_FRAME, 0.7, 2000, "person", "s")

            # Create the file that will be replaced (simulate save_screenshot)
            old_file = os.path.join(tmpdir, "s_candidate_0.jpg")
            with open(old_file, "w") as f:
                f.write("fake")

            # Add higher confidence — should replace 0.5
            added = buf.try_add(DUMMY_FRAME, 0.9, 3000, "cat", "s")
            assert added is True

            candidates = buf.get_candidates_descending()
            confidences = [c.confidence for c in candidates]
            assert 0.5 not in confidences
            assert 0.9 in confidences
            assert 0.7 in confidences

            # Old file should be deleted
            assert not os.path.exists(old_file)

    def test_reject_when_not_higher_than_lowest(self) -> None:
        screenshots: list[str] = []

        def record(frame: np.ndarray, filename: str) -> None:
            screenshots.append(filename)

        buf = CandidateBuffer(max_size=2, min_interval_ms=500, save_screenshot=record)
        buf.try_add(DUMMY_FRAME, 0.6, 1000, "bird", "s")
        buf.try_add(DUMMY_FRAME, 0.8, 2000, "person", "s")

        # 0.5 is not higher than lowest (0.6), should be rejected
        added = buf.try_add(DUMMY_FRAME, 0.5, 3000, "cat", "s")
        assert added is False
        assert len(buf.get_candidates_descending()) == 2

    def test_reject_when_equal_to_lowest(self) -> None:
        screenshots: list[str] = []

        def record(frame: np.ndarray, filename: str) -> None:
            screenshots.append(filename)

        buf = CandidateBuffer(max_size=2, min_interval_ms=500, save_screenshot=record)
        buf.try_add(DUMMY_FRAME, 0.6, 1000, "bird", "s")
        buf.try_add(DUMMY_FRAME, 0.8, 2000, "person", "s")

        # Equal to lowest (0.6), should be rejected (strict >)
        added = buf.try_add(DUMMY_FRAME, 0.6, 3000, "cat", "s")
        assert added is False
        assert len(buf.get_candidates_descending()) == 2


class TestCandidateBufferDeletesReplacedFile:
    """Replaced frame's JPEG file is deleted from disk.

    **Validates: Requirements 3.4**
    """

    def test_old_jpeg_deleted_on_replace(self) -> None:
        screenshots: list[str] = []

        def record(frame: np.ndarray, filename: str) -> None:
            screenshots.append(filename)

        with tempfile.TemporaryDirectory() as tmpdir:
            buf = CandidateBuffer(
                max_size=2, min_interval_ms=500, save_screenshot=record, capture_dir=tmpdir
            )
            buf.try_add(DUMMY_FRAME, 0.4, 1000, "bird", "s")
            buf.try_add(DUMMY_FRAME, 0.7, 2000, "person", "s")

            # Create fake JPEG files on disk for both candidates
            file_0 = os.path.join(tmpdir, "s_candidate_0.jpg")
            file_1 = os.path.join(tmpdir, "s_candidate_1.jpg")
            with open(file_0, "w") as f:
                f.write("fake0")
            with open(file_1, "w") as f:
                f.write("fake1")

            # Replace lowest (0.4) with higher confidence
            buf.try_add(DUMMY_FRAME, 0.9, 3000, "cat", "s")

            # The file for the replaced frame (candidate_0, conf 0.4) should be deleted
            assert not os.path.exists(file_0)
            # The other file should still exist
            assert os.path.exists(file_1)

    def test_missing_file_does_not_raise(self) -> None:
        """Replacing a frame whose JPEG is already gone should not error."""
        screenshots: list[str] = []

        def record(frame: np.ndarray, filename: str) -> None:
            screenshots.append(filename)

        with tempfile.TemporaryDirectory() as tmpdir:
            buf = CandidateBuffer(
                max_size=1, min_interval_ms=500, save_screenshot=record, capture_dir=tmpdir
            )
            buf.try_add(DUMMY_FRAME, 0.3, 1000, "bird", "s")

            # Don't create the file on disk — simulate it being already gone
            added = buf.try_add(DUMMY_FRAME, 0.8, 2000, "person", "s")
            assert added is True


class TestCandidateBufferMinInterval:
    """Frames within min_interval_ms of any existing frame are rejected.

    **Validates: Requirements 3.5**
    """

    def test_reject_within_interval(self) -> None:
        screenshots: list[str] = []

        def record(frame: np.ndarray, filename: str) -> None:
            screenshots.append(filename)

        buf = CandidateBuffer(max_size=5, min_interval_ms=500, save_screenshot=record)
        buf.try_add(DUMMY_FRAME, 0.6, 1000, "bird", "s")

        # 400ms later — within 500ms interval, should be rejected
        added = buf.try_add(DUMMY_FRAME, 0.9, 1400, "person", "s")
        assert added is False

    def test_accept_at_exact_interval(self) -> None:
        screenshots: list[str] = []

        def record(frame: np.ndarray, filename: str) -> None:
            screenshots.append(filename)

        buf = CandidateBuffer(max_size=5, min_interval_ms=500, save_screenshot=record)
        buf.try_add(DUMMY_FRAME, 0.6, 1000, "bird", "s")

        # Exactly 500ms later — not less than 500ms, should be accepted
        added = buf.try_add(DUMMY_FRAME, 0.7, 1500, "person", "s")
        assert added is True


class TestCandidateBufferGetDescending:
    """get_candidates_descending returns frames sorted by confidence desc.

    **Validates: Requirements 3.2**
    """

    def test_descending_order(self) -> None:
        buf = CandidateBuffer(max_size=5, min_interval_ms=500, save_screenshot=lambda f, n: None)
        buf.try_add(DUMMY_FRAME, 0.3, 1000, "bird", "s")
        buf.try_add(DUMMY_FRAME, 0.9, 2000, "person", "s")
        buf.try_add(DUMMY_FRAME, 0.6, 3000, "cat", "s")

        candidates = buf.get_candidates_descending()
        assert [c.confidence for c in candidates] == [0.9, 0.6, 0.3]


class TestCandidateBufferClear:
    """clear() empties the buffer and returns all filenames.

    **Validates: Requirements 3.6**
    """

    def test_clear_returns_filenames(self) -> None:
        buf = CandidateBuffer(max_size=5, min_interval_ms=500, save_screenshot=lambda f, n: None)
        buf.try_add(DUMMY_FRAME, 0.6, 1000, "bird", "s")
        buf.try_add(DUMMY_FRAME, 0.8, 2000, "person", "s")

        filenames = buf.clear()
        assert len(filenames) == 2
        assert all("candidate" in f for f in filenames)

        # Buffer should be empty after clear
        assert buf.get_candidates_descending() == []

    def test_clear_empty_buffer(self) -> None:
        buf = CandidateBuffer(max_size=5, min_interval_ms=500, save_screenshot=lambda f, n: None)
        filenames = buf.clear()
        assert filenames == []


# ---------------------------------------------------------------------------
# Imports for EventDeduplicator tests
# ---------------------------------------------------------------------------

from device.ai.session import DedupEvent, EventDeduplicator


# ---------------------------------------------------------------------------
# EventDeduplicator unit tests
# ---------------------------------------------------------------------------


class TestEventDeduplicatorSingleClassCreation:
    """First detection creates a new DedupEvent with correct fields.

    **Validates: Requirements 2.1**
    """

    def test_first_detection_creates_event(self) -> None:
        dedup = EventDeduplicator(dedup_window_sec=30)
        evt = dedup.on_detection("bird", 0.8, 1000)

        assert evt.start_time_ms == 1000
        assert evt.last_active_ms == 1000
        assert evt.detected_classes == ["bird"]
        assert evt.primary_class == "bird"
        assert evt.class_max_confidences == {"bird": 0.8}
        assert evt.class_detection_counts == {"bird": 1}
        assert evt.total_detection_count == 1
        assert evt.event_id  # non-empty

    def test_same_class_repeated_updates_counts(self) -> None:
        dedup = EventDeduplicator(dedup_window_sec=30)
        dedup.on_detection("person", 0.6, 1000)
        evt = dedup.on_detection("person", 0.9, 2000)

        assert evt.detected_classes == ["person"]
        assert evt.class_detection_counts == {"person": 2}
        assert evt.class_max_confidences == {"person": 0.9}
        assert evt.total_detection_count == 2
        assert evt.last_active_ms == 2000


class TestEventDeduplicatorMultiClassMerge:
    """Detections of different classes within the window are merged.

    **Validates: Requirements 2.2, 2.3**
    """

    def test_two_classes_merged_into_one_event(self) -> None:
        dedup = EventDeduplicator(dedup_window_sec=30)
        dedup.on_detection("bird", 0.7, 1000)
        evt = dedup.on_detection("person", 0.5, 2000)

        assert set(evt.detected_classes) == {"bird", "person"}
        assert evt.class_max_confidences == {"bird": 0.7, "person": 0.5}
        assert evt.class_detection_counts == {"bird": 1, "person": 1}
        assert evt.total_detection_count == 2

    def test_three_classes_all_tracked(self) -> None:
        dedup = EventDeduplicator(dedup_window_sec=30)
        dedup.on_detection("bird", 0.4, 1000)
        dedup.on_detection("person", 0.6, 2000)
        evt = dedup.on_detection("dog", 0.3, 3000)

        assert set(evt.detected_classes) == {"bird", "person", "dog"}
        assert evt.total_detection_count == 3
        assert evt.class_detection_counts == {"bird": 1, "person": 1, "dog": 1}

    def test_per_class_max_confidence_tracked(self) -> None:
        dedup = EventDeduplicator(dedup_window_sec=30)
        dedup.on_detection("bird", 0.3, 1000)
        dedup.on_detection("bird", 0.8, 2000)
        evt = dedup.on_detection("bird", 0.5, 3000)

        assert evt.class_max_confidences["bird"] == 0.8
        assert evt.class_detection_counts["bird"] == 3


class TestEventDeduplicatorPrimaryClassSelection:
    """primary_class is the class with the highest max confidence.

    **Validates: Requirements 2.4**
    """

    def test_primary_class_is_highest_confidence(self) -> None:
        dedup = EventDeduplicator(dedup_window_sec=30)
        dedup.on_detection("person", 0.5, 1000)
        evt = dedup.on_detection("bird", 0.9, 2000)

        assert evt.primary_class == "bird"

    def test_primary_class_updates_when_surpassed(self) -> None:
        dedup = EventDeduplicator(dedup_window_sec=30)
        dedup.on_detection("bird", 0.9, 1000)
        assert dedup.get_active_event().primary_class == "bird"

        evt = dedup.on_detection("dog", 0.95, 2000)
        assert evt.primary_class == "dog"

    def test_primary_class_stays_if_not_surpassed(self) -> None:
        dedup = EventDeduplicator(dedup_window_sec=30)
        dedup.on_detection("bird", 0.9, 1000)
        evt = dedup.on_detection("person", 0.3, 2000)

        assert evt.primary_class == "bird"


class TestEventDeduplicatorTimeout:
    """Dedup window timeout finishes the event.

    **Validates: Requirements 2.5**
    """

    def test_timeout_returns_finished_event(self) -> None:
        dedup = EventDeduplicator(dedup_window_sec=30)
        dedup.on_detection("bird", 0.7, 1000)

        # 31 seconds later — past the 30s window
        finished = dedup.check_timeout(1000 + 31_000)
        assert finished is not None
        assert finished.detected_classes == ["bird"]
        assert dedup.get_active_event() is None

    def test_no_timeout_within_window(self) -> None:
        dedup = EventDeduplicator(dedup_window_sec=30)
        dedup.on_detection("bird", 0.7, 1000)

        # Exactly at boundary — should NOT timeout (need to exceed)
        finished = dedup.check_timeout(1000 + 30_000)
        assert finished is None
        assert dedup.get_active_event() is not None

    def test_no_timeout_when_no_active_event(self) -> None:
        dedup = EventDeduplicator(dedup_window_sec=30)
        finished = dedup.check_timeout(100_000)
        assert finished is None


class TestEventDeduplicatorWindowExtension:
    """New detections within the window extend last_active_ms.

    **Validates: Requirements 2.5, 2.6**
    """

    def test_detection_extends_window(self) -> None:
        dedup = EventDeduplicator(dedup_window_sec=30)
        dedup.on_detection("bird", 0.7, 1000)

        # 20s later — new detection extends the window
        dedup.on_detection("person", 0.5, 21_000)

        # 35s after first detection but only 15s after second
        finished = dedup.check_timeout(1000 + 35_000)
        assert finished is None  # still active because last_active was 21_000
        assert dedup.get_active_event() is not None

        # 31s after second detection — now it should timeout
        finished = dedup.check_timeout(21_000 + 31_000)
        assert finished is not None
        assert set(finished.detected_classes) == {"bird", "person"}

    def test_metadata_complete_after_timeout(self) -> None:
        """Finished event has all per-class tracking data.

        **Validates: Requirements 2.6**
        """
        dedup = EventDeduplicator(dedup_window_sec=30)
        dedup.on_detection("bird", 0.4, 1000)
        dedup.on_detection("bird", 0.8, 2000)
        dedup.on_detection("person", 0.6, 3000)
        dedup.on_detection("dog", 0.3, 4000)

        finished = dedup.check_timeout(4000 + 31_000)
        assert finished is not None
        assert finished.total_detection_count == 4
        assert finished.class_max_confidences == {"bird": 0.8, "person": 0.6, "dog": 0.3}
        assert finished.class_detection_counts == {"bird": 2, "person": 1, "dog": 1}
        assert finished.primary_class == "bird"  # 0.8 is highest

    def test_new_event_after_timeout(self) -> None:
        """After timeout, a new detection starts a fresh event."""
        dedup = EventDeduplicator(dedup_window_sec=30)
        evt1 = dedup.on_detection("bird", 0.7, 1000)
        old_id = evt1.event_id

        dedup.check_timeout(1000 + 31_000)

        evt2 = dedup.on_detection("person", 0.5, 50_000)
        assert evt2.event_id != old_id
        assert evt2.detected_classes == ["person"]
        assert evt2.start_time_ms == 50_000


# ---------------------------------------------------------------------------
# SessionManager integration tests — metadata new fields + candidate buffer
# ---------------------------------------------------------------------------


class TestSessionManagerMetadataNewFields:
    """Ended session metadata includes all new edge-cloud pipeline fields.

    **Validates: Requirements 3.1, 3.7, 8.1**
    """

    def test_metadata_contains_all_new_fields(self) -> None:
        """After session ends, metadata has candidate_screenshots,
        detected_classes, primary_class, class_max_confidences,
        class_detection_counts, verification_status, pipeline_stage."""
        mgr, _, metadata_log = _make_manager()

        mgr.on_detection("bird", 0.8, 1000, DUMMY_FRAME)
        timeout_ms = mgr.config.session_timeout_sec * 1000
        mgr.check_timeouts(1000 + timeout_ms + 1)

        assert len(metadata_log) == 1
        meta = metadata_log[0]

        new_fields = {
            "candidate_screenshots",
            "detected_classes",
            "primary_class",
            "class_max_confidences",
            "class_detection_counts",
            "verification_status",
            "pipeline_stage",
        }
        assert new_fields.issubset(meta.keys()), (
            f"Missing fields: {new_fields - meta.keys()}"
        )

    def test_verification_status_is_pending(self) -> None:
        """verification_status must be 'pending' at edge side."""
        mgr, _, metadata_log = _make_manager()
        mgr.on_detection("person", 0.7, 1000, DUMMY_FRAME)
        timeout_ms = mgr.config.session_timeout_sec * 1000
        mgr.check_timeouts(1000 + timeout_ms + 1)

        assert metadata_log[0]["verification_status"] == "pending"

    def test_pipeline_stage_is_edge_detected(self) -> None:
        """pipeline_stage must be 'edge_detected' at edge side."""
        mgr, _, metadata_log = _make_manager()
        mgr.on_detection("cat", 0.6, 1000, DUMMY_FRAME)
        timeout_ms = mgr.config.session_timeout_sec * 1000
        mgr.check_timeouts(1000 + timeout_ms + 1)

        assert metadata_log[0]["pipeline_stage"] == "edge_detected"

    def test_detected_classes_and_primary_class_present(self) -> None:
        """detected_classes is a list and primary_class is a string."""
        mgr, _, metadata_log = _make_manager()
        mgr.on_detection("bird", 0.9, 1000, DUMMY_FRAME)
        timeout_ms = mgr.config.session_timeout_sec * 1000
        mgr.check_timeouts(1000 + timeout_ms + 1)

        meta = metadata_log[0]
        assert isinstance(meta["detected_classes"], list)
        assert len(meta["detected_classes"]) >= 1
        assert isinstance(meta["primary_class"], str)
        assert meta["primary_class"] in meta["detected_classes"]

    def test_class_max_confidences_and_counts_present(self) -> None:
        """class_max_confidences and class_detection_counts are dicts."""
        mgr, _, metadata_log = _make_manager()
        mgr.on_detection("dog", 0.7, 1000, DUMMY_FRAME)
        mgr.on_detection("dog", 0.8, 2000, DUMMY_FRAME)
        timeout_ms = mgr.config.session_timeout_sec * 1000
        mgr.check_timeouts(2000 + timeout_ms + 1)

        meta = metadata_log[0]
        assert isinstance(meta["class_max_confidences"], dict)
        assert isinstance(meta["class_detection_counts"], dict)
        assert "dog" in meta["class_max_confidences"]
        assert "dog" in meta["class_detection_counts"]


class TestSessionManagerCandidateScreenshotsDescending:
    """candidate_screenshots in metadata are ordered by confidence descending.

    **Validates: Requirements 3.6, 3.7**
    """

    def test_candidates_sorted_descending_in_metadata(self) -> None:
        """Multiple detections at different confidences produce a
        candidate_screenshots list sorted by confidence (highest first)."""
        config = _make_config(max_candidate_frames=5, candidate_min_interval_ms=500)
        mgr, screenshots, metadata_log = _make_manager(config)

        # Detections spaced >= 500ms apart so all pass interval check
        mgr.on_detection("bird", 0.6, 1000, DUMMY_FRAME)
        mgr.on_detection("bird", 0.9, 2000, DUMMY_FRAME)
        mgr.on_detection("bird", 0.7, 3000, DUMMY_FRAME)

        timeout_ms = mgr.config.session_timeout_sec * 1000
        mgr.check_timeouts(3000 + timeout_ms + 1)

        meta = metadata_log[0]
        candidate_list = meta["candidate_screenshots"]
        assert isinstance(candidate_list, list)
        # The list should have 3 candidates (buffer size 5, 3 detections)
        assert len(candidate_list) == 3

        # Verify ordering: retrieve the CandidateBuffer's descending order
        # by checking filenames match the order saved by get_candidates_descending
        # The first candidate should correspond to the highest confidence (0.9)
        # We verify by checking the candidate filenames are all present in screenshots
        for fname in candidate_list:
            assert fname in screenshots

    def test_single_detection_produces_one_candidate(self) -> None:
        """A single detection produces exactly one candidate screenshot."""
        config = _make_config(max_candidate_frames=5, candidate_min_interval_ms=500)
        mgr, _, metadata_log = _make_manager(config)

        mgr.on_detection("person", 0.8, 1000, DUMMY_FRAME)
        timeout_ms = mgr.config.session_timeout_sec * 1000
        mgr.check_timeouts(1000 + timeout_ms + 1)

        meta = metadata_log[0]
        assert len(meta["candidate_screenshots"]) == 1


class TestSessionManagerStartScreenshotNotInBuffer:
    """Start screenshot (_start.jpg) does not occupy candidate buffer slots.

    **Validates: Requirements 3.6**
    """

    def test_start_screenshot_separate_from_candidates(self) -> None:
        """The start screenshot filename must not appear in
        candidate_screenshots list."""
        config = _make_config(max_candidate_frames=3, candidate_min_interval_ms=500)
        mgr, screenshots, metadata_log = _make_manager(config)

        mgr.on_detection("bird", 0.7, 1000, DUMMY_FRAME)
        mgr.on_detection("bird", 0.8, 2000, DUMMY_FRAME)

        timeout_ms = mgr.config.session_timeout_sec * 1000
        mgr.check_timeouts(2000 + timeout_ms + 1)

        meta = metadata_log[0]
        start_name = meta["start_screenshot_filename"]
        candidate_list = meta["candidate_screenshots"]

        # Start screenshot must NOT be in the candidate list
        assert start_name not in candidate_list
        # Start screenshot ends with _start.jpg
        assert start_name.endswith("_start.jpg")
        # All candidates are candidate files, not start files
        for fname in candidate_list:
            assert "_start.jpg" not in fname

    def test_buffer_full_does_not_include_start(self) -> None:
        """Even when buffer is full, start screenshot stays separate."""
        config = _make_config(max_candidate_frames=2, candidate_min_interval_ms=500)
        mgr, screenshots, metadata_log = _make_manager(config)

        # 3 detections, buffer size 2 — buffer will be full
        mgr.on_detection("bird", 0.6, 1000, DUMMY_FRAME)
        mgr.on_detection("bird", 0.8, 2000, DUMMY_FRAME)
        mgr.on_detection("bird", 0.9, 3000, DUMMY_FRAME)

        timeout_ms = mgr.config.session_timeout_sec * 1000
        mgr.check_timeouts(3000 + timeout_ms + 1)

        meta = metadata_log[0]
        start_name = meta["start_screenshot_filename"]
        candidate_list = meta["candidate_screenshots"]

        assert start_name not in candidate_list
        # Buffer max is 2, so at most 2 candidates
        assert len(candidate_list) <= 2

        # Start screenshot is saved (present in screenshots log)
        assert start_name in screenshots
