"""Integration tests for deploy_model.py.

Tests CLI argument parsing, model.tar.gz structure, and SageMaker API calls.
Requirements: 5.1, 5.2, 5.3, 5.4, 7.1, 7.2, 7.3, 7.4, 6.1, 6.2, 6.3, 6.4
"""

import json
import os
import sys
import tarfile

import pytest
from unittest.mock import patch, MagicMock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from deploy_model import parse_args, package_model, main


class TestCLIArgParsing:
    """Test argparse configuration."""

    def test_valid_dinov2_args(self):
        args = parse_args(["--model-type", "dinov2", "--model-path", "/tmp/model"])
        assert args.model_type == "dinov2"
        assert args.model_path == "/tmp/model"
        assert not args.update_endpoint
        assert not args.rollback

    def test_valid_bioclip_args(self):
        args = parse_args(["--model-type", "bioclip", "--model-path", "/tmp/model"])
        assert args.model_type == "bioclip"

    def test_update_endpoint_flag(self):
        args = parse_args(["--model-type", "dinov2", "--model-path", "/tmp/m", "--update-endpoint"])
        assert args.update_endpoint is True

    def test_rollback_flag_no_model_type_required(self):
        args = parse_args(["--rollback"])
        assert args.rollback is True
        assert args.model_type is None

    def test_missing_model_type_without_rollback_errors(self):
        with pytest.raises(SystemExit):
            parse_args(["--model-path", "/tmp/model"])

    def test_missing_model_path_without_rollback_errors(self):
        with pytest.raises(SystemExit):
            parse_args(["--model-type", "dinov2"])

    def test_invalid_model_type_errors(self):
        with pytest.raises(SystemExit):
            parse_args(["--model-type", "invalid", "--model-path", "/tmp/m"])


class TestArchiveStructure:
    """Test model.tar.gz directory structure matches spec."""

    def test_dinov2_archive_has_required_files(self, tmp_path):
        """DINOv2 archive: root has weights + model_config.json, code/ has inference."""
        model_dir = tmp_path / "model"
        model_dir.mkdir()
        (model_dir / "model.pth").write_bytes(b"fake")
        (model_dir / "class_names.json").write_text(json.dumps(["Bird_A", "Bird_B"]))

        work_dir = tmp_path / "work"
        work_dir.mkdir()

        tar_path = package_model(str(model_dir), "dinov2", str(work_dir))

        with tarfile.open(tar_path, "r:gz") as tar:
            names = tar.getnames()

        # Root level
        assert "model.pth" in names
        assert "model_config.json" in names
        assert "class_names.json" in names

        # code/ directory
        assert "code/inference.py" in names
        code_handlers = [n for n in names if n.startswith("code/handler_")]
        assert len(code_handlers) >= 1  # at least handler_dinov2.py
        assert "code/requirements.txt" in names

    def test_model_config_content_correct(self, tmp_path):
        """model_config.json in archive has correct content for DINOv2."""
        model_dir = tmp_path / "model"
        model_dir.mkdir()
        (model_dir / "model.pth").write_bytes(b"fake")
        (model_dir / "class_names.json").write_text(json.dumps(["A", "B", "C"]))

        work_dir = tmp_path / "work"
        work_dir.mkdir()

        tar_path = package_model(str(model_dir), "dinov2", str(work_dir))

        with tarfile.open(tar_path, "r:gz") as tar:
            config = json.load(tar.extractfile("model_config.json"))

        assert config["model_type"] == "dinov2"
        assert config["class_names"] == ["A", "B", "C"]
        assert config["image_size"] == 518
        assert config["confidence_threshold"] == 0.2
        assert config["top_k"] == 3


class TestMainFlow:
    """Test main() end-to-end with mocked AWS calls."""

    @patch("deploy_model.register_model_version")
    @patch("deploy_model.create_endpoint", return_value="bird-classifier-endpoint")
    @patch("deploy_model.create_sagemaker_model", return_value="bird-classifier")
    @patch("deploy_model.upload_to_s3", return_value="s3://bucket/model.tar.gz")
    @patch("deploy_model.package_model", return_value="/tmp/model.tar.gz")
    @patch("deploy_model.get_sagemaker_role_arn", return_value="arn:aws:iam::role/test")
    def test_full_deploy_flow(self, mock_role, mock_pkg, mock_upload,
                               mock_model, mock_ep, mock_reg):
        """main() calls all steps in order for a fresh deploy."""
        main(["--model-type", "dinov2", "--model-path", "/fake/path"])

        mock_role.assert_called_once()
        mock_pkg.assert_called_once()
        mock_upload.assert_called_once()
        mock_model.assert_called_once()
        mock_ep.assert_called_once()
        mock_reg.assert_called_once()

    @patch("deploy_model.register_model_version")
    @patch("deploy_model.update_endpoint", return_value="bird-classifier-config-new")
    @patch("deploy_model.create_endpoint")
    @patch("deploy_model.create_sagemaker_model", return_value="bird-classifier")
    @patch("deploy_model.upload_to_s3", return_value="s3://bucket/model.tar.gz")
    @patch("deploy_model.package_model", return_value="/tmp/model.tar.gz")
    @patch("deploy_model.get_sagemaker_role_arn", return_value="arn:aws:iam::role/test")
    def test_update_endpoint_skips_create(self, mock_role, mock_pkg, mock_upload,
                                           mock_model, mock_create, mock_update, mock_reg):
        """main() with --update-endpoint calls update_endpoint, not create_endpoint."""
        main(["--model-type", "dinov2", "--model-path", "/fake/path", "--update-endpoint"])

        mock_update.assert_called_once()
        mock_create.assert_not_called()

    @patch("deploy_model.rollback_endpoint")
    def test_rollback_skips_all_deploy_steps(self, mock_rollback):
        """main() with --rollback only calls rollback, nothing else."""
        main(["--rollback"])

        mock_rollback.assert_called_once()
