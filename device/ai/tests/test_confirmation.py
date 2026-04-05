"""Unit tests for DetectorConfig confirmation window fields.

Validates that the new confirmation_window_size and confirmation_min_count
fields have correct defaults, are parsed from INI files, and can be
overridden via environment variables.

Requirements: 4.1, 4.2, 4.4
"""

from __future__ import annotations

import os
import tempfile

from device.ai.config import DetectorConfig


class TestDetectorConfigConfirmationDefaults:
    """Test default values for confirmation window config fields."""

    def test_default_confirmation_window_size(self):
        """confirmation_window_size defaults to 3."""
        config = DetectorConfig()
        assert config.confirmation_window_size == 3

    def test_default_confirmation_min_count(self):
        """confirmation_min_count defaults to 2."""
        config = DetectorConfig()
        assert config.confirmation_min_count == 2


class TestDetectorConfigConfirmationINI:
    """Test INI file parsing for confirmation window fields."""

    def test_ini_parses_confirmation_fields(self):
        """Fields are read from [ai_summary] section in INI file."""
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".ini", delete=False
        ) as tmp:
            tmp.write("[ai_summary]\n")
            tmp.write("confirmation_window_size = 5\n")
            tmp.write("confirmation_min_count = 4\n")
            tmp_path = tmp.name

        try:
            config = DetectorConfig.from_ini(tmp_path)
            assert config.confirmation_window_size == 5
            assert config.confirmation_min_count == 4
        finally:
            os.unlink(tmp_path)

    def test_ini_missing_fields_uses_defaults(self):
        """When INI section exists but fields are absent, defaults apply."""
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".ini", delete=False
        ) as tmp:
            tmp.write("[ai_summary]\n")
            tmp.write("export_fps = 1.0\n")
            tmp_path = tmp.name

        try:
            config = DetectorConfig.from_ini(tmp_path)
            assert config.confirmation_window_size == 3
            assert config.confirmation_min_count == 2
        finally:
            os.unlink(tmp_path)


class TestDetectorConfigConfirmationEnvOverride:
    """Test environment variable overrides for confirmation window fields."""

    def test_env_overrides_ini_window_size(self):
        """AI_SUMMARY_CONFIRMATION_WINDOW_SIZE overrides INI value."""
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".ini", delete=False
        ) as tmp:
            tmp.write("[ai_summary]\n")
            tmp.write("confirmation_window_size = 5\n")
            tmp_path = tmp.name

        env_key = "AI_SUMMARY_CONFIRMATION_WINDOW_SIZE"
        old = os.environ.get(env_key)
        try:
            os.environ[env_key] = "7"
            config = DetectorConfig.from_ini(tmp_path)
            assert config.confirmation_window_size == 7
        finally:
            if old is None:
                os.environ.pop(env_key, None)
            else:
                os.environ[env_key] = old
            os.unlink(tmp_path)

    def test_env_overrides_ini_min_count(self):
        """AI_SUMMARY_CONFIRMATION_MIN_COUNT overrides INI value."""
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".ini", delete=False
        ) as tmp:
            tmp.write("[ai_summary]\n")
            tmp.write("confirmation_min_count = 2\n")
            tmp_path = tmp.name

        env_key = "AI_SUMMARY_CONFIRMATION_MIN_COUNT"
        old = os.environ.get(env_key)
        try:
            os.environ[env_key] = "6"
            config = DetectorConfig.from_ini(tmp_path)
            assert config.confirmation_min_count == 6
        finally:
            if old is None:
                os.environ.pop(env_key, None)
            else:
                os.environ[env_key] = old
            os.unlink(tmp_path)

    def test_env_overrides_default_when_no_ini(self):
        """Env vars override defaults even when INI has no section."""
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".ini", delete=False
        ) as tmp:
            tmp.write("")  # empty file, no [ai_summary] section
            tmp_path = tmp.name

        keys = {
            "AI_SUMMARY_CONFIRMATION_WINDOW_SIZE": "8",
            "AI_SUMMARY_CONFIRMATION_MIN_COUNT": "3",
        }
        old_vals = {k: os.environ.get(k) for k in keys}
        try:
            for k, v in keys.items():
                os.environ[k] = v
            config = DetectorConfig.from_ini(tmp_path)
            assert config.confirmation_window_size == 8
            assert config.confirmation_min_count == 3
        finally:
            for k, old in old_vals.items():
                if old is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = old
            os.unlink(tmp_path)


