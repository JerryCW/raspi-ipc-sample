#include "monitor/health_monitor.h"

#include <algorithm>
#include <iostream>
#include <sstream>

namespace sc {

// ============================================================
// Construction / Destruction
// ============================================================

HealthMonitor::HealthMonitor(std::chrono::seconds restart_window,
                             uint32_t max_restarts)
    : restart_window_(restart_window)
    , max_restarts_(max_restarts)
    , window_start_(std::chrono::steady_clock::now()) {}

HealthMonitor::~HealthMonitor() {
    if (running_.load()) {
        stop();
    }
}

// ============================================================
// start / stop
// ============================================================

VoidResult HealthMonitor::start(std::shared_ptr<IGStreamerPipeline> pipeline) {
    if (running_.load()) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "HealthMonitor already running",
                       "HealthMonitor::start");
    }
    if (!pipeline) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "Pipeline pointer is null",
                       "HealthMonitor::start");
    }

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        pipeline_ = std::move(pipeline);
        restart_timestamps_.clear();
        window_start_ = std::chrono::steady_clock::now();
    }

    running_.store(true);
    return OkVoid();
}

VoidResult HealthMonitor::stop() {
    if (!running_.load()) {
        return OkVoid();
    }
    running_.store(false);

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        pipeline_.reset();
    }

    return OkVoid();
}

// ============================================================
// Status accessors
// ============================================================

uint32_t HealthMonitor::restart_count_in_window() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    prune_old_restarts();
    return static_cast<uint32_t>(restart_timestamps_.size());
}

bool HealthMonitor::is_restart_limit_reached() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    prune_old_restarts();
    return restart_timestamps_.size() >= max_restarts_;
}

void HealthMonitor::on_error(ErrorCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    error_callback_ = std::move(cb);
}

void HealthMonitor::on_watchdog_alert(WatchdogAlertCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    watchdog_callback_ = std::move(cb);
}

// ============================================================
// report_error — simulate GStreamer bus ERROR message
// ============================================================

void HealthMonitor::report_error(const PipelineError& error) {
    // Log diagnostic info (error domain, code, debug info)
    {
        std::ostringstream msg;
        msg << "[HealthMonitor] Pipeline ERROR: element=" << error.element_name
            << " domain=" << error.error_domain
            << " code=" << error.error_code
            << " debug=\"" << error.debug_info << "\"";
        std::cerr << msg.str() << std::endl;
    }

    // Notify error callback
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (error_callback_) {
            error_callback_(error);
        }
    }

    // Attempt restart (within 5 seconds — immediate in stub mode)
    if (running_.load()) {
        attempt_restart();
    }
}

// ============================================================
// attempt_restart — restart pipeline if within window limit
// ============================================================

bool HealthMonitor::attempt_restart() {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    // Prune restarts outside the current window
    prune_old_restarts();

    // Check if restart limit reached
    if (restart_timestamps_.size() >= max_restarts_) {
        std::cerr << "[HealthMonitor] Restart limit reached ("
                  << max_restarts_ << " in "
                  << restart_window_.count() << "s window). "
                  << "Stopping auto-restart." << std::endl;

        // Trigger watchdog alert
        lock.unlock();
        {
            std::lock_guard<std::mutex> cb_lock(callback_mutex_);
            if (watchdog_callback_) {
                watchdog_callback_(
                    "Pipeline restart limit reached (" +
                    std::to_string(max_restarts_) + " in " +
                    std::to_string(restart_window_.count()) + "s window)");
            }
        }
        return false;
    }

    // Record this restart
    auto now = std::chrono::steady_clock::now();
    restart_timestamps_.push_back(now);

    // Attempt pipeline stop + start (stub: just track the count)
    if (pipeline_) {
        auto stop_result = pipeline_->stop();
        auto start_result = pipeline_->start();

        if (start_result.is_ok()) {
            std::cerr << "[HealthMonitor] Pipeline restarted successfully. "
                      << "Restart count in window: "
                      << restart_timestamps_.size() << "/" << max_restarts_
                      << std::endl;
        } else {
            std::cerr << "[HealthMonitor] Pipeline restart failed: "
                      << start_result.error().message << std::endl;
        }
    }

    return true;
}

// ============================================================
// prune_old_restarts — remove timestamps outside the window
// ============================================================

void HealthMonitor::prune_old_restarts() const {
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - restart_window_;

    restart_timestamps_.erase(
        std::remove_if(restart_timestamps_.begin(), restart_timestamps_.end(),
                       [&cutoff](const auto& ts) { return ts < cutoff; }),
        restart_timestamps_.end());
}

}  // namespace sc
