"""Property-based tests for the S3 Uploader module.

Uses Hypothesis to verify correctness properties of S3Uploader:
- S3 key path format
- Exponential backoff retry intervals
- Atomic upload (partial failure keeps local files)
- DynamoDB TTL calculation
- Oldest-first upload ordering when storage exceeds limit
"""

from __future__ import annotations

import json
import os
import re
import shutil
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List
from unittest.mock import MagicMock, patch

from hypothesis import given, settings, strategies as st

from device.ai.config import DetectorConfig
from device.ai.s3_uploader import S3Uploader, _RETRY_EXHAUSTED


# ---------------------------------------------------------------------------
# Strategies
# ---------------------------------------------------------------------------

# device_id: alphanumeric + hyphens, 1-30 chars (realistic device identifiers)
device_id_st = st.from_regex(r"[a-zA-Z0-9][a-zA-Z0-9\-]{0,29}", fullmatch=True)

# timestamp_ms: Unix millisecond timestamp in a reasonable range
# From 2000-01-01 to 2099-12-31 in ms
timestamp_ms_st = st.integers(min_value=946684800000, max_value=4102444800000)

# detected class
detected_class_st = st.sampled_from(["person", "cat", "dog", "bird"])

# file extension
ext_st = st.sampled_from(["start.jpg", "end.jpg", "json"])

# retry attempt index
retry_n_st = st.integers(min_value=0, max_value=19)

# initial_interval: positive float
initial_interval_st = st.floats(min_value=0.001, max_value=100.0, allow_nan=False, allow_infinity=False)

# max_interval: positive float
max_interval_st = st.floats(min_value=0.001, max_value=3600.0, allow_nan=False, allow_infinity=False)

# max_retries
max_retries_st = st.integers(min_value=1, max_value=20)

# confidence
confidence_st = st.floats(min_value=0.0, max_value=1.0, allow_nan=False)

# duration
duration_st = st.floats(min_value=0.0, max_value=86400.0, allow_nan=False)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_config(**overrides) -> DetectorConfig:
    """Build a DetectorConfig with sensible test defaults."""
    defaults = dict(
        detect_classes=["person", "cat", "dog", "bird"],
        confidence_threshold=0.5,
        session_timeout_sec=60,
        s3_bucket="test-bucket",
        s3_prefix="captures",
        upload_retry_count=5,
        upload_retry_interval_sec=1.0,
        dynamodb_table="test-events",
        event_ttl_days=90,
        capture_dir="/tmp/test_captures",
        capture_max_size_mb=200,
    )
    defaults.update(overrides)
    return DetectorConfig(**defaults)


def _make_uploader(config: DetectorConfig | None = None, device_id: str = "test-device") -> S3Uploader:
    """Create an S3Uploader with mocked AWS clients."""
    cfg = config or _make_config()
    mock_s3 = MagicMock()
    mock_ddb = MagicMock()
    mock_table = MagicMock()
    mock_ddb.Table.return_value = mock_table
    return S3Uploader(
        config=cfg,
        device_id=device_id,
        s3_client=mock_s3,
        dynamodb_resource=mock_ddb,
    )


# ---------------------------------------------------------------------------
# Property 11: S3 路径格式正确性
# ---------------------------------------------------------------------------