# ---------------------------------------------------------------------------
# Integration tests: process_frame + confirmation window
# Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 5.1, 5.2, 5.3
# ---------------------------------------------------------------------------

import logging
from unittest.mock import MagicMock, patch, PropertyMock

import numpy as np

from device.ai.activity_detector import ActivityDetector, COCO_CLASS_MAP
from device.ai.confirmation import DetectionConfirmationWindow


def _make_mock_boxes(detections: list[tuple[int, float]]):
    """Build a mock ``boxes`` object for YOLO results.

    Args:
        detections: list of (class_id, confidence) tuples.

    Returns a mock with ``cls``, ``conf``, ``xyxy`` attributes that
    behave like tensors (support ``.item()`` and indexing).
    """
    mock_boxes = MagicMock()
    mock_boxes.__len__ = lambda self: len(detections)

    cls_items = []
    conf_items = []
    xyxy_items = []
    for cls_id, conf in detections:
        cls_mock = MagicMock()
        cls_mock.item.return_value = cls_id
        cls_items.append(cls_mock)

        conf_mock = MagicMock()
        conf_mock.item.return_value = conf
        conf_items.append(conf_mock)

        # xyxy: 4-element tensor-like with .item() support
        coords = [MagicMock() for _ in range(4)]
        for idx, val in enumerate([10, 10, 100, 100]):
            coords[idx].item.return_value = val
        xyxy_mock = MagicMock()
        xyxy_mock.__getitem__ = lambda self, i, _c=coords: _c[i]
        xyxy_items.append(xyxy_mock)

    mock_boxes.cls = MagicMock()
    mock_boxes.cls.__getitem__ = lambda self, i, _c=cls_items: _c[i]
    mock_boxes.conf = MagicMock()
    mock_boxes.conf.__getitem__ = lambda self, i, _c=conf_items: _c[i]
    mock_boxes.xyxy = MagicMock()
    mock_boxes.xyxy.__getitem__ = lambda self, i, _c=xyxy_items: _c[i]

    return mock_boxes


def _make_yolo_result(detections: list[tuple[int, float]]):
    """Return a single mock YOLO result object."""
    result = MagicMock()
    result.boxes = _make_mock_boxes(detections)
    return result


def _build_detector(window_size: int = 3, min_count: int = 2):
    """Create an ActivityDetector with mocked YOLO and SessionManager.

    Returns ``(detector, mock_session_mgr)`` so callers can inspect
    ``on_detection`` calls.

    The YOLO import happens inside ``ActivityDetector.__init__`` via
    ``from ultralytics import YOLO``, so we mock the ``ultralytics``
    module in ``sys.modules`` before constructing the detector.
    """
    import sys

    config = DetectorConfig(
        confirmation_window_size=window_size,
        confirmation_min_count=min_count,
        confidence_threshold=0.3,
        capture_dir="/tmp/test_captures",
    )

    mock_model = MagicMock()
    mock_model.model_name = "yolo11n-test"

    mock_ultralytics = MagicMock()
    mock_ultralytics.YOLO.return_value = mock_model

    with patch.dict(sys.modules, {"ultralytics": mock_ultralytics}):
        detector = ActivityDetector(config)

    # Replace session manager with a mock
    mock_session_mgr = MagicMock()
    mock_session_mgr.get_active_sessions.return_value = {}
    mock_session_mgr.check_timeouts.return_value = []
    detector.session_mgr = mock_session_mgr

    # Stub disk protection to always pass
    detector._file_count_ok = MagicMock(return_value=True)
    detector._disk_space_ok = MagicMock(return_value=True)

    return detector, mock_session_mgr


