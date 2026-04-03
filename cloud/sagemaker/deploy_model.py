#!/usr/bin/env python3
"""SageMaker deployment script for BioCLIP bird species classifier.

Steps:
  1. Download HuggingFace model `imageomics/bioclip`
  2. Package as model.tar.gz (model weights + inference.py + requirements.txt + bird_labels.json)
  3. Upload to s3://smart-camera-captures/models/bird-classifier/model.tar.gz
  4. Create SageMaker Model → EndpointConfig (Serverless) → Endpoint
  5. Register model version in SageMaker Model Registry

Prerequisites:
  - AWS credentials configured (aws configure)
  - IAM role `smart-camera-sagemaker-role` already created with:
      - AmazonSageMakerFullAccess
      - AmazonS3ReadOnlyAccess
  - pip install boto3 huggingface_hub

Usage:
  python cloud/sagemaker/deploy_model.py
"""

import os
import tarfile
import tempfile
import time

import boto3
from huggingface_hub import snapshot_download

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
REGION = "ap-southeast-1"
HF_MODEL_ID = "imageomics/bioclip"

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
# Using the pre-built PyTorch 2.0 inference container for Python 3.10
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


def download_model(download_dir: str) -> str:
    """Download the HuggingFace model to a local directory."""
    print(f"[2/7] Downloading HuggingFace model '{HF_MODEL_ID}' ...")
    model_dir = snapshot_download(
        repo_id=HF_MODEL_ID,
        local_dir=os.path.join(download_dir, "model"),
    )
    print(f"       Downloaded to: {model_dir}")
    return model_dir


def package_model(model_dir: str, work_dir: str) -> str:
    """Package model weights + inference.py + requirements.txt into model.tar.gz."""
    print("[3/7] Packaging model.tar.gz ...")

    tar_path = os.path.join(work_dir, "model.tar.gz")
    script_dir = os.path.dirname(os.path.abspath(__file__))

    with tarfile.open(tar_path, "w:gz") as tar:
        # Add all model files (weights, config.json, etc.)
        for item in os.listdir(model_dir):
            item_path = os.path.join(model_dir, item)
            # Skip hidden files and __pycache__
            if item.startswith(".") or item == "__pycache__":
                continue
            tar.add(item_path, arcname=item)
            print(f"       + {item}")

        # Add inference code
        inference_path = os.path.join(script_dir, "inference.py")
        tar.add(inference_path, arcname="code/inference.py")
        print("       + code/inference.py")

        # Add requirements
        requirements_path = os.path.join(script_dir, "requirements.txt")
        tar.add(requirements_path, arcname="code/requirements.txt")
        print("       + code/requirements.txt")

        # Add bird labels
        labels_path = os.path.join(script_dir, "bird_labels.json")
        tar.add(labels_path, arcname="bird_labels.json")
        print("       + bird_labels.json")

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
    print(f"[7/7] Registering model in Model Registry '{MODEL_PACKAGE_GROUP_NAME}' ...")

    image_uri = get_pytorch_inference_image_uri()

    # Create model package group if it doesn't exist
    try:
        sm_client.create_model_package_group(
            ModelPackageGroupName=MODEL_PACKAGE_GROUP_NAME,
            ModelPackageGroupDescription="Bird species classifier models (BioCLIP)",
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
            f"BioCLIP zero-shot bird classifier from HuggingFace ({HF_MODEL_ID}). "
            f"Open-vocabulary species recognition, Serverless Inference."
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
    print("=" * 60)
    print("SageMaker Bird Classifier Deployment")
    print(f"  Model:    {HF_MODEL_ID}")
    print(f"  Region:   {REGION}")
    print(f"  Endpoint: {ENDPOINT_NAME}")
    print("=" * 60)
    print()

    start_time = time.time()

    # Step 1: Resolve IAM role
    role_arn = get_sagemaker_role_arn()
    print()

    # Use a temp directory for model download and packaging
    with tempfile.TemporaryDirectory(prefix="bird-classifier-") as work_dir:
        # Step 2: Download model from HuggingFace
        model_dir = download_model(work_dir)
        print()

        # Step 3: Package model.tar.gz
        tar_path = package_model(model_dir, work_dir)
        print()

        # Step 4: Upload to S3
        s3_uri = upload_to_s3(tar_path)
        print()

    # Step 5: Create SageMaker Model
    create_sagemaker_model(role_arn, s3_uri)
    print()

    # Step 6: Create Endpoint
    create_endpoint(role_arn)
    print()

    # Step 7: Register in Model Registry
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
