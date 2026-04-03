"""SageMaker custom inference handler for BioCLIP bird species classifier.

Implements the four SageMaker inference functions:
- model_fn: Load BioCLIP model, tokenizer, and pre-compute text embeddings
- input_fn: Decode JPEG/PNG bytes to PIL Image (RGB)
- predict_fn: Zero-shot CLIP inference with OOD detection
- output_fn: Format output as JSON

Model: imageomics/bioclip (ViT-B/16 CLIP, 599MB)
Uses open_clip for zero-shot inference via cosine similarity between
image embeddings and pre-cached species text embeddings.
"""

import io
import json
import logging
import os

import torch
from PIL import Image

logger = logging.getLogger(__name__)

# OOD (Out-of-Distribution) detection threshold based on cosine similarity.
# For in-distribution bird images, max cosine similarity is typically > 0.25.
# For non-bird inputs (person, random noise), max similarity is usually < 0.2.
# Configurable via OOD_THRESHOLD environment variable.
OOD_SIMILARITY_THRESHOLD = float(os.environ.get("OOD_THRESHOLD", "0.18"))


def model_fn(model_dir: str) -> dict:
    """Load BioCLIP model, preprocess, tokenizer, and pre-compute text embeddings.

    Loads the BioCLIP model via open_clip, reads the species list from
    bird_labels.json, and pre-computes normalized text embeddings for all
    species using the prompt template.

    Returns:
        {
            "model": open_clip CLIP model (eval mode),
            "preprocess": image transform pipeline,
            "tokenizer": open_clip tokenizer,
            "text_features": Tensor[num_species, embed_dim] (normalized),
            "label_list": list[str],
            "device": torch.device,
        }
    """
    import open_clip

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    # Load BioCLIP model and image preprocess transform
    model, _, preprocess = open_clip.create_model_and_transforms(
        "hf-hub:imageomics/bioclip"
    )
    model.to(device)
    model.eval()

    # Load tokenizer
    tokenizer = open_clip.get_tokenizer("hf-hub:imageomics/bioclip")

    # Read bird_labels.json — try model_dir first, then script directory
    labels_path = os.path.join(model_dir, "bird_labels.json")
    if not os.path.exists(labels_path):
        labels_path = os.path.join(os.path.dirname(__file__), "bird_labels.json")
    with open(labels_path, "r") as f:
        labels_data = json.load(f)

    species_list = labels_data["species"]
    prompt_template = labels_data.get(
        "prompt_template", "a photo of a {species}, a type of bird"
    )

    # Negative classes: non-bird categories for active OOD rejection
    negative_classes = labels_data.get("negative_classes", [])
    negative_prompt_template = labels_data.get(
        "negative_prompt_template", "a photo of a {class_name}"
    )
    num_bird_species = len(species_list)
    logger.info("Loaded %d bird species + %d negative classes", num_bird_species, len(negative_classes))

    # Build combined label list: bird species first, then negative classes
    all_labels = species_list + [f"NOT_BIRD:{nc}" for nc in negative_classes]

    # Pre-compute normalized text embeddings for all labels
    bird_prompts = [prompt_template.format(species=s) for s in species_list]
    neg_prompts = [negative_prompt_template.format(class_name=nc) for nc in negative_classes]
    all_prompts = bird_prompts + neg_prompts

    tokens = tokenizer(all_prompts).to(device)
    with torch.no_grad():
        text_features = model.encode_text(tokens)
        text_features = text_features / text_features.norm(dim=-1, keepdim=True)

    logger.info("Model loaded on %s, text embeddings cached (%d total)", device, len(all_labels))
    return {
        "model": model,
        "preprocess": preprocess,
        "tokenizer": tokenizer,
        "text_features": text_features,
        "label_list": all_labels,
        "num_bird_species": num_bird_species,
        "device": device,
    }


def input_fn(request_body: bytes, content_type: str = "application/x-image") -> Image.Image:
    """Decode JPEG/PNG bytes to PIL Image.

    Args:
        request_body: Raw image bytes.
        content_type: MIME type, must be 'application/x-image', 'image/jpeg', or 'image/png'.

    Returns:
        PIL Image in RGB mode.
    """
    supported_types = {"application/x-image", "image/jpeg", "image/png"}
    if content_type not in supported_types:
        raise ValueError(
            f"Unsupported content type: {content_type}. "
            f"Supported: {supported_types}"
        )
    return Image.open(io.BytesIO(request_body)).convert("RGB")


