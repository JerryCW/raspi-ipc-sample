#!/usr/bin/env python3
"""SageMaker deployment script for bird species classifier.

Supports deploying different model types (dinov2, bioclip) via CLI arguments.

Steps:
  1. Resolve IAM role
  2. Package model weights + inference code as model.tar.gz
  3. Upload to S3
  4. Create SageMaker Model → EndpointConfig (Serverless) → Endpoint
  5. Register model version in SageMaker Model Registry

Prerequisites:
  - AWS credentials configured (aws configure)
  - IAM role `smart-camera-sagemaker-role` already created with:
      - AmazonSageMakerFullAccess
      - AmazonS3ReadOnlyAccess
  - pip install boto3

Usage:
  python cloud/sagemaker/deploy_model.py --model-type dinov2 --model-path /path/to/weights
  python cloud/sagemaker/deploy_model.py --model-type dinov2 --model-path s3://bucket/model.pth
  python cloud/sagemaker/deploy_model.py --model-type dinov2 --model-path /path/to/weights --update-endpoint
  python cloud/sagemaker/deploy_model.py --rollback
"""

import argparse
import datetime
import json
import os
import tarfile
import tempfile
import time

import boto3

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
REGION = "ap-southeast-1"

S3_BUCKET = "smart-camera-captures"
S3_MODEL_KEY = "models/bird-classifier/model.tar.gz"

SAGEMAKER_ROLE_NAME = "smart-camera-sagemaker-role"
MODEL_NAME = "bird-classifier"
ENDPOINT_CONFIG_NAME = "bird-classifier-config"
ENDPOINT_NAME = "bird-classifier-endpoint"

# Serverless inference settings
SERVERLESS_MEMORY_MB = 4096
SERVERLESS_MAX_CONCURRENCY = 5

# Model Registry
MODEL_PACKAGE_GROUP_NAME = "bird-classifier-models"

# SageMaker PyTorch inference image (must match requirements)
FRAMEWORK_VERSION = "2.0.1"
PY_VERSION = "py310"


# ---------------------------------------------------------------------------
# CLI Argument Parsing
# ---------------------------------------------------------------------------
def parse_args(argv=None):
    """Parse command-line arguments for deployment.

    Args:
        argv: Argument list (defaults to sys.argv if None). Useful for testing.

    Returns:
        argparse.Namespace with parsed arguments.
    """
    parser = argparse.ArgumentParser(
        description="Deploy bird classifier to SageMaker",
    )
    parser.add_argument(
        "--model-type",
        choices=["dinov2", "bioclip"],
        help="Model type to deploy (required unless --rollback)",
    )
    parser.add_argument(
        "--model-path",
        help="Path to model weights (local path or S3 URI, required unless --rollback)",
    )
    parser.add_argument(
        "--update-endpoint",
        action="store_true",
        help="Update existing endpoint instead of recreating",
    )
    parser.add_argument(
        "--rollback",
        action="store_true",
        help="Rollback to previous EndpointConfig",
    )
    args = parser.parse_args(argv)

    # Validate: --model-type and --model-path are required unless --rollback
    if not args.rollback:
        if not args.model_type:
            parser.error("--model-type is required unless --rollback is specified")
        if not args.model_path:
            parser.error("--model-path is required unless --rollback is specified")

    return args


# ---------------------------------------------------------------------------
# AWS Clients
# ---------------------------------------------------------------------------
iam_client = boto3.client("iam", region_name=REGION)
s3_client = boto3.client("s3", region_name=REGION)
sm_client = boto3.client("sagemaker", region_name=REGION)


def get_sagemaker_role_arn() -> str:
    """Retrieve the ARN of the SageMaker execution role."""
    print(f"[1/7] Looking up IAM role '{SAGEMAKER_ROLE_NAME}' ...")
    response = iam_client.get_role(RoleName=SAGEMAKER_ROLE_NAME)
    arn = response["Role"]["Arn"]
    print(f"       Role ARN: {arn}")
    return arn


