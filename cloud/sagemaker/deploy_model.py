#!/usr/bin/env python3
"""SageMaker deployment script for DINOv2 fine-tuned bird species classifier.

Steps:
  1. Package model.tar.gz from local checkpoints
     (model.pth + model_config.json + class_names.json + code/inference.py + code/handler_dinov2.py + code/requirements.txt)
  2. Upload to s3://smart-camera-captures/models/bird-classifier/model.tar.gz
  3. Create SageMaker Model → EndpointConfig (Serverless) → Endpoint
  4. Register model version in SageMaker Model Registry

Prerequisites:
  - AWS credentials configured (aws configure)
  - IAM role `smart-camera-sagemaker-role` already created
  - Local model checkpoint at cloud/sagemaker/training/checkpoints/model.pth
  - pip install boto3

Usage:
  python cloud/sagemaker/deploy_model.py
  python cloud/sagemaker/deploy_model.py --model-path /path/to/model.pth
"""

import argparse
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
SERVERLESS_MEMORY_MB = 6144  # DINOv2-Large 需要更多内存
SERVERLESS_MAX_CONCURRENCY = 5

# Model Registry
MODEL_PACKAGE_GROUP_NAME = "bird-classifier-models"

# Default local checkpoint path
DEFAULT_CHECKPOINT_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "training", "checkpoints",
)

# SageMaker PyTorch inference image
FRAMEWORK_VERSION = "2.0.1"
PY_VERSION = "py310"


# ---------------------------------------------------------------------------
# AWS Clients
# ---------------------------------------------------------------------------
iam_client = boto3.client("iam", region_name=REGION)
s3_client = boto3.client("s3", region_name=REGION)
sm_client = boto3.client("sagemaker", region_name=REGION)


def get_sagemaker_role_arn() -> str:
    """Retrieve the ARN of the SageMaker execution role."""
    print(f"[1/6] Looking up IAM role '{SAGEMAKER_ROLE_NAME}' ...")
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


def package_model(checkpoint_dir: str, work_dir: str) -> str:
    """Package DINOv2 model checkpoint + inference code into model.tar.gz.

    Contents:
      model.pth           — DINOv2 fine-tuned weights
      model_config.json   — model type, class names, thresholds
      class_names.json    — class name list (backward compat)
      code/inference.py   — SageMaker pluggable inference entry point
      code/handler_dinov2.py — DINOv2 handler implementation
      code/requirements.txt  — Python dependencies
    """
    print("[2/6] Packaging model.tar.gz ...")

    tar_path = os.path.join(work_dir, "model.tar.gz")
    script_dir = os.path.dirname(os.path.abspath(__file__))

    # Verify checkpoint exists
    model_pth = os.path.join(checkpoint_dir, "model.pth")
    if not os.path.exists(model_pth):
        raise FileNotFoundError(f"model.pth not found in {checkpoint_dir}")

    with tarfile.open(tar_path, "w:gz") as tar:
        # Model weights
        tar.add(model_pth, arcname="model.pth")
        print("       + model.pth")

        # Model config
        config_path = os.path.join(script_dir, "model_config.json")
        tar.add(config_path, arcname="model_config.json")
        print("       + model_config.json")

        # Class names (backward compat)
        class_names_path = os.path.join(checkpoint_dir, "class_names.json")
        if os.path.exists(class_names_path):
            tar.add(class_names_path, arcname="class_names.json")
            print("       + class_names.json")

        # Inference code
        tar.add(os.path.join(script_dir, "inference.py"), arcname="code/inference.py")
        print("       + code/inference.py")

        tar.add(os.path.join(script_dir, "handler_dinov2.py"), arcname="code/handler_dinov2.py")
        print("       + code/handler_dinov2.py")

        tar.add(os.path.join(script_dir, "requirements.txt"), arcname="code/requirements.txt")
        print("       + code/requirements.txt")

    size_mb = os.path.getsize(tar_path) / (1024 * 1024)
    print(f"       Archive size: {size_mb:.1f} MB")
    return tar_path


def upload_to_s3(tar_path: str) -> str:
    """Upload model.tar.gz to S3."""
    s3_uri = f"s3://{S3_BUCKET}/{S3_MODEL_KEY}"
    print(f"[3/6] Uploading to {s3_uri} ...")

    s3_client.upload_file(tar_path, S3_BUCKET, S3_MODEL_KEY)
    print("       Upload complete.")
    return s3_uri


