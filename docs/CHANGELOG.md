# 变更日志

## [0.3.2] - 2026-03-21

### 修复

- **appsink `emit-signals=true` 导致帧不流**：对比 ipc-kvs-demo 源码发现，demo 的 appsink 通过 `g_signal_connect("new-sample", ...)` 连接了回调来消费帧，而我们的 appsink 设了 `emit-signals=true` 但没有连接任何信号处理器。未被消费的信号在 GMainContext 上无限排队，阻塞了整个 context 的 dispatch，导致 kvssink 的 putMedia 回调也无法被调度。改为 `emit-signals=false`（当前不需要从 appsink 读取帧）。
- **移除 GMainLoop**：demo 的主循环就是简单的 `sleep(1)` 循环，没有 `g_main_loop_run`。GStreamer 管道在自己的线程上运行，kvssink 内部处理 putMedia。恢复为简单的 sleep 循环。

### 涉及文件

- `src/pipeline/gstreamer_pipeline.cpp` — appsink emit-signals=false
- `src/main.cpp` — 恢复简单 sleep 循环，移除 GMainLoop

---

## [0.3.1] - 2026-03-21

### 修复

- **主线程改用 `g_main_loop_run()` 替代 `g_main_context_iteration` 循环**：`g_main_context_iteration(ctx, FALSE)` 每次调用都 acquire/release default context，在间隙中其他线程可能抢占 context，导致 libcamerasrc 和 kvssink 的回调调度不稳定。改为直接在主线程跑 `g_main_loop_run()`（和 `gst-launch-1.0` 完全一致），用单独的 shutdown_watcher 线程监听信号并 `g_main_loop_quit`。

### 涉及文件

- `src/main.cpp` — 主事件循环改为 g_main_loop_run

---

## [0.3.0] - 2026-03-21

### 修复

- **appsink 阻塞管道 PAUSED→PLAYING 状态转换**：根因分析发现 `gst_element_set_state(PLAYING)` 返回 ASYNC 后永远不完成。GStreamer 在 PAUSED→PLAYING 转换时要求每个元素完成状态变更，appsink 在 PAUSED 状态会等待 preroll buffer 被消费后才能转到 PLAYING。由于 WebRTC 和 AI 分支的 appsink 没有连接 `new-sample` 信号处理器（没有人 pull sample），preroll buffer 永远不被消费，导致整个管道卡在 ASYNC 状态。添加 `async=false` 让 appsink 不参与异步状态转换，同时给 ai_sink 补上 `max-buffers=2 drop=true` 防止内存泄漏。

### 涉及文件

- `src/pipeline/gstreamer_pipeline.cpp` — appsink 添加 async=false + drop=true

---

## [0.2.9] - 2026-03-21

### 修复

- **移除 bus_thread_ GMainLoop，只靠主线程 pump default context**：bus_thread_ 的 `g_main_loop_run()` 和主线程的 `g_main_context_iteration()` 竞争同一个 default GMainContext 的 ownership，导致主线程拿不到 context，kvssink 的异步回调仍然无法被调度。移除 bus_thread_ 和 GMainLoop，只保留 `gst_bus_add_watch` 注册到 default context，由主线程的 `g_main_context_iteration` 统一 pump。

### 涉及文件

- `include/pipeline/gstreamer_pipeline.h` — 移除 GMainLoop/bus_thread_ 成员
- `src/pipeline/gstreamer_pipeline.cpp` — start/stop/destroy 简化

---

## [0.2.8] - 2026-03-21

### 修复

- **主线程 pump GMainContext 解决 kvssink ASYNC 死锁**：kvssink 的 PAUSED→PLAYING 状态转换和 putMedia 回调都注册在 GLib default GMainContext 上。之前主线程只是 `sleep(200ms)` 循环，default context 从未被 iterate，导致 kvssink 的异步回调永远不被调度。`gst-launch-1.0` 能工作是因为它在主线程跑 `g_main_loop_run()` 来 pump default context。改为在主事件循环中用 `g_main_context_iteration(ctx, FALSE)` 非阻塞 iterate，匹配 `gst-launch-1.0` 的行为。

### 涉及文件

- `src/main.cpp` — 主事件循环改为 pump GMainContext（HAS_GSTREAMER 条件编译）

---

## [0.2.7] - 2026-03-21

### 修复

