"""Unit tests for Cloud_Verifier Lambda function.

Tests cover:
- Bird class verification with early-stop strategy
- Bird class verification failure (rejected)
- Non-bird classes directly verified
- SageMaker timeout/failure degradation
- DynamoDB idempotent conditional writes

Requirements: 5.2, 5.3, 5.4, 5.5, 5.6, 5.7, 8.4
"""

from __future__ import annotations

import io
import json
from decimal import Decimal
from unittest.mock import MagicMock, patch

import importlib
import sys

import pytest

# "lambda" is a Python keyword, so we cannot use a normal import statement.
# Use importlib to load the module from the cloud/lambda/ directory.
_mod = importlib.import_module("cloud.lambda.cloud_verifier")

lambda_handler = _mod.lambda_handler
verify_bird = _mod.verify_bird
_download_metadata = _mod._download_metadata
_extract_event_id = _mod._extract_event_id
_invoke_sagemaker = _mod._invoke_sagemaker
_process_event = _mod._process_event
_write_dynamodb = _mod._write_dynamodb
BIRD_CONFIDENCE_THRESHOLD = _mod.BIRD_CONFIDENCE_THRESHOLD
DIRECT_VERIFY_CLASSES = _mod.DIRECT_VERIFY_CLASSES


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _make_s3_event(bucket: str, key: str) -> dict:
    """Build a minimal S3 event notification payload."""
    return {
        "Records": [
            {
                "s3": {
                    "bucket": {"name": bucket},
                    "object": {"key": key},
                }
            }
        ]
    }


def _make_metadata(
    primary_class: str = "bird",
    candidates: list[str] | None = None,
    session_id: str = "sess123",
    kvs_start_timestamp: int = 1700000000000,
) -> dict:
    """Build a metadata dict matching the expected S3 JSON format."""
    return {
        "session_id": session_id,
        "primary_class": primary_class,
        "detected_class": primary_class,
        "detected_classes": [primary_class],
        "class_max_confidences": {primary_class: 0.42},
        "class_detection_counts": {primary_class: 5},
        "max_confidence": 0.42,
        "kvs_start_timestamp": kvs_start_timestamp,
        "kvs_end_timestamp": kvs_start_timestamp + 60000,
        "duration_seconds": 60.0,
        "detection_count": 5,
        "start_screenshot_filename": f"{session_id}_start.jpg",
        "candidate_screenshots": candidates or [
            f"{session_id}_candidate_0.jpg",
            f"{session_id}_candidate_1.jpg",
        ],
        "verification_status": "pending",
        "pipeline_stage": "edge_detected",
    }


def _mock_s3_get_object(body_bytes: bytes) -> dict:
    """Build a mock S3 GetObject response."""
    body = MagicMock()
    body.read.return_value = body_bytes
    return {"Body": body}


def _mock_sagemaker_response(predictions: list[dict]) -> dict:
    """Build a mock SageMaker InvokeEndpoint response."""
    payload = json.dumps({"predictions": predictions}).encode("utf-8")
    body = MagicMock()
    body.read.return_value = payload
    return {"Body": body}


# ---------------------------------------------------------------------------
# Test: _extract_event_id
# ---------------------------------------------------------------------------


class TestExtractEventId:
    def test_standard_key(self) -> None:
        key = "captures/smart-camera-001/2024-01-15/1700000000_bird.json"
        assert _extract_event_id(key) == "1700000000_bird"

    def test_nested_path(self) -> None:
        key = "captures/dev/sub/1234_person.json"
        assert _extract_event_id(key) == "1234_person"



# ---------------------------------------------------------------------------
# Test: Bird verification passes with early-stop
# Validates: Requirements 5.2, 5.3
# ---------------------------------------------------------------------------


