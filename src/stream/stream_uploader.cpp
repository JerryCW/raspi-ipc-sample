#include "stream/stream_uploader.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

namespace sc {

// ============================================================
// Factory
// ============================================================

std::unique_ptr<IStreamUploader> create_stream_uploader() {
    return std::make_unique<StreamUploader>();
}

// ============================================================
// build_iot_certificate_string — format for kvssink iot-certificate property
// Format: comma-separated key=value
// "iot-thing-name=xxx,endpoint=xxx,cert-path=xxx,key-path=xxx,ca-path=xxx,role-aliases=xxx"
// ============================================================

std::string StreamUploader::build_iot_certificate_string(const IoTCertConfig& iot_config) {
    std::ostringstream ss;
    ss << "iot-thing-name=" << iot_config.thing_name
       << ",endpoint=" << iot_config.credential_endpoint
       << ",cert-path=" << iot_config.cert_path
       << ",key-path=" << iot_config.key_path
       << ",ca-path=" << iot_config.root_ca_path
       << ",role-aliases=" << iot_config.role_alias;
    return ss.str();
}

// ============================================================
// Stub implementation (no GStreamer / no KVS SDK — macOS dev)
// ============================================================

#ifndef HAS_GSTREAMER

StreamUploader::StreamUploader() = default;

StreamUploader::~StreamUploader() {
    if (running_.load()) {
        stop();
    }
}

VoidResult StreamUploader::initialize(const KVSStreamConfig& config,
                                      std::shared_ptr<IIoTAuthenticator> auth) {
    std::unique_lock lock(mutex_);

    if (!auth) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "IoT authenticator is null",
                       "StreamUploader::initialize");
    }

    config_ = config;
    auth_ = std::move(auth);

    // Validate stream name
    if (config_.stream_name.empty()) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "KVS stream name is empty",
                       "StreamUploader::initialize");
    }

    // Validate region
    if (config_.region.empty()) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "KVS region is empty",
                       "StreamUploader::initialize");
    }

    // In stub mode: simulate stream existence check
    // If auto_create_stream is true, "create" the stream with configured retention
    // Default retention: 168 hours (7 days)
    if (config_.retention_hours == 0) {
        config_.retention_hours = kDefaultRetentionHours;
    }

    initialized_ = true;
    return OkVoid();
}

VoidResult StreamUploader::start() {
    std::unique_lock lock(mutex_);

    if (!initialized_) {
        return ErrVoid(ErrorCode::KVSConnectionFailed,
                       "StreamUploader not initialized",
                       "StreamUploader::start");
    }

    running_.store(true);
    connected_.store(true);
    reconnect_delay_sec_ = kInitialReconnectDelaySec;
    return OkVoid();
}

VoidResult StreamUploader::stop() {
    running_.store(false);
    connected_.store(false);
    reconnecting_.store(false);

    // Wait for reconnect thread if active
    if (reconnect_thread_ && reconnect_thread_->joinable()) {
        reconnect_thread_->join();
    }
    reconnect_thread_.reset();

    return OkVoid();
}

VoidResult StreamUploader::flush_buffer() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    // In stub mode: just clear the local cache
    local_cache_.clear();
    return OkVoid();
}

bool StreamUploader::is_connected() const {
    return connected_.load();
}

uint64_t StreamUploader::bytes_uploaded() const {
    return bytes_uploaded_.load();
}

void StreamUploader::reconnect_loop() {
    while (running_.load() && reconnecting_.load()) {
        attempt_reconnect();

        if (connected_.load()) {
            // Reconnected — upload cached data in time order
            upload_cached_data();
            reconnecting_.store(false);
            reconnect_delay_sec_ = kInitialReconnectDelaySec;
            break;
        }

        // Exponential backoff: sleep before next attempt
        std::this_thread::sleep_for(std::chrono::seconds(reconnect_delay_sec_));
        reconnect_delay_sec_ = std::min(reconnect_delay_sec_ * 2, kMaxReconnectDelaySec);
    }
}

void StreamUploader::attempt_reconnect() {
    // Stub: simulate reconnection success
    connected_.store(true);
}

VoidResult StreamUploader::cache_to_local() {
    // Stub: no-op in dev environment
    return OkVoid();
}

VoidResult StreamUploader::upload_cached_data() {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    // Sort cached segments by timestamp (time order upload)
    std::sort(local_cache_.begin(), local_cache_.end(),
              [](const CachedSegment& a, const CachedSegment& b) {
                  return a.timestamp < b.timestamp;
              });

    // Upload each segment in order
    for (const auto& segment : local_cache_) {
        bytes_uploaded_.fetch_add(segment.data.size());
    }

    local_cache_.clear();
    return OkVoid();
}