def create_sagemaker_model(role_arn: str, s3_uri: str) -> str:
    """Create a SageMaker Model pointing to the S3 model artifact."""
    print(f"[4/6] Creating SageMaker Model '{MODEL_NAME}' ...")

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
    print(f"[5/6] Creating Serverless Endpoint '{ENDPOINT_NAME}' ...")

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
        f"(Serverless {SERVERLESS_MEMORY_MB}MB, MaxConcurrency={SERVERLESS_MAX_CONCURRENCY})"
    )

    # --- Endpoint ---
    try:
        sm_client.delete_endpoint(EndpointName=ENDPOINT_NAME)
        print(f"       Deleting existing endpoint '{ENDPOINT_NAME}' ...")
        # Wait for deletion to complete before re-creating
        waiter = sm_client.get_waiter("endpoint_deleted")
        waiter.wait(
            EndpointName=ENDPOINT_NAME,
            WaiterConfig={"Delay": 10, "MaxAttempts": 60},
        )
        print(f"       Existing endpoint deleted.")
    except sm_client.exceptions.ClientError:
        pass

    sm_client.create_endpoint(
        EndpointName=ENDPOINT_NAME,
        EndpointConfigName=ENDPOINT_CONFIG_NAME,
    )
    print(f"       Endpoint '{ENDPOINT_NAME}' creation initiated.")

    # Wait for endpoint to become InService
    print("       Waiting for endpoint to become InService (this may take several minutes) ...")
    waiter = sm_client.get_waiter("endpoint_in_service")
    waiter.wait(
        EndpointName=ENDPOINT_NAME,
        WaiterConfig={"Delay": 30, "MaxAttempts": 40},
    )
    print(f"       Endpoint '{ENDPOINT_NAME}' is InService!")
    return ENDPOINT_NAME


def register_model_version(s3_uri: str, role_arn: str) -> str:
    """Register the model in SageMaker Model Registry."""
    print(f"[6/6] Registering model in Model Registry '{MODEL_PACKAGE_GROUP_NAME}' ...")

    image_uri = get_pytorch_inference_image_uri()

    # Create model package group if it doesn't exist
    try:
        sm_client.create_model_package_group(
            ModelPackageGroupName=MODEL_PACKAGE_GROUP_NAME,
            ModelPackageGroupDescription="Bird species classifier models (DINOv2 fine-tuned)",
        )
        print(f"       Created model package group '{MODEL_PACKAGE_GROUP_NAME}'")
    except sm_client.exceptions.ClientError as e:
        if "already exists" in str(e):
            print(f"       Model package group '{MODEL_PACKAGE_GROUP_NAME}' already exists")
        else:
            raise

    response = sm_client.create_model_package(
        ModelPackageGroupName=MODEL_PACKAGE_GROUP_NAME,
        ModelPackageDescription=(
            "DINOv2-ViT-L/14 fine-tuned bird species classifier (39 species). "
            "Serverless Inference."
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
            "SupportedRealtimeInferenceInstanceTypes": ["ml.m5.large", "ml.c5.large"],
        },
        ModelApprovalStatus="Approved",
    )

    model_package_arn = response["ModelPackageArn"]
    print(f"       Registered model version: {model_package_arn}")
    return model_package_arn


def main() -> None:
    """Run the full deployment pipeline."""
    parser = argparse.ArgumentParser(description="Deploy DINOv2 bird classifier to SageMaker")
    parser.add_argument(
        "--model-path", default=DEFAULT_CHECKPOINT_DIR,
        help="Path to checkpoint directory containing model.pth (default: training/checkpoints/)",
    )
    args = parser.parse_args()

    print("=" * 60)
    print("SageMaker Bird Classifier Deployment (DINOv2)")
    print(f"  Checkpoint: {args.model_path}")
    print(f"  Region:     {REGION}")
    print(f"  Endpoint:   {ENDPOINT_NAME}")
    print("=" * 60)
    print()

    start_time = time.time()

    # Step 1: Resolve IAM role
    role_arn = get_sagemaker_role_arn()
    print()

    with tempfile.TemporaryDirectory(prefix="bird-classifier-") as work_dir:
        # Step 2: Package model.tar.gz
        tar_path = package_model(args.model_path, work_dir)
        print()

        # Step 3: Upload to S3
        s3_uri = upload_to_s3(tar_path)
        print()

    # Step 4: Create SageMaker Model
    create_sagemaker_model(role_arn, s3_uri)
    print()

    # Step 5: Create Endpoint
    create_endpoint(role_arn)
    print()

    # Step 6: Register in Model Registry
    register_model_version(s3_uri, role_arn)
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