- **移除 `gst_element_get_state` 阻塞等待**：`gst_element_get_state` 阻塞调用线程 30 秒等待 PAUSED→PLAYING 转换，但 kvssink 的状态转换需要 GMainLoop 的 default context 被 iterate 才能完成。阻塞等待导致死锁——管道永远卡在 PAUSED。移除等待，让 ASYNC 状态转换通过 bus_thread_ 上的 GMainLoop 自然完成。

### 涉及文件

- `src/pipeline/gstreamer_pipeline.cpp` — 移除 gst_element_get_state 阻塞

---

## [0.2.6] - 2026-03-21

### 修复

- **iot-certificate 改回 gst_parse_launch 内联设置**：`gst-launch-1.0` 测试证实 kvssink 的 `iot-certificate` 属性可以通过 GstStructure 序列化格式在 parse 字符串中设置（`iot-certificate="iot-certificate, iot-thing-name=(string)xxx, ..."`）。之前用 `gst_structure_new` + `g_object_set` 的方式虽然在 READY→PAUSED 时 credential fetch 成功了，但帧不流。改为内联设置后与 `gst-launch-1.0` 的行为完全一致。移除了 post-parse 的 `g_object_set` 代码。

### 验证

- `sudo gst-launch-1.0` 用相同管道描述成功上传视频到 KVS：15fps, 2062 Kbps, RECEIVED + PERSISTED ACK 确认。

### 涉及文件

- `src/pipeline/gstreamer_pipeline.cpp` — iot-certificate 内联到 pipeline description

---

## [0.2.5] - 2026-03-21

### 修复

- **等待 ASYNC 状态转换完成**：`gst_element_set_state(PLAYING)` 返回 `GST_STATE_CHANGE_ASYNC` 时，管道还没真正进入 PLAYING 状态。之前直接继续执行，导致帧可能无法流入 kvssink。现在用 `gst_element_get_state` 等待最多 30 秒让状态转换完成。
- **释放 mutex 再调用 set_state**：避免 kvssink 内部回调与我们的锁产生死锁。
- **发现 kvssink credential fetch 失败根因**：`gst-launch-1.0` 测试暴露了 `unable to set private key file` 错误——私钥文件权限问题。systemd 下以 root 运行可以读取，但 ASYNC 状态转换未等待完成导致帧不流。

### 涉及文件

- `src/pipeline/gstreamer_pipeline.cpp` — start() 等待 ASYNC + 释放锁

---

## [0.2.4] - 2026-03-21

### 修复

- **GMainLoop 必须在 set_state(PLAYING) 之前启动**：kvssink 在状态转换过程中会触发异步回调（IoT credential fetch 等），这些回调依赖 GMainLoop 已经在运行。之前的顺序是先 set_state 再启动 bus_thread_，导致回调无法被调度，帧无法流入 kvssink。调整为：先 `g_main_loop_new` + `gst_bus_add_watch` + 启动 bus_thread_，再 `gst_element_set_state(PLAYING)`。新增 state change return value 诊断日志。

### 涉及文件

- `src/pipeline/gstreamer_pipeline.cpp` — start() 中 GMainLoop 启动顺序调整

---

## [0.2.3] - 2026-03-21

### 修复

- **KVS 管道无视频上传（GMainLoop 缺失）**：`gst-launch-1.0` 测试确认 libcamerasrc → x264enc → fakesink 链路正常出帧，但 smart-camera 进程中管道静默不动。根因：kvssink 依赖 GLib 事件循环（GMainLoop）来调度异步回调（credential refresh、putMedia、bus 消息等），我们的 `start()` 只调用了 `gst_element_set_state(PLAYING)` 但没有启动 GMainLoop，导致 kvssink 内部回调无法被调度，帧无法流入。参考 ipc-kvs-demo 的 `start()` 实现，添加 `g_main_loop_new` + `gst_bus_add_watch` + 专用 bus thread 跑 `g_main_loop_run`。`stop()` 和 `destroy()` 中正确 quit/join/unref。

### 涉及文件

- `include/pipeline/gstreamer_pipeline.h` — 新增 `GMainLoop*`、`std::thread bus_thread_`、`bus_callback` 成员
- `src/pipeline/gstreamer_pipeline.cpp` — start/stop/destroy 集成 GMainLoop + bus watch

---

## [0.2.2] - 2026-03-21

### 修复

