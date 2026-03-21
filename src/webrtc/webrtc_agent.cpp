#include "webrtc/webrtc_agent.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

namespace sc {

// ============================================================
// Factory
// ============================================================

std::unique_ptr<IWebRTCAgent> create_webrtc_agent() {
    return std::make_unique<WebRTCAgent>();
}

// ============================================================
// Stub implementation (no KVS WebRTC SDK — macOS dev)
// ============================================================

#ifndef HAS_KVS_WEBRTC_SDK

WebRTCAgent::WebRTCAgent() = default;

WebRTCAgent::~WebRTCAgent() {
    if (running_.load()) {
        stop_signaling();
    }
}

VoidResult WebRTCAgent::initialize(const WebRTCConfig& config,
                                   std::shared_ptr<IIoTAuthenticator> auth) {
    std::unique_lock lock(mutex_);

    if (!auth) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "IoT authenticator is null",
                       "WebRTCAgent::initialize");
    }

    if (config.channel_name.empty()) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "WebRTC channel name is empty",
                       "WebRTCAgent::initialize");
    }

    if (config.region.empty()) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "WebRTC region is empty",
                       "WebRTCAgent::initialize");
    }

    config_ = config;
    auth_ = std::move(auth);

    // Apply default max viewers if not set
    if (config_.max_viewers == 0) {
        config_.max_viewers = kDefaultMaxViewers;
    }

    // Stub: log that we're in stub mode (MASTER role registration simulated)
    // In real implementation: initKvsWebRtc(), createSignalingClientInfo(),
    // configure STUN/TURN servers, register as MASTER role

    initialized_ = true;
    return OkVoid();
}

