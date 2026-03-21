#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "auth/iot_authenticator.h"
#include "config/config_manager.h"
#include "core/types.h"

namespace sc {

// ============================================================
// IStreamUploader — KVS stream upload interface
// ============================================================

class IStreamUploader {
public:
    virtual ~IStreamUploader() = default;

    virtual VoidResult initialize(const KVSStreamConfig& config,
                                  std::shared_ptr<IIoTAuthenticator> auth) = 0;
    virtual VoidResult start() = 0;
    virtual VoidResult stop() = 0;
    virtual VoidResult flush_buffer() = 0;

    virtual bool is_connected() const = 0;
    virtual uint64_t bytes_uploaded() const = 0;
};

// ============================================================
// StreamUploader — concrete implementation
//   Real kvssink integration when HAS_GSTREAMER is defined;
//   stub otherwise (macOS dev environment).
// ============================================================

class StreamUploader : public IStreamUploader {
public:
    StreamUploader();
    ~StreamUploader() override;

    // Non-copyable, non-movable
    StreamUploader(const StreamUploader&) = delete;
    StreamUploader& operator=(const StreamUploader&) = delete;
    StreamUploader(StreamUploader&&) = delete;
    StreamUploader& operator=(StreamUploader&&) = delete;

    VoidResult initialize(const KVSStreamConfig& config,
                          std::shared_ptr<IIoTAuthenticator> auth) override;
    VoidResult start() override;
    VoidResult stop() override;
    VoidResult flush_buffer() override;

    bool is_connected() const override;
    uint64_t bytes_uploaded() const override;

    // Build the iot-certificate property string for kvssink
    // Format: "iot-thing-name=xxx,endpoint=xxx,cert-path=xxx,key-path=xxx,ca-path=xxx,role-aliases=xxx"
    static std::string build_iot_certificate_string(const IoTCertConfig& iot_config);

    // Reconnection constants
    static constexpr int kInitialReconnectDelaySec = 1;
    static constexpr int kMaxReconnectDelaySec = 30;
    static constexpr uint32_t kDefaultBufferDurationSec = 2;
    static constexpr uint32_t kDefaultRetentionHours = 168;  // 7 days
    static constexpr uint32_t kDefaultStorageSizeMB = 128;   // storage-size in MB (NOT * 1024)

private:
    // Reconnection with exponential backoff
    void reconnect_loop();
    void attempt_reconnect();

    // Local cache management for buffering during disconnection
    VoidResult cache_to_local();
    VoidResult upload_cached_data();

    mutable std::shared_mutex mutex_;
    KVSStreamConfig config_;
    std::shared_ptr<IIoTAuthenticator> auth_;
    bool initialized_ = false;
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> bytes_uploaded_{0};

    // Reconnection state
    std::atomic<bool> reconnecting_{false};
    int reconnect_delay_sec_ = kInitialReconnectDelaySec;
    std::unique_ptr<std::thread> reconnect_thread_;

    // Local cache for buffering during disconnection
    struct CachedSegment {
        std::vector<uint8_t> data;
        std::chrono::system_clock::time_point timestamp;
    };
    std::vector<CachedSegment> local_cache_;
    mutable std::mutex cache_mutex_;
};

// Factory function
std::unique_ptr<IStreamUploader> create_stream_uploader();

}  // namespace sc
