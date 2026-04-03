"""SageMaker custom inference handler for Birds-Classifier-EfficientNetB2.

Implements the four SageMaker inference functions:
- model_fn: Load EfficientNetB2 model and label mapping
- input_fn: Decode JPEG bytes to preprocessed tensor
- predict_fn: Run inference, return top-3 predictions
- output_fn: Format output as JSON

Model: dennisjooo/Birds-Classifier-EfficientNetB2 (525 bird species)
"""

import io
import json
import logging
import os

import timm
import torch
from PIL import Image
from torchvision import transforms

logger = logging.getLogger(__name__)

# EfficientNetB2 preprocessing constants matching the HuggingFace model config.
# The original model uses 260x260 resize, no center crop, and specific normalization.
INPUT_SIZE = (3, 260, 260)
IMAGE_MEAN = [0.485, 0.456, 0.406]
IMAGE_STD = [0.47853944, 0.4732864, 0.47434163]

# OOD (Out-of-Distribution) detection threshold.
# For in-distribution bird images, the max logit before softmax is typically >10.
# For non-bird inputs (person, cat, dog, background), max logit is usually <8.
# Inputs below this threshold are rejected as "not_a_bird".
OOD_LOGIT_THRESHOLD = 8.0


def model_fn(model_dir: str) -> dict:
    """Load EfficientNetB2 model and label mapping from model_dir.

    The model_dir contains:
    - model.safetensors or pytorch_model.bin: model weights
    - config.json: HuggingFace config with id2label mapping

    Returns a dict with 'model', 'labels', and 'transform' keys.
    """
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    # Load label mapping from HuggingFace config.json
    config_path = os.path.join(model_dir, "config.json")
    with open(config_path, "r") as f:
        config = json.load(f)

    id2label = config.get("id2label", {})
    # Ensure keys are ints for index lookup
    labels = {int(k): v for k, v in id2label.items()}
    num_classes = len(labels)
    logger.info("Loaded %d bird species labels", num_classes)

    # Create EfficientNetB2 with matching number of classes
    model = timm.create_model(
        "efficientnet_b2",
        pretrained=False,
        num_classes=num_classes,
    )

    # Load weights — try safetensors first, then pytorch_model.bin
    safetensors_path = os.path.join(model_dir, "model.safetensors")
    pytorch_path = os.path.join(model_dir, "pytorch_model.bin")

    if os.path.exists(safetensors_path):
        from safetensors.torch import load_file

        state_dict = load_file(safetensors_path)
        logger.info("Loading weights from model.safetensors")
    elif os.path.exists(pytorch_path):
        state_dict = torch.load(pytorch_path, map_location=device)
        logger.info("Loading weights from pytorch_model.bin")
    else:
        raise FileNotFoundError(
            f"No model weights found in {model_dir}. "
            "Expected model.safetensors or pytorch_model.bin"
        )

    # The HuggingFace EfficientNet state dict keys have a different prefix
    # than timm. Remap: "efficientnet.{layer}" -> "{layer}" and
    # "classifier.{layer}" -> "classifier.{layer}"
    remapped = _remap_state_dict(state_dict, model)
    model.load_state_dict(remapped, strict=False)

    model.to(device)
    model.eval()

    # Build preprocessing transform matching the original model config
    transform = transforms.Compose([
        transforms.Resize((INPUT_SIZE[1], INPUT_SIZE[2]), interpolation=transforms.InterpolationMode.NEAREST),
        transforms.ToTensor(),
        transforms.Normalize(mean=IMAGE_MEAN, std=IMAGE_STD),
    ])

    logger.info("Model loaded on %s", device)
    return {"model": model, "labels": labels, "transform": transform, "device": device}


