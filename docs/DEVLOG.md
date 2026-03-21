# 开发日志

可搜索的开发挑战、决策和解决方案知识库。

## 反复出现的模式 / 系统性问题

_暂无。_

---

## 日志条目

### 2026-03-21 — 任务: 1.1 创建 CMake 项目结构与核心类型定义

**概要：** 创建了 CMake 项目骨架，配置 C++17 标准、平台检测、条件编译宏，通过 find_package 查找依赖，使用 FetchContent 引入 Google Test，并完成核心类型头文件（types.h、stream_mode.h、frame.h）。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** 依赖项（GStreamer、OpenSSL、CURL）在此阶段设为可选——需要它们的模块在添加时再强制要求即可。

**涉及的文件/组件：** CMakeLists.txt, include/core/types.h, include/core/stream_mode.h, include/core/frame.h, tests/CMakeLists.txt, tests/test_types.cpp

---

### 2026-03-21 — 任务: 1.2 为 Result<T> 编写单元测试

**概要：** 验证了 `Result<T>`、`VoidResult` 和 `ErrorCode` 的 27 个单元测试，全部通过。

**遇到的问题：**
- **[类型: 边界情况]：** `ctest --test-dir build -R test_types` 返回"No tests were found"，因为 `gtest_discover_tests` 按 GTest 测试名（如 `ResultInt.OkConstruction`）注册，而非按二进制目标名。不加 `-R` 过滤或直接运行二进制文件则正常。
  - **解决方案/状态：** 非真实问题——仅是 ctest 正则过滤不匹配。通过 `ctest --test-dir build` 可正常发现并运行所有测试。

**经验教训：** 使用 `gtest_discover_tests` 时，ctest 的 `-R` 过滤匹配的是单个测试名，而非可执行目标名。

**涉及的文件/组件：** tests/test_types.cpp（已验证，无需修改）

---

### 2026-03-21 — 任务: 2.1 实现 Config_Manager 核心功能

**概要：** 创建了 `include/config/config_manager.h`（IConfigManager 接口、AppConfig 结构体及辅助类型）和 `src/config/config_manager.cpp`（ConfigManager 实现），包含 INI 格式解析、CLI 参数覆盖、视频预设加载、配置校验和 Pretty Printer。更新了 CMakeLists.txt 添加 config_manager 静态库目标。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** 预设查找使用大小写不敏感匹配，避免用户输入大小写不一致导致的错误。production/development profile 校验差异化处理是关键设计决策。

**涉及的文件/组件：** include/config/config_manager.h, src/config/config_manager.cpp, CMakeLists.txt

---

### 2026-03-21 — 任务: 2.2 为 Config_Manager 编写单元测试

**概要：** 创建了 `tests/test_config_manager.cpp`，包含 34 个单元测试覆盖 INI 解析、CLI 覆盖、预设加载、profile 校验、参数边界和往返一致性。更新了 `tests/CMakeLists.txt`。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** ArgvHelper 辅助类简化了 CLI 参数测试的构造，后续模块测试可复用此模式。

**涉及的文件/组件：** tests/test_config_manager.cpp, tests/CMakeLists.txt

---

### 2026-03-21 — 任务: 3.1 实现 Log_Manager

**概要：** 创建了 `include/logging/log_manager.h` 和 `src/logging/log_manager.cpp`，实现六级结构化 JSON 日志、文件轮转、敏感信息过滤、动态日志级别和性能指标日志。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** 使用 `std::atomic<LogLevel>` 实现无锁动态级别调整，避免热路径上的锁竞争。敏感信息过滤使用 `std::regex`，性能可接受但在极高吞吐场景下可能需要优化为手动匹配。

**涉及的文件/组件：** include/logging/log_manager.h, src/logging/log_manager.cpp, CMakeLists.txt

---

### 2026-03-21 — 任务: 3.2 为 Log_Manager 编写单元测试

**概要：** 创建了 `tests/test_log_manager.cpp`，包含 25 个单元测试覆盖 JSON 格式、时间戳、敏感信息过滤、文件轮转、动态日志级别和性能指标。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** TempDir 辅助类（基于 std::filesystem）简化了文件轮转测试的临时目录管理，后续文件相关测试可复用。

**涉及的文件/组件：** tests/test_log_manager.cpp, tests/CMakeLists.txt

---

### 2026-03-21 — 任务: 4. 检查点 - 基础设施验证

