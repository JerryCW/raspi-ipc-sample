"""Preservation property tests for SageMaker bird classifier.

Property 2: Preservation — interface format and OOD logic compatibility.

These tests observe and encode the baseline behavior of the inference handler
that MUST be preserved after the fix. They should PASS on both unfixed and
fixed code.

Observation-first methodology:
1. Observe: run input_fn, output_fn, predict_fn on unfixed code
2. Encode: write property-based tests capturing the observed behavior
3. Verify: tests pass on unfixed code (baseline confirmed)
4. Re-verify: tests pass on fixed code (no regression)
"""

import io
import json
import os
import sys

import numpy as np
import pytest
from hypothesis import given, settings, HealthCheck
from hypothesis import strategies as st
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from inference import input_fn, output_fn, predict_fn, model_fn
from inference import OOD_SIMILARITY_THRESHOLD


def _make_jpeg_bytes(width=100, height=100):
    """Create a random JPEG image as bytes."""
    arr = np.random.randint(0, 256, (height, width, 3), dtype=np.uint8)
    img = Image.fromarray(arr, "RGB")
    buf = io.BytesIO()
    img.save(buf, format="JPEG")
    return buf.getvalue()


def _make_png_bytes(width=100, height=100):
    """Create a random PNG image as bytes."""
    arr = np.random.randint(0, 256, (height, width, 3), dtype=np.uint8)
    img = Image.fromarray(arr, "RGB")
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


@pytest.fixture(scope="module")
def model_dict():
    """Load the model once for all tests in this module."""
    sagemaker_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    return model_fn(sagemaker_dir)


class TestInputFnPreservation:
    """Preservation tests for input_fn: content type acceptance/rejection."""

    def test_accepts_application_x_image(self):
        """input_fn accepts application/x-image content type."""
        jpeg_bytes = _make_jpeg_bytes()
        result = input_fn(jpeg_bytes, "application/x-image")
        assert isinstance(result, Image.Image)
        assert result.mode == "RGB"

    def test_accepts_image_jpeg(self):
        """input_fn accepts image/jpeg content type."""
        jpeg_bytes = _make_jpeg_bytes()
        result = input_fn(jpeg_bytes, "image/jpeg")
        assert isinstance(result, Image.Image)

    def test_accepts_image_png(self):
        """input_fn accepts image/png content type."""
        png_bytes = _make_png_bytes()
        result = input_fn(png_bytes, "image/png")
        assert isinstance(result, Image.Image)

    def test_rejects_text_plain(self):
        """input_fn rejects text/plain content type."""
        with pytest.raises(ValueError, match="Unsupported content type"):
            input_fn(b"hello", "text/plain")

    def test_rejects_application_json(self):
        """input_fn rejects application/json content type."""
        with pytest.raises(ValueError, match="Unsupported content type"):
            input_fn(b"{}", "application/json")

    @given(
        width=st.integers(min_value=10, max_value=500),
        height=st.integers(min_value=10, max_value=500),
    )
    @settings(max_examples=10, deadline=None, suppress_health_check=[HealthCheck.too_slow])
    def test_random_image_returns_pil_rgb(self, width, height):
        """For any random RGB image, input_fn returns a PIL Image in RGB mode."""
        arr = np.random.randint(0, 256, (height, width, 3), dtype=np.uint8)
        img = Image.fromarray(arr, "RGB")
        buf = io.BytesIO()
        img.save(buf, format="JPEG")
        jpeg_bytes = buf.getvalue()

        result = input_fn(jpeg_bytes, "application/x-image")
        assert isinstance(result, Image.Image)
        assert result.mode == "RGB"


