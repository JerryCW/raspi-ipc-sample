#include "webrtc/webrtc_agent.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

#ifdef HAS_KVS_WEBRTC_SDK
#include <com/amazonaws/kinesis/video/webrtcclient/Include.h>
#endif

namespace sc {

// ============================================================
// Testable helper: credential refresh decision logic
// ============================================================

bool should_refresh_credentials(
    std::chrono::system_clock::time_point expiration,
    std::chrono::system_clock::time_point now,
    int threshold_seconds) {
    auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
        expiration - now);
    return remaining.count() < threshold_seconds;
}

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

VoidResult WebRTCAgent::send_frame(const uint8_t* /*data*/, size_t /*size*/,
                                   uint64_t /*timestamp_us*/) {
    // Stub: no-op, return success
    return OkVoid();
}

void WebRTCAgent::handle_sdp_offer(const std::string& /*viewer_id*/,
                                   const std::string& /*sdp_offer*/) {
    // Stub: no-op
}

void WebRTCAgent::handle_ice_candidate(const std::string& /*viewer_id*/,
                                       const std::string& /*ice_candidate*/) {
    // Stub: no-op
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

    // Step 1: Initialize KVS WebRTC SDK
    STATUS retStatus = initKvsWebRtc();
    if (retStatus != STATUS_SUCCESS) {
        return ErrVoid(ErrorCode::WebRTCSignalingFailed,
                       "Failed to initialize KVS WebRTC SDK, status: " +
                           std::to_string(retStatus),
                       "WebRTCAgent::initialize");
    }

    // Step 2: Initialize signaling client info
    MEMSET(&client_info_, 0, SIZEOF(SignalingClientInfo));
    client_info_.version = SIGNALING_CLIENT_INFO_CURRENT_VERSION;
    client_info_.loggingLevel = LOG_LEVEL_VERBOSE;  // Verbose for debugging
    client_info_.cacheFilePath = NULL;  // Use default cache path
    client_info_.signalingClientCreationMaxRetryAttempts =
        CREATE_SIGNALING_CLIENT_RETRY_ATTEMPTS_SENTINEL_VALUE;
    if (!config_.channel_name.empty()) {
        STRNCPY(client_info_.clientId, config_.channel_name.c_str(), MAX_SIGNALING_CLIENT_ID_LEN);
    }

    // Step 3: Fill ChannelInfo struct (channel name, region, MASTER role)
    MEMSET(&channel_info_, 0, SIZEOF(ChannelInfo));
    channel_info_.version = CHANNEL_INFO_CURRENT_VERSION;
    channel_info_.channelType = SIGNALING_CHANNEL_TYPE_SINGLE_MASTER;
    channel_info_.channelRoleType = SIGNALING_CHANNEL_ROLE_TYPE_MASTER;
    channel_info_.pChannelName = const_cast<PCHAR>(config_.channel_name.c_str());
    channel_info_.pRegion = const_cast<PCHAR>(config_.region.c_str());
    channel_info_.cachingPolicy = SIGNALING_API_CALL_CACHE_TYPE_FILE;
    channel_info_.cachingPeriod = SIGNALING_API_CALL_CACHE_TTL_SENTINEL_VALUE;
    channel_info_.retry = TRUE;
    channel_info_.reconnect = TRUE;
    channel_info_.messageTtl = 0;  // Default 60 seconds
    // CA cert path for TLS verification — use IoT CA cert (matches ipc-kvs-demo)
    static std::string ca_cert_path;
    ca_cert_path = config_.iot_ca_cert_path.empty()
        ? "/etc/ssl/certs/ca-certificates.crt"
        : config_.iot_ca_cert_path;
    channel_info_.pCertPath = const_cast<PCHAR>(ca_cert_path.c_str());

    // Step 4: Register signaling callbacks
    MEMSET(&signaling_callbacks_, 0, SIZEOF(SignalingClientCallbacks));
    signaling_callbacks_.version = SIGNALING_CLIENT_CALLBACKS_CURRENT_VERSION;
    signaling_callbacks_.customData = reinterpret_cast<UINT64>(this);
    signaling_callbacks_.messageReceivedFn = on_signaling_message;
    signaling_callbacks_.stateChangeFn = on_signaling_state_changed;
    signaling_callbacks_.errorReportFn = on_signaling_error;

    // Step 5: Create IoT credential provider (matches ipc-kvs-demo)
    if (!config_.iot_credential_endpoint.empty() && !config_.iot_cert_path.empty()) {
        retStatus = createLwsIotCredentialProvider(
            const_cast<PCHAR>(config_.iot_credential_endpoint.c_str()),
            const_cast<PCHAR>(config_.iot_cert_path.c_str()),
            const_cast<PCHAR>(config_.iot_key_path.c_str()),
            const_cast<PCHAR>(ca_cert_path.c_str()),
            const_cast<PCHAR>(config_.iot_role_alias.c_str()),
            const_cast<PCHAR>(config_.iot_thing_name.c_str()),
            &credential_provider_);
        if (retStatus != STATUS_SUCCESS) {
            deinitKvsWebRtc();
            return ErrVoid(ErrorCode::WebRTCSignalingFailed,
                           "Failed to create IoT credential provider, status: " +
                               std::to_string(retStatus),
                           "WebRTCAgent::initialize");
        }
    } else {
        deinitKvsWebRtc();
        return ErrVoid(ErrorCode::WebRTCSignalingFailed,
                       "IoT credential config incomplete for WebRTC",
                       "WebRTCAgent::initialize");
    }

    // Step 6: Create signaling client
    retStatus = createSignalingClientSync(&client_info_, &channel_info_,
                                          &signaling_callbacks_,
                                          credential_provider_,
                                          &signaling_client_handle_);
    if (retStatus != STATUS_SUCCESS) {
        deinitKvsWebRtc();
        return ErrVoid(ErrorCode::WebRTCSignalingFailed,
                       "Failed to create signaling client, status: " +
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

    // Step 1: Fetch signaling channel endpoints (HTTPS, WSS, TURN, etc.)
    {
        std::lock_guard<std::mutex> send_lock(signaling_send_mutex_);
        STATUS retStatus = signalingClientFetchSync(signaling_client_handle_);
        if (retStatus != STATUS_SUCCESS) {
            return ErrVoid(ErrorCode::WebRTCSignalingFailed,
                           "Failed to fetch signaling channel endpoints, status: " +
                               std::to_string(retStatus),
                           "WebRTCAgent::attempt_signaling_connect");
        }
    }

    // Step 2: Connect to signaling channel via WebSocket
    {
        std::lock_guard<std::mutex> send_lock(signaling_send_mutex_);
        STATUS retStatus = signalingClientConnectSync(signaling_client_handle_);
        if (retStatus != STATUS_SUCCESS) {
            return ErrVoid(ErrorCode::WebRTCSignalingFailed,
                           "Failed to connect to signaling channel, status: " +
                               std::to_string(retStatus),
                           "WebRTCAgent::attempt_signaling_connect");
        }
    }

    // Step 3: Mark signaling as connected
    signaling_connected_.store(true);

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
                // Free peer connection resources before erasing
                if (it->second.peer_connection != nullptr) {
                    STATUS retStatus = freePeerConnection(&it->second.peer_connection);
                    if (retStatus != STATUS_SUCCESS) {
                        // WARNING: freePeerConnection failed during cleanup — continue
                    }
                    it->second.peer_connection = nullptr;
                }
                it = peers_.erase(it);
                continue;
            }
        }
        ++it;
    }
}

