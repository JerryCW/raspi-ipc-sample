#include "control/bitrate_controller.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <numeric>
#include <sstream>

namespace sc {

// ============================================================
// Lifecycle
// ============================================================

BitrateController::~BitrateController() {
    if (running_.load(std::memory_order_relaxed)) {
        stop();
    }
}

VoidResult BitrateController::start(uint32_t initial_bitrate_kbps,
                                    uint32_t min_bitrate_kbps,
                                    uint32_t max_bitrate_kbps) {
    if (running_.load(std::memory_order_relaxed)) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "BitrateController already running",
                       "BitrateController::start");
    }

    if (min_bitrate_kbps > max_bitrate_kbps) {
        return ErrVoid(ErrorCode::InvalidArgument,
                       "min_bitrate_kbps > max_bitrate_kbps",
                       "BitrateController::start");
    }

    min_bitrate_kbps_ = min_bitrate_kbps;
    max_bitrate_kbps_ = max_bitrate_kbps;
    current_bitrate_kbps_.store(
        clamp_bitrate(initial_bitrate_kbps), std::memory_order_relaxed);

    upscale_tracking_ = false;

    {
        std::unique_lock<std::shared_mutex> lock(metrics_mutex_);
        metrics_buffer_.clear();
    }

    running_.store(true, std::memory_order_release);

    sample_thread_ = std::thread([this]() {
        while (running_.load(std::memory_order_acquire)) {
            {
                std::unique_lock<std::mutex> lock(cv_mutex_);
                cv_.wait_for(lock, kSampleInterval, [this]() {
                    return !running_.load(std::memory_order_acquire);
                });
            }
            if (!running_.load(std::memory_order_acquire)) break;
            evaluate_and_adjust();
        }
    });

    return OkVoid();
}

VoidResult BitrateController::stop() {
    if (!running_.load(std::memory_order_relaxed)) {
        return OkVoid();
    }

    running_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(cv_mutex_);
        cv_.notify_all();
    }

    if (sample_thread_.joinable()) {
        sample_thread_.join();
    }

    return OkVoid();
}

// ============================================================
// Metrics reporting
// ============================================================

void BitrateController::report_metrics(const NetworkMetrics& metrics) {
    std::unique_lock<std::shared_mutex> lock(metrics_mutex_);
    metrics_buffer_.push_back(metrics);

    // Keep only the last 60 seconds of samples (12 samples at 5s interval)
    auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    while (!metrics_buffer_.empty() && metrics_buffer_.front().sample_time < cutoff) {
        metrics_buffer_.pop_front();
    }
}

uint32_t BitrateController::current_bitrate() const {
    return current_bitrate_kbps_.load(std::memory_order_relaxed);
}

void BitrateController::on_adjustment(AdjustmentCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_ = std::move(cb);
}

// ============================================================
// Core evaluation logic
// ============================================================

bool BitrateController::evaluate_and_adjust() {
    NetworkMetrics latest;
    double avg_throughput = 0.0;
    double avg_loss = 0.0;

    {
        std::shared_lock<std::shared_mutex> lock(metrics_mutex_);
        if (metrics_buffer_.empty()) {
            return false;
        }
        latest = metrics_buffer_.back();

        double sum_throughput = 0.0;
        double sum_loss = 0.0;
        for (const auto& m : metrics_buffer_) {
            sum_throughput += m.throughput_kbps;
            sum_loss += m.packet_loss_rate;
        }
        auto count = static_cast<double>(metrics_buffer_.size());
        avg_throughput = sum_throughput / count;
        avg_loss = sum_loss / count;
    }

    uint32_t cur = current_bitrate_kbps_.load(std::memory_order_relaxed);

    // Priority 1: Emergency downscale on high packet loss (>10%)
    if (latest.packet_loss_rate > kEmergencyLossThreshold) {
        uint32_t new_bitrate = static_cast<uint32_t>(cur * kEmergencyFactor);
        new_bitrate = clamp_bitrate(new_bitrate);
        if (new_bitrate != cur) {
            apply_adjustment(new_bitrate, "packet_loss", latest);
            upscale_tracking_ = false;
            return true;
        }
        return false;
    }

    // Priority 2: Downscale when available bandwidth < 80% of current bitrate
    if (avg_throughput < cur * kDownscaleThreshold) {
        uint32_t new_bitrate = static_cast<uint32_t>(avg_throughput * kDownscaleTarget);
        new_bitrate = clamp_bitrate(new_bitrate);
        if (new_bitrate != cur) {
            apply_adjustment(new_bitrate, "bandwidth_low", latest);
            upscale_tracking_ = false;
            return true;
        }
        return false;
    }

    // Priority 3: Upscale when bandwidth sustained > 150% for 30 seconds
    if (avg_throughput > cur * kUpscaleThreshold) {
        auto now = std::chrono::steady_clock::now();
        if (!upscale_tracking_) {
            upscale_tracking_ = true;
            upscale_start_ = now;
            return false;
        }

        if ((now - upscale_start_) >= kUpscaleDuration) {
            // Step up by at most 20%
            uint32_t step = static_cast<uint32_t>(cur * kMaxUpscaleStep);
            if (step == 0) step = 1;
            uint32_t new_bitrate = clamp_bitrate(cur + step);
            if (new_bitrate != cur) {
                apply_adjustment(new_bitrate, "bandwidth_recovery", latest);
                // Reset tracking so next upscale requires another 30s
                upscale_tracking_ = false;
                return true;
            }
        }
        return false;
    }

    // Bandwidth is between 80% and 150% — stable, reset upscale tracking
    upscale_tracking_ = false;
    return false;
}

// ============================================================
// Helpers
// ============================================================

uint32_t BitrateController::clamp_bitrate(uint32_t bitrate_kbps) const {
    if (bitrate_kbps < min_bitrate_kbps_) return min_bitrate_kbps_;
    if (bitrate_kbps > max_bitrate_kbps_) return max_bitrate_kbps_;
    return bitrate_kbps;
}

void BitrateController::apply_adjustment(uint32_t new_bitrate,
                                         const std::string& reason,
                                         const NetworkMetrics& trigger_metrics) {
    uint32_t old_bitrate = current_bitrate_kbps_.exchange(
        new_bitrate, std::memory_order_relaxed);

    BitrateAdjustment adj;
    adj.old_bitrate_kbps = old_bitrate;
    adj.new_bitrate_kbps = new_bitrate;
    adj.reason = reason;
    adj.metrics = trigger_metrics;

    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (callback_) {
        callback_(adj);
    }
}

}  // namespace sc
