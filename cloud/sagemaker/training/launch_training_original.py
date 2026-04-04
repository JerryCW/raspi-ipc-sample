#!/usr/bin/env python3
"""启动训练 Job：清洗后原图，300 张/种，5 epoch"""

import json
import os
import tarfile
import tempfile
import time

import boto3

REGION = "ap-southeast-1"
ROLE_NAME = "smart-camera-sagemaker-role"
S3_BUCKET = "smart-camera-captures"
S3_DATA_URI = f"s3://{S3_BUCKET}/training-data/birds-cleaned/original"
S3_OUTPUT_PATH = f"s3://{S3_BUCKET}/training-output"
INSTANCE_TYPE = "ml.g4dn.xlarge"
TRAINING_IMAGE = "763104351884.dkr.ecr.ap-southeast-1.amazonaws.com/pytorch-training:2.0.1-gpu-py310"
JOB_NAME_PREFIX = "dinov2-original-300"

HYPERPARAMETERS = {
    "epochs": "5",
    "batch-size": "16",
    "lr": "0.001",
    "weight-decay": "0.0001",
    "freeze-backbone": "1",
    "max-samples-per-class": "300",
}


def get_role_arn():
    iam = boto3.client("iam", region_name=REGION)
    return iam.get_role(RoleName=ROLE_NAME)["Role"]["Arn"]


def package_and_upload():
    s3_client = boto3.client("s3", region_name=REGION)
    script_dir = os.path.dirname(os.path.abspath(__file__))

    with tempfile.NamedTemporaryFile(suffix=".tar.gz", delete=False) as tmp:
        tar_path = tmp.name
    with tarfile.open(tar_path, "w:gz") as tar:
        tar.add(os.path.join(script_dir, "train_sagemaker.py"), arcname="train_sagemaker.py")

    s3_key = "training-code/sourcedir-original.tar.gz"
    s3_client.upload_file(tar_path, S3_BUCKET, s3_key)
    os.unlink(tar_path)
    return f"s3://{S3_BUCKET}/{s3_key}"


def main():
    print("=" * 60)
    print("训练: 清洗后原图 + 300张/种")
    print("=" * 60)

    role_arn = get_role_arn()
    source_uri = package_and_upload()
    sm_client = boto3.client("sagemaker", region_name=REGION)
    job_name = f"{JOB_NAME_PREFIX}-{int(time.time())}"

    sm_client.create_training_job(
        TrainingJobName=job_name,
        RoleArn=role_arn,
        AlgorithmSpecification={
            "TrainingImage": TRAINING_IMAGE,
            "TrainingInputMode": "File",
        },
        HyperParameters={
            **HYPERPARAMETERS,
            "sagemaker_program": "train_sagemaker.py",
            "sagemaker_submit_directory": source_uri,
        },
        InputDataConfig=[{
            "ChannelName": "training",
            "DataSource": {
                "S3DataSource": {
                    "S3DataType": "S3Prefix",
                    "S3Uri": S3_DATA_URI,
                    "S3DataDistributionType": "FullyReplicated",
                }
            },
        }],
        OutputDataConfig={"S3OutputPath": S3_OUTPUT_PATH},
        ResourceConfig={
            "InstanceType": INSTANCE_TYPE,
            "InstanceCount": 1,
            "VolumeSizeInGB": 50,
        },
        StoppingCondition={"MaxRuntimeInSeconds": 43200},
    )

    print(f"Job: {job_name}")
    print(f"数据: {S3_DATA_URI}")
    print(f"Console: https://{REGION}.console.aws.amazon.com/sagemaker/home?region={REGION}#/jobs/{job_name}")

    while True:
        resp = sm_client.describe_training_job(TrainingJobName=job_name)
        status = resp["TrainingJobStatus"]
        secondary = resp.get("SecondaryStatus", "")
        print(f"  [{time.strftime('%H:%M:%S')}] {status} — {secondary}")
        if status in ("Completed", "Failed", "Stopped"):
            break
        time.sleep(30)

    if status == "Completed":
        print(f"\n✓ 完成! 模型: {resp['ModelArtifacts']['S3ModelArtifacts']}")
    else:
        print(f"\n✗ 失败: {resp.get('FailureReason', 'Unknown')}")


if __name__ == "__main__":
    main()
