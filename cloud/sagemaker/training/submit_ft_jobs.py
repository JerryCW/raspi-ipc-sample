#!/usr/bin/env python3
"""只提交 full fine-tune jobs，不轮询（避免进程被杀导致 job 被停止）"""
import os, tarfile, tempfile, time
import boto3

REGION = "us-east-1"
ROLE_NAME = "smart-camera-sagemaker-role"
S3_BUCKET = "smart-camera-training-us-east-1"
INSTANCE_TYPE = "ml.p3.2xlarge"
TRAINING_IMAGE = "763104351884.dkr.ecr.us-east-1.amazonaws.com/pytorch-training:2.0.1-gpu-py310"

HYPERPARAMETERS = {
    "epochs": "5", "batch-size": "2", "lr": "0.00001",
    "weight-decay": "0.0001", "freeze-backbone": "0",
    "max-samples-per-class": "500", "grad-accum-steps": "4",
}

def get_role_arn():
    iam = boto3.client("iam", region_name=REGION)
    return iam.get_role(RoleName=ROLE_NAME)["Role"]["Arn"]

def package_and_upload(suffix):
    s3 = boto3.client("s3", region_name=REGION)
    d = os.path.dirname(os.path.abspath(__file__))
    with tempfile.NamedTemporaryFile(suffix=".tar.gz", delete=False) as t:
        p = t.name
    with tarfile.open(p, "w:gz") as tar:
        tar.add(os.path.join(d, "train_sagemaker.py"), arcname="train_sagemaker.py")
    key = f"code/sourcedir-ft-{suffix}.tar.gz"
    s3.upload_file(p, S3_BUCKET, key)
    os.unlink(p)
    return f"s3://{S3_BUCKET}/{key}"

def submit_job(name_prefix, data_subpath, checkpoint_subpath, source_uri, role_arn):
    sm = boto3.client("sagemaker", region_name=REGION)
    job = f"{name_prefix}-{int(time.time())}"
    sm.create_training_job(
        TrainingJobName=job, RoleArn=role_arn,
        AlgorithmSpecification={"TrainingImage": TRAINING_IMAGE, "TrainingInputMode": "File"},
        HyperParameters={**HYPERPARAMETERS, "sagemaker_program": "train_sagemaker.py", "sagemaker_submit_directory": source_uri},
        InputDataConfig=[{"ChannelName": "training", "DataSource": {"S3DataSource": {"S3DataType": "S3Prefix", "S3Uri": f"s3://{S3_BUCKET}/training-data/birds-cleaned/{data_subpath}", "S3DataDistributionType": "FullyReplicated"}}}],
        OutputDataConfig={"S3OutputPath": f"s3://{S3_BUCKET}/training-output"},
        ResourceConfig={"InstanceType": INSTANCE_TYPE, "InstanceCount": 1, "VolumeSizeInGB": 50},
        StoppingCondition={"MaxRuntimeInSeconds": 43200},
        CheckpointConfig={"S3Uri": f"s3://{S3_BUCKET}/checkpoints/{checkpoint_subpath}", "LocalPath": "/opt/ml/checkpoints"},
    )
    print(f"✓ {job}")
    print(f"  https://{REGION}.console.aws.amazon.com/sagemaker/home?region={REGION}#/jobs/{job}")
    return job

if __name__ == "__main__":
    role_arn = get_role_arn()

    print("Packaging training script...")
    src_original = package_and_upload("original")
    src_cropped = package_and_upload("cropped")

    print("\nSubmitting jobs...")
    submit_job("dinov2-full-ft-original", "original", "full-ft-original-v2", src_original, role_arn)
    submit_job("dinov2-full-ft-cropped", "cropped", "full-ft-cropped-v2", src_cropped, role_arn)

    print("\n提交完成。用以下命令查看状态：")
    print(f"  aws sagemaker list-training-jobs --region {REGION} --status-equals InProgress")
