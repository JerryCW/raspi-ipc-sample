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

#ifdef HAS_KVS_WEBRTC_SDK
// SDK include MUST be outside namespace sc to avoid namespace pollution (see CHANGELOG 0.1.4)
#include <com/amazonaws/kinesis/video/webrtcclient/Include.h>
#endif

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

    // Send H.264 frame to all connected peers
    virtual VoidResult send_frame(const uint8_t* data, size_t size,
                                  uint64_t timestamp_us) = 0;

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

    VoidResult send_frame(const uint8_t* data, size_t size,
                          uint64_t timestamp_us) override;

    // Constants
    static constexpr uint32_t kDefaultMaxViewers = 10;
    static constexpr int kInitialSignalingRetryDelaySec = 2;
    static constexpr int kMaxSignalingRetryDelaySec = 60;
    static constexpr int kDisconnectCleanupTimeoutSec = 10;
    static constexpr int kShutdownPeerSleepMs = 1000;
    static constexpr int kCredentialRefreshThresholdSec = 300;  // 5 minutes

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

    // Forward declaration for per-peer callback context
    struct PeerCallbackContext;

    // Peer tracking
    struct PeerInfo {
        std::string viewer_id;
        enum class State { CONNECTING, CONNECTED, DISCONNECTING };
        State state = State::CONNECTING;
        std::chrono::steady_clock::time_point connected_at;
        std::chrono::steady_clock::time_point disconnected_at;

#ifdef HAS_KVS_WEBRTC_SDK
        PRtcPeerConnection peer_connection = nullptr;
        RtcMediaStreamTrack video_track;
        RtcMediaStreamTrack audio_track;
        PRtcRtpTransceiver video_transceiver = nullptr;
        PRtcRtpTransceiver audio_transceiver = nullptr;
        std::unique_ptr<PeerCallbackContext> callback_context;
#endif
    };
    std::unordered_map<std::string, PeerInfo> peers_;

    // Viewer change callback
    ViewerCallback viewer_callback_;

    // Cleanup thread for disconnected peers
    std::unique_ptr<std::thread> cleanup_thread_;
    std::atomic<bool> cleanup_running_{false};

#ifdef HAS_KVS_WEBRTC_SDK
    // SDK handles
    SIGNALING_CLIENT_HANDLE signaling_client_handle_ = INVALID_SIGNALING_CLIENT_HANDLE_VALUE;

    // Signaling channel info
    ChannelInfo channel_info_;
    SignalingClientInfo client_info_;
    SignalingClientCallbacks signaling_callbacks_;

    // ICE server configuration
    IceConfigInfo ice_configs_[MAX_ICE_CONFIG_COUNT];
    UINT32 ice_config_count_ = 0;

    // Credential provider
    PAwsCredentialProvider credential_provider_ = nullptr;

    // Per-peer callback context for onIceCandidate / onConnectionStateChange
    struct PeerCallbackContext {
        WebRTCAgent* agent = nullptr;
        std::string viewer_id;
    };
#endif

    // SDK callback handling (instance methods)
    void handle_sdp_offer(const std::string& viewer_id,
                          const std::string& sdp_offer);
    void handle_ice_candidate(const std::string& viewer_id,
                              const std::string& ice_candidate);

#ifdef HAS_KVS_WEBRTC_SDK
    // Signaling callbacks (static, forwarded to instance methods)
    static STATUS on_signaling_message(UINT64 custom_data,
                                       PReceivedSignalingMessage msg);
    static STATUS on_signaling_state_changed(UINT64 custom_data,
                                              SIGNALING_CLIENT_STATE state);
    static STATUS on_signaling_error(UINT64 custom_data,
                                      STATUS status,
                                      PCHAR msg,
                                      UINT32 msg_len);

    // Peer connection callbacks
    static STATUS on_ice_candidate(UINT64 custom_data,
                                    PCHAR candidate);
    static void on_connection_state_change(UINT64 custom_data,
                                            RTC_PEER_CONNECTION_STATE state);

    // Credential provider callback
    static STATUS get_credentials_callback(UINT64 custom_data,
                                            PAwsCredentials credentials);
#endif
};

// Factory function
std::unique_ptr<IWebRTCAgent> create_webrtc_agent();

// ============================================================
// Testable helper: credential refresh decision logic
// Returns true if credentials need refresh (remaining < threshold)
// Extracted from get_credentials_callback() for unit/property testing
// ============================================================
bool should_refresh_credentials(
    std::chrono::system_clock::time_point expiration,
    std::chrono::system_clock::time_point now,
    int threshold_seconds = WebRTCAgent::kCredentialRefreshThresholdSec);

}  // namespace sc
