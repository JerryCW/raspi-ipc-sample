# AWS 资源配置指南

本文档指导如何创建 Smart Camera 项目所需的 AWS 资源。

## 前提条件

- AWS CLI v2 已安装并配置（`aws configure`）
- 有足够的 IAM 权限创建以下资源

## 1. IoT Core 配置（设备端认证）

> TODO: 补充详细步骤

设备端通过 AWS IoT Core X.509 证书进行 mTLS 认证，获取 STS 临时凭证访问 KVS。

需要创建的资源：
- IoT Thing
- IoT 证书（X.509）+ 私钥
- IoT Policy（允许 connect、publish、subscribe）
- IoT Role Alias（映射到 IAM Role，授予 KVS 权限）
- IAM Role（被 Role Alias 引用，包含 KVS Producer + WebRTC 权限）

设备端需要的文件：
- `certificate.pem.crt` — 设备证书
- `private.pem.key` — 私钥
- `AmazonRootCA1.pem` — CA 根证书

设备端配置项（`config.ini`）：
```ini
[iot]
thing_name = raspi-camera-01
credential_endpoint = xxxx.credentials.iot.ap-southeast-1.amazonaws.com
cert_path = /opt/smart-camera/certs/certificate.pem.crt
key_path = /opt/smart-camera/certs/private.pem.key
ca_cert_path = /opt/smart-camera/certs/AmazonRootCA1.pem
role_alias = SmartCameraRoleAlias
```

---

## 2. Cognito 配置（Viewer 前端认证）

Viewer 前端通过 Cognito Hosted UI 登录，获取 AWS 临时凭证直接调用 KVS API。

### 2.1 创建 User Pool

```bash
USER_POOL_ID=$(aws cognito-idp create-user-pool \
  --pool-name "kvs-camera-viewer" \
  --auto-verified-attributes email \
  --username-attributes email \
  --policies '{"PasswordPolicy":{"MinimumLength":8,"RequireUppercase":true,"RequireLowercase":true,"RequireNumbers":true,"RequireSymbols":false}}' \
  --schema '[{"Name":"email","Required":true,"Mutable":true}]' \
  --region ap-southeast-1 \
  --query 'UserPool.Id' \
  --output text)

echo "User Pool ID: $USER_POOL_ID"
```

### 2.2 创建 Hosted UI 域名

```bash
# 域名前缀需全局唯一，建议加上 AWS Account ID
ACCOUNT_ID=$(aws sts get-caller-identity --query Account --output text)

aws cognito-idp create-user-pool-domain \
  --domain "kvs-camera-viewer-${ACCOUNT_ID}" \
  --user-pool-id "$USER_POOL_ID" \
  --region ap-southeast-1
```

### 2.3 创建 App Client

```bash
CLIENT_ID=$(aws cognito-idp create-user-pool-client \
  --user-pool-id "$USER_POOL_ID" \
  --client-name "kvs-viewer-web" \
  --no-generate-secret \
  --explicit-auth-flows ALLOW_REFRESH_TOKEN_AUTH ALLOW_USER_SRP_AUTH \
  --supported-identity-providers COGNITO \
  --callback-urls "http://localhost:5173/callback" "http://localhost:5173" \
  --logout-urls "http://localhost:5173" \
  --allowed-o-auth-flows code \
  --allowed-o-auth-scopes openid profile email \
  --allowed-o-auth-flows-user-pool-client \
  --region ap-southeast-1 \
  --query 'UserPoolClient.ClientId' \
  --output text)

echo "Client ID: $CLIENT_ID"
```

### 2.4 创建 Identity Pool

```bash
IDENTITY_POOL_ID=$(aws cognito-identity create-identity-pool \
  --identity-pool-name "kvs_camera_viewer_pool" \
  --no-allow-unauthenticated-identities \
  --cognito-identity-providers \
    ProviderName="cognito-idp.ap-southeast-1.amazonaws.com/${USER_POOL_ID}",ClientId="${CLIENT_ID}",ServerSideTokenCheck=false \
  --region ap-southeast-1 \
  --query 'IdentityPoolId' \
  --output text)

echo "Identity Pool ID: $IDENTITY_POOL_ID"
```

### 2.5 创建 IAM Role（认证用户）