class TestS3KeyFormat:
    """Property 11: S3 key path format correctness.

    Feature: ai-video-summary, Property 11: S3 路径格式正确性

    **Validates: Requirements 3.2**

    For any valid device_id, Unix ms timestamp, and class,
    build_s3_key() output matches regex
    captures/{device_id}/\\d{4}-\\d{2}-\\d{2}/\\d+_{class}\\.(jpg|json)
    and date matches UTC date of timestamp.
    """

    @given(
        device_id=device_id_st,
        timestamp_ms=timestamp_ms_st,
        cls=detected_class_st,
        ext=ext_st,
    )
    @settings(max_examples=200)
    def test_s3_key_matches_expected_pattern(
        self, device_id: str, timestamp_ms: int, cls: str, ext: str
    ) -> None:
        key = S3Uploader.build_s3_key(device_id, timestamp_ms, cls, ext)

        # Build expected regex: captures/{device_id}/{YYYY-MM-DD}/{timestamp}_{class}.{ext}
        escaped_device = re.escape(device_id)
        escaped_cls = re.escape(cls)
        escaped_ext = re.escape(ext)
        pattern = rf"^captures/{escaped_device}/\d{{4}}-\d{{2}}-\d{{2}}/\d+_{escaped_cls}\.{escaped_ext}$"
        assert re.match(pattern, key), f"Key '{key}' does not match pattern '{pattern}'"

    @given(
        device_id=device_id_st,
        timestamp_ms=timestamp_ms_st,
        cls=detected_class_st,
        ext=ext_st,
    )
    @settings(max_examples=200)
    def test_s3_key_date_matches_utc_date(
        self, device_id: str, timestamp_ms: int, cls: str, ext: str
    ) -> None:
        key = S3Uploader.build_s3_key(device_id, timestamp_ms, cls, ext)

        # Extract date from key
        parts = key.split("/")
        # captures / device_id / YYYY-MM-DD / filename
        date_in_key = parts[2]

        # Compute expected UTC date
        dt = datetime.fromtimestamp(timestamp_ms / 1000.0, tz=timezone.utc)
        expected_date = dt.strftime("%Y-%m-%d")

        assert date_in_key == expected_date, (
            f"Date in key '{date_in_key}' != expected UTC date '{expected_date}' "
            f"for timestamp_ms={timestamp_ms}"
        )

    @given(
        device_id=device_id_st,
        timestamp_ms=timestamp_ms_st,
        cls=detected_class_st,
        ext=ext_st,
    )
    @settings(max_examples=200)
    def test_s3_key_contains_timestamp(
        self, device_id: str, timestamp_ms: int, cls: str, ext: str
    ) -> None:
        key = S3Uploader.build_s3_key(device_id, timestamp_ms, cls, ext)

        # The filename portion should start with the timestamp
        filename = key.split("/")[-1]
        assert filename.startswith(f"{timestamp_ms}_"), (
            f"Filename '{filename}' should start with '{timestamp_ms}_'"
        )


# ---------------------------------------------------------------------------
# Property 12: 指数退避重试间隔
# ---------------------------------------------------------------------------


class TestExponentialBackoffIntervals:
    """Property 12: Exponential backoff retry intervals.

    Feature: ai-video-summary, Property 12: 指数退避重试间隔

    **Validates: Requirements 3.4**

    For retry N (0 ≤ N < max_retries),
    wait = min(initial_interval * 2^N, max_interval),
    default initial=1s, max=60s.
    """

    @given(
        retry_n=retry_n_st,
        initial_interval=initial_interval_st,
        max_interval=max_interval_st,
    )
    @settings(max_examples=200)
    def test_backoff_interval_formula(
        self, retry_n: int, initial_interval: float, max_interval: float
    ) -> None:
        """Verify the wait time for attempt N follows the formula."""
        expected_wait = min(initial_interval * (2 ** retry_n), max_interval)

        # We test the formula by running retry_with_backoff with a function
        # that always fails, and capturing the sleep calls.
        uploader = _make_uploader()
        sleep_calls: list[float] = []

        def mock_sleep(seconds: float) -> None:
            sleep_calls.append(seconds)

        call_count = 0

        def always_fail() -> None:
            nonlocal call_count
            call_count += 1
            raise RuntimeError("fail")

        # We need at least retry_n + 1 retries to observe the Nth sleep
        max_retries = retry_n + 1

        with patch("device.ai.s3_uploader.time.sleep", side_effect=mock_sleep):
            result = uploader.retry_with_backoff(
                always_fail,
                max_retries=max_retries,
                initial_interval=initial_interval,
                max_interval=max_interval,
            )

        assert result is _RETRY_EXHAUSTED
        assert len(sleep_calls) == max_retries

        # Check the Nth sleep interval
        actual_wait = sleep_calls[retry_n]
        assert abs(actual_wait - expected_wait) < 1e-6, (
            f"Attempt {retry_n}: expected wait={expected_wait}, got={actual_wait}"
        )

    @given(data=st.data())
    @settings(max_examples=100)
    def test_default_backoff_values(self, data: st.DataObject) -> None:
        """With defaults (initial=1s, max=60s), verify all intervals."""
        max_retries = data.draw(st.integers(min_value=1, max_value=10))
        uploader = _make_uploader()
        sleep_calls: list[float] = []

        def mock_sleep(seconds: float) -> None:
            sleep_calls.append(seconds)

        def always_fail() -> None:
            raise RuntimeError("fail")

        with patch("device.ai.s3_uploader.time.sleep", side_effect=mock_sleep):
            uploader.retry_with_backoff(
                always_fail,
                max_retries=max_retries,
                initial_interval=1.0,
                max_interval=60.0,
            )

        for i, actual in enumerate(sleep_calls):
            expected = min(1.0 * (2 ** i), 60.0)
            assert abs(actual - expected) < 1e-6, (
                f"Attempt {i}: expected {expected}, got {actual}"
            )


