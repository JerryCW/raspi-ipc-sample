# Tech Stack & Build System

## 系统架构

```
┌─────────────────────────────────────────────────────────────────────┐
│  Raspberry Pi 5 (Linux aarch64)                                     │
│                                                                     │
│  ┌─────────────┐    ┌──────────────────────────────────────────┐   │
│  │ USB Camera   │    │  smart-camera (C++17)                    │   │
│  │ (IMX678)     │───▶│                                          │   │
│  │ /dev/IMX678  │    │  v4l2src → jpegdec → videoconvert        │   │
│  └─────────────┘    │       │                                   │   │
│                      │       ▼                                   │   │
│                      │    raw_t (tee)                             │   │
│                      │    ├─▶ x264enc → h264parse → h264_t (tee) │   │
│                      │    │   ├─▶ h264parse(avc) → kvssink ──────┼───┼──▶ AWS KVS
│                      │    │   └─▶ appsink(webrtc_sink) ──────────┼───┼──▶ WebRTC Viewers
│                      │    │                                      │   │
│                      │    └─▶ videoconvert(BGR) → appsink(ai_sink)│  │
│                      │            │                               │   │
│                      │            ▼                               │   │
│                      │     FrameBufferPool → AIPipeline           │   │
│                      │            │                               │   │
│                      │            ▼                               │   │
│                      │     FrameExporter (shm + unix socket)      │   │
│                      └────────────┼──────────────────────────────┘   │
│                                   │ IPC                              │
│                      ┌────────────▼──────────────────────────────┐   │
│                      │  activity-detector (Python)                │   │
│                      │  YOLO → 检测 → S3 上传 → DynamoDB          │   │
│                      └───────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│  AWS Cloud (ap-southeast-1)                                         │
│                                                                     │
│  ┌──────────────┐  ┌──────────────┐  ┌────────────────────────┐    │
│  │ KVS          │  │ IoT Core     │  │ S3 (截图)              │    │
│  │ (录像回放)    │  │ (X.509 mTLS) │  │   ↓                    │    │
│  └──────────────┘  └──────────────┘  │ Lambda → SageMaker     │    │
│                                       │   ↓                    │    │
│  ┌──────────────┐  ┌──────────────┐  │ DynamoDB (事件)        │    │
│  │ ECS (Viewer) │  │ Cognito      │  └────────────────────────┘    │
│  │ raspi-eye-web│  │ (用户认证)    │                                │
│  └──────────────┘  └──────────────┘                                │
└─────────────────────────────────────────────────────────────────────┘
```

## GStreamer 管线结构（单编码器两级 tee）

```
v4l2src(/dev/IMX678)
  → image/jpeg,720p@15fps
  → jpegdec
  → videoconvert
  → videoscale
  → video/x-raw,I420,720p@15fps
  → tee(raw_t)
     ├─▶ queue → x264enc(ultrafast,threads=2) → h264parse
     │     → video/x-h264,byte-stream → tee(h264_t)
     │        ├─▶ queue → h264parse → video/x-h264,avc → kvssink (KVS 云录像)
     │        └─▶ queue → appsink:webrtc_sink (WebRTC 实时预览)
     │
     └─▶ queue → videoconvert → video/x-raw,BGR → appsink:ai_sink
           → FrameBufferPool → FrameExporter(shm) → Python YOLO
```

## Language & Standard
- C++17 (required minimum)
- Error handling via `Result<T>` type (no exceptions) — all public APIs return `Result<T>`
- Thread safety via `std::shared_mutex` for shared data
- RAII for all GStreamer objects and AWS SDK resources
- Move semantics (`std::move`) for frame ownership transfer — no unnecessary copies

## Build System
- CMake with platform detection (macOS x86_64, macOS aarch64, Linux aarch64)
- Conditional compilation macros: `HAS_KVS_WEBRTC_SDK`, `HAS_LIBCAMERA`, `HAS_V4L2`, `HAS_SYSTEMD`
- Dependencies resolved via `find_package`: GStreamer, OpenSSL, CURL, KVSWebRTC (optional)
- Build artifacts include semantic version + Git commit hash

