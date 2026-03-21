#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "auth/iot_authenticator.h"
#include "config/config_manager.h"
#include "core/types.h"

namespace sc {

// ============================================================
// IWebRTCAgent — WebRTC signaling and media transport interface
// ============================================================

class IWebRTCAgent {
public:
    virtual ~IWebRTCAgent() = default;

    virtual VoidResult initialize(const WebRTCConfig& config,
                                  std::shared_ptr<IIoTAuthenticator> auth) = 0;
    virtual VoidResult start_signaling() = 0;
    virtual VoidResult stop_signaling() = 0;

    virtual uint32_t active_viewer_count() const = 0;
    virtual bool is_signaling_connected() const = 0;

    // Connection event callback
    using ViewerCallback = std::function<void(const std::string& viewer_id, bool connected)>;
    virtual void on_viewer_change(ViewerCallback cb) = 0;
};

// ============================================================
// WebRTCAgent — concrete implementation
//   Real KVS WebRTC SDK path when HAS_KVS_WEBRTC_SDK is defined;
//   stub otherwise (macOS dev environment).
// ============================================================

class WebRTCAgent : public IWebRTCAgent {
public:
    WebRTCAgent();
    ~WebRTCAgent() override;

    // Non-copyable, non-movable
    WebRTCAgent(const WebRTCAgent&) = delete;
    WebRTCAgent& operator=(const WebRTCAgent&) = delete;
    WebRTCAgent(WebRTCAgent&&) = delete;
    WebRTCAgent& operator=(WebRTCAgent&&) = delete;

    VoidResult initialize(const WebRTCConfig& config,
                          std::shared_ptr<IIoTAuthenticator> auth) override;
    VoidResult start_signaling() override;
    VoidResult stop_signaling() override;

    uint32_t active_viewer_count() const override;
    bool is_signaling_connected() const override;

    void on_viewer_change(ViewerCallback cb) override;

    // Constants
    static constexpr uint32_t kDefaultMaxViewers = 10;
    static constexpr int kInitialSignalingRetryDelaySec = 2;
    static constexpr int kMaxSignalingRetryDelaySec = 60;
    static constexpr int kDisconnectCleanupTimeoutSec = 10;
    static constexpr int kShutdownPeerSleepMs = 1000;

private:
    // Signaling reconnection with exponential backoff
    void signaling_reconnect_loop();
    VoidResult attempt_signaling_connect();

    // Peer connection management
    VoidResult add_viewer(const std::string& viewer_id);
    void remove_viewer(const std::string& viewer_id);
    void cleanup_disconnected_peers();

    // Shutdown sequence: disconnect signaling → close peers → sleep 1s
    //                    → free peers → free signaling → deinit SDK
    void shutdown_sequence();

    // Configuration
    WebRTCConfig config_;
    std::shared_ptr<IIoTAuthenticator> auth_;
    bool initialized_ = false;

    // Thread safety
    mutable std::shared_mutex mutex_;
    std::mutex signaling_send_mutex_;  // Prevents concurrent signaling writes

    // State (atomic for lock-free reads)
    std::atomic<bool> signaling_connected_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> viewer_count_{0};

    // Signaling reconnection state
    std::atomic<bool> reconnecting_{false};
    int signaling_retry_delay_sec_ = kInitialSignalingRetryDelaySec;
    std::unique_ptr<std::thread> reconnect_thread_;

    // Peer tracking
    struct PeerInfo {
        std::string viewer_id;
        enum class State { CONNECTING, CONNECTED, DISCONNECTING };
        State state = State::CONNECTING;
        std::chrono::steady_clock::time_point connected_at;
        std::chrono::steady_clock::time_point disconnected_at;
    };
    std::unordered_map<std::string, PeerInfo> peers_;

    // Viewer change callback
    ViewerCallback viewer_callback_;

    // Cleanup thread for disconnected peers
    std::unique_ptr<std::thread> cleanup_thread_;
    std::atomic<bool> cleanup_running_{false};
};

// Factory function
std::unique_ptr<IWebRTCAgent> create_webrtc_agent();

}  // namespace sc