- **KVS 管道无视频上传**：参考 ipc-kvs-demo 工作管道，发现三个关键差异导致 kvssink 收不到帧：
  1. 原管道在 tee 之前编码（所有分支共享一个 H.264 流），KVS 分支又做 `byte-stream → avc` 转换导致 kvssink 卡住。改为 tee 分发原始视频，每个分支独立编码（匹配 demo 架构）。
  2. 缺少 `aws-region` 和 `frame-timecodes=true` 属性。
  3. 缺少 `videoconvert ! videoscale`（libcamerasrc 输出的 NV12/Rec709 colorimetry 可能不被编码器直接接受）。
- **iot-certificate 改用 `gst_structure_new`**：从 `gst_structure_from_string` 改为 `gst_structure_new`（与 demo 一致），直接传递 IoT 配置字段而非预格式化字符串，更可靠。
- **PipelineConfig 拆分 IoT 字段**：移除 `kvs_iot_certificate` 字符串字段，改为独立的 `iot_thing_name`、`iot_credential_endpoint`、`iot_cert_path`、`iot_key_path`、`iot_ca_path`、`iot_role_alias` 字段，新增 `kvs_region` 字段。

### 涉及文件

- `include/pipeline/gstreamer_pipeline.h` — PipelineConfig 字段重构
- `src/pipeline/gstreamer_pipeline.cpp` — 管道架构重写 + gst_structure_new
- `src/main.cpp` — 传递独立 IoT 字段和 region
- `tests/test_gstreamer_pipeline.cpp` — 测试适配新字段

---

## [0.2.1] - 2026-03-21

### 修复

- **kvssink `iot-certificate` 属性设置失败**：`iot-certificate` 是 `GstStructure` 类型属性，不能通过 `gst_parse_launch` 字符串语法设置。之前将其作为引号字符串嵌入管道描述，导致树莓派上 `could not set property "iot-certificate"` 错误并崩溃重启。改为：管道描述中省略该属性，解析后通过 `find_element_by_factory("kvssink")` 获取元素引用，再用 `gst_structure_from_string()` + `g_object_set()` 程序化设置。
- **`build_iot_certificate_string` 格式改为 GstStructure 序列化格式**：从 `iot-thing-name=xxx,endpoint=xxx,...` 改为 `iot-certificate, iot-thing-name=(string)xxx, endpoint=(string)xxx, ...`，兼容 `gst_structure_from_string()` 解析。

### 涉及文件

- `src/pipeline/gstreamer_pipeline.cpp` — 移除 parse 字符串中的 iot-certificate，改为 post-parse g_object_set
- `src/stream/stream_uploader.cpp` — GstStructure 序列化格式
- `include/stream/stream_uploader.h` — 注释更新
- `tests/test_stream_uploader.cpp` — 测试适配新格式
- `tests/test_gstreamer_pipeline.cpp` — 新增 KVS 配置字段测试
- `.kiro/steering/tech.md` — 更新 gotcha 说明

---

## [0.2.0] - 2026-03-21

### 新功能

- **GStreamer 管道集成 kvssink 直接上传**：当 IoT 证书和 KVS 流名称配置有效时，GStreamer 管道的 KVS 分支直接使用 `kvssink` 元素上传视频到 AWS Kinesis Video Streams，无需通过 appsink 中转。kvssink 使用 `iot-certificate` 属性进行 IoT 认证，`stream-format=avc` 格式封装 H.264 数据。未配置时回退到 `fakesink`。

### 涉及文件

- `include/pipeline/gstreamer_pipeline.h` — PipelineConfig 新增 KVS 字段
- `src/pipeline/gstreamer_pipeline.cpp` — 管道描述构建器集成 kvssink
- `src/main.cpp` — 传递 IoT 证书和 KVS 配置到管道

---

## [0.1.7] - 2026-03-21

### 修复

- **systemd watchdog 30 秒超时杀进程**：`WatchdogSec=30s` 在 `Type=simple` 下仍需 `sd_notify("WATCHDOG=1")` 心跳，未实现导致 30 秒后被 SIGABRT。暂时注释掉 `WatchdogSec`，待 sd_notify 集成后再启用。
- **kvssink 在 systemd 环境下找不到 libKinesisVideoProducer.so**：`gst-plugin-scan` 加载 kvssink 时缺少 Producer SDK 的 .so 路径。添加 `LD_LIBRARY_PATH` 环境变量指向 SDK build 目录。
- **ProtectHome=true 阻止读取 kvs-producer-sdk-cpp**：SDK .so 在 `/home/pi/` 下，`ProtectHome=true` 会阻止访问。改为 `ProtectHome=read-only`。

### 涉及文件

- `deploy/smart-camera.service`

---

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