**概要：** 执行基础设施检查点验证。CMake 构建成功，全部 86 个测试通过（27 types + 34 config_manager + 25 log_manager）。

**遇到的问题：**
- **[类型: 性能]：** 日志轮转测试（LogManagerRotation）耗时较长（3s + 9s），因为需要写入 >1MB 数据触发轮转。
  - **解决方案/状态：** 可接受，测试正确性优先。如需加速可考虑降低轮转阈值的测试方式。

**经验教训：** 检查点验证确认了 macOS aarch64 平台上所有基础设施模块正常工作。GStreamer 未安装不影响当前阶段。

**涉及的文件/组件：** 无文件变更，仅验证构建和测试。

---

### 2026-03-21 — 任务: 5.1 实现 IoT_Authenticator

**概要：** 创建了 `include/auth/iot_authenticator.h` 和 `src/auth/iot_authenticator.cpp`，实现 X.509 mTLS 认证、STS 凭证获取与刷新、指数退避重试、证书有效期检查和私钥内存安全。

**遇到的问题：**
- **[类型: 设计决策]：** OpenSSL 和 CURL 通过条件编译隔离，无 CURL 时返回 stub 凭证用于开发环境。
  - **解决方案/状态：** 使用 `#ifdef USE_OPENSSL` / `#ifdef USE_CURL` 条件编译，macOS 开发环境有 OpenSSL 和 CURL 所以完整路径可用。
- **[类型: 设计决策]：** 私钥内存清零使用 `volatile char*` 防止编译器优化掉 memset。
  - **解决方案/状态：** 采用逐字节 volatile 写入，确保安全清零。

**经验教训：** CURL TLS 版本常量 `(7 << 16)` 是关键陷阱，已正确实现。RAII 包装器（X509Ptr、BIOPtr 等）简化了 OpenSSL 资源管理。

**涉及的文件/组件：** include/auth/iot_authenticator.h, src/auth/iot_authenticator.cpp, CMakeLists.txt

---

### 2026-03-21 — 任务: 5.2 为 IoT_Authenticator 编写单元测试

**概要：** 创建了 `tests/test_iot_authenticator.cpp`，包含 23 个单元测试覆盖证书文件校验、PEM 格式检测、权限检查、凭证有效性和常量验证。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** 使用 TempDir + write_file 辅助函数配合 chmod 设置权限，可有效测试文件权限相关逻辑。测试中使用伪造 PEM 内容（含 BEGIN/END 标记）足以验证格式检测，无需真实证书。

**涉及的文件/组件：** tests/test_iot_authenticator.cpp, tests/CMakeLists.txt

---

### 2026-03-21 — 任务: 6.1 实现 Camera_Source 接口与三种视频源

**概要：** 创建了 ICameraSource 接口、videotestsrc（跨平台 SMPTE NV12 生成）、libcamera（条件编译 HAS_LIBCAMERA，Pi 4/5 驱动检测）、V4L2（条件编译 HAS_V4L2）三种实现，以及工厂函数。

**遇到的问题：**
- **[类型: 设计决策]：** videotestsrc 实现不依赖 GStreamer，直接在软件中生成 NV12 SMPTE 彩条，确保 macOS 开发环境可用。
  - **解决方案/状态：** 纯 C++ 实现 NV12 帧生成，gst_source_description() 返回 GStreamer 管道字符串供后续管道构建使用。

**经验教训：** 工厂模式 + 条件编译有效隔离了平台差异。macOS 上仅编译 videotestsrc 和 factory，Linux 上按需添加 libcamera/V4L2。

**涉及的文件/组件：** include/camera/camera_source.h, src/camera/videotestsrc_source.cpp, src/camera/libcamera_source.cpp, src/camera/v4l2_source.cpp, src/camera/camera_source_factory.cpp, CMakeLists.txt

---

### 2026-03-21 — 任务: 6.2 为 Camera_Source 编写单元测试

**概要：** 创建了 `tests/test_camera_source.cpp`，包含 12 个单元测试覆盖工厂创建、videotestsrc 帧获取、序列号递增、能力查询和关闭重开。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** 使用 `#ifndef HAS_LIBCAMERA` / `#ifndef HAS_V4L2` 条件编译测试，确保平台不可用时的错误路径也被覆盖。

**涉及的文件/组件：** tests/test_camera_source.cpp, tests/CMakeLists.txt

---

