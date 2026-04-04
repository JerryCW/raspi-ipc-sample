#!/usr/bin/env python3
"""Launch DINOv2 fine-tune training job on SageMaker.

使用 boto3 直接调用 SageMaker API 创建 Training Job。
不依赖 sagemaker SDK 的高级封装，更稳定。

Usage:
  python cloud/sagemaker/training/launch_training.py
"""

import json
import os
import tarfile
import tempfile
import time

import boto3

# ---------------------------------------------------------------------------
# 配置
# ---------------------------------------------------------------------------

REGION = "ap-southeast-1"
ROLE_NAME = "smart-camera-sagemaker-role"
S3_BUCKET = "smart-camera-captures"

# 训练数据在 S3 上的路径
S3_DATA_URI = f"s3://{S3_BUCKET}/training-data/birds"

# 训练脚本输出路径
S3_OUTPUT_PATH = f"s3://{S3_BUCKET}/training-output"

# 训练实例
INSTANCE_TYPE = "ml.g4dn.xlarge"  # 1x NVIDIA T4 (16GB), ~$0.74/hr
USE_SPOT = False

# PyTorch 训练镜像（SageMaker 预置）
# PyTorch 2.0.1, Python 3.10, GPU
TRAINING_IMAGE = "763104351884.dkr.ecr.ap-southeast-1.amazonaws.com/pytorch-training:2.0.1-gpu-py310"

# 超参数
HYPERPARAMETERS = {
    "epochs": "5",
    "batch-size": "16",
    "lr": "0.001",
    "weight-decay": "0.0001",
    "freeze-backbone": "1",
}

JOB_NAME_PREFIX = "dinov2-bird-classifier"


def get_role_arn():
    iam = boto3.client("iam", region_name=REGION)
    response = iam.get_role(RoleName=ROLE_NAME)
    return response["Role"]["Arn"]


def package_source_code() -> str:
    """把训练脚本打包成 tar.gz 上传到 S3。

    SageMaker 要求训练代码以 sourcedir.tar.gz 形式提供。
    """
    script_dir = os.path.dirname(os.path.abspath(__file__))
    s3_client = boto3.client("s3", region_name=REGION)

    with tempfile.NamedTemporaryFile(suffix=".tar.gz", delete=False) as tmp:
        tar_path = tmp.name

    with tarfile.open(tar_path, "w:gz") as tar:
        # 只打包训练脚本
        train_script = os.path.join(script_dir, "train_sagemaker.py")
        tar.add(train_script, arcname="train_sagemaker.py")
        print(f"  + train_sagemaker.py")

    # 上传到 S3
    s3_key = "training-code/sourcedir.tar.gz"
    s3_client.upload_file(tar_path, S3_BUCKET, s3_key)
    os.unlink(tar_path)

    s3_uri = f"s3://{S3_BUCKET}/{s3_key}"
    print(f"  Source code uploaded to: {s3_uri}")
    return s3_uri


def create_training_job(role_arn: str, source_code_uri: str) -> str:
    """创建 SageMaker Training Job。"""
    sm_client = boto3.client("sagemaker", region_name=REGION)

    job_name = f"{JOB_NAME_PREFIX}-{int(time.time())}"

    create_params = {
        "TrainingJobName": job_name,
        "RoleArn": role_arn,

        # 训练镜像 + 脚本
        "AlgorithmSpecification": {
            "TrainingImage": TRAINING_IMAGE,
            "TrainingInputMode": "File",
        },

        # 超参数（SageMaker 会转成命令行参数传给脚本）
        "HyperParameters": {
            **HYPERPARAMETERS,
            "sagemaker_program": "train_sagemaker.py",
            "sagemaker_submit_directory": source_code_uri,
        },

        # 输入数据
        "InputDataConfig": [
            {
                "ChannelName": "training",
                "DataSource": {
                    "S3DataSource": {
                        "S3DataType": "S3Prefix",
                        "S3Uri": S3_DATA_URI,
                        "S3DataDistributionType": "FullyReplicated",
                    }
                },
            }
        ],

        # 输出路径
        "OutputDataConfig": {
            "S3OutputPath": S3_OUTPUT_PATH,
        },

        # 实例配置
        "ResourceConfig": {
            "InstanceType": INSTANCE_TYPE,
            "InstanceCount": 1,
            "VolumeSizeInGB": 50,
        },

        # 超时
        "StoppingCondition": {
            "MaxRuntimeInSeconds": 43200,  # 最多跑 12 小时
        },
    }

    # Spot Instance 配置
    if USE_SPOT:
        create_params["EnableManagedSpotTraining"] = True
        create_params["StoppingCondition"]["MaxWaitTimeInSeconds"] = 7200

    sm_client.create_training_job(**create_params)
    print(f"  Training Job created: {job_name}")
    return job_name


def wait_for_job(job_name: str):
    """等待 Training Job 完成，打印状态。"""
    sm_client = boto3.client("sagemaker", region_name=REGION)

    print(f"\n等待训练完成（每 30 秒检查一次）...")
    print(f"你可以在 SageMaker Console 查看实时日志：")
    print(f"  https://{REGION}.console.aws.amazon.com/sagemaker/home?region={REGION}#/jobs/{job_name}")
    print()

    while True:
        response = sm_client.describe_training_job(TrainingJobName=job_name)
        status = response["TrainingJobStatus"]
        secondary = response.get("SecondaryStatus", "")

        print(f"  [{time.strftime('%H:%M:%S')}] Status: {status} — {secondary}")

        if status in ("Completed", "Failed", "Stopped"):
            break

        time.sleep(30)

    if status == "Completed":
        model_uri = response["ModelArtifacts"]["S3ModelArtifacts"]
        duration = (response["TrainingEndTime"] - response["TrainingStartTime"]).total_seconds()
        print(f"\n✓ 训练完成！")
        print(f"  耗时: {duration/60:.1f} 分钟")
        print(f"  模型输出: {model_uri}")

        # 打印 Spot 节省的费用
        if USE_SPOT and "BillableTimeInSeconds" in response:
            billable = response["BillableTimeInSeconds"]
            total = int(duration)
            savings = max(0, (1 - billable / total) * 100) if total > 0 else 0
            print(f"  Spot 节省: {savings:.0f}%")

        return model_uri
    else:
        failure_reason = response.get("FailureReason", "Unknown")
        print(f"\n✗ 训练失败: {failure_reason}")
        return None


def main():
    print("=" * 60)
    print("DINOv2 Bird Classifier — SageMaker Training")
    print("=" * 60)
    print()

    # Step 1: IAM Role
    print("[Step 1] 获取 IAM Role...")
    role_arn = get_role_arn()
    print(f"  Role: {role_arn}")

    # Step 2: 打包上传训练代码
    print(f"\n[Step 2] 打包训练脚本...")
    source_code_uri = package_source_code()

    # Step 3: 创建 Training Job
    print(f"\n[Step 3] 创建 Training Job...")
    print(f"  实例: {INSTANCE_TYPE}")
    print(f"  Spot: {USE_SPOT}")
    print(f"  数据: {S3_DATA_URI}")
    print(f"  超参数: {json.dumps(HYPERPARAMETERS, indent=4)}")
    job_name = create_training_job(role_arn, source_code_uri)

    # Step 4: 等待完成
    model_uri = wait_for_job(job_name)

    if model_uri:
        print(f"\n下一步: 用这个模型部署到 SageMaker Endpoint")
        print(f"  模型 S3: {model_uri}")


if __name__ == "__main__":
    main()