def get_pytorch_inference_image_uri() -> str:
    """Build the ECR URI for the SageMaker pre-built PyTorch inference image."""
    account_map = {
        "us-east-1": "763104351884",
        "us-west-2": "763104351884",
        "eu-west-1": "763104351884",
        "ap-southeast-1": "763104351884",
        "ap-northeast-1": "763104351884",
    }
    account_id = account_map.get(REGION, "763104351884")
    image_uri = (
        f"{account_id}.dkr.ecr.{REGION}.amazonaws.com/"
        f"pytorch-inference:{FRAMEWORK_VERSION}-cpu-{PY_VERSION}"
    )
    print(f"       Inference image: {image_uri}")
    return image_uri


def _generate_model_config(model_type: str, model_path: str) -> dict:
    """Generate model_config.json content based on model type.

    Args:
        model_type: Model type string (e.g. 'dinov2', 'bioclip').
        model_path: Path to model weights directory or file.

    Returns:
        Config dict to be written as model_config.json.

    Raises:
        FileNotFoundError: If required class_names.json is missing for DINOv2.
    """
    config = {
        "model_type": model_type,
        "model_name": f"{model_type} Bird Classifier",
    }

    if model_type == "dinov2":
        config.update({
            "image_size": 518,
            "confidence_threshold": 0.2,
            "top_k": 3,
            "backbone": "dinov2_vitl14",
            "embed_dim": 1024,
        })
        # Read class_names from model path
        if os.path.isdir(model_path):
            class_names_path = os.path.join(model_path, "class_names.json")
        else:
            class_names_path = os.path.join(
                os.path.dirname(model_path), "class_names.json"
            )
        if os.path.exists(class_names_path):
            with open(class_names_path, "r") as f:
                config["class_names"] = json.load(f)
            print(
                f"       Read {len(config['class_names'])} class names "
                f"from {class_names_path}"
            )
        else:
            raise FileNotFoundError(
                f"class_names.json not found at {class_names_path}"
            )
    elif model_type == "bioclip":
        config.update({
            "image_size": 224,
            "confidence_threshold": 0.2,
            "top_k": 3,
        })
        # BioCLIP class_names come from bird_labels.json
        labels_path = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "bird_labels.json"
        )
        if os.path.exists(labels_path):
            with open(labels_path, "r") as f:
                labels_data = json.load(f)
            config["class_names"] = labels_data.get("species", [])

    return config


def package_model(model_path: str, model_type: str, work_dir: str) -> str:
    """Package model weights + inference code into model.tar.gz.

    Args:
        model_path: Path to model weights directory or file.
        model_type: Model type string (e.g. 'dinov2', 'bioclip').
        work_dir: Temporary working directory for the archive.

    Returns:
        Path to the created model.tar.gz file.
    """
    print("[3/7] Packaging model.tar.gz ...")

    tar_path = os.path.join(work_dir, "model.tar.gz")
    script_dir = os.path.dirname(os.path.abspath(__file__))

    with tarfile.open(tar_path, "w:gz") as tar:
        # Add model weights from --model-path
        if os.path.isdir(model_path):
            for item in os.listdir(model_path):
                item_path = os.path.join(model_path, item)
                if item.startswith(".") or item == "__pycache__":
                    continue
                tar.add(item_path, arcname=item)
                print(f"       + {item}")
        elif os.path.isfile(model_path):
            tar.add(model_path, arcname=os.path.basename(model_path))
            print(f"       + {os.path.basename(model_path)}")

        # Generate and add model_config.json
        model_config = _generate_model_config(model_type, model_path)
        config_path = os.path.join(work_dir, "model_config.json")
        with open(config_path, "w") as f:
            json.dump(model_config, f, indent=2)
        tar.add(config_path, arcname="model_config.json")
        print("       + model_config.json")

        # Add inference code
        inference_path = os.path.join(script_dir, "inference.py")
        tar.add(inference_path, arcname="code/inference.py")
        print("       + code/inference.py")

        # Add all handler modules
        for fname in os.listdir(script_dir):
            if fname.startswith("handler_") and fname.endswith(".py"):
                tar.add(os.path.join(script_dir, fname), arcname=f"code/{fname}")
                print(f"       + code/{fname}")

        # Add requirements
        requirements_path = os.path.join(script_dir, "requirements.txt")
        if os.path.exists(requirements_path):
            tar.add(requirements_path, arcname="code/requirements.txt")
            print("       + code/requirements.txt")

    size_mb = os.path.getsize(tar_path) / (1024 * 1024)
    print(f"       Archive size: {size_mb:.1f} MB")
    return tar_path


