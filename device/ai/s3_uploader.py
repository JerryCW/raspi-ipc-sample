"""S3 Uploader — monitors Capture Directory and uploads events to S3.

Runs as an independent Python process. Polls the capture directory for
matched screenshot + JSON file sets, uploads them atomically to S3,
and deletes the local files on success.

DynamoDB writes are handled by the Cloud_Verifier Lambda (triggered by
S3 Event Notification on metadata.json upload).

Requirements: 3.1, 3.7, 3.8, 3.9
"""

from __future__ import annotations

import glob
import json
import logging
import os
import shutil
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

import boto3

from device.ai.config import DetectorConfig

logger = logging.getLogger(__name__)

_RETRY_EXHAUSTED = object()


class S3Uploader:
    """Monitors Capture Directory, uploads to S3.

    DynamoDB writes are handled by Cloud_Verifier Lambda, not by this uploader.
    """

    def __init__(
        self,
        config: DetectorConfig,
        device_id: str = "",
        *,
        s3_client: Any = None,
        poll_interval: float = 5.0,
    ) -> None:
        self.config = config
        self.device_id = device_id or os.environ.get("DEVICE_ID", "unknown")
        self.poll_interval = poll_interval
        self.s3 = s3_client or boto3.client("s3")
        self.capture_dir = Path(self.config.capture_dir)
        self.errors_dir = self.capture_dir / "errors"

    def run(self) -> None:
        """Main loop: scan -> upload -> delete local files."""
        logger.info(
            "S3Uploader started bucket=%s prefix=%s device=%s",
            self.config.s3_bucket, self.config.s3_prefix, self.device_id,
        )
        while True:
            try:
                event_sets = self.scan_file_pairs()
                for start_jpg, json_path in event_sets:
                    self.upload_event(str(start_jpg), str(json_path))
                time.sleep(self.poll_interval)
            except KeyboardInterrupt:
                logger.info("Shutting down S3Uploader")
                break

    def scan_file_pairs(self) -> List[Tuple[Path, Path]]:
        """Scan capture directory for uploadable event file sets.

        An event is ready when its metadata JSON and start screenshot exist.
        Candidate screenshots ({session_id}_candidate_*.jpg) are discovered
        at upload time from the metadata. The old format (start+end) is also
        supported: if _end.jpg exists it will be included in the upload.

        Returns (start_jpg, json_path) tuples sorted oldest-first (req 7.3).
        """
        if not self.capture_dir.exists():
            return []

        json_files: Dict[str, Path] = {}
        for f in self.capture_dir.iterdir():
            if f.suffix == ".json" and f.name.endswith("_metadata.json"):
                session_id = f.name.replace("_metadata.json", "")
                json_files[session_id] = f

        entries: List[Tuple[str, Path, float]] = []
        for session_id, json_path in json_files.items():
            start_jpg = self.capture_dir / f"{session_id}_start.jpg"
            if not start_jpg.exists():
                continue

            # New format: start.jpg + candidate_*.jpg + metadata.json
            # Old format: start.jpg + end.jpg + metadata.json
            # Accept either: candidates present OR end.jpg present
            candidate_pattern = str(
                self.capture_dir / f"{session_id}_candidate_*.jpg"
            )
            candidates = glob.glob(candidate_pattern)
            end_jpg = self.capture_dir / f"{session_id}_end.jpg"

            if candidates or end_jpg.exists():
                try:
                    ctime = json_path.stat().st_ctime
                except OSError:
                    ctime = 0.0
                entries.append((session_id, json_path, ctime))

        entries.sort(key=lambda t: t[2])

        result: List[Tuple[Path, Path]] = []
        for session_id, json_path, _ in entries:
            start_jpg = self.capture_dir / f"{session_id}_start.jpg"
            result.append((start_jpg, json_path))
        return result

    def upload_event(self, jpeg_path: str, json_path: str) -> None:
        """Atomically upload event files to S3, then delete locals.

        Supports both new format (start + candidate_*.jpg + metadata.json)
        and old format (start + end + metadata.json).

        All files must upload successfully before any local file is deleted (req 3.7).
        On retry exhaustion, all local files are moved to errors/ (req 3.9).

        DynamoDB writes are handled by Cloud_Verifier Lambda, not here.
        """
        json_p = Path(json_path)
        session_id = json_p.name.replace("_metadata.json", "")
        start_jpg_p = Path(jpeg_path)
        capture_dir = start_jpg_p.parent

        try:
            with open(json_path, "r") as f:
                metadata = json.load(f)
        except (OSError, json.JSONDecodeError) as exc:
            logger.error("Failed to read metadata %s: %s", json_path, exc)
            self._move_to_errors(start_jpg_p, json_p)
            return

        detected_class = metadata.get("detected_class", "unknown")
        kvs_start_ts = metadata.get("kvs_start_timestamp", 0)

        # Collect all local files belonging to this event
        local_files: List[Path] = [start_jpg_p]

        # Discover candidate screenshots from disk
        candidate_pattern = str(capture_dir / f"{session_id}_candidate_*.jpg")
        candidate_paths = sorted(glob.glob(candidate_pattern))
        for cp in candidate_paths:
            local_files.append(Path(cp))

        # Support old format: include end.jpg if present
        end_jpg_p = capture_dir / f"{session_id}_end.jpg"
        if end_jpg_p.exists():
            local_files.append(end_jpg_p)

        local_files.append(json_p)

        # Build S3 keys for each file
        files_to_upload: List[Tuple[str, str]] = []

        # start.jpg
        s3_start_key = self.build_s3_key(
            self.device_id, kvs_start_ts, detected_class, "start.jpg"
        )
        files_to_upload.append((str(start_jpg_p), s3_start_key))

        # candidate_N.jpg — extract index from filename
        for cp in candidate_paths:
            cp_path = Path(cp)
            # filename: {session_id}_candidate_N.jpg -> ext: candidate_N.jpg
            suffix = cp_path.name.replace(f"{session_id}_", "")  # candidate_N.jpg
            ext = suffix.replace(".jpg", "")  # candidate_N
            # Normalise to dotted form for S3 key: candidate_0.jpg
            s3_key = self.build_s3_key(
                self.device_id, kvs_start_ts, detected_class, f"{ext}.jpg"
            )
            files_to_upload.append((str(cp_path), s3_key))

        # end.jpg (old format backward compat)
        if end_jpg_p.exists():
            s3_end_key = self.build_s3_key(
                self.device_id, kvs_start_ts, detected_class, "end.jpg"
            )
            files_to_upload.append((str(end_jpg_p), s3_end_key))

        # metadata.json — use _metadata.json suffix to match S3 trigger filter
        dt = datetime.fromtimestamp(kvs_start_ts / 1000.0, tz=timezone.utc)
        date_str = dt.strftime("%Y-%m-%d")
        s3_json_key = f"captures/{self.device_id}/{date_str}/{kvs_start_ts}_{detected_class}_metadata.json"

        # Rewrite candidate_screenshots in metadata to use S3 filenames
        # (local names are {session_id}_candidate_N.jpg, S3 names are
        # {timestamp}_{class}.candidate_N.jpg)
        s3_candidate_filenames = []
        for cp in candidate_paths:
            cp_path = Path(cp)
            suffix = cp_path.name.replace(f"{session_id}_", "")  # candidate_N.jpg
            ext = suffix.replace(".jpg", "")  # candidate_N
            s3_candidate_filenames.append(f"{kvs_start_ts}_{detected_class}.{ext}.jpg")
        if s3_candidate_filenames:
            metadata["candidate_screenshots"] = s3_candidate_filenames
            # Also rewrite start_screenshot_filename
            metadata["start_screenshot_filename"] = f"{kvs_start_ts}_{detected_class}.start.jpg"
            # Write updated metadata back to local file before upload
            with open(json_path, "w") as f:
                json.dump(metadata, f, ensure_ascii=False, indent=2)

        files_to_upload.append((str(json_p), s3_json_key))

        # Upload all files with retry
        for local_path, s3_key in files_to_upload:
            result = self.retry_with_backoff(
                lambda lp=local_path, sk=s3_key: self._upload_file(lp, sk)
            )
            if result is _RETRY_EXHAUSTED:
                logger.error(
                    "Upload failed after retries: %s -> %s", local_path, s3_key
                )
                self._move_to_errors(*local_files)
                return

        # All uploads succeeded — delete local files
        for p in local_files:
            try:
                p.unlink(missing_ok=True)
            except OSError as exc:
                logger.warning("Failed to delete local file %s: %s", p, exc)

        logger.info(
            "Event uploaded: session=%s class=%s files=%d",
            session_id, detected_class, len(files_to_upload),
        )

    @staticmethod
    def build_s3_key(
        device_id: str, timestamp_ms: int, cls: str, ext: str
    ) -> str:
        """Generate S3 key: captures/{device_id}/{YYYY-MM-DD}/{timestamp}_{class}.{ext}"""
        dt = datetime.fromtimestamp(timestamp_ms / 1000.0, tz=timezone.utc)
        date_str = dt.strftime("%Y-%m-%d")
        return f"captures/{device_id}/{date_str}/{timestamp_ms}_{cls}.{ext}"

    def retry_with_backoff(
        self,
        fn: Callable[[], Any],
        max_retries: Optional[int] = None,
        initial_interval: Optional[float] = None,
        max_interval: float = 60.0,
    ) -> Any:
        """Execute fn with exponential backoff. Returns _RETRY_EXHAUSTED on failure.

        Backoff: 1s -> 2s -> 4s -> 8s -> 16s (capped at max_interval).
        """
        retries = (
            max_retries
            if max_retries is not None
            else self.config.upload_retry_count
        )
        interval = (
            initial_interval
            if initial_interval is not None
            else self.config.upload_retry_interval_sec
        )

        for attempt in range(retries):
            try:
                return fn()
            except Exception as exc:
                wait = min(interval * (2 ** attempt), max_interval)
                logger.warning(
                    "Attempt %d/%d failed (%s), retrying in %.1fs",
                    attempt + 1,
                    retries,
                    exc,
                    wait,
                )
                time.sleep(wait)

        return _RETRY_EXHAUSTED

    def _upload_file(self, local_path: str, s3_key: str) -> None:
        self.s3.upload_file(local_path, self.config.s3_bucket, s3_key)

    def _move_to_errors(self, *paths: Path) -> None:
        self.errors_dir.mkdir(parents=True, exist_ok=True)
        for p in paths:
            if p.exists():
                dest = self.errors_dir / p.name
                try:
                    shutil.move(str(p), str(dest))
                    logger.info("Moved to errors: %s", dest)
                except OSError as exc:
                    logger.error(
                        "Failed to move %s to errors: %s", p, exc
                    )


def main() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    )
    config_path = os.environ.get(
        "AI_SUMMARY_CONFIG_PATH", "device/config/default.ini"
    )
    config = DetectorConfig.from_ini(config_path)
    device_id = os.environ.get("DEVICE_ID", "unknown")

    # Try IoT credential provider first, fall back to default boto3 chain
    s3_client = None
    try:
        from device.ai.iot_credential_provider import load_iot_config, create_iot_boto3_session
        iot_config = load_iot_config(config_path)
        if iot_config.credential_endpoint and iot_config.cert_path:
            session = create_iot_boto3_session(iot_config)
            s3_client = session.client("s3")
            logger.info("Using IoT credential provider for AWS access")
    except Exception as exc:
        logger.warning("IoT credential provider failed, falling back to default: %s", exc)

    uploader = S3Uploader(
        config, device_id=device_id,
        s3_client=s3_client,
    )
    uploader.run()


if __name__ == "__main__":
    main()
