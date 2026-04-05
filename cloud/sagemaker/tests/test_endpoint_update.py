"""Tests for endpoint update and rollback logic (task 7.3).

Validates:
- update_endpoint creates new EndpointConfig with timestamp suffix
- update_endpoint logs current config for rollback reference
- update_endpoint calls UpdateEndpoint API
- rollback_endpoint finds previous config and rolls back
- rollback_endpoint returns empty string when no previous config exists

Requirements: 6.1, 6.2, 6.3, 6.4
"""

import datetime
import os
import sys
from unittest.mock import MagicMock, patch, call

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from deploy_model import (
    update_endpoint,
    rollback_endpoint,
    ENDPOINT_NAME,
    ENDPOINT_CONFIG_NAME,
    MODEL_NAME,
    SERVERLESS_MEMORY_MB,
    SERVERLESS_MAX_CONCURRENCY,
)


@pytest.fixture
def mock_sm_client():
    """Create a mock SageMaker client with common setup."""
    mock_client = MagicMock()
    mock_waiter = MagicMock()
    mock_client.get_waiter.return_value = mock_waiter
    return mock_client


class TestUpdateEndpoint:
    """Tests for update_endpoint function."""

    def test_creates_new_config_with_timestamp(self, mock_sm_client):
        """update_endpoint creates a new EndpointConfig with timestamp suffix."""
        mock_sm_client.describe_endpoint.return_value = {
            "EndpointConfigName": "bird-classifier-config-old"
        }

        with patch("deploy_model.sm_client", mock_sm_client):
            result = update_endpoint("arn:aws:iam::role/test")

        # Result should start with the config name prefix and have a timestamp suffix
        assert result.startswith(f"{ENDPOINT_CONFIG_NAME}-")
        # Timestamp format: YYYYMMDD-HHMMSS (15 chars)
        timestamp_part = result[len(ENDPOINT_CONFIG_NAME) + 1:]
        assert len(timestamp_part) == 15  # "20250115-103045"

        # Verify create_endpoint_config was called with the returned name
        create_call = mock_sm_client.create_endpoint_config.call_args
        assert create_call.kwargs["EndpointConfigName"] == result
        assert create_call.kwargs["ProductionVariants"][0]["ModelName"] == MODEL_NAME
        assert create_call.kwargs["ProductionVariants"][0]["ServerlessConfig"]["MemorySizeInMB"] == SERVERLESS_MEMORY_MB

    def test_logs_current_config_for_rollback(self, mock_sm_client, capsys):
        """update_endpoint logs the current EndpointConfig name for rollback reference."""
        mock_sm_client.describe_endpoint.return_value = {
            "EndpointConfigName": "bird-classifier-config-20250101-000000"
        }

        with patch("deploy_model.sm_client", mock_sm_client):
            update_endpoint("arn:aws:iam::role/test")

        captured = capsys.readouterr()
        assert "bird-classifier-config-20250101-000000" in captured.out
        assert "rollback" in captured.out.lower()

    def test_calls_update_endpoint_api(self, mock_sm_client):
        """update_endpoint calls UpdateEndpoint with the new config name."""
        mock_sm_client.describe_endpoint.return_value = {
            "EndpointConfigName": "old-config"
        }

        with patch("deploy_model.sm_client", mock_sm_client):
            result = update_endpoint("arn:aws:iam::role/test")

        # Verify update_endpoint was called
        mock_sm_client.update_endpoint.assert_called_once()
        update_call = mock_sm_client.update_endpoint.call_args
        assert update_call.kwargs["EndpointName"] == ENDPOINT_NAME
        assert update_call.kwargs["EndpointConfigName"] == result

    def test_waits_for_in_service(self, mock_sm_client):
        """update_endpoint waits for endpoint to become InService."""
        mock_sm_client.describe_endpoint.return_value = {
            "EndpointConfigName": "old-config"
        }

        with patch("deploy_model.sm_client", mock_sm_client):
            update_endpoint("arn:aws:iam::role/test")

        mock_sm_client.get_waiter.assert_called_with("endpoint_in_service")
        mock_sm_client.get_waiter.return_value.wait.assert_called_once()