def upload_to_s3(tar_path: str) -> str:
    """Upload model.tar.gz to S3."""
    s3_uri = f"s3://{S3_BUCKET}/{S3_MODEL_KEY}"
    print(f"[4/7] Uploading to {s3_uri} ...")

    s3_client.upload_file(tar_path, S3_BUCKET, S3_MODEL_KEY)
    print("       Upload complete.")
    return s3_uri


def create_sagemaker_model(role_arn: str, s3_uri: str) -> str:
    """Create a SageMaker Model pointing to the S3 model artifact."""
    print(f"[5/7] Creating SageMaker Model '{MODEL_NAME}' ...")

    image_uri = get_pytorch_inference_image_uri()

    # Delete existing model if present (idempotent re-deploy)
    try:
        sm_client.delete_model(ModelName=MODEL_NAME)
        print(f"       Deleted existing model '{MODEL_NAME}'")
    except sm_client.exceptions.ClientError:
        pass

    sm_client.create_model(
        ModelName=MODEL_NAME,
        PrimaryContainer={
            "Image": image_uri,
            "ModelDataUrl": s3_uri,
            "Environment": {
                "SAGEMAKER_PROGRAM": "inference.py",
                "SAGEMAKER_SUBMIT_DIRECTORY": "/opt/ml/model/code",
                "SAGEMAKER_CONTAINER_LOG_LEVEL": "20",
            },
        },
        ExecutionRoleArn=role_arn,
    )
    print(f"       Model '{MODEL_NAME}' created.")
    return MODEL_NAME


def create_endpoint(role_arn: str) -> str:
    """Create Serverless EndpointConfig and Endpoint."""
    print(f"[6/7] Creating Serverless Endpoint '{ENDPOINT_NAME}' ...")

    # --- EndpointConfig ---
    try:
        sm_client.delete_endpoint_config(EndpointConfigName=ENDPOINT_CONFIG_NAME)
        print(f"       Deleted existing endpoint config '{ENDPOINT_CONFIG_NAME}'")
    except sm_client.exceptions.ClientError:
        pass

    sm_client.create_endpoint_config(
        EndpointConfigName=ENDPOINT_CONFIG_NAME,
        ProductionVariants=[
            {
                "VariantName": "AllTraffic",
                "ModelName": MODEL_NAME,
                "ServerlessConfig": {
                    "MemorySizeInMB": SERVERLESS_MEMORY_MB,
                    "MaxConcurrency": SERVERLESS_MAX_CONCURRENCY,
                },
            }
        ],
    )
    print(
        f"       EndpointConfig '{ENDPOINT_CONFIG_NAME}' created "
        f"(Serverless {SERVERLESS_MEMORY_MB}MB, "
        f"MaxConcurrency={SERVERLESS_MAX_CONCURRENCY})"
    )

    # --- Endpoint ---
    try:
        sm_client.delete_endpoint(EndpointName=ENDPOINT_NAME)
        print(f"       Deleting existing endpoint '{ENDPOINT_NAME}' ...")
        waiter = sm_client.get_waiter("endpoint_deleted")
        waiter.wait(
            EndpointName=ENDPOINT_NAME,
            WaiterConfig={"Delay": 10, "MaxAttempts": 60},
        )
        print("       Existing endpoint deleted.")
    except sm_client.exceptions.ClientError:
        pass

    sm_client.create_endpoint(
        EndpointName=ENDPOINT_NAME,
        EndpointConfigName=ENDPOINT_CONFIG_NAME,
    )
    print(f"       Endpoint '{ENDPOINT_NAME}' creation initiated.")

    print("       Waiting for endpoint to become InService ...")
    waiter = sm_client.get_waiter("endpoint_in_service")
    waiter.wait(
        EndpointName=ENDPOINT_NAME,
        WaiterConfig={"Delay": 30, "MaxAttempts": 40},
    )
    print(f"       Endpoint '{ENDPOINT_NAME}' is InService!")
    return ENDPOINT_NAME


