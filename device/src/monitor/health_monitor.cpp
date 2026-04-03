#include "monitor/health_monitor.h"

#include <algorithm>
#include <iostream>
#include <sstream>

namespace sc {

// ============================================================
// Construction / Destruction
// ============================================================

HealthMonitor::HealthMonitor(std::chrono::seconds restart_window,
                             uint32_t max_restarts,
                             std::chrono::seconds stall_timeout,
                             uint32_t max_recovery_attempts)
    : restart_window_(restart_window)
    , max_restarts_(max_restarts)
    , window_start_(std::chrono::steady_clock::now())
    , stall_timeout_(stall_timeout)
    , max_recovery_attempts_(max_recovery_attempts) {}

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

        // Initialize frame stall detection state
        last_frame_time_ = std::chrono::steady_clock::now();
        last_checked_frame_count_ = 0;
        consecutive_recovery_failures_ = 0;
    }
    frame_counter_.store(0, std::memory_order_relaxed);

    running_.store(true);
    return OkVoid();
}

VoidResult HealthMonitor::start(std::shared_ptr<IGStreamerPipeline> pipeline,
                                const PipelineConfig& config) {
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
        saved_config_ = config;

        // Initialize frame stall detection state
        last_frame_time_ = std::chrono::steady_clock::now();
        last_checked_frame_count_ = 0;
        consecutive_recovery_failures_ = 0;
    }
    frame_counter_.store(0, std::memory_order_relaxed);

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

void HealthMonitor::on_fatal_failure(FatalFailureCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    fatal_callback_ = std::move(cb);
}

// ============================================================
// Frame stall detection
// ============================================================

void HealthMonitor::report_frame_produced() {
    frame_counter_.fetch_add(1, std::memory_order_relaxed);
}

void HealthMonitor::check_frame_stall() {
    if (!running_.load()) {
        return;
    }

    uint64_t current_count = frame_counter_.load(std::memory_order_relaxed);

    std::unique_lock<std::shared_mutex> lock(mutex_);

    if (current_count != last_checked_frame_count_) {
        // Frames are being produced — update tracking state
        last_checked_frame_count_ = current_count;
        last_frame_time_ = std::chrono::steady_clock::now();
        return;
    }

    // Frame count has NOT changed — check if stall timeout exceeded
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_frame_time_);
    auto stall_timeout_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        stall_timeout_);

    if (elapsed > stall_timeout_ms) {
        std::cerr << "[HealthMonitor] Frame stall detected: no new frames for "
                  << (elapsed.count() / 1000) << "s (timeout=" << stall_timeout_.count()
                  << "s). Attempting full recovery." << std::endl;
        lock.unlock();
        attempt_full_recovery();
    }
}

bool HealthMonitor::attempt_full_recovery() {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    if (!pipeline_) {
        std::cerr << "[HealthMonitor] Full recovery failed: no pipeline."
                  << std::endl;
        return false;
    }

    std::cerr << "[HealthMonitor] Executing full recovery: stop → destroy → build → start"
              << std::endl;

    // Execute stop → destroy → build → start
    auto stop_result = pipeline_->stop();
    auto destroy_result = pipeline_->destroy();
    auto build_result = pipeline_->build(saved_config_);
    auto start_result = pipeline_->start();

    if (start_result.is_ok()) {
        consecutive_recovery_failures_ = 0;
        last_frame_time_ = std::chrono::steady_clock::now();
        last_checked_frame_count_ = frame_counter_.load(std::memory_order_relaxed);

        // Record restart timestamp
        restart_timestamps_.push_back(std::chrono::steady_clock::now());

        std::cerr << "[HealthMonitor] Full recovery succeeded." << std::endl;
        return true;
    }

    // Recovery failed
    consecutive_recovery_failures_++;
    std::cerr << "[HealthMonitor] Full recovery failed ("
              << consecutive_recovery_failures_ << "/"
              << max_recovery_attempts_ << ")." << std::endl;

    // Record restart timestamp even on failure
    restart_timestamps_.push_back(std::chrono::steady_clock::now());

    if (consecutive_recovery_failures_ >= max_recovery_attempts_) {
        std::string reason =
            "Pipeline recovery failed " +
            std::to_string(consecutive_recovery_failures_) +
            " consecutive times. Requesting process exit.";
        std::cerr << "[HealthMonitor] " << reason << std::endl;

        lock.unlock();
        {
            std::lock_guard<std::mutex> cb_lock(callback_mutex_);
            if (fatal_callback_) {
                fatal_callback_(reason);
            }
        }
        return false;
    }

    return false;
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