## Core Libraries & Frameworks
- GStreamer: media pipeline (capture → encode → distribute)
- AWS KVS Producer SDK: video upload to Kinesis Video Streams (kvssink GStreamer element)
- AWS KVS WebRTC C SDK: real-time signaling and media (Linux only; stub on macOS)
- libcurl: HTTPS requests for IoT credential provider and endpoint health checks
- OpenSSL: X.509 certificate validation, mTLS, private key handling
- libcamera: CSI camera access on Raspberry Pi (Linux aarch64 only)
- V4L2: USB camera access (Linux only)
- systemd (sd-daemon): watchdog heartbeat and service integration (Linux only)

## Common Commands

```bash
# Configure (from project root)
cmake -B device/build -S device -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build device/build

# Run tests
cmake --build device/build --target test
# or
ctest --test-dir device/build --output-on-failure

# Install on Raspberry Pi (Linux aarch64)
sudo ./device/scripts/install.sh

# Uninstall
sudo ./device/scripts/uninstall.sh
```

## Viewer 部署流程（ECS via CodeBuild）

AWS 资源名称统一使用 `raspi-eye` 前缀：
- ECR 仓库: `raspi-eye`
- ECS 集群: `raspi-eye-cluster`
- ECS 服务: `raspi-eye-web`
- CodeBuild 项目: `raspi-eye-build`
- Region: `ap-southeast-1`

### 部署步骤

CodeBuild 源是 S3（不是 GitHub），需要先手动打包上传代码再触发构建。

```bash
# 1. 打包 viewer 源码为 zip（在项目根目录执行）
zip -r viewer-source.zip viewer/ -x "viewer/node_modules/*" "viewer/dist/*" "viewer/.env"

# 2. 上传到 S3（CodeBuild 源桶）
aws s3 cp viewer-source.zip s3://kvs-viewer-build-823092/viewer-source.zip --region ap-southeast-1

# 3. 触发 CodeBuild
aws codebuild start-build --project-name raspi-eye-build --region ap-southeast-1
```

CodeBuild 会自动完成以下流程（由 `viewer/buildspec.yml` 定义）：
1. 从 S3 下载 `viewer-source.zip` 并解压
2. 在 `viewer/` 目录下执行 `docker build`（多阶段构建：Vite 前端编译 → Node.js 生产镜像）
3. Push 镜像到 ECR `raspi-eye:latest`
4. 更新 ECS 服务 `raspi-eye-web`，触发滚动部署

整个过程约 2-3 分钟，ECS 滚动更新再 1-2 分钟。

### 查看构建状态

```bash
# 查看最近一次构建状态
aws codebuild list-builds-for-project --project-name raspi-eye-build --region ap-southeast-1 --max-items 1

# 查看具体构建详情
aws codebuild batch-get-builds --ids <build-id> --region ap-southeast-1 --query 'builds[0].buildStatus'
```

### 手动部署（不走 CodeBuild）

如果需要跳过 CodeBuild 直接部署：

```bash
# 1. 登录 ECR
aws ecr get-login-password --region ap-southeast-1 | docker login --username AWS --password-stdin 823092283330.dkr.ecr.ap-southeast-1.amazonaws.com

# 2. 在 viewer/ 目录构建镜像（需要传入 Vite 环境变量）
docker build -t raspi-eye \
  --build-arg VITE_COGNITO_USER_POOL_ID=<value> \
  --build-arg VITE_COGNITO_CLIENT_ID=<value> \
  --build-arg VITE_COGNITO_IDENTITY_POOL_ID=<value> \
  --build-arg VITE_COGNITO_DOMAIN=<value> \
  --build-arg VITE_COGNITO_REDIRECT_URI=<value> \
  viewer/

# 3. Tag + Push 到 ECR
docker tag raspi-eye:latest 823092283330.dkr.ecr.ap-southeast-1.amazonaws.com/raspi-eye:latest
docker push 823092283330.dkr.ecr.ap-southeast-1.amazonaws.com/raspi-eye:latest

# 4. 强制 ECS 重新部署
aws ecs update-service --cluster raspi-eye-cluster --service raspi-eye-web --force-new-deployment --region ap-southeast-1
```

