#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include "ai/ai_pipeline.h"
#include "core/types.h"

namespace sc {

// ============================================================
// FrameExporterConfig — configuration for FrameExporter
// ============================================================

struct FrameExporterConfig {
    std::string shm_name = "/smart_camera_frames";   // POSIX shm name
    size_t shm_size = 20 * 1024 * 1024;              // 20MB (ping-pong double buffer)
    std::string socket_path = "/tmp/smart_camera_ai.sock";
    double target_fps = 2.0;                          // sampling frequency
};

// ============================================================
// SharedMemoryHeader — 64-byte header at start of shm region
// ============================================================

struct SharedMemoryHeader {
    uint32_t magic = 0x46524D45;   // "FRME"
    uint32_t version = 1;
    uint32_t buffer_size = 0;
    uint8_t active_buffer = 0;     // 0 or 1
    uint8_t padding[51] = {};
};

// ============================================================
// FrameNotification — message sent over Unix Socket (JSON)
// ============================================================

struct FrameNotification {
    std::string shm_name;
    uint8_t buffer_index;
    size_t offset;
    size_t size;
    uint32_t width;
    uint32_t height;
    std::string pixel_format;
    uint64_t timestamp_ms;
    uint64_t sequence;
};

// ============================================================
// FrameExporter — IAIModel that exports frames via IPC
//   Writes frames to POSIX shared memory (ping-pong) and
//   notifies a Python process over Unix Domain Socket.
// ============================================================

class FrameExporter : public IAIModel {
public:
    explicit FrameExporter(const FrameExporterConfig& config);
    ~FrameExporter() override;

    // IAIModel interface
    std::string name() const override;  // returns "frame_exporter"
    Result<std::vector<AnalysisResult>> analyze(const FrameBuffer& frame) override;

    // Stats accessors
    uint64_t frames_skipped() const;
    uint64_t frames_exported() const;

private:
    // Shared memory management
    VoidResult init_shared_memory();
    void cleanup_shared_memory();

    // Unix Socket management
    VoidResult init_socket();
    void cleanup_socket();
    VoidResult send_frame_notification(const FrameNotification& msg);

    // Frame rate control
    bool should_sample_frame() const;

    FrameExporterConfig config_;
    int shm_fd_ = -1;
    void* shm_ptr_ = nullptr;
    int server_fd_ = -1;
    int client_fd_ = -1;            // currently connected Python client
    uint8_t current_buffer_ = 0;    // ping-pong: 0 or 1
    std::chrono::steady_clock::time_point last_sample_time_;
    mutable std::mutex socket_mutex_;

    // Counters
    uint64_t frames_skipped_ = 0;
    uint64_t frames_exported_ = 0;
};

}  // namespace sc