void WebRTCAgent::shutdown_sequence() {
    // Mark state flags first so background threads can exit
    signaling_connected_.store(false);
    running_.store(false);
    reconnecting_.store(false);

    // Wait for cleanup thread and reconnect thread to finish before
    // releasing SDK resources — prevents use-after-free (Req 9.5)
    cleanup_running_.store(false);
    if (cleanup_thread_ && cleanup_thread_->joinable()) {
        cleanup_thread_->join();
    }
    cleanup_thread_.reset();

    if (reconnect_thread_ && reconnect_thread_->joinable()) {
        reconnect_thread_->join();
    }
    reconnect_thread_.reset();

    // Step 1: Disconnect signaling (Req 9.1)
    {
        STATUS retStatus = signalingClientDisconnectSync(signaling_client_handle_);
        if (retStatus != STATUS_SUCCESS) {
            // WARNING: signaling disconnect failed — continue shutdown (Req 9.6)
        }
    }

    // Step 2: Close all Peer Connections (Req 9.2)
    {
        std::unique_lock lock(mutex_);
        for (auto& [id, peer] : peers_) {
            if (peer.state == PeerInfo::State::CONNECTED) {
                viewer_count_.fetch_sub(1);
            }
            peer.state = PeerInfo::State::DISCONNECTING;
            peer.disconnected_at = std::chrono::steady_clock::now();

            if (peer.peer_connection != nullptr) {
                STATUS retStatus = closePeerConnection(peer.peer_connection);
                if (retStatus != STATUS_SUCCESS) {
                    // WARNING: closePeerConnection failed — continue (Req 9.6)
                }
            }
        }
    }

    // Step 3: Sleep 1s — allow pending operations to complete (Req 9.1)
    std::this_thread::sleep_for(std::chrono::milliseconds(kShutdownPeerSleepMs));

    // Step 4: Free Peer Connection resources (Req 9.3)
    {
        std::unique_lock lock(mutex_);
        for (auto& [id, peer] : peers_) {
            if (peer.peer_connection != nullptr) {
                STATUS retStatus = freePeerConnection(&peer.peer_connection);
                if (retStatus != STATUS_SUCCESS) {
                    // WARNING: freePeerConnection failed — continue (Req 9.6)
                }
                peer.peer_connection = nullptr;
            }
        }
        peers_.clear();
    }

    // Step 5: Free signaling client (Req 9.4)
    {
        STATUS retStatus = freeSignalingClient(&signaling_client_handle_);
        if (retStatus != STATUS_SUCCESS) {
            // WARNING: freeSignalingClient failed — continue (Req 9.6)
        }
    }

    // Step 5.5: Free credential provider
    if (credential_provider_ != nullptr) {
        freeIotCredentialProvider(&credential_provider_);
        credential_provider_ = nullptr;
    }

    // Step 6: Deinit KVS WebRTC SDK (Req 9.1)
    {
        STATUS retStatus = deinitKvsWebRtc();
        if (retStatus != STATUS_SUCCESS) {
            // WARNING: deinitKvsWebRtc failed — continue (Req 9.6)
        }
    }

    initialized_ = false;
}