class TestProcessFrameConfirmationIntegration:
    """Integration tests for process_frame with confirmation window.

    Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 5.1, 5.2, 5.3
    """

    def test_single_frame_does_not_trigger_on_detection(self):
        """With N=3, M=2 a single detection frame must NOT call on_detection.

        Requirements: 3.2, 3.3
        """
        detector, mock_sm = _build_detector(window_size=3, min_count=2)
        frame = np.zeros((480, 640, 3), dtype=np.uint8)

        # YOLO returns one "person" detection
        detector.model.predict.return_value = [
            _make_yolo_result([(0, 0.85)]),  # person
        ]

        detector.process_frame(frame, timestamp_ms=1000)

        mock_sm.on_detection.assert_not_called()

    def test_two_consecutive_frames_triggers_on_detection(self):
        """Same class in 2 consecutive frames DOES trigger on_detection on frame 2.

        Requirements: 3.1, 3.2
        """
        detector, mock_sm = _build_detector(window_size=3, min_count=2)
        frame = np.zeros((480, 640, 3), dtype=np.uint8)

        detector.model.predict.return_value = [
            _make_yolo_result([(0, 0.85)]),  # person
        ]

        # Frame 1 — pending
        detector.process_frame(frame, timestamp_ms=1000)
        mock_sm.on_detection.assert_not_called()

        # Frame 2 — confirmed
        detector.process_frame(frame, timestamp_ms=1500)
        assert mock_sm.on_detection.call_count == 1
        args = mock_sm.on_detection.call_args
        assert args[0][0] == "person"  # class name
        assert args[0][1] == 0.85      # confidence

    def test_different_classes_are_independent(self):
        """Confirming 'person' does NOT affect 'bird' confirmation status.

        Requirements: 2.3, 3.2, 3.3
        """
        detector, mock_sm = _build_detector(window_size=3, min_count=2)
        frame = np.zeros((480, 640, 3), dtype=np.uint8)

        # Frame 1: person only
        detector.model.predict.return_value = [
            _make_yolo_result([(0, 0.85)]),  # person
        ]
        detector.process_frame(frame, timestamp_ms=1000)
        mock_sm.on_detection.assert_not_called()

        # Frame 2: person + bird
        detector.model.predict.return_value = [
            _make_yolo_result([(0, 0.80), (14, 0.60)]),  # person + bird
        ]
        detector.process_frame(frame, timestamp_ms=1500)

        # person should be confirmed (2/3), bird should NOT (1/3)
        assert mock_sm.on_detection.call_count == 1
        called_class = mock_sm.on_detection.call_args[0][0]
        assert called_class == "person"

        # Frame 3: bird only — now bird reaches 2/3
        mock_sm.on_detection.reset_mock()
        detector.model.predict.return_value = [
            _make_yolo_result([(14, 0.65)]),  # bird
        ]
        detector.process_frame(frame, timestamp_ms=2000)

        assert mock_sm.on_detection.call_count == 1
        called_class = mock_sm.on_detection.call_args[0][0]
        assert called_class == "bird"


