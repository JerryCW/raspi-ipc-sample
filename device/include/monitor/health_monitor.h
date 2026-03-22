#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "core/types.h"
#include "pipeline/gstreamer_pipeline.h"

namespace sc {

// ============================================================
// PipelineError — diagnostic info from a GStreamer bus error
// ============================================================

struct PipelineError {
    std::string element_name;
    uint32_t error_domain = 0;
    int32_t error_code = 0;
    std::string debug_info;
    std::chrono::steady_clock::time_point timestamp;
};

// ============================================================
// IHealthMonitor — interface for pipeline health monitoring
// ============================================================

class IHealthMonitor {
public:
    virtual ~IHealthMonitor() = default;

    virtual VoidResult start(std::shared_ptr<IGStreamerPipeline> pipeline) = 0;
    virtual VoidResult stop() = 0;

    virtual uint32_t restart_count_in_window() const = 0;
    virtual bool is_restart_limit_reached() const = 0;

    using ErrorCallback = std::function<void(const PipelineError&)>;
    virtual void on_error(ErrorCallback cb) = 0;
};

// ============================================================
// HealthMonitor — concrete implementation
//
// Monitors GStreamer bus for ERROR/WARNING/EOS messages.
// On ERROR: logs diagnostics, attempts pipeline restart within
// 5 seconds, subject to a fixed 10-minute window restart limit
// (default max 5 restarts per window).
//
// Without GStreamer, provides a stub that tracks errors and
// restart counts via report_error() / attempt_restart().
// ============================================================

class HealthMonitor : public IHealthMonitor {
public:
    /// @param restart_window  fixed window duration (default 10 min)
    /// @param max_restarts    max restarts allowed in window (default 5)
    explicit HealthMonitor(
        std::chrono::seconds restart_window = std::chrono::seconds(600),
        uint32_t max_restarts = 5);

    ~HealthMonitor() override;

    // Non-copyable, non-movable
    HealthMonitor(const HealthMonitor&) = delete;
    HealthMonitor& operator=(const HealthMonitor&) = delete;
    HealthMonitor(HealthMonitor&&) = delete;
    HealthMonitor& operator=(HealthMonitor&&) = delete;

    VoidResult start(std::shared_ptr<IGStreamerPipeline> pipeline) override;
    VoidResult stop() override;

    uint32_t restart_count_in_window() const override;
    bool is_restart_limit_reached() const override;

    void on_error(ErrorCallback cb) override;

    // ---- Public for testing (simulates GStreamer bus events) ----

    /// Report a pipeline error (triggers error callback + restart logic)
    void report_error(const PipelineError& error);

    /// Attempt a pipeline restart; returns false if limit reached
    bool attempt_restart();

    // ---- Watchdog alert callback ----
    using WatchdogAlertCallback = std::function<void(const std::string& reason)>;
    void on_watchdog_alert(WatchdogAlertCallback cb);

private:
    void prune_old_restarts() const;

    std::chrono::seconds restart_window_;
    uint32_t max_restarts_;

    mutable std::shared_mutex mutex_;
    std::shared_ptr<IGStreamerPipeline> pipeline_;
    std::atomic<bool> running_{false};

    // Restart timestamps within the current window
    mutable std::vector<std::chrono::steady_clock::time_point> restart_timestamps_;
    std::chrono::steady_clock::time_point window_start_;

    std::mutex callback_mutex_;
    ErrorCallback error_callback_;
    WatchdogAlertCallback watchdog_callback_;
};

}  // namespace sc