VoidResult WebRTCAgent::send_frame(const uint8_t* data, size_t size,
                                   uint64_t timestamp_us) {
    // Construct KVS WebRTC SDK Frame struct
    Frame frame;
    MEMSET(&frame, 0, SIZEOF(Frame));
    frame.frameData = const_cast<PBYTE>(data);
    frame.size = static_cast<UINT32>(size);
    frame.presentationTs = timestamp_us * DEFAULT_TIME_UNIT_IN_NANOS;
    frame.flags = FRAME_FLAG_NONE;

    // Use shared_lock (read lock) to iterate peers — allows concurrent
    // frame sends without blocking peer management operations
    std::shared_lock<std::shared_mutex> lock(mutex_);

    for (const auto& [viewer_id, peer] : peers_) {
        // Only send to CONNECTED peers; skip CONNECTING and DISCONNECTING
        if (peer.state != PeerInfo::State::CONNECTED) {
            continue;
        }

        STATUS retStatus = writeFrame(peer.video_transceiver, &frame);
        if (retStatus != STATUS_SUCCESS) {
            // Single peer failure: log warning, continue to other peers
            // (do not abort the entire frame distribution)
        }
    }

    return OkVoid();
}

void WebRTCAgent::handle_sdp_offer(const std::string& viewer_id,
                                   const std::string& sdp_offer) {
    std::unique_lock lock(mutex_);

    // Check viewer capacity
    if (viewer_count_.load() >= config_.max_viewers) {
        return;
    }

    // Create RtcConfiguration with ICE servers
    RtcConfiguration rtc_config;
    MEMSET(&rtc_config, 0x00, SIZEOF(RtcConfiguration));
    rtc_config.iceTransportPolicy = ICE_TRANSPORT_POLICY_ALL;

    // Set STUN server (slot 0)
    SNPRINTF(rtc_config.iceServers[0].urls, MAX_ICE_CONFIG_URI_LEN,
             "stun:stun.kinesisvideo.%s.amazonaws.com:443",
             config_.region.c_str());

    // Get TURN servers from signaling client (slots 1+)
    UINT32 iceConfigCount = 0;
    signalingClientGetIceConfigInfoCount(signaling_client_handle_, &iceConfigCount);
    UINT32 uriCount = 0;
    // Use only 1 TURN server to optimize candidate gathering
    UINT32 maxTurnServer = (iceConfigCount > 0) ? 1 : 0;
    for (UINT32 i = 0; i < maxTurnServer; i++) {
        PIceConfigInfo pIceConfigInfo = nullptr;
        if (signalingClientGetIceConfigInfo(signaling_client_handle_, i, &pIceConfigInfo) == STATUS_SUCCESS && pIceConfigInfo != nullptr) {
            for (UINT32 j = 0; j < pIceConfigInfo->uriCount; j++) {
                if (uriCount + 1 < MAX_ICE_SERVERS_COUNT) {
                    STRNCPY(rtc_config.iceServers[uriCount + 1].urls, pIceConfigInfo->uris[j], MAX_ICE_CONFIG_URI_LEN);
                    STRNCPY(rtc_config.iceServers[uriCount + 1].credential, pIceConfigInfo->password, MAX_ICE_CONFIG_CREDENTIAL_LEN);
                    STRNCPY(rtc_config.iceServers[uriCount + 1].username, pIceConfigInfo->userName, MAX_ICE_CONFIG_USER_NAME_LEN);
                    uriCount++;
                }
            }
        }
    }

    // Create peer connection
    PRtcPeerConnection peer_connection = nullptr;
    STATUS retStatus = createPeerConnection(&rtc_config, &peer_connection);
    if (retStatus != STATUS_SUCCESS) {
        return;
    }

    // Set up video track: H.264
    RtcMediaStreamTrack video_track;
    MEMSET(&video_track, 0, SIZEOF(RtcMediaStreamTrack));
    video_track.kind = MEDIA_STREAM_TRACK_KIND_VIDEO;
    video_track.codec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE;
    STRCPY(video_track.streamId, "smart-camera-video");
    STRCPY(video_track.trackId, "smart-camera-video-track");

    PRtcRtpTransceiver video_transceiver = nullptr;
    RtcRtpTransceiverInit video_init;
    video_init.direction = RTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV;
    retStatus = addTransceiver(peer_connection, &video_track, &video_init, &video_transceiver);
    if (retStatus != STATUS_SUCCESS) { freePeerConnection(&peer_connection); return; }

    // Set up audio track: OPUS (required even without audio)
    RtcMediaStreamTrack audio_track;
    MEMSET(&audio_track, 0, SIZEOF(RtcMediaStreamTrack));
    audio_track.kind = MEDIA_STREAM_TRACK_KIND_AUDIO;
    audio_track.codec = RTC_CODEC_OPUS;
    STRCPY(audio_track.streamId, "smart-camera-audio");
    STRCPY(audio_track.trackId, "smart-camera-audio-track");

    PRtcRtpTransceiver audio_transceiver = nullptr;
    RtcRtpTransceiverInit audio_init;
    audio_init.direction = RTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV;
    retStatus = addTransceiver(peer_connection, &audio_track, &audio_init, &audio_transceiver);
    if (retStatus != STATUS_SUCCESS) { freePeerConnection(&peer_connection); return; }

    // Build PeerInfo and callback context
    PeerInfo peer;
    peer.viewer_id = viewer_id;
    peer.state = PeerInfo::State::CONNECTING;
    peer.connected_at = std::chrono::steady_clock::now();
    peer.peer_connection = peer_connection;
    peer.video_track = video_track;
    peer.audio_track = audio_track;
    peer.video_transceiver = video_transceiver;
    peer.audio_transceiver = audio_transceiver;
    peer.callback_context = std::make_unique<PeerCallbackContext>();
    peer.callback_context->agent = this;
    peer.callback_context->viewer_id = viewer_id;
    auto* ctx = peer.callback_context.get();

    // Register callbacks
    retStatus = peerConnectionOnIceCandidate(peer_connection, reinterpret_cast<UINT64>(ctx), on_ice_candidate);
    if (retStatus != STATUS_SUCCESS) { freePeerConnection(&peer_connection); return; }

    retStatus = peerConnectionOnConnectionStateChange(peer_connection, reinterpret_cast<UINT64>(ctx), on_connection_state_change);
    if (retStatus != STATUS_SUCCESS) { freePeerConnection(&peer_connection); return; }

    // Deserialize and set remote SDP offer (matching SDK sample pattern)
    RtcSessionDescriptionInit offer_sdp;
    MEMSET(&offer_sdp, 0x00, SIZEOF(RtcSessionDescriptionInit));
    retStatus = deserializeSessionDescriptionInit(
        const_cast<PCHAR>(sdp_offer.c_str()),
        static_cast<UINT32>(sdp_offer.size()),
        &offer_sdp);
    if (retStatus != STATUS_SUCCESS) { freePeerConnection(&peer_connection); return; }

    retStatus = setRemoteDescription(peer_connection, &offer_sdp);
    if (retStatus != STATUS_SUCCESS) { freePeerConnection(&peer_connection); return; }

    // SDK sample order: setLocalDescription → createAnswer → send
    RtcSessionDescriptionInit answer_sdp;
    MEMSET(&answer_sdp, 0x00, SIZEOF(RtcSessionDescriptionInit));

    retStatus = setLocalDescription(peer_connection, &answer_sdp);
    if (retStatus != STATUS_SUCCESS) { freePeerConnection(&peer_connection); return; }

    retStatus = createAnswer(peer_connection, &answer_sdp);
    if (retStatus != STATUS_SUCCESS) { freePeerConnection(&peer_connection); return; }

    // Send SDP answer via signaling channel
    {
        SignalingMessage msg;
        MEMSET(&msg, 0, SIZEOF(SignalingMessage));
        msg.version = SIGNALING_MESSAGE_CURRENT_VERSION;
        msg.messageType = SIGNALING_MESSAGE_TYPE_ANSWER;
        STRNCPY(msg.peerClientId, viewer_id.c_str(), MAX_SIGNALING_CLIENT_ID_LEN);
        msg.payloadLen = (UINT32) STRLEN(answer_sdp.sdp);
        STRNCPY(msg.payload, answer_sdp.sdp, msg.payloadLen);

        std::lock_guard<std::mutex> send_lock(signaling_send_mutex_);
        retStatus = signalingClientSendMessageSync(signaling_client_handle_, &msg);
        if (retStatus != STATUS_SUCCESS) { freePeerConnection(&peer_connection); return; }
    }

    peers_[viewer_id] = std::move(peer);
}