class TestBirdVerificationEarlyStop:
    """Bird class: SageMaker returns high confidence on first candidate → verified, stops early."""

    def test_first_candidate_passes_threshold(self) -> None:
        """Top-1 confidence >= 0.5 on first candidate → verified immediately."""
        s3_client = MagicMock()
        sagemaker_client = MagicMock()

        # S3 returns image bytes for the first candidate
        s3_client.get_object.return_value = _mock_s3_get_object(b"fake-jpeg-bytes")

        # SageMaker returns high confidence
        sagemaker_client.invoke_endpoint.return_value = _mock_sagemaker_response([
            {"species": "House Sparrow", "confidence": 0.87},
            {"species": "Eurasian Tree Sparrow", "confidence": 0.08},
        ])

        candidates = ["sess_candidate_0.jpg", "sess_candidate_1.jpg", "sess_candidate_2.jpg"]
        metadata = _make_metadata(candidates=candidates)

        result = verify_bird(
            s3_client, sagemaker_client,
            bucket="test-bucket",
            s3_prefix="captures/dev/2024-01-15/",
            candidates=candidates,
            metadata=metadata,
        )

        assert result["verification_status"] == "verified"
        assert result["pipeline_stage"] == "cloud_verified"
        assert result["bird_species"] == "House Sparrow"
        assert result["species_confidence"] == 0.87
        assert result["verification_fallback"] is False

        # Early stop: only 1 SageMaker call (not 3)
        assert sagemaker_client.invoke_endpoint.call_count == 1
        # Only 1 S3 download (not 3)
        assert s3_client.get_object.call_count == 1

    def test_second_candidate_passes_threshold(self) -> None:
        """First candidate below threshold, second passes → verified after 2 calls."""
        s3_client = MagicMock()
        sagemaker_client = MagicMock()

        s3_client.get_object.return_value = _mock_s3_get_object(b"fake-jpeg")

        # First call: low confidence; second call: high confidence
        sagemaker_client.invoke_endpoint.side_effect = [
            _mock_sagemaker_response([{"species": "Unknown Bird", "confidence": 0.2}]),
            _mock_sagemaker_response([{"species": "Blue Jay", "confidence": 0.75}]),
        ]

        candidates = ["c0.jpg", "c1.jpg", "c2.jpg"]
        result = verify_bird(
            s3_client, sagemaker_client,
            bucket="b", s3_prefix="p/", candidates=candidates,
            metadata=_make_metadata(candidates=candidates),
        )

        assert result["verification_status"] == "verified"
        assert result["bird_species"] == "Blue Jay"
        assert result["species_confidence"] == 0.75
        # Stopped after 2nd candidate
        assert sagemaker_client.invoke_endpoint.call_count == 2

    def test_exact_threshold_passes(self) -> None:
        """Confidence exactly at 0.5 threshold → verified."""
        s3_client = MagicMock()
        sagemaker_client = MagicMock()
        s3_client.get_object.return_value = _mock_s3_get_object(b"img")
        sagemaker_client.invoke_endpoint.return_value = _mock_sagemaker_response([
            {"species": "Robin", "confidence": 0.5},
        ])

        candidates = ["c0.jpg"]
        result = verify_bird(
            s3_client, sagemaker_client,
            bucket="b", s3_prefix="p/", candidates=candidates,
            metadata=_make_metadata(candidates=candidates),
        )

        assert result["verification_status"] == "verified"
        assert result["bird_species"] == "Robin"
        assert result["species_confidence"] == 0.5


# ---------------------------------------------------------------------------
# Test: Bird verification fails (rejected)
# Validates: Requirements 5.5
# ---------------------------------------------------------------------------