```bash
# 创建信任策略
cat > /tmp/cognito-trust-policy.json << EOF
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Principal": { "Federated": "cognito-identity.amazonaws.com" },
      "Action": "sts:AssumeRoleWithWebIdentity",
      "Condition": {
        "StringEquals": {
          "cognito-identity.amazonaws.com:aud": "${IDENTITY_POOL_ID}"
        },
        "ForAnyValue:StringLike": {
          "cognito-identity.amazonaws.com:amr": "authenticated"
        }
      }
    }
  ]
}
EOF

# 创建 Role
ROLE_ARN=$(aws iam create-role \
  --role-name "KVSViewerCognitoAuthRole" \
  --assume-role-policy-document file:///tmp/cognito-trust-policy.json \
  --query 'Role.Arn' \
  --output text)

echo "Role ARN: $ROLE_ARN"
```

### 2.6 附加 KVS 权限策略

```bash
cat > /tmp/kvs-viewer-policy.json << EOF
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": [
        "kinesisvideo:DescribeSignalingChannel",
        "kinesisvideo:GetSignalingChannelEndpoint",
        "kinesisvideo:GetIceServerConfig",
        "kinesisvideo:ConnectAsMaster",
        "kinesisvideo:ConnectAsViewer",
        "kinesisvideo:DescribeStream",
        "kinesisvideo:GetDataEndpoint",
        "kinesisvideo:GetHLSStreamingSessionURL",
        "kinesisvideo:ListFragments",
        "kinesisvideo:GetMediaForFragmentList"
      ],
      "Resource": "*"
    }
  ]
}
EOF

aws iam put-role-policy \
  --role-name "KVSViewerCognitoAuthRole" \
  --policy-name "KVSAccessPolicy" \
  --policy-document file:///tmp/kvs-viewer-policy.json
```

### 2.7 关联 Role 到 Identity Pool

```bash
aws cognito-identity set-identity-pool-roles \
  --identity-pool-id "$IDENTITY_POOL_ID" \
  --roles authenticated="$ROLE_ARN" \
  --region ap-southeast-1
```

### 2.8 创建测试用户

```bash
aws cognito-idp admin-create-user \
  --user-pool-id "$USER_POOL_ID" \
  --username "test@example.com" \
  --user-attributes Name=email,Value=test@example.com Name=email_verified,Value=true \
  --temporary-password "TempPass1234" \
  --region ap-southeast-1

# 设置永久密码（跳过首次登录强制修改）
aws cognito-idp admin-set-user-password \
  --user-pool-id "$USER_POOL_ID" \
  --username "test@example.com" \
  --password "Test1234" \
  --permanent \
  --region ap-southeast-1
```

### 2.9 配置 Viewer 环境变量

将以下内容写入 `viewer/.env`：

```bash
COGNITO_DOMAIN="kvs-camera-viewer-${ACCOUNT_ID}"

cat > viewer/.env << EOF
VITE_COGNITO_USER_POOL_ID=${USER_POOL_ID}
VITE_COGNITO_CLIENT_ID=${CLIENT_ID}
VITE_COGNITO_IDENTITY_POOL_ID=${IDENTITY_POOL_ID}
VITE_COGNITO_DOMAIN=${COGNITO_DOMAIN}.auth.ap-southeast-1.amazoncognito.com
VITE_COGNITO_REDIRECT_URI=http://localhost:5173
VITE_AWS_REGION=ap-southeast-1
VITE_DEFAULT_CHANNEL_NAME=raspi_camera_channel
VITE_DEFAULT_STREAM_NAME=raspi_camera_stream
EOF
```

### 2.10 验证

```bash
# 启动 Viewer
cd viewer && npm run dev

# 浏览器打开 http://localhost:5173
# 使用 test@example.com / Test1234 登录
```

---

## 3. ECR 镜像仓库

### 3.1 创建 ECR 仓库

```bash
ECR_URI=$(aws ecr create-repository \
  --repository-name kvs-camera-viewer \
  --region ap-southeast-1 \
  --query 'repository.repositoryUri' \
  --output text)

echo "ECR URI: $ECR_URI"
```

### 3.2 构建并推送 Docker 镜像