### Dockerfile 说明

`viewer/Dockerfile` 使用两阶段构建：
- Stage 1 (`build`): `node:20` 全量镜像，`npm ci` + `npm run build`（Vite 编译前端，VITE_* 环境变量通过 `--build-arg` 注入）
- Stage 2 (`production`): `node:20-alpine` 精简镜像，仅包含 `server/` + `dist/`，以 `node` 用户运行
- 健康检查: `GET /health` 每 30 秒一次
- 端口: 3000

### TODO: 完整 CI/CD（GitHub → CodeBuild 自动触发）

当前 CodeBuild 源是 S3，需要手动打 zip 上传。后续改为：
- 把 `raspi-eye-build` 的 source type 从 S3 改为 GitHub（`https://github.com/JerryCW/raspi-ipc-sample.git`）
- 启用 Webhook，监听 `main` 分支 push 事件
- buildspec.yml 路径调整（GitHub 源拉整个 repo，不只是 viewer/）
- 改完后 `git push` 即自动触发构建部署，不再需要手动打 zip

## Key Gotchas (from real-world experience)
- GStreamer element names from `gst_parse_launch` are unpredictable (e.g. `x264enc0`). Get element refs via `gst_bin_iterate_elements` + factory name match.
- kvssink `storage-size` is already in MB — do NOT multiply by 1024.
- kvssink `iot-certificate` is a `GstStructure` property — it CANNOT be set via `gst_parse_launch` string syntax. Build the pipeline without it, then use `gst_structure_from_string()` + `g_object_set()` on the kvssink element after parsing. Format: `"iot-certificate, iot-thing-name=(string)xxx, endpoint=(string)xxx, ..."`.
- `CURL_SSLVERSION_MAX_TLSv1_3` correct value is `(7 << 16)`, NOT `(4 << 16)`.
- WebRTC `payloadLen` must use `STRLEN` (no null terminator) or ICE negotiation fails.
- WebRTC needs audio transceiver + OPUS codec even when not sending audio.
- WebRTC transceiver direction must be `SENDRECV`; only send frames to `CONNECTED` peers.
- Raspberry Pi 5 CSI driver is `rp1-cfe` (Pi 4 is `bcm2835-unicam`) — detect both.
- GCC 12 strict mode requires explicit `#include <mutex>` in all files using mutex types.
- AWS IoT cert validation: use `X509_V_FLAG_PARTIAL_CHAIN`; full chain validation happens server-side during mTLS.
- kvssink 的 video sink pad 要求 `stream-format=avc`（源码确认：`videosink_templ` 定义为 `video/x-h264, stream-format=(string)avc, alignment=(string)au`）。而 WebRTC appsink 需要 `byte-stream`。两级 tee 共享编码器时，h264_t tee 输出 `byte-stream`，kvssink 分支需要额外加一个 `h264parse` 做 `byte-stream → avc` 格式转换（只是 NAL 单元重新打包，CPU 开销接近零）。之前误以为 kvssink 不参与 caps 协商，实际上它是标准的 GStreamer 元素，只是 caps 要求和 WebRTC 不同。
- `avdec_mjpeg`（GStreamer 1.22 / libav）的 sink caps 要求 `parsed: true`，必须在前面加 `jpegparse`。但 `jpegparse` 对某些 USB 摄像头（如 DECXIN/IMX678）的 JPEG APP0 段不兼容，每帧刷 warning。且 GStreamer 1.22 的 `avdec_mjpeg` 没有 `max-threads` 属性（更高版本才有）。在树莓派 5 + GStreamer 1.22 环境下，`jpegdec` 是更稳定的选择。
- `gst_bin_get_by_name` 返回的元素引用（refcount+1）不能在线程还在使用该指针时 `gst_object_unref`。如果 appsink 指针被传给后台线程，必须在线程退出后才能 unref，否则指针悬空导致 `invalid unclassed pointer in cast to GstAppSink`。
- Linux `/dev/videoX` 编号不固定，每次重启或 USB 拔插都可能变。用 udev 规则创建稳定符号链接：`SUBSYSTEM=="video4linux", ENV{ID_VENDOR_ID}=="xxxx", ENV{ID_MODEL_ID}=="xxxx", ATTR{index}=="0", SYMLINK+="IMX678"`。注意用 `ENV{}` 匹配而非 `ATTRS{}`，后者需要沿设备树向上匹配，容易失败。
- `build_pipeline_description` 中 V4L2 分支必须包含 `image/jpeg` caps + `jpegdec`。USB 摄像头 YUYV 720p@15fps ≈ 27MB/s 超出 USB 2.0 带宽，只有 MJPG 模式（≈ 3-5MB/s）才能稳定工作。之前 `gst_source_description()` 里写了但 `build_pipeline_description` 没用它，导致 v4l2src 尝试 YUYV 协商失败。
- USB 3.0 环境下 YUYV 比 MJPG 更省 CPU：省掉了摄像头端 JPEG 压缩 + CPU 端 jpegdec 解码。`gst-launch-1.0 v4l2src ! video/x-raw,format=YUY2,... ! videoconvert ! fakesink` 实测 CPU 更低。但 YUYV 在高分辨率下受限（4K 可能只有 5fps），720p@15fps 没问题。如果后续提到 1080p@30fps 以上，需要切回 MJPG。

