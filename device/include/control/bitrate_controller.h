#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>

#include "core/types.h"

namespace sc {

// ============================================================
// NetworkMetrics — a single network sample
// ============================================================

struct NetworkMetrics {
    double throughput_kbps;     // Current throughput
    double packet_loss_rate;   // Packet loss ratio (0.0 - 1.0)
    double rtt_ms;             // Round-trip time
    std::chrono::steady_clock::time_point sample_time;
};

// ============================================================
// BitrateAdjustment — describes a single bitrate change event
// ============================================================

struct BitrateAdjustment {
    uint32_t old_bitrate_kbps;
    uint32_t new_bitrate_kbps;
    std::string reason;        // "bandwidth_low", "packet_loss", "bandwidth_recovery"
    NetworkMetrics metrics;
};

// ============================================================
// IBitrateController — adaptive bitrate control interface
// ============================================================

class IBitrateController {
public:
    virtual ~IBitrateController() = default;

    virtual VoidResult start(uint32_t initial_bitrate_kbps,
                             uint32_t min_bitrate_kbps,
                             uint32_t max_bitrate_kbps) = 0;
    virtual VoidResult stop() = 0;
    virtual void report_metrics(const NetworkMetrics& metrics) = 0;
    virtual uint32_t current_bitrate() const = 0;

    // Register callback for bitrate adjustments
    using AdjustmentCallback = std::function<void(const BitrateAdjustment&)>;
    virtual void on_adjustment(AdjustmentCallback cb) = 0;
};

// ============================================================
// BitrateController — concrete implementation
// ============================================================

class BitrateController : public IBitrateController {
public:
    BitrateController() = default;
    ~BitrateController() override;

    VoidResult start(uint32_t initial_bitrate_kbps,
                     uint32_t min_bitrate_kbps,
                     uint32_t max_bitrate_kbps) override;
    VoidResult stop() override;
    void report_metrics(const NetworkMetrics& metrics) override;
    uint32_t current_bitrate() const override;
    void on_adjustment(AdjustmentCallback cb) override;

    // Exposed for testing: evaluate metrics and adjust bitrate.
    // Returns true if an adjustment was made.
    bool evaluate_and_adjust();

private:
    uint32_t clamp_bitrate(uint32_t bitrate_kbps) const;
    void apply_adjustment(uint32_t new_bitrate, const std::string& reason,
                          const NetworkMetrics& trigger_metrics);

    // Configuration
    uint32_t min_bitrate_kbps_ = 256;
    uint32_t max_bitrate_kbps_ = 4096;

    // Current bitrate (atomic for lock-free reads)
    std::atomic<uint32_t> current_bitrate_kbps_{0};

    // Metrics buffer (protected by shared_mutex)
    mutable std::shared_mutex metrics_mutex_;
    std::deque<NetworkMetrics> metrics_buffer_;

    // Callback (protected by mutex)
    std::mutex callback_mutex_;
    AdjustmentCallback callback_;

    // Sampling thread
    std::thread sample_thread_;
    std::atomic<bool> running_{false};
    std::mutex cv_mutex_;
    std::condition_variable cv_;

    // Upscale tracking: bandwidth must exceed 150% for 30 seconds
    std::chrono::steady_clock::time_point upscale_start_{};
    bool upscale_tracking_ = false;

    static constexpr auto kSampleInterval = std::chrono::seconds(5);
    static constexpr auto kUpscaleDuration = std::chrono::seconds(30);
    static constexpr double kDownscaleThreshold = 0.80;
    static constexpr double kUpscaleThreshold = 1.50;
    static constexpr double kDownscaleTarget = 0.70;
    static constexpr double kEmergencyLossThreshold = 0.10;
    static constexpr double kEmergencyFactor = 0.50;
    static constexpr double kMaxUpscaleStep = 0.20;
};

}  // namespace sc