VoidResult WebRTCAgent::start_signaling() {
    std::unique_lock lock(mutex_);

    if (!initialized_) {
        return ErrVoid(ErrorCode::WebRTCSignalingFailed,
                       "WebRTCAgent not initialized",
                       "WebRTCAgent::start_signaling");
    }

    running_.store(true);
    signaling_connected_.store(true);
    signaling_retry_delay_sec_ = kInitialSignalingRetryDelaySec;

    // Start cleanup thread for disconnected peers
    cleanup_running_.store(true);
    cleanup_thread_ = std::make_unique<std::thread>([this]() {
        while (cleanup_running_.load()) {
            cleanup_disconnected_peers();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

    return OkVoid();
}

VoidResult WebRTCAgent::stop_signaling() {
    // Shutdown sequence:
    // 1. Disconnect signaling
    // 2. Close peers
    // 3. Sleep 1s
    // 4. Free peers
    // 5. Free signaling
    // 6. Deinit SDK
    shutdown_sequence();
    return OkVoid();
}

uint32_t WebRTCAgent::active_viewer_count() const {
    return viewer_count_.load();
}

bool WebRTCAgent::is_signaling_connected() const {
    return signaling_connected_.load();
}

void WebRTCAgent::on_viewer_change(ViewerCallback cb) {
    std::unique_lock lock(mutex_);
    viewer_callback_ = std::move(cb);
}

void WebRTCAgent::signaling_reconnect_loop() {
    while (running_.load() && reconnecting_.load()) {
        auto result = attempt_signaling_connect();

        if (result.is_ok()) {
            signaling_connected_.store(true);
            reconnecting_.store(false);
            signaling_retry_delay_sec_ = kInitialSignalingRetryDelaySec;
            break;
        }

        // Exponential backoff: initial 2s, max 60s
        std::this_thread::sleep_for(
            std::chrono::seconds(signaling_retry_delay_sec_));
        signaling_retry_delay_sec_ = std::min(
            signaling_retry_delay_sec_ * 2, kMaxSignalingRetryDelaySec);
    }
}

VoidResult WebRTCAgent::attempt_signaling_connect() {
    // Stub: simulate successful signaling connection
    return OkVoid();
}

VoidResult WebRTCAgent::add_viewer(const std::string& viewer_id) {
    std::unique_lock lock(mutex_);

    // Check capacity: max concurrent viewers
    if (viewer_count_.load() >= config_.max_viewers) {
        return ErrVoid(ErrorCode::WebRTCCapacityFull,
                       "Maximum viewer capacity reached (" +
                           std::to_string(config_.max_viewers) + ")",
                       "WebRTCAgent::add_viewer");
    }

    // Add peer in CONNECTING state
    PeerInfo peer;
    peer.viewer_id = viewer_id;
    peer.state = PeerInfo::State::CONNECTING;
    peer.connected_at = std::chrono::steady_clock::now();
    peers_[viewer_id] = std::move(peer);

    // Transition to CONNECTED (in real impl, this happens after ICE completes)
    peers_[viewer_id].state = PeerInfo::State::CONNECTED;
    viewer_count_.fetch_add(1);

    // Notify callback
    if (viewer_callback_) {
        viewer_callback_(viewer_id, true);
    }

    return OkVoid();
}

void WebRTCAgent::remove_viewer(const std::string& viewer_id) {
    std::unique_lock lock(mutex_);

    auto it = peers_.find(viewer_id);
    if (it == peers_.end()) {
        return;
    }

    if (it->second.state == PeerInfo::State::CONNECTED) {
        viewer_count_.fetch_sub(1);
    }

    it->second.state = PeerInfo::State::DISCONNECTING;
    it->second.disconnected_at = std::chrono::steady_clock::now();

    // Notify callback
    if (viewer_callback_) {
        viewer_callback_(viewer_id, false);
    }
}

void WebRTCAgent::cleanup_disconnected_peers() {
    std::unique_lock lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    auto it = peers_.begin();
    while (it != peers_.end()) {
        if (it->second.state == PeerInfo::State::DISCONNECTING) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second.disconnected_at);
            // Release resources within 10 seconds of disconnect
            if (elapsed.count() >= kDisconnectCleanupTimeoutSec) {
                it = peers_.erase(it);
                continue;
            }
        }
        ++it;
    }
}

void WebRTCAgent::shutdown_sequence() {
    // Step 1: Disconnect signaling
    signaling_connected_.store(false);
    running_.store(false);
    reconnecting_.store(false);

    // Stop reconnect thread
    if (reconnect_thread_ && reconnect_thread_->joinable()) {
        reconnect_thread_->join();
    }
    reconnect_thread_.reset();

    // Stop cleanup thread
    cleanup_running_.store(false);
    if (cleanup_thread_ && cleanup_thread_->joinable()) {
        cleanup_thread_->join();
    }
    cleanup_thread_.reset();

    // Step 2: Close peers (mark all as disconnecting)
    {
        std::unique_lock lock(mutex_);
        for (auto& [id, peer] : peers_) {
            if (peer.state == PeerInfo::State::CONNECTED) {
                viewer_count_.fetch_sub(1);
            }
            peer.state = PeerInfo::State::DISCONNECTING;
            peer.disconnected_at = std::chrono::steady_clock::now();
        }
    }

    // Step 3: Sleep 1s (allow pending operations to complete)
    std::this_thread::sleep_for(std::chrono::milliseconds(kShutdownPeerSleepMs));

    // Step 4: Free peers
    {
        std::unique_lock lock(mutex_);
        peers_.clear();
    }

    // Step 5: Free signaling (no-op in stub)
    // Step 6: Deinit SDK (no-op in stub)

    initialized_ = false;
}

#else  // HAS_KVS_WEBRTC_SDK defined — real implementation

#include <com/amazonaws/kinesis/video/webrtcclient/Include.h>

WebRTCAgent::WebRTCAgent() = default;

WebRTCAgent::~WebRTCAgent() {
    if (running_.load()) {
        stop_signaling();
    }
}

VoidResult WebRTCAgent::initialize(const WebRTCConfig& config,
                                   std::shared_ptr<IIoTAuthenticator> auth) {
    std::unique_lock lock(mutex_);

    if (!auth) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "IoT authenticator is null",
                       "WebRTCAgent::initialize");
    }

    if (config.channel_name.empty()) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "WebRTC channel name is empty",
                       "WebRTCAgent::initialize");
    }

    if (config.region.empty()) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "WebRTC region is empty",
                       "WebRTCAgent::initialize");
    }

    config_ = config;
    auth_ = std::move(auth);

    if (config_.max_viewers == 0) {
        config_.max_viewers = kDefaultMaxViewers;
    }

    // Initialize KVS WebRTC SDK
    STATUS retStatus = initKvsWebRtc();
    if (retStatus != STATUS_SUCCESS) {
        return ErrVoid(ErrorCode::WebRTCSignalingFailed,
                       "Failed to initialize KVS WebRTC SDK, status: " +
                           std::to_string(retStatus),
                       "WebRTCAgent::initialize");
    }

    initialized_ = true;
    return OkVoid();
}

