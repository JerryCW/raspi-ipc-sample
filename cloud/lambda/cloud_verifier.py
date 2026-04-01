"""Cloud Verifier Lambda — S3 Event Notification → SageMaker verification → DynamoDB write.

Triggered when a metadata.json file is uploaded to the captures/ prefix in S3.
Implements two-stage verification:
  - bird: download candidate screenshots, call SageMaker endpoint with early-stop
  - non-bird (person/cat/dog): mark verified directly, skip SageMaker

Requirements: 5.1-5.7, 6.1-6.3, 8.2-8.4
"""

from __future__ import annotations

import json
import logging
import os
import time
import urllib.parse
from datetime import datetime, timezone
from typing import Any

import boto3
from botocore.exceptions import ClientError

logger = logging.getLogger()
logger.setLevel(logging.INFO)

# Environment variables
SAGEMAKER_ENDPOINT_NAME = os.environ.get("SAGEMAKER_ENDPOINT_NAME", "bird-classifier-endpoint")
DYNAMODB_TABLE = os.environ.get("DYNAMODB_TABLE", "smart-camera-events")
DEVICE_ID = os.environ.get("DEVICE_ID", "smart-camera-001")

# Verification threshold for SageMaker top-1 prediction
BIRD_CONFIDENCE_THRESHOLD = 0.5

# Non-bird classes that skip SageMaker and are directly verified
DIRECT_VERIFY_CLASSES = {"person", "cat", "dog"}


def lambda_handler(event: dict, context: Any) -> dict:
    """Entry point for S3 Event Notification trigger.

    Parses the S3 event, downloads metadata.json, routes to the appropriate
    verification strategy, writes DynamoDB on success, and updates S3 metadata.
    """
    start_time = time.time()

    s3_client = boto3.client("s3")
    sagemaker_client = boto3.client("sagemaker-runtime")
    dynamodb = boto3.resource("dynamodb")
    table = dynamodb.Table(DYNAMODB_TABLE)

    # Parse S3 event notification
    for record in event.get("Records", []):
        s3_info = record.get("s3", {})
        bucket = s3_info.get("bucket", {}).get("name", "")
        raw_key = s3_info.get("object", {}).get("key", "")
        key = urllib.parse.unquote_plus(raw_key)

        if not key.endswith(".json") or not key.startswith("captures/"):
            logger.info("Skipping non-metadata object: %s", key)
            continue

        event_id = _extract_event_id(key)
        logger.info("Processing event: bucket=%s key=%s event_id=%s", bucket, key, event_id)

        try:
            result = _process_event(
                s3_client, sagemaker_client, table, bucket, key, event_id, start_time,
            )
        except Exception:
            logger.exception("Unhandled error processing %s", key)
            result = {"status": "error", "event_id": event_id}

    return {"statusCode": 200, "body": "OK"}


def _extract_event_id(key: str) -> str:
    """Extract a human-readable event ID from the S3 key.

    Key format: captures/{device_id}/{date}/{timestamp}_{class}.json
    Returns: {timestamp}_{class}
    """
    filename = key.rsplit("/", 1)[-1]
    return filename.rsplit(".", 1)[0]  # strip .json


def _process_event(
    s3_client: Any,
    sagemaker_client: Any,
    table: Any,
    bucket: str,
    metadata_key: str,
    event_id: str,
    start_time: float,
) -> dict:
    """Core processing: download metadata → verify → write DynamoDB → update S3."""
    # 1. Download and parse metadata.json
    metadata = _download_metadata(s3_client, bucket, metadata_key)
    session_id = metadata.get("session_id", event_id)
    primary_class = metadata.get("primary_class", metadata.get("detected_class", "unknown"))
    candidates = metadata.get("candidate_screenshots", [])

    logger.info(
        "Event metadata: session_id=%s primary_class=%s candidates=%d",
        session_id, primary_class, len(candidates),
    )

    # Derive the S3 prefix from the metadata key (same directory)
    s3_prefix = metadata_key.rsplit("/", 1)[0] + "/"

    # 2. Always try SageMaker bird classification first, regardless of primary_class.
    # YOLO's low accuracy may misclassify birds as person/dog, so we let the
    # cloud model decide. If SageMaker identifies a bird, we override primary_class.
    if candidates:
        verification = verify_bird(
            s3_client, sagemaker_client, bucket, s3_prefix, candidates, metadata,
        )
        if verification["verification_status"] == "verified" and verification.get("bird_species"):
            # SageMaker found a bird — override primary_class
            logger.info(
                "SageMaker identified bird (%s) even though YOLO said primary_class=%s",
                verification["bird_species"], primary_class,
            )
            metadata["primary_class"] = "bird"
        elif verification["verification_status"] == "rejected":
            # SageMaker says it's not a bird — fall back to YOLO's primary_class
            verification = {
                "verification_status": "verified",
                "pipeline_stage": "cloud_verified",
                "verification_fallback": False,
            }
            logger.info("SageMaker rejected bird, using YOLO class=%s as verified", primary_class)
    else:
        # No candidate screenshots — just verify with YOLO's class
        verification = {
            "verification_status": "verified",
            "pipeline_stage": "cloud_verified",
            "verification_fallback": False,
        }
        logger.info("No candidates, direct verify for class=%s", primary_class)

    elapsed_ms = int((time.time() - start_time) * 1000)
    verification_status = verification["verification_status"]

    # 3. Write DynamoDB only if verified (req 5.5, 5.6)
    if verification_status == "verified":
        _write_dynamodb(table, metadata, verification, metadata_key, s3_prefix)
    else:
        logger.info("Event rejected, skipping DynamoDB write: event_id=%s", event_id)

    # 4. Update S3 metadata.json with verification results (req 8.2)
    _update_s3_metadata(s3_client, bucket, metadata_key, metadata, verification)

    # 5. Log summary (req 8.3)
    logger.info(
        "Verification complete: event_id=%s status=%s elapsed_ms=%d "
        "primary_class=%s bird_species=%s species_confidence=%s fallback=%s",
        event_id,
        verification_status,
        elapsed_ms,
        primary_class,
        verification.get("bird_species", "N/A"),
        verification.get("species_confidence", "N/A"),
        verification.get("verification_fallback", False),
    )

    return {"status": verification_status, "event_id": event_id, "elapsed_ms": elapsed_ms}