def _remap_state_dict(hf_state_dict: dict, timm_model: torch.nn.Module) -> dict:
    """Remap HuggingFace EfficientNet state dict keys to timm format.

    HuggingFace keys look like: efficientnet.encoder.blocks.0.0.project_conv.weight
    timm keys look like:        blocks.0.0.project_conv.weight

    Falls back to loading as-is if keys already match (e.g. if model was
    saved in timm format).
    """
    timm_keys = set(timm_model.state_dict().keys())

    # Check if keys already match timm format
    if len(set(hf_state_dict.keys()) & timm_keys) > len(timm_keys) * 0.5:
        return hf_state_dict

    remapped = {}
    for key, value in hf_state_dict.items():
        new_key = key
        # Strip HuggingFace "efficientnet." prefix
        if new_key.startswith("efficientnet."):
            new_key = new_key[len("efficientnet."):]
        # Map HuggingFace encoder to timm blocks
        if new_key.startswith("encoder."):
            new_key = new_key[len("encoder."):]

        # Map stem layers
        new_key = new_key.replace("embeddings.convolution", "conv_stem")
        new_key = new_key.replace("embeddings.batchnorm", "bn1")

        # Map classifier head
        new_key = new_key.replace("classifier.dense", "classifier")
        new_key = new_key.replace("classifier.weight", "classifier.weight")
        new_key = new_key.replace("classifier.bias", "classifier.bias")

        # Map top conv/bn (before classifier)
        new_key = new_key.replace("top_conv", "conv_head")
        new_key = new_key.replace("top_bn", "bn2")

        remapped[new_key] = value

    return remapped


def input_fn(request_body: bytes, content_type: str = "application/x-image") -> torch.Tensor:
    """Decode JPEG bytes to preprocessed tensor.

    Args:
        request_body: Raw JPEG image bytes.
        content_type: MIME type, must be 'application/x-image' or 'image/jpeg'.

    Returns:
        Preprocessed tensor of shape (1, 3, 260, 260).
    """
    supported_types = {"application/x-image", "image/jpeg", "image/png"}
    if content_type not in supported_types:
        raise ValueError(
            f"Unsupported content type: {content_type}. "
            f"Supported: {supported_types}"
        )

    image = Image.open(io.BytesIO(request_body)).convert("RGB")
    return image


def predict_fn(input_data: Image.Image, model_dict: dict) -> list[dict]:
    """Execute inference and return top-3 predictions.

    Args:
        input_data: PIL Image from input_fn.
        model_dict: Dict from model_fn with 'model', 'labels', 'transform', 'device'.

    Returns:
        List of top-3 dicts: [{"species": str, "confidence": float}, ...]
    """
    model = model_dict["model"]
    labels = model_dict["labels"]
    transform = model_dict["transform"]
    device = model_dict["device"]

    # Preprocess
    tensor = transform(input_data).unsqueeze(0).to(device)

    # Inference
    with torch.no_grad():
        logits = model(tensor)

        # OOD detection: check max logit before softmax.
        # Bird images produce high max logits (>10); non-bird inputs (person,
        # cat, dog, background) produce low, spread-out logits (<8).
        max_logit = logits.max().item()
        if max_logit < OOD_LOGIT_THRESHOLD:
            logger.info(
                "OOD detected: max_logit=%.2f < threshold=%.1f — not a bird",
                max_logit, OOD_LOGIT_THRESHOLD,
            )
            return [{
                "species": "not_a_bird",
                "confidence": 0.0,
                "max_logit": round(max_logit, 4),
            }]

        probabilities = torch.nn.functional.softmax(logits, dim=1)

    # Top-3
    top3_probs, top3_indices = torch.topk(probabilities, k=min(3, probabilities.shape[1]), dim=1)

    predictions = []
    for prob, idx in zip(top3_probs[0], top3_indices[0]):
        species = labels.get(idx.item(), f"unknown_{idx.item()}")
        predictions.append({
            "species": species,
            "confidence": round(prob.item(), 4),
        })

    logger.info(
        "Prediction: top-1=%s (%.4f), top-2=%s (%.4f), top-3=%s (%.4f), max_logit=%.2f",
        predictions[0]["species"] if len(predictions) > 0 else "N/A",
        predictions[0]["confidence"] if len(predictions) > 0 else 0,
        predictions[1]["species"] if len(predictions) > 1 else "N/A",
        predictions[1]["confidence"] if len(predictions) > 1 else 0,
        predictions[2]["species"] if len(predictions) > 2 else "N/A",
        predictions[2]["confidence"] if len(predictions) > 2 else 0,
        max_logit,
    )

    # Include max_logit in response for debugging/calibration
    for p in predictions:
        p["max_logit"] = round(max_logit, 4)

    return predictions


def output_fn(prediction: list[dict], accept: str = "application/json") -> str:
    """Format predictions as JSON.

    Args:
        prediction: List of prediction dicts from predict_fn.
        accept: Response MIME type, must be 'application/json'.

    Returns:
        JSON string: {"predictions": [{"species": "...", "confidence": 0.87}, ...]}
    """
    if accept != "application/json":
        raise ValueError(f"Unsupported accept type: {accept}. Supported: application/json")

    return json.dumps({"predictions": prediction})
