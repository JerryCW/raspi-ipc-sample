# AWS 基础设施部署指南

本文档包含端云协同事件检测系统所需的全部 AWS 资源部署命令。所有资源部署在 `ap-southeast-1` 区域。

> **注意：** 请将以下命令中的 `<ACCOUNT_ID>` 替换为你的 AWS 账户 ID。

---

## 目录

1. [Lambda IAM 角色和权限策略](#1-lambda-iam-角色和权限策略)
2. [Lambda 函数部署](#2-lambda-函数部署)
3. [S3 Event Notification 配置](#3-s3-event-notification-配置)
4. [SageMaker IAM 角色和模型部署](#4-sagemaker-iam-角色和模型部署)

---

## 1. Lambda IAM 角色和权限策略

### 1.1 创建信任策略文件

```bash
cat > /tmp/lambda-trust-policy.json << 'EOF'
{
  "Version": "2012-10-17",
  "Statement": [{
    "Effect": "Allow",
    "Principal": {"Service": "lambda.amazonaws.com"},
    "Action": "sts:AssumeRole"
  }]
}
EOF
```

### 1.2 创建 IAM Role

```bash
aws iam create-role \
  --role-name smart-camera-cloud-verifier-role \
  --assume-role-policy-document file:///tmp/lambda-trust-policy.json \
  --region ap-southeast-1
```

### 1.3 附加基础 Lambda 执行策略（CloudWatch Logs）

```bash
aws iam attach-role-policy \
  --role-name smart-camera-cloud-verifier-role \
  --policy-arn arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole
```

### 1.4 创建自定义内联策略

自定义策略包含以下权限：
- **S3**: `GetObject`/`PutObject` — 读取截图和更新元数据（`smart-camera-captures/captures/*`）
- **S3**: `ListBucket` — 桶级别权限，缺少时 GetObject 对不存在的 key 返回 AccessDenied 而非 404
- **DynamoDB**: `PutItem`/`GetItem` — 写入和查询事件记录（`smart-camera-events`）
- **SageMaker**: `InvokeEndpoint` — 调用鸟类分类端点（`bird-classifier-endpoint`）

```bash
cat > /tmp/cloud-verifier-policy.json << 'EOF'
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": ["s3:GetObject", "s3:PutObject"],
      "Resource": "arn:aws:s3:::smart-camera-captures/captures/*"
    },
    {
      "Effect": "Allow",
      "Action": "s3:ListBucket",
      "Resource": "arn:aws:s3:::smart-camera-captures"
    },
    {
      "Effect": "Allow",
      "Action": ["dynamodb:PutItem", "dynamodb:GetItem"],
      "Resource": "arn:aws:dynamodb:ap-southeast-1:<ACCOUNT_ID>:table/smart-camera-events"
    },
    {
      "Effect": "Allow",
      "Action": "sagemaker:InvokeEndpoint",
      "Resource": "arn:aws:sagemaker:ap-southeast-1:<ACCOUNT_ID>:endpoint/bird-classifier-endpoint"
    }
  ]
}
EOF

aws iam put-role-policy \
  --role-name smart-camera-cloud-verifier-role \
  --policy-name cloud-verifier-permissions \
  --policy-document file:///tmp/cloud-verifier-policy.json
```

---

## 2. Lambda 函数部署

### 2.1 打包 Lambda 代码

```bash
cd cloud/lambda
zip cloud-verifier.zip cloud_verifier.py
cd ../..
```

### 2.2 创建 Lambda 函数

- **Runtime**: Python 3.12
- **Handler**: `cloud_verifier.lambda_handler`
- **Timeout**: 120 秒（考虑 SageMaker 冷启动）
- **Memory**: 256 MB
- **环境变量**: `SAGEMAKER_ENDPOINT_NAME`、`DYNAMODB_TABLE`、`DEVICE_ID`

```bash
aws lambda create-function \
  --function-name cloud-verifier \
  --runtime python3.12 \
  --handler cloud_verifier.lambda_handler \
  --role arn:aws:iam::<ACCOUNT_ID>:role/smart-camera-cloud-verifier-role \
  --zip-file fileb://cloud/lambda/cloud-verifier.zip \
  --timeout 120 \
  --memory-size 256 \
  --environment "Variables={SAGEMAKER_ENDPOINT_NAME=bird-classifier-endpoint,DYNAMODB_TABLE=smart-camera-events,DEVICE_ID=smart-camera-001}" \
  --region ap-southeast-1
```

### 2.3 授权 S3 调用 Lambda

```bash
aws lambda add-permission \
  --function-name cloud-verifier \
  --statement-id s3-trigger \
  --action lambda:InvokeFunction \
  --principal s3.amazonaws.com \
  --source-arn arn:aws:s3:::smart-camera-captures \
  --region ap-southeast-1
```

---

## 3. S3 Event Notification 配置

配置 S3 桶在 `captures/` 前缀下有 `_metadata.json` 文件创建时触发 Lambda 函数。

> **注意：** 后缀必须是 `_metadata.json` 而非 `.json`，避免 Lambda 写回 `_verified.json` 时触发自身形成循环。

### 3.1 创建通知配置文件

```bash
cat > /tmp/s3-notification.json << 'EOF'
{
  "LambdaFunctionConfigurations": [{
    "LambdaFunctionArn": "arn:aws:lambda:ap-southeast-1:<ACCOUNT_ID>:function:cloud-verifier",
    "Events": ["s3:ObjectCreated:*"],
    "Filter": {
      "Key": {
        "FilterRules": [
          {"Name": "prefix", "Value": "captures/"},
          {"Name": "suffix", "Value": "_metadata.json"}
        ]
      }
    }
  }]
}
EOF
```

### 3.2 应用通知配置

```bash
aws s3api put-bucket-notification-configuration \
  --bucket smart-camera-captures \
  --notification-configuration file:///tmp/s3-notification.json \
  --region ap-southeast-1
```

---

## 4. SageMaker IAM 角色和模型部署

### 4.1 创建 SageMaker 执行角色信任策略

```bash
cat > /tmp/sagemaker-trust-policy.json << 'EOF'
{
  "Version": "2012-10-17",
  "Statement": [{
    "Effect": "Allow",
    "Principal": {"Service": "sagemaker.amazonaws.com"},
    "Action": "sts:AssumeRole"
  }]
}
EOF
```

### 4.2 创建 SageMaker IAM Role

```bash
aws iam create-role \
  --role-name smart-camera-sagemaker-role \
  --assume-role-policy-document file:///tmp/sagemaker-trust-policy.json
```

### 4.3 附加 SageMaker 和 S3 策略

```bash
# SageMaker 完整访问权限（创建模型、端点等）
aws iam attach-role-policy \
  --role-name smart-camera-sagemaker-role \
  --policy-arn arn:aws:iam::aws:policy/AmazonSageMakerFullAccess

# S3 只读权限（读取模型文件）
aws iam attach-role-policy \
  --role-name smart-camera-sagemaker-role \
  --policy-arn arn:aws:iam::aws:policy/AmazonS3ReadOnlyAccess
```

### 4.4 运行模型部署脚本

完成上述 IAM 角色创建后，运行部署脚本自动完成以下步骤：
1. 从 HuggingFace 下载 `dennisjooo/Birds-Classifier-EfficientNetB2` 模型
2. 打包 `model.tar.gz`（模型权重 + `inference.py` + `requirements.txt`）
3. 上传到 `s3://smart-camera-captures/models/bird-classifier/model.tar.gz`
4. 调用 SageMaker API 创建 Model → EndpointConfig（Serverless, 2048MB, MaxConcurrency=5）→ Endpoint
5. 在 Model Registry 注册模型版本

```bash
python cloud/sagemaker/deploy_model.py
```

---

## 资源汇总

| 资源 | 名称 | 区域 |
|------|------|------|
| IAM Role (Lambda) | `smart-camera-cloud-verifier-role` | 全局 |
| IAM Role (SageMaker) | `smart-camera-sagemaker-role` | 全局 |
| Lambda 函数 | `cloud-verifier` | ap-southeast-1 |
| S3 桶 | `smart-camera-captures` | ap-southeast-1 |
| DynamoDB 表 | `smart-camera-events` | ap-southeast-1 |
| SageMaker Endpoint | `bird-classifier-endpoint` | ap-southeast-1 |