def verify_bird(
    s3_client: Any,
    sagemaker_client: Any,
    bucket: str,
    s3_prefix: str,
    candidates: list[str],
    metadata: dict,
) -> dict:
    """Verify bird class via SageMaker endpoint with early-stop strategy.

    Downloads candidate screenshots in confidence-descending order (as stored
    in metadata.candidate_screenshots), calls SageMaker for each. Stops as
    soon as top-1 confidence >= 0.5 (req 5.2, 5.3).

    On SageMaker timeout/failure, degrades to verified with fallback flag (req 5.7).

    Returns dict with verification_status, pipeline_stage, and optional bird fields.
    """
    if not candidates:
        logger.warning("No candidate screenshots for bird verification, rejecting")
        return {
            "verification_status": "rejected",
            "pipeline_stage": "cloud_rejected",
            "verification_fallback": False,
        }

    for i, candidate_filename in enumerate(candidates):
        # Build S3 key for this candidate
        s3_key = s3_prefix + candidate_filename

        try:
            # Download candidate screenshot from S3
            logger.info("Downloading candidate %d/%d: %s", i + 1, len(candidates), s3_key)
            response = s3_client.get_object(Bucket=bucket, Key=s3_key)
            image_bytes = response["Body"].read()
        except ClientError as exc:
            logger.warning("Failed to download candidate %s: %s", s3_key, exc)
            continue

        # Call SageMaker endpoint
        try:
            predictions = _invoke_sagemaker(sagemaker_client, image_bytes)
        except Exception as exc:
            # SageMaker timeout/failure → degrade to verified (req 5.7)
            logger.error(
                "SageMaker call failed for candidate %s: %s — degrading to verified",
                s3_key, exc,
            )
            return {
                "verification_status": "verified",
                "pipeline_stage": "cloud_verified",
                "verification_fallback": True,
                "bird_species": None,
                "species_confidence": None,
            }

        if not predictions:
            logger.warning("Empty predictions for candidate %s", s3_key)
            continue

        top1 = predictions[0]
        top1_species = top1.get("species", "unknown")
        top1_confidence = top1.get("confidence", 0.0)

        logger.info(
            "SageMaker result for candidate %d: species=%s confidence=%.4f",
            i + 1, top1_species, top1_confidence,
        )

        # Early stop: top-1 >= threshold (req 5.3)
        if top1_confidence >= BIRD_CONFIDENCE_THRESHOLD:
            logger.info(
                "Bird verified (early stop at candidate %d/%d): %s (%.4f)",
                i + 1, len(candidates), top1_species, top1_confidence,
            )
            return {
                "verification_status": "verified",
                "pipeline_stage": "cloud_verified",
                "verification_fallback": False,
                "bird_species": top1_species,
                "species_confidence": round(top1_confidence, 4),
            }

    # All candidates below threshold → rejected (req 5.5)
    logger.info("All %d candidates below threshold, rejecting bird event", len(candidates))
    return {
        "verification_status": "rejected",
        "pipeline_stage": "cloud_rejected",
        "verification_fallback": False,
    }


def _download_metadata(s3_client: Any, bucket: str, key: str) -> dict:
    """Download and parse metadata.json from S3."""
    response = s3_client.get_object(Bucket=bucket, Key=key)
    body = response["Body"].read().decode("utf-8")
    return json.loads(body)