def predict_fn(input_data: Image.Image, model_dict: dict) -> list[dict]:
    """Zero-shot CLIP inference with OOD detection.

    Computes cosine similarity between the image embedding and all pre-cached
    species text embeddings. If max similarity is below OOD_SIMILARITY_THRESHOLD,
    returns not_a_bird. Otherwise returns top-3 predictions with softmax
    confidence (cosine_similarity * 100 → softmax).

    Returns:
        Normal: [{"species": str, "confidence": float, "similarity_score": float}, ...] (top-3)
        OOD:    [{"species": "not_a_bird", "confidence": 0.0, "similarity_score": float}]
    """
    model = model_dict["model"]
    preprocess = model_dict["preprocess"]
    text_features = model_dict["text_features"]
    label_list = model_dict["label_list"]
    num_bird_species = model_dict["num_bird_species"]
    device = model_dict["device"]

    # Preprocess image using BioCLIP transform
    tensor = preprocess(input_data).unsqueeze(0).to(device)

    with torch.no_grad():
        # Encode image and normalize
        image_features = model.encode_image(tensor)
        image_features = image_features / image_features.norm(dim=-1, keepdim=True)

        # Cosine similarity between image and all text embeddings
        similarities = (image_features @ text_features.T).squeeze(0)

        max_sim = similarities.max().item()

        # OOD detection (threshold-based fallback)
        if max_sim < OOD_SIMILARITY_THRESHOLD:
            logger.info(
                "OOD detected: max_similarity=%.4f < threshold=%.4f — not a bird",
                max_sim, OOD_SIMILARITY_THRESHOLD,
            )
            return [{
                "species": "not_a_bird",
                "confidence": 0.0,
                "similarity_score": round(max_sim, 4),
            }]

        # Negative class rejection: if top-1 overall is a negative class,
        # the image is not a bird (e.g. person, cat, dog)
        top1_idx = similarities.argmax().item()
        if top1_idx >= num_bird_species:
            neg_label = label_list[top1_idx]  # e.g. "NOT_BIRD:person"
            logger.info(
                "Negative class detected: %s (similarity=%.4f) — not a bird",
                neg_label, similarities[top1_idx].item(),
            )
            return [{
                "species": "not_a_bird",
                "confidence": 0.0,
                "similarity_score": round(similarities[top1_idx].item(), 4),
            }]

        # Scale bird-only similarities and apply softmax for confidence scores
        bird_similarities = similarities[:num_bird_species]
        scaled = bird_similarities * 100.0
        probabilities = torch.nn.functional.softmax(scaled, dim=0)

    # Top-3 from bird species only
    top3_probs, top3_indices = torch.topk(
        probabilities, k=min(3, probabilities.shape[0])
    )

    predictions = []
    for prob, idx in zip(top3_probs, top3_indices):
        species = label_list[idx.item()]
        sim_score = similarities[idx.item()].item()
        predictions.append({
            "species": species,
            "confidence": round(prob.item(), 4),
            "similarity_score": round(sim_score, 4),
        })

    logger.info(
        "Prediction: top-1=%s (conf=%.4f, sim=%.4f), top-2=%s (conf=%.4f), top-3=%s (conf=%.4f)",
        predictions[0]["species"] if len(predictions) > 0 else "N/A",
        predictions[0]["confidence"] if len(predictions) > 0 else 0,
        predictions[0]["similarity_score"] if len(predictions) > 0 else 0,
        predictions[1]["species"] if len(predictions) > 1 else "N/A",
        predictions[1]["confidence"] if len(predictions) > 1 else 0,
        predictions[2]["species"] if len(predictions) > 2 else "N/A",
        predictions[2]["confidence"] if len(predictions) > 2 else 0,
    )

    return predictions


def output_fn(prediction: list[dict], accept: str = "application/json") -> str:
    """Format predictions as JSON."""
    if accept != "application/json":
        raise ValueError(f"Unsupported accept type: {accept}. Supported: application/json")
    return json.dumps({"predictions": prediction})