### 2026-03-21 — 任务: 7.1 实现 GStreamer_Pipeline 核心

**概要：** 创建了 `include/pipeline/gstreamer_pipeline.h` 和 `src/pipeline/gstreamer_pipeline.cpp`，实现 IGStreamerPipeline 接口，包含管道构建、编码器回退、动态码率调整和 RAII 生命周期管理。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** Stub 实现模式（无 GStreamer 时的状态机模拟）确保 macOS 开发环境可编译和测试。

**涉及的文件/组件：** include/pipeline/gstreamer_pipeline.h, src/pipeline/gstreamer_pipeline.cpp, CMakeLists.txt

---

### 2026-03-21 — 任务: 7.2 为 GStreamer_Pipeline 编写单元测试

**概要：** 创建了 `tests/test_gstreamer_pipeline.cpp`，包含 30 个单元测试覆盖管道构建（videotestsrc 三种预设）、编码器回退逻辑、动态码率调整（各状态下的成功/失败）、管道状态转换（完整生命周期、无效转换、重建）和线程安全并发读取。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** 条件编译 `#ifndef HAS_GSTREAMER` 区分 stub 和真实 GStreamer 环境的编码器名称断言，确保测试在两种环境下都有效。

**涉及的文件/组件：** tests/test_gstreamer_pipeline.cpp, tests/CMakeLists.txt

---

### 2026-03-21 — 任务: 8.1 实现 Stream_Uploader

**概要：** 创建了 `include/stream/stream_uploader.h` 和 `src/stream/stream_uploader.cpp`，实现 IStreamUploader 接口，包含 kvssink iot-certificate 属性格式化、指数退避重连（1s→30s）、本地缓存管理和时间顺序上传。同时创建了 `tests/test_stream_uploader.cpp`（17 个测试）。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** `build_iot_certificate_string()` 作为静态方法暴露，方便测试验证 kvssink 属性格式。storage-size 常量命名为 `kDefaultStorageSizeMB` 明确单位，避免误乘 1024。

**涉及的文件/组件：** include/stream/stream_uploader.h, src/stream/stream_uploader.cpp, tests/test_stream_uploader.cpp, CMakeLists.txt, tests/CMakeLists.txt

---

### 2026-03-21 — 任务: 8.2 为 Stream_Uploader 编写单元测试

**概要：** 测试已在任务 8.1 中一并创建（17 个测试），覆盖 iot-certificate 属性格式、storage-size 单位验证、断线重连指数退避逻辑、初始化校验和生命周期管理。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** 实现和测试同步创建可减少上下文切换开销。

**涉及的文件/组件：** tests/test_stream_uploader.cpp（已在 8.1 中创建）

---

### 2026-03-21 — 任务: 9.1 实现 WebRTC_Agent + 9.2 编写单元测试

**概要：** 创建了 `include/webrtc/webrtc_agent.h` 和 `src/webrtc/webrtc_agent.cpp`，实现 IWebRTCAgent 接口，包含 stub（macOS）和真实 SDK（Linux）双路径。同时创建了 `tests/test_webrtc_agent.cpp`（20 个测试）。

**遇到的问题：**
- **[类型: 设计决策]：** 关闭顺序严格按照 disconnect signaling → close peers → sleep 1s → free peers → free signaling → deinit SDK 实现，避免资源泄漏。
  - **解决方案/状态：** `shutdown_sequence()` 方法封装完整关闭流程，析构函数自动调用。
- **[类型: 设计决策]：** `signaling_send_mutex_` 独立于 `shared_mutex`，专门防止并发信令写入。
  - **解决方案/状态：** 两把锁分离关注点：shared_mutex 保护配置/状态，signaling_send_mutex_ 保护信令通道写入。

**经验教训：** WebRTC SDK 的多个关键陷阱（payloadLen STRLEN、audio transceiver、SENDRECV 方向）已在代码注释中标注，确保后续真实 SDK 集成时不遗漏。

**涉及的文件/组件：** include/webrtc/webrtc_agent.h, src/webrtc/webrtc_agent.cpp, tests/test_webrtc_agent.cpp, CMakeLists.txt, tests/CMakeLists.txt

---

### 2026-03-21 — 任务: 10. 检查点 - 核心管道验证