class TestBirdVerificationRejected:
    """Bird class: all candidates below threshold → rejected, no DynamoDB write."""

    def test_all_candidates_below_threshold(self) -> None:
        s3_client = MagicMock()
        sagemaker_client = MagicMock()
        s3_client.get_object.return_value = _mock_s3_get_object(b"img")

        # All candidates return low confidence
        sagemaker_client.invoke_endpoint.side_effect = [
            _mock_sagemaker_response([{"species": "Unknown", "confidence": 0.3}]),
            _mock_sagemaker_response([{"species": "Unknown", "confidence": 0.1}]),
        ]

        candidates = ["c0.jpg", "c1.jpg"]
        result = verify_bird(
            s3_client, sagemaker_client,
            bucket="b", s3_prefix="p/", candidates=candidates,
            metadata=_make_metadata(candidates=candidates),
        )

        assert result["verification_status"] == "rejected"
        assert result["pipeline_stage"] == "cloud_rejected"
        assert result["verification_fallback"] is False
        # All candidates were tried
        assert sagemaker_client.invoke_endpoint.call_count == 2

    def test_no_candidates_rejected(self) -> None:
        """Empty candidate list → rejected."""
        s3_client = MagicMock()
        sagemaker_client = MagicMock()

        result = verify_bird(
            s3_client, sagemaker_client,
            bucket="b", s3_prefix="p/", candidates=[],
            metadata=_make_metadata(candidates=[]),
        )

        assert result["verification_status"] == "rejected"
        assert result["pipeline_stage"] == "cloud_rejected"
        assert sagemaker_client.invoke_endpoint.call_count == 0

    def test_just_below_threshold_rejected(self) -> None:
        """Confidence 0.49 (just below 0.5) → rejected."""
        s3_client = MagicMock()
        sagemaker_client = MagicMock()
        s3_client.get_object.return_value = _mock_s3_get_object(b"img")
        sagemaker_client.invoke_endpoint.return_value = _mock_sagemaker_response([
            {"species": "Sparrow", "confidence": 0.49},
        ])

        candidates = ["c0.jpg"]
        result = verify_bird(
            s3_client, sagemaker_client,
            bucket="b", s3_prefix="p/", candidates=candidates,
            metadata=_make_metadata(candidates=candidates),
        )

        assert result["verification_status"] == "rejected"



# ---------------------------------------------------------------------------
# Test: Non-bird classes directly verified
# Validates: Requirements 5.4
# ---------------------------------------------------------------------------


