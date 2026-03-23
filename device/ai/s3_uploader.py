"""S3 Uploader — monitors Capture Directory and uploads events to S3 / DynamoDB.

Runs as an independent Python process. Polls the capture directory for
matched JPEG + JSON file pairs, uploads them atomically to S3, writes
an event record to DynamoDB, and deletes the local files on success.

Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 4.1, 4.2, 4.3, 7.2, 7.3
"""

from __future__ import annotations

import json
import logging
import os
import shutil
import time
from datetime import datetime, timezone
from decimal import Decimal
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Tuple

import boto3

from device.ai.config import DetectorConfig

logger = logging.getLogger(__name__)

_RETRY_EXHAUSTED = object()


def _to_decimal(value: float) -> Decimal:
    return Decimal(str(value))


class S3Uploader:
    """Monitors Capture Directory, uploads to S3, writes DynamoDB."""

    def __init__(
        self,
        config: DetectorConfig,
        device_id: str = "",
        *,
        s3_client: Any = None,
        dynamodb_resource: Any = None,
        poll_interval: float = 5.0,
    ) -> None:
        self.config = config
        self.device_id = device_id or os.environ.get("DEVICE_ID", "unknown")
        self.poll_interval = poll_interval
        self.s3 = s3_client or boto3.client("s3")
        ddb = dynamodb_resource or boto3.resource("dynamodb")
        self.table = ddb.Table(self.config.dynamodb_table)
        self.capture_dir = Path(self.config.capture_dir)
        self.errors_dir = self.capture_dir / "errors"

    def run(self) -> None:
        """Main loop: scan -> upload -> write DDB -> delete local files."""
        logger.info(
            "S3Uploader started bucket=%s prefix=%s device=%s",
            self.config.s3_bucket, self.config.s3_prefix, self.device_id,
        )
        while True:
            try:
                pairs = self.scan_file_pairs()
                for jpeg_path, json_path in pairs:
                    self.upload_event(str(jpeg_path), str(json_path))
                time.sleep(self.poll_interval)
            except KeyboardInterrupt:
                logger.info("Shutting down S3Uploader")
                break

    def scan_file_pairs(self) -> List[Tuple[Path, Path]]:
        """Scan capture directory for matched JPEG + JSON file pairs.

        Returns pairs sorted by file creation time oldest-first (req 7.3).
        """
        if not self.capture_dir.exists():
            return []

        json_files: Dict[str, Path] = {}
        for f in self.capture_dir.iterdir():
            if f.suffix == ".json" and f.name.endswith("_metadata.json"):
                session_id = f.name.replace("_metadata.json", "")
                json_files[session_id] = f

        triplets: List[Tuple[str, Path, float]] = []
        for session_id, json_path in json_files.items():
            start_jpg = self.capture_dir / f"{session_id}_start.jpg"
            end_jpg = self.capture_dir / f"{session_id}_end.jpg"
            if start_jpg.exists() and end_jpg.exists():
                try:
                    ctime = json_path.stat().st_ctime
                except OSError:
                    ctime = 0.0
                triplets.append((session_id, json_path, ctime))

        triplets.sort(key=lambda t: t[2])

        result: List[Tuple[Path, Path]] = []
        for session_id, json_path, _ in triplets:
            start_jpg = self.capture_dir / f"{session_id}_start.jpg"
            result.append((start_jpg, json_path))
        return result

    def upload_event(self, jpeg_path: str, json_path: str) -> None:
        """Atomically upload event files to S3, write DDB, then delete locals.

        All files must upload successfully before any local file is deleted (req 3.7).
        """
        json_p = Path(json_path)
        session_id = json_p.name.replace("_metadata.json", "")
        start_jpg_p = Path(jpeg_path)
        end_jpg_p = start_jpg_p.parent / f"{session_id}_end.jpg"

        try:
            with open(json_path, "r") as f:
                metadata = json.load(f)
        except (OSError, json.JSONDecodeError) as exc:
            logger.error("Failed to read metadata %s: %s", json_path, exc)
            self._move_to_errors(start_jpg_p, end_jpg_p, json_p)
            return

        detected_class = metadata.get("detected_class", "unknown")
        kvs_start_ts = metadata.get("kvs_start_timestamp", 0)

        s3_start_key = self.build_s3_key(
            self.device_id, kvs_start_ts, detected_class, "start.jpg"
        )
        s3_end_key = self.build_s3_key(
            self.device_id, kvs_start_ts, detected_class, "end.jpg"
        )
        s3_json_key = self.build_s3_key(
            self.device_id, kvs_start_ts, detected_class, "json"
        )

        files_to_upload = [
            (str(start_jpg_p), s3_start_key),
            (str(end_jpg_p), s3_end_key),
            (str(json_p), s3_json_key),
        ]

        for local_path, s3_key in files_to_upload:
            result = self.retry_with_backoff(
                lambda lp=local_path, sk=s3_key: self._upload_file(lp, sk)
            )
            if result is _RETRY_EXHAUSTED:
                logger.error(
                    "Upload failed after retries: %s -> %s", local_path, s3_key
                )
                self._move_to_errors(start_jpg_p, end_jpg_p, json_p)
                return

        s3_paths = {
            "s3_start_jpeg_path": s3_start_key,
            "s3_end_jpeg_path": s3_end_key,
            "s3_metadata_path": s3_json_key,
        }
        ddb_result = self.retry_with_backoff(
            lambda: self.write_dynamodb_record(metadata, s3_paths)
        )
        if ddb_result is _RETRY_EXHAUSTED:
            logger.error(
                "DynamoDB write failed after retries for session %s", session_id
            )
            self._move_to_errors(start_jpg_p, end_jpg_p, json_p)
            return

        for p in (start_jpg_p, end_jpg_p, json_p):
            try:
                p.unlink(missing_ok=True)
            except OSError as exc:
                logger.warning("Failed to delete local file %s: %s", p, exc)

        logger.info(
            "Event uploaded: session=%s class=%s", session_id, detected_class
        )

    @staticmethod
    def build_s3_key(
        device_id: str, timestamp_ms: int, cls: str, ext: str
    ) -> str:
        """Generate S3 key: captures/{device_id}/{YYYY-MM-DD}/{timestamp}_{class}.{ext}"""
        dt = datetime.fromtimestamp(timestamp_ms / 1000.0, tz=timezone.utc)
        date_str = dt.strftime("%Y-%m-%d")
        return f"captures/{device_id}/{date_str}/{timestamp_ms}_{cls}.{ext}"

    def write_dynamodb_record(self, metadata: dict, s3_paths: dict) -> None:
        """Write activity event record to DynamoDB.

        PK=device_id, SK=event_timestamp (ISO 8601).
        expiry_ttl = event_timestamp_unix + event_ttl_days * 86400.
        """
        kvs_start_ts = metadata.get("kvs_start_timestamp", 0)
        event_dt = datetime.fromtimestamp(
            kvs_start_ts / 1000.0, tz=timezone.utc
        )
        event_iso = event_dt.isoformat()
        event_unix_sec = int(kvs_start_ts / 1000)
        ttl_seconds = self.config.event_ttl_days * 24 * 3600
        expiry_ttl = event_unix_sec + ttl_seconds

        item = {
            "device_id": self.device_id,
            "event_timestamp": event_iso,
            "session_id": metadata.get("session_id", ""),
            "detected_class": metadata.get("detected_class", ""),
            "max_confidence": _to_decimal(
                metadata.get("max_confidence", 0.0)
            ),
            "duration_seconds": _to_decimal(
                metadata.get("duration_seconds", 0.0)
            ),
            "kvs_start_timestamp": kvs_start_ts,
            "kvs_end_timestamp": metadata.get("kvs_end_timestamp", 0),
            "detection_count": metadata.get("detection_count", 0),
            "expiry_ttl": expiry_ttl,
        }
        item.update(s3_paths)
        self.table.put_item(Item=item)

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
    uploader = S3Uploader(config, device_id=device_id)
    uploader.run()


if __name__ == "__main__":
    main()
