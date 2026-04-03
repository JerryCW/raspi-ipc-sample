"""Bug condition exploration test for SageMaker bird classifier.

Property 1: Bug Condition — random-weight model produces identical output for different inputs.

This test MUST be written and run BEFORE implementing the fix.
It MUST FAIL on unfixed code — failure proves the bug exists.
After the fix, this test should PASS, confirming expected behavior.

Scoped PBT approach: since the bug is deterministic (random weights produce
identical output for all inputs), we scope the property to concrete failure
cases: bird-sample.jpg and person-sample.jpg.
"""

import os
import sys

import pytest
from hypothesis import given, settings, HealthCheck
from hypothesis import strategies as st
from PIL import Image

# Ensure the sagemaker module is importable
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from inference import model_fn, input_fn, predict_fn

# Path to the HuggingFace model cache
MODEL_DIR = os.path.join(
    os.path.expanduser("~"),
    ".cache/huggingface/hub/models--dennisjooo--Birds-Classifier-EfficientNetB2/snapshots",
)

# Resolve the actual snapshot directory
def _get_model_dir():
    snapshots = os.listdir(MODEL_DIR)
    snapshots = [s for s in snapshots if not s.startswith(".")]
    assert len(snapshots) >= 1, f"No snapshots found in {MODEL_DIR}"
    return os.path.join(MODEL_DIR, snapshots[0])


# Project root for sample images
PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
BIRD_SAMPLE = os.path.join(PROJECT_ROOT, "bird-sample.jpg")
PERSON_SAMPLE = os.path.join(PROJECT_ROOT, "person-sample.jpg")


@pytest.fixture(scope="module")
def model_dict():
    """Load the model once for all tests in this module."""
    model_dir = _get_model_dir()
    return model_fn(model_dir)


