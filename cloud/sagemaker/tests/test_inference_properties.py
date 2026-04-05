"""Property-based tests for model-switching inference architecture.

Uses hypothesis to verify correctness properties defined in the design document.
"""

import os
import sys

import pytest
import torch
from hypothesis import given, settings, assume
from hypothesis import strategies as st
from PIL import Image as PILImage

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from inference import _validate_config, _load_handler, REQUIRED_FIELDS


# Feature: model-switching-inference, Property 3: 配置验证捕获缺失的必填字段
# **Validates: Requirements 3.2, 3.4**


@given(
    fields_to_remove=st.frozensets(
        st.sampled_from(sorted(REQUIRED_FIELDS)),
        min_size=1,
    ),
    extra_keys=st.dictionaries(
        keys=st.text(min_size=1, max_size=20).filter(lambda k: k not in REQUIRED_FIELDS),
        values=st.text(min_size=1, max_size=50),
        max_size=5,
    ),
)
@settings(max_examples=100)
def test_validate_config_catches_missing_required_fields(fields_to_remove, extra_keys):
    """For any config dict missing one or more required fields,
    _validate_config should raise ValueError whose message contains
    every missing field name."""
    # Build a complete config, then remove a random non-empty subset of required fields
    complete_config = {
        "model_type": "dinov2",
        "model_name": "test-model",
        "class_names": ["bird_a", "bird_b"],
    }
    # Merge extra arbitrary keys (non-required) to ensure they don't interfere
    config = {**complete_config, **extra_keys}

    for field in fields_to_remove:
        config.pop(field, None)

    with pytest.raises(ValueError) as exc_info:
        _validate_config(config)

    error_msg = str(exc_info.value)
    for field in fields_to_remove:
        assert field in error_msg, (
            f"Missing field '{field}' not mentioned in error: {error_msg}"
        )


@given(
    extra_keys=st.dictionaries(
        keys=st.text(min_size=1, max_size=20).filter(lambda k: k not in REQUIRED_FIELDS),
        values=st.text(min_size=1, max_size=50),
        max_size=5,
    ),
)
@settings(max_examples=100)
def test_validate_config_passes_when_all_required_fields_present(extra_keys):
    """For any config dict that contains all required fields (plus arbitrary extras),
    _validate_config should NOT raise."""
    config = {
        "model_type": "dinov2",
        "model_name": "test-model",
        "class_names": ["bird_a", "bird_b"],
        **extra_keys,
    }
    # Should not raise
    _validate_config(config)


# Feature: model-switching-inference, Property 4: 无效 model_type 产生明确错误
# **Validates: Requirements 2.3**

# Known handler types that actually exist as modules
KNOWN_HANDLER_TYPES = {"dinov2", "bioclip"}


@given(
    model_type=st.text(
        min_size=1,
        max_size=30,
        alphabet=st.characters(whitelist_categories=("L", "N"), whitelist_characters="_-"),
    ).filter(lambda s: s not in KNOWN_HANDLER_TYPES),
)
@settings(max_examples=100)
def test_invalid_model_type_produces_clear_error(model_type):
    """For any model_type string that doesn't correspond to an existing handler module,
    _load_handler should raise an error whose message contains the model_type name."""
    with pytest.raises((ImportError, ModuleNotFoundError)) as exc_info:
        _load_handler(model_type)
    assert model_type in str(exc_info.value), (
        f"Error message should contain model_type '{model_type}': {exc_info.value}"
    )


# ---------------------------------------------------------------------------
# DINOv2 preprocess tests
# ---------------------------------------------------------------------------

from handler_dinov2 import preprocess as dinov2_preprocess

# Feature: model-switching-inference, Property 5: 预处理输出张量形状正确
# **Validates: Requirements 4.2**


@given(
    width=st.integers(min_value=1, max_value=2000),
    height=st.integers(min_value=1, max_value=2000),
)
@settings(max_examples=100, deadline=None)
def test_dinov2_preprocess_output_shape(width, height):
    """For any RGB PIL Image with width/height >= 1,
    DINOv2 preprocess should return a float tensor of shape (1, 3, 518, 518)."""
    image = PILImage.new("RGB", (width, height), color=(128, 128, 128))
    config = {"image_size": 518}

    result = dinov2_preprocess(image, config)

    assert isinstance(result, torch.Tensor)
    assert result.shape == (1, 3, 518, 518), f"Expected (1, 3, 518, 518), got {result.shape}"
    assert result.is_floating_point(), f"Expected float tensor, got {result.dtype}"


# ---------------------------------------------------------------------------
# DINOv2 predict output format tests
# ---------------------------------------------------------------------------

from handler_dinov2 import predict as dinov2_predict

# Feature: model-switching-inference, Property 6: 预测输出格式合法且按置信度降序排列
# **Validates: Requirements 1.2, 4.4**


class _MockDINOv2Model:
    """Mock model that returns pre-set logits for testing predict()."""

    def __init__(self, logits):
        self._logits = logits

    def __call__(self, x):
        return self._logits.unsqueeze(0)  # (1, num_classes)

    def eval(self):
        return self

    def to(self, device):
        return self