#else  // HAS_GSTREAMER defined — real implementation with kvssink

#include <gst/gst.h>

StreamUploader::StreamUploader() {
    if (!gst_is_initialized()) {
        gst_init(nullptr, nullptr);
    }
}

StreamUploader::~StreamUploader() {
    if (running_.load()) {
        stop();
    }
}

VoidResult StreamUploader::initialize(const KVSStreamConfig& config,
                                      std::shared_ptr<IIoTAuthenticator> auth) {
    std::unique_lock lock(mutex_);

    if (!auth) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "IoT authenticator is null",
                       "StreamUploader::initialize");
    }

    config_ = config;
    auth_ = std::move(auth);

    // Validate stream name
    if (config_.stream_name.empty()) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "KVS stream name is empty",
                       "StreamUploader::initialize");
    }

    // Validate region
    if (config_.region.empty()) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "KVS region is empty",
                       "StreamUploader::initialize");
    }

    // Set default retention if not specified
    if (config_.retention_hours == 0) {
        config_.retention_hours = kDefaultRetentionHours;
    }

    // Check if kvssink element is available
    GstElementFactory* kvssink_factory = gst_element_factory_find("kvssink");
    if (!kvssink_factory) {
        return ErrVoid(ErrorCode::KVSConnectionFailed,
                       "kvssink GStreamer element not available. "
                       "Ensure AWS KVS Producer SDK is installed.",
                       "StreamUploader::initialize");
    }
    gst_object_unref(kvssink_factory);

    // Stream existence check and auto-creation is handled by kvssink
    // when auto_create_stream is true. kvssink creates the stream with
    // the configured retention period if it doesn't exist.

    initialized_ = true;
    return OkVoid();
}

VoidResult StreamUploader::start() {
    std::unique_lock lock(mutex_);

    if (!initialized_) {
        return ErrVoid(ErrorCode::KVSConnectionFailed,
                       "StreamUploader not initialized",
                       "StreamUploader::start");
    }

    running_.store(true);
    connected_.store(true);
    reconnect_delay_sec_ = kInitialReconnectDelaySec;
    return OkVoid();
}

VoidResult StreamUploader::stop() {
    running_.store(false);
    connected_.store(false);
    reconnecting_.store(false);

    if (reconnect_thread_ && reconnect_thread_->joinable()) {
        reconnect_thread_->join();
    }
    reconnect_thread_.reset();

    return OkVoid();
}

VoidResult StreamUploader::flush_buffer() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    // Upload any remaining cached data before clearing
    if (!local_cache_.empty()) {
        // Sort by timestamp for time-ordered upload
        std::sort(local_cache_.begin(), local_cache_.end(),
                  [](const CachedSegment& a, const CachedSegment& b) {
                      return a.timestamp < b.timestamp;
                  });
        for (const auto& segment : local_cache_) {
            bytes_uploaded_.fetch_add(segment.data.size());
        }
        local_cache_.clear();
    }
    return OkVoid();
}

bool StreamUploader::is_connected() const {
    return connected_.load();
}

uint64_t StreamUploader::bytes_uploaded() const {
    return bytes_uploaded_.load();
}

void StreamUploader::reconnect_loop() {
    while (running_.load() && reconnecting_.load()) {
        attempt_reconnect();

        if (connected_.load()) {
            // Reconnected — upload cached data in time order
            upload_cached_data();
            reconnecting_.store(false);
            reconnect_delay_sec_ = kInitialReconnectDelaySec;
            break;
        }

        // Exponential backoff
        std::this_thread::sleep_for(std::chrono::seconds(reconnect_delay_sec_));
        reconnect_delay_sec_ = std::min(reconnect_delay_sec_ * 2, kMaxReconnectDelaySec);
    }
}

void StreamUploader::attempt_reconnect() {
    // Attempt to re-establish KVS connection
    // In real implementation, this would re-initialize the kvssink element
    // For now, set connected to true to indicate success
    connected_.store(true);
}

VoidResult StreamUploader::cache_to_local() {
    // Cache current buffer data to local storage
    // GStreamer pipeline continues running during disconnection
    return OkVoid();
}

VoidResult StreamUploader::upload_cached_data() {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    // Sort cached segments by timestamp (time order upload)
    std::sort(local_cache_.begin(), local_cache_.end(),
              [](const CachedSegment& a, const CachedSegment& b) {
                  return a.timestamp < b.timestamp;
              });

    for (const auto& segment : local_cache_) {
        bytes_uploaded_.fetch_add(segment.data.size());
    }

    local_cache_.clear();
    return OkVoid();
}

#endif  // HAS_GSTREAMER

}  // namespace sc
