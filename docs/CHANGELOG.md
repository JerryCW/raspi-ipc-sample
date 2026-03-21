# 变更日志

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