class TestRollbackEndpoint:
    """Tests for rollback_endpoint function."""

    def test_rolls_back_to_previous_config(self, mock_sm_client):
        """rollback_endpoint finds previous config and updates endpoint."""
        mock_sm_client.describe_endpoint.return_value = {
            "EndpointConfigName": "bird-classifier-config-20250115-103045"
        }
        mock_sm_client.list_endpoint_configs.return_value = {
            "EndpointConfigs": [
                {"EndpointConfigName": "bird-classifier-config-20250115-103045"},
                {"EndpointConfigName": "bird-classifier-config-20250114-090000"},
                {"EndpointConfigName": "bird-classifier-config"},
            ]
        }

        with patch("deploy_model.sm_client", mock_sm_client):
            result = rollback_endpoint()

        assert result == "bird-classifier-config-20250114-090000"
        mock_sm_client.update_endpoint.assert_called_once_with(
            EndpointName=ENDPOINT_NAME,
            EndpointConfigName="bird-classifier-config-20250114-090000",
        )

    def test_returns_empty_when_no_previous_config(self, mock_sm_client, capsys):
        """rollback_endpoint returns empty string when no previous config exists."""
        mock_sm_client.describe_endpoint.return_value = {
            "EndpointConfigName": "bird-classifier-config"
        }
        mock_sm_client.list_endpoint_configs.return_value = {
            "EndpointConfigs": [
                {"EndpointConfigName": "bird-classifier-config"},
            ]
        }

        with patch("deploy_model.sm_client", mock_sm_client):
            result = rollback_endpoint()

        assert result == ""
        mock_sm_client.update_endpoint.assert_not_called()
        captured = capsys.readouterr()
        assert "ERROR" in captured.out

    def test_waits_for_in_service_after_rollback(self, mock_sm_client):
        """rollback_endpoint waits for endpoint to become InService."""
        mock_sm_client.describe_endpoint.return_value = {
            "EndpointConfigName": "bird-classifier-config-20250115-103045"
        }
        mock_sm_client.list_endpoint_configs.return_value = {
            "EndpointConfigs": [
                {"EndpointConfigName": "bird-classifier-config-20250115-103045"},
                {"EndpointConfigName": "bird-classifier-config-20250114-090000"},
            ]
        }

        with patch("deploy_model.sm_client", mock_sm_client):
            rollback_endpoint()

        mock_sm_client.get_waiter.assert_called_with("endpoint_in_service")
        mock_sm_client.get_waiter.return_value.wait.assert_called_once()

    def test_lists_configs_with_correct_params(self, mock_sm_client):
        """rollback_endpoint queries configs with correct sort and filter."""
        mock_sm_client.describe_endpoint.return_value = {
            "EndpointConfigName": "bird-classifier-config-20250115-103045"
        }
        mock_sm_client.list_endpoint_configs.return_value = {
            "EndpointConfigs": [
                {"EndpointConfigName": "bird-classifier-config-20250115-103045"},
                {"EndpointConfigName": "bird-classifier-config-old"},
            ]
        }

        with patch("deploy_model.sm_client", mock_sm_client):
            rollback_endpoint()

        mock_sm_client.list_endpoint_configs.assert_called_once_with(
            SortBy="CreationTime",
            SortOrder="Descending",
            NameContains=ENDPOINT_CONFIG_NAME,
            MaxResults=10,
        )


class TestMainIntegration:
    """Tests for main() wiring of update and rollback."""

    def test_rollback_calls_rollback_endpoint(self):
        """main --rollback calls rollback_endpoint and returns."""
        with patch("deploy_model.rollback_endpoint") as mock_rollback, \
             patch("deploy_model.parse_args") as mock_parse:
            mock_parse.return_value = MagicMock(
                rollback=True,
                update_endpoint=False,
                model_type=None,
                model_path=None,
            )
            from deploy_model import main
            main()

        mock_rollback.assert_called_once()

    def test_update_endpoint_calls_update_not_create(self):
        """main --update-endpoint calls update_endpoint instead of create_endpoint."""
        with patch("deploy_model.update_endpoint") as mock_update, \
             patch("deploy_model.create_endpoint") as mock_create, \
             patch("deploy_model.get_sagemaker_role_arn", return_value="arn:test"), \
             patch("deploy_model.package_model", return_value="/tmp/model.tar.gz"), \
             patch("deploy_model.upload_to_s3", return_value="s3://bucket/model.tar.gz"), \
             patch("deploy_model.create_sagemaker_model"), \
             patch("deploy_model.register_model_version"), \
             patch("deploy_model.parse_args") as mock_parse:
            mock_parse.return_value = MagicMock(
                rollback=False,
                update_endpoint=True,
                model_type="dinov2",
                model_path="/fake/path",
            )
            from deploy_model import main
            main()

        mock_update.assert_called_once_with("arn:test")
        mock_create.assert_not_called()