def update_endpoint(role_arn: str) -> str:
    """Update existing endpoint with new EndpointConfig.

    Creates a new EndpointConfig with timestamp suffix and updates the endpoint.
    Logs the current EndpointConfig name for rollback reference.

    Args:
        role_arn: IAM role ARN (unused but kept for interface consistency).

    Returns:
        Name of the newly created EndpointConfig.
    """
    print(f"Updating endpoint '{ENDPOINT_NAME}' ...")

    # Get current EndpointConfig for rollback reference
    response = sm_client.describe_endpoint(EndpointName=ENDPOINT_NAME)
    current_config = response["EndpointConfigName"]
    print(f"  Current EndpointConfig: {current_config} (save for rollback)")

    # Create new EndpointConfig with timestamp
    timestamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    new_config_name = f"{ENDPOINT_CONFIG_NAME}-{timestamp}"

    sm_client.create_endpoint_config(
        EndpointConfigName=new_config_name,
        ProductionVariants=[
            {
                "VariantName": "AllTraffic",
                "ModelName": MODEL_NAME,
                "ServerlessConfig": {
                    "MemorySizeInMB": SERVERLESS_MEMORY_MB,
                    "MaxConcurrency": SERVERLESS_MAX_CONCURRENCY,
                },
            }
        ],
    )
    print(f"  Created new EndpointConfig: {new_config_name}")

    # Update endpoint
    sm_client.update_endpoint(
        EndpointName=ENDPOINT_NAME,
        EndpointConfigName=new_config_name,
    )
    print("  Endpoint update initiated. Waiting for InService ...")

    waiter = sm_client.get_waiter("endpoint_in_service")
    waiter.wait(
        EndpointName=ENDPOINT_NAME,
        WaiterConfig={"Delay": 30, "MaxAttempts": 40},
    )
    print(f"  Endpoint '{ENDPOINT_NAME}' updated and InService!")
    print(f"  To rollback: python deploy_model.py --rollback")
    print(f"  Previous config: {current_config}")

    return new_config_name


def rollback_endpoint() -> str:
    """Rollback endpoint to previous EndpointConfig.

    Describes the current endpoint, finds the previous EndpointConfig
    by listing configs sorted by creation time, and updates the endpoint.

    Returns:
        Name of the EndpointConfig rolled back to, or empty string on failure.
    """
    print(f"Rolling back endpoint '{ENDPOINT_NAME}' ...")

    # Get current EndpointConfig
    response = sm_client.describe_endpoint(EndpointName=ENDPOINT_NAME)
    current_config = response["EndpointConfigName"]
    print(f"  Current EndpointConfig: {current_config}")

    # List endpoint configs matching our naming pattern, sorted by creation time
    configs = sm_client.list_endpoint_configs(
        SortBy="CreationTime",
        SortOrder="Descending",
        NameContains=ENDPOINT_CONFIG_NAME,
        MaxResults=10,
    )

    config_names = [c["EndpointConfigName"] for c in configs["EndpointConfigs"]]

    # Find the config right after the current one (previous by time)
    previous_config = None
    found_current = False
    for name in config_names:
        if found_current:
            previous_config = name
            break
        if name == current_config:
            found_current = True

    if not previous_config:
        print("  ERROR: No previous EndpointConfig found for rollback.")
        print(f"  Available configs: {config_names}")
        return ""

    print(f"  Rolling back to: {previous_config}")

    sm_client.update_endpoint(
        EndpointName=ENDPOINT_NAME,
        EndpointConfigName=previous_config,
    )
    print("  Rollback initiated. Waiting for InService ...")

    waiter = sm_client.get_waiter("endpoint_in_service")
    waiter.wait(
        EndpointName=ENDPOINT_NAME,
        WaiterConfig={"Delay": 30, "MaxAttempts": 40},
    )
    print(f"  Endpoint '{ENDPOINT_NAME}' rolled back and InService!")

    return previous_config