# ---------------------------------------------------------------------------
# Property 13: 原子上传——部分失败不删除本地文件
# ---------------------------------------------------------------------------


class TestAtomicUploadPartialFailure:
    """Property 13: Atomic upload — partial failure keeps local files.

    Feature: ai-video-summary, Property 13: 原子上传——部分失败不删除本地文件

    **Validates: Requirements 3.7**

    If JPEG upload succeeds but JSON fails (or vice versa), neither
    local file is deleted. Only when both succeed are locals deleted.
    """

    @given(
        cls=detected_class_st,
        timestamp_ms=timestamp_ms_st,
        fail_index=st.integers(min_value=0, max_value=2),
    )
    @settings(max_examples=200)
    def test_partial_failure_preserves_all_local_files(
        self, cls: str, timestamp_ms: int, fail_index: int
    ) -> None:
        """When one upload fails, no local files are deleted."""
        tmp_dir = tempfile.mkdtemp()
        try:
            self._run_partial_failure(cls, timestamp_ms, fail_index, Path(tmp_dir))
        finally:
            shutil.rmtree(tmp_dir, ignore_errors=True)

    def _run_partial_failure(
        self, cls: str, timestamp_ms: int, fail_index: int, tmp_path: Path
    ) -> None:
        # Set up capture directory with test files
        capture_dir = tmp_path / "captures"
        capture_dir.mkdir()

        session_id = "test-session-001"
        start_jpg = capture_dir / f"{session_id}_start.jpg"
        end_jpg = capture_dir / f"{session_id}_end.jpg"
        meta_json = capture_dir / f"{session_id}_metadata.json"

        metadata = {
            "session_id": session_id,
            "detected_class": cls,
            "kvs_start_timestamp": timestamp_ms,
            "kvs_end_timestamp": timestamp_ms + 60000,
            "duration_seconds": 60.0,
            "max_confidence": 0.95,
            "detection_count": 10,
            "start_screenshot_filename": start_jpg.name,
            "end_screenshot_filename": end_jpg.name,
        }

        start_jpg.write_bytes(b"\xff\xd8\xff\xe0fake-jpeg-start")
        end_jpg.write_bytes(b"\xff\xd8\xff\xe0fake-jpeg-end")
        meta_json.write_text(json.dumps(metadata))

        config = _make_config(
            capture_dir=str(capture_dir),
            upload_retry_count=1,
            upload_retry_interval_sec=0.001,
        )

        mock_s3 = MagicMock()
        mock_ddb = MagicMock()
        mock_table = MagicMock()
        mock_ddb.Table.return_value = mock_table

        # Make the fail_index-th upload_file call raise an exception
        upload_call_count = 0

        def upload_side_effect(*args, **kwargs):
            nonlocal upload_call_count
            current = upload_call_count
            upload_call_count += 1
            if current == fail_index:
                raise Exception("S3 upload failed")

        mock_s3.upload_file.side_effect = upload_side_effect

        uploader = S3Uploader(
            config=config,
            device_id="test-device",
            s3_client=mock_s3,
            dynamodb_resource=mock_ddb,
        )

        with patch("device.ai.s3_uploader.time.sleep"):
            uploader.upload_event(str(start_jpg), str(meta_json))

        # Files should have been moved to errors dir (not deleted)
        # The key property: original files are NOT silently deleted
        # when upload partially fails. They either remain or go to errors/.
        errors_dir = capture_dir / "errors"

        # Count files that still exist (either in capture_dir or errors_dir)
        surviving_start = start_jpg.exists() or (errors_dir / start_jpg.name).exists()
        surviving_end = end_jpg.exists() or (errors_dir / end_jpg.name).exists()
        surviving_json = meta_json.exists() or (errors_dir / meta_json.name).exists()

        assert surviving_start, "Start JPEG was lost on partial failure"
        assert surviving_end, "End JPEG was lost on partial failure"
        assert surviving_json, "Metadata JSON was lost on partial failure"

    @given(
        cls=detected_class_st,
        timestamp_ms=timestamp_ms_st,
    )
    @settings(max_examples=100)
    def test_all_succeed_deletes_local_files(
        self, cls: str, timestamp_ms: int
    ) -> None:
        """When all uploads succeed, local files are deleted."""
        tmp_dir = tempfile.mkdtemp()
        try:
            self._run_all_succeed(cls, timestamp_ms, Path(tmp_dir))
        finally:
            shutil.rmtree(tmp_dir, ignore_errors=True)

    def _run_all_succeed(self, cls: str, timestamp_ms: int, tmp_path: Path) -> None:
        capture_dir = tmp_path / "captures"
        capture_dir.mkdir()

        session_id = "test-session-ok"
        start_jpg = capture_dir / f"{session_id}_start.jpg"
        end_jpg = capture_dir / f"{session_id}_end.jpg"
        meta_json = capture_dir / f"{session_id}_metadata.json"

        metadata = {
            "session_id": session_id,
            "detected_class": cls,
            "kvs_start_timestamp": timestamp_ms,
            "kvs_end_timestamp": timestamp_ms + 60000,
            "duration_seconds": 60.0,
            "max_confidence": 0.85,
            "detection_count": 5,
            "start_screenshot_filename": start_jpg.name,
            "end_screenshot_filename": end_jpg.name,
        }

        start_jpg.write_bytes(b"\xff\xd8\xff\xe0fake-jpeg-start")
        end_jpg.write_bytes(b"\xff\xd8\xff\xe0fake-jpeg-end")
        meta_json.write_text(json.dumps(metadata))

        config = _make_config(capture_dir=str(capture_dir))

        mock_s3 = MagicMock()
        mock_ddb = MagicMock()
        mock_table = MagicMock()
        mock_ddb.Table.return_value = mock_table

        uploader = S3Uploader(
            config=config,
            device_id="test-device",
            s3_client=mock_s3,
            dynamodb_resource=mock_ddb,
        )

        with patch("device.ai.s3_uploader.time.sleep"):
            uploader.upload_event(str(start_jpg), str(meta_json))

        # All local files should be deleted after successful upload
        assert not start_jpg.exists(), "Start JPEG should be deleted after success"
        assert not end_jpg.exists(), "End JPEG should be deleted after success"
        assert not meta_json.exists(), "Metadata JSON should be deleted after success"


