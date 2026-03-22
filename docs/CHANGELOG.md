# 变更日志

## [0.4.0] - 2026-03-22

### 新功能（Viewer）

- **Viewer Rewrite 属性测试与单元测试全覆盖**：完成设计文档中全部 15 个正确性属性的 property-based 测试和所有单元测试，共 118 个测试用例。
- **Cognito 认证环境搭建**：通过 AWS CLI 创建 User Pool、App Client（OAuth2 授权码流程）、Identity Pool（含 KVS 权限 IAM Role），配置 `.env` 环境变量。
- **WebRTC 实时视频连接成功**：修复缺失的 `pc.onicecandidate` 处理器，实现双向 ICE candidate 交换，WebRTC 连接从 `checking` 超时变为正常 `connected`。
- **时间轴重写**：固定当日 00:00-24:00（UTC+8）范围，日期选择器（◀ 日期 ▶ + "今天"按钮），蓝色虚线当前时间指示器，红色圆点播放位置指示器，底部图例。
- **Fragment 预加载**：登录完成后立即在 ViewerPage 层级加载今天的 fragments，切换到录像回放 Tab 时直接显示绿色录像段。

### 改进

- **Tab 切换保持连接状态**：从 key-based remounting 改为 CSS `hidden` 切换，WebRTC/HLS 连接在 Tab 切换时不断开。
- **全局 UTC+8 24 小时制**：时间轴标签、调试日志时间戳、统计面板"当前时间"统一使用 UTC+8。
- **HLS 播放位置准确映射**：`stats.currentTime` 从 `new Date()`（当前时间）改为 `playbackStartRef + video.currentTime`，准确映射到实际录像时间点。
- **ListFragments 分页**：添加 `NextToken` 循环分页 + `MaxResults: 1000`，获取完整 fragment 列表。
- **WebSocket 空消息过滤**：跳过 KVS 信令 WebSocket 的空帧/心跳消息，消除 JSON 解析错误。
- **Express 5 路由兼容**：通配符路由从 `app.get('*', ...)` 改为 `app.get('/{*splat}', ...)`，添加 `/api/*` 显式 404 处理器。

### 修复

- **`computeRefreshDelay` 浮点精度 bug**：属性测试发现 `Math.floor(remaining * 0.9)` 与 `remaining - remaining * 0.1` 在某些整数值下不等。
- **缺失 `pc.onicecandidate`**：Viewer 未发送本地 ICE candidate 给 MASTER，导致 ICE 协商单向失败。

### 涉及文件

- `viewer/src/hooks/useWebRTC.ts` — onicecandidate、空消息过滤
- `viewer/src/hooks/useHLS.ts` — playbackStartRef、播放位置修正
- `viewer/src/services/kvs.ts` — ListFragments 分页
- `viewer/src/components/Timeline.tsx` — 完全重写（固定日范围、日期选择器、指示器）
- `viewer/src/components/HLSPanel.tsx` — 预加载 fragments、UTC+8 时间
- `viewer/src/components/TabView.tsx` — CSS hidden 替代 key remounting
- `viewer/src/components/WebRTCPanel.tsx` — UTC+8 日志时间戳
- `viewer/src/pages/ViewerPage.tsx` — fragment 预加载逻辑
- `viewer/server/index.js` — Express 5 路由兼容
- `viewer/src/__tests__/` — 15 个属性测试 + 单元测试文件
- `viewer/.env` — Cognito 环境变量配置

---

## [0.3.1] - 2026-03-22

### 修复（树莓派真机调试）