**概要：** 执行核心管道检查点验证。CMake 构建成功，全部 213 个测试通过（types 27 + config_manager 34 + log_manager 25 + iot_authenticator 23 + camera_source 12 + gstreamer_pipeline 30 + stream_uploader 17 + webrtc_agent 20 + 内部测试 25）。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** macOS aarch64 平台上所有核心管道模块（含 stub 实现）正常工作。GStreamer 未安装不影响 stub 模式测试。

**涉及的文件/组件：** 无文件变更，仅验证构建和测试。

---

### 2026-03-21 — 任务: 11.1 实现 Bitrate_Controller

**概要：** 创建了 `include/control/bitrate_controller.h` 和 `src/control/bitrate_controller.cpp`，实现 IBitrateController 接口，包含采样线程、三种码率调整规则（下调/紧急下调/上调）、min/max 钳位和回调通知。添加了 CMake 库目标。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** `evaluate_and_adjust()` 暴露为 public 方法方便单元测试直接调用，避免依赖定时器。`std::atomic<uint32_t>` 用于 current_bitrate 实现无锁读取。

**涉及的文件/组件：** include/control/bitrate_controller.h, src/control/bitrate_controller.cpp, CMakeLists.txt

---

### 2026-03-21 — 任务: 11.2 为 Bitrate_Controller 编写单元测试

**概要：** 创建了 `tests/test_bitrate_controller.cpp`，包含 31 个单元测试覆盖下调/上调/紧急下调逻辑、码率上下限约束、回调通知、边界条件和生命周期管理。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** `evaluate_and_adjust()` 暴露为 public 方法使得测试可以直接调用而不依赖定时器，避免了 30 秒上调等待的测试困难。

**涉及的文件/组件：** tests/test_bitrate_controller.cpp, tests/CMakeLists.txt

---

### 2026-03-21 — 任务: 12.1 实现 Connection_Monitor 与 Stream_Mode 状态机

**概要：** 创建了 StreamModeFSM（4 文件）和 ConnectionMonitor（4 文件），实现端点健康检查、连续失败/成功计数驱动模式转换、CURL 条件编译。

**遇到的问题：**
- **[类型: 设计决策]：** FSM 回调在锁内调用，要求回调函数轻量级。
  - **解决方案/状态：** 文档注释说明回调应轻量，后续可改为异步通知。
- **[类型: 设计决策]：** CURL 不可用时 stub 始终报告端点可达，确保 macOS 开发环境正常。
  - **解决方案/状态：** 与其他模块（IoT_Authenticator、WebRTC_Agent）的 stub 策略一致。

**经验教训：** `check_once()` 暴露为 public 方法方便测试直接调用，避免依赖 30 秒定时器。

**涉及的文件/组件：** include/core/stream_mode_fsm.h, src/core/stream_mode_fsm.cpp, include/monitor/connection_monitor.h, src/monitor/connection_monitor.cpp, CMakeLists.txt

---

### 2026-03-21 — 任务: 12.2 为 Stream_Mode 状态机编写单元测试

**概要：** 创建了 `tests/test_stream_mode_fsm.cpp`（23 个测试）和 `tests/test_connection_monitor.cpp`（14 个测试），覆盖所有状态转换路径、回调通知、阈值逻辑和 FSM 集成。

**遇到的问题：**
- **[类型: 设计决策]：** 使用空 URL 模拟不可达端点，避免依赖真实网络请求。
  - **解决方案/状态：** `check_endpoint("")` 始终返回 false，与 CURL 可用性无关。
- **[类型: 设计决策]：** 使用 start→stop→手动 check_once() 模式避免后台线程竞争。
  - **解决方案/状态：** 与 BitrateController 的 evaluate_and_adjust() 测试模式一致。

**经验教训：** 暴露内部方法（check_once、evaluate_and_adjust）为 public 是测试定时器驱动模块的有效模式，已在多个模块中复用。

**涉及的文件/组件：** tests/test_stream_mode_fsm.cpp, tests/test_connection_monitor.cpp, tests/CMakeLists.txt

---

### 2026-03-21 — 任务: 13.1 + 13.2 Health_Monitor 实现与测试

**概要：** 创建了 HealthMonitor 模块（header + cpp）和 10 个单元测试，实现固定窗口重启限制、错误回调、Watchdog 告警和窗口到期自动重置。

**遇到的问题：**
- **[类型: 设计决策]：** 使用 `mutable std::vector` + prune 模式实现滑动窗口，而非固定窗口起止时间。
  - **解决方案/状态：** prune_old_restarts() 在每次查询时清理过期时间戳，更灵活。