# ---------------------------------------------------------------------------
# Property 14: DynamoDB TTL 计算
# ---------------------------------------------------------------------------


class TestDynamoDBTTLCalculation:
    """Property 14: DynamoDB TTL calculation.

    Feature: ai-video-summary, Property 14: DynamoDB TTL 计算

    **Validates: Requirements 4.3**

    expiry_ttl = event_timestamp_unix_sec + 90 * 24 * 3600
    """

    @given(
        timestamp_ms=timestamp_ms_st,
        cls=detected_class_st,
        confidence=confidence_st,
        duration=duration_st,
        ttl_days=st.integers(min_value=1, max_value=365),
    )
    @settings(max_examples=200)
    def test_ttl_equals_timestamp_plus_retention(
        self,
        timestamp_ms: int,
        cls: str,
        confidence: float,
        duration: float,
        ttl_days: int,
    ) -> None:
        config = _make_config(event_ttl_days=ttl_days)
        mock_s3 = MagicMock()
        mock_ddb = MagicMock()
        mock_table = MagicMock()
        mock_ddb.Table.return_value = mock_table

        uploader = S3Uploader(
            config=config,
            device_id="test-device",
            s3_client=mock_s3,
            dynamodb_resource=mock_ddb,
        )

        metadata = {
            "session_id": "sess-001",
            "detected_class": cls,
            "kvs_start_timestamp": timestamp_ms,
            "kvs_end_timestamp": timestamp_ms + 60000,
            "duration_seconds": duration,
            "max_confidence": confidence,
            "detection_count": 5,
        }
        s3_paths = {
            "s3_start_jpeg_path": "captures/dev/2024-01-01/123_person.start.jpg",
            "s3_end_jpeg_path": "captures/dev/2024-01-01/123_person.end.jpg",
            "s3_metadata_path": "captures/dev/2024-01-01/123_person.json",
        }

        uploader.write_dynamodb_record(metadata, s3_paths)

        # Capture the item written to DynamoDB
        mock_table.put_item.assert_called_once()
        item = mock_table.put_item.call_args[1]["Item"]

        event_unix_sec = int(timestamp_ms / 1000)
        expected_ttl = event_unix_sec + ttl_days * 24 * 3600

        assert item["expiry_ttl"] == expected_ttl, (
            f"TTL mismatch: expected {expected_ttl}, got {item['expiry_ttl']} "
            f"(timestamp_ms={timestamp_ms}, ttl_days={ttl_days})"
        )

    @given(timestamp_ms=timestamp_ms_st)
    @settings(max_examples=100)
    def test_default_90_day_ttl(self, timestamp_ms: int) -> None:
        """With default 90-day TTL, verify the calculation."""
        config = _make_config(event_ttl_days=90)
        mock_s3 = MagicMock()
        mock_ddb = MagicMock()
        mock_table = MagicMock()
        mock_ddb.Table.return_value = mock_table

        uploader = S3Uploader(
            config=config,
            device_id="test-device",
            s3_client=mock_s3,
            dynamodb_resource=mock_ddb,
        )

        metadata = {
            "session_id": "sess-default",
            "detected_class": "person",
            "kvs_start_timestamp": timestamp_ms,
            "kvs_end_timestamp": timestamp_ms + 60000,
            "duration_seconds": 60.0,
            "max_confidence": 0.9,
            "detection_count": 1,
        }
        s3_paths = {
            "s3_start_jpeg_path": "p1",
            "s3_end_jpeg_path": "p2",
            "s3_metadata_path": "p3",
        }

        uploader.write_dynamodb_record(metadata, s3_paths)

        item = mock_table.put_item.call_args[1]["Item"]
        event_unix_sec = int(timestamp_ms / 1000)
        expected_ttl = event_unix_sec + 90 * 24 * 3600

        assert item["expiry_ttl"] == expected_ttl