class TestNonBirdDirectVerify:
    """All classes go through SageMaker first. If SageMaker doesn't find a bird,
    the event is verified with the original YOLO class."""

    @pytest.mark.parametrize("cls", ["person", "cat", "dog"])
    def test_non_bird_verified_after_sagemaker_rejects(self, cls: str) -> None:
        """SageMaker doesn't find a bird → verified with YOLO's class."""
        s3_client = MagicMock()
        sagemaker_client = MagicMock()
        table = MagicMock()

        metadata = _make_metadata(primary_class=cls)
        metadata_bytes = json.dumps(metadata).encode("utf-8")

        # S3 returns metadata JSON, then candidate images
        s3_client.get_object.side_effect = [
            _mock_s3_get_object(metadata_bytes),  # metadata download
            _mock_s3_get_object(b"fake-jpeg"),     # candidate 0
            _mock_s3_get_object(b"fake-jpeg"),     # candidate 1
        ]
        s3_client.put_object.return_value = {}

        # SageMaker returns low confidence (not a bird)
        sagemaker_client.invoke_endpoint.side_effect = [
            _mock_sagemaker_response([{"species": "Unknown", "confidence": 0.1}]),
            _mock_sagemaker_response([{"species": "Unknown", "confidence": 0.05}]),
        ]

        result = _process_event(
            s3_client, sagemaker_client, table,
            bucket="test-bucket",
            metadata_key=f"captures/dev/2024-01-15/1700000000_{cls}.json",
            event_id=f"1700000000_{cls}",
            start_time=1000.0,
        )

        assert result["status"] == "verified"
        # SageMaker IS called now (for all classes)
        assert sagemaker_client.invoke_endpoint.call_count == 2
        # DynamoDB should be written (verified event)
        table.put_item.assert_called_once()

    def test_non_bird_overridden_when_sagemaker_finds_bird(self) -> None:
        """YOLO says person, but SageMaker finds a bird → primary_class overridden to bird."""
        s3_client = MagicMock()
        sagemaker_client = MagicMock()
        table = MagicMock()

        metadata = _make_metadata(primary_class="person")
        metadata_bytes = json.dumps(metadata).encode("utf-8")

        s3_client.get_object.side_effect = [
            _mock_s3_get_object(metadata_bytes),
            _mock_s3_get_object(b"fake-jpeg"),
        ]
        s3_client.put_object.return_value = {}

        sagemaker_client.invoke_endpoint.return_value = _mock_sagemaker_response([
            {"species": "House Sparrow", "confidence": 0.85},
        ])

        result = _process_event(
            s3_client, sagemaker_client, table,
            bucket="b",
            metadata_key="captures/dev/2024-01-15/123_person.json",
            event_id="123_person",
            start_time=1000.0,
        )

        assert result["status"] == "verified"
        table.put_item.assert_called_once()

    def test_unknown_class_verified_after_sagemaker(self) -> None:
        """Unknown classes also go through SageMaker, then verified."""
        s3_client = MagicMock()
        sagemaker_client = MagicMock()
        table = MagicMock()

        metadata = _make_metadata(primary_class="raccoon")
        metadata_bytes = json.dumps(metadata).encode("utf-8")

        s3_client.get_object.side_effect = [
            _mock_s3_get_object(metadata_bytes),
            _mock_s3_get_object(b"fake-jpeg"),
            _mock_s3_get_object(b"fake-jpeg"),
        ]
        s3_client.put_object.return_value = {}

        sagemaker_client.invoke_endpoint.side_effect = [
            _mock_sagemaker_response([{"species": "Unknown", "confidence": 0.1}]),
            _mock_sagemaker_response([{"species": "Unknown", "confidence": 0.05}]),
        ]

        result = _process_event(
            s3_client, sagemaker_client, table,
            bucket="b",
            metadata_key="captures/dev/2024-01-15/123_raccoon.json",
            event_id="123_raccoon",
            start_time=1000.0,
        )

        assert result["status"] == "verified"
        assert sagemaker_client.invoke_endpoint.call_count == 2


# ---------------------------------------------------------------------------
# Test: SageMaker timeout/failure degradation
# Validates: Requirements 5.7
# ---------------------------------------------------------------------------