- **[类型: 性能]：** 窗口到期测试需要 sleep 1.1 秒等待窗口过期。
  - **解决方案/状态：** 使用 1 秒短窗口减少等待时间，可接受。

**经验教训：** StubPipeline 模式（实现 IGStreamerPipeline 接口的测试桩）有效隔离了 GStreamer 依赖，后续模块可复用。

**涉及的文件/组件：** include/monitor/health_monitor.h, src/monitor/health_monitor.cpp, tests/test_health_monitor.cpp, CMakeLists.txt, tests/CMakeLists.txt

---

### 2026-03-21 — 任务: 14.1 + 14.2 Watchdog 实现与测试

**概要：** 创建了 Watchdog 模块（header + cpp）和 14 个单元测试，实现错误率滑动窗口、心跳陈旧检测、内存监控、认证失败追踪和保护模式。

**遇到的问题：**
- **[类型: 设计决策]：** 使用 `__process__` 和 `__memory_cleanup__` 特殊模块名区分进程级重启和内存清理回调。
  - **解决方案/状态：** 回调接收方通过模块名判断操作类型，简化接口设计。

**经验教训：** WatchdogConfig 结构体使所有阈值可配置，测试中使用极端值（0 秒窗口、100 字节内存限制）加速验证。

**涉及的文件/组件：** include/monitor/watchdog.h, src/monitor/watchdog.cpp, tests/test_watchdog.cpp, CMakeLists.txt, tests/CMakeLists.txt

---

### 2026-03-21 — 任务: 15. 检查点 - 监控层验证

**概要：** 执行监控层检查点验证。CMake 构建成功，全部 305 个测试通过。12 个库目标全部编译正常。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** 监控层（BitrateController、ConnectionMonitor、StreamModeFSM、HealthMonitor、Watchdog）全部通过验证，可进入扩展层开发。

**涉及的文件/组件：** 无文件变更，仅验证构建和测试。

---

### 2026-03-21 — 任务: 16.1 + 16.2 Frame_Buffer_Pool 实现与测试

**概要：** 创建了 FrameBuffer 类和 FrameBufferPool（header + cpp + 19 个测试），实现预分配缓冲池、移动语义、shared_ptr 自动归还、池满丢弃最旧帧和多线程安全。

**遇到的问题：**
- **[类型: 设计决策]：** shared_ptr 自定义 deleter 回调 return_buffer() 需要获取 mutex_，acquire() 中释放锁后再销毁 shared_ptr 避免死锁。
  - **解决方案/状态：** 采用 lock-release-before-destroy 模式，先移出 to_drop 再释放锁后 reset。

**经验教训：** 自定义 deleter + shared_mutex 组合需要特别注意锁的获取顺序，避免在持锁状态下触发 deleter 导致死锁。

**涉及的文件/组件：** include/buffer/frame_buffer_pool.h, src/buffer/frame_buffer_pool.cpp, tests/test_frame_buffer_pool.cpp, CMakeLists.txt, tests/CMakeLists.txt

---

### 2026-03-21 — 任务: 17.1 + 17.2 Video_Producer 实现与测试

**概要：** 创建了 VideoProducer 模块（header + cpp + 26 个测试），实现 NAL 单元完整性检查、ICE 协商帧丢弃、force_idr 机制和丢弃统计。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** H.264 NAL 解析需要同时支持 3 字节和 4 字节起始码。静态方法 validate_nal_units/is_idr_frame 方便测试和外部调用。

**涉及的文件/组件：** include/pipeline/video_producer.h, src/pipeline/video_producer.cpp, tests/test_video_producer.cpp, CMakeLists.txt, tests/CMakeLists.txt

---

### 2026-03-21 — 任务: 18.1 + 18.2 AI_Pipeline 实现与测试

**概要：** 创建了 AIPipeline 模块（header + cpp + 14 个测试），实现模型注册/注销、独立线程消费帧、超时跳帧和 JSON 结果输出。

**遇到的问题：**
- **[类型: 性能]：** 超时测试需要 SlowModel sleep 3 秒 + 等待 4 秒，是最慢的测试。
  - **解决方案/状态：** std::async future 析构函数会阻塞直到任务完成，这是 C++ 标准行为，可接受。

**经验教训：** std::async + wait_for 实现超时简单但有 future 析构阻塞的代价。生产环境可考虑 detached thread + 取消令牌。