void WebRTCAgent::handle_ice_candidate(const std::string& viewer_id,
                                       const std::string& ice_candidate) {
    std::unique_lock lock(mutex_);

    auto it = peers_.find(viewer_id);
    if (it == peers_.end()) {
        return;
    }

    RtcIceCandidateInit candidate_init;
    MEMSET(&candidate_init, 0, SIZEOF(RtcIceCandidateInit));
    // Use SDK deserializer matching the sample's handleRemoteCandidate pattern
    STATUS retStatus = deserializeRtcIceCandidateInit(
        const_cast<PCHAR>(ice_candidate.c_str()),
        static_cast<UINT32>(ice_candidate.size()),
        &candidate_init);
    if (retStatus == STATUS_SUCCESS) {
        addIceCandidate(it->second.peer_connection, candidate_init.candidate);
    }
}

// ============================================================
// Static callbacks — forwarded to instance methods
// ============================================================

STATUS WebRTCAgent::on_signaling_message(UINT64 custom_data,
                                          PReceivedSignalingMessage msg) {
    auto* self = reinterpret_cast<WebRTCAgent*>(custom_data);
    if (self == nullptr || msg == nullptr) {
        return STATUS_NULL_ARG;
    }

    // Extract viewer_id and payload from the signaling message
    std::string viewer_id(msg->signalingMessage.peerClientId);
    std::string payload(msg->signalingMessage.payload,
                        msg->signalingMessage.payloadLen);

    // Dispatch based on message type
    switch (msg->signalingMessage.messageType) {
        case SIGNALING_MESSAGE_TYPE_OFFER:
            self->handle_sdp_offer(viewer_id, payload);
            break;

        case SIGNALING_MESSAGE_TYPE_ICE_CANDIDATE:
            self->handle_ice_candidate(viewer_id, payload);
            break;

        default:
            // Ignore other message types (e.g., ANSWER — we are MASTER)
            break;
    }

    return STATUS_SUCCESS;
}