```bash
ACCOUNT_ID=$(aws sts get-caller-identity --query Account --output text)
REGION=ap-southeast-1
ECR_URI="${ACCOUNT_ID}.dkr.ecr.${REGION}.amazonaws.com/kvs-camera-viewer"

# 登录 ECR
aws ecr get-login-password --region $REGION | docker login --username AWS --password-stdin "${ACCOUNT_ID}.dkr.ecr.${REGION}.amazonaws.com"

# 构建镜像（从 viewer/ 目录，通过 --build-arg 注入 Vite 环境变量）
cd viewer

docker build \
  --build-arg VITE_COGNITO_USER_POOL_ID=<your-user-pool-id> \
  --build-arg VITE_COGNITO_CLIENT_ID=<your-client-id> \
  --build-arg VITE_COGNITO_IDENTITY_POOL_ID=<your-identity-pool-id> \
  --build-arg VITE_COGNITO_DOMAIN=<your-cognito-domain>.auth.ap-southeast-1.amazoncognito.com \
  --build-arg VITE_COGNITO_REDIRECT_URI=https://<your-domain> \
  -t kvs-camera-viewer .

# 打标签并推送
docker tag kvs-camera-viewer:latest "${ECR_URI}:latest"
docker push "${ECR_URI}:latest"

cd ..
```

> 注意：Vite 环境变量在构建时注入（`import.meta.env` 是编译时替换），不是运行时。每次修改环境变量需要重新构建镜像。

---

## 4. ECS Fargate 部署

### 4.1 创建 ECS 集群

```bash
aws ecs create-cluster \
  --cluster-name kvs-viewer-cluster \
  --region ap-southeast-1
```

### 4.2 创建 ECS 任务执行 Role

```bash
# 信任策略
cat > /tmp/ecs-trust-policy.json << 'EOF'
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Principal": { "Service": "ecs-tasks.amazonaws.com" },
      "Action": "sts:AssumeRole"
    }
  ]
}
EOF

# 创建 Role
TASK_EXEC_ROLE_ARN=$(aws iam create-role \
  --role-name KVSViewerECSTaskExecutionRole \
  --assume-role-policy-document file:///tmp/ecs-trust-policy.json \
  --query 'Role.Arn' \
  --output text)

# 附加 ECS 任务执行策略（拉取 ECR 镜像 + CloudWatch 日志）
aws iam attach-role-policy \
  --role-name KVSViewerECSTaskExecutionRole \
  --policy-arn arn:aws:iam::aws:policy/service-role/AmazonECSTaskExecutionRolePolicy

echo "Task Execution Role ARN: $TASK_EXEC_ROLE_ARN"
```

### 4.3 创建 CloudWatch 日志组

```bash
aws logs create-log-group \
  --log-group-name /ecs/kvs-camera-viewer \
  --region ap-southeast-1
```

### 4.4 注册任务定义

```bash
ACCOUNT_ID=$(aws sts get-caller-identity --query Account --output text)
REGION=ap-southeast-1

cat > /tmp/task-definition.json << EOF
{
  "family": "kvs-camera-viewer",
  "networkMode": "awsvpc",
  "requiresCompatibilities": ["FARGATE"],
  "cpu": "256",
  "memory": "512",
  "executionRoleArn": "arn:aws:iam::${ACCOUNT_ID}:role/KVSViewerECSTaskExecutionRole",
  "containerDefinitions": [
    {
      "name": "viewer",
      "image": "${ACCOUNT_ID}.dkr.ecr.${REGION}.amazonaws.com/kvs-camera-viewer:latest",
      "portMappings": [
        { "containerPort": 3000, "protocol": "tcp" }
      ],
      "environment": [
        { "name": "PORT", "value": "3000" },
        { "name": "AWS_REGION", "value": "ap-southeast-1" }
      ],
      "logConfiguration": {
        "logDriver": "awslogs",
        "options": {
          "awslogs-group": "/ecs/kvs-camera-viewer",
          "awslogs-region": "${REGION}",
          "awslogs-stream-prefix": "viewer"
        }
      },
      "healthCheck": {
        "command": ["CMD-SHELL", "wget --no-verbose --tries=1 --spider http://localhost:3000/health || exit 1"],
        "interval": 30,
        "timeout": 3,
        "startPeriod": 5,
        "retries": 3
      },
      "essential": true
    }
  ]
}
EOF

aws ecs register-task-definition \
  --cli-input-json file:///tmp/task-definition.json \
  --region $REGION
```

### 4.5 创建 ECS 服务

需要一个 VPC 子网和安全组。使用默认 VPC：