class TestProcessFrameDiskProtectionWithConfirmation:
    """Verify disk protection logic is compatible with confirmation window.

    Requirements: 3.4, 3.5
    """

    def test_disk_protection_blocks_new_session_after_confirmation(self):
        """Even after confirmation, disk protection can block new sessions.

        Requirements: 3.4
        """
        detector, mock_sm = _build_detector(window_size=3, min_count=2)
        frame = np.zeros((480, 640, 3), dtype=np.uint8)

        detector.model.predict.return_value = [
            _make_yolo_result([(0, 0.85)]),
        ]

        # Frame 1 — pending
        detector.process_frame(frame, timestamp_ms=1000)

        # Frame 2 — confirmed, but disk is full
        detector._file_count_ok = MagicMock(return_value=False)
        detector._disk_space_ok = MagicMock(return_value=True)
        mock_sm.get_active_sessions.return_value = {}  # no active session

        detector.process_frame(frame, timestamp_ms=1500)

        # on_detection should NOT be called because disk protection blocks it
        mock_sm.on_detection.assert_not_called()

    def test_disk_protection_allows_existing_session_update(self):
        """Disk protection still allows updating an existing active session.

        Requirements: 3.4
        """
        detector, mock_sm = _build_detector(window_size=3, min_count=2)
        frame = np.zeros((480, 640, 3), dtype=np.uint8)

        detector.model.predict.return_value = [
            _make_yolo_result([(0, 0.85)]),
        ]

        # Frame 1 — pending
        detector.process_frame(frame, timestamp_ms=1000)

        # Frame 2 — confirmed, disk full but session already active
        detector._file_count_ok = MagicMock(return_value=False)
        detector._disk_space_ok = MagicMock(return_value=True)
        mock_sm.get_active_sessions.return_value = {"person": MagicMock()}

        detector.process_frame(frame, timestamp_ms=1500)

        # on_detection IS called because session already exists
        assert mock_sm.on_detection.call_count == 1


class TestProcessFrameConfirmationLogging:
    """Verify log output for confirmation window integration.

    Requirements: 5.1, 5.2, 5.3
    """

    def test_startup_log_contains_confirmation_window_params(self, caplog):
        """Startup log must include 'confirmation_window=3/2'.

        Requirements: 5.3
        """
        with caplog.at_level(logging.INFO, logger="device.ai.activity_detector"):
            _build_detector(window_size=3, min_count=2)

        startup_msgs = [r.message for r in caplog.records]
        assert any("confirmation_window=3/2" in msg for msg in startup_msgs), (
            f"Expected 'confirmation_window=3/2' in startup log, got: {startup_msgs}"
        )

    def test_confirmed_class_logs_info(self, caplog):
        """Confirmed class must produce INFO log 'confirmed: X/Y frames'.

        Requirements: 5.1
        """
        detector, _ = _build_detector(window_size=3, min_count=2)
        frame = np.zeros((480, 640, 3), dtype=np.uint8)

        detector.model.predict.return_value = [
            _make_yolo_result([(0, 0.85)]),
        ]

        with caplog.at_level(logging.DEBUG, logger="device.ai.activity_detector"):
            detector.process_frame(frame, timestamp_ms=1000)
            caplog.clear()
            detector.process_frame(frame, timestamp_ms=1500)

        info_msgs = [
            r.message for r in caplog.records if r.levelno == logging.INFO
        ]
        assert any("person confirmed:" in msg and "frames" in msg for msg in info_msgs), (
            f"Expected INFO log with 'person confirmed: .../... frames', got: {info_msgs}"
        )

    def test_pending_class_logs_debug(self, caplog):
        """Pending (unconfirmed) class must produce DEBUG log 'pending: X/Y frames'.

        Requirements: 5.2
        """
        detector, _ = _build_detector(window_size=3, min_count=2)
        frame = np.zeros((480, 640, 3), dtype=np.uint8)

        detector.model.predict.return_value = [
            _make_yolo_result([(0, 0.85)]),
        ]

        with caplog.at_level(logging.DEBUG, logger="device.ai.activity_detector"):
            detector.process_frame(frame, timestamp_ms=1000)

        debug_msgs = [
            r.message for r in caplog.records if r.levelno == logging.DEBUG
        ]
        assert any("person pending:" in msg and "frames" in msg for msg in debug_msgs), (
            f"Expected DEBUG log with 'person pending: .../... frames', got: {debug_msgs}"
        )
