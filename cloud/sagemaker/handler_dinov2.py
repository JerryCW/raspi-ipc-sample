"""DINOv2 handler module for SageMaker pluggable inference architecture.

Implements the handler interface (load_model, preprocess, predict) for
DINOv2-ViT-L/14 fine-tuned bird species classifier.

Model structure: DINOv2 backbone → LayerNorm(1024) → Linear(1024,512) → GELU
                 → Dropout(0.3) → Linear(512, num_classes)

Reference: cloud/sagemaker/training/test_model.py
"""

import logging
import os

import torch
import torch.nn as nn
from PIL import Image
from torchvision import transforms

logger = logging.getLogger(__name__)

# DINOv2 constants (must match training config)
DINOV2_MODEL = "dinov2_vitl14"
IMAGE_SIZE = 518
EMBED_DIM = 1024


class DINOv2Classifier(nn.Module):
    """DINOv2-ViT-L/14 backbone + classification head.

    Structure: DINOv2 backbone → LayerNorm(1024) → Linear(1024,512)
               → GELU → Dropout(0.3) → Linear(512, num_classes)
    """

    def __init__(self, num_classes, freeze_backbone=True):
        super().__init__()
        self.backbone = torch.hub.load("facebookresearch/dinov2", DINOV2_MODEL)
        self.freeze_backbone = freeze_backbone
        if freeze_backbone:
            for param in self.backbone.parameters():
                param.requires_grad = False
            self.backbone.eval()
        self.head = nn.Sequential(
            nn.LayerNorm(EMBED_DIM),
            nn.Linear(EMBED_DIM, 512),
            nn.GELU(),
            nn.Dropout(0.3),
            nn.Linear(512, num_classes),
        )

    def forward(self, x):
        with torch.no_grad():
            features = self.backbone(x)
        return self.head(features)


def load_model(model_dir: str, config: dict) -> dict:
    """Load DINOv2 model weights and return model dict.

    Supports two checkpoint formats:
    - model_state_dict: full model (backbone + head) weights
    - head_state_dict: head-only weights (backbone loaded fresh from hub)

    Args:
        model_dir: SageMaker model directory containing model.pth.
        config: Parsed model_config.json dict with class_names.

    Returns:
        {"model": DINOv2Classifier, "class_names": list, "device": torch.device}
    """
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    class_names = config["class_names"]
    num_classes = len(class_names)

    logger.info("Loading DINOv2 model with %d classes on %s", num_classes, device)

    model = DINOv2Classifier(num_classes)

    checkpoint_path = os.path.join(model_dir, "model.pth")
    checkpoint = torch.load(checkpoint_path, map_location=device, weights_only=False)

    if "model_state_dict" in checkpoint:
        # Full model weights (backbone + head)
        model.load_state_dict(checkpoint["model_state_dict"])
        logger.info("Loaded full model state dict")
    elif "head_state_dict" in checkpoint:
        # Head-only weights (backbone already loaded from hub)
        model.head.load_state_dict(checkpoint["head_state_dict"])
        logger.info("Loaded head-only state dict")
    else:
        raise ValueError(
            f"Checkpoint at {checkpoint_path} must contain "
            "'model_state_dict' or 'head_state_dict'"
        )

    model.to(device)
    model.eval()

    return {
        "model": model,
        "class_names": class_names,
        "device": device,
    }


# Preprocessing pipeline matching training val transform
_preprocess_transform = transforms.Compose([
    transforms.Resize(IMAGE_SIZE + 32),   # 550
    transforms.CenterCrop(IMAGE_SIZE),     # 518
    transforms.ToTensor(),
    transforms.Normalize(
        mean=[0.485, 0.456, 0.406],
        std=[0.229, 0.224, 0.225],
    ),
])


def preprocess(image: Image.Image, config: dict) -> torch.Tensor:
    """Preprocess PIL Image for DINOv2 inference.

    Pipeline: Resize(550) → CenterCrop(518) → ToTensor → Normalize(ImageNet)

    Args:
        image: RGB PIL Image.
        config: Model config (unused for DINOv2, kept for interface).

    Returns:
        Tensor of shape (1, 3, 518, 518).
    """
    tensor = _preprocess_transform(image)
    return tensor.unsqueeze(0)


def predict(model_dict: dict, tensor: torch.Tensor, config: dict) -> list:
    """Run DINOv2 inference and return predictions.

    Forward pass → softmax → top_k filter → confidence_threshold check.
    If max confidence < threshold, returns not_a_bird.

    Args:
        model_dict: Dict from load_model with model, class_names, device.
        tensor: Preprocessed input tensor (1, 3, 518, 518).
        config: Model config with confidence_threshold and top_k.

    Returns:
        List of {"species": str, "confidence": float} sorted by confidence
        descending. Returns [{"species": "not_a_bird", "confidence": 0.0}]
        if below threshold.
    """
    model = model_dict["model"]
    class_names = model_dict["class_names"]
    device = model_dict["device"]

    confidence_threshold = config.get("confidence_threshold", 0.2)
    top_k = config.get("top_k", 3)

    tensor = tensor.to(device)

    with torch.no_grad():
        logits = model(tensor)
        probs = torch.nn.functional.softmax(logits, dim=1)

    # Get top_k predictions
    k = min(top_k, probs.shape[1])
    top_probs, top_indices = torch.topk(probs, k=k, dim=1)

    max_confidence = top_probs[0][0].item()

    # Below threshold → not a bird
    if max_confidence < confidence_threshold:
        logger.info(
            "Below threshold: max_conf=%.4f < threshold=%.4f → not_a_bird",
            max_confidence, confidence_threshold,
        )
        return [{"species": "not_a_bird", "confidence": 0.0}]

    # Build predictions list sorted by confidence descending
    predictions = []
    for prob, idx in zip(top_probs[0], top_indices[0]):
        predictions.append({
            "species": class_names[idx.item()],
            "confidence": round(prob.item(), 4),
        })

    logger.info(
        "Prediction: top-1=%s (conf=%.4f)",
        predictions[0]["species"],
        predictions[0]["confidence"],
    )

    return predictions