STATUS WebRTCAgent::on_signaling_state_changed(UINT64 custom_data,
                                                SIGNALING_CLIENT_STATE state) {
    auto* self = reinterpret_cast<WebRTCAgent*>(custom_data);
    if (self == nullptr) {
        return STATUS_NULL_ARG;
    }

    switch (state) {
        case SIGNALING_CLIENT_STATE_CONNECTED:
            self->signaling_connected_.store(true);
            break;

        case SIGNALING_CLIENT_STATE_DISCONNECTED:
            self->signaling_connected_.store(false);

            // Trigger reconnection if the agent is still running
            if (self->running_.load()) {
                bool expected = false;
                if (self->reconnecting_.compare_exchange_strong(expected, true)) {
                    // Launch reconnect thread only if not already running
                    if (self->reconnect_thread_ &&
                        self->reconnect_thread_->joinable()) {
                        self->reconnect_thread_->join();
                    }
                    self->reconnect_thread_ = std::make_unique<std::thread>(
                        &WebRTCAgent::signaling_reconnect_loop, self);
                }
            }
            break;

        default:
            // Other states (NEW, GET_CREDENTIALS, DESCRIBE, CREATE,
            // GET_ENDPOINT, GET_ICE_CONFIG, READY, CONNECTING) — no action needed
            break;
    }

    return STATUS_SUCCESS;
}