```bash
# 获取默认 VPC ID
VPC_ID=$(aws ec2 describe-vpcs \
  --filters Name=isDefault,Values=true \
  --region ap-southeast-1 \
  --query 'Vpcs[0].VpcId' \
  --output text)

# 获取公有子网（取第一个）
SUBNET_ID=$(aws ec2 describe-subnets \
  --filters Name=vpc-id,Values=$VPC_ID Name=default-for-az,Values=true \
  --region ap-southeast-1 \
  --query 'Subnets[0].SubnetId' \
  --output text)

# 创建安全组（开放 3000 端口）
SG_ID=$(aws ec2 create-security-group \
  --group-name kvs-viewer-sg \
  --description "KVS Camera Viewer - port 3000" \
  --vpc-id $VPC_ID \
  --region ap-southeast-1 \
  --query 'GroupId' \
  --output text)

aws ec2 authorize-security-group-ingress \
  --group-id $SG_ID \
  --protocol tcp \
  --port 3000 \
  --cidr 0.0.0.0/0 \
  --region ap-southeast-1

# 创建 ECS 服务
aws ecs create-service \
  --cluster kvs-viewer-cluster \
  --service-name kvs-camera-viewer \
  --task-definition kvs-camera-viewer \
  --desired-count 1 \
  --launch-type FARGATE \
  --network-configuration "awsvpcConfiguration={subnets=[$SUBNET_ID],securityGroups=[$SG_ID],assignPublicIp=ENABLED}" \
  --region ap-southeast-1
```

### 4.6 更新部署（推送新镜像后）

```bash
# 强制重新部署（拉取最新 :latest 镜像）
aws ecs update-service \
  --cluster kvs-viewer-cluster \
  --service kvs-camera-viewer \
  --force-new-deployment \
  --region ap-southeast-1
```

### 4.7 查看运行状态

```bash
# 查看服务状态
aws ecs describe-services \
  --cluster kvs-viewer-cluster \
  --services kvs-camera-viewer \
  --region ap-southeast-1 \
  --query 'services[0].{status:status,running:runningCount,desired:desiredCount}'

# 获取任务公网 IP
TASK_ARN=$(aws ecs list-tasks \
  --cluster kvs-viewer-cluster \
  --service-name kvs-camera-viewer \
  --region ap-southeast-1 \
  --query 'taskArns[0]' \
  --output text)

ENI_ID=$(aws ecs describe-tasks \
  --cluster kvs-viewer-cluster \
  --tasks $TASK_ARN \
  --region ap-southeast-1 \
  --query 'tasks[0].attachments[0].details[?name==`networkInterfaceId`].value' \
  --output text)

PUBLIC_IP=$(aws ec2 describe-network-interfaces \
  --network-interface-ids $ENI_ID \
  --region ap-southeast-1 \
  --query 'NetworkInterfaces[0].Association.PublicIp' \
  --output text)

echo "Viewer URL: http://${PUBLIC_IP}:3000"
```

---

## 5. AI 视频活动摘要资源

活动事件功能需要 S3 存储桶（截图）、DynamoDB 表（事件记录）和 ECS 任务角色权限。

### 5.1 创建 S3 存储桶（活动截图）

```bash
aws s3api create-bucket \
  --bucket smart-camera-captures \
  --region ap-southeast-1 \
  --create-bucket-configuration LocationConstraint=ap-southeast-1
```

### 5.2 创建 DynamoDB 表（活动事件）

```bash
# 创建表：PK=device_id, SK=event_timestamp
aws dynamodb create-table \
  --table-name smart-camera-events \
  --attribute-definitions \
    AttributeName=device_id,AttributeType=S \
    AttributeName=event_timestamp,AttributeType=S \
  --key-schema \
    AttributeName=device_id,KeyType=HASH \
    AttributeName=event_timestamp,KeyType=RANGE \
  --billing-mode PAY_PER_REQUEST \
  --region ap-southeast-1

# 等待表创建完成
aws dynamodb wait table-exists --table-name smart-camera-events --region ap-southeast-1

# 启用 TTL（90 天自动过期）
aws dynamodb update-time-to-live \
  --table-name smart-camera-events \
  --time-to-live-specification Enabled=true,AttributeName=expiry_ttl \
  --region ap-southeast-1
```

### 5.3 更新 ECS 任务角色权限

给 `raspi-camera-task-role` 添加 DynamoDB 和 S3 访问权限：

```bash
aws iam put-role-policy \
  --role-name raspi-camera-task-role \
  --policy-name raspi-camera-events-policy \
  --policy-document '{
    "Version": "2012-10-17",
    "Statement": [
      {
        "Effect": "Allow",
        "Action": ["dynamodb:Query", "dynamodb:GetItem", "dynamodb:PutItem"],
        "Resource": "arn:aws:dynamodb:ap-southeast-1:823092283330:table/smart-camera-events"
      },
      {
        "Effect": "Allow",
        "Action": ["s3:GetObject", "s3:PutObject"],
        "Resource": "arn:aws:s3:::smart-camera-captures/*"
      },
      {
        "Effect": "Allow",
        "Action": ["kinesisvideo:GetDataEndpoint", "kinesisvideo:GetClip"],
        "Resource": "*"
      }
    ]
  }'
```