def register_model_version(
    s3_uri: str, role_arn: str, model_type: str
) -> str:
    """Register the model in SageMaker Model Registry."""
    print(
        f"[7/7] Registering model in Model Registry "
        f"'{MODEL_PACKAGE_GROUP_NAME}' ..."
    )

    image_uri = get_pytorch_inference_image_uri()

    # Create model package group if it doesn't exist
    try:
        sm_client.create_model_package_group(
            ModelPackageGroupName=MODEL_PACKAGE_GROUP_NAME,
            ModelPackageGroupDescription="Bird species classifier models",
        )
        print(f"       Created model package group '{MODEL_PACKAGE_GROUP_NAME}'")
    except sm_client.exceptions.ClientError as e:
        if "already exists" in str(e):
            print(
                f"       Model package group "
                f"'{MODEL_PACKAGE_GROUP_NAME}' already exists"
            )
        else:
            raise

    response = sm_client.create_model_package(
        ModelPackageGroupName=MODEL_PACKAGE_GROUP_NAME,
        ModelPackageDescription=(
            f"{model_type} bird classifier, Serverless Inference."
        ),
        InferenceSpecification={
            "Containers": [
                {
                    "Image": image_uri,
                    "ModelDataUrl": s3_uri,
                    "Environment": {
                        "SAGEMAKER_PROGRAM": "inference.py",
                        "SAGEMAKER_SUBMIT_DIRECTORY": "/opt/ml/model/code",
                    },
                }
            ],
            "SupportedContentTypes": ["application/x-image", "image/jpeg"],
            "SupportedResponseMIMETypes": ["application/json"],
            "SupportedRealtimeInferenceInstanceTypes": [
                "ml.m5.large",
                "ml.c5.large",
            ],
        },
        ModelApprovalStatus="Approved",
    )

    model_package_arn = response["ModelPackageArn"]
    print(f"       Registered model version: {model_package_arn}")
    return model_package_arn


def main(argv=None) -> None:
    """Run the full deployment pipeline."""
    args = parse_args(argv)

    # Handle --rollback
    if args.rollback:
        rollback_endpoint()
        return

    model_type = args.model_type
    model_path = args.model_path

    print("=" * 60)
    print("SageMaker Bird Classifier Deployment")
    print(f"  Model type: {model_type}")
    print(f"  Model path: {model_path}")
    print(f"  Region:     {REGION}")
    print(f"  Endpoint:   {ENDPOINT_NAME}")
    if args.update_endpoint:
        print("  Mode:       Update existing endpoint")
    print("=" * 60)
    print()

    start_time = time.time()

    # Step 1: Resolve IAM role
    role_arn = get_sagemaker_role_arn()
    print()

    with tempfile.TemporaryDirectory(prefix="bird-classifier-") as work_dir:
        # Step 3: Package model.tar.gz (using --model-path directly)
        tar_path = package_model(model_path, model_type, work_dir)
        print()

        # Step 4: Upload to S3
        s3_uri = upload_to_s3(tar_path)
        print()

    # Step 5: Create SageMaker Model
    create_sagemaker_model(role_arn, s3_uri)
    print()

    if args.update_endpoint:
        # Step 6: Update existing endpoint with new EndpointConfig
        update_endpoint(role_arn)
    else:
        # Step 6: Create Endpoint from scratch
        create_endpoint(role_arn)
    print()

    # Step 7: Register in Model Registry
    register_model_version(s3_uri, role_arn, model_type)
    print()

    elapsed = time.time() - start_time
    print("=" * 60)
    print(f"Deployment complete in {elapsed:.0f}s")
    print(f"  Endpoint:  {ENDPOINT_NAME}")
    print(f"  Model S3:  s3://{S3_BUCKET}/{S3_MODEL_KEY}")
    print(f"  Registry:  {MODEL_PACKAGE_GROUP_NAME}")
    print("=" * 60)


if __name__ == "__main__":
    main()