def _invoke_sagemaker(sagemaker_client: Any, image_bytes: bytes) -> list[dict]:
    """Call SageMaker endpoint with JPEG image bytes, return predictions list.

    Expected response: {"predictions": [{"species": "...", "confidence": 0.87}, ...]}
    """
    response = sagemaker_client.invoke_endpoint(
        EndpointName=SAGEMAKER_ENDPOINT_NAME,
        ContentType="application/x-image",
        Accept="application/json",
        Body=image_bytes,
    )
    result = json.loads(response["Body"].read().decode("utf-8"))
    return result.get("predictions", [])


def _write_dynamodb(
    table: Any,
    metadata: dict,
    verification: dict,
    metadata_key: str,
    s3_prefix: str,
) -> None:
    """Write verified event to DynamoDB with conditional put for idempotency (req 8.4).

    Uses ConditionExpression to prevent duplicate writes from repeated S3 triggers.
    PK = device_id, SK = event_timestamp (ISO 8601).
    """
    kvs_start_ts = metadata.get("kvs_start_timestamp", 0)
    event_timestamp = datetime.fromtimestamp(
        kvs_start_ts / 1000.0, tz=timezone.utc
    ).isoformat()

    # Build S3 paths for screenshots
    detected_class = metadata.get("detected_class", metadata.get("primary_class", "unknown"))
    s3_start_key = s3_prefix + metadata.get("start_screenshot_filename", "")

    # Candidate screenshot S3 paths
    candidate_s3_paths = [
        s3_prefix + fname
        for fname in metadata.get("candidate_screenshots", [])
    ]

    # TTL: 30 days from now
    expiry_ttl = int(time.time()) + 30 * 24 * 3600

    item = {
        # Keys
        "device_id": DEVICE_ID,
        "event_timestamp": event_timestamp,
        # Existing fields (backward compatible — req 6.3)
        "session_id": metadata.get("session_id", ""),
        "detected_class": detected_class,
        "max_confidence": _to_decimal(metadata.get("max_confidence", 0)),
        "duration_seconds": _to_decimal(metadata.get("duration_seconds", 0)),
        "kvs_start_timestamp": kvs_start_ts,
        "kvs_end_timestamp": metadata.get("kvs_end_timestamp", 0),
        "detection_count": metadata.get("detection_count", 0),
        "s3_start_jpeg_path": s3_start_key,
        "expiry_ttl": expiry_ttl,
        # New fields (req 6.1)
        "verification_status": verification.get("verification_status", "verified"),
        "detected_classes": metadata.get("detected_classes", [detected_class]),
        "primary_class": metadata.get("primary_class", detected_class),
        "candidate_screenshots": candidate_s3_paths,
        "pipeline_stage": verification.get("pipeline_stage", "cloud_verified"),
    }

    # Bird-specific fields (req 6.2)
    if verification.get("bird_species"):
        item["bird_species"] = verification["bird_species"]
    if verification.get("species_confidence") is not None:
        item["species_confidence"] = _to_decimal(verification["species_confidence"])

    # Fallback marker (req 5.7)
    if verification.get("verification_fallback"):
        item["verification_fallback"] = True

    try:
        table.put_item(
            Item=item,
            ConditionExpression="attribute_not_exists(device_id) AND attribute_not_exists(event_timestamp)",
        )
        logger.info(
            "DynamoDB write success: device_id=%s event_timestamp=%s",
            DEVICE_ID, event_timestamp,
        )
    except ClientError as exc:
        if exc.response["Error"]["Code"] == "ConditionalCheckFailedException":
            logger.info(
                "Idempotent skip: event already exists device_id=%s event_timestamp=%s",
                DEVICE_ID, event_timestamp,
            )
        else:
            raise


def _update_s3_metadata(
    s3_client: Any,
    bucket: str,
    key: str,
    metadata: dict,
    verification: dict,
) -> None:
    """Update the metadata.json on S3 with verification results (req 8.2).

    Adds/overwrites pipeline_stage and verification_status fields.
    """
    metadata["pipeline_stage"] = verification.get("pipeline_stage", "cloud_verified")
    metadata["verification_status"] = verification.get("verification_status", "verified")

    if verification.get("bird_species"):
        metadata["bird_species"] = verification["bird_species"]
    if verification.get("species_confidence") is not None:
        metadata["species_confidence"] = verification["species_confidence"]
    if verification.get("verification_fallback"):
        metadata["verification_fallback"] = True

    s3_client.put_object(
        Bucket=bucket,
        Key=key,
        Body=json.dumps(metadata, ensure_ascii=False, indent=2).encode("utf-8"),
        ContentType="application/json",
    )
    logger.info("S3 metadata updated: %s", key)


def _to_decimal(value: Any) -> Any:
    """Convert float to Decimal for DynamoDB compatibility.

    DynamoDB does not accept Python floats directly. We convert via string
    to avoid floating-point representation issues.
    """
    from decimal import Decimal

    if isinstance(value, float):
        return Decimal(str(value))
    if isinstance(value, int):
        return value
    return value