VoidResult WebRTCAgent::start_signaling() {
    std::unique_lock lock(mutex_);

    if (!initialized_) {
        return ErrVoid(ErrorCode::WebRTCSignalingFailed,
                       "WebRTCAgent not initialized",
                       "WebRTCAgent::start_signaling");
    }

    running_.store(true);
    signaling_retry_delay_sec_ = kInitialSignalingRetryDelaySec;

    // Attempt initial signaling connection
    auto result = attempt_signaling_connect();
    if (result.is_err()) {
        // Start reconnection loop in background
        reconnecting_.store(true);
        reconnect_thread_ = std::make_unique<std::thread>(
            &WebRTCAgent::signaling_reconnect_loop, this);
    } else {
        signaling_connected_.store(true);
    }

    // Start cleanup thread for disconnected peers
    cleanup_running_.store(true);
    cleanup_thread_ = std::make_unique<std::thread>([this]() {
        while (cleanup_running_.load()) {
            cleanup_disconnected_peers();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

    return OkVoid();
}

VoidResult WebRTCAgent::stop_signaling() {
    shutdown_sequence();
    return OkVoid();
}

uint32_t WebRTCAgent::active_viewer_count() const {
    return viewer_count_.load();
}

bool WebRTCAgent::is_signaling_connected() const {
    return signaling_connected_.load();
}

void WebRTCAgent::on_viewer_change(ViewerCallback cb) {
    std::unique_lock lock(mutex_);
    viewer_callback_ = std::move(cb);
}

void WebRTCAgent::signaling_reconnect_loop() {
    while (running_.load() && reconnecting_.load()) {
        auto result = attempt_signaling_connect();

        if (result.is_ok()) {
            signaling_connected_.store(true);
            reconnecting_.store(false);
            signaling_retry_delay_sec_ = kInitialSignalingRetryDelaySec;
            break;
        }

        // Exponential backoff: initial 2s, max 60s
        std::this_thread::sleep_for(
            std::chrono::seconds(signaling_retry_delay_sec_));
        signaling_retry_delay_sec_ = std::min(
            signaling_retry_delay_sec_ * 2, kMaxSignalingRetryDelaySec);
    }
}

VoidResult WebRTCAgent::attempt_signaling_connect() {
    // Get credentials from IoT authenticator
    auto cred_result = auth_->get_credentials();
    if (cred_result.is_err()) {
        return ErrVoid(ErrorCode::WebRTCSignalingFailed,
                       "Failed to get credentials: " + cred_result.error().message,
                       "WebRTCAgent::attempt_signaling_connect");
    }

    // In real implementation:
    // 1. Create signaling client with MASTER role
    // 2. Configure STUN/TURN ICE servers
    // 3. Set up SDP offer/answer callbacks
    // 4. Set up ICE candidate exchange callbacks
    // 5. Connect to signaling channel
    //
    // Key points:
    // - payloadLen must use STRLEN (no null terminator)
    // - Add audio transceiver + OPUS codec even if not sending audio
    // - Transceiver direction must be SENDRECV
    // - Stream/track names must match SDK conventions
    // - Use signaling_send_mutex_ for all signaling writes

    return OkVoid();
}

VoidResult WebRTCAgent::add_viewer(const std::string& viewer_id) {
    std::unique_lock lock(mutex_);

    // Check capacity
    if (viewer_count_.load() >= config_.max_viewers) {
        // Send capacity-full signaling message to the viewer
        // Key: use STRLEN for payloadLen (no null terminator)
        return ErrVoid(ErrorCode::WebRTCCapacityFull,
                       "Maximum viewer capacity reached (" +
                           std::to_string(config_.max_viewers) + ")",
                       "WebRTCAgent::add_viewer");
    }

    PeerInfo peer;
    peer.viewer_id = viewer_id;
    peer.state = PeerInfo::State::CONNECTING;
    peer.connected_at = std::chrono::steady_clock::now();
    peers_[viewer_id] = std::move(peer);

    // In real implementation:
    // 1. Create peer connection
    // 2. Add video transceiver (H.264, SENDRECV)
    // 3. Add audio transceiver (OPUS, SENDRECV) — required even without audio
    // 4. Set SDP offer/answer handlers
    // 5. Set ICE candidate handler (Trickle ICE)
    // 6. On ICE connected → transition to CONNECTED, increment viewer_count_
    // 7. Only send frames to CONNECTED peers, skip CONNECTING peers

    // Simulate immediate connection for now
    peers_[viewer_id].state = PeerInfo::State::CONNECTED;
    viewer_count_.fetch_add(1);

    if (viewer_callback_) {
        viewer_callback_(viewer_id, true);
    }

    return OkVoid();
}

void WebRTCAgent::remove_viewer(const std::string& viewer_id) {
    std::unique_lock lock(mutex_);

    auto it = peers_.find(viewer_id);
    if (it == peers_.end()) {
        return;
    }

    if (it->second.state == PeerInfo::State::CONNECTED) {
        viewer_count_.fetch_sub(1);
    }

    it->second.state = PeerInfo::State::DISCONNECTING;
    it->second.disconnected_at = std::chrono::steady_clock::now();

    if (viewer_callback_) {
        viewer_callback_(viewer_id, false);
    }
}

void WebRTCAgent::cleanup_disconnected_peers() {
    std::unique_lock lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    auto it = peers_.begin();
    while (it != peers_.end()) {
        if (it->second.state == PeerInfo::State::DISCONNECTING) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second.disconnected_at);
            if (elapsed.count() >= kDisconnectCleanupTimeoutSec) {
                // In real implementation: free peer connection resources
                it = peers_.erase(it);
                continue;
            }
        }
        ++it;
    }
}

void WebRTCAgent::shutdown_sequence() {
    // Step 1: Disconnect signaling
    signaling_connected_.store(false);
    running_.store(false);
    reconnecting_.store(false);

    // Stop reconnect thread
    if (reconnect_thread_ && reconnect_thread_->joinable()) {
        reconnect_thread_->join();
    }
    reconnect_thread_.reset();

    // Stop cleanup thread
    cleanup_running_.store(false);
    if (cleanup_thread_ && cleanup_thread_->joinable()) {
        cleanup_thread_->join();
    }
    cleanup_thread_.reset();

    // Step 2: Close peers
    {
        std::unique_lock lock(mutex_);
        for (auto& [id, peer] : peers_) {
            if (peer.state == PeerInfo::State::CONNECTED) {
                viewer_count_.fetch_sub(1);
            }
            peer.state = PeerInfo::State::DISCONNECTING;
            peer.disconnected_at = std::chrono::steady_clock::now();
            // In real implementation: closePeerConnection()
        }
    }

    // Step 3: Sleep 1s (allow pending operations to complete)
    std::this_thread::sleep_for(std::chrono::milliseconds(kShutdownPeerSleepMs));

    // Step 4: Free peers
    {
        std::unique_lock lock(mutex_);
        // In real implementation: freePeerConnection() for each peer
        peers_.clear();
    }

    // Step 5: Free signaling
    // In real implementation: freeSignalingClient()

    // Step 6: Deinit SDK
    // In real implementation: deinitKvsWebRtc()

    initialized_ = false;
}

#endif  // HAS_KVS_WEBRTC_SDK

}  // namespace sc