### TODO: 端云协同推理（Edge-Cloud Collaborative Inference）

端侧 YOLO 低阈值粗筛 → 截图上传 S3 → 云端精确验证 → 确认后写 DynamoDB。

方案：
1. 端侧降低置信度阈值（0.15~0.20），增加召回率
2. S3 上传完成后触发 Lambda
3. Lambda 调云端模型做二次验证，确认是真检测才写 DynamoDB，否则丢弃
4. 前端只展示云端确认过的事件

### TODO: 部署脚本支持 S3 直接引用（避免重复上传）

当前 `deploy_model.py` 只支持本地 `--model-path`，训练产物已在 S3 上时会出现"S3 下载到本地 → 打包 → 再上传 S3"的冗余流程。优化方案：
- `--model-path` 支持 `s3://` URI，直接从 S3 拉取模型权重打包（或在 S3 上直接组装）
- 更好的方案：训练脚本输出时直接打包推理代码（`inference.py`、`handler_*.py`、`model_config.json`）到 `model.tar.gz`，训练完即可直接部署
- 可考虑用 SageMaker Model Registry 管理模型版本，部署时直接引用 Model Package ARN

### TODO: SageMaker 鸟类识别（Bird Species Classification）

用开源模型 + SageMaker 实现鸟类品种识别，作为端云协同的云端验证环节。

方案：
- 方案 A（快速上手）：直接部署 HuggingFace 预训练鸟类分类模型（如 `dennisjooo/Birds-Classifier-EfficientNetB2`，500+ 种鸟）到 SageMaker Serverless Inference，不需要训练数据
- 方案 B（学习 Training）：用公开数据集（CUB-200-2011 / NABirds / iNaturalist 鸟类子集）在 SageMaker Training Job 上 fine-tune ViT/DINOv2 模型
- 部署：SageMaker Serverless Inference Endpoint（按调用计费，空闲不收费）
- 集成：S3 截图上传 → Lambda → SageMaker endpoint → 结果写回 DynamoDB

学习目标：SageMaker Training Jobs、Model Registry、Endpoint 部署、Serverless Inference