class TestSageMakerTimeoutDegradation:
    """SageMaker call failure → degrade to verified with fallback flag."""

    def test_sagemaker_exception_degrades_to_verified(self) -> None:
        """When SageMaker raises an exception, result is verified + fallback=True."""
        s3_client = MagicMock()
        sagemaker_client = MagicMock()

        s3_client.get_object.return_value = _mock_s3_get_object(b"fake-jpeg")
        sagemaker_client.invoke_endpoint.side_effect = Exception("Endpoint timeout")

        candidates = ["c0.jpg", "c1.jpg"]
        result = verify_bird(
            s3_client, sagemaker_client,
            bucket="b", s3_prefix="p/", candidates=candidates,
            metadata=_make_metadata(candidates=candidates),
        )

        assert result["verification_status"] == "verified"
        assert result["pipeline_stage"] == "cloud_verified"
        assert result["verification_fallback"] is True
        assert result["bird_species"] is None
        assert result["species_confidence"] is None
        # Only 1 SageMaker call attempted before failure
        assert sagemaker_client.invoke_endpoint.call_count == 1

    def test_sagemaker_timeout_on_second_candidate(self) -> None:
        """First candidate below threshold, SageMaker fails on second → fallback verified."""
        s3_client = MagicMock()
        sagemaker_client = MagicMock()

        s3_client.get_object.return_value = _mock_s3_get_object(b"img")
        sagemaker_client.invoke_endpoint.side_effect = [
            _mock_sagemaker_response([{"species": "Unknown", "confidence": 0.2}]),
            Exception("Connection timeout"),
        ]

        candidates = ["c0.jpg", "c1.jpg", "c2.jpg"]
        result = verify_bird(
            s3_client, sagemaker_client,
            bucket="b", s3_prefix="p/", candidates=candidates,
            metadata=_make_metadata(candidates=candidates),
        )

        assert result["verification_status"] == "verified"
        assert result["verification_fallback"] is True
        assert sagemaker_client.invoke_endpoint.call_count == 2

    def test_s3_download_failure_skips_candidate(self) -> None:
        """If S3 download fails for a candidate, it's skipped (not a full failure)."""
        from botocore.exceptions import ClientError

        s3_client = MagicMock()
        sagemaker_client = MagicMock()

        # First candidate: S3 download fails; second: succeeds with low confidence
        s3_client.get_object.side_effect = [
            ClientError({"Error": {"Code": "NoSuchKey", "Message": "Not found"}}, "GetObject"),
            _mock_s3_get_object(b"img"),
        ]
        sagemaker_client.invoke_endpoint.return_value = _mock_sagemaker_response([
            {"species": "Sparrow", "confidence": 0.3},
        ])

        candidates = ["c0.jpg", "c1.jpg"]
        result = verify_bird(
            s3_client, sagemaker_client,
            bucket="b", s3_prefix="p/", candidates=candidates,
            metadata=_make_metadata(candidates=candidates),
        )

        # First candidate skipped, second below threshold → rejected
        assert result["verification_status"] == "rejected"
        # SageMaker only called once (for the second candidate)
        assert sagemaker_client.invoke_endpoint.call_count == 1


# ---------------------------------------------------------------------------
# Test: DynamoDB idempotent conditional write
# Validates: Requirements 8.4
# ---------------------------------------------------------------------------