class TestBugConditionExploration:
    """Exploration tests to prove the bug exists on unfixed code."""

    def test_bird_image_high_confidence(self, model_dict):
        """Bird image should produce a meaningful prediction (not identical to all others).

        Expected behavior (design doc): model produces different predictions
        for different inputs. With correct weights, the model should produce
        a prediction with some confidence for a bird image.
        On buggy code: FAILS because random weights produce identical low
        confidence for all inputs.
        """
        with open(BIRD_SAMPLE, "rb") as f:
            image_bytes = f.read()
        image = input_fn(image_bytes, "application/x-image")
        predictions = predict_fn(image, model_dict)

        assert len(predictions) >= 1, "Should return at least 1 prediction"
        top = predictions[0]

        # The model should produce a meaningful prediction with max_logit field
        assert "max_logit" in top, (
            f"Prediction should contain 'max_logit' field, got keys: {list(top.keys())}"
        )
        assert isinstance(top["max_logit"], float), "max_logit should be a float"
        assert isinstance(top["confidence"], float), "confidence should be a float"

        # With correct weights, the bird image should NOT produce the same
        # fixed output as the buggy code (NORTHERN GANNET, confidence=0.0675)
        # Either it's a real bird prediction or OOD detection
        if top["species"] == "not_a_bird":
            # OOD detection triggered — this is valid behavior for the model
            assert top["max_logit"] < 8.0, "OOD should have max_logit < 8.0"
        else:
            # Bird prediction — should have reasonable confidence
            assert top["confidence"] > 0.01, (
                f"Bird prediction should have confidence > 0.01, "
                f"got {top['confidence']} for '{top['species']}'"
            )

    def test_person_image_ood_detection(self, model_dict):
        """Person image should produce a different prediction than bird image.

        Expected behavior (design doc): non-bird images should ideally trigger
        OOD detection. At minimum, the model should produce DIFFERENT predictions
        for bird vs person images (proving weights are loaded correctly).
        On buggy code: FAILS because random weights produce identical output
        for all inputs, and no max_logit/OOD detection exists.
        """
        with open(PERSON_SAMPLE, "rb") as f:
            image_bytes = f.read()
        image = input_fn(image_bytes, "application/x-image")
        predictions = predict_fn(image, model_dict)

        assert len(predictions) >= 1, "Should return at least 1 prediction"
        top = predictions[0]

        # The fixed code must have max_logit field (OOD detection support)
        assert "max_logit" in top, (
            f"Prediction should contain 'max_logit' field for OOD detection, "
            f"got keys: {list(top.keys())}"
        )

        # Get bird prediction for comparison
        with open(BIRD_SAMPLE, "rb") as f:
            bird_bytes = f.read()
        bird_image = input_fn(bird_bytes, "application/x-image")
        bird_preds = predict_fn(bird_image, model_dict)

        # The key property: person and bird predictions must differ
        # (buggy code returns identical output for all inputs)
        assert (
            top["species"] != bird_preds[0]["species"]
            or abs(top["max_logit"] - bird_preds[0].get("max_logit", 0)) > 0.1
        ), (
            f"Person and bird images produced identical predictions: "
            f"person={top}, bird={bird_preds[0]}. "
            f"This indicates model weights are not properly loaded."
        )

    def test_different_inputs_produce_different_outputs(self, model_dict):
        """Bird and person images should produce different predictions.

        On buggy code: FAILS because random weights produce identical output
        for all inputs (deterministic behavior of random weights).
        """
        with open(BIRD_SAMPLE, "rb") as f:
            bird_bytes = f.read()
        with open(PERSON_SAMPLE, "rb") as f:
            person_bytes = f.read()

        bird_image = input_fn(bird_bytes, "application/x-image")
        person_image = input_fn(person_bytes, "application/x-image")

        bird_preds = predict_fn(bird_image, model_dict)
        person_preds = predict_fn(person_image, model_dict)

        # The predictions should differ for different input categories
        bird_species = bird_preds[0]["species"]
        person_species = person_preds[0]["species"]

        assert bird_species != person_species or (
            bird_preds[0]["confidence"] != person_preds[0]["confidence"]
        ), (
            f"Bird and person images produced identical predictions: "
            f"bird={bird_preds[0]}, person={person_preds[0]}. "
            f"This indicates the model weights are not properly loaded."
        )

    @given(
        width=st.integers(min_value=32, max_value=300),
        height=st.integers(min_value=32, max_value=300),
    )
    @settings(
        max_examples=5,
        suppress_health_check=[HealthCheck.too_slow],
        deadline=None,
    )
    def test_random_images_produce_varied_output(self, model_dict, width, height):
        """Random images of different sizes should not all produce the same output.

        Uses Hypothesis to generate random image dimensions. Combined with
        the bird-sample.jpg baseline, we check that at least some variation
        exists in model output.
        """
        import numpy as np

        # Generate a random RGB image
        rng = np.random.RandomState(seed=width * 1000 + height)
        arr = rng.randint(0, 256, (height, width, 3), dtype=np.uint8)
        random_image = Image.fromarray(arr, "RGB")

        random_preds = predict_fn(random_image, model_dict)

        # Load bird baseline
        with open(BIRD_SAMPLE, "rb") as f:
            bird_bytes = f.read()
        bird_image = input_fn(bird_bytes, "application/x-image")
        bird_preds = predict_fn(bird_image, model_dict)

        # At least the confidence values should differ for random noise vs real bird
        random_conf = random_preds[0]["confidence"]
        bird_conf = bird_preds[0]["confidence"]
        random_species = random_preds[0]["species"]
        bird_species = bird_preds[0]["species"]

        # If both species AND confidence are identical, the model is broken
        assert not (random_species == bird_species and abs(random_conf - bird_conf) < 0.001), (
            f"Random {width}x{height} image produced same output as bird image: "
            f"species='{random_species}', confidence={random_conf:.4f}. "
            f"Model weights are likely random/not loaded."
        )
