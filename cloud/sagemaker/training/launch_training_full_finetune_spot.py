#!/usr/bin/env python3
"""启动训练 Job：Full fine-tune，Spot 实例，带 checkpoint 恢复"""

import os
import tarfile
import tempfile
import time

import boto3

REGION = "ap-southeast-1"
ROLE_NAME = "smart-camera-sagemaker-role"
S3_BUCKET = "smart-camera-captures"
S3_DATA_URI = f"s3://{S3_BUCKET}/training-data/birds-cleaned/cropped"
S3_OUTPUT_PATH = f"s3://{S3_BUCKET}/training-output"
S3_CHECKPOINT_URI = f"s3://{S3_BUCKET}/training-checkpoints/dinov2-full-finetune-spot"
INSTANCE_TYPE = "ml.p3.2xlarge"
TRAINING_IMAGE = "763104351884.dkr.ecr.ap-southeast-1.amazonaws.com/pytorch-training:2.0.1-gpu-py310"
JOB_NAME_PREFIX = "dinov2-full-ft-cropped-spot"

HYPERPARAMETERS = {
    "epochs": "5",
    "batch-size": "8",
    "lr": "0.00001",
    "weight-decay": "0.0001",
    "freeze-backbone": "0",
    "max-samples-per-class": "500",
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

    s3_key = "training-code/sourcedir-full-ft-spot.tar.gz"
    s3_client.upload_file(tar_path, S3_BUCKET, s3_key)
    os.unlink(tar_path)
    return f"s3://{S3_BUCKET}/{s3_key}"


def main():
    print("=" * 60)
    print("训练: Full Fine-tune + Spot 实例 + Checkpoint 恢复")
    print("=" * 60)
    print(f"Spot 实例: {INSTANCE_TYPE}")
    print(f"Checkpoint S3: {S3_CHECKPOINT_URI}")
    print(f"如果 Spot 中断，重新运行此脚本即可从断点恢复")
    print()

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
        # Spot 实例配置
        EnableManagedSpotTraining=True,
        StoppingCondition={
            "MaxRuntimeInSeconds": 43200,
            "MaxWaitTimeInSeconds": 86400,  # 最多等 24 小时分配 Spot
        },
        # Checkpoint 配置：实时同步到 S3，Spot 中断后可恢复
        CheckpointConfig={
            "S3Uri": S3_CHECKPOINT_URI,
            "LocalPath": "/opt/ml/checkpoints",
        },
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
        duration = (resp["TrainingEndTime"] - resp["TrainingStartTime"]).total_seconds()
        billable = resp.get("BillableTimeInSeconds", int(duration))
        savings = max(0, (1 - billable / int(duration)) * 100) if duration > 0 else 0
        print(f"\n✓ 完成!")
        print(f"  模型: {resp['ModelArtifacts']['S3ModelArtifacts']}")
        print(f"  耗时: {duration/60:.1f} 分钟")
        print(f"  Spot 节省: {savings:.0f}%")
    else:
        print(f"\n✗ 失败: {resp.get('FailureReason', 'Unknown')}")


if __name__ == "__main__":
    main()
