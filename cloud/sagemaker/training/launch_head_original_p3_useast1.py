#!/usr/bin/env python3
"""Head 分类训练：清洗后原图 300张/种，P3 实例 — us-east-1"""

import os, tarfile, tempfile, time
import boto3

REGION = "us-east-1"
ROLE_NAME = "smart-camera-sagemaker-role"
S3_BUCKET = "smart-camera-training-us-east-1"
S3_DATA_URI = f"s3://{S3_BUCKET}/training-data/birds-cleaned/original"
S3_OUTPUT_PATH = f"s3://{S3_BUCKET}/training-output"
INSTANCE_TYPE = "ml.p3.2xlarge"
TRAINING_IMAGE = "763104351884.dkr.ecr.us-east-1.amazonaws.com/pytorch-training:2.0.1-gpu-py310"
JOB_NAME_PREFIX = "dinov2-head-original-p3"

HYPERPARAMETERS = {
    "epochs": "5", "batch-size": "16", "lr": "0.001",
    "weight-decay": "0.0001", "freeze-backbone": "1",
    "max-samples-per-class": "300",
}

def get_role_arn():
    iam = boto3.client("iam", region_name=REGION)
    return iam.get_role(RoleName=ROLE_NAME)["Role"]["Arn"]

def package_and_upload():
    s3 = boto3.client("s3", region_name=REGION)
    d = os.path.dirname(os.path.abspath(__file__))
    with tempfile.NamedTemporaryFile(suffix=".tar.gz", delete=False) as t:
        p = t.name
    with tarfile.open(p, "w:gz") as tar:
        tar.add(os.path.join(d, "train_sagemaker.py"), arcname="train_sagemaker.py")
    s3.upload_file(p, S3_BUCKET, "code/sourcedir-head-original-p3.tar.gz")
    os.unlink(p)
    return f"s3://{S3_BUCKET}/code/sourcedir-head-original-p3.tar.gz"

def main():
    print(f"Head 分类训练 原图 P3 — {REGION}")
    role_arn = get_role_arn()
    source_uri = package_and_upload()
    sm = boto3.client("sagemaker", region_name=REGION)
    job = f"{JOB_NAME_PREFIX}-{int(time.time())}"

    sm.create_training_job(
        TrainingJobName=job, RoleArn=role_arn,
        AlgorithmSpecification={"TrainingImage": TRAINING_IMAGE, "TrainingInputMode": "File"},
        HyperParameters={**HYPERPARAMETERS, "sagemaker_program": "train_sagemaker.py", "sagemaker_submit_directory": source_uri},
        InputDataConfig=[{"ChannelName": "training", "DataSource": {"S3DataSource": {"S3DataType": "S3Prefix", "S3Uri": S3_DATA_URI, "S3DataDistributionType": "FullyReplicated"}}}],
        OutputDataConfig={"S3OutputPath": S3_OUTPUT_PATH},
        ResourceConfig={"InstanceType": INSTANCE_TYPE, "InstanceCount": 1, "VolumeSizeInGB": 50},
        StoppingCondition={"MaxRuntimeInSeconds": 43200},
    )
    print(f"Job: {job}")
    print(f"Console: https://{REGION}.console.aws.amazon.com/sagemaker/home?region={REGION}#/jobs/{job}")

    while True:
        r = sm.describe_training_job(TrainingJobName=job)
        s = r["TrainingJobStatus"]
        print(f"  [{time.strftime('%H:%M:%S')}] {s} — {r.get('SecondaryStatus','')}")
        if s in ("Completed", "Failed", "Stopped"): break
        time.sleep(30)

    if s == "Completed":
        d = (r["TrainingEndTime"] - r["TrainingStartTime"]).total_seconds()
        print(f"\n✓ 完成! 耗时: {d/60:.1f}分钟")
        print(f"  模型: {r['ModelArtifacts']['S3ModelArtifacts']}")
    else:
        print(f"\n✗ {r.get('FailureReason','Unknown')}")

if __name__ == "__main__":
    main()
