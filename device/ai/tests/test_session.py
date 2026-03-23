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
        # Start screenshot saved
        assert len(screenshots) == 1
        assert screenshots[0] == session.start_screenshot
        assert session.start_screenshot.endswith("_start.jpg")


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

        # First detection — creates session + 1 screenshot
        mgr.on_detection(cls, conf1, ts1, DUMMY_FRAME)
        assert len(screenshots) == 1

        # Second detection — updates session, no new screenshot
        session = mgr.on_detection(cls, conf2, ts2, DUMMY_FRAME)
        assert session is not None
        assert session.last_active_ms == ts2
        assert session.detection_count == 2
        assert session.max_confidence == max(conf1, conf2)
        # Still only 1 screenshot (the start one)
        assert len(screenshots) == 1


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
        # 1 start screenshot + 1 end screenshot
        assert len(screenshots) == 2
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
        # Only start screenshot, no end screenshot
        assert len(screenshots) == 1
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
