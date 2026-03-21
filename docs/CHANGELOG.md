# 变更日志

## [0.1.6] - 2026-03-21

### 修复

- **kvssink 插件在 sudo/systemd 下找不到**：GStreamer 插件搜索路径在 sudo 环境下丢失，导致 `gst_element_factory_find("kvssink")` 返回 null。在 systemd service 文件中添加 `Environment=GST_PLUGIN_SYSTEM_PATH` 解决。

### 涉及文件

- `deploy/smart-camera.service`

---

## [0.1.5] - 2026-03-21

### 修复

- **IoT 证书链验证过严**：AWS IoT 设备证书由中间 CA 签发，本地用 `AmazonRootCA1.pem`（根 CA）做 `X509_verify_cert` 即使加了 `X509_V_FLAG_PARTIAL_CHAIN` 也会失败。改为验证失败时记录 WARNING 而非拒绝启动，真正的链验证在 mTLS 握手时由 AWS IoT Core 服务端完成。
- **缺少 `<iostream>` 头文件**：上述改动引入了 `std::cerr` 但未包含 `<iostream>`，导致编译失败。

### 测试

- 新增 `IoTAuthenticatorChain` 测试套件（3 个测试），使用 openssl CLI 生成真实 X.509 证书验证链验证行为：
  - `ValidChain_InitializeSucceeds`：设备证书 + 匹配 CA → 初始化成功
  - `MismatchedCA_InitializeSucceeds`：设备证书由 CA-A 签发，root CA 为 CA-B → 初始化成功（仅警告）
  - `SelfSignedCert_InitializeSucceeds`：自签名证书同时作为设备证书和 CA → 初始化成功

### 涉及文件

- `src/auth/iot_authenticator.cpp`
- `tests/test_iot_authenticator.cpp`

---

## [0.1.4] - 2026-03-21

### 修复

- **GCC 12 命名空间污染**：WebRTC SDK 头文件 `Include.h` 被放在 `namespace sc { }` 内部 include，导致 `std::abs`、`std::string` 等标准库符号被解析为 `sc::std::abs`，引发大量编译错误。将 SDK include 移到文件顶部、namespace 之外。

### 涉及文件

- `src/webrtc/webrtc_agent.cpp`

---

## [0.1.3] - 2026-03-21

### 修复

- **WebRTC SDK 头文件路径**：SDK 的头文件分布在 `src/include/`（webrtcclient）和 `open-source/include/`（依赖库）两个目录，之前只搜索了 `include/` 导致找不到 `com/amazonaws/kinesis/video/webrtcclient/Include.h`。

### 涉及文件

- `CMakeLists.txt`

---

## [0.1.2] - 2026-03-21

### 修复

- **CMake 语法错误**：`message()` 中误用了 bash 变量展开语法 `${VAR:+...}`，CMake 不支持冒号字符，导致树莓派上配置失败。
- **WebRTC SDK 链接失败**：手动路径检测找到 SDK 后，webrtc_agent 仍尝试链接不存在的 `KVSWebRTC::KVSWebRTC` imported target。改为先检查 imported target 是否存在，不存在则用 `find_library` 找到的实际库路径链接。

### 涉及文件

- `CMakeLists.txt`

---

## [0.1.1] - 2026-03-21

### 修复

- **GCC 12 编译错误**：`include/control/bitrate_controller.h` 缺少 `#include <condition_variable>`。macOS clang 隐式包含该头文件，但 GCC 12 严格模式要求显式包含，导致树莓派上编译失败。

### 改进

- **KVS WebRTC SDK 检测**：CMakeLists.txt 新增路径扫描逻辑，自动检测 `/opt/amazon-kinesis-video-streams-webrtc-sdk-c` 目录下的 SDK，不再仅依赖 `find_package(KVSWebRTC)`。
- **install.sh 依赖补全**：新增 `libsystemd-dev`（systemd watchdog 心跳集成）和 `libcamera-dev`（CSI 摄像头支持）两个依赖包。
- **install.sh SDK 路径传递**：构建时自动扫描常见 SDK 安装路径，通过 `CMAKE_PREFIX_PATH` 传给 CMake。

### 涉及文件

- `include/control/bitrate_controller.h`
- `CMakeLists.txt`
- `scripts/install.sh`

---

## [0.1.0] - 2026-03-21

### 初始版本

完整实现 14 个模块、408 个单元测试：

- ConfigManager、LogManager、IoTAuthenticator
- CameraSource（libcamera/V4L2/videotestsrc）
- GStreamerPipeline、StreamUploader、WebRTCAgent
- BitrateController、ConnectionMonitor/StreamModeFSM
- HealthMonitor、Watchdog
- FrameBufferPool、VideoProducer、AIPipeline、PipelineOrchestrator
- ResourceGuard、ShutdownHandler
- main.cpp 主程序入口
- systemd 服务单元、install/uninstall 脚本、默认配置文件