### 5.4 更新 ECS 任务定义

任务定义需要包含以下环境变量和 taskRoleArn：

```bash
# 环境变量（在 containerDefinitions.environment 中）：
# COGNITO_USER_POOL_ID=ap-southeast-1_dN28pXdRp
# COGNITO_REGION=ap-southeast-1
# DYNAMODB_TABLE=smart-camera-events
# S3_BUCKET=smart-camera-captures
# DEVICE_ID=raspi-camera-01

# taskRoleArn: arn:aws:iam::823092283330:role/raspi-camera-task-role
```

### 5.5 Cloud Verifier Lambda 角色权限

`smart-camera-cloud-verifier-role` 需要以下权限：

```bash
# S3 读写（截图下载 + 验证结果写回）
aws iam put-role-policy \
  --role-name smart-camera-cloud-verifier-role \
  --policy-name cloud-verifier-s3-policy \
  --policy-document '{
    "Version": "2012-10-17",
    "Statement": [
      {
        "Effect": "Allow",
        "Action": ["s3:GetObject", "s3:PutObject"],
        "Resource": "arn:aws:s3:::smart-camera-captures/*"
      },
      {
        "Effect": "Allow",
        "Action": "s3:ListBucket",
        "Resource": "arn:aws:s3:::smart-camera-captures"
      }
    ]
  }'
```

> 注意：`s3:ListBucket` 作用于 bucket 级别（不带 `/*`），`s3:GetObject/PutObject` 作用于对象级别（带 `/*`）。缺少 `s3:ListBucket` 会导致 GetObject 对不存在的 key 返回 AccessDenied 而非 404。

### 5.6 资源清理

```bash
# 删除 DynamoDB 表
aws dynamodb delete-table --table-name smart-camera-events --region ap-southeast-1

# 清空并删除 S3 桶
aws s3 rm s3://smart-camera-captures --recursive
aws s3api delete-bucket --bucket smart-camera-captures --region ap-southeast-1

# 删除 IAM 内联策略
aws iam delete-role-policy --role-name raspi-camera-task-role --policy-name raspi-camera-events-policy
```

---

## 资源清理

```bash
# ===== ECS / ECR =====

# 删除 ECS 服务
aws ecs update-service --cluster kvs-viewer-cluster --service kvs-camera-viewer --desired-count 0 --region ap-southeast-1
aws ecs delete-service --cluster kvs-viewer-cluster --service kvs-camera-viewer --force --region ap-southeast-1

# 删除 ECS 集群
aws ecs delete-cluster --cluster kvs-viewer-cluster --region ap-southeast-1

# 删除 ECR 仓库（含所有镜像）
aws ecr delete-repository --repository-name kvs-camera-viewer --force --region ap-southeast-1

# 删除 ECS 任务执行 Role
aws iam detach-role-policy --role-name KVSViewerECSTaskExecutionRole --policy-arn arn:aws:iam::aws:policy/service-role/AmazonECSTaskExecutionRolePolicy
aws iam delete-role --role-name KVSViewerECSTaskExecutionRole

# 删除安全组
aws ec2 delete-security-group --group-id $SG_ID --region ap-southeast-1

# 删除 CloudWatch 日志组
aws logs delete-log-group --log-group-name /ecs/kvs-camera-viewer --region ap-southeast-1

# ===== Cognito =====
aws cognito-identity set-identity-pool-roles \
  --identity-pool-id "$IDENTITY_POOL_ID" \
  --roles '{}' \
  --region ap-southeast-1

# 删除 IAM Role 策略和 Role
aws iam delete-role-policy --role-name KVSViewerCognitoAuthRole --policy-name KVSAccessPolicy
aws iam delete-role --role-name KVSViewerCognitoAuthRole

# 删除 Identity Pool
aws cognito-identity delete-identity-pool --identity-pool-id "$IDENTITY_POOL_ID" --region ap-southeast-1

# 删除 User Pool Domain
aws cognito-idp delete-user-pool-domain --domain "kvs-camera-viewer-${ACCOUNT_ID}" --user-pool-id "$USER_POOL_ID" --region ap-southeast-1

# 删除 User Pool
aws cognito-idp delete-user-pool --user-pool-id "$USER_POOL_ID" --region ap-southeast-1
```