- **SDK 头文件缺失**：`webrtc_agent.h` 在 `#ifdef HAS_KVS_WEBRTC_SDK` 块中使用 SDK 类型但未 include SDK 头文件，macOS 跳过但 RPi 编译失败。添加条件 `#include <com/amazonaws/kinesis/video/webrtcclient/Include.h>`。
- **SDK include 路径和编译宏 PRIVATE→PUBLIC**：`HAS_KVS_WEBRTC_SDK` 和 SDK include 路径仅对 `webrtc_agent` target 可见，`smart-camera` 和测试 target 编译失败。改为 PUBLIC 传播。
- **`get_pipeline_element()` 纯虚函数导致 StubPipeline 抽象**：RPi 有 GStreamer，纯虚方法使测试中的 StubPipeline 无法实例化。改为带默认实现（返回 nullptr）的虚方法。
- **SDK API 名称不匹配**：`createSignalingClientInfo()` 不存在（改为 MEMSET 直接初始化）、`createSignalingSync()` → `createSignalingClientSync()`、`on_ice_candidate` 返回类型 STATUS → void。
- **`channelRoleType` 未设置**：SDK 发送 `"Role": "UNKOWN"` 导致 400 错误。添加 `SIGNALING_CHANNEL_ROLE_TYPE_MASTER`。
- **SSL CA 证书路径缺失**：SDK 的 libwebsockets 无法验证 AWS 服务 TLS 证书。设置 `channelInfo.pCertPath`。
- **凭证提供者 nullptr**：`createStaticCredentialProvider` 替换为 `createLwsIotCredentialProvider`（匹配 ipc-kvs-demo 参考实现），通过 IoT X.509 证书直接获取 STS 凭证。
- **SDP 反序列化缺失**：直接传原始 payload 给 `setRemoteDescription()` 导致 `0x40100001` 错误。添加 `deserializeSessionDescriptionInit()` / `serializeSessionDescriptionInit()`。
- **缺少 `addSupportedCodec()`**：Peer Connection 创建后未注册支持的编解码器，导致 SDP 协商失败。添加 H264 + OPUS codec 注册。
- **链接缺少 `libkvsCommonLws`**：`createStaticCredentialProvider` / `createLwsIotCredentialProvider` 在 `libkvsCommonLws.so` 中，CMake 未链接。

### 新功能

- **`--webrtc-only` 调试模式**：跳过 GStreamer/KVS/监控模块，仅运行 WebRTC 信令，每 3 秒打印连接状态和 viewer 数量，方便树莓派上独立调试信令连接。
- **WebRTCConfig 新增 IoT 证书字段**：`iot_credential_endpoint`、`iot_cert_path`、`iot_key_path`、`iot_ca_cert_path`、`iot_role_alias`、`iot_thing_name`，从 `AppConfig.iot` 自动填充。

### 经验教训

- macOS stub 编译通过不代表 RPi SDK 路径能编译——条件编译块中的类型、API 名称、回调签名必须在真机上验证。
- KVS WebRTC C SDK 的 API 与文档/示例有差异，以实际 `Include.h` 和 SDK sample 代码为准。
- `createLwsIotCredentialProvider` 是 IoT 设备的正确凭证方式，`createStaticCredentialProvider` 的 STS token 会过期且不自动刷新。
- SDP 必须通过 SDK 的 `deserializeSessionDescriptionInit` / `serializeSessionDescriptionInit` 处理，不能直接传原始字符串。

### 涉及文件

- `include/webrtc/webrtc_agent.h` — SDK include、回调签名修复
- `include/pipeline/gstreamer_pipeline.h` — `get_pipeline_element()` 默认实现
- `include/config/config_manager.h` — WebRTCConfig IoT 字段
- `src/webrtc/webrtc_agent.cpp` — 凭证提供者、SDP 序列化、addSupportedCodec、channelRoleType
- `src/pipeline/gstreamer_pipeline.cpp` — `get_pipeline_element()` 实现
- `src/main.cpp` — `--webrtc-only` 模式、IoT 字段填充
- `CMakeLists.txt` — PUBLIC 传播、kvsCommonLws 链接

---

## [0.3.0] - 2026-03-22

### 新功能

- **KVS WebRTC 实时直播集成**：完成 WebRTCAgent 的 KVS WebRTC C SDK 真实实现，替换骨架代码。
  - SDK 初始化与信令客户端创建（initKvsWebRtc、createSignalingSync、MASTER 角色注册）
  - 信令连接与指数退避重连（signalingClientFetchSync、signalingClientConnectSync、2s→60s 退避）
  - Peer Connection 创建与 SDP/ICE 协商（H.264 视频 + OPUS 音频 transceiver、SENDRECV 方向）
  - H.264 帧分发（send_frame() 遍历 CONNECTED peers 调用 writeFrame()，shared_lock 读锁）
  - 凭证自动刷新（getCredentialsFn 回调，剩余 <5 分钟触发 force_refresh()）
  - 优雅关闭（6 步顺序：断开信令→关闭 peers→sleep 1s→释放 peers→释放信令→反初始化 SDK）
