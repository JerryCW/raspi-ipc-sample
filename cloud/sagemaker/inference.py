"""SageMaker pluggable inference handler for bird species classification.

Implements the four SageMaker inference functions with a pluggable handler
architecture. The model type is determined by model_config.json, and the
corresponding handler_<type>.py module is dynamically loaded.

Supported handlers: handler_dinov2.py (DINOv2 fine-tuned classifier)
"""

import importlib
import io
import json
import logging
import os

from PIL import Image

logger = logging.getLogger(__name__)

# --- Config validation for pluggable model architecture ---

REQUIRED_FIELDS = {"model_type", "model_name", "class_names"}
REQUIRED_HANDLER_FUNCS = {"load_model", "preprocess", "predict"}

CONFIG_DEFAULTS = {
    "image_size": 224,
    "confidence_threshold": 0.2,
    "top_k": 3,
}


def _validate_config(config: dict) -> None:
    """Validate model_config.json required fields."""
    missing = REQUIRED_FIELDS - set(config.keys())
    if missing:
        raise ValueError(f"model_config.json 缺少必填字段: {missing}")


def _load_config(model_dir: str) -> dict:
    """Load model_config.json from model_dir, validate, and fill defaults."""
    config_path = os.path.join(model_dir, "model_config.json")
    if not os.path.exists(config_path):
        raise FileNotFoundError(f"model_config.json not found in {model_dir}")

    with open(config_path, "r") as f:
        config = json.load(f)

    _validate_config(config)

    for key, default_value in CONFIG_DEFAULTS.items():
        config.setdefault(key, default_value)

    return config


def _load_handler(model_type: str):
    """Dynamically load a handler module by model_type."""
    module_name = f"handler_{model_type}"
    try:
        handler = importlib.import_module(module_name)
    except ModuleNotFoundError:
        raise ImportError(
            f"No handler module found for model_type '{model_type}': "
            f"cannot import '{module_name}'"
        )

    missing_funcs = {
        fn for fn in REQUIRED_HANDLER_FUNCS if not hasattr(handler, fn)
    }
    if missing_funcs:
        raise AttributeError(
            f"Handler module '{module_name}' is missing required functions: {missing_funcs}"
        )

    return handler


# --- SageMaker inference functions ---


def model_fn(model_dir: str) -> dict:
    """Load model via pluggable handler architecture.

    Reads model_config.json, dynamically loads the handler module,
    calls handler.load_model(), and returns model_dict with handler
    reference and config.
    """
    config = _load_config(model_dir)
    model_type = config["model_type"]

    logger.info("Loading model type '%s' from %s", model_type, model_dir)

    handler = _load_handler(model_type)
    model_dict = handler.load_model(model_dir, config)

    # Store handler and config in model_dict for predict_fn
    model_dict["_handler"] = handler
    model_dict["_config"] = config

    logger.info("Model '%s' loaded successfully", config["model_name"])
    return model_dict


def input_fn(request_body: bytes, content_type: str = "application/x-image") -> Image.Image:
    """Decode JPEG/PNG bytes to PIL Image (model-agnostic)."""
    supported_types = {"application/x-image", "image/jpeg", "image/png"}
    if content_type not in supported_types:
        raise ValueError(
            f"Unsupported content type: {content_type}. "
            f"Supported: {supported_types}"
        )
    return Image.open(io.BytesIO(request_body)).convert("RGB")


def predict_fn(input_data: Image.Image, model_dict: dict) -> list[dict]:
    """Run inference via handler's preprocess + predict."""
    handler = model_dict["_handler"]
    config = model_dict["_config"]

    tensor = handler.preprocess(input_data, config)
    predictions = handler.predict(model_dict, tensor, config)

    return predictions


def output_fn(prediction: list[dict], accept: str = "application/json") -> str:
    """Format predictions as JSON (model-agnostic)."""
    if accept != "application/json":
        raise ValueError(f"Unsupported accept type: {accept}. Supported: application/json")
    return json.dumps({"predictions": prediction})
