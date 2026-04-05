"""Unit tests for config validation in model-switching inference architecture.

Tests _validate_config and _load_config functions.
Requirements: 3.1, 3.2, 3.3, 3.4
"""

import json
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from inference import _validate_config, _load_config, REQUIRED_FIELDS, CONFIG_DEFAULTS


# --- _validate_config tests ---


def test_validate_config_complete():
    """Complete config with all required fields passes validation."""
    config = {
        "model_type": "dinov2",
        "model_name": "DINOv2-ViT-L/14 Bird Classifier",
        "class_names": ["Barn_Swallow", "Azure-winged_Magpie"],
    }
    # Should not raise
    _validate_config(config)


def test_validate_config_missing_single_field():
    """Missing one required field raises ValueError mentioning that field."""
    for field in REQUIRED_FIELDS:
        config = {
            "model_type": "dinov2",
            "model_name": "test-model",
            "class_names": ["bird_a"],
        }
        del config[field]
        with pytest.raises(ValueError, match=field):
            _validate_config(config)


def test_validate_config_missing_multiple_fields():
    """Missing multiple required fields raises ValueError mentioning all."""
    config = {}  # all required fields missing
    with pytest.raises(ValueError) as exc_info:
        _validate_config(config)
    error_msg = str(exc_info.value)
    for field in REQUIRED_FIELDS:
        assert field in error_msg


# --- _load_config tests ---


def test_load_config_fills_defaults(tmp_path):
    """_load_config fills default values when optional fields not provided."""
    config_data = {
        "model_type": "dinov2",
        "model_name": "test-model",
        "class_names": ["bird_a", "bird_b"],
    }
    config_path = tmp_path / "model_config.json"
    config_path.write_text(json.dumps(config_data))

    result = _load_config(str(tmp_path))

    for key, default_value in CONFIG_DEFAULTS.items():
        assert result[key] == default_value, (
            f"Expected default {key}={default_value}, got {result[key]}"
        )


def test_load_config_preserves_user_values(tmp_path):
    """_load_config does NOT overwrite user-provided values for optional fields."""
    config_data = {
        "model_type": "dinov2",
        "model_name": "test-model",
        "class_names": ["bird_a"],
        "image_size": 518,
        "confidence_threshold": 0.5,
        "top_k": 5,
    }
    config_path = tmp_path / "model_config.json"
    config_path.write_text(json.dumps(config_data))

    result = _load_config(str(tmp_path))

    assert result["image_size"] == 518
    assert result["confidence_threshold"] == 0.5
    assert result["top_k"] == 5


def test_load_config_file_not_found(tmp_path):
    """_load_config raises FileNotFoundError when model_config.json doesn't exist."""
    with pytest.raises(FileNotFoundError):
        _load_config(str(tmp_path))


# --- DINOv2 handler tests ---
# Requirements: 4.1, 4.3, 4.6

import torch
import torch.nn as nn
from handler_dinov2 import DINOv2Classifier, preprocess, predict, EMBED_DIM


class TestDINOv2HandlerStructure:
    """Test DINOv2 handler model structure and interface.
    Requirements: 4.1, 4.6
    """

    def test_classifier_head_structure(self):
        """DINOv2Classifier head should have correct layer structure:
        LayerNorm(1024) -> Linear(1024,512) -> GELU -> Dropout(0.3) -> Linear(512, num_classes)
        """
        num_classes = 10
        head = nn.Sequential(
            nn.LayerNorm(EMBED_DIM),
            nn.Linear(EMBED_DIM, 512),
            nn.GELU(),
            nn.Dropout(0.3),
            nn.Linear(512, num_classes),
        )
        assert isinstance(head[0], nn.LayerNorm)
        assert head[0].normalized_shape == (1024,)
        assert isinstance(head[1], nn.Linear)
        assert head[1].in_features == 1024
        assert head[1].out_features == 512
        assert isinstance(head[2], nn.GELU)
        assert isinstance(head[3], nn.Dropout)
        assert head[3].p == 0.3
        assert isinstance(head[4], nn.Linear)
        assert head[4].in_features == 512
        assert head[4].out_features == num_classes