- **main.cpp 帧拉取线程**：GStreamer appsink pull 模式，从 webrtc_sink 拉取 H.264 帧并调用 send_frame()，无 Viewer 时跳过以节省 CPU。
- **RapidCheck 属性测试**：10 个正确性属性的 11 个属性测试，覆盖指数退避、容量上限、STRLEN、Viewer 计数、帧分发、凭证刷新、关闭一致性、stub no-op。

### 改进

- IWebRTCAgent 接口新增 send_frame() 方法
- PeerCallbackContext 结构体关联 C SDK 回调与 C++ 对象
- WebRTC_FramePull 关闭步骤确保帧拉取在信令之前停止
- 条件编译兼容：macOS stub 模式 481 个测试全部通过

### 涉及文件

- `include/webrtc/webrtc_agent.h` — 接口变更、PeerCallbackContext、SDK 私有成员
- `src/webrtc/webrtc_agent.cpp` — SDK 路径完整实现
- `src/main.cpp` — start_signaling()、帧拉取线程、关闭顺序
- `tests/test_webrtc_agent.cpp` — 11 个属性测试 + 7 个 send_frame 单元测试
- `CMakeLists.txt` — RapidCheck FetchContent 集成
- `tests/CMakeLists.txt` — rapidcheck 链接
- `docs/DEVLOG.md` — 开发日志合并

---

## [0.2.0] - 2026-03-22

### 新功能

- **GStreamer 管道集成 kvssink 直接上传**：当 IoT 证书和 KVS 流名称配置有效时，GStreamer 管道直接使用 `kvssink` 元素上传视频到 AWS Kinesis Video Streams。

### 管道架构（参考 ipc-kvs-demo）

- 原始视频经 `videoconvert ! videoscale` 后通过 `tee` 分发到三个分支，每个分支独立编码
- KVS 分支：`x264enc ! h264parse ! kvssink`（byte-stream 直接进 kvssink）
- WebRTC 分支：`x264enc ! h264parse ! appsink`（byte-stream，async=false emit-signals=false）
- AI 分支：`appsink`（原始视频，async=false emit-signals=false drop=true）
- kvssink 属性：`aws-region`、`frame-timecodes=true`、`iot-certificate` 内联 GstStructure 格式

### 修复

- **x264enc `option-string="--profile=baseline"` 导致编码器拒绝所有帧**（根因）：x264enc 不认识 `--profile` 作为 option-string 参数，静默 `rejected caps`，导致零帧输出。改为 `bframes=0`。
- **kvssink `iot-certificate` 是 GstStructure 属性**：最初尝试用引号字符串嵌入管道描述失败（`could not set property`），后发现用 GstStructure 序列化格式 `iot-certificate="iot-certificate, iot-thing-name=(string)xxx, ..."` 可以在 `gst_parse_launch` 中内联设置。
- **appsink `async=false`**：appsink 在 PAUSED→PLAYING 时等待 preroll buffer 被消费，没有信号处理器时永远不完成，导致管道卡在 ASYNC。
- **appsink `emit-signals=false`**：未连接信号处理器时 `emit-signals=true` 无意义。
- **GMainLoop + bus_thread_**：添加 bus watch 和 GMainLoop 用于错误/警告/EOS 监控。
- **`#include <iostream>`**：GCC 12 严格模式需要显式包含。
- **PipelineConfig 拆分 IoT 字段**：`iot_thing_name`、`iot_credential_endpoint`、`iot_cert_path`、`iot_key_path`、`iot_ca_path`、`iot_role_alias`、`kvs_region`。

### 经验教训

- `GST_DEBUG=3` 是定位 GStreamer 管道问题的关键工具——x264enc 的 `rejected caps` 错误只在 debug level 3 才可见。
- `gst-launch-1.0` 测试和程序内 `gst_parse_launch` 的管道描述必须完全一致，包括编码器参数。
- kvssink 的 `iot-certificate` 可以通过 GstStructure 序列化格式在 parse 字符串中设置。

### 涉及文件

- `include/pipeline/gstreamer_pipeline.h` — PipelineConfig 重构 + bus_callback
- `src/pipeline/gstreamer_pipeline.cpp` — 管道架构重写 + GMainLoop + bus watch
- `src/main.cpp` — 传递 IoT 字段和 region
- `tests/test_gstreamer_pipeline.cpp` — KVS 配置字段测试
- `.kiro/steering/tech.md` — 更新 iot-certificate gotcha

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