class TestPredictFnPreservation:
    """Preservation tests for predict_fn: output structure."""

    @given(
        width=st.integers(min_value=32, max_value=300),
        height=st.integers(min_value=32, max_value=300),
    )
    @settings(max_examples=5, deadline=None, suppress_health_check=[HealthCheck.too_slow])
    def test_returns_list_of_dicts(self, model_dict, width, height):
        """predict_fn returns a list of dicts for any input image."""
        arr = np.random.randint(0, 256, (height, width, 3), dtype=np.uint8)
        image = Image.fromarray(arr, "RGB")

        predictions = predict_fn(image, model_dict)

        assert isinstance(predictions, list)
        assert len(predictions) >= 1

        for pred in predictions:
            assert isinstance(pred, dict)
            assert "species" in pred, f"Missing 'species' key in prediction: {pred}"
            assert "confidence" in pred, f"Missing 'confidence' key in prediction: {pred}"
            assert "similarity_score" in pred, f"Missing 'similarity_score' key in prediction: {pred}"
            assert isinstance(pred["species"], str)
            assert isinstance(pred["confidence"], float)
            assert isinstance(pred["similarity_score"], float)
            # Confidence should be in [0, 1]
            assert 0.0 <= pred["confidence"] <= 1.0, (
                f"Confidence out of range: {pred['confidence']}"
            )


class TestOutputFnPreservation:
    """Preservation tests for output_fn: JSON format."""

    def test_output_contains_predictions_key(self):
        """output_fn wraps predictions in {"predictions": [...]} format."""
        sample_predictions = [
            {"species": "BALD EAGLE", "confidence": 0.95, "similarity_score": 0.35},
            {"species": "GOLDEN EAGLE", "confidence": 0.03, "similarity_score": 0.25},
        ]
        result = output_fn(sample_predictions)
        parsed = json.loads(result)

        assert "predictions" in parsed
        assert isinstance(parsed["predictions"], list)
        assert len(parsed["predictions"]) == 2

    def test_output_preserves_prediction_fields(self):
        """output_fn preserves all fields in each prediction dict."""
        sample = [{"species": "CROW", "confidence": 0.8, "similarity_score": 0.30}]
        result = output_fn(sample)
        parsed = json.loads(result)

        pred = parsed["predictions"][0]
        assert pred["species"] == "CROW"
        assert pred["confidence"] == 0.8
        assert pred["similarity_score"] == 0.30

    def test_output_rejects_non_json_accept(self):
        """output_fn rejects non-JSON accept types."""
        with pytest.raises(ValueError, match="Unsupported accept type"):
            output_fn([], "text/html")

    @given(
        n=st.integers(min_value=0, max_value=10),
    )
    @settings(max_examples=10, deadline=None)
    def test_output_preserves_list_length(self, n):
        """For any prediction list, output_fn preserves the list length."""
        predictions = [
            {"species": f"BIRD_{i}", "confidence": 0.5, "similarity_score": 0.25}
            for i in range(n)
        ]
        result = output_fn(predictions)
        parsed = json.loads(result)

        assert len(parsed["predictions"]) == n


class TestOODThresholdPreservation:
    """Preservation tests for OOD detection threshold."""

    def test_ood_similarity_threshold_is_float(self):
        """OOD_SIMILARITY_THRESHOLD must be a float."""
        assert isinstance(OOD_SIMILARITY_THRESHOLD, float)

    def test_ood_similarity_threshold_default(self):
        """OOD_SIMILARITY_THRESHOLD default is 0.18."""
        default = float(os.environ.get("OOD_THRESHOLD", "0.18"))
        assert OOD_SIMILARITY_THRESHOLD == default


class TestUnsupportedContentTypePreservation:
    """Property-based test for unsupported content types."""

    @given(
        content_type=st.sampled_from([
            "text/plain", "text/html", "application/json",
            "application/xml", "multipart/form-data", "text/csv",
        ])
    )
    @settings(max_examples=6, deadline=None)
    def test_unsupported_types_raise_valueerror(self, content_type):
        """input_fn raises ValueError for any unsupported content type."""
        with pytest.raises(ValueError):
            input_fn(b"data", content_type)