**涉及的文件/组件：** include/ai/ai_pipeline.h, src/ai/ai_pipeline.cpp, tests/test_ai_pipeline.cpp, CMakeLists.txt, tests/CMakeLists.txt

---

### 2026-03-21 — 任务: 19.1 + 19.2 Pipeline_Orchestrator 实现与测试

**概要：** 创建了 PipelineOrchestrator 模块（header + cpp + 12 个测试），实现多管道管理、故障隔离、独立重启和统一状态查询。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** StubPipeline + StubHealthMonitor 测试桩模式在多个模块中复用，有效隔离了 GStreamer 依赖。

**涉及的文件/组件：** include/pipeline/pipeline_orchestrator.h, src/pipeline/pipeline_orchestrator.cpp, tests/test_pipeline_orchestrator.cpp, CMakeLists.txt, tests/CMakeLists.txt

---

### 2026-03-21 — 任务: 20. 检查点 - 扩展层验证

**概要：** 执行扩展层检查点验证。CMake 构建成功，全部 376 个测试通过。20 个库目标全部编译正常。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** 扩展层（FrameBufferPool、VideoProducer、AIPipeline、PipelineOrchestrator）全部通过验证，可进入最终集成阶段。

**涉及的文件/组件：** 无文件变更，仅验证构建和测试。

---

### 2026-03-21 — 任务: 21.1 + 21.2 资源保护与循环缓冲区

**概要：** 创建了 CircularBuffer 和 ResourceGuard 模块（header + cpp + 20 个测试），实现循环缓冲区视频缓存、内存阈值监控和 CPU 配额配置。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** CircularBuffer 的 head/tail/used 三指针设计比 head/size 更直观，wrap-around 写入分两段 memcpy 实现。

**涉及的文件/组件：** include/core/resource_guard.h, src/core/resource_guard.cpp, tests/test_resource_guard.cpp, CMakeLists.txt, tests/CMakeLists.txt

---

### 2026-03-21 — 任务: 22.1 + 22.2 优雅关闭与信号处理

**概要：** 创建了 ShutdownHandler 模块（header + cpp + 12 个测试），实现 SIGINT/SIGTERM 信号处理、有序关闭步骤、15 秒超时保护、关闭摘要日志和缓存持久化回调。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** 信号处理器仅设置 atomic 标志（async-signal-safe），实际关闭逻辑在主线程执行。re-entrant 保护通过 compare_exchange_strong 实现。

**涉及的文件/组件：** include/core/shutdown_handler.h, src/core/shutdown_handler.cpp, tests/test_shutdown_handler.cpp, CMakeLists.txt, tests/CMakeLists.txt

---

### 2026-03-21 — 任务: 23.1 实现 main.cpp 主程序入口

**概要：** 创建了 `src/main.cpp`，按规定顺序初始化所有 14 个模块，注册信号处理器和反序关闭步骤，实现主事件循环。添加了 smart-camera 可执行目标到 CMakeLists.txt。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** 非 production profile 允许 IoT 认证失败后继续运行，方便 macOS 开发环境测试。关闭步骤按初始化反序注册确保依赖关系正确。

**涉及的文件/组件：** src/main.cpp, CMakeLists.txt

---

### 2026-03-21 — 任务: 24.1 Systemd 服务与部署脚本

**概要：** 创建了 systemd 服务单元文件、install.sh、uninstall.sh 和 default.ini 默认配置文件。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** install.sh 不覆盖已存在的 config.ini，避免部署时丢失用户自定义配置。

**涉及的文件/组件：** deploy/smart-camera.service, scripts/install.sh, scripts/uninstall.sh, config/default.ini

---

### 2026-03-21 — 任务: 25. 最终检查点 - 全系统集成验证

**概要：** 执行最终全系统集成验证。CMake 构建成功，smart-camera 可执行文件编译通过，全部 408 个测试通过（0 失败）。21 个库目标 + 1 个可执行目标 + 16 个测试目标全部正常。

**遇到的问题：**
- 顺利完成，未遇到问题。

**经验教训：** 全部 25 个任务（含 5 个检查点）完成。macOS aarch64 开发环境使用 videotestsrc + stub 模式验证了完整系统架构。下一步是在 Raspberry Pi (Linux aarch64) 上进行真实硬件集成测试。

**涉及的文件/组件：** 无文件变更，仅验证构建和测试。

---
