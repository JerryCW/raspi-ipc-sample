"""Tests for model.tar.gz packaging logic (task 7.2).

Validates:
- _generate_model_config produces correct config for dinov2 and bioclip
- DINOv2 reads class_names.json from model path
- DINOv2 raises FileNotFoundError when class_names.json is missing
- package_model includes model_config.json in the archive
- Archive directory structure matches spec: root has weights + model_config.json, code/ has inference code
"""

import json
import os
import tarfile
import tempfile

import pytest

import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from deploy_model import _generate_model_config, package_model


class TestGenerateModelConfig:
    """Tests for _generate_model_config."""

    def test_dinov2_config_with_class_names(self, tmp_path):
        """DINOv2 config includes class_names read from class_names.json."""
        class_names = ["Sparrow", "Robin", "Eagle"]
        class_names_file = tmp_path / "class_names.json"
        class_names_file.write_text(json.dumps(class_names))

        # Also create a dummy model file so model_path is a directory
        (tmp_path / "model.pth").write_bytes(b"fake")

        config = _generate_model_config("dinov2", str(tmp_path))

        assert config["model_type"] == "dinov2"
        assert config["class_names"] == class_names
        assert config["image_size"] == 518
        assert config["confidence_threshold"] == 0.2
        assert config["top_k"] == 3
        assert config["backbone"] == "dinov2_vitl14"
        assert config["embed_dim"] == 1024
        assert "model_name" in config

    def test_dinov2_config_from_file_path(self, tmp_path):
        """DINOv2 reads class_names.json from parent dir when model_path is a file."""
        class_names = ["Hawk", "Dove"]
        (tmp_path / "class_names.json").write_text(json.dumps(class_names))
        model_file = tmp_path / "model.pth"
        model_file.write_bytes(b"fake")

        config = _generate_model_config("dinov2", str(model_file))

        assert config["class_names"] == class_names

    def test_dinov2_missing_class_names_raises(self, tmp_path):
        """DINOv2 raises FileNotFoundError when class_names.json is missing."""
        with pytest.raises(FileNotFoundError, match="class_names.json"):
            _generate_model_config("dinov2", str(tmp_path))

    def test_bioclip_config(self):
        """BioCLIP config has correct defaults."""
        config = _generate_model_config("bioclip", "/fake/path")

        assert config["model_type"] == "bioclip"
        assert config["image_size"] == 224
        assert config["confidence_threshold"] == 0.2
        assert config["top_k"] == 3
        assert "model_name" in config


class TestPackageModelWithConfig:
    """Tests that package_model includes model_config.json in the archive."""

    def test_archive_contains_model_config(self, tmp_path):
        """model.tar.gz should contain model_config.json at root level."""
        # Setup: create model dir with weights and class_names.json
        model_dir = tmp_path / "model"
        model_dir.mkdir()
        (model_dir / "model.pth").write_bytes(b"fake weights")
        (model_dir / "class_names.json").write_text(
            json.dumps(["Bird_A", "Bird_B"])
        )

        work_dir = tmp_path / "work"
        work_dir.mkdir()

        tar_path = package_model(str(model_dir), "dinov2", str(work_dir))

        # Verify archive contents
        with tarfile.open(tar_path, "r:gz") as tar:
            names = tar.getnames()

        assert "model_config.json" in names

        # Verify model_config.json content
        with tarfile.open(tar_path, "r:gz") as tar:
            f = tar.extractfile("model_config.json")
            config = json.load(f)

        assert config["model_type"] == "dinov2"
        assert config["class_names"] == ["Bird_A", "Bird_B"]
        assert config["image_size"] == 518

    def test_archive_structure(self, tmp_path):
        """Archive has correct directory structure: root weights + config, code/ has inference."""
        model_dir = tmp_path / "model"
        model_dir.mkdir()
        (model_dir / "model.pth").write_bytes(b"fake weights")
        (model_dir / "class_names.json").write_text(
            json.dumps(["Bird_A"])
        )

        work_dir = tmp_path / "work"
        work_dir.mkdir()

        tar_path = package_model(str(model_dir), "dinov2", str(work_dir))

        with tarfile.open(tar_path, "r:gz") as tar:
            names = tar.getnames()

        # Root level: model weights + config
        assert "model.pth" in names
        assert "model_config.json" in names

        # code/ directory: inference code
        assert "code/inference.py" in names
        assert any(n.startswith("code/handler_") for n in names)
        assert "code/requirements.txt" in names

    def test_archive_single_file_model(self, tmp_path):
        """package_model works when model_path is a single file."""
        model_dir = tmp_path / "models"
        model_dir.mkdir()
        model_file = model_dir / "model.pth"
        model_file.write_bytes(b"fake weights")
        (model_dir / "class_names.json").write_text(
            json.dumps(["Bird_A"])
        )

        work_dir = tmp_path / "work"
        work_dir.mkdir()

        tar_path = package_model(str(model_file), "dinov2", str(work_dir))

        with tarfile.open(tar_path, "r:gz") as tar:
            names = tar.getnames()

        assert "model.pth" in names
        assert "model_config.json" in names
