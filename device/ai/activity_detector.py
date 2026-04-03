"""Activity Detector main process.

Connects to the C++ FrameExporter via IPC, runs YOLOv8n inference on
received frames, and manages activity sessions per detected class.

Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 2.8, 2.9, 2.10
"""

from __future__ import annotations

import json
import logging
import os
import shutil
import sys
import time

import cv2
import numpy as np

from device.ai.config import DetectorConfig
from device.ai.ipc_client import IPCClient
from device.ai.session import SessionManager

logger = logging.getLogger(__name__)

# COCO class IDs for target categories
COCO_CLASS_MAP: dict[int, str] = {
    0: "person",
    14: "bird",
    15: "cat",
    16: "dog",
}


class ActivityDetector:
    """Main activity detection loop.

    Connects to the C++ FrameExporter via Unix Socket + shared memory,
    runs YOLOv8n inference on each frame, and delegates session
    management to :class:`SessionManager`.
    """

    def __init__(self, config: DetectorConfig) -> None:
        self.config = config
        self.ipc = IPCClient()

        # Load YOLO model — failure here is fatal (req 2.9)
        try:
            from ultralytics import YOLO

            self.model = YOLO("yolo11n.pt")
        except Exception as exc:
            logger.error("Failed to load YOLOv8n model: %s", exc)
            sys.exit(1)

        capture_dir = self.config.capture_dir
        os.makedirs(capture_dir, exist_ok=True)

        self.session_mgr = SessionManager(
            config=config,
            capture_dir=capture_dir,
            save_screenshot=self._save_screenshot,
            save_metadata=self._save_metadata,
        )

        # Startup log (req 2.10)
        model_info = getattr(self.model, "model_name", "yolov8n")
        logger.info(
            "ActivityDetector started — model=%s, classes=%s, "
            "confidence=%.2f, session_timeout=%ds",
            model_info,
            self.config.detect_classes,
            self.config.confidence_threshold,
            self.config.session_timeout_sec,
        )

    # ------------------------------------------------------------------
    # IPC connection
    # ------------------------------------------------------------------

    def connect_ipc(self) -> None:
        """Connect to the C++ FrameExporter Unix Socket and map shared memory."""
        self.ipc.connect(self.config.socket_path)
        shm_size = self.config.shm_size_mb * 1024 * 1024
        self.ipc.map_shared_memory(self.config.shm_name, shm_size)
        logger.info("IPC connected: socket=%s, shm=%s", self.config.socket_path, self.config.shm_name)

    # ------------------------------------------------------------------
    # Main loop
    # ------------------------------------------------------------------

    def run(self) -> None:
        """Main loop: receive notification → read frame → infer → update sessions."""
        consecutive_failures = 0
        while True:
            try:
                notif = self.ipc.receive_notification()
                raw = self.ipc.read_frame(notif)

                # Convert frame to BGR for YOLO
                # AI appsink outputs BGR directly via GStreamer videoconvert,
                # so just reshape — no color conversion needed.
                if notif.pixel_format == "BGR":
                    frame = raw.reshape((notif.height, notif.width, 3))
                elif notif.pixel_format == "NV12":
                    # Legacy fallback: NV12/NV21 from older pipeline versions
                    yuv = raw.reshape((notif.height * 3 // 2, notif.width))
                    frame = cv2.cvtColor(yuv, cv2.COLOR_YUV2BGR_NV21)
                else:
                    # Fallback: assume raw BGR
                    frame = raw.reshape((notif.height, notif.width, 3))

                self.process_frame(frame, notif.timestamp_ms)
                consecutive_failures = 0  # reset on success
            except ConnectionError as exc:
                consecutive_failures += 1
                # Exponential backoff: 0s, 2s, 4s, 8s, 16s, 30s, 30s, ...
                backoff = min(2 ** consecutive_failures, 30) if consecutive_failures > 1 else 0

                # After 5 consecutive failures (~1 min of stall), restart smart-camera
                if consecutive_failures == 5:
                    logger.error(
                        "FrameExporter stalled for %d attempts, restarting smart-camera service",
                        consecutive_failures,
                    )
                    import subprocess
                    try:
                        subprocess.run(
                            ["sudo", "systemctl", "restart", "smart-camera"],
                            timeout=30, check=False,
                        )
                        logger.info("smart-camera service restart requested")
                        time.sleep(10)  # wait for pipeline to initialize
                    except Exception as restart_exc:
                        logger.error("Failed to restart smart-camera: %s", restart_exc)

                logger.warning(
                    "IPC connection lost (%s), reconnecting in %ds… (attempt %d)",
                    exc, backoff, consecutive_failures,
                )
                self.ipc.close()
                if backoff > 0:
                    time.sleep(backoff)
                self.connect_ipc()
            except KeyboardInterrupt:
                logger.info("Shutting down ActivityDetector")
                break
            except Exception as exc:
                # Catch-all: log the error and continue processing next frame
                # This prevents crashes from unexpected errors in frame processing
                logger.error("Unexpected error in main loop: %s", exc, exc_info=True)

    # ------------------------------------------------------------------
    # Frame processing
    # ------------------------------------------------------------------

    def process_frame(self, frame: np.ndarray, timestamp_ms: int) -> None:
        """Run YOLO inference on a single frame and update sessions."""
        import time as _time
        t0 = _time.monotonic()

        try:
            results = self.model.predict(frame, verbose=False)
        except Exception as exc:
            logger.warning("YOLO inference failed, skipping frame: %s", exc)
            return

        inference_ms = (_time.monotonic() - t0) * 1000

        # Collect detections for debug logging
        detections = []

        # Check disk protection before processing detections
        disk_ok = self._disk_space_ok()
        files_ok = self._file_count_ok()

        for result in results:
            boxes = result.boxes
            if boxes is None:
                continue
            for i in range(len(boxes)):
                cls_id = int(boxes.cls[i].item())
                conf = float(boxes.conf[i].item())

                cls_name = COCO_CLASS_MAP.get(cls_id)
                if cls_name is None:
                    continue

                detections.append(f"{cls_name}:{conf:.2f}")

                # Use per-class threshold if configured, otherwise default
                threshold = self.config.confidence_overrides.get(
                    cls_name, self.config.confidence_threshold
                )
                if conf < threshold:
                    continue

                # Disk protection: skip new session creation when limits exceeded
                if not files_ok or not disk_ok:
                    # Still update existing sessions, but don't create new ones
                    active = self.session_mgr.get_active_sessions()
                    if cls_name in active:
                        self.session_mgr.on_detection(cls_name, conf, timestamp_ms, frame)
                    else:
                        if not files_ok:
                            logger.warning("File count exceeds %d — not creating new session", self.config.capture_max_files)
                        if not disk_ok:
                            logger.warning("Disk free space below %dMB — not saving screenshots", self.config.disk_min_free_mb)
                    continue

                self.session_mgr.on_detection(cls_name, conf, timestamp_ms, frame)

        # Per-frame log: model + inference time + detections
        model_name = getattr(self.model, "model_name", "unknown")
        if detections:
            logger.info("[%s] Frame ts=%d infer=%.0fms: %s", model_name, timestamp_ms, inference_ms, ", ".join(detections))
        else:
            logger.debug("[%s] Frame ts=%d infer=%.0fms: no targets", model_name, timestamp_ms, inference_ms)

        # Check for timed-out sessions
        self.check_session_timeouts(timestamp_ms)

    def check_session_timeouts(self, current_time_ms: int) -> None:
        """End sessions that have exceeded the inactivity timeout."""
        ended = self.session_mgr.check_timeouts(current_time_ms)
        for session in ended:
            logger.info(
                "Session ended: class=%s, duration=%.1fs, detections=%d",
                session.detected_class,
                (session.end_time_ms - session.start_time_ms) / 1000.0,
                session.detection_count,
            )

    # ------------------------------------------------------------------
    # Screenshot / metadata I/O
    # ------------------------------------------------------------------

    def _save_screenshot(self, frame: np.ndarray, filename: str) -> None:
        """Save a JPEG screenshot to the Capture Directory."""
        if not self._disk_space_ok():
            logger.warning("Disk free space below %dMB — skipping screenshot save", self.config.disk_min_free_mb)
            return
        path = os.path.join(self.config.capture_dir, filename)
        cv2.imwrite(path, frame)
        logger.debug("Screenshot saved: %s", path)

    def _save_metadata(self, metadata: dict) -> None:
        """Save JSON event metadata to the Capture Directory."""
        session_id = metadata.get("session_id", "unknown")
        filename = f"{session_id}_metadata.json"
        path = os.path.join(self.config.capture_dir, filename)
        with open(path, "w") as f:
            json.dump(metadata, f, indent=2)
        logger.debug("Metadata saved: %s", path)

    # ------------------------------------------------------------------
    # Disk protection helpers
    # ------------------------------------------------------------------

    def _file_count_ok(self) -> bool:
        """Return True if the capture directory file count is within limits."""
        try:
            count = len(os.listdir(self.config.capture_dir))
            return count <= self.config.capture_max_files
        except OSError:
            return True

    def _disk_space_ok(self) -> bool:
        """Return True if free disk space is above the minimum threshold."""
        try:
            usage = shutil.disk_usage(self.config.capture_dir)
            free_mb = usage.free / (1024 * 1024)
            return free_mb >= self.config.disk_min_free_mb
        except OSError:
            return True


# ------------------------------------------------------------------
# Entry point
# ------------------------------------------------------------------

def main() -> None:
    """Read config from INI and start the Activity Detector."""
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    )

    config_path = os.environ.get(
        "AI_SUMMARY_CONFIG_PATH",
        "device/config/default.ini",
    )
    config = DetectorConfig.from_ini(config_path)

    detector = ActivityDetector(config)
    detector.connect_ipc()
    detector.run()


if __name__ == "__main__":
    main()