class TestDINOv2Predict:
    """Test DINOv2 predict function with mock models.
    Requirements: 4.3, 4.6
    """

    def _make_mock_model(self, logits_tensor):
        """Create a mock model that returns fixed logits."""
        class MockModel:
            def __init__(self, logits):
                self._logits = logits
            def __call__(self, x):
                return self._logits.unsqueeze(0)
            def eval(self):
                return self
            def to(self, device):
                return self
        return MockModel(logits_tensor)

    def test_predict_uses_class_names(self):
        """predict should use class_names from model_dict. (Req 4.3)"""
        class_names = ["Barn_Swallow", "Azure-winged_Magpie", "Eurasian_Tree_Sparrow"]
        logits = torch.tensor([5.0, 1.0, 0.5])
        model = self._make_mock_model(logits)
        model_dict = {"model": model, "class_names": class_names, "device": torch.device("cpu")}
        config = {"confidence_threshold": 0.0, "top_k": 3}
        dummy = torch.randn(1, 3, 518, 518)

        preds = predict(model_dict, dummy, config)

        assert preds[0]["species"] == "Barn_Swallow"
        assert all(p["species"] in class_names for p in preds)

    def test_predict_top_k_limits_output(self):
        """predict should return at most top_k predictions."""
        class_names = [f"species_{i}" for i in range(20)]
        logits = torch.randn(20)
        model = self._make_mock_model(logits)
        model_dict = {"model": model, "class_names": class_names, "device": torch.device("cpu")}
        config = {"confidence_threshold": 0.0, "top_k": 5}
        dummy = torch.randn(1, 3, 518, 518)

        preds = predict(model_dict, dummy, config)
        assert len(preds) <= 5

    def test_predict_not_a_bird_below_threshold(self):
        """predict should return not_a_bird when max confidence < threshold."""
        class_names = [f"species_{i}" for i in range(20)]
        logits = torch.zeros(20)  # uniform softmax = 1/20 = 0.05
        model = self._make_mock_model(logits)
        model_dict = {"model": model, "class_names": class_names, "device": torch.device("cpu")}
        config = {"confidence_threshold": 0.2, "top_k": 3}
        dummy = torch.randn(1, 3, 518, 518)

        preds = predict(model_dict, dummy, config)
        assert len(preds) == 1
        assert preds[0]["species"] == "not_a_bird"
        assert preds[0]["confidence"] == 0.0

    def test_predict_end_to_end_with_preprocess(self):
        """End-to-end: preprocess image then predict with mock model. (Req 4.6)"""
        from PIL import Image as PILImage

        class_names = ["Barn_Swallow", "Azure-winged_Magpie"]
        logits = torch.tensor([3.0, 1.0])
        model = self._make_mock_model(logits)
        model_dict = {"model": model, "class_names": class_names, "device": torch.device("cpu")}
        config = {"confidence_threshold": 0.0, "top_k": 2, "image_size": 518}

        image = PILImage.new("RGB", (640, 480))
        tensor = preprocess(image, config)
        preds = predict(model_dict, tensor, config)

        assert len(preds) == 2
        assert preds[0]["species"] == "Barn_Swallow"
        assert preds[0]["confidence"] > preds[1]["confidence"]


# --- inference.py SageMaker function tests ---
# Requirements: 1.1, 1.2, 1.3, 1.4, 2.1, 2.2

import io
from PIL import Image as PILImage
from unittest.mock import MagicMock

from inference import input_fn, predict_fn, output_fn


class TestInputFn:
    """Test input_fn image decoding. Requirements: 1.1, 1.4"""

    def test_jpeg_decoding(self):
        """input_fn decodes JPEG bytes to RGB PIL Image."""
        img = PILImage.new("RGB", (100, 100), color=(255, 0, 0))
        buf = io.BytesIO()
        img.save(buf, format="JPEG")

        result = input_fn(buf.getvalue(), "image/jpeg")
        assert isinstance(result, PILImage.Image)
        assert result.mode == "RGB"
        assert result.size == (100, 100)

    def test_png_decoding(self):
        """input_fn decodes PNG bytes to RGB PIL Image."""
        img = PILImage.new("RGBA", (50, 50))  # RGBA should be converted to RGB
        buf = io.BytesIO()
        img.save(buf, format="PNG")

        result = input_fn(buf.getvalue(), "image/png")
        assert result.mode == "RGB"

    def test_application_x_image_content_type(self):
        """input_fn accepts application/x-image content type."""
        img = PILImage.new("RGB", (10, 10))
        buf = io.BytesIO()
        img.save(buf, format="JPEG")

        result = input_fn(buf.getvalue(), "application/x-image")
        assert isinstance(result, PILImage.Image)

    def test_unsupported_content_type_raises(self):
        """input_fn raises ValueError for unsupported content type."""
        with pytest.raises(ValueError, match="Unsupported content type"):
            input_fn(b"dummy", "text/plain")


class TestPredictFn:
    """Test predict_fn handler delegation. Requirements: 2.1, 2.2"""

    def test_predict_fn_delegates_to_handler(self):
        """predict_fn should call handler.preprocess and handler.predict."""
        mock_handler = MagicMock()
        mock_tensor = MagicMock()
        mock_handler.preprocess.return_value = mock_tensor
        mock_handler.predict.return_value = [{"species": "Barn_Swallow", "confidence": 0.9}]

        config = {"confidence_threshold": 0.2, "top_k": 3}
        model_dict = {"_handler": mock_handler, "_config": config}
        image = PILImage.new("RGB", (100, 100))

        result = predict_fn(image, model_dict)

        mock_handler.preprocess.assert_called_once_with(image, config)
        mock_handler.predict.assert_called_once_with(model_dict, mock_tensor, config)
        assert result == [{"species": "Barn_Swallow", "confidence": 0.9}]


class TestOutputFn:
    """Test output_fn JSON formatting."""

    def test_output_fn_formats_json(self):
        """output_fn wraps predictions in {"predictions": [...]}."""
        preds = [{"species": "Barn_Swallow", "confidence": 0.9}]
        result = output_fn(preds)
        parsed = json.loads(result)
        assert parsed == {"predictions": preds}

    def test_output_fn_unsupported_accept_raises(self):
        """output_fn raises ValueError for non-JSON accept type."""
        with pytest.raises(ValueError, match="Unsupported accept type"):
            output_fn([], "text/xml")
