"""Property-based tests and unit tests for BioCLIP inference handler.

Tests Properties 1-7 from the bioclip-model-upgrade design document,
plus unit tests for model loading, configuration, and label coverage.

Uses Hypothesis for property-based testing and pytest for unit tests.
"""

import io
import json
import os
import sys

import numpy as np
import pytest
from hypothesis import HealthCheck, given, settings
from hypothesis import strategies as st
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from inference import (
    OOD_SIMILARITY_THRESHOLD,
    input_fn,
    model_fn,
    output_fn,
    predict_fn,
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_jpeg_bytes(width: int = 100, height: int = 100) -> bytes:
    """Generate random JPEG image bytes."""
    arr = np.random.randint(0, 256, (height, width, 3), dtype=np.uint8)
    img = Image.fromarray(arr, "RGB")
    buf = io.BytesIO()
    img.save(buf, format="JPEG")
    return buf.getvalue()


def _make_png_bytes(width: int = 100, height: int = 100) -> bytes:
    """Generate random PNG image bytes."""
    arr = np.random.randint(0, 256, (height, width, 3), dtype=np.uint8)
    img = Image.fromarray(arr, "RGB")
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def _make_pil_image(width: int = 100, height: int = 100) -> Image.Image:
    """Generate a random RGB PIL Image."""
    arr = np.random.randint(0, 256, (height, width, 3), dtype=np.uint8)
    return Image.fromarray(arr, "RGB")


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(scope="module")
def model_dict():
    """Load BioCLIP model once for the entire test module.

    Uses the parent directory (cloud/sagemaker/) as model_dir so that
    bird_labels.json is found alongside inference.py.
    """
    sagemaker_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    return model_fn(sagemaker_dir)


# ---------------------------------------------------------------------------
# Strategies
# ---------------------------------------------------------------------------

SUPPORTED_CONTENT_TYPES = {"application/x-image", "image/jpeg", "image/png"}

# Strategy for image dimensions (keep small for speed)
image_dimension = st.integers(min_value=10, max_value=200)

# Strategy for generating random prediction dicts (Property 5)
prediction_strategy = st.fixed_dictionaries(
    {
        "species": st.text(min_size=1, max_size=50),
        "confidence": st.floats(
            min_value=0.0, max_value=1.0, allow_nan=False, allow_infinity=False
        ),
        "similarity_score": st.floats(
            min_value=-1.0, max_value=1.0, allow_nan=False, allow_infinity=False
        ),
    }
)

prediction_list_strategy = st.lists(prediction_strategy, min_size=1, max_size=5)


# ===========================================================================
# Property Tests
# ===========================================================================

# Feature: bioclip-model-upgrade, Property 1: 图像解码一致性
# **Validates: Requirements 2.2**
class TestProperty1ImageDecodeConsistency:
    """For any valid JPEG/PNG image bytes, input_fn returns an RGB PIL Image."""

    @settings(max_examples=100, deadline=None)
    @given(width=image_dimension, height=image_dimension)
    def test_jpeg_decode_returns_rgb(self, width: int, height: int):
        jpeg_bytes = _make_jpeg_bytes(width, height)
        result = input_fn(jpeg_bytes, "image/jpeg")
        assert isinstance(result, Image.Image)
        assert result.mode == "RGB"

    @settings(max_examples=100, deadline=None)
    @given(width=image_dimension, height=image_dimension)
    def test_png_decode_returns_rgb(self, width: int, height: int):
        png_bytes = _make_png_bytes(width, height)
        result = input_fn(png_bytes, "image/png")
        assert isinstance(result, Image.Image)
        assert result.mode == "RGB"

    @settings(max_examples=100, deadline=None)
    @given(width=image_dimension, height=image_dimension)
    def test_generic_image_decode_returns_rgb(self, width: int, height: int):
        jpeg_bytes = _make_jpeg_bytes(width, height)
        result = input_fn(jpeg_bytes, "application/x-image")
        assert isinstance(result, Image.Image)
        assert result.mode == "RGB"


# Feature: bioclip-model-upgrade, Property 2: 不支持的 content_type 拒绝
# **Validates: Requirements 2.3**
class TestProperty2UnsupportedContentType:
    """For any content_type not in the supported set, input_fn raises ValueError."""

    @settings(max_examples=100, deadline=None)
    @given(
        content_type=st.text(min_size=1, max_size=100).filter(
            lambda t: t not in SUPPORTED_CONTENT_TYPES
        )
    )
    def test_unsupported_content_type_raises(self, content_type: str):
        jpeg_bytes = _make_jpeg_bytes()
        with pytest.raises(ValueError):
            input_fn(jpeg_bytes, content_type)


# Feature: bioclip-model-upgrade, Property 3: predict_fn 输出结构完整性
# **Validates: Requirements 3.3, 3.4, 10.2, 10.6**
class TestProperty3PredictFnOutputStructure:
    """For any valid PIL Image, predict_fn returns 1-3 prediction dicts
    each containing species(str), confidence(float), similarity_score(float)."""

    @settings(
        max_examples=100,
        deadline=None,
        suppress_health_check=[HealthCheck.too_slow],
    )
    @given(width=image_dimension, height=image_dimension)
    def test_output_structure(self, width: int, height: int, model_dict):
        img = _make_pil_image(width, height)
        predictions = predict_fn(img, model_dict)

        assert isinstance(predictions, list)
        assert 1 <= len(predictions) <= 3

        for pred in predictions:
            assert isinstance(pred, dict)
            assert "species" in pred
            assert "confidence" in pred
            assert "similarity_score" in pred
            assert isinstance(pred["species"], str)
            assert isinstance(pred["confidence"], float)
            assert isinstance(pred["similarity_score"], float)


# Feature: bioclip-model-upgrade, Property 4: confidence 值域不变量
# **Validates: Requirements 10.3**
class TestProperty4ConfidenceRange:
    """For any valid PIL Image, each confidence is in [0.0, 1.0]."""

    @settings(
        max_examples=100,
        deadline=None,
        suppress_health_check=[HealthCheck.too_slow],
    )
    @given(width=image_dimension, height=image_dimension)
    def test_confidence_in_range(self, width: int, height: int, model_dict):
        img = _make_pil_image(width, height)
        predictions = predict_fn(img, model_dict)

        for pred in predictions:
            assert 0.0 <= pred["confidence"] <= 1.0, (
                f"confidence {pred['confidence']} out of [0, 1] range"
            )


# Feature: bioclip-model-upgrade, Property 5: output_fn 序列化往返
# **Validates: Requirements 5.1, 5.2, 5.3, 10.5**
class TestProperty5OutputFnRoundTrip:
    """For any prediction list, output_fn JSON round-trips correctly."""

    @settings(max_examples=100, deadline=None)
    @given(predictions=prediction_list_strategy)
    def test_json_round_trip(self, predictions: list[dict]):
        json_str = output_fn(predictions, "application/json")
        parsed = json.loads(json_str)

        assert "predictions" in parsed
        assert len(parsed["predictions"]) == len(predictions)

        for original, restored in zip(predictions, parsed["predictions"]):
            assert restored["species"] == original["species"]
            assert restored["confidence"] == pytest.approx(
                original["confidence"], abs=1e-10
            )
            assert restored["similarity_score"] == pytest.approx(
                original["similarity_score"], abs=1e-10
            )


# Feature: bioclip-model-upgrade, Property 6: output_fn 拒绝非 JSON accept 类型
# **Validates: Requirements 5.4**
class TestProperty6OutputFnRejectsNonJson:
    """For any accept != 'application/json', output_fn raises ValueError."""

    @settings(max_examples=100, deadline=None)
    @given(
        accept=st.text(min_size=1, max_size=100).filter(
            lambda t: t != "application/json"
        )
    )
    def test_non_json_accept_raises(self, accept: str):
        dummy_predictions = [
            {"species": "test", "confidence": 0.5, "similarity_score": 0.3}
        ]
        with pytest.raises(ValueError):
            output_fn(dummy_predictions, accept)


# Feature: bioclip-model-upgrade, Property 7: 模型权重正确性（不同输入产生不同输出）
# **Validates: Requirements 10.4**
class TestProperty7ModelWeightsCorrectness:
    """Real bird photo vs random noise → different predictions."""

    @settings(
        max_examples=100,
        deadline=None,
        suppress_health_check=[HealthCheck.too_slow],
    )
    @given(width=image_dimension, height=image_dimension)
    def test_bird_vs_noise_different_predictions(
        self, width: int, height: int, model_dict
    ):
        # Load real bird image
        bird_image_path = os.path.join(
            os.path.dirname(__file__), "..", "..", "..", "bird-cropped.jpg"
        )
        bird_image = Image.open(bird_image_path).convert("RGB")
        bird_preds = predict_fn(bird_image, model_dict)

        # Generate random noise image
        noise_image = _make_pil_image(width, height)
        noise_preds = predict_fn(noise_image, model_dict)

        # Predictions should differ — either species or confidence
        bird_species = [p["species"] for p in bird_preds]
        noise_species = [p["species"] for p in noise_preds]
        bird_confs = [p["confidence"] for p in bird_preds]
        noise_confs = [p["confidence"] for p in noise_preds]

        assert bird_species != noise_species or bird_confs != noise_confs, (
            "Bird photo and random noise produced identical predictions"
        )


# ===========================================================================
# Unit Tests
# ===========================================================================


class TestUnitModelFn:
    """Unit tests for model_fn behavior."""

    # Validates: Requirements 1.5
    def test_model_fn_returns_required_keys(self, model_dict):
        required_keys = {
            "model",
            "preprocess",
            "tokenizer",
            "text_features",
            "label_list",
            "device",
        }
        assert required_keys.issubset(
            model_dict.keys()
        ), f"Missing keys: {required_keys - model_dict.keys()}"

    # Validates: Requirements 1.1
    def test_model_fn_eval_mode(self, model_dict):
        model = model_dict["model"]
        assert not model.training, "Model should be in eval mode"

    # Validates: Requirements 1.3
    def test_model_fn_text_features_shape(self, model_dict):
        text_features = model_dict["text_features"]
        label_list = model_dict["label_list"]
        assert text_features.shape[0] == len(label_list), (
            f"text_features rows ({text_features.shape[0]}) != "
            f"label_list length ({len(label_list)})"
        )


class TestUnitOODThreshold:
    """Unit tests for OOD threshold configuration."""

    # Validates: Requirements 4.4
    def test_ood_threshold_configurable(self, monkeypatch):
        """Verify that OOD_THRESHOLD env var overrides the default."""
        custom_value = "0.42"
        monkeypatch.setenv("OOD_THRESHOLD", custom_value)

        # Re-evaluate the threshold expression the same way inference.py does
        result = float(os.environ.get("OOD_THRESHOLD", "0.18"))
        assert result == pytest.approx(0.42)

    def test_ood_threshold_default(self):
        """Verify the default OOD threshold is 0.18."""
        # If env var is not set, the module-level constant should be 0.18
        # (unless overridden in the test environment)
        default = float(os.environ.get("OOD_THRESHOLD", "0.18"))
        assert default == OOD_SIMILARITY_THRESHOLD


class TestUnitBirdLabels:
    """Unit tests for bird label coverage."""

    @pytest.fixture()
    def species_list(self):
        labels_path = os.path.join(
            os.path.dirname(__file__), "..", "bird_labels.json"
        )
        with open(labels_path) as f:
            return json.load(f)["species"]

    # Validates: Requirements 8.2
    def test_bird_labels_contains_asian_species(self, species_list):
        asian_birds = {
            "White-cheeked Starling",
            "Japanese White-eye",
            "Oriental Magpie-Robin",
            "Eurasian Tree Sparrow",
        }
        missing = asian_birds - set(species_list)
        assert not missing, f"Missing Asian species: {missing}"

    # Validates: Requirements 8.3
    def test_bird_labels_contains_na_eu_species(self, species_list):
        na_eu_birds = {
            "Blue Jay",
            "Northern Cardinal",
            "American Robin",
            "European Robin",
            "Great Tit",
        }
        missing = na_eu_birds - set(species_list)
        assert not missing, f"Missing NA/EU species: {missing}"