class TestDynamoDBIdempotentWrite:
    """DynamoDB conditional write prevents duplicate records on repeated triggers."""

    def test_first_write_succeeds(self) -> None:
        """Normal write with ConditionExpression succeeds."""
        table = MagicMock()
        table.put_item.return_value = {}

        metadata = _make_metadata(primary_class="bird")
        verification = {
            "verification_status": "verified",
            "pipeline_stage": "cloud_verified",
            "verification_fallback": False,
            "bird_species": "House Sparrow",
            "species_confidence": 0.87,
        }

        _write_dynamodb(
            table, metadata, verification,
            metadata_key="captures/dev/2024-01-15/123_bird.json",
            s3_prefix="captures/dev/2024-01-15/",
        )

        table.put_item.assert_called_once()
        call_kwargs = table.put_item.call_args[1]

        # Verify ConditionExpression for idempotency
        assert "ConditionExpression" in call_kwargs
        assert "attribute_not_exists" in call_kwargs["ConditionExpression"]

        # Verify item contains required fields
        item = call_kwargs["Item"]
        assert item["verification_status"] == "verified"
        assert item["bird_species"] == "House Sparrow"
        assert item["pipeline_stage"] == "cloud_verified"
        assert "candidate_screenshots" in item
        assert "detected_classes" in item

    def test_duplicate_write_is_idempotent(self) -> None:
        """ConditionalCheckFailedException is caught silently (no re-raise)."""
        from botocore.exceptions import ClientError

        table = MagicMock()
        table.put_item.side_effect = ClientError(
            {"Error": {"Code": "ConditionalCheckFailedException", "Message": "Already exists"}},
            "PutItem",
        )

        metadata = _make_metadata(primary_class="person")
        verification = {
            "verification_status": "verified",
            "pipeline_stage": "cloud_verified",
            "verification_fallback": False,
        }

        # Should NOT raise — duplicate is silently ignored
        _write_dynamodb(
            table, metadata, verification,
            metadata_key="captures/dev/2024-01-15/123_person.json",
            s3_prefix="captures/dev/2024-01-15/",
        )

    def test_other_dynamodb_errors_propagate(self) -> None:
        """Non-idempotency DynamoDB errors are re-raised."""
        from botocore.exceptions import ClientError

        table = MagicMock()
        table.put_item.side_effect = ClientError(
            {"Error": {"Code": "ProvisionedThroughputExceededException", "Message": "Throttled"}},
            "PutItem",
        )

        metadata = _make_metadata(primary_class="person")
        verification = {
            "verification_status": "verified",
            "pipeline_stage": "cloud_verified",
            "verification_fallback": False,
        }

        with pytest.raises(ClientError):
            _write_dynamodb(
                table, metadata, verification,
                metadata_key="captures/dev/2024-01-15/123_person.json",
                s3_prefix="captures/dev/2024-01-15/",
            )

    def test_rejected_event_falls_back_to_verified(self) -> None:
        """SageMaker rejects bird → falls back to verified with YOLO class (req 5.5 updated)."""
        s3_client = MagicMock()
        sagemaker_client = MagicMock()
        table = MagicMock()

        metadata = _make_metadata(primary_class="bird", candidates=["c0.jpg"])
        metadata_bytes = json.dumps(metadata).encode("utf-8")

        # S3 returns metadata, then image
        s3_client.get_object.side_effect = [
            _mock_s3_get_object(metadata_bytes),  # metadata download
            _mock_s3_get_object(b"img"),           # candidate download
        ]
        s3_client.put_object.return_value = {}

        # SageMaker returns low confidence → rejected by verify_bird
        # But _process_event falls back to verified
        sagemaker_client.invoke_endpoint.return_value = _mock_sagemaker_response([
            {"species": "Unknown", "confidence": 0.1},
        ])

        result = _process_event(
            s3_client, sagemaker_client, table,
            bucket="b",
            metadata_key="captures/dev/2024-01-15/123_bird.json",
            event_id="123_bird",
            start_time=1000.0,
        )

        assert result["status"] == "verified"
        # DynamoDB IS written (fallback to verified)
        table.put_item.assert_called_once()


# ---------------------------------------------------------------------------
# Test: lambda_handler end-to-end
# ---------------------------------------------------------------------------


class TestLambdaHandler:
    """Integration-level tests for the Lambda entry point."""

    def test_skips_non_metadata_keys(self) -> None:
        """Keys not matching captures/*.json are skipped."""
        with patch.object(_mod, "boto3"):
            event = _make_s3_event("bucket", "other/path/image.jpg")
            result = lambda_handler(event, None)
            assert result["statusCode"] == 200

    def test_skips_non_captures_prefix(self) -> None:
        """JSON files outside captures/ prefix are skipped."""
        with patch.object(_mod, "boto3"):
            event = _make_s3_event("bucket", "logs/something.json")
            result = lambda_handler(event, None)
            assert result["statusCode"] == 200

    def test_handles_url_encoded_keys(self) -> None:
        """URL-encoded S3 keys are properly decoded."""
        mock_s3 = MagicMock()
        mock_sagemaker = MagicMock()
        mock_table = MagicMock()

        mock_boto3 = MagicMock()
        mock_boto3.client.side_effect = lambda svc: {
            "s3": mock_s3,
            "sagemaker-runtime": mock_sagemaker,
        }[svc]
        mock_boto3.resource.return_value.Table.return_value = mock_table

        metadata = _make_metadata(primary_class="person")
        metadata_bytes = json.dumps(metadata).encode("utf-8")
        mock_s3.get_object.return_value = _mock_s3_get_object(metadata_bytes)
        mock_s3.put_object.return_value = {}

        with patch.object(_mod, "boto3", mock_boto3):
            # URL-encoded key with spaces
            event = _make_s3_event("bucket", "captures/dev/2024-01-15/123+person.json")
            result = lambda_handler(event, None)
            assert result["statusCode"] == 200