# ---------------------------------------------------------------------------
# Property 20: 存储超限时优先上传最旧文件
# ---------------------------------------------------------------------------


class TestOldestFirstUploadOrdering:
    """Property 20: Oldest-first upload ordering when storage exceeds limit.

    Feature: ai-video-summary, Property 20: 存储超限时优先上传最旧文件

    **Validates: Requirements 7.3**

    When capture dir total size > 200MB, S3 Uploader processes files
    oldest-first by creation time.
    """

    @given(
        num_sessions=st.integers(min_value=2, max_value=6),
        data=st.data(),
    )
    @settings(max_examples=100)
    def test_scan_returns_oldest_first(
        self, num_sessions: int, data: st.DataObject
    ) -> None:
        """scan_file_pairs() returns pairs sorted by creation time, oldest first."""
        tmp_dir = tempfile.mkdtemp()
        try:
            self._run_oldest_first(num_sessions, data, Path(tmp_dir))
        finally:
            shutil.rmtree(tmp_dir, ignore_errors=True)

    def _run_oldest_first(
        self, num_sessions: int, data: st.DataObject, tmp_path: Path
    ) -> None:
        capture_dir = tmp_path / "captures"
        capture_dir.mkdir()

        config = _make_config(capture_dir=str(capture_dir))
        uploader = _make_uploader(config=config)
        uploader.capture_dir = Path(capture_dir)

        # Generate unique session IDs with distinct creation times
        session_ids = [f"session-{i:03d}" for i in range(num_sessions)]
        # Shuffle to ensure ordering isn't just alphabetical
        shuffled = data.draw(st.permutations(session_ids))

        created_files: list[tuple[str, float]] = []
        base_time = time.time() - num_sessions * 2

        for idx, sid in enumerate(shuffled):
            start_jpg = capture_dir / f"{sid}_start.jpg"
            end_jpg = capture_dir / f"{sid}_end.jpg"
            meta_json = capture_dir / f"{sid}_metadata.json"

            start_jpg.write_bytes(b"jpeg-data")
            end_jpg.write_bytes(b"jpeg-data")
            meta_json.write_text(json.dumps({"session_id": sid}))

            # Set distinct modification/creation times
            ctime = base_time + idx
            os.utime(str(meta_json), (ctime, ctime))
            os.utime(str(start_jpg), (ctime, ctime))
            os.utime(str(end_jpg), (ctime, ctime))

            created_files.append((sid, ctime))

        # Sort by creation time to get expected order
        created_files.sort(key=lambda t: t[1])
        expected_order = [sid for sid, _ in created_files]

        pairs = uploader.scan_file_pairs()

        # Extract session IDs from returned pairs
        actual_order: list[str] = []
        for jpeg_path, json_path in pairs:
            # json_path name is like "session-001_metadata.json"
            sid = json_path.name.replace("_metadata.json", "")
            actual_order.append(sid)

        assert actual_order == expected_order, (
            f"Expected oldest-first order {expected_order}, got {actual_order}"
        )

    @given(num_sessions=st.integers(min_value=2, max_value=4))
    @settings(max_examples=100)
    def test_scan_returns_complete_pairs_only(
        self, num_sessions: int
    ) -> None:
        """scan_file_pairs() only returns sessions with all 3 files present."""
        tmp_dir = tempfile.mkdtemp()
        try:
            self._run_complete_pairs(num_sessions, Path(tmp_dir))
        finally:
            shutil.rmtree(tmp_dir, ignore_errors=True)

    def _run_complete_pairs(self, num_sessions: int, tmp_path: Path) -> None:
        capture_dir = tmp_path / "captures"
        capture_dir.mkdir()

        config = _make_config(capture_dir=str(capture_dir))
        uploader = _make_uploader(config=config)
        uploader.capture_dir = Path(capture_dir)

        complete_sessions: list[str] = []
        base_time = time.time() - num_sessions * 2

        for i in range(num_sessions):
            sid = f"session-{i:03d}"
            start_jpg = capture_dir / f"{sid}_start.jpg"
            end_jpg = capture_dir / f"{sid}_end.jpg"
            meta_json = capture_dir / f"{sid}_metadata.json"

            meta_json.write_text(json.dumps({"session_id": sid}))
            start_jpg.write_bytes(b"jpeg-data")

            ctime = base_time + i
            os.utime(str(meta_json), (ctime, ctime))
            os.utime(str(start_jpg), (ctime, ctime))

            if i % 2 == 0:
                # Complete pair: also create end jpg
                end_jpg.write_bytes(b"jpeg-data")
                os.utime(str(end_jpg), (ctime, ctime))
                complete_sessions.append(sid)

        pairs = uploader.scan_file_pairs()
        returned_sids = [p[1].name.replace("_metadata.json", "") for p in pairs]

        assert set(returned_sids) == set(complete_sessions), (
            f"Expected only complete sessions {complete_sessions}, got {returned_sids}"
        )