@given(
    num_classes=st.integers(min_value=2, max_value=50),
    top_k=st.integers(min_value=1, max_value=10),
    data=st.data(),
)
@settings(max_examples=100, deadline=None)
def test_dinov2_predict_output_format_and_order(num_classes, top_k, data):
    """For any valid logits tensor, predict output should have:
    - confidence in [0.0, 1.0]
    - sorted by confidence descending
    - length <= top_k"""
    # Generate random logits
    logits = torch.randn(num_classes)

    class_names = [f"species_{i}" for i in range(num_classes)]
    mock_model = _MockDINOv2Model(logits)

    model_dict = {
        "model": mock_model,
        "class_names": class_names,
        "device": torch.device("cpu"),
    }
    config = {
        "confidence_threshold": 0.0,  # Set to 0 so we always get predictions (not not_a_bird)
        "top_k": top_k,
    }

    # Create dummy input tensor
    dummy_tensor = torch.randn(1, 3, 518, 518)

    predictions = dinov2_predict(model_dict, dummy_tensor, config)

    # Length <= top_k
    assert len(predictions) <= top_k, f"Expected <= {top_k} predictions, got {len(predictions)}"

    # Each prediction has species and confidence
    for pred in predictions:
        assert "species" in pred
        assert "confidence" in pred
        assert 0.0 <= pred["confidence"] <= 1.0, f"Confidence {pred['confidence']} out of range"

    # Sorted by confidence descending
    confidences = [p["confidence"] for p in predictions]
    for i in range(len(confidences) - 1):
        assert confidences[i] >= confidences[i + 1], (
            f"Not sorted descending: {confidences}"
        )


# Feature: model-switching-inference, Property 7: 低置信度返回 not_a_bird
# **Validates: Requirements 1.3, 4.5**


@given(
    num_classes=st.integers(min_value=10, max_value=50),
)
@settings(max_examples=100, deadline=None)
def test_dinov2_predict_low_confidence_returns_not_a_bird(num_classes):
    """When softmax max probability is below confidence_threshold,
    predict should return [{"species": "not_a_bird", "confidence": 0.0}]."""
    confidence_threshold = 0.2

    # All-zero logits → softmax gives 1/num_classes for each class
    # With num_classes >= 10, max prob = 0.1 < 0.2 threshold
    logits = torch.zeros(num_classes)

    class_names = [f"species_{i}" for i in range(num_classes)]
    mock_model = _MockDINOv2Model(logits)

    model_dict = {
        "model": mock_model,
        "class_names": class_names,
        "device": torch.device("cpu"),
    }
    config = {
        "confidence_threshold": confidence_threshold,
        "top_k": 3,
    }

    dummy_tensor = torch.randn(1, 3, 518, 518)
    predictions = dinov2_predict(model_dict, dummy_tensor, config)

    assert len(predictions) == 1
    assert predictions[0]["species"] == "not_a_bird"
    assert predictions[0]["confidence"] == 0.0


# ---------------------------------------------------------------------------
# input_fn tests
# ---------------------------------------------------------------------------

from inference import input_fn
import io

# Feature: model-switching-inference, Property 1: 合法图片类型被正确解码
# **Validates: Requirements 1.1**

SUPPORTED_CONTENT_TYPES = ["application/x-image", "image/jpeg", "image/png"]

def _make_image_bytes(width, height, fmt="JPEG"):
    """Helper to create valid image bytes."""
    img = PILImage.new("RGB", (width, height), color=(100, 150, 200))
    buf = io.BytesIO()
    img.save(buf, format=fmt)
    return buf.getvalue()

@given(
    width=st.integers(min_value=1, max_value=500),
    height=st.integers(min_value=1, max_value=500),
    content_type=st.sampled_from(SUPPORTED_CONTENT_TYPES),
    fmt=st.sampled_from(["JPEG", "PNG"]),
)
@settings(max_examples=100, deadline=None)
def test_valid_image_decoded_correctly(width, height, content_type, fmt):
    """For any valid JPEG/PNG image bytes and supported content type,
    input_fn should return an RGB PIL Image with size > 0."""
    image_bytes = _make_image_bytes(width, height, fmt)
    
    result = input_fn(image_bytes, content_type)
    
    assert isinstance(result, PILImage.Image)
    assert result.mode == "RGB"
    assert result.size[0] > 0
    assert result.size[1] > 0


# Feature: model-switching-inference, Property 2: 非法 content type 被拒绝
# **Validates: Requirements 1.4**

@given(
    content_type=st.text(min_size=1, max_size=50).filter(
        lambda s: s not in {"application/x-image", "image/jpeg", "image/png"}
    ),
)
@settings(max_examples=100)
def test_invalid_content_type_rejected(content_type):
    """For any content type not in the supported set,
    input_fn should raise ValueError."""
    dummy_bytes = b"dummy"
    with pytest.raises(ValueError):
        input_fn(dummy_bytes, content_type)
