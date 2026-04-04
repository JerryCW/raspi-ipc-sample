#!/usr/bin/env python3
"""启动 SageMaker Processing Job 进行数据清洗。

使用 boto3 直接调用 SageMaker API 创建 Processing Job。
Processing Job 会：
1. 启动一台 GPU 实例
2. 从 S3 下载原始训练数据
3. 运行清洗脚本（YOLO 检测 + 过滤 + train/val 分割）
4. 把清洗后的数据上传回 S3
5. 关闭实例

Usage:
  python cloud/sagemaker/training/launch_cleaning.py
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

# 输入：原始数据在 S3 上的路径
S3_RAW_DATA_URI = f"s3://{S3_BUCKET}/training-data/birds/raw"

# 输出：清洗后的数据保存路径
S3_CLEANED_DATA_URI = f"s3://{S3_BUCKET}/training-data/birds-cleaned"

# Processing 实例（GPU，YOLO 检测快 10 倍）
INSTANCE_TYPE = "ml.g4dn.xlarge"  # 1x T4 GPU
INSTANCE_COUNT = 1
VOLUME_SIZE_GB = 50

# PyTorch GPU 镜像
PROCESSING_IMAGE = "763104351884.dkr.ecr.ap-southeast-1.amazonaws.com/pytorch-training:2.0.1-gpu-py310"

JOB_NAME_PREFIX = "bird-data-cleaning"


def get_role_arn():
    iam = boto3.client("iam", region_name=REGION)
    response = iam.get_role(RoleName=ROLE_NAME)
    return response["Role"]["Arn"]


def package_and_upload_script() -> str:
    """把清洗脚本上传到 S3。"""
    s3_client = boto3.client("s3", region_name=REGION)
    script_dir = os.path.dirname(os.path.abspath(__file__))
    script_path = os.path.join(script_dir, "clean_sagemaker.py")

    s3_key = "training-code/clean_sagemaker.py"
    s3_client.upload_file(script_path, S3_BUCKET, s3_key)
    s3_uri = f"s3://{S3_BUCKET}/{s3_key}"
    print(f"  清洗脚本上传到: {s3_uri}")
    return s3_uri


def create_processing_job(role_arn: str) -> str:
    """创建 SageMaker Processing Job。"""
    sm_client = boto3.client("sagemaker", region_name=REGION)
    job_name = f"{JOB_NAME_PREFIX}-{int(time.time())}"

    # 上传清洗脚本
    script_uri = package_and_upload_script()

    sm_client.create_processing_job(
        ProcessingJobName=job_name,
        RoleArn=role_arn,

        # 使用 PyTorch GPU 镜像
        AppSpecification={
            "ImageUri": PROCESSING_IMAGE,
            "ContainerEntrypoint": ["python3", "/opt/ml/processing/code/clean_sagemaker.py"],
        },

        # 输入：原始数据 + 清洗脚本
        ProcessingInputs=[
            {
                "InputName": "input-data",
                "S3Input": {
                    "S3Uri": S3_RAW_DATA_URI,
                    "LocalPath": "/opt/ml/processing/input/raw",
                    "S3DataType": "S3Prefix",
                    "S3InputMode": "File",
                    "S3DataDistributionType": "FullyReplicated",
                },
            },
            {
                "InputName": "code",
                "S3Input": {
                    "S3Uri": script_uri,
                    "LocalPath": "/opt/ml/processing/code",
                    "S3DataType": "S3Prefix",
                    "S3InputMode": "File",
                    "S3DataDistributionType": "FullyReplicated",
                },
            },
        ],

        # 输出：清洗后的数据
        ProcessingOutputConfig={
            "Outputs": [
                {
                    "OutputName": "cleaned-data",
                    "S3Output": {
                        "S3Uri": S3_CLEANED_DATA_URI,
                        "LocalPath": "/opt/ml/processing/output",
                        "S3UploadMode": "EndOfJob",
                    },
                },
            ],
        },

        # 环境变量（告诉脚本输入输出路径）
        Environment={
            "SM_INPUT_DIR": "/opt/ml/processing/input",
            "SM_OUTPUT_DIR": "/opt/ml/processing/output",
        },

        # 实例配置
        ProcessingResources={
            "ClusterConfig": {
                "InstanceType": INSTANCE_TYPE,
                "InstanceCount": INSTANCE_COUNT,
                "VolumeSizeInGB": VOLUME_SIZE_GB,
            },
        },

        # 超时
        StoppingCondition={
            "MaxRuntimeInSeconds": 14400,  # 最多 4 小时
        },
    )

    print(f"  Processing Job created: {job_name}")
    return job_name


def wait_for_job(job_name: str):
    """等待 Processing Job 完成。"""
    sm_client = boto3.client("sagemaker", region_name=REGION)

    print(f"\n等待清洗完成（每 30 秒检查一次）...")
    print(f"Console 查看:")
    print(f"  https://{REGION}.console.aws.amazon.com/sagemaker/home?region={REGION}#/processing-jobs/{job_name}")
    print()

    while True:
        response = sm_client.describe_processing_job(ProcessingJobName=job_name)
        status = response["ProcessingJobStatus"]

        print(f"  [{time.strftime('%H:%M:%S')}] Status: {status}")

        if status in ("Completed", "Failed", "Stopped"):
            break

        time.sleep(30)

    if status == "Completed":
        print(f"\n✓ 清洗完成!")
        print(f"  清洗后数据: {S3_CLEANED_DATA_URI}")
    else:
        failure_reason = response.get("FailureReason", "Unknown")
        print(f"\n✗ 清洗失败: {failure_reason}")


def main():
    print("=" * 60)
    print("数据清洗 — SageMaker Processing Job")
    print("=" * 60)
    print(f"输入: {S3_RAW_DATA_URI}")
    print(f"输出: {S3_CLEANED_DATA_URI}")
    print(f"实例: {INSTANCE_TYPE}")
    print()

    # Step 1: IAM Role
    print("[Step 1] 获取 IAM Role...")
    role_arn = get_role_arn()
    print(f"  Role: {role_arn}")

    # Step 2: 创建 Processing Job
    print(f"\n[Step 2] 创建 Processing Job...")
    job_name = create_processing_job(role_arn)

    # Step 3: 等待完成
    wait_for_job(job_name)


if __name__ == "__main__":
    main()