STATUS WebRTCAgent::on_signaling_error(UINT64 custom_data,
                                        STATUS status,
                                        PCHAR msg,
                                        UINT32 msg_len) {
    UNUSED_PARAM(custom_data);
    UNUSED_PARAM(status);
    UNUSED_PARAM(msg);
    UNUSED_PARAM(msg_len);
    return STATUS_SUCCESS;
}

void WebRTCAgent::on_ice_candidate(UINT64 custom_data,
                                    PCHAR candidate) {
    auto* ctx = reinterpret_cast<PeerCallbackContext*>(custom_data);
    if (ctx == nullptr || ctx->agent == nullptr) {
        return;
    }

    // NULL candidate signals end of ICE gathering — nothing to send
    if (candidate == nullptr) {
        return;
    }

    auto* self = ctx->agent;
    const auto& viewer_id = ctx->viewer_id;

    SignalingMessage msg;
    MEMSET(&msg, 0, SIZEOF(SignalingMessage));
    msg.version = SIGNALING_MESSAGE_CURRENT_VERSION;
    msg.messageType = SIGNALING_MESSAGE_TYPE_ICE_CANDIDATE;
    STRNCPY(msg.peerClientId, viewer_id.c_str(), MAX_SIGNALING_CLIENT_ID_LEN);
    msg.payloadLen = (UINT32) STRNLEN(candidate, MAX_SIGNALING_MESSAGE_LEN);
    STRNCPY(msg.payload, candidate, msg.payloadLen);
    msg.correlationId[0] = '\0';

    // Send via signaling channel (mutex-protected)
    std::lock_guard<std::mutex> send_lock(self->signaling_send_mutex_);
    signalingClientSendMessageSync(self->signaling_client_handle_, &msg);
}

