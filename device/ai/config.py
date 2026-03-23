"""Configuration for AI Video Activity Summary components.

Defines DetectorConfig dataclass with support for INI file parsing
and environment variable overrides (AI_SUMMARY_{FIELD_NAME_UPPER}).
"""

from __future__ import annotations

import configparser
import os
from dataclasses import dataclass, field, fields
from typing import List


@dataclass
class DetectorConfig:
    """Configuration for all AI summary components.

    Covers FrameExporter, Activity Detector, S3 Uploader, and DynamoDB params.
    All fields have defaults matching device/config/default.ini [ai_summary].
    """

    # FrameExporter params
    export_fps: float = 2.0
    shm_name: str = "/smart_camera_frames"
    shm_size_mb: int = 20
    socket_path: str = "/tmp/smart_camera_ai.sock"

    # Activity Detector params
    detect_classes: List[str] = field(default_factory=lambda: ["person", "cat", "dog", "bird"])
    confidence_threshold: float = 0.5
    session_timeout_sec: int = 60
    capture_dir: str = "/var/lib/smart-camera/captures/"
    capture_max_files: int = 500
    capture_max_size_mb: int = 200
    disk_min_free_mb: int = 100

    # S3 Uploader params
    s3_bucket: str = "smart-camera-captures"
    s3_prefix: str = "captures"
    upload_retry_count: int = 5
    upload_retry_interval_sec: float = 1.0

    # DynamoDB params
    dynamodb_table: str = "smart-camera-events"
    event_ttl_days: int = 90

    @classmethod
    def from_ini(cls, path: str, section: str = "ai_summary") -> "DetectorConfig":
        """Create a DetectorConfig from an INI file with env-var overrides.

        1. Read the [ai_summary] section from the INI file.
        2. For each field, check for an environment variable
           ``AI_SUMMARY_{FIELD_NAME_UPPER}`` and use it if set.

        Args:
            path: Path to the INI configuration file.
            section: INI section name to read (default ``ai_summary``).

        Returns:
            A fully populated DetectorConfig instance.
        """
        parser = configparser.ConfigParser()
        parser.read(path)

        ini_values: dict[str, str] = {}
        if parser.has_section(section):
            ini_values = dict(parser.items(section))

        kwargs: dict = {}
        for f in fields(cls):
            env_key = f"AI_SUMMARY_{f.name.upper()}"
            # Environment variable takes precedence over INI value
            if env_key in os.environ:
                raw = os.environ[env_key]
            elif f.name in ini_values:
                raw = ini_values[f.name]
            else:
                continue
            kwargs[f.name] = _coerce(raw, f.type, f.name)

        return cls(**kwargs)


def _coerce(raw: str, type_hint: str | type, field_name: str) -> object:
    """Convert a raw string value to the expected field type."""
    # Normalise type_hint to a string for comparison
    hint = type_hint if isinstance(type_hint, str) else getattr(type_hint, "__name__", str(type_hint))

    if hint in ("float",):
        return float(raw)
    if hint in ("int",):
        return int(raw)
    if hint in ("str",):
        return raw
    if hint in ("List[str]",):
        if isinstance(raw, list):
            return raw
        return [s.strip() for s in raw.split(",") if s.strip()]
    # Fallback: return as-is
    return raw
