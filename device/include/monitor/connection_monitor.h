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

#include "core/stream_mode.h"
#include "core/stream_mode_fsm.h"
#include "core/types.h"

namespace sc {

// ============================================================
// EndpointStatus — health state of a single endpoint
// ============================================================

struct EndpointStatus {
    std::string url;
    bool reachable = false;
    double response_time_ms = 0.0;
    std::chrono::steady_clock::time_point check_time;
    uint32_t consecutive_failures = 0;
    uint32_t consecutive_successes = 0;
};

// ============================================================
// IConnectionMonitor — interface for endpoint health checking
// ============================================================

class IConnectionMonitor {
public:
    virtual ~IConnectionMonitor() = default;

    virtual VoidResult start(const std::string& kvs_endpoint,
                             const std::string& webrtc_endpoint) = 0;
    virtual VoidResult stop() = 0;

    virtual EndpointStatus kvs_status() const = 0;
    virtual EndpointStatus webrtc_status() const = 0;

    using ModeCallback = std::function<void(StreamMode)>;
    virtual void on_mode_change(ModeCallback cb) = 0;
};

// ============================================================
// ConnectionMonitor — concrete implementation
//
// Every 30 seconds performs HTTPS HEAD requests to KVS and
// WebRTC endpoints (timeout 5s).
//   - 3 consecutive failures  → mark offline, notify mode switch
//   - 2 consecutive successes → mark recovered, notify mode upgrade
//
// Uses CURL when CURL_FOUND is defined; otherwise a stub that
// always reports endpoints as reachable.
// ============================================================

class ConnectionMonitor : public IConnectionMonitor {
public:
    /// @param failure_threshold  consecutive failures to mark offline (default 3)
    /// @param success_threshold  consecutive successes to mark recovered (default 2)
    /// @param check_interval     interval between checks (default 30s)
    /// @param request_timeout    per-request timeout (default 5s)
    explicit ConnectionMonitor(
        uint32_t failure_threshold = 3,
        uint32_t success_threshold = 2,
        std::chrono::seconds check_interval = std::chrono::seconds(30),
        std::chrono::seconds request_timeout = std::chrono::seconds(5));

    ~ConnectionMonitor() override;

    VoidResult start(const std::string& kvs_endpoint,
                     const std::string& webrtc_endpoint) override;
    VoidResult stop() override;

    EndpointStatus kvs_status() const override;
    EndpointStatus webrtc_status() const override;

    void on_mode_change(ModeCallback cb) override;

    // Exposed for testing: perform a single check cycle synchronously.
    void check_once();

private:
    void monitor_loop();
    bool check_endpoint(const std::string& url, double& response_time_ms);
    void update_status(EndpointStatus& status, bool reachable, double response_time_ms);
    void evaluate_and_notify();

    uint32_t failure_threshold_;
    uint32_t success_threshold_;
    std::chrono::seconds check_interval_;
    std::chrono::seconds request_timeout_;

    mutable std::shared_mutex status_mutex_;
    EndpointStatus kvs_status_;
    EndpointStatus webrtc_status_;

    StreamModeFSM fsm_;

    std::mutex callback_mutex_;
    ModeCallback mode_callback_;

    std::atomic<bool> running_{false};
    std::thread monitor_thread_;
};

}  // namespace sc