void WebRTCAgent::on_connection_state_change(UINT64 custom_data,
                                              RTC_PEER_CONNECTION_STATE state) {
    auto* ctx = reinterpret_cast<PeerCallbackContext*>(custom_data);
    if (ctx == nullptr || ctx->agent == nullptr) {
        return;
    }

    auto* self = ctx->agent;
    const auto& viewer_id = ctx->viewer_id;

    switch (state) {
        case RTC_PEER_CONNECTION_STATE_CONNECTED: {
            std::unique_lock lock(self->mutex_);
            auto it = self->peers_.find(viewer_id);
            if (it != self->peers_.end() &&
                it->second.state == PeerInfo::State::CONNECTING) {
                it->second.state = PeerInfo::State::CONNECTED;
                it->second.connected_at = std::chrono::steady_clock::now();
                self->viewer_count_.fetch_add(1);

                // Notify viewer callback (outside lock would be ideal,
                // but callback should be lightweight)
                if (self->viewer_callback_) {
                    self->viewer_callback_(viewer_id, true);
                }
            }
            break;
        }

        case RTC_PEER_CONNECTION_STATE_DISCONNECTED:
        case RTC_PEER_CONNECTION_STATE_FAILED:
            self->remove_viewer(viewer_id);
            break;

        default:
            // Other states (NEW, CONNECTING, CLOSING, CLOSED) — no action
            break;
    }
}

STATUS WebRTCAgent::get_credentials_callback(UINT64 custom_data,
                                              PAwsCredentials credentials) {
    auto* self = reinterpret_cast<WebRTCAgent*>(custom_data);
    if (self == nullptr || credentials == nullptr) {
        return STATUS_NULL_ARG;
    }

    // Get credentials from IoT authenticator
    auto cred_result = self->auth_->get_credentials();
    if (cred_result.is_err()) {
        return STATUS_INVALID_OPERATION;
    }

    const auto& aws_creds = cred_result.value();

    // Check if credential is expiring within 5 minutes (300 seconds)
    auto now = std::chrono::system_clock::now();
    auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
        aws_creds.expiration - now);

    if (remaining.count() < kCredentialRefreshThresholdSec) {
        // Force refresh to get new credentials before expiry
        auto refresh_result = self->auth_->force_refresh();
        if (refresh_result.is_ok()) {
            // Re-fetch the refreshed credentials
            auto refreshed = self->auth_->get_credentials();
            if (refreshed.is_ok()) {
                const auto& new_creds = refreshed.value();
                STRNCPY(credentials->accessKeyId,
                         new_creds.access_key_id.c_str(),
                         MAX_ACCESS_KEY_LEN);
                credentials->accessKeyIdLen = static_cast<UINT32>(
                    new_creds.access_key_id.size());
                STRNCPY(credentials->secretKey,
                         new_creds.secret_access_key.c_str(),
                         MAX_SECRET_KEY_LEN);
                credentials->secretKeyLen = static_cast<UINT32>(
                    new_creds.secret_access_key.size());
                STRNCPY(credentials->sessionToken,
                         new_creds.session_token.c_str(),
                         MAX_SESSION_TOKEN_LEN);
                credentials->sessionTokenLen = static_cast<UINT32>(
                    new_creds.session_token.size());
                auto exp_epoch = std::chrono::duration_cast<
                    std::chrono::seconds>(
                    new_creds.expiration.time_since_epoch());
                credentials->expiration = static_cast<UINT64>(
                    exp_epoch.count()) * HUNDREDS_OF_NANOS_IN_A_SECOND;
                return STATUS_SUCCESS;
            }
        }
        // If refresh failed, fall through and use the existing credentials
    }

    // Fill PAwsCredentials struct with current credentials
    STRNCPY(credentials->accessKeyId,
             aws_creds.access_key_id.c_str(),
             MAX_ACCESS_KEY_LEN);
    credentials->accessKeyIdLen = static_cast<UINT32>(
        aws_creds.access_key_id.size());
    STRNCPY(credentials->secretKey,
             aws_creds.secret_access_key.c_str(),
             MAX_SECRET_KEY_LEN);
    credentials->secretKeyLen = static_cast<UINT32>(
        aws_creds.secret_access_key.size());
    STRNCPY(credentials->sessionToken,
             aws_creds.session_token.c_str(),
             MAX_SESSION_TOKEN_LEN);
    credentials->sessionTokenLen = static_cast<UINT32>(
        aws_creds.session_token.size());
    auto exp_epoch = std::chrono::duration_cast<std::chrono::seconds>(
        aws_creds.expiration.time_since_epoch());
    credentials->expiration = static_cast<UINT64>(
        exp_epoch.count()) * HUNDREDS_OF_NANOS_IN_A_SECOND;

    return STATUS_SUCCESS;
}

#endif  // HAS_KVS_WEBRTC_SDK

}  // namespace sc
